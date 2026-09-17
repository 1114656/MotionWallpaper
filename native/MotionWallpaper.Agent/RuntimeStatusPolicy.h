#pragma once

#include <string_view>

namespace motion::agent
{
    struct DisplayRuntimeSignals
    {
        bool rendererFailed{};
        bool optimizationPending{};
        bool paused{};
        bool rendererReady{};
        bool degradedPlayback{};
    };

    // Status precedence is deliberately user-facing: a blank/crashed route
    // must never be hidden behind optimization progress, while an intentional
    // pause is distinct from a Renderer that has not acknowledged its target.
    [[nodiscard]] constexpr std::string_view display_runtime_state(
        DisplayRuntimeSignals const& signals) noexcept
    {
        if (signals.rendererFailed) return "failed";
        // Neither optimization nor pause is real until Renderer has
        // acknowledged the requested target. Publishing either state early
        // makes a blank or still-playing route look settled.
        if (!signals.rendererReady) return "applying";
        if (signals.paused) return "paused";
        if (signals.optimizationPending) return "optimizing";
        if (signals.degradedPlayback) return "degraded";
        return "applied";
    }

    [[nodiscard]] constexpr std::string_view desktop_pause_reason(
        bool manuallyPaused) noexcept
    {
        return manuallyPaused ? "manual-pause" : "desktop-covered";
    }
}
