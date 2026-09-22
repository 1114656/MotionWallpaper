#include "VideoTranscoder.h"

#include "../MotionWallpaper.Common/Common.h"

#include <windows.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace
{
    constexpr uint32_t vendorNvidia = 0x10de;
    constexpr uint32_t vendorIntel = 0x8086;
    constexpr uint32_t vendorAmd = 0x1002;
    constexpr uint64_t softwarePixelRateLimit =
        static_cast<uint64_t>(2560) * 1440 * 60;

    std::vector<motion::agent::VideoTranscodeAdapter> installed_adapters(bool& succeeded)
    {
        std::vector<motion::agent::VideoTranscodeAdapter> result;
        ComPtr<IDXGIFactory1> factory;
        succeeded = SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        if (!succeeded) return result;
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            auto status = factory->EnumAdapters1(index, &adapter);
            if (status == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(status)) {
                succeeded = false;
                result.clear();
                return result;
            }
            DXGI_ADAPTER_DESC1 description{};
            if (FAILED(adapter->GetDesc1(&description)) ||
                (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            result.push_back({ description.VendorId,
                static_cast<uint64_t>(description.DedicatedVideoMemory), index,
                description.AdapterLuid.HighPart, description.AdapterLuid.LowPart, true });
        }
        return result;
    }

    std::optional<uint32_t> current_dxgi_adapter_index(
        motion::agent::VideoTranscodeAdapter const& expected)
    {
        if (!expected.identityKnown) return std::nullopt;
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) {
            return std::nullopt;
        }
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            auto status = factory->EnumAdapters1(index, &adapter);
            if (status == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(status) || !adapter) return std::nullopt;
            DXGI_ADAPTER_DESC1 description{};
            if (FAILED(adapter->GetDesc1(&description))) continue;
            if (description.VendorId == expected.vendorId &&
                description.AdapterLuid.HighPart == expected.luidHigh &&
                description.AdapterLuid.LowPart == expected.luidLow) {
                return index;
            }
        }
        return std::nullopt;
    }

    uint32_t quality_target_bitrate_kbps(uint32_t width, uint32_t height, uint32_t targetFps)
    {
        // Bound VBR so a performance copy cannot grow without limit merely
        // because one vendor's constant-quality implementation is aggressive.
        auto pixelsPerSecond = static_cast<uint64_t>(width) * height * targetFps;
        auto estimate = static_cast<uint32_t>((pixelsPerSecond * 65 + 999'999) / 1'000'000);
        return std::clamp(estimate, 4'000u, 60'000u);
    }

    uint64_t probe_duration_100ns(fs::path const& source) noexcept
    {
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) return 0;
        struct MediaFoundationShutdown
        {
            ~MediaFoundationShutdown() { MFShutdown(); }
        } shutdown;

        ComPtr<IMFSourceReader> reader;
        if (FAILED(MFCreateSourceReaderFromURL(source.c_str(), nullptr, &reader))) return 0;
        PROPVARIANT duration{};
        PropVariantInit(&duration);
        auto status = reader->GetPresentationAttribute(
            static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration);
        uint64_t result{};
        if (SUCCEEDED(status)) {
            if (duration.vt == VT_UI8) result = duration.uhVal.QuadPart;
            else if (duration.vt == VT_I8 && duration.hVal.QuadPart > 0) {
                result = static_cast<uint64_t>(duration.hVal.QuadPart);
            }
        }
        PropVariantClear(&duration);
        return result;
    }

    void append_system_memory_input(std::vector<std::wstring>& arguments,
        fs::path const& source, uint32_t width, uint32_t height, uint32_t targetFps,
        std::wstring const& format)
    {
        arguments.insert(arguments.end(), {
            // This is an input codec option: cap frame-threaded decoding before
            // FFmpeg opens a high-frame-rate source. Together with
            // -filter_threads it prevents a background copy from consuming
            // nearly every logical processor while preserving full decoding.
            L"-threads:v", L"4", L"-i", source.wstring(),
            L"-map", L"0:v:0", L"-map_metadata", L"0", L"-an", L"-vf",
            L"fps=" + std::to_wstring(targetFps) + L",scale=" +
                std::to_wstring(width) + L":" + std::to_wstring(height) +
                L":flags=lanczos,format=" + format
        });
    }

    void append_hardware_upload_input(std::vector<std::wstring>& arguments,
        fs::path const& source, uint32_t width, uint32_t height, uint32_t targetFps,
        std::wstring const& hardwareType, std::wstring const& deviceName,
        uint32_t dxgiAdapterIndex, std::wstring const& format, bool hardwareDecode)
    {
        std::wstring specification;
        if (hardwareType == L"qsv") {
            specification = hardwareType + L"=" + deviceName +
                L":,child_device=" + std::to_wstring(dxgiAdapterIndex) +
                L",child_device_type=d3d11va";
        } else {
            specification = hardwareType + L"=" + deviceName + L":" +
                std::to_wstring(dxgiAdapterIndex);
        }
        arguments.insert(arguments.end(), {
            L"-init_hw_device", std::move(specification),
            L"-filter_hw_device", deviceName
        });
        if (hardwareDecode) {
            // NVENC/AMF use the exact named D3D11 device. QSV uses the exact
            // QSV device derived from its selected D3D11 child. Download only
            // after fps has discarded excess presentation frames, then retain
            // the proven Lanczos scaler and upload to the bound encoder.
            arguments.insert(arguments.end(), {
                L"-hwaccel", hardwareType == L"qsv" ? L"qsv" : L"d3d11va",
                L"-hwaccel_device", deviceName,
                L"-hwaccel_output_format", hardwareType == L"qsv" ? L"qsv" : L"d3d11"
            });
        } else {
            arguments.insert(arguments.end(), { L"-threads:v", L"4" });
        }
        arguments.insert(arguments.end(), {
            L"-i", source.wstring(), L"-map", L"0:v:0", L"-map_metadata", L"0", L"-an", L"-vf"
        });
        auto filter = L"fps=" + std::to_wstring(targetFps);
        if (hardwareDecode) filter += L",hwdownload,format=" + format;
        filter += L",scale=" + std::to_wstring(width) + L":" + std::to_wstring(height) +
            L":flags=lanczos,format=" + format + L",hwupload";
        arguments.push_back(std::move(filter));
    }

    void append_bounded_rate(std::vector<std::wstring>& arguments,
        motion::agent::VideoTranscodeRateControl const& rate)
    {
        arguments.insert(arguments.end(), {
            L"-b:v", std::to_wstring(rate.averageKbps) + L"k",
            L"-maxrate", std::to_wstring(rate.maximumKbps) + L"k",
            L"-bufsize", std::to_wstring(rate.bufferKbps) + L"k"
        });
    }

    std::vector<std::wstring> transcode_arguments(
        fs::path const& ffmpeg,
        fs::path const& source,
        fs::path const& destination,
        uint32_t width,
        uint32_t height,
        uint32_t targetFps,
        motion::agent::VideoTranscodeCandidate const& candidate,
        motion::agent::VideoTranscodeCodec codec,
        motion::agent::VideoTranscodeRateControl const& rate,
        bool hardwareDecode)
    {
        using motion::agent::VideoTranscodeBackend;
        using motion::agent::VideoTranscodeCodec;
        std::vector<std::wstring> arguments{
            ffmpeg.wstring(), L"-nostdin", L"-hide_banner", L"-loglevel", L"info", L"-nostats",
            L"-stats_period", L"0.5", L"-progress", L"pipe:1", L"-filter_threads", L"2", L"-y"
        };
        bool h264 = codec == VideoTranscodeCodec::H264;
        auto backend = candidate.backend;
        switch (backend) {
        case VideoTranscodeBackend::nvidiaNvenc:
            append_hardware_upload_input(arguments, source, width, height, targetFps,
                L"d3d11va", L"mwm_nvenc", candidate.adapter.dxgiAdapterIndex,
                h264 ? L"nv12" : L"p010le", hardwareDecode);
            arguments.insert(arguments.end(), {
                L"-c:v", h264 ? L"h264_nvenc" : L"hevc_nvenc",
                L"-preset", L"p5", L"-tune", L"hq",
                L"-profile:v", h264 ? L"high" : L"main10", L"-rc", L"vbr", L"-cq", L"21",
                L"-spatial-aq", L"1", L"-temporal-aq", L"1"
            });
            append_bounded_rate(arguments, rate);
            break;
        case VideoTranscodeBackend::intelQsv:
            append_hardware_upload_input(arguments, source, width, height, targetFps,
                L"qsv", L"mwm_qsv", candidate.adapter.dxgiAdapterIndex,
                h264 ? L"nv12" : L"p010le", hardwareDecode);
            arguments.insert(arguments.end(), {
                L"-c:v", h264 ? L"h264_qsv" : L"hevc_qsv",
                L"-preset", L"slow", L"-profile:v", h264 ? L"high" : L"main10",
                L"-scenario", L"archive", L"-global_quality", L"21"
            });
            append_bounded_rate(arguments, rate);
            break;
        case VideoTranscodeBackend::amdAmf:
            append_hardware_upload_input(arguments, source, width, height, targetFps,
                L"d3d11va", L"mwm_amf", candidate.adapter.dxgiAdapterIndex,
                h264 ? L"nv12" : L"p010le", hardwareDecode);
            arguments.insert(arguments.end(), {
                L"-c:v", h264 ? L"h264_amf" : L"hevc_amf",
                L"-usage", L"high_quality", L"-quality", L"quality",
                L"-profile:v", h264 ? L"high" : L"main10"
            });
            if (!h264) arguments.insert(arguments.end(), { L"-bitdepth", L"10" });
            arguments.insert(arguments.end(), {
                L"-rc", L"qvbr", L"-qvbr_quality_level", L"21", L"-vbaq", L"1"
            });
            append_bounded_rate(arguments, rate);
            break;
        case VideoTranscodeBackend::softwareOpenH264:
            append_system_memory_input(arguments, source, width, height, targetFps, L"yuv420p");
            arguments.insert(arguments.end(), {
                L"-c:v", L"libopenh264", L"-profile:v", L"high", L"-threads", L"2"
            });
            append_bounded_rate(arguments, rate);
            break;
        }
        arguments.insert(arguments.end(), {
            L"-r", std::to_wstring(targetFps), L"-fps_mode", L"cfr", L"-tag:v",
            h264 ? L"avc1" : L"hvc1",
            L"-movflags", L"+faststart", destination.wstring()
        });
        return arguments;
    }

    uint64_t clock_microseconds(std::string_view value) noexcept
    {
        unsigned hours{}, minutes{};
        double seconds{};
        std::string text(value);
        if (sscanf_s(text.c_str(), "%u:%u:%lf", &hours, &minutes, &seconds) != 3 ||
            minutes >= 60 || !std::isfinite(seconds) || seconds < 0 || seconds >= 60) return 0;
        auto total = (static_cast<long double>(hours) * 3600.0L +
            static_cast<long double>(minutes) * 60.0L + seconds) * 1'000'000.0L;
        return total > 0 && total <= static_cast<long double>(UINT64_MAX)
            ? static_cast<uint64_t>(total) : 0;
    }

    class FfmpegProgressReader
    {
    public:
        FfmpegProgressReader(motion::agent::VideoTranscodeProgressCallback const& callback,
            motion::agent::VideoTranscodeBackend backend, uint32_t attempt,
            uint64_t fallbackDurationMicroseconds) noexcept
            : callback_(callback), backend_(backend), attempt_(attempt),
              durationMicroseconds_(fallbackDurationMicroseconds)
        {
            Publish(0, true);
        }

        void Feed(std::string_view bytes)
        {
            buffer_.append(bytes);
            for (;;) {
                auto newline = buffer_.find_first_of("\r\n");
                if (newline == std::string::npos) break;
                auto line = buffer_.substr(0, newline);
                auto next = newline + 1;
                while (next < buffer_.size() &&
                    (buffer_[next] == '\r' || buffer_[next] == '\n')) ++next;
                buffer_.erase(0, next);
                ParseLine(line);
            }
            // FFmpeg output is bounded by -nostats, but retain a defensive cap
            // in case a future backend emits a malformed unterminated line.
            if (buffer_.size() > 64 * 1024) buffer_.erase(0, buffer_.size() - 4096);
        }

        void Finish()
        {
            if (!buffer_.empty()) ParseLine(buffer_);
            buffer_.clear();
        }

        [[nodiscard]] uint64_t DurationMicroseconds() const noexcept
        {
            return durationMicroseconds_;
        }

        [[nodiscard]] uint64_t FurthestProcessedMicroseconds() const noexcept
        {
            return furthestProcessedMicroseconds_;
        }

    private:
        void ParseLine(std::string_view line)
        {
            auto duration = line.find("Duration: ");
            if (duration != std::string_view::npos) {
                auto begin = duration + std::string_view("Duration: ").size();
                auto end = line.find(',', begin);
                auto parsed = clock_microseconds(line.substr(begin,
                    end == std::string_view::npos ? end : end - begin));
                // Prefer the demuxer's own duration over the Media Foundation
                // fallback once FFmpeg has opened the exact input it will encode.
                if (parsed) durationMicroseconds_ = parsed;
                return;
            }
            constexpr std::string_view outTimePrefix = "out_time_us=";
            if (line.starts_with(outTimePrefix)) {
                int64_t processed{};
                auto value = line.substr(outTimePrefix.size());
                auto parsed = std::from_chars(value.data(), value.data() + value.size(), processed);
                if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()) {
                    auto timeline = processed > 0 ? static_cast<uint64_t>(processed) : 0;
                    furthestProcessedMicroseconds_ = (std::max)(
                        furthestProcessedMicroseconds_, timeline);
                    Publish(timeline, false);
                }
            }
        }

        void Publish(uint64_t processedMicroseconds, bool attemptStarted) noexcept
        {
            if (!callback_) return;
            motion::agent::VideoTranscodeProgress progress;
            progress.processedMicroseconds = processedMicroseconds;
            progress.durationMicroseconds = durationMicroseconds_;
            progress.attempt = attempt_;
            progress.backend = backend_;
            progress.attemptStarted = attemptStarted;
            progress.determinate = durationMicroseconds_ != 0;
            if (progress.determinate) {
                auto ratio = static_cast<long double>((std::min)(processedMicroseconds,
                    durationMicroseconds_)) / static_cast<long double>(durationMicroseconds_);
                // 100 means the complete output has passed all process/file
                // checks. Timeline samples remain at 99 until that point.
                progress.percent = (std::min)(99u,
                    static_cast<uint32_t>(ratio * 100.0L));
            }
            try { callback_(progress); } catch (...) {}
        }

        motion::agent::VideoTranscodeProgressCallback const& callback_;
        motion::agent::VideoTranscodeBackend backend_;
        uint32_t attempt_{};
        uint64_t durationMicroseconds_{};
        uint64_t furthestProcessedMicroseconds_{};
        std::string buffer_;
    };

    motion::agent::VideoTranscodeResult run_ffmpeg(
        fs::path const& ffmpeg,
        std::vector<std::wstring> const& arguments,
        std::function<motion::agent::VideoTranscodeControl()> const& control,
        std::wstring& error,
        motion::agent::VideoTranscodeBackend backend,
        uint32_t attempt,
        uint64_t fallbackDurationMicroseconds,
        motion::agent::VideoTranscodeProgressCallback const& progress,
        uint64_t& resolvedDurationMicroseconds,
        bool& stalled)
    {
        using namespace motion::agent;
        using SteadyClock = std::chrono::steady_clock;
        stalled = false;
        auto command = motion::build_command_line(arguments);

        SECURITY_ATTRIBUTES inheritable{ sizeof(inheritable), nullptr, TRUE };
        HANDLE readRaw{}, writeRaw{};
        if (!CreatePipe(&readRaw, &writeRaw, &inheritable, 0)) {
            error = L"无法创建 FFmpeg 进度管道";
            return VideoTranscodeResult::failed;
        }
        motion::unique_handle progressRead{ readRaw };
        motion::unique_handle progressWrite{ writeRaw };
        if (!SetHandleInformation(progressRead.get(), HANDLE_FLAG_INHERIT, 0)) {
            error = L"无法初始化 FFmpeg 进度管道";
            return VideoTranscodeResult::failed;
        }
        motion::unique_handle nullInput{ CreateFileW(L"NUL", GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr) };
        if (!nullInput) {
            error = L"无法初始化 FFmpeg 标准输入";
            return VideoTranscodeResult::failed;
        }

        STARTUPINFOW startup{ sizeof(startup) };
        startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
        startup.wShowWindow = SW_HIDE;
        startup.hStdInput = nullInput.get();
        startup.hStdOutput = progressWrite.get();
        startup.hStdError = progressWrite.get();
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(ffmpeg.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS,
            nullptr, ffmpeg.parent_path().c_str(), &startup, &process)) {
            error = L"无法启动 FFmpeg 优化后端";
            return VideoTranscodeResult::failed;
        }
        progressWrite.reset();
        nullInput.reset();
        motion::unique_handle processHandle{ process.hProcess };
        motion::unique_handle threadHandle{ process.hThread };
        motion::unique_handle job{ CreateJobObjectW(nullptr, nullptr) };
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
                !AssignProcessToJobObject(job.get(), processHandle.get())) {
                job.reset();
            }
        }

        PROCESS_POWER_THROTTLING_STATE powerThrottling{};
        powerThrottling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        powerThrottling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        powerThrottling.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        SetProcessInformation(processHandle.get(), ProcessPowerThrottling,
            &powerThrottling, sizeof(powerThrottling));
        MEMORY_PRIORITY_INFORMATION memoryPriority{ MEMORY_PRIORITY_LOW };
        SetProcessInformation(processHandle.get(), ProcessMemoryPriority,
            &memoryPriority, sizeof(memoryPriority));

        // A cancelled optimization must not report quiescence while FFmpeg can
        // still hold the source or the temporary destination open. Direct
        // termination is normally enough; the job is the second line of
        // defence. If Windows cannot immediately confirm either one, remain
        // fail-closed and keep retrying until the child is observably gone.
        auto stopAndConfirm = [&](DWORD requestedExitCode) noexcept {
            auto wait = WaitForSingleObject(processHandle.get(), 0);
            if (wait == WAIT_OBJECT_0) return;

            TerminateProcess(processHandle.get(), requestedExitCode);
            wait = WaitForSingleObject(processHandle.get(), 5000);
            if (wait == WAIT_OBJECT_0) return;

            // Closing a configured kill-on-close job terminates the complete
            // child process tree. It is harmless when job assignment failed.
            job.reset();
            for (;;) {
                wait = WaitForSingleObject(processHandle.get(), 1000);
                if (wait == WAIT_OBJECT_0) return;

                DWORD exitCode{};
                if (GetExitCodeProcess(processHandle.get(), &exitCode) &&
                    exitCode != STILL_ACTIVE) {
                    return;
                }
                TerminateProcess(processHandle.get(), requestedExitCode);
                if (wait != WAIT_TIMEOUT) Sleep(1000);
            }
        };
        if (ResumeThread(threadHandle.get()) == static_cast<DWORD>(-1)) {
            stopAndConfirm(ERROR_PROCESS_ABORTED);
            error = L"无法启动 FFmpeg 优化任务";
            return VideoTranscodeResult::failed;
        }

        FfmpegProgressReader progressReader(progress, backend, attempt,
            fallbackDurationMicroseconds);
        auto drainProgress = [&] {
            bool readAny{};
            for (;;) {
                DWORD available{};
                if (!PeekNamedPipe(progressRead.get(), nullptr, 0, nullptr, &available, nullptr) ||
                    !available) return readAny;
                char bytes[4096];
                DWORD read{};
                if (!ReadFile(progressRead.get(), bytes, (std::min)(available,
                    static_cast<DWORD>(sizeof(bytes))), &read, nullptr) || !read) return readAny;
                readAny = true;
                progressReader.Feed(std::string_view(bytes, read));
            }
        };

        DWORD waitResult{};
        auto lastPipeActivity = SteadyClock::now();
        auto lastTimelineAdvance = lastPipeActivity;
        uint64_t furthestTimeline{};
        bool hardwareBackend = backend != VideoTranscodeBackend::softwareOpenH264;
        auto pipeIdleTimeout = hardwareBackend
            ? std::chrono::seconds(30) : std::chrono::seconds(120);
        auto timelineStallTimeout = hardwareBackend
            ? std::chrono::seconds(90) : std::chrono::minutes(5);
        try {
            for (;;) {
                auto pipeActivity = drainProgress();
                auto now = SteadyClock::now();
                if (pipeActivity) lastPipeActivity = now;
                auto currentTimeline = progressReader.FurthestProcessedMicroseconds();
                if (currentTimeline > furthestTimeline) {
                    furthestTimeline = currentTimeline;
                    lastTimelineAdvance = now;
                }
                waitResult = WaitForSingleObject(processHandle.get(), 100);
                if (waitResult != WAIT_TIMEOUT) break;
                auto state = control();
                if (state != VideoTranscodeControl::running) {
                    stopAndConfirm(ERROR_CANCELLED);
                    drainProgress();
                    progressReader.Finish();
                    resolvedDurationMicroseconds = progressReader.DurationMicroseconds();
                    return state == VideoTranscodeControl::paused
                        ? VideoTranscodeResult::paused : VideoTranscodeResult::cancelled;
                }
                // A healthy slow encode advances out_time regularly. Pipe
                // activity additionally covers demux/device initialization.
                // Bound both silence and endlessly-chatty zero-progress hangs
                // so a wedged hardware driver cannot pin the optimizer forever.
                if (now - lastPipeActivity >= pipeIdleTimeout ||
                    now - lastTimelineAdvance >= timelineStallTimeout) {
                    stopAndConfirm(ERROR_TIMEOUT);
                    drainProgress();
                    progressReader.Finish();
                    resolvedDurationMicroseconds = progressReader.DurationMicroseconds();
                    stalled = true;
                    error = video_transcode_backend_name(backend) +
                        L" 长时间没有编码进度，已终止并尝试兼容后端";
                    return VideoTranscodeResult::unsupported;
                }
            }
        } catch (...) {
            stopAndConfirm(ERROR_PROCESS_ABORTED);
            throw;
        }
        drainProgress();
        progressReader.Finish();
        resolvedDurationMicroseconds = progressReader.DurationMicroseconds();
        if (waitResult != WAIT_OBJECT_0) {
            stopAndConfirm(ERROR_PROCESS_ABORTED);
            error = L"无法等待 FFmpeg 优化任务";
            return VideoTranscodeResult::failed;
        }
        DWORD exitCode{};
        if (!GetExitCodeProcess(processHandle.get(), &exitCode)) {
            error = L"无法读取 FFmpeg 优化任务状态";
            return VideoTranscodeResult::failed;
        }
        return exitCode == 0 ? VideoTranscodeResult::succeeded : VideoTranscodeResult::unsupported;
    }
}

namespace motion::agent
{
    VideoTranscodeRateControl video_transcode_rate_control(
        uint32_t width,
        uint32_t height,
        uint32_t targetFps,
        VideoTranscodeCodec codec,
        uint64_t sourceSizeBytes,
        uint64_t sourceDuration100ns) noexcept
    {
        auto average = quality_target_bitrate_kbps(width, height, targetFps);
        if (codec == VideoTranscodeCodec::H264) {
            // H.264 needs moderately more data than HEVC for comparable
            // detail, but remains subject to the source-relative budget below.
            average = (std::min)(60'000u, (average * 4u + 2u) / 3u);
        }
        auto maximum = average + average / 2;
        auto buffer = average <= UINT32_MAX / 2 ? average * 2 : UINT32_MAX;

        uint64_t outputLimit{};
        if (sourceSizeBytes) {
            auto growth = (std::max)(64'000'000ULL, sourceSizeBytes / 5);
            outputLimit = sourceSizeBytes > UINT64_MAX - growth
                ? UINT64_MAX : sourceSizeBytes + growth;
        }
        if (outputLimit && sourceDuration100ns) {
            // Keep an explicit mux/VBR margin, then make the instantaneous
            // max-rate fit beneath that ceiling. The average target uses 90%
            // of the ceiling so CQ/QVBR backends cannot consume the entire
            // acceptance budget through ordinary rate-control variance.
            auto reserve = (std::max)(1'000'000ULL, outputLimit / 32);
            auto payloadBudget = outputLimit > reserve ? outputLimit - reserve : outputLimit;
            auto ceilingValue = static_cast<long double>(payloadBudget) * 80'000.0L /
                static_cast<long double>(sourceDuration100ns);
            auto ceiling = static_cast<uint32_t>((std::max)(1.0L,
                (std::min)(ceilingValue, static_cast<long double>(UINT32_MAX))));
            auto safeAverage = (std::max)(1u, ceiling - ceiling / 10);
            average = (std::min)(average, safeAverage);
            maximum = (std::max)(average, (std::min)(maximum, ceiling));
            buffer = average <= UINT32_MAX / 2 ? average * 2 : UINT32_MAX;
        }
        return { average, maximum, buffer, outputLimit };
    }

    std::vector<VideoTranscodeCandidate> video_transcode_backend_order(
        std::vector<VideoTranscodeAdapter> adapters,
        uint32_t width,
        uint32_t height,
        uint32_t targetFps,
        bool adapterProbeSucceeded,
        bool softwareFallbackAllowed,
        bool softwarePlaybackTarget)
    {
        auto pixelRate = static_cast<uint64_t>(width) * height * targetFps;
        auto longEdge = (std::max)(width, height);
        auto shortEdge = (std::min)(width, height);
        if (softwarePlaybackTarget) {
            // Compatibility copies are always bounded, broadly decodable
            // H.264. Prefer a precisely bound hardware encoder when one is
            // available, but retain OpenH264 as the final machine-independent
            // fallback. Software decoding is still attempted with each
            // hardware encoder if the source itself cannot use that GPU.
            if (!softwareFallbackAllowed || !width || !height || !targetFps ||
                pixelRate > static_cast<uint64_t>(1920) * 1080 * 60 ||
                longEdge > 1920 || shortEdge > 1080 || targetFps > 60) {
                return {};
            }
        }

        std::stable_sort(adapters.begin(), adapters.end(), [](auto const& left, auto const& right) {
            return left.dedicatedVideoMemory > right.dedicatedVideoMemory;
        });
        std::vector<VideoTranscodeCandidate> result;
        auto appendAdapter = [&](VideoTranscodeAdapter const& adapter) {
            // FFmpeg's CUDA/NVENC ordinal is not a DXGI LUID and can select a
            // different same-vendor GPU. Feed an upload surface created on the
            // exact DXGI adapter instead. QSV is likewise created from its
            // documented DirectX adapter index. Unknown identities are never
            // sent to a hardware encoder on a guess.
            if (!adapter.identityKnown) return;
            if (adapter.vendorId == vendorNvidia) {
                result.push_back({ VideoTranscodeBackend::nvidiaNvenc, adapter, true });
            } else if (adapter.vendorId == vendorIntel) {
                result.push_back({ VideoTranscodeBackend::intelQsv, adapter, true });
            } else if (adapter.vendorId == vendorAmd) {
                result.push_back({ VideoTranscodeBackend::amdAmf, adapter, true });
            }
        };
        if (adapterProbeSucceeded) {
            for (auto const& adapter : adapters) appendAdapter(adapter);
        }

        if (softwareFallbackAllowed && width && height && targetFps && pixelRate <= softwarePixelRateLimit &&
            longEdge <= 2560 && shortEdge <= 1440 && targetFps <= 60) {
            // The software fallback must remain playable on stock Windows
            // systems that do not have an optional HEVC decoder installed.
            result.push_back({ VideoTranscodeBackend::softwareOpenH264, {}, false });
        }
        return result;
    }

    std::wstring video_transcode_backend_name(VideoTranscodeBackend backend)
    {
        switch (backend) {
        case VideoTranscodeBackend::nvidiaNvenc: return L"NVIDIA NVENC";
        case VideoTranscodeBackend::intelQsv: return L"Intel Quick Sync";
        case VideoTranscodeBackend::amdAmf: return L"AMD AMF";
        case VideoTranscodeBackend::softwareOpenH264: return L"软件 H.264（OpenH264）";
        }
        return L"未知后端";
    }

    bool video_candidate_decodes_first_frame(fs::path const& candidate) noexcept
    {
        try {
            auto decodesAs = [&](GUID const& subtype, bool enableVideoProcessing) {
                ComPtr<IMFAttributes> attributes;
                if (FAILED(MFCreateAttributes(&attributes, 2))) return false;
                if (FAILED(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE))) {
                    return false;
                }
                if (enableVideoProcessing && FAILED(attributes->SetUINT32(
                    MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE))) {
                    return false;
                }

                ComPtr<IMFSourceReader> reader;
                if (FAILED(MFCreateSourceReaderFromURL(candidate.c_str(), attributes.Get(), &reader))) {
                    return false;
                }
                if (FAILED(reader->SetStreamSelection(
                        static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE)) ||
                    FAILED(reader->SetStreamSelection(
                        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE))) {
                    return false;
                }

                ComPtr<IMFMediaType> outputType;
                if (FAILED(MFCreateMediaType(&outputType)) ||
                    FAILED(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
                    FAILED(outputType->SetGUID(MF_MT_SUBTYPE, subtype)) ||
                    FAILED(reader->SetCurrentMediaType(
                        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr,
                        outputType.Get()))) {
                    return false;
                }

                // A valid candidate must yield a real decoded frame. Bound the
                // loop so malformed streams made only of ticks/type changes can
                // never stall variant generation indefinitely.
                for (uint32_t attempt = 0; attempt < 64; ++attempt) {
                    DWORD flags{};
                    ComPtr<IMFSample> sample;
                    auto status = reader->ReadSample(
                        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                        nullptr, &flags, nullptr, &sample);
                    if (FAILED(status) || (flags & MF_SOURCE_READERF_ERROR)) return false;
                    if (sample) {
                        DWORD buffers{};
                        if (SUCCEEDED(sample->GetBufferCount(&buffers)) && buffers) return true;
                    }
                    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return false;
                }
                return false;
            };

            // NV12 covers stock 8-bit H.264/HEVC output. P010 preserves a
            // Main10 decoder path, while RGB32 gives the source reader one
            // final presentation-friendly conversion route.
            return decodesAs(MFVideoFormat_NV12, false) ||
                decodesAs(MFVideoFormat_P010, false) ||
                decodesAs(MFVideoFormat_ARGB32, true);
        } catch (...) {
            return false;
        }
    }

    VideoTranscodeResult transcode_video(
        fs::path const& ffmpeg,
        fs::path const& source,
        fs::path const& destination,
        uint32_t width,
        uint32_t height,
        uint32_t targetFps,
        std::function<VideoTranscodeControl()> const& control,
        std::wstring& error,
        std::wstring* selectedBackend,
        bool softwareFallbackAllowed,
        bool softwarePlaybackTarget,
        uint64_t sourceDuration100ns,
        VideoTranscodeProgressCallback const& progress,
        VideoTranscodeCandidateValidator const& validateCandidate,
        VideoTranscodeCodec* selectedCodec,
        VideoTranscodePathAccess const& pathAccess)
    {
        if (selectedBackend) selectedBackend->clear();
        std::error_code fileError;
        if (!fs::is_regular_file(ffmpeg, fileError)) {
            error = L"FFmpeg 优化后端未安装: " + ffmpeg.wstring();
            return VideoTranscodeResult::unsupported;
        }
        if (!width || !height || !targetFps) {
            error = L"无法验证优化副本规格";
            return VideoTranscodeResult::failed;
        }
        fileError.clear();
        auto sourceSize = fs::file_size(source, fileError);
        if (fileError || !sourceSize) {
            error = L"无法读取源视频大小";
            return VideoTranscodeResult::failed;
        }
        auto effectiveDuration100ns = sourceDuration100ns
            ? sourceDuration100ns : probe_duration_100ns(source);

        bool adapterProbeSucceeded{};
        auto adapters = installed_adapters(adapterProbeSucceeded);
        auto candidates = video_transcode_backend_order(std::move(adapters),
            width, height, targetFps, adapterProbeSucceeded, softwareFallbackAllowed,
            softwarePlaybackTarget);
        if (candidates.empty()) {
            if (!adapterProbeSucceeded) {
                error = L"无法可靠读取显卡 DXGI/LUID 身份；已跳过无法安全绑定设备的 NVENC/QSV/AMF";
            } else {
                error = softwarePlaybackTarget
                    ? L"该视频无法安全转换为 CPU 流畅副本"
                    : L"没有可用的硬件编码器；该规格超过软件编码的安全上限";
            }
            return VideoTranscodeResult::unsupported;
        }

        std::wstring attemptedBackends;
        uint32_t attempt{};
        for (auto const& candidate : candidates) {
            auto backend = candidate.backend;
            auto codec = video_transcode_backend_codec(backend, softwareFallbackAllowed);
            auto backendName = video_transcode_backend_name(backend);
            if (backend != VideoTranscodeBackend::softwareOpenH264) {
                backendName += codec == VideoTranscodeCodec::H264
                    ? L" H.264" : L" HEVC Main10";
                backendName += L"（LUID " +
                    std::to_wstring(candidate.adapter.luidHigh) + L":" +
                    std::to_wstring(candidate.adapter.luidLow) + L"）";
            } else if (!adapterProbeSucceeded) {
                backendName += L"（显卡身份探测失败，已安全降级）";
            }
            if (!attemptedBackends.empty()) attemptedBackends += L"、";
            attemptedBackends += backendName;

            bool hardwareEncoder = backend != VideoTranscodeBackend::softwareOpenH264;
            auto decodeAttempts = hardwareEncoder ? 2u : 1u;
            for (uint32_t decodeAttempt = 0; decodeAttempt < decodeAttempts; ++decodeAttempt) {
                bool hardwareDecode = hardwareEncoder && decodeAttempt == 0;
                auto state = control();
                if (state != VideoTranscodeControl::running) {
                    return state == VideoTranscodeControl::paused
                        ? VideoTranscodeResult::paused : VideoTranscodeResult::cancelled;
                }
                auto boundCandidate = candidate;
                if (boundCandidate.adapterBound) {
                    // Re-resolve immediately before every actual process start;
                    // the DXGI ordinal may change between a fast hardware-
                    // decode rejection and the bounded compatibility attempt.
                    auto currentIndex = current_dxgi_adapter_index(candidate.adapter);
                    if (!currentIndex) break;
                    boundCandidate.adapter.dxgiAdapterIndex = *currentIndex;
                }
                std::shared_ptr<void> attemptAccess;
                if (pathAccess) {
                    attemptAccess = pathAccess();
                    if (!attemptAccess) return VideoTranscodeResult::cancelled;
                }

                auto rate = video_transcode_rate_control(width, height, targetFps, codec,
                    sourceSize, effectiveDuration100ns);
                fileError.clear();
                fs::remove(destination, fileError);
                auto arguments = transcode_arguments(ffmpeg, source, destination, width, height,
                    targetFps, boundCandidate, codec, rate, hardwareDecode);
                auto resolvedDurationMicroseconds = effectiveDuration100ns / 10;
                bool stalled{};
                ++attempt;
                auto result = run_ffmpeg(ffmpeg, arguments, control, error, backend, attempt,
                    resolvedDurationMicroseconds, progress, resolvedDurationMicroseconds, stalled);
                if (!effectiveDuration100ns && resolvedDurationMicroseconds <= UINT64_MAX / 10) {
                    effectiveDuration100ns = resolvedDurationMicroseconds * 10;
                }
                // A removable drive may disappear while FFmpeg owns its files.
                // Do not let cleanup land on a different volume mounted at the
                // same path after this attempt released its child process.
                std::shared_ptr<void> postAttemptAccess;
                if (pathAccess) {
                    postAttemptAccess = pathAccess();
                    if (!postAttemptAccess) return VideoTranscodeResult::cancelled;
                }
                if (result == VideoTranscodeResult::paused ||
                    result == VideoTranscodeResult::cancelled ||
                    result == VideoTranscodeResult::failed) {
                    fs::remove(destination, fileError);
                    return result;
                }
                if (result != VideoTranscodeResult::succeeded) {
                    fs::remove(destination, fileError);
                    // A timed-out driver/encoder should not receive another
                    // long-running attempt. A fast hardware-decode rejection
                    // is safe to retry with bounded software decoding.
                    if (stalled) break;
                    continue;
                }
                if (boundCandidate.adapterBound) {
                    auto currentIndex = current_dxgi_adapter_index(boundCandidate.adapter);
                    if (!currentIndex || *currentIndex != boundCandidate.adapter.dxgiAdapterIndex) {
                        fs::remove(destination, fileError);
                        break;
                    }
                }

                fileError.clear();
                auto actualSize = fs::file_size(destination, fileError);
                if (fileError || !actualSize ||
                    (rate.maximumOutputBytes && actualSize > rate.maximumOutputBytes)) {
                    fs::remove(destination, fileError);
                    break;
                }
                if (validateCandidate && !validateCandidate(destination, backend, codec)) {
                    fs::remove(destination, fileError);
                    break;
                }
                if (selectedBackend) {
                    *selectedBackend = backendName + (hardwareDecode
                        ? L"（硬件解码）" : L"（兼容解码）");
                }
                if (selectedCodec) *selectedCodec = codec;
                // Timeline progress remains capped at 99 here. The optimizer
                // owns the authoritative container/dimension/rate/codec
                // validation and publishes 100 only after atomic adoption.
                error.clear();
                return VideoTranscodeResult::succeeded;
            }
        }

        error = L"可用的编码后端均不支持该视频规格";
        if (!attemptedBackends.empty()) error += L"（已尝试：" + attemptedBackends + L"）";
        return VideoTranscodeResult::unsupported;
    }
}
