#pragma once

#include <cstdint>
#include <algorithm>
#include <string>
#include <vector>

namespace motion::agent
{
    struct WindowBounds
    {
        int32_t left{};
        int32_t top{};
        int32_t right{};
        int32_t bottom{};
    };

    [[nodiscard]] constexpr bool covers_display(WindowBounds const& window, WindowBounds const& display) noexcept
    {
        constexpr int32_t tolerance = 2;
        return display.right > display.left && display.bottom > display.top &&
            window.right > window.left && window.bottom > window.top &&
            window.left <= display.left + tolerance && window.top <= display.top + tolerance &&
            window.right >= display.right - tolerance && window.bottom >= display.bottom - tolerance;
    }

    struct DisplayCoverage
    {
        std::wstring deviceName;
        WindowBounds bounds;
        WindowBounds workArea;
        bool covered{};
    };

    inline void mark_covered_displays(WindowBounds const& window,
        std::vector<DisplayCoverage>& displays)
    {
        // Compare against every physical display: a spanning window can cover
        // more than one, while a maximized window covers only its own output.
        for (auto& display : displays) {
            display.covered = display.covered || covers_display(window, display.bounds) ||
                covers_display(window, display.workArea);
        }
    }

    struct CoverageSelection
    {
        std::vector<std::wstring> coveredDevices;
        bool allCovered{};
    };

    [[nodiscard]] inline CoverageSelection select_display_coverage(
        std::vector<DisplayCoverage> const& displays,
        std::vector<std::wstring> const& targets)
    {
        CoverageSelection result;
        result.allCovered = !targets.empty();
        for (auto const& target : targets) {
            bool covered = std::any_of(displays.begin(), displays.end(), [&](auto const& display) {
                return display.deviceName == target && display.covered;
            });
            if (covered) result.coveredDevices.push_back(target);
            else result.allCovered = false;
        }
        return result;
    }
}
