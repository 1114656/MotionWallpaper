#include <windows.h>
#include <bcrypt.h>

#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "LibraryMigration.h"
#include "../MotionWallpaper.Common/Common.h"

namespace fs = std::filesystem;

namespace
{
    constexpr wchar_t transactionMarkerName[] = L".motionwallpaper-migration-owner";
    constexpr wchar_t transactionStateName[] = L".motionwallpaper-migration-state";
    constexpr char transactionMarkerPrefix[] = "MotionWallpaper.Migration/v2\n";
    constexpr char transactionStatePrefix[] = "MotionWallpaper.MigrationState/v1\n";

    enum class MigrationPhase
    {
        copying,
        prepared,
        committing,
        committed,
    };

    struct ManifestEntry
    {
        fs::path relative;
        bool directory{};
        uint64_t bytes{};
        std::array<uint8_t, 32> digest{};
        bool digestKnown{};
    };

    struct MigrationOwner
    {
        std::string transactionId;
        std::string libraryId;
        motion::FilesystemObjectIdentity sourceIdentity;
        bool operator==(MigrationOwner const&) const = default;
    };

    struct MigrationState
    {
        MigrationOwner owner;
        MigrationPhase phase{};
        std::vector<ManifestEntry> manifest;
    };

    class bcrypt_algorithm
    {
    public:
        ~bcrypt_algorithm() { if (value_) BCryptCloseAlgorithmProvider(value_, 0); }
        BCRYPT_ALG_HANDLE* put() noexcept { return &value_; }
        BCRYPT_ALG_HANDLE get() const noexcept { return value_; }
    private:
        BCRYPT_ALG_HANDLE value_{};
    };

    class bcrypt_hash
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

    bool is_reparse_point(fs::path const& path)
    {
        auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
        }
        return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    }

    std::string read_small_file(fs::path const& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot read library ownership marker");
        std::string value((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (input.bad() || value.size() > 256) throw std::runtime_error("invalid library ownership marker");
        return value;
    }

    void write_small_file_new(fs::path const& path, std::string const& value)
    {
        motion::unique_handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr));
        if (!file) throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
        DWORD written{};
        if (value.size() > MAXDWORD || !WriteFile(file.get(), value.data(), static_cast<DWORD>(value.size()), &written, nullptr) ||
            written != value.size() || !FlushFileBuffers(file.get())) {
            auto error = GetLastError();
            file.reset();
            std::error_code ignored;
            fs::remove(path, ignored);
            throw std::system_error(static_cast<int>(error ? error : ERROR_WRITE_FAULT), std::system_category());
        }
    }

    std::string read_control_file(fs::path const& path, size_t maximumBytes)
    {
        motion::unique_handle input(CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!input) throw std::runtime_error("cannot read migration control file");
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(input.get(), &information) ||
            (information.dwFileAttributes &
                (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
            throw std::runtime_error("invalid migration control file");
        }
        ULARGE_INTEGER size{};
        size.HighPart = information.nFileSizeHigh;
        size.LowPart = information.nFileSizeLow;
        if (size.QuadPart > maximumBytes) {
            throw std::runtime_error("migration control file is too large");
        }
        std::string value;
        value.reserve(static_cast<size_t>(size.QuadPart));
        std::array<char, 64 * 1024> buffer{};
        for (;;) {
            DWORD count{};
            if (!ReadFile(input.get(), buffer.data(),
                static_cast<DWORD>(buffer.size()), &count, nullptr)) {
                throw std::runtime_error("cannot read migration control file");
            }
            if (!count) break;
            if (static_cast<size_t>(count) > maximumBytes ||
                value.size() > maximumBytes - static_cast<size_t>(count)) {
                throw std::runtime_error("migration control file is too large");
            }
            value.append(buffer.data(), count);
        }
        return value;
    }

    void write_control_file(fs::path const& path, std::string const& value, bool replace)
    {
        auto temporary = path;
        temporary += L".new-" + motion::utf8_to_wide(motion::new_id());
        try {
            motion::unique_handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0,
                nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr));
            if (!file) throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
            size_t offset{};
            while (offset < value.size()) {
                auto remaining = value.size() - offset;
                auto requested = static_cast<DWORD>((std::min)(remaining,
                    static_cast<size_t>(MAXDWORD)));
                DWORD written{};
                if (!WriteFile(file.get(), value.data() + offset, requested, &written, nullptr) ||
                    written != requested) {
                    throw std::system_error(static_cast<int>(GetLastError() ? GetLastError() : ERROR_WRITE_FAULT),
                        std::system_category());
                }
                offset += written;
            }
            if (!FlushFileBuffers(file.get())) {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
            }
            file.reset();
            DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
            if (!MoveFileExW(temporary.c_str(), path.c_str(), flags)) {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
            }
        } catch (...) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            throw;
        }
    }

    std::string marker_value(char const* prefix, std::string const& id)
    {
        return std::string(prefix) + id + "\n";
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
            std::copy(std::begin(fileId.FileId.Identifier), std::end(fileId.FileId.Identifier),
                identity.fileId.begin());
        } else {
            identity.volumeSerialNumber = information.dwVolumeSerialNumber;
            uint64_t fallbackId =
                (static_cast<uint64_t>(information.nFileIndexHigh) << 32) |
                information.nFileIndexLow;
            std::memcpy(identity.fileId.data(), &fallbackId, sizeof(fallbackId));
        }
        return identity;
    }

    struct StableDirectoryIdentity
    {
        fs::path stablePath;
        motion::FilesystemObjectIdentity identity;
        motion::unique_handle aliasHandle;
        motion::unique_handle stableHandle;
    };

    std::optional<fs::path> stable_directory_path(HANDLE handle) noexcept
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
                    prefix.data(), static_cast<int>(prefix.size()), TRUE) !=
                    CSTR_EQUAL ||
                value.find(L"}\\", prefix.size()) == std::wstring::npos) {
                return std::nullopt;
            }
            fs::path stable(value);
            return stable.is_absolute() && stable.has_filename()
                ? std::optional<fs::path>(std::move(stable)) : std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
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

    std::optional<StableDirectoryIdentity> capture_stable_directory(
        fs::path const& configuredPath) noexcept
    {
        try {
            auto alias = open_direct_directory(configuredPath, true);
            if (!alias) return std::nullopt;
            auto identity = direct_handle_identity(alias.get(), true);
            auto stablePath = stable_directory_path(alias.get());
            if (!identity || !stablePath) return std::nullopt;
            auto stable = open_direct_directory(*stablePath, true);
            auto stableIdentity = direct_handle_identity(stable.get(), true);
            if (!stable || !stableIdentity || *stableIdentity != *identity) {
                return std::nullopt;
            }
            auto aliasAgain = open_direct_directory(configuredPath, false);
            auto aliasAgainIdentity = direct_handle_identity(aliasAgain.get(), true);
            if (!aliasAgain || !aliasAgainIdentity || *aliasAgainIdentity != *identity) {
                return std::nullopt;
            }
            return StableDirectoryIdentity{ std::move(*stablePath), *identity,
                std::move(alias), std::move(stable) };
        } catch (...) {
            return std::nullopt;
        }
    }

    bool path_has_directory_identity(fs::path const& path,
        motion::FilesystemObjectIdentity const& expected) noexcept
    {
        auto object = open_direct_directory(path, false);
        auto identity = direct_handle_identity(object.get(), true);
        return object && identity && *identity == expected;
    }

    motion::unique_handle open_verified_archive_root(
        motion::MediaLibraryTrustIdentity const& expected) noexcept
    {
        try {
            motion::unique_handle root(CreateFileW(expected.stableRoot.c_str(),
                DELETE | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!root) return {};
            auto rootIdentity = direct_handle_identity(root.get(), true);
            if (!rootIdentity || *rootIdentity != expected.rootIdentity) {
                SetLastError(ERROR_INVALID_DATA);
                return {};
            }

            // While this DELETE-capable root handle denies another rename,
            // prove that the path, marker contents, and Groups directory still
            // resolve to the identity captured before Agent quiescence.
            auto current = motion::capture_media_library_trust(expected.root);
            if (!current || current->ownershipId != expected.ownershipId ||
                current->rootIdentity != expected.rootIdentity ||
                current->markerIdentity != expected.markerIdentity ||
                current->groupsIdentity != expected.groupsIdentity) {
                SetLastError(ERROR_INVALID_DATA);
                return {};
            }
            rootIdentity = direct_handle_identity(root.get(), true);
            if (!rootIdentity || *rootIdentity != expected.rootIdentity) {
                SetLastError(ERROR_INVALID_DATA);
                return {};
            }
            return root;
        } catch (...) {
            SetLastError(ERROR_INVALID_DATA);
            return {};
        }
    }

    bool rename_open_directory(HANDLE directory, fs::path const& destination) noexcept
    {
        try {
            auto normalized = fs::absolute(destination).lexically_normal();
            auto fileName = normalized.wstring();
            if (fileName.empty() ||
                fileName.size() > (std::numeric_limits<DWORD>::max)() / sizeof(wchar_t)) {
                SetLastError(ERROR_INVALID_NAME);
                return false;
            }
            size_t bytes = sizeof(FILE_RENAME_INFO) +
                fileName.size() * sizeof(wchar_t);
            if (bytes > (std::numeric_limits<DWORD>::max)()) {
                SetLastError(ERROR_BUFFER_OVERFLOW);
                return false;
            }
            std::vector<unsigned char> storage(bytes);
            auto information = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
            information->ReplaceIfExists = FALSE;
            information->RootDirectory = nullptr;
            information->FileNameLength = static_cast<DWORD>(
                fileName.size() * sizeof(wchar_t));
            std::memcpy(information->FileName, fileName.data(),
                information->FileNameLength);
            return SetFileInformationByHandle(directory, FileRenameInfo,
                information, static_cast<DWORD>(bytes)) != FALSE;
        } catch (...) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return false;
        }
    }

    void validate_no_reparse_tree(fs::path const& root)
    {
        if (is_reparse_point(root)) throw std::runtime_error("library root cannot be a reparse point");
        std::error_code error;
        for (fs::recursive_directory_iterator entries(root, fs::directory_options::none, error), end;
            !error && entries != end; entries.increment(error)) {
            if (is_reparse_point(entries->path())) {
                throw std::runtime_error("library contains an unsupported reparse point");
            }
            std::error_code typeError;
            if (!entries->is_directory(typeError) && !entries->is_regular_file(typeError)) {
                throw std::runtime_error("library contains an unsupported filesystem object");
            }
            if (typeError) throw std::system_error(typeError);
        }
        if (error) throw std::system_error(error);
    }

    bool is_owned_media_control_file(fs::path const& name)
    {
        return name == L".optimization-request" || name == L".optimization-cancelled" ||
            name == L".optimization-paused" || name == L".optimization-failed" ||
            name == L".optimization-progress" ||
            name == L".optimization-suppressed-balanced" ||
            name == L".optimization-suppressed-power-saver";
    }

    bool is_owned_variant_file(fs::path const& name)
    {
        auto value = name.wstring();
        return motion::safe_file_name(name) && value.ends_with(L".mp4") &&
            (value.starts_with(L"balanced-") || value.starts_with(L"power-saver-") ||
                value.starts_with(L"cpu-smooth-"));
    }

    void validate_media_directory(fs::path const& directory, motion::MediaMetadata const& media)
    {
        std::error_code error;
        for (fs::directory_iterator children(directory, error), end;
            !error && children != end; children.increment(error)) {
            auto name = children->path().filename();
            if ((name == L"metadata.json" || name == media.fileName ||
                (!media.coverFileName.empty() && name == media.coverFileName) ||
                name == L"poster.png" || is_owned_media_control_file(name)) &&
                children->is_regular_file()) {
                continue;
            }
            if (name == L"Variants" && children->is_directory()) {
                std::error_code variantsError;
                for (fs::directory_iterator variants(children->path(), variantsError), variantsEnd;
                    !variantsError && variants != variantsEnd; variants.increment(variantsError)) {
                    if (!variants->is_regular_file() ||
                        !is_owned_variant_file(variants->path().filename())) {
                        throw std::runtime_error("media Variants directory contains unowned data");
                    }
                }
                if (variantsError) throw std::system_error(variantsError);
                continue;
            }
            throw std::runtime_error("media directory contains data not owned by MotionWallpaper");
        }
        if (error) throw std::system_error(error);
    }

    void validate_library_structure(fs::path const& source, bool markerRequired)
    {
        std::error_code error;
        if (!fs::is_directory(source, error) || error || !source.has_filename()) {
            throw std::runtime_error("source is not a library directory");
        }
        validate_no_reparse_tree(source);

        auto groupsRoot = source / L"Groups";
        if (!fs::is_directory(groupsRoot, error) || error) {
            throw std::runtime_error("library Groups directory is missing");
        }
        bool hasGroup{};
        for (fs::directory_iterator entries(source, error), end; !error && entries != end; entries.increment(error)) {
            auto name = entries->path().filename();
            if (name == L"Groups" && entries->is_directory()) continue;
            if (name == motion::media_library_ownership_marker_name && entries->is_regular_file()) continue;
            throw std::runtime_error("library root contains data not owned by MotionWallpaper");
        }
        if (error) throw std::system_error(error);
        if (markerRequired && !motion::media_library_ownership_id(source)) {
            throw std::runtime_error("invalid library ownership marker");
        }

        for (fs::directory_iterator groupEntries(groupsRoot, error), end;
            !error && groupEntries != end; groupEntries.increment(error)) {
            if (!groupEntries->is_directory()) throw std::runtime_error("invalid item in Groups directory");
            auto groupId = motion::wide_to_utf8(groupEntries->path().filename().wstring());
            if (!motion::valid_id(groupId)) throw std::runtime_error("invalid group directory id");
            auto group = motion::load_group(groupEntries->path() / L"group.json");
            if (!group || group->id != groupId) throw std::runtime_error("group metadata does not match its directory");
            auto videos = groupEntries->path() / L"Videos";
            if (!fs::is_directory(videos, error) || error) throw std::runtime_error("group Videos directory is missing");

            for (fs::directory_iterator groupChildren(groupEntries->path(), error), groupEnd;
                !error && groupChildren != groupEnd; groupChildren.increment(error)) {
                auto name = groupChildren->path().filename();
                if (name == L"group.json" && groupChildren->is_regular_file()) continue;
                if (name == L"Videos" && groupChildren->is_directory()) continue;
                throw std::runtime_error("group directory contains unowned data");
            }
            if (error) throw std::system_error(error);

            for (fs::directory_iterator mediaEntries(videos, error), mediaEnd;
                !error && mediaEntries != mediaEnd; mediaEntries.increment(error)) {
                if (!mediaEntries->is_directory()) throw std::runtime_error("invalid item in Videos directory");
                auto mediaId = motion::wide_to_utf8(mediaEntries->path().filename().wstring());
                if (!motion::valid_id(mediaId)) throw std::runtime_error("invalid media directory id");
                auto media = motion::load_media(mediaEntries->path() / L"metadata.json");
                if (!media || media->id != mediaId || media->groupId != groupId ||
                    !motion::safe_file_name(media->fileName) ||
                    (!media->coverFileName.empty() && !motion::safe_file_name(media->coverFileName))) {
                    throw std::runtime_error("media metadata does not match its directory");
                }
                validate_media_directory(mediaEntries->path(), *media);
            }
            if (error) throw std::system_error(error);
            hasGroup = true;
        }
        if (error) throw std::system_error(error);
        if (!hasGroup) throw std::runtime_error("library contains no valid groups");
    }

    std::array<uint8_t, 32> hash_file(fs::path const& path)
    {
        bcrypt_algorithm algorithm;
        bcrypt_hash hash;
        DWORD objectLength{}, resultLength{};
        require_nt(BCryptOpenAlgorithmProvider(algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0),
            "cannot open SHA-256 provider");
        require_nt(BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &resultLength, 0),
            "cannot query SHA-256 provider");
        std::vector<uint8_t> object(objectLength);
        std::array<uint8_t, 32> digest{};
        require_nt(BCryptCreateHash(algorithm.get(), hash.put(), object.data(),
            static_cast<ULONG>(object.size()), nullptr, 0, 0), "cannot create SHA-256 hash");
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open migration file for verification");
        std::vector<uint8_t> buffer(4 * 1024 * 1024);
        while (input) {
            input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            auto count = input.gcount();
            if (count > 0) require_nt(BCryptHashData(hash.get(), buffer.data(), static_cast<ULONG>(count), 0),
                "cannot update SHA-256 hash");
        }
        if (!input.eof()) throw std::runtime_error("cannot read migration file for verification");
        require_nt(BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0),
            "cannot finish SHA-256 hash");
        return digest;
    }

    std::array<uint8_t, 32> copy_and_hash(fs::path const& source, fs::path const& destination,
        uint64_t& copiedBytes, uint64_t totalBytes, motion::app::LibraryMigrationProgress const& progress,
        std::atomic_bool const* cancelled)
    {
        bcrypt_algorithm algorithm;
        bcrypt_hash hash;
        DWORD objectLength{}, resultLength{};
        require_nt(BCryptOpenAlgorithmProvider(algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0),
            "cannot open SHA-256 provider");
        require_nt(BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &resultLength, 0),
            "cannot query SHA-256 provider");
        std::vector<uint8_t> object(objectLength);
        std::array<uint8_t, 32> digest{};
        require_nt(BCryptCreateHash(algorithm.get(), hash.put(), object.data(),
            static_cast<ULONG>(object.size()), nullptr, 0, 0), "cannot create SHA-256 hash");

        std::ifstream input(source, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open migration source");
        motion::unique_handle output(CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!output) throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
        std::vector<uint8_t> buffer(4 * 1024 * 1024);
        while (input) {
            if (cancelled && cancelled->load(std::memory_order_acquire)) {
                throw std::runtime_error("library migration cancelled");
            }
            input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            auto count = input.gcount();
            if (count <= 0) continue;
            require_nt(BCryptHashData(hash.get(), buffer.data(), static_cast<ULONG>(count), 0),
                "cannot update SHA-256 hash");
            DWORD written{};
            if (!WriteFile(output.get(), buffer.data(), static_cast<DWORD>(count), &written, nullptr) ||
                written != static_cast<DWORD>(count)) {
                auto writeError = GetLastError();
                throw std::system_error(static_cast<int>(writeError ? writeError : ERROR_WRITE_FAULT),
                    std::system_category());
            }
            copiedBytes += static_cast<uint64_t>(count);
            if (progress) progress(copiedBytes, totalBytes);
        }
        if (!input.eof()) throw std::runtime_error("cannot read migration source");
        if (!FlushFileBuffers(output.get())) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
        }
        require_nt(BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0),
            "cannot finish SHA-256 hash");
        return digest;
    }

    void copy_file_metadata(fs::path const& source, fs::path const& destination)
    {
        motion::unique_handle sourceHandle(CreateFileW(source.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr));
        motion::unique_handle destinationHandle(CreateFileW(destination.c_str(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!sourceHandle || !destinationHandle) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
        }
        FILETIME created{}, accessed{}, modified{};
        if (!GetFileTime(sourceHandle.get(), &created, &accessed, &modified) ||
            !SetFileTime(destinationHandle.get(), &created, &accessed, &modified)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
        }
        auto attributes = GetFileAttributesW(source.c_str());
        constexpr DWORD preservedAttributes = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN |
            FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM;
        attributes = attributes == INVALID_FILE_ATTRIBUTES ? attributes : attributes & preservedAttributes;
        if (attributes == 0) attributes = FILE_ATTRIBUTE_NORMAL;
        if (attributes == INVALID_FILE_ATTRIBUTES || !SetFileAttributesW(destination.c_str(), attributes)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
        }
    }

    std::vector<ManifestEntry> build_manifest(fs::path const& source, bool includeHashes)
    {
        std::vector<ManifestEntry> manifest;
        std::error_code error;
        for (fs::recursive_directory_iterator entries(source, fs::directory_options::none, error), end;
            !error && entries != end; entries.increment(error)) {
            auto relative = fs::relative(entries->path(), source, error);
            if (error || relative.empty() || relative.is_absolute() || relative.native().find(L"..") == 0) {
                throw std::runtime_error("unsafe path in library manifest");
            }
            ManifestEntry item;
            item.relative = std::move(relative);
            item.directory = entries->is_directory(error);
            if (error) throw std::system_error(error);
            if (!item.directory) {
                if (!entries->is_regular_file(error) || error) throw std::runtime_error("unsupported library item");
                item.bytes = entries->file_size(error);
                if (error) throw std::system_error(error);
                if (includeHashes) {
                    item.digest = hash_file(entries->path());
                    item.digestKnown = true;
                }
            }
            manifest.push_back(std::move(item));
        }
        if (error) throw std::system_error(error);
        std::sort(manifest.begin(), manifest.end(), [](auto const& left, auto const& right) {
            return left.relative.generic_wstring() < right.relative.generic_wstring();
        });
        return manifest;
    }

    bool same_manifest(std::vector<ManifestEntry> const& left, std::vector<ManifestEntry> const& right)
    {
        if (left.size() != right.size()) return false;
        for (size_t index = 0; index < left.size(); ++index) {
            auto const& a = left[index];
            auto const& b = right[index];
            if (a.relative != b.relative || a.directory != b.directory || a.bytes != b.bytes ||
                (!a.directory && (!a.digestKnown || !b.digestKnown || a.digest != b.digest))) return false;
        }
        return true;
    }

    char hex_digit(unsigned value) noexcept
    {
        return static_cast<char>(value < 10 ? '0' + value : 'a' + value - 10);
    }

    int hex_value(char value) noexcept
    {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        return -1;
    }

    template <size_t Size>
    std::string bytes_to_hex(std::array<uint8_t, Size> const& value)
    {
        std::string result;
        result.reserve(Size * 2);
        for (auto byte : value) {
            result.push_back(hex_digit(byte >> 4));
            result.push_back(hex_digit(byte & 0x0f));
        }
        return result;
    }

    template <size_t Size>
    bool hex_to_bytes(std::string_view value, std::array<uint8_t, Size>& result) noexcept
    {
        if (value.size() != Size * 2) return false;
        for (size_t index = 0; index < Size; ++index) {
            auto high = hex_value(value[index * 2]);
            auto low = hex_value(value[index * 2 + 1]);
            if (high < 0 || low < 0) return false;
            result[index] = static_cast<uint8_t>((high << 4) | low);
        }
        return true;
    }

    std::string path_to_hex(fs::path const& path)
    {
        static_assert(sizeof(wchar_t) == sizeof(uint16_t));
        auto value = path.generic_wstring();
        std::string result;
        result.reserve(value.size() * 4);
        for (auto character : value) {
            auto unit = static_cast<uint16_t>(character);
            result.push_back(hex_digit((unit >> 12) & 0xf));
            result.push_back(hex_digit((unit >> 8) & 0xf));
            result.push_back(hex_digit((unit >> 4) & 0xf));
            result.push_back(hex_digit(unit & 0xf));
        }
        return result;
    }

    std::optional<fs::path> path_from_hex(std::string_view value) noexcept
    {
        try {
            if (value.empty() || value.size() % 4 != 0) return std::nullopt;
            std::wstring decoded;
            decoded.reserve(value.size() / 4);
            for (size_t offset = 0; offset < value.size(); offset += 4) {
                uint16_t unit{};
                for (size_t index = 0; index < 4; ++index) {
                    auto nibble = hex_value(value[offset + index]);
                    if (nibble < 0) return std::nullopt;
                    unit = static_cast<uint16_t>((unit << 4) | nibble);
                }
                if (!unit) return std::nullopt;
                decoded.push_back(static_cast<wchar_t>(unit));
            }
            fs::path result(decoded);
            if (result.empty() || result.is_absolute()) return std::nullopt;
            for (auto const& component : result) {
                if (component.empty() || component == L"." || component == L"..") return std::nullopt;
            }
            return result;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::vector<std::string_view> control_lines(std::string const& value)
    {
        std::vector<std::string_view> result;
        size_t begin{};
        while (begin < value.size()) {
            auto end = value.find('\n', begin);
            if (end == std::string::npos) throw std::runtime_error("unterminated migration control file");
            auto line = std::string_view(value).substr(begin, end - begin);
            if (line.find('\r') != std::string_view::npos) {
                throw std::runtime_error("invalid migration control file");
            }
            result.push_back(line);
            begin = end + 1;
        }
        return result;
    }

    bool parse_uint64(std::string_view value, uint64_t& result) noexcept
    {
        if (value.empty()) return false;
        auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
        return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
    }

    std::string serialize_owner(MigrationOwner const& owner)
    {
        return std::string(transactionMarkerPrefix) + owner.transactionId + "\n" +
            owner.libraryId + "\n" + std::to_string(owner.sourceIdentity.volumeSerialNumber) + "\n" +
            bytes_to_hex(owner.sourceIdentity.fileId) + "\n";
    }

    std::optional<MigrationOwner> parse_owner(std::string const& value) noexcept
    {
        try {
            auto lines = control_lines(value);
            if (lines.size() != 5 || lines[0] !=
                std::string_view(transactionMarkerPrefix, std::strlen(transactionMarkerPrefix) - 1)) {
                return std::nullopt;
            }
            MigrationOwner result;
            result.transactionId.assign(lines[1]);
            result.libraryId.assign(lines[2]);
            if (!motion::valid_id(result.transactionId) || !motion::valid_id(result.libraryId) ||
                !parse_uint64(lines[3], result.sourceIdentity.volumeSerialNumber) ||
                !hex_to_bytes(lines[4], result.sourceIdentity.fileId)) {
                return std::nullopt;
            }
            return result;
        } catch (...) {
            return std::nullopt;
        }
    }

    char const* phase_name(MigrationPhase phase) noexcept
    {
        switch (phase) {
        case MigrationPhase::copying: return "copying";
        case MigrationPhase::prepared: return "prepared";
        case MigrationPhase::committing: return "committing";
        case MigrationPhase::committed: return "committed";
        }
        return "invalid";
    }

    std::optional<MigrationPhase> parse_phase(std::string_view value) noexcept
    {
        if (value == "copying") return MigrationPhase::copying;
        if (value == "prepared") return MigrationPhase::prepared;
        if (value == "committing") return MigrationPhase::committing;
        if (value == "committed") return MigrationPhase::committed;
        return std::nullopt;
    }

    std::string serialize_state(MigrationOwner const& owner, MigrationPhase phase,
        std::vector<ManifestEntry> const& manifest)
    {
        if (manifest.size() > 500000) {
            throw std::runtime_error("migration manifest has too many entries");
        }
        std::string result = transactionStatePrefix;
        result += owner.transactionId + "\n" + owner.libraryId + "\n" +
            std::to_string(owner.sourceIdentity.volumeSerialNumber) + "\n" +
            bytes_to_hex(owner.sourceIdentity.fileId) + "\n" + phase_name(phase) + "\n" +
            std::to_string(manifest.size()) + "\n";
        for (auto const& item : manifest) {
            if (!item.directory && !item.digestKnown) {
                throw std::runtime_error("migration manifest is missing a digest");
            }
            result += item.directory ? "D\t" : "F\t";
            result += path_to_hex(item.relative);
            if (!item.directory) {
                result += "\t" + std::to_string(item.bytes) + "\t" + bytes_to_hex(item.digest);
            }
            result += "\n";
            if (result.size() > 64 * 1024 * 1024) {
                throw std::runtime_error("migration manifest is too large");
            }
        }
        result += "end\n";
        if (result.size() > 64 * 1024 * 1024) {
            throw std::runtime_error("migration manifest is too large");
        }
        return result;
    }

    std::optional<MigrationState> parse_state(std::string const& value) noexcept
    {
        try {
            auto lines = control_lines(value);
            if (lines.size() < 8 || lines[0] !=
                std::string_view(transactionStatePrefix, std::strlen(transactionStatePrefix) - 1)) {
                return std::nullopt;
            }
            MigrationState result;
            result.owner.transactionId.assign(lines[1]);
            result.owner.libraryId.assign(lines[2]);
            auto phase = parse_phase(lines[5]);
            uint64_t count{};
            if (!motion::valid_id(result.owner.transactionId) ||
                !motion::valid_id(result.owner.libraryId) ||
                !parse_uint64(lines[3], result.owner.sourceIdentity.volumeSerialNumber) ||
                !hex_to_bytes(lines[4], result.owner.sourceIdentity.fileId) || !phase ||
                !parse_uint64(lines[6], count) || count > 500000 ||
                lines.size() != static_cast<size_t>(count) + 8 || lines.back() != "end") {
                return std::nullopt;
            }
            result.phase = *phase;
            result.manifest.reserve(static_cast<size_t>(count));
            for (size_t index = 0; index < count; ++index) {
                auto line = lines[index + 7];
                auto first = line.find('\t');
                if (first != 1 || (line[0] != 'D' && line[0] != 'F')) return std::nullopt;
                auto second = line.find('\t', first + 1);
                auto relative = path_from_hex(line.substr(first + 1,
                    second == std::string_view::npos ? line.size() - first - 1 : second - first - 1));
                if (!relative) return std::nullopt;
                ManifestEntry entry;
                entry.relative = std::move(*relative);
                entry.directory = line[0] == 'D';
                if (entry.directory) {
                    if (second != std::string_view::npos) return std::nullopt;
                } else {
                    if (second == std::string_view::npos) return std::nullopt;
                    auto third = line.find('\t', second + 1);
                    if (third == std::string_view::npos ||
                        !parse_uint64(line.substr(second + 1, third - second - 1), entry.bytes) ||
                        !hex_to_bytes(line.substr(third + 1), entry.digest)) {
                        return std::nullopt;
                    }
                    entry.digestKnown = true;
                }
                if (!result.manifest.empty() &&
                    result.manifest.back().relative.generic_wstring() >= entry.relative.generic_wstring()) {
                    return std::nullopt;
                }
                result.manifest.push_back(std::move(entry));
            }
            return result;
        } catch (...) {
            return std::nullopt;
        }
    }

    bool matches_manifest_file(fs::path const& path, ManifestEntry const& entry) noexcept
    {
        try {
            auto attributes = GetFileAttributesW(path.c_str());
            std::error_code error;
            return !entry.directory && entry.digestKnown && attributes != INVALID_FILE_ATTRIBUTES &&
                !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                fs::is_regular_file(path, error) && !error &&
                fs::file_size(path, error) == entry.bytes && !error && hash_file(path) == entry.digest;
        } catch (...) {
            return false;
        }
    }

    bool direct_directory_no_reparse(fs::path const& path) noexcept
    {
        auto attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
    }

    bool exact_marker_file(fs::path const& path, std::string const& expected) noexcept
    {
        try {
            auto attributes = GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES &&
                !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                read_small_file(path) == expected;
        } catch (...) {
            return false;
        }
    }

    bool safe_manifest_path(fs::path const& root, fs::path const& relative) noexcept
    {
        if (!direct_directory_no_reparse(root) || relative.empty() || relative.is_absolute()) return false;
        auto current = root;
        for (auto const& component : relative) {
            if (component.empty() || component == L"." || component == L"..") return false;
            current /= component;
            auto attributes = GetFileAttributesW(current.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        }
        return true;
    }

    void make_file_writable(fs::path const& path) noexcept
    {
        auto attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY)) {
            SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
        }
    }

    void erase_manifest_entry(std::vector<ManifestEntry>& manifest, fs::path const& relative)
    {
        auto found = std::find_if(manifest.begin(), manifest.end(), [&](auto const& entry) {
            return entry.relative == relative;
        });
        if (found == manifest.end()) throw std::runtime_error("migration ownership marker is missing");
        manifest.erase(found);
    }

    void erase_manifest_entry_if_present(std::vector<ManifestEntry>& manifest,
        fs::path const& relative)
    {
        manifest.erase(std::remove_if(manifest.begin(), manifest.end(), [&](auto const& entry) {
            return entry.relative == relative;
        }), manifest.end());
    }

    void erase_manifest_tree_if_present(std::vector<ManifestEntry>& manifest,
        fs::path const& root)
    {
        manifest.erase(std::remove_if(manifest.begin(), manifest.end(), [&](auto const& entry) {
            auto iterator = entry.relative.begin();
            return iterator != entry.relative.end() && *iterator == root;
        }), manifest.end());
    }

    std::optional<MigrationOwner> read_owner_file(fs::path const& path) noexcept
    {
        try {
            return parse_owner(read_control_file(path, 1024));
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<MigrationState> read_state_file(fs::path const& path) noexcept
    {
        try {
            return parse_state(read_control_file(path, 64 * 1024 * 1024));
        } catch (...) {
            return std::nullopt;
        }
    }

    struct MigrationRecord
    {
        MigrationOwner owner;
        MigrationPhase phase{};
        std::vector<ManifestEntry> manifest;
    };

    std::optional<MigrationRecord> read_migration_record(fs::path const& directory) noexcept
    {
        try {
            if (!direct_directory_no_reparse(directory)) return std::nullopt;
            auto owner = read_owner_file(directory / transactionMarkerName);
            auto state = read_state_file(directory / transactionStateName);
            if (!owner || !state || state->owner != *owner) return std::nullopt;
            return MigrationRecord{ std::move(*owner), state->phase,
                std::move(state->manifest) };
        } catch (...) {
            return std::nullopt;
        }
    }

    void erase_migration_controls(std::vector<ManifestEntry>& manifest)
    {
        erase_manifest_entry_if_present(manifest, transactionMarkerName);
        erase_manifest_entry_if_present(manifest, transactionStateName);
        erase_manifest_entry_if_present(manifest,
            fs::path(L"Groups") / transactionMarkerName);
        erase_manifest_entry_if_present(manifest,
            fs::path(L"Groups") / transactionStateName);
    }

    bool copying_manifest_is_safe_subset(
        std::vector<ManifestEntry> const& staged,
        std::vector<ManifestEntry> const& expected) noexcept
    {
        for (auto const& item : staged) {
            auto found = std::lower_bound(expected.begin(), expected.end(), item.relative,
                [](ManifestEntry const& candidate, fs::path const& relative) {
                    return candidate.relative.generic_wstring() < relative.generic_wstring();
                });
            if (found == expected.end() || found->relative != item.relative ||
                found->directory != item.directory) {
                return false;
            }
            if (!item.directory && (item.bytes > found->bytes ||
                (item.bytes == found->bytes &&
                    (!item.digestKnown || !found->digestKnown || item.digest != found->digest)))) {
                return false;
            }
        }
        return true;
    }

    bool verified_stale_transaction(fs::path const& target,
        motion::MediaLibraryTrustIdentity const& sourceIdentity,
        std::vector<ManifestEntry> const& sourceManifest,
        std::string& transactionId) noexcept
    {
        try {
            validate_no_reparse_tree(target);
            auto staging = target / L".mwm-stage";
            auto groups = target / L"Groups";
            auto stageRecord = read_migration_record(staging);
            auto groupsRecord = read_migration_record(groups);
            if (!stageRecord && !groupsRecord) return false;
            auto const& record = groupsRecord ? *groupsRecord : *stageRecord;
            if (record.owner.libraryId != sourceIdentity.ownershipId ||
                record.owner.sourceIdentity != sourceIdentity.rootIdentity ||
                !same_manifest(record.manifest, sourceManifest)) {
                return false;
            }
            if (stageRecord && (stageRecord->owner != record.owner ||
                !same_manifest(stageRecord->manifest, record.manifest))) {
                return false;
            }
            if (groupsRecord && (groupsRecord->owner != record.owner ||
                !same_manifest(groupsRecord->manifest, record.manifest))) {
                return false;
            }

            std::error_code error;
            bool groupsPresent = fs::exists(groups, error);
            if (error || (groupsPresent && !direct_directory_no_reparse(groups)) ||
                (groupsPresent && !groupsRecord)) {
                return false;
            }
            bool stagePresent = fs::exists(staging, error);
            if (error || (stagePresent && !direct_directory_no_reparse(staging))) return false;

            auto targetManifest = build_manifest(target, true);
            erase_manifest_tree_if_present(targetManifest, L".mwm-stage");
            erase_migration_controls(targetManifest);
            if (groupsPresent) {
                if (record.phase == MigrationPhase::copying) return false;
                std::vector<ManifestEntry> stagedRemainder;
                if (stagePresent) {
                    stagedRemainder = build_manifest(staging, true);
                    erase_migration_controls(stagedRemainder);
                }
                auto libraryEntry = std::find_if(sourceManifest.begin(), sourceManifest.end(),
                    [](auto const& entry) {
                        return entry.relative == motion::media_library_ownership_marker_name;
                    });
                if (libraryEntry == sourceManifest.end()) return false;
                auto ownership = target / motion::media_library_ownership_marker_name;
                auto expectedOwnership = marker_value(
                    motion::media_library_ownership_marker_prefix,
                    sourceIdentity.ownershipId);
                if (exact_marker_file(ownership, expectedOwnership)) {
                    if (!same_manifest(targetManifest, sourceManifest)) return false;
                    if (!stagedRemainder.empty() &&
                        !same_manifest(stagedRemainder,
                            std::vector<ManifestEntry>{ *libraryEntry })) return false;
                } else {
                    if (!stagePresent || !same_manifest(stagedRemainder,
                        std::vector<ManifestEntry>{ *libraryEntry })) return false;
                    targetManifest.push_back(*libraryEntry);
                    std::sort(targetManifest.begin(), targetManifest.end(), [](auto const& left, auto const& right) {
                        return left.relative.generic_wstring() < right.relative.generic_wstring();
                    });
                    if (!same_manifest(targetManifest, sourceManifest)) return false;
                }
            } else {
                if (!stagePresent || !stageRecord) return false;
                auto stagedManifest = build_manifest(staging, true);
                erase_migration_controls(stagedManifest);
                if (record.phase == MigrationPhase::copying) {
                    if (!copying_manifest_is_safe_subset(stagedManifest,
                        sourceManifest)) return false;
                } else if (!same_manifest(stagedManifest, sourceManifest)) {
                    return false;
                }
                if (!targetManifest.empty()) return false;
            }
            transactionId = record.owner.transactionId;
            return true;
        } catch (...) {
            return false;
        }
    }

    motion::unique_handle open_verified_migration_target_for_rename(
        fs::path const& configuredTarget, fs::path const& stableTarget,
        motion::FilesystemObjectIdentity const& expectedIdentity) noexcept
    {
        try {
            motion::unique_handle root(CreateFileW(stableTarget.c_str(),
                DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            auto rootIdentity = direct_handle_identity(root.get(), true);
            if (!root || !rootIdentity || *rootIdentity != expectedIdentity ||
                !path_has_directory_identity(configuredTarget, expectedIdentity) ||
                !motion::same_direct_filesystem_object(configuredTarget, stableTarget)) {
                SetLastError(ERROR_INVALID_DATA);
                return {};
            }
            rootIdentity = direct_handle_identity(root.get(), true);
            if (!rootIdentity || *rootIdentity != expectedIdentity) {
                SetLastError(ERROR_INVALID_DATA);
                return {};
            }
            return root;
        } catch (...) {
            SetLastError(ERROR_INVALID_DATA);
            return {};
        }
    }

    fs::path quarantine_verified_stale_target(fs::path const& configuredTarget,
        StableDirectoryIdentity& targetDirectory, std::string const& transactionId)
    {
        auto stableTarget = targetDirectory.stablePath;
        auto targetIdentity = targetDirectory.identity;
        targetDirectory.aliasHandle.reset();
        targetDirectory.stableHandle.reset();
        auto root = open_verified_migration_target_for_rename(
            configuredTarget, stableTarget, targetIdentity);
        if (!root) throw std::runtime_error("migration target changed before conflict preservation");

        fs::path stableConflict;
        for (size_t attempt = 0; attempt < 16; ++attempt) {
            auto suffix = attempt == 0 ? transactionId : motion::new_id();
            stableConflict = stableTarget.parent_path() /
                (stableTarget.filename().wstring() +
                    L".MotionWallpaper-migration-conflict-" + motion::utf8_to_wide(suffix));
            std::error_code error;
            if (!fs::exists(stableConflict, error) && !error) break;
            stableConflict.clear();
        }
        if (stableConflict.empty() || !rename_open_directory(root.get(), stableConflict)) {
            throw std::runtime_error("cannot preserve stale migration target as a conflict");
        }
        root.reset();
        auto configuredConflict = configuredTarget.parent_path() / stableConflict.filename();
        if (!fs::create_directory(stableTarget)) {
            throw std::runtime_error("stale migration data was preserved at " +
                motion::wide_to_utf8(configuredConflict.wstring()) +
                "; cannot recreate the migration target");
        }
        return configuredConflict;
    }

    void require_exclusive_target(fs::path const& target, fs::path const& staging)
    {
        if (is_reparse_point(target) || is_reparse_point(staging)) {
            throw std::runtime_error("migration target changed to a reparse point");
        }
        std::error_code error;
        for (fs::directory_iterator entries(target, error), end; !error && entries != end; entries.increment(error)) {
            if (entries->path() != staging) {
                throw std::runtime_error("migration target was modified by another process");
            }
        }
        if (error) throw std::system_error(error);
    }

    DWORD wait_timeout(std::chrono::milliseconds timeout) noexcept
    {
        auto count = timeout.count();
        if (count <= 0) return 0;
        return static_cast<DWORD>((std::min)(count, static_cast<int64_t>(MAXDWORD - 1)));
    }
}

namespace motion::app
{
    struct LibraryAccessState
    {
        mutable std::mutex mutex;
        uint32_t writers{};
        bool migrating{};
    };

    LibraryWriteLease::LibraryWriteLease(std::shared_ptr<LibraryAccessState> state) : state_(std::move(state)) {}
    LibraryWriteLease::~LibraryWriteLease()
    {
        // Release the filesystem identity handles before advertising that the
        // operation has left the library. A migration may begin as soon as the
        // writer count reaches zero and must not race a still-open root handle.
        trust_.reset();
        if (!state_) return;
        std::scoped_lock lock(state_->mutex);
        if (state_->writers) --state_->writers;
    }

    void LibraryWriteLease::RetainMediaLibraryTrust(
        motion::MediaLibraryTrustIdentity identity,
        std::shared_ptr<motion::MediaLibraryTrustLease> trust) noexcept
    {
        trustIdentity_ = std::move(identity);
        trust_ = std::move(trust);
    }

    bool LibraryWriteLease::RevalidateMediaLibraryTrust() const noexcept
    {
        return !trustIdentity_ || (trust_ &&
            motion::revalidate_media_library_trust(*trustIdentity_));
    }

    LibraryMigrationLease::LibraryMigrationLease(std::shared_ptr<LibraryAccessState> state) : state_(std::move(state)) {}
    LibraryMigrationLease::~LibraryMigrationLease()
    {
        if (!state_) return;
        std::scoped_lock lock(state_->mutex);
        state_->migrating = false;
    }

    LibraryAccessGate::LibraryAccessGate() : state_(std::make_shared<LibraryAccessState>()) {}

    std::shared_ptr<LibraryWriteLease> LibraryAccessGate::TryAcquireWrite()
    {
        std::scoped_lock lock(state_->mutex);
        if (state_->migrating) return {};
        ++state_->writers;
        return std::shared_ptr<LibraryWriteLease>(new LibraryWriteLease(state_));
    }

    std::shared_ptr<LibraryMigrationLease> LibraryAccessGate::TryBeginMigration()
    {
        std::scoped_lock lock(state_->mutex);
        if (state_->migrating || state_->writers) return {};
        state_->migrating = true;
        return std::shared_ptr<LibraryMigrationLease>(new LibraryMigrationLease(state_));
    }

    bool LibraryAccessGate::MigrationInProgress() const
    {
        std::scoped_lock lock(state_->mutex);
        return state_->migrating;
    }

    struct AgentLibraryMigrationPause::Impl
    {
        motion::unique_handle requested{ CreateEventW(nullptr, TRUE, FALSE, motion::library_migration_request_event_name) };
        motion::unique_handle quiesced{ CreateEventW(nullptr, TRUE, FALSE, motion::library_migration_quiesced_event_name) };
        motion::unique_handle applied{ CreateEventW(nullptr, TRUE, FALSE, motion::library_migration_applied_event_name) };
        motion::LibraryMigrationOwnerChannel owner;
        std::atomic_bool active{};
    };

    AgentLibraryMigrationPause::AgentLibraryMigrationPause() : impl_(std::make_unique<Impl>())
    {
        if (!impl_->requested || !impl_->quiesced || !impl_->applied || !impl_->owner) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
        }
    }

    AgentLibraryMigrationPause::~AgentLibraryMigrationPause() { Cancel(); }

    bool AgentLibraryMigrationPause::RequestAndWait(std::chrono::milliseconds timeout) noexcept
    {
        if (!impl_ || !impl_->requested || !impl_->quiesced || !impl_->applied) return false;
        if (!impl_->owner.TryClaimAndReset(
            impl_->requested.get(), impl_->quiesced.get(), impl_->applied.get())) return false;
        impl_->active.store(true, std::memory_order_release);
        if (!SetEvent(impl_->requested.get())) {
            impl_->active.store(false, std::memory_order_release);
            impl_->owner.ReleaseClaim();
            return false;
        }
        motion::notify_settings_changed();
        if (WaitForSingleObject(impl_->quiesced.get(), wait_timeout(timeout)) == WAIT_OBJECT_0) return true;
        Cancel();
        return false;
    }

    bool AgentLibraryMigrationPause::ResumeAndWait(std::chrono::milliseconds timeout) noexcept
    {
        if (!impl_ || !impl_->requested || !impl_->applied) return false;
        ResetEvent(impl_->applied.get());
        if (!impl_->owner.ResetRequestForClaim(impl_->requested.get())) return false;
        impl_->active.store(false, std::memory_order_release);
        motion::notify_settings_changed();
        auto applied = WaitForSingleObject(impl_->applied.get(), wait_timeout(timeout)) == WAIT_OBJECT_0;
        impl_->owner.ReleaseClaim();
        return applied;
    }

    void AgentLibraryMigrationPause::Cancel() noexcept
    {
        if (!impl_ || !impl_->requested) return;
        auto wasActive = impl_->active.exchange(false, std::memory_order_acq_rel);
        impl_->owner.ResetRequestForClaim(impl_->requested.get());
        impl_->owner.ReleaseClaim();
        if (wasActive) motion::notify_settings_changed();
    }

    struct LibraryMigrationTransaction::Impl
    {
        fs::path source;
        fs::path sourceAccess;
        fs::path target;
        fs::path targetAccess;
        fs::path staging;
        motion::MediaLibraryTrustIdentity sourceIdentity;
        std::shared_ptr<motion::MediaLibraryTrustLease> sourceTrust;
        motion::FilesystemObjectIdentity targetIdentity;
        motion::unique_handle targetAliasHandle;
        motion::unique_handle targetStableHandle;
        std::string transactionId;
        std::string libraryId;
        MigrationOwner owner;
        std::vector<ManifestEntry> manifest;
        bool targetRootCreated{};
        bool copied{};
        bool committed{};
        bool activated{};
        bool targetLibraryMarkerCreated{};

        void RequireSourceTrust() const
        {
            if (!sourceTrust || !motion::revalidate_media_library_trust(sourceIdentity)) {
                throw std::runtime_error("source media library identity changed");
            }
        }

        [[nodiscard]] bool StableTargetTrusted() const noexcept
        {
            auto held = direct_handle_identity(targetStableHandle.get(), true);
            return held && *held == targetIdentity &&
                path_has_directory_identity(targetAccess, targetIdentity);
        }

        void RequireTargetTrust() const
        {
            auto aliasHeld = direct_handle_identity(targetAliasHandle.get(), true);
            if (!aliasHeld || *aliasHeld != targetIdentity ||
                !StableTargetTrusted() ||
                !path_has_directory_identity(target, targetIdentity) ||
                !motion::same_direct_filesystem_object(target, targetAccess)) {
                throw std::runtime_error("migration target identity changed");
            }
        }
    };

    LibraryMigrationTransaction::LibraryMigrationTransaction(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

    std::shared_ptr<LibraryMigrationTransaction> LibraryMigrationTransaction::Begin(
        fs::path source, fs::path target,
        motion::MediaLibraryTrustIdentity const& expectedSource)
    {
        if (source.empty() || target.empty()) {
            throw std::runtime_error("unsafe or overlapping library paths");
        }
        std::error_code normalizeError;
        source = fs::absolute(source, normalizeError).lexically_normal();
        if (normalizeError) throw std::system_error(normalizeError);
        target = fs::absolute(target, normalizeError).lexically_normal();
        auto sourceTrust = motion::acquire_media_library_trust(expectedSource);
        if (normalizeError || !source.has_filename() || !target.has_filename() ||
            !sourceTrust || !motion::same_filesystem_path(source, expectedSource.root) ||
            motion::same_filesystem_path(source, target) ||
            motion::filesystem_path_is_nested(source, target) ||
            motion::filesystem_path_is_nested(target, source)) {
            throw std::runtime_error("unsafe or overlapping library paths");
        }
        std::error_code sourceError;
        if (expectedSource.stableRoot.empty() ||
            !fs::is_directory(expectedSource.stableRoot, sourceError) ||
            sourceError || is_reparse_point(expectedSource.stableRoot)) {
            throw std::runtime_error("source is not a direct library directory");
        }
        if (!source.has_filename() || !motion::same_filesystem_path(source, expectedSource.root) ||
            !motion::revalidate_media_library_trust(expectedSource)) {
            throw std::runtime_error("source media library identity changed");
        }
        std::error_code error;
        bool targetExists = fs::exists(target, error);
        if (error || (targetExists && (!fs::is_directory(target, error) || error))) {
            throw std::runtime_error("target library path is not a directory");
        }
        if (targetExists && is_reparse_point(target)) throw std::runtime_error("target cannot be a reparse point");

        validate_library_structure(expectedSource.stableRoot, true);
        auto sourceManifest = build_manifest(expectedSource.stableRoot, true);

        auto impl = std::make_unique<Impl>();
        impl->source = std::move(source);
        impl->sourceAccess = expectedSource.stableRoot;
        impl->target = fs::absolute(target).lexically_normal();
        impl->sourceIdentity = expectedSource;
        impl->sourceTrust = std::move(sourceTrust);
        impl->transactionId = motion::new_id();
        impl->libraryId = expectedSource.ownershipId;
        impl->owner = MigrationOwner{ impl->transactionId, impl->libraryId,
            expectedSource.rootIdentity };
        impl->manifest = std::move(sourceManifest);
        impl->RequireSourceTrust();
        if (!targetExists) {
            fs::create_directories(impl->target);
            impl->targetRootCreated = true;
        }
        auto targetDirectory = capture_stable_directory(impl->target);
        if (!targetDirectory) {
            // Without a Volume-GUID identity there is no path on which a
            // rollback can safely act if the configured alias is reassigned.
            // Leave a just-created empty directory rather than guessing.
            throw std::runtime_error("migration target has no stable volume identity");
        }
        impl->targetAccess = std::move(targetDirectory->stablePath);
        impl->targetIdentity = targetDirectory->identity;
        impl->targetAliasHandle = std::move(targetDirectory->aliasHandle);
        impl->targetStableHandle = std::move(targetDirectory->stableHandle);
        try {
            impl->RequireTargetTrust();
            error.clear();
            if (!fs::is_empty(impl->targetAccess, error) || error) {
                std::string staleTransactionId;
                if (!verified_stale_transaction(impl->targetAccess,
                    expectedSource, impl->manifest, staleTransactionId)) {
                    throw std::runtime_error(
                        "target contains an unverified migration conflict");
                }
                auto staleDirectory = StableDirectoryIdentity{
                    impl->targetAccess, impl->targetIdentity,
                    std::move(impl->targetAliasHandle),
                    std::move(impl->targetStableHandle) };
                (void)quarantine_verified_stale_target(
                    impl->target, staleDirectory, staleTransactionId);
                auto replacement = capture_stable_directory(impl->target);
                if (!replacement) {
                    throw std::runtime_error(
                        "recreated migration target has no stable volume identity");
                }
                impl->targetAccess = std::move(replacement->stablePath);
                impl->targetIdentity = replacement->identity;
                impl->targetAliasHandle = std::move(replacement->aliasHandle);
                impl->targetStableHandle = std::move(replacement->stableHandle);
                impl->targetRootCreated = true;
                impl->RequireTargetTrust();
                error.clear();
                if (!fs::is_empty(impl->targetAccess, error) || error) {
                    throw std::runtime_error(
                        "recreated migration target is not empty");
                }
            }
            // The selected target is required to be empty, so a compact fixed
            // staging name is collision-safe and preserves MAX_PATH headroom.
            impl->staging = impl->targetAccess / L".mwm-stage";
            if (!fs::create_directory(impl->staging)) {
                throw std::runtime_error(
                    "cannot create migration staging directory");
            }
            impl->RequireTargetTrust();
            write_control_file(impl->staging / transactionMarkerName,
                serialize_owner(impl->owner), false);
            write_control_file(impl->staging / transactionStateName,
                serialize_state(impl->owner, MigrationPhase::copying,
                    impl->manifest), false);
            impl->RequireTargetTrust();
        } catch (...) {
            if (impl->StableTargetTrusted()) {
                if (!impl->staging.empty()) {
                    auto stageMarker = impl->staging / transactionMarkerName;
                    auto expected = serialize_owner(impl->owner);
                    auto stageState = impl->staging / transactionStateName;
                    auto persistedState = read_state_file(stageState);
                    if (persistedState && persistedState->owner == impl->owner &&
                        same_manifest(persistedState->manifest, impl->manifest)) {
                        make_file_writable(stageState);
                        fs::remove(stageState, error);
                    }
                    if (exact_marker_file(stageMarker, expected)) {
                        make_file_writable(stageMarker);
                        fs::remove(stageMarker, error);
                    }
                    error.clear();
                    fs::remove(impl->staging, error);
                }
                if (impl->targetRootCreated) {
                    auto targetAccess = impl->targetAccess;
                    auto targetIdentity = impl->targetIdentity;
                    impl->targetAliasHandle.reset();
                    impl->targetStableHandle.reset();
                    if (path_has_directory_identity(targetAccess, targetIdentity)) {
                        fs::remove(targetAccess, error);
                    }
                }
            }
            throw;
        }
        return std::shared_ptr<LibraryMigrationTransaction>(new LibraryMigrationTransaction(std::move(impl)));
    }

    LibraryMigrationTransaction::~LibraryMigrationTransaction()
    {
        if (impl_ && !impl_->activated) RollbackOwnedTarget();
    }

    void LibraryMigrationTransaction::CopyAndVerify(LibraryMigrationProgress const& progress,
        std::atomic_bool const* cancelled)
    {
        if (!impl_ || impl_->copied || impl_->committed) throw std::logic_error("invalid migration copy state");
        impl_->RequireSourceTrust();
        impl_->RequireTargetTrust();
        validate_library_structure(impl_->sourceAccess, true);
        auto initialSource = build_manifest(impl_->sourceAccess, true);
        if (!same_manifest(impl_->manifest, initialSource)) {
            throw std::runtime_error("source library changed before migration copy");
        }
        uint64_t totalBytes{};
        for (auto const& item : impl_->manifest) {
            if (item.directory) continue;
            if (item.bytes > std::numeric_limits<uint64_t>::max() - totalBytes) {
                throw std::runtime_error("library is too large");
            }
            totalBytes += item.bytes;
        }
        ULARGE_INTEGER available{};
        if (!GetDiskFreeSpaceExW(impl_->targetAccess.c_str(), &available, nullptr, nullptr)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
        }
        constexpr uint64_t reserve = 256ULL * 1024 * 1024;
        auto required = totalBytes > std::numeric_limits<uint64_t>::max() - reserve
            ? std::numeric_limits<uint64_t>::max() : totalBytes + reserve;
        if (available.QuadPart < required) throw std::runtime_error("insufficient disk space for migration");

        uint64_t copiedBytes{};
        for (auto& item : impl_->manifest) {
            impl_->RequireSourceTrust();
            impl_->RequireTargetTrust();
            if (cancelled && cancelled->load(std::memory_order_acquire)) {
                throw std::runtime_error("library migration cancelled");
            }
            auto destination = impl_->staging / item.relative;
            if (item.directory) {
                fs::create_directories(destination);
                impl_->RequireTargetTrust();
                continue;
            }
            fs::create_directories(destination.parent_path());
            auto digest = copy_and_hash(impl_->sourceAccess / item.relative, destination,
                copiedBytes, totalBytes, progress, cancelled);
            impl_->RequireTargetTrust();
            if (!item.digestKnown || digest != item.digest) {
                throw std::runtime_error("source content changed during migration copy");
            }
            copy_file_metadata(impl_->sourceAccess / item.relative, destination);
            if (fs::file_size(destination) != item.bytes || hash_file(destination) != item.digest) {
                throw std::runtime_error("destination content verification failed");
            }
            impl_->RequireSourceTrust();
            impl_->RequireTargetTrust();
        }
        impl_->RequireSourceTrust();
        impl_->RequireTargetTrust();
        auto finalSource = build_manifest(impl_->sourceAccess, true);
        if (!same_manifest(impl_->manifest, finalSource)) {
            throw std::runtime_error("source library changed during migration");
        }
        auto stagedManifest = build_manifest(impl_->staging, true);
        impl_->RequireTargetTrust();
        erase_manifest_entry(stagedManifest, transactionMarkerName);
        erase_manifest_entry(stagedManifest, transactionStateName);
        if (!same_manifest(impl_->manifest, stagedManifest)) {
            throw std::runtime_error("staged library content verification failed");
        }
        write_control_file(impl_->staging / transactionStateName,
            serialize_state(impl_->owner, MigrationPhase::prepared,
                impl_->manifest), true);
        impl_->RequireTargetTrust();
        if (progress) progress(totalBytes, totalBytes);
        impl_->copied = true;
    }

    void LibraryMigrationTransaction::CommitPreparedTarget()
    {
        if (!impl_ || !impl_->copied || impl_->committed) throw std::logic_error("invalid migration commit state");
        impl_->RequireSourceTrust();
        impl_->RequireTargetTrust();
        require_exclusive_target(impl_->targetAccess, impl_->staging);
        validate_no_reparse_tree(impl_->staging);
        auto preparedManifest = build_manifest(impl_->staging, true);
        impl_->RequireTargetTrust();
        erase_manifest_entry(preparedManifest, transactionMarkerName);
        erase_manifest_entry(preparedManifest, transactionStateName);
        if (!same_manifest(impl_->manifest, preparedManifest)) {
            throw std::runtime_error("staged library changed before commit");
        }
        write_control_file(impl_->staging / transactionStateName,
            serialize_state(impl_->owner, MigrationPhase::committing,
                impl_->manifest), true);
        auto sourceGroups = impl_->staging / L"Groups";
        auto targetGroups = impl_->targetAccess / L"Groups";
        impl_->RequireTargetTrust();
        write_control_file(sourceGroups / transactionMarkerName,
            serialize_owner(impl_->owner), false);
        write_control_file(sourceGroups / transactionStateName,
            serialize_state(impl_->owner, MigrationPhase::committing,
                impl_->manifest), false);
        impl_->RequireTargetTrust();
        if (!MoveFileExW(sourceGroups.c_str(), targetGroups.c_str(), MOVEFILE_WRITE_THROUGH)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
        }
        try {
            impl_->RequireTargetTrust();
            write_small_file_new(impl_->targetAccess /
                    motion::media_library_ownership_marker_name,
                marker_value(motion::media_library_ownership_marker_prefix, impl_->libraryId));
            impl_->targetLibraryMarkerCreated = true;
            impl_->RequireTargetTrust();
        } catch (...) {
            impl_->committed = true;
            RollbackOwnedTarget();
            throw;
        }
        impl_->committed = true;
        try {
            write_control_file(targetGroups / transactionStateName,
                serialize_state(impl_->owner, MigrationPhase::committed,
                    impl_->manifest), true);
            impl_->RequireTargetTrust();
            std::error_code stagingError;
            auto libraryEntry = std::find_if(impl_->manifest.begin(), impl_->manifest.end(), [](auto const& entry) {
                return entry.relative == motion::media_library_ownership_marker_name;
            });
            auto stagedLibraryMarker = impl_->staging / motion::media_library_ownership_marker_name;
            if (libraryEntry == impl_->manifest.end() ||
                !safe_manifest_path(impl_->staging, motion::media_library_ownership_marker_name) ||
                !matches_manifest_file(stagedLibraryMarker, *libraryEntry)) {
                throw std::runtime_error("staged library ownership marker changed before cleanup");
            }
            make_file_writable(stagedLibraryMarker);
            if (!fs::remove(stagedLibraryMarker, stagingError) || stagingError) {
                throw std::runtime_error("cannot remove staged library ownership marker");
            }
            auto stagedTransactionMarker = impl_->staging / transactionMarkerName;
            if (!exact_marker_file(stagedTransactionMarker,
                serialize_owner(impl_->owner))) {
                throw std::runtime_error("staged transaction ownership marker changed before cleanup");
            }
            auto stagedTransactionState = impl_->staging / transactionStateName;
            auto persistedState = read_state_file(stagedTransactionState);
            if (!persistedState || persistedState->owner != impl_->owner ||
                !same_manifest(persistedState->manifest, impl_->manifest)) {
                throw std::runtime_error("staged transaction state changed before cleanup");
            }
            make_file_writable(stagedTransactionState);
            if (!fs::remove(stagedTransactionState, stagingError) || stagingError) {
                throw std::runtime_error("cannot remove staged transaction state");
            }
            make_file_writable(stagedTransactionMarker);
            if (!fs::remove(stagedTransactionMarker, stagingError) || stagingError ||
                !fs::remove(impl_->staging, stagingError) || stagingError) {
                throw std::runtime_error("cannot finalize migration staging directory");
            }
            impl_->RequireTargetTrust();
            auto targetManifest = build_manifest(impl_->targetAccess, true);
            impl_->RequireTargetTrust();
            erase_manifest_entry(targetManifest, fs::path(L"Groups") / transactionMarkerName);
            erase_manifest_entry(targetManifest, fs::path(L"Groups") / transactionStateName);
            if (!same_manifest(impl_->manifest, targetManifest)) {
                throw std::runtime_error("committed library content verification failed");
            }
        } catch (...) {
            RollbackOwnedTarget();
            throw;
        }
    }

    void LibraryMigrationTransaction::MarkActivated()
    {
        if (!impl_ || !impl_->committed) return;
        impl_->RequireSourceTrust();
        impl_->RequireTargetTrust();
        impl_->activated = true;
        try {
            auto groups = impl_->targetAccess / L"Groups";
            auto marker = groups / transactionMarkerName;
            auto state = groups / transactionStateName;
            if (impl_->StableTargetTrusted() &&
                direct_directory_no_reparse(impl_->targetAccess) &&
                direct_directory_no_reparse(groups) &&
                exact_marker_file(marker, serialize_owner(impl_->owner))) {
                std::error_code ignored;
                auto persistedState = read_state_file(state);
                if (persistedState && persistedState->owner == impl_->owner &&
                    same_manifest(persistedState->manifest, impl_->manifest)) {
                    make_file_writable(state);
                    fs::remove(state, ignored);
                }
                make_file_writable(marker);
                fs::remove(marker, ignored);
            }
        } catch (...) {}
    }

    fs::path LibraryMigrationTransaction::ArchiveVerifiedSource()
    {
        if (!impl_ || !impl_->activated) throw std::logic_error("migration target is not active");
        impl_->RequireSourceTrust();
        impl_->RequireTargetTrust();
        validate_library_structure(impl_->sourceAccess, true);
        auto current = build_manifest(impl_->sourceAccess, true);
        if (!same_manifest(impl_->manifest, current)) {
            throw std::runtime_error("source library changed after migration");
        }
        auto target = build_manifest(impl_->targetAccess, true);
        impl_->RequireTargetTrust();
        if (!same_manifest(impl_->manifest, target)) {
            throw std::runtime_error("active library changed before source archival");
        }
        auto backup = impl_->source.parent_path() /
            (impl_->source.filename().wstring() + L".MotionWallpaper-backup-" +
                motion::utf8_to_wide(impl_->transactionId));
        auto stableBackup = impl_->sourceAccess.parent_path() /
            (impl_->sourceAccess.filename().wstring() +
                L".MotionWallpaper-backup-" +
                motion::utf8_to_wide(impl_->transactionId));
        std::error_code error;
        if (fs::exists(stableBackup, error) || error) {
            throw std::runtime_error("library backup path already exists");
        }
        // The ordinary trust handles deliberately deny rename/delete. Release
        // them only to open the same root with DELETE access, verify its file
        // identity again, then rename that already-open handle. A drive-letter
        // replacement can therefore fail the archive but cannot be renamed.
        impl_->RequireSourceTrust();
        impl_->sourceTrust.reset();
        auto archiveRoot = open_verified_archive_root(impl_->sourceIdentity);
        if (!archiveRoot || !rename_open_directory(archiveRoot.get(), stableBackup)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
        }
        return backup;
    }

    fs::path const& LibraryMigrationTransaction::Source() const noexcept { return impl_->source; }
    fs::path const& LibraryMigrationTransaction::Target() const noexcept { return impl_->target; }
    std::string const& LibraryMigrationTransaction::TransactionId() const noexcept { return impl_->transactionId; }

    void LibraryMigrationTransaction::RollbackOwnedTarget() noexcept
    {
        if (!impl_ || impl_->activated) return;
        try {
            if (!impl_->StableTargetTrusted()) {
                impl_->copied = false;
                impl_->committed = false;
                impl_->targetLibraryMarkerCreated = false;
                return;
            }
            auto expected = serialize_owner(impl_->owner);
            std::error_code ignored;
            auto stageMarker = impl_->staging / transactionMarkerName;
            if (direct_directory_no_reparse(impl_->targetAccess) &&
                direct_directory_no_reparse(impl_->staging) && exact_marker_file(stageMarker, expected)) {
                auto stagedGroupsMarker = impl_->staging / L"Groups" / transactionMarkerName;
                auto stagedGroupsState = impl_->staging / L"Groups" / transactionStateName;
                if (safe_manifest_path(impl_->staging,
                    fs::path(L"Groups") / transactionMarkerName) &&
                    exact_marker_file(stagedGroupsMarker, expected)) {
                    auto state = read_state_file(stagedGroupsState);
                    if (state && state->owner == impl_->owner &&
                        same_manifest(state->manifest, impl_->manifest)) {
                        make_file_writable(stagedGroupsState);
                        ignored.clear();
                        fs::remove(stagedGroupsState, ignored);
                    }
                    make_file_writable(stagedGroupsMarker);
                    ignored.clear();
                    fs::remove(stagedGroupsMarker, ignored);
                }
                for (auto entry = impl_->manifest.rbegin(); entry != impl_->manifest.rend(); ++entry) {
                    if (!safe_manifest_path(impl_->staging, entry->relative)) continue;
                    auto path = impl_->staging / entry->relative;
                    auto attributes = GetFileAttributesW(path.c_str());
                    ignored.clear();
                    if (entry->directory) {
                        if (attributes != INVALID_FILE_ATTRIBUTES &&
                            (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
                            !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                            fs::remove(path, ignored);
                        }
                    } else if (matches_manifest_file(path, *entry)) {
                        make_file_writable(path);
                        fs::remove(path, ignored);
                    }
                }
                bool controlsOnly = true;
                ignored.clear();
                for (fs::directory_iterator entries(impl_->staging, ignored), end;
                    !ignored && entries != end; entries.increment(ignored)) {
                    auto name = entries->path().filename();
                    if (name != transactionMarkerName && name != transactionStateName) {
                        controlsOnly = false;
                        break;
                    }
                }
                if (!ignored && controlsOnly && exact_marker_file(stageMarker, expected)) {
                    auto stageState = impl_->staging / transactionStateName;
                    auto state = read_state_file(stageState);
                    if (state && state->owner == impl_->owner &&
                        same_manifest(state->manifest, impl_->manifest)) {
                        make_file_writable(stageState);
                        ignored.clear();
                        fs::remove(stageState, ignored);
                        make_file_writable(stageMarker);
                        ignored.clear();
                        fs::remove(stageMarker, ignored);
                        ignored.clear();
                        fs::remove(impl_->staging, ignored);
                    }
                }
            }

            auto groups = impl_->targetAccess / L"Groups";
            auto groupsMarker = groups / transactionMarkerName;
            auto groupsState = groups / transactionStateName;
            ignored.clear();
            if (direct_directory_no_reparse(impl_->targetAccess) &&
                direct_directory_no_reparse(groups) &&
                exact_marker_file(groupsMarker, expected)) {
                for (auto entry = impl_->manifest.rbegin(); entry != impl_->manifest.rend(); ++entry) {
                    if (entry->relative.empty() || *entry->relative.begin() != L"Groups") continue;
                    auto path = impl_->targetAccess / entry->relative;
                    if (!safe_manifest_path(impl_->targetAccess, entry->relative)) continue;
                    auto attributes = GetFileAttributesW(path.c_str());
                    ignored.clear();
                    if (entry->directory) {
                        if (attributes != INVALID_FILE_ATTRIBUTES &&
                            (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
                            !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                            fs::remove(path, ignored);
                        }
                    } else if (matches_manifest_file(path, *entry)) {
                        make_file_writable(path);
                        fs::remove(path, ignored);
                    }
                }
                bool controlsOnly = true;
                ignored.clear();
                for (fs::directory_iterator entries(groups, ignored), end;
                    !ignored && entries != end; entries.increment(ignored)) {
                    auto name = entries->path().filename();
                    if (name != transactionMarkerName && name != transactionStateName) {
                        controlsOnly = false;
                        break;
                    }
                }
                if (!ignored && controlsOnly && exact_marker_file(groupsMarker, expected)) {
                    auto state = read_state_file(groupsState);
                    if (state && state->owner == impl_->owner &&
                        same_manifest(state->manifest, impl_->manifest)) {
                        make_file_writable(groupsState);
                        ignored.clear();
                        fs::remove(groupsState, ignored);
                        make_file_writable(groupsMarker);
                        ignored.clear();
                        fs::remove(groupsMarker, ignored);
                        if (direct_directory_no_reparse(groups)) {
                            ignored.clear();
                            fs::remove(groups, ignored);
                        }
                    }
                }
            }

            auto libraryMarker = impl_->targetAccess /
                motion::media_library_ownership_marker_name;
            ignored.clear();
            if (impl_->targetLibraryMarkerCreated &&
                direct_directory_no_reparse(impl_->targetAccess) &&
                exact_marker_file(libraryMarker,
                    marker_value(motion::media_library_ownership_marker_prefix, impl_->libraryId))) {
                make_file_writable(libraryMarker);
                fs::remove(libraryMarker, ignored);
            }
            if (impl_->targetRootCreated &&
                direct_directory_no_reparse(impl_->targetAccess) &&
                impl_->StableTargetTrusted()) {
                auto targetAccess = impl_->targetAccess;
                auto targetIdentity = impl_->targetIdentity;
                impl_->targetAliasHandle.reset();
                impl_->targetStableHandle.reset();
                if (path_has_directory_identity(targetAccess, targetIdentity)) {
                    ignored.clear();
                    fs::remove(targetAccess, ignored);
                }
            }
        } catch (...) {}
        impl_->copied = false;
        impl_->committed = false;
        impl_->targetLibraryMarkerCreated = false;
    }
}
