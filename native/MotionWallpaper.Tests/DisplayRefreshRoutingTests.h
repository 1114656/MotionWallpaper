#pragma once

#include "../MotionWallpaper.Common/DisplayTopology.h"
#include "../MotionWallpaper.Agent/SharedRendererPolicy.h"

namespace motion::tests
{
    template <typename Require>
    void display_refresh_reads_physical_rational_timing(Require require)
    {
        DISPLAYCONFIG_PATH_INFO path{};
        path.flags = DISPLAYCONFIG_PATH_ACTIVE;
        path.targetInfo.id = 7;
        path.targetInfo.adapterId = { 12, 3 };
        path.targetInfo.modeInfoIdx = 0;
        path.targetInfo.refreshRate = { 60, 1 };
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(3);
        modes[0].infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
        modes[0].id = path.targetInfo.id;
        modes[0].adapterId = path.targetInfo.adapterId;
        modes[0].targetMode.targetVideoSignalInfo.vSyncFreq = { 60000, 1001 };
        require(display_path_refresh_rate(path, modes) == DisplayRefreshRate{ 60000, 1001 },
            "fractional physical refresh was rounded to DEVMODE/path integer Hz");

        path.flags |= DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE | DISPLAYCONFIG_PATH_BOOST_REFRESH_RATE;
        path.targetInfo.desktopModeInfoIdx = 1;
        path.targetInfo.targetModeInfoIdx = 2;
        modes[2] = modes[0];
        modes[2].targetMode.targetVideoSignalInfo.vSyncFreq = { 120000, 1001 };
        require(display_path_refresh_rate(path, modes) == DisplayRefreshRate{ 120000, 1001 },
            "DRR virtual Hz or desktop index replaced the physical target timing");

        modes[2].id = 8;
        require(display_path_refresh_rate(path, modes) == DisplayRefreshRate{ 60, 1 },
            "an unrelated target's timing was accepted after a topology change");
        modes[2] = modes[0];
        modes[2].infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
        require(display_path_refresh_rate(path, modes) == DisplayRefreshRate{ 60, 1 },
            "source mode union was interpreted as a physical target timing");
        path.targetInfo.targetModeInfoIdx = DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID;
        path.targetInfo.refreshRate = { 0, 0 };
        require(!display_path_refresh_rate(path, modes),
            "an invalid physical/path rate suppressed integer fallback");
        DisplayTarget legacy;
        legacy.refreshRateHz = 144;
        require(effective_display_refresh_rate(legacy) == DisplayRefreshRate{ 144, 1 },
            "remote/older display fallback lost dmDisplayFrequency");
        legacy.refreshRate = { 120000, 2002 };
        require(effective_display_refresh_rate(legacy) == DisplayRefreshRate{ 60000, 1001 },
            "equivalent precise rates were not normalized");
        legacy.refreshRate = { 60, 0 };
        require(effective_display_refresh_rate(legacy) == DisplayRefreshRate{ 144, 1 },
            "zero denominator reached route comparison");
    }

    template <typename Require>
    void renderer_routes_share_decode_across_refresh_rates(Require require)
    {
        using namespace motion::agent;
        auto media = renderer_media_key(L"C:\\wallpapers\\valley.mov", "video");
        std::vector<RendererRoute> routes{
            { media, L"display-60", L"gpu-a", 100, { 60, 1 } },
            { media, L"display-144", L"gpu-a", 200, { 144, 1 } },
            { media, L"display-equivalent-60", L"gpu-a", 300, { 120, 2 } }
        };
        auto grouped = group_renderer_routes(routes, true);
        require(grouped.size() == 1 && grouped[0].monitorDevices.size() == 3 &&
            grouped[0].aggregateOutputPixels == 600 && grouped[0].refreshRate == DisplayRefreshRate{144, 1} &&
            grouped[0].monitorRefreshRates == std::vector<DisplayRefreshRate>{{60, 1}, {144, 1}, {60, 1}},
            "mixed-refresh siblings duplicated decode or lost their individual display timing");
        routes.push_back({ media, L"display-59.94", L"gpu-a", 10, { 60000, 1001 } });
        routes.push_back({ media, L"display-119.88", L"gpu-a", 20, { 120000, 1001 } });
        routes.push_back({ media, L"display-equivalent-59.94", L"gpu-a", 30, { 120000, 2002 } });
        routes.push_back({ media, L"display-59.997", L"gpu-a", 40, { 59997, 1000 } });
        grouped = group_renderer_routes(routes, true);
        require(grouped.size() == 1 && grouped[0].monitorDevices.size() == 7 &&
            grouped[0].aggregateOutputPixels == 700 &&
            grouped[0].monitorRefreshRates.back() == DisplayRefreshRate{59997, 1000},
            "fractional output timing unnecessarily split a synchronous decoder");
        auto key = renderer_route_key(grouped[0], L"auto", false, 0, 1);
        routes.back().refreshRate = {60, 1};
        require(renderer_route_key(group_renderer_routes(routes, true)[0], L"auto", false, 0, 1) != key,
            "a slower output timing change was hidden by the group's fastest display");
        routes.push_back({ media, L"display-other-gpu", L"gpu-b", 100, { 60, 1 } });
        routes.push_back({ renderer_media_key(L"C:\\other.mp4", "video"), L"display-other-media",
            L"gpu-a", 100, { 60, 1 } });
        routes.push_back({ media, L"display-preview", renderer_preview_adapter_key(L"gpu-a", true),
            100, { 60, 1 } });
        require(group_renderer_routes(routes, true).size() == 4,
            "decode sharing merged distinct GPUs, media, or static-preview routes");
        auto withoutNames = group_renderer_routes(routes, false);
        require(withoutNames.size() == 4 && std::all_of(withoutNames.begin(), withoutNames.end(),
            [](auto const& route) { return route.monitorDevices.empty(); }),
            "probe grouping no longer matches decoder sharing");
    }

    template <typename Require>
    void renderer_route_identity_tracks_refresh_and_hotplug(Require require)
    {
        using namespace motion::agent;
        SharedRendererRoute route{ L"media", L"gpu", { L"display" }, 100, { 60000, 1001 } };
        auto key = renderer_route_key(route, L"auto", false, 90, 1);
        route.refreshRate = { 120000, 2002 };
        require(renderer_route_key(route, L"auto", false, 90, 1) == key,
            "equivalent rational timing needlessly restarted Renderer");
        route.refreshRate = { 60, 1 };
        require(renderer_route_key(route, L"auto", false, 90, 1) != key,
            "fractional-to-integer refresh change reused the old presentation route");
        route.refreshRate = { 60000, 1001 };
        require(renderer_route_key(route, L"auto", false, 90, 2) != key,
            "hotplug generation reused the old swapchain route");
        route.monitorDevices = { L"replacement-display" };
        require(renderer_route_key(route, L"auto", false, 90, 1) != key,
            "replacement output reused a route for the removed monitor");
    }
}
