#pragma once

#include "../MotionWallpaper.Common/Common.h"

#include <cstdint>

namespace motion::agent
{
    inline constexpr uint32_t responsive_wait_ms = 50;
    inline constexpr uint32_t background_work_wait_ms = 500;
    inline constexpr uint32_t stable_wait_ms = 1000;

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

    // Active desktop playback must reflect the selected performance tier. A
    // source-file fallback is still safe for screensaver playback, but it must
    // not make an in-progress balanced/power-saver task appear to be ready.
    [[nodiscard]] inline bool active_playback_waits_for_performance_copy(
        std::string const& performanceMode,
        bool performanceCopyPending,
        bool videoSourceFallback) noexcept
    {
        return videoSourceFallback &&
            (performanceMode == "balanced" || performanceMode == "power-saver") &&
            performanceCopyPending;
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
