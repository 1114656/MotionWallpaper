#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace motion::agent
{
    struct RendererRoute
    {
        std::wstring mediaKey;
        std::wstring monitorDevice;
        std::wstring adapterKey;
        uint64_t outputPixels{};
    };

    struct SharedRendererRoute
    {
        std::wstring mediaKey;
        std::wstring adapterKey;
        std::vector<std::wstring> monitorDevices;
        uint64_t aggregateOutputPixels{};
    };

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

    [[nodiscard]] inline std::vector<SharedRendererRoute> group_renderer_routes(
        std::vector<RendererRoute> const& routes, bool includeMonitorDevices)
    {
        using GroupKey = std::pair<std::wstring, std::wstring>;
        struct GroupValue
        {
            std::vector<std::wstring> monitors;
            uint64_t outputPixels{};
        };
        std::map<GroupKey, GroupValue> grouped;
        for (auto const& route : routes) {
            auto& group = grouped[{ route.mediaKey, route.adapterKey }];
            if (includeMonitorDevices) group.monitors.push_back(route.monitorDevice);
            auto remaining = (std::numeric_limits<uint64_t>::max)() - group.outputPixels;
            group.outputPixels += (std::min)(route.outputPixels, remaining);
        }

        std::vector<SharedRendererRoute> result;
        result.reserve(grouped.size());
        for (auto& entry : grouped) {
            result.push_back({ entry.first.first, entry.first.second,
                std::move(entry.second.monitors), entry.second.outputPixels });
        }
        return result;
    }
}
