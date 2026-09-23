#pragma once

#include "../MotionWallpaper.Agent/VideoTranscodeColorPolicy.h"
#include "../MotionWallpaper.Agent/VideoTranscoder.h"

#include <stdexcept>

namespace motion::tests
{
    inline void transcode_completion_priority_and_gpu_scaling()
    {
        auto check = [](bool value, char const* message) { if (!value) throw std::runtime_error(message); };
        check(agent::video_transcode_worker_threads(0) == 1 && agent::video_transcode_worker_threads(2) == 1 &&
            agent::video_transcode_worker_threads(4) == 3 && agent::video_transcode_worker_threads(8) == 6 &&
            agent::video_transcode_worker_threads(16) == 12 && agent::video_transcode_worker_threads(128) == 12 &&
            agent::video_transcode_worker_threads(16, 8) == 8, "generation parallelism lost its headroom or memory bound");
        VideoProbeInfo source;
        source.width = 3840; source.height = 2160; source.bitDepth = 8;
        source.pixelFormat = "yuv420p"; source.colorTransfer = "bt709";
        source.colorPrimaries = "bt709"; source.colorSpace = "bt709"; source.colorRange = "tv";
        check(agent::video_gpu_scale_eligible(source, 1920, 1080) &&
            agent::video_gpu_scale_eligible(source, 3840, 2160), "compatible SDR cannot stay on the GPU");
        check(!agent::video_gpu_scale_eligible(source, 1920, 1200) &&
            !agent::video_gpu_scale_eligible(source, 7680, 4320), "GPU resize stretched or upscaled a source");
        for (auto invalid : { "smpte2084", "arib-std-b67", "iec61966-2-1", "unknown" }) {
            source.colorTransfer = invalid;
            check(!agent::video_gpu_scale_eligible(source, 1920, 1080), "GPU scaler bypassed required transfer conversion");
        }
        source.colorTransfer = "bt709"; source.colorPrimaries = "smpte432";
        check(!agent::video_gpu_scale_eligible(source, 1920, 1080), "P3 gamut conversion was silently skipped");
        source.colorPrimaries = "bt709"; source.colorRange = "pc";
        check(!agent::video_gpu_scale_eligible(source, 1920, 1080), "full-range source lost range conversion");
        source.colorRange = "tv"; source.rotationDegrees = 90;
        check(!agent::video_gpu_scale_eligible(source, 1920, 1080), "MOV autorotation lost its compatible path");
        source.rotationDegrees = 0; source.bitDepth = 10; source.pixelFormat = "yuv420p10le";
        check(!agent::video_gpu_scale_eligible(source, 1920, 1080), "10-bit conversion bypassed explicit dithering");
    }

    inline void transcode_color_and_rate_contracts()
    {
        auto check = [](bool value, char const* message) { if (!value) throw std::runtime_error(message); };
        VideoProbeInfo source;
        source.width = 3840;
        source.height = 2160;
        source.bitDepth = 10;
        source.pixelFormat = "yuv420p10le";
        source.colorTransfer = "smpte2084";
        source.colorPrimaries = "bt2020";
        source.colorSpace = "bt2020nc";
        source.colorRange = "tv";
        auto pq = agent::video_transcode_color_plan(source, 1920, 1080);
        check(pq && pq->toneMapped, "PQ requires a tone-mapped SDR output");
        check(pq->filter.find(L"transfer=linear") < pq->filter.find(L"tonemap="),
            "HDR tone mapping must run in linear light");
        check(pq->filter.find(L"tonemap=") < pq->filter.rfind(L"transfer=bt709"),
            "SDR transfer must be applied after tone mapping");
        check(pq->filter.find(L"dither=error_diffusion") != std::wstring::npos &&
                pq->filter.ends_with(L"format=yuv420p,setsar=1,sidedata=mode=delete"),
            "SDR copy must dither to 8-bit and remove obsolete HDR frame metadata");
        source.colorTransfer = "arib-std-b67";
        check(agent::video_transcode_color_plan(source, 1920, 1080)->toneMapped,
            "HLG requires a tone-mapped SDR output");
        source.colorTransfer = "unknown";
        check(!agent::video_transcode_color_plan(source, 1920, 1080),
            "BT.2020 without a transfer function must not be silently interpreted as SDR");
        source.colorTransfer = "iec61966-2-1";
        source.colorPrimaries = "bt709";
        source.colorSpace = "bt709";
        auto sdr = agent::video_transcode_color_plan(source, 1920, 1080);
        check(sdr && !sdr->toneMapped && sdr->filter.find(L"transferin=iec61966-2-1") != std::wstring::npos,
            "ordinary SDR 10-bit uses transfer conversion and dithering without tone mapping");
        source.colorTransfer = "bt709,scale=1:1";
        check(!agent::video_transcode_color_plan(source, 1920, 1080),
            "untrusted probe metadata must not be inserted into a filter graph");
        source.frameRateNumerator = 30000;
        source.frameRateDenominator = 1001;
        check(agent::video_transcode_frame_rate(source, 30) == L"30000/1001",
            "29.97 FPS sources must not be upsampled to 30 FPS");
        source.frameRateNumerator = 24000;
        check(agent::video_transcode_frame_rate(source, 60) == L"24000/1001",
            "23.976 FPS sources must retain their rational cadence");
        source.frameRateNumerator = 240000;
        check(agent::video_transcode_frame_rate(source, 60) == L"60",
            "high-rate MOV sources must obey the output frame-rate cap");
    }
}
