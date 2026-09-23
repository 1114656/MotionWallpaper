#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>

namespace motion::protocol
{
    inline constexpr size_t renderer_protocol_line_limit = 256;
    inline constexpr DWORD renderer_protocol_stop_drain_limit = 64 * 1024;

    // Exactly one reader owns this pipe. ReadFile is only called for bytes
    // confirmed by PeekNamedPipe, so an inherited writer that remains alive
    // cannot trap shutdown in a blocking read. Once stopping is observed, take
    // a bounded snapshot of the available bytes rather than follow a writer.
    template<typename OnLine>
    void read_bounded_pipe_lines(HANDLE pipe, std::atomic_bool const& stopping,
        OnLine const& onLine)
    {
        std::string pending;
        pending.reserve(renderer_protocol_line_limit);
        bool discardLine{};
        bool draining{};
        DWORD remaining{};
        char buffer[256];
        for (;;) {
            DWORD available{};
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return;
            if (!draining && stopping.load(std::memory_order_acquire)) {
                draining = true;
                remaining = (std::min)(available, renderer_protocol_stop_drain_limit);
            }
            if (draining && (!remaining || !available)) return;
            if (!available) {
                // There is no pending I/O to cancel or race with CloseHandle.
                // A stop request is noticed within one inexpensive poll.
                Sleep(50);
                continue;
            }
            DWORD request = (std::min)(available, static_cast<DWORD>(sizeof(buffer)));
            if (draining) request = (std::min)(request, remaining);
            DWORD bytes{};
            if (!ReadFile(pipe, buffer, request, &bytes, nullptr) || !bytes) return;
            if (draining) remaining -= bytes;
            for (DWORD index = 0; index < bytes; ++index) {
                char value = buffer[index];
                if (value == '\n') {
                    if (!discardLine) {
                        if (!pending.empty() && pending.back() == '\r') pending.pop_back();
                        onLine(std::string_view(pending));
                    }
                    pending.clear();
                    discardLine = false;
                } else if (!discardLine) {
                    if (pending.size() == renderer_protocol_line_limit) {
                        pending.clear();
                        discardLine = true;
                    } else {
                        pending.push_back(value);
                    }
                }
            }
        }
    }
}
