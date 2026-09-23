#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace motion
{
    struct VideoProbeInfo
    {
        uint32_t width{}, height{};
        uint32_t frameRateNumerator{}, frameRateDenominator{};
        uint32_t bitDepth{};
        uint64_t duration100ns{};
        std::string codecName, profile, pixelFormat;
        std::string colorTransfer, colorPrimaries, colorSpace, colorRange;
        // Coded width/height remain unchanged; callers swap their display
        // dimensions for a 90/270-degree container rotation.
        int rotationDegrees{};
    };

    // ffprobe is distributed alongside ffmpeg. Inspect only the first video
    // stream in a bounded child process; no source decoder is loaded in-process.
    [[nodiscard]] std::optional<VideoProbeInfo> probe_video(
        std::filesystem::path const& ffmpeg,
        std::filesystem::path const& source,
        uint32_t timeoutMs = 10000,
        std::function<bool()> const& cancelled = {});

    // Kept separate for deterministic tests of untrusted probe output.
    [[nodiscard]] std::optional<VideoProbeInfo> parse_video_probe_json(std::string_view json);

    namespace media_tool_detail
    {
        // Captures at most 256 KiB of stdout; stderr/stdin are redirected to NUL.
        // The child starts suspended and must join a kill-on-close job before
        // it can execute. Timeout, cancellation and oversized output fail closed.
        [[nodiscard]] std::optional<std::string> run_bounded(
            std::filesystem::path const& executable,
            std::vector<std::wstring> const& arguments,
            uint32_t timeoutMs,
            std::function<bool()> const& cancelled = {});
    }
}
