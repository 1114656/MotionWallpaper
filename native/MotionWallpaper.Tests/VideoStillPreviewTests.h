#pragma once

#include "../MotionWallpaper.Agent/VideoStillPreview.h"

#include <iostream>

namespace motion::tests
{
    template <typename Require>
    inline void video_still_resolution_and_color(Require require)
    {
        VideoProbeInfo source;
        source.width = 7680;
        source.height = 4320;
        auto size = agent::video_still_size(source);
        require(size.width == 3840 && size.height == 2160, "8K preview escaped the 4K pixel budget");
        source.rotationDegrees = 270;
        size = agent::video_still_size(source);
        require(size.width == 2160 && size.height == 3840, "rotated MOV preview lost its portrait geometry");
        source.width = 1920;
        source.height = 1080;
        size = agent::video_still_size(source);
        require(size.width == 1080 && size.height == 1920, "a small source was upscaled for the preview");
        source.rotationDegrees = 0;
        source.width = 5120;
        source.height = 1440;
        size = agent::video_still_size(source);
        require(size.width == 3840 && size.height == 1080, "ultrawide preview was stretched to 16:9");
        source.pixelFormat = "yuv420p10le";
        source.colorTransfer = "iec61966-2-1";
        source.colorPrimaries = "bt709";
        source.colorSpace = "bt709";
        auto sdr = agent::video_transcode_color_plan(source, size.width, size.height, agent::VideoColorOutput::SrgbStill);
        require(sdr && !sdr->toneMapped && sdr->filter.find(L"crop=") == std::wstring::npos &&
            sdr->filter.find(L"transfer=iec61966-2-1") != std::wstring::npos &&
            sdr->filter.ends_with(L"format=rgb24,setsar=1,sidedata=mode=delete"),
            "SDR still lost full-frame sRGB output or inherited video YUV conversion");
        source.colorPrimaries = "bt2020";
        source.colorSpace = "bt2020nc";
        for (auto transfer : { "smpte2084", "arib-std-b67" }) {
            source.colorTransfer = transfer;
            auto hdr = agent::video_transcode_color_plan(source, 3840, 1080, agent::VideoColorOutput::SrgbStill);
            require(hdr && hdr->toneMapped && hdr->filter.find(L"tonemap=") <
                hdr->filter.rfind(L"transfer=iec61966-2-1"), "HDR preview is not tone mapped before sRGB encoding");
        }
    }

    inline agent::VideoStillPreview::Result await_video_still(agent::VideoStillPreview& service,
        std::filesystem::path const& source)
    {
        auto deadline = GetTickCount64() + 45'000;
        do {
            auto result = service.Read(source);
            if (result.lease) return result;
            Sleep(50);
        } while (GetTickCount64() < deadline);
        return {};
    }

    // Also provides a bounded production-path smoke check for real user MOVs,
    // without starting another Agent or touching their installed library.
    inline std::optional<int> handle_video_still_test_command(int argc, wchar_t** argv)
    {
        if (argc != 5 || std::wstring_view(argv[1]) != L"--extract-still") return std::nullopt;
        agent::VideoStillPreview service(argv[2], argv[4]);
        auto result = await_video_still(service, argv[3]);
        if (!result.lease) return 3;
        std::wcout << result.path.wstring() << L'\n';
        return 0;
    }

    template <typename Require>
    inline void video_still_real_extraction_cache_and_leases(std::filesystem::path const& root, Require require)
    {
        namespace fs = std::filesystem;
        auto ffmpeg = ffmpeg_executable_path(executable_directory());
        auto directory = root / L"high resolution still fixtures";
        fs::create_directories(directory);
        std::array<fs::path, 3> sources;
        std::array<std::wstring, 3> transfers{ L"iec61966-2-1", L"smpte2084", L"arib-std-b67" };
        for (size_t index = 0; index < sources.size(); ++index) {
            sources[index] = directory / (L"source-" + std::to_wstring(index) + L".mkv");
            auto tags = L"format=yuv420p10le,setparams=range=limited:color_primaries=" +
                std::wstring(index ? L"bt2020" : L"bt709") + L":color_trc=" + transfers[index] +
                L":colorspace=" + (index ? L"bt2020nc" : L"bt709");
            auto made = media_tool_detail::run_bounded(ffmpeg, {
                L"-nostdin", L"-hide_banner", L"-loglevel", L"error", L"-y", L"-filter_threads", L"1",
                L"-f", L"lavfi", L"-i", L"testsrc2=s=640x360:r=1", L"-frames:v", L"1",
                L"-vf", tags, L"-c:v", L"ffv1", L"-threads", L"2", L"-pix_fmt", L"yuv420p10le",
                L"-color_trc", transfers[index], L"-color_primaries", index ? L"bt2020" : L"bt709",
                L"-colorspace", index ? L"bt2020nc" : L"bt709", L"-color_range", L"tv", sources[index].wstring()
            }, 15'000);
            require(made.has_value(), "could not create a tagged 10-bit preview fixture");
            auto tagged = probe_video(ffmpeg, sources[index]);
            require(tagged && utf8_to_wide(tagged->colorTransfer) == transfers[index],
                "preview fixture did not preserve its SDR/HDR transfer metadata");
        }
        auto cache = directory / L"cache";
        agent::VideoStillPreview service(ffmpeg, cache, 1); // Force eviction of every unleased old file.
        auto first = await_video_still(service, sources[0]);
        require(static_cast<bool>(first.lease), "real SDR 10-bit extraction failed");
        auto info = probe_video(ffmpeg, first.path);
        require(info && info->width == 640 && info->height == 360 && info->pixelFormat == "rgb24",
            "actual PNG is not a full-resolution RGB still");
        auto timestamp = fs::last_write_time(first.path);
        {
            // A restart can immediately reuse a valid cached frame even if
            // extraction tools are temporarily unavailable.
            agent::VideoStillPreview cached(directory / L"missing-ffmpeg.exe", cache);
            auto reused = cached.Read(sources[0]);
            require(reused.path == first.path && reused.lease && fs::last_write_time(first.path) == timestamp,
                "cache reuse started a redundant decode or did not survive a service restart");
        }
        std::error_code error;
        require(!fs::remove(first.path, error) && error, "a displayed preview was not protected by its file lease");
        auto second = await_video_still(service, sources[1]);
        require(service.Quiesce(5000), "PQ cache publication did not settle");
        require(second.lease && fs::exists(first.path), "PQ extraction or leased-cache eviction failed");
        first.lease.reset();
        auto third = await_video_still(service, sources[2]);
        require(service.Quiesce(5000), "HLG cache publication did not settle");
        require(third.lease && fs::exists(second.path) && !fs::exists(first.path),
            "HLG extraction did not prune an unused image while protecting a displayed one");
        info = probe_video(ffmpeg, third.path);
        require(info && info->pixelFormat == "rgb24" && info->colorTransfer != "arib-std-b67",
            "an SDR PNG inherited HLG video metadata");
        third.lease.reset();
        second.lease.reset();
        auto oldThirdPath = third.path;
        fs::last_write_time(sources[2], fs::last_write_time(sources[2]) + std::chrono::seconds(2));
        auto changed = await_video_still(service, sources[2]);
        require(changed.lease && changed.path != oldThirdPath, "modified source reused a stale desktop frame");

        // Cancelling must release pending/active source pins and temporary files.
        auto cancelCache = directory / L"cancelled";
        agent::VideoStillPreview cancelled(ffmpeg, cancelCache);
        (void)cancelled.Read(sources[0]);
        require(cancelled.Quiesce(5000), "preview extraction did not quiesce promptly");
        fs::rename(sources[0], directory / L"released-source.mkv");
        if (fs::exists(cancelCache)) {
            for (auto const& file : fs::directory_iterator(cancelCache))
                require(!file.path().filename().wstring().ends_with(L".part.png"), "cancelled extraction left a partial PNG");
        }
    }
}
