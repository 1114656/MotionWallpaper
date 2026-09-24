#include <windows.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <wincodec.h>

#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>
#include "MediaLibrary.h"
#include "ThumbnailGenerator.h"
#include "../MotionWallpaper.Common/MediaProbe.h"

namespace fs = std::filesystem;

namespace
{
    constexpr uint64_t maximumDecodedPixels = 100'000'000;
    constexpr std::wstring_view recycleStagePrefix = L".mw-delete-";
    constexpr std::wstring_view recycleRestorePrefix = L".mw-restore-";
    constexpr std::wstring_view recycleConflictPrefix = L".mw-conflict-";
    constexpr size_t recycleTokenLength = 20;
    constexpr std::string_view recycleRestoreMagic = "MotionWallpaper.Delete/v1\n";
    constexpr std::wstring_view moveStagePrefix = L".mw-moving-";
    constexpr std::wstring_view moveRecordPrefix = L".mw-move-";
    constexpr std::wstring_view moveConflictPrefix = L".mw-move-conflict-";
    constexpr std::string_view moveRecordMagic = "MotionWallpaper.Move/v1\n";
    constexpr size_t maximumMediaTags = 32;
    constexpr size_t maximumMediaTagCharacters = 64;

    struct MoveRestoreRecord
    {
        std::string mediaId;
        std::string sourceGroupId;
        std::string targetGroupId;
    };

    class bcrypt_algorithm
    {
    public:
        ~bcrypt_algorithm() { if (value_) BCryptCloseAlgorithmProvider(value_, 0); }
        bcrypt_algorithm(bcrypt_algorithm const&) = delete;
        bcrypt_algorithm& operator=(bcrypt_algorithm const&) = delete;
        bcrypt_algorithm() = default;
        BCRYPT_ALG_HANDLE get() const noexcept { return value_; }
        BCRYPT_ALG_HANDLE* put() noexcept { return &value_; }
    private:
        BCRYPT_ALG_HANDLE value_{};
    };

    class bcrypt_hash
    {
    public:
        ~bcrypt_hash() { if (value_) BCryptDestroyHash(value_); }
        bcrypt_hash(bcrypt_hash const&) = delete;
        bcrypt_hash& operator=(bcrypt_hash const&) = delete;
        bcrypt_hash() = default;
        BCRYPT_HASH_HANDLE get() const noexcept { return value_; }
        BCRYPT_HASH_HANDLE* put() noexcept { return &value_; }
    private:
        BCRYPT_HASH_HANDLE value_{};
    };

    std::wstring trim(std::wstring value)
    {
        auto notSpace = [](wchar_t character) { return !iswspace(character); };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
        return value;
    }

    std::wstring folded(std::wstring_view value)
    {
        std::wstring result(value);
        std::transform(result.begin(), result.end(), result.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(towlower(character));
        });
        return result;
    }

    std::vector<std::wstring> normalize_media_tags(
        std::vector<std::wstring> const& values)
    {
        std::vector<std::wstring> result;
        result.reserve((std::min)(values.size(), maximumMediaTags));
        for (auto value : values) {
            value = trim(std::move(value));
            if (value.empty() || value.size() > maximumMediaTagCharacters ||
                !std::all_of(value.begin(), value.end(), [](wchar_t character) {
                    return character >= 0x20 && character != 0x7f &&
                        character != L'|' && character != L'\r' && character != L'\n';
                })) {
                throw std::invalid_argument("invalid media tag");
            }
            auto duplicate = std::find_if(result.begin(), result.end(),
                [&](auto const& existing) {
                    return _wcsicmp(existing.c_str(), value.c_str()) == 0;
                });
            if (duplicate == result.end()) result.push_back(std::move(value));
        }
        if (result.size() > maximumMediaTags) {
            throw std::invalid_argument("too many media tags");
        }
        return result;
    }

    bool contains_folded(std::wstring const& value, std::wstring const& needle)
    {
        return needle.empty() || folded(value).find(needle) != std::wstring::npos;
    }

    std::wstring recycle_token()
    {
        auto value = motion::utf8_to_wide(motion::new_id());
        std::erase(value, L'-');
        value.resize(recycleTokenLength);
        return value;
    }

    bool recycle_token_name(std::wstring_view name, std::wstring_view prefix) noexcept
    {
        if (!name.starts_with(prefix) || name.size() != prefix.size() + recycleTokenLength) {
            return false;
        }
        return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
            name.end(), [](wchar_t value) {
                return (value >= L'0' && value <= L'9') ||
                    (value >= L'a' && value <= L'f');
            });
    }

    void write_recycle_restore_record(fs::path const& path,
        std::string const& ownershipId, fs::path const& originalName)
    {
        if (!motion::valid_id(ownershipId) || !motion::safe_file_name(originalName)) {
            throw std::invalid_argument("invalid recycle restore record");
        }
        auto value = std::string(recycleRestoreMagic) + ownershipId + "\n" +
            motion::wide_to_utf8(originalName.wstring()) + "\n";
        motion::unique_handle file(CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
            nullptr));
        if (!file) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
        }
        DWORD written{};
        if (value.size() > MAXDWORD ||
            !WriteFile(file.get(), value.data(), static_cast<DWORD>(value.size()), &written,
                nullptr) || written != static_cast<DWORD>(value.size()) ||
            !FlushFileBuffers(file.get())) {
            auto error = GetLastError();
            FILE_DISPOSITION_INFO disposition{ TRUE };
            SetFileInformationByHandle(file.get(), FileDispositionInfo,
                &disposition, sizeof(disposition));
            throw std::system_error(static_cast<int>(error ? error : ERROR_WRITE_FAULT),
                std::system_category());
        }
    }

    std::optional<fs::path> read_recycle_restore_record(fs::path const& path,
        std::string const& ownershipId) noexcept
    {
        try {
            motion::unique_handle file(CreateFileW(path.c_str(), GENERIC_READ,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr));
            if (!file) return std::nullopt;
            BY_HANDLE_FILE_INFORMATION information{};
            LARGE_INTEGER size{};
            if (!GetFileInformationByHandle(file.get(), &information) ||
                information.dwFileAttributes &
                    (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) ||
                !GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 ||
                size.QuadPart > 4096) return std::nullopt;
            std::string value(static_cast<size_t>(size.QuadPart), '\0');
            DWORD read{};
            if (!ReadFile(file.get(), value.data(), static_cast<DWORD>(value.size()),
                &read, nullptr) || read != static_cast<DWORD>(value.size()) ||
                value.back() != '\n' ||
                !value.starts_with(recycleRestoreMagic)) return std::nullopt;
            auto ownerStart = recycleRestoreMagic.size();
            auto ownerEnd = value.find('\n', ownerStart);
            if (ownerEnd == std::string::npos ||
                value.substr(ownerStart, ownerEnd - ownerStart) != ownershipId) {
                return std::nullopt;
            }
            auto name = motion::utf8_to_wide(value.substr(
                ownerEnd + 1, value.size() - ownerEnd - 2));
            fs::path result(name);
            return motion::safe_file_name(result)
                ? std::optional<fs::path>(std::move(result)) : std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    }

    void write_move_restore_record(fs::path const& path,
        std::string const& ownershipId, MoveRestoreRecord const& record)
    {
        if (!motion::valid_id(ownershipId) ||
            !motion::valid_id(record.mediaId) ||
            !motion::valid_id(record.sourceGroupId) ||
            !motion::valid_id(record.targetGroupId) ||
            record.sourceGroupId == record.targetGroupId) {
            throw std::invalid_argument("invalid move restore record");
        }
        auto value = std::string(moveRecordMagic) + ownershipId + "\n" +
            record.mediaId + "\n" + record.sourceGroupId + "\n" +
            record.targetGroupId + "\n";
        motion::unique_handle file(CreateFileW(path.c_str(), GENERIC_WRITE | DELETE,
            0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr));
        if (!file) {
            throw std::system_error(static_cast<int>(GetLastError()),
                std::system_category());
        }
        DWORD written{};
        if (value.size() > MAXDWORD ||
            !WriteFile(file.get(), value.data(), static_cast<DWORD>(value.size()),
                &written, nullptr) ||
            written != static_cast<DWORD>(value.size()) ||
            !FlushFileBuffers(file.get())) {
            auto error = GetLastError();
            FILE_DISPOSITION_INFO disposition{ TRUE };
            SetFileInformationByHandle(file.get(), FileDispositionInfo,
                &disposition, sizeof(disposition));
            throw std::system_error(static_cast<int>(error ? error : ERROR_WRITE_FAULT),
                std::system_category());
        }
    }

    std::optional<MoveRestoreRecord> read_move_restore_record(
        fs::path const& path, std::string const& ownershipId) noexcept
    {
        try {
            motion::unique_handle file(CreateFileW(path.c_str(), GENERIC_READ,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!file) return std::nullopt;
            BY_HANDLE_FILE_INFORMATION information{};
            LARGE_INTEGER size{};
            if (!GetFileInformationByHandle(file.get(), &information) ||
                (information.dwFileAttributes &
                    (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
                !GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 ||
                size.QuadPart > 1024) return std::nullopt;
            std::string value(static_cast<size_t>(size.QuadPart), '\0');
            DWORD read{};
            if (!ReadFile(file.get(), value.data(), static_cast<DWORD>(value.size()),
                    &read, nullptr) || read != static_cast<DWORD>(value.size()) ||
                value.back() != '\n' || !value.starts_with(moveRecordMagic)) {
                return std::nullopt;
            }
            std::array<std::string, 4> fields;
            size_t position = moveRecordMagic.size();
            for (auto& field : fields) {
                auto end = value.find('\n', position);
                if (end == std::string::npos) return std::nullopt;
                field = value.substr(position, end - position);
                position = end + 1;
            }
            if (position != value.size() || fields[0] != ownershipId ||
                !motion::valid_id(fields[1]) || !motion::valid_id(fields[2]) ||
                !motion::valid_id(fields[3]) || fields[2] == fields[3]) {
                return std::nullopt;
            }
            return MoveRestoreRecord{
                std::move(fields[1]), std::move(fields[2]), std::move(fields[3]) };
        } catch (...) {
            return std::nullopt;
        }
    }

    void ensure_owned_default_library(fs::path const& root)
    {
        if (motion::media_library_ownership_id(root)) return;
        auto marker = root / motion::media_library_ownership_marker_name;
        auto value = std::string(motion::media_library_ownership_marker_prefix) +
            motion::new_id() + "\n";
        motion::unique_handle file(CreateFileW(marker.c_str(), GENERIC_WRITE | DELETE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr));
        if (!file) {
            auto error = GetLastError();
            if (error == ERROR_FILE_EXISTS && motion::media_library_ownership_id(root)) return;
            throw std::system_error(static_cast<int>(error), std::system_category());
        }
        DWORD written{};
        if (value.size() > MAXDWORD ||
            !WriteFile(file.get(), value.data(), static_cast<DWORD>(value.size()), &written, nullptr) ||
            written != static_cast<DWORD>(value.size()) || !FlushFileBuffers(file.get())) {
            auto error = GetLastError();
            FILE_DISPOSITION_INFO disposition{ TRUE };
            SetFileInformationByHandle(file.get(), FileDispositionInfo,
                &disposition, sizeof(disposition));
            file.reset();
            throw std::system_error(static_cast<int>(error ? error : ERROR_WRITE_FAULT),
                std::system_category());
        }
    }

    std::wstring copy_and_sha256(fs::path const& source, fs::path const& destination,
        motion::app::MediaLibrary::ImportProgress const& progress, std::atomic_bool const* cancelled)
    {
        bcrypt_algorithm algorithm;
        bcrypt_hash hash;
        DWORD objectLength{}, resultLength{};
        winrt::check_nt(BCryptOpenAlgorithmProvider(algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0));
        winrt::check_nt(BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &resultLength, 0));
        std::vector<uint8_t> object(objectLength);
        std::array<uint8_t, 32> digest{};
        winrt::check_nt(BCryptCreateHash(algorithm.get(), hash.put(), object.data(),
            static_cast<ULONG>(object.size()), nullptr, 0, 0));
        std::ifstream input(source, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open media for hashing");
        std::ofstream destinationStream(destination, std::ios::binary | std::ios::trunc);
        if (!destinationStream) throw std::runtime_error("cannot create imported media");
        uint64_t total = fs::file_size(source);
        uint64_t copied{};
        std::vector<uint8_t> buffer(4 * 1024 * 1024);
        while (input) {
            if (cancelled && cancelled->load(std::memory_order_relaxed)) throw std::runtime_error("import cancelled");
            input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            auto count = input.gcount();
            if (count > 0) {
                winrt::check_nt(BCryptHashData(hash.get(), buffer.data(), static_cast<ULONG>(count), 0));
                destinationStream.write(reinterpret_cast<char*>(buffer.data()), count);
                if (!destinationStream) throw std::runtime_error("cannot write imported media");
                copied += static_cast<uint64_t>(count);
                if (progress) progress(copied, total);
            }
        }
        if (!input.eof()) throw std::runtime_error("cannot read imported media");
        destinationStream.flush();
        if (!destinationStream) throw std::runtime_error("cannot flush imported media");
        winrt::check_nt(BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0));
        std::wostringstream hexDigest;
        hexDigest << std::hex << std::setfill(L'0');
        for (auto byte : digest) hexDigest << std::setw(2) << static_cast<unsigned>(byte);
        return hexDigest.str();
    }

    std::wstring sha256_file(fs::path const& path)
    {
        bcrypt_algorithm algorithm;
        bcrypt_hash hash;
        DWORD objectLength{}, resultLength{};
        winrt::check_nt(BCryptOpenAlgorithmProvider(
            algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0));
        winrt::check_nt(BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &resultLength, 0));
        std::vector<uint8_t> object(objectLength);
        std::array<uint8_t, 32> digest{};
        winrt::check_nt(BCryptCreateHash(algorithm.get(), hash.put(), object.data(),
            static_cast<ULONG>(object.size()), nullptr, 0, 0));
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open file for hashing");
        std::vector<uint8_t> buffer(4 * 1024 * 1024);
        while (input) {
            input.read(reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(buffer.size()));
            auto count = input.gcount();
            if (count > 0) {
                winrt::check_nt(BCryptHashData(hash.get(), buffer.data(),
                    static_cast<ULONG>(count), 0));
            }
        }
        if (!input.eof()) throw std::runtime_error("cannot hash file");
        winrt::check_nt(BCryptFinishHash(hash.get(), digest.data(),
            static_cast<ULONG>(digest.size()), 0));
        std::wostringstream hexDigest;
        hexDigest << std::hex << std::setfill(L'0');
        for (auto byte : digest) hexDigest << std::setw(2) << static_cast<unsigned>(byte);
        return hexDigest.str();
    }

    void validate_image(fs::path const& source)
    {
        winrt::com_ptr<IWICImagingFactory> factory;
        winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IWICImagingFactory2), factory.put_void()));
        winrt::com_ptr<IWICBitmapDecoder> decoder;
        winrt::check_hresult(factory->CreateDecoderFromFilename(source.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, decoder.put()));
        winrt::com_ptr<IWICBitmapFrameDecode> frame;
        winrt::check_hresult(decoder->GetFrame(0, frame.put()));
        UINT width{}, height{};
        winrt::check_hresult(frame->GetSize(&width, &height));
        if (!width || !height || static_cast<uint64_t>(width) * height > maximumDecodedPixels) {
            throw std::runtime_error("image dimensions exceed safety limit");
        }
    }

    void validate_video(fs::path const& source, std::atomic_bool const* cancelled)
    {
        auto isCancelled = [&] { return cancelled && cancelled->load(std::memory_order_acquire); };
        auto ffmpeg = motion::executable_directory() / L"Tools" / L"ffmpeg" / L"ffmpeg.exe";
        auto metadata = motion::probe_video(ffmpeg, source, 10'000, isCancelled);
        if (isCancelled()) throw std::runtime_error("import cancelled");
        if (!metadata) throw std::runtime_error("video metadata probe failed");
        if (!motion::app::video_import_dimensions_allowed(metadata->width, metadata->height)) {
            throw std::runtime_error("video resolution exceeds 8K import limit");
        }
        if (!motion::app::video_import_frame_rate_allowed(
                metadata->frameRateNumerator, metadata->frameRateDenominator)) {
            throw std::runtime_error("video frame rate exceeds 240 FPS import limit");
        }
    }

    void require_import_space(fs::path const& destinationRoot, fs::path const& source)
    {
        auto sourceSize = fs::file_size(source);
        ULARGE_INTEGER available{};
        if (!GetDiskFreeSpaceExW(destinationRoot.c_str(), &available, nullptr, nullptr)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
        }
        constexpr uint64_t minimumReserve = 256ULL * 1024 * 1024;
        uint64_t required = sourceSize > UINT64_MAX - minimumReserve ? UINT64_MAX : sourceSize + minimumReserve;
        if (available.QuadPart < required) throw std::runtime_error("insufficient disk space for import");
    }
}

namespace motion::app
{
    MediaLibrary::MediaLibrary(fs::path root, DeleteMode deleteMode, fs::path wallpapersPath,
        std::optional<motion::MediaLibraryTrustIdentity> expectedIdentity)
        : root_(std::move(root)),
          wallpapersPath_(wallpapersPath.empty() ? root_ / L"Wallpapers" : std::move(wallpapersPath)),
          expectedIdentity_(std::move(expectedIdentity)),
          deleteMode_(deleteMode)
    {
        identityRequired_ = !motion::same_filesystem_path(
            wallpapersPath_, (root_ / L"Wallpapers").lexically_normal());
        if (expectedIdentity_ &&
            !motion::same_filesystem_path(wallpapersPath_, expectedIdentity_->root)) {
            throw std::invalid_argument("media library trust identity belongs to another path");
        }
        accessWallpapersPath_ = expectedIdentity_
            ? expectedIdentity_->stableRoot : wallpapersPath_;
        if (accessWallpapersPath_.empty()) {
            throw std::invalid_argument("media library has no stable access path");
        }
        if (expectedIdentity_ || !identityRequired_) RequireTrustedLibrary();
    }

    bool MediaLibrary::LibraryTrusted() const noexcept
    {
        return expectedIdentity_
            ? motion::revalidate_media_library_trust(*expectedIdentity_)
            : !identityRequired_;
    }

    bool MediaLibrary::StableLibraryTrusted() const noexcept
    {
        return expectedIdentity_
            ? motion::revalidate_media_library_stable_root(*expectedIdentity_)
            : !identityRequired_;
    }

    void MediaLibrary::RequireTrustedLibrary() const
    {
        if (!LibraryTrusted()) {
            throw std::runtime_error("media library identity changed");
        }
    }

    void MediaLibrary::EnsureDirectories() const
    {
        RequireTrustedLibrary();
        fs::create_directories(AccessWallpapersPath() / L"Groups");
        // The default library is app-owned as well. Giving it the same stable
        // identity marker as a custom library lets a later migration hold a
        // source identity lease for its entire transaction.
        ensure_owned_default_library(AccessWallpapersPath());
        fs::create_directories(root_ / L"Config");
    }

    fs::path MediaLibrary::WallpapersPath() const { return wallpapersPath_; }

    fs::path const& MediaLibrary::AccessWallpapersPath() const noexcept
    {
        return accessWallpapersPath_;
    }

    fs::path MediaLibrary::ConfiguredAliasPath(fs::path const& accessPath) const
    {
        auto normalizedRoot = accessWallpapersPath_.lexically_normal();
        auto normalizedPath = accessPath.lexically_normal();
        auto relative = normalizedPath.lexically_relative(normalizedRoot);
        if (relative.empty() || relative.is_absolute() || relative.has_root_name()) {
            throw std::runtime_error("media path is outside the library");
        }
        for (auto const& component : relative) {
            if (component == L"..") {
                throw std::runtime_error("media path is outside the library");
            }
        }
        return relative == L"." ? wallpapersPath_ : wallpapersPath_ / relative;
    }

    GroupLoadResult MediaLibrary::LoadGroups()
    {
        std::scoped_lock lock(mutex_);
        RequireTrustedLibrary();
        GroupLoadResult result;
        RecoverInterruptedMoves();
        RecoverInterruptedRecycleDeletes();
        auto groupsRoot = AccessWallpapersPath() / L"Groups";
        for (auto const& entry : fs::directory_iterator(groupsRoot)) {
            if (!entry.is_directory()) continue;
            try {
                if (auto group = motion::load_group(entry.path() / L"group.json")) result.groups.push_back(std::move(*group));
            } catch (...) {}
        }
        std::stable_sort(result.groups.begin(), result.groups.end(), [](auto const& left, auto const& right) { return left.order < right.order; });

        if (result.groups.empty()) {
            motion::GroupMetadata group;
            group.id = motion::new_id();
            group.name = L"我的壁纸";
            group.createdAt = group.updatedAt = motion::timestamp_utc();
            auto directory = groupsRoot / motion::utf8_to_wide(group.id);
            fs::create_directories(directory / L"Videos");
            motion::save_group(directory / L"group.json", group);
            result.groups.push_back(std::move(group));
        }
        return result;
    }

    std::vector<motion::MediaMetadata> MediaLibrary::LoadMedia(std::string const& groupId)
    {
        std::scoped_lock lock(mutex_);
        RequireTrustedLibrary();
        std::vector<motion::MediaMetadata> result;
        if (!motion::valid_id(groupId)) return result;
        RecoverInterruptedMoves();
        RecoverInterruptedRecycleDeletes();
        auto directory = AccessWallpapersPath() / L"Groups" / motion::utf8_to_wide(groupId) / L"Videos";
        fs::create_directories(directory);
        for (auto const& entry : fs::directory_iterator(directory)) {
            if (!entry.is_directory()) continue;
            try {
                auto media = motion::load_media(entry.path() / L"metadata.json");
                if (!media) continue;
                auto directoryId = motion::wide_to_utf8(entry.path().filename().wstring());
                if (!motion::valid_id(directoryId) || media->id != directoryId) continue;
                RecoverInterruptedVariantDeletions(entry.path());
                if (media->groupId != groupId) {
                    media->groupId = groupId;
                    ++media->revision;
                    media->updatedAt = motion::timestamp_utc();
                    motion::save_media(entry.path() / L"metadata.json", *media);
                }
                result.push_back(std::move(*media));
            } catch (...) {}
        }
        return result;
    }

    std::vector<CatalogMediaEntry> MediaLibrary::QueryMedia(MediaQuery const& query)
    {
        if (!query.groupId.empty() && !motion::valid_id(query.groupId)) {
            throw std::invalid_argument("invalid catalog group id");
        }
        if (!query.kind.empty() && query.kind != "video" && query.kind != "image") {
            throw std::invalid_argument("invalid catalog media kind");
        }
        auto requiredTags = normalize_media_tags(query.tags);
        auto search = folded(trim(query.text));
        std::vector<CatalogMediaEntry> result;
        auto groups = LoadGroups().groups;
        for (auto const& group : groups) {
            if (!query.groupId.empty() && group.id != query.groupId) continue;
            for (auto& media : LoadMedia(group.id)) {
                if (!query.kind.empty() && media.kind != query.kind) continue;
                if (query.favoritesOnly && !media.favorite) continue;

                bool tagsMatch = std::all_of(requiredTags.begin(), requiredTags.end(),
                    [&](auto const& required) {
                        return std::any_of(media.tags.begin(), media.tags.end(),
                            [&](auto const& existing) {
                                return _wcsicmp(existing.c_str(), required.c_str()) == 0;
                            });
                    });
                if (!tagsMatch) continue;

                bool textMatches = search.empty() ||
                    contains_folded(media.name, search) ||
                    contains_folded(media.originalName, search) ||
                    contains_folded(group.name, search) ||
                    std::any_of(media.tags.begin(), media.tags.end(),
                        [&](auto const& tag) { return contains_folded(tag, search); });
                if (!textMatches) continue;
                result.push_back({ std::move(media), group.name });
            }
        }
        std::stable_sort(result.begin(), result.end(), [&](auto const& left, auto const& right) {
            if (query.sort == MediaCatalogSort::Newest &&
                left.media.importedAt != right.media.importedAt) {
                return left.media.importedAt > right.media.importedAt;
            }
            if (query.sort == MediaCatalogSort::Size &&
                left.media.sizeBytes != right.media.sizeBytes) {
                return left.media.sizeBytes > right.media.sizeBytes;
            }
            if (query.sort == MediaCatalogSort::Kind &&
                left.media.kind != right.media.kind) {
                return left.media.kind == "video";
            }
            auto compared = _wcsicmp(left.media.name.c_str(), right.media.name.c_str());
            if (compared != 0) return compared < 0;
            return left.media.id < right.media.id;
        });
        return result;
    }

    std::vector<DuplicateMediaSet> MediaLibrary::FindDuplicateMedia()
    {
        using DuplicateKey = std::tuple<std::wstring, std::string, uint64_t>;
        std::map<DuplicateKey, std::vector<CatalogMediaEntry>> candidates;
        for (auto& item : QueryMedia()) {
            if (item.media.sha256.empty() || !item.media.sizeBytes) continue;
            candidates[{ folded(item.media.sha256), item.media.kind,
                item.media.sizeBytes }].push_back(std::move(item));
        }
        std::vector<DuplicateMediaSet> result;
        for (auto& [key, items] : candidates) {
            if (items.size() < 2) continue;
            result.push_back({ std::get<0>(key), std::get<1>(key),
                std::get<2>(key), std::move(items) });
        }
        std::stable_sort(result.begin(), result.end(), [](auto const& left, auto const& right) {
            if (left.items.size() != right.items.size()) {
                return left.items.size() > right.items.size();
            }
            if (left.sizeBytes != right.sizeBytes) return left.sizeBytes > right.sizeBytes;
            return left.sha256 < right.sha256;
        });
        return result;
    }

    motion::GroupMetadata MediaLibrary::CreateGroup(std::wstring const& value, std::vector<motion::GroupMetadata> const& existing)
    {
        std::scoped_lock lock(mutex_);
        RequireTrustedLibrary();
        auto name = trim(value);
        if (name.empty()) throw std::runtime_error("group name is empty");
        for (auto const& group : existing) if (_wcsicmp(group.name.c_str(), name.c_str()) == 0) throw std::runtime_error("group already exists");
        motion::GroupMetadata group;
        group.id = motion::new_id();
        group.name = std::move(name);
        for (auto const& item : existing) group.order = (std::max)(group.order, item.order + 1);
        group.createdAt = group.updatedAt = motion::timestamp_utc();
        auto directory = AccessWallpapersPath() / L"Groups" / motion::utf8_to_wide(group.id);
        fs::create_directories(directory / L"Videos");
        motion::save_group(directory / L"group.json", group);
        return group;
    }

    void MediaLibrary::RenameGroup(motion::GroupMetadata const& group, std::wstring const& value,
        std::vector<motion::GroupMetadata> const& existing)
    {
        std::scoped_lock lock(mutex_);
        RequireTrustedLibrary();
        auto name = trim(value);
        if (name.empty()) throw std::runtime_error("group name is empty");
        for (auto const& item : existing) {
            if (item.id != group.id && _wcsicmp(item.name.c_str(), name.c_str()) == 0) throw std::runtime_error("group already exists");
        }
        auto path = AccessWallpapersPath() / L"Groups" / motion::utf8_to_wide(group.id) / L"group.json";
        auto current = motion::load_group(path);
        if (!current || current->id != group.id) throw std::runtime_error("group changed during rename");
        auto updated = std::move(*current);
        updated.name = std::move(name);
        updated.updatedAt = motion::timestamp_utc();
        motion::save_group(path, updated);
    }

    void MediaLibrary::ReorderGroup(std::string const& groupId, int direction, std::vector<motion::GroupMetadata> const& groups)
    {
        auto found = std::find_if(groups.begin(), groups.end(), [&](auto const& group) { return group.id == groupId; });
        if (found == groups.end() || !direction) return;
        auto index = static_cast<std::ptrdiff_t>(std::distance(groups.begin(), found));
        auto target = index + (direction < 0 ? -1 : 1);
        if (target < 0 || target >= static_cast<std::ptrdiff_t>(groups.size())) return;

        std::vector<std::string> orderedIds;
        orderedIds.reserve(groups.size());
        for (auto const& group : groups) orderedIds.push_back(group.id);
        std::swap(orderedIds[static_cast<size_t>(index)], orderedIds[static_cast<size_t>(target)]);
        SetGroupOrder(orderedIds, groups);
    }

    void MediaLibrary::SetGroupOrder(std::vector<std::string> const& orderedIds, std::vector<motion::GroupMetadata> const& groups)
    {
        std::scoped_lock lock(mutex_);
        RequireTrustedLibrary();
        if (orderedIds.size() != groups.size()) throw std::runtime_error("invalid group order");
        auto root = AccessWallpapersPath() / L"Groups";
        auto timestamp = motion::timestamp_utc();
        std::vector<std::string> visited;
        std::vector<motion::GroupMetadata> originals;
        std::vector<motion::GroupMetadata> updatedGroups;
        visited.reserve(orderedIds.size());
        originals.reserve(orderedIds.size());
        updatedGroups.reserve(orderedIds.size());

        for (size_t index = 0; index < orderedIds.size(); ++index) {
            auto const& id = orderedIds[index];
            if (std::find(visited.begin(), visited.end(), id) != visited.end()) throw std::runtime_error("duplicate group id");
            auto found = std::find_if(groups.begin(), groups.end(), [&](auto const& group) { return group.id == id; });
            if (found == groups.end()) throw std::runtime_error("unknown group id");
            visited.push_back(id);

            auto path = root / motion::utf8_to_wide(id) / L"group.json";
            auto current = motion::load_group(path);
            if (!current || current->id != id) throw std::runtime_error("group changed during reorder");
            originals.push_back(*current);
            auto updated = std::move(*current);
            updated.order = static_cast<int>(index);
            updated.updatedAt = timestamp;
            updatedGroups.push_back(std::move(updated));
        }

        std::vector<fs::path> staged;
        staged.reserve(updatedGroups.size());
        try {
            for (auto const& updated : updatedGroups) {
                RequireTrustedLibrary();
                auto pending = root / motion::utf8_to_wide(updated.id) / L"group.order.pending.json";
                motion::save_group(pending, updated);
                staged.push_back(std::move(pending));
            }
            for (size_t index = 0; index < updatedGroups.size(); ++index) {
                RequireTrustedLibrary();
                auto destination = root / motion::utf8_to_wide(updatedGroups[index].id) / L"group.json";
                if (!MoveFileExW(staged[index].c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
                }
            }
        } catch (...) {
            for (auto const& pending : staged) {
                if (!StableLibraryTrusted()) break;
                std::error_code ignored;
                fs::remove(pending, ignored);
            }
            for (auto const& original : originals) {
                if (!StableLibraryTrusted()) break;
                try {
                    motion::save_group(root / motion::utf8_to_wide(original.id) / L"group.json", original);
                } catch (...) {}
            }
            throw;
        }
    }

    void MediaLibrary::DeleteGroup(motion::GroupMetadata const& group)
    {
        std::scoped_lock lock(mutex_);
        RequireTrustedLibrary();
        if (!motion::valid_id(group.id)) throw std::runtime_error("invalid group id");
        DeletePath(AccessWallpapersPath() / L"Groups" / motion::utf8_to_wide(group.id));
    }

    std::string MediaLibrary::Import(fs::path const& source, std::string const& kind, std::string const& groupId,
        ImportProgress const& progress, std::atomic_bool const* cancelled, std::wstring const& displayName)
    {
        RequireTrustedLibrary();
        auto extension = source.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
        static std::array<std::wstring, 6> const videos{ L".mp4", L".m4v", L".mov", L".mkv", L".webm", L".avi" };
        static std::array<std::wstring, 8> const images{ L".jpg", L".jpeg", L".png", L".bmp", L".gif", L".tif", L".tiff", L".webp" };
        bool supported = kind == "image" ? std::find(images.begin(), images.end(), extension) != images.end()
            : kind == "video" && std::find(videos.begin(), videos.end(), extension) != videos.end();
        if (!supported || !motion::valid_id(groupId)) throw std::runtime_error("unsupported import");

        std::error_code sourceError;
        if (!fs::is_regular_file(source, sourceError) || sourceError || !fs::file_size(source, sourceError) || sourceError) {
            throw std::runtime_error("import source is not a readable file");
        }
        if (kind == "image") validate_image(source);
        else validate_video(source, cancelled);

        // Source validation can invoke external codecs. Never resolve or
        // create a destination after that slow operation unless the external
        // library is still the exact volume/root/marker/Groups identity that
        // this MediaLibrary instance was constructed for.
        RequireTrustedLibrary();
        auto groupDirectory = AccessWallpapersPath() / L"Groups" / motion::utf8_to_wide(groupId);
        if (!fs::is_regular_file(groupDirectory / L"group.json")) throw std::runtime_error("import group does not exist");
        require_import_space(groupDirectory, source);
        auto mediaId = motion::new_id();
        auto directory = groupDirectory / L"Videos" / motion::utf8_to_wide(mediaId);
        fs::create_directories(directory);
        try {
            auto fileName = L"source" + extension;
            auto destination = directory / fileName;
            auto sourceHash = copy_and_sha256(source, destination, progress, cancelled);
            std::scoped_lock lock(mutex_);
            RequireTrustedLibrary();
            if (!fs::is_regular_file(groupDirectory / L"group.json")) throw std::runtime_error("import group was removed");
            auto siblings = directory.parent_path();
            for (auto const& entry : fs::directory_iterator(siblings)) {
                if (!entry.is_directory() || entry.path() == directory) continue;
                try {
                    auto existing = motion::load_media(entry.path() / L"metadata.json");
                    if (existing && existing->sha256 == sourceHash && existing->kind == kind) {
                        RequireTrustedLibrary();
                        std::error_code ignored;
                        fs::remove_all(directory, ignored);
                        return existing->id;
                    }
                } catch (...) {}
            }
            motion::MediaMetadata media;
            media.id = mediaId;
            media.groupId = groupId;
            // A display name belongs only to metadata. Duplicate imports above
            // keep the existing record, and source names/paths remain unchanged.
            media.name = trim(displayName);
            if (media.name.empty()) media.name = source.stem().wstring();
            media.kind = kind;
            media.originalName = source.filename().wstring();
            media.fileName = fileName;
            media.sha256 = sourceHash;
            media.sizeBytes = fs::file_size(destination);
            media.revision = 1;
            media.importedAt = media.updatedAt = motion::timestamp_utc();
            RequireTrustedLibrary();
            motion::save_media(directory / L"metadata.json", media);
            return mediaId;
        } catch (...) {
            // On surprise removal the configured drive letter may already
            // name a different disk. Orphaned staging data on the original
            // volume is safer than path-based cleanup on the replacement.
            if (StableLibraryTrusted()) {
                std::error_code ignored;
                fs::remove_all(directory, ignored);
            }
            throw;
        }
    }

    fs::path MediaLibrary::MediaDirectory(motion::MediaMetadata const& media) const
    {
        if (!motion::valid_id(media.groupId) || !motion::valid_id(media.id)) throw std::runtime_error("invalid media id");
        return WallpapersPath() / L"Groups" / motion::utf8_to_wide(media.groupId) / L"Videos" / motion::utf8_to_wide(media.id);
    }

    void MediaLibrary::RecoverInterruptedRecycleDeletes() const
    {
        if (recycleRecoveryComplete_) return;
        RequireTrustedLibrary();
        if (!expectedIdentity_) {
            recycleRecoveryComplete_ = true;
            return;
        }

        std::vector<fs::path> records;
        std::error_code error;
        for (fs::recursive_directory_iterator entries(AccessWallpapersPath(),
                 fs::directory_options::none, error), end;
             !error && entries != end; entries.increment(error)) {
            auto attributes = GetFileAttributesW(entries->path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                error = std::error_code(static_cast<int>(GetLastError()),
                    std::system_category());
                break;
            }
            auto name = entries->path().filename().wstring();
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
                ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                    (recycle_token_name(name, recycleStagePrefix) ||
                     recycle_token_name(name, recycleConflictPrefix)))) {
                if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    entries.disable_recursion_pending();
                }
                continue;
            }
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                recycle_token_name(name, recycleRestorePrefix)) {
                records.push_back(entries->path());
            }
        }
        if (error) throw std::system_error(error);

        auto preserveConflict = [&](fs::path const& record, fs::path const& staged,
                                    std::wstring_view reason) {
            if (!StableLibraryTrusted()) {
                throw std::runtime_error("media library volume became unavailable");
            }
            auto token = record.filename().wstring().substr(recycleRestorePrefix.size());
            fs::path preservedStage;
            auto stagedAttributes = GetFileAttributesW(staged.c_str());
            if (stagedAttributes != INVALID_FILE_ATTRIBUTES &&
                (stagedAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                for (unsigned attempt = 0; attempt != 8; ++attempt) {
                    auto suffix = attempt == 0 ? token : recycle_token();
                    auto candidate = staged.parent_path() /
                        (std::wstring(recycleConflictPrefix) + suffix);
                    if (MoveFileExW(staged.c_str(), candidate.c_str(),
                        MOVEFILE_WRITE_THROUGH)) {
                        preservedStage = std::move(candidate);
                        break;
                    }
                    auto moveError = GetLastError();
                    if (moveError != ERROR_ALREADY_EXISTS &&
                        moveError != ERROR_FILE_EXISTS) {
                        throw std::system_error(static_cast<int>(moveError),
                            std::system_category());
                    }
                }
                if (preservedStage.empty()) {
                    throw std::system_error(ERROR_ALREADY_EXISTS,
                        std::system_category());
                }
            }

            fs::path preservedRecord;
            for (unsigned attempt = 0; attempt != 8; ++attempt) {
                auto suffix = attempt == 0 ? token : recycle_token();
                auto candidate = record.parent_path() /
                    (std::wstring(recycleConflictPrefix) + suffix + L".record");
                if (MoveFileExW(record.c_str(), candidate.c_str(),
                    MOVEFILE_WRITE_THROUGH)) {
                    preservedRecord = std::move(candidate);
                    break;
                }
                auto moveError = GetLastError();
                if (moveError != ERROR_ALREADY_EXISTS &&
                    moveError != ERROR_FILE_EXISTS) {
                    throw std::system_error(static_cast<int>(moveError),
                        std::system_category());
                }
            }
            if (preservedRecord.empty()) {
                throw std::system_error(ERROR_ALREADY_EXISTS,
                    std::system_category());
            }
            motion::append_utf8_log(root_ / L"Config" / L"app.log",
                L"回收站删除恢复发现冲突，数据已保留在 " +
                (preservedStage.empty() ? preservedRecord.wstring()
                                        : preservedStage.wstring()) +
                L"（" + std::wstring(reason) + L"）");
        };

        for (auto const& record : records) {
            if (!StableLibraryTrusted()) {
                throw std::runtime_error("media library volume became unavailable");
            }
            auto recordName = record.filename().wstring();
            auto token = recordName.substr(recycleRestorePrefix.size());
            auto staged = record.parent_path() /
                (std::wstring(recycleStagePrefix) + token);
            auto originalName = read_recycle_restore_record(
                record, expectedIdentity_->ownershipId);
            if (!originalName) {
                preserveConflict(record, staged, L"恢复记录无效");
                continue;
            }
            auto original = record.parent_path() / *originalName;
            auto stagedAttributes = GetFileAttributesW(staged.c_str());
            auto originalAttributes = GetFileAttributesW(original.c_str());
            bool stagedExists = stagedAttributes != INVALID_FILE_ATTRIBUTES;
            bool originalExists = originalAttributes != INVALID_FILE_ATTRIBUTES;
            if ((stagedExists &&
                    (stagedAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) ||
                (originalExists &&
                    (originalAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) ||
                (stagedExists && originalExists)) {
                preserveConflict(record, staged,
                    stagedExists && originalExists
                        ? L"原路径与暂存路径同时存在" : L"恢复路径是重解析点");
                continue;
            }
            if (stagedExists && !originalExists) {
                if (!MoveFileExW(staged.c_str(), original.c_str(),
                    MOVEFILE_WRITE_THROUGH)) {
                    throw std::system_error(static_cast<int>(GetLastError()),
                        std::system_category());
                }
            }
            if (!StableLibraryTrusted()) {
                throw std::runtime_error("media library volume became unavailable");
            }
            error.clear();
            if (!fs::remove(record, error) || error) {
                throw std::system_error(error ? error :
                    std::make_error_code(std::errc::operation_canceled));
            }
        }
        recycleRecoveryComplete_ = true;
    }

    void MediaLibrary::RecoverInterruptedMoves() const
    {
        if (moveRecoveryComplete_) return;
        RequireTrustedLibrary();
        auto ownershipId = expectedIdentity_
            ? std::optional<std::string>(expectedIdentity_->ownershipId)
            : motion::media_library_ownership_id(AccessWallpapersPath());
        if (!ownershipId) {
            throw std::runtime_error("media library ownership marker is unavailable");
        }

        std::vector<fs::path> records;
        std::error_code error;
        for (fs::recursive_directory_iterator entries(AccessWallpapersPath(),
                 fs::directory_options::none, error), end;
             !error && entries != end; entries.increment(error)) {
            auto attributes = GetFileAttributesW(entries->path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                error = std::error_code(static_cast<int>(GetLastError()),
                    std::system_category());
                break;
            }
            auto name = entries->path().filename().wstring();
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
                ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                    (name.starts_with(moveStagePrefix) ||
                     name.starts_with(moveConflictPrefix) ||
                     name.starts_with(recycleStagePrefix) ||
                     name.starts_with(recycleConflictPrefix)))) {
                if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    entries.disable_recursion_pending();
                }
                continue;
            }
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                recycle_token_name(name, moveRecordPrefix)) {
                records.push_back(entries->path());
            }
        }
        if (error) throw std::system_error(error);

        auto preserveConflict = [&](fs::path const& record, fs::path const& staged,
                                    std::wstring_view reason) {
            if (!StableLibraryTrusted()) {
                throw std::runtime_error("media library volume became unavailable");
            }
            auto token = record.filename().wstring().substr(moveRecordPrefix.size());
            fs::path preservedStage;
            auto stagedAttributes = GetFileAttributesW(staged.c_str());
            if (stagedAttributes != INVALID_FILE_ATTRIBUTES &&
                (stagedAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                for (unsigned attempt = 0; attempt != 8; ++attempt) {
                    auto suffix = attempt == 0 ? token : recycle_token();
                    auto candidate = staged.parent_path() /
                        (std::wstring(moveConflictPrefix) + suffix);
                    if (MoveFileExW(staged.c_str(), candidate.c_str(),
                        MOVEFILE_WRITE_THROUGH)) {
                        preservedStage = std::move(candidate);
                        break;
                    }
                    auto moveError = GetLastError();
                    if (moveError != ERROR_ALREADY_EXISTS &&
                        moveError != ERROR_FILE_EXISTS) {
                        throw std::system_error(static_cast<int>(moveError),
                            std::system_category());
                    }
                }
                if (preservedStage.empty()) {
                    throw std::system_error(ERROR_ALREADY_EXISTS,
                        std::system_category());
                }
            }
            fs::path preservedRecord;
            for (unsigned attempt = 0; attempt != 8; ++attempt) {
                auto suffix = attempt == 0 ? token : recycle_token();
                auto candidate = record.parent_path() /
                    (std::wstring(moveConflictPrefix) + suffix + L".record");
                if (MoveFileExW(record.c_str(), candidate.c_str(),
                    MOVEFILE_WRITE_THROUGH)) {
                    preservedRecord = std::move(candidate);
                    break;
                }
                auto moveError = GetLastError();
                if (moveError != ERROR_ALREADY_EXISTS &&
                    moveError != ERROR_FILE_EXISTS) {
                    throw std::system_error(static_cast<int>(moveError),
                        std::system_category());
                }
            }
            if (preservedRecord.empty()) {
                throw std::system_error(ERROR_ALREADY_EXISTS,
                    std::system_category());
            }
            motion::append_utf8_log(root_ / L"Config" / L"app.log",
                L"媒体移动恢复发现冲突，数据已保留在 " +
                (preservedStage.empty() ? preservedRecord.wstring()
                                        : preservedStage.wstring()) +
                L"（" + std::wstring(reason) + L"）");
        };

        for (auto const& recordPath : records) {
            if (!StableLibraryTrusted()) {
                throw std::runtime_error("media library volume became unavailable");
            }
            auto token = recordPath.filename().wstring().substr(moveRecordPrefix.size());
            auto staged = recordPath.parent_path() /
                (std::wstring(moveStagePrefix) + token);
            auto record = read_move_restore_record(recordPath, *ownershipId);
            if (!record) {
                preserveConflict(recordPath, staged, L"移动恢复记录无效");
                continue;
            }
            auto expectedParent = AccessWallpapersPath() / L"Groups" /
                motion::utf8_to_wide(record->targetGroupId) / L"Videos";
            if (!motion::same_filesystem_path(
                    recordPath.parent_path(), expectedParent)) {
                preserveConflict(recordPath, staged, L"移动恢复记录位于错误目录");
                continue;
            }
            auto source = AccessWallpapersPath() / L"Groups" /
                motion::utf8_to_wide(record->sourceGroupId) / L"Videos" /
                motion::utf8_to_wide(record->mediaId);
            auto target = expectedParent / motion::utf8_to_wide(record->mediaId);
            auto sourceAttributes = GetFileAttributesW(source.c_str());
            auto stagedAttributes = GetFileAttributesW(staged.c_str());
            auto targetAttributes = GetFileAttributesW(target.c_str());
            bool sourceExists = sourceAttributes != INVALID_FILE_ATTRIBUTES;
            bool stagedExists = stagedAttributes != INVALID_FILE_ATTRIBUTES;
            bool targetExists = targetAttributes != INVALID_FILE_ATTRIBUTES;
            if ((sourceExists &&
                    (sourceAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) ||
                (stagedExists &&
                    (stagedAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) ||
                (targetExists &&
                    (targetAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) ||
                (static_cast<unsigned>(sourceExists) +
                    static_cast<unsigned>(stagedExists) +
                    static_cast<unsigned>(targetExists) > 1)) {
                preserveConflict(recordPath, staged,
                    L"移动源、暂存或目标发生冲突");
                continue;
            }

            auto validMetadata = [&](fs::path const& directory,
                                     std::string const& groupId) {
                try {
                    auto media = motion::load_media(directory / L"metadata.json");
                    return media && media->id == record->mediaId &&
                        media->groupId == groupId;
                } catch (...) {
                    return false;
                }
            };
            if (targetExists) {
                if (!validMetadata(target, record->targetGroupId)) {
                    preserveConflict(recordPath, staged,
                        L"已提交目标的元数据不匹配");
                    continue;
                }
            } else if (stagedExists) {
                auto metadata = motion::load_media(staged / L"metadata.json");
                if (!metadata || metadata->id != record->mediaId ||
                    (metadata->groupId != record->sourceGroupId &&
                     metadata->groupId != record->targetGroupId)) {
                    preserveConflict(recordPath, staged,
                        L"暂存媒体的元数据不匹配");
                    continue;
                }
                metadata->groupId = record->sourceGroupId;
                ++metadata->revision;
                metadata->updatedAt = motion::timestamp_utc();
                motion::save_media(staged / L"metadata.json", *metadata);
                if (!StableLibraryTrusted()) {
                    throw std::runtime_error("media library volume became unavailable");
                }
                if (!MoveFileExW(staged.c_str(), source.c_str(),
                        MOVEFILE_WRITE_THROUGH)) {
                    throw std::system_error(static_cast<int>(GetLastError()),
                        std::system_category());
                }
            } else if (sourceExists) {
                if (!validMetadata(source, record->sourceGroupId)) {
                    preserveConflict(recordPath, staged,
                        L"移动源的元数据不匹配");
                    continue;
                }
            } else {
                preserveConflict(recordPath, staged,
                    L"移动源、暂存与目标均不存在");
                continue;
            }
            if (!StableLibraryTrusted()) {
                throw std::runtime_error("media library volume became unavailable");
            }
            error.clear();
            if (!fs::remove(recordPath, error) || error) {
                throw std::system_error(error ? error :
                    std::make_error_code(std::errc::operation_canceled));
            }
        }
        moveRecoveryComplete_ = true;
    }

    void MediaLibrary::RecoverInterruptedVariantDeletions(fs::path const& mediaDirectory) const
    {
        RequireTrustedLibrary();
        auto variants = mediaDirectory / L"Variants";
        std::error_code error;
        if (!fs::exists(variants, error)) {
            if (error) throw std::system_error(error);
            return;
        }

        std::vector<fs::path> tombstones;
        for (fs::directory_iterator entries(variants, error), end;
            !error && entries != end; entries.increment(error)) {
            std::error_code itemError;
            auto name = entries->path().filename().wstring();
            auto suffix = name.starts_with(L".deleting-")
                ? name.substr(std::wstring_view(L".deleting-").size()) : std::wstring{};
            if (!suffix.empty() && motion::valid_id(suffix) &&
                entries->is_directory(itemError) && !itemError) {
                auto attributes = GetFileAttributesW(entries->path().c_str());
                if (attributes != INVALID_FILE_ATTRIBUTES &&
                    !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    tombstones.push_back(entries->path());
                }
            }
        }
        if (error) throw std::system_error(error);

        for (auto const& tombstone : tombstones) {
            RequireTrustedLibrary();
            auto preserveConflict = [&](std::wstring_view reason) -> void {
                auto deletingName = tombstone.filename().wstring();
                auto suffix = deletingName.substr(std::wstring_view(L".deleting-").size());
                fs::path conflict;
                DWORD moveError = ERROR_ALREADY_EXISTS;
                for (unsigned attempt = 0; attempt != 8; ++attempt) {
                    auto token = attempt == 0 ? suffix : motion::utf8_to_wide(motion::new_id()).substr(0, 8);
                    auto candidate = variants / (L".conflict-" + token);
                    RequireTrustedLibrary();
                    if (MoveFileExW(tombstone.c_str(), candidate.c_str(), MOVEFILE_WRITE_THROUGH)) {
                        conflict = std::move(candidate);
                        break;
                    }
                    moveError = GetLastError();
                    if (moveError != ERROR_ALREADY_EXISTS && moveError != ERROR_FILE_EXISTS) {
                        throw std::system_error(static_cast<int>(moveError), std::system_category());
                    }
                }
                if (conflict.empty()) {
                    throw std::system_error(static_cast<int>(moveError), std::system_category());
                }
                RequireTrustedLibrary();
                motion::append_utf8_log(root_ / L"Config" / L"app.log",
                    L"优化副本删除恢复发现同名冲突，暂存数据已保留在 " +
                    conflict.wstring() + L"（" + std::wstring(reason) + L"）");
                throw std::runtime_error(
                    "conflicting interrupted variant deletion was preserved");
            };
            std::vector<fs::path> files;
            error.clear();
            for (fs::directory_iterator entries(tombstone, error), end;
                !error && entries != end; entries.increment(error)) {
                std::error_code itemError;
                auto name = entries->path().filename().wstring();
                bool recognized = (name.starts_with(L"balanced-") ||
                    name.starts_with(L"power-saver-")) &&
                    name.ends_with(L".mp4") && !name.ends_with(L".part.mp4");
                auto attributes = GetFileAttributesW(entries->path().c_str());
                if (recognized && motion::safe_file_name(name) &&
                    entries->is_regular_file(itemError) && !itemError &&
                    attributes != INVALID_FILE_ATTRIBUTES &&
                    !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    files.push_back(entries->path());
                }
            }
            if (error) throw std::system_error(error);

            for (auto const& source : files) {
                RequireTrustedLibrary();
                auto destination = variants / source.filename();
                error.clear();
                if (fs::exists(destination, error)) {
                    if (error) throw std::system_error(error);
                    auto attributes = GetFileAttributesW(destination.c_str());
                    bool comparable = attributes != INVALID_FILE_ATTRIBUTES &&
                        !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
                    bool identical{};
                    try {
                        std::error_code sourceSizeError;
                        std::error_code destinationSizeError;
                        auto sourceSize = fs::file_size(source, sourceSizeError);
                        auto destinationSize = fs::file_size(destination, destinationSizeError);
                        comparable = comparable && !sourceSizeError && !destinationSizeError &&
                            sourceSize == destinationSize;
                        if (comparable) {
                            auto sourceHash = sha256_file(source);
                            RequireTrustedLibrary();
                            auto destinationHash = sha256_file(destination);
                            RequireTrustedLibrary();
                            identical = sourceHash == destinationHash;
                        }
                    } catch (...) {
                        RequireTrustedLibrary();
                        comparable = false;
                    }
                    if (!comparable || !identical) {
                        preserveConflict(comparable
                            ? L"同名文件内容不同" : L"同名文件无法安全比较");
                    }
                    RequireTrustedLibrary();
                    error.clear();
                    if (!fs::remove(source, error) || error) {
                        preserveConflict(L"相同副本无法清理");
                    }
                    RequireTrustedLibrary();
                    continue;
                }
                if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
                    throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
                }
                RequireTrustedLibrary();
            }
            RequireTrustedLibrary();
            error.clear();
            if (!fs::remove(tombstone, error) || error) {
                preserveConflict(L"暂存目录含未知或无法恢复的文件");
            }
        }
    }

    fs::path MediaLibrary::ResolveMediaDirectory(motion::MediaMetadata const& media) const
    {
        RequireTrustedLibrary();
        auto preferred = AccessWallpapersPath() / L"Groups" /
            motion::utf8_to_wide(media.groupId) / L"Videos" /
            motion::utf8_to_wide(media.id);
        try {
            if (auto current = motion::load_media(preferred / L"metadata.json");
                current && current->id == media.id) return preferred;
        } catch (...) {}
        std::error_code error;
        auto groups = AccessWallpapersPath() / L"Groups";
        for (fs::directory_iterator entries(groups, error), end; !error && entries != end; entries.increment(error)) {
            auto candidate = entries->path() / L"Videos" / motion::utf8_to_wide(media.id);
            try {
                auto current = motion::load_media(candidate / L"metadata.json");
                if (current && current->id == media.id) return candidate;
            } catch (...) {}
        }
        throw std::runtime_error("media no longer exists");
    }

    void MediaLibrary::Rename(motion::MediaMetadata const& media, std::wstring const& value)
    {
        std::scoped_lock lock(mutex_);
        RequireTrustedLibrary();
        auto name = trim(value);
        if (name.empty()) throw std::runtime_error("empty media name");
        auto path = ResolveMediaDirectory(media) / L"metadata.json";
        auto loaded = motion::load_media(path);
        if (!loaded || loaded->id != media.id) throw std::runtime_error("media changed during rename");
        auto updated = std::move(*loaded);
        updated.name = std::move(name);
        ++updated.revision;
        updated.updatedAt = motion::timestamp_utc();
        motion::save_media(path, updated);
    }

    void MediaLibrary::MutateCatalogMetadata(
        std::vector<motion::MediaMetadata> const& requestedMedia,
        std::function<void(motion::MediaMetadata&)> const& mutation)
    {
        if (!mutation || requestedMedia.empty()) return;
        std::scoped_lock lock(mutex_);
        RequireTrustedLibrary();

        struct PendingUpdate
        {
            fs::path path;
            motion::MediaMetadata before;
            motion::MediaMetadata after;
        };
        std::vector<PendingUpdate> updates;
        std::set<std::string> visited;
        updates.reserve(requestedMedia.size());
        auto changedAt = motion::timestamp_utc();
        for (auto const& requested : requestedMedia) {
            if (!motion::valid_id(requested.id) || !visited.insert(requested.id).second) {
                if (!motion::valid_id(requested.id)) {
                    throw std::invalid_argument("invalid media id");
                }
                continue;
            }
            auto path = ResolveMediaDirectory(requested) / L"metadata.json";
            auto current = motion::load_media(path);
            if (!current || current->id != requested.id) {
                throw std::runtime_error("media changed during catalog update");
            }
            auto updated = *current;
            mutation(updated);
            updated.version = motion::media_schema_version;
            ++updated.revision;
            updated.updatedAt = changedAt;
            updates.push_back({ std::move(path), std::move(*current), std::move(updated) });
        }

        size_t committed{};
        try {
            for (; committed < updates.size(); ++committed) {
                RequireTrustedLibrary();
                motion::save_media(updates[committed].path, updates[committed].after);
            }
        } catch (...) {
            while (committed && StableLibraryTrusted()) {
                --committed;
                try {
                    motion::save_media(updates[committed].path,
                        updates[committed].before);
                } catch (...) {}
            }
            throw;
        }
    }

    void MediaLibrary::SetFavorite(motion::MediaMetadata const& media, bool favorite)
    {
        SetFavorite(std::vector<motion::MediaMetadata>{ media }, favorite);
    }

    void MediaLibrary::SetFavorite(
        std::vector<motion::MediaMetadata> const& media, bool favorite)
    {
        MutateCatalogMetadata(media,
            [favorite](motion::MediaMetadata& item) { item.favorite = favorite; });
    }

    void MediaLibrary::SetTags(motion::MediaMetadata const& media,
        std::vector<std::wstring> const& tags)
    {
        auto normalized = normalize_media_tags(tags);
        MutateCatalogMetadata({ media },
            [normalized = std::move(normalized)](motion::MediaMetadata& item) {
                item.tags = normalized;
            });
    }

    void MediaLibrary::AddTags(std::vector<motion::MediaMetadata> const& media,
        std::vector<std::wstring> const& tags)
    {
        auto normalized = normalize_media_tags(tags);
        MutateCatalogMetadata(media,
            [normalized = std::move(normalized)](motion::MediaMetadata& item) {
                auto combined = item.tags;
                combined.insert(combined.end(), normalized.begin(), normalized.end());
                item.tags = normalize_media_tags(combined);
            });
    }

    motion::MediaMetadata MediaLibrary::MergeDuplicateMedia(
        motion::MediaMetadata const& keep, motion::MediaMetadata const& duplicate)
    {
        if (keep.id == duplicate.id) {
            throw std::invalid_argument("cannot merge a media item into itself");
        }
        std::scoped_lock lock(mutex_);
        RequireTrustedLibrary();
        auto keepDirectory = ResolveMediaDirectory(keep);
        auto duplicateDirectory = ResolveMediaDirectory(duplicate);
        auto currentKeep = motion::load_media(keepDirectory / L"metadata.json");
        auto currentDuplicate = motion::load_media(duplicateDirectory / L"metadata.json");
        if (!currentKeep || !currentDuplicate || currentKeep->id != keep.id ||
            currentDuplicate->id != duplicate.id || currentKeep->sha256.empty() ||
            _wcsicmp(currentKeep->sha256.c_str(), currentDuplicate->sha256.c_str()) != 0 ||
            currentKeep->kind != currentDuplicate->kind ||
            currentKeep->sizeBytes != currentDuplicate->sizeBytes) {
            throw std::runtime_error("media items are not verified duplicates");
        }

        // Metadata hashes are an efficient discovery index, but the files may
        // have been edited outside MotionWallpaper since import. Re-hash both
        // sources immediately before the destructive half of a repair.
        auto keepSource = keepDirectory / currentKeep->fileName;
        auto duplicateSource = duplicateDirectory / currentDuplicate->fileName;
        std::error_code keepSizeError;
        std::error_code duplicateSizeError;
        auto keepSize = fs::file_size(keepSource, keepSizeError);
        auto duplicateSize = fs::file_size(duplicateSource, duplicateSizeError);
        if (keepSizeError || duplicateSizeError ||
            keepSize != currentKeep->sizeBytes ||
            duplicateSize != currentDuplicate->sizeBytes) {
            throw std::runtime_error("duplicate source changed since import");
        }
        auto keepHash = sha256_file(keepSource);
        auto duplicateHash = sha256_file(duplicateSource);
        if (_wcsicmp(keepHash.c_str(), currentKeep->sha256.c_str()) != 0 ||
            _wcsicmp(duplicateHash.c_str(), currentDuplicate->sha256.c_str()) != 0 ||
            _wcsicmp(keepHash.c_str(), duplicateHash.c_str()) != 0) {
            throw std::runtime_error("duplicate source changed since import");
        }

        auto merged = *currentKeep;
        merged.favorite = merged.favorite || currentDuplicate->favorite;
        auto tags = merged.tags;
        tags.insert(tags.end(), currentDuplicate->tags.begin(), currentDuplicate->tags.end());
        merged.tags = normalize_media_tags(tags);
        merged.version = motion::media_schema_version;
        ++merged.revision;
        merged.updatedAt = motion::timestamp_utc();
        motion::save_media(keepDirectory / L"metadata.json", merged);
        // A failed recycle leaves the duplicate intact and the canonical item
        // with a harmless metadata union, so retry never loses user data.
        DeletePath(duplicateDirectory);
        return merged;
    }

    void MediaLibrary::UpdateCover(motion::MediaMetadata const& media, std::wstring const& coverFileName)
    {
        std::scoped_lock lock(mutex_);
        RequireTrustedLibrary();
        if (!motion::safe_file_name(coverFileName)) throw std::runtime_error("invalid cover file name");
        auto directory = ResolveMediaDirectory(media);
        if (!fs::is_regular_file(directory / coverFileName)) throw std::runtime_error("cover does not exist");
        auto loaded = motion::load_media(directory / L"metadata.json");
        if (!loaded || loaded->id != media.id) throw std::runtime_error("media changed during cover update");
        auto updated = std::move(*loaded);
        updated.coverFileName = coverFileName;
        ++updated.revision;
        updated.updatedAt = motion::timestamp_utc();
        motion::save_media(directory / L"metadata.json", updated);
    }

    bool MediaLibrary::EnsureCover(motion::MediaMetadata const& media)
    {
        fs::path source;
        std::string sourceKind;
        {
            std::scoped_lock lock(mutex_);
            RequireTrustedLibrary();
            auto directory = ResolveMediaDirectory(media);
            auto current = motion::load_media(directory / L"metadata.json");
            if (!current || current->id != media.id) return false;
            if (!current->coverFileName.empty()) {
                std::error_code coverError;
                bool coverExists = fs::is_regular_file(
                    directory / current->coverFileName, coverError) && !coverError;
                // Older image imports pointed the UI at the full source file.
                // Replace that legacy reference with the same bounded poster
                // cache used by videos, without touching the wallpaper source.
                bool legacyFullImageCover = current->kind == "image" &&
                    current->coverFileName == current->fileName;
                if (coverExists && !legacyFullImageCover) return false;
            }
            source = directory / current->fileName;
            sourceKind = current->kind;
        }

        HRESULT apartmentResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(apartmentResult) && apartmentResult != RPC_E_CHANGED_MODE) return false;
        struct ApartmentRelease
        {
            bool initialized{};
            ~ApartmentRelease() { if (initialized) CoUninitialize(); }
        } apartment{ SUCCEEDED(apartmentResult) };

        fs::path scratch;
        try {
            scratch = fs::temp_directory_path() /
                (L"MotionWallpaper-cover-" + motion::utf8_to_wide(motion::new_id()));
            fs::create_directory(scratch);
            struct ScratchCleanup
            {
                fs::path path;
                ~ScratchCleanup()
                {
                    std::error_code ignored;
                    fs::remove_all(path, ignored);
                }
            } cleanup{ scratch };
            auto generatedPoster = scratch / L"poster.png";
            bool generated = sourceKind == "image"
                ? ThumbnailGenerator::EnsureImageCover(source, generatedPoster)
                : ThumbnailGenerator::EnsureVideoCover(source, generatedPoster);
            if (!generated) return false;

            // Thumbnail decoding/FFmpeg can take seconds, so it intentionally
            // runs without the media-library mutex. Revalidate identity and
            // current cover choice before atomically publishing the small file.
            std::scoped_lock lock(mutex_);
            RequireTrustedLibrary();
            auto directory = ResolveMediaDirectory(media);
            auto current = motion::load_media(directory / L"metadata.json");
            if (!current || current->id != media.id || current->fileName != source.filename() ||
                current->kind != sourceKind) return false;
            if (!current->coverFileName.empty()) {
                std::error_code coverError;
                bool coverExists = fs::is_regular_file(
                    directory / current->coverFileName, coverError) && !coverError;
                bool legacyFullImageCover = current->kind == "image" &&
                    current->coverFileName == current->fileName;
                if (coverExists && !legacyFullImageCover) return false;
            }

            auto staging = directory /
                (L".poster-" + motion::utf8_to_wide(motion::new_id()) + L".png");
            std::error_code stagingError;
            RequireTrustedLibrary();
            fs::copy_file(generatedPoster, staging, fs::copy_options::overwrite_existing,
                stagingError);
            RequireTrustedLibrary();
            if (stagingError || !MoveFileExW(staging.c_str(), (directory / L"poster.png").c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                if (StableLibraryTrusted()) fs::remove(staging, stagingError);
                return false;
            }
            current->coverFileName = L"poster.png";
            ++current->revision;
            current->updatedAt = motion::timestamp_utc();
            RequireTrustedLibrary();
            motion::save_media(directory / L"metadata.json", *current);
            return true;
        } catch (...) {
            if (!scratch.empty()) {
                std::error_code ignored;
                fs::remove_all(scratch, ignored);
            }
            return false;
        }
    }

    bool MediaLibrary::RequestOptimization(motion::MediaMetadata const& media, std::string const& mode,
        bool automatic)
    {
        std::scoped_lock lock(mutex_);
        RequireTrustedLibrary();
        if (media.kind != "video" || mode == "original") return false;
        auto directory = ResolveMediaDirectory(media);
        RecoverInterruptedVariantDeletions(directory);
        std::error_code error;
        if (!fs::is_regular_file(directory / media.fileName, error) || error) return false;
        if (automatic) return static_cast<bool>(motion::ensure_variant_generation_request(directory, mode));
        return motion::request_variant_generation(directory, mode);
    }

    void MediaLibrary::PauseOptimization(motion::MediaMetadata const& media)
    {
        std::scoped_lock lock(mutex_);
        RequireTrustedLibrary();
        if (!motion::pause_variant_generation(ResolveMediaDirectory(media))) {
            throw std::runtime_error("unable to persist optimization pause");
        }
    }

    void MediaLibrary::ResumeOptimization(motion::MediaMetadata const& media)
    {
        std::scoped_lock lock(mutex_);
        RequireTrustedLibrary();
        if (!motion::resume_variant_generation(ResolveMediaDirectory(media))) {
            throw std::runtime_error("unable to resume optimization");
        }
    }

    void MediaLibrary::CancelOptimization(motion::MediaMetadata const& media)
    {
        std::scoped_lock lock(mutex_);
        RequireTrustedLibrary();
        if (!motion::cancel_variant_generation(ResolveMediaDirectory(media))) {
            throw std::runtime_error("unable to persist optimization cancellation");
        }
    }

    void MediaLibrary::SuppressOptimization(motion::MediaMetadata const& media, std::string const& mode)
    {
        std::scoped_lock lock(mutex_);
        RequireTrustedLibrary();
        if (!motion::suppress_variant_generation(ResolveMediaDirectory(media), mode)) {
            throw std::runtime_error("unable to suppress optimization profile");
        }
    }

    void MediaLibrary::DeleteVariantProfile(motion::MediaMetadata const& media, std::string const& mode)
    {
        DeleteVariantProfiles(media, { mode });
    }

    void MediaLibrary::DeleteVariantProfiles(motion::MediaMetadata const& media,
        std::vector<std::string> const& requestedModes)
    {
        std::scoped_lock lock(mutex_);
        RequireTrustedLibrary();
        std::vector<std::string> modes;
        for (auto const& mode : requestedModes) {
            if (mode != "balanced" && mode != "power-saver") {
                throw std::invalid_argument("unknown optimization profile");
            }
            if (std::find(modes.begin(), modes.end(), mode) == modes.end()) {
                modes.push_back(mode);
            }
        }
        if (modes.empty()) return;
        auto directory = ResolveMediaDirectory(media);
        RecoverInterruptedVariantDeletions(directory);
        auto status = motion::inspect_variant_cache(directory);
        std::error_code sourceError;
        bool sourceAvailable = fs::is_regular_file(directory / media.fileName, sourceError) && !sourceError;
        if (!sourceAvailable) {
            bool hasRetainedProfile = std::any_of(status.entries.begin(), status.entries.end(),
                [&](auto const& entry) {
                    return std::find(modes.begin(), modes.end(), entry.mode) == modes.end() &&
                        entry.bytes;
                });
            if (!hasRetainedProfile) throw std::runtime_error("cannot delete the last playable media file");
        }
        auto variants = directory / L"Variants";
        std::error_code error;
        if (!fs::exists(variants, error)) {
            if (error) throw std::system_error(error);
            return;
        }

        struct StagedVariant
        {
            fs::path source;
            fs::path staged;
        };
        std::vector<StagedVariant> selected;
        for (fs::directory_iterator entry(variants, error), end; !error && entry != end; entry.increment(error)) {
            std::error_code itemError;
            if (!entry->is_regular_file(itemError) || itemError) continue;
            auto name = entry->path().filename().wstring();
            auto selectedMode = std::find_if(modes.begin(), modes.end(), [&](auto const& mode) {
                return name.starts_with(motion::utf8_to_wide(mode) + L"-");
            });
            if (selectedMode == modes.end()) continue;
            selected.push_back({ entry->path(), {} });
        }
        if (error) throw std::system_error(error);

        // Rename every selected profile into one owned tombstone directory
        // before deleting anything. A failed rename can therefore be rolled
        // back without leaving a half-deleted multi-profile selection.
        // Keep the private staging name short. Media paths can already be near
        // MAX_PATH on supported Windows versions, and the full UUID used here
        // previously made an otherwise valid variant impossible to delete.
        fs::path tombstone;
        for (unsigned attempt = 0; attempt != 8 && tombstone.empty(); ++attempt) {
            auto token = motion::new_id();
            token.resize(8);
            auto candidate = variants / (L".deleting-" + motion::utf8_to_wide(token));
            RequireTrustedLibrary();
            error.clear();
            if (fs::create_directory(candidate, error)) {
                tombstone = std::move(candidate);
            } else if (error && error != std::errc::file_exists) {
                throw std::system_error(error);
            }
        }
        if (tombstone.empty()) {
            throw std::system_error(std::make_error_code(std::errc::file_exists));
        }
        auto rollback = [&]() noexcept {
            for (auto item = selected.rbegin(); item != selected.rend(); ++item) {
                if (!StableLibraryTrusted()) return;
                if (item->staged.empty()) continue;
                MoveFileExW(item->staged.c_str(), item->source.c_str(), MOVEFILE_WRITE_THROUGH);
            }
            if (StableLibraryTrusted()) {
                std::error_code ignored;
                fs::remove(tombstone, ignored);
            }
        };
        try {
            for (auto& item : selected) {
                RequireTrustedLibrary();
                item.staged = tombstone / item.source.filename();
                if (!MoveFileExW(item.source.c_str(), item.staged.c_str(), MOVEFILE_WRITE_THROUGH)) {
                    throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
                }
            }
            DeletePath(tombstone);
        } catch (...) {
            rollback();
            throw;
        }

        // Commit durable suppression only after the selected files have been
        // removed. A failure can make a profile eligible for regeneration, but
        // can never strand an existing file behind a stale suppression marker.
        std::vector<std::string> suppressed;
        for (auto const& mode : modes) {
            RequireTrustedLibrary();
            if (!motion::suppress_variant_generation(directory, mode)) {
                for (auto const& committed : suppressed) {
                    if (!StableLibraryTrusted()) break;
                    motion::unsuppress_variant_generation(directory, committed);
                }
                throw std::runtime_error("unable to persist optimization suppression");
            }
            suppressed.push_back(mode);
        }
        RequireTrustedLibrary();
        fs::remove(variants, error);
        if (error && error != std::errc::directory_not_empty) throw std::system_error(error);
    }

    void MediaLibrary::DeleteVariants(motion::MediaMetadata const& media)
    {
        std::scoped_lock lock(mutex_);
        RequireTrustedLibrary();
        auto directory = ResolveMediaDirectory(media);
        RecoverInterruptedVariantDeletions(directory);
        std::error_code sourceError;
        if (!fs::is_regular_file(directory / media.fileName, sourceError) || sourceError) {
            throw std::runtime_error("cannot delete the last playable media files");
        }
        if (!motion::cancel_variant_generation(directory)) {
            throw std::runtime_error("unable to persist optimization cancellation");
        }
        RequireTrustedLibrary();
        std::error_code error;
        fs::remove_all(directory / L"Variants", error);
        RequireTrustedLibrary();
        if (error || fs::exists(directory / L"Variants")) {
            throw std::system_error(error ? error : std::make_error_code(std::errc::operation_canceled));
        }
    }

    void MediaLibrary::ReclaimVariantsForStorageQuota(
        motion::MediaMetadata const& media)
    {
        std::scoped_lock lock(mutex_);
        RequireTrustedLibrary();
        auto directory = ResolveMediaDirectory(media);
        RecoverInterruptedVariantDeletions(directory);
        std::error_code sourceError;
        if (!fs::is_regular_file(directory / media.fileName, sourceError) || sourceError) {
            throw std::runtime_error("cannot reclaim the last playable media files");
        }
        if (!motion::abandon_variant_generation_for_cache_eviction(directory)) {
            throw std::runtime_error("unable to abandon optimization work for cache eviction");
        }
        RequireTrustedLibrary();
        std::error_code error;
        fs::remove_all(directory / L"Variants", error);
        RequireTrustedLibrary();
        if (error || fs::exists(directory / L"Variants")) {
            throw std::system_error(error ? error :
                std::make_error_code(std::errc::operation_canceled));
        }
    }

    motion::VariantCacheStatus MediaLibrary::VariantStatus(motion::MediaMetadata const& media) const
    {
        std::scoped_lock lock(mutex_);
        try {
            RequireTrustedLibrary();
            auto directory = ResolveMediaDirectory(media);
            RecoverInterruptedVariantDeletions(directory);
            return motion::inspect_variant_cache(directory);
        }
        catch (...) { return {}; }
    }

    OptimizationStorageSummary MediaLibrary::InspectOptimizationStorage()
    {
        OptimizationStorageSummary result;
        for (auto const& item : QueryMedia()) {
            auto status = VariantStatus(item.media);
            result.bytes += status.bytes;
            result.files += status.files;
            result.queuedTasks += status.queued ? 1u : 0u;
            if (status.files) {
                ++result.mediaWithCopies;
                if (SourceAvailable(item.media)) {
                    result.reclaimableBytes += status.bytes;
                }
            }
        }
        return result;
    }

    OptimizationCleanupResult MediaLibrary::TrimOptimizationStorage(
        uint64_t quotaBytes, std::vector<std::string> const& protectedMediaIds)
    {
        OptimizationCleanupResult result;
        result.before = InspectOptimizationStorage();
        if (result.before.bytes <= quotaBytes) {
            result.after = result.before;
            return result;
        }

        std::set<std::string> protectedIds;
        for (auto const& id : protectedMediaIds) {
            if (!motion::valid_id(id)) throw std::invalid_argument("invalid protected media id");
            protectedIds.insert(id);
        }
        struct Candidate
        {
            motion::MediaMetadata media;
            uint64_t bytes{};
            fs::file_time_type lastUsed{ fs::file_time_type::min() };
        };
        std::vector<Candidate> candidates;
        for (auto const& item : QueryMedia()) {
            auto status = VariantStatus(item.media);
            if (!status.files) continue;
            if (protectedIds.contains(item.media.id)) {
                ++result.skippedProtected;
                continue;
            }
            if (!SourceAvailable(item.media)) {
                ++result.skippedSourceLess;
                continue;
            }
            Candidate candidate{ item.media, status.bytes };
            std::error_code error;
            auto variants = MediaDirectory(item.media) / L"Variants";
            for (fs::directory_iterator entries(variants, error), end;
                !error && entries != end; entries.increment(error)) {
                std::error_code itemError;
                auto modified = entries->last_write_time(itemError);
                if (!itemError && modified > candidate.lastUsed) {
                    candidate.lastUsed = modified;
                }
            }
            candidates.push_back(std::move(candidate));
        }
        std::stable_sort(candidates.begin(), candidates.end(),
            [](auto const& left, auto const& right) {
                if (left.lastUsed != right.lastUsed) return left.lastUsed < right.lastUsed;
                return left.media.id < right.media.id;
            });

        uint64_t remaining = result.before.bytes;
        for (auto const& candidate : candidates) {
            if (remaining <= quotaBytes) break;
            ReclaimVariantsForStorageQuota(candidate.media);
            remaining = remaining >= candidate.bytes ? remaining - candidate.bytes : 0;
            ++result.cleanedMedia;
        }
        result.after = InspectOptimizationStorage();
        result.freedBytes = result.before.bytes >= result.after.bytes
            ? result.before.bytes - result.after.bytes : 0;
        return result;
    }

    OptimizationCleanupResult MediaLibrary::ReleaseOptimizationStorage(
        std::vector<std::string> const& protectedMediaIds)
    {
        return TrimOptimizationStorage(0, protectedMediaIds);
    }

    bool MediaLibrary::SourceAvailable(motion::MediaMetadata const& media) const
    {
        std::scoped_lock lock(mutex_);
        try {
            RequireTrustedLibrary();
            auto directory = ResolveMediaDirectory(media);
            std::error_code error;
            return fs::is_regular_file(directory / media.fileName, error) && !error;
        } catch (...) {
            return false;
        }
    }

    void MediaLibrary::DeleteSource(motion::MediaMetadata const& media)
    {
        bool coverRequired{};
        {
            std::scoped_lock lock(mutex_);
            RequireTrustedLibrary();
            auto directory = ResolveMediaDirectory(media);
            auto current = motion::load_media(directory / L"metadata.json");
            if (!current || current->id != media.id || current->kind != "video") {
                throw std::runtime_error("only video sources can be removed");
            }
            std::error_code sourceError;
            if (!fs::is_regular_file(directory / current->fileName, sourceError) || sourceError) return;
            RecoverInterruptedVariantDeletions(directory);
            auto status = motion::inspect_variant_cache(directory);
            bool hasPlayableCopy = std::any_of(status.entries.begin(), status.entries.end(),
                [](auto const& entry) { return entry.bytes != 0; });
            if (!hasPlayableCopy) throw std::runtime_error("no playable performance copy exists");
            std::error_code coverError;
            coverRequired = current->coverFileName.empty() ||
                !fs::is_regular_file(directory / current->coverFileName, coverError) || coverError;
        }

        // Poster generation is an unbounded codec operation. EnsureCover uses
        // a system-temp scratch file and revalidates immediately before its
        // atomic publish, so no slow decoder writes through a reused drive path.
        if (coverRequired) {
            EnsureCover(media);
        }

        std::scoped_lock lock(mutex_);
        RequireTrustedLibrary();
        auto directory = ResolveMediaDirectory(media);
        auto current = motion::load_media(directory / L"metadata.json");
        if (!current || current->id != media.id || current->kind != "video") {
            throw std::runtime_error("media changed while preserving its poster");
        }
        auto source = directory / current->fileName;
        std::error_code sourceError;
        if (!fs::is_regular_file(source, sourceError) || sourceError) return;
        RecoverInterruptedVariantDeletions(directory);
        auto status = motion::inspect_variant_cache(directory);
        bool hasPlayableCopy = std::any_of(status.entries.begin(), status.entries.end(),
            [](auto const& entry) { return entry.bytes != 0; });
        std::error_code coverError;
        bool hasCover = !current->coverFileName.empty() &&
            fs::is_regular_file(directory / current->coverFileName, coverError) && !coverError;
        if (!hasPlayableCopy || !hasCover) {
            throw std::runtime_error("source removal prerequisites changed");
        }
        RequireTrustedLibrary();
        if (!motion::cancel_variant_generation(directory)) {
            throw std::runtime_error("unable to stop source-dependent optimization");
        }
        RequireTrustedLibrary();
        DeletePath(source);
    }

    void MediaLibrary::Move(motion::MediaMetadata const& media, std::string const& targetGroupId)
    {
        std::scoped_lock lock(mutex_);
        RequireTrustedLibrary();
        if (!motion::valid_id(targetGroupId)) return;
        RecoverInterruptedMoves();
        auto source = ResolveMediaDirectory(media);
        auto sourceMetadata = motion::load_media(source / L"metadata.json");
        if (!sourceMetadata || sourceMetadata->id != media.id) throw std::runtime_error("media changed during move");
        if (sourceMetadata->groupId == targetGroupId) return;
        auto targetGroup = AccessWallpapersPath() / L"Groups" / motion::utf8_to_wide(targetGroupId);
        if (!fs::is_regular_file(targetGroup / L"group.json")) throw std::runtime_error("move target group does not exist");
        auto targetRoot = targetGroup / L"Videos";
        auto target = targetRoot / motion::utf8_to_wide(media.id);
        if (fs::exists(target)) throw std::runtime_error("duplicate move target");
        fs::create_directories(targetRoot);
        auto ownershipId = expectedIdentity_
            ? std::optional<std::string>(expectedIdentity_->ownershipId)
            : motion::media_library_ownership_id(AccessWallpapersPath());
        if (!ownershipId) throw std::runtime_error("media library ownership marker is unavailable");

        fs::path staging;
        fs::path recordPath;
        for (unsigned attempt = 0; attempt != 8; ++attempt) {
            auto token = recycle_token();
            staging = targetRoot / (std::wstring(moveStagePrefix) + token);
            recordPath = targetRoot / (std::wstring(moveRecordPrefix) + token);
            try {
                write_move_restore_record(recordPath, *ownershipId,
                    MoveRestoreRecord{ media.id, sourceMetadata->groupId, targetGroupId });
                break;
            } catch (std::system_error const& exception) {
                auto code = static_cast<DWORD>(exception.code().value());
                staging.clear();
                recordPath.clear();
                if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) throw;
            }
        }
        if (staging.empty() || recordPath.empty()) {
            throw std::system_error(ERROR_ALREADY_EXISTS, std::system_category());
        }
        moveRecoveryComplete_ = false;

        DWORD moveError = ERROR_SHARING_VIOLATION;
        for (int attempt = 0; attempt < 30; ++attempt) {
            RequireTrustedLibrary();
            if (MoveFileExW(source.c_str(), staging.c_str(), MOVEFILE_WRITE_THROUGH)) { moveError = ERROR_SUCCESS; break; }
            moveError = GetLastError();
            Sleep(100);
        }
        if (moveError != ERROR_SUCCESS) {
            if (StableLibraryTrusted()) {
                std::error_code ignored;
                fs::remove(recordPath, ignored);
                moveRecoveryComplete_ = !ignored;
            }
            throw std::system_error(static_cast<int>(moveError), std::system_category());
        }
        try {
            RequireTrustedLibrary();
            auto loaded = motion::load_media(staging / L"metadata.json");
            if (!loaded || loaded->id != media.id || loaded->groupId != sourceMetadata->groupId) {
                throw std::runtime_error("media changed during move");
            }
            loaded->groupId = targetGroupId;
            ++loaded->revision;
            loaded->updatedAt = motion::timestamp_utc();
            RequireTrustedLibrary();
            motion::save_media(staging / L"metadata.json", *loaded);
            RequireTrustedLibrary();
            if (!MoveFileExW(staging.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
            }
        } catch (...) {
            if (StableLibraryTrusted()) {
                bool restored{};
                bool metadataRestored{};
                try {
                    if (auto rollback = motion::load_media(staging / L"metadata.json")) {
                        if (rollback->id == sourceMetadata->id) {
                            rollback->groupId = sourceMetadata->groupId;
                            ++rollback->revision;
                            rollback->updatedAt = motion::timestamp_utc();
                            motion::save_media(staging / L"metadata.json", *rollback);
                            metadataRestored = true;
                        }
                    }
                } catch (...) {}
                if (metadataRestored && StableLibraryTrusted()) {
                    restored = MoveFileExW(staging.c_str(), source.c_str(),
                        MOVEFILE_WRITE_THROUGH) != FALSE;
                }
                if (restored) {
                    std::error_code ignored;
                    fs::remove(recordPath, ignored);
                    moveRecoveryComplete_ = !ignored;
                }
            }
            throw;
        }
        std::error_code recordError;
        fs::remove(recordPath, recordError);
        moveRecoveryComplete_ = !recordError;
    }

    void MediaLibrary::Delete(motion::MediaMetadata const& media)
    {
        std::scoped_lock lock(mutex_);
        RequireTrustedLibrary();
        DeletePath(ResolveMediaDirectory(media));
    }

    void MediaLibrary::DeletePath(fs::path const& path) const
    {
        RequireTrustedLibrary();
        RecoverInterruptedMoves();
        RecoverInterruptedRecycleDeletes();
        std::error_code error;
        auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            auto pathError = GetLastError();
            if (pathError == ERROR_FILE_NOT_FOUND ||
                pathError == ERROR_PATH_NOT_FOUND) return;
            throw std::system_error(static_cast<int>(pathError),
                std::system_category());
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            throw std::runtime_error("refusing to delete a reparse point");
        }
        RequireTrustedLibrary();
        if (deleteMode_ == DeleteMode::Permanent) {
            fs::remove_all(path, error);
            if (!StableLibraryTrusted()) {
                throw std::runtime_error("media library volume became unavailable");
            }
            if (error || fs::exists(path)) throw std::system_error(error ? error : std::make_error_code(std::errc::operation_canceled));
            return;
        }

        // SHFileOperation cannot consume extended-length volume GUID paths.
        // For an external library first rename the exact stable-path object to
        // an unguessable sibling, then let Shell see only that random alias.
        // A drive-letter replacement can therefore never receive the original
        // logical path as a deletion target.
        if (expectedIdentity_) {
            auto configuredOriginal = ConfiguredAliasPath(path);
            (void)configuredOriginal;
            fs::path staged;
            fs::path record;
            DWORD moveError = ERROR_ALREADY_EXISTS;
            for (unsigned attempt = 0; attempt != 8; ++attempt) {
                auto token = recycle_token();
                staged = path.parent_path() /
                    (std::wstring(recycleStagePrefix) + token);
                record = path.parent_path() /
                    (std::wstring(recycleRestorePrefix) + token);
                try {
                    write_recycle_restore_record(record,
                        expectedIdentity_->ownershipId, path.filename());
                } catch (std::system_error const& exception) {
                    auto code = static_cast<DWORD>(exception.code().value());
                    if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
                        continue;
                    }
                    throw;
                }
                if (MoveFileExW(path.c_str(), staged.c_str(), MOVEFILE_WRITE_THROUGH)) {
                    moveError = ERROR_SUCCESS;
                    break;
                }
                moveError = GetLastError();
                if (StableLibraryTrusted()) {
                    std::error_code ignored;
                    fs::remove(record, ignored);
                }
                staged.clear();
                record.clear();
                if (moveError != ERROR_ALREADY_EXISTS &&
                    moveError != ERROR_FILE_EXISTS) break;
            }
            if (moveError != ERROR_SUCCESS || staged.empty() || record.empty()) {
                throw std::system_error(static_cast<int>(moveError),
                    std::system_category());
            }
            recycleRecoveryComplete_ = false;

            auto rollback = [&]() noexcept {
                if (!StableLibraryTrusted()) return;
                auto stagedAttributes = GetFileAttributesW(staged.c_str());
                auto originalAttributes = GetFileAttributesW(path.c_str());
                if (stagedAttributes != INVALID_FILE_ATTRIBUTES &&
                    originalAttributes == INVALID_FILE_ATTRIBUTES) {
                    MoveFileExW(staged.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH);
                }
                if (GetFileAttributesW(staged.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    std::error_code ignored;
                    fs::remove(record, ignored);
                }
            };

            auto aliasStage = ConfiguredAliasPath(staged);
            try {
                RequireTrustedLibrary();
                if (!motion::same_direct_filesystem_object(staged, aliasStage)) {
                    throw std::runtime_error("media library alias changed before recycle");
                }
                auto alias = aliasStage.wstring();
                if (alias.size() + 2 >= MAX_PATH) {
                    throw std::system_error(ERROR_FILENAME_EXCED_RANGE,
                        std::system_category());
                }
                alias.push_back(L'\0');
                alias.push_back(L'\0');
                SHFILEOPSTRUCTW operation{};
                operation.wFunc = FO_DELETE;
                operation.pFrom = alias.c_str();
                operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION |
                    FOF_NOERRORUI | FOF_SILENT | FOF_NO_CONNECTED_ELEMENTS;
                int result = SHFileOperationW(&operation);
                if (!StableLibraryTrusted()) {
                    throw std::runtime_error("media library volume became unavailable");
                }
                if (result || operation.fAnyOperationsAborted ||
                    GetFileAttributesW(staged.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    throw std::system_error(result ? result : ERROR_CANCELLED,
                        std::system_category());
                }
                error.clear();
                fs::remove(record, error);
                // The payload is already gone. A retained record is harmless
                // and is deterministically removed by the reconnect recovery.
                recycleRecoveryComplete_ = !error;
                return;
            } catch (...) {
                rollback();
                recycleRecoveryComplete_ =
                    StableLibraryTrusted() &&
                    GetFileAttributesW(record.c_str()) == INVALID_FILE_ATTRIBUTES;
                throw;
            }
        }

        std::wstring source = path.wstring();
        source.push_back(L'\0');
        source.push_back(L'\0');
        SHFILEOPSTRUCTW operation{};
        operation.wFunc = FO_DELETE;
        operation.pFrom = source.c_str();
        operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        int result = SHFileOperationW(&operation);
        if (!StableLibraryTrusted()) {
            throw std::runtime_error("media library volume became unavailable");
        }
        if (result || operation.fAnyOperationsAborted || fs::exists(path)) {
            throw std::system_error(result ? result : ERROR_CANCELLED, std::system_category());
        }
    }

}
