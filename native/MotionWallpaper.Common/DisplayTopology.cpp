#include "DisplayTopology.h"
#include "TextEncoding.h"

#include <algorithm>

namespace motion
{
    std::optional<DisplayRefreshRate> display_path_refresh_rate(
        DISPLAYCONFIG_PATH_INFO const& path, std::vector<DISPLAYCONFIG_MODE_INFO> const& modes) noexcept
    {
        bool virtualMode = (path.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) != 0;
        auto index = virtualMode ? path.targetInfo.targetModeInfoIdx : path.targetInfo.modeInfoIdx;
        auto invalidIndex = virtualMode ? DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID :
            DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        if (index != invalidIndex && index < modes.size()) {
            auto const& mode = modes[index];
            if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET &&
                mode.id == path.targetInfo.id &&
                mode.adapterId.HighPart == path.targetInfo.adapterId.HighPart &&
                mode.adapterId.LowPart == path.targetInfo.adapterId.LowPart) {
                auto const& sync = mode.targetMode.targetVideoSignalInfo.vSyncFreq;
                auto rate = normalized_display_refresh_rate({ sync.Numerator, sync.Denominator });
                if (rate.numerator) return rate;
            }
        }
        // With Windows 11 DRR, path.refreshRate is virtual while the target
        // mode's vSyncFreq above is physical. Use the virtual timing only when
        // no valid physical mode was returned, never instead of that mode.
        auto const& refresh = path.targetInfo.refreshRate;
        auto rate = normalized_display_refresh_rate({ refresh.Numerator, refresh.Denominator });
        return rate.numerator ? std::optional<DisplayRefreshRate>(rate) : std::nullopt;
    }

    namespace
    {
        void collect_precise_refresh_rates(std::vector<DisplayTarget>& displays)
        {
            UINT32 flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE |
                QDC_VIRTUAL_REFRESH_RATE_AWARE;
            std::vector<DISPLAYCONFIG_PATH_INFO> paths;
            std::vector<DISPLAYCONFIG_MODE_INFO> modes;
            bool queried{};
            // A hotplug can change buffer sizes between the two calls. Bound
            // retries so a flapping/remote driver cannot monopolize the Agent.
            for (unsigned attempt = 0; attempt < 4; ++attempt) {
                UINT32 pathCount{}, modeCount{};
                auto result = GetDisplayConfigBufferSizes(flags, &pathCount, &modeCount);
                if (result == ERROR_INVALID_PARAMETER && (flags & QDC_VIRTUAL_REFRESH_RATE_AWARE)) {
                    flags &= ~QDC_VIRTUAL_REFRESH_RATE_AWARE; // Windows 10
                    continue;
                }
                if (result != ERROR_SUCCESS || !pathCount || !modeCount ||
                    pathCount > 4096 || modeCount > 16384) return;
                paths.resize(pathCount);
                modes.resize(modeCount);
                result = QueryDisplayConfig(flags, &pathCount, paths.data(),
                    &modeCount, modes.data(), nullptr);
                if (result == ERROR_INVALID_PARAMETER && (flags & QDC_VIRTUAL_REFRESH_RATE_AWARE)) {
                    flags &= ~QDC_VIRTUAL_REFRESH_RATE_AWARE;
                    continue;
                }
                if (result == ERROR_INSUFFICIENT_BUFFER) continue;
                if (result != ERROR_SUCCESS) return;
                paths.resize(pathCount);
                modes.resize(modeCount);
                queried = true;
                break;
            }
            if (!queried) return;
            for (auto const& path : paths) {
                if (!(path.flags & DISPLAYCONFIG_PATH_ACTIVE) || !path.targetInfo.targetAvailable) continue;
                auto rate = display_path_refresh_rate(path, modes);
                if (!rate) continue;
                DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
                source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                source.header.size = sizeof(source);
                source.header.adapterId = path.sourceInfo.adapterId;
                source.header.id = path.sourceInfo.id;
                if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
                auto display = std::find_if(displays.begin(), displays.end(), [&](auto const& candidate) {
                    return _wcsicmp(candidate.deviceName.c_str(), source.viewGdiDeviceName) == 0;
                });
                // Clone targets share one logical desktop monitor. Follow the
                // first active path in Windows' priority order for that view.
                if (display != displays.end() && !display->refreshRate.numerator) display->refreshRate = *rate;
            }
        }

        BOOL CALLBACK collect_display(HMONITOR monitor, HDC, RECT*, LPARAM parameter)
        {
            auto& displays = *reinterpret_cast<std::vector<DisplayTarget>*>(parameter);
            MONITORINFOEXW info{ sizeof(info) };
            if (!GetMonitorInfoW(monitor, &info)) return TRUE;

            DISPLAY_DEVICEW monitorDevice{ sizeof(monitorDevice) };
            bool foundDevice{};
            for (DWORD index = 0; EnumDisplayDevicesW(info.szDevice, index, &monitorDevice,
                EDD_GET_DEVICE_INTERFACE_NAME); ++index) {
                if (monitorDevice.StateFlags & DISPLAY_DEVICE_ACTIVE) {
                    foundDevice = true;
                    break;
                }
                monitorDevice = { sizeof(monitorDevice) };
            }

            DisplayTarget target;
            target.deviceName = info.szDevice;
            target.bounds = info.rcMonitor;
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsExW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0) &&
                mode.dmDisplayFrequency > 1) {
                target.refreshRateHz = mode.dmDisplayFrequency;
            }
            target.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
            target.friendlyName = foundDevice && monitorDevice.DeviceString[0]
                ? monitorDevice.DeviceString : info.szDevice;
            target.id = utf8_from_wide(foundDevice && monitorDevice.DeviceID[0]
                ? std::wstring_view(monitorDevice.DeviceID) : std::wstring_view(info.szDevice));
            displays.push_back(std::move(target));
            return TRUE;
        }
    }

    std::vector<DisplayTarget> enumerate_displays()
    {
        std::vector<DisplayTarget> displays;
        EnumDisplayMonitors(nullptr, nullptr, collect_display, reinterpret_cast<LPARAM>(&displays));
        collect_precise_refresh_rates(displays);
        std::stable_sort(displays.begin(), displays.end(), [](auto const& left, auto const& right) {
            if (left.primary != right.primary) return left.primary;
            if (left.bounds.left != right.bounds.left) return left.bounds.left < right.bounds.left;
            return left.bounds.top < right.bounds.top;
        });
        for (size_t index = 0; index < displays.size(); ++index) {
            if (displays[index].friendlyName.empty()) displays[index].friendlyName = L"显示器 " + std::to_wstring(index + 1);
        }
        return displays;
    }

    std::optional<RECT> find_display_bounds(std::wstring const& deviceName) noexcept
    {
        try {
            auto displays = enumerate_displays();
            auto found = std::find_if(displays.begin(), displays.end(), [&](auto const& display) {
                return _wcsicmp(display.deviceName.c_str(), deviceName.c_str()) == 0;
            });
            if (found != displays.end()) return found->bounds;
        } catch (...) {}
        return std::nullopt;
    }

    RECT primary_display_bounds() noexcept
    {
        try {
            auto displays = enumerate_displays();
            auto primary = std::find_if(displays.begin(), displays.end(), [](auto const& display) { return display.primary; });
            if (primary != displays.end()) return primary->bounds;
        } catch (...) {}
        return { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    }
}
