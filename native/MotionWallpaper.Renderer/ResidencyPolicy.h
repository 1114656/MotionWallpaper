#pragma once

#include <cstdint>

namespace motion::renderer
{
    inline constexpr uint32_t residency_pause_grace_ms = 30'000;
    inline constexpr uint32_t residency_release_delay_ms = 250;
    inline constexpr uint32_t residency_retry_ms = 30'000;

    // A frozen desktop needs only its captured image. Brief coverage pauses
    // retain the decoder for a bounded resume window, even with ample RAM.
    class IdleResidencyPolicy
    {
    public:
        void Begin(uint64_t now, bool staticDesktop, bool lowMemory) noexcept
        {
            auto deadline = now + (staticDesktop || lowMemory
                ? residency_release_delay_ms : residency_pause_grace_ms);
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
