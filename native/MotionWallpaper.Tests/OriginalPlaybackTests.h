#pragma once

#include "../MotionWallpaper.Agent/RuntimeStatusPolicy.h"

namespace motion::tests
{
    template <typename Require>
    inline void original_playback_has_a_bounded_first_frame_attempt(Require require)
    {
        using namespace motion::agent;
        require(!original_first_frame_expired(true, true, true, 5'999) &&
            original_first_frame_expired(true, true, true, 6'000),
            "original playback no longer has a bounded six-second first-frame attempt");
        require(!original_first_frame_expired(true, true, false, 20'000),
            "an acknowledged original was mistaken for a first-frame timeout");
        require(!original_first_frame_expired(true, false, true, 20'000) &&
            !original_first_frame_expired(false, true, true, 20'000),
            "the original-only failure policy leaked into poster or performance-copy startup");
        require(!original_playback_failed(true, true, false, "software-fallback") &&
            !original_playback_failed(true, true, false, ""),
            "an unsupported hardware profile was treated as a real original playback failure");
        require(original_playback_failed(true, true, false, "unavailable") &&
            original_playback_failed(true, true, true, "automatic"),
            "a real original decoder failure or renderer crash was not retained");
        require(defer_source_decode_probe(true, true) &&
            !defer_source_decode_probe(true, false) &&
            !defer_source_decode_probe(false, true),
            "pending performance copies inherited their source's hardware-decode failure");

        // The user may request a copy immediately after original playback
        // fails, before its poster has replaced the failed Renderer route.
        auto exitedOriginal = preview_freeze_action(false, false, true, false);
        require(exitedOriginal == PreviewFreezeAction::stop &&
            preview_freeze_settled(exitedOriginal, true),
            "a failed original route still waits forever for a freeze acknowledgement");
        require(!preview_freeze_settled(exitedOriginal, false),
            "copy generation bypassed an original Renderer whose retirement failed");
        require(preview_freeze_action(false, false, false, false) == PreviewFreezeAction::stop,
            "an already exited route without a failure flag cannot settle the preview barrier");
        require(preview_freeze_action(true, true, true, false) == PreviewFreezeAction::stop,
            "a failed route with a pending command was allowed to stall the preview barrier");
        auto pendingPoster = preview_freeze_action(true, true, false, false);
        require(pendingPoster == PreviewFreezeAction::wait &&
            !preview_freeze_settled(pendingPoster, true),
            "a healthy pending poster was retired or marked ready before its frame acknowledgement");
        require(preview_freeze_settled(
                preview_freeze_action(true, false, false, true), false),
            "a successfully frozen live route stopped satisfying the preview barrier");
    }

    template <typename Require>
    inline void original_failure_poster_preserves_identity_and_status(Require require)
    {
        using namespace motion::agent;
        OriginalPlaybackFailure failure{ "group-a", "media-a", L"D:\\media\\source.mov",
            "original-first-frame-timeout" };
        require(failure.Matches("group-a", "media-a", L"D:\\media\\source.mov") &&
            !failure.Matches("group-b", "media-a", L"D:\\media\\source.mov") &&
            !failure.Matches("group-a", "media-b", L"D:\\media\\source.mov") &&
            !failure.Matches("group-a", "media-a", L"D:\\media\\replacement.mov"),
            "an original failure leaked across media, group, or source replacement");
        DisplayRuntimeSignals poster;
        poster.rendererReady = true;
        poster.originalPlaybackFailed = true;
        require(display_runtime_state(poster) == "failed",
            "a successfully presented poster concealed the original playback failure");
        poster.rendererReady = false;
        poster.rendererAbsent = true;
        require(display_runtime_state(poster) == "failed",
            "a missing poster lost the original playback failure");
        poster.paused = true;
        require(display_runtime_state(poster) == "paused",
            "paused, locked, or powered-off originals kept exposing a failure action");
        poster.paused = false;
        poster.originalPlaybackFailed = false;
        require(display_runtime_state(poster) == "applying",
            "an explicit retry retained the previous original failure state");
        poster.rendererAbsent = false;
        poster.rendererReady = true;
        require(display_runtime_state(poster) == "applied",
            "a successful original retry did not restore its applied status");
    }
}
