#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <powrprof.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include <wrl.h>

#include "../MotionWallpaper.Common/Common.h"
#include "../MotionWallpaper.Common/DisplayAwareness.h"
#include "../MotionWallpaper.Common/DisplayTopology.h"
#include "../MotionWallpaper.Common/VariantCache.h"
#include "CoveragePolicy.h"
#include "IdlePolicy.h"
#include "LibraryMigrationProtocol.h"
#include "PlaybackCapabilityPolicy.h"
#include "RandomSelectionPolicy.h"
#include "resource.h"
#include "RuntimePolicy.h"
#include "SharedRendererPolicy.h"
#include "VideoOptimizer.h"
#include "../MotionWallpaper.Protocol/RendererProtocol.h"

#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace
{
    std::wstring display_adapter_key(std::wstring const& displayDevice)
    {
        if (displayDevice.empty()) return {};
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) return {};
        for (UINT adapterIndex = 0;; ++adapterIndex) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            auto adapterResult = factory->EnumAdapters1(adapterIndex, &adapter);
            if (adapterResult == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(adapterResult) || !adapter) break;
            DXGI_ADAPTER_DESC1 adapterDescription{};
            if (FAILED(adapter->GetDesc1(&adapterDescription))) continue;
            for (UINT outputIndex = 0;; ++outputIndex) {
                Microsoft::WRL::ComPtr<IDXGIOutput> output;
                auto outputResult = adapter->EnumOutputs(outputIndex, &output);
                if (outputResult == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(outputResult) || !output) break;
                DXGI_OUTPUT_DESC outputDescription{};
                if (FAILED(output->GetDesc(&outputDescription)) ||
                    _wcsicmp(outputDescription.DeviceName, displayDevice.c_str()) != 0) continue;
                return std::to_wstring(adapterDescription.AdapterLuid.HighPart) + L":" +
                    std::to_wstring(adapterDescription.AdapterLuid.LowPart);
            }
        }
        return {};
    }

    bool physical_video_device_available()
    {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) return false;
        D3D_FEATURE_LEVEL levels[]{
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        for (UINT index = 0;; ++index) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            auto adapterResult = factory->EnumAdapters1(index, &adapter);
            if (adapterResult == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(adapterResult) || !adapter) break;
            DXGI_ADAPTER_DESC1 description{};
            if (FAILED(adapter->GetDesc1(&description)) ||
                (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            if (SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr, nullptr))) return true;
        }
        return false;
    }

    struct MediaSelection
    {
        fs::path path;
        std::string kind{ "video" };
        std::string id;
        bool sourceBacked{};
        motion::agent::VideoPlaybackLease playbackLease;
    };

    MediaSelection media_by_id(fs::path const& wallpapers, std::string const& groupId,
        std::string const& mediaId, std::string const& performanceMode)
    {
        if (!motion::valid_id(groupId) || !motion::valid_id(mediaId)) return {};
        auto directory = wallpapers / L"Groups" / motion::utf8_to_wide(groupId) / L"Videos" / motion::utf8_to_wide(mediaId);
        motion::MediaMetadata metadata;
        if (!motion::try_load_media(directory / L"metadata.json", metadata) ||
            (metadata.kind != "video" && metadata.kind != "image")) return {};
        if (metadata.id != mediaId || metadata.groupId != groupId || !motion::safe_file_name(metadata.fileName)) return {};
        auto fileName = fs::path(metadata.fileName);
        auto path = directory / fileName;
        std::error_code error;
        if (fs::is_regular_file(path, error) && !error) {
            return MediaSelection{ path, metadata.kind, mediaId, true };
        }
        if (metadata.kind != "video") return {};
        auto retained = performanceMode == motion::agent::cpu_smooth_mode
            ? motion::select_cpu_smooth_variant_file(directory)
            : motion::select_variant_file(motion::inspect_variant_cache(directory), performanceMode);
        if (retained.empty()) return {};
        auto retainedPath = directory / L"Variants" / retained;
        error.clear();
        return fs::is_regular_file(retainedPath, error) && !error
            ? MediaSelection{ std::move(retainedPath), "video", mediaId, false }
            : MediaSelection{};
    }

    struct ImportedOptimizationRequest
    {
        MediaSelection media;
        std::string mode;
    };

    std::vector<ImportedOptimizationRequest> imported_optimization_requests(fs::path const& wallpapers)
    {
        std::vector<ImportedOptimizationRequest> result;
        std::error_code error;
        auto groups = wallpapers / L"Groups";
        for (fs::directory_iterator groupEntries(groups, fs::directory_options::skip_permission_denied, error), groupEnd;
            !error && groupEntries != groupEnd; groupEntries.increment(error)) {
            std::error_code groupError;
            if (!groupEntries->is_directory(groupError) || groupError) continue;
            auto videos = groupEntries->path() / L"Videos";
            for (fs::directory_iterator mediaEntries(videos, fs::directory_options::skip_permission_denied, groupError), mediaEnd;
                !groupError && mediaEntries != mediaEnd; mediaEntries.increment(groupError)) {
                std::error_code mediaError;
                if (!mediaEntries->is_directory(mediaError) || mediaError) continue;
                auto mode = motion::read_variant_request(mediaEntries->path());
                if (mode.empty()) continue;
                motion::MediaMetadata metadata;
                if (!motion::try_load_media(mediaEntries->path() / L"metadata.json", metadata) ||
                    metadata.kind != "video" || !motion::safe_file_name(metadata.fileName)) continue;
                auto source = mediaEntries->path() / metadata.fileName;
                if (!fs::is_regular_file(source, mediaError) || mediaError) continue;
                result.push_back({ { std::move(source), "video", metadata.id, true }, std::move(mode) });
            }
        }
        return result;
    }

    struct OptimizationTarget
    {
        uint32_t width{};
        uint32_t height{};
        uint32_t refreshRateHz{};
    };

    OptimizationTarget optimization_target_size(motion::Settings const& settings)
    {
        auto displays = motion::enumerate_displays();
        if (settings.displayMode == "primary") {
            auto primary = std::find_if(displays.begin(), displays.end(), [](auto const& value) { return value.primary; });
            if (primary == displays.end()) return {};
            return { static_cast<uint32_t>(primary->bounds.right - primary->bounds.left),
                static_cast<uint32_t>(primary->bounds.bottom - primary->bounds.top), primary->refreshRateHz };
        }
        OptimizationTarget result;
        for (auto const& display : displays) {
            result.width = (std::max)(result.width,
                static_cast<uint32_t>(display.bounds.right - display.bounds.left));
            result.height = (std::max)(result.height,
                static_cast<uint32_t>(display.bounds.bottom - display.bounds.top));
            result.refreshRateHz = (std::max)(result.refreshRateHz, display.refreshRateHz);
        }
        return result;
    }

    std::vector<std::string> group_media_ids(fs::path const& wallpapers, std::string const& groupId,
        std::string const& performanceMode)
    {
        std::vector<std::string> result;
        if (!motion::valid_id(groupId)) return result;
        std::error_code error;
        auto directory = wallpapers / L"Groups" / motion::utf8_to_wide(groupId) / L"Videos";
        for (fs::directory_iterator entries(directory, error), end; !error && entries != end; entries.increment(error)) {
            std::error_code typeError;
            if (!entries->is_directory(typeError) || typeError) continue;
            auto id = entries->path().filename().string();
            if (motion::valid_id(id) && !media_by_id(wallpapers, groupId, id, performanceMode).path.empty()) {
                result.push_back(std::move(id));
            }
        }
        return result;
    }

    std::string random_media_id(fs::path const& wallpapers, std::string const& groupId,
        std::string const& previous, std::string const& performanceMode)
    {
        auto ids = group_media_ids(wallpapers, groupId, performanceMode);
        if (ids.empty()) return {};
        if (ids.size() > 1) ids.erase(std::remove(ids.begin(), ids.end(), previous), ids.end());
        static std::mt19937_64 generator{ std::random_device{}() };
        return ids[std::uniform_int_distribution<size_t>(0, ids.size() - 1)(generator)];
    }

    bool request_display_off() noexcept
    {
        DWORD_PTR result{};
        SetLastError(ERROR_SUCCESS);
        return SendMessageTimeoutW(
            HWND_BROADCAST,
            WM_SYSCOMMAND,
            SC_MONITORPOWER,
            2,
            SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
            1000,
            &result) != 0;
    }

    class Renderer
    {
    public:
        enum class Target { Unknown, DesktopPlay, DesktopFreeze, ScreensaverPlay, Paused };

        explicit Renderer(fs::path executable) : executable_(std::move(executable)) {}
        ~Renderer()
        {
            if (!Stop()) ForceCleanupForDestruction();
        }

        bool Apply(Target target, MediaSelection const& media, std::string const& decodeMode,
            std::string const& displayMode, uint32_t frameRateCap,
            std::vector<std::wstring> const& monitorDevices = {},
            std::wstring const& decodeAdapter = {})
        {
            auto now = std::chrono::steady_clock::now();
            std::wstring requestKey = media.path.wstring() + L"\n" + motion::utf8_to_wide(media.kind) + L"\n" +
                motion::utf8_to_wide(decodeMode) + L"\n" + motion::utf8_to_wide(displayMode) + L"\n" +
                std::to_wstring(frameRateCap) + L"\n" + decodeAdapter;
            for (auto const& monitor : monitorDevices) requestKey += L"\n" + monitor;
            bool configurationChanged = requestKey != requestedKey_;
            if (configurationChanged && process_ && !Stop()) return false;
            if (configurationChanged) {
                requestedKey_ = std::move(requestKey);
                permanentlyUnavailable_.store(false, std::memory_order_release);
                automaticStartupTimeouts_ = 0;
                ResetBackOff();
            }
            Refresh();
            if (failed_.exchange(false)) FailAndBackOff();
            if (permanentlyUnavailable_.load(std::memory_order_acquire)) return false;
            if (!process_) {
                if (now < nextLaunchAllowed_) return false;
                if (!Launch(media, decodeMode, displayMode, frameRateCap,
                        monitorDevices, decodeAdapter)) {
                    RecordFailure();
                    return false;
                }
            }
            bool awaitingAck = targetRevision_ && targetAcknowledgedRevision_.load() < targetRevision_;
            if (awaitingAck && now - targetFirstSentAt_ >= 6s) {
                if (decodeMode_ == "auto" && !decodeAdapter_.empty() &&
                    ++automaticStartupTimeouts_ >= 2) {
                    SetDecodeUnavailable("automatic-first-frame-timeout");
                    Shutdown(false);
                    return false;
                }
                FailAndBackOff();
                return false;
            }
            if (target != target_ || (awaitingAck && now - targetSentAt_ >= 2s)) {
                if (target != target_) targetFirstSentAt_ = now;
                target_ = target;
                targetRevision_ = Send(TargetName(target));
                targetSentAt_ = now;
            }
            if (TargetReady()) {
                automaticStartupTimeouts_ = 0;
                if (now - launchedAt_ >= 15s) ResetBackOff();
            }
            return true;
        }

        void Pause()
        {
            RequestExistingTarget(Target::Paused);
        }

        void Freeze()
        {
            RequestExistingTarget(Target::DesktopFreeze);
        }

        [[nodiscard]] bool TargetReady() const noexcept
        {
            return process_ && targetRevision_ &&
                targetAcknowledgedRevision_.load(std::memory_order_acquire) >= targetRevision_;
        }

        [[nodiscard]] bool TransitionPending() const noexcept
        {
            if (permanentlyUnavailable_.load(std::memory_order_acquire) || !process_) return false;
            return !targetRevision_ ||
                targetAcknowledgedRevision_.load(std::memory_order_acquire) < targetRevision_;
        }

        [[nodiscard]] motion::protocol::DecodeStatus DecodeState() const
        {
            std::scoped_lock lock(decodeStatusMutex_);
            return decodeStatus_;
        }

        bool Stop()
        {
            return Shutdown(true);
        }

        // Routes kept alive during a seamless transition are not otherwise
        // applied. Reap a renderer that exited on its own so its playback cache
        // lease is released even if the replacement route cannot become ready.
        void ReapExited()
        {
            Refresh();
        }

    private:
        void RequestExistingTarget(Target target)
        {
            Refresh();
            if (failed_.exchange(false)) { FailAndBackOff(); return; }
            auto now = std::chrono::steady_clock::now();
            bool awaitingAck = targetRevision_ && targetAcknowledgedRevision_.load() < targetRevision_;
            if (awaitingAck && now - targetFirstSentAt_ >= 6s) { FailAndBackOff(); return; }
            if (process_ && (target_ != target || (awaitingAck && now - targetSentAt_ >= 2s))) {
                if (target_ != target) targetFirstSentAt_ = now;
                target_ = target;
                targetRevision_ = Send(TargetName(target));
                targetSentAt_ = now;
            }
        }
        bool Shutdown(bool resetBackOff, bool processAlreadyExited = false)
        {
            if (process_) {
                bool stopped = processAlreadyExited ||
                    WaitForSingleObject(process_.get(), 0) == WAIT_OBJECT_0;
                if (!stopped) {
                    Send("stop");
                    auto wait = WaitForSingleObject(process_.get(), 1200);
                    if (wait == WAIT_TIMEOUT) {
                        if (!TerminateProcess(process_.get(), 0) && job_) {
                            // Closing a kill-on-close job is the final bounded
                            // stop attempt when direct termination is denied.
                            job_.reset();
                        }
                        wait = WaitForSingleObject(process_.get(), 2000);
                    }
                    stopped = wait == WAIT_OBJECT_0;
                }
                // A migration/delete quiescence acknowledgement must never be
                // emitted while a renderer may still own the media file.
                if (!stopped) return false;
                process_.reset();
            }
            job_.reset();
            ResetTransport();
            playbackLease_.reset();
            media_.clear();
            kind_.clear();
            decodeMode_.clear();
            decodeAdapter_.clear();
            monitorDevices_.clear();
            target_ = Target::Unknown;
            targetRevision_ = 0;
            failed_.store(false);
            if (resetBackOff) {
                permanentlyUnavailable_.store(false, std::memory_order_release);
                ResetBackOff();
                requestedKey_.clear();
                std::scoped_lock lock(decodeStatusMutex_);
                decodeStatus_ = {};
            }
            return true;
        }

        void ResetBackOff() noexcept
        {
            failureCount_ = 0;
            nextLaunchAllowed_ = std::chrono::steady_clock::time_point::min();
        }

        void RecordFailure() noexcept
        {
            failureCount_ = (std::min)(failureCount_ + 1u, 7u);
            auto delay = std::chrono::milliseconds(500u << (failureCount_ - 1));
            nextLaunchAllowed_ = std::chrono::steady_clock::now() +
                (std::min)(delay, std::chrono::duration_cast<std::chrono::milliseconds>(30s));
        }

        void FailAndBackOff()
        {
            Shutdown(false);
            RecordFailure();
        }

        void SetDecodeUnavailable(std::string reason)
        {
            permanentlyUnavailable_.store(true, std::memory_order_release);
            std::scoped_lock lock(decodeStatusMutex_);
            decodeStatus_ = { "unavailable", std::move(reason) };
        }

        static char const* TargetName(Target target)
        {
            switch (target) {
            case Target::DesktopPlay: return "desktop-play";
            case Target::DesktopFreeze: return "desktop-freeze";
            case Target::ScreensaverPlay: return "screensaver-play";
            default: return "pause";
            }
        }

        bool Launch(MediaSelection const& media, std::string const& decodeMode,
            std::string const& displayMode, uint32_t frameRateCap,
            std::vector<std::wstring> const& monitorDevices,
            std::wstring const& decodeAdapter)
        {
            if (!Shutdown(false)) return false;
            if (media.path.empty() || !fs::is_regular_file(executable_)) return false;

            SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
            HANDLE inputReadRaw{}, inputWriteRaw{}, outputReadRaw{}, outputWriteRaw{};
            if (!CreatePipe(&inputReadRaw, &inputWriteRaw, &security, 0)) return false;
            motion::unique_handle inputRead(inputReadRaw);
            inputWrite_.reset(inputWriteRaw);
            SetHandleInformation(inputWrite_.get(), HANDLE_FLAG_INHERIT, 0);
            if (!CreatePipe(&outputReadRaw, &outputWriteRaw, &security, 0)) { inputWrite_.reset(); return false; }
            outputRead_.reset(outputReadRaw);
            motion::unique_handle outputWrite(outputWriteRaw);
            SetHandleInformation(outputRead_.get(), HANDLE_FLAG_INHERIT, 0);
            motion::unique_handle nullOutput(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));

            wchar_t hidden[2]{};
            bool launchHidden = GetEnvironmentVariableW(L"MOTIONWALLPAPER_RENDERER_HIDDEN", hidden, ARRAYSIZE(hidden)) && hidden[0] == L'1';
            std::vector<std::wstring> arguments{
                executable_.wstring(), launchHidden ? L"-hidden" : L"-desktop", L"-video", media.path.wstring(),
                L"-kind", motion::utf8_to_wide(media.kind), L"-decode", motion::utf8_to_wide(decodeMode),
                L"-display", motion::utf8_to_wide(displayMode),
                L"-frame-cap", std::to_wstring(frameRateCap)
            };
            for (auto const& monitorDevice : monitorDevices) {
                arguments.push_back(L"-monitor");
                arguments.push_back(monitorDevice);
            }
            if (!decodeAdapter.empty()) {
                arguments.push_back(L"-adapter");
                arguments.push_back(decodeAdapter);
            }
            auto command = motion::build_command_line(arguments);
            STARTUPINFOW startup{ sizeof(startup) };
            startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            startup.wShowWindow = SW_HIDE;
            startup.hStdInput = inputRead.get();
            startup.hStdOutput = outputWrite.get();
            startup.hStdError = nullOutput.get();
            PROCESS_INFORMATION created{};
            if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                nullptr, executable_.parent_path().c_str(), &startup, &created)) {
                ResetTransport();
                return false;
            }
            motion::unique_handle createdThread(created.hThread);
            process_.reset(created.hProcess);
            job_.reset(CreateJobObjectW(nullptr, nullptr));
            if (job_) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (!SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
                    !AssignProcessToJobObject(job_.get(), process_.get())) {
                    job_.reset();
                }
            }
            if (ResumeThread(createdThread.get()) == static_cast<DWORD>(-1)) {
                TerminateProcess(process_.get(), ERROR_PROCESS_ABORTED);
                Shutdown(false);
                return false;
            }
            media_ = media.path;
            playbackLease_ = media.playbackLease;
            kind_ = media.kind;
            decodeMode_ = decodeMode;
            decodeAdapter_ = decodeAdapter;
            displayMode_ = displayMode;
            monitorDevices_ = monitorDevices;
            target_ = Target::Unknown;
            targetAcknowledgedRevision_.store(0);
            failed_.store(false);
            launchedAt_ = std::chrono::steady_clock::now();
            {
                std::scoped_lock lock(decodeStatusMutex_);
                decodeStatus_ = { "probing", "detecting" };
            }
            outputStopping_.store(false, std::memory_order_release);
            outputThread_ = std::thread([this] { ReadAcks(); });
            return true;
        }

        uint64_t Send(std::string const& command)
        {
            if (!inputWrite_) return 0;
            uint64_t revision = ++revision_;
            std::string value = command + " " + std::to_string(revision) + "\n";
            DWORD written{};
            if (!WriteFile(inputWrite_.get(), value.data(), static_cast<DWORD>(value.size()), &written, nullptr) ||
                written != static_cast<DWORD>(value.size())) {
                // Never re-enter Refresh/Shutdown from a broken control pipe.
                // The next policy tick owns process cleanup and backoff.
                failed_.store(true, std::memory_order_release);
                inputWrite_.reset();
                return 0;
            }
            return revision;
        }

        void ReadAcks()
        {
            std::string pending;
            char buffer[256];
            DWORD bytes{};
            while (!outputStopping_.load(std::memory_order_acquire) && outputRead_ &&
                ReadFile(outputRead_.get(), buffer, sizeof(buffer), &bytes, nullptr) && bytes) {
                pending.append(buffer, bytes);
                for (size_t end; (end = pending.find('\n')) != std::string::npos; pending.erase(0, end + 1)) {
                    auto value = pending.substr(0, end);
                    auto ack = motion::protocol::parse_ack(value);
                    if (ack.channel == motion::protocol::AckChannel::Target) {
                        uint64_t previous = targetAcknowledgedRevision_.load();
                        while (previous < ack.revision && !targetAcknowledgedRevision_.compare_exchange_weak(previous, ack.revision)) {}
                    } else if (auto status = motion::protocol::parse_decode_status(value); !status.path.empty()) {
                        if (status.path == "unavailable") {
                            permanentlyUnavailable_.store(true, std::memory_order_release);
                        }
                        std::scoped_lock lock(decodeStatusMutex_);
                        decodeStatus_ = std::move(status);
                    } else if (value.starts_with("error ")) {
                        failed_.store(true);
                    }
                }
            }
        }

        void Refresh()
        {
            if (!process_ || WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT) return;
            // The process is already signaled, so sending "stop" would write
            // to a broken pipe and recursively enter this cleanup path.
            if (!Shutdown(false, true)) return;
            RecordFailure();
        }

        void ResetTransport()
        {
            inputWrite_.reset();
            outputStopping_.store(true, std::memory_order_release);
            if (outputThread_.joinable()) outputThread_.join();
            outputRead_.reset();
        }

        void ForceCleanupForDestruction() noexcept
        {
            // Destruction cannot leave a joinable acknowledgement thread. This
            // path is reserved for Agent teardown after the bounded graceful
            // stop failed; closing the kill-on-close job and cancelling the
            // pipe read make member destruction deterministic.
            inputWrite_.reset();
            if (job_) job_.reset();
            if (process_) {
                TerminateProcess(process_.get(), 0);
                WaitForSingleObject(process_.get(), 2000);
            }
            outputStopping_.store(true, std::memory_order_release);
            if (outputThread_.joinable()) {
                CancelSynchronousIo(outputThread_.native_handle());
                outputThread_.join();
            }
            outputRead_.reset();
            process_.reset();
            playbackLease_.reset();
        }

        fs::path executable_;
        motion::unique_handle process_;
        motion::unique_handle job_;
        motion::unique_handle inputWrite_;
        motion::unique_handle outputRead_;
        std::thread outputThread_;
        std::atomic_bool outputStopping_{ true };
        fs::path media_;
        motion::agent::VideoPlaybackLease playbackLease_;
        std::string kind_;
        std::string decodeMode_;
        std::wstring decodeAdapter_;
        std::string displayMode_{ "primary" };
        std::vector<std::wstring> monitorDevices_;
        std::wstring requestedKey_;
        Target target_{ Target::Unknown };
        uint64_t revision_{};
        uint64_t targetRevision_{};
        std::atomic_uint64_t targetAcknowledgedRevision_{};
        std::atomic_bool failed_{};
        std::atomic_bool permanentlyUnavailable_{};
        mutable std::mutex decodeStatusMutex_;
        motion::protocol::DecodeStatus decodeStatus_;
        std::chrono::steady_clock::time_point targetSentAt_{};
        std::chrono::steady_clock::time_point targetFirstSentAt_{};
        std::chrono::steady_clock::time_point launchedAt_{};
        std::chrono::steady_clock::time_point nextLaunchAllowed_{};
        unsigned failureCount_{};
        unsigned automaticStartupTimeouts_{};
    };

    struct DisplayMediaTarget
    {
        std::wstring deviceName;
        std::string groupId;
        std::string mediaId;
        MediaSelection media;
        uint32_t targetWidth{};
        uint32_t targetHeight{};
        uint32_t targetRefreshRate{};
        std::wstring decodeAdapter;
        bool softwarePlaybackTarget{};
        uint32_t playbackFrameRateCap{};
    };

    std::vector<DisplayMediaTarget> display_media_targets(fs::path const& wallpapers, motion::Settings const& settings,
        std::string const& defaultGroupId, std::string const& defaultMediaId,
        std::string const& performanceMode)
    {
        auto defaultMedia = media_by_id(wallpapers, defaultGroupId, defaultMediaId, performanceMode);
        auto displays = motion::enumerate_displays();
        if (settings.displayMode == "primary") {
            if (defaultMedia.path.empty()) return {};
            auto primary = std::find_if(displays.begin(), displays.end(), [](auto const& display) { return display.primary; });
            uint32_t width = primary == displays.end() ? 0u : static_cast<uint32_t>(primary->bounds.right - primary->bounds.left);
            uint32_t height = primary == displays.end() ? 0u : static_cast<uint32_t>(primary->bounds.bottom - primary->bounds.top);
            uint32_t refreshRate = primary == displays.end() ? 60u : primary->refreshRateHz;
            return { { primary == displays.end() ? std::wstring{} : primary->deviceName,
                defaultGroupId, defaultMediaId, std::move(defaultMedia), width, height, refreshRate } };
        }

        std::vector<DisplayMediaTarget> targets;
        for (auto const& display : displays) {
            auto width = static_cast<uint32_t>(display.bounds.right - display.bounds.left);
            auto height = static_cast<uint32_t>(display.bounds.bottom - display.bounds.top);
            auto groupId = defaultGroupId;
            auto mediaId = defaultMediaId;
            auto assignment = std::find_if(settings.displayAssignments.begin(), settings.displayAssignments.end(),
                [&](auto const& value) { return value.displayId == display.id; });
            if (assignment != settings.displayAssignments.end()) {
                auto assigned = media_by_id(
                    wallpapers, assignment->groupId, assignment->mediaId, performanceMode);
                if (!assigned.path.empty()) {
                    groupId = assignment->groupId;
                    mediaId = assignment->mediaId;
                    targets.push_back({ display.deviceName,
                        std::move(groupId), std::move(mediaId), std::move(assigned), width, height, display.refreshRateHz });
                    continue;
                }
            }
            if (!defaultMedia.path.empty()) {
                targets.push_back({ display.deviceName,
                    std::move(groupId), std::move(mediaId), defaultMedia, width, height, display.refreshRateHz });
            }
        }
        return targets;
    }

    class RendererPool
    {
    public:
        explicit RendererPool(fs::path executable) : executable_(std::move(executable)) {}

        void Apply(Renderer::Target target, std::vector<DisplayMediaTarget> const& outputs,
            std::string const& decodeMode, bool primaryOnly)
        {
            std::vector<motion::agent::RendererRoute> routes;
            routes.reserve(outputs.size());
            for (auto const& output : outputs) {
                auto adapterKey = primaryOnly ? std::wstring{} : display_adapter_key(output.deviceName);
                routes.push_back({ motion::agent::renderer_media_key(output.media.path, output.media.kind),
                    output.deviceName, primaryOnly ? std::wstring{} :
                    motion::agent::renderer_adapter_key(std::move(adapterKey), output.deviceName),
                    static_cast<uint64_t>(output.targetWidth) * output.targetHeight });
            }
            auto grouped = motion::agent::group_renderer_routes(routes, !primaryOnly);
            std::vector<std::wstring> desiredKeys;
            desiredKeys.reserve(grouped.size());
            bool allReady = !grouped.empty();
            for (auto const& route : grouped) {
                auto output = std::find_if(outputs.begin(), outputs.end(), [&](auto const& value) {
                    if (motion::agent::renderer_media_key(value.media.path, value.media.kind) !=
                        route.mediaKey) return false;
                    // The same media can have one route per display adapter.
                    // Select an output that actually belongs to this route so
                    // its probed decoder LUID cannot leak to another GPU.
                    return primaryOnly || route.monitorDevices.empty() ||
                        std::find(route.monitorDevices.begin(), route.monitorDevices.end(),
                            value.deviceName) != route.monitorDevices.end();
                });
                if (output == outputs.end()) continue;
                uint32_t routeFrameRateCap{};
                for (auto const& candidate : outputs) {
                    if (motion::agent::renderer_media_key(candidate.media.path,
                            candidate.media.kind) != route.mediaKey) continue;
                    if (!primaryOnly && !route.monitorDevices.empty() &&
                        std::find(route.monitorDevices.begin(), route.monitorDevices.end(),
                            candidate.deviceName) == route.monitorDevices.end()) continue;
                    if (candidate.playbackFrameRateCap) {
                        routeFrameRateCap = routeFrameRateCap
                            ? (std::min)(routeFrameRateCap, candidate.playbackFrameRateCap)
                            : candidate.playbackFrameRateCap;
                    }
                }
                auto routeKey = RouteKey(route, decodeMode, primaryOnly, routeFrameRateCap);
                desiredKeys.push_back(routeKey);
                auto& renderer = renderers_[routeKey];
                if (!renderer) renderer = std::make_unique<Renderer>(executable_);
                bool applied = renderer->Apply(target, output->media, decodeMode,
                    primaryOnly ? "primary" : "monitor", routeFrameRateCap,
                    route.monitorDevices, output->decodeAdapter);
                auto decode = renderer->DecodeState();
                if (motion::agent::automatic_decode_failure_requires_cpu_smooth(
                        decodeMode, decode.path, decode.reason,
                        !output->decodeAdapter.empty())) {
                    RememberAutoDecodeFailure(output->media.path, output->decodeAdapter);
                }
                allReady = applied && renderer->TargetReady() && allReady;
            }
            desiredKeys_ = desiredKeys;
            if (allReady) {
                for (auto iterator = renderers_.begin(); iterator != renderers_.end();) {
                    if (std::find(desiredKeys.begin(), desiredKeys.end(), iterator->first) == desiredKeys.end()) {
                        // Do not destroy a Renderer until its child process and
                        // acknowledgement thread are confirmed stopped. A
                        // failed retirement keeps the playback lease pinned and
                        // is retried by the next policy pass.
                        if (iterator->second->Stop()) iterator = renderers_.erase(iterator);
                        else ++iterator;
                    } else {
                        ++iterator;
                    }
                }
            } else {
                for (auto& [key, renderer] : renderers_) {
                    if (std::find(desiredKeys.begin(), desiredKeys.end(), key) == desiredKeys.end()) {
                        renderer->ReapExited();
                    }
                }
            }
        }

        void Pause()
        {
            for (auto& [_, renderer] : renderers_) renderer->Pause();
        }

        void Freeze()
        {
            for (auto& [_, renderer] : renderers_) renderer->Freeze();
        }

        [[nodiscard]] bool TargetReady() const
        {
            return !desiredKeys_.empty() && std::all_of(desiredKeys_.begin(), desiredKeys_.end(), [&](auto const& key) {
                auto found = renderers_.find(key);
                return found != renderers_.end() && found->second->TargetReady();
            });
        }

        [[nodiscard]] bool TransitionPending() const
        {
            return std::any_of(desiredKeys_.begin(), desiredKeys_.end(), [&](auto const& key) {
                auto found = renderers_.find(key);
                return found != renderers_.end() && found->second->TransitionPending();
            });
        }

        [[nodiscard]] bool HasActiveRoute() const noexcept { return !desiredKeys_.empty(); }

        [[nodiscard]] bool AutoDecodeRouteRejected(
            fs::path const& media, std::wstring const& adapter) const
        {
            if (adapter.empty()) return false;
            auto normalized = media.lexically_normal().wstring();
            return std::any_of(autoDecodeFailures_.begin(), autoDecodeFailures_.end(),
                [&](auto const& failure) {
                    return failure.second == adapter &&
                        _wcsicmp(failure.first.c_str(), normalized.c_str()) == 0;
                });
        }

        void ResetAutoDecodeFailures() noexcept { autoDecodeFailures_.clear(); }

        [[nodiscard]] motion::protocol::DecodeStatus DecodeState() const
        {
            motion::protocol::DecodeStatus aggregate;
            int aggregateRank = -1;
            auto rank = [](std::string const& path) {
                if (path == "unavailable") return 6;
                if (path == "software-fallback") return 5;
                if (path == "software") return 4;
                if (path == "automatic") return 3;
                if (path == "hardware") return 3;
                if (path == "not-applicable") return 2;
                if (path == "probing") return 1;
                return 0;
            };
            for (auto const& key : desiredKeys_) {
                auto found = renderers_.find(key);
                if (found == renderers_.end()) continue;
                auto status = found->second->DecodeState();
                int statusRank = rank(status.path);
                if (statusRank > aggregateRank) {
                    aggregateRank = statusRank;
                    aggregate = std::move(status);
                }
            }
            return aggregate;
        }

        void TopologyChanged() noexcept
        {
            ++topologyGeneration_;
            ResetAutoDecodeFailures();
        }
        bool Stop()
        {
            desiredKeys_.clear();
            bool stopped = true;
            for (auto& [_, renderer] : renderers_) {
                stopped = renderer->Stop() && stopped;
            }
            if (stopped) renderers_.clear();
            return stopped;
        }

    private:
        std::wstring RouteKey(motion::agent::SharedRendererRoute const& route,
            std::string const& decodeMode, bool primaryOnly,
            uint32_t frameRateCap) const
        {
            std::wstring key = route.mediaKey + L"\n" + motion::utf8_to_wide(decodeMode) +
                (primaryOnly ? L"\nprimary\n" : L"\nmonitor\n") + route.adapterKey + L"\n" +
                std::to_wstring(frameRateCap) + L"\n" +
                std::to_wstring(topologyGeneration_);
            for (auto const& monitor : route.monitorDevices) key += L"\n" + monitor;
            return key;
        }

        void RememberAutoDecodeFailure(fs::path const& media, std::wstring const& adapter)
        {
            if (AutoDecodeRouteRejected(media, adapter)) return;
            autoDecodeFailures_.emplace_back(media.lexically_normal().wstring(), adapter);
        }

        fs::path executable_;
        std::map<std::wstring, std::unique_ptr<Renderer>> renderers_;
        std::vector<std::wstring> desiredKeys_;
        std::vector<std::pair<std::wstring, std::wstring>> autoDecodeFailures_;
        uint64_t topologyGeneration_{};
    };

    struct InputState
    {
        DWORD tick{};
        std::chrono::milliseconds idle{};
    };

    InputState input_state()
    {
        LASTINPUTINFO input{ sizeof(input) };
        if (!GetLastInputInfo(&input)) return {};
        auto currentTick = static_cast<DWORD>(GetTickCount64());
        return { input.dwTime, std::chrono::milliseconds(static_cast<DWORD>(currentTick - input.dwTime)) };
    }

    struct IdleInhibition
    {
        bool display{};
        bool system{};
    };

    class IdleInhibitor
    {
    public:
        IdleInhibition State(std::chrono::steady_clock::time_point now)
        {
            if (now < nextSample_) return state_;
            nextSample_ = now + 500ms;
            EXECUTION_STATE state{};
            if (CallNtPowerInformation(SystemExecutionState, nullptr, 0, &state, sizeof(state)) == ERROR_SUCCESS) {
                state_.display = (state & ES_DISPLAY_REQUIRED) != 0;
                state_.system = (state & ES_SYSTEM_REQUIRED) != 0;
            } else {
                state_ = {};
            }
            return state_;
        }

    private:
        IdleInhibition state_{};
        std::chrono::steady_clock::time_point nextSample_{};
    };

    std::optional<bool> query_session_locked()
    {
        LPWSTR buffer{};
        DWORD bytes{};
        if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSConnectState, &buffer, &bytes)) return std::nullopt;
        auto release = [](wchar_t* value) noexcept { if (value) WTSFreeMemory(value); };
        std::unique_ptr<wchar_t, decltype(release)> memory(buffer, release);
        if (!buffer || bytes < sizeof(WTS_CONNECTSTATE_CLASS)) return std::nullopt;
        auto state = *reinterpret_cast<WTS_CONNECTSTATE_CLASS*>(buffer);
        return state != WTSActive;
    }

    class RuntimeEvents
    {
    public:
        explicit RuntimeEvents(fs::path settingsExecutable) : settingsExecutable_(std::move(settingsExecutable))
        {
            appExitEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, motion::app_exit_event_name));
            if (auto locked = query_session_locked()) locked_ = *locked;
            SYSTEM_POWER_STATUS power{};
            if (GetSystemPowerStatus(&power) && power.ACLineStatus != 255) {
                onBattery_ = power.ACLineStatus == 0;
            }
            taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
            WNDCLASSEXW definition{ sizeof(definition) };
            definition.lpfnWndProc = WindowProc;
            definition.hInstance = GetModuleHandleW(nullptr);
            definition.lpszClassName = L"MotionWallpaper.Agent.Events";
            if (!RegisterClassExW(&definition) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
            window_ = CreateWindowExW(0, definition.lpszClassName, L"", WS_OVERLAPPED,
                0, 0, 0, 0, nullptr, nullptr, definition.hInstance, this);
            if (!window_) return;
            sessionNotificationRegistered_ =
                WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION) != FALSE;
            displayNotification_ = RegisterPowerSettingNotification(window_, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
            powerSourceNotification_ = RegisterPowerSettingNotification(window_, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);
            eventWindow_.store(window_, std::memory_order_release);
            foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                ForegroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            minimizeHook_ = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr,
                ForegroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            locationHook_ = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
                ForegroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            AddTrayIcon();
        }

        ~RuntimeEvents()
        {
            if (foregroundHook_) UnhookWinEvent(foregroundHook_);
            if (minimizeHook_) UnhookWinEvent(minimizeHook_);
            if (locationHook_) UnhookWinEvent(locationHook_);
            HWND expected = window_;
            eventWindow_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
            NOTIFYICONDATAW icon{ sizeof(icon) };
            icon.hWnd = window_;
            icon.uID = trayIconId;
            if (window_) Shell_NotifyIconW(NIM_DELETE, &icon);
            if (displayNotification_) UnregisterPowerSettingNotification(displayNotification_);
            if (powerSourceNotification_) UnregisterPowerSettingNotification(powerSourceNotification_);
            if (window_) {
                if (sessionNotificationRegistered_) WTSUnRegisterSessionNotification(window_);
                DestroyWindow(window_);
            }
        }

        explicit operator bool() const { return window_ != nullptr && static_cast<bool>(appExitEvent_); }
        bool Locked() const { return locked_; }
        bool DisplayOn() const { return displayOn_; }
        bool OnBattery() const { return onBattery_; }
        uint64_t TopologyRevision() const { return topologyRevision_; }
        bool ExitRequested() const { return exitRequested_; }

        void PollSessionState(std::chrono::steady_clock::time_point now)
        {
            if (now < nextSessionPoll_) return;
            nextSessionPoll_ = now + 1s;
            if (auto locked = query_session_locked()) locked_ = *locked;
        }

        bool Wait(HANDLE settingsEvent, DWORD milliseconds)
        {
            HANDLE handles[]{ settingsEvent, appExitEvent_.get() };
            DWORD result = MsgWaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, milliseconds, QS_ALLINPUT);
            if (result == WAIT_OBJECT_0) return true;
            if (result == WAIT_OBJECT_0 + 1) {
                exitRequested_ = true;
                return false;
            }
            if (result == WAIT_OBJECT_0 + ARRAYSIZE(handles)) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            return false;
        }

    private:
        static constexpr UINT wmTrayIcon = WM_APP + 100;
        static constexpr UINT wmForegroundChanged = WM_APP + 101;
        static constexpr UINT trayIconId = 1;
        static inline std::atomic<HWND> eventWindow_{};
        static inline std::atomic_bool foregroundWakePending_{};

        static void CALLBACK ForegroundEvent(HWINEVENTHOOK, DWORD event, HWND, LONG object, LONG child, DWORD, DWORD)
        {
            if (event == EVENT_OBJECT_LOCATIONCHANGE && (object != OBJID_WINDOW || child != CHILDID_SELF)) return;
            if (!foregroundWakePending_.exchange(true, std::memory_order_acq_rel)) {
                if (HWND window = eventWindow_.load(std::memory_order_acquire); !window || !PostMessageW(window, wmForegroundChanged, 0, 0)) {
                    foregroundWakePending_.store(false, std::memory_order_release);
                }
            }
        }

        void AddTrayIcon()
        {
            if (!window_) return;
            NOTIFYICONDATAW icon{ sizeof(icon) };
            icon.hWnd = window_;
            icon.uID = trayIconId;
            icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
            icon.uCallbackMessage = wmTrayIcon;
            icon.hIcon = static_cast<HICON>(LoadImageW(
                GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_MOTIONWALLPAPER), IMAGE_ICON,
                GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
            if (!icon.hIcon) icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
            wcscpy_s(icon.szTip, L"MotionWallpaper");
            Shell_NotifyIconW(NIM_ADD, &icon);
            icon.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &icon);
        }

        void OpenSettings() const
        {
            if (fs::is_regular_file(settingsExecutable_)) {
                ShellExecuteW(nullptr, L"open", settingsExecutable_.c_str(), nullptr, settingsExecutable_.parent_path().c_str(), SW_SHOWNORMAL);
            }
        }

        void ShowTrayMenu()
        {
            POINT cursor{};
            GetCursorPos(&cursor);
            HMENU menu = CreatePopupMenu();
            if (!menu) return;
            AppendMenuW(menu, MF_STRING, 1, L"打开 MotionWallpaper");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 2, L"退出");
            SetForegroundWindow(window_);
            UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                cursor.x, cursor.y, 0, window_, nullptr);
            DestroyMenu(menu);
            if (command == 1) OpenSettings();
            else if (command == 2) {
                if (appExitEvent_) SetEvent(appExitEvent_.get());
                exitRequested_ = true;
            }
        }

        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
        {
            RuntimeEvents* self = reinterpret_cast<RuntimeEvents*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_NCCREATE) {
                auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
                self = static_cast<RuntimeEvents*>(create->lpCreateParams);
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            if (!self) return DefWindowProcW(window, message, wParam, lParam);
            if (self->taskbarCreated_ && message == self->taskbarCreated_) {
                ++self->topologyRevision_;
                self->AddTrayIcon();
                return 0;
            }
            if (message == wmTrayIcon) {
                auto event = LOWORD(lParam);
                if (event == WM_LBUTTONDBLCLK) self->OpenSettings();
                else if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) self->ShowTrayMenu();
                return 0;
            }
            if (message == wmForegroundChanged) {
                foregroundWakePending_.store(false, std::memory_order_release);
                return 0;
            }
            switch (message) {
            case WM_WTSSESSION_CHANGE:
                switch (wParam) {
                case WTS_SESSION_LOCK:
                case WTS_CONSOLE_DISCONNECT:
                case WTS_REMOTE_DISCONNECT:
                case WTS_SESSION_LOGOFF:
                    self->locked_ = true;
                    break;
                case WTS_SESSION_UNLOCK:
                case WTS_CONSOLE_CONNECT:
                case WTS_REMOTE_CONNECT:
                case WTS_SESSION_LOGON:
                case WTS_SESSION_DESKTOP_READY:
                    self->locked_ = false;
                    break;
                }
                return 0;
            case WM_DISPLAYCHANGE:
                ++self->topologyRevision_;
                return 0;
            case WM_POWERBROADCAST:
                if (wParam == PBT_POWERSETTINGCHANGE) {
                    auto setting = reinterpret_cast<POWERBROADCAST_SETTING*>(lParam);
                    if (setting && IsEqualGUID(setting->PowerSetting, GUID_CONSOLE_DISPLAY_STATE) && setting->DataLength >= sizeof(DWORD)) {
                        self->displayOn_ = *reinterpret_cast<DWORD*>(setting->Data) != 0;
                    } else if (setting && IsEqualGUID(setting->PowerSetting, GUID_ACDC_POWER_SOURCE) &&
                        setting->DataLength >= sizeof(DWORD)) {
                        auto source = *reinterpret_cast<DWORD*>(setting->Data);
                        if (source <= PoHot) self->onBattery_ = source != PoAc;
                    }
                }
                return TRUE;
            }
            return DefWindowProcW(window, message, wParam, lParam);
        }

        HWND window_{};
        HPOWERNOTIFY displayNotification_{};
        HPOWERNOTIFY powerSourceNotification_{};
        HWINEVENTHOOK foregroundHook_{};
        HWINEVENTHOOK minimizeHook_{};
        HWINEVENTHOOK locationHook_{};
        UINT taskbarCreated_{};
        fs::path settingsExecutable_;
        motion::unique_handle appExitEvent_;
        bool locked_{};
        bool sessionNotificationRegistered_{};
        bool displayOn_{ true };
        bool onBattery_{};
        bool exitRequested_{};
        uint64_t topologyRevision_{};
        std::chrono::steady_clock::time_point nextSessionPoll_{};
    };

    bool shell_window(HWND window)
    {
        wchar_t name[128]{};
        GetClassNameW(window, name, ARRAYSIZE(name));
        return !_wcsicmp(name, L"Progman") || !_wcsicmp(name, L"WorkerW") || !_wcsicmp(name, L"SHELLDLL_DefView") || !_wcsicmp(name, L"Shell_TrayWnd");
    }

    motion::agent::WindowBounds window_bounds(RECT const& value)
    {
        return { value.left, value.top, value.right, value.bottom };
    }

    bool visible_application_window(HWND window)
    {
        if (!window || !IsWindowVisible(window) || IsIconic(window) || shell_window(window)) return false;
        DWORD cloaked{};
        if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
        if (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED) {
            BYTE alpha{};
            DWORD flags{};
            if (GetLayeredWindowAttributes(window, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) && alpha == 0) return false;
        }
        return true;
    }

    bool window_covers_display(HWND window)
    {
        if (!visible_application_window(window)) return false;
        HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
        MONITORINFO info{ sizeof(info) };
        if (!monitor || !GetMonitorInfoW(monitor, &info)) return false;
        RECT bounds{};
        if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))) && !GetWindowRect(window, &bounds)) return false;
        auto windowBounds = window_bounds(bounds);
        return motion::agent::covers_display(windowBounds, window_bounds(info.rcMonitor)) ||
            motion::agent::covers_display(windowBounds, window_bounds(info.rcWork));
    }

    bool desktop_covered()
    {
        bool covered{};
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
            auto result = reinterpret_cast<bool*>(parameter);
            if (!window_covers_display(window)) return TRUE;
            *result = true;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&covered));
        return covered;
    }

    void enable_eco_qos()
    {
        struct PowerThrottlingState { ULONG version; ULONG controlMask; ULONG stateMask; };
        constexpr ULONG processPowerThrottling = 4;
        constexpr ULONG executionSpeed = 0x1;
        PowerThrottlingState state{ 1, executionSpeed, executionSpeed };
        SetProcessInformation(GetCurrentProcess(), static_cast<PROCESS_INFORMATION_CLASS>(processPowerThrottling), &state, sizeof(state));
    }

    void append_agent_log(fs::path const& root, std::wstring_view message) noexcept
    {
        motion::append_utf8_log(root / L"Config" / L"agent.log", message);
    }

    std::optional<motion::MediaLibraryTrustIdentity> trusted_custom_library(
        motion::Settings const& settings, fs::path const& wallpapers)
    {
        if (settings.mediaLibraryPath.empty()) return std::nullopt;
        auto identity = motion::capture_media_library_trust(wallpapers);
        // Bind the complete owned-tree check to stable root/marker/Groups
        // identities. The cheap loop guard can then avoid rescanning a large
        // library while still detecting removal and path reuse.
        if (!identity || identity->ownershipId != settings.mediaLibraryId ||
            !motion::is_owned_media_library(wallpapers) ||
            !motion::revalidate_media_library_trust(*identity)) {
            throw std::runtime_error("media library trust identity changed");
        }
        return identity;
    }

    bool same_library_trust(
        std::optional<motion::MediaLibraryTrustIdentity> const& left,
        std::optional<motion::MediaLibraryTrustIdentity> const& right) noexcept
    {
        if (left.has_value() != right.has_value()) return false;
        if (!left) return true;
        return left->ownershipId == right->ownershipId &&
            left->rootIdentity == right->rootIdentity &&
            left->markerIdentity == right->markerIdentity &&
            left->groupsIdentity == right->groupsIdentity;
    }
}

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
{
    motion::enable_per_monitor_dpi_awareness();
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    motion::unique_handle mutex(CreateMutexW(nullptr, TRUE, L"Local\\MotionWallpaper.Agent"));
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    motion::unique_handle settingsEvent(CreateEventW(nullptr, FALSE, FALSE, motion::settings_event_name));
    if (!settingsEvent) return 1;
    motion::agent::LibraryMigrationProtocol libraryMigration;
    if (!libraryMigration) return 1;
    libraryMigration.ClearAcknowledgements();
    enable_eco_qos();
    auto applicationRoot = motion::executable_directory();
    bool legacyDataConflict = motion::legacy_data_conflict_present(applicationRoot);
    // A conflict marker means neither the legacy nor install-tree copy is
    // authoritative. Do not even select or inspect a data tree until an
    // installer/recovery pass has resolved the conflict.
    auto root = legacyDataConflict ? applicationRoot : motion::application_data_directory();

    try {
        fs::path configPath = root / L"Config" / L"settings.json";
        fs::path runtimePath = root / L"Config" / L"runtime.json";
        motion::Settings settings;
        auto initialSettingsStatus = legacyDataConflict
            ? motion::SettingsFileStatus::invalid
            : motion::load_settings_file(configPath, settings);
        // A genuinely missing document is the compatible first-run case. Once
        // any document has existed, removing or corrupting it must not silently
        // redirect the Agent to the writable default library.
        bool allowMissingFirstRun = !legacyDataConflict &&
            initialSettingsStatus == motion::SettingsFileStatus::missing;
        bool configurationAvailable = initialSettingsStatus == motion::SettingsFileStatus::valid ||
            allowMissingFirstRun;
        fs::path wallpapers;
        std::optional<motion::MediaLibraryTrustIdentity> mediaLibraryTrust;
        RendererPool renderers(applicationRoot / L"motionwallpaper-renderer.exe");
        std::unique_ptr<motion::agent::VideoOptimizer> videoOptimizer;
        if (configurationAvailable) {
            try {
                wallpapers = motion::wallpaper_library_directory(root, settings.mediaLibraryPath);
                mediaLibraryTrust = trusted_custom_library(settings, wallpapers);
                videoOptimizer = std::make_unique<motion::agent::VideoOptimizer>(
                    wallpapers, root, applicationRoot, mediaLibraryTrust);
            } catch (...) {
                configurationAvailable = false;
                allowMissingFirstRun = false;
                wallpapers.clear();
                mediaLibraryTrust.reset();
            }
        }
        RuntimeEvents runtimeEvents(applicationRoot / L"MotionWallpaper.exe");
        if (!runtimeEvents) return 1;
        std::string previousRandomGroup, previousSelectedGroup, previousSelectedMedia, previousPerformanceMode, randomId;
        int previousRandomInterval = -1;
        bool wasLocked = false, autoLockTriggered = false, displayOffAfterLockTriggered = false;
        bool reload = true, selectionInitialized = false;
        motion::IdleTimer screensaverIdleTimer;
        motion::IdleTimer autoLockIdleTimer;
        bool screensaverWasActive{};
        IdleInhibitor idleInhibitor;
        auto nextFallbackReload = std::chrono::steady_clock::now();
        std::error_code configWriteTimeError;
        fs::file_time_type configWriteTime{};
        bool configWriteTimeKnown{};
        if (!legacyDataConflict) {
            configWriteTime = fs::last_write_time(configPath, configWriteTimeError);
            configWriteTimeKnown = !configWriteTimeError;
        }
        auto nextOptimizationRequestScan = std::chrono::steady_clock::time_point::min();
        std::vector<ImportedOptimizationRequest> importedRequests;
        auto nextRandomChange = std::chrono::steady_clock::time_point::max();
        auto nextAutoLockAttempt = std::chrono::steady_clock::time_point::min();
        auto lockObservedAt = std::chrono::steady_clock::time_point::max();
        auto nextDisplayOffAfterLockAttempt = std::chrono::steady_clock::time_point::min();
        std::optional<std::chrono::steady_clock::time_point> missingMediaSince;
        uint64_t topologyRevision = runtimeEvents.TopologyRevision();
        uint32_t logicalProcessors = (std::max)(1u,
            static_cast<uint32_t>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
        bool physicalVideoDeviceAvailable = physical_video_device_available();
        std::string publishedGroupId, publishedMediaId, publishedDecodePath, publishedDecodeReason;
        bool configFailureReported{};
        bool runtimeFailureReported{};
        bool automaticCompatibilityPriority{};
        auto publishRuntime = [&](std::string groupId, std::string mediaId,
            std::string decodePath = {}, std::string decodeReason = {}) {
            if (legacyDataConflict) return;
            if (groupId == publishedGroupId && mediaId == publishedMediaId &&
                decodePath == publishedDecodePath && decodeReason == publishedDecodeReason) return;
            try {
                motion::RuntimeState runtime;
                runtime.activeGroupId = groupId;
                runtime.activeMediaId = mediaId;
                runtime.decodePath = decodePath;
                runtime.decodeReason = decodeReason;
                runtime.updatedAt = motion::timestamp_utc();
                motion::save_runtime(runtimePath, runtime);
                publishedGroupId = std::move(groupId);
                publishedMediaId = std::move(mediaId);
                publishedDecodePath = std::move(decodePath);
                publishedDecodeReason = std::move(decodeReason);
                runtimeFailureReported = false;
            } catch (...) {
                if (!runtimeFailureReported) append_agent_log(root, L"无法写入运行时壁纸状态。");
                runtimeFailureReported = true;
            }
        };

        for (;;) {
            if (runtimeEvents.ExitRequested()) { renderers.Stop(); return 0; }

            bool conflictNow = motion::legacy_data_conflict_present(applicationRoot);
            if (conflictNow) {
                if (!legacyDataConflict) {
                    renderers.Stop();
                    renderers.ResetAutoDecodeFailures();
                    videoOptimizer.reset();
                    importedRequests.clear();
                    wallpapers.clear();
                    mediaLibraryTrust.reset();
                    configurationAvailable = false;
                    allowMissingFirstRun = false;
                    missingMediaSince.reset();
                    selectionInitialized = false;
                    publishedGroupId.clear();
                    publishedMediaId.clear();
                    publishedDecodePath.clear();
                    publishedDecodeReason.clear();
                }
                legacyDataConflict = true;
                runtimeEvents.Wait(settingsEvent.get(), 1000);
                continue;
            }
            if (legacyDataConflict) {
                // Conflict recovery is a fresh trust boundary. Re-select the
                // authoritative root, but require an actual valid settings
                // document before media work resumes.
                legacyDataConflict = false;
                root = motion::application_data_directory();
                configPath = root / L"Config" / L"settings.json";
                runtimePath = root / L"Config" / L"runtime.json";
                settings = {};
                wallpapers.clear();
                mediaLibraryTrust.reset();
                configurationAvailable = false;
                allowMissingFirstRun = false;
                reload = true;
                configWriteTime = {};
                configWriteTimeKnown = false;
                configFailureReported = false;
                runtimeFailureReported = false;
                nextFallbackReload = std::chrono::steady_clock::now();
            }

            if (libraryMigration.Requested()) {
                // The App must not copy a library while a decoder, FFmpeg, or
                // cache-normalization pass still owns a handle into it.
                bool renderersStopped = renderers.Stop();
                renderers.ResetAutoDecodeFailures();
                videoOptimizer.reset();
                importedRequests.clear();
                mediaLibraryTrust.reset();
                publishRuntime({}, {}, "unavailable", "library-migration");
                if (!renderersStopped) {
                    // Keep the request unacknowledged. The App will retain all
                    // files and either observe a later successful retry or
                    // time out fail-closed.
                    runtimeEvents.Wait(settingsEvent.get(), 250);
                    continue;
                }
                if (!libraryMigration.AcknowledgeQuiesced()) {
                    throw std::runtime_error("cannot acknowledge library migration quiescence");
                }

                // A manual-reset event cannot be waited for its reset state.
                // The App also pulses settingsEvent when it releases the
                // request, while this bounded poll keeps cancellation and tray
                // messages responsive if that notification is ever missed.
                while (libraryMigration.Requested()) {
                    runtimeEvents.Wait(settingsEvent.get(), 250);
                    if (runtimeEvents.ExitRequested()) {
                        libraryMigration.ClearAcknowledgements();
                        return 0;
                    }
                }

                motion::Settings migratedSettings;
                if (motion::load_settings_file(configPath, migratedSettings) !=
                    motion::SettingsFileStatus::valid) {
                    libraryMigration.ClearQuiesced();
                    append_agent_log(root,
                        L"媒体库迁移后的设置或所有权校验失败；Agent 保持停用并等待配置恢复。");
                    configurationAvailable = false;
                    allowMissingFirstRun = false;
                    wallpapers.clear();
                    mediaLibraryTrust.reset();
                    missingMediaSince.reset();
                    selectionInitialized = false;
                    reload = true;
                    continue;
                }

                try {
                    auto migratedWallpapers = motion::wallpaper_library_directory(
                        root, migratedSettings.mediaLibraryPath);
                    auto migratedTrust = trusted_custom_library(
                        migratedSettings, migratedWallpapers);
                    auto migratedOptimizer = std::make_unique<motion::agent::VideoOptimizer>(
                        migratedWallpapers, root, applicationRoot, migratedTrust);
                    settings = std::move(migratedSettings);
                    wallpapers = std::move(migratedWallpapers);
                    mediaLibraryTrust = std::move(migratedTrust);
                    videoOptimizer = std::move(migratedOptimizer);
                    renderers.ResetAutoDecodeFailures();
                    configurationAvailable = true;
                    allowMissingFirstRun = false;
                } catch (...) {
                    libraryMigration.ClearQuiesced();
                    configurationAvailable = false;
                    allowMissingFirstRun = false;
                    wallpapers.clear();
                    mediaLibraryTrust.reset();
                    append_agent_log(root,
                        L"媒体库迁移后的目标无法安全启用；Agent 保持停用并等待配置恢复。");
                    reload = true;
                    continue;
                }
                nextOptimizationRequestScan = std::chrono::steady_clock::time_point::min();
                nextFallbackReload = std::chrono::steady_clock::now() + 2s;
                configWriteTimeError.clear();
                configWriteTime = fs::last_write_time(configPath, configWriteTimeError);
                configWriteTimeKnown = !configWriteTimeError;
                missingMediaSince.reset();
                selectionInitialized = false;
                libraryMigration.ClearQuiesced();
                if (!libraryMigration.AcknowledgeApplied()) {
                    throw std::runtime_error("cannot acknowledge migrated library settings");
                }
                reload = false;
                continue;
            }

            auto now = std::chrono::steady_clock::now();
            runtimeEvents.PollSessionState(now);
            if (reload) nextOptimizationRequestScan = std::chrono::steady_clock::time_point::min();
            if (reload || now >= nextFallbackReload) {
                std::error_code currentWriteTimeError;
                auto currentWriteTime = fs::last_write_time(configPath, currentWriteTimeError);
                bool currentWriteTimeKnown = !currentWriteTimeError;
                bool shouldReloadConfiguration = reload || !configurationAvailable ||
                    currentWriteTimeKnown != configWriteTimeKnown ||
                    (currentWriteTimeKnown && currentWriteTime != configWriteTime);
                if (shouldReloadConfiguration) {
                    motion::Settings candidateSettings;
                    auto status = motion::load_settings_file(configPath, candidateSettings);
                    bool usable = status == motion::SettingsFileStatus::valid ||
                        (status == motion::SettingsFileStatus::missing && allowMissingFirstRun);
                    try {
                        if (usable) {
                            auto configuredWallpapers = motion::wallpaper_library_directory(
                                root, candidateSettings.mediaLibraryPath);
                            auto configuredTrust = trusted_custom_library(
                                candidateSettings, configuredWallpapers);
                            if (!configurationAvailable || !videoOptimizer ||
                                !motion::same_filesystem_path(configuredWallpapers, wallpapers) ||
                                !same_library_trust(configuredTrust, mediaLibraryTrust)) {
                                renderers.Stop();
                                renderers.ResetAutoDecodeFailures();
                                // Join every worker that can still write the old
                                // cache before a replacement optimizer starts.
                                videoOptimizer.reset();
                                importedRequests.clear();
                                wallpapers.clear();
                                auto configuredOptimizer = std::make_unique<motion::agent::VideoOptimizer>(
                                    configuredWallpapers, root, applicationRoot, configuredTrust);
                                videoOptimizer = std::move(configuredOptimizer);
                                wallpapers = std::move(configuredWallpapers);
                                nextOptimizationRequestScan = std::chrono::steady_clock::time_point::min();
                                missingMediaSince.reset();
                                selectionInitialized = false;
                            }
                            mediaLibraryTrust = std::move(configuredTrust);
                            settings = std::move(candidateSettings);
                            configurationAvailable = true;
                            if (status == motion::SettingsFileStatus::valid) allowMissingFirstRun = false;
                            configFailureReported = false;
                        }
                    } catch (...) {
                        usable = false;
                    }
                    if (!usable) {
                        if (configurationAvailable || videoOptimizer) {
                            renderers.Stop();
                        }
                        renderers.ResetAutoDecodeFailures();
                        videoOptimizer.reset();
                        importedRequests.clear();
                        wallpapers.clear();
                        mediaLibraryTrust.reset();
                        missingMediaSince.reset();
                        selectionInitialized = false;
                        configurationAvailable = false;
                        allowMissingFirstRun = false;
                        if (!configFailureReported) {
                            append_agent_log(root,
                                L"设置文件损坏、版本不受支持或媒体库所有权校验失败；Agent 已停用并等待配置恢复。");
                            configFailureReported = true;
                        }
                    }
                    currentWriteTimeError.clear();
                    currentWriteTime = fs::last_write_time(configPath, currentWriteTimeError);
                    currentWriteTimeKnown = !currentWriteTimeError;
                    configWriteTime = currentWriteTime;
                    configWriteTimeKnown = currentWriteTimeKnown;
                }
                reload = false;
                nextFallbackReload = now + 2s;
            }
            std::shared_ptr<motion::MediaLibraryTrustLease> loopLibraryTrust;
            if (configurationAvailable && !settings.mediaLibraryPath.empty()) {
                loopLibraryTrust = mediaLibraryTrust
                    ? motion::acquire_media_library_trust(*mediaLibraryTrust)
                    : std::shared_ptr<motion::MediaLibraryTrustLease>{};
                if (!loopLibraryTrust) {
                    // Stop and join every consumer before any later path-based
                    // access can reach a replacement drive or directory.
                    renderers.Stop();
                    renderers.ResetAutoDecodeFailures();
                    videoOptimizer.reset();
                    importedRequests.clear();
                    wallpapers.clear();
                    mediaLibraryTrust.reset();
                    missingMediaSince.reset();
                    selectionInitialized = false;
                    configurationAvailable = false;
                    allowMissingFirstRun = false;
                    nextFallbackReload = now;
                    if (!configFailureReported) {
                        append_agent_log(root,
                            L"自定义媒体库的磁盘、目录身份或所有权标记已变化；Agent 已停止并等待完整校验恢复。");
                        configFailureReported = true;
                    }
                }
            }
            if (!configurationAvailable || !videoOptimizer) {
                renderers.Stop();
                publishRuntime({}, {}, "unavailable", "settings-unavailable");
                reload = runtimeEvents.Wait(settingsEvent.get(), 1000);
                continue;
            }
            if (now >= nextOptimizationRequestScan) {
                importedRequests = imported_optimization_requests(wallpapers);
                nextOptimizationRequestScan = now + (importedRequests.empty() ? 10s : 1s);
            }
            auto prepareImported = [&] {
                if (importedRequests.empty()) return;
                auto target = optimization_target_size(settings);
                for (auto const& request : importedRequests) {
                    videoOptimizer->Prepare(request.media.path, request.mode,
                        target.width, target.height, target.refreshRateHz);
                }
            };

            bool locked = runtimeEvents.Locked();
            auto input = input_state();
            auto uptime = std::chrono::milliseconds(GetTickCount64());
            auto inhibition = idleInhibitor.State(now);
            auto screensaverIdle = screensaverIdleTimer.Update(uptime, input.tick, input.idle,
                motion::agent::screensaver_idle_is_inhibited(inhibition.display, screensaverWasActive));
            DWORD waitMilliseconds = motion::agent::stable_wait_ms;
            if (runtimeEvents.TopologyRevision() != topologyRevision) {
                topologyRevision = runtimeEvents.TopologyRevision();
                renderers.TopologyChanged();
                bool available = physical_video_device_available();
                physicalVideoDeviceAvailable = available;
                // Adapter membership and per-codec decode profiles can change
                // even while at least one physical video device remains. Drop
                // both target choices and the source-capability cache for every
                // topology revision.
                videoOptimizer->InvalidateChoices();
                automaticCompatibilityPriority = false;
            }
            auto sessionAction = motion::agent::reduce_runtime_action(settings,
                { runtimeEvents.DisplayOn(), locked, false, false, 0 });
            if (sessionAction == motion::agent::RuntimeAction::DisplayOff) {
                screensaverWasActive = false;
                // The display-off contract wins over background preparation:
                // stop hardware work now and resume the durable request later.
                videoOptimizer->SetGenerationAllowed(false);
                renderers.Stop();
                waitMilliseconds = 1000;
            } else if (sessionAction == motion::agent::RuntimeAction::Locked) {
                screensaverWasActive = false;
                videoOptimizer->SetGenerationAllowed(false);
                renderers.Stop();
                if (!wasLocked) {
                    wasLocked = true;
                    lockObservedAt = now;
                    displayOffAfterLockTriggered = false;
                    nextDisplayOffAfterLockAttempt = std::chrono::steady_clock::time_point::min();
                }
                if (motion::agent::display_off_after_lock_is_due(
                    settings.displayOffAfterLockEnabled,
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - lockObservedAt),
                    settings.displayOffAfterLockDelaySeconds,
                    displayOffAfterLockTriggered,
                    now >= nextDisplayOffAfterLockAttempt)) {
                    displayOffAfterLockTriggered = request_display_off();
                    if (!displayOffAfterLockTriggered) {
                        nextDisplayOffAfterLockAttempt = now + 5s;
                        append_agent_log(root, L"锁屏后熄屏请求失败，5 秒后重试，错误码 " +
                            std::to_wstring(GetLastError()) + L"。");
                    }
                }
                waitMilliseconds = 1000;
            } else {
                bool softwarePlayback = motion::agent::uses_software_playback(
                    settings.decodeMode, physicalVideoDeviceAvailable);
                if (settings.decodeMode != "auto") automaticCompatibilityPriority = false;
                std::string playbackPerformanceMode = softwarePlayback
                    ? std::string(motion::agent::cpu_smooth_mode) : settings.performanceMode;
                bool randomGroupChanged = settings.randomGroupId != previousRandomGroup;
                if (randomGroupChanged) previousRandomGroup = settings.randomGroupId;
                bool selectionChanged = selectionInitialized &&
                    (settings.selectedGroupId != previousSelectedGroup || settings.selectedMediaId != previousSelectedMedia);
                previousSelectedGroup = settings.selectedGroupId;
                previousSelectedMedia = settings.selectedMediaId;
                selectionInitialized = true;
                bool performanceModeChanged = settings.performanceMode != previousPerformanceMode;
                previousPerformanceMode = settings.performanceMode;
                if (selectionChanged || performanceModeChanged) {
                    videoOptimizer->InvalidateChoices();
                    renderers.ResetAutoDecodeFailures();
                    automaticCompatibilityPriority = false;
                }
                bool randomIntervalChanged = settings.randomIntervalMinutes != previousRandomInterval;
                if (randomIntervalChanged) {
                    previousRandomInterval = settings.randomIntervalMinutes;
                    nextRandomChange = settings.randomIntervalMinutes > 0
                        ? now + std::chrono::minutes(settings.randomIntervalMinutes)
                        : std::chrono::steady_clock::time_point::max();
                }
                bool timedChange = settings.randomIntervalMinutes > 0 && now >= nextRandomChange &&
                    screensaverIdle < std::chrono::seconds(settings.idleTimeoutSeconds);
                bool randomActive = motion::valid_id(settings.randomGroupId) &&
                    settings.randomGroupId == settings.selectedGroupId;
                bool currentRandomIsValid = randomActive && !randomId.empty() &&
                    !media_by_id(wallpapers, settings.randomGroupId, randomId, playbackPerformanceMode).path.empty();
                auto randomAction = motion::agent::random_selection_action(
                    randomActive, randomGroupChanged, selectionChanged, wasLocked, timedChange, currentRandomIsValid);
                if (randomAction == motion::agent::RandomSelectionAction::ChooseRandom) {
                    randomId = random_media_id(
                        wallpapers, settings.randomGroupId, randomId, playbackPerformanceMode);
                    nextRandomChange = settings.randomIntervalMinutes > 0
                        ? now + std::chrono::minutes(settings.randomIntervalMinutes)
                        : std::chrono::steady_clock::time_point::max();
                } else if (randomAction == motion::agent::RandomSelectionAction::UseSelected) {
                    randomId = randomActive ? settings.selectedMediaId : std::string{};
                    nextRandomChange = settings.randomIntervalMinutes > 0
                        ? now + std::chrono::minutes(settings.randomIntervalMinutes)
                        : std::chrono::steady_clock::time_point::max();
                }
                if (wasLocked) {
                    wasLocked = false;
                    autoLockTriggered = false;
                    displayOffAfterLockTriggered = false;
                    lockObservedAt = std::chrono::steady_clock::time_point::max();
                }

                auto groupId = randomId.empty() ? settings.selectedGroupId : settings.randomGroupId;
                auto mediaId = randomId.empty() ? settings.selectedMediaId : randomId;
                auto outputs = display_media_targets(
                    wallpapers, settings, groupId, mediaId, playbackPerformanceMode);
                if (softwarePlayback) {
                    for (auto& output : outputs) {
                        output.softwarePlaybackTarget = output.media.kind == "video";
                        if (output.softwarePlaybackTarget) {
                            output.playbackFrameRateCap =
                                motion::agent::software_playback_profile(true,
                                    logicalProcessors, output.targetWidth,
                                    output.targetHeight, output.targetRefreshRate).frameRate;
                        }
                    }
                }
                auto assignDecodeAdapters = [&](auto& targets) {
                        std::vector<size_t> routesWithoutHardwareDecode;
                        std::vector<motion::agent::RendererRoute> probeRoutes;
                        std::vector<std::wstring> preferredAdapters(targets.size());
                        probeRoutes.reserve(targets.size());
                        for (size_t index = 0; index < targets.size(); ++index) {
                            auto& output = targets[index];
                            if (output.media.kind != "video") continue;
                            preferredAdapters[index] = display_adapter_key(output.deviceName);
                            auto routeAdapter = motion::agent::renderer_adapter_key(
                                preferredAdapters[index], output.deviceName);
                            probeRoutes.push_back({
                                motion::agent::renderer_media_key(output.media.path, output.media.kind),
                                output.deviceName, std::move(routeAdapter),
                                static_cast<uint64_t>(output.targetWidth) * output.targetHeight });
                        }
                        auto groupedProbeRoutes = motion::agent::group_renderer_routes(probeRoutes, false);
                        for (size_t index = 0; index < targets.size(); ++index) {
                            auto& output = targets[index];
                            if (output.media.kind != "video") continue;
                            auto mediaKey = motion::agent::renderer_media_key(
                                output.media.path, output.media.kind);
                            auto routeAdapter = motion::agent::renderer_adapter_key(
                                preferredAdapters[index], output.deviceName);
                            auto route = std::find_if(groupedProbeRoutes.begin(), groupedProbeRoutes.end(),
                                [&](auto const& candidate) {
                                    return candidate.mediaKey == mediaKey &&
                                        candidate.adapterKey == routeAdapter;
                                });
                            auto aggregateOutputPixels = route == groupedProbeRoutes.end()
                                ? static_cast<uint64_t>(output.targetWidth) * output.targetHeight
                                : route->aggregateOutputPixels;
                            output.decodeAdapter = videoOptimizer->SourceHardwareDecodeAdapter(
                                output.media.path, preferredAdapters[index], aggregateOutputPixels);
                            if (renderers.AutoDecodeRouteRejected(
                                    output.media.path, output.decodeAdapter)) {
                                output.decodeAdapter.clear();
                            }
                            if (output.decodeAdapter.empty()) {
                                routesWithoutHardwareDecode.push_back(index);
                            }
                        }
                        return routesWithoutHardwareDecode;
                };
                bool selectedMediaTemporarilyMissing = outputs.empty() &&
                    motion::valid_id(groupId) && motion::valid_id(mediaId) && renderers.HasActiveRoute();
                if (selectedMediaTemporarilyMissing && !missingMediaSince) missingMediaSince = now;
                bool holdExistingRenderer = selectedMediaTemporarilyMissing && missingMediaSince &&
                    now - *missingMediaSince < 2s;
                if (!outputs.empty() || !selectedMediaTemporarilyMissing) missingMediaSince.reset();
                auto state = motion::agent::reduce_runtime_action(settings,
                    { true, false, desktop_covered(), !outputs.empty(), screensaverIdle.count() / 1000 });
                bool ownScreensaverActive = screensaverWasActive ||
                    state == motion::agent::RuntimeAction::ScreensaverPlay;
                auto autoLockIdle = autoLockIdleTimer.Update(uptime, input.tick, input.idle,
                    motion::agent::automatic_lock_idle_is_inhibited(
                        inhibition.display, inhibition.system, ownScreensaverActive));
                screensaverWasActive = state == motion::agent::RuntimeAction::ScreensaverPlay;
                // Explicit/import requests are durable and run at background
                // priority. Power saver is also allowed to prepare its selected
                // display-sized stream while the source remains visible. The
                // selected tier is never changed implicitly by power state.
                bool powerSaverNeedsPriority = settings.performanceMode == "power-saver" ||
                    softwarePlayback || automaticCompatibilityPriority;
                bool playbackIdle = state != motion::agent::RuntimeAction::DesktopPlay &&
                    state != motion::agent::RuntimeAction::ScreensaverPlay;
                bool onBattery = runtimeEvents.OnBattery();
                bool selectedTierGenerationAllowed = motion::agent::variant_generation_allowed(
                    onBattery, settings.performanceMode == "power-saver" ||
                        !importedRequests.empty(), playbackIdle);
                videoOptimizer->SetGenerationAllowed(motion::agent::variant_generation_allowed(
                    onBattery, powerSaverNeedsPriority || !importedRequests.empty(), playbackIdle));
                prepareImported();
                auto resolveVideoOutputs = [&](bool onlySoftwareTargets = false) {
                    using ResolveKey = std::pair<std::wstring, bool>;
                    std::map<ResolveKey, OptimizationTarget> requiredVideoTargets;
                    for (auto const& output : outputs) {
                        if (output.media.kind != "video" || !output.media.sourceBacked) continue;
                        if (onlySoftwareTargets && !output.softwarePlaybackTarget) continue;
                        auto key = ResolveKey{ output.media.path.wstring(),
                            output.softwarePlaybackTarget };
                        auto& required = requiredVideoTargets[key];
                        auto profile = motion::agent::software_playback_profile(
                            output.softwarePlaybackTarget,
                            logicalProcessors, output.targetWidth, output.targetHeight,
                            output.targetRefreshRate);
                        required.width = (std::max)(required.width,
                            profile.enabled ? profile.width : output.targetWidth);
                        required.height = (std::max)(required.height,
                            profile.enabled ? profile.height : output.targetHeight);
                        required.refreshRateHz = (std::max)(required.refreshRateHz,
                            profile.enabled ? profile.frameRate : output.targetRefreshRate);
                    }
                    bool waiting{};
                    for (auto& output : outputs) {
                        if (output.media.kind != "video" || !output.media.sourceBacked) continue;
                        if (onlySoftwareTargets && !output.softwarePlaybackTarget) continue;
                        auto source = output.media.path;
                        auto const required = requiredVideoTargets[{ source.wstring(),
                            output.softwarePlaybackTarget }];
                        auto resolved = videoOptimizer->ResolveWithLease(source, settings.performanceMode,
                            required.width, required.height, required.refreshRateHz,
                            output.softwarePlaybackTarget,
                            output.softwarePlaybackTarget || selectedTierGenerationAllowed);
                        output.media.path = std::move(resolved.path);
                        output.media.playbackLease = std::move(resolved.lease);
                        output.media.sourceBacked = output.media.path == source;
                        if (state == motion::agent::RuntimeAction::DesktopPlay &&
                            motion::agent::active_playback_waits_for_performance_copy(
                                settings.performanceMode, resolved.performanceCopyPending,
                                output.media.sourceBacked)) {
                            waiting = true;
                        }
                    }
                    for (auto& output : outputs) {
                        // External-library identity must remain pinned for every
                        // concrete path handed to Renderer, including images.
                        // Video variants additionally use this token as their
                        // cache-deletion lease.
                        if (!output.media.playbackLease) {
                            output.media.playbackLease = videoOptimizer->AcquirePlaybackLease(
                                output.media.path);
                        }
                    }
                    return waiting;
                };
                bool waitingForPerformanceCopy = resolveVideoOutputs();

                if (settings.decodeMode == "auto") {
                    // Resolve can switch codecs/profiles (for example H.264
                    // source -> HEVC Main10 balanced copy). Probe the exact
                    // file that Renderer will open, never the logical source's
                    // capabilities. Runtime driver failures are keyed to this
                    // same concrete path and adapter.
                    auto unsupportedRoutes = assignDecodeAdapters(outputs);
                    if (!softwarePlayback && !unsupportedRoutes.empty()) {
                        automaticCompatibilityPriority = true;
                        auto cpuOutputs = display_media_targets(wallpapers, settings,
                            groupId, mediaId, std::string(motion::agent::cpu_smooth_mode));
                        for (auto index : unsupportedRoutes) {
                            if (index >= outputs.size()) continue;
                            auto& output = outputs[index];
                            auto fallback = std::find_if(cpuOutputs.begin(), cpuOutputs.end(),
                                [&](auto const& candidate) {
                                    return candidate.deviceName == output.deviceName &&
                                        candidate.groupId == output.groupId &&
                                        candidate.mediaId == output.mediaId;
                                });
                            if (fallback != cpuOutputs.end()) {
                                output.media = std::move(fallback->media);
                            }
                            output.softwarePlaybackTarget = true;
                            output.playbackFrameRateCap =
                                motion::agent::software_playback_profile(true,
                                    logicalProcessors, output.targetWidth,
                                    output.targetHeight, output.targetRefreshRate).frameRate;
                        }
                        powerSaverNeedsPriority = true;
                        videoOptimizer->SetGenerationAllowed(motion::agent::variant_generation_allowed(
                            onBattery, true, playbackIdle));
                        waitingForPerformanceCopy = resolveVideoOutputs(true) ||
                            waitingForPerformanceCopy;
                        // The CPU-friendly file may still be decoded and
                        // composed by a capable physical GPU. If it is not yet
                        // ready or no adapter supports it, automatic playback
                        // retains its CPU-decoder fallback.
                        assignDecodeAdapters(outputs);
                    } else if (!softwarePlayback) {
                        automaticCompatibilityPriority = false;
                    }
                }

                if (settings.autoLockEnabled &&
                    autoLockIdle >= std::chrono::seconds(settings.autoLockTimeoutSeconds)) {
                    if (!autoLockTriggered && now >= nextAutoLockAttempt) {
                        // Stop decoding before requesting the secure desktop.
                        // Display power-off is independently scheduled only
                        // after WTS confirms that Windows is locked.
                        renderers.Stop();
                        if (LockWorkStation()) {
                            autoLockTriggered = true;
                            screensaverWasActive = false;
                        } else {
                            nextAutoLockAttempt = now + 5s;
                            append_agent_log(root, L"自动锁屏请求失败，5 秒后重试，错误码 " +
                                std::to_wstring(GetLastError()) + L"。");
                        }
                    }
                } else {
                    autoLockTriggered = false;
                    nextAutoLockAttempt = std::chrono::steady_clock::time_point::min();
                }

                bool targetReady{};
                if (autoLockTriggered) {
                    waitMilliseconds = 1000;
                } else if (holdExistingRenderer) {
                    // Import/move publishes metadata and files in separate atomic
                    // steps. Preserve the last fully presented frame during that
                    // short transaction instead of exposing the Windows wallpaper.
                    waitMilliseconds = 50;
                } else if (waitingForPerformanceCopy) {
                    // Honor the selected tier without exposing the Windows
                    // wallpaper or continuing an expensive source stream. Keep
                    // the old route's exact last frame until every requested
                    // copy is ready; RendererPool will retire stale routes only
                    // after the replacement route receives its first-frame ACK.
                    renderers.Freeze();
                    targetReady = renderers.TargetReady();
                    waitMilliseconds = motion::agent::performance_copy_wait_interval_ms(
                        targetReady || !renderers.TransitionPending());
                } else {
                    switch (state) {
                    case motion::agent::RuntimeAction::ScreensaverPlay:
                        renderers.Apply(Renderer::Target::ScreensaverPlay, outputs, settings.decodeMode,
                            settings.displayMode == "primary");
                        targetReady = renderers.TargetReady();
                        waitMilliseconds = motion::agent::runtime_wait_interval_ms(
                            targetReady || !renderers.TransitionPending());
                        break;
                    case motion::agent::RuntimeAction::DesktopPlay:
                        renderers.Apply(Renderer::Target::DesktopPlay, outputs, settings.decodeMode,
                            settings.displayMode == "primary");
                        targetReady = renderers.TargetReady();
                        waitMilliseconds = motion::agent::runtime_wait_interval_ms(
                            targetReady || !renderers.TransitionPending());
                        break;
                    case motion::agent::RuntimeAction::DesktopFrozen:
                        renderers.Apply(Renderer::Target::DesktopFreeze, outputs, settings.decodeMode,
                            settings.displayMode == "primary");
                        targetReady = renderers.TargetReady();
                        waitMilliseconds = motion::agent::runtime_wait_interval_ms(
                            targetReady || !renderers.TransitionPending());
                        break;
                    case motion::agent::RuntimeAction::DesktopPaused:
                        renderers.Pause();
                        targetReady = renderers.TargetReady();
                        waitMilliseconds = motion::agent::runtime_wait_interval_ms(
                            targetReady || !renderers.TransitionPending());
                        break;
                    default:
                        renderers.Stop();
                        publishRuntime({}, {});
                        break;
                    }
                }
                if (motion::agent::runtime_selection_can_publish(
                        targetReady, waitingForPerformanceCopy)) {
                    auto decode = renderers.DecodeState();
                    if (!groupId.empty() && !mediaId.empty()) publishRuntime(groupId, mediaId, decode.path, decode.reason);
                    else if (!outputs.empty()) publishRuntime(outputs.front().groupId, outputs.front().mediaId, decode.path, decode.reason);
                }
            }
            auto decode = renderers.DecodeState();
            publishRuntime(publishedGroupId, publishedMediaId, decode.path, decode.reason);
            reload = runtimeEvents.Wait(settingsEvent.get(), waitMilliseconds);
        }
    } catch (std::exception const& error) {
        if (!legacyDataConflict && !motion::legacy_data_conflict_present(applicationRoot)) {
            try { append_agent_log(root, L"Agent 遇到致命错误: " + motion::utf8_to_wide(error.what())); } catch (...) {}
        }
        return 1;
    } catch (...) {
        if (!legacyDataConflict && !motion::legacy_data_conflict_present(applicationRoot)) {
            append_agent_log(root, L"Agent 遇到未知致命错误。");
        }
        return 1;
    }
}
