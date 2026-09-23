#pragma once

#include "VideoGpuProbe.h"
#include "VideoTranscodeColorPolicy.h"

#include <array>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace motion::agent
{
    struct VideoStillSize { uint32_t width{}, height{}; };

    [[nodiscard]] inline VideoStillSize video_still_size(VideoProbeInfo const& source)
    {
        auto width = source.width, height = source.height;
        if (source.rotationDegrees == 90 || source.rotationDegrees == 270) std::swap(width, height);
        if (!width || !height) return {};
        auto maxWidth = width >= height ? 3840u : 2160u;
        auto maxHeight = width >= height ? 2160u : 3840u;
        double scale = std::min({ 1.0, static_cast<double>(maxWidth) / width,
            static_cast<double>(maxHeight) / height });
        return { std::max(1u, static_cast<uint32_t>(width * scale)),
            std::max(1u, static_cast<uint32_t>(height * scale)) };
    }

    // A separate on-demand disk cache: gallery posters remain 480x270. The
    // service never retains decoded image pixels and runs at most one bounded
    // child process. An OS file lease protects each displayed PNG from pruning.
    class VideoStillPreview
    {
    public:
        struct Result { std::filesystem::path path; std::shared_ptr<void> lease; };

        VideoStillPreview(std::filesystem::path ffmpeg, std::filesystem::path cache,
            uint64_t quotaBytes = 128ull * 1024 * 1024)
            : ffmpeg_(std::move(ffmpeg)), cache_(std::move(cache)), quotaBytes_(quotaBytes),
              worker_([this](std::stop_token stop) { Run(stop); }) {}

        ~VideoStillPreview()
        {
            worker_.request_stop();
            condition_.notify_all();
            if (worker_.joinable()) worker_.join();
        }

        // Caller supplies an identity-stable source path and keeps any media
        // library / performance-variant leases alive until extraction finishes.
        [[nodiscard]] Result Read(std::filesystem::path const& source,
            std::shared_ptr<void> sourceLease = {})
        {
            try {
                unique_handle input(CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
                if (!input) return {};
                auto fingerprint = video_file_fingerprint(source);
                if (fingerprint == "unavailable") return {};
                std::replace(fingerprint.begin(), fingerprint.end(), ':', '-');
                auto key = L"still-v1-" + utf8_to_wide(fingerprint);
                auto path = cache_ / (key + L".png");
                std::lock_guard lock(mutex_);
                if (auto cached = Open(path); cached.lease) return cached;
                auto now = std::chrono::steady_clock::now();
                if (queued_.contains(key) || pending_.size() >= 8) return {};
                if (auto failed = failures_.find(key); failed != failures_.end() &&
                    failed->second > now) return {};
                pending_.push_back({ key, source, path, std::move(sourceLease), std::move(input) });
                queued_.insert(key);
                condition_.notify_all();
            } catch (...) {}
            return {};
        }

        [[nodiscard]] bool Quiesce(uint32_t timeoutMs)
        {
            std::unique_lock lock(mutex_);
            ++generation_;
            for (auto const& request : pending_) queued_.erase(request.key);
            pending_.clear();
            condition_.notify_all();
            return condition_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return !active_; });
        }

    private:
        struct Request
        {
            std::wstring key;
            std::filesystem::path source, destination;
            std::shared_ptr<void> sourceLease;
            unique_handle input;
        };

        [[nodiscard]] static Result Open(std::filesystem::path const& path)
        {
            unique_handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            LARGE_INTEGER size{};
            std::array<unsigned char, 33> header{};
            DWORD count{};
            constexpr std::array<unsigned char, 16> prefix{
                137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R' };
            if (!file || !GetFileSizeEx(file.get(), &size) || size.QuadPart <= 33 ||
                size.QuadPart > 32ll * 1024 * 1024 ||
                !ReadFile(file.get(), header.data(), static_cast<DWORD>(header.size()), &count, nullptr) ||
                count != header.size() || !std::equal(prefix.begin(), prefix.end(), header.begin())) return {};
            auto dimension = [&](size_t offset) {
                return static_cast<uint32_t>(header[offset]) << 24 |
                    static_cast<uint32_t>(header[offset + 1]) << 16 |
                    static_cast<uint32_t>(header[offset + 2]) << 8 | header[offset + 3];
            };
            auto width = dimension(16), height = dimension(20);
            if (!width || !height || std::max(width, height) > 3840 ||
                std::min(width, height) > 2160 || header[24] != 8 || header[25] != 2) return {};
            return { path, std::make_shared<unique_handle>(std::move(file)) };
        }

        void Prune(std::filesystem::path const& protectedPath)
        {
            struct Entry { std::filesystem::path path; uint64_t size; std::filesystem::file_time_type time; };
            std::vector<Entry> entries;
            uint64_t bytes{};
            std::error_code error;
            for (std::filesystem::directory_iterator it(cache_, error), end; !error && it != end; it.increment(error)) {
                auto const path = it->path();
                auto name = path.filename().wstring();
                if (!name.starts_with(L"still-v1-") || !name.ends_with(L".png") ||
                    !it->is_regular_file(error) || error) continue;
                auto size = it->file_size(error);
                if (error) continue;
                auto time = it->last_write_time(error);
                if (error) continue;
                if (name.ends_with(L".part.png")) {
                    // Recover temporary files from a killed Agent. A normal
                    // probe + extraction is bounded to less than one minute.
                    if (time < std::filesystem::file_time_type::clock::now() - std::chrono::minutes(5))
                        std::filesystem::remove(path, error);
                    error.clear();
                    continue;
                }
                bytes += size;
                entries.push_back({ path, size, time });
            }
            std::sort(entries.begin(), entries.end(), [](auto const& a, auto const& b) { return a.time < b.time; });
            for (auto const& entry : entries) {
                if (bytes <= quotaBytes_) break;
                if (entry.path == protectedPath) continue;
                // Display leases deny FILE_SHARE_DELETE, including during a
                // delayed Renderer startup. A sharing violation means keep it.
                if (std::filesystem::remove(entry.path, error)) bytes -= entry.size;
                error.clear();
            }
        }

        bool Generate(Request const& request, std::function<bool()> const& cancelled)
        {
            auto temporary = request.destination;
            temporary.replace_extension(L".part.png");
            struct Cleanup
            {
                std::filesystem::path path;
                ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); }
            } cleanup{ temporary };
            try {
                auto probe = probe_video(ffmpeg_, request.source, 10'000, cancelled);
                if (!probe || cancelled()) return false;
                auto size = video_still_size(*probe);
                auto color = video_transcode_color_plan(*probe, size.width, size.height, VideoColorOutput::SrgbStill);
                if (!color) return false;
                std::filesystem::create_directories(cache_);
                auto result = media_tool_detail::run_bounded(ffmpeg_, {
                    L"-nostdin", L"-hide_banner", L"-loglevel", L"error", L"-y",
                    L"-threads", L"2", L"-filter_threads", L"1", L"-ss", L"0",
                    L"-i", request.source.wstring(), L"-map", L"0:v:0", L"-an", L"-sn", L"-dn",
                    L"-frames:v", L"1", L"-vf", color->filter, L"-pix_fmt", L"rgb24",
                    L"-compression_level", L"3", L"-update", L"1", L"-f", L"image2", temporary.wstring()
                }, 30'000, cancelled);
                if (!result || cancelled() || !Open(temporary).lease) return false;
                if (!MoveFileExW(temporary.c_str(), request.destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return false;
                Prune(request.destination);
                return true;
            } catch (...) { return false; }
        }

        void Run(std::stop_token stop)
        {
            for (;;) {
                Request request;
                uint64_t generation{};
                {
                    std::unique_lock lock(mutex_);
                    if (!condition_.wait(lock, stop, [&] { return !pending_.empty(); })) return;
                    request = std::move(pending_.front());
                    pending_.pop_front();
                    active_ = true;
                    generation = generation_.load();
                }
                auto cancelled = [&] { return stop.stop_requested() || generation != generation_.load(); };
                bool success = Generate(request, cancelled);
                {
                    std::lock_guard lock(mutex_);
                    queued_.erase(request.key);
                    if (!success && !cancelled()) {
                        if (failures_.size() >= 64) failures_.clear();
                        failures_[request.key] = std::chrono::steady_clock::now() + std::chrono::minutes(5);
                    }
                    // Release trust/source pins before reporting quiescence.
                    request = {};
                    active_ = false;
                }
                condition_.notify_all();
                if (stop.stop_requested()) return;
            }
        }

        std::filesystem::path ffmpeg_, cache_;
        uint64_t quotaBytes_;
        std::mutex mutex_;
        std::condition_variable_any condition_;
        std::deque<Request> pending_;
        std::set<std::wstring> queued_;
        std::map<std::wstring, std::chrono::steady_clock::time_point> failures_;
        std::atomic_uint64_t generation_{};
        bool active_{};
        std::jthread worker_;
    };
}
