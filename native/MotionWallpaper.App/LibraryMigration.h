#pragma once

#include "../MotionWallpaper.Common/Common.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace motion::app
{
    struct LibraryAccessState;

    class LibraryWriteLease final
    {
    public:
        ~LibraryWriteLease();
        LibraryWriteLease(LibraryWriteLease const&) = delete;
        LibraryWriteLease& operator=(LibraryWriteLease const&) = delete;
        void RetainMediaLibraryTrust(
            motion::MediaLibraryTrustIdentity identity,
            std::shared_ptr<motion::MediaLibraryTrustLease> trust) noexcept;
        [[nodiscard]] bool RevalidateMediaLibraryTrust() const noexcept;

    private:
        friend class LibraryAccessGate;
        explicit LibraryWriteLease(std::shared_ptr<LibraryAccessState> state);
        std::shared_ptr<LibraryAccessState> state_;
        std::optional<motion::MediaLibraryTrustIdentity> trustIdentity_;
        std::shared_ptr<motion::MediaLibraryTrustLease> trust_;
    };

    class LibraryMigrationLease final
    {
    public:
        ~LibraryMigrationLease();
        LibraryMigrationLease(LibraryMigrationLease const&) = delete;
        LibraryMigrationLease& operator=(LibraryMigrationLease const&) = delete;

    private:
        friend class LibraryAccessGate;
        explicit LibraryMigrationLease(std::shared_ptr<LibraryAccessState> state);
        std::shared_ptr<LibraryAccessState> state_;
    };

    // Coordinates media-library writes performed by the App. A migration can
    // start only when every asynchronous writer has released its lease, and no
    // new writer can enter until the migration lease is destroyed.
    class LibraryAccessGate final
    {
    public:
        LibraryAccessGate();
        std::shared_ptr<LibraryWriteLease> TryAcquireWrite();
        std::shared_ptr<LibraryMigrationLease> TryBeginMigration();
        bool MigrationInProgress() const;

    private:
        std::shared_ptr<LibraryAccessState> state_;
    };

    // Named-event handshake with the Agent. RequestAndWait requires the Agent
    // to close renderers/transcoders and all old-library handles. ResumeAndWait
    // completes only after the Agent has loaded the newly saved library path.
    class AgentLibraryMigrationPause final
    {
    public:
        AgentLibraryMigrationPause();
        ~AgentLibraryMigrationPause();
        AgentLibraryMigrationPause(AgentLibraryMigrationPause const&) = delete;
        AgentLibraryMigrationPause& operator=(AgentLibraryMigrationPause const&) = delete;

        bool RequestAndWait(std::chrono::milliseconds timeout) noexcept;
        bool ResumeAndWait(std::chrono::milliseconds timeout) noexcept;
        void Cancel() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    using LibraryMigrationProgress = std::function<void(uint64_t copiedBytes, uint64_t totalBytes)>;

    class LibraryMigrationTransaction final
    {
    public:
        static std::shared_ptr<LibraryMigrationTransaction> Begin(
            std::filesystem::path source,
            std::filesystem::path target,
            motion::MediaLibraryTrustIdentity const& expectedSource);
        ~LibraryMigrationTransaction();
        LibraryMigrationTransaction(LibraryMigrationTransaction const&) = delete;
        LibraryMigrationTransaction& operator=(LibraryMigrationTransaction const&) = delete;

        void CopyAndVerify(
            LibraryMigrationProgress const& progress = {},
            std::atomic_bool const* cancelled = nullptr);
        void CommitPreparedTarget();
        void MarkActivated();
        std::filesystem::path ArchiveVerifiedSource();

        std::filesystem::path const& Source() const noexcept;
        std::filesystem::path const& Target() const noexcept;
        std::string const& TransactionId() const noexcept;
        void RollbackOwnedTarget() noexcept;

    private:
        struct Impl;
        explicit LibraryMigrationTransaction(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };
}
