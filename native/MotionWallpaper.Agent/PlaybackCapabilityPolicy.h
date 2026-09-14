#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace motion::agent
{
    inline constexpr std::string_view cpu_smooth_mode = "cpu-smooth";

    struct SoftwarePlaybackProfile
    {
        bool enabled{};
        uint32_t width{};
        uint32_t height{};
        uint32_t frameRate{};
    };

    [[nodiscard]] constexpr bool uses_software_playback(
        std::string_view decodeMode, bool physicalVideoDeviceAvailable,
        bool sourceHardwareDecodeAvailable = true) noexcept
    {
        if (decodeMode == "software") return true;
        if (decodeMode == "hardware") return false;
        return !physicalVideoDeviceAvailable || !sourceHardwareDecodeAvailable;
    }

    // Static decoder-profile probing is necessary but some drivers still
    // advertise combinations that fail when Media Foundation starts the real
    // stream. Only automatic routes with a concrete probed adapter may feed
    // that runtime failure back into cpu-smooth selection. Explicit hardware
    // mode remains strict, and failures after playback has started remain
    // ordinary renderer/device recovery events.
    [[nodiscard]] constexpr bool automatic_decode_failure_requires_cpu_smooth(
        std::string_view decodeMode,
        std::string_view path,
        std::string_view reason,
        bool hasProbedAdapter) noexcept
    {
        if (decodeMode != "auto" || !hasProbedAdapter) return false;
        if (path == "software-fallback") return true;
        if (path != "unavailable") return false;
        return reason == "automatic-media-startup" ||
            reason == "automatic-first-frame-timeout" ||
            reason == "no-d3d11-video-device";
    }

    [[nodiscard]] constexpr SoftwarePlaybackProfile software_playback_profile(
        bool enabled,
        uint32_t logicalProcessors,
        uint32_t displayWidth,
        uint32_t displayHeight,
        uint32_t displayRefreshRate) noexcept
    {
        if (!enabled) return {};

        uint32_t maximumWidth{};
        uint32_t maximumHeight{};
        uint32_t frameRate{};
        if (logicalProcessors >= 12) {
            maximumWidth = 1920;
            maximumHeight = 1080;
            frameRate = 60;
        } else if (logicalProcessors >= 6) {
            maximumWidth = 1280;
            maximumHeight = 720;
            frameRate = 60;
        } else if (logicalProcessors >= 4) {
            maximumWidth = 1280;
            maximumHeight = 720;
            frameRate = 30;
        } else {
            maximumWidth = 854;
            maximumHeight = 480;
            frameRate = 30;
        }

        if (displayHeight > displayWidth) std::swap(maximumWidth, maximumHeight);
        if (!displayWidth || !displayHeight) {
            displayWidth = maximumWidth;
            displayHeight = maximumHeight;
        }
        if (displayRefreshRate) frameRate = (std::min)(frameRate, displayRefreshRate);
        frameRate = (std::max)(1u, frameRate);

        if (displayWidth <= maximumWidth && displayHeight <= maximumHeight) {
            return { true, displayWidth, displayHeight, frameRate };
        }

        uint32_t width{};
        uint32_t height{};
        if (static_cast<uint64_t>(maximumWidth) * displayHeight <=
            static_cast<uint64_t>(maximumHeight) * displayWidth) {
            width = maximumWidth;
            height = static_cast<uint32_t>((static_cast<uint64_t>(displayHeight) * maximumWidth +
                displayWidth - 1) / displayWidth);
        } else {
            height = maximumHeight;
            width = static_cast<uint32_t>((static_cast<uint64_t>(displayWidth) * maximumHeight +
                displayHeight - 1) / displayHeight);
        }
        width = (std::max)(2u, (width + 1u) & ~1u);
        height = (std::max)(2u, (height + 1u) & ~1u);
        return { true, width, height, frameRate };
    }
}
