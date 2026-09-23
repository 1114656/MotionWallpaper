#pragma once

#include "../MotionWallpaper.Common/UniqueHandle.h"
#include "../MotionWallpaper.Protocol/BoundedPipeReader.h"
#include "../MotionWallpaper.Protocol/RendererProtocol.h"

#include <chrono>
#include <thread>
#include <vector>

namespace motion::tests
{
    template<typename Require>
    void renderer_pipe_stop_preserves_buffered_error(Require require)
    {
        HANDLE readRaw{}, writeRaw{};
        require(CreatePipe(&readRaw, &writeRaw, nullptr, 4096) != FALSE,
            "could not create renderer diagnostic fixture pipe");
        unique_handle reader(readRaw), writer(writeRaw);
        std::string output;
        for (unsigned index = 0; index < 30; ++index) output += "status decode automatic first-frame-presented\r\n";
        output += "error 19 present 0x887a0005\r\n";
        DWORD written{};
        require(WriteFile(writer.get(), output.data(), static_cast<DWORD>(output.size()), &written, nullptr) &&
            written == output.size(), "could not fill renderer diagnostic fixture");
        writer.reset();
        // Process exit and stop precede the first scheduling of the reader.
        // The final error crosses several 256-byte reads and must survive.
        std::atomic_bool stopping{ true };
        std::optional<protocol::RendererError> error;
        size_t count{};
        protocol::read_bounded_pipe_lines(reader.get(), stopping, [&](std::string_view line) {
            ++count;
            if (auto parsed = protocol::parse_renderer_error(line)) error = std::move(parsed);
        });
        require(count == 31 && error && error->code == 0x887a0005u && error->operation == "present",
            "stopping reader discarded the final buffered Renderer HRESULT");
    }

    template<typename Require>
    void renderer_pipe_stop_does_not_wait_for_inherited_writer(Require require)
    {
        HANDLE readRaw{}, writeRaw{};
        require(CreatePipe(&readRaw, &writeRaw, nullptr, 4096) != FALSE,
            "could not create retained-writer fixture pipe");
        unique_handle reader(readRaw), writer(writeRaw);
        std::atomic_bool stopping{}, finished{};
        std::thread worker([&] {
            protocol::read_bounded_pipe_lines(reader.get(), stopping, [](std::string_view) {});
            finished.store(true, std::memory_order_release);
        });
        Sleep(20);
        stopping.store(true, std::memory_order_release);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (!finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) Sleep(5);
        bool stoppedWithoutEof = finished.load(std::memory_order_acquire);
        // Closing the writer also guarantees cleanup if a future regression
        // reintroduces ReadFile-before-Peek and the assertion needs to fail.
        writer.reset();
        worker.join();
        require(stoppedWithoutEof, "renderer stop waited for a retained writer handle to close");
    }

    template<typename Require>
    void renderer_pipe_reader_discards_oversize_lines(Require require)
    {
        HANDLE readRaw{}, writeRaw{};
        require(CreatePipe(&readRaw, &writeRaw, nullptr, 4096) != FALSE,
            "could not create oversized-line fixture pipe");
        unique_handle reader(readRaw), writer(writeRaw);
        std::string output(2048, 'x');
        output += "\nerror 0 media 0xc00d36b4\nerror 2 truncated";
        DWORD written{};
        require(WriteFile(writer.get(), output.data(), static_cast<DWORD>(output.size()), &written, nullptr) &&
            written == output.size(), "could not fill oversized-line fixture");
        writer.reset();
        std::atomic_bool stopping{ true };
        std::vector<std::string> lines;
        protocol::read_bounded_pipe_lines(reader.get(), stopping,
            [&](std::string_view line) { lines.emplace_back(line); });
        require(lines.size() == 1 && lines.front() == "error 0 media 0xc00d36b4",
            "oversized or unterminated protocol input was accepted, or damaged the following valid error");
    }
}
