#pragma once

#include "../MotionWallpaper.Common/Common.h"
#include "../MotionWallpaper.Common/VariantCache.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

namespace motion::app
{
    enum class DeleteMode { RecycleBin, Permanent };

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
        motion::GroupMetadata CreateGroup(std::wstring const& name, std::vector<motion::GroupMetadata> const& existing);
        void RenameGroup(motion::GroupMetadata const& group, std::wstring const& name, std::vector<motion::GroupMetadata> const& existing);
        void ReorderGroup(std::string const& groupId, int direction, std::vector<motion::GroupMetadata> const& groups);
        void SetGroupOrder(std::vector<std::string> const& orderedIds, std::vector<motion::GroupMetadata> const& groups);
        void DeleteGroup(motion::GroupMetadata const& group);
        using ImportProgress = std::function<void(uint64_t copiedBytes, uint64_t totalBytes)>;
        std::string Import(std::filesystem::path const& source, std::string const& kind, std::string const& groupId,
            ImportProgress const& progress = {}, std::atomic_bool const* cancelled = nullptr);
        void Rename(motion::MediaMetadata const& media, std::wstring const& name);
        void UpdateCover(motion::MediaMetadata const& media, std::wstring const& coverFileName);
        bool EnsureCover(motion::MediaMetadata const& media);
        bool RequestOptimization(motion::MediaMetadata const& media, std::string const& mode);
        void PauseOptimization(motion::MediaMetadata const& media);
        void ResumeOptimization(motion::MediaMetadata const& media);
        void CancelOptimization(motion::MediaMetadata const& media);
        void SuppressOptimization(motion::MediaMetadata const& media, std::string const& mode);
        void DeleteVariantProfile(motion::MediaMetadata const& media, std::string const& mode);
        void DeleteVariantProfiles(motion::MediaMetadata const& media,
            std::vector<std::string> const& modes);
        void DeleteVariants(motion::MediaMetadata const& media);
        motion::VariantCacheStatus VariantStatus(motion::MediaMetadata const& media) const;
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
