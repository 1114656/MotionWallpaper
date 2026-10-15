#pragma once

#include <cstdint>

namespace motion::renderer
{
    inline constexpr uint32_t residency_pause_grace_ms = 30'000;
    inline constexpr uint32_t residency_desktop_grace_ms = 5'000;
    inline constexpr uint32_t residency_release_delay_ms = 250;
    inline constexpr uint32_t residency_retry_ms = 30'000;

    enum class IdleResidencyReason { Covered, DesktopFrozen, ReleasePromptly };

    // Pausing stops decoding immediately. Retaining those idle resources for
    // a bounded window lets quick toggles resume without opening/seeking again.
    class IdleResidencyPolicy
    {
    public:
        void Begin(uint64_t now, IdleResidencyReason reason, bool lowMemory) noexcept
        {
            auto delay = lowMemory || reason == IdleResidencyReason::ReleasePromptly
                ? residency_release_delay_ms : reason == IdleResidencyReason::DesktopFrozen
                ? residency_desktop_grace_ms : residency_pause_grace_ms;
            auto deadline = now + delay;
            if (!active_ || deadline < deadline_) deadline_ = deadline;
            active_ = true; // Repeated pause commands cannot extend the window.
        }
        void Cancel() noexcept { active_ = false; }
        void Retry(uint64_t now) noexcept { deadline_ = now + residency_retry_ms; active_ = true; }
        [[nodiscard]] bool Active() const noexcept { return active_; }
        [[nodiscard]] bool Due(uint64_t now) const noexcept { return active_ && now >= deadline_; }
        [[nodiscard]] uint32_t Delay(uint64_t now) const noexcept
        {
            return !active_ || now >= deadline_ ? 1u : static_cast<uint32_t>(deadline_ - now);
        }
    private:
        bool active_{};
        uint64_t deadline_{};
    };
}
