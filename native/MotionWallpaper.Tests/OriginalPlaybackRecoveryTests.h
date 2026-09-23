#pragma once

#include "../MotionWallpaper.App/OriginalPlaybackRecovery.h"

namespace motion::tests
{
    template <typename Require>
    inline void original_playback_recovery_targets_confirmed_unique_media(Require require)
    {
        motion::DisplayRuntimeState first;
        first.displayId = "display-one";
        first.groupId = "group-one";
        first.mediaId = "video-one";
        first.state = "failed";
        first.reason = "original-playback-failed";
        first.decodeReason = "original-first-frame-timeout";
        auto second = first;
        second.displayId = "display-two";
        auto different = second;
        different.mediaId = "video-two";
        auto paused = first;
        paused.state = "paused";
        paused.mediaId = "paused-video";
        auto offline = first;
        offline.reason = "agent-not-running";
        offline.mediaId = "offline-video";
        auto missingIdentity = first;
        missingIdentity.groupId.clear();
        auto states = std::vector{ first, second, different, paused, offline, missingIdentity };
        auto failures = motion::app::original_playback_failures("original", states);
        require(failures.size() == 2 && failures[0].mediaId == "video-one" &&
            failures[1].mediaId == "video-two" && failures[0].decodeReason == first.decodeReason,
            "original recovery duplicated a multi-screen video, lost its reason, or offered conversion for an unrelated failure");
        require(motion::app::original_playback_failures("balanced", states).empty() &&
            motion::app::original_playback_failures("power-saver", states).empty() &&
            motion::app::original_playback_failures("original", {}).empty(),
            "a stale original failure survived a mode change or cleared runtime state");
        motion::Settings settings;
        settings.performanceMode = "original";
        motion::SceneProfile automaticScene;
        automaticScene.id = "night";
        automaticScene.performanceMode = "original";
        automaticScene.activation.enabled = true;
        automaticScene.activation.trigger = "time-range";
        automaticScene.activation.startMinute = 1200;
        auto otherScene = automaticScene;
        otherScene.id = "other";
        settings.scenes = { automaticScene, otherScene };
        require(motion::app::set_original_recovery_mode(settings, "balanced", "night") &&
            settings.performanceMode == "balanced" && settings.scenes[0].performanceMode == "balanced" &&
            settings.scenes[1].performanceMode == "original" && settings.scenes[0].activation.enabled &&
            settings.scenes[0].activation.startMinute == 1200,
            "automatic scene recovery was overwritten or changed unrelated scene/trigger settings");
        require(!motion::app::set_original_recovery_mode(settings, "original", "night") &&
            settings.performanceMode == "balanced", "invalid recovery mode mutated settings");
    }
}
