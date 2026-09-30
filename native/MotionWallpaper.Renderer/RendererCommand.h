#pragma once

#include "TransitionPolicy.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace motion::renderer
{
    struct RendererCommand
    {
        Command command{Command::Unknown};
        uint64_t revision{};
        std::vector<std::wstring> coveredDisplays;
    };

    enum class CommandParseError { None, MissingRevision, InvalidCommand, InvalidPayload };

    inline CommandParseError parse_renderer_command(std::string const& line, RendererCommand& result)
    {
        result = {};
        if (line.size() > 8192) return CommandParseError::InvalidPayload;
        std::istringstream input(line);
        std::string name, revision;
        input >> name >> revision;
        auto parsed = std::from_chars(revision.data(), revision.data() + revision.size(), result.revision);
        if (parsed.ec != std::errc{} || parsed.ptr != revision.data() + revision.size() || !result.revision)
            return CommandParseError::MissingRevision;
        if (name == "desktop-play") result.command = Command::DesktopPlay;
        else if (name == "desktop-freeze") result.command = Command::DesktopFreeze;
        else if (name == "screensaver-play") result.command = Command::ScreensaverPlay;
        else if (name == "pause") result.command = Command::Pause;
        else if (name == "stop") result.command = Command::Stop;
        else return CommandParseError::InvalidCommand;
        std::string display;
        while (input >> display) {
            if (result.command != Command::DesktopPlay || result.coveredDisplays.size() >= 64 ||
                display.size() > 256 || !display.starts_with("\\\\.\\DISPLAY") || display.size() == 11 ||
                !std::all_of(display.begin() + 11, display.end(), [](char value) {return value >= '0' && value <= '9';}))
                return CommandParseError::InvalidPayload;
            result.coveredDisplays.emplace_back(display.begin(), display.end());
        }
        std::sort(result.coveredDisplays.begin(), result.coveredDisplays.end());
        result.coveredDisplays.erase(std::unique(result.coveredDisplays.begin(), result.coveredDisplays.end()),
            result.coveredDisplays.end());
        return CommandParseError::None;
    }

    // Every command describes the complete desired state. Keep only the newest
    // pending revision while the UI thread is busy, with no heap pointer in a
    // posted window message and no unbounded command queue during a transition.
    class RendererCommandMailbox
    {
    public:
        bool Put(RendererCommand command, bool* notify = nullptr)
        {
            std::scoped_lock lock(mutex_);
            if (notify) *notify = false;
            if (stopping_ || command.revision < highestRevision_) return false;
            if (notify) *notify = !pending_.has_value();
            highestRevision_ = command.revision;
            stopping_ = command.command == Command::Stop;
            pending_ = std::move(command);
            return true;
        }

        std::optional<RendererCommand> Take()
        {
            std::scoped_lock lock(mutex_);
            auto command = std::move(pending_);
            pending_.reset();
            return command;
        }

    private:
        std::mutex mutex_;
        std::optional<RendererCommand> pending_;
        uint64_t highestRevision_{};
        bool stopping_{};
    };

    struct DesktopOutputPolicy
    {
        bool covered{};

        [[nodiscard]] bool ShouldPresent(bool captureForFreeze) const noexcept
        {
            return captureForFreeze || !covered;
        }

        [[nodiscard]] bool ContributesProgress() const noexcept { return !covered; }
    };
}
