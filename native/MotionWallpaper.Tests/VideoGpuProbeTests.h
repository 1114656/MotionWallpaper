#pragma once

#include "../MotionWallpaper.Agent/VideoGpuProbe.h"
#include "../MotionWallpaper.Agent/PlaybackCapabilityPolicy.h"
#include "../MotionWallpaper.Common/VariantCache.h"
#include <stdexcept>
#include <fstream>

namespace motion::tests
{
    inline void video_gpu_probe_contracts()
    {
        using namespace motion::agent::gpu_probe_detail;
        auto check = [](bool value, char const* message) {
            if (!value) throw std::runtime_error(message);
        };
        using motion::agent::automatic_decode_failure_action;
        auto originalTimeout = automatic_decode_failure_action("auto", "unavailable",
            "original-first-frame-timeout", true, true, false, true);
        auto startupCrash = automatic_decode_failure_action("auto", "probing",
            "detecting", true, true, false, true);
        auto exhausted = automatic_decode_failure_action("auto", "probing",
            "detecting", true, true, false, false);
        auto playbackCrash = automatic_decode_failure_action("auto", "hardware",
            "playing", true, true, true, true);
        auto strictHardware = automatic_decode_failure_action("hardware", "unavailable",
            "original-first-frame-timeout", true, true, false, true);
        auto unprobed = automatic_decode_failure_action("auto", "unavailable",
            "no-d3d11-video-device", false, true, false, true);
        check(originalTimeout.rejectAdapter && originalTimeout.tryAlternative &&
            startupCrash.rejectAdapter && startupCrash.tryAlternative &&
            exhausted.rejectAdapter && !exhausted.tryAlternative,
            "startup failure made original playback terminal before trying the next GPU");
        check(!playbackCrash.rejectAdapter && !playbackCrash.tryAlternative &&
            !strictHardware.rejectAdapter && !unprobed.rejectAdapter,
            "a later playback crash or explicit decode choice incorrectly rejected a startup GPU route");
        auto inventory = parse_inventory(std::string("gpu-v1 1 2\n4318 100 0 1 2 570 1 \\\\.\\DISPLAY1\n32902 0 1 3 4 999 0\n"));
        auto reordered = parse_inventory(std::string("gpu-v1 1 2\n32902 0 0 3 4 999 0\n4318 100 1 1 2 570 1 \\\\.\\DISPLAY1\n"));
        auto changedDriver = parse_inventory(std::string("gpu-v1 1 2\n4318 100 0 1 2 571 1 \\\\.\\DISPLAY1\n32902 0 1 3 4 999 0\n"));
        check(inventory.succeeded && inventory.adapters.size() == 2 &&
            inventory.displayAdapters.at(L"\\\\.\\DISPLAY1") == L"1:2", "GPU probe lost adapter/display identity");
        check(inventory.environment == reordered.environment && inventory.environment != changedDriver.environment,
            "GPU environment identity depends on ordinal or ignores driver replacement");
        auto cudaAdapter = inventory.adapters.front();
        check(motion::agent::parse_video_cuda_device(std::string("cuda-v1 3 1 2\n"), cudaAdapter) == 3u,
            "CUDA device mapping assumed that CUDA and DXGI ordinals are identical");
        for (auto invalid : { "cuda-v1 0 9 2", "cuda-v1 0 1 9", "cuda-v1 -1 1 2",
            "cuda-v1 64 1 2", "cuda-v1 3 1 2 trailing", "cuda-v1 3 1", "cuda-v2 3 1 2" }) {
            check(!motion::agent::parse_video_cuda_device(std::string(invalid), cudaAdapter),
                "invalid CUDA identity mapping was accepted");
        }
        cudaAdapter.identityKnown = false;
        check(!motion::agent::parse_video_cuda_device(std::string("cuda-v1 3 1 2"), cudaAdapter) &&
            !motion::agent::parse_video_cuda_device(std::nullopt, inventory.adapters.front()),
            "missing CUDA identity was treated as a safe adapter binding");
        check(!parse_inventory(std::string("gpu-v1 1 999\n")).succeeded &&
            !parse_inventory(std::string("gpu-v1 1 0\ntrailing")).succeeded &&
            !parse_decode(std::string("decode-v2\n0 not-an-adapter")).succeeded &&
            !parse_decode(std::string("decode-v2\n0")).succeeded &&
            !parse_decode(std::string("decode-v2\n64 1:2")).succeeded,
            "malformed GPU child output was accepted");
        check(!motion::variant_failure_context_matches("balanced", "balanced", "new") &&
            motion::variant_failure_context_matches("balanced\nv1|2K90|source1|driver1", "balanced", "v1|2K90|source1|driver1") &&
            !motion::variant_failure_context_matches("balanced\nv1|2K90|source1|driver1", "balanced", "v1|1080p60|source1|driver1") &&
            !motion::variant_failure_context_matches("balanced\nv1|2K90|source1|driver1", "balanced", "v1|2K90|source1|driver2") &&
            !motion::variant_failure_context_matches("balanced\nv1|2K90|source1|driver1", "balanced", "v1|2K90|source2|driver1") &&
            motion::variant_failure_mode("balanced\nv1|2K90|source1|driver1") == "balanced",
            "persistent GPU failure survived target/source/environment replacement or broke UI mode parsing");

        std::atomic_bool entered{}, release{};
        std::atomic_uint firstAttempts{}, secondAttempts{};
        AsyncProbe service([&](std::vector<std::wstring> const& arguments, uint32_t timeout,
            std::function<bool()> const& cancelled) -> std::optional<std::string> {
            if (arguments.front() == L"hold") {
                entered = true;
                while (!release && !cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return "obsolete";
            }
            if (arguments.front() == L"fresh") return "fresh";
            if (arguments.front() == L"--probe-gpu-decode" && timeout == 12000 && arguments.size() == 6) {
                if (arguments.back() == L"1:1") { ++firstAttempts; return std::nullopt; }
                if (arguments.back() == L"2:2") { ++secondAttempts; return "decode-v2\n1 2:2\n"; }
            }
            return std::nullopt;
        });
        auto waitUntil = [&](auto&& predicate) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!predicate()) {
                if (std::chrono::steady_clock::now() >= deadline) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return true;
        };
        auto lease = std::make_shared<int>(1);
        std::weak_ptr<int> weakLease = lease;
        auto started = std::chrono::steady_clock::now();
        auto initial = service.Read(L"old", { L"hold" }, lease);
        lease.reset();
        check(initial.pending && std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500),
            "main-thread GPU read waited for the driver job");
        check(waitUntil([&] { return entered.load(); }), "GPU background probe never started");
        check(service.Quiesce(2000) && weakLease.expired(), "probe quiescence retained an active library lease");
        service.Invalidate();
        check(waitUntil([&] { return !service.Read(L"old", { L"fresh" }).pending; }), "new probe did not replace cancelled state");
        check(service.Read(L"old", { L"fresh" }).output == std::optional<std::string>("fresh"),
            "cancelled probe resurrected a stale result");
        std::vector<std::wstring> decode{ L"--probe-gpu-decode", L"ffmpeg", L"source", L"none", L"100", L"1:1", L"2:2" };
        check(waitUntil([&] { return !service.Read(L"decode", decode).pending; }), "multi-GPU probe did not finish");
        auto result = parse_decode(service.Read(L"decode", decode).output);
        check(result.succeeded && result.adapters == std::vector<std::wstring>{ L"2:2" } &&
            firstAttempts == 1 && secondAttempts == 1, "one failing adapter concealed a working second GPU");

        // On a muxless system the display-attached iGPU can launch first, but
        // heavy media must retain the child's complete DXGI high-performance
        // ranking after the independently isolated probes finish.
        check(motion::renderer::prefer_high_performance_adapter(3840, 2160, 60, 1),
            "4K60 no longer uses the high-performance GPU preference");
        AsyncProbe heavyService([](std::vector<std::wstring> const& arguments, uint32_t,
            std::function<bool()> const&) -> std::optional<std::string> {
            if (arguments.back() == L"1:1") return "decode-v2\n1 1:1\n";
            if (arguments.back() == L"2:2") return "decode-v2\n0 2:2\n";
            return std::nullopt;
        });
        auto heavyDecode = decode;
        heavyDecode[3] = L"1:1";
        heavyDecode[4] = L"8294400";
        check(waitUntil([&] { return !heavyService.Read(L"heavy", heavyDecode).pending; }),
            "heavy multi-GPU probe did not finish");
        auto heavyOutput = heavyService.Read(L"heavy", heavyDecode).output;
        check(heavyOutput == std::optional<std::string>("decode-v2\n0 2:2\n1 1:1\n") &&
            parse_decode(heavyOutput).adapters == std::vector<std::wstring>{ L"2:2", L"1:1" },
            "isolated GPU aggregation replaced DXGI performance rank with display or launch order");

        auto actualInventory = motion::agent::video_gpu_inventory_bounded();
        check(actualInventory.succeeded && !actualInventory.pending,
            "the real GPU inventory CLI did not return a validated child-process result");
        for (bool cancel : { false, true }) {
            auto marker = std::filesystem::temp_directory_path() /
                (L"MotionWallpaper-gpu-probe-" + motion::utf8_to_wide(motion::new_id()) + L".pid");
            struct RemoveMarker { std::filesystem::path value; ~RemoveMarker() { std::error_code ignored; std::filesystem::remove(value, ignored); } } cleanup{ marker };
            AsyncProbe realService([&](std::vector<std::wstring> const& arguments, uint32_t,
                std::function<bool()> const& cancelled) {
                return motion::media_tool_detail::run_bounded(executable(), arguments,
                    cancel ? 10000u : 1500u, cancelled);
            });
            std::vector<std::wstring> arguments{ L"--media-probe-test-child", L"hold", marker.wstring() };
            auto startedProcess = std::chrono::steady_clock::now();
            check(realService.Read(L"real", arguments).pending,
                "an asynchronous real child skipped its pending state");
            motion::unique_handle child;
            check(waitUntil([&] {
                std::ifstream file(marker);
                DWORD pid{};
                if (!(file >> pid) || !pid) return false;
                child.reset(OpenProcess(SYNCHRONIZE, FALSE, pid));
                return static_cast<bool>(child);
            }), "real asynchronous probe child did not start");
            if (cancel) check(realService.Quiesce(2000), "real asynchronous probe cancellation did not quiesce");
            else check(waitUntil([&] { return !realService.Read(L"real", arguments).pending; }),
                "real asynchronous probe timeout left its request pending");
            check(std::chrono::steady_clock::now() - startedProcess < std::chrono::seconds(5) &&
                WaitForSingleObject(child.get(), 2000) == WAIT_OBJECT_0,
                "real asynchronous probe deadline/cancellation left a child process alive");
        }
    }
}
