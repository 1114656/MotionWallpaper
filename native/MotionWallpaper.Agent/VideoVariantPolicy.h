#pragma once

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>

namespace motion::agent
{
    [[nodiscard]] constexpr std::pair<uint32_t, uint32_t> video_display_aspect(uint32_t width, uint32_t height)
    {
        if (!width || !height) return { 0, 0 };
        auto divisor = std::gcd(width, height);
        return { width / divisor, height / divisor };
    }
    // A performance copy has an explicit portable output contract, independent
    // of which GPU/CPU encoder happens to create it. HDR inputs are transformed
    // by the transcoder, never merely relabelled as SDR.
    [[nodiscard]] constexpr bool video_matches_sdr_output(
        std::string_view codec, std::string_view pixelFormat, uint32_t bitDepth,
        std::string_view transfer, std::string_view primaries,
        std::string_view matrix, std::string_view range) noexcept
    {
        return codec == "h264" && pixelFormat == "yuv420p" && bitDepth == 8 &&
            transfer == "bt709" && primaries == "bt709" && matrix == "bt709" &&
            (range == "tv" || range == "mpeg");
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

    struct VideoVariantDecision
    {
        uint32_t targetFps{};
        std::wstring fileName;
    };

    [[nodiscard]] inline std::wstring unchanged_legacy_fill_variant(std::wstring name,
        uint32_t sourceWidth, uint32_t sourceHeight, uint32_t width, uint32_t height)
    {
        if (!sourceWidth || !sourceHeight || !width || !height || !name.ends_with(L"-v7.mp4") ||
            video_display_aspect(sourceWidth, sourceHeight) != video_display_aspect(width, height)) return {};
        name.replace(name.size() - 7, 7, L"-v6.mp4");
        return name;
    }

    // Both quality profiles follow the display refresh budget. Extra encoded
    // frames consume decode bandwidth without adding visible display updates.
    // Keep the separate CPU compatibility budget bounded below.
    [[nodiscard]] constexpr uint32_t video_frame_rate_cap(
        std::string_view performanceMode, uint32_t displayRefreshRate = 0) noexcept
    {
        if (performanceMode == "original") return 0;
        return displayRefreshRate ? displayRefreshRate : 60u;
    }

    [[nodiscard]] constexpr uint32_t video_cpu_frame_rate_cap(
        uint32_t requestedCpuCap = 0) noexcept
    {
        return (std::min)(60u, requestedCpuCap ? requestedCpuCap : 60u);
    }

    [[nodiscard]] inline VideoVariantDecision video_variant_decision(
        std::string const& performanceMode,
        uint32_t width = 0, uint32_t height = 0,
        uint32_t sourceRateNumerator = 0, uint32_t sourceRateDenominator = 0,
        uint32_t displayRefreshRateOrCpuCap = 0)
    {
        if (performanceMode == "original") return {};
        bool cpuSmooth = performanceMode == "cpu-smooth";
        uint32_t cap = cpuSmooth
            ? video_cpu_frame_rate_cap(displayRefreshRateOrCpuCap)
            : video_frame_rate_cap(performanceMode, displayRefreshRateOrCpuCap);
        // The integer is an upper bound/cache identity. Round it upward so
        // the transcoder can retain the exact source rational below the cap
        // (for example 24.4 FPS), rather than unnecessarily lowering it.
        uint32_t sourceFps = sourceRateNumerator && sourceRateDenominator
            ? static_cast<uint32_t>((static_cast<uint64_t>(sourceRateNumerator) + sourceRateDenominator - 1) /
                sourceRateDenominator)
            : cap;
        uint32_t targetFps = (std::max)(1u, (std::min)(sourceFps, cap));
        auto dimensions = width && height
            ? L"-" + std::to_wstring(width) + L"x" + std::to_wstring(height)
            : std::wstring{};
        if (cpuSmooth) {
            return { targetFps, L"cpu-smooth-" + std::to_wstring(targetFps) + dimensions + L"-v7.mp4" };
        }
        if (performanceMode == "power-saver") {
            return { targetFps, L"power-saver-" + std::to_wstring(targetFps) + dimensions + L"-v7.mp4" };
        }
        return { targetFps, L"balanced-" + std::to_wstring(targetFps) + dimensions + L"-v7.mp4" };
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

    [[nodiscard]] constexpr std::pair<uint32_t, uint32_t> video_sdr_variant_dimensions(
        std::string_view performanceMode,
        uint32_t sourceWidth, uint32_t sourceHeight,
        uint32_t displayWidth, uint32_t displayHeight) noexcept
    {
        if (performanceMode == "original") return { sourceWidth, sourceHeight };
        // Copies use the same centered fill geometry as Renderer. The output
        // rectangle is limited by source pixels; crop before scaling, never
        // shrink the full image and enlarge its remaining center afterwards.
        auto fill = [&](uint32_t width, uint32_t height) {
            if (!sourceWidth || !sourceHeight || !width || !height) return std::pair{sourceWidth, sourceHeight};
            if (width > sourceWidth || height > sourceHeight) {
                if (static_cast<uint64_t>(sourceWidth) * height <= static_cast<uint64_t>(sourceHeight) * width) {
                    height = static_cast<uint32_t>(static_cast<uint64_t>(height) * sourceWidth / width);
                    width = sourceWidth;
                } else {
                    width = static_cast<uint32_t>(static_cast<uint64_t>(width) * sourceHeight / height);
                    height = sourceHeight;
                }
            }
            return std::pair{width & ~1u, height & ~1u};
        };
        if (performanceMode == "balanced") {
            return fill(
                displayWidth ? displayWidth : sourceWidth,
                displayHeight ? displayHeight : sourceHeight);
        }
        bool portrait = displayWidth && displayHeight
            ? displayHeight > displayWidth : sourceHeight > sourceWidth;
        uint32_t maximumWidth = portrait ? 1080u : 1920u;
        uint32_t maximumHeight = portrait ? 1920u : 1080u;
        if (displayWidth) maximumWidth = (std::min)(maximumWidth, displayWidth);
        if (displayHeight) maximumHeight = (std::min)(maximumHeight, displayHeight);
        return fill(maximumWidth, maximumHeight);
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
