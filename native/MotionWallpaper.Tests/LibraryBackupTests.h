#pragma once

#include "../MotionWallpaper.App/LibraryBackup.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <string>

namespace motion::tests
{
    inline void library_backup_restores_verified_snapshot(
        std::filesystem::path const& testRoot,
        void (*require)(bool, char const*))
    {
        namespace fs = std::filesystem;
        constexpr char libraryId[] = "12121212-1212-1212-1212-121212121212";
        constexpr char groupId[] = "34343434-3434-3434-3434-343434343434";
        constexpr char mediaId[] = "56565656-5656-5656-5656-565656565656";
        std::string original = "verified backup wallpaper bytes";
        auto dataRoot = testRoot / L"backup-roundtrip-data";
        auto library = dataRoot / L"Wallpapers";
        auto mediaDirectory = library / L"Groups" /
            motion::utf8_to_wide(groupId) / L"Videos" /
            motion::utf8_to_wide(mediaId);
        fs::create_directories(mediaDirectory);
        std::ofstream(library / motion::media_library_ownership_marker_name,
            std::ios::binary) << motion::media_library_ownership_marker_prefix <<
            libraryId << '\n';
        motion::GroupMetadata group;
        group.id = groupId;
        group.name = L"Backup test";
        group.createdAt = group.updatedAt = motion::timestamp_utc();
        motion::save_group(mediaDirectory.parent_path().parent_path() /
            L"group.json", group);
        motion::MediaMetadata media;
        media.id = mediaId;
        media.groupId = groupId;
        media.name = media.originalName = media.fileName = L"wallpaper.mp4";
        media.kind = "video";
        media.sizeBytes = original.size();
        media.revision = 1;
        media.importedAt = media.updatedAt = motion::timestamp_utc();
        motion::save_media(mediaDirectory / L"metadata.json", media);
        std::ofstream(mediaDirectory / media.fileName, std::ios::binary) << original;

        motion::Settings settings;
        settings.selectedGroupId = groupId;
        settings.selectedMediaId = mediaId;
        motion::save_settings(dataRoot / L"Config" / L"settings.json", settings);
        auto identity = motion::capture_media_library_trust(library);
        require(identity.has_value(), "backup test library identity was not captured");
        auto destination = testRoot / L"backup-roundtrip-destination";
        fs::create_directories(destination);
        bool sawCopy{};
        bool sawVerification{};
        auto created = motion::app::LibraryBackupService::Create(
            dataRoot, library, *identity, destination,
            [&](motion::app::LibraryBackupProgress const& update) {
                sawCopy = sawCopy ||
                    update.phase == motion::app::LibraryBackupPhase::CopyingLibrary;
                sawVerification = sawVerification ||
                    update.phase == motion::app::LibraryBackupPhase::Verifying;
            });
        require(fs::is_directory(created.backup.path) && sawCopy && sawVerification,
            "backup did not publish a completed directory with observable progress");
        auto inspected = motion::app::LibraryBackupService::Validate(
            created.backup.path);
        require(inspected.backupId == created.backup.backupId &&
            inspected.libraryId == libraryId && inspected.fileCount >= 4,
            "completed backup did not validate against its manifest");

        std::ofstream(mediaDirectory / media.fileName,
            std::ios::binary | std::ios::trunc) << "changed after backup";
        settings.selectedGroupId.clear();
        settings.selectedMediaId.clear();
        motion::save_settings(dataRoot / L"Config" / L"settings.json", settings);
        auto restoreIdentity = motion::capture_media_library_trust(library);
        require(restoreIdentity.has_value(),
            "active library identity changed before restore test");
        auto restored = motion::app::LibraryBackupService::Restore(
            dataRoot, *restoreIdentity, created.backup.path);
        std::ifstream restoredSource(library / L"Groups" /
            motion::utf8_to_wide(groupId) / L"Videos" /
            motion::utf8_to_wide(mediaId) / L"wallpaper.mp4", std::ios::binary);
        std::string restoredBytes((std::istreambuf_iterator<char>(restoredSource)), {});
        auto restoredSettings = motion::load_settings(
            dataRoot / L"Config" / L"settings.json");
        require(restoredBytes == original && restoredSettings &&
            restoredSettings->selectedGroupId == groupId &&
            restoredSettings->selectedMediaId == mediaId &&
            fs::is_directory(restored.previousLibraryPath) &&
            fs::is_regular_file(restored.previousSettingsPath),
            "restore did not atomically activate the backed-up library and settings");

        std::ofstream(created.backup.path / L"manifest.json",
            std::ios::binary | std::ios::app) << "tampered";
        bool rejectedTampering{};
        try {
            (void)motion::app::LibraryBackupService::Validate(created.backup.path);
        } catch (...) {
            rejectedTampering = true;
        }
        require(rejectedTampering,
            "backup validation accepted a manifest changed after completion");
    }

    namespace library_backup_detail
    {
        struct Fixture
        {
            std::filesystem::path dataRoot;
            std::filesystem::path library;
            std::filesystem::path mediaDirectory;
            std::string groupId;
            std::string mediaId;
            std::string backedUpBytes;
            motion::app::LibraryBackupResult backup;
        };

        inline std::string read_bytes(std::filesystem::path const& path)
        {
            std::ifstream input(path, std::ios::binary);
            return { std::istreambuf_iterator<char>(input), {} };
        }

        inline Fixture create_fixture(std::filesystem::path const& root,
            std::string const& label, void (*require)(bool, char const*))
        {
            namespace fs = std::filesystem;
            Fixture fixture;
            fixture.groupId = "67676767-6767-6767-6767-676767676767";
            fixture.mediaId = "89898989-8989-8989-8989-898989898989";
            fixture.backedUpBytes = "durable restore snapshot " + label;
            fixture.dataRoot = root / motion::utf8_to_wide(label + "-data");
            fixture.library = fixture.dataRoot / L"Wallpapers";
            fixture.mediaDirectory = fixture.library / L"Groups" /
                motion::utf8_to_wide(fixture.groupId) / L"Videos" /
                motion::utf8_to_wide(fixture.mediaId);
            fs::create_directories(fixture.mediaDirectory);
            std::ofstream(fixture.library /
                motion::media_library_ownership_marker_name, std::ios::binary) <<
                motion::media_library_ownership_marker_prefix <<
                "45454545-4545-4545-4545-454545454545\n";
            motion::GroupMetadata group;
            group.id = fixture.groupId;
            group.name = L"Journal test";
            group.createdAt = group.updatedAt = motion::timestamp_utc();
            motion::save_group(fixture.mediaDirectory.parent_path().parent_path() /
                L"group.json", group);
            motion::MediaMetadata media;
            media.id = fixture.mediaId;
            media.groupId = fixture.groupId;
            media.name = media.originalName = media.fileName = L"wallpaper.mp4";
            media.kind = "video";
            media.sizeBytes = fixture.backedUpBytes.size();
            media.revision = 1;
            media.importedAt = media.updatedAt = motion::timestamp_utc();
            motion::save_media(fixture.mediaDirectory / L"metadata.json", media);
            std::ofstream(fixture.mediaDirectory / media.fileName,
                std::ios::binary) << fixture.backedUpBytes;
            motion::Settings settings;
            settings.selectedGroupId = fixture.groupId;
            settings.selectedMediaId = fixture.mediaId;
            motion::save_settings(fixture.dataRoot / L"Config" /
                L"settings.json", settings);
            auto identity = motion::capture_media_library_trust(fixture.library);
            require(identity.has_value(),
                "journal test library identity was not captured");
            auto destination = root / motion::utf8_to_wide(label + "-backup");
            fs::create_directories(destination);
            fixture.backup = motion::app::LibraryBackupService::Create(
                fixture.dataRoot, fixture.library, *identity, destination);
            return fixture;
        }

        inline void mutate_live_state(Fixture const& fixture)
        {
            std::ofstream(fixture.mediaDirectory / L"wallpaper.mp4",
                std::ios::binary | std::ios::trunc) << "live state after backup";
            motion::Settings settings;
            motion::save_settings(fixture.dataRoot / L"Config" /
                L"settings.json", settings);
        }
    }

    inline void library_restore_recovers_every_durable_crash_boundary(
        std::filesystem::path const& testRoot,
        void (*require)(bool, char const*))
    {
        using motion::app::LibraryRestoreCrashPoint;
        using motion::app::LibraryRestoreRecoveryOutcome;
        struct Boundary
        {
            LibraryRestoreCrashPoint point;
            bool completes;
            char const* name;
        };
        constexpr std::array boundaries{
            Boundary{ LibraryRestoreCrashPoint::JournalPrepared, false, "journal-prepared" },
            Boundary{ LibraryRestoreCrashPoint::PreviousLibraryRenamed, false, "previous-renamed" },
            Boundary{ LibraryRestoreCrashPoint::RestoredLibraryRenamed, false, "restored-renamed" },
            Boundary{ LibraryRestoreCrashPoint::SettingsCommitIntentPersisted, true, "commit-intent" },
            Boundary{ LibraryRestoreCrashPoint::SettingsReplaced, true, "settings-replaced" },
            Boundary{ LibraryRestoreCrashPoint::CommitRecorded, true, "commit-recorded" }
        };
        for (auto const& boundary : boundaries) {
            auto fixture = library_backup_detail::create_fixture(
                testRoot, std::string("restore-crash-") + boundary.name, require);
            library_backup_detail::mutate_live_state(fixture);
            auto identity = motion::capture_media_library_trust(fixture.library);
            require(identity.has_value(),
                "crash-boundary active library identity was not captured");
            bool interrupted{};
            try {
                (void)motion::app::LibraryBackupService::Restore(
                    fixture.dataRoot, *identity, fixture.backup.backup.path,
                    {}, nullptr, boundary.point);
            } catch (...) {
                interrupted = true;
            }
            require(interrupted &&
                motion::app::LibraryBackupService::HasPendingRestore(
                    fixture.dataRoot),
                "crash injection did not leave a durable restore journal");
            auto recovered =
                motion::app::LibraryBackupService::RecoverPendingRestore(
                    fixture.dataRoot);
            require(recovered.outcome == (boundary.completes
                    ? LibraryRestoreRecoveryOutcome::Completed
                    : LibraryRestoreRecoveryOutcome::RolledBack) &&
                !motion::app::LibraryBackupService::HasPendingRestore(
                    fixture.dataRoot),
                "startup recovery chose the wrong durable outcome");

            auto bytes = library_backup_detail::read_bytes(
                fixture.mediaDirectory / L"wallpaper.mp4");
            auto settings = motion::load_settings(
                fixture.dataRoot / L"Config" / L"settings.json");
            require(settings.has_value(),
                "startup recovery left settings unreadable");
            if (boundary.completes) {
                require(bytes == fixture.backedUpBytes &&
                    settings->selectedGroupId == fixture.groupId &&
                    settings->selectedMediaId == fixture.mediaId,
                    "roll-forward did not activate the verified snapshot");
            } else {
                require(bytes == "live state after backup" &&
                    settings->selectedGroupId.empty() &&
                    settings->selectedMediaId.empty(),
                    "pre-commit crash did not restore the original library and settings");
            }
            auto secondPass =
                motion::app::LibraryBackupService::RecoverPendingRestore(
                    fixture.dataRoot);
            require(secondPass.outcome == LibraryRestoreRecoveryOutcome::None,
                "restore recovery was not idempotent after journal removal");
        }
    }

    inline void library_restore_recovers_offline_library_and_corrupt_settings(
        std::filesystem::path const& testRoot,
        void (*require)(bool, char const*))
    {
        namespace fs = std::filesystem;
        auto fixture = library_backup_detail::create_fixture(
            testRoot, "offline-restore-source", require);

        auto restoreOnce = [&](std::string const& label, bool corruptSettings) {
            auto dataRoot = testRoot / motion::utf8_to_wide(label);
            auto settingsPath = dataRoot / L"Config" / L"settings.json";
            fs::create_directories(settingsPath.parent_path());
            auto offlinePath = testRoot / motion::utf8_to_wide(label + "-offline") /
                L"Wallpapers";
            if (corruptSettings) {
                std::ofstream(settingsPath, std::ios::binary) << "{corrupt-settings";
            } else {
                motion::Settings offline;
                offline.mediaLibraryPath = offlinePath.wstring();
                offline.mediaLibraryId = "abababab-abab-abab-abab-abababababab";
                motion::save_settings(settingsPath, offline);
                motion::Settings loaded;
                require(motion::load_settings_file(settingsPath, loaded) ==
                    motion::SettingsFileStatus::libraryUnavailable,
                    "offline restore fixture did not preserve its unavailable path");
            }

            auto restored = motion::app::LibraryBackupService::Restore(
                dataRoot, std::nullopt, fixture.backup.backup.path);
            require(restored.previousLibraryPath.empty() &&
                !fs::exists(offlinePath) &&
                motion::filesystem_path_is_nested(dataRoot,
                    restored.restoredLibraryIdentity.root),
                "offline restore touched the unavailable path or escaped local data");
            auto recaptured = motion::capture_media_library_trust(
                restored.restoredLibraryIdentity.root);
            require(recaptured.has_value(),
                "offline restore local library could not be recaptured");
            require(recaptured->ownershipId ==
                    restored.restoredLibraryIdentity.ownershipId,
                "offline restore local library ownership changed after commit");
            require(motion::is_owned_media_library(recaptured->root),
                "offline restore local library failed the owned-library shape check");
            require(motion::revalidate_media_library_trust(*recaptured),
                "offline restore local library trust could not be revalidated");
            motion::Settings diagnosticSettings;
            auto settingsStatus = motion::load_settings_file(
                settingsPath, diagnosticSettings);
            require(settingsStatus != motion::SettingsFileStatus::libraryUnavailable,
                "offline restore published settings whose new local library is unavailable");
            require(settingsStatus == motion::SettingsFileStatus::valid,
                "offline restore published an invalid settings document");
            auto activeSettings = motion::load_settings(settingsPath);
            require(activeSettings.has_value(),
                "offline restore left the atomically replaced settings unreadable");
            require(activeSettings->mediaLibraryPath ==
                    restored.restoredLibraryIdentity.root.wstring(),
                "offline restore settings did not reference the new local library path");
            require(activeSettings->mediaLibraryId ==
                    restored.restoredLibraryIdentity.ownershipId,
                "offline restore settings did not bind the new local library identity");
            require(activeSettings->selectedGroupId == fixture.groupId &&
                    activeSettings->selectedMediaId == fixture.mediaId,
                "offline restore settings did not preserve the backed-up selection");
            require(fs::is_regular_file(restored.previousSettingsPath),
                "offline restore did not preserve the previous settings document");
            if (corruptSettings) {
                require(library_backup_detail::read_bytes(
                        restored.previousSettingsPath) == "{corrupt-settings",
                    "corrupt settings were not preserved before explicit replacement");
            }
        };

        restoreOnce("offline-valid-settings", false);
        restoreOnce("offline-corrupt-settings", true);
    }
}
