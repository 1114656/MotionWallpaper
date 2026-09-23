#pragma once

#include "../MotionWallpaper.Common/Common.h"
#include "VideoGpuProbe.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace motion::agent
{
    using VideoPlaybackLease = std::shared_ptr<void>;

    struct ResolvedVideoPath
    {
        std::filesystem::path path;
        // Opaque, copyable lifetime token. For a performance variant it pins
        // the exact cache file against optimizer-owned deletion. For any path
        // in an external media library it also retains the root/marker/Groups
        // identity handles until Renderer has stopped using the path.
        VideoPlaybackLease lease;
        // True whenever the selected balanced/power-saver tier or the current
        // cpu-smooth compatibility path cannot yet be honored by a validated
        // copy. This is intentionally independent of whether generation can
        // run now: battery, pause, cancellation, suppression and failure must
        // still use a static preview rather than continuously decoding the
        // full-quality source.
        bool performanceCopyRequired{};
        // True only while the required copy is eligible and actually queued
        // or running in this optimizer instance.
        bool performanceCopyPending{};
        bool gpuProbePending{};
    };

    class VideoOptimizer
    {
    public:
        VideoOptimizer(
            std::filesystem::path wallpapersPath,
            std::filesystem::path logRoot,
            std::filesystem::path applicationRoot,
            std::optional<motion::MediaLibraryTrustIdentity> libraryTrust = std::nullopt);
        ~VideoOptimizer();
        VideoOptimizer(VideoOptimizer const&) = delete;
        VideoOptimizer& operator=(VideoOptimizer const&) = delete;

        [[nodiscard]] std::filesystem::path Resolve(
            std::filesystem::path const& source, std::string const& performanceMode,
            uint32_t targetWidth = 0, uint32_t targetHeight = 0,
            uint32_t targetRefreshRate = 0,
            bool softwarePlaybackTarget = false);
        [[nodiscard]] ResolvedVideoPath ResolveWithLease(
            std::filesystem::path const& source, std::string const& performanceMode,
            uint32_t targetWidth = 0, uint32_t targetHeight = 0,
            uint32_t targetRefreshRate = 0,
            bool softwarePlaybackTarget = false,
            bool allowGenerationRequest = true);
        [[nodiscard]] VideoPlaybackLease AcquirePlaybackLease(
            std::filesystem::path const& path);
        // Nonblocking: returns a cached sRGB PNG (up to 4K), or queues one
        // source frame and returns empty while the small gallery poster shows.
        [[nodiscard]] ResolvedVideoPath ResolveStillPreview(
            std::filesystem::path const& source);
        void Prepare(std::filesystem::path const& source, std::string const& performanceMode,
            uint32_t targetWidth = 0, uint32_t targetHeight = 0,
            uint32_t targetRefreshRate = 0);
        [[nodiscard]] std::wstring SourceHardwareDecodeAdapter(
            std::filesystem::path const& source,
            std::wstring const& preferredAdapter = {},
            uint64_t aggregateOutputPixels = 0);
        [[nodiscard]] VideoGpuDecodeProbe SourceHardwareDecodeCandidates(
            std::filesystem::path const& source,
            std::wstring const& preferredAdapter = {},
            uint64_t aggregateOutputPixels = 0);
        // Zero means unlimited. The value is updated from settings while the
        // worker is live, so pruning always follows the user's current quota.
        void SetStorageQuotaBytes(uint64_t quotaBytes) noexcept;
        void SetGenerationAllowed(bool allowed);
        // Atomically prevents the worker from taking another request, asks an
        // active transcode to stop, and waits for its process/cleanup path to
        // become idle. A timeout leaves generation disabled and must be
        // treated by callers as a fail-closed presentation barrier.
        [[nodiscard]] bool Quiesce(uint32_t timeoutMilliseconds);
        void InvalidateChoices();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
