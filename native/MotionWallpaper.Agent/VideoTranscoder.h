#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace motion::agent
{
    enum class VideoTranscodeControl { running, paused, cancelled };
    enum class VideoTranscodeResult { succeeded, unsupported, paused, cancelled, failed };

    enum class VideoTranscodeBackend
    {
        nvidiaNvenc,
        intelQsv,
        amdAmf,
        softwareOpenH264
    };

    enum class VideoTranscodeCodec { H264, HevcMain10 };

    [[nodiscard]] constexpr VideoTranscodeCodec video_transcode_backend_codec(
        VideoTranscodeBackend backend, bool sdrH264Allowed) noexcept
    {
        return backend == VideoTranscodeBackend::softwareOpenH264 || sdrH264Allowed
            ? VideoTranscodeCodec::H264 : VideoTranscodeCodec::HevcMain10;
    }

    struct VideoTranscodeAdapter
    {
        uint32_t vendorId{};
        uint64_t dedicatedVideoMemory{};
        uint32_t dxgiAdapterIndex{};
        int32_t luidHigh{};
        uint32_t luidLow{};
        bool identityKnown{};
    };

    struct VideoTranscodeCandidate
    {
        VideoTranscodeBackend backend{ VideoTranscodeBackend::softwareOpenH264 };
        VideoTranscodeAdapter adapter;
        bool adapterBound{};
    };

    struct VideoTranscodeRateControl
    {
        uint32_t averageKbps{};
        uint32_t maximumKbps{};
        uint32_t bufferKbps{};
        uint64_t maximumOutputBytes{};
    };

    struct VideoTranscodeProgress
    {
        uint32_t percent{};
        uint64_t processedMicroseconds{};
        uint64_t durationMicroseconds{};
        uint32_t attempt{};
        VideoTranscodeBackend backend{ VideoTranscodeBackend::softwareOpenH264 };
        bool determinate{};
        bool attemptStarted{};
    };

    using VideoTranscodeProgressCallback = std::function<void(VideoTranscodeProgress const&)>;
    using VideoTranscodeCandidateValidator = std::function<bool(
        std::filesystem::path const&, VideoTranscodeBackend, VideoTranscodeCodec)>;
    // Returns an opaque lease that pins the trusted media-library identity for
    // one path-I/O phase. A configured callback returning null means the drive
    // or owned library changed and no cleanup/write may be attempted.
    using VideoTranscodePathAccess = std::function<std::shared_ptr<void>()>;

    [[nodiscard]] std::vector<VideoTranscodeCandidate> video_transcode_backend_order(
        std::vector<VideoTranscodeAdapter> adapters,
        uint32_t width,
        uint32_t height,
        uint32_t targetFps,
        bool adapterProbeSucceeded = true,
        bool softwareFallbackAllowed = true,
        bool softwarePlaybackTarget = false);

    [[nodiscard]] std::wstring video_transcode_backend_name(VideoTranscodeBackend backend);

    // Selects a quality-oriented rate first, then constrains it to the same
    // source-relative file-growth budget used when accepting the completed
    // output. A zero size or duration keeps the normal quality target.
    [[nodiscard]] VideoTranscodeRateControl video_transcode_rate_control(
        uint32_t width,
        uint32_t height,
        uint32_t targetFps,
        VideoTranscodeCodec codec,
        uint64_t sourceSizeBytes = 0,
        uint64_t sourceDuration100ns = 0) noexcept;

    VideoTranscodeResult transcode_video(
        std::filesystem::path const& ffmpeg,
        std::filesystem::path const& source,
        std::filesystem::path const& destination,
        uint32_t width,
        uint32_t height,
        uint32_t targetFps,
        std::function<VideoTranscodeControl()> const& control,
        std::wstring& error,
        std::wstring* selectedBackend = nullptr,
        bool softwareFallbackAllowed = true,
        bool softwarePlaybackTarget = false,
        uint64_t sourceDuration100ns = 0,
        VideoTranscodeProgressCallback const& progress = {},
        VideoTranscodeCandidateValidator const& validateCandidate = {},
        VideoTranscodeCodec* selectedCodec = nullptr,
        VideoTranscodePathAccess const& pathAccess = {});

    // Media Foundation must already be started on the calling process. The
    // probe succeeds only after a real uncompressed first video sample has
    // been produced by an installed decoder.
    [[nodiscard]] bool video_candidate_decodes_first_frame(
        std::filesystem::path const& candidate) noexcept;
}
