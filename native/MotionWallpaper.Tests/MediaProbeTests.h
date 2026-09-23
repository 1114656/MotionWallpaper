#pragma once

#include "../MotionWallpaper.Common/Common.h"
#include "../MotionWallpaper.Common/MediaProbe.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace motion::tests
{
    inline bool media_probe_json_fixture();

    // Call at the very beginning of Tests.exe's wmain, before apartment setup.
    // These child-only commands never enter the ordinary test suite.
    inline std::optional<int> handle_media_probe_test_command(int argc, wchar_t** argv)
    {
        if (argc < 2 || std::wstring_view(argv[1]) != L"--media-probe-test-child") return {};
        if (argc < 3) return 64;
        auto mode = std::wstring_view(argv[2]);
        if (mode == L"empty") return 0;
        if (mode == L"exit7") return 7;
        if (mode == L"json") return media_probe_json_fixture() ? 0 : 67;
        if (mode == L"stdout" || mode == L"oversize") {
            auto output = mode == L"stdout"
                ? std::string("begin:") + std::string(128 * 1024, 'x') + ":end"
                : std::string(300 * 1024, 'x');
            return std::fwrite(output.data(), 1, output.size(), stdout) == output.size() &&
                std::fflush(stdout) == 0 ? 0 : 65;
        }
        if (mode == L"hold" && argc == 4) {
            {
                std::ofstream marker(std::filesystem::path(argv[3]), std::ios::binary);
                marker << GetCurrentProcessId() << '\n';
                marker.flush();
                if (!marker) return 66;
            }
            // A broken Job cleanup cannot leave a permanent orphan in a test
            // run: the fixture eventually exits even if the assertion fails.
            Sleep(15'000);
            return 0;
        }
        return 64;
    }

    inline bool media_probe_json_fixture()
    {
        bool valid = true;
        // Each new thread starts without a COM/WinRT apartment. Repeated calls
        // reproduce the dangling global activation-factory cache regression;
        // the test runner's own MTA must not mask that lifecycle.
        for (int workerIndex = 0; workerIndex < 3 && valid; ++workerIndex) {
            std::thread worker([&] {
                constexpr auto json = R"({"streams":[{"codec_name":"hevc","profile":"Main 10","width":3840,"height":2160,"pix_fmt":"p010le","avg_frame_rate":"0/0","r_frame_rate":"240000/1001","color_transfer":"smpte2084","color_primaries":"bt2020","side_data_list":[{"side_data_type":"Display Matrix","rotation":-90}]}],"format":{"duration":"2.002"}})";
                for (int iteration = 0; iteration < 12; ++iteration) {
                    auto parsed = motion::parse_video_probe_json(json);
                    if (!parsed || parsed->width != 3840 || parsed->height != 2160 ||
                        parsed->bitDepth != 10 || parsed->frameRateNumerator != 240000 ||
                        parsed->frameRateDenominator != 1001 ||
                        parsed->colorTransfer != "smpte2084" || parsed->rotationDegrees != 270 ||
                        parsed->duration100ns != 20'020'000 ||
                        motion::parse_video_probe_json(R"({"streams":[]})") ||
                        motion::parse_video_probe_json("not json")) {
                        valid = false;
                        return;
                    }
                }
            });
            worker.join();
        }
        return valid;
    }

    template <typename Require>
    inline void media_probe_json_repeated_worker_lifetimes(Require require)
    {
        auto executable = motion::executable_directory() / L"MotionWallpaper.Tests.exe";
        // Running this before the child initializes its main-thread apartment
        // ensures the last worker really tears down WinRT between calls.
        auto result = motion::media_tool_detail::run_bounded(executable,
            { L"--media-probe-test-child", L"json" }, 10'000);
        require(result && result->empty(),
            "repeated standalone video-probe parsing failed across worker lifetimes");
    }

    template <typename Require>
    inline void media_probe_child_output_is_bounded(Require require)
    {
        auto executable = motion::executable_directory() / L"MotionWallpaper.Tests.exe";
        auto run = [&](wchar_t const* mode) {
            return motion::media_tool_detail::run_bounded(executable,
                { L"--media-probe-test-child", mode }, 10'000);
        };
        auto output = run(L"stdout");
        require(output && *output == std::string("begin:") + std::string(128 * 1024, 'x') + ":end",
            "the bounded media child lost output, including bytes written immediately before exit");
        auto empty = run(L"empty");
        require(empty && empty->empty(), "a successful media child with empty stdout was rejected");
        require(!run(L"exit7"), "a nonzero media-child exit code was accepted");
        require(!run(L"oversize"), "media-child stdout exceeded its 256 KiB bound");
        require(!motion::media_tool_detail::run_bounded(executable,
                { L"--media-probe-test-child", L"stdout" }, 10'000, [] { return true; }),
            "an already-cancelled media probe still ran successfully");
    }

    template <typename Require>
    inline void media_probe_timeout_and_cancel_reap_children(
        std::filesystem::path const& testRoot, Require require)
    {
        auto executable = motion::executable_directory() / L"MotionWallpaper.Tests.exe";
        // Spaces exercise the real Windows argument quoting as well as cleanup.
        auto directory = testRoot / L"media probe child fixtures";
        std::filesystem::create_directories(directory);
        for (auto mode : { 0, 1, 2 }) {
            auto marker = directory / (L"child-" + std::to_wstring(mode) + L".pid");
            std::error_code ignored;
            std::filesystem::remove(marker, ignored);
            motion::unique_handle observedChild;
            auto observeAndCancel = [&]() -> bool {
                if (!observedChild) {
                    std::ifstream input(marker, std::ios::binary);
                    DWORD processId{};
                    if (input >> processId && processId) {
                        observedChild.reset(OpenProcess(SYNCHRONIZE, FALSE, processId));
                    }
                }
                if (!observedChild || mode == 0) return false;
                if (mode == 2) throw std::runtime_error("simulated probe cancellation callback failure");
                return true;
            };
            auto started = GetTickCount64();
            auto result = motion::media_tool_detail::run_bounded(executable,
                { L"--media-probe-test-child", L"hold", marker.wstring() },
                mode == 0 ? 3'000u : 10'000u, observeAndCancel);
            auto elapsed = GetTickCount64() - started;
            require(!result, "an interrupted media child reported success");
            require(static_cast<bool>(observedChild),
                "the cleanup fixture did not start or its process could not be observed");
            require(elapsed < 6'000,
                "a media-probe timeout or cancellation did not return within its bounded deadline");
            require(WaitForSingleObject(observedChild.get(), 2'000) == WAIT_OBJECT_0,
                "a timed-out, cancelled, or exception-aborted media probe left its child alive");
            std::filesystem::remove(marker, ignored);
        }
    }
}
