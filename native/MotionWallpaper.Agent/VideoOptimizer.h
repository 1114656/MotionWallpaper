#pragma once

#include "../MotionWallpaper.Common/Common.h"

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
        // True only while the selected balanced/power-saver copy is both
        // generatable now and actually queued/running. Source fallback alone
        // is not enough to freeze playback (for example on battery or after a
        // failed encode).
        bool performanceCopyPending{};
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
        void Prepare(std::filesystem::path const& source, std::string const& performanceMode,
            uint32_t targetWidth = 0, uint32_t targetHeight = 0,
            uint32_t targetRefreshRate = 0);
        [[nodiscard]] std::wstring SourceHardwareDecodeAdapter(
            std::filesystem::path const& source,
            std::wstring const& preferredAdapter = {},
            uint64_t aggregateOutputPixels = 0);
        void SetGenerationAllowed(bool allowed);
        void InvalidateChoices();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
