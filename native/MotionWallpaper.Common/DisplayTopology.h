#pragma once

#include <windows.h>

#include <cstdint>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

namespace motion
{
    struct DisplayRefreshRate
    {
        uint32_t numerator{};
        uint32_t denominator{ 1 };

        bool operator==(DisplayRefreshRate const&) const = default;
    };

    [[nodiscard]] constexpr DisplayRefreshRate normalized_display_refresh_rate(
        DisplayRefreshRate rate) noexcept
    {
        // Drivers can return zero/one for an unknown timing. Keep that distinct
        // from a usable rate so callers can retain their legacy fallback.
        if (!rate.denominator || rate.numerator <= rate.denominator) return {};
        auto divisor = std::gcd(rate.numerator, rate.denominator);
        return { rate.numerator / divisor, rate.denominator / divisor };
    }

    [[nodiscard]] inline std::wstring display_refresh_rate_key(DisplayRefreshRate rate)
    {
        rate = normalized_display_refresh_rate(rate);
        return std::to_wstring(rate.numerator) + L"/" + std::to_wstring(rate.denominator);
    }

    struct DisplayTarget
    {
        std::string id;
        std::wstring deviceName;
        std::wstring friendlyName;
        RECT bounds{};
        uint32_t refreshRateHz{ 60 };
        bool primary{};
        // Physical signal timing (path timing if unavailable), including
        // fractional Hz. refreshRateHz stays the existing integer profile
        // input; this field partitions playback.
        DisplayRefreshRate refreshRate;
    };

    [[nodiscard]] inline DisplayRefreshRate effective_display_refresh_rate(
        DisplayTarget const& display) noexcept
    {
        auto rate = normalized_display_refresh_rate(display.refreshRate);
        if (rate.numerator) return rate;
        rate = normalized_display_refresh_rate({ display.refreshRateHz, 1 });
        return rate.numerator ? rate : DisplayRefreshRate{ 60, 1 };
    }

    [[nodiscard]] std::optional<DisplayRefreshRate> display_path_refresh_rate(
        DISPLAYCONFIG_PATH_INFO const& path, std::vector<DISPLAYCONFIG_MODE_INFO> const& modes) noexcept;
    [[nodiscard]] std::vector<DisplayTarget> enumerate_displays();
    [[nodiscard]] std::optional<RECT> find_display_bounds(std::wstring const& deviceName) noexcept;
    [[nodiscard]] RECT primary_display_bounds() noexcept;
}
