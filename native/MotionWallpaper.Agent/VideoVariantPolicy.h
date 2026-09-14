#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace motion::agent
{
    [[nodiscard]] constexpr bool video_cpu_conversion_allowed(
        bool hdrTransfer, bool bt2020Primaries) noexcept
    {
        return !hdrTransfer && !bt2020Primaries;
    }

    [[nodiscard]] constexpr bool video_variant_color_metadata_matches(
        bool sourceTransferKnown, uint32_t sourceTransfer,
        bool actualTransferKnown, uint32_t actualTransfer,
        bool sourcePrimariesKnown, uint32_t sourcePrimaries,
        bool actualPrimariesKnown, uint32_t actualPrimaries) noexcept
    {
        return (!sourceTransferKnown ||
                (actualTransferKnown && actualTransfer == sourceTransfer)) &&
            (!sourcePrimariesKnown ||
                (actualPrimariesKnown && actualPrimaries == sourcePrimaries));
    }

    [[nodiscard]] constexpr bool video_variant_is_hevc_main10(
        bool profileKnown, uint32_t profile) noexcept
    {
        return profileKnown && profile == 2;
    }

    enum class VideoSourceCodec { Unknown, H264, Hevc, Vp9, Av1 };

    enum class VideoHardwareDecodeProfile
    {
        Unsupported,
        H264,
        HevcMain,
        HevcMain10,
        Vp9Profile0,
        Vp9Profile2,
        Av1Profile0
    };

    [[nodiscard]] constexpr VideoHardwareDecodeProfile video_hardware_decode_profile(
        VideoSourceCodec codec, bool profileKnown, uint32_t profile) noexcept
    {
        if (!profileKnown) return VideoHardwareDecodeProfile::Unsupported;
        switch (codec) {
        case VideoSourceCodec::H264:
            // Baseline/Main/Extended/High are the common 8-bit profiles
            // covered by the D3D11 H.264 VLD profile. High10 and the 4:2:2 /
            // 4:4:4 families must not inherit that capability result.
            return profile == 66 || profile == 77 || profile == 88 || profile == 100
                ? VideoHardwareDecodeProfile::H264
                : VideoHardwareDecodeProfile::Unsupported;
        case VideoSourceCodec::Hevc:
            return profile == 1 ? VideoHardwareDecodeProfile::HevcMain :
                profile == 2 ? VideoHardwareDecodeProfile::HevcMain10 :
                VideoHardwareDecodeProfile::Unsupported;
        case VideoSourceCodec::Vp9:
            return profile == 0 ? VideoHardwareDecodeProfile::Vp9Profile0 :
                profile == 2 ? VideoHardwareDecodeProfile::Vp9Profile2 :
                VideoHardwareDecodeProfile::Unsupported;
        case VideoSourceCodec::Av1:
            // AV1 profile 0 may be either 8- or 10-bit. The runtime probe
            // therefore requires both NV12 and P010 support before accepting
            // it as an automatic hardware-decode path.
            return profile == 0 ? VideoHardwareDecodeProfile::Av1Profile0 :
                VideoHardwareDecodeProfile::Unsupported;
        default:
            return VideoHardwareDecodeProfile::Unsupported;
        }
    }

    [[nodiscard]] constexpr bool video_software_fallback_allowed(
        VideoSourceCodec codec, bool profileKnown, uint32_t profile,
        bool hdrTransfer = false, bool bt2020Primaries = false) noexcept
    {
        if (hdrTransfer || bt2020Primaries) return false;
        // OpenH264 produces 8-bit H.264. Accept a compatibility conversion
        // only when the compressed profile positively identifies an 8-bit
        // H.264/HEVC source. Unknown, VP9 and AV1 media may be 10/12-bit even
        // without HDR metadata, so silently flattening them is a quality and
        // colour correctness failure.
        if (codec == VideoSourceCodec::Hevc) return profileKnown && profile == 1;
        if (codec == VideoSourceCodec::H264) {
            return profileKnown && (profile == 66 || profile == 77 || profile == 88 || profile == 100);
        }
        return false;
    }

    struct VideoVariantDecision
    {
        uint32_t targetFps{};
        std::wstring fileName;
    };

    [[nodiscard]] inline VideoVariantDecision video_variant_decision(
        std::string const& performanceMode,
        uint32_t width = 0, uint32_t height = 0,
        uint32_t sourceRateNumerator = 0, uint32_t sourceRateDenominator = 0,
        uint32_t displayRefreshRate = 0)
    {
        if (performanceMode == "original") return {};
        bool cpuSmooth = performanceMode == "cpu-smooth";
        uint32_t cap = performanceMode == "power-saver" || cpuSmooth
            ? (std::min)(60u, displayRefreshRate ? displayRefreshRate : 60u)
            : (std::min)(120u, displayRefreshRate ? displayRefreshRate : 120u);
        uint32_t sourceFps = sourceRateNumerator && sourceRateDenominator
            ? static_cast<uint32_t>((static_cast<uint64_t>(sourceRateNumerator) + sourceRateDenominator / 2) /
                sourceRateDenominator)
            : cap;
        uint32_t targetFps = (std::max)(1u, (std::min)(sourceFps, cap));
        auto dimensions = width && height
            ? L"-" + std::to_wstring(width) + L"x" + std::to_wstring(height)
            : std::wstring{};
        if (cpuSmooth) {
            return { targetFps, L"cpu-smooth-" + std::to_wstring(targetFps) + dimensions + L"-v5.mp4" };
        }
        if (performanceMode == "power-saver") {
            return { targetFps, L"power-saver-" + std::to_wstring(targetFps) + dimensions + L"-v5.mp4" };
        }
        return { targetFps, L"balanced-" + std::to_wstring(targetFps) + dimensions + L"-v5.mp4" };
    }

    [[nodiscard]] constexpr std::pair<uint32_t, uint32_t> video_variant_dimensions(
        uint32_t sourceWidth, uint32_t sourceHeight,
        uint32_t targetWidth, uint32_t targetHeight) noexcept
    {
        if (!sourceWidth || !sourceHeight || !targetWidth || !targetHeight ||
            targetWidth >= sourceWidth || targetHeight >= sourceHeight) {
            return { sourceWidth, sourceHeight };
        }
        auto ceil_div = [](uint64_t value, uint64_t divisor) {
            return static_cast<uint32_t>((value + divisor - 1) / divisor);
        };
        uint32_t width{}, height{};
        if (static_cast<uint64_t>(targetWidth) * sourceHeight >=
            static_cast<uint64_t>(targetHeight) * sourceWidth) {
            width = targetWidth;
            height = ceil_div(static_cast<uint64_t>(sourceHeight) * targetWidth, sourceWidth);
        } else {
            height = targetHeight;
            width = ceil_div(static_cast<uint64_t>(sourceWidth) * targetHeight, sourceHeight);
        }
        width = (width + 1u) & ~1u;
        height = (height + 1u) & ~1u;
        return { (std::min)(width, sourceWidth), (std::min)(height, sourceHeight) };
    }

    // CPU playback has a hard pixel budget. Fit the complete source inside it
    // and let the existing Renderer crop/scale for the display. Unlike the
    // quality profiles, this may upscale at presentation time because decoding
    // an oversized off-screen area defeats the compatibility fallback.
    [[nodiscard]] constexpr std::pair<uint32_t, uint32_t> video_cpu_variant_dimensions(
        uint32_t sourceWidth, uint32_t sourceHeight,
        uint32_t maximumWidth, uint32_t maximumHeight) noexcept
    {
        if (!sourceWidth || !sourceHeight || !maximumWidth || !maximumHeight ||
            (sourceWidth <= maximumWidth && sourceHeight <= maximumHeight)) {
            return { sourceWidth, sourceHeight };
        }
        uint32_t width{};
        uint32_t height{};
        if (static_cast<uint64_t>(maximumWidth) * sourceHeight <=
            static_cast<uint64_t>(maximumHeight) * sourceWidth) {
            width = maximumWidth;
            height = static_cast<uint32_t>((static_cast<uint64_t>(sourceHeight) * maximumWidth +
                sourceWidth - 1) / sourceWidth);
        } else {
            height = maximumHeight;
            width = static_cast<uint32_t>((static_cast<uint64_t>(sourceWidth) * maximumHeight +
                sourceHeight - 1) / sourceHeight);
        }
        width = (std::max)(2u, (width + 1u) & ~1u);
        height = (std::max)(2u, (height + 1u) & ~1u);
        return { (std::min)(width, sourceWidth), (std::min)(height, sourceHeight) };
    }

    [[nodiscard]] inline bool video_needs_variant(
        uint32_t numerator, uint32_t denominator, uint32_t targetFps,
        uint32_t sourceWidth = 0, uint32_t sourceHeight = 0,
        uint32_t targetWidth = 0, uint32_t targetHeight = 0) noexcept
    {
        if (!numerator || !denominator || !targetFps) return false;
        bool frameRateDiffers = static_cast<uint64_t>(numerator) >
            static_cast<uint64_t>(targetFps) * denominator;
        bool dimensionsDiffer = sourceWidth && sourceHeight && targetWidth && targetHeight &&
            (sourceWidth != targetWidth || sourceHeight != targetHeight);
        return frameRateDiffers || dimensionsDiffer;
    }

    [[nodiscard]] inline bool video_variant_rate_matches(
        uint32_t numerator, uint32_t denominator, uint32_t targetFps) noexcept
    {
        if (!numerator || !denominator || !targetFps) return false;
        auto expected = static_cast<uint64_t>(targetFps) * denominator;
        auto actual = static_cast<uint64_t>(numerator);
        auto delta = actual > expected ? actual - expected : expected - actual;
        return delta <= denominator;
    }

    [[nodiscard]] inline bool video_variant_dimensions_match(
        uint32_t actualWidth, uint32_t actualHeight,
        uint32_t visibleWidth, uint32_t visibleHeight) noexcept
    {
        if (!actualWidth || !actualHeight || !visibleWidth || !visibleHeight) return false;
        auto coded = [](uint32_t value) { return (value + 31u) & ~31u; };
        return (actualWidth == visibleWidth || actualWidth == coded(visibleWidth)) &&
            (actualHeight == visibleHeight || actualHeight == coded(visibleHeight));
    }

    // Container timestamps can differ slightly after CFR conversion and MP4
    // remuxing. Accept a bounded drift, but reject an empty or materially
    // truncated output before it can become a looping wallpaper. When the
    // source container exposes no duration, a positive output duration is the
    // strongest comparison available.
    [[nodiscard]] constexpr bool video_variant_duration_matches(
        uint64_t actual100ns, uint64_t source100ns, uint32_t targetFps) noexcept
    {
        if (!actual100ns) return false;
        if (!source100ns) return true;

        constexpr uint64_t unitsPerSecond = 10'000'000;
        constexpr uint64_t minimumTolerance = unitsPerSecond / 2;
        constexpr uint64_t maximumTolerance = unitsPerSecond * 5;
        auto percentageTolerance = (std::min)(maximumTolerance,
            (std::max)(minimumTolerance, source100ns / 100));
        auto frameTolerance = targetFps
            ? (2 * unitsPerSecond + targetFps - 1) / targetFps
            : minimumTolerance;
        auto tolerance = (std::min)(maximumTolerance,
            (std::max)(percentageTolerance, frameTolerance));
        auto difference = actual100ns > source100ns
            ? actual100ns - source100ns : source100ns - actual100ns;
        return difference <= tolerance;
    }
}
