#pragma once

#include "../MotionWallpaper.Common/Common.h"
#include "../MotionWallpaper.Common/VariantCache.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace motion::app
{
    inline constexpr uint32_t maximumImportedVideoLongEdge = 7680;
    inline constexpr uint32_t maximumImportedVideoShortEdge = 4320;
    inline constexpr uint32_t maximumImportedVideoFrameRate = 240;

    // Treat 8K as UHD 7680x4320 and apply the same budget to portrait media.
    // An aspect ratio alone must not let a dimension or total decode surface
    // exceed that envelope.
    [[nodiscard]] constexpr bool video_import_dimensions_allowed(
        uint32_t width, uint32_t height) noexcept
    {
        if (!width || !height) return false;
        auto longEdge = width > height ? width : height;
        auto shortEdge = width > height ? height : width;
        return longEdge <= maximumImportedVideoLongEdge &&
            shortEdge <= maximumImportedVideoShortEdge;
    }

    [[nodiscard]] constexpr bool video_import_frame_rate_allowed(
        uint32_t numerator, uint32_t denominator) noexcept
    {
        return numerator && denominator &&
            static_cast<uint64_t>(numerator) <=
                static_cast<uint64_t>(maximumImportedVideoFrameRate) * denominator;
    }

    enum class DeleteMode { RecycleBin, Permanent };

    enum class MediaCatalogSort { Name, Newest, Size, Kind };

    struct MediaQuery
    {
        // Empty groupId searches the entire library. Text is matched against
        // wallpaper name, original file name, group name, and tags.
        std::string groupId;
        std::wstring text;
        std::string kind;
        bool favoritesOnly{};
        std::vector<std::wstring> tags;
        MediaCatalogSort sort{ MediaCatalogSort::Name };
    };

    struct CatalogMediaEntry
    {
        motion::MediaMetadata media;
        std::wstring groupName;
    };

    struct DuplicateMediaSet
    {
        std::wstring sha256;
        std::string kind;
        uint64_t sizeBytes{};
        std::vector<CatalogMediaEntry> items;
    };

    struct OptimizationStorageSummary
    {
        uint64_t bytes{};
        uint64_t reclaimableBytes{};
        uint32_t files{};
        uint32_t mediaWithCopies{};
        uint32_t queuedTasks{};
    };

    struct OptimizationCleanupResult
    {
        OptimizationStorageSummary before;
        OptimizationStorageSummary after;
        uint64_t freedBytes{};
        uint32_t cleanedMedia{};
        uint32_t skippedProtected{};
        uint32_t skippedSourceLess{};
    };

    struct GroupLoadResult
    {
        std::vector<motion::GroupMetadata> groups;
    };

    class MediaLibrary
    {
    public:
        explicit MediaLibrary(std::filesystem::path root, DeleteMode deleteMode = DeleteMode::RecycleBin,
            std::filesystem::path wallpapersPath = {},
            std::optional<motion::MediaLibraryTrustIdentity> expectedIdentity = std::nullopt);
        void EnsureDirectories() const;
        std::filesystem::path WallpapersPath() const;
        GroupLoadResult LoadGroups();
        std::vector<motion::MediaMetadata> LoadMedia(std::string const& groupId);
        std::vector<CatalogMediaEntry> QueryMedia(MediaQuery const& query = {});
        std::vector<DuplicateMediaSet> FindDuplicateMedia();
        motion::GroupMetadata CreateGroup(std::wstring const& name, std::vector<motion::GroupMetadata> const& existing);
        void RenameGroup(motion::GroupMetadata const& group, std::wstring const& name, std::vector<motion::GroupMetadata> const& existing);
        void ReorderGroup(std::string const& groupId, int direction, std::vector<motion::GroupMetadata> const& groups);
        void SetGroupOrder(std::vector<std::string> const& orderedIds, std::vector<motion::GroupMetadata> const& groups);
        void DeleteGroup(motion::GroupMetadata const& group);
        using ImportProgress = std::function<void(uint64_t copiedBytes, uint64_t totalBytes)>;
        std::string Import(std::filesystem::path const& source, std::string const& kind, std::string const& groupId,
            ImportProgress const& progress = {}, std::atomic_bool const* cancelled = nullptr);
        void Rename(motion::MediaMetadata const& media, std::wstring const& name);
        void SetFavorite(motion::MediaMetadata const& media, bool favorite);
        void SetFavorite(std::vector<motion::MediaMetadata> const& media, bool favorite);
        void SetTags(motion::MediaMetadata const& media, std::vector<std::wstring> const& tags);
        void AddTags(std::vector<motion::MediaMetadata> const& media,
            std::vector<std::wstring> const& tags);
        motion::MediaMetadata MergeDuplicateMedia(
            motion::MediaMetadata const& keep, motion::MediaMetadata const& duplicate);
        void UpdateCover(motion::MediaMetadata const& media, std::wstring const& coverFileName);
        bool EnsureCover(motion::MediaMetadata const& media);
        bool RequestOptimization(motion::MediaMetadata const& media, std::string const& mode,
            bool automatic = false);
        void PauseOptimization(motion::MediaMetadata const& media);
        void ResumeOptimization(motion::MediaMetadata const& media);
        void CancelOptimization(motion::MediaMetadata const& media);
        void SuppressOptimization(motion::MediaMetadata const& media, std::string const& mode);
        void DeleteVariantProfile(motion::MediaMetadata const& media, std::string const& mode);
        void DeleteVariantProfiles(motion::MediaMetadata const& media,
            std::vector<std::string> const& modes);
        void DeleteVariants(motion::MediaMetadata const& media);
        motion::VariantCacheStatus VariantStatus(motion::MediaMetadata const& media) const;
        OptimizationStorageSummary InspectOptimizationStorage();
        // Renderer/Agent users must be quiesced by the caller. Source-less
        // wallpapers and explicitly protected media are never reclaimed.
        OptimizationCleanupResult TrimOptimizationStorage(uint64_t quotaBytes,
            std::vector<std::string> const& protectedMediaIds = {});
        OptimizationCleanupResult ReleaseOptimizationStorage(
            std::vector<std::string> const& protectedMediaIds = {});
        bool SourceAvailable(motion::MediaMetadata const& media) const;
        void DeleteSource(motion::MediaMetadata const& media);
        void Move(motion::MediaMetadata const& media, std::string const& targetGroupId);
        void Delete(motion::MediaMetadata const& media);
        std::filesystem::path MediaDirectory(motion::MediaMetadata const& media) const;
    private:
        [[nodiscard]] bool LibraryTrusted() const noexcept;
        [[nodiscard]] bool StableLibraryTrusted() const noexcept;
        void RequireTrustedLibrary() const;
        std::filesystem::path const& AccessWallpapersPath() const noexcept;
        std::filesystem::path ConfiguredAliasPath(
            std::filesystem::path const& accessPath) const;
        void RecoverInterruptedRecycleDeletes() const;
        void RecoverInterruptedMoves() const;
        void RecoverInterruptedVariantDeletions(
            std::filesystem::path const& mediaDirectory) const;
        void ReclaimVariantsForStorageQuota(motion::MediaMetadata const& media);
        void MutateCatalogMetadata(std::vector<motion::MediaMetadata> const& media,
            std::function<void(motion::MediaMetadata&)> const& mutation);
        void DeletePath(std::filesystem::path const& path) const;
        std::filesystem::path ResolveMediaDirectory(motion::MediaMetadata const& media) const;
        std::filesystem::path root_;
        std::filesystem::path wallpapersPath_;
        std::filesystem::path accessWallpapersPath_;
        std::optional<motion::MediaLibraryTrustIdentity> expectedIdentity_;
        // A custom path is never implicitly trusted. Keeping an unavailable
        // instance is useful for reconnect UI, but every operation stays
        // fail-closed until a captured identity is supplied.
        bool identityRequired_{};
        DeleteMode deleteMode_;
        mutable bool recycleRecoveryComplete_{};
        mutable bool moveRecoveryComplete_{};
        mutable std::mutex mutex_;
    };
}
