#pragma once

#include <cstdint>

namespace motion::agent
{
    struct RuntimeSystemEventEffect
    {
        bool handled{};
        bool topologyChanged{};
        bool inputActivity{};
        bool resumed{};
        bool shellRestarted{};
    };

    // Keep Windows message classification independent from the hidden window
    // so the exact event contract can be exercised without suspending the test
    // machine or restarting Explorer. Values are stable Win32 ABI constants.
    [[nodiscard]] constexpr RuntimeSystemEventEffect runtime_system_event_effect(
        uint32_t message, uintptr_t parameter, uint32_t taskbarCreatedMessage,
        bool rawInputWakeEnabled) noexcept
    {
        constexpr uint32_t wmInput = 0x00FF;
        constexpr uint32_t wmDisplayChange = 0x007E;
        constexpr uint32_t wmPowerBroadcast = 0x0218;
        constexpr uintptr_t pbtApmResumeSuspend = 0x0007;
        constexpr uintptr_t pbtApmResumeAutomatic = 0x0012;

        if (taskbarCreatedMessage && message == taskbarCreatedMessage) {
            return { true, true, false, false, true };
        }
        if (message == wmDisplayChange) {
            return { true, true, false, false, false };
        }
        if (message == wmInput) {
            return { false, false, rawInputWakeEnabled, false, false };
        }
        if (message == wmPowerBroadcast &&
            (parameter == pbtApmResumeAutomatic || parameter == pbtApmResumeSuspend)) {
            return { true, true, false, true, false };
        }
        return {};
    }

    inline void apply_runtime_system_event_effect(
        RuntimeSystemEventEffect const& effect, uint64_t& topologyRevision,
        uint64_t& inputRevision, bool& displayOn) noexcept
    {
        if (effect.topologyChanged) ++topologyRevision;
        if (effect.inputActivity) ++inputRevision;
        if (effect.resumed) displayOn = true;
    }
}
