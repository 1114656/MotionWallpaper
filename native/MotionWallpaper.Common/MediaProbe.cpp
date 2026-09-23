#include "MediaProbe.h"
#include "Common.h"

#include <windows.h>
#include <roapi.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <numeric>

namespace fs = std::filesystem;
using namespace winrt::Windows::Data::Json;

namespace
{
    constexpr size_t maximumProbeBytes = 256 * 1024;

    std::string string_field(JsonObject const& object, wchar_t const* name)
    {
        auto value = object.TryLookup(name);
        return value && value.ValueType() == JsonValueType::String
            ? winrt::to_string(value.GetString()) : std::string{};
    }

    uint32_t unsigned_field(JsonObject const& object, wchar_t const* name)
    {
        auto value = object.TryLookup(name);
        if (!value || value.ValueType() != JsonValueType::Number) return 0;
        auto number = value.GetNumber();
        return std::isfinite(number) && number > 0 && number <= UINT32_MAX &&
            std::floor(number) == number ? static_cast<uint32_t>(number) : 0;
    }

    uint32_t unsigned_text(std::string_view text)
    {
        uint32_t value{};
        auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? value : 0;
    }

    std::pair<uint32_t, uint32_t> frame_rate(std::string_view text)
    {
        auto separator = text.find('/');
        if (separator == std::string_view::npos) return {};
        auto numerator = unsigned_text(text.substr(0, separator));
        auto denominator = unsigned_text(text.substr(separator + 1));
        if (!numerator || !denominator) return {};
        auto divisor = std::gcd(numerator, denominator);
        return { numerator / divisor, denominator / divisor };
    }

    uint64_t duration_units(std::string_view text)
    {
        double seconds{};
        auto parsed = std::from_chars(text.data(), text.data() + text.size(), seconds);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
            !std::isfinite(seconds) || seconds <= 0) return 0;
        auto units = static_cast<long double>(seconds) * 10'000'000.0L;
        if (units >= static_cast<long double>(UINT64_MAX)) return 0;
        return static_cast<uint64_t>(std::round(units));
    }

    uint32_t pixel_bit_depth(std::string const& format)
    {
        // FFmpeg's named planar formats encode their component depth in the
        // suffix; semi-planar P010/P016 use a different spelling.
        for (auto depth : { 9u, 10u, 12u, 14u, 16u }) {
            auto suffix = std::to_string(depth);
            if (format.ends_with(suffix + "le") || format.ends_with(suffix + "be")) return depth;
        }
        if (format == "p010le" || format == "p010be" || format == "p210le" ||
            format == "p210be" || format == "p410le" || format == "p410be") return 10;
        if (format == "p016le" || format == "p016be" || format == "p216le" ||
            format == "p216be" || format == "p416le" || format == "p416be") return 16;
        if (format == "yuv420p" || format == "yuv422p" || format == "yuv444p" ||
            format == "yuvj420p" || format == "yuvj422p" || format == "yuvj444p" ||
            format == "yuva420p" || format == "yuva422p" || format == "yuva444p" ||
            format == "nv12" || format == "nv21" || format == "nv16" || format == "nv24" ||
            format == "gray" || format == "gbrp" || format == "gbrap" ||
            format == "rgb24" || format == "bgr24" || format == "rgba" ||
            format == "bgra" || format == "argb" || format == "abgr" ||
            format == "yuyv422" || format == "uyvy422") return 8;
        return 0;
    }

    std::optional<int> rotation_degrees(IJsonValue const& value)
    {
        int rotation{};
        if (value.ValueType() == JsonValueType::Number) {
            auto number = value.GetNumber();
            if (!std::isfinite(number) || std::floor(number) != number ||
                number < INT32_MIN || number > INT32_MAX) return {};
            rotation = static_cast<int>(number);
        } else if (value.ValueType() == JsonValueType::String) {
            auto text = winrt::to_string(value.GetString());
            auto parsed = std::from_chars(text.data(), text.data() + text.size(), rotation);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return {};
        } else {
            return {};
        }
        if (rotation % 90 != 0) return {};
        return (rotation % 360 + 360) % 360;
    }

    std::optional<int> stream_rotation(JsonObject const& stream)
    {
        if (auto sideData = stream.TryLookup(L"side_data_list")) {
            if (sideData.ValueType() != JsonValueType::Array) return {};
            for (auto const& entry : sideData.GetArray()) {
                if (entry.ValueType() != JsonValueType::Object) return {};
                auto data = entry.GetObject();
                auto type = string_field(data, L"side_data_type");
                if (!type.empty() && type != "Display Matrix") continue;
                if (auto rotation = data.TryLookup(L"rotation")) return rotation_degrees(rotation);
            }
        }
        if (auto tags = stream.TryLookup(L"tags")) {
            if (tags.ValueType() != JsonValueType::Object) return {};
            if (auto rotation = tags.GetObject().TryLookup(L"rotate")) return rotation_degrees(rotation);
        }
        return 0;
    }
}

namespace motion::media_tool_detail
{
    std::optional<std::string> run_bounded(fs::path const& executable,
        std::vector<std::wstring> const& arguments, uint32_t timeoutMs,
        std::function<bool()> const& cancelled)
    {
        try {
            if (!timeoutMs || (cancelled && cancelled()) || !fs::is_regular_file(executable)) return {};
            auto started = GetTickCount64();
            auto expired = [&] {
                return GetTickCount64() - started >= timeoutMs || (cancelled && cancelled());
            };
            SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
            HANDLE rawRead{}, rawWrite{};
            if (!CreatePipe(&rawRead, &rawWrite, &security, 0)) return {};
            unique_handle readPipe(rawRead), writePipe(rawWrite);
            if (!SetHandleInformation(readPipe.get(), HANDLE_FLAG_INHERIT, 0)) return {};
            unique_handle nullDevice(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
            if (!nullDevice) return {};
            unique_handle job(CreateJobObjectW(nullptr, nullptr));
            if (!job) return {};
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                    &limits, sizeof(limits))) return {};

            SIZE_T attributeBytes{};
            InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
            if (!attributeBytes) return {};
            std::vector<unsigned char> attributes(attributeBytes);
            auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
            if (!InitializeProcThreadAttributeList(list, 1, 0, &attributeBytes)) return {};
            struct AttributeCleanup
            {
                LPPROC_THREAD_ATTRIBUTE_LIST value;
                ~AttributeCleanup() { DeleteProcThreadAttributeList(value); }
            } attributeCleanup{ list };
            HANDLE inherited[]{ writePipe.get(), nullDevice.get() };
            if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                    inherited, sizeof(inherited), nullptr, nullptr)) return {};
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            startup.StartupInfo.wShowWindow = SW_HIDE;
            startup.StartupInfo.hStdInput = nullDevice.get();
            startup.StartupInfo.hStdOutput = writePipe.get();
            startup.StartupInfo.hStdError = nullDevice.get();
            startup.lpAttributeList = list;
            std::vector<std::wstring> commandArguments{ executable.wstring() };
            commandArguments.insert(commandArguments.end(), arguments.begin(), arguments.end());
            auto command = build_command_line(commandArguments);
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                    CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS |
                        EXTENDED_STARTUPINFO_PRESENT,
                    nullptr, executable.parent_path().c_str(), &startup.StartupInfo, &process)) return {};
            unique_handle processHandle(process.hProcess), threadHandle(process.hThread);
            if (!AssignProcessToJobObject(job.get(), processHandle.get()) ||
                    ResumeThread(threadHandle.get()) == static_cast<DWORD>(-1)) {
                TerminateProcess(processHandle.get(), ERROR_PROCESS_ABORTED);
                WaitForSingleObject(processHandle.get(), 2000);
                return {};
            }
            writePipe.reset();
            auto abort = [&]() -> std::optional<std::string> {
                TerminateJobObject(job.get(), ERROR_OPERATION_ABORTED);
                WaitForSingleObject(processHandle.get(), 2000);
                return {};
            };
            std::string output;
            std::array<char, 4096> buffer{};
            for (;;) {
                if (expired()) return abort();
                DWORD available{};
                if (!PeekNamedPipe(readPipe.get(), nullptr, 0, nullptr, &available, nullptr)) {
                    if (GetLastError() != ERROR_BROKEN_PIPE) return abort();
                    available = 0;
                }
                if (available) {
                    DWORD received{};
                    if (!ReadFile(readPipe.get(), buffer.data(),
                            (std::min)(available, static_cast<DWORD>(buffer.size())),
                            &received, nullptr)) return abort();
                    if (output.size() + received > maximumProbeBytes) return abort();
                    output.append(buffer.data(), received);
                    continue;
                }
                auto wait = WaitForSingleObject(processHandle.get(), 25);
                if (wait == WAIT_OBJECT_0) {
                    // The process may have exited while the wait was pending.
                    // Drain its final JSON bytes before checking the exit code.
                    if (PeekNamedPipe(readPipe.get(), nullptr, 0, nullptr, &available, nullptr) && available) continue;
                    DWORD exitCode{};
                    if (!GetExitCodeProcess(processHandle.get(), &exitCode) || exitCode) return {};
                    return output;
                }
                if (wait != WAIT_TIMEOUT) return abort();
            }
        } catch (...) {
            return {};
        }
    }
}

namespace motion
{
    std::optional<VideoProbeInfo> parse_video_probe_json(std::string_view json)
    {
        if (json.empty() || json.size() > maximumProbeBytes) return {};
        auto initialized = RoInitialize(RO_INIT_MULTITHREADED);
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return {};
        struct ApartmentCleanup
        {
            bool owns{};
            ~ApartmentCleanup() { if (owns) RoUninitialize(); }
        } apartment{ SUCCEEDED(initialized) };
        try {
            // The class-static Parse helper caches its activation factory
            // globally. A standalone worker may own the last WinRT apartment;
            // uninitializing it would leave that cache stale on the next call.
            // Acquire the factory by runtime class name instead (uncached), and
            // release it with all JSON objects before the local apartment ends.
            auto factory = winrt::get_activation_factory<IJsonObjectStatics>(
                L"Windows.Data.Json.JsonObject");
            auto document = factory.Parse(winrt::to_hstring(json));
            auto streams = document.GetNamedArray(L"streams");
            if (streams.Size() != 1) return {};
            auto stream = streams.GetObjectAt(0);
            VideoProbeInfo result;
            result.width = unsigned_field(stream, L"width");
            result.height = unsigned_field(stream, L"height");
            auto rate = frame_rate(string_field(stream, L"avg_frame_rate"));
            if (!rate.first) rate = frame_rate(string_field(stream, L"r_frame_rate"));
            result.frameRateNumerator = rate.first;
            result.frameRateDenominator = rate.second;
            result.codecName = string_field(stream, L"codec_name");
            result.profile = string_field(stream, L"profile");
            result.pixelFormat = string_field(stream, L"pix_fmt");
            result.bitDepth = unsigned_text(string_field(stream, L"bits_per_raw_sample"));
            if (!result.bitDepth) result.bitDepth = pixel_bit_depth(result.pixelFormat);
            if (result.bitDepth > 32) return {};
            result.colorTransfer = string_field(stream, L"color_transfer");
            result.colorPrimaries = string_field(stream, L"color_primaries");
            result.colorSpace = string_field(stream, L"color_space");
            result.colorRange = string_field(stream, L"color_range");
            auto rotation = stream_rotation(stream);
            if (!rotation) return {};
            result.rotationDegrees = *rotation;
            result.duration100ns = duration_units(string_field(stream, L"duration"));
            if (!result.duration100ns) {
                auto format = document.TryLookup(L"format");
                if (format && format.ValueType() == JsonValueType::Object) {
                    result.duration100ns = duration_units(string_field(format.GetObject(), L"duration"));
                }
            }
            if (!result.width || !result.height || !result.frameRateNumerator ||
                !result.frameRateDenominator || result.codecName.empty()) return {};
            return result;
        } catch (...) {
            return {};
        }
    }

    std::optional<VideoProbeInfo> probe_video(fs::path const& ffmpeg,
        fs::path const& source, uint32_t timeoutMs, std::function<bool()> const& cancelled)
    {
        auto result = media_tool_detail::run_bounded(ffmpeg.parent_path() / L"ffprobe.exe", {
            L"-v", L"error", L"-select_streams", L"v:0", L"-show_entries",
            L"stream=codec_name,profile,width,height,pix_fmt,bits_per_raw_sample,avg_frame_rate,r_frame_rate,duration,color_transfer,color_primaries,color_space,color_range:stream_tags=rotate:stream_side_data=side_data_type,rotation:format=duration",
            L"-of", L"json", L"-i", source.wstring()
        }, timeoutMs, cancelled);
        return result ? parse_video_probe_json(*result) : std::nullopt;
    }
}
