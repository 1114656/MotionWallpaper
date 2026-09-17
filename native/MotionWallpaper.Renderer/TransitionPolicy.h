#pragma once

#include <cstdint>

namespace motion::renderer
{
    enum class Command : uintptr_t { Unknown, DesktopPlay, DesktopFreeze, ScreensaverPlay, Pause, Stop };
    enum class PresentationMode { Desktop, Screensaver };

    [[nodiscard]] constexpr bool presentation_hides_cursor(
        PresentationMode mode) noexcept
    {
        return mode == PresentationMode::Screensaver;
    }

    [[nodiscard]] constexpr bool leaves_screensaver(
        PresentationMode current, Command command) noexcept
    {
        return current == PresentationMode::Screensaver &&
            command != Command::ScreensaverPlay && command != Command::Stop;
    }
}
