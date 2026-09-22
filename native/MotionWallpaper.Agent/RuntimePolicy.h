#pragma once

#include "../MotionWallpaper.Common/Common.h"

#include <cstdint>

namespace motion::agent
{
    inline constexpr uint32_t responsive_wait_ms = 50;
    inline constexpr uint32_t background_work_wait_ms = 500;
    inline constexpr uint32_t stable_wait_ms = 1000;
    inline constexpr uint32_t optimizer_quiesce_timeout_ms = 2000;

    enum class RuntimeAction
    {
        DisplayOff,
        Locked,
        ScreensaverPlay,
        Stopped,
        DesktopPaused,
        DesktopFrozen,
        DesktopPlay
    };

    struct RuntimeSignals
    {
        bool displayOn{ true };
        bool sessionLocked{};
        bool covered{};
        bool hasMedia{};
        int64_t idleSeconds{};
    };

    // This is the only playback priority reducer used by the Agent. Keep the
    // order explicit: power/session > screensaver > disabled/missing media >
    // full-screen coverage > activity freeze > normal playback.
    [[nodiscard]] inline RuntimeAction reduce_runtime_action(
        motion::Settings const& settings, RuntimeSignals const& signals) noexcept
    {
        if (!signals.displayOn) return RuntimeAction::DisplayOff;
        if (signals.sessionLocked) return RuntimeAction::Locked;
        if (signals.hasMedia && settings.screensaverEnabled &&
            signals.idleSeconds >= settings.idleTimeoutSeconds) {
            return RuntimeAction::ScreensaverPlay;
        }
        if (!settings.desktopPlayback || !signals.hasMedia) return RuntimeAction::Stopped;
        if (signals.covered && !settings.continueWhenCovered) return RuntimeAction::DesktopPaused;
        if (!settings.activePlaybackEnabled) return RuntimeAction::DesktopFrozen;
        return RuntimeAction::DesktopPlay;
    }

    // Renderer ACKs and short media-library transactions need a responsive
    // retry. Once the requested state is acknowledged, window, settings,
    // session and power events wake the Agent immediately; a one-second poll
    // is only a safety net and avoids a permanent 20 Hz policy loop.
    [[nodiscard]] constexpr uint32_t runtime_wait_interval_ms(
        bool targetReady, bool shortTransaction = false) noexcept
    {
        return targetReady && !shortTransaction ? stable_wait_ms : responsive_wait_ms;
    }

    // A selected performance copy has no kernel completion event, so the
    // Agent still samples its durable state. Once the old Renderer has frozen,
    // a 500 ms cadence is responsive enough for handoff without waking the
    // policy loop twenty times per second for the duration of a long encode.
    [[nodiscard]] constexpr uint32_t performance_copy_wait_interval_ms(
        bool frozenRendererSettled) noexcept
    {
        return frozenRendererSettled ? background_work_wait_ms : responsive_wait_ms;
    }

    // Transcoding is optional background work. Never start or continue it on
    // battery; the selected playback tier remains unchanged and any durable
    // request resumes when AC power returns.
    [[nodiscard]] constexpr bool variant_generation_allowed(
        bool onBattery, bool priorityRequest, bool playbackIdle) noexcept
    {
        return !onBattery && (priorityRequest || playbackIdle);
    }

    // Active desktop playback must reflect the selected performance tier or
    // cpu-smooth compatibility path. A source-file fallback is still safe for
    // screensaver playback, but it must not appear ready while that copy is
    // unavailable.
    [[nodiscard]] inline bool active_playback_waits_for_performance_copy(
        bool performanceCopyRequired,
        bool videoSourceFallback) noexcept
    {
        return videoSourceFallback && performanceCopyRequired;
    }

    // A mixed-display pool chooses its target per route. Only an output whose
    // selected performance tier is unavailable freezes; unrelated displays
    // continue playing normally.
    [[nodiscard]] constexpr bool performance_copy_preview_required(
        RuntimeAction action, bool performanceCopyRequired) noexcept
    {
        return action == RuntimeAction::DesktopPlay && performanceCopyRequired;
    }

    // An automatic cpu-smooth copy exists specifically because the current
    // Renderer route cannot decode the source. Asking that failed route to
    // freeze creates a deadlock before either a poster or a stopped state can
    // become the generation barrier. Stop only that route first; the original
    // file remains untouched.
    [[nodiscard]] constexpr bool compatibility_copy_requires_renderer_stop(
        bool performanceCopyRequired, bool softwarePlaybackTarget) noexcept
    {
        return performanceCopyRequired && softwarePlaybackTarget;
    }

    // A transcode may run only after every old source route is known to be
    // stationary or gone. Desktop playback needs its explicit poster/first-
    // frame barrier; freeze/pause need their target ACK; stopped playback must
    // have no active route at all. Retiring routes are checked in every case.
    [[nodiscard]] constexpr bool optimization_renderer_is_static(
        RuntimeAction action, bool waitingForPerformanceCopy,
        bool previewBarrierReady, bool targetReady, bool hasActiveRoute,
        bool retiringRoutesStopped, bool holdExistingRenderer = false) noexcept
    {
        if (holdExistingRenderer || !retiringRoutesStopped) return false;
        if (waitingForPerformanceCopy) return previewBarrierReady;
        if (action == RuntimeAction::DesktopFrozen ||
            action == RuntimeAction::DesktopPaused) return targetReady;
        if (action == RuntimeAction::Stopped) return !hasActiveRoute;
        return false;
    }

    [[nodiscard]] constexpr bool source_presentation_may_apply(
        bool sourceTransitionNeedsIdle, bool optimizerIdle) noexcept
    {
        return !sourceTransitionNeedsIdle || optimizerIdle;
    }

    // A frozen previous route can acknowledge its freeze target while a new
    // performance copy is still pending. That ACK protects the picture, but it
    // is not evidence that the newly selected media has presented a first
    // frame and therefore must not advance runtime.json to the new IDs.
    [[nodiscard]] constexpr bool runtime_selection_can_publish(
        bool targetReady, bool waitingForPerformanceCopy) noexcept
    {
        return targetReady && !waitingForPerformanceCopy;
    }
}
