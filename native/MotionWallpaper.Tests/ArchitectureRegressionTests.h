#pragma once
#include "../MotionWallpaper.Agent/RendererLifecyclePolicy.h"
#include "../MotionWallpaper.Agent/VideoVariantPolicy.h"
#include "../MotionWallpaper.Renderer/FrameTiming.h"
#include "../MotionWallpaper.Protocol/RendererProtocol.h"

namespace motion::tests
{
    template<class Require> void failed_display_does_not_pin_unrelated_renderer_generations(Require require)
    {
        agent::RendererRetirementPolicy policy;
        using DisplayMap = std::map<std::string, std::wstring>;
        policy.Keep(DisplayMap{{"one", L"a"}, {"two", L"b"}}, {L"a", L"b"}, 0);
        auto keep = policy.Keep(DisplayMap{{"one", L"c"}, {"two", L"broken"}}, {L"c"}, 100);
        require(!keep.contains(L"a") && keep.contains(L"b"), "a failed peer kept a successfully replaced route alive");
        for (uint64_t index = 1; index <= 50; ++index) {
            auto candidate = L"candidate-" + std::to_wstring(index);
            keep = policy.Keep(DisplayMap{{"one", candidate}, {"two", L"broken"}}, {}, 100 + index);
            require(keep.size() == 4 && keep.contains(L"c") && keep.contains(L"b"),
                "rapid replacement retained unconfirmed candidate generations");
        }
        keep = policy.Keep(DisplayMap{{"one", L"last"}, {"two", L"broken"}}, {}, 12'500);
        require(keep == std::set<std::wstring>{L"last", L"broken"}, "fallback retirement has no deadline");
        keep = policy.Keep(DisplayMap{{"one", L"last"}}, {L"last"}, 13'000);
        require(keep == std::set<std::wstring>{L"last"}, "unplugged displays retained their route");
        policy.Clear();
        require(policy.Keep({}, {}, 14'000).empty(), "stopping the pool retained predecessors");
    }

    template<class Require> void playback_watchdog_distinguishes_stalls_stale_reports_and_slow_media(Require require)
    {
        using agent::PlaybackHealthFailure;
        agent::PlaybackHealthMonitor monitor;
        monitor.Reset(7, 0);
        monitor.Observe(7, 1, 333'334, 1000);
        for (uint64_t now = 2000; now < 16'000; now += 1000) {
            monitor.Observe(7, 1, 333'334, now); // responsive but stuck after first ACK
        }
        require(monitor.Check(16'000) == PlaybackHealthFailure::Frames, "a live heartbeat hid a stalled video");
        monitor.Reset(8, 20'000);
        monitor.Observe(7, 900, 333'334, 29'999);
        require(monitor.Check(30'000) == PlaybackHealthFailure::Heartbeat, "old command progress revived the current target");
        monitor.Reset(9, 40'000);
        for (uint64_t now = 41'000; now <= 60'000; now += 1000) monitor.Observe(9, 1, 100'000'000, now);
        require(monitor.Check(60'000) == PlaybackHealthFailure::None, "low frame-rate input was treated as stuck");
        monitor.Observe(9, 2, 100'000'000, 61'000);
        require(monitor.Check(61'000) == PlaybackHealthFailure::None, "new frames failed to reset the watchdog");
        agent::PlaybackRecoveryBudget budget;
        require(!budget.Exhausted(0) && !budget.Exhausted(20'000) && budget.Exhausted(40'000),
            "repeated stalls were allowed to restart forever");
        require(!budget.Exhausted(300'000), "recovery window did not expire");
        auto heartbeat = protocol::parse_playback_heartbeat("status playback 9 3 166833");
        require(heartbeat && heartbeat->revision == 9 && heartbeat->serial == 3, "valid playback progress was rejected");
        require(protocol::parse_playback_heartbeat("status playback 9 3 166833\r").has_value(),
            "Windows CRLF broke playback progress parsing");
        for (auto malformed : {"status playback 0 1 2", "status playback 1 -1 2", "status playback 1 2 0",
                "status playback 1 2 166833 extra", "status playback 1 18446744073709551616 166833"}) {
            require(!protocol::parse_playback_heartbeat(malformed), "invalid heartbeat altered watchdog state");
        }
    }

    template<class Require> void frame_deadlines_account_for_processing_without_drift_or_bursts(Require require)
    {
        renderer::FrameDeadline clock;
        constexpr int64_t period = 166'667;
        int64_t started = 1'000'000;
        for (int index = 1; index <= 600; ++index) {
            auto next = clock.AfterFrame(started, started + 30'000, period);
            require(next == 1'000'000 + index * period, "3ms processing accumulated into the 60 FPS cadence");
            started = next;
        }
        auto afterStall = clock.AfterFrame(started, started + 4 * period + 1, period);
        require(afterStall > started + 4 * period && afterStall <= started + 5 * period,
            "a missed deadline caused catch-up bursts or unnecessary full-frame delay");
        clock.Reset();
        require(clock.AfterFrame(100, 200, 166'833) == 166'933, "59.94 FPS was rounded to integer milliseconds");
        require(clock.AfterFrame(166'933, 167'000, 333'334) == 500'267, "frame-rate change retained the old period");
    }

    template<class Require> void fill_geometry_preserves_visible_pixels_and_keeps_distinct_aspects(Require require)
    {
        require(agent::unchanged_legacy_fill_variant(L"balanced-90-1920x1080-v7.mp4", 3840, 2160, 1920, 1080) ==
            L"balanced-90-1920x1080-v6.mp4", "unchanged copies unnecessarily require a full re-encode");
        require(agent::unchanged_legacy_fill_variant(L"balanced-90-1080x1920-v7.mp4", 3840, 2160, 1080, 1920).empty(),
            "a differently cropped legacy copy was reused");
        require(agent::video_sdr_variant_dimensions("balanced", 3840, 2160, 3440, 1440) == std::pair{3440u, 1440u},
            "ultrawide fill was reduced to 2560 pixels before being enlarged");
        require(agent::video_sdr_variant_dimensions("balanced", 3840, 2160, 1080, 1920) == std::pair{1080u, 1920u},
            "portrait fill lost its available vertical detail");
        require(agent::video_sdr_variant_dimensions("balanced", 640, 360, 1080, 1920) == std::pair{202u, 360u},
            "fill upscaled a small source during transcoding");
        require(agent::video_display_aspect(3840, 2160) == agent::video_display_aspect(2560, 1440) &&
            agent::video_display_aspect(3840, 2160) != agent::video_display_aspect(1080, 1920),
            "landscape and portrait monitors share a destructively cropped copy");
    }
}
