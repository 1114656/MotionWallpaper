#pragma once

#include <windows.h>
#include "UniqueHandle.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace motion
{
    inline constexpr wchar_t settings_event_name[] = L"Local\\MotionWallpaper.SettingsChanged";
    inline constexpr wchar_t app_exit_event_name[] = L"Local\\MotionWallpaper.ExitRequested";
    inline constexpr wchar_t library_migration_request_event_name[] = L"Local\\MotionWallpaper.LibraryMigrationRequested";
    inline constexpr wchar_t library_migration_quiesced_event_name[] = L"Local\\MotionWallpaper.LibraryMigrationQuiesced";
    inline constexpr wchar_t library_migration_applied_event_name[] = L"Local\\MotionWallpaper.LibraryMigrationApplied";
    inline constexpr wchar_t library_migration_owner_mutex_name[] = L"Local\\MotionWallpaper.LibraryMigrationOwner.Mutex";
    inline constexpr wchar_t library_migration_owner_mapping_name[] = L"Local\\MotionWallpaper.LibraryMigrationOwner.Mapping";
    inline constexpr wchar_t legacy_data_fallback_marker_name[] = L"legacy-data-fallback.mode";
    inline constexpr wchar_t legacy_data_conflict_marker_name[] = L"legacy-data-conflict.mode";
    inline constexpr wchar_t media_library_ownership_marker_name[] = L".motionwallpaper-library";
    inline constexpr char media_library_ownership_marker_prefix[] = "MotionWallpaper.Library/v1\n";
    inline constexpr int settings_schema_version = 10;
    inline constexpr int runtime_schema_version = 1;
    inline constexpr int group_schema_version = 1;
    inline constexpr int media_schema_version = 2;

    struct DisplayAssignment
    {
        std::string displayId;
        std::string groupId;
        std::string mediaId;
    };

    struct Settings
    {
        int version{ settings_schema_version };
        bool desktopPlayback{ true };
        bool activePlaybackEnabled{ true };
        bool continueWhenCovered{ false };
        bool screensaverEnabled{ true };
        int idleTimeoutSeconds{ 30 };
        bool autoLockEnabled{ true };
        int autoLockTimeoutSeconds{ 300 };
        bool displayOffAfterLockEnabled{ true };
        int displayOffAfterLockDelaySeconds{ 30 };
        std::string decodeMode{ "auto" };
        std::string performanceMode{ "balanced" };
        std::wstring mediaLibraryPath;
        std::string mediaLibraryId;
        std::string selectedGroupId;
        std::string selectedMediaId;
        std::string randomGroupId;
        int randomIntervalMinutes{};
        bool startWithWindows{};
        std::string displayMode{ "independent" };
        std::vector<DisplayAssignment> displayAssignments;
    };

    struct GroupMetadata
    {
        int version{ group_schema_version };
        std::string id;
        std::wstring name;
        int order{};
        std::wstring createdAt;
        std::wstring updatedAt;
    };

    struct RuntimeState
    {
        int version{ runtime_schema_version };
        std::string activeGroupId;
        std::string activeMediaId;
        std::string decodePath;
        std::string decodeReason;
        std::wstring updatedAt;
    };

    struct MediaMetadata
    {
        int version{ media_schema_version };
        std::string id;
        std::string groupId;
        std::wstring name;
        std::wstring originalName;
        std::wstring fileName;
        std::string kind{ "video" };
        std::wstring coverFileName;
        std::wstring sha256;
        uint64_t sizeBytes{};
        uint64_t revision{};
        std::wstring importedAt;
        std::wstring updatedAt;
    };

    enum class DesktopIntent { Off, Play, Freeze, Pause };
    enum class SettingsFileStatus { missing, valid, libraryUnavailable, invalid };

    struct FilesystemObjectIdentity
    {
        uint64_t volumeSerialNumber{};
        std::array<uint8_t, 16> fileId{};
        bool operator==(FilesystemObjectIdentity const&) const = default;
    };

    struct MediaLibraryTrustIdentity
    {
        // User-configured alias retained for settings/UI and checked on every
        // acquisition. stableRoot is an in-memory volume-GUID path derived
        // from the opened root handle; it is deliberately never persisted.
        std::filesystem::path root;
        std::filesystem::path stableRoot;
        std::string ownershipId;
        FilesystemObjectIdentity rootIdentity;
        FilesystemObjectIdentity markerIdentity;
        FilesystemObjectIdentity groupsIdentity;
    };

    // Holds direct handles that deny rename/delete of the trusted root,
    // ownership marker, and Groups directory for the lifetime of a media
    // operation. Physical removal is still detected by identity revalidation.
    class MediaLibraryTrustLease final
    {
    public:
        MediaLibraryTrustLease(MediaLibraryTrustLease const&) = delete;
        MediaLibraryTrustLease& operator=(MediaLibraryTrustLease const&) = delete;

    private:
        friend std::shared_ptr<MediaLibraryTrustLease> acquire_media_library_trust(
            MediaLibraryTrustIdentity const&) noexcept;
        MediaLibraryTrustLease(unique_handle root, unique_handle stableRoot,
            unique_handle marker, unique_handle groups) noexcept;
        unique_handle root_;
        unique_handle stableRoot_;
        unique_handle marker_;
        unique_handle groups_;
    };

    class IdleTimer
    {
    public:
        std::chrono::milliseconds Update(
            std::chrono::milliseconds now,
            uint32_t inputTick,
            std::chrono::milliseconds rawIdle,
            bool activityInhibitsIdle) noexcept;

    private:
        bool initialized_{};
        uint32_t inputTick_{};
        std::chrono::milliseconds idleSince_{};
    };

    // Serializes migration ownership across App processes and lets the Agent
    // distinguish a live requester from a manual-reset event orphaned by a
    // terminated process. PID reuse is rejected with the process creation time.
    class LibraryMigrationOwnerChannel final
    {
    public:
        LibraryMigrationOwnerChannel(
            wchar_t const* mutexName = library_migration_owner_mutex_name,
            wchar_t const* mappingName = library_migration_owner_mapping_name) noexcept;
        ~LibraryMigrationOwnerChannel();
        LibraryMigrationOwnerChannel(LibraryMigrationOwnerChannel const&) = delete;
        LibraryMigrationOwnerChannel& operator=(LibraryMigrationOwnerChannel const&) = delete;

        explicit operator bool() const noexcept;
        bool TryClaimAndReset(HANDLE requested, HANDLE quiesced, HANDLE applied) noexcept;
        bool ResetRequestForClaim(HANDLE requested) noexcept;
        void ReleaseClaim() noexcept;
        bool ClearOrphanedRequest(HANDLE requested, HANDLE quiesced, HANDLE applied) noexcept;

    private:
        unique_handle mutex_;
        unique_handle mapping_;
        void* view_{};
        uint64_t processCreationTime_{};
        uint64_t token_{};
    };

    std::filesystem::path executable_directory();
    std::filesystem::path application_data_directory();
    std::filesystem::path wallpaper_library_directory(
        std::filesystem::path const& dataRoot,
        std::wstring const& configuredPath = {});
    std::filesystem::path select_application_data_directory(
        std::filesystem::path const& applicationRoot,
        std::filesystem::path const& localAppDataRoot);
    bool legacy_data_conflict_present(
        std::filesystem::path const& applicationRoot) noexcept;
    bool same_filesystem_path(
        std::filesystem::path const& left,
        std::filesystem::path const& right);
    bool filesystem_path_is_nested(
        std::filesystem::path const& parent,
        std::filesystem::path const& child);
    std::filesystem::path ffmpeg_executable_path(std::filesystem::path const& applicationRoot);
    std::wstring utf8_to_wide(std::string const& value);
    std::string wide_to_utf8(std::wstring_view value);
    std::string new_id();
    std::wstring timestamp_utc();
    bool valid_id(std::string const& value);
    bool valid_id(std::wstring const& value);
    bool safe_file_name(std::filesystem::path const& value);
    std::optional<std::string> media_library_ownership_id(
        std::filesystem::path const& libraryRoot) noexcept;
    bool is_owned_media_library(std::filesystem::path const& libraryRoot) noexcept;
    std::optional<MediaLibraryTrustIdentity> capture_media_library_trust(
        std::filesystem::path const& libraryRoot) noexcept;
    std::shared_ptr<MediaLibraryTrustLease> acquire_media_library_trust(
        MediaLibraryTrustIdentity const& identity) noexcept;
    bool revalidate_media_library_trust(
        MediaLibraryTrustIdentity const& identity) noexcept;
    bool revalidate_media_library_stable_root(
        MediaLibraryTrustIdentity const& identity) noexcept;
    std::optional<std::filesystem::path> media_library_stable_path(
        MediaLibraryTrustIdentity const& identity,
        std::filesystem::path const& configuredPath) noexcept;
    bool same_direct_filesystem_object(
        std::filesystem::path const& left,
        std::filesystem::path const& right) noexcept;
    void append_utf8_log(std::filesystem::path const& path, std::wstring_view message) noexcept;
    std::wstring quote_command_line_argument(std::wstring_view value);
    std::wstring build_command_line(std::vector<std::wstring> const& arguments);

    std::optional<Settings> load_settings(std::filesystem::path const& path);
    bool try_load_settings(std::filesystem::path const& path, Settings& destination) noexcept;
    SettingsFileStatus load_settings_file(
        std::filesystem::path const& path,
        Settings& destination) noexcept;
    void save_settings(std::filesystem::path const& path, Settings const& settings);
    std::optional<RuntimeState> load_runtime(std::filesystem::path const& path);
    bool try_load_runtime(std::filesystem::path const& path, RuntimeState& destination) noexcept;
    void save_runtime(std::filesystem::path const& path, RuntimeState const& runtime);
    std::optional<GroupMetadata> load_group(std::filesystem::path const& path);
    void save_group(std::filesystem::path const& path, GroupMetadata const& group);
    std::optional<MediaMetadata> load_media(std::filesystem::path const& path);
    bool try_load_media(std::filesystem::path const& path, MediaMetadata& destination) noexcept;
    void save_media(std::filesystem::path const& path, MediaMetadata const& media);

    DesktopIntent desktop_intent(Settings const& settings, bool covered, bool hasMedia);
    bool notify_settings_changed();
}
