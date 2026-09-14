#pragma once

#include "VariantPageModel.h"

#include <functional>

namespace motion::app
{
    using VariantPauseAction = std::function<void(bool paused)>;
    using VariantCancelAction = std::function<void()>;

    // Keep the mutable controls for an active task so a percentage-only refresh
    // can update them in place. Replacing the visual tree once per percentage
    // change is needlessly expensive and also discards keyboard/UIA focus.
    struct VariantTaskCard
    {
        winrt::Microsoft::UI::Xaml::Controls::Border root{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock stateText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::ProgressBar progress{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button pause{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button cancel{ nullptr };
    };

    [[nodiscard]] VariantTaskCard create_variant_task_card_view(
        VariantMediaSummary const& item,
        std::filesystem::path const& cover,
        bool waitingForPower,
        VariantPauseAction pause,
        VariantCancelAction cancel);

    void update_variant_task_card(
        VariantTaskCard const& card,
        VariantMediaSummary const& item,
        bool waitingForPower);

    // Compatibility wrapper for callers that do not need incremental updates.
    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::Border create_variant_task_card(
        VariantMediaSummary const& item,
        std::filesystem::path const& cover,
        bool waitingForPower,
        VariantPauseAction pause,
        VariantCancelAction cancel);
}
