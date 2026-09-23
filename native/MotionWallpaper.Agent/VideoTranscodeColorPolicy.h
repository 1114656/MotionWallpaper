#pragma once

#include "../MotionWallpaper.Common/MediaProbe.h"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace motion::agent
{
    enum class VideoColorOutput { Bt709Video, SrgbStill };

    // A video processor may resize these surfaces without changing the color
    // interpretation. P3, HDR, 10-bit, unknown tags and aspect crops still use
    // the fully specified, dithered conversion path below.
    [[nodiscard]] inline bool video_gpu_scale_eligible(
        motion::VideoProbeInfo const& source, uint32_t width, uint32_t height) noexcept
    {
        return width && height && source.width && source.height && source.rotationDegrees == 0 &&
            source.bitDepth == 8 && (source.pixelFormat == "yuv420p" || source.pixelFormat == "nv12") &&
            source.colorTransfer == "bt709" && source.colorPrimaries == "bt709" &&
            source.colorSpace == "bt709" && source.colorRange == "tv" &&
            static_cast<uint64_t>(source.width) * height == static_cast<uint64_t>(source.height) * width &&
            width <= source.width && height <= source.height;
    }

    struct VideoTranscodeColorPlan
    {
        std::wstring filter;
        bool toneMapped{};
        bool assumedSdr{};
    };

    [[nodiscard]] inline std::wstring video_transcode_frame_rate(
        motion::VideoProbeInfo const& source, uint32_t cap)
    {
        if (source.frameRateNumerator && source.frameRateDenominator &&
            source.frameRateNumerator <= static_cast<uint64_t>(cap) * source.frameRateDenominator) {
            return std::to_wstring(source.frameRateNumerator) + L"/" + std::to_wstring(source.frameRateDenominator);
        }
        return std::to_wstring(cap);
    }

    [[nodiscard]] inline bool video_color_unspecified(std::string_view value) noexcept
    {
        return value.empty() || value == "unknown" || value == "unspecified" || value == "reserved";
    }

    // Construct options only from a fixed allow-list, never from probe text.
    [[nodiscard]] inline std::optional<VideoTranscodeColorPlan> video_transcode_color_plan(
        motion::VideoProbeInfo const& source, uint32_t width, uint32_t height,
        VideoColorOutput output = VideoColorOutput::Bt709Video)
    {
        if (!width || !height) return std::nullopt;
        auto allowed = [](std::string_view value, std::initializer_list<std::string_view> values) {
            return std::find(values.begin(), values.end(), value) != values.end();
        };
        bool hdr = source.colorTransfer == "smpte2084" || source.colorTransfer == "arib-std-b67";
        bool rgb = source.pixelFormat.starts_with("rgb") || source.pixelFormat.starts_with("bgr") ||
            source.pixelFormat.starts_with("gbr") || source.pixelFormat.starts_with("rgba") ||
            source.pixelFormat.starts_with("bgra");
        auto transfer = source.colorTransfer;
        auto primaries = source.colorPrimaries;
        auto matrix = source.colorSpace;
        auto range = source.colorRange;
        bool assumed{};
        if (video_color_unspecified(transfer)) {
            // BT.2020 with an absent transfer function is ambiguous: it may be
            // SDR, PQ or HLG. A pixel-format conversion cannot resolve this.
            if (primaries == "bt2020" || matrix == "bt2020nc" || matrix == "bt2020c") return std::nullopt;
            transfer = rgb ? "iec61966-2-1" : "bt709";
            assumed = true;
        }
        if (video_color_unspecified(primaries)) {
            primaries = hdr ? "bt2020" : "bt709";
            assumed = true;
        }
        if (video_color_unspecified(matrix)) {
            matrix = rgb ? "gbr" : hdr ? "bt2020nc" : source.height <= 576 ? "smpte170m" : "bt709";
            assumed = true;
        }
        if (matrix == "rgb") matrix = "gbr";
        if (video_color_unspecified(range)) {
            range = rgb || source.pixelFormat.starts_with("yuvj") ? "pc" : "tv";
            assumed = true;
        }
        if (!allowed(transfer, { "bt709", "bt470m", "bt470bg", "smpte170m", "smpte240m",
                "linear", "iec61966-2-4", "iec61966-2-1", "bt2020-10", "bt2020-12",
                "smpte2084", "arib-std-b67" }) ||
            !allowed(primaries, { "bt709", "bt470m", "bt470bg", "smpte170m", "smpte240m",
                "film", "bt2020", "smpte428", "smpte431", "smpte432", "jedec-p22" }) ||
            !allowed(matrix, { "gbr", "bt709", "fcc", "bt470bg", "smpte170m", "smpte240m",
                "ycgco", "bt2020nc", "bt2020c", "chroma-derived-nc", "chroma-derived-c", "ictcp" }) ||
            !allowed(range, { "pc", "tv" })) return std::nullopt;
        auto widen = [](std::string const& value) { return std::wstring(value.begin(), value.end()); };
        auto input = L":transferin=" + widen(transfer) + L":primariesin=" + widen(primaries) +
            L":matrixin=" + widen(matrix) + L":rangein=" + (range == "pc" ? L"full" : L"limited");
        // FFmpeg autorotation precedes this filter. Derive the crop from the
        // actual input frame so 90-degree MOV metadata and ordinary inputs
        // share the same centered-fill contract. Align chroma to even pixels.
        auto ratio = std::to_wstring(width) + L"/" + std::to_wstring(height);
        auto inverse = std::to_wstring(height) + L"/" + std::to_wstring(width);
        bool still = output == VideoColorOutput::SrgbStill;
        // Still dimensions preserve the complete, autorotated source aspect.
        // Do not crop a photographic preview to the video's chroma grid.
        auto filter = still ? std::wstring{} : L"crop=w='max(2,trunc(min(iw,ih*" + ratio + L")/2)*2)'"
            L":h='max(2,trunc(min(ih,iw*" + inverse + L")/2)*2)':x=(iw-ow)/2:y=(ih-oh)/2,";
        filter += L"zscale=w=" + std::to_wstring(width) + L":h=" + std::to_wstring(height) +
            L":filter=lanczos" + input;
        std::wstring outputColor = still
            ? L"transfer=iec61966-2-1:primaries=bt709:matrix=gbr:range=full:dither=error_diffusion"
            : L"transfer=bt709:primaries=bt709:matrix=bt709:range=limited:dither=error_diffusion";
        if (hdr) {
            // Tone mapping operates on linear-light float RGB. First reduce
            // spatial work, then map the HDR signal and wide gamut to SDR.
            filter += L":transfer=linear:primaries=bt709:matrix=gbr:range=full:npl=100,format=gbrpf32le,"
                L"tonemap=tonemap=mobius:desat=2,zscale=" + outputColor;
        } else {
            filter += L":" + outputColor;
        }
        // A derived SDR file must not inherit HDR mastering/content-light or
        // Dolby Vision side data. Eight-bit reduction uses explicit dithering.
        // zscale needs an explicit planar RGB output before packing PNG's
        // rgb24, otherwise negotiation can leave it in the YUV color family.
        filter += still ? L",format=gbrp,format=rgb24,setsar=1,sidedata=mode=delete"
            : L",format=yuv420p,setsar=1,sidedata=mode=delete";
        return VideoTranscodeColorPlan{ std::move(filter), hdr, assumed };
    }
}
