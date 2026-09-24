#include "pch.h"
#include "VariantTaskView.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace
{
    std::wstring file_uri(std::filesystem::path const& path)
    {
        return L"file:///" + path.generic_wstring();
    }

    std::wstring variant_mode_label(std::string const& mode)
    {
        return mode == "power-saver" ? L"低功耗" : L"自动平衡";
    }

    std::wstring task_context(motion::app::VariantMediaSummary const& item)
    {
        return item.media.name + L"，" + variant_mode_label(item.status.failed
            ? item.status.failedMode : item.status.requestedMode) + L"优化版本";
    }

    std::wstring eta_label(uint64_t seconds)
    {
        if (seconds < 60) return L"预计不到 1 分钟";
        auto minutes = (seconds + 59) / 60;
        if (minutes < 60) return L"预计约 " + std::to_wstring(minutes) + L" 分钟";
        auto hours = minutes / 60;
        auto remainder = minutes % 60;
        return L"预计约 " + std::to_wstring(hours) + L" 小时" +
            (remainder ? L" " + std::to_wstring(remainder) + L" 分钟" : L"");
    }
}

namespace motion::app
{
    void update_variant_task_card(VariantTaskCard const& card,
        VariantMediaSummary const& item, bool waitingForPower)
    {
        bool taskWaitingForPower = item.status.waitingForPower ||
            (waitingForPower && !item.status.generating);
        std::wstring stateLabel;
        if (item.status.failed) stateLabel = L"生成失败 · " + (item.status.failedReason.empty()
            ? std::wstring(L"原文件已保留，可重试") : motion::utf8_to_wide(item.status.failedReason));
        else if (item.status.paused) stateLabel = L"已停止 · 再次生成会从头开始";
        else if (item.status.generating) stateLabel = L"正在优化";
        else if (taskWaitingForPower) stateLabel = L"等待接通电源";
        else stateLabel = L"等待优化";
        if (item.status.progressKnown && !item.status.paused && !item.status.failed) {
            stateLabel += L" · " + std::to_wstring(item.status.progressPercent) + L"%";
        }
        if (item.status.generating && item.status.estimatedRemainingKnown) {
            stateLabel += L" · " + eta_label(item.status.estimatedRemainingSeconds);
        }

        auto context = task_context(item);
        if (card.root) {
            Automation::AutomationProperties::SetName(card.root, hstring(context + L"任务"));
        }
        if (card.stateText) {
            card.stateText.Text(stateLabel);
            card.stateText.TextTrimming(TextTrimming::CharacterEllipsis);
            ToolTipService::SetToolTip(card.stateText, box_value(stateLabel));
        }
        if (card.progress) {
            card.progress.Value(item.status.progressKnown ? item.status.progressPercent : 0);
            // FFmpeg reports a real media-time percentage once probing succeeds.
            // Queued jobs without a known duration remain honestly indeterminate;
            // paused/power-waiting jobs keep a stable bar instead of suggesting work.
            card.progress.IsIndeterminate(!item.status.progressKnown &&
                !item.status.paused && !item.status.failed && !taskWaitingForPower);
            Automation::AutomationProperties::SetName(
                card.progress, hstring(context + L"，" + stateLabel));
        }
        if (card.pause) {
            auto actionLabel = item.status.failed ? std::wstring(L"重试")
                : item.status.paused ? std::wstring(L"重新生成") : std::wstring(L"停止");
            card.pause.Content(box_value(actionLabel));
            ToolTipService::SetToolTip(card.pause, box_value(item.status.paused || item.status.failed
                ? L"从头生成完整副本" : L"停止本次生成；下次会从头开始，原文件保留"));
            // The click handler reads this current value rather than capturing
            // the state that existed when the card was first constructed.
            card.pause.Tag(box_value(item.status.paused || item.status.failed));
            Automation::AutomationProperties::SetName(
                card.pause, hstring(context + L"，" + actionLabel + L"优化"));
        }
        if (card.cancel) {
            Automation::AutomationProperties::SetName(
                card.cancel, hstring(context + L"，取消优化"));
        }
    }

    VariantTaskCard create_variant_task_card_view(VariantMediaSummary const& item,
        std::filesystem::path const& cover, bool waitingForPower,
        VariantPauseAction pauseAction, VariantCancelAction cancelAction)
    {
        auto stroke = Microsoft::UI::Xaml::Media::SolidColorBrush{
            Windows::UI::ColorHelper::FromArgb(255, 220, 226, 232) };
        auto surface = Microsoft::UI::Xaml::Media::SolidColorBrush{ Windows::UI::Colors::White() };
        auto muted = Microsoft::UI::Xaml::Media::SolidColorBrush{
            Windows::UI::ColorHelper::FromArgb(255, 245, 247, 249) };

        Border task;
        task.Padding(ThicknessHelper::FromUniformLength(14));
        task.CornerRadius(CornerRadiusHelper::FromUniformRadius(12));
        task.BorderThickness(ThicknessHelper::FromUniformLength(1));
        task.BorderBrush(stroke);
        task.Background(surface);

        Grid layout;
        layout.ColumnSpacing(14);
        for (auto width : { 72.0, 0.0, 130.0, 190.0, -1.0 }) {
            ColumnDefinition column;
            column.Width(width == 0.0
                ? GridLengthHelper::FromValueAndType(1, GridUnitType::Star)
                : width < 0.0 ? GridLengthHelper::Auto()
                : GridLengthHelper::FromPixels(width));
            layout.ColumnDefinitions().Append(column);
        }

        Border preview;
        preview.Width(72);
        preview.Height(46);
        preview.CornerRadius(CornerRadiusHelper::FromUniformRadius(7));
        preview.Background(muted);
        if (!item.media.coverFileName.empty() && std::filesystem::is_regular_file(cover)) {
            Image image;
            image.Stretch(Microsoft::UI::Xaml::Media::Stretch::UniformToFill);
            image.Source(Microsoft::UI::Xaml::Media::Imaging::BitmapImage{
                Windows::Foundation::Uri(file_uri(cover)) });
            preview.Child(image);
        }
        layout.Children().Append(preview);

        StackPanel identity;
        identity.VerticalAlignment(VerticalAlignment::Center);
        TextBlock name;
        name.Text(item.media.name);
        name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        name.TextTrimming(TextTrimming::CharacterEllipsis);
        TextBlock group;
        group.Text(item.groupName);
        group.FontSize(12);
        group.Opacity(0.58);
        identity.Children().Append(name);
        identity.Children().Append(group);
        Grid::SetColumn(identity, 1);
        layout.Children().Append(identity);

        TextBlock mode;
        mode.Text(variant_mode_label(item.status.failed ? item.status.failedMode : item.status.requestedMode));
        mode.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(mode, 2);
        layout.Children().Append(mode);

        StackPanel state;
        state.Spacing(5);
        state.VerticalAlignment(VerticalAlignment::Center);
        TextBlock stateText;
        stateText.FontSize(12);
        state.Children().Append(stateText);
        ProgressBar progress;
        progress.Minimum(0);
        progress.Maximum(100);
        progress.Height(4);
        state.Children().Append(progress);
        Grid::SetColumn(state, 3);
        layout.Children().Append(state);

        StackPanel actions;
        actions.Orientation(Orientation::Horizontal);
        actions.Spacing(8);
        actions.VerticalAlignment(VerticalAlignment::Center);
        Button pause;
        pause.Click([action = std::move(pauseAction)](auto const& sender, auto const&) {
            auto button = sender.template as<Button>();
            action(!unbox_value_or<bool>(button.Tag(), false));
        });
        Button cancel;
        cancel.Content(box_value(L"取消"));
        cancel.Click([action = std::move(cancelAction)](auto const&, auto const&) { action(); });
        actions.Children().Append(pause);
        actions.Children().Append(cancel);
        Grid::SetColumn(actions, 4);
        layout.Children().Append(actions);

        task.Child(layout);
        VariantTaskCard card{ task, stateText, progress, pause, cancel };
        update_variant_task_card(card, item, waitingForPower);
        return card;
    }

    Border create_variant_task_card(VariantMediaSummary const& item,
        std::filesystem::path const& cover, bool waitingForPower,
        VariantPauseAction pauseAction, VariantCancelAction cancelAction)
    {
        return create_variant_task_card_view(item, cover, waitingForPower,
            std::move(pauseAction), std::move(cancelAction)).root;
    }
}
