#pragma once

#include <cstdint>
#include <optional>

namespace motion::agent
{
    struct RuntimeSessionState
    {
        bool connected{ true };
        // Until a real lock-state query or an unlock event succeeds, keep
        // presentation stopped. Connection alone never proves an unlock.
        bool locked{ true };

        [[nodiscard]] constexpr bool Inactive() const noexcept { return !connected || locked; }

        constexpr void Observe(std::optional<bool> connection,
            std::optional<bool> lockState) noexcept
        {
            if (connection) connected = *connection;
            if (lockState) locked = *lockState;
        }

        constexpr void Notify(uint32_t event) noexcept
        {
            switch (event) {
            case 0x1: // WTS_CONSOLE_CONNECT
            case 0x3: // WTS_REMOTE_CONNECT
            case 0x5: // WTS_SESSION_LOGON
            case 0xF: // WTS_SESSION_DESKTOP_READY
                connected = true;
                break;
            case 0x2: // WTS_CONSOLE_DISCONNECT
            case 0x4: // WTS_REMOTE_DISCONNECT
            case 0x6: // WTS_SESSION_LOGOFF
                connected = false;
                break;
            case 0x7: // WTS_SESSION_LOCK
                locked = true;
                break;
            case 0x8: // WTS_SESSION_UNLOCK
                locked = false;
                break;
            }
        }
    };

    struct RuntimeSystemEventEffect
    {
        bool handled{};
        bool topologyChanged{};
        bool inputActivity{};
        bool resumed{};
        bool shellRestarted{};
        bool interactiveResume{};
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
            return { true, true, false, true, false, parameter == pbtApmResumeSuspend };
        }
        return {};
    }

    inline void apply_runtime_system_event_effect(
        RuntimeSystemEventEffect const& effect, uint64_t& topologyRevision,
        uint64_t& inputRevision, bool& displayOn) noexcept
    {
        if (effect.topologyChanged) ++topologyRevision;
        if (effect.inputActivity) ++inputRevision;
        // Timer/network wake does not mean the user or session display woke.
        if (effect.resumed && effect.interactiveResume) displayOn = true;
    }
}
