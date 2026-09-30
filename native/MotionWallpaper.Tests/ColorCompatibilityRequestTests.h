#pragma once

#include "../MotionWallpaper.Common/VariantCache.h"

#include <array>
#include <barrier>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace motion::tests
{
    inline void color_compatibility_requests_are_atomic(
        std::filesystem::path const& root, void (*require)(bool, char const*))
    {
        namespace fs = std::filesystem;
        auto suite = root / L"color-compatibility-requests" / new_variant_request_id();
        auto makeCase = [&](wchar_t const* name) {
            auto directory = suite / name;
            fs::create_directories(directory);
            return directory;
        };

        auto idle = makeCase(L"idle-roundtrip");
        require(request_variant_color_compatibility(idle, true),
            "idle compatible-color request was not published");
        auto first = read_variant_generation_request(idle);
        auto progress = read_variant_progress(idle);
        require(first.mode == "balanced" && valid_variant_request_id(first.requestId) &&
            variant_compatibility_color_enabled(idle) && progress &&
            progress->requestId == first.requestId && progress->state == VariantProgressState::queued,
            "compatible-color request lost its persistent preference, GUID, or queued progress");
        require(complete_variant_generation(idle, first) && !read_variant_generation_request(idle) &&
            variant_compatibility_color_enabled(idle),
            "completion erased the persistent compatible preference or left a runnable request");
        require(request_variant_color_compatibility(idle, false),
            "completed compatible job could not switch back to automatic color");
        auto automatic = read_variant_generation_request(idle);
        require(automatic.mode == "balanced" && valid_variant_request_id(automatic.requestId) &&
            automatic.requestId != first.requestId && !variant_compatibility_color_enabled(idle),
            "switching back to automatic reused a completed GUID or retained compatibility");

        for (auto state : {VariantProgressState::queued, VariantProgressState::paused,
                VariantProgressState::generating}) {
            auto busy = makeCase(state == VariantProgressState::queued ? L"other-mode-queued" :
                state == VariantProgressState::paused ? L"paused" : L"generating");
            require(set_variant_compatibility_color(busy, true), "could not seed busy color preference");
            require(request_variant_generation(busy,
                state == VariantProgressState::queued ? "power-saver" : "balanced"),
                "could not seed existing optimization request");
            auto existing = read_variant_generation_request(busy);
            if (state == VariantProgressState::paused) {
                require(pause_variant_generation(busy), "could not seed paused optimization request");
            } else if (state == VariantProgressState::generating) {
                require(write_variant_progress_if_current(busy, existing, state, 37, true),
                    "could not seed generating optimization progress");
            }
            fs::create_directories(busy / L"Variants");
            auto partial = busy / L"Variants" / L"existing.part.mp4";
            require(write_small_file(partial, "owned by the existing worker"), "could not seed partial video");
            auto beforeProgress = read_small_file(variant_progress_path(busy));
            require(!request_variant_color_compatibility(busy, false) &&
                read_variant_generation_request(busy) == existing && variant_compatibility_color_enabled(busy) &&
                read_small_file(variant_progress_path(busy)) == beforeProgress &&
                read_small_file(partial) == "owned by the existing worker",
                "color preference change replaced a queued/paused/generating job or its partial/progress");
        }

        auto orphan = makeCase(L"orphan-partial");
        fs::create_directories(orphan / L"Variants");
        auto orphanPartial = orphan / L"Variants" / L"orphan.part.mp4";
        require(write_small_file(orphanPartial, "still owned by a worker") &&
            !request_variant_color_compatibility(orphan, true) && !read_variant_generation_request(orphan) &&
            !variant_compatibility_color_enabled(orphan) && read_small_file(orphanPartial) == "still owned by a worker",
            "orphaned in-progress video was overwritten by a new compatible-color request");

        // A failed atomic marker replacement must not expose the reserved
        // GUID to the Agent. A directory supplies a deterministic I/O failure.
        auto blockedMarker = makeCase(L"marker-is-directory");
        fs::create_directories(variant_compatibility_color_path(blockedMarker));
        require(!request_variant_color_compatibility(blockedMarker, true) &&
            !read_variant_generation_request(blockedMarker) && !fs::exists(variant_request_path(blockedMarker)) &&
            fs::is_directory(variant_compatibility_color_path(blockedMarker)),
            "failed color-marker publication left a runnable request or changed the obstruction");

        auto lockedMarker = makeCase(L"existing-marker-locked");
        require(set_variant_compatibility_color(lockedMarker, true), "could not seed locked preference");
        {
            unique_handle marker(CreateFileW(variant_compatibility_color_path(lockedMarker).c_str(),
                GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            require(static_cast<bool>(marker), "could not lock existing preference against replacement/deletion");
            for (bool enabled : {false, true}) {
                require(!request_variant_color_compatibility(lockedMarker, enabled) &&
                    !read_variant_generation_request(lockedMarker) && !fs::exists(variant_request_path(lockedMarker)) &&
                    read_small_file(variant_compatibility_color_path(lockedMarker)) == "compatible-v1",
                    "failed marker update exposed a request or altered the previous color preference");
            }
        }

        // The same Win32 exclusive reservation used by publication must hide
        // an otherwise valid GUID until its color marker has been committed.
        // Windows enforces this sharing contract for other processes too.
        auto reserved = makeCase(L"exclusive-reservation");
        VariantGenerationRequest reservedRequest{"balanced", new_variant_request_id()};
        auto serialized = serialize_variant_request(reservedRequest);
        unique_handle reservation(CreateFileW(variant_request_path(reserved).c_str(),
            GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr));
        require(static_cast<bool>(reservation), "could not reserve test request");
        DWORD written{};
        require(WriteFile(reservation.get(), serialized.data(), static_cast<DWORD>(serialized.size()), &written, nullptr) &&
            written == serialized.size() && FlushFileBuffers(reservation.get()), "could not write reserved GUID");
        require(!read_variant_generation_request(reserved), "Agent can read a GUID before its color preference is ready");
        {
            unique_handle reader(CreateFileW(variant_request_path(reserved).c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            unique_handle writer(CreateFileW(variant_request_path(reserved).c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            require(!reader && !writer, "exclusive request reservation permitted a competing read or overwrite");
        }
        constexpr size_t contenders = 8;
        std::array<VariantGenerationRequest, contenders> autoRequests;
        std::barrier reservationGate(static_cast<std::ptrdiff_t>(contenders + 1));
        std::vector<std::jthread> readers;
        for (size_t index = 0; index < contenders; ++index) {
            readers.emplace_back([&, index] {
                reservationGate.arrive_and_wait();
                autoRequests[index] = ensure_variant_generation_request(reserved, index % 2 ? "balanced" : "power-saver");
            });
        }
        reservationGate.arrive_and_wait();
        readers.clear();
        for (auto const& request : autoRequests) {
            require(!request, "automatic enqueue adopted or replaced an unpublished color-preference request");
        }
        require(set_variant_compatibility_color(reserved, true), "could not publish reserved preference");
        reservation.reset();
        require(read_variant_generation_request(reserved) == reservedRequest &&
            ensure_variant_generation_request(reserved, "balanced") == reservedRequest &&
            variant_compatibility_color_enabled(reserved),
            "releasing the reservation did not publish the original GUID with its matching preference");

        // Race opposite user choices. Exactly one request may own each new
        // directory, and the durable color preference must match that winner.
        for (size_t round = 0; round < 8; ++round) {
            auto raced = suite / (L"race-" + std::to_wstring(round));
            fs::create_directories(raced);
            std::array<bool, contenders> won{};
            std::barrier startGate(static_cast<std::ptrdiff_t>(contenders + 1));
            std::vector<std::jthread> workers;
            for (size_t index = 0; index < contenders; ++index) {
                workers.emplace_back([&, index] {
                    startGate.arrive_and_wait();
                    won[index] = request_variant_color_compatibility(raced, index % 2 == 0);
                });
            }
            startGate.arrive_and_wait();
            workers.clear();
            size_t winnerCount{}, winner{};
            for (size_t index = 0; index < contenders; ++index) {
                if (won[index]) { ++winnerCount; winner = index; }
            }
            auto published = read_variant_generation_request(raced);
            auto publishedProgress = read_variant_progress(raced);
            require(winnerCount == 1 && published.mode == "balanced" && valid_variant_request_id(published.requestId) &&
                variant_compatibility_color_enabled(raced) == (winner % 2 == 0) && publishedProgress &&
                publishedProgress->requestId == published.requestId && publishedProgress->state == VariantProgressState::queued,
                "concurrent compatible/automatic requests did not publish exactly one internally consistent winner");
        }
    }
}
