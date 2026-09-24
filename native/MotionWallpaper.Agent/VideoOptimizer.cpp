#include "VideoOptimizer.h"

#include "VideoTranscoder.h"
#include "VideoVariantPolicy.h"
#include "VideoStillPreview.h"
#include "../MotionWallpaper.Common/Common.h"
#include "../MotionWallpaper.Common/VariantCache.h"
#include "../MotionWallpaper.Common/MediaProbe.h"
#include "../MotionWallpaper.Renderer/AdapterPolicy.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace winrt;

namespace
{
    struct SourceRate
    {
        uint32_t numerator{};
        uint32_t denominator{};
        uint32_t width{};
        uint32_t height{};
        uint32_t encodedWidth{};
        uint32_t encodedHeight{};
        uint64_t duration100ns{};
        bool matchesSdrOutput{};
        uint32_t profile{};
        bool profileKnown{};
        motion::agent::VideoSourceCodec codec{ motion::agent::VideoSourceCodec::Unknown };
    };

    struct VariantValidationSpec
    {
        uint32_t width{}, height{}, targetFps{};
        uint64_t duration100ns{};
        bool operator==(VariantValidationSpec const&) const = default;
    };

    struct VariantFileFingerprint
    {
        motion::FilesystemObjectIdentity identity;
        uint64_t size{}, modified{};
        bool operator==(VariantFileFingerprint const&) const = default;
    };

    struct VariantValidationEntry
    {
        VariantFileFingerprint source;
        VariantFileFingerprint variant;
        VariantValidationSpec specification;
        bool valid{};
    };

    std::optional<VariantFileFingerprint> variant_file_fingerprint(HANDLE file) noexcept
    {
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(file, &information) ||
            (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return {};
        VariantFileFingerprint result;
        result.size = (static_cast<uint64_t>(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
        result.modified = (static_cast<uint64_t>(information.ftLastWriteTime.dwHighDateTime) << 32) |
            information.ftLastWriteTime.dwLowDateTime;
        FILE_ID_INFO identity{};
        if (GetFileInformationByHandleEx(file, FileIdInfo, &identity, sizeof(identity))) {
            result.identity.volumeSerialNumber = identity.VolumeSerialNumber;
            std::copy(std::begin(identity.FileId.Identifier), std::end(identity.FileId.Identifier),
                result.identity.fileId.begin());
        } else {
            // FAT/exFAT may not expose FILE_ID_INFO. The handle's volume and
            // 64-bit file index still distinguish ordinary replacements.
            result.identity.volumeSerialNumber = information.dwVolumeSerialNumber;
            auto index = (static_cast<uint64_t>(information.nFileIndexHigh) << 32) | information.nFileIndexLow;
            for (size_t byte = 0; byte < sizeof(index); ++byte) {
                result.identity.fileId[byte] = static_cast<uint8_t>(index >> (byte * 8));
            }
        }
        return result.size ? std::optional<VariantFileFingerprint>(result) : std::nullopt;
    }

    SourceRate source_rate(fs::path const& ffmpeg, fs::path const& source,
        std::function<bool()> const& cancelled = {})
    {
        auto info = motion::probe_video(ffmpeg, source, 10000, cancelled);
        if (!info) return {};
        SourceRate result;
        result.numerator = info->frameRateNumerator;
        result.denominator = info->frameRateDenominator;
        result.width = info->width;
        result.height = info->height;
        result.encodedWidth = info->width;
        result.encodedHeight = info->height;
        if (info->rotationDegrees == 90 || info->rotationDegrees == 270) {
            std::swap(result.width, result.height);
        }
        result.duration100ns = info->duration100ns;
        using motion::agent::VideoSourceCodec;
        if (info->codecName == "h264") {
            result.codec = VideoSourceCodec::H264;
            result.profile = info->profile == "High" ? 100u : info->profile == "Main" ? 77u :
                info->profile == "Baseline" || info->profile == "Constrained Baseline" ? 66u : 0u;
            result.profileKnown = result.profile != 0;
        } else if (info->codecName == "hevc") {
            result.codec = VideoSourceCodec::Hevc;
            result.profile = info->profile == "Main 10" ? 2u : info->profile == "Main" ? 1u : 0u;
            result.profileKnown = result.profile != 0;
        } else if (info->codecName == "vp9") {
            result.codec = VideoSourceCodec::Vp9;
            result.profile = info->profile == "Profile 2" ? 2u : 0u;
            result.profileKnown = info->profile == "Profile 0" || info->profile == "Profile 2";
        } else if (info->codecName == "av1") {
            result.codec = VideoSourceCodec::Av1;
            result.profileKnown = info->profile == "Main";
        }
        result.matchesSdrOutput = info->rotationDegrees == 0 && motion::agent::video_matches_sdr_output(
            info->codecName, info->pixelFormat, info->bitDepth,
            info->colorTransfer, info->colorPrimaries, info->colorSpace, info->colorRange);
        return result;
    }

    struct SourceDecodeRequirement
    {
        motion::agent::VideoHardwareDecodeProfile profile{
            motion::agent::VideoHardwareDecodeProfile::Unsupported };
        uint32_t width{};
        uint32_t height{};
        uint32_t frameRateNumerator{};
        uint32_t frameRateDenominator{};
    };

    SourceDecodeRequirement source_decode_requirement(fs::path const& ffmpeg, fs::path const& source)
    {
        auto info = source_rate(ffmpeg, source);
        return { motion::agent::video_hardware_decode_profile(
                info.codec, info.profileKnown, info.profile),
            info.encodedWidth, info.encodedHeight, info.numerator, info.denominator };
    }

    bool decoder_configuration_available(ID3D11VideoDevice* videoDevice, GUID const& profile,
        DXGI_FORMAT format, uint32_t width, uint32_t height) noexcept
    {
        if (!videoDevice || !width || !height) return false;
        bool profileAdvertised{};
        auto count = videoDevice->GetVideoDecoderProfileCount();
        for (UINT index = 0; index < count; ++index) {
            GUID advertised{};
            if (SUCCEEDED(videoDevice->GetVideoDecoderProfile(index, &advertised)) &&
                advertised == profile) {
                profileAdvertised = true;
                break;
            }
        }
        if (!profileAdvertised) return false;

        BOOL formatSupported{};
        if (FAILED(videoDevice->CheckVideoDecoderFormat(&profile, format, &formatSupported)) ||
            !formatSupported) return false;
        D3D11_VIDEO_DECODER_DESC description{};
        description.Guid = profile;
        description.SampleWidth = width;
        description.SampleHeight = height;
        description.OutputFormat = format;
        UINT configurations{};
        return SUCCEEDED(videoDevice->GetVideoDecoderConfigCount(&description, &configurations)) &&
            configurations > 0;
    }

    bool device_supports_decode_requirement(ID3D11VideoDevice* videoDevice,
        SourceDecodeRequirement const& requirement) noexcept
    {
        using motion::agent::VideoHardwareDecodeProfile;
        switch (requirement.profile) {
        case VideoHardwareDecodeProfile::H264:
            return decoder_configuration_available(videoDevice,
                D3D11_DECODER_PROFILE_H264_VLD_NOFGT, DXGI_FORMAT_NV12,
                requirement.width, requirement.height);
        case VideoHardwareDecodeProfile::HevcMain:
            return decoder_configuration_available(videoDevice,
                D3D11_DECODER_PROFILE_HEVC_VLD_MAIN, DXGI_FORMAT_NV12,
                requirement.width, requirement.height);
        case VideoHardwareDecodeProfile::HevcMain10:
            return decoder_configuration_available(videoDevice,
                D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10, DXGI_FORMAT_P010,
                requirement.width, requirement.height);
        case VideoHardwareDecodeProfile::Vp9Profile0:
            return decoder_configuration_available(videoDevice,
                D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0, DXGI_FORMAT_NV12,
                requirement.width, requirement.height);
        case VideoHardwareDecodeProfile::Vp9Profile2:
            return decoder_configuration_available(videoDevice,
                D3D11_DECODER_PROFILE_VP9_VLD_10BIT_PROFILE2, DXGI_FORMAT_P010,
                requirement.width, requirement.height);
        case VideoHardwareDecodeProfile::Av1Profile0:
            // MF's native AV1 type does not expose bit depth independently
            // from sequence profile 0. Require both common output formats so
            // an 8-bit-only decoder cannot be mistaken for 10-bit capability.
            return decoder_configuration_available(videoDevice,
                    D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0, DXGI_FORMAT_NV12,
                    requirement.width, requirement.height) &&
                decoder_configuration_available(videoDevice,
                    D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0, DXGI_FORMAT_P010,
                    requirement.width, requirement.height);
        default:
            return false;
        }
    }

    std::wstring adapter_luid_key(LUID luid)
    {
        return std::to_wstring(luid.HighPart) + L":" + std::to_wstring(luid.LowPart);
    }

    int adapter_display_rank(IDXGIAdapter1* adapter, std::wstring const& preferredAdapter)
    {
        if (!adapter) return 2;
        DXGI_ADAPTER_DESC1 adapterDescription{};
        if (FAILED(adapter->GetDesc1(&adapterDescription))) return 2;
        if (!preferredAdapter.empty() &&
            adapter_luid_key(adapterDescription.AdapterLuid) == preferredAdapter) return 0;
        bool hasDesktopOutput{};
        for (UINT index = 0;; ++index) {
            com_ptr<IDXGIOutput> output;
            auto status = adapter->EnumOutputs(index, output.put());
            if (status == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(status) || !output) break;
            DXGI_OUTPUT_DESC description{};
            if (SUCCEEDED(output->GetDesc(&description)) && description.AttachedToDesktop) {
                hasDesktopOutput = true;
            }
        }
        return hasDesktopOutput ? 1 : 2;
    }

    std::vector<motion::agent::gpu_probe_detail::RankedDecodeAdapter> source_hardware_decode_adapters(fs::path const& ffmpeg, fs::path const& source,
        std::wstring const& preferredAdapter, uint64_t aggregateOutputPixels, std::wstring const& onlyAdapter)
    {
        auto requirement = source_decode_requirement(ffmpeg, source);
        if (requirement.profile == motion::agent::VideoHardwareDecodeProfile::Unsupported) return {};

        com_ptr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put()))) || !factory) return {};
        struct Candidate
        {
            com_ptr<IDXGIAdapter1> adapter;
            motion::renderer::AdapterCandidate policy;
            std::wstring luid;
        };
        std::vector<Candidate> candidates;
        auto highPerformance = motion::renderer::prefer_high_performance_adapter(
            requirement.width, requirement.height,
            requirement.frameRateNumerator, requirement.frameRateDenominator,
            aggregateOutputPixels);
        auto factory6 = factory.try_as<IDXGIFactory6>();
        bool gpuPreferenceOrderAvailable = static_cast<bool>(factory6);
        if (factory6) {
            auto preference = highPerformance ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE :
                DXGI_GPU_PREFERENCE_MINIMUM_POWER;
            for (UINT index = 0;; ++index) {
                com_ptr<IDXGIAdapter1> adapter;
                auto status = factory6->EnumAdapterByGpuPreference(
                    index, preference, __uuidof(IDXGIAdapter1), adapter.put_void());
                if (status == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(status) || !adapter) break;
                DXGI_ADAPTER_DESC1 description{};
                if (FAILED(adapter->GetDesc1(&description)) ||
                    (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
                auto displayRank = adapter_display_rank(adapter.get(), preferredAdapter);
                candidates.push_back({ std::move(adapter), {
                    index, displayRank,
                    static_cast<uint64_t>(description.DedicatedVideoMemory) },
                    adapter_luid_key(description.AdapterLuid) });
            }
        } else {
            for (UINT index = 0;; ++index) {
                com_ptr<IDXGIAdapter1> adapter;
                auto status = factory->EnumAdapters1(index, adapter.put());
                if (status == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(status) || !adapter) break;
                DXGI_ADAPTER_DESC1 description{};
                if (FAILED(adapter->GetDesc1(&description)) ||
                    (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
                auto displayRank = adapter_display_rank(adapter.get(), preferredAdapter);
                candidates.push_back({ std::move(adapter), {
                    index, displayRank,
                    static_cast<uint64_t>(description.DedicatedVideoMemory) },
                    adapter_luid_key(description.AdapterLuid) });
            }
        }
        std::stable_sort(candidates.begin(), candidates.end(), [&](auto const& left, auto const& right) {
            return motion::renderer::adapter_candidate_precedes(
                left.policy, right.policy, highPerformance, gpuPreferenceOrderAvailable);
        });

        D3D_FEATURE_LEVEL levels[]{
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        std::vector<motion::agent::gpu_probe_detail::RankedDecodeAdapter> supported;
        for (size_t rank = 0; rank < candidates.size(); ++rank) {
            auto const& candidate = candidates[rank];
            if (!onlyAdapter.empty() && candidate.luid != onlyAdapter) continue;
            com_ptr<ID3D11Device> device;
            if (FAILED(D3D11CreateDevice(candidate.adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                    levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                    device.put(), nullptr, nullptr)) || !device) continue;
            auto videoDevice = device.try_as<ID3D11VideoDevice>();
            if (videoDevice && device_supports_decode_requirement(videoDevice.get(), requirement)) {
                supported.push_back({ static_cast<uint32_t>(rank), candidate.luid });
            }
        }
        return supported;
    }

    bool current_variant(fs::path const& source, fs::path const& variant)
    {
        std::error_code error;
        if (!fs::is_regular_file(variant, error) || error || !fs::file_size(variant, error) || error) return false;
        auto sourceTime = fs::last_write_time(source, error);
        if (error) return false;
        auto variantTime = fs::last_write_time(variant, error);
        return !error && variantTime >= sourceTime;
    }

    std::optional<fs::path> rebase_path_under_root(fs::path const& sourceRoot,
        fs::path const& destinationRoot, fs::path const& path) noexcept
    {
        try {
            auto normalizedSource = sourceRoot.lexically_normal();
            auto normalizedDestination = destinationRoot.lexically_normal();
            auto normalizedPath = path.lexically_normal();
            if (!normalizedSource.is_absolute() || !normalizedDestination.is_absolute() ||
                !normalizedPath.is_absolute()) return std::nullopt;

            auto rootPart = normalizedSource.begin();
            auto pathPart = normalizedPath.begin();
            for (; rootPart != normalizedSource.end(); ++rootPart, ++pathPart) {
                if (pathPart == normalizedPath.end()) return std::nullopt;
                auto left = rootPart->native();
                auto right = pathPart->native();
                if (CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                        right.data(), static_cast<int>(right.size()), TRUE) != CSTR_EQUAL) {
                    return std::nullopt;
                }
            }

            fs::path relative;
            for (; pathPart != normalizedPath.end(); ++pathPart) {
                if (*pathPart == L"." || *pathPart == L".." ||
                    pathPart->has_root_name() || pathPart->has_root_directory()) {
                    return std::nullopt;
                }
                relative /= *pathPart;
            }
            return relative.empty()
                ? std::optional<fs::path>(normalizedDestination)
                : std::optional<fs::path>(normalizedDestination / relative);
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<fs::path> unique_link_temporary(fs::path const& destination) noexcept
    {
        try {
            auto token = motion::new_variant_request_id();
            if (token.empty()) return std::nullopt;
            auto temporary = destination;
            temporary += L".link.tmp-" + motion::utf8_to_wide(token);
            return temporary;
        } catch (...) {
            return std::nullopt;
        }
    }

    bool reuse_equivalent_variant(fs::path const& source, fs::path const& destination,
        std::string const& mode)
    {
        if (mode == "cpu-smooth") return false;
        auto name = destination.filename().wstring();
        auto prefix = mode == "power-saver" ? std::wstring(L"power-saver-") : std::wstring(L"balanced-");
        auto alternatePrefix = mode == "power-saver" ? std::wstring(L"balanced-") : std::wstring(L"power-saver-");
        if (!name.starts_with(prefix)) return false;
        auto alternate = destination.parent_path() / (alternatePrefix + name.substr(prefix.size()));
        if (!current_variant(source, alternate)) return false;
        std::error_code ignored;
        fs::create_directories(destination.parent_path(), ignored);
        auto temporary = unique_link_temporary(destination);
        if (!temporary) return false;
        fs::remove(*temporary, ignored);
        if (!CreateHardLinkW(temporary->c_str(), alternate.c_str(), nullptr)) return false;
        if (!MoveFileExW(temporary->c_str(), destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            fs::remove(*temporary, ignored);
            return false;
        }
        return current_variant(source, destination);
    }

    bool on_battery()
    {
        SYSTEM_POWER_STATUS status{};
        return GetSystemPowerStatus(&status) && status.ACLineStatus == 0;
    }

    void append_log(fs::path const& root, std::wstring const& message)
    {
        motion::append_utf8_log(root / L"Config" / L"agent.log", message);
    }

    constexpr uint64_t defaultVariantCacheLimit = 10ULL * 1024 * 1024 * 1024;
    constexpr uint64_t diskReserve = 256ULL * 1024 * 1024;

    struct PhysicalVariantIdentityLess
    {
        bool operator()(motion::FilesystemObjectIdentity const& left,
            motion::FilesystemObjectIdentity const& right) const noexcept
        {
            if (left.volumeSerialNumber != right.volumeSerialNumber) {
                return left.volumeSerialNumber < right.volumeSerialNumber;
            }
            return left.fileId < right.fileId;
        }
    };

    std::optional<motion::FilesystemObjectIdentity> physical_variant_identity(
        fs::path const& path) noexcept
    {
        motion::unique_handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!file) return std::nullopt;
        BY_HANDLE_FILE_INFORMATION basic{};
        FILE_ID_INFO identity{};
        if (!GetFileInformationByHandle(file.get(), &basic) ||
            (basic.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
            !GetFileInformationByHandleEx(file.get(), FileIdInfo, &identity, sizeof(identity))) {
            return std::nullopt;
        }
        motion::FilesystemObjectIdentity result;
        result.volumeSerialNumber = identity.VolumeSerialNumber;
        std::copy(std::begin(identity.FileId.Identifier), std::end(identity.FileId.Identifier),
            result.fileId.begin());
        return result;
    }

    bool has_transcode_space(fs::path const& directory, fs::path const& source,
        VariantValidationSpec const& specification)
    {
        ULARGE_INTEGER available{};
        if (!GetDiskFreeSpaceExW(directory.c_str(), &available, nullptr, nullptr)) return false;
        std::error_code error;
        auto sourceSize = fs::file_size(source, error);
        if (error) return false;
        auto rate = motion::agent::video_transcode_rate_control(
            specification.width, specification.height, specification.targetFps,
            motion::agent::VideoTranscodeCodec::H264, sourceSize, specification.duration100ns);
        auto outputBudget = rate.maximumOutputBytes;
        auto worstCase = outputBudget > UINT64_MAX - diskReserve
            ? UINT64_MAX : outputBudget + diskReserve;
        return available.QuadPart >= worstCase;
    }

    void prune_variant_cache(fs::path const& wallpapers, uint64_t cacheLimit,
        motion::VariantRemovalCallback const& removeCandidate)
    {
        if (!cacheLimit) return;
        struct PhysicalAllocation
        {
            uint64_t size{};
            size_t remainingCacheLinks{};
        };
        struct Candidate
        {
            fs::path path;
            fs::file_time_type modified{};
            size_t allocationIndex{};
        };
        std::vector<PhysicalAllocation> allocations;
        std::map<motion::FilesystemObjectIdentity, size_t,
            PhysicalVariantIdentityLess> allocationByIdentity;
        std::vector<Candidate> candidates;
        uint64_t total{};
        auto addPhysicalBytes = [&](uint64_t size) {
            total = total > UINT64_MAX - size ? UINT64_MAX : total + size;
        };
        std::error_code error;
        fs::recursive_directory_iterator iterator(wallpapers,
            fs::directory_options::skip_permission_denied, error), end;
        for (; !error && iterator != end; iterator.increment(error)) {
            std::error_code itemError;
            if (!iterator->is_regular_file(itemError) || itemError ||
                iterator->path().parent_path().filename() != L"Variants") continue;
            auto size = iterator->file_size(itemError);
            if (itemError) continue;
            size_t allocationIndex{};
            auto identity = physical_variant_identity(iterator->path());
            if (identity) {
                auto [entry, inserted] = allocationByIdentity.try_emplace(
                    *identity, allocations.size());
                if (inserted) {
                    allocationIndex = allocations.size();
                    allocations.push_back({ size, 1 });
                    addPhysicalBytes(size);
                } else {
                    allocationIndex = entry->second;
                    auto& allocation = allocations[allocationIndex];
                    ++allocation.remainingCacheLinks;
                    // All hard links normally report the same size. If the file
                    // changes during traversal, keep the largest observation so
                    // accounting remains conservative rather than wrapping or
                    // silently dropping physical bytes.
                    if (size > allocation.size) {
                        addPhysicalBytes(size - allocation.size);
                        allocation.size = size;
                    }
                }
            } else {
                // Unknown identity is deliberately treated as a unique physical
                // allocation. This can over-count an inaccessible hard link, but
                // never under-count cache pressure or merge unrelated files.
                allocationIndex = allocations.size();
                allocations.push_back({ size, 1 });
                addPhysicalBytes(size);
            }
            auto mediaDirectory = iterator->path().parent_path().parent_path();
            motion::MediaMetadata metadata;
            if (motion::try_load_media(mediaDirectory / L"metadata.json", metadata)) {
                std::error_code sourceError;
                if (!fs::is_regular_file(mediaDirectory / metadata.fileName, sourceError) || sourceError) {
                    // Once the source is deliberately removed, retained variants
                    // become the wallpaper's only playable media and are not an
                    // evictable cache anymore.
                    continue;
                }
            }
            candidates.push_back({ iterator->path(),
                iterator->last_write_time(itemError), allocationIndex });
        }
        if (total <= cacheLimit) return;
        std::sort(candidates.begin(), candidates.end(), [](auto const& left, auto const& right) {
            return left.modified < right.modified;
        });
        for (auto const& candidate : candidates) {
            if (total <= cacheLimit) break;
            if (!removeCandidate || !removeCandidate(candidate.path)) continue;
            auto& allocation = allocations[candidate.allocationIndex];
            if (allocation.remainingCacheLinks && --allocation.remainingCacheLinks == 0) {
                total = total >= allocation.size ? total - allocation.size : 0;
            }
        }
    }

    std::wstring variant_prefix(std::string const& mode)
    {
        return mode == "balanced" ? L"balanced-" :
            mode == "power-saver" ? L"power-saver-" : L"cpu-smooth-";
    }

    void coalesce_equivalent_profiles(fs::path const& variants) noexcept
    {
        try {
            fs::path balanced, powerSaver;
            std::error_code error;
            for (fs::directory_iterator entries(variants, error), end;
                !error && entries != end; entries.increment(error)) {
                std::error_code itemError;
                if (!entries->is_regular_file(itemError) || itemError) continue;
                auto name = entries->path().filename().wstring();
                if (name.starts_with(L"balanced-") && name.ends_with(L"-v7.mp4")) balanced = entries->path();
                else if (name.starts_with(L"power-saver-") && name.ends_with(L"-v7.mp4")) powerSaver = entries->path();
            }
            if (balanced.empty() || powerSaver.empty()) return;
            auto balancedName = balanced.filename().wstring();
            auto powerSaverName = powerSaver.filename().wstring();
            if (balancedName.substr(std::wstring(L"balanced-").size()) !=
                powerSaverName.substr(std::wstring(L"power-saver-").size())) return;

            auto temporary = unique_link_temporary(powerSaver);
            if (!temporary) return;
            fs::remove(*temporary, error);
            if (!CreateHardLinkW(temporary->c_str(), balanced.c_str(), nullptr)) return;
            if (!MoveFileExW(temporary->c_str(), powerSaver.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                fs::remove(*temporary, error);
            }
        } catch (...) {}
    }

    void normalize_variant_profiles(fs::path const& wallpapers)
    {
        std::set<fs::path> directories;
        std::set<fs::path> mediaDirectories;
        std::error_code error;
        fs::recursive_directory_iterator iterator(wallpapers,
            fs::directory_options::skip_permission_denied, error), end;
        for (; !error && iterator != end; iterator.increment(error)) {
            std::error_code itemError;
            if (iterator->is_regular_file(itemError) && !itemError &&
                iterator->path().parent_path().filename() == L"Variants") {
                directories.insert(iterator->path().parent_path());
            }
            if (iterator->is_regular_file(itemError) && !itemError &&
                iterator->path().filename() == L"metadata.json") {
                mediaDirectories.insert(iterator->path().parent_path());
            }
        }

        // A crashed agent cannot resume an FFmpeg partial. Normalize its
        // durable UI state before accepting work so a stale "generating 73%"
        // never survives while the next attempt is actually queued at zero.
        for (auto const& mediaDirectory : mediaDirectories) {
            auto request = motion::read_variant_generation_request(mediaDirectory);
            if (!request) {
                motion::clear_variant_progress(mediaDirectory);
            } else if (motion::variant_generation_paused(mediaDirectory)) {
                auto prior = motion::read_variant_progress(mediaDirectory);
                motion::write_variant_progress_if_current(mediaDirectory, request,
                    motion::VariantProgressState::paused,
                    prior && prior->mode == request.mode && prior->requestId == request.requestId
                        ? prior->percent : 0,
                    prior && prior->mode == request.mode && prior->requestId == request.requestId &&
                        prior->determinate);
            } else {
                motion::write_variant_progress_if_current(mediaDirectory, request,
                    motion::VariantProgressState::queued);
            }
        }

        struct Candidate
        {
            fs::path path;
            bool currentPolicy{};
            fs::file_time_type modified{};
        };
        for (auto const& variants : directories) {
            // No transcoder exists while the optimizer is being constructed.
            // Any partial at this point belongs to a crashed or cancelled run.
            motion::remove_variant_partials(variants.parent_path());
            for (auto const& mode : { std::string("balanced"), std::string("power-saver"),
                std::string("cpu-smooth") }) {
                std::optional<Candidate> keep;
                std::error_code entriesError;
                for (fs::directory_iterator entries(variants, entriesError), entriesEnd;
                    !entriesError && entries != entriesEnd; entries.increment(entriesError)) {
                    std::error_code itemError;
                    if (!entries->is_regular_file(itemError) || itemError) continue;
                    auto name = entries->path().filename().wstring();
                    if (!name.starts_with(variant_prefix(mode)) || name.ends_with(L".part.mp4") ||
                        entries->path().extension() != L".mp4") continue;
                    Candidate candidate{ entries->path(), name.ends_with(L"-v7.mp4"),
                        entries->last_write_time(itemError) };
                    if (itemError) continue;
                    if (!keep || (candidate.currentPolicy && !keep->currentPolicy) ||
                        (candidate.currentPolicy == keep->currentPolicy && candidate.modified > keep->modified)) {
                        keep = std::move(candidate);
                    }
                }
                if (keep) motion::retain_variant_profile(
                    variants.parent_path(), mode, keep->path.filename().wstring());
            }
            coalesce_equivalent_profiles(variants);
        }
    }
}

namespace motion::agent
{
    int run_video_gpu_probe_cli(int argc, wchar_t** argv)
    {
        if (argc < 2 || (wcscmp(argv[1], L"--probe-gpu-inventory") != 0 &&
            wcscmp(argv[1], L"--probe-gpu-decode") != 0 &&
            wcscmp(argv[1], L"--probe-cuda-device") != 0)) return -1;
        auto status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(status)) return 2;
        struct ComGuard { ~ComGuard() { CoUninitialize(); } } guard;
        try {
            std::ostringstream output;
            if (wcscmp(argv[1], L"--probe-cuda-device") == 0) {
                if (argc != 3) return 2;
                // Load optional vendor code only in this bounded child. CUDA
                // ordinals are NOT DXGI indices; match the Windows adapter LUID.
                auto module = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (!module) return 3;
                struct ModuleGuard { HMODULE value; ~ModuleGuard() { FreeLibrary(value); } } moduleGuard{ module };
                auto initialize = reinterpret_cast<int (WINAPI*)(unsigned)>(GetProcAddress(module, "cuInit"));
                auto getCount = reinterpret_cast<int (WINAPI*)(int*)>(GetProcAddress(module, "cuDeviceGetCount"));
                auto getDevice = reinterpret_cast<int (WINAPI*)(int*, int)>(GetProcAddress(module, "cuDeviceGet"));
                auto getLuid = reinterpret_cast<int (WINAPI*)(char*, unsigned*, int)>(GetProcAddress(module, "cuDeviceGetLuid"));
                int count{};
                if (!initialize || !getCount || !getDevice || !getLuid || initialize(0) || getCount(&count) || count > 64) return 3;
                bool found{};
                for (int index = 0; index < count; ++index) {
                    int device{}; unsigned nodeMask{}; LUID luid{};
                    if (getDevice(&device, index) || getLuid(reinterpret_cast<char*>(&luid), &nodeMask, device)) continue;
                    auto identity = std::to_wstring(luid.HighPart) + L":" + std::to_wstring(luid.LowPart);
                    if (identity != argv[2]) continue;
                    output << "cuda-v1 " << index << ' ' << luid.HighPart << ' ' << luid.LowPart << '\n';
                    found = true;
                    break;
                }
                if (!found) return 3;
            } else if (wcscmp(argv[1], L"--probe-gpu-decode") == 0) {
                if (argc != 7) return 2;
                size_t consumed{};
                auto pixels = std::stoull(argv[5], &consumed);
                if (consumed != wcslen(argv[5])) return 2;
                auto candidates = source_hardware_decode_adapters(argv[2], argv[3],
                    wcscmp(argv[4], L"none") == 0 ? std::wstring{} : std::wstring(argv[4]), pixels, argv[6]);
                output << gpu_probe_detail::ranked_decode_output(std::move(candidates));
            } else {
                if (argc != 2) return 2;
                com_ptr<IDXGIFactory1> factory;
                if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) return 3;
                std::vector<std::string> adapters;
                for (UINT index = 0; index < 64; ++index) {
                    com_ptr<IDXGIAdapter1> adapter;
                    auto result = factory->EnumAdapters1(index, adapter.put());
                    if (result == DXGI_ERROR_NOT_FOUND) break;
                    if (FAILED(result)) return 3;
                    DXGI_ADAPTER_DESC1 description{};
                    if (FAILED(adapter->GetDesc1(&description))) return 3;
                    if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
                    LARGE_INTEGER driver{};
                    adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
                    std::vector<std::string> displays;
                    for (UINT displayIndex = 0; displayIndex < 64; ++displayIndex) {
                        com_ptr<IDXGIOutput> display;
                        auto displayResult = adapter->EnumOutputs(displayIndex, display.put());
                        if (displayResult == DXGI_ERROR_NOT_FOUND) break;
                        if (FAILED(displayResult)) return 3;
                        DXGI_OUTPUT_DESC value{};
                        if (SUCCEEDED(display->GetDesc(&value)) && value.AttachedToDesktop)
                            displays.push_back(wide_to_utf8(value.DeviceName));
                    }
                    std::ostringstream line;
                    line << description.VendorId << ' ' << description.DedicatedVideoMemory << ' ' << index << ' ' <<
                        description.AdapterLuid.HighPart << ' ' << description.AdapterLuid.LowPart << ' ' <<
                        static_cast<uint64_t>(driver.QuadPart) << ' ' << displays.size();
                    for (auto const& display : displays) line << ' ' << display;
                    adapters.push_back(line.str());
                }
                // Device creation is tested separately per adapter so one bad
                // driver cannot prevent us enumerating the remaining devices.
                output << "gpu-v1 " << (adapters.empty() ? 0 : 1) << ' ' << adapters.size() << '\n';
                for (auto const& adapter : adapters) output << adapter << '\n';
            }
            auto text = output.str();
            DWORD written{};
            return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()),
                &written, nullptr) && written == text.size() ? 0 : 3;
        } catch (...) { return 3; }
    }

    struct VideoOptimizer::Impl
    {
        struct Request
        {
            fs::path source;
            fs::path destination;
            uint32_t targetFps{};
            uint32_t width{};
            uint32_t height{};
            uint64_t duration100ns{};
            std::string mode;
            motion::VariantGenerationRequest durableRequest;
            uint64_t generation{};
            bool explicitRequest{};
            bool softwareFallbackAllowed{ true };
            bool softwarePlaybackTarget{};
            std::string failureContext;

            [[nodiscard]] std::wstring Key() const
            {
                return source.wstring() + L"\n" + destination.filename().wstring();
            }
        };

        Impl(fs::path wallpapersPath, fs::path logRoot, fs::path applicationRoot,
            std::optional<motion::MediaLibraryTrustIdentity> libraryTrust)
            : wallpapersPath_(std::move(wallpapersPath)),
              logRoot_(std::move(logRoot)),
              ffmpeg_(motion::ffmpeg_executable_path(applicationRoot)),
              libraryTrust_(std::move(libraryTrust))
        {
            auto wallpapersAccess = AcquireStableAccess(wallpapersPath_);
            if (!wallpapersAccess) {
                throw std::runtime_error("media library trust identity changed");
            }
            // Startup normalization performs deletions and durable progress
            // repair. Keep the root identity handles alive for the whole pass
            // and resolve every path through the volume-GUID namespace.
            normalize_variant_profiles(wallpapersAccess->path);
            stillPreviews_ = std::make_unique<VideoStillPreview>(ffmpeg_,
                applicationRoot / L"Config" / L"DesktopPreviews");
            mediaFoundationStarted_ = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL));
            if (mediaFoundationStarted_) {
                worker_ = std::jthread([this](std::stop_token stop) { Run(stop); });
            } else {
                append_log(logRoot_, L"无法初始化媒体优化器，所需性能副本不可用，保留原视频并显示静态预览。");
            }
        }

        ~Impl()
        {
            stillPreviews_.reset();
            gpu_probe_detail::service().Quiesce(15000);
            worker_.request_stop();
            generation_.fetch_add(1, std::memory_order_relaxed);
            condition_.notify_all();
            if (worker_.joinable()) worker_.join();
            if (mediaFoundationStarted_) MFShutdown();
        }

        ResolvedVideoPath ResolveWithLease(fs::path const& source, std::string const& requestedMode,
            uint32_t targetWidth, uint32_t targetHeight, uint32_t targetRefreshRate,
            bool softwarePlaybackTarget, bool acquirePlaybackLease,
            bool allowGenerationRequest)
        {
            auto trust = AcquireLibraryTrust();
            if (libraryTrust_ && !trust) return {};
            auto sourceResult = [&](bool performanceCopyRequired = false,
                bool performanceCopyPending = false, std::string reason = {}) -> ResolvedVideoPath {
                return { source, RetainLibraryPlaybackLease(trust, acquirePlaybackLease),
                    performanceCopyRequired, performanceCopyPending, false, std::move(reason) };
            };
            if (source.empty()) return {};
            auto stableSource = StablePath(source);
            auto stableMediaDirectory = StablePath(source.parent_path());
            if (!stableSource || !stableMediaDirectory) return {};
            // Original is an explicit direct-play request even if a caller
            // still carries a software compatibility flag from an earlier
            // selection. It must never adopt or generate a replacement.
            if (requestedMode == "original") return sourceResult();
            auto mode = softwarePlaybackTarget ? std::string("cpu-smooth") : requestedMode;
            bool selectedPerformanceMode = !softwarePlaybackTarget &&
                (mode == "balanced" || mode == "power-saver");
            bool playbackCopyMode = selectedPerformanceMode || softwarePlaybackTarget;
            // If the optimizer itself is unavailable, fail closed for a
            // selected performance tier or a CPU compatibility request.
            if (!mediaFoundationStarted_) return sourceResult(playbackCopyMode);
            SourceRate rate;
            bool rateCached{};
            auto rateKey = source.wstring() + L"\n" + motion::utf8_to_wide(
                video_file_fingerprint(*stableSource) + "|" + video_file_fingerprint(ffmpeg_.parent_path() / L"ffprobe.exe"));
            {
                std::lock_guard lock(mutex_);
                auto found = rates_.find(rateKey);
                if (found != rates_.end()) { rate = found->second; rateCached = true; }
            }
            if (!rateCached) {
                rate = source_rate(ffmpeg_, *stableSource);
                if (!rate.numerator) append_log(logRoot_, L"视频信息探测失败或超时: " + source.filename().wstring());
                std::lock_guard lock(mutex_);
                rates_[rateKey] = rate;
            }

            auto dimensions = video_sdr_variant_dimensions(
                mode, rate.width, rate.height, targetWidth, targetHeight);
            auto decision = video_variant_decision(
                mode, dimensions.first, dimensions.second,
                rate.numerator, rate.denominator,
                targetRefreshRate);
            bool codecNeedsVariant = !rate.matchesSdrOutput;
            if (!video_needs_variant(rate.numerator, rate.denominator, decision.targetFps,
                rate.width, rate.height, dimensions.first, dimensions.second) && !codecNeedsVariant) return sourceResult();
            if (!rate.width || !rate.height || !rate.numerator || !rate.denominator) {
                if (selectedPerformanceMode) {
                    auto request = EnsureAutomaticRequest(source.parent_path(), mode,
                        motion::VariantProgressState::queued);
                    if (request) FailGeneration(source.parent_path(), request, {},
                        "视频信息读取失败，请检查源文件和视频工具后重试。");
                }
                return sourceResult(true);
            }
            auto destination = source.parent_path() / L"Variants" / decision.fileName;
            auto key = source.wstring() + L"\n" + decision.fileName;

            std::shared_ptr<void> playbackLease;
            auto playbackLeaseOutput = acquirePlaybackLease ? &playbackLease : nullptr;
            VariantValidationSpec expected{ dimensions.first, dimensions.second, decision.targetFps, rate.duration100ns };
            if (TryAdoptVariant(source, mode, destination, expected, playbackLeaseOutput, trust)) {
                return { destination, std::move(playbackLease), false, false };
            }
            auto legacyName = unchanged_legacy_fill_variant(decision.fileName,
                rate.width, rate.height, dimensions.first, dimensions.second);
            if (!legacyName.empty()) {
                auto legacy = destination.parent_path() / legacyName;
                if (TryAdoptVariant(source, mode, legacy, expected, playbackLeaseOutput, trust)) {
                    return { legacy, std::move(playbackLease), false, false };
                }
            }
            if (ReuseEquivalentVariant(source, destination, mode)) {
                if (TryAdoptVariant(source, mode, destination, expected, playbackLeaseOutput, trust)) {
                    append_log(logRoot_, L"复用相同规格的壁纸优化副本: " + destination.filename().wstring());
                    return { destination, std::move(playbackLease), false, false };
                }
            }
            auto inventory = video_gpu_inventory_async();
            if (inventory.pending) {
                auto result = sourceResult(playbackCopyMode);
                result.gpuProbePending = true;
                return result;
            }
            auto failureContext = FailureContext(*stableSource, destination, inventory);
            // Cancellation, pause and profile suppression control generation,
            // not adoption of an already validated copy or presentation
            // safety. Once we know a missing copy is genuinely required,
            // retain that fact even when work cannot be queued.
            if (fs::is_regular_file(motion::variant_cancelled_path(*stableMediaDirectory)))
                return sourceResult(playbackCopyMode, false, "performance-copy-cancelled");
            if (motion::variant_generation_paused(*stableMediaDirectory))
                return sourceResult(playbackCopyMode, false, "performance-copy-paused");
            if (motion::variant_generation_suppressed(*stableMediaDirectory, mode))
                return sourceResult(playbackCopyMode, false, "performance-copy-deleted");
            if (GenerationFailed(source.parent_path(), mode))
                return sourceResult(playbackCopyMode, false, "performance-copy-failed");
            bool battery = on_battery();
            auto durableRequest = selectedPerformanceMode
                ? EnsureAutomaticRequest(source.parent_path(), mode,
                    battery ? motion::VariantProgressState::waitingForPower :
                        motion::VariantProgressState::queued)
                : motion::VariantGenerationRequest{};
            // A conflicting explicit request, or a marker that landed while
            // probing the source, must not be replaced by automatic playback.
            if (selectedPerformanceMode && !durableRequest) {
                return sourceResult(true);
            }
            bool performanceCopyPending{};
            {
                std::lock_guard lock(mutex_);
                auto currentGeneration = generation_.load(std::memory_order_relaxed);
                bool activeCurrent = active_ && active_->Key() == key &&
                    active_->generation == currentGeneration;
                auto pending = std::find_if(pending_.begin(), pending_.end(),
                    [&](auto const& request) { return request.Key() == key; });
                bool queued = pending != pending_.end();
                if (selectedPerformanceMode && durableRequest) {
                    // A durable request with no failure marker is either the
                    // automatic request above or a fresh UI retry.
                    failed_.erase(key);
                }
                bool canGenerate = generationAllowed_ && !battery &&
                    failed_.find(key) == failed_.end();
                bool mayEnqueue = allowGenerationRequest && canGenerate;
                if (mayEnqueue && !activeCurrent && !queued) {
                    // Resolve is the playback path, so its target is the
                    // wallpaper the user is waiting to see. Put it ahead of
                    // background/import work without interrupting an active
                    // transcode, which keeps prioritization cheap and stable.
                    pending_.push_front(Request{ source, destination, decision.targetFps,
                        dimensions.first, dimensions.second, rate.duration100ns, mode,
                        durableRequest, currentGeneration,
                        static_cast<bool>(durableRequest),
                        true,
                        softwarePlaybackTarget, failureContext });
                    if (softwarePlaybackTarget) {
                        append_log(logRoot_, L"源视频无法直接播放，已排队生成 H.264 兼容副本: " +
                            source.filename().wstring());
                    }
                    condition_.notify_one();
                    queued = true;
                } else if (mayEnqueue && !activeCurrent && queued) {
                    if (durableRequest) {
                        pending->explicitRequest = true;
                        pending->durableRequest = durableRequest;
                    }
                    if (pending != pending_.begin()) {
                        auto selected = std::move(*pending);
                        pending_.erase(pending);
                        pending_.push_front(std::move(selected));
                    }
                }
                // cpu-smooth is internal rather than a durable UI request, but
                // it obeys the same static-preview barrier as selected
                // balanced/power-saver copies before it may be queued.
                performanceCopyPending = playbackCopyMode && canGenerate &&
                    (activeCurrent || queued);
            }
            // Never cache a fallback. Resolve must keep observing generation
            // eligibility and discover a completed target without another mode
            // toggle or process restart.
            return sourceResult(playbackCopyMode, performanceCopyPending);
        }

        void Prepare(fs::path const& source, std::string const& mode,
            uint32_t targetWidth, uint32_t targetHeight, uint32_t targetRefreshRate)
        {
            auto trust = AcquireLibraryTrust();
            if (libraryTrust_ && !trust) return;
            if (source.empty() || mode == "original" || !mediaFoundationStarted_) return;
            auto mediaDirectory = source.parent_path();
            auto stableSource = StablePath(source);
            auto stableMediaDirectory = StablePath(mediaDirectory);
            if (!stableSource || !stableMediaDirectory) return;
            auto durableRequest = motion::read_variant_generation_request(*stableMediaDirectory);
            if (durableRequest.mode != mode) return;
            auto durableRequestIsCurrent = [&] {
                if (!LibraryTrusted()) return false;
                return motion::read_variant_generation_request(*stableMediaDirectory) == durableRequest &&
                    !fs::is_regular_file(motion::variant_cancelled_path(*stableMediaDirectory)) &&
                    !motion::variant_generation_paused(*stableMediaDirectory) &&
                    !motion::variant_generation_suppressed(*stableMediaDirectory, mode);
            };
            // importedRequests is deliberately cached by the Agent between
            // scans. Revalidate the durable marker before probing the source,
            // clearing a failure, pruning, or enqueueing any work.
            if (!durableRequestIsCurrent()) return;
            SourceRate rate;
            bool rateCached{};
            auto rateKey = source.wstring() + L"\n" + motion::utf8_to_wide(
                video_file_fingerprint(*stableSource) + "|" + video_file_fingerprint(ffmpeg_.parent_path() / L"ffprobe.exe"));
            {
                std::lock_guard lock(mutex_);
                auto found = rates_.find(rateKey);
                // An explicit retry must not inherit a transient probe failure.
                if (found != rates_.end() && found->second.width && found->second.height &&
                    found->second.numerator && found->second.denominator) {
                    rate = found->second;
                    rateCached = true;
                }
            }
            if (!rateCached) {
                rate = source_rate(ffmpeg_, *stableSource);
                std::lock_guard lock(mutex_);
                rates_[rateKey] = rate;
            }
            if (!rate.width || !rate.height || !rate.numerator || !rate.denominator) {
                FailGeneration(mediaDirectory, durableRequest, {},
                    "视频信息读取失败，请检查源文件和视频工具后重试。");
                return;
            }
            auto dimensions = video_sdr_variant_dimensions(mode, rate.width, rate.height, targetWidth, targetHeight);
            auto decision = video_variant_decision(mode, dimensions.first, dimensions.second,
                rate.numerator, rate.denominator, targetRefreshRate);
            if (!durableRequestIsCurrent()) return;
            if (rate.matchesSdrOutput && !video_needs_variant(rate.numerator, rate.denominator, decision.targetFps,
                rate.width, rate.height, dimensions.first, dimensions.second)) {
                CompleteGeneration(mediaDirectory, durableRequest);
                return;
            }
            auto destination = mediaDirectory / L"Variants" / decision.fileName;
            auto inventory = video_gpu_inventory_async();
            if (inventory.pending) return;
            auto failureContext = FailureContext(*stableSource, destination, inventory);
            VariantValidationSpec expected{ dimensions.first, dimensions.second, decision.targetFps, rate.duration100ns };
            if (TryAdoptVariant(source, mode, destination, expected)) {
                CompleteGeneration(mediaDirectory, durableRequest);
                return;
            }
            auto legacyName = unchanged_legacy_fill_variant(decision.fileName,
                rate.width, rate.height, dimensions.first, dimensions.second);
            if (!legacyName.empty() && TryAdoptVariant(source, mode,
                    destination.parent_path() / legacyName, expected)) {
                CompleteGeneration(mediaDirectory, durableRequest);
                return;
            }
            if (ReuseEquivalentVariant(source, destination, mode)) {
                if (TryAdoptVariant(source, mode, destination, expected)) {
                    CompleteGeneration(mediaDirectory, durableRequest);
                    return;
                }
            }
            if (!durableRequestIsCurrent()) return;
            auto key = source.wstring() + L"\n" + decision.fileName;
            auto battery = on_battery();
            auto publishState = motion::VariantProgressState::none;
            {
                std::lock_guard lock(mutex_);
                // A durable request is a fresh user retry. A prior automatic or
                // explicit failure for the same filename must not immediately
                // reject it from the in-memory failure cache.
                failed_.erase(key);
                if (!generationAllowed_ || battery) {
                    publishState = battery ? motion::VariantProgressState::waitingForPower
                        : motion::VariantProgressState::queued;
                } else if (active_ && active_->Key() == key) {
                    return;
                } else {
                    bool alreadyPending{};
                    for (auto& pending : pending_) {
                        if (pending.Key() != key) continue;
                        pending.explicitRequest = true;
                        pending.durableRequest = durableRequest;
                        alreadyPending = true;
                        break;
                    }
                    if (!alreadyPending) {
                        pending_.push_back(Request{ source, destination, decision.targetFps,
                            dimensions.first, dimensions.second, rate.duration100ns, mode,
                            durableRequest, generation_.load(std::memory_order_relaxed), true,
                            true, false, failureContext });
                        condition_.notify_one();
                    }
                    publishState = motion::VariantProgressState::queued;
                }
            }
            // Keep durable I/O outside the optimizer mutex so Resolve() on the
            // playback path never waits behind FlushFileBuffers.
            WriteProgress(mediaDirectory, durableRequest, publishState);
        }

        VideoGpuDecodeProbe SourceHardwareDecodeCandidates(fs::path const& source,
            std::wstring const& preferredAdapter, uint64_t aggregateOutputPixels)
        {
            auto trust = AcquireLibraryTrust();
            if (libraryTrust_ && !trust) return {};
            if (source.empty() || !mediaFoundationStarted_) return {};
            auto stableSource = StablePath(source);
            if (!stableSource) return {};
            return video_gpu_decode_async(ffmpeg_, *stableSource, preferredAdapter,
                aggregateOutputPixels, std::static_pointer_cast<void>(trust));
        }

        VideoPlaybackLease AcquirePlaybackLease(fs::path const& path)
        {
            auto trust = AcquireLibraryTrust();
            if (libraryTrust_ && !trust) return {};
            if (path.empty()) return {};
            try {
                if (!motion::filesystem_path_is_nested(wallpapersPath_, path)) return {};
            } catch (...) {
                return {};
            }

            bool variant = _wcsicmp(path.parent_path().filename().c_str(), L"Variants") == 0 &&
                _wcsicmp(path.extension().c_str(), L".mp4") == 0 &&
                !path.filename().wstring().ends_with(L".part.mp4");

            // Existence and variant-lease publication share the removal mutex.
            // Source paths do not need a cache pin, but external ones still
            // return an identity-only token for Renderer's entire lifetime.
            std::lock_guard lock(mutex_);
            auto stablePath = StablePath(path);
            if (!stablePath) return {};
            std::error_code error;
            if (!fs::is_regular_file(*stablePath, error) || error) return {};
            return variant
                ? RetainPlaybackVariantLocked(path, trust)
                : RetainLibraryPlaybackLease(trust, true);
        }

        ResolvedVideoPath ResolveStillPreview(fs::path const& source)
        {
            if (source.empty() || !motion::filesystem_path_is_nested(wallpapersPath_, source)) return {};
            auto access = AcquireStableAccess(source);
            if (!access) return {};
            struct SourceLease
            {
                std::shared_ptr<motion::MediaLibraryTrustLease> trust;
                VideoPlaybackLease playback;
            };
            auto lease = std::make_shared<SourceLease>(access->trust, AcquirePlaybackLease(source));
            auto preview = stillPreviews_->Read(access->path, std::move(lease));
            return { std::move(preview.path), std::move(preview.lease) };
        }

        void InvalidateChoices()
        {
            invalidate_video_gpu_probes();
            generation_.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard lock(mutex_);
            RetireChoicesLocked();
            failed_.clear();
            pending_.clear();
            hardwareDecodeAdapters_.clear();
            rates_.clear();
            validatedVariants_.clear();
        }

        void SetGenerationAllowed(bool allowed)
        {
            std::lock_guard lock(mutex_);
            if (generationAllowed_ == allowed) return;
            generationAllowed_ = allowed;
            if (allowed) {
                RetireChoicesLocked();
            } else {
                generation_.fetch_add(1, std::memory_order_relaxed);
                pending_.clear();
            }
        }

        [[nodiscard]] bool Quiesce(uint32_t timeoutMilliseconds)
        {
            auto started = std::chrono::steady_clock::now();
            {
                std::lock_guard lock(mutex_);
                if (generationAllowed_) {
                    generationAllowed_ = false;
                    generation_.fetch_add(1, std::memory_order_relaxed);
                }
                pending_.clear();
                condition_.notify_all();
            }
            bool probesIdle = gpu_probe_detail::service().Quiesce(timeoutMilliseconds);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            auto remaining = elapsed >= timeoutMilliseconds ? 0u : timeoutMilliseconds - static_cast<uint32_t>(elapsed);
            bool previewsIdle = stillPreviews_->Quiesce(remaining);
            elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            remaining = elapsed >= timeoutMilliseconds ? 0u : timeoutMilliseconds - static_cast<uint32_t>(elapsed);
            std::unique_lock lock(mutex_);
            return condition_.wait_for(lock,
                std::chrono::milliseconds(remaining),
                [&] { return !active_; }) && probesIdle && previewsIdle;
        }

        void SetStorageQuotaBytes(uint64_t quotaBytes) noexcept
        {
            storageQuotaBytes_.store(quotaBytes, std::memory_order_relaxed);
        }

    private:
        static constexpr auto variantLeaseGrace = std::chrono::seconds(30);

        struct StablePathAccess
        {
            fs::path path;
            std::shared_ptr<motion::MediaLibraryTrustLease> trust;
        };

        struct PlaybackLeaseAnchor
        {
            explicit PlaybackLeaseAnchor(
                std::shared_ptr<motion::MediaLibraryTrustLease> trust = {}) noexcept
                : libraryTrust(std::move(trust)) {}

            // Keeping these direct handles alive prevents ordinary root,
            // marker, or Groups replacement during delayed Renderer startup
            // and for as long as the Renderer owns this opaque token.
            std::shared_ptr<motion::MediaLibraryTrustLease> libraryTrust;
        };

        [[nodiscard]] std::shared_ptr<motion::MediaLibraryTrustLease> AcquireLibraryTrust() const noexcept
        {
            return libraryTrust_
                ? motion::acquire_media_library_trust(*libraryTrust_)
                : std::shared_ptr<motion::MediaLibraryTrustLease>{};
        }

        [[nodiscard]] bool LibraryTrusted() const noexcept
        {
            return !libraryTrust_ ||
                (motion::revalidate_media_library_stable_root(*libraryTrust_) &&
                    motion::revalidate_media_library_trust(*libraryTrust_));
        }

        [[nodiscard]] std::optional<fs::path> StablePath(
            fs::path const& configuredPath) const noexcept
        {
            if (!libraryTrust_) return configuredPath;
            return motion::media_library_stable_path(*libraryTrust_, configuredPath);
        }

        [[nodiscard]] std::optional<fs::path> ConfiguredPath(
            fs::path const& stablePath) const noexcept
        {
            if (!libraryTrust_) return stablePath;
            return rebase_path_under_root(
                libraryTrust_->stableRoot, libraryTrust_->root, stablePath);
        }

        // Acquiring an access object is the fail-closed boundary for every
        // optimizer-owned mutation. The returned handles remain alive across
        // the write while the path itself cannot follow a reused drive letter.
        [[nodiscard]] std::optional<StablePathAccess> AcquireStableAccess(
            fs::path const& configuredPath) const noexcept
        {
            if (!libraryTrust_) return StablePathAccess{ configuredPath, {} };
            auto trust = AcquireLibraryTrust();
            if (!trust || !motion::revalidate_media_library_stable_root(*libraryTrust_)) {
                return std::nullopt;
            }
            auto path = motion::media_library_stable_path(*libraryTrust_, configuredPath);
            if (!path) return std::nullopt;
            return StablePathAccess{ std::move(*path), std::move(trust) };
        }

        [[nodiscard]] motion::VariantGenerationRequest ReadGenerationRequest(
            fs::path const& configuredMediaDirectory) const noexcept
        {
            auto stable = StablePath(configuredMediaDirectory);
            return stable ? motion::read_variant_generation_request(*stable)
                : motion::VariantGenerationRequest{};
        }

        [[nodiscard]] std::string FailureContext(fs::path const& source, fs::path const& destination,
            VideoGpuInventory const& inventory) const
        {
            return "failure-v1|" + motion::wide_to_utf8(destination.filename().wstring()) + "|" +
                video_file_fingerprint(source) + "|" +
                (inventory.succeeded ? inventory.environment : std::string("gpu-probe-unavailable")) + "|" +
                video_file_fingerprint(ffmpeg_) + "|" + video_file_fingerprint(ffmpeg_.parent_path() / L"ffprobe.exe");
        }

        [[nodiscard]] bool GenerationFailed(
            fs::path const& configuredMediaDirectory,
            std::string const& mode) const noexcept
        {
            auto access = AcquireStableAccess(configuredMediaDirectory);
            if (!access) return true;
            motion::unique_handle failure(CreateFileW(motion::variant_failed_path(access->path).c_str(),
                GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!failure) return false;
            char value[4096]{};
            DWORD read{};
            if (!ReadFile(failure.get(), value, sizeof(value), &read, nullptr)) return true;
            std::string_view record(value, read);
            // Failures are durable user-visible results. Driver/probe changes
            // must not silently erase them and restart expensive work.
            return motion::variant_failure_mode(record) == mode;
        }

        // Playback-created work is a real library task: publish one durable,
        // tokenized request so the UI can observe, pause and cancel it. Unlike
        // an explicit retry, this helper never clears cancellation, pause,
        // failure or suppression markers and never replaces another mode.
        [[nodiscard]] motion::VariantGenerationRequest EnsureAutomaticRequest(
            fs::path const& configuredMediaDirectory, std::string const& mode,
            motion::VariantProgressState initialState) const noexcept
        {
            auto access = AcquireStableAccess(configuredMediaDirectory);
            return access ? motion::ensure_variant_generation_request(access->path, mode, initialState)
                : motion::VariantGenerationRequest{};
        }

        [[nodiscard]] bool GenerationPaused(
            fs::path const& configuredMediaDirectory) const noexcept
        {
            auto stable = StablePath(configuredMediaDirectory);
            return stable && motion::variant_generation_paused(*stable);
        }

        [[nodiscard]] bool GenerationSuppressed(fs::path const& configuredMediaDirectory,
            std::string const& mode) const noexcept
        {
            auto stable = StablePath(configuredMediaDirectory);
            return stable && motion::variant_generation_suppressed(*stable, mode);
        }

        [[nodiscard]] bool GenerationCancelled(
            fs::path const& configuredMediaDirectory) const noexcept
        {
            auto stable = StablePath(configuredMediaDirectory);
            if (!stable) return true;
            std::error_code error;
            return fs::is_regular_file(motion::variant_cancelled_path(*stable), error) && !error;
        }

        bool WriteProgress(fs::path const& configuredMediaDirectory,
            motion::VariantGenerationRequest const& request,
            motion::VariantProgressState state, uint32_t percent = 0,
            bool determinate = false) const noexcept
        {
            auto access = AcquireStableAccess(configuredMediaDirectory);
            return access && motion::write_variant_progress_if_current(
                access->path, request, state, percent, determinate);
        }

        void ClearProgress(fs::path const& configuredMediaDirectory,
            motion::VariantGenerationRequest const& request) const noexcept
        {
            auto access = AcquireStableAccess(configuredMediaDirectory);
            if (access) motion::clear_variant_progress(access->path, request);
        }

        bool CompleteGeneration(fs::path const& configuredMediaDirectory,
            motion::VariantGenerationRequest const& request) const noexcept
        {
            auto access = AcquireStableAccess(configuredMediaDirectory);
            return access && motion::complete_variant_generation(access->path, request);
        }

        bool FailGeneration(fs::path const& configuredMediaDirectory,
            motion::VariantGenerationRequest const& request, std::string const& context = {},
            std::string const& reason = {}) const noexcept
        {
            auto access = AcquireStableAccess(configuredMediaDirectory);
            return access && motion::fail_variant_generation(access->path, request, context, reason);
        }

        [[nodiscard]] bool ReuseEquivalentVariant(fs::path const& configuredSource,
            fs::path const& configuredDestination, std::string const& mode) const noexcept
        {
            auto destinationAccess = AcquireStableAccess(configuredDestination);
            auto stableSource = StablePath(configuredSource);
            return destinationAccess && stableSource && reuse_equivalent_variant(
                *stableSource, destinationAccess->path, mode);
        }

        [[nodiscard]] std::shared_ptr<void> RetainLibraryPlaybackLease(
            std::shared_ptr<motion::MediaLibraryTrustLease> const& trust,
            bool requested) const
        {
            if (!requested || !libraryTrust_ || !trust) return {};
            return std::make_shared<PlaybackLeaseAnchor>(trust);
        }

        void RemoveExpiredRetiredLeasesLocked(std::chrono::steady_clock::time_point now)
        {
            for (auto lease = retiredVariantLeases_.begin(); lease != retiredVariantLeases_.end();) {
                if (lease->second <= now) lease = retiredVariantLeases_.erase(lease);
                else ++lease;
            }
        }

        void RetireLeaseLocked(fs::path const& path, std::chrono::steady_clock::time_point now)
        {
            if (path.empty()) return;
            auto expires = now + variantLeaseGrace;
            auto [entry, inserted] = retiredVariantLeases_.try_emplace(path, expires);
            if (!inserted && entry->second < expires) entry->second = expires;
        }

        void RetireChoicesLocked()
        {
            auto now = std::chrono::steady_clock::now();
            RemoveExpiredRetiredLeasesLocked(now);
        }

        [[nodiscard]] std::shared_ptr<void> RetainPlaybackVariantLocked(fs::path const& path,
            std::shared_ptr<motion::MediaLibraryTrustLease> const& trust)
        {
            RemoveExpiredPlaybackLeasesLocked();
            auto& weakLease = playbackVariantLeases_[path];
            auto lease = weakLease.lock();
            if (!lease) {
                lease = std::make_shared<PlaybackLeaseAnchor>(trust);
                weakLease = lease;
            }
            return lease;
        }

        void RemoveExpiredPlaybackLeasesLocked()
        {
            for (auto lease = playbackVariantLeases_.begin();
                lease != playbackVariantLeases_.end();) {
                if (lease->second.expired()) lease = playbackVariantLeases_.erase(lease);
                else ++lease;
            }
        }

        [[nodiscard]] bool PlaybackVariantIsLeasedLocked(fs::path const& path)
        {
            auto found = playbackVariantLeases_.find(path);
            if (found == playbackVariantLeases_.end()) return false;
            if (!found->second.expired()) return true;
            playbackVariantLeases_.erase(found);
            return false;
        }

        [[nodiscard]] motion::unique_handle PinAlternatePlayableFileForRemoval(
            fs::path const& stableCandidate) const noexcept
        {
            try {
                auto variants = stableCandidate.parent_path();
                auto mediaDirectory = variants.parent_path();
                if (variants.filename() != L"Variants" || mediaDirectory.empty()) return {};
                auto metadata = motion::load_media(mediaDirectory / L"metadata.json");
                if (!metadata || !motion::safe_file_name(metadata->fileName)) return {};

                auto pinRegularNonEmptyFile = [](fs::path const& path) noexcept {
                    // Deliberately omit FILE_SHARE_DELETE. While the candidate
                    // is removed, the source (or another retained variant)
                    // cannot be renamed/deleted by another process after our
                    // final last-copy check.
                    motion::unique_handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr));
                    if (!file) return motion::unique_handle{};
                    BY_HANDLE_FILE_INFORMATION information{};
                    LARGE_INTEGER size{};
                    if (!GetFileInformationByHandle(file.get(), &information) ||
                        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                        !GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0) {
                        return motion::unique_handle{};
                    }
                    return file;
                };

                if (auto source = pinRegularNonEmptyFile(mediaDirectory / metadata->fileName)) {
                    return source;
                }

                std::error_code error;
                for (fs::directory_iterator entries(variants, error), end;
                    !error && entries != end; entries.increment(error)) {
                    auto path = entries->path();
                    auto name = path.filename().wstring();
                    if (path == stableCandidate || path.extension() != L".mp4" ||
                        name.ends_with(L".part.mp4") ||
                        (!name.starts_with(L"balanced-") &&
                            !name.starts_with(L"power-saver-") &&
                            !name.starts_with(L"cpu-smooth-"))) {
                        continue;
                    }
                    if (auto retained = pinRegularNonEmptyFile(path)) return retained;
                }
            } catch (...) {}
            return {};
        }

        bool RemoveVariantIfUnleased(fs::path const& path,
            std::set<fs::path> const* additionallyProtected = nullptr,
            bool activeRequestOwnsRemoval = false)
        {
            auto access = AcquireStableAccess(path);
            if (!access) return false;
            std::lock_guard lock(mutex_);
            auto now = std::chrono::steady_clock::now();
            RemoveExpiredRetiredLeasesLocked(now);
            RemoveExpiredPlaybackLeasesLocked();
            if ((additionallyProtected && additionallyProtected->contains(path)) ||
                (!activeRequestOwnsRemoval && active_ && active_->destination == path) ||
                retiredVariantLeases_.contains(path) ||
                PlaybackVariantIsLeasedLocked(path)) {
                return false;
            }
            // Re-evaluate the playable-file invariant at the final deletion
            // point, not only during the earlier cache traversal. Pinning the
            // alternate prevents a concurrent path-based removal while this
            // final delete runs; App-initiated multi-step deletions additionally
            // hold the global Agent-quiescence request across check + mutation.
            auto alternatePlayable = PinAlternatePlayableFileForRemoval(access->path);
            if (!alternatePlayable) return false;
            std::error_code error;
            if (fs::remove(access->path, error) && !error) {
                retainedProfiles_.erase(path);
                retentionRetryAfter_.erase(path);
                variantUseTouchAfter_.erase(path);
                validatedVariants_.erase(path);
                return true;
            }
            if (error) return false;
            error.clear();
            if (!fs::exists(access->path, error) && !error) {
                retainedProfiles_.erase(path);
                retentionRetryAfter_.erase(path);
                variantUseTouchAfter_.erase(path);
                validatedVariants_.erase(path);
                return true;
            }
            return false;
        }

        bool ValidateVariantForAdoption(fs::path const& stableSource,
            fs::path const& stableDestination, fs::path const& configuredDestination,
            VariantValidationSpec const& expected, motion::unique_handle& pinnedVariant)
        {
            // Pin this exact file against writes and replacement while the
            // external probes run. The caller keeps this handle until a
            // Renderer lease has been published under the removal mutex.
            pinnedVariant.reset(CreateFileW(stableDestination.c_str(), GENERIC_READ,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!pinnedVariant) return false;
            if (!current_variant(stableSource, stableDestination)) return false;
            auto variantFingerprint = variant_file_fingerprint(pinnedVariant.get());
            motion::unique_handle sourceHandle(CreateFileW(stableSource.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            auto sourceFingerprint = sourceHandle ? variant_file_fingerprint(sourceHandle.get()) : std::nullopt;
            if (!variantFingerprint || !sourceFingerprint) return false;
            {
                std::lock_guard lock(mutex_);
                auto known = validatedVariants_.find(configuredDestination);
                if (known != validatedVariants_.end() &&
                    known->second.source == *sourceFingerprint &&
                    known->second.variant == *variantFingerprint &&
                    known->second.specification == expected) return known->second.valid;
            }
            auto epoch = generation_.load(std::memory_order_relaxed);
            auto cancelled = [&] {
                return !LibraryTrusted() || generation_.load(std::memory_order_relaxed) != epoch;
            };
            // Never hold mutex_ across an external decoder or a bounded
            // process wait. Invalid fingerprints are remembered too so a
            // damaged cache is not probed again on every policy iteration.
            auto actual = source_rate(ffmpeg_, stableDestination, cancelled);
            bool valid = actual.matchesSdrOutput &&
                video_variant_dimensions_match(actual.width, actual.height, expected.width, expected.height) &&
                video_variant_rate_matches(actual.numerator, actual.denominator, expected.targetFps) &&
                video_variant_duration_matches(actual.duration100ns, expected.duration100ns, expected.targetFps) &&
                video_candidate_decodes_first_frame(stableDestination, cancelled);
            if (cancelled()) return false;
            {
                std::lock_guard lock(mutex_);
                if (validatedVariants_.size() >= 1024) validatedVariants_.clear();
                validatedVariants_[configuredDestination] = {
                    *sourceFingerprint, *variantFingerprint, expected, valid };
            }
            if (!valid) append_log(logRoot_, L"优化缓存已损坏或不符合 SDR 规格，将重新生成: " +
                configuredDestination.filename().wstring());
            return valid;
        }

        bool TryAdoptVariant(fs::path const& source, std::string const& mode,
            fs::path const& destination, VariantValidationSpec const& expected,
            std::shared_ptr<void>* playbackLease = nullptr,
            std::shared_ptr<motion::MediaLibraryTrustLease> const& libraryTrust = {})
        {
            auto destinationAccess = AcquireStableAccess(destination);
            auto stableSource = StablePath(source);
            auto stableMediaDirectory = StablePath(source.parent_path());
            if (!destinationAccess || !stableSource || !stableMediaDirectory) return false;
            motion::unique_handle pinnedVariant;
            if (!ValidateVariantForAdoption(*stableSource, destinationAccess->path,
                    destination, expected, pinnedVariant)) return false;
            bool shouldRetain{};
            bool shouldTouchUseTime{};
            auto now = std::chrono::steady_clock::now();
            {
                std::lock_guard lock(mutex_);
                RemoveExpiredRetiredLeasesLocked(now);
                // Validation and lease publication share the same lock as every
                // optimizer-owned final-variant deletion. A cache-prune worker
                // therefore cannot delete the file between this check and the
                // lease becoming visible.
                if (!current_variant(*stableSource, destinationAccess->path)) return false;
                retiredVariantLeases_.erase(destination);
                if (playbackLease) {
                    // Publish the lease under the same lock used by every
                    // optimizer-owned final-file removal. There is therefore no
                    // gap between validating this exact path and pinning it for
                    // the Renderer that will consume the returned result.
                    *playbackLease = RetainPlaybackVariantLocked(destination, libraryTrust);
                } else {
                    // Callers that cannot carry an exact lifetime token get a
                    // short hand-off grace. Do not pin every variant visited by
                    // random rotation for the entire Agent session.
                    RetireLeaseLocked(destination, now);
                }
                auto nextTouch = variantUseTouchAfter_.find(destination);
                if (nextTouch == variantUseTouchAfter_.end() ||
                    now >= nextTouch->second) {
                    // Cache quota eviction orders by last_write_time. Touch at
                    // most once per half hour so it approximates last use
                    // without turning the playback policy loop into disk I/O.
                    variantUseTouchAfter_[destination] = now + std::chrono::minutes(30);
                    shouldTouchUseTime = true;
                }
                if (!retainedProfiles_.contains(destination)) {
                    auto retry = retentionRetryAfter_.find(destination);
                    if (retry == retentionRetryAfter_.end() || now >= retry->second) {
                        retentionRetryAfter_[destination] = now + std::chrono::seconds(30);
                        shouldRetain = true;
                    }
                }
            }
            // Internal removal is now blocked by the Renderer lease/grace.
            // Releasing this OS pin also permits the LRU timestamp update.
            // That timestamp changes the fingerprint, causing at most one
            // fresh validation after a half-hour touch, never per frame.
            pinnedVariant.reset();
            if (shouldTouchUseTime) {
                std::error_code ignored;
                fs::last_write_time(destinationAccess->path,
                    fs::file_time_type::clock::now(), ignored);
            }
            if (shouldRetain && motion::retain_variant_profile(
                *stableMediaDirectory, mode, destination.filename().wstring(),
                [this](fs::path const& candidate) {
                    auto configured = ConfiguredPath(candidate);
                    return configured && RemoveVariantIfUnleased(*configured);
                })) {
                std::lock_guard lock(mutex_);
                retainedProfiles_.insert(destination);
                retentionRetryAfter_.erase(destination);
            }
            return true;
        }

        void Run(std::stop_token stop)
        {
            init_apartment(apartment_type::multi_threaded);
            while (!stop.stop_requested()) {
                Request request;
                {
                    std::unique_lock lock(mutex_);
                    condition_.wait(lock, stop, [&] { return generationAllowed_ && !pending_.empty(); });
                    if (stop.stop_requested()) return;
                    request = std::move(pending_.front());
                    pending_.pop_front();
                    active_ = request;
                }
                auto operationTrust = AcquireLibraryTrust();
                if (libraryTrust_ && !operationTrust) {
                    {
                        std::lock_guard lock(mutex_);
                        active_.reset();
                    }
                    condition_.notify_all();
                    continue;
                }
                std::wstring failureReason = L"生成失败，请检查磁盘空间、源文件或显卡驱动后重试。";
                auto transcodeResult = Transcode(request, stop, failureReason);
                // No status, cleanup, or cache write is safe after a removable
                // drive was replaced at the configured path. The Agent notices
                // the same loss on its next policy loop and joins this worker.
                if (!LibraryTrusted()) {
                    {
                        std::lock_guard lock(mutex_);
                        active_.reset();
                    }
                    condition_.notify_all();
                    continue;
                }
                bool succeeded = transcodeResult == VideoTranscodeResult::succeeded;
                bool paused = transcodeResult == VideoTranscodeResult::paused ||
                    GenerationPaused(request.source.parent_path());
                bool obsolete = request.generation != generation_.load(std::memory_order_relaxed);
                bool cancelled = GenerationCancelled(request.source.parent_path());
                bool suppressed = GenerationSuppressed(
                    request.source.parent_path(), request.mode);
                auto durableRequestBeforeCompletion = ReadGenerationRequest(
                    request.source.parent_path());
                bool superseded = request.explicitRequest
                    ? durableRequestBeforeCompletion != request.durableRequest
                    : static_cast<bool>(durableRequestBeforeCompletion);
                bool accepted = succeeded && !paused && !obsolete && !cancelled && !suppressed && !superseded;
                if (accepted) {
                    accepted = TryAdoptVariant(request.source, request.mode, request.destination,
                        { request.width, request.height, request.targetFps, request.duration100ns });
                    if (accepted) {
                        if (request.explicitRequest) {
                            WriteProgress(request.source.parent_path(),
                                request.durableRequest,
                                motion::VariantProgressState::generating, 100, true);
                            CompleteGeneration(
                                request.source.parent_path(), request.durableRequest);
                        } else {
                            // Implicit tasks have no durable request to complete.
                            // Remove only their legacy/tokenless progress record.
                            ClearProgress(request.source.parent_path(),
                                motion::VariantGenerationRequest{ request.mode, {} });
                        }
                    }
                }
                if (succeeded && !accepted) {
                    RemoveVariantIfUnleased(request.destination, nullptr, true);
                    failureReason = L"生成副本未通过最终播放校验，原视频已保留，请重试。";
                }
                if (!accepted && !paused && request.explicitRequest && !stop.stop_requested() && !obsolete &&
                    !cancelled && !suppressed && !superseded) {
                    auto reasonLength = (std::min)(failureReason.size(), size_t{ 512 });
                    if (reasonLength && failureReason[reasonLength - 1] >= 0xd800 &&
                        failureReason[reasonLength - 1] <= 0xdbff) --reasonLength;
                    FailGeneration(
                        request.source.parent_path(), request.durableRequest, request.failureContext,
                        motion::wide_to_utf8(failureReason.substr(0, reasonLength)));
                }
                {
                    std::lock_guard lock(mutex_);
                    active_.reset();
                    if (!accepted && !paused && !stop.stop_requested() && !obsolete && !cancelled &&
                        !suppressed && !superseded) failed_.insert(request.Key());
                }
                condition_.notify_all();

                auto durableRequest = ReadGenerationRequest(request.source.parent_path());
                bool requestStillCurrent = request.explicitRequest &&
                    durableRequest == request.durableRequest;
                if (paused && requestStillCurrent) {
                    // Pausing terminates the current FFmpeg process; the next
                    // attempt restarts at zero, so do not preserve a misleading
                    // processed-time percentage from the discarded partial.
                    WriteProgress(request.source.parent_path(),
                        request.durableRequest,
                        motion::VariantProgressState::paused);
                } else if ((obsolete || stop.stop_requested()) && requestStillCurrent &&
                    !cancelled && !suppressed) {
                    WriteProgress(request.source.parent_path(),
                        request.durableRequest,
                        on_battery() ? motion::VariantProgressState::waitingForPower
                            : motion::VariantProgressState::queued);
                } else if (!accepted) {
                    ClearProgress(request.source.parent_path(),
                        request.explicitRequest ? request.durableRequest :
                            motion::VariantGenerationRequest{ request.mode, {} });
                }
                if (!paused && !obsolete && !cancelled && !suppressed && !superseded) {
                    append_log(logRoot_, accepted
                        ? L"已生成壁纸优化副本: " + request.destination.filename().wstring()
                        : L"所选壁纸副本规格不可用，保留原文件并显示静态预览: " + request.source.filename().wstring());
                }
            }
        }

        VideoTranscodeResult Transcode(Request const& request, std::stop_token stop, std::wstring& failureReason)
        {
            auto operationTrust = AcquireLibraryTrust();
            if (libraryTrust_ && !operationTrust) return VideoTranscodeResult::cancelled;
            auto destinationAccess = AcquireStableAccess(request.destination);
            auto stableSource = StablePath(request.source);
            auto stableWallpapers = StablePath(wallpapersPath_);
            auto stableMediaDirectory = StablePath(request.source.parent_path());
            if (!destinationAccess || !stableSource || !stableWallpapers || !stableMediaDirectory) {
                return VideoTranscodeResult::cancelled;
            }
            bool trustLost{};
            auto directory = destinationAccess->path.parent_path();
            auto partialToken = motion::new_variant_request_id();
            if (partialToken.empty()) return VideoTranscodeResult::failed;
            auto temporary = directory / (request.destination.stem().wstring() + L".part-" +
                motion::utf8_to_wide(partialToken) + L".part.mp4");
            auto control = [&] {
                if (!LibraryTrusted()) {
                    trustLost = true;
                    return VideoTranscodeControl::cancelled;
                }
                if (motion::variant_generation_paused(*stableMediaDirectory)) {
                    return VideoTranscodeControl::paused;
                }
                auto durableRequest = motion::read_variant_generation_request(
                    *stableMediaDirectory);
                bool cancelled = stop.stop_requested() ||
                    request.generation != generation_.load(std::memory_order_relaxed) ||
                    fs::is_regular_file(motion::variant_cancelled_path(*stableMediaDirectory)) ||
                    motion::variant_generation_suppressed(*stableMediaDirectory, request.mode) ||
                    (request.explicitRequest
                        ? durableRequest != request.durableRequest
                        : static_cast<bool>(durableRequest));
                return cancelled ? VideoTranscodeControl::cancelled : VideoTranscodeControl::running;
            };
            auto removeTemporaryIfTrusted = [&] {
                if (!LibraryTrusted()) {
                    trustLost = true;
                    return;
                }
                std::error_code ignored;
                fs::remove(temporary, ignored);
            };
            auto initialControl = control();
            if (initialControl != VideoTranscodeControl::running) {
                return initialControl == VideoTranscodeControl::paused
                    ? VideoTranscodeResult::paused : VideoTranscodeResult::cancelled;
            }
            std::error_code ignored;
            try {
                fs::create_directories(directory);
                fs::remove(temporary, ignored);
                std::set<fs::path> protectedFiles;
                protectedFiles.insert(request.destination);
                auto pruneToStorageQuota = [&] {
                    prune_variant_cache(*stableWallpapers,
                        storageQuotaBytes_.load(std::memory_order_relaxed),
                        [this, &protectedFiles](fs::path const& candidate) {
                        // The expensive traversal remains outside the mutex;
                        // only the final protection check and deletion are
                        // serialized with Resolve()/TryAdoptVariant().
                        auto configured = ConfiguredPath(candidate);
                        return configured && RemoveVariantIfUnleased(*configured, &protectedFiles);
                        });
                };
                pruneToStorageQuota();
                if (!has_transcode_space(directory, *stableSource,
                        { request.width, request.height, request.targetFps, request.duration100ns })) {
                    append_log(logRoot_, L"磁盘空间不足，跳过壁纸优化副本生成。");
                    failureReason = L"磁盘空间不足，释放空间后可重试。";
                    return VideoTranscodeResult::failed;
                }
                auto targetFps = request.targetFps;
                std::wstring error;
                std::wstring selectedBackend;
                auto lastProgressWrite = std::chrono::steady_clock::time_point::min();
                auto attemptStartedAt = std::chrono::steady_clock::now();
                uint32_t lastProgressPercent{};
                uint32_t lastProgressAttempt{};
                bool lastProgressKnown{};
                auto progressIsAuthoritative = [&] {
                    if (!LibraryTrusted()) {
                        trustLost = true;
                        return false;
                    }
                    if (fs::is_regular_file(motion::variant_cancelled_path(*stableMediaDirectory)) ||
                        motion::variant_generation_paused(*stableMediaDirectory) ||
                        motion::variant_generation_suppressed(*stableMediaDirectory, request.mode)) {
                        return false;
                    }
                    auto durableRequest = motion::read_variant_generation_request(*stableMediaDirectory);
                    return request.explicitRequest
                        ? durableRequest == request.durableRequest
                        : !durableRequest;
                };
                auto publishProgress = [&](VideoTranscodeProgress const& progress) {
                    if (!progressIsAuthoritative()) return;

                    auto now = std::chrono::steady_clock::now();
                    bool newAttempt = progress.attemptStarted ||
                        progress.attempt != lastProgressAttempt;
                    if (newAttempt) attemptStartedAt = now;
                    bool completed = progress.percent == 100;
                    if (!newAttempt && !completed) {
                        if (!progress.determinate ||
                            (lastProgressKnown && progress.percent <= lastProgressPercent) ||
                            now - lastProgressWrite < std::chrono::seconds(1)) return;
                    }
                    lastProgressAttempt = progress.attempt;
                    uint64_t estimatedRemainingSeconds{};
                    bool estimatedRemainingKnown{};
                    auto elapsed = now - attemptStartedAt;
                    if (progress.determinate && progress.processedMicroseconds &&
                        progress.durationMicroseconds > progress.processedMicroseconds &&
                        elapsed >= std::chrono::seconds(2)) {
                        auto elapsedSeconds =
                            std::chrono::duration<long double>(elapsed).count();
                        auto remainingMedia = static_cast<long double>(
                            progress.durationMicroseconds - progress.processedMicroseconds);
                        auto encodedMedia = static_cast<long double>(
                            progress.processedMicroseconds);
                        auto estimate = elapsedSeconds * remainingMedia / encodedMedia;
                        constexpr long double maximumEtaSeconds =
                            365.0L * 24.0L * 60.0L * 60.0L;
                        if (estimate >= 0.0L && estimate <= maximumEtaSeconds) {
                            estimatedRemainingSeconds = static_cast<uint64_t>(
                                std::ceil(estimate));
                            estimatedRemainingKnown = true;
                        }
                    }
                    bool persisted = request.explicitRequest
                        ? motion::write_variant_progress_if_current(*stableMediaDirectory,
                            request.durableRequest, motion::VariantProgressState::generating,
                            progress.percent, progress.determinate,
                            estimatedRemainingSeconds, estimatedRemainingKnown)
                        : motion::write_variant_progress(*stableMediaDirectory, request.mode,
                            motion::VariantProgressState::generating, progress.percent,
                            progress.determinate, {}, estimatedRemainingSeconds, estimatedRemainingKnown);
                    // Cancel/pause/suppress and a replacement request are
                    // cross-process file operations. They may land between
                    // the optimistic check above and this write; re-check the
                    // durable authority after publication so an obsolete
                    // worker cannot resurrect a stale generating marker.
                    if (!progressIsAuthoritative()) {
                        motion::clear_variant_progress(*stableMediaDirectory,
                            request.explicitRequest ? request.durableRequest :
                                motion::VariantGenerationRequest{ request.mode, {} });
                        return;
                    }
                    if (persisted) {
                        lastProgressWrite = now;
                        lastProgressPercent = progress.percent;
                        lastProgressKnown = progress.determinate;
                    } else {
                        // Retry a transient persistence failure later, without
                        // hammering the disk twice per second for this attempt.
                        lastProgressWrite = now;
                    }
                };
                auto validationCancelled = [&] { return control() != VideoTranscodeControl::running; };
                auto validatePreview = [&](fs::path const& candidate,
                    VideoTranscodeBackend, VideoTranscodeCodec codec) {
                    if (!LibraryTrusted()) {
                        trustLost = true;
                        return false;
                    }
                    auto actual = source_rate(ffmpeg_, candidate, validationCancelled);
                    return codec == VideoTranscodeCodec::H264 && video_variant_dimensions_match(
                            actual.width, actual.height, request.width, request.height) &&
                        video_variant_rate_matches(
                            actual.numerator, actual.denominator, targetFps) &&
                        actual.matchesSdrOutput && actual.duration100ns > 0 &&
                        video_candidate_decodes_first_frame(candidate, validationCancelled);
                };
                auto validateCandidate = [&](fs::path const& candidate,
                    VideoTranscodeBackend backend, VideoTranscodeCodec codec) {
                    if (!validatePreview(candidate, backend, codec)) return false;
                    auto actual = source_rate(ffmpeg_, candidate, validationCancelled);
                    return video_variant_duration_matches(
                        actual.duration100ns, request.duration100ns, targetFps);
                };
                VideoTranscodePathAccess pathAccess;
                if (libraryTrust_) {
                    pathAccess = [this]() -> std::shared_ptr<void> {
                        return std::static_pointer_cast<void>(AcquireLibraryTrust());
                    };
                }
                VideoTranscodeCodec selectedCodec{};
                auto copyLabel = request.softwarePlaybackTarget
                    ? std::wstring(L"H.264 兼容副本")
                    : std::wstring(L"优化副本");
                auto result = transcode_video(
                    ffmpeg_,
                    *stableSource, temporary, request.width, request.height, targetFps,
                    control, error, &selectedBackend, request.softwareFallbackAllowed,
                    request.softwarePlaybackTarget, request.duration100ns, publishProgress,
                    validateCandidate, &selectedCodec, pathAccess,
                    [&](std::wstring const& message) { append_log(logRoot_, message); }, validatePreview);
                if (trustLost || !LibraryTrusted()) return VideoTranscodeResult::cancelled;
                if (result == VideoTranscodeResult::cancelled || result == VideoTranscodeResult::paused) {
                    removeTemporaryIfTrusted();
                    return result;
                }
                if (result != VideoTranscodeResult::succeeded) {
                    if (!error.empty()) failureReason = error;
                    append_log(logRoot_, copyLabel + L" " +
                        std::to_wstring(request.width) + L"x" + std::to_wstring(request.height) + L" / " +
                        std::to_wstring(targetFps) + L" FPS 不可用: " + error);
                    removeTemporaryIfTrusted();
                    return result;
                }
                auto finalControl = control();
                if (finalControl != VideoTranscodeControl::running) {
                    removeTemporaryIfTrusted();
                    return finalControl == VideoTranscodeControl::paused
                        ? VideoTranscodeResult::paused : VideoTranscodeResult::cancelled;
                }
                if (!fs::is_regular_file(temporary) || !fs::file_size(temporary)) {
                    failureReason = L"转码没有生成有效文件，请查看日志后重试。";
                    removeTemporaryIfTrusted();
                    return VideoTranscodeResult::failed;
                }
                auto actual = source_rate(ffmpeg_, temporary, validationCancelled);
                // Container duration seldom ends exactly on a frame boundary,
                // so Media Foundation may report 59.94 for a CFR 60 stream.
                // One-frame-per-second tolerance accepts that representation
                // without accepting a different performance tier.
                bool matchingRate = video_variant_rate_matches(
                    actual.numerator, actual.denominator, targetFps);
                bool matchingDimensions = video_variant_dimensions_match(
                    actual.width, actual.height, request.width, request.height);
                bool matchingCodec = selectedCodec == VideoTranscodeCodec::H264 &&
                    actual.codec == VideoSourceCodec::H264;
                bool matchingVisualMetadata = actual.matchesSdrOutput;
                bool matchingDuration = video_variant_duration_matches(
                    actual.duration100ns, request.duration100ns, targetFps);
                bool decodesFirstFrame = video_candidate_decodes_first_frame(temporary, validationCancelled);
                finalControl = control();
                if (finalControl != VideoTranscodeControl::running) {
                    removeTemporaryIfTrusted();
                    return finalControl == VideoTranscodeControl::paused
                        ? VideoTranscodeResult::paused : VideoTranscodeResult::cancelled;
                }
                if (!matchingDimensions || !matchingRate || !matchingCodec ||
                    !matchingVisualMetadata || !matchingDuration || !decodesFirstFrame) {
                    failureReason = L"副本的分辨率、帧率、色彩或播放校验失败，请重试。";
                    append_log(logRoot_, copyLabel + L"校验失败（实际 " +
                        std::to_wstring(actual.width) + L"x" + std::to_wstring(actual.height) + L", " +
                        std::to_wstring(actual.numerator) + L"/" + std::to_wstring(actual.denominator) +
                        L" FPS，时长 " + std::to_wstring(actual.duration100ns / 10'000) +
                        L" ms），已自动删除。");
                    removeTemporaryIfTrusted();
                    return VideoTranscodeResult::failed;
                }
                finalControl = control();
                if (finalControl != VideoTranscodeControl::running) {
                    removeTemporaryIfTrusted();
                    return finalControl == VideoTranscodeControl::paused
                        ? VideoTranscodeResult::paused : VideoTranscodeResult::cancelled;
                }
                if (!MoveFileExW(temporary.c_str(), destinationAccess->path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    failureReason = L"无法保存完成的副本，请检查目录权限和文件占用后重试。";
                    append_log(logRoot_, L"无法原子发布壁纸优化副本，继续保留上一份可用副本。");
                    removeTemporaryIfTrusted();
                    return VideoTranscodeResult::failed;
                }
                finalControl = control();
                if (finalControl != VideoTranscodeControl::running) {
                    if (!trustLost) RemoveVariantIfUnleased(request.destination, nullptr, true);
                    return finalControl == VideoTranscodeControl::paused
                        ? VideoTranscodeResult::paused : VideoTranscodeResult::cancelled;
                }
                append_log(logRoot_, copyLabel + L"实际帧率: " +
                    std::to_wstring(targetFps) + L" FPS；编码后端: " +
                    (selectedBackend.empty() ? std::wstring(L"未知") : selectedBackend) + L"。");
                if (!current_variant(*stableSource, destinationAccess->path)) {
                    RemoveVariantIfUnleased(request.destination, nullptr, true);
                    return VideoTranscodeResult::failed;
                }
                // A completed encode can itself cross the quota. Re-run exact
                // physical-allocation accounting while protecting the new file
                // and all Renderer leases, instead of waiting for another task.
                pruneToStorageQuota();
                return VideoTranscodeResult::succeeded;
            } catch (...) {
                removeTemporaryIfTrusted();
                return VideoTranscodeResult::failed;
            }
        }

        fs::path wallpapersPath_;
        fs::path logRoot_;
        fs::path ffmpeg_;
        std::optional<motion::MediaLibraryTrustIdentity> libraryTrust_;
        std::unique_ptr<VideoStillPreview> stillPreviews_;
        std::mutex mutex_;
        std::condition_variable_any condition_;
        std::deque<Request> pending_;
        std::optional<Request> active_;
        std::map<std::wstring, SourceRate> rates_;
        std::map<fs::path, VariantValidationEntry> validatedVariants_;
        std::map<std::wstring, std::wstring> hardwareDecodeAdapters_;
        std::set<std::wstring> failed_;
        std::map<fs::path, std::chrono::steady_clock::time_point> retiredVariantLeases_;
        // Weak registration lets pruning observe every live Renderer copy.
        // Resolve and deletion passes sweep all expired entries, so externally
        // removed media cannot leave one control block per historical path.
        std::map<fs::path, std::weak_ptr<PlaybackLeaseAnchor>> playbackVariantLeases_;
        std::set<fs::path> retainedProfiles_;
        std::map<fs::path, std::chrono::steady_clock::time_point> retentionRetryAfter_;
        std::map<fs::path, std::chrono::steady_clock::time_point> variantUseTouchAfter_;
        std::jthread worker_;
        std::atomic_uint64_t generation_{};
        std::atomic_uint64_t storageQuotaBytes_{ defaultVariantCacheLimit };
        bool generationAllowed_{};
        bool mediaFoundationStarted_{};
    };

    VideoOptimizer::VideoOptimizer(fs::path wallpapersPath, fs::path logRoot, fs::path applicationRoot,
        std::optional<motion::MediaLibraryTrustIdentity> libraryTrust)
        : impl_(std::make_unique<Impl>(std::move(wallpapersPath), std::move(logRoot),
            std::move(applicationRoot), std::move(libraryTrust))) {}
    VideoOptimizer::~VideoOptimizer() = default;
    fs::path VideoOptimizer::Resolve(fs::path const& source, std::string const& performanceMode,
        uint32_t targetWidth, uint32_t targetHeight, uint32_t targetRefreshRate,
        bool softwarePlaybackTarget)
    {
        return impl_->ResolveWithLease(source, performanceMode, targetWidth, targetHeight,
            targetRefreshRate, softwarePlaybackTarget, false, true).path;
    }
    ResolvedVideoPath VideoOptimizer::ResolveWithLease(fs::path const& source,
        std::string const& performanceMode, uint32_t targetWidth, uint32_t targetHeight,
        uint32_t targetRefreshRate, bool softwarePlaybackTarget,
        bool allowGenerationRequest)
    {
        return impl_->ResolveWithLease(source, performanceMode, targetWidth, targetHeight,
            targetRefreshRate, softwarePlaybackTarget, true, allowGenerationRequest);
    }
    VideoPlaybackLease VideoOptimizer::AcquirePlaybackLease(fs::path const& path)
    {
        return impl_->AcquirePlaybackLease(path);
    }
    void VideoOptimizer::Prepare(fs::path const& source, std::string const& performanceMode,
        uint32_t targetWidth, uint32_t targetHeight, uint32_t targetRefreshRate)
    {
        impl_->Prepare(source, performanceMode, targetWidth, targetHeight, targetRefreshRate);
    }
    std::wstring VideoOptimizer::SourceHardwareDecodeAdapter(fs::path const& source,
        std::wstring const& preferredAdapter, uint64_t aggregateOutputPixels)
    {
        auto result = impl_->SourceHardwareDecodeCandidates(source, preferredAdapter, aggregateOutputPixels);
        return result.adapters.empty() ? std::wstring{} : result.adapters.front();
    }
    ResolvedVideoPath VideoOptimizer::ResolveStillPreview(fs::path const& source)
    {
        return impl_->ResolveStillPreview(source);
    }

    VideoGpuDecodeProbe VideoOptimizer::SourceHardwareDecodeCandidates(fs::path const& source,
        std::wstring const& preferredAdapter, uint64_t aggregateOutputPixels)
    {
        return impl_->SourceHardwareDecodeCandidates(source, preferredAdapter, aggregateOutputPixels);
    }
    void VideoOptimizer::SetStorageQuotaBytes(uint64_t quotaBytes) noexcept
    {
        impl_->SetStorageQuotaBytes(quotaBytes);
    }
    void VideoOptimizer::SetGenerationAllowed(bool allowed) { impl_->SetGenerationAllowed(allowed); }
    bool VideoOptimizer::Quiesce(uint32_t timeoutMilliseconds)
    {
        return impl_->Quiesce(timeoutMilliseconds);
    }
    void VideoOptimizer::InvalidateChoices() { impl_->InvalidateChoices(); }
}
