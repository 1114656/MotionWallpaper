#pragma once

#include "../MotionWallpaper.Common/Common.h"
#include "../MotionWallpaper.Common/UniqueHandle.h"

#include <windows.h>

namespace motion::agent
{
    // Agent side of the media-library handoff. Manual-reset acknowledgements
    // make the protocol tolerant of scheduling gaps between the WinUI process
    // and the background agent. Only the App owns the request bit; only the
    // Agent owns the two acknowledgements.
    class LibraryMigrationProtocol final
    {
    public:
        LibraryMigrationProtocol() noexcept
            : requested_(CreateEventW(nullptr, TRUE, FALSE,
                motion::library_migration_request_event_name)),
              quiesced_(CreateEventW(nullptr, TRUE, FALSE,
                motion::library_migration_quiesced_event_name)),
              applied_(CreateEventW(nullptr, TRUE, FALSE,
                motion::library_migration_applied_event_name))
        {
        }

        ~LibraryMigrationProtocol()
        {
            // Never leave a live App observing an acknowledgement from an
            // Agent that has already exited or faulted.
            ClearAcknowledgements();
        }

        LibraryMigrationProtocol(LibraryMigrationProtocol const&) = delete;
        LibraryMigrationProtocol& operator=(LibraryMigrationProtocol const&) = delete;

        explicit operator bool() const noexcept
        {
            return requested_ && quiesced_ && applied_ && owner_;
        }

        [[nodiscard]] bool Requested() noexcept
        {
            if (!requested_ || WaitForSingleObject(requested_.get(), 0) != WAIT_OBJECT_0) return false;
            // Clear only while holding the cross-process owner lock and only
            // after the recorded PID + creation time proves the requester is
            // gone. A newly claimed request therefore cannot be mistaken for
            // an orphan from a terminated App.
            return !owner_.ClearOrphanedRequest(
                requested_.get(), quiesced_.get(), applied_.get());
        }

        [[nodiscard]] bool AcknowledgeQuiesced() noexcept
        {
            return quiesced_ && SetEvent(quiesced_.get()) != FALSE;
        }

        void ClearQuiesced() noexcept
        {
            if (quiesced_) ResetEvent(quiesced_.get());
        }

        [[nodiscard]] bool AcknowledgeApplied() noexcept
        {
            return applied_ && SetEvent(applied_.get()) != FALSE;
        }

        void ClearAcknowledgements() noexcept
        {
            if (quiesced_) ResetEvent(quiesced_.get());
            if (applied_) ResetEvent(applied_.get());
        }

    private:
        motion::unique_handle requested_;
        motion::unique_handle quiesced_;
        motion::unique_handle applied_;
        motion::LibraryMigrationOwnerChannel owner_;
    };
}
