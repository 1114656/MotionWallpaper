#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace motion::agent
{
    struct OriginalPlaybackFailure
    {
        std::string groupId;
        std::string mediaId;
        std::wstring source;
        std::string reason;
        std::string errorDetail;

        [[nodiscard]] bool Matches(std::string_view group, std::string_view media,
            std::wstring_view path) const noexcept
        {
            return groupId == group && mediaId == media && source == path;
        }
    };

    inline constexpr uint64_t original_first_frame_timeout_ms = 6'000;

    [[nodiscard]] constexpr bool original_first_frame_expired(
        bool originalPlayback, bool video, bool awaitingFrame, uint64_t elapsedMs) noexcept
    {
        return originalPlayback && video && awaitingFrame &&
            elapsedMs >= original_first_frame_timeout_ms;
    }

    struct DisplayRuntimeSignals
    {
        bool rendererFailed{};
        bool optimizationPending{};
        bool paused{};
        bool rendererReady{};
        bool degradedPlayback{};
        bool rendererAbsent{};
        bool originalPlaybackFailed{};
    };

    // Status precedence is deliberately user-facing: a blank/crashed route
    // must never be hidden behind optimization progress, while an intentional
    // pause is distinct from a Renderer that has not acknowledged its target.
    [[nodiscard]] constexpr std::string_view display_runtime_state(
        DisplayRuntimeSignals const& signals) noexcept
    {
        // A failed original stays failed behind its static poster. Explicit
        // pause/lock/off takes precedence so a hidden wallpaper does not keep
        // presenting an actionable failure card while playback is disabled.
        if (signals.originalPlaybackFailed) return signals.paused ? "paused" : "failed";
        if (signals.rendererFailed) return "failed";
        if (signals.paused && signals.rendererAbsent) return "paused";
        // Neither optimization nor pause is real until Renderer has
        // acknowledged the requested target. Publishing either state early
        // makes a blank or still-playing route look settled.
        if (!signals.rendererReady) return "applying";
        if (signals.paused) return "paused";
        if (signals.optimizationPending) return "optimizing";
        if (signals.degradedPlayback) return "degraded";
        return "applied";
    }

    [[nodiscard]] constexpr bool defer_source_decode_probe(
        bool sourceBacked, bool performanceCopyRequired) noexcept
    {
        return sourceBacked && performanceCopyRequired;
    }

    [[nodiscard]] constexpr bool original_playback_failed(
        bool originalPlayback, bool video, bool rendererFailed,
        std::string_view decodePath) noexcept
    {
        return originalPlayback && video && (rendererFailed || decodePath == "unavailable");
    }

    enum class PreviewFreezeAction { wait, ready, stop };

    [[nodiscard]] constexpr PreviewFreezeAction preview_freeze_action(
        bool running, bool transitionPending, bool rendererFailed, bool targetReady) noexcept
    {
        if (rendererFailed || (!running && !transitionPending)) return PreviewFreezeAction::stop;
        return targetReady ? PreviewFreezeAction::ready : PreviewFreezeAction::wait;
    }

    [[nodiscard]] constexpr bool preview_freeze_settled(
        PreviewFreezeAction action, bool stopSucceeded) noexcept
    {
        return action == PreviewFreezeAction::ready ||
            (action == PreviewFreezeAction::stop && stopSucceeded);
    }

    [[nodiscard]] constexpr std::string_view desktop_pause_reason(
        bool manuallyPaused) noexcept
    {
        return manuallyPaused ? "manual-pause" : "desktop-covered";
    }
}
