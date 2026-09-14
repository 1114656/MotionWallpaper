#include "Common.h"
#include "TextEncoding.h"
#include "UniqueHandle.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#include <objbase.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

namespace fs = std::filesystem;
using namespace winrt;
using namespace Windows::Data::Json;

namespace
{
    bool safe_display_id(std::string const& value)
    {
        return !value.empty() && value.size() <= 512 && std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return character >= 0x20 && character != 0x7f;
        });
    }

    bool direct_directory_no_reparse(fs::path const& path) noexcept
    {
        auto attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    }

    bool direct_regular_file_no_reparse(fs::path const& path) noexcept
    {
        auto attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
    }

    struct DirectObject
    {
        motion::unique_handle handle;
        motion::FilesystemObjectIdentity identity;
    };

    std::optional<DirectObject> open_direct_object(
        fs::path const& path, bool directory, bool protectIdentity) noexcept
    {
        DWORD access = FILE_READ_ATTRIBUTES | (directory ? 0 : GENERIC_READ);
        DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE |
            (protectIdentity ? 0 : FILE_SHARE_DELETE);
        if (protectIdentity && !directory) share = FILE_SHARE_READ;
        DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT |
            (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0);
        motion::unique_handle handle(CreateFileW(path.c_str(), access, share, nullptr,
            OPEN_EXISTING, flags, nullptr));
        if (!handle) return std::nullopt;

        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(handle.get(), &information) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            (((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory)) {
            return std::nullopt;
        }

        motion::FilesystemObjectIdentity identity{};
        FILE_ID_INFO fileId{};
        if (GetFileInformationByHandleEx(handle.get(), FileIdInfo, &fileId, sizeof(fileId))) {
            identity.volumeSerialNumber = fileId.VolumeSerialNumber;
            std::copy(std::begin(fileId.FileId.Identifier), std::end(fileId.FileId.Identifier),
                identity.fileId.begin());
        } else {
            identity.volumeSerialNumber = information.dwVolumeSerialNumber;
            uint64_t fallbackId =
                (static_cast<uint64_t>(information.nFileIndexHigh) << 32) |
                information.nFileIndexLow;
            std::memcpy(identity.fileId.data(), &fallbackId, sizeof(fallbackId));
        }
        return DirectObject{ std::move(handle), identity };
    }

    std::optional<std::string> ownership_id_from_handle(HANDLE marker) noexcept
    {
        try {
            LARGE_INTEGER size{};
            if (!marker || !GetFileSizeEx(marker, &size) || size.QuadPart <= 0 ||
                size.QuadPart > 256) return std::nullopt;
            LARGE_INTEGER beginning{};
            if (!SetFilePointerEx(marker, beginning, nullptr, FILE_BEGIN)) return std::nullopt;
            std::string value(static_cast<size_t>(size.QuadPart), '\0');
            DWORD read{};
            if (!ReadFile(marker, value.data(), static_cast<DWORD>(value.size()), &read, nullptr) ||
                read != static_cast<DWORD>(value.size())) return std::nullopt;
            auto prefixLength = std::char_traits<char>::length(
                motion::media_library_ownership_marker_prefix);
            if (value.size() <= prefixLength ||
                !value.starts_with(motion::media_library_ownership_marker_prefix) ||
                value.back() != '\n') return std::nullopt;
            auto id = value.substr(prefixLength, value.size() - prefixLength - 1);
            if (id.empty() || id.size() > 64 ||
                !std::all_of(id.begin(), id.end(), [](unsigned char character) {
                    return (character >= '0' && character <= '9') ||
                        (character >= 'a' && character <= 'f') || character == '-';
                })) return std::nullopt;
            return id;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<fs::path> stable_volume_path_from_handle(HANDLE handle) noexcept
    {
        try {
            std::wstring value(512, L'\0');
            DWORD length{};
            for (;;) {
                length = GetFinalPathNameByHandleW(handle, value.data(),
                    static_cast<DWORD>(value.size()),
                    FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
                if (!length) return std::nullopt;
                if (length < value.size()) break;
                if (length >= 32768) return std::nullopt;
                value.resize(static_cast<size_t>(length) + 1);
            }
            value.resize(length);
            constexpr std::wstring_view prefix = LR"(\\?\Volume{)";
            if (value.size() <= prefix.size() ||
                CompareStringOrdinal(value.data(), static_cast<int>(prefix.size()),
                    prefix.data(), static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL ||
                value.find(L"}\\", prefix.size()) == std::wstring::npos) {
                // VOLUME_NAME_GUID is intentionally mandatory. UNC/provider
                // aliases have no non-reassignable volume path and therefore
                // cannot safely support external-library mutation.
                return std::nullopt;
            }
            fs::path stable(value);
            return stable.is_absolute() && stable.has_filename()
                ? std::optional<fs::path>(std::move(stable)) : std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    }

    bool stable_identity_matches(motion::MediaLibraryTrustIdentity const& identity,
        bool protectIdentity, std::optional<DirectObject>* rootOutput = nullptr,
        std::optional<DirectObject>* markerOutput = nullptr,
        std::optional<DirectObject>* groupsOutput = nullptr) noexcept
    {
        try {
            if (identity.stableRoot.empty()) return false;
            auto root = open_direct_object(identity.stableRoot, true, protectIdentity);
            auto marker = open_direct_object(
                identity.stableRoot / motion::media_library_ownership_marker_name,
                false, protectIdentity);
            auto groups = open_direct_object(identity.stableRoot / L"Groups", true,
                protectIdentity);
            if (!root || !marker || !groups ||
                root->identity != identity.rootIdentity ||
                marker->identity != identity.markerIdentity ||
                groups->identity != identity.groupsIdentity) return false;
            auto ownershipId = ownership_id_from_handle(marker->handle.get());
            if (!ownershipId || *ownershipId != identity.ownershipId) return false;
            if (rootOutput) *rootOutput = std::move(root);
            if (markerOutput) *markerOutput = std::move(marker);
            if (groupsOutput) *groupsOutput = std::move(groups);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool safe_default_library(fs::path const& path) noexcept
    {
        try {
            auto attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                auto error = GetLastError();
                // A missing default is the compatible first-start case. The App
                // may create it only after the settings document is accepted.
                return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
            }
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;

            std::error_code error;
            for (fs::recursive_directory_iterator entries(path, fs::directory_options::none, error), end;
                !error && entries != end; entries.increment(error)) {
                attributes = GetFileAttributesW(entries->path().c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES ||
                    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
                    ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                        !entries->is_regular_file(error))) return false;
                if (error) return false;
            }
            return !error;
        } catch (...) {
            return false;
        }
    }

    fs::path canonical_path_identity(fs::path const& value)
    {
        if (value.empty()) throw std::invalid_argument("empty filesystem path");
        std::error_code error;
        auto normalized = fs::weakly_canonical(value, error);
        if (error) {
            error.clear();
            normalized = fs::absolute(value, error).lexically_normal();
            if (error) throw std::system_error(error);
        }
        if (!normalized.is_absolute()) {
            normalized = fs::absolute(normalized, error).lexically_normal();
            if (error || !normalized.is_absolute()) throw std::runtime_error("cannot normalize filesystem path");
        }
        return normalized.lexically_normal();
    }

    bool same_path_component(fs::path const& left, fs::path const& right) noexcept
    {
        auto leftText = left.native();
        auto rightText = right.native();
        if (leftText.size() > INT_MAX || rightText.size() > INT_MAX) return false;
        return CompareStringOrdinal(leftText.c_str(), static_cast<int>(leftText.size()),
            rightText.c_str(), static_cast<int>(rightText.size()), TRUE) == CSTR_EQUAL;
    }

    constexpr uint32_t migrationOwnerMagic = 0x4D574D4F; // MWMO
    constexpr uint32_t migrationOwnerVersion = 1;

    struct MigrationOwnerRecord
    {
        uint32_t magic{};
        uint32_t version{};
        uint32_t processId{};
        uint32_t reserved{};
        uint64_t processCreationTime{};
        uint64_t token{};
    };

    uint64_t file_time_value(FILETIME const& value) noexcept
    {
        ULARGE_INTEGER result{};
        result.LowPart = value.dwLowDateTime;
        result.HighPart = value.dwHighDateTime;
        return result.QuadPart;
    }

    uint64_t process_creation_time(HANDLE process) noexcept
    {
        FILETIME created{}, exited{}, kernel{}, user{};
        return GetProcessTimes(process, &created, &exited, &kernel, &user)
            ? file_time_value(created) : 0;
    }

    class migration_owner_lock final
    {
    public:
        explicit migration_owner_lock(HANDLE mutex) noexcept : mutex_(mutex)
        {
            auto result = mutex ? WaitForSingleObject(mutex, 1000) : WAIT_FAILED;
            owns_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        }
        ~migration_owner_lock() { if (owns_) ReleaseMutex(mutex_); }
        explicit operator bool() const noexcept { return owns_; }
    private:
        HANDLE mutex_{};
        bool owns_{};
    };

    bool migration_owner_is_live(MigrationOwnerRecord const& owner) noexcept
    {
        if (owner.magic != migrationOwnerMagic || owner.version != migrationOwnerVersion ||
            !owner.processId || !owner.processCreationTime || !owner.token) return false;
        motion::unique_handle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE, owner.processId));
        if (!process) {
            // Access denial is treated conservatively as a live owner. App and
            // Agent normally run as the same user, so stale owners remain
            // recoverable without risking another session's active request.
            return GetLastError() != ERROR_INVALID_PARAMETER;
        }
        auto creationTime = process_creation_time(process.get());
        if (!creationTime) return true;
        if (creationTime != owner.processCreationTime) return false;
        auto wait = WaitForSingleObject(process.get(), 0);
        return wait == WAIT_TIMEOUT || wait == WAIT_FAILED;
    }

    bool same_migration_owner(MigrationOwnerRecord const& owner,
        DWORD processId, uint64_t creationTime, uint64_t token) noexcept
    {
        return owner.magic == migrationOwnerMagic && owner.version == migrationOwnerVersion &&
            owner.processId == processId && owner.processCreationTime == creationTime &&
            owner.token == token;
    }

    std::wstring read_text(fs::path const& path)
    {
        std::error_code sizeError;
        auto size = fs::file_size(path, sizeError);
        if (!sizeError && size > 4 * 1024 * 1024) throw std::runtime_error("JSON file is too large");
        std::ifstream input(path, std::ios::binary);
        if (!input) return {};
        std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        return motion::utf8_to_wide(bytes);
    }

    void write_text_atomic(fs::path const& path, std::wstring const& value)
    {
        fs::create_directories(path.parent_path());
        auto identity = fs::absolute(path).lexically_normal().wstring();
        uint64_t hash = 1469598103934665603ULL;
        for (auto character : identity) {
            hash ^= static_cast<uint16_t>(towlower(character));
            hash *= 1099511628211ULL;
        }
        auto mutexName = L"Local\\MotionWallpaper.AtomicJson." + std::to_wstring(hash);
        motion::unique_handle writeMutex(CreateMutexW(nullptr, FALSE, mutexName.c_str()));
        if (!writeMutex) throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
        auto waitResult = WaitForSingleObject(writeMutex.get(), 10'000);
        if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED) {
            throw std::runtime_error("timed out waiting for atomic JSON writer");
        }
        struct MutexRelease
        {
            HANDLE value{};
            ~MutexRelease() { if (value) ReleaseMutex(value); }
        } release{ writeMutex.get() };
        static std::atomic_uint64_t temporarySequence{};
        auto temporary = path.parent_path() /
            (path.filename().wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(++temporarySequence));
        try {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("cannot open temporary JSON file: " + motion::wide_to_utf8(temporary.wstring()));
            auto bytes = motion::wide_to_utf8(value);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.flush();
            if (!output) throw std::runtime_error("cannot write temporary JSON file");
            output.close();
            DWORD replaceError{};
            bool replaced{};
            for (unsigned attempt = 0; attempt < 8; ++attempt) {
                if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    replaced = true;
                    break;
                }
                replaceError = GetLastError();
                if (replaceError != ERROR_ACCESS_DENIED && replaceError != ERROR_SHARING_VIOLATION &&
                    replaceError != ERROR_LOCK_VIOLATION) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2u << attempt));
            }
            if (!replaced) throw std::system_error(static_cast<int>(replaceError), std::system_category());
        } catch (...) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            throw;
        }
    }

    JsonObject parse_object(fs::path const& path)
    {
        auto text = read_text(path);
        if (text.empty()) throw std::runtime_error("JSON file is empty");
        return JsonObject::Parse(text);
    }

    std::wstring json_wstring(JsonObject const& object, wchar_t const* name)
    {
        return object.GetNamedString(name, L"").c_str();
    }

    std::string json_string(JsonObject const& object, wchar_t const* name)
    {
        return motion::wide_to_utf8(json_wstring(object, name));
    }

    int json_int(JsonObject const& object, wchar_t const* name, int fallback, int minimum, int maximum)
    {
        auto value = object.GetNamedNumber(name, fallback);
        if (!std::isfinite(value)) return fallback;
        value = (std::clamp)(value, static_cast<double>(minimum), static_cast<double>(maximum));
        return static_cast<int>(value);
    }

    uint64_t json_uint64(JsonObject const& object, wchar_t const* name)
    {
        auto value = object.GetNamedNumber(name, 0);
        if (!std::isfinite(value) || value <= 0) return 0;
        // JSON numbers are doubles. UINT64_MAX rounds up to 2^64 and would make
        // the final conversion undefined, so cap at an exactly convertible bound.
        constexpr auto maximum = static_cast<double>((std::numeric_limits<int64_t>::max)());
        return static_cast<uint64_t>((std::min)(value, maximum));
    }
}

namespace motion
{
    LibraryMigrationOwnerChannel::LibraryMigrationOwnerChannel(
        wchar_t const* mutexName, wchar_t const* mappingName) noexcept
        : mutex_(CreateMutexW(nullptr, FALSE, mutexName)),
          mapping_(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
              sizeof(MigrationOwnerRecord), mappingName)),
          processCreationTime_(process_creation_time(GetCurrentProcess()))
    {
        if (mapping_) view_ = MapViewOfFile(mapping_.get(), FILE_MAP_READ | FILE_MAP_WRITE,
            0, 0, sizeof(MigrationOwnerRecord));
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        token_ = static_cast<uint64_t>(counter.QuadPart) ^ GetTickCount64() ^
            (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this));
        if (!token_) token_ = 1;
    }

    LibraryMigrationOwnerChannel::~LibraryMigrationOwnerChannel()
    {
        ReleaseClaim();
        if (view_) UnmapViewOfFile(view_);
    }

    LibraryMigrationOwnerChannel::operator bool() const noexcept
    {
        return mutex_ && mapping_ && view_ && processCreationTime_ && token_;
    }

    bool LibraryMigrationOwnerChannel::TryClaimAndReset(
        HANDLE requested, HANDLE quiesced, HANDLE applied) noexcept
    {
        if (!*this || !requested || !quiesced || !applied) return false;
        migration_owner_lock lock(mutex_.get());
        if (!lock) return false;
        auto& owner = *static_cast<MigrationOwnerRecord*>(view_);
        if (migration_owner_is_live(owner)) return false;
        if (!ResetEvent(requested) || !ResetEvent(quiesced) || !ResetEvent(applied)) return false;
        owner = { migrationOwnerMagic, migrationOwnerVersion, GetCurrentProcessId(), 0,
            processCreationTime_, token_ };
        MemoryBarrier();
        return true;
    }

    bool LibraryMigrationOwnerChannel::ResetRequestForClaim(HANDLE requested) noexcept
    {
        if (!*this || !requested) return false;
        migration_owner_lock lock(mutex_.get());
        if (!lock) return false;
        auto const& owner = *static_cast<MigrationOwnerRecord*>(view_);
        return same_migration_owner(owner, GetCurrentProcessId(), processCreationTime_, token_) &&
            ResetEvent(requested) != FALSE;
    }

    void LibraryMigrationOwnerChannel::ReleaseClaim() noexcept
    {
        if (!*this) return;
        migration_owner_lock lock(mutex_.get());
        if (!lock) return;
        auto& owner = *static_cast<MigrationOwnerRecord*>(view_);
        if (same_migration_owner(owner, GetCurrentProcessId(), processCreationTime_, token_)) {
            owner = {};
            MemoryBarrier();
        }
    }

    bool LibraryMigrationOwnerChannel::ClearOrphanedRequest(
        HANDLE requested, HANDLE quiesced, HANDLE applied) noexcept
    {
        if (!*this || !requested || !quiesced || !applied) return false;
        migration_owner_lock lock(mutex_.get());
        if (!lock) return false;
        auto& owner = *static_cast<MigrationOwnerRecord*>(view_);
        if (migration_owner_is_live(owner)) return false;
        if (!ResetEvent(requested) || !ResetEvent(quiesced) || !ResetEvent(applied)) return false;
        owner = {};
        MemoryBarrier();
        return true;
    }

    void append_utf8_log(fs::path const& path, std::wstring_view message) noexcept
    {
        try {
            if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
            constexpr uintmax_t maximumLogBytes = 2 * 1024 * 1024;
            constexpr uintmax_t retainedLogBytes = maximumLogBytes / 2;
            std::error_code sizeError;
            auto size = fs::file_size(path, sizeError);
            if (!sizeError && size > maximumLogBytes) {
                std::ifstream input(path, std::ios::binary);
                if (input) {
                    input.seekg(-static_cast<std::streamoff>(retainedLogBytes), std::ios::end);
                    std::string retained((std::istreambuf_iterator<char>(input)), {});
                    auto firstLine = retained.find('\n');
                    if (firstLine != std::string::npos) retained.erase(0, firstLine + 1);
                    auto temporary = path;
                    temporary += L".rotate.tmp";
                    std::ofstream rotated(temporary, std::ios::binary | std::ios::trunc);
                    if (rotated) {
                        rotated.write(retained.data(), static_cast<std::streamsize>(retained.size()));
                        rotated.close();
                        MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                    }
                }
            }
            auto line = wide_to_utf8(timestamp_utc() + L" " + std::wstring(message) + L"\n");
            std::ofstream output(path, std::ios::binary | std::ios::app);
            if (output) output.write(line.data(), static_cast<std::streamsize>(line.size()));
        } catch (...) {}
    }

    std::wstring quote_command_line_argument(std::wstring_view value)
    {
        if (value.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) return std::wstring(value);
        std::wstring result{ L'\"' };
        size_t backslashes{};
        for (auto character : value) {
            if (character == L'\\') {
                ++backslashes;
            } else if (character == L'\"') {
                result.append(backslashes * 2 + 1, L'\\');
                result.push_back(character);
                backslashes = 0;
            } else {
                result.append(backslashes, L'\\');
                result.push_back(character);
                backslashes = 0;
            }
        }
        result.append(backslashes * 2, L'\\');
        result.push_back(L'\"');
        return result;
    }

    std::wstring build_command_line(std::vector<std::wstring> const& arguments)
    {
        std::wstring result;
        for (auto const& argument : arguments) {
            if (!result.empty()) result.push_back(L' ');
            result += quote_command_line_argument(argument);
        }
        return result;
    }

    std::chrono::milliseconds IdleTimer::Update(
        std::chrono::milliseconds now,
        uint32_t inputTick,
        std::chrono::milliseconds rawIdle,
        bool activityInhibitsIdle) noexcept
    {
        if (!initialized_ || inputTick != inputTick_) {
            initialized_ = true;
            inputTick_ = inputTick;
            idleSince_ = now - std::min(now, rawIdle);
        }
        if (activityInhibitsIdle) idleSince_ = now;
        return now > idleSince_ ? now - idleSince_ : std::chrono::milliseconds::zero();
    }

    fs::path executable_directory()
    {
        std::wstring value(32768, L'\0');
        DWORD length = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
        if (!length || length >= value.size()) throw std::runtime_error("GetModuleFileNameW failed");
        value.resize(length);
        return fs::path(value).parent_path();
    }

    fs::path select_application_data_directory(fs::path const& applicationRoot, fs::path const& localAppDataRoot)
    {
        auto legacyRoot = localAppDataRoot / L"MotionWallpaper";
        // The installer writes this only when its verified legacy-data handoff
        // did not complete. It must override portable/legacy target discovery so
        // a partial install-tree copy cannot become authoritative.
        if (!localAppDataRoot.empty() &&
            direct_regular_file_no_reparse(applicationRoot / legacy_data_fallback_marker_name) &&
            direct_directory_no_reparse(legacyRoot)) return legacyRoot;

        std::error_code error;
        bool portable = fs::is_regular_file(applicationRoot / L"portable.mode", error);
        error.clear();
        bool legacyConfig = fs::is_directory(applicationRoot / L"Config", error);
        error.clear();
        bool legacyLibrary = fs::is_directory(applicationRoot / L"Wallpapers", error);
        if (portable || legacyConfig || legacyLibrary || localAppDataRoot.empty()) return applicationRoot;
        return localAppDataRoot / L"MotionWallpaper";
    }

    bool legacy_data_conflict_present(fs::path const& applicationRoot) noexcept
    {
        try {
            return direct_regular_file_no_reparse(
                applicationRoot / legacy_data_conflict_marker_name);
        } catch (...) {
            return false;
        }
    }

    fs::path application_data_directory()
    {
        auto applicationRoot = executable_directory();
        DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        if (!required) return select_application_data_directory(applicationRoot, {});
        std::wstring value(required, L'\0');
        DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), required);
        if (!length || length >= required) return select_application_data_directory(applicationRoot, {});
        value.resize(length);
        return select_application_data_directory(applicationRoot, fs::path(value));
    }

    fs::path wallpaper_library_directory(fs::path const& dataRoot, std::wstring const& configuredPath)
    {
        if (!configuredPath.empty()) {
            fs::path configured = fs::path(configuredPath).lexically_normal();
            if (!configured.is_absolute() || !configured.has_filename()) {
                throw std::invalid_argument("invalid configured media-library path");
            }
            return configured;
        }
        auto library = (dataRoot / L"Wallpapers").lexically_normal();
        if (!library.is_absolute() || !library.has_filename() || !safe_default_library(library)) {
            throw std::invalid_argument("unsafe default media-library path");
        }
        return library;
    }

    bool same_filesystem_path(fs::path const& left, fs::path const& right)
    {
        auto normalizedLeft = canonical_path_identity(left);
        auto normalizedRight = canonical_path_identity(right);
        auto leftEntry = normalizedLeft.begin();
        auto rightEntry = normalizedRight.begin();
        for (; leftEntry != normalizedLeft.end() && rightEntry != normalizedRight.end();
            ++leftEntry, ++rightEntry) {
            if (!same_path_component(*leftEntry, *rightEntry)) return false;
        }
        return leftEntry == normalizedLeft.end() && rightEntry == normalizedRight.end();
    }

    bool filesystem_path_is_nested(fs::path const& parent, fs::path const& child)
    {
        auto normalizedParent = canonical_path_identity(parent);
        auto normalizedChild = canonical_path_identity(child);
        auto parentEntry = normalizedParent.begin();
        auto childEntry = normalizedChild.begin();
        for (; parentEntry != normalizedParent.end() && childEntry != normalizedChild.end();
            ++parentEntry, ++childEntry) {
            if (!same_path_component(*parentEntry, *childEntry)) return false;
        }
        return parentEntry == normalizedParent.end() && childEntry != normalizedChild.end();
    }

    fs::path ffmpeg_executable_path(fs::path const& applicationRoot)
    {
        return applicationRoot / L"Tools" / L"ffmpeg" / L"ffmpeg.exe";
    }

    std::wstring utf8_to_wide(std::string const& value)
    {
        if (value.empty()) return {};
        int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (length <= 0) throw std::runtime_error("invalid UTF-8");
        std::wstring result(static_cast<size_t>(length), L'\0');
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length)) {
            throw std::runtime_error("UTF-8 conversion failed");
        }
        return result;
    }

    std::string wide_to_utf8(std::wstring_view value)
    {
        return utf8_from_wide(value);
    }

    std::string new_id()
    {
        GUID id{};
        if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("CoCreateGuid failed");
        wchar_t value[40]{};
        if (StringFromGUID2(id, value, ARRAYSIZE(value)) <= 0) throw std::runtime_error("GUID conversion failed");
        std::wstring result(value + 1, value + 37);
        std::transform(result.begin(), result.end(), result.begin(), towlower);
        return wide_to_utf8(result);
    }

    std::wstring timestamp_utc()
    {
        SYSTEMTIME now{};
        GetSystemTime(&now);
        wchar_t value[40]{};
        swprintf_s(value, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
        return value;
    }

    bool valid_id(std::string const& value)
    {
        return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || character == '-';
        });
    }

    bool valid_id(std::wstring const& value)
    {
        return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](wchar_t character) {
            return (character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f') || character == L'-';
        });
    }

    bool safe_file_name(fs::path const& value)
    {
        return !value.empty() && !value.is_absolute() && value == value.filename() &&
            value != L"." && value != L"..";
    }

    MediaLibraryTrustLease::MediaLibraryTrustLease(
        unique_handle root, unique_handle stableRoot,
        unique_handle marker, unique_handle groups) noexcept
        : root_(std::move(root)), stableRoot_(std::move(stableRoot)),
          marker_(std::move(marker)), groups_(std::move(groups))
    {
    }

    std::optional<std::string> media_library_ownership_id(fs::path const& libraryRoot) noexcept
    {
        try {
            if (!libraryRoot.is_absolute() || !libraryRoot.has_filename() ||
                !direct_directory_no_reparse(libraryRoot)) return std::nullopt;
            auto marker = libraryRoot / media_library_ownership_marker_name;
            auto markerObject = open_direct_object(marker, false, false);
            return markerObject ? ownership_id_from_handle(markerObject->handle.get()) : std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<MediaLibraryTrustIdentity> capture_media_library_trust(
        fs::path const& libraryRoot) noexcept
    {
        try {
            auto root = libraryRoot.lexically_normal();
            if (!root.is_absolute() || !root.has_filename()) return std::nullopt;
            auto rootObject = open_direct_object(root, true, false);
            if (!rootObject) return std::nullopt;
            auto stableRoot = stable_volume_path_from_handle(rootObject->handle.get());
            if (!stableRoot) return std::nullopt;
            auto stableRootObject = open_direct_object(*stableRoot, true, false);
            if (!stableRootObject || stableRootObject->identity != rootObject->identity) {
                return std::nullopt;
            }
            auto markerObject = open_direct_object(
                *stableRoot / media_library_ownership_marker_name, false, false);
            auto groupsObject = open_direct_object(*stableRoot / L"Groups", true, false);
            if (!markerObject || !groupsObject) return std::nullopt;
            auto ownershipId = ownership_id_from_handle(markerObject->handle.get());
            if (!ownershipId) return std::nullopt;

            // Close the first root handle only after every child identity has
            // been captured, then prove the path still resolves to that root.
            auto rootAgain = open_direct_object(root, true, false);
            if (!rootAgain || rootAgain->identity != rootObject->identity) return std::nullopt;
            auto stableAgain = open_direct_object(*stableRoot, true, false);
            if (!stableAgain || stableAgain->identity != rootObject->identity) return std::nullopt;
            return MediaLibraryTrustIdentity{ std::move(root), std::move(*stableRoot),
                std::move(*ownershipId),
                rootObject->identity, markerObject->identity, groupsObject->identity };
        } catch (...) {
            return std::nullopt;
        }
    }

    std::shared_ptr<MediaLibraryTrustLease> acquire_media_library_trust(
        MediaLibraryTrustIdentity const& identity) noexcept
    {
        try {
            if (!identity.root.is_absolute() || !identity.root.has_filename() ||
                identity.stableRoot.empty()) return {};
            auto root = open_direct_object(identity.root, true, true);
            std::optional<DirectObject> stableRoot;
            std::optional<DirectObject> marker;
            std::optional<DirectObject> groups;
            if (!root || root->identity != identity.rootIdentity ||
                !stable_identity_matches(identity, true, &stableRoot, &marker, &groups) ||
                !stableRoot || stableRoot->identity != root->identity) return {};
            return std::shared_ptr<MediaLibraryTrustLease>(new MediaLibraryTrustLease(
                std::move(root->handle), std::move(stableRoot->handle),
                std::move(marker->handle), std::move(groups->handle)));
        } catch (...) {
            return {};
        }
    }

    bool revalidate_media_library_trust(MediaLibraryTrustIdentity const& identity) noexcept
    {
        return static_cast<bool>(acquire_media_library_trust(identity));
    }

    bool revalidate_media_library_stable_root(
        MediaLibraryTrustIdentity const& identity) noexcept
    {
        return stable_identity_matches(identity, false);
    }

    std::optional<fs::path> media_library_stable_path(
        MediaLibraryTrustIdentity const& identity,
        fs::path const& configuredPath) noexcept
    {
        try {
            if (identity.root.empty() || identity.stableRoot.empty() ||
                !identity.root.is_absolute() || !configuredPath.is_absolute()) {
                return std::nullopt;
            }
            auto root = identity.root.lexically_normal();
            auto path = configuredPath.lexically_normal();
            auto rootPart = root.begin();
            auto pathPart = path.begin();
            for (; rootPart != root.end(); ++rootPart, ++pathPart) {
                if (pathPart == path.end()) return std::nullopt;
                auto left = rootPart->native();
                auto right = pathPart->native();
                if (CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                        right.data(), static_cast<int>(right.size()), TRUE) !=
                    CSTR_EQUAL) {
                    return std::nullopt;
                }
            }
            fs::path relative;
            for (; pathPart != path.end(); ++pathPart) {
                if (*pathPart == L"." || *pathPart == L".." ||
                    pathPart->has_root_name() || pathPart->has_root_directory()) {
                    return std::nullopt;
                }
                relative /= *pathPart;
            }
            return relative.empty()
                ? std::optional<fs::path>(identity.stableRoot)
                : std::optional<fs::path>(identity.stableRoot / relative);
        } catch (...) {
            return std::nullopt;
        }
    }

    bool same_direct_filesystem_object(
        fs::path const& left, fs::path const& right) noexcept
    {
        try {
            auto leftAttributes = GetFileAttributesW(left.c_str());
            auto rightAttributes = GetFileAttributesW(right.c_str());
            if (leftAttributes == INVALID_FILE_ATTRIBUTES ||
                rightAttributes == INVALID_FILE_ATTRIBUTES ||
                (leftAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
                (rightAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                return false;
            }
            bool leftDirectory =
                (leftAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            bool rightDirectory =
                (rightAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (leftDirectory != rightDirectory) return false;
            auto leftObject = open_direct_object(left, leftDirectory, false);
            auto rightObject = open_direct_object(right, rightDirectory, false);
            return leftObject && rightObject &&
                leftObject->identity == rightObject->identity;
        } catch (...) {
            return false;
        }
    }

    bool is_owned_media_library(fs::path const& libraryRoot) noexcept
    {
        try {
            if (!media_library_ownership_id(libraryRoot)) return false;
            auto groups = libraryRoot / L"Groups";
            if (!direct_directory_no_reparse(groups)) return false;

            std::error_code error;
            for (fs::directory_iterator entries(libraryRoot, error), end;
                !error && entries != end; entries.increment(error)) {
                auto name = entries->path().filename();
                if (name == media_library_ownership_marker_name &&
                    direct_regular_file_no_reparse(entries->path())) continue;
                if (name == L"Groups" && direct_directory_no_reparse(entries->path())) continue;
                return false;
            }
            if (error) return false;

            for (fs::recursive_directory_iterator entries(groups, fs::directory_options::none, error), end;
                !error && entries != end; entries.increment(error)) {
                auto attributes = GetFileAttributesW(entries->path().c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                    (!(attributes & FILE_ATTRIBUTE_DIRECTORY) && !entries->is_regular_file(error))) {
                    return false;
                }
                if (error) return false;
            }
            return !error;
        } catch (...) {
            return false;
        }
    }

    static std::optional<Settings> load_settings_document(
        fs::path const& path,
        bool preserveUnavailableLibrary,
        bool& libraryUnavailable)
    {
        libraryUnavailable = false;
        if (!direct_regular_file_no_reparse(path)) return std::nullopt;
        auto object = parse_object(path);
        Settings settings;
        auto storedVersion = json_int(object, L"version", 1, 0, settings_schema_version + 1);
        if (storedVersion < 1 || storedVersion > settings_schema_version) return std::nullopt;
        settings.version = settings_schema_version;
        settings.desktopPlayback = object.GetNamedBoolean(L"desktopPlayback", settings.desktopPlayback);
        settings.activePlaybackEnabled = object.GetNamedBoolean(L"activePlaybackEnabled", settings.activePlaybackEnabled);
        settings.continueWhenCovered = object.GetNamedBoolean(L"continueWhenCovered", settings.continueWhenCovered);
        settings.screensaverEnabled = object.GetNamedBoolean(L"screensaverEnabled", settings.screensaverEnabled);
        settings.idleTimeoutSeconds = json_int(object, L"idleTimeoutSeconds", settings.idleTimeoutSeconds, 10, 86400);
        if (storedVersion >= 8) {
            settings.autoLockEnabled = object.GetNamedBoolean(L"autoLockEnabled", settings.autoLockEnabled);
            settings.autoLockTimeoutSeconds = json_int(
                object, L"autoLockTimeoutSeconds", settings.autoLockTimeoutSeconds, 30, 86400);
            settings.displayOffAfterLockEnabled = object.GetNamedBoolean(
                L"displayOffAfterLockEnabled", settings.displayOffAfterLockEnabled);
            settings.displayOffAfterLockDelaySeconds = json_int(
                object, L"displayOffAfterLockDelaySeconds", settings.displayOffAfterLockDelaySeconds, 0, 86400);
        } else if (storedVersion == 7) {
            settings.autoLockEnabled = object.GetNamedBoolean(L"displayOffEnabled", settings.autoLockEnabled);
            settings.autoLockTimeoutSeconds = json_int(
                object, L"displayOffTimeoutSeconds", settings.autoLockTimeoutSeconds, 30, 86400);
            settings.displayOffAfterLockEnabled = settings.autoLockEnabled;
            settings.displayOffAfterLockDelaySeconds = 30;
        } else {
            settings.autoLockEnabled = object.GetNamedBoolean(L"autoLockEnabled", settings.autoLockEnabled);
            settings.autoLockTimeoutSeconds = json_int(
                object, L"lockTimeoutSeconds", settings.autoLockTimeoutSeconds, 30, 86400);
            settings.displayOffAfterLockEnabled = false;
        }
        settings.decodeMode = json_string(object, L"decodeMode");
        if (settings.decodeMode != "auto" && settings.decodeMode != "hardware" && settings.decodeMode != "software") settings.decodeMode = "auto";
        settings.performanceMode = json_string(object, L"performanceMode");
        if (settings.performanceMode != "balanced" && settings.performanceMode != "original" &&
            settings.performanceMode != "power-saver") settings.performanceMode = "balanced";
        if (storedVersion >= 9 && object.HasKey(L"mediaLibraryPath")) {
            auto configured = json_wstring(object, L"mediaLibraryPath");
            fs::path configuredPath(configured);
            if (!configured.empty()) {
                if (!configuredPath.is_absolute() || !configuredPath.has_filename()) return std::nullopt;
                configuredPath = configuredPath.lexically_normal();
                if (!configuredPath.is_absolute() || !configuredPath.has_filename()) return std::nullopt;
                settings.mediaLibraryPath = configuredPath.wstring();
                // Version 10 binds an external path to the durable ownership ID
                // stored inside that library.  A v9 external path deliberately
                // stays unavailable: this unpublished schema must not silently
                // bless whichever valid-looking library currently occupies the
                // same drive letter and directory.
                settings.mediaLibraryId = storedVersion >= 10
                    ? json_string(object, L"mediaLibraryId") : std::string{};
                auto identity = valid_id(settings.mediaLibraryId)
                    ? capture_media_library_trust(configuredPath)
                    : std::optional<MediaLibraryTrustIdentity>{};
                if (!identity || identity->ownershipId != settings.mediaLibraryId ||
                    !is_owned_media_library(identity->root) ||
                    !revalidate_media_library_trust(*identity)) {
                    if (!preserveUnavailableLibrary) return std::nullopt;
                    libraryUnavailable = true;
                }
            }
        }
        if (storedVersion >= 10 && settings.mediaLibraryPath.empty()) {
            settings.mediaLibraryId = json_string(object, L"mediaLibraryId");
            if (!settings.mediaLibraryId.empty()) return std::nullopt;
        }
        settings.selectedGroupId = json_string(object, L"selectedGroupId");
        settings.selectedMediaId = object.HasKey(L"selectedMediaId")
            ? json_string(object, L"selectedMediaId")
            : json_string(object, L"selectedVideoId");
        settings.randomGroupId = json_string(object, L"randomGroupId");
        settings.randomIntervalMinutes = json_int(object, L"randomIntervalMinutes", 0, 0, 1440);
        settings.startWithWindows = object.GetNamedBoolean(L"startWithWindows", false);
        settings.displayMode = json_string(object, L"displayMode");
        if (settings.displayMode == "span") settings.displayMode = "independent";
        if (settings.displayMode != "independent" && settings.displayMode != "primary") settings.displayMode = "independent";
        if (object.HasKey(L"displayAssignments")) {
            auto assignments = object.GetNamedArray(L"displayAssignments");
            for (auto const& value : assignments) {
                if (value.ValueType() != JsonValueType::Object) continue;
                auto assignmentObject = value.GetObject();
                DisplayAssignment assignment{
                    json_string(assignmentObject, L"displayId"),
                    json_string(assignmentObject, L"groupId"),
                    json_string(assignmentObject, L"mediaId")
                };
                if (!safe_display_id(assignment.displayId) || !valid_id(assignment.groupId) || !valid_id(assignment.mediaId)) continue;
                auto duplicate = std::find_if(settings.displayAssignments.begin(), settings.displayAssignments.end(),
                    [&](auto const& existing) { return existing.displayId == assignment.displayId; });
                if (duplicate == settings.displayAssignments.end()) settings.displayAssignments.push_back(std::move(assignment));
            }
        }
        return settings;
    }

    std::optional<Settings> load_settings(fs::path const& path)
    {
        bool libraryUnavailable{};
        return load_settings_document(path, false, libraryUnavailable);
    }

    bool try_load_settings(fs::path const& path, Settings& destination) noexcept
    {
        try {
            auto loaded = load_settings(path);
            if (!loaded) return false;
            destination = std::move(*loaded);
            return true;
        } catch (...) {
            return false;
        }
    }

    SettingsFileStatus load_settings_file(fs::path const& path, Settings& destination) noexcept
    {
        auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            auto error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                ? SettingsFileStatus::missing : SettingsFileStatus::invalid;
        }
        if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
            return SettingsFileStatus::invalid;
        }
        try {
            bool libraryUnavailable{};
            auto loaded = load_settings_document(path, true, libraryUnavailable);
            if (!loaded) return SettingsFileStatus::invalid;
            destination = std::move(*loaded);
            return libraryUnavailable
                ? SettingsFileStatus::libraryUnavailable : SettingsFileStatus::valid;
        } catch (...) {
            return SettingsFileStatus::invalid;
        }
    }

    void save_settings(fs::path const& path, Settings const& settings)
    {
        JsonObject object;
        object.Insert(L"version", JsonValue::CreateNumberValue(settings_schema_version));
        object.Insert(L"desktopPlayback", JsonValue::CreateBooleanValue(settings.desktopPlayback));
        object.Insert(L"activePlaybackEnabled", JsonValue::CreateBooleanValue(settings.activePlaybackEnabled));
        object.Insert(L"continueWhenCovered", JsonValue::CreateBooleanValue(settings.continueWhenCovered));
        object.Insert(L"screensaverEnabled", JsonValue::CreateBooleanValue(settings.screensaverEnabled));
        object.Insert(L"idleTimeoutSeconds", JsonValue::CreateNumberValue(settings.idleTimeoutSeconds));
        object.Insert(L"autoLockEnabled", JsonValue::CreateBooleanValue(settings.autoLockEnabled));
        object.Insert(L"autoLockTimeoutSeconds", JsonValue::CreateNumberValue(settings.autoLockTimeoutSeconds));
        object.Insert(L"displayOffAfterLockEnabled", JsonValue::CreateBooleanValue(settings.displayOffAfterLockEnabled));
        object.Insert(L"displayOffAfterLockDelaySeconds", JsonValue::CreateNumberValue(settings.displayOffAfterLockDelaySeconds));
        object.Insert(L"decodeMode", JsonValue::CreateStringValue(utf8_to_wide(settings.decodeMode)));
        object.Insert(L"performanceMode", JsonValue::CreateStringValue(utf8_to_wide(settings.performanceMode)));
        object.Insert(L"mediaLibraryPath", JsonValue::CreateStringValue(settings.mediaLibraryPath));
        object.Insert(L"mediaLibraryId", JsonValue::CreateStringValue(utf8_to_wide(settings.mediaLibraryId)));
        object.Insert(L"selectedGroupId", JsonValue::CreateStringValue(utf8_to_wide(settings.selectedGroupId)));
        object.Insert(L"selectedMediaId", JsonValue::CreateStringValue(utf8_to_wide(settings.selectedMediaId)));
        object.Insert(L"randomGroupId", JsonValue::CreateStringValue(utf8_to_wide(settings.randomGroupId)));
        object.Insert(L"randomIntervalMinutes", JsonValue::CreateNumberValue(settings.randomIntervalMinutes));
        object.Insert(L"startWithWindows", JsonValue::CreateBooleanValue(settings.startWithWindows));
        object.Insert(L"displayMode", JsonValue::CreateStringValue(utf8_to_wide(settings.displayMode)));
        JsonArray assignments;
        for (auto const& assignment : settings.displayAssignments) {
            if (!safe_display_id(assignment.displayId) || !valid_id(assignment.groupId) || !valid_id(assignment.mediaId)) continue;
            JsonObject value;
            value.Insert(L"displayId", JsonValue::CreateStringValue(utf8_to_wide(assignment.displayId)));
            value.Insert(L"groupId", JsonValue::CreateStringValue(utf8_to_wide(assignment.groupId)));
            value.Insert(L"mediaId", JsonValue::CreateStringValue(utf8_to_wide(assignment.mediaId)));
            assignments.Append(value);
        }
        object.Insert(L"displayAssignments", assignments);
        write_text_atomic(path, object.Stringify().c_str());
    }

    std::optional<RuntimeState> load_runtime(fs::path const& path)
    {
        if (!fs::is_regular_file(path)) return std::nullopt;
        auto object = parse_object(path);
        RuntimeState runtime;
        auto version = json_int(object, L"version", runtime_schema_version, 0, runtime_schema_version + 1);
        if (version != runtime_schema_version) return std::nullopt;
        runtime.activeGroupId = json_string(object, L"activeGroupId");
        runtime.activeMediaId = json_string(object, L"activeMediaId");
        runtime.decodePath = json_string(object, L"decodePath");
        runtime.decodeReason = json_string(object, L"decodeReason");
        runtime.updatedAt = json_wstring(object, L"updatedAt");
        if ((!runtime.activeGroupId.empty() && !valid_id(runtime.activeGroupId)) ||
            (!runtime.activeMediaId.empty() && !valid_id(runtime.activeMediaId))) return std::nullopt;
        if (runtime.activeGroupId.empty() != runtime.activeMediaId.empty()) return std::nullopt;
        if (runtime.decodePath != "" && runtime.decodePath != "probing" &&
            runtime.decodePath != "automatic" && runtime.decodePath != "hardware" && runtime.decodePath != "software" &&
            runtime.decodePath != "software-fallback" && runtime.decodePath != "unavailable" &&
            runtime.decodePath != "not-applicable") return std::nullopt;
        return runtime;
    }

    bool try_load_runtime(fs::path const& path, RuntimeState& destination) noexcept
    {
        try {
            auto loaded = load_runtime(path);
            if (!loaded) return false;
            destination = std::move(*loaded);
            return true;
        } catch (...) {
            return false;
        }
    }

    void save_runtime(fs::path const& path, RuntimeState const& runtime)
    {
        if ((!runtime.activeGroupId.empty() && !valid_id(runtime.activeGroupId)) ||
            (!runtime.activeMediaId.empty() && !valid_id(runtime.activeMediaId)) ||
            runtime.activeGroupId.empty() != runtime.activeMediaId.empty()) {
            throw std::runtime_error("invalid runtime state");
        }
        JsonObject object;
        object.Insert(L"version", JsonValue::CreateNumberValue(runtime_schema_version));
        object.Insert(L"activeGroupId", JsonValue::CreateStringValue(utf8_to_wide(runtime.activeGroupId)));
        object.Insert(L"activeMediaId", JsonValue::CreateStringValue(utf8_to_wide(runtime.activeMediaId)));
        object.Insert(L"decodePath", JsonValue::CreateStringValue(utf8_to_wide(runtime.decodePath)));
        object.Insert(L"decodeReason", JsonValue::CreateStringValue(utf8_to_wide(runtime.decodeReason)));
        object.Insert(L"updatedAt", JsonValue::CreateStringValue(runtime.updatedAt));
        write_text_atomic(path, object.Stringify().c_str());
    }

    std::optional<GroupMetadata> load_group(fs::path const& path)
    {
        if (!fs::is_regular_file(path)) return std::nullopt;
        auto object = parse_object(path);
        GroupMetadata group;
        group.version = json_int(object, L"version", group_schema_version, 0, group_schema_version + 1);
        group.id = json_string(object, L"id");
        group.name = json_wstring(object, L"name");
        group.order = json_int(object, L"order", 0, 0, 1'000'000);
        group.createdAt = json_wstring(object, L"createdAt");
        group.updatedAt = json_wstring(object, L"updatedAt");
        if (group.version != group_schema_version || !valid_id(group.id) || group.name.empty()) return std::nullopt;
        return group;
    }

    void save_group(fs::path const& path, GroupMetadata const& group)
    {
        JsonObject object;
        object.Insert(L"version", JsonValue::CreateNumberValue(group.version));
        object.Insert(L"id", JsonValue::CreateStringValue(utf8_to_wide(group.id)));
        object.Insert(L"name", JsonValue::CreateStringValue(group.name));
        object.Insert(L"order", JsonValue::CreateNumberValue(group.order));
        object.Insert(L"createdAt", JsonValue::CreateStringValue(group.createdAt));
        object.Insert(L"updatedAt", JsonValue::CreateStringValue(group.updatedAt));
        write_text_atomic(path, object.Stringify().c_str());
    }

    std::optional<MediaMetadata> load_media(fs::path const& path)
    {
        if (!fs::is_regular_file(path)) return std::nullopt;
        auto object = parse_object(path);
        MediaMetadata media;
        media.version = json_int(object, L"version", media_schema_version, 0, media_schema_version + 1);
        media.id = json_string(object, L"id");
        media.groupId = json_string(object, L"groupId");
        media.name = json_wstring(object, L"name");
        media.originalName = json_wstring(object, L"originalName");
        media.fileName = json_wstring(object, L"fileName");
        media.kind = json_string(object, L"kind");
        if (media.kind.empty()) media.kind = "video";
        media.coverFileName = json_wstring(object, L"coverFileName");
        media.sha256 = json_wstring(object, L"sha256");
        media.sizeBytes = json_uint64(object, L"sizeBytes");
        media.revision = json_uint64(object, L"revision");
        media.importedAt = json_wstring(object, L"importedAt");
        media.updatedAt = json_wstring(object, L"updatedAt");
        if (media.version != media_schema_version || !valid_id(media.id) || !valid_id(media.groupId) || !safe_file_name(media.fileName) ||
            (!media.coverFileName.empty() && !safe_file_name(media.coverFileName)) ||
            (media.kind != "video" && media.kind != "image")) return std::nullopt;
        return media;
    }

    bool try_load_media(fs::path const& path, MediaMetadata& destination) noexcept
    {
        try {
            auto loaded = load_media(path);
            if (!loaded) return false;
            destination = std::move(*loaded);
            return true;
        } catch (...) {
            return false;
        }
    }

    void save_media(fs::path const& path, MediaMetadata const& media)
    {
        if (!valid_id(media.id) || !valid_id(media.groupId) || !safe_file_name(media.fileName) ||
            (!media.coverFileName.empty() && !safe_file_name(media.coverFileName)) ||
            (media.kind != "video" && media.kind != "image")) {
            throw std::runtime_error("invalid media metadata");
        }
        JsonObject object;
        object.Insert(L"version", JsonValue::CreateNumberValue(media.version));
        object.Insert(L"id", JsonValue::CreateStringValue(utf8_to_wide(media.id)));
        object.Insert(L"groupId", JsonValue::CreateStringValue(utf8_to_wide(media.groupId)));
        object.Insert(L"name", JsonValue::CreateStringValue(media.name));
        object.Insert(L"kind", JsonValue::CreateStringValue(utf8_to_wide(media.kind)));
        object.Insert(L"originalName", JsonValue::CreateStringValue(media.originalName));
        object.Insert(L"fileName", JsonValue::CreateStringValue(media.fileName));
        object.Insert(L"coverFileName", JsonValue::CreateStringValue(media.coverFileName));
        object.Insert(L"sha256", JsonValue::CreateStringValue(media.sha256));
        object.Insert(L"sizeBytes", JsonValue::CreateNumberValue(static_cast<double>(media.sizeBytes)));
        object.Insert(L"revision", JsonValue::CreateNumberValue(static_cast<double>(media.revision)));
        object.Insert(L"importedAt", JsonValue::CreateStringValue(media.importedAt));
        object.Insert(L"updatedAt", JsonValue::CreateStringValue(media.updatedAt));
        write_text_atomic(path, object.Stringify().c_str());
    }

    DesktopIntent desktop_intent(Settings const& settings, bool covered, bool hasMedia)
    {
        if (!settings.desktopPlayback || !hasMedia) return DesktopIntent::Off;
        if (covered && !settings.continueWhenCovered) return DesktopIntent::Pause;
        return settings.activePlaybackEnabled ? DesktopIntent::Play : DesktopIntent::Freeze;
    }

    bool notify_settings_changed()
    {
        unique_handle event(OpenEventW(EVENT_MODIFY_STATE, FALSE, settings_event_name));
        return event && SetEvent(event.get());
    }
}
