#pragma once

#include "../MotionWallpaper.Common/Common.h"

#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace motion::app
{
    // Automatic scenes are applied on every Agent policy pass. An explicit
    // recovery must update the currently effective scene's mode as well,
    // while preserving its trigger and every other saved preference.
    inline bool set_original_recovery_mode(motion::Settings& settings,
        std::string_view mode, std::string_view effectiveSceneId)
    {
        if (mode != "balanced" && mode != "power-saver") return false;
        settings.performanceMode = mode;
        for (auto& scene : settings.scenes) {
            if (scene.id == effectiveSceneId && scene.activation.enabled &&
                scene.activation.trigger != "manual") {
                scene.performanceMode = mode;
                break;
            }
        }
        return true;
    }

    // Only a confirmed original-media failure offers a conversion remedy.
    // Agent availability and ordinary paused states need different recovery.
    [[nodiscard]] inline std::vector<motion::DisplayRuntimeState> original_playback_failures(
        std::string_view performanceMode, std::vector<motion::DisplayRuntimeState> const& states)
    {
        std::vector<motion::DisplayRuntimeState> failures;
        if (performanceMode != "original") return failures;
        std::set<std::pair<std::string, std::string>> seen;
        for (auto const& state : states) {
            if (state.state != "failed" || state.reason != "original-playback-failed" ||
                state.groupId.empty() || state.mediaId.empty()) continue;
            if (seen.emplace(state.groupId, state.mediaId).second) failures.push_back(state);
        }
        return failures;
    }
}
