#pragma once

#include "../MotionWallpaper.Common/DisplayTopology.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace motion::agent
{
    struct RendererRoute
    {
        std::wstring mediaKey;
        std::wstring monitorDevice;
        std::wstring adapterKey;
        uint64_t outputPixels{};
        motion::DisplayRefreshRate refreshRate{ 60, 1 };
    };

    struct SharedRendererRoute
    {
        std::wstring mediaKey;
        std::wstring adapterKey;
        std::vector<std::wstring> monitorDevices;
        uint64_t aggregateOutputPixels{};
        // Maximum output cadence is useful for budgets, not a decode-sharing
        // boundary. Each swap chain owns its own nonblocking readiness token.
        motion::DisplayRefreshRate refreshRate{ 60, 1 };
        std::vector<motion::DisplayRefreshRate> monitorRefreshRates;
    };

    [[nodiscard]] inline std::wstring renderer_route_key(SharedRendererRoute const& route,
        std::wstring const& decodeMode, bool primaryOnly, uint32_t frameRateCap,
        uint64_t topologyGeneration)
    {
        std::wstring key = route.mediaKey + L"\n" + decodeMode +
            (primaryOnly ? L"\nprimary\n" : L"\nmonitor\n") + route.adapterKey + L"\n" +
            std::to_wstring(frameRateCap) + L"\n" +
            motion::display_refresh_rate_key(route.refreshRate) + L"\n" +
            std::to_wstring(topologyGeneration);
        for (size_t index = 0; index < route.monitorDevices.size(); ++index) {
            key += L"\n" + route.monitorDevices[index];
            if (index < route.monitorRefreshRates.size())
                key += L"\n" + motion::display_refresh_rate_key(route.monitorRefreshRates[index]);
        }
        return key;
    }

    // Compatibility preparation is polled until a copy is ready. Only retire
    // the old video routes: a newly launched poster must survive subsequent
    // polls, including the polls before it acknowledges its first frame.
    template<typename IsStaticImage>
    [[nodiscard]] std::vector<std::wstring> compatibility_renderer_routes_to_stop(
        std::vector<std::string> const& displayIds,
        std::map<std::string, std::wstring> const& displayRoutes,
        IsStaticImage const& isStaticImage)
    {
        std::vector<std::wstring> keys;
        for (auto const& displayId : displayIds) {
            auto route = displayRoutes.find(displayId);
            if (route == displayRoutes.end() || isStaticImage(route->second) ||
                std::find(keys.begin(), keys.end(), route->second) != keys.end()) {
                continue;
            }
            keys.push_back(route->second);
        }
        return keys;
    }

    [[nodiscard]] inline std::wstring renderer_media_key(
        std::filesystem::path const& path, std::string const& kind)
    {
        return path.wstring() + L"\n" + std::wstring(kind.begin(), kind.end());
    }

    [[nodiscard]] inline std::wstring renderer_adapter_key(
        std::wstring adapterKey, std::wstring const& monitorDevice)
    {
        if (!adapterKey.empty()) return adapterKey;
        // Indirect displays, wireless projection, Remote Desktop and some
        // DisplayLink drivers do not expose a matching IDXGIOutput. Keep each
        // unknown display isolated instead of treating all empty LUIDs as one
        // adapter and accidentally sharing a cross-device Renderer.
        return monitorDevice.empty() ? std::wstring(L"unknown-display") :
            L"display:" + monitorDevice;
    }

    // A static performance preview and a continuously playing sibling must
    // never collapse into the same Renderer route. This partition is local to
    // presentation; the real decoder adapter LUID is still passed separately
    // when the Renderer process is launched.
    [[nodiscard]] inline std::wstring renderer_preview_adapter_key(
        std::wstring adapterKey, bool staticPerformancePreview)
    {
        if (staticPerformancePreview) adapterKey += L"\nperformance-preview";
        return adapterKey;
    }

    [[nodiscard]] inline std::vector<SharedRendererRoute> group_renderer_routes(
        std::vector<RendererRoute> const& routes, bool includeMonitorDevices)
    {
        using GroupKey = std::pair<std::wstring, std::wstring>;
        struct GroupValue
        {
            std::vector<std::wstring> monitors;
            uint64_t outputPixels{};
            motion::DisplayRefreshRate maximumRefresh{};
            std::vector<motion::DisplayRefreshRate> refreshRates;
        };
        std::map<GroupKey, GroupValue> grouped;
        for (auto const& route : routes) {
            // One media clock/decoder feeds all synchronous outputs on this
            // adapter. Fractional or mixed refresh rates belong to presentation,
            // where waitable swap chains and DO_NOT_WAIT isolate each output.
            auto rate = motion::normalized_display_refresh_rate(route.refreshRate);
            auto& group = grouped[{ route.mediaKey, route.adapterKey }];
            if (includeMonitorDevices) {
                group.monitors.push_back(route.monitorDevice);
                group.refreshRates.push_back(rate);
            }
            if (static_cast<uint64_t>(rate.numerator) * group.maximumRefresh.denominator >
                static_cast<uint64_t>(group.maximumRefresh.numerator) * rate.denominator)
                group.maximumRefresh = rate;
            auto remaining = (std::numeric_limits<uint64_t>::max)() - group.outputPixels;
            group.outputPixels += (std::min)(route.outputPixels, remaining);
        }

        std::vector<SharedRendererRoute> result;
        result.reserve(grouped.size());
        for (auto& entry : grouped) {
            result.push_back({ entry.first.first, entry.first.second,
                std::move(entry.second.monitors), entry.second.outputPixels,
                entry.second.maximumRefresh, std::move(entry.second.refreshRates) });
        }
        return result;
    }
}
