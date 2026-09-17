#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace motion::agent
{
    enum class TrayStatus
    {
        Starting,
        Applying,
        Playing,
        Frozen,
        Paused,
        Optimizing,
        Screensaver,
        Stopped,
        Locked,
        DisplayOff,
        Failed,
        Unavailable
    };

    [[nodiscard]] constexpr wchar_t const* tray_status_text(TrayStatus status) noexcept
    {
        switch (status) {
        case TrayStatus::Starting: return L"正在启动";
        case TrayStatus::Applying: return L"正在应用";
        case TrayStatus::Playing: return L"正在播放";
        case TrayStatus::Frozen: return L"已冻结";
        case TrayStatus::Paused: return L"已暂停";
        case TrayStatus::Optimizing: return L"正在优化";
        case TrayStatus::Screensaver: return L"屏保运行中";
        case TrayStatus::Stopped: return L"未运行";
        case TrayStatus::Locked: return L"系统已锁定";
        case TrayStatus::DisplayOff: return L"显示器已关闭";
        case TrayStatus::Failed: return L"失败";
        default: return L"不可用";
        }
    }

    [[nodiscard]] constexpr TrayStatus tray_status_with_renderer_health(
        TrayStatus requested, bool rendererFailed) noexcept
    {
        return rendererFailed ? TrayStatus::Failed : requested;
    }

    // The event is always consumed by RuntimeEvents. While manually paused it
    // is deliberately ignored, so it cannot become a delayed surprise change
    // when playback resumes.
    [[nodiscard]] constexpr bool tray_next_wallpaper_should_advance(
        bool requested, bool manuallyPaused) noexcept
    {
        return requested && !manuallyPaused;
    }

    class TrayControlState
    {
    public:
        void TogglePlayback() noexcept
        {
            manuallyPaused_ = !manuallyPaused_;
            if (manuallyPaused_) previewActive_ = false;
        }

        [[nodiscard]] bool ManuallyPaused() const noexcept { return manuallyPaused_; }

        void RequestScreensaverPreview(uint32_t inputTick, uint64_t inputRevision) noexcept
        {
            previewActive_ = true;
            previewInputTick_ = inputTick;
            previewInputRevision_ = inputRevision;
        }

        [[nodiscard]] bool ObserveInput(uint32_t inputTick, uint64_t inputRevision) noexcept
        {
            if (!previewActive_ ||
                (previewInputTick_ == inputTick && previewInputRevision_ == inputRevision)) return false;
            previewActive_ = false;
            return true;
        }

        void CancelScreensaverPreview() noexcept { previewActive_ = false; }
        [[nodiscard]] bool ScreensaverPreviewActive() const noexcept { return previewActive_; }

    private:
        bool manuallyPaused_{};
        bool previewActive_{};
        uint32_t previewInputTick_{};
        uint64_t previewInputRevision_{};
    };

    [[nodiscard]] inline std::string next_media_id(
        std::vector<std::string> ids, std::string_view current)
    {
        if (ids.empty()) return {};
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        auto found = std::find(ids.begin(), ids.end(), current);
        if (found == ids.end() || ++found == ids.end()) return ids.front();
        return *found;
    }
}
