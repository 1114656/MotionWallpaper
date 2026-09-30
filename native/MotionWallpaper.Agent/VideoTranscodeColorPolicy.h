#pragma once

#include "VideoColorProfile.h"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace motion::agent
{
    enum class VideoColorOutput { Bt709Video, SrgbStill };

    // GPU resizing preserves the samples' color interpretation, but some
    // hardware filters drop AVFrame color properties. Restore only a validated
    // unchanged SDR profile so the encoder cannot replace sRGB with BT.709.
    [[nodiscard]] inline std::optional<std::wstring> video_gpu_color_metadata_filter(
        VideoColorProfile const& profile)
    {
        auto allowed = [](std::string const& value, std::initializer_list<std::string_view> values) {
            return std::find(values.begin(), values.end(), value) != values.end();
        };
        if (!allowed(profile.primaries, { "bt709", "smpte432", "bt2020" }) ||
            !allowed(profile.transfer, { "bt709", "bt470m", "iec61966-2-1", "bt2020-10" }) ||
            !allowed(profile.matrix, { "bt709", "bt2020nc" })) return std::nullopt;
        auto widen = [](std::string const& value) { return std::wstring(value.begin(), value.end()); };
        return L"setparams=range=limited:color_primaries=" + widen(profile.primaries) +
            L":color_trc=" + widen(profile.transfer) + L":colorspace=" + widen(profile.matrix);
    }

    [[nodiscard]] inline bool video_gpu_scale_eligible(
        motion::VideoProbeInfo const& source, uint32_t width, uint32_t height,
        VideoColorProfile const& profile = {}, VideoColorOptions options = {})
    {
        // HDR retains mastering side data via the color-managed CPU filter;
        // only unchanged SDR interpretation may take this zero-copy shortcut.
        // An explicit compatibility retry must exercise CPU color conversion,
        // even when its resulting color labels happen to match the source.
        return !options.compatibility && video_gpu_color_metadata_filter(profile).has_value() &&
            !profile.hdr() && source.rotationDegrees == 0 && width && height &&
            source.width && source.height && width <= source.width && height <= source.height &&
            static_cast<uint64_t>(source.width) * height == static_cast<uint64_t>(source.height) * width &&
            source.bitDepth == (profile.tenBit ? 10u : 8u) &&
            (source.pixelFormat == (profile.tenBit ? "yuv420p10le" : "yuv420p") ||
             source.pixelFormat == (profile.tenBit ? "p010le" : "nv12")) && source.colorRange == "tv" &&
            source.colorPrimaries == profile.primaries && source.colorTransfer == profile.transfer &&
            source.colorSpace == profile.matrix;
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
        VideoColorOutput output = VideoColorOutput::Bt709Video,
        VideoColorProfile profile = {})
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
        if (still) profile = {};
        // Still dimensions preserve the complete, autorotated source aspect.
        // Do not crop a photographic preview to the video's chroma grid.
        auto filter = still ? std::wstring{} : L"crop=w='max(2,trunc(min(iw,ih*" + ratio + L")/2)*2)'"
            L":h='max(2,trunc(min(ih,iw*" + inverse + L")/2)*2)':x=(iw-ow)/2:y=(ih-oh)/2,";
        filter += L"zscale=w=" + std::to_wstring(width) + L":h=" + std::to_wstring(height) +
            L":filter=lanczos" + input;
        std::wstring outputColor = still
            ? L"transfer=iec61966-2-1:primaries=bt709:matrix=gbr:range=full:dither=error_diffusion"
            : L"transfer=" + widen(profile.transfer) + L":primaries=" + widen(profile.primaries) +
                L":matrix=" + widen(profile.matrix) + L":range=limited:dither=error_diffusion";
        if (hdr && !profile.hdr()) {
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
            : (profile.tenBit ? L",format=yuv420p10le,setsar=1" : L",format=yuv420p,setsar=1");
        if (!still) {
            // Preserve static mastering/light metadata only for an unchanged
            // HDR transfer. Dynamic Dolby Vision instructions cannot survive
            // resizing/re-encoding and must never be copied into the new file.
            filter += profile.hdr()
                ? L",sidedata=mode=delete:type=DOVI_RPU_BUFFER,sidedata=mode=delete:type=DOVI_METADATA"
                  L",sidedata=mode=delete:type=DYNAMIC_HDR_PLUS,sidedata=mode=delete:type=DYNAMIC_HDR_VIVID"
                : L",sidedata=mode=delete";
        }
        return VideoTranscodeColorPlan{ std::move(filter), hdr && !profile.hdr(), assumed };
    }
}
