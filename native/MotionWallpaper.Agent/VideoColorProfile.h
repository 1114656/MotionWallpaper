#pragma once
#include "../MotionWallpaper.Common/MediaProbe.h"
#include <string>

namespace motion::agent {
    // These are independent capabilities: 10-bit decoding does not imply an
    // HDR monitor, and an HDR-capable monitor may have HDR disabled in Windows.
    struct VideoColorCapabilities {
        bool wideColor{};
        bool hdr{};
        bool operator==(VideoColorCapabilities const&) const = default;
    };
    struct VideoColorOptions {
        bool adaptive{};
        VideoColorCapabilities display;
        bool compatibility{};
        bool operator==(VideoColorOptions const&) const = default;
    };
    struct VideoColorProfile {
        bool tenBit{};
        std::string primaries{"bt709"}, transfer{"bt709"}, matrix{"bt709"};
        bool hdr() const { return transfer == "smpte2084" || transfer == "arib-std-b67"; }
    };
    inline bool video_color_requires_builtin(VideoColorProfile const& profile)
    {
        return profile.tenBit || profile.primaries != "bt709" ||
            profile.transfer != "bt709" || profile.matrix != "bt709";
    }
    inline VideoColorProfile video_color_profile(motion::VideoProbeInfo const& source,
        VideoColorOptions options, bool tenBit)
    {
        VideoColorProfile result;
        result.tenBit = options.adaptive && tenBit && source.bitDepth >= 10;
        // The explicit compatibility copy normalizes color with the CPU
        // pipeline, but retains 10-bit precision when encoding/playback work.
        if (!options.adaptive || options.compatibility) return result;
        bool hdr = source.colorTransfer == "smpte2084" || source.colorTransfer == "arib-std-b67";
        // Only transfer functions/gamuts implemented by the bundled playback
        // shader can be retained. Unknown tags still go through validated SDR
        // conversion; they are never interpreted as proof of HDR support.
        bool knownGamut = source.colorPrimaries == "bt709" || source.colorPrimaries == "smpte432" ||
            source.colorPrimaries == "bt2020";
        bool knownSdr = source.colorTransfer == "bt709" || source.colorTransfer == "bt470m" ||
            source.colorTransfer == "iec61966-2-1" || source.colorTransfer == "bt2020-10";
        // BT.709 primaries already fit an ordinary SDR display. Its supported
        // SDR transfer need not be rewritten merely because Advanced Color
        // is disabled: the bundled player interprets that transfer explicitly.
        if (source.colorPrimaries == "bt709" && knownSdr) {
            result.transfer = source.colorTransfer;
        } else if (knownGamut && options.display.wideColor && (hdr || knownSdr)) {
            result.primaries = source.colorPrimaries;
            result.transfer = hdr && !(options.display.hdr && result.tenBit) ? "bt709" : source.colorTransfer;
            result.matrix = source.colorPrimaries == "bt2020" ? "bt2020nc" : "bt709";
        }
        return result;
    }
    inline bool video_matches_color_profile(motion::VideoProbeInfo const& actual,
        VideoColorProfile const& profile)
    {
        return actual.rotationDegrees == 0 && actual.codecName == (profile.tenBit ? "hevc" : "h264") &&
            actual.bitDepth == (profile.tenBit ? 10u : 8u) &&
            actual.pixelFormat == (profile.tenBit ? "yuv420p10le" : "yuv420p") &&
            actual.colorPrimaries == profile.primaries && actual.colorTransfer == profile.transfer &&
            actual.colorSpace == profile.matrix && (actual.colorRange == "tv" || actual.colorRange == "mpeg");
    }
    inline bool video_matches_color_options(motion::VideoProbeInfo const& actual,
        motion::VideoProbeInfo const& source, VideoColorOptions options,
        bool allowCompatibleFallback = false)
    {
        if (actual.bitDepth != 8 && actual.bitDepth != 10) return false;
        if (video_matches_color_profile(actual, video_color_profile(source, options, actual.bitDepth == 10))) return true;
        if (!allowCompatibleFallback || !options.adaptive || options.compatibility) return false;
        options.compatibility = true;
        return video_matches_color_profile(actual, video_color_profile(source, options, actual.bitDepth == 10));
    }
    inline std::wstring video_color_variant_name(std::wstring name, VideoColorOptions options)
    {
        if (!options.adaptive || !name.ends_with(L"-v7.mp4")) return name;
        name.resize(name.size() - 7);
        if (options.compatibility) return name + L"-compatible-v9.mp4";
        return name + (options.display.hdr ? L"-adaptive-hdr-v8.mp4" :
            options.display.wideColor ? L"-adaptive-wide-v8.mp4" : L"-adaptive-sdr-v8.mp4");
    }
}
