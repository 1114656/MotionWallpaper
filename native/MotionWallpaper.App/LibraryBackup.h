#pragma once

#include "../MotionWallpaper.Common/Common.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace motion::app
{
    inline constexpr int library_backup_format_version = 1;

    enum class LibraryBackupPhase
    {
        Inspecting,
        CopyingSettings,
        CopyingLibrary,
        Verifying,
        PreparingRestore,
        SwappingLibrary,
        CommittingSettings,
        Completed
    };

    struct LibraryBackupProgress
    {
        LibraryBackupPhase phase{ LibraryBackupPhase::Inspecting };
        uint64_t completedBytes{};
        uint64_t totalBytes{};
        uint32_t completedFiles{};
        uint32_t totalFiles{};
        std::filesystem::path currentPath;
    };

    using LibraryBackupProgressCallback =
        std::function<void(LibraryBackupProgress const& progress)>;

    struct LibraryBackupInfo
    {
        int version{ library_backup_format_version };
        std::string backupId;
        std::wstring createdAt;
        std::string libraryId;
        uint64_t totalBytes{};
        uint32_t fileCount{};
        std::filesystem::path path;
    };

    struct LibraryBackupResult
    {
        LibraryBackupInfo backup;
    };

    struct LibraryRestoreResult
    {
        LibraryBackupInfo backup;
        motion::Settings restoredSettings;
        motion::MediaLibraryTrustIdentity restoredLibraryIdentity;
        // The previous library and settings are deliberately retained after a
        // successful restore. The UI may offer their paths for manual cleanup
        // after the restored library has been used successfully.
        std::filesystem::path previousLibraryPath;
        std::filesystem::path previousSettingsPath;
    };

    enum class LibraryRestoreRecoveryOutcome
    {
        None,
        RolledBack,
        Completed
    };

    struct LibraryRestoreRecoveryResult
    {
        LibraryRestoreRecoveryOutcome outcome{ LibraryRestoreRecoveryOutcome::None };
        std::filesystem::path activeLibraryPath;
    };

    // Deterministic abrupt-process boundaries used by the native recovery
    // tests. Production callers always use None. Unlike ordinary exceptions,
    // an injected boundary deliberately leaves the durable journal and owned
    // staging objects in place so RecoverPendingRestore can exercise the same
    // path used after a real process crash or power loss.
    enum class LibraryRestoreCrashPoint
    {
        None,
        JournalPrepared,
        PreviousLibraryRenamed,
        RestoredLibraryRenamed,
        SettingsCommitIntentPersisted,
        SettingsReplaced,
        CommitRecorded
    };

    // Synchronous, exception-reporting backend intended to be called from a
    // WinUI resume_background continuation. The caller must hold the App's
    // LibraryMigrationLease. Restore must quiesce Agent/Renderer when replacing
    // a trusted current library; the null-identity recovery path publishes a
    // new app-local library and never accesses the unavailable old path. Create
    // only needs a stable source identity, but quiescing gives a point-in-time
    // backup when imports or optimization jobs can otherwise change the tree.
    class LibraryBackupService final
    {
    public:
        static LibraryBackupResult Create(
            std::filesystem::path dataRoot,
            std::filesystem::path libraryPath,
            motion::MediaLibraryTrustIdentity const& expectedLibrary,
            std::filesystem::path destinationDirectory,
            LibraryBackupProgressCallback const& progress = {},
            std::atomic_bool const* cancelled = nullptr);

        static LibraryBackupInfo Validate(
            std::filesystem::path backupPath,
            LibraryBackupProgressCallback const& progress = {},
            std::atomic_bool const* cancelled = nullptr);

        static bool HasPendingRestore(
            std::filesystem::path dataRoot) noexcept;

        static LibraryRestoreRecoveryResult RecoverPendingRestore(
            std::filesystem::path dataRoot);

        static LibraryRestoreResult Restore(
            std::filesystem::path dataRoot,
            std::optional<motion::MediaLibraryTrustIdentity> expectedCurrentLibrary,
            std::filesystem::path backupPath,
            LibraryBackupProgressCallback const& progress = {},
            std::atomic_bool const* cancelled = nullptr,
            LibraryRestoreCrashPoint crashPoint = LibraryRestoreCrashPoint::None);
    };
}
