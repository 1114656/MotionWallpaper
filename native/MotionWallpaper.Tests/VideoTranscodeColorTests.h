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
    inline void adaptive_color_profile_contracts()
    {
        auto check = [](bool value, char const* message) { if (!value) throw std::runtime_error(message); };
        VideoProbeInfo valley;
        valley.bitDepth=10; valley.pixelFormat="yuv420p10le"; valley.codecName="hevc";
        valley.colorPrimaries="smpte432"; valley.colorTransfer="bt470m";
        valley.colorSpace="bt709"; valley.colorRange="tv";
        auto full=agent::video_color_profile(valley, {true,{true,true}}, true);
        check(full.tenBit && full.primaries=="smpte432" && full.transfer=="bt470m" && !full.hdr(),
            "P3 SDR valley must not turn into HDR");
        auto plan=agent::video_transcode_color_plan(valley, 3840,2160,agent::VideoColorOutput::Bt709Video,full);
        check(plan && !plan->toneMapped && plan->filter.find(L"format=yuv420p10le")!=std::wstring::npos &&
            plan->filter.find(L"primaries=smpte432")!=std::wstring::npos,"10-bit/P3 filter lost source precision");
        auto sdr=agent::video_color_profile(valley,{true,{}},true);
        check(sdr.tenBit && sdr.primaries=="bt709","SDR display must not force 8-bit encoding");
        auto power=agent::video_color_profile(valley,{},true);
        check(!power.tenBit && power.primaries=="bt709" && !power.hdr(),"power saver must remain portable SDR8");
        valley.bitDepth=8;
        check(!agent::video_color_profile(valley,{true,{true,true}},true).tenBit,"8-bit input was promoted to 10-bit");
        valley.bitDepth=10; valley.colorPrimaries="bt2020"; valley.colorTransfer="smpte2084";
        valley.colorSpace="bt2020nc";
        full=agent::video_color_profile(valley,{true,{true,true}},true);
        check(full.hdr() && full.tenBit,"compatible HDR lost PQ/10-bit");
        plan=agent::video_transcode_color_plan(valley,1920,1080,agent::VideoColorOutput::Bt709Video,full);
        check(plan && !plan->toneMapped && plan->filter.find(L"tonemap=")==std::wstring::npos,
            "retained HDR was tone mapped or mislabeled");
        auto noHdr=agent::video_color_profile(valley,{true,{true,false}},true);
        check(noHdr.tenBit && !noHdr.hdr(),"HDR disabled must preserve depth independently");
        plan=agent::video_transcode_color_plan(valley,1920,1080,agent::VideoColorOutput::Bt709Video,noHdr);
        check(plan && plan->toneMapped && plan->filter.find(L"format=yuv420p10le")!=std::wstring::npos,
            "HDR to SDR10 must tone map before quantization");
        auto fallback=agent::video_color_profile(valley,{true,{true,true}},false);
        check(!fallback.tenBit && !fallback.hdr(),"8-bit fallback retained unsupported HDR");
        auto candidate=valley;
        candidate.colorTransfer="bt709"; candidate.colorPrimaries="bt709"; candidate.colorSpace="bt709";
        check(agent::video_matches_color_options(candidate,valley,{true,{}}),"SDR10 result rejected");
        check(!agent::video_matches_color_options(candidate,valley,{}),"power saver accepted HEVC10");
        auto old=L"balanced-60-3840x2160-v7.mp4";
        check(agent::video_color_variant_name(old,{true,{}})!=old &&
            agent::video_color_variant_name(old,{true,{}})!=agent::video_color_variant_name(old,{true,{true,true}}),
            "color capabilities collide with old SDR or HDR caches");

        // Sunset is already BT.709 gamut, but uses an sRGB transfer. An
        // ordinary SDR display should not force this through CPU conversion.
        VideoProbeInfo sunset;
        sunset.width=3840; sunset.height=2160; sunset.bitDepth=10;
        sunset.pixelFormat="yuv420p10le"; sunset.codecName="hevc";
        sunset.colorPrimaries="bt709"; sunset.colorTransfer="iec61966-2-1";
        sunset.colorSpace="bt709"; sunset.colorRange="tv";
        auto automatic=agent::video_color_profile(sunset,{true,{}},true);
        check(automatic.tenBit && automatic.transfer=="iec61966-2-1" && automatic.primaries=="bt709" &&
            agent::video_gpu_scale_eligible(sunset,3840,2160,automatic),
            "SDR-only monitor unnecessarily forces BT.709-gamut sRGB through CPU color conversion");
        check(agent::video_color_requires_builtin(automatic), "Main10 must use validated built-in playback");
        auto gpuMetadata=agent::video_gpu_color_metadata_filter(automatic);
        check(gpuMetadata && *gpuMetadata==L"setparams=range=limited:color_primaries=bt709:color_trc=iec61966-2-1:colorspace=bt709",
            "GPU filter output must explicitly retain verified sRGB frame properties for NVENC VUI");
        auto invalidMetadata=automatic;
        invalidMetadata.transfer="iec61966-2-1,scale=1:1";
        check(!agent::video_gpu_color_metadata_filter(invalidMetadata), "GPU color metadata accepted filter injection");
        invalidMetadata=automatic; invalidMetadata.primaries="unknown";
        check(!agent::video_gpu_color_metadata_filter(invalidMetadata), "GPU color metadata guessed an unknown gamut");
        invalidMetadata=automatic; invalidMetadata.transfer="smpte2084";
        check(!agent::video_gpu_color_metadata_filter(invalidMetadata), "SDR GPU fast path accepted HDR without tone mapping");
        for (auto transfer : { "bt709", "bt470m", "iec61966-2-1", "bt2020-10" }) {
            sunset.colorTransfer=transfer;
            automatic=agent::video_color_profile(sunset,{true,{}},true);
            check(automatic.transfer==transfer && agent::video_gpu_scale_eligible(sunset,1920,1080,automatic),
                "supported BT.709-gamut SDR transfer was changed by the display capability gate");
        }
        sunset.colorTransfer="iec61966-2-1";
        auto portable=agent::video_color_profile(sunset,{},true);
        check(!portable.tenBit && portable.transfer=="bt709" && !agent::video_color_requires_builtin(portable) &&
            !agent::video_gpu_scale_eligible(sunset,1920,1080,portable),
            "power saver must retain explicit 8-bit/BT.709 color conversion");

        agent::VideoColorOptions compatible{true,{true,true},true};
        auto normalized=agent::video_color_profile(sunset,compatible,true);
        check(normalized.tenBit && normalized.primaries=="bt709" && normalized.transfer=="bt709" && !normalized.hdr(),
            "compatibility retry lost supported precision or failed to normalize SDR color");
        sunset.colorTransfer="bt709";
        check(!agent::video_gpu_scale_eligible(sunset,1920,1080,normalized,compatible),
            "explicit compatible copy bypassed the CPU color pipeline for matching source tags");
        sunset.colorTransfer="iec61966-2-1";
        plan=agent::video_transcode_color_plan(sunset,1920,1080,agent::VideoColorOutput::Bt709Video,normalized);
        check(plan && !plan->toneMapped && plan->filter.find(L"transferin=iec61966-2-1")!=std::wstring::npos &&
            plan->filter.find(L":transfer=bt709")!=std::wstring::npos &&
            plan->filter.find(L"format=yuv420p10le")!=std::wstring::npos,
            "compatible SDR10 must transform samples, not merely relabel sRGB as BT.709");
        auto compatibleName=agent::video_color_variant_name(old,compatible);
        check(compatibleName==L"balanced-60-3840x2160-compatible-v9.mp4" &&
            compatibleName!=agent::video_color_variant_name(old,{true,{}}) &&
            compatibleName==agent::video_color_variant_name(old,{true,{},true}),
            "compatible copy cache must remain separate and independent of display HDR capability");

        auto normalizedCandidate=sunset;
        normalizedCandidate.colorTransfer="bt709";
        check(!agent::video_matches_color_options(normalizedCandidate,sunset,{true,{}}) &&
            agent::video_matches_color_options(normalizedCandidate,sunset,{true,{}},true),
            "automatic fallback acceptance must not weaken strict generated-profile validation");
        check(agent::video_matches_color_options(normalizedCandidate,sunset,compatible) &&
            !agent::video_matches_color_options(sunset,sunset,compatible,true),
            "explicit compatible request incorrectly accepted the unconverted source color");
        check(!agent::video_matches_color_options(normalizedCandidate,sunset,{},true),
            "compatible fallback accepted 10-bit for power saver");
        normalizedCandidate.bitDepth=8; normalizedCandidate.codecName="h264";
        normalizedCandidate.pixelFormat="yuv420p";
        check(agent::video_matches_color_options(normalizedCandidate,sunset,{true,{}},true),
            "automatic compatibility retry must accept a validated H.264 8-bit downgrade");

        sunset.bitDepth=8; sunset.pixelFormat="yuv420p"; sunset.codecName="h264";
        automatic=agent::video_color_profile(sunset,{true,{}},true);
        check(!automatic.tenBit && automatic.transfer=="iec61966-2-1" &&
            agent::video_color_requires_builtin(automatic) &&
            agent::video_gpu_scale_eligible(sunset,1920,1080,automatic),
            "8-bit sRGB preservation must use the built-in color-aware playback path without promoting depth");
        normalizedCandidate.bitDepth=10; normalizedCandidate.codecName="hevc";
        normalizedCandidate.pixelFormat="yuv420p10le";
        check(!agent::video_matches_color_options(normalizedCandidate,sunset,{true,{}},true),
            "compatible fallback must never allow an 8-bit source to become 10-bit");

        auto hdrCompatibility=agent::video_color_profile(valley,compatible,true);
        plan=agent::video_transcode_color_plan(valley,1920,1080,agent::VideoColorOutput::Bt709Video,hdrCompatibility);
        check(hdrCompatibility.tenBit && !hdrCompatibility.hdr() && hdrCompatibility.primaries=="bt709" &&
            plan && plan->toneMapped && !agent::video_gpu_scale_eligible(valley,1920,1080,hdrCompatibility,compatible),
            "explicit compatible HDR source must tone-map through CPU even on an HDR display");
    }

}
