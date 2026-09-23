#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <hidusage.h>
#include <powrprof.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include <wrl.h>

#include "../MotionWallpaper.Common/Common.h"
#include "../MotionWallpaper.Common/DisplayAwareness.h"
#include "../MotionWallpaper.Common/DisplayTopology.h"
#include "../MotionWallpaper.Common/SceneProfiles.h"
#include "../MotionWallpaper.Common/StartupEnvironment.h"
#include "../MotionWallpaper.Common/VariantCache.h"
#include "CoveragePolicy.h"
#include "IdlePolicy.h"
#include "LibraryMigrationProtocol.h"
#include "PlaybackCapabilityPolicy.h"
#include "RandomSelectionPolicy.h"
#include "resource.h"
#include "RuntimePolicy.h"
#include "RuntimeEventPolicy.h"
#include "RuntimeStatusPolicy.h"
#include "SharedRendererPolicy.h"
#include "RendererLifecyclePolicy.h"
#include "TrayControlPolicy.h"
#include "VideoOptimizer.h"
#include "VideoVariantPolicy.h"
#include "VideoTranscoder.h"
#include "../MotionWallpaper.Protocol/RendererProtocol.h"
#include "../MotionWallpaper.Protocol/BoundedPipeReader.h"

#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    struct IterationSettingsRestore
    {
        motion::Settings& target;
        motion::Settings configured;
        ~IterationSettingsRestore() noexcept { target = std::move(configured); }
    };

    std::wstring display_adapter_key(std::wstring const& displayDevice)
    {
        auto inventory = motion::agent::video_gpu_inventory_async();
        auto found = inventory.displayAdapters.find(displayDevice);
        return found == inventory.displayAdapters.end() ? std::wstring{} : found->second;
    }

    bool physical_video_device_available()
    {
        auto inventory = motion::agent::video_gpu_inventory_async();
        // Pending is not proof that the machine needs CPU compatibility.
        return inventory.pending || inventory.physicalVideoDeviceAvailable;
    }

    bool cloned_or_projected_display_active() noexcept
    {
        UINT32 pathCount{};
        UINT32 modeCount{};
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,
                &pathCount, &modeCount) != ERROR_SUCCESS || pathCount < 2) {
            return false;
        }
        try {
            std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
            std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
            if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount,
                    paths.data(), &modeCount, modes.data(), nullptr) !=
                ERROR_SUCCESS) return false;
            paths.resize(pathCount);
            for (size_t first = 0; first < paths.size(); ++first) {
                if ((paths[first].flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) continue;
                for (size_t second = first + 1; second < paths.size(); ++second) {
                    if ((paths[second].flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) continue;
                    auto const& left = paths[first].sourceInfo;
                    auto const& right = paths[second].sourceInfo;
                    if (left.id == right.id &&
                        left.adapterId.HighPart == right.adapterId.HighPart &&
                        left.adapterId.LowPart == right.adapterId.LowPart) {
                        return true;
                    }
                }
            }
        } catch (...) {}
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
        if (performanceMode == "original") {
            // Missing originals are visible playback failures. Never replace
            // an explicit original choice with a retained performance copy.
            return MediaSelection{ path, "video", mediaId, true };
        }
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

    MediaSelection media_poster_by_id(fs::path const& wallpapers,
        std::string const& groupId, std::string const& mediaId,
        motion::agent::VideoOptimizer& optimizer)
    {
        if (!motion::valid_id(groupId) || !motion::valid_id(mediaId)) return {};
        auto directory = wallpapers / L"Groups" / motion::utf8_to_wide(groupId) /
            L"Videos" / motion::utf8_to_wide(mediaId);
        motion::MediaMetadata metadata;
        if (!motion::try_load_media(directory / L"metadata.json", metadata) ||
            metadata.id != mediaId || metadata.groupId != groupId) return {};
        if (metadata.kind == "video") {
            // Existing imports upgrade lazily too. When the user has removed
            // the original, the best retained copy can still supply a frame.
            auto video = media_by_id(wallpapers, groupId, mediaId, "balanced");
            auto preview = optimizer.ResolveStillPreview(video.path);
            if (!preview.path.empty()) {
                return { std::move(preview.path), "image", mediaId, false, std::move(preview.lease) };
            }
        }
        if (metadata.coverFileName.empty() || !motion::safe_file_name(metadata.coverFileName)) return {};
        auto poster = directory / metadata.coverFileName;
        std::error_code error;
        if (!fs::is_regular_file(poster, error) || error) return {};
        auto lease = optimizer.AcquirePlaybackLease(poster);
        return { std::move(poster), "image", mediaId, false, std::move(lease) };
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

    enum class PerformancePreviewStage
    {
        Inactive,
        FreezePrevious,
        PresentPreview,
        Ready
    };

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

    std::string next_media_id(fs::path const& wallpapers, std::string const& groupId,
        std::string const& current, std::string const& performanceMode)
    {
        return motion::agent::next_media_id(
            group_media_ids(wallpapers, groupId, performanceMode), current);
    }

    std::string display_assignments_key(
        std::vector<motion::DisplayAssignment> const& assignments)
    {
        std::string result;
        for (auto const& assignment : assignments) {
            for (auto const* value : { &assignment.displayId, &assignment.groupId,
                    &assignment.mediaId }) {
                result += std::to_string(value->size());
                result.push_back(':');
                result += *value;
            }
        }
        return result;
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

        struct Snapshot
        {
            bool running{};
            bool targetReady{};
            bool transitionPending{};
            bool failed{};
            uint32_t processId{};
            motion::protocol::DecodeStatus decode;
            std::string errorDetail;
            bool firstFrameSeen{};
        };

        explicit Renderer(fs::path executable) : executable_(std::move(executable)) {}
        ~Renderer()
        {
            if (!Stop()) ForceCleanupForDestruction();
        }

        bool Apply(Target target, MediaSelection const& media, std::string const& decodeMode,
            std::string const& displayMode, uint32_t frameRateCap,
            std::vector<std::wstring> const& monitorDevices = {},
            std::wstring const& decodeAdapter = {}, bool originalPlayback = false)
        {
            auto now = std::chrono::steady_clock::now();
            std::wstring requestKey = media.path.wstring() + L"\n" + motion::utf8_to_wide(media.kind) + L"\n" +
                motion::utf8_to_wide(decodeMode) + L"\n" + motion::utf8_to_wide(displayMode) + L"\n" +
                std::to_wstring(frameRateCap) + L"\n" + decodeAdapter;
            for (auto const& monitor : monitorDevices) requestKey += L"\n" + monitor;
            bool configurationChanged = requestKey != requestedKey_;
            if (configurationChanged && process_ && !Stop()) return false;
            if (configurationChanged) {
                recoveryBudget_.Clear();
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
            if (motion::agent::original_first_frame_expired(originalPlayback,
                    media.kind == "video", awaitingAck,
                    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - targetFirstSentAt_).count()))) {
                SetDecodeUnavailable("original-first-frame-timeout");
                Shutdown(false);
                return false;
            }
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
                bool playing = kind_ == "video" &&
                    (target_ == Target::DesktopPlay || target_ == Target::ScreensaverPlay);
                motion::agent::PlaybackHealthFailure healthFailure{};
                {
                    std::scoped_lock lock(healthMutex_);
                    if (playing) healthFailure = health_.Check(GetTickCount64());
                }
                if (healthFailure != motion::agent::PlaybackHealthFailure::None) {
                    auto reason = healthFailure == motion::agent::PlaybackHealthFailure::Heartbeat
                        ? "playback-heartbeat-timeout" : "playback-frame-stalled";
                    {
                        std::scoped_lock lock(decodeStatusMutex_);
                        errorDetail_ = motion::protocol::format_renderer_error(reason, WAIT_TIMEOUT);
                    }
                    bool exhausted = recoveryBudget_.Exhausted(GetTickCount64());
                    FailAndBackOff();
                    if (exhausted) SetDecodeUnavailable(reason);
                    return false;
                }
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

        [[nodiscard]] bool IsStaticImage() const noexcept { return kind_ == "image"; }

        [[nodiscard]] motion::protocol::DecodeStatus DecodeState() const
        {
            std::scoped_lock lock(decodeStatusMutex_);
            return decodeStatus_;
        }

        [[nodiscard]] Snapshot RuntimeSnapshot() const
        {
            Snapshot result;
            result.running = static_cast<bool>(process_);
            result.targetReady = TargetReady();
            result.transitionPending = TransitionPending();
            result.failed = permanentlyUnavailable_.load(std::memory_order_acquire) ||
                (!process_ && failureCount_ != 0);
            // Shutdown(false) preserves the acknowledgement so a later device
            // loss is not mistaken for a decoder that never produced a frame.
            result.firstFrameSeen = targetAcknowledgedRevision_.load() != 0;
            result.processId = process_ ? GetProcessId(process_.get()) : 0;
            std::scoped_lock lock(decodeStatusMutex_);
            result.decode = decodeStatus_;
            result.errorDetail = errorDetail_;
            return result;
        }

        bool RetryFailed()
        {
            if (!permanentlyUnavailable_.load(std::memory_order_acquire) &&
                (process_ || failureCount_ == 0)) return false;
            return Shutdown(true);
        }

        bool Restart()
        {
            return Shutdown(true);
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
                recoveryBudget_.Clear();
                permanentlyUnavailable_.store(false, std::memory_order_release);
                ResetBackOff();
                requestedKey_.clear();
                std::scoped_lock lock(decodeStatusMutex_);
                decodeStatus_ = {};
                errorDetail_.clear();
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
            {
                std::scoped_lock lock(decodeStatusMutex_);
                errorDetail_.clear();
            }
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
                {
                    auto code = GetLastError();
                    std::scoped_lock lock(decodeStatusMutex_);
                    errorDetail_ = motion::protocol::format_renderer_error("create-renderer", code);
                }
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
            if (command == "desktop-play" || command == "screensaver-play" ||
                command == "desktop-freeze" || command == "pause") {
                std::scoped_lock lock(healthMutex_);
                health_.Reset(revision, GetTickCount64());
            }
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
            if (!outputRead_) return;
            motion::protocol::read_bounded_pipe_lines(outputRead_.get(), outputStopping_,
                [&](std::string_view value) {
                    auto ack = motion::protocol::parse_ack(value);
                    if (ack.channel == motion::protocol::AckChannel::Target) {
                        uint64_t previous = targetAcknowledgedRevision_.load();
                        while (previous < ack.revision && !targetAcknowledgedRevision_.compare_exchange_weak(previous, ack.revision)) {}
                    } else if (auto heartbeat = motion::protocol::parse_playback_heartbeat(value)) {
                        std::scoped_lock lock(healthMutex_);
                        health_.Observe(heartbeat->revision, heartbeat->serial, heartbeat->period100ns, GetTickCount64());
                    } else if (auto status = motion::protocol::parse_decode_status(value); !status.path.empty()) {
                        if (status.path == "unavailable") {
                            permanentlyUnavailable_.store(true, std::memory_order_release);
                        }
                        std::scoped_lock lock(decodeStatusMutex_);
                        decodeStatus_ = std::move(status);
                    } else if (value.starts_with("error ")) {
                        std::scoped_lock lock(decodeStatusMutex_);
                        auto error = motion::protocol::parse_renderer_error(value);
                        errorDetail_ = error
                            ? motion::protocol::format_renderer_error(error->operation, error->code)
                            : "renderer-protocol-error";
                        failed_.store(true);
                    }
                });
        }

        void Refresh()
        {
            if (!process_ || WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT) return;
            DWORD exitCode{};
            bool hasExitCode = GetExitCodeProcess(process_.get(), &exitCode) != FALSE;
            // The process is already signaled, so sending "stop" would write
            // to a broken pipe and recursively enter this cleanup path.
            if (!Shutdown(false, true)) return;
            {
                std::scoped_lock lock(decodeStatusMutex_);
                // Drain error messages before recording the process status;
                // the precise HRESULT from Renderer takes precedence.
                if (errorDetail_.empty() && hasExitCode) {
                    errorDetail_ = motion::protocol::format_renderer_error("renderer-exit", exitCode);
                }
            }
            RecordFailure();
        }

        void ResetTransport()
        {
            inputWrite_.reset();
            // The reader still consumes its bounded snapshot of already
            // buffered messages, even if it has not been scheduled before this
            // flag is set. An inherited writer cannot make join wait for EOF.
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
        std::string errorDetail_;
        std::chrono::steady_clock::time_point targetSentAt_{};
        std::chrono::steady_clock::time_point targetFirstSentAt_{};
        std::chrono::steady_clock::time_point launchedAt_{};
        std::chrono::steady_clock::time_point nextLaunchAllowed_{};
        unsigned failureCount_{};
        unsigned automaticStartupTimeouts_{};
        std::mutex healthMutex_;
        motion::agent::PlaybackHealthMonitor health_;
        motion::agent::PlaybackRecoveryBudget recoveryBudget_;
    };

    struct DisplayMediaTarget
    {
        std::string displayId;
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
        bool performanceCopyRequired{};
        bool performanceCopyPending{};
        bool originalPlayback{};
        bool gpuProbePending{};
        bool hasAlternativeDecodeAdapter{};
        motion::DisplayRefreshRate displayRefreshRate{ 60, 1 };
    };

    std::vector<DisplayMediaTarget> display_media_targets(fs::path const& wallpapers, motion::Settings const& settings,
        std::string const& defaultGroupId, std::string const& defaultMediaId,
        std::string const& performanceMode,
        std::vector<motion::DisplayTarget> const& displays)
    {
        auto defaultMedia = media_by_id(wallpapers, defaultGroupId, defaultMediaId, performanceMode);
        if (settings.displayMode == "primary") {
            if (defaultMedia.path.empty()) return {};
            auto primary = std::find_if(displays.begin(), displays.end(), [](auto const& display) { return display.primary; });
            uint32_t width = primary == displays.end() ? 0u : static_cast<uint32_t>(primary->bounds.right - primary->bounds.left);
            uint32_t height = primary == displays.end() ? 0u : static_cast<uint32_t>(primary->bounds.bottom - primary->bounds.top);
            uint32_t refreshRate = primary == displays.end() ? 60u : primary->refreshRateHz;
            DisplayMediaTarget target{ primary == displays.end() ? std::string{} : primary->id,
                primary == displays.end() ? std::wstring{} : primary->deviceName,
                defaultGroupId, defaultMediaId, std::move(defaultMedia), width, height, refreshRate };
            if (primary != displays.end()) target.displayRefreshRate = motion::effective_display_refresh_rate(*primary);
            return { std::move(target) };
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
                    targets.push_back({ display.id, display.deviceName,
                        std::move(groupId), std::move(mediaId), std::move(assigned), width, height, display.refreshRateHz });
                    targets.back().displayRefreshRate = motion::effective_display_refresh_rate(display);
                    continue;
                }
            }
            if (!defaultMedia.path.empty()) {
                targets.push_back({ display.id, display.deviceName,
                    std::move(groupId), std::move(mediaId), defaultMedia, width, height, display.refreshRateHz });
                targets.back().displayRefreshRate = motion::effective_display_refresh_rate(display);
            }
        }
        return targets;
    }

    OptimizationTarget optimization_target_size(
        std::vector<DisplayMediaTarget> const& targets, std::string const& mediaId,
        std::vector<motion::DisplayTarget> const& displays)
    {
        OptimizationTarget result;
        for (auto const& output : targets) {
            if (output.mediaId != mediaId) continue;
            // A pre-generated copy belongs to a real viewport, never to a
            // synthetic square made from unrelated landscape/portrait maxima.
            if (result.width && motion::agent::video_display_aspect(result.width, result.height) !=
                motion::agent::video_display_aspect(output.targetWidth, output.targetHeight)) continue;
            result.width = (std::max)(result.width, output.targetWidth);
            result.height = (std::max)(result.height, output.targetHeight);
            result.refreshRateHz = (std::max)(result.refreshRateHz, output.targetRefreshRate);
        }
        if (result.width && result.height) return result;
        // A library item with no assigned output is prepared for the primary
        // display, rather than unrelated dimensions/Hz from every monitor.
        auto primary = std::find_if(displays.begin(), displays.end(),
            [](auto const& display) { return display.primary; });
        if (primary == displays.end()) return {};
        return { static_cast<uint32_t>(primary->bounds.right - primary->bounds.left),
            static_cast<uint32_t>(primary->bounds.bottom - primary->bounds.top), primary->refreshRateHz };
    }

    class RendererPool
    {
    public:
        explicit RendererPool(fs::path executable) : executable_(std::move(executable)) {}

        [[nodiscard]] bool SourcePresentationNeedsOptimizerIdle(
            Renderer::Target target,
            std::vector<DisplayMediaTarget> const& outputs,
            std::string const& decodeMode, bool primaryOnly,
            bool freezePerformanceCopies = false) const
        {
            bool hasSourceVideo = std::any_of(outputs.begin(), outputs.end(),
                [](auto const& output) {
                    return output.media.kind == "video" &&
                        output.media.sourceBacked;
                });
            if (!hasSourceVideo) return false;
            return PresentationKey(target, outputs, decodeMode, primaryOnly,
                       freezePerformanceCopies) != acknowledgedPresentationKey_ ||
                !TargetReady();
        }

        void Apply(Renderer::Target target, std::vector<DisplayMediaTarget> const& outputs,
            std::string const& decodeMode, bool primaryOnly,
            bool freezePerformanceCopies = false)
        {
            auto presentationKey = PresentationKey(target, outputs, decodeMode,
                primaryOnly, freezePerformanceCopies);
            std::vector<motion::agent::RendererRoute> routes;
            routes.reserve(outputs.size());
            for (auto const& output : outputs) {
                auto adapterKey = primaryOnly ? std::wstring{} :
                    motion::agent::renderer_adapter_key(
                        display_adapter_key(output.deviceName), output.deviceName);
                if (!primaryOnly) {
                    adapterKey = motion::agent::renderer_preview_adapter_key(
                        std::move(adapterKey), freezePerformanceCopies &&
                            motion::agent::performance_copy_preview_required(
                                motion::agent::RuntimeAction::DesktopPlay,
                                output.performanceCopyRequired));
                }
                routes.push_back({ motion::agent::renderer_media_key(output.media.path, output.media.kind),
                    output.deviceName, std::move(adapterKey),
                    static_cast<uint64_t>(output.targetWidth) * output.targetHeight, output.displayRefreshRate });
            }
            auto grouped = motion::agent::group_renderer_routes(routes, !primaryOnly);
            std::vector<std::wstring> desiredKeys;
            std::map<std::string, std::wstring> desiredDisplayKeys;
            desiredKeys.reserve(grouped.size());
            bool allReady = !grouped.empty();
            std::set<std::wstring> readyKeys;
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
                bool freezeRoute = freezePerformanceCopies &&
                    std::any_of(outputs.begin(), outputs.end(), [&](auto const& candidate) {
                        if (!motion::agent::performance_copy_preview_required(
                                motion::agent::RuntimeAction::DesktopPlay,
                                candidate.performanceCopyRequired) ||
                            motion::agent::renderer_media_key(candidate.media.path,
                                candidate.media.kind) != route.mediaKey) return false;
                        return primaryOnly || route.monitorDevices.empty() ||
                            std::find(route.monitorDevices.begin(), route.monitorDevices.end(),
                                candidate.deviceName) != route.monitorDevices.end();
                    });
                for (auto const& candidate : outputs) {
                    if (motion::agent::renderer_media_key(candidate.media.path,
                            candidate.media.kind) != route.mediaKey) continue;
                    if (!primaryOnly && !route.monitorDevices.empty() &&
                        std::find(route.monitorDevices.begin(), route.monitorDevices.end(),
                            candidate.deviceName) == route.monitorDevices.end()) continue;
                    if (!candidate.displayId.empty()) {
                        desiredDisplayKeys[candidate.displayId] = routeKey;
                    }
                }
                auto& renderer = renderers_[routeKey];
                if (!renderer) renderer = std::make_unique<Renderer>(executable_);
                auto routeTarget = freezeRoute && target != Renderer::Target::ScreensaverPlay
                    ? Renderer::Target::DesktopFreeze : target;
                bool applied = renderer->Apply(routeTarget, output->media, decodeMode,
                    primaryOnly ? "primary" : "monitor", routeFrameRateCap,
                    route.monitorDevices, output->decodeAdapter, output->originalPlayback);
                auto decode = renderer->DecodeState();
                auto snapshot = renderer->RuntimeSnapshot();
                auto failureAction = motion::agent::automatic_decode_failure_action(
                    decodeMode, decode.path, decode.reason, !output->decodeAdapter.empty(),
                    snapshot.failed, snapshot.firstFrameSeen, output->hasAlternativeDecodeAdapter);
                if (failureAction.rejectAdapter) {
                    RememberAutoDecodeFailure(output->media.path, output->decodeAdapter);
                }
                if (!failureAction.tryAlternative &&
                    motion::agent::original_playback_failed(output->originalPlayback,
                        output->media.kind == "video", snapshot.failed, decode.path)) {
                    auto reason = decode.reason.empty() ? std::string("renderer-process-failed") : decode.reason;
                    for (auto const& candidate : outputs) {
                        if (!candidate.originalPlayback || candidate.media.kind != "video" ||
                            motion::agent::renderer_media_key(candidate.media.path, candidate.media.kind) != route.mediaKey) continue;
                        if (!primaryOnly && std::find(route.monitorDevices.begin(), route.monitorDevices.end(),
                                candidate.deviceName) == route.monitorDevices.end()) continue;
                        originalPlaybackFailures_[candidate.displayId] = {
                            candidate.groupId, candidate.mediaId, candidate.media.path.native(), reason,
                            snapshot.errorDetail };
                    }
                }
                if (applied && renderer->TargetReady()) readyKeys.insert(routeKey);
                allReady = applied && renderer->TargetReady() && allReady;
            }
            desiredKeys_ = desiredKeys;
            desiredDisplayKeys_ = std::move(desiredDisplayKeys);
            auto keep = retirement_.Keep(desiredDisplayKeys_, readyKeys, GetTickCount64());
            keep.insert(desiredKeys.begin(), desiredKeys.end());
            {
                for (auto iterator = renderers_.begin(); iterator != renderers_.end();) {
                    if (!keep.contains(iterator->first)) {
                        // Do not destroy a Renderer until its child process and
                        // acknowledgement thread are confirmed stopped. A
                        // failed retirement keeps the playback lease pinned and
                        // is retried by the next policy pass.
                        if (iterator->second->Stop()) iterator = renderers_.erase(iterator);
                        else ++iterator;
                    } else {
                        iterator->second->ReapExited();
                        ++iterator;
                    }
                }
            }
            if (allReady && RetiringRoutesStopped()) {
                acknowledgedPresentationKey_ = std::move(presentationKey);
            }
        }

        void Pause()
        {
            acknowledgedPresentationKey_.clear();
            for (auto& [_, renderer] : renderers_) renderer->Pause();
        }

        void Freeze()
        {
            acknowledgedPresentationKey_.clear();
            for (auto& [_, renderer] : renderers_) renderer->Freeze();
        }

        // Freeze only old routes used exclusively by displays that are about
        // to wait for a performance preview. A shared route that also owns a
        // non-pending sibling must remain playing until the replacement routes
        // are ready; the generation barrier prevents overlap in the meantime.
        [[nodiscard]] bool FreezeDisplays(
            std::vector<std::string> const& displayIds)
        {
            acknowledgedPresentationKey_.clear();
            std::vector<std::wstring> keys;
            for (auto const& displayId : displayIds) {
                auto route = desiredDisplayKeys_.find(displayId);
                if (route == desiredDisplayKeys_.end() ||
                    std::find(keys.begin(), keys.end(), route->second) != keys.end()) {
                    continue;
                }
                bool sharedWithPlayingSibling = std::any_of(
                    desiredDisplayKeys_.begin(), desiredDisplayKeys_.end(),
                    [&](auto const& candidate) {
                        return candidate.second == route->second &&
                            std::find(displayIds.begin(), displayIds.end(),
                                candidate.first) == displayIds.end();
                    });
                if (!sharedWithPlayingSibling) keys.push_back(route->second);
            }
            bool ready = true;
            for (auto const& key : keys) {
                auto renderer = renderers_.find(key);
                if (renderer == renderers_.end()) continue;
                renderer->second->Freeze();
                // Freeze refreshes process health and can discover an exit or
                // first-frame failure. Such a route can never acknowledge the
                // freeze; retire it instead of blocking the replacement copy.
                // The exclusive-display filter above still protects shared
                // routes that have a sibling outside this preview transition.
                auto snapshot = renderer->second->RuntimeSnapshot();
                auto action = motion::agent::preview_freeze_action(snapshot.running,
                    snapshot.transitionPending, snapshot.failed, snapshot.targetReady);
                bool stopped = action == motion::agent::PreviewFreezeAction::stop &&
                    renderer->second->Stop();
                ready = motion::agent::preview_freeze_settled(action, stopped) && ready;
            }
            return ready;
        }

        // A failed source route cannot acknowledge a freeze. Retire that
        // complete video route, including shared displays, but keep static
        // image routes alive while the compatibility copy is generated.
        [[nodiscard]] bool StopVideoDisplays(
            std::vector<std::string> const& displayIds)
        {
            auto keys = motion::agent::compatibility_renderer_routes_to_stop(
                displayIds, desiredDisplayKeys_, [&](auto const& key) {
                    auto renderer = renderers_.find(key);
                    return renderer != renderers_.end() && renderer->second->IsStaticImage();
                });
            if (!keys.empty()) acknowledgedPresentationKey_.clear();
            bool stopped = true;
            for (auto const& key : keys) {
                auto renderer = renderers_.find(key);
                bool routeStopped = renderer == renderers_.end() ||
                    renderer->second->Stop();
                stopped = routeStopped && stopped;
                if (!routeStopped) continue;
                if (renderer != renderers_.end()) renderers_.erase(renderer);
                desiredKeys_.erase(std::remove(desiredKeys_.begin(),
                    desiredKeys_.end(), key), desiredKeys_.end());
                for (auto display = desiredDisplayKeys_.begin();
                    display != desiredDisplayKeys_.end();) {
                    if (display->second == key) display = desiredDisplayKeys_.erase(display);
                    else ++display;
                }
            }
            return stopped;
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

        [[nodiscard]] bool HasRunningVideoRoute() const
        {
            return std::any_of(renderers_.begin(), renderers_.end(), [](auto const& entry) {
                return !entry.second->IsStaticImage() && entry.second->RuntimeSnapshot().running;
            });
        }

        [[nodiscard]] bool RetiringRoutesStopped() const
        {
            return std::none_of(renderers_.begin(), renderers_.end(),
                [&](auto const& entry) {
                    return std::find(desiredKeys_.begin(), desiredKeys_.end(),
                        entry.first) == desiredKeys_.end() &&
                        entry.second->RuntimeSnapshot().running;
                });
        }

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

        void ResetAutoDecodeFailures() noexcept
        {
            autoDecodeFailures_.clear();
            originalPlaybackFailures_.clear();
        }

        [[nodiscard]] std::string OriginalPlaybackFailure(DisplayMediaTarget const& output) const
        {
            if (!output.originalPlayback) return {};
            auto found = originalPlaybackFailures_.find(output.displayId);
            if (found == originalPlaybackFailures_.end()) return {};
            auto const& failure = found->second;
            return failure.Matches(output.groupId, output.mediaId, output.media.path.native())
                ? failure.reason : std::string{};
        }

        [[nodiscard]] std::string OriginalPlaybackErrorDetail(DisplayMediaTarget const& output) const
        {
            if (!output.originalPlayback) return {};
            auto found = originalPlaybackFailures_.find(output.displayId);
            if (found == originalPlaybackFailures_.end()) return {};
            auto const& failure = found->second;
            return failure.Matches(output.groupId, output.mediaId, output.media.path.native())
                ? failure.errorDetail : std::string{};
        }

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

        [[nodiscard]] std::vector<motion::DisplayRuntimeState> RuntimeStates(
            std::vector<motion::DisplayTarget> const& displays,
            std::vector<DisplayMediaTarget> const& outputs,
            motion::agent::RuntimeAction action, bool primaryOnly,
            bool manuallyPaused = false) const
        {
            std::vector<motion::DisplayRuntimeState> result;
            result.reserve(displays.size());
            bool paused = action == motion::agent::RuntimeAction::DisplayOff ||
                action == motion::agent::RuntimeAction::Locked ||
                action == motion::agent::RuntimeAction::Stopped ||
                action == motion::agent::RuntimeAction::DesktopPaused ||
                action == motion::agent::RuntimeAction::DesktopFrozen;
            auto pausedReason = [&]() -> std::string {
                switch (action) {
                case motion::agent::RuntimeAction::DisplayOff: return "display-off";
                case motion::agent::RuntimeAction::Locked: return "session-locked";
                case motion::agent::RuntimeAction::Stopped: return "playback-stopped";
                case motion::agent::RuntimeAction::DesktopPaused:
                    return std::string(motion::agent::desktop_pause_reason(manuallyPaused));
                case motion::agent::RuntimeAction::DesktopFrozen:
                    return "active-playback-disabled";
                default: return {};
                }
            };
            for (auto const& display : displays) {
                motion::DisplayRuntimeState state;
                state.displayId = display.id;
                state.deviceName = display.deviceName;
                state.displayName = display.friendlyName;
                auto output = std::find_if(outputs.begin(), outputs.end(), [&](auto const& value) {
                    return value.displayId == display.id;
                });
                if (output == outputs.end()) {
                    state.state = "paused";
                    state.reason = paused ? pausedReason() :
                        (primaryOnly ? "not-targeted" : "no-wallpaper");
                    result.push_back(std::move(state));
                    continue;
                }
                state.groupId = output->groupId;
                state.mediaId = output->mediaId;
                Renderer::Snapshot snapshot;
                bool hasRenderer{};
                auto route = desiredDisplayKeys_.find(display.id);
                if (route != desiredDisplayKeys_.end()) {
                    auto renderer = renderers_.find(route->second);
                    if (renderer != renderers_.end()) {
                        snapshot = renderer->second->RuntimeSnapshot();
                        hasRenderer = true;
                    }
                }
                bool performanceCopyUnavailable = output->performanceCopyRequired &&
                    !output->performanceCopyPending;
                bool degraded = output->softwarePlaybackTarget ||
                    snapshot.decode.path == "software-fallback" ||
                    performanceCopyUnavailable;
                bool rendererAbsent = !snapshot.running && !snapshot.transitionPending;
                auto originalFailure = OriginalPlaybackFailure(*output);
                motion::agent::DisplayRuntimeSignals signals{
                    snapshot.failed,
                    output->performanceCopyPending,
                    paused,
                    snapshot.targetReady,
                    degraded,
                    rendererAbsent,
                    !originalFailure.empty()
                };
                state.state = motion::agent::display_runtime_state(signals);
                state.decodePath = snapshot.decode.path;
                state.decodeReason = snapshot.decode.reason;
                state.errorDetail = snapshot.errorDetail;
                state.rendererProcessId = snapshot.processId;
                state.canRetry = snapshot.failed;
                state.canRestartRenderer = hasRenderer;
                if (!originalFailure.empty()) {
                    state.reason = paused ? pausedReason() : "original-playback-failed";
                    state.decodePath = "unavailable";
                    state.decodeReason = std::move(originalFailure);
                    state.errorDetail = OriginalPlaybackErrorDetail(*output);
                    state.canRetry = !paused;
                    state.canRestartRenderer = !paused;
                } else if (snapshot.failed) {
                    state.reason = snapshot.decode.reason.empty()
                        ? "renderer-process-failed" : snapshot.decode.reason;
                } else if (paused && rendererAbsent) {
                    state.reason = pausedReason();
                } else if (!snapshot.targetReady) {
                    state.reason = paused ? "pause-pending" :
                        snapshot.transitionPending
                            ? "waiting-for-first-frame" : "renderer-starting";
                } else if (paused) {
                    state.reason = pausedReason();
                } else if (output->performanceCopyPending) {
                    state.reason = "performance-copy-pending";
                } else if (performanceCopyUnavailable) {
                    state.reason = "performance-copy-unavailable";
                } else if (degraded) {
                    state.reason = snapshot.decode.reason.empty()
                        ? "compatibility-fallback" : snapshot.decode.reason;
                }
                if (output->gpuProbePending && !paused && !signals.originalPlaybackFailed) {
                    state.state = "applying";
                    state.reason = "checking-decoder";
                    state.decodePath = "probing";
                    state.decodeReason = "checking-decoder";
                    state.errorDetail.clear();
                    state.canRetry = false;
                }
                result.push_back(std::move(state));
            }
            return result;
        }

        bool Retry(std::string const& displayId)
        {
            auto keys = ControlKeys(displayId);
            bool retried = ClearOriginalPlaybackFailures(displayId);
            for (auto const& key : keys) {
                auto renderer = renderers_.find(key);
                if (renderer != renderers_.end()) {
                    retried = renderer->second->RetryFailed() || retried;
                }
            }
            if (retried) autoDecodeFailures_.clear();
            return retried;
        }

        bool Restart(std::string const& displayId)
        {
            auto keys = ControlKeys(displayId);
            bool found = ClearOriginalPlaybackFailures(displayId);
            bool stopped = true;
            for (auto const& key : keys) {
                auto renderer = renderers_.find(key);
                if (renderer == renderers_.end()) continue;
                found = true;
                stopped = renderer->second->Restart() && stopped;
            }
            if (found && stopped) autoDecodeFailures_.clear();
            return found && stopped;
        }

        void TopologyChanged() noexcept
        {
            ++topologyGeneration_;
            acknowledgedPresentationKey_.clear();
            ResetAutoDecodeFailures();
        }
        bool Stop()
        {
            acknowledgedPresentationKey_.clear();
            desiredKeys_.clear();
            desiredDisplayKeys_.clear();
            retirement_.Clear();
            bool stopped = true;
            for (auto& [_, renderer] : renderers_) {
                stopped = renderer->Stop() && stopped;
            }
            if (stopped) renderers_.clear();
            return stopped;
        }

    private:
        [[nodiscard]] std::wstring PresentationKey(
            Renderer::Target target,
            std::vector<DisplayMediaTarget> const& outputs,
            std::string const& decodeMode, bool primaryOnly,
            bool freezePerformanceCopies) const
        {
            std::wstring key = std::to_wstring(static_cast<unsigned>(target)) +
                L"\n" + motion::utf8_to_wide(decodeMode) + L"\n" +
                (primaryOnly ? L"primary" : L"monitor") + L"\n" +
                (freezePerformanceCopies ? L"performance-preview" : L"normal") +
                L"\n" + std::to_wstring(topologyGeneration_);
            for (auto const& output : outputs) {
                key += L"\n" + motion::utf8_to_wide(output.displayId) + L"\n" +
                    output.deviceName + L"\n" + output.media.path.wstring() + L"\n" +
                    motion::utf8_to_wide(output.media.kind) + L"\n" +
                    output.decodeAdapter + L"\n" +
                    std::to_wstring(output.playbackFrameRateCap) + L"\n" +
                    motion::display_refresh_rate_key(output.displayRefreshRate) + L"\n" +
                    (output.media.sourceBacked ? L"source" : L"derived") + L"\n" +
                    (output.performanceCopyRequired ? L"copy-required" : L"copy-ready");
            }
            return key;
        }

        [[nodiscard]] std::vector<std::wstring> ControlKeys(
            std::string const& displayId) const
        {
            if (displayId.empty()) return desiredKeys_;
            auto route = desiredDisplayKeys_.find(displayId);
            if (route == desiredDisplayKeys_.end()) return {};
            return { route->second };
        }

        std::wstring RouteKey(motion::agent::SharedRendererRoute const& route,
            std::string const& decodeMode, bool primaryOnly,
            uint32_t frameRateCap) const
        {
            return motion::agent::renderer_route_key(route, motion::utf8_to_wide(decodeMode),
                primaryOnly, frameRateCap, topologyGeneration_);
        }

        void RememberAutoDecodeFailure(fs::path const& media, std::wstring const& adapter)
        {
            if (AutoDecodeRouteRejected(media, adapter)) return;
            autoDecodeFailures_.emplace_back(media.lexically_normal().wstring(), adapter);
        }

        bool ClearOriginalPlaybackFailures(std::string const& displayId)
        {
            if (!displayId.empty()) return originalPlaybackFailures_.erase(displayId) != 0;
            bool hadFailures = !originalPlaybackFailures_.empty();
            originalPlaybackFailures_.clear();
            return hadFailures;
        }

        fs::path executable_;
        std::map<std::wstring, std::unique_ptr<Renderer>> renderers_;
        motion::agent::RendererRetirementPolicy retirement_;
        std::vector<std::wstring> desiredKeys_;
        std::map<std::string, std::wstring> desiredDisplayKeys_;
        std::vector<std::pair<std::wstring, std::wstring>> autoDecodeFailures_;
        std::map<std::string, motion::agent::OriginalPlaybackFailure> originalPlaybackFailures_;
        std::wstring acknowledgedPresentationKey_;
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

    void query_session_state(motion::agent::RuntimeSessionState& session)
    {
        LPWSTR buffer{};
        DWORD bytes{};
        auto release = [](wchar_t* value) noexcept { if (value) WTSFreeMemory(value); };
        if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
                WTSSessionInfoEx, &buffer, &bytes)) {
            std::unique_ptr<wchar_t, decltype(release)> memory(buffer, release);
            if (buffer && bytes >= sizeof(WTSINFOEXW)) {
                auto const& info = *reinterpret_cast<WTSINFOEXW const*>(buffer);
                if (info.Level == 1) {
                    auto const& details = info.Data.WTSInfoExLevel1;
                    std::optional<bool> locked;
                    if (details.SessionFlags == WTS_SESSIONSTATE_LOCK) locked = true;
                    else if (details.SessionFlags == WTS_SESSIONSTATE_UNLOCK) locked = false;
                    session.Observe(details.SessionState == WTSActive, locked);
                    return;
                }
            }
        } else if (buffer) {
            WTSFreeMemory(buffer);
        }
        buffer = nullptr;
        bytes = 0;
        if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
                WTSConnectState, &buffer, &bytes)) {
            std::unique_ptr<wchar_t, decltype(release)> memory(buffer, release);
            if (buffer && bytes >= sizeof(WTS_CONNECTSTATE_CLASS)) {
                session.Observe(*reinterpret_cast<WTS_CONNECTSTATE_CLASS const*>(buffer) == WTSActive,
                    std::nullopt);
            }
        } else if (buffer) {
            WTSFreeMemory(buffer);
        }
    }

    class RuntimeEvents
    {
    public:
        explicit RuntimeEvents(fs::path settingsExecutable) : settingsExecutable_(std::move(settingsExecutable))
        {
            appExitEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, motion::app_exit_event_name));
            togglePlaybackEvent_.reset(CreateEventW(nullptr, FALSE, FALSE, motion::toggle_playback_event_name));
            nextWallpaperEvent_.reset(CreateEventW(nullptr, FALSE, FALSE, motion::next_wallpaper_event_name));
            screensaverPreviewEvent_.reset(CreateEventW(nullptr, FALSE, FALSE, motion::screensaver_preview_event_name));
            query_session_state(session_);
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
            displayNotification_ = RegisterPowerSettingNotification(window_, &GUID_SESSION_DISPLAY_STATUS, DEVICE_NOTIFY_WINDOW_HANDLE);
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
            SetScreensaverInputWakeEnabled(false);
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

        explicit operator bool() const
        {
            return window_ != nullptr && appExitEvent_ && togglePlaybackEvent_ &&
                nextWallpaperEvent_ && screensaverPreviewEvent_;
        }
        bool Locked() const { return session_.Inactive(); }
        bool DisplayOn() const { return displayOn_; }
        bool OnBattery() const { return onBattery_; }
        uint64_t TopologyRevision() const { return topologyRevision_; }
        uint64_t InputRevision() const { return inputRevision_; }
        bool ExitRequested() const { return exitRequested_; }

        bool TakeTogglePlaybackRequested() noexcept
        {
            return std::exchange(togglePlaybackRequested_, false);
        }

        bool TakeNextWallpaperRequested() noexcept
        {
            return std::exchange(nextWallpaperRequested_, false);
        }

        bool TakeScreensaverPreviewRequested() noexcept
        {
            return std::exchange(screensaverPreviewRequested_, false);
        }

        void SetTrayStatus(motion::agent::TrayStatus status, bool manuallyPaused)
        {
            bool changed = status_ != status || manuallyPaused_ != manuallyPaused;
            status_ = status;
            manuallyPaused_ = manuallyPaused;
            if (!changed || !window_) return;
            NOTIFYICONDATAW icon{ sizeof(icon) };
            icon.hWnd = window_;
            icon.uID = trayIconId;
            icon.uFlags = NIF_TIP;
            auto tip = std::wstring(L"MotionWallpaper · ") +
                motion::agent::tray_status_text(status_);
            wcscpy_s(icon.szTip, tip.c_str());
            Shell_NotifyIconW(NIM_MODIFY, &icon);
        }

        bool SetScreensaverInputWakeEnabled(bool enabled) noexcept
        {
            if (!window_ || enabled == rawInputWakeEnabled_) return enabled == rawInputWakeEnabled_;
            RAWINPUTDEVICE devices[]{
                { HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_MOUSE,
                    static_cast<DWORD>(enabled ? RIDEV_INPUTSINK : RIDEV_REMOVE),
                    enabled ? window_ : nullptr },
                { HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_KEYBOARD,
                    static_cast<DWORD>(enabled ? RIDEV_INPUTSINK : RIDEV_REMOVE),
                    enabled ? window_ : nullptr }
            };
            if (!RegisterRawInputDevices(devices, ARRAYSIZE(devices), sizeof(RAWINPUTDEVICE))) return false;
            rawInputWakeEnabled_ = enabled;
            return true;
        }

        void PollSessionState(std::chrono::steady_clock::time_point now)
        {
            if (now < nextSessionPoll_) return;
            nextSessionPoll_ = now + 1s;
            query_session_state(session_);
        }

        bool Wait(HANDLE settingsEvent, DWORD milliseconds)
        {
            HANDLE handles[]{ settingsEvent, appExitEvent_.get(), togglePlaybackEvent_.get(),
                nextWallpaperEvent_.get(), screensaverPreviewEvent_.get() };
            DWORD result = MsgWaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, milliseconds, QS_ALLINPUT);
            if (result == WAIT_OBJECT_0) return true;
            if (result == WAIT_OBJECT_0 + 1) {
                exitRequested_ = true;
                return false;
            }
            if (result == WAIT_OBJECT_0 + 2) {
                togglePlaybackRequested_ = true;
                return false;
            }
            if (result == WAIT_OBJECT_0 + 3) {
                nextWallpaperRequested_ = true;
                return false;
            }
            if (result == WAIT_OBJECT_0 + 4) {
                screensaverPreviewRequested_ = true;
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
        static constexpr UINT commandOpen = 1;
        static constexpr UINT commandExit = 2;
        static constexpr UINT commandTogglePlayback = 3;
        static constexpr UINT commandNextWallpaper = 4;
        static constexpr UINT commandPreviewScreensaver = 5;
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
            auto tip = std::wstring(L"MotionWallpaper · ") +
                motion::agent::tray_status_text(status_);
            wcscpy_s(icon.szTip, tip.c_str());
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
            auto status = std::wstring(L"当前状态：") +
                motion::agent::tray_status_text(status_);
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status.c_str());
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, commandTogglePlayback,
                manuallyPaused_ ? L"恢复壁纸" : L"暂停壁纸");
            AppendMenuW(menu, MF_STRING | (manuallyPaused_ ? MF_GRAYED : 0),
                commandNextWallpaper, L"下一张");
            AppendMenuW(menu, MF_STRING, commandPreviewScreensaver, L"立即屏保");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, commandOpen, L"打开 MotionWallpaper");
            AppendMenuW(menu, MF_STRING, commandExit, L"退出");
            SetForegroundWindow(window_);
            UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                cursor.x, cursor.y, 0, window_, nullptr);
            DestroyMenu(menu);
            if (command == commandOpen) OpenSettings();
            else if (command == commandTogglePlayback && togglePlaybackEvent_) {
                SetEvent(togglePlaybackEvent_.get());
            } else if (command == commandNextWallpaper && nextWallpaperEvent_) {
                SetEvent(nextWallpaperEvent_.get());
            } else if (command == commandPreviewScreensaver && screensaverPreviewEvent_) {
                SetEvent(screensaverPreviewEvent_.get());
            } else if (command == commandExit) {
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
            auto eventEffect = motion::agent::runtime_system_event_effect(
                message, wParam, self->taskbarCreated_, self->rawInputWakeEnabled_);
            motion::agent::apply_runtime_system_event_effect(eventEffect,
                self->topologyRevision_, self->inputRevision_, self->displayOn_);
            if (eventEffect.shellRestarted) {
                self->AddTrayIcon();
            }
            if (eventEffect.handled) return message == WM_POWERBROADCAST ? TRUE : 0;
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
            case WM_INPUT:
                return DefWindowProcW(window, message, wParam, lParam);
            case WM_WTSSESSION_CHANGE:
                self->session_.Notify(static_cast<uint32_t>(wParam));
                self->nextSessionPoll_ = std::chrono::steady_clock::time_point::min();
                if (wParam == WTS_CONSOLE_CONNECT || wParam == WTS_CONSOLE_DISCONNECT ||
                    wParam == WTS_REMOTE_CONNECT || wParam == WTS_REMOTE_DISCONNECT) {
                    ++self->topologyRevision_;
                }
                return 0;
            case WM_POWERBROADCAST:
                if (wParam == PBT_POWERSETTINGCHANGE) {
                    auto setting = reinterpret_cast<POWERBROADCAST_SETTING*>(lParam);
                    if (setting && IsEqualGUID(setting->PowerSetting, GUID_SESSION_DISPLAY_STATUS) && setting->DataLength >= sizeof(DWORD)) {
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
        motion::unique_handle togglePlaybackEvent_;
        motion::unique_handle nextWallpaperEvent_;
        motion::unique_handle screensaverPreviewEvent_;
        motion::agent::TrayStatus status_{ motion::agent::TrayStatus::Starting };
        motion::agent::RuntimeSessionState session_;
        bool sessionNotificationRegistered_{};
        bool displayOn_{ true };
        bool onBattery_{};
        bool exitRequested_{};
        bool togglePlaybackRequested_{};
        bool nextWallpaperRequested_{};
        bool screensaverPreviewRequested_{};
        bool manuallyPaused_{};
        bool rawInputWakeEnabled_{};
        uint64_t topologyRevision_{};
        uint64_t inputRevision_{};
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
    int argumentCount{};
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments) return 1;
    if (argumentCount >= 2 &&
        wcscmp(arguments[1], L"--probe-video-first-frame") == 0) {
        if (argumentCount != 3) {
            LocalFree(arguments);
            return 1;
        }
        fs::path candidate(arguments[2]);
        LocalFree(arguments);
        return motion::agent::run_video_first_frame_probe(candidate);
    }
    auto gpuProbeExitCode = motion::agent::run_video_gpu_probe_cli(argumentCount, arguments);
    LocalFree(arguments);
    if (gpuProbeExitCode >= 0) return gpuProbeExitCode;
    auto installation = motion::startup::inspect_installation(motion::executable_directory());
    if (!installation.conflict.empty()) {
        MessageBoxW(nullptr, installation.conflict.c_str(), L"MotionWallpaper · 安装位置冲突",
            MB_OK | MB_ICONWARNING);
        return ERROR_ALREADY_EXISTS;
    }
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
        fs::path runtimeControlPath = root / L"Config" / L"runtime-command.json";
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
        std::string trayOverrideGroupId, trayOverrideMediaId;
        std::string previousDisplayAssignmentsKey;
        std::map<std::string, std::pair<std::string, std::string>> trayDisplayOverrides;
        int previousRandomInterval = -1;
        bool wasLocked = false, autoLockTriggered = false, displayOffAfterLockTriggered = false;
        bool reload = true, selectionInitialized = false;
        PerformancePreviewStage performancePreviewStage{
            PerformancePreviewStage::Inactive };
        bool performanceCopyWasRequired{};
        motion::IdleTimer screensaverIdleTimer;
        motion::IdleTimer autoLockIdleTimer;
        bool screensaverWasActive{};
        motion::agent::TrayControlState trayControls;
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
        std::string gpuEnvironment;
        std::string publishedGroupId, publishedMediaId, publishedDecodePath, publishedDecodeReason;
        std::string publishedPerformanceMode, publishedActiveSceneId;
        bool effectiveSettingsActive{};
        std::vector<motion::DisplayRuntimeState> publishedDisplayStates;
        auto agentInstanceId = motion::new_id();
        uint32_t agentProcessId = GetCurrentProcessId();
        std::string publishedAgentInstanceId;
        uint32_t publishedAgentProcessId{};
        std::string lastCommandId, lastCommandAction, lastCommandMessage;
        std::string publishedLastCommandId, publishedLastCommandAction,
            publishedLastCommandMessage;
        bool lastCommandSucceeded{};
        bool publishedLastCommandSucceeded{};
        if (!legacyDataConflict) {
            motion::RuntimeState previousRuntime;
            if (motion::try_load_runtime(runtimePath, previousRuntime)) {
                publishedGroupId = std::move(previousRuntime.activeGroupId);
                publishedMediaId = std::move(previousRuntime.activeMediaId);
                publishedDecodePath = std::move(previousRuntime.decodePath);
                publishedDecodeReason = std::move(previousRuntime.decodeReason);
                publishedPerformanceMode = std::move(previousRuntime.performanceMode);
                publishedActiveSceneId = std::move(previousRuntime.activeSceneId);
                publishedDisplayStates = std::move(previousRuntime.displayStates);
                publishedAgentInstanceId = std::move(previousRuntime.agentInstanceId);
                publishedAgentProcessId = previousRuntime.agentProcessId;
                lastCommandId = std::move(previousRuntime.lastCommandId);
                lastCommandAction = std::move(previousRuntime.lastCommandAction);
                lastCommandSucceeded = previousRuntime.lastCommandSucceeded;
                lastCommandMessage = std::move(previousRuntime.lastCommandMessage);
                publishedLastCommandId = lastCommandId;
                publishedLastCommandAction = lastCommandAction;
                publishedLastCommandSucceeded = lastCommandSucceeded;
                publishedLastCommandMessage = lastCommandMessage;
            }
        }
        bool configFailureReported{};
        bool runtimeFailureReported{};
        bool automaticCompatibilityPriority{};
        auto publishRuntime = [&](std::string groupId, std::string mediaId,
            std::string decodePath = {}, std::string decodeReason = {},
            std::optional<std::vector<motion::DisplayRuntimeState>> displayStates = std::nullopt) {
            if (legacyDataConflict) return;
            auto nextDisplayStates = displayStates
                ? std::move(*displayStates) : publishedDisplayStates;
            // Command acknowledgements can be published before this pass has
            // applied an automatic scene. Preserve the previous effective
            // context then; publishing base settings would briefly mislabel
            // the still-current display failures as a different quality mode.
            auto nextPerformanceMode = effectiveSettingsActive
                ? settings.performanceMode : publishedPerformanceMode;
            auto nextActiveSceneId = effectiveSettingsActive
                ? settings.activeSceneId : publishedActiveSceneId;
            if (!configurationAvailable) {
                nextPerformanceMode.clear();
                nextActiveSceneId.clear();
            }
            if (groupId == publishedGroupId && mediaId == publishedMediaId &&
                decodePath == publishedDecodePath && decodeReason == publishedDecodeReason &&
                nextPerformanceMode == publishedPerformanceMode &&
                nextActiveSceneId == publishedActiveSceneId &&
                nextDisplayStates == publishedDisplayStates &&
                agentInstanceId == publishedAgentInstanceId &&
                agentProcessId == publishedAgentProcessId &&
                lastCommandId == publishedLastCommandId &&
                lastCommandAction == publishedLastCommandAction &&
                lastCommandSucceeded == publishedLastCommandSucceeded &&
                lastCommandMessage == publishedLastCommandMessage) return;
            try {
                motion::RuntimeState runtime;
                runtime.activeGroupId = groupId;
                runtime.activeMediaId = mediaId;
                runtime.decodePath = decodePath;
                runtime.decodeReason = decodeReason;
                runtime.performanceMode = nextPerformanceMode;
                runtime.activeSceneId = nextActiveSceneId;
                runtime.agentInstanceId = agentInstanceId;
                runtime.agentProcessId = agentProcessId;
                runtime.displayStates = nextDisplayStates;
                runtime.lastCommandId = lastCommandId;
                runtime.lastCommandAction = lastCommandAction;
                runtime.lastCommandSucceeded = lastCommandSucceeded;
                runtime.lastCommandMessage = lastCommandMessage;
                runtime.updatedAt = motion::timestamp_utc();
                motion::save_runtime(runtimePath, runtime);
                for (auto const& display : nextDisplayStates) {
                    if (display.errorDetail.empty()) continue;
                    auto previous = std::find_if(publishedDisplayStates.begin(), publishedDisplayStates.end(),
                        [&](auto const& value) { return value.displayId == display.displayId; });
                    if (previous == publishedDisplayStates.end() || previous->errorDetail != display.errorDetail) {
                        append_agent_log(root, L"Renderer [" + display.displayName + L"] " +
                            motion::utf8_to_wide(display.errorDetail));
                    }
                }
                publishedGroupId = std::move(groupId);
                publishedMediaId = std::move(mediaId);
                publishedDecodePath = std::move(decodePath);
                publishedDecodeReason = std::move(decodeReason);
                publishedPerformanceMode = std::move(nextPerformanceMode);
                publishedActiveSceneId = std::move(nextActiveSceneId);
                publishedDisplayStates = std::move(nextDisplayStates);
                publishedAgentInstanceId = agentInstanceId;
                publishedAgentProcessId = agentProcessId;
                publishedLastCommandId = lastCommandId;
                publishedLastCommandAction = lastCommandAction;
                publishedLastCommandSucceeded = lastCommandSucceeded;
                publishedLastCommandMessage = lastCommandMessage;
                runtimeFailureReported = false;
            } catch (...) {
                if (!runtimeFailureReported) append_agent_log(root, L"无法写入运行时壁纸状态。");
                runtimeFailureReported = true;
            }
        };
        auto uniformDisplayStates = [](std::string state, std::string reason) {
            std::vector<motion::DisplayRuntimeState> result;
            for (auto const& display : motion::enumerate_displays()) {
                motion::DisplayRuntimeState value;
                value.displayId = display.id;
                value.deviceName = display.deviceName;
                value.displayName = display.friendlyName;
                value.state = state;
                value.reason = reason;
                result.push_back(std::move(value));
            }
            return result;
        };

        for (;;) {
            effectiveSettingsActive = false;
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
                    publishedPerformanceMode.clear();
                    publishedActiveSceneId.clear();
                    publishedDisplayStates.clear();
                    publishedAgentInstanceId.clear();
                    publishedAgentProcessId = 0;
                    lastCommandId.clear();
                    lastCommandAction.clear();
                    lastCommandMessage.clear();
                    lastCommandSucceeded = false;
                    publishedLastCommandId.clear();
                    publishedLastCommandAction.clear();
                    publishedLastCommandMessage.clear();
                    publishedLastCommandSucceeded = false;
                }
                legacyDataConflict = true;
                trayControls.CancelScreensaverPreview();
                runtimeEvents.SetScreensaverInputWakeEnabled(false);
                runtimeEvents.SetTrayStatus(motion::agent::TrayStatus::Unavailable,
                    trayControls.ManuallyPaused());
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
                runtimeControlPath = root / L"Config" / L"runtime-command.json";
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
                publishedGroupId.clear();
                publishedMediaId.clear();
                publishedDecodePath.clear();
                publishedDecodeReason.clear();
                publishedPerformanceMode.clear();
                publishedActiveSceneId.clear();
                publishedDisplayStates.clear();
                publishedAgentInstanceId.clear();
                publishedAgentProcessId = 0;
                lastCommandId.clear();
                lastCommandAction.clear();
                lastCommandMessage.clear();
                lastCommandSucceeded = false;
                motion::RuntimeState previousRuntime;
                if (motion::try_load_runtime(runtimePath, previousRuntime)) {
                    publishedGroupId = std::move(previousRuntime.activeGroupId);
                    publishedMediaId = std::move(previousRuntime.activeMediaId);
                    publishedDecodePath = std::move(previousRuntime.decodePath);
                    publishedDecodeReason = std::move(previousRuntime.decodeReason);
                    publishedPerformanceMode = std::move(previousRuntime.performanceMode);
                    publishedActiveSceneId = std::move(previousRuntime.activeSceneId);
                    publishedDisplayStates = std::move(previousRuntime.displayStates);
                    publishedAgentInstanceId = std::move(previousRuntime.agentInstanceId);
                    publishedAgentProcessId = previousRuntime.agentProcessId;
                    lastCommandId = std::move(previousRuntime.lastCommandId);
                    lastCommandAction = std::move(previousRuntime.lastCommandAction);
                    lastCommandSucceeded = previousRuntime.lastCommandSucceeded;
                    lastCommandMessage = std::move(previousRuntime.lastCommandMessage);
                }
                publishedLastCommandId = lastCommandId;
                publishedLastCommandAction = lastCommandAction;
                publishedLastCommandSucceeded = lastCommandSucceeded;
                publishedLastCommandMessage = lastCommandMessage;
                nextFallbackReload = std::chrono::steady_clock::now();
            }

            motion::RuntimeControlRequest controlRequest;
            if (motion::try_load_runtime_control_request(
                    runtimeControlPath, controlRequest) &&
                controlRequest.requestId != lastCommandId) {
                bool succeeded{};
                if (controlRequest.action == "retry") {
                    succeeded = renderers.Retry(controlRequest.displayId);
                    lastCommandMessage = succeeded
                        ? "retry-scheduled" : "no-failed-renderer";
                } else if (controlRequest.action == "restart-renderer") {
                    succeeded = renderers.Restart(controlRequest.displayId);
                    lastCommandMessage = succeeded
                        ? "renderer-restart-scheduled" :
                        "renderer-not-found-or-stop-failed";
                }
                lastCommandId = controlRequest.requestId;
                lastCommandAction = controlRequest.action;
                lastCommandSucceeded = succeeded;
                publishRuntime(publishedGroupId, publishedMediaId,
                    publishedDecodePath, publishedDecodeReason);
            }

            if (libraryMigration.Requested()) {
                // The App must not copy a library while a decoder, FFmpeg, or
                // cache-normalization pass still owns a handle into it.
                bool renderersStopped = renderers.Stop();
                renderers.ResetAutoDecodeFailures();
                videoOptimizer.reset();
                importedRequests.clear();
                mediaLibraryTrust.reset();
                trayControls.CancelScreensaverPreview();
                runtimeEvents.SetScreensaverInputWakeEnabled(false);
                runtimeEvents.SetTrayStatus(motion::agent::TrayStatus::Paused,
                    trayControls.ManuallyPaused());
                publishRuntime({}, {}, "unavailable", "library-migration",
                    uniformDisplayStates("paused", "library-migration"));
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
                trayControls.CancelScreensaverPreview();
                runtimeEvents.SetScreensaverInputWakeEnabled(false);
                runtimeEvents.SetTrayStatus(motion::agent::TrayStatus::Unavailable,
                    trayControls.ManuallyPaused());
                publishRuntime({}, {}, "unavailable", "settings-unavailable",
                    uniformDisplayStates("failed", "settings-unavailable"));
                reload = runtimeEvents.Wait(settingsEvent.get(), 1000);
                continue;
            }
            if (now >= nextOptimizationRequestScan) {
                importedRequests = imported_optimization_requests(wallpapers);
                nextOptimizationRequestScan = now + (importedRequests.empty() ? 10s : 1s);
            }
            auto displays = motion::enumerate_displays();
            auto prepareImported = [&] {
                if (importedRequests.empty()) return;
                for (auto const& request : importedRequests) {
                    auto targets = display_media_targets(wallpapers, settings,
                        settings.selectedGroupId, settings.selectedMediaId, request.mode, displays);
                    auto target = optimization_target_size(targets, request.media.id, displays);
                    std::map<std::pair<uint32_t, uint32_t>, OptimizationTarget> aspects;
                    for (auto const& output : targets) {
                        if (output.mediaId != request.media.id) continue;
                        auto& required = aspects[motion::agent::video_display_aspect(output.targetWidth, output.targetHeight)];
                        required.width = (std::max)(required.width, output.targetWidth);
                        required.height = (std::max)(required.height, output.targetHeight);
                        required.refreshRateHz = (std::max)(required.refreshRateHz, output.targetRefreshRate);
                    }
                    // A mode has one durable request token. Select its next
                    // missing viewport before completing that token, otherwise
                    // a ready landscape copy repeatedly cancels portrait work.
                    for (auto const& [_, required] : aspects) {
                        auto resolved = videoOptimizer->ResolveWithLease(request.media.path, request.mode,
                            required.width, required.height, required.refreshRateHz, false, false);
                        if (resolved.performanceCopyRequired || resolved.gpuProbePending) { target = required; break; }
                    }
                    videoOptimizer->Prepare(request.media.path, request.mode,
                        target.width, target.height, target.refreshRateHz);
                }
            };

            bool locked = runtimeEvents.Locked();
            auto input = input_state();
            auto uptime = std::chrono::milliseconds(GetTickCount64());
            if (runtimeEvents.TakeTogglePlaybackRequested()) trayControls.TogglePlayback();
            if (runtimeEvents.TakeScreensaverPreviewRequested()) {
                trayControls.RequestScreensaverPreview(input.tick, runtimeEvents.InputRevision());
            }
            (void)trayControls.ObserveInput(input.tick, runtimeEvents.InputRevision());
            auto inhibition = idleInhibitor.State(now);
            auto screensaverIdle = screensaverIdleTimer.Update(uptime, input.tick, input.idle,
                motion::agent::screensaver_idle_is_inhibited(inhibition.display, screensaverWasActive));
            DWORD waitMilliseconds = motion::agent::stable_wait_ms;
            std::vector<motion::DisplayRuntimeState> currentDisplayStates;
            if (runtimeEvents.TopologyRevision() != topologyRevision) {
                topologyRevision = runtimeEvents.TopologyRevision();
                renderers.TopologyChanged();
                motion::agent::invalidate_video_gpu_probes();
                bool available = physical_video_device_available();
                physicalVideoDeviceAvailable = available;
                // Adapter membership and per-codec decode profiles can change
                // even while at least one physical video device remains. Drop
                // both target choices and the source-capability cache for every
                // topology revision.
                videoOptimizer->InvalidateChoices();
                automaticCompatibilityPriority = false;
            }
            auto gpuInventory = motion::agent::video_gpu_inventory_async();
            if (!gpuInventory.pending) {
                auto environment = gpuInventory.succeeded ? gpuInventory.environment : std::string("gpu-probe-unavailable");
                if (environment != gpuEnvironment) {
                    gpuEnvironment = std::move(environment);
                    physicalVideoDeviceAvailable = gpuInventory.physicalVideoDeviceAvailable;
                    renderers.ResetAutoDecodeFailures();
                    videoOptimizer->InvalidateChoices();
                }
            }
            IterationSettingsRestore restoreConfiguredSettings{ settings, settings };
            SYSTEMTIME localTime{};
            GetLocalTime(&localTime);
            motion::SceneActivationContext sceneContext{
                static_cast<int>(localTime.wHour) * 60 + localTime.wMinute,
                runtimeEvents.OnBattery(),
                cloned_or_projected_display_active()
            };
            if (auto automaticScene = motion::automatic_scene_for_context(
                    settings, sceneContext)) {
                (void)motion::apply_scene_profile(settings, *automaticScene);
            }
            effectiveSettingsActive = true;
            videoOptimizer->SetStorageQuotaBytes(
                settings.optimizationStorageQuotaBytes);
            auto sessionAction = motion::agent::reduce_runtime_action(settings,
                { runtimeEvents.DisplayOn(), locked, false, false, 0 });
            if (sessionAction == motion::agent::RuntimeAction::DisplayOff) {
                screensaverWasActive = false;
                trayControls.CancelScreensaverPreview();
                runtimeEvents.SetScreensaverInputWakeEnabled(false);
                runtimeEvents.SetTrayStatus(motion::agent::TrayStatus::DisplayOff,
                    trayControls.ManuallyPaused());
                bool stopped = renderers.Stop();
                bool mayPrepare = motion::agent::hidden_session_generation_allowed(
                    sessionAction, runtimeEvents.OnBattery(), stopped);
                videoOptimizer->SetGenerationAllowed(mayPrepare);
                if (mayPrepare) prepareImported();
                currentDisplayStates = renderers.RuntimeStates(displays, {},
                    sessionAction, settings.displayMode == "primary");
                waitMilliseconds = 1000;
            } else if (sessionAction == motion::agent::RuntimeAction::Locked) {
                screensaverWasActive = false;
                trayControls.CancelScreensaverPreview();
                runtimeEvents.SetScreensaverInputWakeEnabled(false);
                runtimeEvents.SetTrayStatus(motion::agent::TrayStatus::Locked,
                    trayControls.ManuallyPaused());
                bool stopped = renderers.Stop();
                bool mayPrepare = motion::agent::hidden_session_generation_allowed(
                    sessionAction, runtimeEvents.OnBattery(), stopped);
                videoOptimizer->SetGenerationAllowed(mayPrepare);
                if (mayPrepare) prepareImported();
                currentDisplayStates = renderers.RuntimeStates(displays, {},
                    sessionAction, settings.displayMode == "primary");
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
                // Original is a direct-play choice, including when the user
                // explicitly requests a software decoder. It never implies a
                // silent cpu-smooth conversion or a different selected file.
                bool softwarePlayback = settings.performanceMode != "original" &&
                    motion::agent::uses_software_playback(
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
                auto displayAssignmentsKey = display_assignments_key(settings.displayAssignments);
                bool displayAssignmentsChanged = displayAssignmentsKey != previousDisplayAssignmentsKey;
                previousDisplayAssignmentsKey = std::move(displayAssignmentsKey);
                if (displayAssignmentsChanged) trayDisplayOverrides.clear();
                if (selectionChanged || performanceModeChanged ||
                    displayAssignmentsChanged) {
                    // A new playback epoch must establish its static preview
                    // before any selected-wallpaper transcode can run beside
                    // an older source Renderer.
                    videoOptimizer->SetGenerationAllowed(false);
                    videoOptimizer->InvalidateChoices();
                    renderers.ResetAutoDecodeFailures();
                    automaticCompatibilityPriority = false;
                    performancePreviewStage = PerformancePreviewStage::Inactive;
                    performanceCopyWasRequired = false;
                }
                if (selectionChanged || randomGroupChanged) {
                    trayOverrideGroupId.clear();
                    trayOverrideMediaId.clear();
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
                // Always consume the auto-reset request. A command received
                // while manually paused is intentionally discarded instead
                // of firing unexpectedly after the later resume.
                bool nextWallpaperRequested =
                    runtimeEvents.TakeNextWallpaperRequested();
                if (motion::agent::tray_next_wallpaper_should_advance(
                        nextWallpaperRequested, trayControls.ManuallyPaused())) {
                    auto currentGroupId = trayOverrideMediaId.empty()
                        ? (randomId.empty() ? settings.selectedGroupId : settings.randomGroupId)
                        : trayOverrideGroupId;
                    auto currentMediaId = trayOverrideMediaId.empty()
                        ? (randomId.empty() ? settings.selectedMediaId : randomId)
                        : trayOverrideMediaId;
                    auto nextId = next_media_id(wallpapers, currentGroupId,
                        currentMediaId, playbackPerformanceMode);
                    bool advanced{};
                    if (!nextId.empty()) {
                        advanced = true;
                        if (randomActive) {
                            randomId = std::move(nextId);
                            trayOverrideGroupId.clear();
                            trayOverrideMediaId.clear();
                            nextRandomChange = settings.randomIntervalMinutes > 0
                                ? now + std::chrono::minutes(settings.randomIntervalMinutes)
                                : std::chrono::steady_clock::time_point::max();
                        } else {
                            trayOverrideGroupId = std::move(currentGroupId);
                            trayOverrideMediaId = std::move(nextId);
                        }
                    }
                    if (settings.displayMode != "primary") {
                        for (auto const& assignment : settings.displayAssignments) {
                            auto activeDisplay = std::find_if(displays.begin(), displays.end(),
                                [&](auto const& display) {
                                    return display.id == assignment.displayId;
                                });
                            if (activeDisplay == displays.end()) continue;
                            auto existing = trayDisplayOverrides.find(assignment.displayId);
                            auto assignedCurrent = existing != trayDisplayOverrides.end() &&
                                existing->second.first == assignment.groupId
                                ? existing->second.second : assignment.mediaId;
                            auto assignedNext = next_media_id(wallpapers, assignment.groupId,
                                assignedCurrent, playbackPerformanceMode);
                            if (!assignedNext.empty()) {
                                trayDisplayOverrides[assignment.displayId] = {
                                    assignment.groupId, std::move(assignedNext) };
                                advanced = true;
                            }
                        }
                    }
                    if (advanced) {
                        videoOptimizer->SetGenerationAllowed(false);
                        videoOptimizer->InvalidateChoices();
                        renderers.ResetAutoDecodeFailures();
                        performancePreviewStage = PerformancePreviewStage::Inactive;
                        performanceCopyWasRequired = false;
                    }
                }
                if (wasLocked) {
                    wasLocked = false;
                    autoLockTriggered = false;
                    displayOffAfterLockTriggered = false;
                    lockObservedAt = std::chrono::steady_clock::time_point::max();
                }

                auto groupId = trayOverrideMediaId.empty()
                    ? (randomId.empty() ? settings.selectedGroupId : settings.randomGroupId)
                    : trayOverrideGroupId;
                auto mediaId = trayOverrideMediaId.empty()
                    ? (randomId.empty() ? settings.selectedMediaId : randomId)
                    : trayOverrideMediaId;
                auto outputSettings = settings;
                for (auto& assignment : outputSettings.displayAssignments) {
                    auto override = trayDisplayOverrides.find(assignment.displayId);
                    if (override != trayDisplayOverrides.end() &&
                        override->second.first == assignment.groupId) {
                        assignment.mediaId = override->second.second;
                    }
                }
                auto outputs = display_media_targets(
                    wallpapers, outputSettings, groupId, mediaId, playbackPerformanceMode,
                    displays);
                for (auto& output : outputs) {
                    output.originalPlayback = settings.performanceMode == "original" &&
                        output.media.kind == "video";
                }
                if (softwarePlayback) {
                    for (auto& output : outputs) {
                        output.softwarePlaybackTarget = output.media.kind == "video";
                        if (output.softwarePlaybackTarget) {
                            output.playbackFrameRateCap =
                                motion::agent::software_playback_profile(true,
                                    logicalProcessors, output.targetWidth,
                                    output.targetHeight, output.targetRefreshRate).frameRate;
                            output.playbackFrameRateCap = motion::agent::video_cpu_frame_rate_cap(
                                output.playbackFrameRateCap);
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
                            if (motion::agent::defer_source_decode_probe(
                                    output.media.sourceBacked, output.performanceCopyRequired)) continue;
                            preferredAdapters[index] = display_adapter_key(output.deviceName);
                            auto routeAdapter = motion::agent::renderer_adapter_key(
                                preferredAdapters[index], output.deviceName);
                            probeRoutes.push_back({
                                motion::agent::renderer_media_key(output.media.path, output.media.kind),
                                output.deviceName, std::move(routeAdapter),
                                static_cast<uint64_t>(output.targetWidth) * output.targetHeight,
                                output.displayRefreshRate });
                        }
                        auto groupedProbeRoutes = motion::agent::group_renderer_routes(probeRoutes, false);
                        for (size_t index = 0; index < targets.size(); ++index) {
                            auto& output = targets[index];
                            if (output.media.kind != "video") continue;
                            if (motion::agent::defer_source_decode_probe(
                                    output.media.sourceBacked, output.performanceCopyRequired)) continue;
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
                            auto probe = videoOptimizer->SourceHardwareDecodeCandidates(
                                output.media.path, preferredAdapters[index], aggregateOutputPixels);
                            output.gpuProbePending = probe.pending;
                            output.decodeAdapter.clear();
                            output.hasAlternativeDecodeAdapter = false;
                            if (probe.pending) continue;
                            for (auto const& adapter : probe.adapters) {
                                if (renderers.AutoDecodeRouteRejected(output.media.path, adapter)) continue;
                                if (output.decodeAdapter.empty()) output.decodeAdapter = adapter;
                                else if (adapter != output.decodeAdapter) {
                                    output.hasAlternativeDecodeAdapter = true;
                                    break;
                                }
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
                if (outputs.empty()) trayControls.CancelScreensaverPreview();
                if (trayControls.ManuallyPaused() && !outputs.empty()) {
                    state = motion::agent::RuntimeAction::DesktopPaused;
                }
                if (trayControls.ScreensaverPreviewActive() && !outputs.empty()) {
                    state = motion::agent::RuntimeAction::ScreensaverPlay;
                }
                bool ownScreensaverActive = screensaverWasActive ||
                    state == motion::agent::RuntimeAction::ScreensaverPlay;
                auto autoLockIdle = autoLockIdleTimer.Update(uptime, input.tick, input.idle,
                    motion::agent::automatic_lock_idle_is_inhibited(
                        inhibition.display, inhibition.system, ownScreensaverActive));
                screensaverWasActive = state == motion::agent::RuntimeAction::ScreensaverPlay;
                runtimeEvents.SetScreensaverInputWakeEnabled(screensaverWasActive);
                // Explicit/import requests are durable background work. The
                // selected tier is never changed implicitly by power state;
                // current-wallpaper work is admitted separately only after a
                // static Renderer barrier below.
                bool powerSaverNeedsPriority = settings.performanceMode == "power-saver" ||
                    softwarePlayback || automaticCompatibilityPriority;
                bool playbackIdle = state != motion::agent::RuntimeAction::DesktopPlay &&
                    state != motion::agent::RuntimeAction::ScreensaverPlay;
                bool onBattery = runtimeEvents.OnBattery();
                bool backgroundGenerationAllowed = motion::agent::variant_generation_allowed(
                    onBattery, powerSaverNeedsPriority || !importedRequests.empty(), playbackIdle);
                auto resolveVideoOutputs = [&](bool onlySoftwareTargets = false,
                    bool allowGenerationRequest = false) {
                    using ResolveKey = std::tuple<std::wstring, bool, std::pair<uint32_t, uint32_t>>;
                    std::map<ResolveKey, OptimizationTarget> requiredVideoTargets;
                    for (auto const& output : outputs) {
                        if (output.media.kind != "video" || !output.media.sourceBacked) continue;
                        if (onlySoftwareTargets && !output.softwarePlaybackTarget) continue;
                        auto key = ResolveKey{ output.media.path.wstring(),
                            output.softwarePlaybackTarget,
                            motion::agent::video_display_aspect(output.targetWidth, output.targetHeight) };
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
                    for (auto& output : outputs) {
                        if (output.media.kind != "video" || !output.media.sourceBacked) continue;
                        if (onlySoftwareTargets && !output.softwarePlaybackTarget) continue;
                        auto source = output.media.path;
                        auto const required = requiredVideoTargets[{ source.wstring(),
                            output.softwarePlaybackTarget,
                            motion::agent::video_display_aspect(output.targetWidth, output.targetHeight) }];
                        auto resolved = videoOptimizer->ResolveWithLease(source, settings.performanceMode,
                            required.width, required.height, required.refreshRateHz,
                            output.softwarePlaybackTarget,
                            allowGenerationRequest);
                        output.performanceCopyRequired =
                            resolved.performanceCopyRequired;
                        output.performanceCopyPending =
                            resolved.performanceCopyPending;
                        output.gpuProbePending = resolved.gpuProbePending;
                        output.media.path = std::move(resolved.path);
                        output.media.playbackLease = std::move(resolved.lease);
                        output.media.sourceBacked = output.media.path == source;
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
                };
                resolveVideoOutputs();

                if (settings.decodeMode == "auto") {
                    // Resolve can switch codecs/profiles (for example H.264
                    // source -> HEVC Main10 balanced copy). Probe the exact
                    // file that Renderer will open, never the logical source's
                    // capabilities. Runtime driver failures are keyed to this
                    // same concrete path and adapter.
                    auto unsupportedRoutes = assignDecodeAdapters(outputs);
                    if (settings.performanceMode != "original" &&
                        !softwarePlayback && !unsupportedRoutes.empty()) {
                        automaticCompatibilityPriority = true;
                        auto cpuOutputs = display_media_targets(wallpapers, settings,
                            groupId, mediaId, std::string(motion::agent::cpu_smooth_mode),
                            displays);
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
                            // The selected balanced/power-saver route is being
                            // replaced by an internal cpu-smooth route. Do not
                            // carry its preview requirement across that policy
                            // boundary.
                            output.performanceCopyRequired = false;
                            output.performanceCopyPending = false;
                            output.softwarePlaybackTarget = true;
                            output.playbackFrameRateCap =
                                motion::agent::software_playback_profile(true,
                                    logicalProcessors, output.targetWidth,
                                    output.targetHeight, output.targetRefreshRate).frameRate;
                            output.playbackFrameRateCap = motion::agent::video_cpu_frame_rate_cap(
                                output.playbackFrameRateCap);
                        }
                        powerSaverNeedsPriority = true;
                        backgroundGenerationAllowed = motion::agent::variant_generation_allowed(
                            onBattery, true, playbackIdle);
                        resolveVideoOutputs(true);
                        // The CPU-friendly file may still be decoded and
                        // composed by a capable physical GPU. If it is not yet
                        // ready or no adapter supports it, automatic playback
                        // retains its CPU-decoder fallback.
                        assignDecodeAdapters(outputs);
                    } else if (!softwarePlayback) {
                        automaticCompatibilityPriority = false;
                    }
                }

                bool anyPlaybackCopyRequired = std::any_of(outputs.begin(),
                    outputs.end(), [](auto const& output) {
                        return output.performanceCopyRequired &&
                            output.media.sourceBacked;
                    });
                bool waitingForPerformanceCopy =
                    std::any_of(outputs.begin(), outputs.end(), [&](auto const& output) {
                        bool required = motion::agent::active_playback_waits_for_performance_copy(
                            output.performanceCopyRequired, output.media.sourceBacked);
                        return motion::agent::performance_copy_preview_required(state, required || output.gpuProbePending);
                    });
                std::vector<std::string> performanceCopyDisplayIds;
                if (waitingForPerformanceCopy) {
                    for (auto const& output : outputs) {
                        if ((output.performanceCopyRequired || output.gpuProbePending) &&
                            !output.displayId.empty()) {
                            performanceCopyDisplayIds.push_back(output.displayId);
                        }
                    }
                }

                if (waitingForPerformanceCopy && !performanceCopyWasRequired) {
                    // Establish the preview barrier without discarding an
                    // AC-powered task that survived lock or display-off.
                    // Source presentation still enforces quiescence below.
                    performancePreviewStage = renderers.HasActiveRoute()
                        ? PerformancePreviewStage::FreezePrevious
                        : PerformancePreviewStage::PresentPreview;
                } else if (!waitingForPerformanceCopy) {
                    performancePreviewStage = PerformancePreviewStage::Inactive;
                }
                performanceCopyWasRequired = waitingForPerformanceCopy;

                // Keep logical outputs unchanged for runtime identity and
                // error reporting. Only the concrete presentation route uses
                // the original video's poster after a real playback failure.
                auto playbackOutputs = outputs;
                std::vector<std::string> originalFailedDisplayIds;
                std::vector<std::string> originalMissingPosterDisplayIds;
                for (auto& output : playbackOutputs) {
                    if (renderers.OriginalPlaybackFailure(output).empty()) continue;
                    originalFailedDisplayIds.push_back(output.displayId);
                    auto poster = media_poster_by_id(wallpapers, output.groupId, output.mediaId, *videoOptimizer);
                    if (poster.path.empty()) {
                        originalMissingPosterDisplayIds.push_back(output.displayId);
                        continue;
                    }
                    output.media = std::move(poster);
                    output.decodeAdapter.clear();
                    output.softwarePlaybackTarget = false;
                    output.playbackFrameRateCap = 0;
                }
                playbackOutputs.erase(std::remove_if(playbackOutputs.begin(), playbackOutputs.end(),
                    [&](auto const& output) {
                        return std::find(originalMissingPosterDisplayIds.begin(), originalMissingPosterDisplayIds.end(),
                            output.displayId) != originalMissingPosterDisplayIds.end();
                    }), playbackOutputs.end());
                bool originalFailedRoutesStopped = originalFailedDisplayIds.empty() ||
                    renderers.StopVideoDisplays(originalFailedDisplayIds);

                auto performanceCopyPreviewOutputs = outputs;
                std::vector<std::string> compatibilityCopyStoppedDisplayIds;
                std::vector<std::string> compatibilityCopyNoPreviewDisplayIds;
                if (waitingForPerformanceCopy) {
                    for (auto& output : performanceCopyPreviewOutputs) {
                        if (!output.performanceCopyRequired && !output.gpuProbePending) continue;
                        auto poster = media_poster_by_id(
                            wallpapers, output.groupId, output.mediaId, *videoOptimizer);
                        bool posterAvailable = !poster.path.empty();
                        if ((motion::agent::compatibility_copy_requires_renderer_stop(
                                output.performanceCopyRequired || output.gpuProbePending,
                                output.softwarePlaybackTarget) || !posterAvailable) &&
                            !output.displayId.empty()) {
                            compatibilityCopyStoppedDisplayIds.push_back(output.displayId);
                        }
                        if (!poster.path.empty()) {
                            output.media = std::move(poster);
                            output.decodeAdapter.clear();
                            output.softwarePlaybackTarget = false;
                            output.playbackFrameRateCap = 0;
                        } else if (!output.displayId.empty()) {
                            // A missing poster is not a reason to initialize
                            // the expensive source decoder and cancel an
                            // in-flight encode. Windows keeps its own desktop
                            // background until the completed copy is available.
                            compatibilityCopyNoPreviewDisplayIds.push_back(output.displayId);
                        }
                    }
                    performanceCopyPreviewOutputs.erase(std::remove_if(
                        performanceCopyPreviewOutputs.begin(),
                        performanceCopyPreviewOutputs.end(), [&](auto const& output) {
                            return std::find(compatibilityCopyNoPreviewDisplayIds.begin(),
                                compatibilityCopyNoPreviewDisplayIds.end(),
                                output.displayId) !=
                                compatibilityCopyNoPreviewDisplayIds.end();
                        }), performanceCopyPreviewOutputs.end());
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
                bool optimizerQuiescenceBlocked{};
                auto sourcePresentationMayApply = [&](Renderer::Target target,
                    std::vector<DisplayMediaTarget> const& presentationOutputs,
                    bool freezePerformanceCopies = false) {
                    if (!originalFailedRoutesStopped) return false;
                    bool needsIdle = renderers.SourcePresentationNeedsOptimizerIdle(
                        target, presentationOutputs, settings.decodeMode,
                        settings.displayMode == "primary", freezePerformanceCopies);
                    bool idle = !needsIdle || videoOptimizer->Quiesce(
                        motion::agent::optimizer_quiesce_timeout_ms);
                    bool allowed = motion::agent::source_presentation_may_apply(
                        needsIdle, idle);
                    optimizerQuiescenceBlocked = !allowed;
                    return allowed;
                };
                auto trayStatus = motion::agent::TrayStatus::Applying;
                if (autoLockTriggered) {
                    targetReady = renderers.Stop();
                    runtimeEvents.SetScreensaverInputWakeEnabled(false);
                    trayControls.CancelScreensaverPreview();
                    trayStatus = motion::agent::TrayStatus::Locked;
                    waitMilliseconds = 1000;
                } else if (holdExistingRenderer) {
                    // Import/move publishes metadata and files in separate atomic
                    // steps. Preserve the last fully presented frame during that
                    // short transaction instead of exposing the Windows wallpaper.
                    trayStatus = motion::agent::TrayStatus::Applying;
                    waitMilliseconds = 50;
                } else if (waitingForPerformanceCopy) {
                    if (performancePreviewStage ==
                        PerformancePreviewStage::FreezePrevious) {
                        // Barrier 1: an old source route must acknowledge its
                        // freeze before a selected optimization can start. Do
                        // not rely on the later seamless-retirement path: a
                        // slow poster could otherwise overlap the whole encode.
                        bool compatibilityRoutesStopped = renderers.StopVideoDisplays(
                            compatibilityCopyStoppedDisplayIds);
                        std::vector<std::string> previewDisplayIds;
                        std::copy_if(performanceCopyDisplayIds.begin(),
                            performanceCopyDisplayIds.end(),
                            std::back_inserter(previewDisplayIds),
                            [&](auto const& displayId) {
                                return std::find(
                                    compatibilityCopyStoppedDisplayIds.begin(),
                                    compatibilityCopyStoppedDisplayIds.end(),
                                    displayId) ==
                                    compatibilityCopyStoppedDisplayIds.end();
                            });
                        if (compatibilityRoutesStopped &&
                            renderers.FreezeDisplays(previewDisplayIds)) {
                            performancePreviewStage =
                                PerformancePreviewStage::PresentPreview;
                        }
                        targetReady = false;
                        trayStatus = motion::agent::TrayStatus::Applying;
                        waitMilliseconds = motion::agent::responsive_wait_ms;
                    } else {
                        // Barrier 2: present available posters per pending
                        // display. Other routes keep their requested target.
                        bool compatibilityRoutesStopped = renderers.StopVideoDisplays(
                            compatibilityCopyStoppedDisplayIds);
                        auto previewTarget = state == motion::agent::RuntimeAction::ScreensaverPlay
                            ? Renderer::Target::ScreensaverPlay : Renderer::Target::DesktopPlay;
                        if (performanceCopyPreviewOutputs.empty()) {
                            targetReady = compatibilityRoutesStopped &&
                                renderers.RetiringRoutesStopped();
                        } else if (compatibilityRoutesStopped &&
                            sourcePresentationMayApply(
                                previewTarget,
                                performanceCopyPreviewOutputs, true)) {
                            renderers.Apply(previewTarget,
                                performanceCopyPreviewOutputs, settings.decodeMode,
                                settings.displayMode == "primary", true);
                            targetReady = renderers.TargetReady() &&
                                renderers.RetiringRoutesStopped();
                        }
                        if (targetReady) {
                            performancePreviewStage =
                                PerformancePreviewStage::Ready;
                        }
                        bool anyCopyPending = std::any_of(outputs.begin(),
                            outputs.end(), [](auto const& output) {
                                return output.performanceCopyPending || output.gpuProbePending;
                            });
                        trayStatus = !targetReady
                            ? motion::agent::TrayStatus::Applying
                            : anyCopyPending
                                ? motion::agent::TrayStatus::Optimizing
                                : motion::agent::TrayStatus::Frozen;
                        waitMilliseconds = anyCopyPending
                            ? motion::agent::performance_copy_wait_interval_ms(
                                targetReady || !renderers.TransitionPending())
                            : motion::agent::runtime_wait_interval_ms(
                                targetReady || !renderers.TransitionPending());
                    }
                } else {
                    switch (state) {
                    case motion::agent::RuntimeAction::ScreensaverPlay:
                        if (sourcePresentationMayApply(
                                Renderer::Target::ScreensaverPlay, playbackOutputs)) {
                            renderers.Apply(Renderer::Target::ScreensaverPlay,
                                playbackOutputs, settings.decodeMode,
                                settings.displayMode == "primary");
                            targetReady = renderers.TargetReady();
                        }
                        trayStatus = targetReady ? motion::agent::TrayStatus::Screensaver :
                            motion::agent::TrayStatus::Applying;
                        waitMilliseconds = motion::agent::runtime_wait_interval_ms(
                            targetReady || !renderers.TransitionPending());
                        break;
                    case motion::agent::RuntimeAction::DesktopPlay:
                        if (sourcePresentationMayApply(
                                Renderer::Target::DesktopPlay, playbackOutputs)) {
                            renderers.Apply(Renderer::Target::DesktopPlay,
                                playbackOutputs, settings.decodeMode,
                                settings.displayMode == "primary");
                            targetReady = renderers.TargetReady();
                        }
                        trayStatus = targetReady ? motion::agent::TrayStatus::Playing :
                            motion::agent::TrayStatus::Applying;
                        waitMilliseconds = motion::agent::runtime_wait_interval_ms(
                            targetReady || !renderers.TransitionPending());
                        break;
                    case motion::agent::RuntimeAction::DesktopFrozen:
                        if (sourcePresentationMayApply(
                                Renderer::Target::DesktopFreeze, playbackOutputs)) {
                            renderers.Apply(Renderer::Target::DesktopFreeze,
                                playbackOutputs, settings.decodeMode,
                                settings.displayMode == "primary");
                            targetReady = renderers.TargetReady();
                        }
                        trayStatus = targetReady ? motion::agent::TrayStatus::Frozen :
                            motion::agent::TrayStatus::Applying;
                        waitMilliseconds = motion::agent::runtime_wait_interval_ms(
                            targetReady || !renderers.TransitionPending());
                        break;
                    case motion::agent::RuntimeAction::DesktopPaused:
                        renderers.Pause();
                        targetReady = renderers.TargetReady() ||
                            (!renderers.HasActiveRoute() && renderers.RetiringRoutesStopped());
                        trayStatus = targetReady
                            ? motion::agent::TrayStatus::Paused
                            : motion::agent::TrayStatus::Applying;
                        waitMilliseconds = motion::agent::runtime_wait_interval_ms(
                            targetReady || !renderers.TransitionPending());
                        break;
                    default:
                        renderers.Stop();
                        trayStatus = motion::agent::TrayStatus::Stopped;
                        publishRuntime({}, {});
                        break;
                    }
                }
                bool previewBarrierReady = !waitingForPerformanceCopy ||
                    performancePreviewStage == PerformancePreviewStage::Ready;
                bool retiredRoutesStopped = renderers.RetiringRoutesStopped();
                bool rendererStaticForOptimization =
                    !optimizerQuiescenceBlocked &&
                    motion::agent::optimization_renderer_is_static(
                        state, waitingForPerformanceCopy, previewBarrierReady,
                        targetReady, renderers.HasActiveRoute(),
                        retiredRoutesStopped, holdExistingRenderer,
                        !renderers.HasRunningVideoRoute());
                bool currentWallpaperMayGenerate = anyPlaybackCopyRequired &&
                    rendererStaticForOptimization && !onBattery;
                bool optimizerMayRun = autoLockTriggered
                    ? motion::agent::hidden_session_generation_allowed(
                        motion::agent::RuntimeAction::Locked, onBattery, targetReady)
                    : rendererStaticForOptimization &&
                        (backgroundGenerationAllowed || currentWallpaperMayGenerate);
                videoOptimizer->SetGenerationAllowed(optimizerMayRun);
                if (optimizerMayRun) {
                    if (anyPlaybackCopyRequired && !autoLockTriggered) {
                        // The first Resolve published durable selected-tier
                        // requests without starting them. Re-resolve every
                        // source-backed output only after the static barrier;
                        // this queues balanced, power-saver and cpu-smooth
                        // using the same aggregate per-source target and also
                        // refreshes pending state in this pass.
                        resolveVideoOutputs(false, true);
                    }
                    prepareImported();
                }
                if (waitingForPerformanceCopy && targetReady && !autoLockTriggered) {
                    // Resolve/Prepare above may queue the current copy during
                    // this same pass. Publish it immediately instead of
                    // leaving tray/runtime one stable poll behind.
                    bool anyCopyPending = std::any_of(outputs.begin(), outputs.end(),
                        [](auto const& output) {
                            return output.performanceCopyPending;
                        });
                    trayStatus = anyCopyPending
                        ? motion::agent::TrayStatus::Optimizing
                        : motion::agent::TrayStatus::Frozen;
                    waitMilliseconds = anyCopyPending
                        ? motion::agent::performance_copy_wait_interval_ms(true)
                        : motion::agent::runtime_wait_interval_ms(true);
                }
                auto statusAction = autoLockTriggered
                    ? motion::agent::RuntimeAction::Locked : state;
                currentDisplayStates = renderers.RuntimeStates(displays, outputs,
                    statusAction, settings.displayMode == "primary",
                    trayControls.ManuallyPaused());
                if (optimizerQuiescenceBlocked) {
                    for (auto& display : currentDisplayStates) {
                        if (display.state == "failed") continue;
                        auto targeted = std::any_of(outputs.begin(), outputs.end(),
                            [&](auto const& output) {
                                return output.displayId == display.displayId;
                            });
                        if (!targeted) continue;
                        display.state = "applying";
                        display.reason = "optimizer-quiescence-timeout";
                    }
                } else if (holdExistingRenderer) {
                    for (auto& display : currentDisplayStates) {
                        if (display.state == "failed") continue;
                        display.state = "applying";
                        display.reason = "media-transaction";
                    }
                } else if (!autoLockTriggered && waitingForPerformanceCopy &&
                    performancePreviewStage ==
                        PerformancePreviewStage::FreezePrevious) {
                    for (auto& display : currentDisplayStates) {
                        bool requiresPreview = std::any_of(outputs.begin(),
                            outputs.end(), [&](auto const& output) {
                                return output.displayId == display.displayId &&
                                    output.performanceCopyRequired;
                            });
                        if (!requiresPreview || display.state == "paused" ||
                            display.state == "failed") continue;
                        display.state = "applying";
                        display.reason = "freezing-previous-route";
                    }
                }
                if (!autoLockTriggered && waitingForPerformanceCopy &&
                    performancePreviewStage == PerformancePreviewStage::Ready &&
                    !compatibilityCopyNoPreviewDisplayIds.empty()) {
                    for (auto& display : currentDisplayStates) {
                        if (std::find(compatibilityCopyNoPreviewDisplayIds.begin(),
                                compatibilityCopyNoPreviewDisplayIds.end(),
                                display.displayId) ==
                            compatibilityCopyNoPreviewDisplayIds.end()) continue;
                        auto output = std::find_if(outputs.begin(), outputs.end(),
                            [&](auto const& candidate) {
                                return candidate.displayId == display.displayId;
                            });
                        if (output == outputs.end()) continue;
                        display.state = output->performanceCopyPending
                            ? "optimizing" : "applying";
                        display.reason = output->performanceCopyPending
                            ? "compatibility-copy-pending"
                            : "compatibility-copy-waiting";
                        display.decodePath = "unavailable";
                        display.decodeReason = "compatibility-copy-required";
                        display.rendererProcessId = 0;
                        display.canRetry = false;
                        display.canRestartRenderer = false;
                    }
                }
                trayStatus = motion::agent::tray_status_with_renderer_health(
                    trayStatus, std::any_of(currentDisplayStates.begin(),
                        currentDisplayStates.end(), [](auto const& display) {
                            return display.state == "failed";
                        }));
                runtimeEvents.SetTrayStatus(trayStatus, trayControls.ManuallyPaused());
                bool originalFailureVisible = std::any_of(currentDisplayStates.begin(), currentDisplayStates.end(),
                    [](auto const& display) { return display.reason == "original-playback-failed"; });
                if (!autoLockTriggered && (originalFailureVisible ||
                    motion::agent::runtime_selection_can_publish(targetReady, waitingForPerformanceCopy))) {
                    auto decode = renderers.DecodeState();
                    if (!groupId.empty() && !mediaId.empty()) publishRuntime(groupId, mediaId, decode.path, decode.reason);
                    else if (!outputs.empty()) publishRuntime(outputs.front().groupId, outputs.front().mediaId, decode.path, decode.reason);
                }
            }
            auto decode = renderers.DecodeState();
            publishRuntime(publishedGroupId, publishedMediaId, decode.path, decode.reason,
                std::optional<std::vector<motion::DisplayRuntimeState>>(
                    std::move(currentDisplayStates)));
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
