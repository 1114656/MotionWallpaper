#pragma once

#include "VideoTranscoder.h"
#include "VideoVariantPolicy.h"
#include "../MotionWallpaper.Common/Common.h"
#include "../MotionWallpaper.Common/MediaProbe.h"
#include "../MotionWallpaper.Renderer/AdapterPolicy.h"

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

namespace motion::agent
{
    struct VideoGpuInventory
    {
        bool pending{};
        bool succeeded{};
        bool physicalVideoDeviceAvailable{};
        std::vector<VideoTranscodeAdapter> adapters;
        std::map<std::wstring, std::wstring> displayAdapters;
        std::string environment;
    };

    struct VideoGpuDecodeProbe
    {
        bool pending{};
        bool succeeded{};
        std::vector<std::wstring> adapters;
    };

    inline std::string video_file_fingerprint(std::filesystem::path const& path)
    {
        unique_handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!file || !GetFileInformationByHandle(file.get(), &info)) return "unavailable";
        return std::to_string(info.dwVolumeSerialNumber) + ":" + std::to_string(info.nFileIndexHigh) + ":" +
            std::to_string(info.nFileIndexLow) + ":" + std::to_string(info.nFileSizeHigh) + ":" +
            std::to_string(info.nFileSizeLow) + ":" + std::to_string(info.ftLastWriteTime.dwHighDateTime) + ":" +
            std::to_string(info.ftLastWriteTime.dwLowDateTime);
    }

    namespace gpu_probe_detail
    {
        inline std::wstring executable()
        {
            std::wstring value(32768, L'\0');
            auto length = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
            if (!length || length >= value.size()) return {};
            value.resize(length);
            return value;
        }

        inline bool valid_luid(std::string const& value)
        {
            auto colon = value.find(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 == value.size()) return false;
            for (size_t index = 0; index < value.size(); ++index) {
                if (index == colon || (index == 0 && value[index] == '-')) continue;
                if (value[index] < '0' || value[index] > '9') return false;
            }
            return value.size() <= 32;
        }

        inline VideoGpuInventory parse_inventory(std::optional<std::string> const& text)
        {
            VideoGpuInventory result;
            if (!text) return result;
            std::istringstream input(*text);
            std::string version;
            unsigned available{}, count{};
            if (!(input >> version >> available >> count) || version != "gpu-v1" || available > 1 || count > 64) return {};
            for (unsigned index = 0; index < count; ++index) {
                VideoTranscodeAdapter adapter;
                unsigned displays{};
                if (!(input >> adapter.vendorId >> adapter.dedicatedVideoMemory >> adapter.dxgiAdapterIndex >>
                    adapter.luidHigh >> adapter.luidLow >> adapter.driverVersion >> displays) || displays > 64) return {};
                adapter.identityKnown = true;
                result.adapters.push_back(adapter);
                auto luid = std::to_wstring(adapter.luidHigh) + L":" + std::to_wstring(adapter.luidLow);
                for (unsigned display = 0; display < displays; ++display) {
                    std::string name;
                    if (!(input >> name) || name.size() > 128) return {};
                    result.displayAdapters[utf8_to_wide(name)] = luid;
                }
            }
            std::string trailing;
            if (input >> trailing) return {};
            result.succeeded = true;
            result.physicalVideoDeviceAvailable = available != 0;
            // Sorted identities make enum-order changes harmless, while driver
            // changes and physical device replacement invalidate old failures.
            std::vector<std::string> identities;
            for (auto const& adapter : result.adapters) identities.push_back(
                std::to_string(adapter.vendorId) + ":" + std::to_string(adapter.luidHigh) + ":" +
                std::to_string(adapter.luidLow) + ":" + std::to_string(adapter.driverVersion));
            std::sort(identities.begin(), identities.end());
            result.environment = "gpu-v1:" + std::to_string(available);
            for (auto const& identity : identities) result.environment += "|" + identity;
            return result;
        }

        struct RankedDecodeAdapter { uint32_t rank{}; std::wstring luid; };

        inline std::optional<std::vector<RankedDecodeAdapter>> parse_ranked_decode(
            std::optional<std::string> const& text)
        {
            if (!text) return std::nullopt;
            std::vector<RankedDecodeAdapter> result;
            std::istringstream input(*text);
            std::string version, luid;
            if (!(input >> version) || version != "decode-v2") return std::nullopt;
            while (!(input >> std::ws).eof()) {
                uint32_t rank{};
                if (!(input >> rank >> luid) || rank >= 64 || !valid_luid(luid) || result.size() >= 64)
                    return std::nullopt;
                result.push_back({ rank, utf8_to_wide(luid) });
            }
            return result;
        }

        inline std::string ranked_decode_output(std::vector<RankedDecodeAdapter> adapters)
        {
            std::stable_sort(adapters.begin(), adapters.end(), [](auto const& left, auto const& right) {
                return left.rank < right.rank;
            });
            std::string result = "decode-v2\n";
            for (auto const& adapter : adapters)
                result += std::to_string(adapter.rank) + " " + wide_to_utf8(adapter.luid) + "\n";
            return result;
        }

        inline VideoGpuDecodeProbe parse_decode(std::optional<std::string> const& text)
        {
            VideoGpuDecodeProbe result;
            auto ranked = parse_ranked_decode(text);
            if (!ranked) return result;
            std::stable_sort(ranked->begin(), ranked->end(), [](auto const& left, auto const& right) {
                return left.rank < right.rank;
            });
            for (auto const& adapter : *ranked) result.adapters.push_back(adapter.luid);
            result.succeeded = true;
            return result;
        }

        struct AsyncResult { bool pending{}; std::optional<std::string> output; };
        class AsyncProbe
        {
            struct Entry { bool completed{}, queued{}; std::optional<std::string> output; std::chrono::steady_clock::time_point refreshed{}; };
            struct Job { std::wstring key; std::vector<std::wstring> arguments; uint64_t generation{}; std::shared_ptr<void> lease; };
            std::mutex mutex_;
            std::condition_variable condition_;
            std::map<std::wstring, Entry> entries_;
            std::deque<Job> jobs_;
            std::atomic_uint64_t generation_{};
            bool busy_{};
            using Runner = std::function<std::optional<std::string>(std::vector<std::wstring> const&,
                uint32_t, std::function<bool()> const&)>;
            Runner runner_;
            std::jthread worker_;
        public:
            explicit AsyncProbe(Runner runner = {}) : runner_(std::move(runner)), worker_([this](std::stop_token stop) {
                for (;;) {
                    Job job;
                    {
                        std::unique_lock lock(mutex_);
                        condition_.wait_for(lock, std::chrono::milliseconds(100), [&] { return stop.stop_requested() || !jobs_.empty(); });
                        if (stop.stop_requested()) return;
                        if (jobs_.empty()) continue;
                        job = std::move(jobs_.front()); jobs_.pop_front();
                        busy_ = true;
                    }
                    auto cancelled = [&] { return stop.stop_requested() || generation_.load() != job.generation; };
                    auto run = [&](std::vector<std::wstring> const& arguments, uint32_t timeout) {
                        return runner_ ? runner_(arguments, timeout, cancelled) :
                            media_tool_detail::run_bounded(executable(), arguments, timeout, cancelled);
                    };
                    std::optional<std::string> output;
                    if (!job.arguments.empty() && job.arguments[0] == L"--probe-gpu-decode") {
                        // Each adapter gets its own process and deadline. A
                        // hung first driver must not conceal a working second GPU.
                        std::vector<RankedDecodeAdapter> supported;
                        for (size_t index = 5; index < job.arguments.size() && !cancelled(); ++index) {
                            std::vector<std::wstring> arguments(job.arguments.begin(), job.arguments.begin() + 5);
                            arguments.push_back(job.arguments[index]);
                            auto result = parse_ranked_decode(run(arguments, 12000));
                            if (result) supported.insert(supported.end(), result->begin(), result->end());
                        }
                        // A child reports its rank in the complete DXGI preference
                        // order, before filtering to its isolated adapter. Process
                        // launch order must not override the heavy-workload policy.
                        output = ranked_decode_output(std::move(supported));
                    } else {
                        output = run(job.arguments, 10000);
                    }
                    job.lease.reset();
                    std::lock_guard lock(mutex_);
                    busy_ = false;
                    condition_.notify_all();
                    if (cancelled()) continue;
                    auto found = entries_.find(job.key);
                    if (found == entries_.end()) continue;
                    found->second = { true, false, std::move(output), std::chrono::steady_clock::now() };
                }
            }) {}
            ~AsyncProbe() { worker_.request_stop(); condition_.notify_all(); }
            AsyncResult Read(std::wstring key, std::vector<std::wstring> arguments,
                std::shared_ptr<void> lease = {}, bool refreshInventory = false)
            {
                std::lock_guard lock(mutex_);
                auto& entry = entries_[key];
                bool refresh = refreshInventory && entry.completed &&
                    std::chrono::steady_clock::now() - entry.refreshed >= std::chrono::seconds(60);
                if (!entry.queued && (!entry.completed || refresh)) {
                    entry.queued = true;
                    jobs_.push_back({ std::move(key), std::move(arguments), generation_.load(), std::move(lease) });
                    condition_.notify_one();
                }
                return { !entry.completed, entry.output };
            }
            void Invalidate()
            {
                ++generation_;
                std::lock_guard lock(mutex_);
                entries_.clear(); jobs_.clear();
                condition_.notify_all();
            }
            bool Quiesce(uint32_t timeoutMs)
            {
                std::unique_lock lock(mutex_);
                if (!busy_ && jobs_.empty()) return true;
                ++generation_;
                jobs_.clear();
                for (auto entry = entries_.begin(); entry != entries_.end();) {
                    if (!entry->second.completed) entry = entries_.erase(entry);
                    else { entry->second.queued = false; ++entry; }
                }
                condition_.notify_all();
                return condition_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return !busy_; });
            }
        };
        inline AsyncProbe& service() { static AsyncProbe value; return value; }
    }

    inline VideoGpuInventory video_gpu_inventory_bounded(std::function<bool()> const& cancelled = {})
    {
        return gpu_probe_detail::parse_inventory(media_tool_detail::run_bounded(
            gpu_probe_detail::executable(), { L"--probe-gpu-inventory" }, 10000, cancelled));
    }

    inline std::optional<uint32_t> parse_video_cuda_device(std::optional<std::string> const& output,
        VideoTranscodeAdapter const& expected)
    {
        if (!output || !expected.identityKnown) return {};
        std::istringstream input(*output);
        std::string marker, extra;
        uint32_t index{}; int64_t high{}; uint64_t low{};
        if (!(input >> marker >> index >> high >> low) || input >> extra || marker != "cuda-v1" ||
            index >= 64 || high != expected.luidHigh || low != expected.luidLow) return {};
        return index;
    }

    inline std::optional<uint32_t> video_cuda_device_bounded(VideoTranscodeAdapter const& expected,
        std::function<bool()> const& cancelled = {})
    {
        if (!expected.identityKnown || expected.vendorId != 0x10de) return {};
        auto luid = std::to_wstring(expected.luidHigh) + L":" + std::to_wstring(expected.luidLow);
        return parse_video_cuda_device(media_tool_detail::run_bounded(gpu_probe_detail::executable(),
            { L"--probe-cuda-device", luid }, 10'000, cancelled), expected);
    }

    inline VideoGpuInventory video_gpu_inventory_async()
    {
        auto value = gpu_probe_detail::service().Read(L"inventory", { L"--probe-gpu-inventory" }, {}, true);
        auto result = gpu_probe_detail::parse_inventory(value.output);
        result.pending = value.pending;
        return result;
    }

    inline void invalidate_video_gpu_probes() { gpu_probe_detail::service().Invalidate(); }

    inline VideoGpuDecodeProbe video_gpu_decode_async(std::filesystem::path const& ffmpeg,
        std::filesystem::path const& source, std::wstring const& preferredAdapter,
        uint64_t aggregateOutputPixels, std::shared_ptr<void> lease = {})
    {
        auto inventory = video_gpu_inventory_async();
        if (inventory.pending) return { true, false, {} };
        if (!inventory.succeeded) return {};
        auto key = source.wstring() + L"\n" + preferredAdapter + L"\n" + std::to_wstring(aggregateOutputPixels) +
            L"\n" + utf8_to_wide(inventory.environment + "|" + video_file_fingerprint(source) + "|" + video_file_fingerprint(ffmpeg));
        std::vector<std::wstring> arguments{ L"--probe-gpu-decode", ffmpeg.wstring(), source.wstring(),
            preferredAdapter.empty() ? L"none" : preferredAdapter, std::to_wstring(aggregateOutputPixels) };
        std::stable_sort(inventory.adapters.begin(), inventory.adapters.end(), [&](auto const& left, auto const& right) {
            auto leftKey = std::to_wstring(left.luidHigh) + L":" + std::to_wstring(left.luidLow);
            auto rightKey = std::to_wstring(right.luidHigh) + L":" + std::to_wstring(right.luidLow);
            if ((leftKey == preferredAdapter) != (rightKey == preferredAdapter)) return leftKey == preferredAdapter;
            // Retain DXGI's display-first order when neither is preferred.
            return left.dxgiAdapterIndex < right.dxgiAdapterIndex;
        });
        for (auto const& adapter : inventory.adapters)
            arguments.push_back(std::to_wstring(adapter.luidHigh) + L":" + std::to_wstring(adapter.luidLow));
        auto value = gpu_probe_detail::service().Read(std::move(key), std::move(arguments), std::move(lease));
        auto result = gpu_probe_detail::parse_decode(value.output);
        result.pending = value.pending;
        return result;
    }

    // Implemented beside the existing decoder-profile query. Call this before
    // COM/GUI/single-instance initialization in both Agent and the test host.
    int run_video_gpu_probe_cli(int argc, wchar_t** argv);
}
