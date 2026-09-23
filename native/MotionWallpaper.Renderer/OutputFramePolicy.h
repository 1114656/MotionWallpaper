#pragma once

#include <cstdint>

namespace motion::renderer
{
    // A decode tick belongs to all outputs, but a busy swap chain must not
    // consume another output's opportunity to present it. Serial numbers are
    // local to the engine lifetime, so looped timestamps remain unambiguous.
    struct OutputFrameProgress
    {
        uint64_t presentedSerial{};
        bool freezeCaptured{};

        [[nodiscard]] constexpr bool NeedsFrame(uint64_t serial, bool freeze) const noexcept
        {
            return serial != 0 && (freeze ? !freezeCaptured : presentedSerial != serial);
        }

        constexpr void Presented(uint64_t serial, bool freeze) noexcept
        {
            presentedSerial = serial;
            if (freeze) freezeCaptured = true;
        }

        constexpr void BeginFreeze() noexcept { freezeCaptured = false; }

        [[nodiscard]] constexpr bool Ready(bool freeze) const noexcept
        {
            return freeze ? freezeCaptured : presentedSerial != 0;
        }
    };
}
