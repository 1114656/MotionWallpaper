#pragma once

#include <algorithm>
#include <cstdint>

namespace motion::renderer
{
    class FrameDeadline
    {
    public:
        void Reset() noexcept { next_ = period_ = 0; }
        [[nodiscard]] int64_t AfterFrame(int64_t started, int64_t finished, int64_t period) noexcept
        {
            period = (std::max)(int64_t{10'000}, period);
            if (!next_ || period_ != period) next_ = started + period;
            else next_ += period;
            period_ = period;
            if (next_ <= finished) next_ += ((finished - next_) / period + 1) * period;
            return next_;
        }
    private:
        int64_t next_{}, period_{};
    };
    inline constexpr uint32_t minimum_frame_wait_ms = 1;
    inline constexpr uint32_t maximum_frame_wait_ms = 1000;

    [[nodiscard]] constexpr uint32_t clamp_frame_wait_ms(uint32_t interval) noexcept
    {
        return (std::clamp)(interval, minimum_frame_wait_ms, maximum_frame_wait_ms);
    }

    // Waitable timers use negative 100 ns units for a relative deadline.
    [[nodiscard]] constexpr int64_t frame_due_time_100ns(uint32_t interval) noexcept
    {
        return -static_cast<int64_t>(clamp_frame_wait_ms(interval)) * 10'000;
    }

    // Wake shortly before the next decoded frame is expected. A miss is retried
    // after 2 ms by Renderer, so this cuts idle polling without ever sleeping a
    // complete frame and feeding a late presentation back into the estimator.
    [[nodiscard]] constexpr uint32_t presentation_probe_interval_ms(int64_t frameDuration100ns) noexcept
    {
        if (frameDuration100ns <= 0) return 4;
        auto threeQuarterFrameMs = static_cast<uint32_t>((frameDuration100ns * 3 + 20'000) / 40'000);
        return (std::clamp)(threeQuarterFrameMs, 3u, 24u);
    }
}
