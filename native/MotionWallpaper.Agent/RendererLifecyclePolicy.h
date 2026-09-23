#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace motion::agent
{
    class RendererRetirementPolicy
    {
    public:
        // One confirmed predecessor per display, with a bounded handoff.
        std::set<std::wstring> Keep(std::map<std::string, std::wstring> const& desired,
            std::set<std::wstring> const& ready, uint64_t now)
        {
            std::set<std::wstring> keep;
            for (auto const& [display, key] : desired) {
                keep.insert(key);
                auto previous = owners_.find(display);
                if (ready.contains(key)) {
                    owners_[display] = { key, 0, false };
                } else if (previous != owners_.end() && previous->second.key != key) {
                    auto& owner = previous->second;
                    if (!owner.retiring) { owner.since = now; owner.retiring = true; }
                    if (now - owner.since < handoffTimeoutMs) keep.insert(owner.key);
                    else owners_.erase(previous);
                }
            }
            std::erase_if(owners_, [&](auto const& entry) { return !desired.contains(entry.first); });
            return keep;
        }
        void Clear() { owners_.clear(); }
        static constexpr uint64_t handoffTimeoutMs = 12'000;
    private:
        struct Owner { std::wstring key; uint64_t since{}; bool retiring{}; };
        std::map<std::string, Owner> owners_;
    };

    enum class PlaybackHealthFailure { None, Heartbeat, Frames };

    class PlaybackHealthMonitor
    {
    public:
        void Reset(uint64_t revision, uint64_t now)
        {
            revision_ = revision;
            heartbeatAt_ = progressedAt_ = now;
            serial_ = 0;
            frameTimeout_ = 15'000;
        }
        void Observe(uint64_t revision, uint64_t serial, uint64_t period100ns, uint64_t now)
        {
            if (revision != revision_) return;
            heartbeatAt_ = now;
            frameTimeout_ = (std::clamp)(period100ns / 2500, uint64_t{15'000}, uint64_t{300'000});
            if (serial != serial_) { serial_ = serial; progressedAt_ = now; }
        }
        [[nodiscard]] PlaybackHealthFailure Check(uint64_t now) const
        {
            if (now - heartbeatAt_ >= 10'000) return PlaybackHealthFailure::Heartbeat;
            if (now - progressedAt_ >= frameTimeout_) return PlaybackHealthFailure::Frames;
            return PlaybackHealthFailure::None;
        }
    private:
        uint64_t revision_{}, heartbeatAt_{}, progressedAt_{}, serial_{}, frameTimeout_{15'000};
    };

    class PlaybackRecoveryBudget
    {
    public:
        bool Exhausted(uint64_t now)
        {
            if (!failures_ || now - windowStart_ >= 300'000) { windowStart_ = now; failures_ = 0; }
            return ++failures_ >= 3;
        }
        void Clear() { failures_ = 0; }
    private:
        uint64_t windowStart_{};
        unsigned failures_{};
    };
}
