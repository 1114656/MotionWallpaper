#pragma once
#include "../MotionWallpaper.App/WallpaperAssignment.h"

namespace motion::tests
{
    template<class Require>
    void wallpaper_apply_is_explicit_and_target_scoped(Require require)
    {
        auto group = new_id(), first = new_id(), second = new_id();
        DisplayTarget left; left.id = "left"; left.primary = true;
        DisplayTarget right; right.id = "right";
        std::vector<DisplayTarget> connected{ left, right };
        Settings settings;
        settings.selectedGroupId = group; settings.selectedMediaId = first;
        settings.performanceMode = "original"; settings.activePlaybackEnabled = false;
        settings.displayMode = "primary";
        settings.displayAssignments = { { "left", group, first }, { "right", group, first } };
        auto original = settings.displayAssignments;

        require(!app::apply_wallpaper_assignment(settings, group, second, "disconnected", connected),
            "a disconnected screen must not silently fall back to all screens");
        require(settings.displayAssignments.size() == original.size() &&
            settings.displayAssignments.front().mediaId == first && settings.displayAssignments.front().displayId == "left" &&
            settings.displayAssignments.back().mediaId == first && settings.displayAssignments.back().displayId == "right" &&
            settings.displayMode == "primary" && settings.selectedMediaId == first,
            "a rejected target changed settings");
        require(!app::apply_wallpaper_assignment(settings, group, second, {}, {}),
            "an empty topology must not accept an all-screens assignment");
        require(!app::apply_wallpaper_assignment(settings, "../invalid", second, "right", connected),
            "invalid library identifiers must not be persisted");

        require(app::apply_wallpaper_assignment(settings, group, second, "right", connected), "single-screen apply failed");
        require(settings.displayAssignments.front().mediaId == first && settings.displayAssignments.back().mediaId == second &&
            settings.selectedMediaId == first && settings.displayMode == "independent",
            "single-screen apply changed an unrelated screen or failed to leave primary-only mode");
        require(!settings.activePlaybackEnabled && settings.performanceMode == "original",
            "wallpaper assignment unexpectedly changed global playback or quality");
        require(app::apply_wallpaper_assignment(settings, group, first, "right", connected) && settings.displayAssignments.size() == 2,
            "reapplying a screen duplicated its assignment");
        require(app::apply_wallpaper_assignment(settings, group, second, {}, connected), "all-screens apply failed");
        require(settings.selectedMediaId == second && settings.displayAssignments.empty() && settings.displayMode == "independent",
            "explicit all-screens apply did not replace independent overrides");
    }
}
