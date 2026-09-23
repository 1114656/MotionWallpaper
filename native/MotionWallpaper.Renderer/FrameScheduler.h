#pragma once

#include "../MotionWallpaper.Common/UniqueHandle.h"
#include "FrameTiming.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <thread>

namespace motion::renderer
{
    class FrameScheduler
    {
    public:
        explicit FrameScheduler(UINT message) : message_(message)
        {
            stopEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            updateEvent_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
            timer_.reset(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS));
            if (!timer_) timer_.reset(CreateWaitableTimerW(nullptr, FALSE, nullptr));
            if (stopEvent_ && updateEvent_ && timer_) worker_ = std::thread([this] { Run(); });
        }

        ~FrameScheduler() { Shutdown(); }
        FrameScheduler(FrameScheduler const&) = delete;
        FrameScheduler& operator=(FrameScheduler const&) = delete;

        void Start(HWND target, UINT interval)
        {
            StartAt(target, Now100ns() + static_cast<int64_t>((std::max)(1u, interval)) * 10'000);
        }

        static int64_t Now100ns() noexcept
        {
            LARGE_INTEGER counter{}, frequency{};
            QueryPerformanceCounter(&counter);
            QueryPerformanceFrequency(&frequency);
            return (counter.QuadPart / frequency.QuadPart) * 10'000'000 +
                (counter.QuadPart % frequency.QuadPart) * 10'000'000 / frequency.QuadPart;
        }

        void StartAt(HWND target, int64_t deadline100ns)
        {
            target_.store(target, std::memory_order_release);
            deadline_.store(deadline100ns, std::memory_order_release);
            running_.store(true, std::memory_order_release);
            if (updateEvent_) SetEvent(updateEvent_.get());
        }

        void Stop()
        {
            running_.store(false, std::memory_order_release);
            tickPending_.store(false, std::memory_order_release);
            if (timer_) CancelWaitableTimer(timer_.get());
            if (updateEvent_) SetEvent(updateEvent_.get());
        }

        void TickHandled() { tickPending_.store(false, std::memory_order_release); }

    private:
        void Shutdown()
        {
            if (!worker_.joinable()) return;
            SetEvent(stopEvent_.get());
            worker_.join();
        }

        void Run()
        {
            HANDLE handles[]{ stopEvent_.get(), updateEvent_.get(), timer_.get() };
            for (;;) {
                DWORD count = running_.load(std::memory_order_acquire) ? ARRAYSIZE(handles) : 2;
                DWORD result = WaitForMultipleObjects(count, handles, FALSE, INFINITE);
                if (result == WAIT_OBJECT_0) return;
                if (result == WAIT_OBJECT_0 + 1) {
                    CancelWaitableTimer(timer_.get());
                    if (!running_.load(std::memory_order_acquire)) continue;
                    LARGE_INTEGER due{};
                    due.QuadPart = -(std::max)(int64_t{1},
                        deadline_.load(std::memory_order_acquire) - Now100ns());
                    SetWaitableTimerEx(timer_.get(), &due, 0, nullptr, nullptr, nullptr, 0);
                    continue;
                }
                if (result == WAIT_OBJECT_0 + 2 && running_.exchange(false, std::memory_order_acq_rel)) {
                    if (!tickPending_.exchange(true, std::memory_order_acq_rel)) {
                        HWND target = target_.load(std::memory_order_acquire);
                        if (!target || !PostMessageW(target, message_, 0, 0)) {
                            tickPending_.store(false, std::memory_order_release);
                        }
                    }
                }
            }
        }

        UINT message_{};
        motion::unique_handle stopEvent_;
        motion::unique_handle updateEvent_;
        motion::unique_handle timer_;
        std::thread worker_;
        std::atomic<HWND> target_{};
        std::atomic_int64_t deadline_{};
        std::atomic_bool running_{};
        std::atomic_bool tickPending_{};
    };
}
