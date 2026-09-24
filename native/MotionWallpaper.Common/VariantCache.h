#pragma once

#include "UniqueHandle.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace motion
{
    [[nodiscard]] inline int variant_policy_version(std::wstring_view name) noexcept
    {
        if (name.ends_with(L"-v7.mp4")) return 7;
        if (name.ends_with(L"-v6.mp4")) return 6;
        if (name.ends_with(L"-v5.mp4")) return 5;
        return 0;
    }
    enum class VariantProgressState
    {
        none,
        queued,
        waitingForPower,
        generating,
        paused
    };

    struct VariantGenerationProgress
    {
        std::string mode;
        std::string requestId;
        VariantProgressState state{ VariantProgressState::none };
        uint32_t percent{};
        bool determinate{};
        uint64_t estimatedRemainingSeconds{};
        bool estimatedRemainingKnown{};
    };

    struct VariantGenerationRequest
    {
        std::string mode;
        // Empty identifies a legacy, pre-token request marker. New requests
        // always carry a GUID so a worker can distinguish a same-mode retry.
        std::string requestId;

        [[nodiscard]] explicit operator bool() const noexcept { return !mode.empty(); }
        bool operator==(VariantGenerationRequest const&) const = default;
    };

    struct VariantCacheFile
    {
        std::wstring fileName;
        std::string mode;
        uint64_t bytes{};
        bool sharedStorage{};
    };

    struct VariantCacheStatus
    {
        std::string requestedMode;
        bool queued{};
        bool generating{};
        bool paused{};
        bool cancelled{};
        bool failed{};
        std::string failedMode;
        std::string failedReason;
        bool waitingForPower{};
        uint32_t progressPercent{};
        bool progressKnown{};
        uint64_t estimatedRemainingSeconds{};
        bool estimatedRemainingKnown{};
        bool balancedSuppressed{};
        bool powerSaverSuppressed{};
        uint64_t bytes{};
        uint32_t files{};
        std::vector<VariantCacheFile> entries;
    };

    [[nodiscard]] inline std::wstring select_variant_file(
        VariantCacheStatus const& status, std::string const& performanceMode) noexcept
    {
        auto rank = [&](VariantCacheFile const& entry) {
            if (performanceMode == "power-saver") return entry.mode == "power-saver" ? 0 : 1;
            // When the source has been removed, original mode cannot be honored.
            // Prefer the higher-quality retained profile, then fall back to the
            // power-saving copy so the wallpaper remains playable.
            return entry.mode == "balanced" ? 0 : 1;
        };
        VariantCacheFile const* best{};
        for (auto const& entry : status.entries) {
            if (entry.fileName.empty() || !entry.bytes) continue;
            if (!best || rank(entry) < rank(*best) ||
                (rank(entry) == rank(*best) &&
                    variant_policy_version(entry.fileName) > variant_policy_version(best->fileName))) {
                best = &entry;
            }
        }
        return best ? best->fileName : std::wstring{};
    }

    // CPU-smooth copies are an internal compatibility cache rather than a
    // user-selected quality tier. Keep them out of the variants UI, but allow
    // a source-less wallpaper to remain playable on a WARP-only system.
    [[nodiscard]] inline std::wstring select_cpu_smooth_variant_file(
        std::filesystem::path const& mediaDirectory) noexcept
    {
        try {
            std::wstring best;
            std::filesystem::file_time_type bestTime{};
            std::error_code error;
            for (std::filesystem::directory_iterator entries(mediaDirectory / L"Variants", error), end;
                !error && entries != end; entries.increment(error)) {
                std::error_code itemError;
                if (!entries->is_regular_file(itemError) || itemError || !entries->file_size(itemError) || itemError) continue;
                auto name = entries->path().filename().wstring();
                if (!name.starts_with(L"cpu-smooth-") || !variant_policy_version(name)) continue;
                auto modified = entries->last_write_time(itemError);
                if (itemError) continue;
                if (best.empty() || modified > bestTime) {
                    best = std::move(name);
                    bestTime = modified;
                }
            }
            return best;
        } catch (...) {
            return {};
        }
    }

    inline std::filesystem::path variant_request_path(std::filesystem::path const& mediaDirectory)
    {
        return mediaDirectory / L".optimization-request";
    }

    inline std::filesystem::path variant_cancelled_path(std::filesystem::path const& mediaDirectory)
    {
        return mediaDirectory / L".optimization-cancelled";
    }

    inline std::filesystem::path variant_paused_path(std::filesystem::path const& mediaDirectory)
    {
        return mediaDirectory / L".optimization-paused";
    }

    inline std::filesystem::path variant_failed_path(std::filesystem::path const& mediaDirectory)
    {
        return mediaDirectory / L".optimization-failed";
    }

    inline std::filesystem::path variant_progress_path(std::filesystem::path const& mediaDirectory)
    {
        return mediaDirectory / L".optimization-progress";
    }

    inline std::filesystem::path variant_suppressed_path(std::filesystem::path const& mediaDirectory,
        std::string const& mode)
    {
        return mediaDirectory / (mode == "power-saver"
            ? L".optimization-suppressed-power-saver"
            : L".optimization-suppressed-balanced");
    }

    inline bool variant_generation_suppressed(std::filesystem::path const& mediaDirectory,
        std::string const& mode) noexcept
    {
        if (mode != "balanced" && mode != "power-saver") return false;
        std::error_code error;
        return std::filesystem::is_regular_file(variant_suppressed_path(mediaDirectory, mode), error) && !error;
    }

    using VariantRemovalCallback = std::function<bool(std::filesystem::path const&)>;

    inline bool retain_variant_profile(std::filesystem::path const& mediaDirectory,
        std::string const& mode, std::wstring const& keepFileName,
        VariantRemovalCallback const& removeCandidate = {}) noexcept
    {
        if ((mode != "balanced" && mode != "power-saver" && mode != "cpu-smooth") || keepFileName.empty()) return false;
        try {
            auto variants = mediaDirectory / L"Variants";
            auto prefix = mode == "balanced" ? L"balanced-" :
                mode == "power-saver" ? L"power-saver-" : L"cpu-smooth-";
            if (!keepFileName.starts_with(prefix) || !keepFileName.ends_with(L".mp4") ||
                keepFileName.ends_with(L".part.mp4")) return false;
            std::error_code error;
            if (!std::filesystem::exists(variants, error)) return !error;
            if (!std::filesystem::is_regular_file(variants / keepFileName, error) || error) return false;
            bool retainedOnlyCurrent = true;
            for (std::filesystem::directory_iterator entries(variants, error), end;
                !error && entries != end; entries.increment(error)) {
                std::error_code itemError;
                auto regular = entries->is_regular_file(itemError);
                if (itemError) {
                    retainedOnlyCurrent = false;
                    continue;
                }
                if (!regular) continue;
                auto name = entries->path().filename().wstring();
                if (!name.starts_with(prefix) || name == keepFileName ||
                    name.ends_with(L".part.mp4") || entries->path().extension() != L".mp4") continue;
                // Keep completed v6 specifications across monitor/profile
                // changes. Global quota/LRU removes unleased files instead of
                // throwing away a usable copy each time the topology changes.
                if (variant_policy_version(name) >= 6) continue;
                bool removed = removeCandidate
                    ? removeCandidate(entries->path())
                    : std::filesystem::remove(entries->path(), itemError);
                if (!removed || itemError) {
                    retainedOnlyCurrent = false;
                }
            }
            return !error && retainedOnlyCurrent;
        } catch (...) {
            return false;
        }
    }

    inline bool write_small_file(std::filesystem::path const& destination, std::string const& value) noexcept
    {
        try {
            auto temporary = destination;
            // UI and agent may update a task at the same time. A per-process,
            // per-thread temporary name prevents one writer from deleting or
            // replacing another writer's staging file before the atomic move.
            temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetCurrentThreadId());
            unique_handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, nullptr));
            if (!file || value.size() > MAXDWORD) return false;
            DWORD written{};
            if (!WriteFile(file.get(), value.data(), static_cast<DWORD>(value.size()), &written, nullptr) ||
                written != static_cast<DWORD>(value.size()) || !FlushFileBuffers(file.get())) {
                file.reset();
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return false;
            }
            file.reset();
            if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return false;
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    inline std::string read_small_file(std::filesystem::path const& source,
        DWORD maximumBytes = 256) noexcept
    {
        try {
            unique_handle file(CreateFileW(source.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!file) return {};
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 ||
                static_cast<uint64_t>(size.QuadPart) > maximumBytes) return {};
            std::string value(static_cast<size_t>(size.QuadPart), '\0');
            DWORD read{};
            if (!value.empty() && (!ReadFile(file.get(), value.data(), static_cast<DWORD>(value.size()),
                &read, nullptr) || read != value.size())) return {};
            return value;
        } catch (...) {
            return {};
        }
    }

    [[nodiscard]] inline bool valid_variant_request_mode(std::string_view mode) noexcept
    {
        return mode == "balanced" || mode == "power-saver";
    }

    [[nodiscard]] inline bool valid_variant_request_id(std::string_view requestId) noexcept
    {
        return requestId.size() == 36 && std::all_of(requestId.begin(), requestId.end(),
            [](unsigned char character) {
                return (character >= '0' && character <= '9') ||
                    (character >= 'a' && character <= 'f') || character == '-';
            });
    }

    [[nodiscard]] inline std::string new_variant_request_id() noexcept
    {
        GUID id{};
        wchar_t text[40]{};
        if (FAILED(CoCreateGuid(&id)) || StringFromGUID2(id, text, ARRAYSIZE(text)) != 39) return {};
        std::string result;
        result.reserve(36);
        for (size_t index = 1; index != 37; ++index) {
            wchar_t character = text[index];
            if (character >= L'A' && character <= L'F') character += L'a' - L'A';
            if (character > 0x7f) return {};
            result.push_back(static_cast<char>(character));
        }
        return valid_variant_request_id(result) ? result : std::string{};
    }

    [[nodiscard]] inline std::string serialize_variant_request(
        VariantGenerationRequest const& request) noexcept
    {
        if (!valid_variant_request_mode(request.mode)) return {};
        if (request.requestId.empty()) return request.mode;
        if (!valid_variant_request_id(request.requestId)) return {};
        return request.mode + "|" + request.requestId;
    }

    [[nodiscard]] inline VariantGenerationRequest parse_variant_request(
        std::string_view value) noexcept
    {
        auto separator = value.find('|');
        if (separator == std::string_view::npos) {
            return valid_variant_request_mode(value)
                ? VariantGenerationRequest{ std::string(value), {} }
                : VariantGenerationRequest{};
        }
        if (value.find('|', separator + 1) != std::string_view::npos) return {};
        auto mode = value.substr(0, separator);
        auto requestId = value.substr(separator + 1);
        if (!valid_variant_request_mode(mode) || !valid_variant_request_id(requestId)) return {};
        return { std::string(mode), std::string(requestId) };
    }

    [[nodiscard]] inline VariantGenerationRequest read_variant_generation_request(
        std::filesystem::path const& mediaDirectory) noexcept
    {
        return parse_variant_request(read_small_file(variant_request_path(mediaDirectory)));
    }

    inline std::string read_small_file_handle(HANDLE file, DWORD maximumBytes = 256) noexcept
    {
        if (!file || file == INVALID_HANDLE_VALUE) return {};
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
            static_cast<uint64_t>(size.QuadPart) > maximumBytes) return {};
        LARGE_INTEGER beginning{};
        if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN)) return {};
        std::string value(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read{};
        if (!value.empty() && (!ReadFile(file, value.data(), static_cast<DWORD>(value.size()),
            &read, nullptr) || read != value.size())) return {};
        return value;
    }

    [[nodiscard]] inline unique_handle lock_variant_request(
        std::filesystem::path const& mediaDirectory,
        VariantGenerationRequest const& expected) noexcept
    {
        auto serialized = serialize_variant_request(expected);
        if (serialized.empty()) return {};
        unique_handle file(CreateFileW(variant_request_path(mediaDirectory).c_str(),
            GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file || read_small_file_handle(file.get()) != serialized) return {};
        return file;
    }

    inline bool mark_locked_request_for_deletion(HANDLE file) noexcept
    {
        FILE_DISPOSITION_INFO disposition{ TRUE };
        return file && file != INVALID_HANDLE_VALUE &&
            SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition));
    }

    inline std::string_view variant_progress_state_name(VariantProgressState state) noexcept
    {
        switch (state) {
        case VariantProgressState::queued: return "queued";
        case VariantProgressState::waitingForPower: return "waiting-power";
        case VariantProgressState::generating: return "generating";
        case VariantProgressState::paused: return "paused";
        default: return "none";
        }
    }

    inline VariantProgressState parse_variant_progress_state(std::string_view value) noexcept
    {
        if (value == "queued") return VariantProgressState::queued;
        if (value == "waiting-power") return VariantProgressState::waitingForPower;
        if (value == "generating") return VariantProgressState::generating;
        if (value == "paused") return VariantProgressState::paused;
        return VariantProgressState::none;
    }

    [[nodiscard]] inline std::optional<VariantGenerationProgress> parse_variant_progress(
        std::string_view value) noexcept
    {
        // v1|<mode>|<state>|<0..100 or ?> (legacy request without token)
        // v2|<mode>|<request-id>|<state>|<0..100 or ?>
        // v3|<mode>|<request-id>|<state>|<0..100 or ?>|<eta-seconds or ?>
        auto view = value;
        std::vector<std::string_view> fields;
        while (fields.size() != 6) {
            auto separator = view.find('|');
            if (separator == std::string_view::npos) break;
            fields.push_back(view.substr(0, separator));
            view.remove_prefix(separator + 1);
        }
        fields.push_back(view);
        bool legacy = fields.size() == 4 && fields[0] == "v1";
        bool tokenizedV2 = fields.size() == 5 && fields[0] == "v2";
        bool tokenizedV3 = fields.size() == 6 && fields[0] == "v3";
        bool tokenized = tokenizedV2 || tokenizedV3;
        if ((!legacy && !tokenizedV2 && !tokenizedV3) ||
            (fields[1] != "balanced" && fields[1] != "power-saver" && fields[1] != "cpu-smooth")) {
            return std::nullopt;
        }
        size_t stateIndex = tokenized ? 3 : 2;
        size_t percentIndex = tokenized ? 4 : 3;
        if (tokenized && !valid_variant_request_id(fields[2])) return std::nullopt;
        auto state = parse_variant_progress_state(fields[stateIndex]);
        if (state == VariantProgressState::none) return std::nullopt;
        VariantGenerationProgress result;
        result.mode.assign(fields[1]);
        if (tokenized) result.requestId.assign(fields[2]);
        result.state = state;
        if (fields[percentIndex] != "?") {
            uint32_t percent{};
            auto parsed = std::from_chars(fields[percentIndex].data(),
                fields[percentIndex].data() + fields[percentIndex].size(), percent);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != fields[percentIndex].data() + fields[percentIndex].size() ||
                percent > 100) return std::nullopt;
            result.percent = percent;
            result.determinate = true;
        }
        if (tokenizedV3 && fields[5] != "?") {
            uint64_t seconds{};
            auto parsed = std::from_chars(fields[5].data(),
                fields[5].data() + fields[5].size(), seconds);
            constexpr uint64_t maximumEtaSeconds = 365ULL * 24 * 60 * 60;
            if (parsed.ec != std::errc{} ||
                parsed.ptr != fields[5].data() + fields[5].size() ||
                seconds > maximumEtaSeconds) return std::nullopt;
            result.estimatedRemainingSeconds = seconds;
            result.estimatedRemainingKnown = true;
        }
        return result;
    }

    [[nodiscard]] inline std::optional<VariantGenerationProgress> read_variant_progress(
        std::filesystem::path const& mediaDirectory) noexcept
    {
        return parse_variant_progress(read_small_file(variant_progress_path(mediaDirectory)));
    }

    inline bool write_variant_progress(std::filesystem::path const& mediaDirectory,
        std::string const& mode, VariantProgressState state, uint32_t percent = 0,
        bool determinate = false, std::string const& requestId = {},
        uint64_t estimatedRemainingSeconds = 0,
        bool estimatedRemainingKnown = false) noexcept
    {
        if ((mode != "balanced" && mode != "power-saver" && mode != "cpu-smooth") ||
            state == VariantProgressState::none ||
            (!requestId.empty() && !valid_variant_request_id(requestId))) return false;
        percent = (std::min)(percent, 100u);
        constexpr uint64_t maximumEtaSeconds = 365ULL * 24 * 60 * 60;
        estimatedRemainingSeconds = (std::min)(
            estimatedRemainingSeconds, maximumEtaSeconds);
        std::string value = requestId.empty()
            ? "v1|" + mode + "|"
            : estimatedRemainingKnown
                ? "v3|" + mode + "|" + requestId + "|"
                : "v2|" + mode + "|" + requestId + "|";
        value += variant_progress_state_name(state);
        value.push_back('|');
        value += determinate ? std::to_string(percent) : std::string("?");
        if (!requestId.empty() && estimatedRemainingKnown) {
            value.push_back('|');
            value += std::to_string(estimatedRemainingSeconds);
        }
        // Prepare() may revisit a durable request once per policy tick while it
        // waits for AC power. Avoid a needless write-through/flush when the
        // externally visible state has not changed.
        if (read_small_file(variant_progress_path(mediaDirectory)) == value) return true;
        return write_small_file(variant_progress_path(mediaDirectory), value);
    }

    inline bool write_variant_progress(std::filesystem::path const& mediaDirectory,
        VariantGenerationRequest const& request, VariantProgressState state,
        uint32_t percent = 0, bool determinate = false,
        uint64_t estimatedRemainingSeconds = 0,
        bool estimatedRemainingKnown = false) noexcept
    {
        return write_variant_progress(mediaDirectory, request.mode, state, percent,
            determinate, request.requestId, estimatedRemainingSeconds,
            estimatedRemainingKnown);
    }

    inline bool write_variant_progress_if_current(std::filesystem::path const& mediaDirectory,
        VariantGenerationRequest const& request, VariantProgressState state,
        uint32_t percent = 0, bool determinate = false,
        uint64_t estimatedRemainingSeconds = 0,
        bool estimatedRemainingKnown = false) noexcept
    {
        auto locked = lock_variant_request(mediaDirectory, request);
        return locked && write_variant_progress(
            mediaDirectory, request, state, percent, determinate,
            estimatedRemainingSeconds, estimatedRemainingKnown);
    }

    inline void clear_variant_progress(std::filesystem::path const& mediaDirectory,
        std::string const& expectedMode = {}) noexcept
    {
        try {
            if (!expectedMode.empty()) {
                auto current = read_variant_progress(mediaDirectory);
                if (current && current->mode != expectedMode) return;
            }
            std::error_code ignored;
            std::filesystem::remove(variant_progress_path(mediaDirectory), ignored);
        } catch (...) {}
    }

    inline void clear_variant_progress(std::filesystem::path const& mediaDirectory,
        VariantGenerationRequest const& expected) noexcept
    {
        try {
            unique_handle file(CreateFileW(variant_progress_path(mediaDirectory).c_str(),
                GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!file) return;
            auto current = parse_variant_progress(read_small_file_handle(file.get()));
            if (current && (current->mode != expected.mode ||
                current->requestId != expected.requestId)) return;
            if (!current) return;
            mark_locked_request_for_deletion(file.get());
        } catch (...) {}
    }

    inline std::string read_variant_request(std::filesystem::path const& mediaDirectory) noexcept
    {
        return read_variant_generation_request(mediaDirectory).mode;
    }

    inline bool request_variant_generation(std::filesystem::path const& mediaDirectory,
        std::string const& mode) noexcept
    {
        if (!valid_variant_request_mode(mode)) return false;
        try {
            VariantGenerationRequest request{ mode, new_variant_request_id() };
            auto serialized = serialize_variant_request(request);
            if (serialized.empty()) return false;
            std::filesystem::create_directories(mediaDirectory);
            if (!write_small_file(variant_request_path(mediaDirectory), serialized)) return false;
            std::error_code ignored;
            std::filesystem::remove(variant_cancelled_path(mediaDirectory), ignored);
            std::filesystem::remove(variant_paused_path(mediaDirectory), ignored);
            std::filesystem::remove(variant_failed_path(mediaDirectory), ignored);
            std::filesystem::remove(variant_suppressed_path(mediaDirectory, mode), ignored);
            // Failure to persist optional progress must not lose the durable
            // request; the UI will simply use an indeterminate bar until the
            // agent publishes its first sample.
            write_variant_progress(mediaDirectory, request, VariantProgressState::queued);
            return true;
        } catch (...) {
            return false;
        }
    }

    inline bool variant_generation_paused(std::filesystem::path const& mediaDirectory) noexcept
    {
        std::error_code error;
        return std::filesystem::is_regular_file(variant_paused_path(mediaDirectory), error) && !error;
    }

    inline bool pause_variant_generation(std::filesystem::path const& mediaDirectory) noexcept
    {
        auto request = read_variant_generation_request(mediaDirectory);
        if (!request) return false;
        auto locked = lock_variant_request(mediaDirectory, request);
        if (!locked) return false;
        if (!write_small_file(variant_paused_path(mediaDirectory), "paused")) return false;
        auto progress = read_variant_progress(mediaDirectory);
        bool matchingProgress = progress && progress->mode == request.mode &&
            progress->requestId == request.requestId;
        write_variant_progress(mediaDirectory, request, VariantProgressState::paused,
            matchingProgress ? progress->percent : 0,
            matchingProgress && progress->determinate);
        return true;
    }

    inline bool resume_variant_generation(std::filesystem::path const& mediaDirectory) noexcept
    {
        auto request = read_variant_generation_request(mediaDirectory);
        if (!request) return false;
        auto locked = lock_variant_request(mediaDirectory, request);
        if (!locked) return false;
        std::error_code error;
        std::filesystem::remove(variant_paused_path(mediaDirectory), error);
        if (error) return false;
        // FFmpeg partials are not resumable, so a resumed job starts a fresh,
        // honest attempt rather than retaining the previous attempt's percent.
        write_variant_progress(mediaDirectory, request, VariantProgressState::queued);
        return true;
    }

    inline void remove_variant_partials(std::filesystem::path const& mediaDirectory) noexcept
    {
        try {
            std::error_code error;
            auto variants = mediaDirectory / L"Variants";
            for (std::filesystem::directory_iterator entries(variants, error), end;
                !error && entries != end; entries.increment(error)) {
                std::error_code itemError;
                if (!entries->is_regular_file(itemError) || itemError) continue;
                auto name = entries->path().filename().wstring();
                if (name.ends_with(L".part.mp4")) std::filesystem::remove(entries->path(), itemError);
            }
        } catch (...) {}
    }

    // Drop a queued/in-progress request without installing the persistent
    // "cancelled by user" marker. Callers must first quiesce the Agent. This
    // is used by cache quota eviction: the files may be regenerated the next
    // time the wallpaper becomes current, while an existing explicit cancel
    // marker remains untouched.
    inline bool abandon_variant_generation_for_cache_eviction(
        std::filesystem::path const& mediaDirectory) noexcept
    {
        try {
            auto request = read_variant_generation_request(mediaDirectory);
            if (request) {
                auto locked = lock_variant_request(mediaDirectory, request);
                if (!locked || !mark_locked_request_for_deletion(locked.get())) return false;
                std::error_code ignored;
                std::filesystem::remove(variant_paused_path(mediaDirectory), ignored);
                std::filesystem::remove(variant_failed_path(mediaDirectory), ignored);
                clear_variant_progress(mediaDirectory, request);
            }
            remove_variant_partials(mediaDirectory);
            return true;
        } catch (...) {
            return false;
        }
    }

    inline bool suppress_variant_generation(std::filesystem::path const& mediaDirectory,
        std::string const& mode) noexcept
    {
        if (!valid_variant_request_mode(mode)) return false;
        if (!write_small_file(variant_suppressed_path(mediaDirectory, mode), "suppressed")) return false;
        auto request = read_variant_generation_request(mediaDirectory);
        if (request.mode == mode) {
            auto locked = lock_variant_request(mediaDirectory, request);
            if (!locked) return false;
            if (!mark_locked_request_for_deletion(locked.get())) return false;
            std::error_code ignored;
            std::filesystem::remove(variant_paused_path(mediaDirectory), ignored);
            std::filesystem::remove(variant_failed_path(mediaDirectory), ignored);
            clear_variant_progress(mediaDirectory, request);
        }
        return true;
    }

    inline void unsuppress_variant_generation(std::filesystem::path const& mediaDirectory,
        std::string const& mode) noexcept
    {
        if (mode != "balanced" && mode != "power-saver") return;
        try {
            std::error_code ignored;
            std::filesystem::remove(variant_suppressed_path(mediaDirectory, mode), ignored);
        } catch (...) {}
    }

    inline bool cancel_variant_generation(std::filesystem::path const& mediaDirectory) noexcept
    {
        auto request = read_variant_generation_request(mediaDirectory);
        auto locked = request ? lock_variant_request(mediaDirectory, request) : unique_handle{};
        if (request && !locked) return false;
        std::error_code ignored;
        if (!write_small_file(variant_cancelled_path(mediaDirectory), "cancelled")) return false;
        if (locked && !mark_locked_request_for_deletion(locked.get())) return false;
        std::filesystem::remove(variant_paused_path(mediaDirectory), ignored);
        std::filesystem::remove(variant_failed_path(mediaDirectory), ignored);
        remove_variant_partials(mediaDirectory);
        if (request) clear_variant_progress(mediaDirectory, request);
        else clear_variant_progress(mediaDirectory);
        return true;
    }

    inline bool complete_variant_generation(std::filesystem::path const& mediaDirectory,
        VariantGenerationRequest const& expected) noexcept
    {
        auto locked = lock_variant_request(mediaDirectory, expected);
        if (!locked || !mark_locked_request_for_deletion(locked.get())) return false;
        std::error_code ignored;
        std::filesystem::remove(variant_paused_path(mediaDirectory), ignored);
        std::filesystem::remove(variant_failed_path(mediaDirectory), ignored);
        clear_variant_progress(mediaDirectory, expected);
        return true;
    }

    inline bool complete_variant_generation(std::filesystem::path const& mediaDirectory,
        std::string const& expectedMode = {}) noexcept
    {
        auto current = read_variant_generation_request(mediaDirectory);
        if (!current || (!expectedMode.empty() && current.mode != expectedMode)) return false;
        return complete_variant_generation(mediaDirectory, current);
    }

    inline std::string variant_failure_mode(std::string_view record)
    {
        auto mode = record.substr(0, record.find('\n'));
        return mode == "balanced" || mode == "power-saver" || mode == "cpu-smooth" ? std::string(mode) : std::string{};
    }

    inline bool variant_failure_context_matches(std::string_view record,
        std::string_view mode, std::string_view context)
    {
        auto newline = record.find('\n');
        return !context.empty() && newline != std::string_view::npos &&
            record.substr(0, newline) == mode &&
            record.substr(newline + 1, record.find('\n', newline + 1) - newline - 1) == context;
    }

    inline std::string variant_failure_reason(std::string_view record)
    {
        auto first = record.find('\n');
        if (first == std::string_view::npos) return {};
        auto second = record.find('\n', first + 1);
        return second == std::string_view::npos ? std::string{} : std::string(record.substr(second + 1));
    }

    inline bool fail_variant_generation(std::filesystem::path const& mediaDirectory,
        VariantGenerationRequest const& expected, std::string const& context = {},
        std::string const& reason = {}) noexcept
    {
        auto locked = lock_variant_request(mediaDirectory, expected);
        if (!locked) return false;
        if (!write_small_file(variant_failed_path(mediaDirectory), expected.mode +
            (context.empty() && reason.empty() ? std::string{} : "\n" + context) +
            (reason.empty() ? std::string{} : "\n" + reason))) return false;
        if (!mark_locked_request_for_deletion(locked.get())) {
            std::error_code ignored;
            std::filesystem::remove(variant_failed_path(mediaDirectory), ignored);
            return false;
        }
        std::error_code ignored;
        std::filesystem::remove(variant_paused_path(mediaDirectory), ignored);
        clear_variant_progress(mediaDirectory, expected);
        return true;
    }

    inline bool fail_variant_generation(std::filesystem::path const& mediaDirectory,
        std::string const& expectedMode = {}) noexcept
    {
        auto current = read_variant_generation_request(mediaDirectory);
        if (!current || (!expectedMode.empty() && current.mode != expectedMode)) return false;
        return fail_variant_generation(mediaDirectory, current);
    }

    // Import and playback use the same non-destructive enqueue operation.
    // Only explicit user retries may clear a failure, cancellation or pause.
    inline VariantGenerationRequest ensure_variant_generation_request(
        std::filesystem::path const& mediaDirectory, std::string const& mode,
        VariantProgressState initialState = VariantProgressState::queued) noexcept
    {
        namespace fs = std::filesystem;
        try {
            if (!motion::valid_variant_request_mode(mode)) return {};
            auto blocked = [&] {
                std::error_code error;
                return (fs::is_regular_file(
                            motion::variant_cancelled_path(mediaDirectory), error) && !error) ||
                    motion::variant_generation_paused(mediaDirectory) ||
                    motion::variant_generation_suppressed(mediaDirectory, mode) ||
                    motion::variant_failure_mode(motion::read_small_file(
                        motion::variant_failed_path(mediaDirectory), 8192)) == mode;
            };
            if (blocked()) return {};

            auto current = motion::read_variant_generation_request(mediaDirectory);
            if (current) return current.mode == mode ? current :
                motion::VariantGenerationRequest{};

            motion::VariantGenerationRequest created{
                mode, motion::new_variant_request_id() };
            auto serialized = motion::serialize_variant_request(created);
            if (serialized.empty()) return {};
            motion::unique_handle request(CreateFileW(
                motion::variant_request_path(mediaDirectory).c_str(),
                GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr));
            if (!request) {
                auto error = GetLastError();
                if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return {};
                current = motion::read_variant_generation_request(mediaDirectory);
                return current.mode == mode ? current :
                    motion::VariantGenerationRequest{};
            }
            DWORD written{};
            bool published = WriteFile(request.get(), serialized.data(),
                    static_cast<DWORD>(serialized.size()), &written, nullptr) &&
                written == static_cast<DWORD>(serialized.size()) &&
                FlushFileBuffers(request.get());
            if (!published) motion::mark_locked_request_for_deletion(request.get());
            request.reset();
            if (!published) return {};
            if (blocked()) {
                auto locked = lock_variant_request(mediaDirectory, created);
                if (locked) mark_locked_request_for_deletion(locked.get());
                return {};
            }
            if (motion::read_variant_generation_request(mediaDirectory) != created) return {};
            // The request is authoritative even if this optional progress
            // write loses a race. A later policy pass will repair visibility.
            motion::write_variant_progress_if_current(
                mediaDirectory, created, initialState);
            return created;
        } catch (...) { return {}; }
    }

    inline VariantCacheStatus inspect_variant_cache(std::filesystem::path const& mediaDirectory) noexcept
    {
        VariantCacheStatus result;
        try {
            auto request = read_variant_generation_request(mediaDirectory);
            result.requestedMode = request.mode;
            result.queued = !result.requestedMode.empty();
            result.paused = result.queued && variant_generation_paused(mediaDirectory);
            auto progress = read_variant_progress(mediaDirectory);
            bool internalProgress = !request && progress && progress->mode == "cpu-smooth" &&
                progress->requestId.empty();
            if (progress && ((result.queued && progress->mode == result.requestedMode &&
                    progress->requestId == request.requestId) || internalProgress)) {
                result.waitingForPower = !result.paused &&
                    progress->state == VariantProgressState::waitingForPower;
                result.progressPercent = progress->percent;
                result.progressKnown = progress->determinate;
                result.estimatedRemainingSeconds =
                    progress->estimatedRemainingSeconds;
                result.estimatedRemainingKnown =
                    progress->estimatedRemainingKnown;
                result.generating = !result.paused &&
                    progress->state == VariantProgressState::generating;
            }
            result.cancelled = std::filesystem::is_regular_file(variant_cancelled_path(mediaDirectory));
            result.failed = std::filesystem::is_regular_file(variant_failed_path(mediaDirectory));
            if (result.failed) {
                unique_handle failure(CreateFileW(variant_failed_path(mediaDirectory).c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr));
                char value[8192]{};
                DWORD read{};
                if (failure && ReadFile(failure.get(), value, sizeof(value) - 1, &read, nullptr)) {
                    result.failedMode = variant_failure_mode(std::string_view(value, read));
                    result.failedReason = variant_failure_reason(std::string_view(value, read));
                }
            }
            result.balancedSuppressed = variant_generation_suppressed(mediaDirectory, "balanced");
            result.powerSaverSuppressed = variant_generation_suppressed(mediaDirectory, "power-saver");
            auto variants = mediaDirectory / L"Variants";
            std::error_code error;
            for (std::filesystem::directory_iterator entries(variants, error), end;
                !error && entries != end; entries.increment(error)) {
                std::error_code itemError;
                if (!entries->is_regular_file(itemError) || itemError) continue;
                auto name = entries->path().filename().wstring();
                if (name.ends_with(L".part.mp4")) result.generating = !result.paused;
                else if (entries->path().extension() == L".mp4") {
                    auto size = entries->file_size(itemError);
                    if (!itemError) {
                        std::string mode;
                        if (name.starts_with(L"balanced-")) mode = "balanced";
                        else if (name.starts_with(L"power-saver-")) mode = "power-saver";
                        else if (name.starts_with(L"cpu-smooth-")) mode = "cpu-smooth";
                        if (mode.empty()) continue;
                        result.entries.push_back({ name, std::move(mode), size });
                    }
                }
            }
            struct FileIdentity
            {
                DWORD volume{};
                DWORD high{};
                DWORD low{};
            };
            std::vector<std::pair<FileIdentity, size_t>> identities;
            for (size_t index = 0; index < result.entries.size(); ++index) {
                auto const path = variants / result.entries[index].fileName;
                unique_handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
                BY_HANDLE_FILE_INFORMATION information{};
                bool identified = file && GetFileInformationByHandle(file.get(), &information);
                FileIdentity identity{ information.dwVolumeSerialNumber,
                    information.nFileIndexHigh, information.nFileIndexLow };
                auto duplicate = identified ? std::find_if(identities.begin(), identities.end(),
                    [&](auto const& existing) {
                        return existing.first.volume == identity.volume &&
                            existing.first.high == identity.high && existing.first.low == identity.low;
                    }) : identities.end();
                if (duplicate != identities.end()) {
                    result.entries[index].sharedStorage = true;
                    result.entries[duplicate->second].sharedStorage = true;
                    continue;
                }
                if (identified) identities.emplace_back(identity, index);
                ++result.files;
                result.bytes += result.entries[index].bytes;
            }
        } catch (...) {}
        return result;
    }
}
