#include <windows.h>
#include <bcrypt.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "LibraryBackup.h"
#include "LibraryMigration.h"

namespace fs = std::filesystem;
using namespace winrt::Windows::Data::Json;

namespace
{
    constexpr std::string_view backupFormat = "MotionWallpaper.Backup";
    constexpr std::string_view backupOwnerMagic = "MotionWallpaper.BackupOwner/v1\n";
    constexpr std::string_view backupCompleteMagic = "MotionWallpaper.BackupComplete/v1\n";
    constexpr wchar_t backupOwnerName[] = L".motionwallpaper-backup";
    constexpr wchar_t backupManifestName[] = L"manifest.json";
    constexpr wchar_t backupCompleteName[] = L".motionwallpaper-backup-complete";
    constexpr wchar_t backupStagePrefix[] = L".motionwallpaper-backup-stage-";
    constexpr wchar_t restoreStagePrefix[] = L".motionwallpaper-restore-stage-";
    constexpr wchar_t restoreJournalName[] = L".motionwallpaper-restore-journal.json";
    constexpr wchar_t restoreSettingsPrefix[] = L".motionwallpaper-settings-restore-";
    constexpr wchar_t restorePreviousSettingsPrefix[] = L"settings.before-restore-";
    // Keep the activated path compact. Settings validation still uses some
    // Win32 APIs without an extended-length prefix, while a library contains
    // two nested UUID directories. The full 128-bit transaction id remains in
    // the name; only its separators and the decorative prefix are shortened.
    constexpr wchar_t restoredLibraryPrefix[] = L"R-";
    constexpr std::string_view restoreJournalFormat = "MotionWallpaper.RestoreJournal";
    constexpr int restoreJournalVersion = 1;
    constexpr size_t maximumRestoreJournalBytes = 64 * 1024;
    constexpr uint32_t maximumManifestEntries = 500'000;
    constexpr size_t maximumManifestBytes = 128ULL * 1024 * 1024;

    fs::path restored_library_name(std::string_view transactionId)
    {
        std::wstring result(restoredLibraryPrefix);
        result.reserve(result.size() + transactionId.size());
        for (auto value : transactionId) {
            if (value != '-') result.push_back(static_cast<wchar_t>(value));
        }
        return fs::path(std::move(result));
    }

    using Digest = std::array<uint8_t, 32>;

    struct ManifestEntry
    {
        fs::path relative;
        bool directory{};
        uint64_t bytes{};
        Digest digest{};

        bool operator==(ManifestEntry const&) const = default;
    };

    struct BackupManifest
    {
        int version{ motion::app::library_backup_format_version };
        std::string backupId;
        std::wstring createdAt;
        std::string libraryId;
        uint64_t totalBytes{};
        uint32_t fileCount{};
        std::vector<ManifestEntry> entries;
    };

    enum class RestoreJournalPhase
    {
        Prepared,
        PreviousLibraryArchived,
        LibraryActivated,
        SettingsCommitPending,
        SettingsCommitted
    };

    struct RestoreJournal
    {
        std::string transactionId;
        std::string backupId;
        RestoreJournalPhase phase{ RestoreJournalPhase::Prepared };
        bool replacesExistingLibrary{};
        fs::path activeLibraryPath;
        fs::path stagedLibraryPath;
        fs::path archivedLibraryPath;
        fs::path preparedSettingsPath;
        fs::path previousSettingsPath;
        std::string previousLibraryId;
        motion::FilesystemObjectIdentity previousLibraryIdentity{};
        std::string restoredLibraryId;
        motion::FilesystemObjectIdentity restoredLibraryIdentity{};
        Digest restoredSettingsDigest{};
    };

    class simulated_restore_crash final : public std::runtime_error
    {
    public:
        simulated_restore_crash() : std::runtime_error("simulated restore process crash") {}
    };

    void inject_restore_crash(motion::app::LibraryRestoreCrashPoint selected,
        motion::app::LibraryRestoreCrashPoint current)
    {
        if (selected == current) throw simulated_restore_crash{};
    }

    class bcrypt_algorithm final
    {
    public:
        ~bcrypt_algorithm() { if (value_) BCryptCloseAlgorithmProvider(value_, 0); }
        BCRYPT_ALG_HANDLE* put() noexcept { return &value_; }
        BCRYPT_ALG_HANDLE get() const noexcept { return value_; }
    private:
        BCRYPT_ALG_HANDLE value_{};
    };

    class bcrypt_hash final
    {
    public:
        ~bcrypt_hash() { if (value_) BCryptDestroyHash(value_); }
        BCRYPT_HASH_HANDLE* put() noexcept { return &value_; }
        BCRYPT_HASH_HANDLE get() const noexcept { return value_; }
    private:
        BCRYPT_HASH_HANDLE value_{};
    };

    void require_nt(NTSTATUS status, char const* message)
    {
        if (!BCRYPT_SUCCESS(status)) throw std::runtime_error(message);
    }

    void throw_cancelled(std::atomic_bool const* cancelled)
    {
        if (cancelled && cancelled->load(std::memory_order_acquire)) {
            throw std::runtime_error("library backup operation cancelled");
        }
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

    std::optional<motion::FilesystemObjectIdentity> direct_handle_identity(
        HANDLE handle, bool directory) noexcept
    {
        if (!handle || handle == INVALID_HANDLE_VALUE) return std::nullopt;
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(handle, &information) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            (((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory)) {
            return std::nullopt;
        }
        motion::FilesystemObjectIdentity identity{};
        FILE_ID_INFO fileId{};
        if (GetFileInformationByHandleEx(handle, FileIdInfo, &fileId, sizeof(fileId))) {
            identity.volumeSerialNumber = fileId.VolumeSerialNumber;
            std::copy(std::begin(fileId.FileId.Identifier),
                std::end(fileId.FileId.Identifier), identity.fileId.begin());
        } else {
            identity.volumeSerialNumber = information.dwVolumeSerialNumber;
            uint64_t fallback =
                (static_cast<uint64_t>(information.nFileIndexHigh) << 32) |
                information.nFileIndexLow;
            std::memcpy(identity.fileId.data(), &fallback, sizeof(fallback));
        }
        return identity;
    }

    motion::unique_handle open_direct_directory(
        fs::path const& path, bool protectIdentity) noexcept
    {
        DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE |
            (protectIdentity ? 0 : FILE_SHARE_DELETE);
        motion::unique_handle result(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            share, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!result || !direct_handle_identity(result.get(), true)) return {};
        return result;
    }

    std::optional<fs::path> stable_directory_path(HANDLE handle) noexcept
    {
        try {
            std::wstring value(512, L'\0');
            DWORD length{};
            for (;;) {
                length = GetFinalPathNameByHandleW(handle, value.data(),
                    static_cast<DWORD>(value.size()),
                    FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
                if (!length || length >= 32768) return std::nullopt;
                if (length < value.size()) break;
                value.resize(static_cast<size_t>(length) + 1);
            }
            value.resize(length);
            constexpr std::wstring_view prefix = LR"(\\?\Volume{)";
            if (value.size() <= prefix.size() ||
                CompareStringOrdinal(value.data(), static_cast<int>(prefix.size()),
                    prefix.data(), static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL ||
                value.find(L"}\\", prefix.size()) == std::wstring::npos) {
                return std::nullopt;
            }
            fs::path result(value);
            return result.is_absolute() && result.has_filename()
                ? std::optional<fs::path>(std::move(result)) : std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    }

    struct StableDirectory
    {
        fs::path configuredPath;
        fs::path stablePath;
        motion::FilesystemObjectIdentity identity;
        motion::unique_handle configuredHandle;
        motion::unique_handle stableHandle;

        bool Revalidate() const noexcept
        {
            auto configuredIdentity = direct_handle_identity(configuredHandle.get(), true);
            auto stableIdentity = direct_handle_identity(stableHandle.get(), true);
            auto configuredNow = open_direct_directory(configuredPath, false);
            auto configuredNowIdentity = direct_handle_identity(configuredNow.get(), true);
            auto stableNow = open_direct_directory(stablePath, false);
            auto stableNowIdentity = direct_handle_identity(stableNow.get(), true);
            return configuredIdentity && stableIdentity && configuredNowIdentity &&
                stableNowIdentity && *configuredIdentity == identity &&
                *stableIdentity == identity && *configuredNowIdentity == identity &&
                *stableNowIdentity == identity &&
                motion::same_direct_filesystem_object(configuredPath, stablePath);
        }
    };

    std::optional<StableDirectory> capture_stable_directory(
        fs::path configuredPath) noexcept
    {
        try {
            configuredPath = fs::absolute(configuredPath).lexically_normal();
            auto configured = open_direct_directory(configuredPath, true);
            auto identity = direct_handle_identity(configured.get(), true);
            auto stablePath = stable_directory_path(configured.get());
            if (!configured || !identity || !stablePath) return std::nullopt;
            auto stable = open_direct_directory(*stablePath, true);
            auto stableIdentity = direct_handle_identity(stable.get(), true);
            if (!stable || !stableIdentity || *stableIdentity != *identity) {
                return std::nullopt;
            }
            StableDirectory result{ std::move(configuredPath), std::move(*stablePath),
                *identity, std::move(configured), std::move(stable) };
            return result.Revalidate()
                ? std::optional<StableDirectory>(std::move(result)) : std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    }

    bool safe_relative_path(fs::path const& relative) noexcept
    {
        try {
            if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
                relative.has_root_directory() || relative.native().size() > 32700) {
                return false;
            }
            size_t count{};
            for (auto const& component : relative) {
                if (!motion::safe_file_name(component) ||
                    component.native().find(L':') != std::wstring::npos) {
                    return false;
                }
                ++count;
            }
            if (!count) return false;
            auto first = *relative.begin();
            return first == L"Config" || first == L"Library";
        } catch (...) {
            return false;
        }
    }

    std::string hex_digest(Digest const& digest)
    {
        constexpr char digits[] = "0123456789abcdef";
        std::string result;
        result.reserve(digest.size() * 2);
        for (auto byte : digest) {
            result.push_back(digits[byte >> 4]);
            result.push_back(digits[byte & 0x0f]);
        }
        return result;
    }

    std::optional<Digest> parse_digest(std::string_view value) noexcept
    {
        if (value.size() != 64) return std::nullopt;
        auto nibble = [](char character) -> int {
            if (character >= '0' && character <= '9') return character - '0';
            if (character >= 'a' && character <= 'f') return character - 'a' + 10;
            return -1;
        };
        Digest result{};
        for (size_t index = 0; index < result.size(); ++index) {
            int high = nibble(value[index * 2]);
            int low = nibble(value[index * 2 + 1]);
            if (high < 0 || low < 0) return std::nullopt;
            result[index] = static_cast<uint8_t>((high << 4) | low);
        }
        return result;
    }

    std::optional<uint64_t> parse_uint64(std::string_view value) noexcept
    {
        if (value.empty()) return std::nullopt;
        uint64_t result{};
        auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
        return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()
            ? std::optional<uint64_t>(result) : std::nullopt;
    }

    std::string hex_file_id(std::array<uint8_t, 16> const& value)
    {
        constexpr char digits[] = "0123456789abcdef";
        std::string result;
        result.reserve(value.size() * 2);
        for (auto byte : value) {
            result.push_back(digits[byte >> 4]);
            result.push_back(digits[byte & 0x0f]);
        }
        return result;
    }

    std::optional<std::array<uint8_t, 16>> parse_file_id(
        std::string_view value) noexcept
    {
        if (value.size() != 32) return std::nullopt;
        auto nibble = [](char character) -> int {
            if (character >= '0' && character <= '9') return character - '0';
            if (character >= 'a' && character <= 'f') return character - 'a' + 10;
            return -1;
        };
        std::array<uint8_t, 16> result{};
        for (size_t index = 0; index < result.size(); ++index) {
            int high = nibble(value[index * 2]);
            int low = nibble(value[index * 2 + 1]);
            if (high < 0 || low < 0) return std::nullopt;
            result[index] = static_cast<uint8_t>((high << 4) | low);
        }
        return result;
    }

    std::wstring restore_phase_name(RestoreJournalPhase phase)
    {
        switch (phase) {
        case RestoreJournalPhase::Prepared: return L"prepared";
        case RestoreJournalPhase::PreviousLibraryArchived: return L"previous-library-archived";
        case RestoreJournalPhase::LibraryActivated: return L"library-activated";
        case RestoreJournalPhase::SettingsCommitPending: return L"settings-commit-pending";
        case RestoreJournalPhase::SettingsCommitted: return L"settings-committed";
        }
        throw std::runtime_error("invalid restore journal phase");
    }

    std::optional<RestoreJournalPhase> parse_restore_phase(
        std::wstring_view value) noexcept
    {
        if (value == L"prepared") return RestoreJournalPhase::Prepared;
        if (value == L"previous-library-archived") {
            return RestoreJournalPhase::PreviousLibraryArchived;
        }
        if (value == L"library-activated") return RestoreJournalPhase::LibraryActivated;
        if (value == L"settings-commit-pending") {
            return RestoreJournalPhase::SettingsCommitPending;
        }
        if (value == L"settings-committed") return RestoreJournalPhase::SettingsCommitted;
        return std::nullopt;
    }

    void insert_identity(JsonObject& object, wchar_t const* prefix,
        motion::FilesystemObjectIdentity const& identity)
    {
        auto volumeName = std::wstring(prefix) + L"Volume";
        auto fileName = std::wstring(prefix) + L"FileId";
        object.Insert(volumeName, JsonValue::CreateStringValue(
            motion::utf8_to_wide(std::to_string(identity.volumeSerialNumber))));
        object.Insert(fileName, JsonValue::CreateStringValue(
            motion::utf8_to_wide(hex_file_id(identity.fileId))));
    }

    motion::FilesystemObjectIdentity parse_identity(JsonObject const& object,
        wchar_t const* prefix)
    {
        auto volumeName = std::wstring(prefix) + L"Volume";
        auto fileName = std::wstring(prefix) + L"FileId";
        auto volume = parse_uint64(motion::wide_to_utf8(
            object.GetNamedString(volumeName).c_str()));
        auto fileId = parse_file_id(motion::wide_to_utf8(
            object.GetNamedString(fileName).c_str()));
        if (!volume || !fileId) {
            throw std::runtime_error("restore journal contains an invalid object identity");
        }
        return motion::FilesystemObjectIdentity{ *volume, *fileId };
    }

    std::string serialize_restore_journal(RestoreJournal const& journal)
    {
        JsonObject object;
        object.Insert(L"format", JsonValue::CreateStringValue(
            motion::utf8_to_wide(std::string(restoreJournalFormat))));
        object.Insert(L"version", JsonValue::CreateNumberValue(restoreJournalVersion));
        object.Insert(L"transactionId", JsonValue::CreateStringValue(
            motion::utf8_to_wide(journal.transactionId)));
        object.Insert(L"backupId", JsonValue::CreateStringValue(
            motion::utf8_to_wide(journal.backupId)));
        object.Insert(L"phase", JsonValue::CreateStringValue(
            restore_phase_name(journal.phase)));
        object.Insert(L"replacesExistingLibrary", JsonValue::CreateBooleanValue(
            journal.replacesExistingLibrary));
        object.Insert(L"activeLibraryPath", JsonValue::CreateStringValue(
            journal.activeLibraryPath.wstring()));
        object.Insert(L"stagedLibraryPath", JsonValue::CreateStringValue(
            journal.stagedLibraryPath.wstring()));
        object.Insert(L"archivedLibraryPath", JsonValue::CreateStringValue(
            journal.archivedLibraryPath.wstring()));
        object.Insert(L"preparedSettingsPath", JsonValue::CreateStringValue(
            journal.preparedSettingsPath.wstring()));
        object.Insert(L"previousSettingsPath", JsonValue::CreateStringValue(
            journal.previousSettingsPath.wstring()));
        object.Insert(L"previousLibraryId", JsonValue::CreateStringValue(
            motion::utf8_to_wide(journal.previousLibraryId)));
        insert_identity(object, L"previousLibrary", journal.previousLibraryIdentity);
        object.Insert(L"restoredLibraryId", JsonValue::CreateStringValue(
            motion::utf8_to_wide(journal.restoredLibraryId)));
        insert_identity(object, L"restoredLibrary", journal.restoredLibraryIdentity);
        object.Insert(L"restoredSettingsSha256", JsonValue::CreateStringValue(
            motion::utf8_to_wide(hex_digest(journal.restoredSettingsDigest))));
        return motion::wide_to_utf8(object.Stringify().c_str());
    }

    RestoreJournal parse_restore_journal(std::string const& value)
    {
        if (value.empty() || value.size() > maximumRestoreJournalBytes) {
            throw std::runtime_error("restore journal is empty or too large");
        }
        auto object = JsonObject::Parse(motion::utf8_to_wide(value));
        if (object.GetNamedString(L"format") !=
                motion::utf8_to_wide(std::string(restoreJournalFormat)) ||
            object.GetNamedNumber(L"version") != restoreJournalVersion) {
            throw std::runtime_error("restore journal format is unsupported");
        }
        RestoreJournal result;
        result.transactionId = motion::wide_to_utf8(
            object.GetNamedString(L"transactionId").c_str());
        result.backupId = motion::wide_to_utf8(
            object.GetNamedString(L"backupId").c_str());
        auto phase = parse_restore_phase(
            object.GetNamedString(L"phase").c_str());
        if (!motion::valid_id(result.transactionId) ||
            !motion::valid_id(result.backupId) || !phase) {
            throw std::runtime_error("restore journal identifiers are invalid");
        }
        result.phase = *phase;
        result.replacesExistingLibrary =
            object.GetNamedBoolean(L"replacesExistingLibrary");
        result.activeLibraryPath = fs::path(
            object.GetNamedString(L"activeLibraryPath").c_str()).lexically_normal();
        result.stagedLibraryPath = fs::path(
            object.GetNamedString(L"stagedLibraryPath").c_str()).lexically_normal();
        result.archivedLibraryPath = fs::path(
            object.GetNamedString(L"archivedLibraryPath").c_str()).lexically_normal();
        result.preparedSettingsPath = fs::path(
            object.GetNamedString(L"preparedSettingsPath").c_str()).lexically_normal();
        result.previousSettingsPath = fs::path(
            object.GetNamedString(L"previousSettingsPath").c_str()).lexically_normal();
        result.previousLibraryId = motion::wide_to_utf8(
            object.GetNamedString(L"previousLibraryId").c_str());
        result.previousLibraryIdentity = parse_identity(object, L"previousLibrary");
        result.restoredLibraryId = motion::wide_to_utf8(
            object.GetNamedString(L"restoredLibraryId").c_str());
        result.restoredLibraryIdentity = parse_identity(object, L"restoredLibrary");
        auto digest = parse_digest(motion::wide_to_utf8(
            object.GetNamedString(L"restoredSettingsSha256").c_str()));
        if (!motion::valid_id(result.restoredLibraryId) || !digest ||
            (result.replacesExistingLibrary &&
                !motion::valid_id(result.previousLibraryId)) ||
            (!result.replacesExistingLibrary &&
                !result.previousLibraryId.empty())) {
            throw std::runtime_error("restore journal payload is invalid");
        }
        result.restoredSettingsDigest = *digest;
        return result;
    }

    template<typename ChunkCallback>
    Digest hash_handle(HANDLE file, ChunkCallback&& onChunk)
    {
        bcrypt_algorithm algorithm;
        require_nt(BCryptOpenAlgorithmProvider(algorithm.put(), BCRYPT_SHA256_ALGORITHM,
            nullptr, 0), "cannot open SHA-256 provider");
        DWORD objectBytes{};
        DWORD returned{};
        require_nt(BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &returned, 0),
            "cannot read SHA-256 object length");
        std::vector<unsigned char> object(objectBytes);
        bcrypt_hash hash;
        require_nt(BCryptCreateHash(algorithm.get(), hash.put(), object.data(), objectBytes,
            nullptr, 0, 0), "cannot create SHA-256 hash");
        // Keep the streaming buffer off the worker thread's comparatively small
        // Windows stack. A 1 MiB local array can exhaust the default stack before
        // the first payload byte is hashed.
        std::vector<unsigned char> buffer(1024 * 1024);
        for (;;) {
            DWORD read{};
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                &read, nullptr)) {
                throw std::system_error(static_cast<int>(GetLastError()),
                    std::system_category());
            }
            if (!read) break;
            require_nt(BCryptHashData(hash.get(), buffer.data(), read, 0),
                "cannot update SHA-256 hash");
            onChunk(read);
        }
        Digest result{};
        require_nt(BCryptFinishHash(hash.get(), result.data(),
            static_cast<ULONG>(result.size()), 0), "cannot finish SHA-256 hash");
        return result;
    }

    Digest hash_file(fs::path const& path,
        std::function<void(uint64_t)> const& onChunk = {})
    {
        motion::unique_handle input(CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!input || !direct_handle_identity(input.get(), false)) {
            throw std::runtime_error("backup payload contains an unsafe file");
        }
        return hash_handle(input.get(), [&](uint64_t count) {
            if (onChunk) onChunk(count);
        });
    }

    Digest hash_bytes(std::string_view value)
    {
        bcrypt_algorithm algorithm;
        require_nt(BCryptOpenAlgorithmProvider(algorithm.put(), BCRYPT_SHA256_ALGORITHM,
            nullptr, 0), "cannot open SHA-256 provider");
        DWORD objectBytes{};
        DWORD returned{};
        require_nt(BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &returned, 0),
            "cannot read SHA-256 object length");
        std::vector<unsigned char> object(objectBytes);
        bcrypt_hash hash;
        require_nt(BCryptCreateHash(algorithm.get(), hash.put(), object.data(), objectBytes,
            nullptr, 0, 0), "cannot create SHA-256 hash");
        if (!value.empty()) {
            if (value.size() > (std::numeric_limits<ULONG>::max)()) {
                throw std::runtime_error("backup manifest is too large");
            }
            require_nt(BCryptHashData(hash.get(),
                reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
                static_cast<ULONG>(value.size()), 0), "cannot hash backup manifest");
        }
        Digest result{};
        require_nt(BCryptFinishHash(hash.get(), result.data(),
            static_cast<ULONG>(result.size()), 0), "cannot finish SHA-256 hash");
        return result;
    }

    std::string read_control_file(fs::path const& path, size_t maximumBytes)
    {
        motion::unique_handle input(CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        BY_HANDLE_FILE_INFORMATION information{};
        if (!input || !GetFileInformationByHandle(input.get(), &information) ||
            (information.dwFileAttributes &
                (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            throw std::runtime_error("invalid backup control file");
        }
        ULARGE_INTEGER size{};
        size.HighPart = information.nFileSizeHigh;
        size.LowPart = information.nFileSizeLow;
        if (size.QuadPart > maximumBytes) {
            throw std::runtime_error("backup control file is too large");
        }
        std::string value;
        value.reserve(static_cast<size_t>(size.QuadPart));
        std::array<char, 64 * 1024> buffer{};
        for (;;) {
            DWORD read{};
            if (!ReadFile(input.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                &read, nullptr)) {
                throw std::system_error(static_cast<int>(GetLastError()),
                    std::system_category());
            }
            if (!read) break;
            if (value.size() > maximumBytes - read) {
                throw std::runtime_error("backup control file is too large");
            }
            value.append(buffer.data(), read);
        }
        return value;
    }

    void write_new_file(fs::path const& path, std::string_view value, DWORD attributes)
    {
        motion::unique_handle output(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, attributes, nullptr));
        if (!output) {
            throw std::system_error(static_cast<int>(GetLastError()),
                std::system_category());
        }
        size_t offset{};
        try {
            while (offset < value.size()) {
                auto requested = static_cast<DWORD>((std::min)(value.size() - offset,
                    static_cast<size_t>(MAXDWORD)));
                DWORD written{};
                if (!WriteFile(output.get(), value.data() + offset, requested,
                    &written, nullptr) || written != requested) {
                    throw std::system_error(static_cast<int>(
                        GetLastError() ? GetLastError() : ERROR_WRITE_FAULT),
                        std::system_category());
                }
                offset += written;
            }
            if (!FlushFileBuffers(output.get())) {
                throw std::system_error(static_cast<int>(GetLastError()),
                    std::system_category());
            }
        } catch (...) {
            output.reset();
            std::error_code ignored;
            fs::remove(path, ignored);
            throw;
        }
    }

    fs::path restore_journal_path(fs::path const& dataRoot)
    {
        return fs::absolute(dataRoot / L"Config" / restoreJournalName).lexically_normal();
    }

    bool safe_absolute_restore_path(fs::path const& path) noexcept
    {
        try {
            return path.is_absolute() && path.has_filename() &&
                path.native().size() < 32700;
        } catch (...) {
            return false;
        }
    }

    void validate_restore_journal_paths(fs::path const& dataRoot,
        RestoreJournal const& journal)
    {
        auto normalizedRoot = fs::absolute(dataRoot).lexically_normal();
        auto config = normalizedRoot / L"Config";
        auto transaction = motion::utf8_to_wide(journal.transactionId);
        auto expectedStage = journal.activeLibraryPath.parent_path() /
            (restoreStagePrefix + transaction);
        auto expectedPrepared = config /
            (restoreSettingsPrefix + transaction + L".json");
        auto expectedPreviousSettings = config /
            (restorePreviousSettingsPrefix + transaction + L".json");
        if (!safe_absolute_restore_path(journal.activeLibraryPath) ||
            !safe_absolute_restore_path(journal.stagedLibraryPath) ||
            !safe_absolute_restore_path(journal.preparedSettingsPath) ||
            !safe_absolute_restore_path(journal.previousSettingsPath) ||
            !motion::same_filesystem_path(journal.stagedLibraryPath, expectedStage) ||
            !motion::same_filesystem_path(journal.preparedSettingsPath, expectedPrepared) ||
            !motion::same_filesystem_path(journal.previousSettingsPath,
                expectedPreviousSettings)) {
            throw std::runtime_error("restore journal paths are not derived safely");
        }
        if (journal.replacesExistingLibrary) {
            auto archiveName = journal.activeLibraryPath.filename().wstring() +
                L".MotionWallpaper-before-restore-" +
                motion::utf8_to_wide(journal.backupId) + L"-" + transaction;
            auto expectedArchive = journal.activeLibraryPath.parent_path() / archiveName;
            if (!safe_absolute_restore_path(journal.archivedLibraryPath) ||
                !motion::same_filesystem_path(journal.archivedLibraryPath,
                    expectedArchive)) {
                throw std::runtime_error("restore journal archive path is unsafe");
            }
        } else {
            auto expectedTarget = normalizedRoot /
                restored_library_name(journal.transactionId);
            if (!journal.archivedLibraryPath.empty() ||
                !motion::same_filesystem_path(journal.activeLibraryPath,
                    expectedTarget)) {
                throw std::runtime_error("offline restore target is not application-local");
            }
        }
    }

    RestoreJournal read_restore_journal(fs::path const& dataRoot)
    {
        auto path = restore_journal_path(dataRoot);
        if (!direct_regular_file_no_reparse(path)) {
            throw std::runtime_error("pending restore journal is unsafe");
        }
        auto result = parse_restore_journal(
            read_control_file(path, maximumRestoreJournalBytes));
        validate_restore_journal_paths(dataRoot, result);
        return result;
    }

    void persist_restore_journal(fs::path const& dataRoot,
        RestoreJournal const& journal, bool initial)
    {
        validate_restore_journal_paths(dataRoot, journal);
        auto config = capture_stable_directory(
            fs::absolute(dataRoot / L"Config").lexically_normal());
        if (!config) throw std::runtime_error("Config directory identity is unsafe");
        auto journalPath = config->stablePath / restoreJournalName;
        if (initial) {
            auto attributes = GetFileAttributesW(journalPath.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES ||
                GetLastError() != ERROR_FILE_NOT_FOUND) {
                throw std::runtime_error("another restore transaction is pending");
            }
        } else if (!direct_regular_file_no_reparse(journalPath)) {
            throw std::runtime_error("pending restore journal changed unexpectedly");
        }
        auto temporary = config->stablePath /
            (std::wstring(restoreJournalName) + L".next-" +
                motion::utf8_to_wide(motion::new_id()));
        auto bytes = serialize_restore_journal(journal);
        try {
            write_new_file(temporary, bytes,
                FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
            BOOL committed = initial
                ? MoveFileExW(temporary.c_str(), journalPath.c_str(), MOVEFILE_WRITE_THROUGH)
                : ReplaceFileW(journalPath.c_str(), temporary.c_str(), nullptr,
                    REPLACEFILE_WRITE_THROUGH | REPLACEFILE_IGNORE_MERGE_ERRORS |
                        REPLACEFILE_IGNORE_ACL_ERRORS,
                    nullptr, nullptr);
            if (!committed) {
                throw std::system_error(static_cast<int>(GetLastError()),
                    std::system_category(), "cannot durably update restore journal");
            }
        } catch (...) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            throw;
        }
        auto persisted = read_restore_journal(dataRoot);
        if (persisted.transactionId != journal.transactionId ||
            persisted.phase != journal.phase) {
            throw std::runtime_error("restore journal verification failed");
        }
    }

    void remove_restore_journal(fs::path const& dataRoot,
        std::string const& transactionId)
    {
        auto persisted = read_restore_journal(dataRoot);
        if (persisted.transactionId != transactionId) {
            throw std::runtime_error("restore journal transaction changed unexpectedly");
        }
        auto path = restore_journal_path(dataRoot);
        if (!DeleteFileW(path.c_str())) {
            throw std::system_error(static_cast<int>(GetLastError()),
                std::system_category(), "cannot remove completed restore journal");
        }
    }

    std::string owner_value(std::string const& backupId)
    {
        return std::string(backupOwnerMagic) + backupId + "\n";
    }

    std::string complete_value(std::string const& backupId,
        Digest const& manifestDigest)
    {
        return std::string(backupCompleteMagic) + backupId + "\n" +
            hex_digest(manifestDigest) + "\n";
    }

    std::optional<std::string> parse_owner(std::string_view value) noexcept
    {
        if (!value.starts_with(backupOwnerMagic) || value.back() != '\n') return std::nullopt;
        auto id = value.substr(backupOwnerMagic.size(),
            value.size() - backupOwnerMagic.size() - 1);
        std::string result(id);
        return motion::valid_id(result) ? std::optional<std::string>(std::move(result))
            : std::nullopt;
    }

    struct CompletionRecord
    {
        std::string backupId;
        Digest manifestDigest{};
    };

    std::optional<CompletionRecord> parse_completion(std::string_view value) noexcept
    {
        if (!value.starts_with(backupCompleteMagic) || value.back() != '\n') {
            return std::nullopt;
        }
        auto idStart = backupCompleteMagic.size();
        auto idEnd = value.find('\n', idStart);
        if (idEnd == std::string_view::npos) return std::nullopt;
        std::string id(value.substr(idStart, idEnd - idStart));
        auto digestValue = value.substr(idEnd + 1, value.size() - idEnd - 2);
        auto digest = parse_digest(digestValue);
        if (!motion::valid_id(id) || !digest) return std::nullopt;
        return CompletionRecord{ std::move(id), *digest };
    }

    std::wstring json_path(fs::path const& path)
    {
        auto value = path.generic_wstring();
        if (!safe_relative_path(path)) throw std::runtime_error("unsafe manifest path");
        return value;
    }

    std::string serialize_manifest(BackupManifest const& manifest)
    {
        JsonObject object;
        object.Insert(L"format", JsonValue::CreateStringValue(
            motion::utf8_to_wide(std::string(backupFormat))));
        object.Insert(L"version", JsonValue::CreateNumberValue(manifest.version));
        object.Insert(L"backupId", JsonValue::CreateStringValue(
            motion::utf8_to_wide(manifest.backupId)));
        object.Insert(L"createdAt", JsonValue::CreateStringValue(manifest.createdAt));
        object.Insert(L"libraryId", JsonValue::CreateStringValue(
            motion::utf8_to_wide(manifest.libraryId)));
        object.Insert(L"totalBytes", JsonValue::CreateStringValue(
            motion::utf8_to_wide(std::to_string(manifest.totalBytes))));
        object.Insert(L"fileCount", JsonValue::CreateStringValue(
            motion::utf8_to_wide(std::to_string(manifest.fileCount))));
        JsonArray entries;
        for (auto const& entry : manifest.entries) {
            JsonObject value;
            value.Insert(L"path", JsonValue::CreateStringValue(json_path(entry.relative)));
            value.Insert(L"type", JsonValue::CreateStringValue(
                entry.directory ? L"directory" : L"file"));
            if (!entry.directory) {
                value.Insert(L"bytes", JsonValue::CreateStringValue(
                    motion::utf8_to_wide(std::to_string(entry.bytes))));
                value.Insert(L"sha256", JsonValue::CreateStringValue(
                    motion::utf8_to_wide(hex_digest(entry.digest))));
            }
            entries.Append(value);
        }
        object.Insert(L"entries", entries);
        return motion::wide_to_utf8(object.Stringify().c_str());
    }

    BackupManifest parse_manifest(std::string const& value)
    {
        if (value.empty() || value.size() > maximumManifestBytes) {
            throw std::runtime_error("invalid backup manifest size");
        }
        auto object = JsonObject::Parse(motion::utf8_to_wide(value));
        if (motion::wide_to_utf8(object.GetNamedString(L"format").c_str()) != backupFormat) {
            throw std::runtime_error("unsupported backup format");
        }
        auto versionValue = object.GetNamedNumber(L"version");
        if (!std::isfinite(versionValue) || std::floor(versionValue) != versionValue ||
            versionValue != motion::app::library_backup_format_version) {
            throw std::runtime_error("unsupported backup version");
        }
        BackupManifest result;
        result.version = static_cast<int>(versionValue);
        result.backupId = motion::wide_to_utf8(
            object.GetNamedString(L"backupId").c_str());
        result.createdAt = object.GetNamedString(L"createdAt").c_str();
        result.libraryId = motion::wide_to_utf8(
            object.GetNamedString(L"libraryId").c_str());
        auto totalBytes = parse_uint64(motion::wide_to_utf8(
            object.GetNamedString(L"totalBytes").c_str()));
        auto fileCount = parse_uint64(motion::wide_to_utf8(
            object.GetNamedString(L"fileCount").c_str()));
        if (!motion::valid_id(result.backupId) ||
            !motion::valid_id(result.libraryId) || result.createdAt.empty() ||
            result.createdAt.size() > 64 || !totalBytes || !fileCount ||
            *fileCount > maximumManifestEntries) {
            throw std::runtime_error("invalid backup manifest header");
        }
        result.totalBytes = *totalBytes;
        result.fileCount = static_cast<uint32_t>(*fileCount);
        auto entries = object.GetNamedArray(L"entries");
        if (entries.Size() > maximumManifestEntries) {
            throw std::runtime_error("backup manifest has too many entries");
        }
        uint64_t summedBytes{};
        uint32_t countedFiles{};
        result.entries.reserve(entries.Size());
        for (auto const& item : entries) {
            if (item.ValueType() != JsonValueType::Object) {
                throw std::runtime_error("invalid backup manifest entry");
            }
            auto entryObject = item.GetObject();
            fs::path relative(entryObject.GetNamedString(L"path").c_str());
            if (!safe_relative_path(relative)) {
                throw std::runtime_error("unsafe backup manifest path");
            }
            auto type = entryObject.GetNamedString(L"type");
            ManifestEntry entry;
            entry.relative = std::move(relative);
            if (type == L"directory") {
                entry.directory = true;
            } else if (type == L"file") {
                auto bytes = parse_uint64(motion::wide_to_utf8(
                    entryObject.GetNamedString(L"bytes").c_str()));
                auto digest = parse_digest(motion::wide_to_utf8(
                    entryObject.GetNamedString(L"sha256").c_str()));
                if (!bytes || !digest || *bytes >
                    (std::numeric_limits<uint64_t>::max)() - summedBytes) {
                    throw std::runtime_error("invalid backup manifest file entry");
                }
                entry.bytes = *bytes;
                entry.digest = *digest;
                summedBytes += *bytes;
                ++countedFiles;
            } else {
                throw std::runtime_error("invalid backup manifest entry type");
            }
            if (!result.entries.empty() &&
                result.entries.back().relative.generic_wstring() >=
                    entry.relative.generic_wstring()) {
                throw std::runtime_error("backup manifest entries are not unique and sorted");
            }
            result.entries.push_back(std::move(entry));
        }
        if (summedBytes != result.totalBytes || countedFiles != result.fileCount) {
            throw std::runtime_error("backup manifest totals do not match its entries");
        }
        return result;
    }

    void report(motion::app::LibraryBackupProgressCallback const& callback,
        motion::app::LibraryBackupPhase phase, uint64_t completedBytes,
        uint64_t totalBytes, uint32_t completedFiles, uint32_t totalFiles,
        fs::path const& current = {})
    {
        if (!callback) return;
        callback(motion::app::LibraryBackupProgress{ phase, completedBytes,
            totalBytes, completedFiles, totalFiles, current });
    }

    BackupManifest build_payload_manifest(fs::path const& root,
        std::string backupId, std::wstring createdAt, std::string libraryId,
        motion::app::LibraryBackupProgressCallback const& progress,
        std::atomic_bool const* cancelled)
    {
        if (!direct_directory_no_reparse(root) ||
            !direct_directory_no_reparse(root / L"Config") ||
            !direct_regular_file_no_reparse(root / L"Config" / L"settings.json") ||
            !direct_directory_no_reparse(root / L"Library") ||
            !motion::is_owned_media_library(root / L"Library")) {
            throw std::runtime_error("backup payload structure is invalid");
        }
        BackupManifest result;
        result.backupId = std::move(backupId);
        result.createdAt = std::move(createdAt);
        result.libraryId = std::move(libraryId);
        std::error_code error;
        for (fs::recursive_directory_iterator entries(root,
                fs::directory_options::none, error), end;
            !error && entries != end; entries.increment(error)) {
            throw_cancelled(cancelled);
            auto relative = entries->path().lexically_relative(root);
            auto component = relative.begin();
            if (component == relative.end() ||
                (*component != L"Config" && *component != L"Library")) {
                entries.disable_recursion_pending();
                continue;
            }
            if (!safe_relative_path(relative)) {
                entries.disable_recursion_pending();
                throw std::runtime_error("backup payload escaped its root");
            }
            auto first = *relative.begin();
            (void)first;
            auto attributes = GetFileAttributesW(entries->path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                entries.disable_recursion_pending();
                throw std::runtime_error("backup payload contains a reparse point");
            }
            ManifestEntry entry;
            entry.relative = std::move(relative);
            entry.directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!entry.directory) {
                if (!entries->is_regular_file(error) || error) {
                    throw std::runtime_error("backup payload contains an unsupported object");
                }
                entry.bytes = entries->file_size(error);
                if (error || entry.bytes >
                    (std::numeric_limits<uint64_t>::max)() - result.totalBytes) {
                    throw std::runtime_error("backup payload size overflow");
                }
                auto prior = result.totalBytes;
                entry.digest = hash_file(entries->path(), [&](uint64_t chunk) {
                    throw_cancelled(cancelled);
                    report(progress, motion::app::LibraryBackupPhase::Verifying,
                        prior + chunk, 0, result.fileCount,
                        0, entry.relative);
                    prior += chunk;
                });
                result.totalBytes += entry.bytes;
                ++result.fileCount;
            }
            if (result.entries.size() >= maximumManifestEntries) {
                throw std::runtime_error("backup payload has too many entries");
            }
            result.entries.push_back(std::move(entry));
        }
        if (error) throw std::system_error(error);
        std::sort(result.entries.begin(), result.entries.end(), [](auto const& left,
            auto const& right) {
            return left.relative.generic_wstring() < right.relative.generic_wstring();
        });
        auto configFiles = std::count_if(result.entries.begin(), result.entries.end(),
            [](auto const& entry) {
                return !entry.directory &&
                    entry.relative == fs::path(L"Config") / L"settings.json";
            });
        auto anyOtherConfig = std::any_of(result.entries.begin(), result.entries.end(),
            [](auto const& entry) {
                if (entry.directory) return false;
                auto iterator = entry.relative.begin();
                return iterator != entry.relative.end() && *iterator == L"Config" &&
                    entry.relative != fs::path(L"Config") / L"settings.json";
            });
        if (configFiles != 1 || anyOtherConfig) {
            throw std::runtime_error("backup Config payload must contain only settings.json");
        }
        return result;
    }

    void require_exact_backup_root(fs::path const& root)
    {
        if (!direct_directory_no_reparse(root)) {
            throw std::runtime_error("backup root is not a direct directory");
        }
        std::array<std::wstring_view, 5> allowed{
            backupOwnerName, backupManifestName, backupCompleteName,
            L"Config", L"Library" };
        std::error_code error;
        size_t count{};
        for (fs::directory_iterator entries(root, error), end;
            !error && entries != end; entries.increment(error)) {
            auto name = entries->path().filename().wstring();
            if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
                throw std::runtime_error("backup root contains an unexpected item");
            }
            ++count;
        }
        if (error) throw std::system_error(error);
        if (count != allowed.size()) {
            throw std::runtime_error("backup root is incomplete");
        }
    }

    struct BackupInspection
    {
        motion::app::LibraryBackupInfo info;
        BackupManifest manifest;
        StableDirectory root;
    };

    BackupInspection inspect_backup(fs::path backupPath,
        motion::app::LibraryBackupProgressCallback const& progress,
        std::atomic_bool const* cancelled)
    {
        throw_cancelled(cancelled);
        backupPath = fs::absolute(backupPath).lexically_normal();
        auto stable = capture_stable_directory(backupPath);
        if (!stable) throw std::runtime_error("backup path is not a stable direct directory");
        require_exact_backup_root(stable->stablePath);
        if (!stable->Revalidate()) throw std::runtime_error("backup root identity changed");
        auto owner = parse_owner(read_control_file(
            stable->stablePath / backupOwnerName, 512));
        auto manifestBytes = read_control_file(
            stable->stablePath / backupManifestName, maximumManifestBytes);
        auto completion = parse_completion(read_control_file(
            stable->stablePath / backupCompleteName, 1024));
        if (!owner || !completion || *owner != completion->backupId ||
            hash_bytes(manifestBytes) != completion->manifestDigest) {
            throw std::runtime_error("backup completion record is invalid");
        }
        auto manifest = parse_manifest(manifestBytes);
        if (manifest.backupId != *owner) {
            throw std::runtime_error("backup identity does not match its manifest");
        }
        auto libraryIdentity = motion::capture_media_library_trust(
            stable->stablePath / L"Library");
        if (!libraryIdentity || libraryIdentity->ownershipId != manifest.libraryId ||
            !motion::is_owned_media_library(libraryIdentity->root) ||
            !motion::revalidate_media_library_trust(*libraryIdentity)) {
            throw std::runtime_error("backup library ownership is invalid");
        }
        report(progress, motion::app::LibraryBackupPhase::Verifying, 0,
            manifest.totalBytes, 0, manifest.fileCount);
        uint64_t verifiedBytes{};
        uint32_t verifiedFiles{};
        auto actual = build_payload_manifest(stable->stablePath, manifest.backupId,
            manifest.createdAt, manifest.libraryId,
            [&](motion::app::LibraryBackupProgress const& update) {
                auto completed = (std::min)(manifest.totalBytes,
                    verifiedBytes + update.completedBytes);
                report(progress, motion::app::LibraryBackupPhase::Verifying,
                    completed, manifest.totalBytes, verifiedFiles,
                    manifest.fileCount, update.currentPath);
            }, cancelled);
        // build_payload_manifest reports local per-file chunks. Emit exact
        // monotonic file totals here as a compact final verification update.
        for (auto const& entry : actual.entries) {
            if (!entry.directory) {
                verifiedBytes += entry.bytes;
                ++verifiedFiles;
            }
        }
        if (actual.entries != manifest.entries ||
            actual.totalBytes != manifest.totalBytes ||
            actual.fileCount != manifest.fileCount) {
            throw std::runtime_error("backup payload does not match its manifest");
        }
        if (!stable->Revalidate()) throw std::runtime_error("backup root identity changed");
        report(progress, motion::app::LibraryBackupPhase::Verifying,
            manifest.totalBytes, manifest.totalBytes, manifest.fileCount,
            manifest.fileCount);
        motion::app::LibraryBackupInfo info;
        info.version = manifest.version;
        info.backupId = manifest.backupId;
        info.createdAt = manifest.createdAt;
        info.libraryId = manifest.libraryId;
        info.totalBytes = manifest.totalBytes;
        info.fileCount = manifest.fileCount;
        info.path = std::move(backupPath);
        return BackupInspection{ std::move(info), std::move(manifest),
            std::move(*stable) };
    }

    void copy_settings(fs::path const& source, fs::path const& destination,
        motion::app::LibraryBackupProgressCallback const& progress,
        uint64_t totalBytes, std::atomic_bool const* cancelled)
    {
        motion::unique_handle input(CreateFileW(source.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        BY_HANDLE_FILE_INFORMATION information{};
        if (!input || !GetFileInformationByHandle(input.get(), &information) ||
            (information.dwFileAttributes &
                (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            throw std::runtime_error("settings file is unsafe");
        }
        ULARGE_INTEGER size{};
        size.HighPart = information.nFileSizeHigh;
        size.LowPart = information.nFileSizeLow;
        motion::unique_handle output(CreateFileW(destination.c_str(), GENERIC_WRITE,
            0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!output) throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category());
        // Keep the transfer buffer off the caller's default 1 MiB Windows
        // thread stack. Entering this function with a 1 MiB local array can
        // raise STATUS_STACK_OVERFLOW before the first copy operation runs.
        std::vector<unsigned char> buffer(1024 * 1024);
        uint64_t copied{};
        try {
            for (;;) {
                throw_cancelled(cancelled);
                DWORD read{};
                if (!ReadFile(input.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                    &read, nullptr)) {
                    throw std::system_error(static_cast<int>(GetLastError()),
                        std::system_category());
                }
                if (!read) break;
                DWORD written{};
                if (!WriteFile(output.get(), buffer.data(), read, &written, nullptr) ||
                    written != read) {
                    throw std::system_error(static_cast<int>(
                        GetLastError() ? GetLastError() : ERROR_WRITE_FAULT),
                        std::system_category());
                }
                copied += written;
                report(progress, motion::app::LibraryBackupPhase::CopyingSettings,
                    copied, totalBytes, 0, 0,
                    fs::path(L"Config") / L"settings.json");
            }
            if (copied != size.QuadPart || !FlushFileBuffers(output.get())) {
                throw std::runtime_error("settings copy could not be completed");
            }
            output.reset();
            if (hash_file(source) != hash_file(destination)) {
                throw std::runtime_error("settings copy verification failed");
            }
        } catch (...) {
            output.reset();
            std::error_code ignored;
            fs::remove(destination, ignored);
            throw;
        }
    }

    uint64_t inspected_library_bytes(fs::path const& root,
        uint32_t& fileCount, std::atomic_bool const* cancelled)
    {
        if (!direct_directory_no_reparse(root)) {
            throw std::runtime_error("library root is unsafe");
        }
        uint64_t result{};
        std::error_code error;
        for (fs::recursive_directory_iterator entries(root,
                fs::directory_options::none, error), end;
            !error && entries != end; entries.increment(error)) {
            throw_cancelled(cancelled);
            auto attributes = GetFileAttributesW(entries->path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                entries.disable_recursion_pending();
                throw std::runtime_error("library contains a reparse point");
            }
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                if (!entries->is_regular_file(error) || error) {
                    throw std::runtime_error("library contains an unsupported object");
                }
                auto size = entries->file_size(error);
                if (error || size > (std::numeric_limits<uint64_t>::max)() - result) {
                    throw std::runtime_error("library size overflow");
                }
                result += size;
                if (++fileCount > maximumManifestEntries) {
                    throw std::runtime_error("library has too many files");
                }
            }
        }
        if (error) throw std::system_error(error);
        return result;
    }

    fs::path generated_backup_name(std::string const& backupId)
    {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t value[80]{};
        swprintf_s(value, L"MotionWallpaper-Backup-%04u%02u%02u-%02u%02u%02u-%.*s",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
            8, motion::utf8_to_wide(backupId).c_str());
        return value;
    }

    bool rename_open_directory(HANDLE directory, fs::path const& destination) noexcept
    {
        try {
            auto normalized = fs::absolute(destination).lexically_normal();
            auto name = normalized.wstring();
            if (name.empty() || name.size() >
                (std::numeric_limits<DWORD>::max)() / sizeof(wchar_t)) {
                SetLastError(ERROR_INVALID_NAME);
                return false;
            }
            size_t bytes = sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t);
            if (bytes > (std::numeric_limits<DWORD>::max)()) {
                SetLastError(ERROR_BUFFER_OVERFLOW);
                return false;
            }
            std::vector<unsigned char> storage(bytes);
            auto information = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
            information->ReplaceIfExists = FALSE;
            information->RootDirectory = nullptr;
            information->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
            std::memcpy(information->FileName, name.data(), information->FileNameLength);
            return SetFileInformationByHandle(directory, FileRenameInfo, information,
                static_cast<DWORD>(bytes)) != FALSE;
        } catch (...) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return false;
        }
    }

    motion::unique_handle open_verified_directory_for_rename(
        fs::path const& path, motion::FilesystemObjectIdentity const& expected) noexcept
    {
        motion::unique_handle result(CreateFileW(path.c_str(),
            DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        auto identity = direct_handle_identity(result.get(), true);
        if (!result || !identity || *identity != expected) return {};
        return result;
    }

    motion::unique_handle open_verified_library_for_rename(
        motion::MediaLibraryTrustIdentity const& expected) noexcept
    {
        try {
            auto result = open_verified_directory_for_rename(
                expected.stableRoot, expected.rootIdentity);
            if (!result) return {};
            auto current = motion::capture_media_library_trust(expected.root);
            if (!current || current->ownershipId != expected.ownershipId ||
                current->rootIdentity != expected.rootIdentity ||
                current->markerIdentity != expected.markerIdentity ||
                current->groupsIdentity != expected.groupsIdentity) {
                return {};
            }
            return result;
        } catch (...) {
            return {};
        }
    }

    bool exact_file(fs::path const& path, std::string const& expected) noexcept
    {
        try {
            return direct_regular_file_no_reparse(path) &&
                read_control_file(path, (std::max)(expected.size(), size_t{ 1 })) == expected;
        } catch (...) {
            return false;
        }
    }

    void remove_owned_backup_tree_noexcept(fs::path const& root,
        std::string const& backupId) noexcept
    {
        try {
            if (!direct_directory_no_reparse(root) ||
                !exact_file(root / backupOwnerName, owner_value(backupId))) return;
            std::vector<fs::path> files;
            std::vector<fs::path> directories;
            std::error_code error;
            for (fs::recursive_directory_iterator entries(root,
                    fs::directory_options::none, error), end;
                !error && entries != end; entries.increment(error)) {
                auto attributes = GetFileAttributesW(entries->path().c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES ||
                    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                    entries.disable_recursion_pending();
                    return;
                }
                if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
                    directories.push_back(entries->path());
                } else {
                    files.push_back(entries->path());
                }
            }
            if (error || !exact_file(root / backupOwnerName, owner_value(backupId))) return;
            for (auto const& file : files) {
                auto attributes = GetFileAttributesW(file.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES ||
                    (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return;
                if (attributes & FILE_ATTRIBUTE_READONLY) {
                    SetFileAttributesW(file.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
                }
                error.clear();
                if (!fs::remove(file, error) || error) return;
            }
            std::sort(directories.begin(), directories.end(), [](auto const& left,
                auto const& right) { return left.native().size() > right.native().size(); });
            for (auto const& directory : directories) {
                if (!direct_directory_no_reparse(directory)) return;
                error.clear();
                if (!fs::remove(directory, error) || error) return;
            }
            error.clear();
            fs::remove(root, error);
        } catch (...) {}
    }

    void remove_owned_library_tree_noexcept(fs::path const& root,
        motion::MediaLibraryTrustIdentity const& expected) noexcept
    {
        try {
            auto identity = motion::capture_media_library_trust(root);
            if (!identity || identity->ownershipId != expected.ownershipId ||
                identity->rootIdentity != expected.rootIdentity ||
                !motion::is_owned_media_library(root)) return;
            auto trust = motion::acquire_media_library_trust(*identity);
            if (!trust) return;
            std::vector<fs::path> files;
            std::vector<fs::path> directories;
            std::error_code error;
            for (fs::recursive_directory_iterator entries(identity->stableRoot,
                    fs::directory_options::none, error), end;
                !error && entries != end; entries.increment(error)) {
                auto attributes = GetFileAttributesW(entries->path().c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES ||
                    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                    entries.disable_recursion_pending();
                    return;
                }
                ((attributes & FILE_ATTRIBUTE_DIRECTORY) ? directories : files)
                    .push_back(entries->path());
            }
            if (error) return;
            trust.reset();
            for (auto const& file : files) {
                auto attributes = GetFileAttributesW(file.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES ||
                    (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return;
                if (attributes & FILE_ATTRIBUTE_READONLY) {
                    SetFileAttributesW(file.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
                }
                error.clear();
                if (!fs::remove(file, error) || error) return;
            }
            std::sort(directories.begin(), directories.end(), [](auto const& left,
                auto const& right) { return left.native().size() > right.native().size(); });
            for (auto const& directory : directories) {
                if (!direct_directory_no_reparse(directory)) return;
                error.clear();
                if (!fs::remove(directory, error) || error) return;
            }
            error.clear();
            fs::remove(identity->stableRoot, error);
        } catch (...) {}
    }

    void remove_owned_library_tree_checked(fs::path const& root,
        motion::MediaLibraryTrustIdentity const& expected)
    {
        auto identity = motion::capture_media_library_trust(root);
        if (!identity) throw std::runtime_error("owned restore staging could not be captured");
        if (identity->ownershipId != expected.ownershipId) {
            throw std::runtime_error("owned restore staging ownership changed");
        }
        if (identity->rootIdentity != expected.rootIdentity) {
            throw std::runtime_error("owned restore staging root identity changed");
        }
        auto trust = motion::acquire_media_library_trust(*identity);
        if (!trust) throw std::runtime_error("cannot lock restore staging library");
        std::vector<fs::path> files;
        std::vector<fs::path> directories;
        std::error_code error;
        {
            // Enumerate and delete through the handle-derived volume path. It
            // carries the extended-length prefix, whereas a deeply nested
            // configured path may exceed legacy MAX_PATH during recovery.
            for (fs::recursive_directory_iterator entries(identity->stableRoot,
                    fs::directory_options::none, error), end;
                !error && entries != end; entries.increment(error)) {
                auto attributes = GetFileAttributesW(entries->path().c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES ||
                    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                    entries.disable_recursion_pending();
                    throw std::runtime_error("restore staging library contains an unsafe object: " +
                        motion::wide_to_utf8(entries->path().wstring()));
                }
                ((attributes & FILE_ATTRIBUTE_DIRECTORY) ? directories : files)
                    .push_back(entries->path());
            }
        }
        if (error) throw std::system_error(error);
        trust.reset();
        for (auto const& file : files) {
            auto attributes = GetFileAttributesW(file.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & (FILE_ATTRIBUTE_DIRECTORY |
                    FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
                throw std::runtime_error("restore staging file identity became unsafe");
            }
            if ((attributes & FILE_ATTRIBUTE_READONLY) != 0 &&
                !SetFileAttributesW(file.c_str(),
                    attributes & ~FILE_ATTRIBUTE_READONLY)) {
                throw std::system_error(static_cast<int>(GetLastError()),
                    std::system_category(),
                    "cannot clear restore staging file attributes");
            }
            error.clear();
            if (!fs::remove(file, error) || error) {
                throw std::system_error(error ? error :
                    std::make_error_code(std::errc::operation_not_permitted),
                    "cannot remove restore staging file");
            }
        }
        std::sort(directories.begin(), directories.end(), [](auto const& left,
            auto const& right) { return left.native().size() > right.native().size(); });
        for (auto const& directory : directories) {
            if (!direct_directory_no_reparse(directory)) {
                throw std::runtime_error("restore staging directory became unsafe");
            }
            error.clear();
            if (!fs::remove(directory, error) || error) {
                throw std::system_error(error ? error :
                    std::make_error_code(std::errc::operation_not_permitted),
                    "cannot remove restore staging directory");
            }
        }
        error.clear();
        if (!fs::remove(identity->stableRoot, error) || error) {
            throw std::system_error(error ? error :
                std::make_error_code(std::errc::operation_not_permitted),
                "cannot remove restore staging root");
        }
    }

    class PreparedLibraryGuard final
    {
    public:
        PreparedLibraryGuard(fs::path path,
            motion::MediaLibraryTrustIdentity identity)
            : path_(std::move(path)), identity_(std::move(identity)) {}
        ~PreparedLibraryGuard()
        {
            if (active_) remove_owned_library_tree_noexcept(path_, identity_);
        }
        PreparedLibraryGuard(PreparedLibraryGuard const&) = delete;
        PreparedLibraryGuard& operator=(PreparedLibraryGuard const&) = delete;
        void Release() noexcept { active_ = false; }
    private:
        fs::path path_;
        motion::MediaLibraryTrustIdentity identity_;
        bool active_{ true };
    };

    class TemporaryFileGuard final
    {
    public:
        explicit TemporaryFileGuard(fs::path path) : path_(std::move(path)) {}
        ~TemporaryFileGuard()
        {
            if (!active_) return;
            std::error_code ignored;
            fs::remove(path_, ignored);
        }
        TemporaryFileGuard(TemporaryFileGuard const&) = delete;
        TemporaryFileGuard& operator=(TemporaryFileGuard const&) = delete;
        void Release() noexcept { active_ = false; }
    private:
        fs::path path_;
        bool active_{ true };
    };

    bool current_library_is_default(fs::path const& dataRoot,
        fs::path const& libraryPath)
    {
        return motion::same_filesystem_path(
            fs::absolute(dataRoot / L"Wallpapers").lexically_normal(),
            fs::absolute(libraryPath).lexically_normal());
    }

    enum class RestoreLibraryObject
    {
        Missing,
        Previous,
        Restored,
        Unsafe
    };

    bool missing_direct_object(fs::path const& path) noexcept
    {
        auto attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) return false;
        auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }

    bool trust_matches(motion::MediaLibraryTrustIdentity const& identity,
        std::string const& ownershipId,
        motion::FilesystemObjectIdentity const& rootIdentity) noexcept
    {
        return identity.ownershipId == ownershipId &&
            identity.rootIdentity == rootIdentity &&
            motion::revalidate_media_library_trust(identity);
    }

    RestoreLibraryObject inspect_restore_library(fs::path const& path,
        RestoreJournal const& journal) noexcept
    {
        try {
            if (missing_direct_object(path)) return RestoreLibraryObject::Missing;
            if (!direct_directory_no_reparse(path)) return RestoreLibraryObject::Unsafe;
            auto identity = motion::capture_media_library_trust(path);
            if (!identity) return RestoreLibraryObject::Unsafe;
            if (trust_matches(*identity, journal.restoredLibraryId,
                    journal.restoredLibraryIdentity)) {
                return RestoreLibraryObject::Restored;
            }
            if (journal.replacesExistingLibrary &&
                trust_matches(*identity, journal.previousLibraryId,
                    journal.previousLibraryIdentity)) {
                return RestoreLibraryObject::Previous;
            }
            return RestoreLibraryObject::Unsafe;
        } catch (...) {
            return RestoreLibraryObject::Unsafe;
        }
    }

    void rename_expected_restore_library(fs::path const& source,
        fs::path const& destination, std::string const& ownershipId,
        motion::FilesystemObjectIdentity const& rootIdentity)
    {
        if (!missing_direct_object(destination)) {
            throw std::runtime_error("restore recovery rename destination is occupied");
        }
        auto identity = motion::capture_media_library_trust(source);
        if (!identity || !trust_matches(*identity, ownershipId, rootIdentity)) {
            throw std::runtime_error("restore recovery source identity is unsafe");
        }
        auto handle = open_verified_directory_for_rename(source, rootIdentity);
        if (!handle || !rename_open_directory(handle.get(), destination)) {
            throw std::runtime_error("restore recovery could not rename a verified library");
        }
        auto moved = motion::capture_media_library_trust(destination);
        if (!moved || !trust_matches(*moved, ownershipId, rootIdentity)) {
            throw std::runtime_error("restore recovery rename verification failed");
        }
    }

    bool expected_regular_file(fs::path const& path,
        Digest const& digest) noexcept
    {
        try {
            return direct_regular_file_no_reparse(path) && hash_file(path) == digest;
        } catch (...) {
            return false;
        }
    }

    void remove_expected_regular_file(fs::path const& path,
        Digest const& digest)
    {
        if (missing_direct_object(path)) return;
        if (!expected_regular_file(path, digest) || !DeleteFileW(path.c_str())) {
            throw std::runtime_error("restore recovery could not remove its prepared settings");
        }
    }

    void remove_expected_restored_library(fs::path const& path,
        RestoreJournal const& journal)
    {
        if (missing_direct_object(path)) return;
        auto identity = motion::capture_media_library_trust(path);
        if (!identity || !trust_matches(*identity, journal.restoredLibraryId,
                journal.restoredLibraryIdentity)) {
            throw std::runtime_error("restore recovery staging library is unsafe");
        }
        remove_owned_library_tree_checked(path, *identity);
        if (!missing_direct_object(path)) {
            throw std::runtime_error("restore recovery could not remove its staging library");
        }
    }

    motion::app::LibraryRestoreRecoveryResult rollback_restore(
        fs::path const& dataRoot, RestoreJournal const& journal)
    {
        auto target = inspect_restore_library(journal.activeLibraryPath, journal);
        auto stage = inspect_restore_library(journal.stagedLibraryPath, journal);
        auto archive = journal.replacesExistingLibrary
            ? inspect_restore_library(journal.archivedLibraryPath, journal)
            : RestoreLibraryObject::Missing;

        if (journal.replacesExistingLibrary) {
            // Directory rename is atomic but a process may die before the next
            // phase write. Accept every exact prefix of the swap and infer the
            // durable position from the recorded object identities.
            if (target == RestoreLibraryObject::Restored &&
                stage == RestoreLibraryObject::Missing &&
                archive == RestoreLibraryObject::Previous) {
                rename_expected_restore_library(journal.activeLibraryPath,
                    journal.stagedLibraryPath, journal.restoredLibraryId,
                    journal.restoredLibraryIdentity);
                target = RestoreLibraryObject::Missing;
                stage = RestoreLibraryObject::Restored;
            }
            if (target == RestoreLibraryObject::Missing &&
                stage == RestoreLibraryObject::Restored &&
                archive == RestoreLibraryObject::Previous) {
                rename_expected_restore_library(journal.archivedLibraryPath,
                    journal.activeLibraryPath, journal.previousLibraryId,
                    journal.previousLibraryIdentity);
                target = RestoreLibraryObject::Previous;
                archive = RestoreLibraryObject::Missing;
            }
            bool preparedPrefix = target == RestoreLibraryObject::Previous &&
                stage == RestoreLibraryObject::Restored &&
                archive == RestoreLibraryObject::Missing;
            bool alreadyRolledBack = target == RestoreLibraryObject::Previous &&
                stage == RestoreLibraryObject::Missing &&
                archive == RestoreLibraryObject::Missing;
            if (!preparedPrefix && !alreadyRolledBack) {
                throw std::runtime_error("pending restore library layout is ambiguous");
            }
        } else {
            if (target == RestoreLibraryObject::Restored &&
                stage == RestoreLibraryObject::Missing) {
                rename_expected_restore_library(journal.activeLibraryPath,
                    journal.stagedLibraryPath, journal.restoredLibraryId,
                    journal.restoredLibraryIdentity);
                target = RestoreLibraryObject::Missing;
                stage = RestoreLibraryObject::Restored;
            }
            if (!((target == RestoreLibraryObject::Missing &&
                    stage == RestoreLibraryObject::Restored) ||
                (target == RestoreLibraryObject::Missing &&
                    stage == RestoreLibraryObject::Missing))) {
                throw std::runtime_error("pending offline restore layout is ambiguous");
            }
        }

        if (!missing_direct_object(journal.previousSettingsPath)) {
            throw std::runtime_error("restore rollback found an unexpected settings archive");
        }
        remove_expected_regular_file(journal.preparedSettingsPath,
            journal.restoredSettingsDigest);
        remove_expected_restored_library(journal.stagedLibraryPath, journal);
        remove_restore_journal(dataRoot, journal.transactionId);
        return { motion::app::LibraryRestoreRecoveryOutcome::RolledBack,
            journal.replacesExistingLibrary ? journal.activeLibraryPath : fs::path{} };
    }

    motion::app::LibraryRestoreRecoveryResult complete_restore(
        fs::path const& dataRoot, RestoreJournal journal)
    {
        auto target = inspect_restore_library(journal.activeLibraryPath, journal);
        auto stage = inspect_restore_library(journal.stagedLibraryPath, journal);
        auto archive = journal.replacesExistingLibrary
            ? inspect_restore_library(journal.archivedLibraryPath, journal)
            : RestoreLibraryObject::Missing;
        if (target != RestoreLibraryObject::Restored ||
            stage != RestoreLibraryObject::Missing ||
            (journal.replacesExistingLibrary
                ? archive != RestoreLibraryObject::Previous
                : archive != RestoreLibraryObject::Missing)) {
            throw std::runtime_error("committing restore library layout is ambiguous");
        }

        auto settingsPath = fs::absolute(dataRoot / L"Config" /
            L"settings.json").lexically_normal();
        bool currentMatches = expected_regular_file(settingsPath,
            journal.restoredSettingsDigest);
        bool preparedMatches = expected_regular_file(journal.preparedSettingsPath,
            journal.restoredSettingsDigest);
        bool previousExists = !missing_direct_object(journal.previousSettingsPath);
        if (journal.phase == RestoreJournalPhase::SettingsCommitted) {
            if (!currentMatches || !previousExists ||
                !direct_regular_file_no_reparse(journal.previousSettingsPath) ||
                !missing_direct_object(journal.preparedSettingsPath)) {
                throw std::runtime_error("committed restore settings are incomplete");
            }
        } else {
            if (previousExists) {
                if (!currentMatches ||
                    !direct_regular_file_no_reparse(journal.previousSettingsPath) ||
                    !missing_direct_object(journal.preparedSettingsPath)) {
                    throw std::runtime_error("restore settings commit is ambiguous");
                }
            } else {
                if (!preparedMatches || !direct_regular_file_no_reparse(settingsPath)) {
                    throw std::runtime_error("prepared restore settings are unavailable");
                }
                if (!ReplaceFileW(settingsPath.c_str(),
                        journal.preparedSettingsPath.c_str(),
                        journal.previousSettingsPath.c_str(),
                        REPLACEFILE_WRITE_THROUGH | REPLACEFILE_IGNORE_MERGE_ERRORS |
                            REPLACEFILE_IGNORE_ACL_ERRORS,
                        nullptr, nullptr)) {
                    throw std::system_error(static_cast<int>(GetLastError()),
                        std::system_category(),
                        "cannot finish pending restored settings commit");
                }
                currentMatches = expected_regular_file(settingsPath,
                    journal.restoredSettingsDigest);
                if (!currentMatches ||
                    !direct_regular_file_no_reparse(journal.previousSettingsPath) ||
                    !missing_direct_object(journal.preparedSettingsPath)) {
                    throw std::runtime_error("restored settings commit verification failed");
                }
            }
            journal.phase = RestoreJournalPhase::SettingsCommitted;
            persist_restore_journal(dataRoot, journal, false);
        }
        remove_restore_journal(dataRoot, journal.transactionId);
        return { motion::app::LibraryRestoreRecoveryOutcome::Completed,
            journal.activeLibraryPath };
    }

    motion::app::LibraryRestoreRecoveryResult recover_pending_restore(
        fs::path const& dataRoot)
    {
        auto journal = read_restore_journal(dataRoot);
        return journal.phase == RestoreJournalPhase::SettingsCommitPending ||
            journal.phase == RestoreJournalPhase::SettingsCommitted
            ? complete_restore(dataRoot, std::move(journal))
            : rollback_restore(dataRoot, journal);
    }
}

namespace motion::app
{
    LibraryBackupResult LibraryBackupService::Create(
        fs::path dataRoot, fs::path libraryPath,
        motion::MediaLibraryTrustIdentity const& expectedLibrary,
        fs::path destinationDirectory,
        LibraryBackupProgressCallback const& progress,
        std::atomic_bool const* cancelled)
    {
        throw_cancelled(cancelled);
        dataRoot = fs::absolute(dataRoot).lexically_normal();
        libraryPath = fs::absolute(libraryPath).lexically_normal();
        destinationDirectory = fs::absolute(destinationDirectory).lexically_normal();
        if (!dataRoot.has_filename() || !libraryPath.has_filename() ||
            !destinationDirectory.has_filename() ||
            !motion::same_filesystem_path(libraryPath, expectedLibrary.root)) {
            throw std::runtime_error("unsafe backup paths");
        }
        auto sourceTrust = motion::acquire_media_library_trust(expectedLibrary);
        if (!sourceTrust || !motion::revalidate_media_library_trust(expectedLibrary)) {
            throw std::runtime_error("media library identity changed before backup");
        }
        auto settingsPath = dataRoot / L"Config" / L"settings.json";
        motion::Settings currentSettings;
        auto settingsStatus = motion::load_settings_file(settingsPath, currentSettings);
        if (settingsStatus != motion::SettingsFileStatus::valid) {
            throw std::runtime_error("settings cannot be backed up safely");
        }
        bool defaultLibrary = current_library_is_default(dataRoot, libraryPath);
        if ((!defaultLibrary &&
                (!motion::same_filesystem_path(currentSettings.mediaLibraryPath, libraryPath) ||
                    currentSettings.mediaLibraryId != expectedLibrary.ownershipId)) ||
            (defaultLibrary && !currentSettings.mediaLibraryPath.empty())) {
            throw std::runtime_error("settings and media library do not describe the same state");
        }
        auto destination = capture_stable_directory(destinationDirectory);
        if (!destination) {
            throw std::runtime_error("backup destination is not a stable direct directory");
        }
        auto backupId = motion::new_id();
        auto finalName = generated_backup_name(backupId);
        auto stageName = fs::path(backupStagePrefix + motion::utf8_to_wide(backupId));
        auto finalStable = destination->stablePath / finalName;
        auto stageStable = destination->stablePath / stageName;
        auto finalConfigured = destinationDirectory / finalName;
        if (motion::same_filesystem_path(libraryPath, finalStable) ||
            motion::filesystem_path_is_nested(libraryPath, finalStable) ||
            motion::filesystem_path_is_nested(finalStable, libraryPath)) {
            throw std::runtime_error("backup destination overlaps the media library");
        }
        std::error_code error;
        if (fs::exists(finalStable, error) || error || fs::exists(stageStable, error) || error) {
            throw std::runtime_error("backup destination already contains this operation");
        }

        uint32_t sourceFileCount{};
        report(progress, LibraryBackupPhase::Inspecting, 0, 0, 0, 0);
        auto libraryBytes = inspected_library_bytes(expectedLibrary.stableRoot,
            sourceFileCount, cancelled);
        auto settingsBytes = fs::file_size(settingsPath, error);
        if (error || settingsBytes >
            (std::numeric_limits<uint64_t>::max)() - libraryBytes) {
            throw std::runtime_error("backup source size is unavailable");
        }
        auto totalBytes = libraryBytes + settingsBytes;
        if (!destination->Revalidate() || !fs::create_directory(stageStable)) {
            throw std::runtime_error("cannot create backup staging directory");
        }
        bool committed{};
        try {
            write_new_file(stageStable / backupOwnerName, owner_value(backupId),
                FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
            fs::create_directory(stageStable / L"Config");
            if (!direct_directory_no_reparse(stageStable / L"Config")) {
                throw std::runtime_error("cannot create backup Config directory");
            }
            copy_settings(settingsPath, stageStable / L"Config" / L"settings.json",
                progress, totalBytes, cancelled);
            motion::Settings copiedSettings;
            auto copiedSettingsStatus = motion::load_settings_file(
                stageStable / L"Config" / L"settings.json", copiedSettings);
            if (copiedSettingsStatus != motion::SettingsFileStatus::valid ||
                (!defaultLibrary &&
                    (!motion::same_filesystem_path(copiedSettings.mediaLibraryPath,
                        libraryPath) ||
                        copiedSettings.mediaLibraryId != expectedLibrary.ownershipId)) ||
                (defaultLibrary && !copiedSettings.mediaLibraryPath.empty())) {
                throw std::runtime_error(
                    "settings changed or became inconsistent during backup");
            }
            auto transaction = LibraryMigrationTransaction::Begin(libraryPath,
                stageStable / L"Library", expectedLibrary);
            // The transaction owns an independent trust lease now.
            sourceTrust.reset();
            transaction->CopyAndVerify([&](uint64_t copied, uint64_t total) {
                report(progress, LibraryBackupPhase::CopyingLibrary,
                    settingsBytes + copied, settingsBytes + total, 1,
                    sourceFileCount + 1, L"Library");
            }, cancelled);
            transaction->CommitPreparedTarget();
            transaction->MarkActivated();
            transaction.reset();
            throw_cancelled(cancelled);

            auto manifest = build_payload_manifest(stageStable, backupId,
                motion::timestamp_utc(), expectedLibrary.ownershipId,
                progress, cancelled);
            auto manifestBytes = serialize_manifest(manifest);
            if (manifestBytes.size() > maximumManifestBytes) {
                throw std::runtime_error("backup manifest is too large");
            }
            write_new_file(stageStable / backupManifestName, manifestBytes,
                FILE_ATTRIBUTE_NORMAL);
            write_new_file(stageStable / backupCompleteName,
                complete_value(backupId, hash_bytes(manifestBytes)),
                FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);

            auto inspection = inspect_backup(stageStable, progress, cancelled);
            if (inspection.info.backupId != backupId || !destination->Revalidate()) {
                throw std::runtime_error("backup staging identity changed before commit");
            }
            auto stageIdentity = inspection.root.identity;
            auto inspectedStagePath = inspection.root.stablePath;
            // Release read-only identity handles before requesting the rename.
            inspection.root.configuredHandle.reset();
            inspection.root.stableHandle.reset();
            auto stageHandle = open_verified_directory_for_rename(
                inspectedStagePath, stageIdentity);
            if (!stageHandle || !rename_open_directory(stageHandle.get(), finalStable)) {
                throw std::runtime_error("cannot atomically commit backup directory");
            }
            if (!destination->Revalidate() ||
                !direct_directory_no_reparse(finalStable) ||
                !exact_file(finalStable / backupOwnerName, owner_value(backupId)) ||
                !exact_file(finalStable / backupCompleteName,
                    complete_value(backupId, hash_bytes(manifestBytes)))) {
                if (!rename_open_directory(stageHandle.get(), stageStable)) {
                    committed = true;
                    throw std::runtime_error(
                        "backup was committed but its final-path verification failed");
                }
                throw std::runtime_error("committed backup could not be verified");
            }
            committed = true;
            LibraryBackupInfo info = std::move(inspection.info);
            info.path = std::move(finalConfigured);
            try {
                report(progress, LibraryBackupPhase::Completed, info.totalBytes,
                    info.totalBytes, info.fileCount, info.fileCount);
            } catch (...) {}
            return LibraryBackupResult{ std::move(info) };
        } catch (...) {
            if (!committed) remove_owned_backup_tree_noexcept(stageStable, backupId);
            throw;
        }
    }

    LibraryBackupInfo LibraryBackupService::Validate(
        fs::path backupPath, LibraryBackupProgressCallback const& progress,
        std::atomic_bool const* cancelled)
    {
        return inspect_backup(std::move(backupPath), progress, cancelled).info;
    }

    bool LibraryBackupService::HasPendingRestore(fs::path dataRoot) noexcept
    {
        try {
            auto path = restore_journal_path(dataRoot);
            auto attributes = GetFileAttributesW(path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES) return true;
            auto error = GetLastError();
            return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
        } catch (...) {
            return true;
        }
    }

    LibraryRestoreRecoveryResult LibraryBackupService::RecoverPendingRestore(
        fs::path dataRoot)
    {
        dataRoot = fs::absolute(dataRoot).lexically_normal();
        if (!dataRoot.has_filename()) {
            throw std::runtime_error("unsafe restore recovery root");
        }
        if (!HasPendingRestore(dataRoot)) return {};
        return recover_pending_restore(dataRoot);
    }

    LibraryRestoreResult LibraryBackupService::Restore(
        fs::path dataRoot,
        std::optional<motion::MediaLibraryTrustIdentity> expectedCurrentLibrary,
        fs::path backupPath, LibraryBackupProgressCallback const& progress,
        std::atomic_bool const* cancelled, LibraryRestoreCrashPoint crashPoint)
    {
        throw_cancelled(cancelled);
        dataRoot = fs::absolute(dataRoot).lexically_normal();
        backupPath = fs::absolute(backupPath).lexically_normal();
        if (!dataRoot.has_filename() || !backupPath.has_filename() ||
            motion::same_filesystem_path(dataRoot, backupPath) ||
            motion::filesystem_path_is_nested(backupPath, dataRoot)) {
            throw std::runtime_error("unsafe restore paths");
        }
        if (HasPendingRestore(dataRoot)) {
            throw std::runtime_error("a previous restore transaction requires recovery");
        }

        std::shared_ptr<motion::MediaLibraryTrustLease> currentTrust;
        fs::path currentLibraryPath;
        if (expectedCurrentLibrary) {
            currentLibraryPath = fs::absolute(
                expectedCurrentLibrary->root).lexically_normal();
            if (!currentLibraryPath.has_filename() ||
                motion::same_filesystem_path(currentLibraryPath, backupPath) ||
                motion::filesystem_path_is_nested(currentLibraryPath, backupPath) ||
                motion::filesystem_path_is_nested(backupPath, currentLibraryPath)) {
                throw std::runtime_error("backup and active library paths overlap");
            }
            currentTrust = motion::acquire_media_library_trust(
                *expectedCurrentLibrary);
            if (!currentTrust ||
                !motion::revalidate_media_library_trust(*expectedCurrentLibrary)) {
                throw std::runtime_error("active media library identity changed before restore");
            }
        }

        auto inspection = inspect_backup(backupPath, progress, cancelled);
        auto backupLibrary = inspection.root.stablePath / L"Library";
        auto backupIdentity = motion::capture_media_library_trust(backupLibrary);
        if (!backupIdentity || backupIdentity->ownershipId != inspection.info.libraryId ||
            !motion::revalidate_media_library_trust(*backupIdentity)) {
            throw std::runtime_error("backup library changed before restore");
        }

        auto backupSettingsPath = inspection.root.stablePath /
            L"Config" / L"settings.json";
        motion::unique_handle protectedBackupSettings(CreateFileW(
            backupSettingsPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!protectedBackupSettings ||
            !direct_handle_identity(protectedBackupSettings.get(), false)) {
            throw std::runtime_error("backup settings changed before restore");
        }
        auto expectedSettings = std::find_if(inspection.manifest.entries.begin(),
            inspection.manifest.entries.end(), [](auto const& entry) {
                return !entry.directory && entry.relative ==
                    fs::path(L"Config") / L"settings.json";
            });
        if (expectedSettings == inspection.manifest.entries.end() ||
            hash_file(backupSettingsPath) != expectedSettings->digest) {
            throw std::runtime_error("backup settings no longer match the manifest");
        }
        motion::Settings restoredSettings;
        auto settingsStatus = motion::load_settings_file(
            backupSettingsPath, restoredSettings);
        if (settingsStatus != motion::SettingsFileStatus::valid &&
            settingsStatus != motion::SettingsFileStatus::libraryUnavailable) {
            throw std::runtime_error("backup settings schema is unsupported or corrupt");
        }
        if (hash_file(backupSettingsPath) != expectedSettings->digest) {
            throw std::runtime_error("backup settings changed while being read");
        }

        auto transactionId = motion::new_id();
        auto transactionName = motion::utf8_to_wide(transactionId);
        auto stageName = fs::path(restoreStagePrefix + transactionName);
        fs::path targetConfigured;
        fs::path targetStable;
        fs::path stageConfigured;
        fs::path stageStable;
        fs::path archiveConfigured;
        fs::path archiveStable;
        std::optional<StableDirectory> localRoot;
        if (expectedCurrentLibrary) {
            targetConfigured = currentLibraryPath;
            targetStable = expectedCurrentLibrary->stableRoot;
            stageConfigured = currentLibraryPath.parent_path() / stageName;
            stageStable = expectedCurrentLibrary->stableRoot.parent_path() / stageName;
            auto archiveName = currentLibraryPath.filename().wstring() +
                L".MotionWallpaper-before-restore-" +
                motion::utf8_to_wide(inspection.info.backupId) + L"-" + transactionName;
            archiveConfigured = currentLibraryPath.parent_path() / archiveName;
            archiveStable = expectedCurrentLibrary->stableRoot.parent_path() / archiveName;
        } else {
            localRoot = capture_stable_directory(dataRoot);
            if (!localRoot) {
                throw std::runtime_error("application data root identity is unsafe");
            }
            auto targetName = restored_library_name(transactionId);
            targetConfigured = dataRoot / targetName;
            targetStable = localRoot->stablePath / targetName;
            stageConfigured = dataRoot / stageName;
            stageStable = localRoot->stablePath / stageName;
        }
        std::error_code error;
        bool stageOccupied = fs::exists(stageStable, error) || error;
        error.clear();
        bool targetConflict = !expectedCurrentLibrary &&
            (fs::exists(targetStable, error) || error);
        error.clear();
        bool archiveOccupied = !archiveStable.empty() &&
            (fs::exists(archiveStable, error) || error);
        if (stageOccupied || targetConflict || archiveOccupied) {
            throw std::runtime_error("restore destination is already occupied");
        }

        report(progress, LibraryBackupPhase::PreparingRestore, 0,
            inspection.info.totalBytes, 0, inspection.info.fileCount);
        auto transaction = LibraryMigrationTransaction::Begin(
            backupIdentity->root, stageStable, *backupIdentity);
        transaction->CopyAndVerify([&](uint64_t copied, uint64_t total) {
            report(progress, LibraryBackupPhase::PreparingRestore, copied, total,
                0, inspection.info.fileCount, L"Library");
        }, cancelled);
        transaction->CommitPreparedTarget();
        transaction->MarkActivated();
        transaction.reset();
        auto stagedIdentity = motion::capture_media_library_trust(stageConfigured);
        if (!stagedIdentity) {
            stagedIdentity = motion::capture_media_library_trust(stageStable);
        }
        if (!stagedIdentity ||
            stagedIdentity->ownershipId != inspection.info.libraryId ||
            !motion::revalidate_media_library_trust(*stagedIdentity)) {
            throw std::runtime_error("prepared restore library failed validation");
        }
        PreparedLibraryGuard preparedLibraryCleanup(stageStable, *stagedIdentity);
        throw_cancelled(cancelled);

        bool defaultLibrary = expectedCurrentLibrary &&
            current_library_is_default(dataRoot, currentLibraryPath);
        restoredSettings.mediaLibraryPath = defaultLibrary
            ? std::wstring{} : targetConfigured.wstring();
        restoredSettings.mediaLibraryId = defaultLibrary
            ? std::string{} : stagedIdentity->ownershipId;

        auto config = capture_stable_directory(dataRoot / L"Config");
        if (!config) throw std::runtime_error("Config directory identity is unsafe");
        auto settingsStable = config->stablePath / L"settings.json";
        auto settingsConfigured = config->configuredPath / L"settings.json";
        motion::unique_handle activeSettings(CreateFileW(settingsStable.c_str(),
            FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!activeSettings || !direct_handle_identity(activeSettings.get(), false)) {
            throw std::runtime_error("active settings file is unsafe");
        }
        auto preparedSettingsName = fs::path(
            restoreSettingsPrefix + transactionName + L".json");
        auto previousSettingsName = fs::path(
            restorePreviousSettingsPrefix + transactionName + L".json");
        auto preparedSettingsStable = config->stablePath / preparedSettingsName;
        auto preparedSettingsConfigured = config->configuredPath / preparedSettingsName;
        auto previousSettingsStable = config->stablePath / previousSettingsName;
        auto previousSettingsConfigured = config->configuredPath / previousSettingsName;
        TemporaryFileGuard preparedSettingsCleanup(preparedSettingsStable);
        motion::save_settings(preparedSettingsStable, restoredSettings);
        motion::Settings preparedRoundTrip;
        auto preparedStatus = motion::load_settings_file(
            preparedSettingsStable, preparedRoundTrip);
        if ((preparedStatus != motion::SettingsFileStatus::libraryUnavailable &&
                preparedStatus != motion::SettingsFileStatus::valid) ||
            !missing_direct_object(previousSettingsStable)) {
            throw std::runtime_error("prepared restored settings failed validation");
        }
        auto restoredSettingsDigest = hash_file(preparedSettingsStable);

        // All expensive/fallible preparation is complete. Acquire exact
        // DELETE-capable handles, then durably publish enough identity and path
        // information for startup to infer an interrupted atomic rename.
        currentTrust.reset();
        motion::unique_handle currentHandle;
        if (expectedCurrentLibrary) {
            currentHandle = open_verified_library_for_rename(*expectedCurrentLibrary);
        }
        auto stagedHandle = open_verified_library_for_rename(*stagedIdentity);
        if ((expectedCurrentLibrary && !currentHandle) || !stagedHandle ||
            !config->Revalidate()) {
            throw std::runtime_error("library identity changed before restore commit");
        }
        protectedBackupSettings.reset();
        activeSettings.reset();

        RestoreJournal journal;
        journal.transactionId = transactionId;
        journal.backupId = inspection.info.backupId;
        journal.replacesExistingLibrary = expectedCurrentLibrary.has_value();
        journal.activeLibraryPath = targetConfigured;
        journal.stagedLibraryPath = stageConfigured;
        journal.archivedLibraryPath = archiveConfigured;
        journal.preparedSettingsPath = preparedSettingsConfigured;
        journal.previousSettingsPath = previousSettingsConfigured;
        if (expectedCurrentLibrary) {
            journal.previousLibraryId = expectedCurrentLibrary->ownershipId;
            journal.previousLibraryIdentity = expectedCurrentLibrary->rootIdentity;
        }
        journal.restoredLibraryId = stagedIdentity->ownershipId;
        journal.restoredLibraryIdentity = stagedIdentity->rootIdentity;
        journal.restoredSettingsDigest = restoredSettingsDigest;

        bool journalMayExist{};
        try {
            persist_restore_journal(dataRoot, journal, true);
            journalMayExist = true;
            preparedLibraryCleanup.Release();
            preparedSettingsCleanup.Release();
            inject_restore_crash(crashPoint,
                LibraryRestoreCrashPoint::JournalPrepared);

            report(progress, LibraryBackupPhase::SwappingLibrary,
                inspection.info.totalBytes, inspection.info.totalBytes,
                inspection.info.fileCount, inspection.info.fileCount);
            if (expectedCurrentLibrary) {
                if (!rename_open_directory(currentHandle.get(), archiveStable)) {
                    throw std::runtime_error("cannot preserve active library before restore");
                }
                inject_restore_crash(crashPoint,
                    LibraryRestoreCrashPoint::PreviousLibraryRenamed);
                journal.phase = RestoreJournalPhase::PreviousLibraryArchived;
                persist_restore_journal(dataRoot, journal, false);
            }
            if (!rename_open_directory(stagedHandle.get(), targetStable)) {
                throw std::runtime_error("cannot activate prepared restore library");
            }
            inject_restore_crash(crashPoint,
                LibraryRestoreCrashPoint::RestoredLibraryRenamed);
            journal.phase = RestoreJournalPhase::LibraryActivated;
            persist_restore_journal(dataRoot, journal, false);

            auto activatedIdentity = motion::capture_media_library_trust(targetConfigured);
            if (!activatedIdentity || !trust_matches(*activatedIdentity,
                    journal.restoredLibraryId, journal.restoredLibraryIdentity)) {
                throw std::runtime_error("activated restore library identity is invalid");
            }
            report(progress, LibraryBackupPhase::CommittingSettings,
                inspection.info.totalBytes, inspection.info.totalBytes,
                inspection.info.fileCount, inspection.info.fileCount);
            journal.phase = RestoreJournalPhase::SettingsCommitPending;
            persist_restore_journal(dataRoot, journal, false);
            inject_restore_crash(crashPoint,
                LibraryRestoreCrashPoint::SettingsCommitIntentPersisted);
            if (!ReplaceFileW(settingsStable.c_str(), preparedSettingsStable.c_str(),
                    previousSettingsStable.c_str(), REPLACEFILE_WRITE_THROUGH |
                        REPLACEFILE_IGNORE_MERGE_ERRORS |
                        REPLACEFILE_IGNORE_ACL_ERRORS,
                    nullptr, nullptr)) {
                throw std::system_error(static_cast<int>(GetLastError()),
                    std::system_category(),
                    "cannot atomically commit restored settings");
            }
            inject_restore_crash(crashPoint,
                LibraryRestoreCrashPoint::SettingsReplaced);
            journal.phase = RestoreJournalPhase::SettingsCommitted;
            persist_restore_journal(dataRoot, journal, false);
            inject_restore_crash(crashPoint,
                LibraryRestoreCrashPoint::CommitRecorded);
            remove_restore_journal(dataRoot, journal.transactionId);

            LibraryRestoreResult result;
            result.backup = inspection.info;
            result.restoredSettings = restoredSettings;
            result.restoredLibraryIdentity = std::move(*activatedIdentity);
            result.previousLibraryPath = archiveConfigured;
            result.previousSettingsPath = previousSettingsConfigured;
            try {
                report(progress, LibraryBackupPhase::Completed,
                    result.backup.totalBytes, result.backup.totalBytes,
                    result.backup.fileCount, result.backup.fileCount);
            } catch (...) {}
            return result;
        } catch (simulated_restore_crash const&) {
            // Deliberately model abrupt process death: do not run in-process
            // compensation and leave the durable transaction for startup.
            if (!journalMayExist && HasPendingRestore(dataRoot)) {
                preparedLibraryCleanup.Release();
                preparedSettingsCleanup.Release();
            }
            throw;
        } catch (...) {
            auto failure = std::current_exception();
            currentHandle.reset();
            stagedHandle.reset();
            if (!journalMayExist && HasPendingRestore(dataRoot)) {
                journalMayExist = true;
                preparedLibraryCleanup.Release();
                preparedSettingsCleanup.Release();
            }
            if (journalMayExist) {
                try {
                    auto recovered = recover_pending_restore(dataRoot);
                    if (recovered.outcome == LibraryRestoreRecoveryOutcome::Completed) {
                        auto activatedIdentity = motion::capture_media_library_trust(
                            targetConfigured);
                        if (!activatedIdentity || !trust_matches(*activatedIdentity,
                                journal.restoredLibraryId,
                                journal.restoredLibraryIdentity)) {
                            throw std::runtime_error(
                                "completed restore identity could not be reopened");
                        }
                        LibraryRestoreResult result;
                        result.backup = inspection.info;
                        result.restoredSettings = restoredSettings;
                        result.restoredLibraryIdentity = std::move(*activatedIdentity);
                        result.previousLibraryPath = archiveConfigured;
                        result.previousSettingsPath = previousSettingsConfigured;
                        return result;
                    }
                } catch (...) {
                    throw std::runtime_error(
                        "restore failed and durable recovery remains pending");
                }
            }
            std::rethrow_exception(failure);
        }
    }
}
