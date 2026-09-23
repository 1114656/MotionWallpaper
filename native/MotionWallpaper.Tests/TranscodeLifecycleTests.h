#pragma once

#include "../MotionWallpaper.Agent/RuntimePolicy.h"
#include "../MotionWallpaper.Agent/RuntimeStatusPolicy.h"
#include "../MotionWallpaper.Common/Common.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace motion::tests
{
    template <typename Require>
    inline void transcode_lifecycle_survives_session_transitions(Require require)
    {
        using namespace motion::agent;
        for (auto action : { RuntimeAction::DesktopPlay, RuntimeAction::ScreensaverPlay,
                RuntimeAction::DesktopFrozen, RuntimeAction::DesktopPaused }) {
            require(performance_copy_preview_required(action, true),
                "a visible session transition abandoned a required performance copy");
            require(!performance_copy_preview_required(action, false),
                "an already playable copy was replaced with a preview");
        }
        for (auto action : { RuntimeAction::Locked, RuntimeAction::DisplayOff }) {
            require(!performance_copy_preview_required(action, true),
                "locked or powered-off sessions still create visible preview windows");
            require(hidden_session_generation_allowed(action, false, true),
                "AC-powered conversion is cancelled by a locked or powered-off session");
            require(!hidden_session_generation_allowed(action, true, true),
                "hidden-session conversion continues on battery");
            require(!hidden_session_generation_allowed(action, false, false),
                "hidden-session background work bypasses Renderer shutdown");
        }
        require(!hidden_session_generation_allowed(RuntimeAction::DesktopPlay, false, true),
            "the hidden-session policy bypasses ordinary desktop Renderer barriers");
        require(optimization_renderer_is_static(RuntimeAction::ScreensaverPlay,
                true, false, false, true, true, false, true),
            "a pending poster ACK cancels an encode after all video routes stopped");
        require(optimization_renderer_is_static(RuntimeAction::DesktopPlay,
                true, false, false, true, false, false, true),
            "a retiring image cancels safe background conversion after unlocking");
        require(!optimization_renderer_is_static(RuntimeAction::DesktopPlay,
                true, false, false, true, true, false, false),
            "transcoding is admitted before an active video route freezes");
        require(!optimization_renderer_is_static(RuntimeAction::DesktopPlay,
                true, true, true, true, true, true, true),
            "an unresolved media transaction admits new background work");

        motion::Settings settings;
        settings.screensaverEnabled = true;
        require(reduce_runtime_action(settings, { true, true, false, true, 600 }) == RuntimeAction::Locked,
            "background conversion changed secure-desktop lock priority");
        require(reduce_runtime_action(settings, { false, false, false, true, 600 }) == RuntimeAction::DisplayOff,
            "background conversion changed display power priority");
    }

    template <typename Require>
    inline void stopped_renderer_is_a_settled_pause(Require require)
    {
        using motion::agent::display_runtime_state;
        require(display_runtime_state({ false, false, true, false, false, true }) == "paused",
            "an intentionally absent Renderer still reports pause-pending");
        require(display_runtime_state({ false, false, true, false, false, false }) == "applying",
            "an actual pending pause ACK was reported as settled");
        require(display_runtime_state({ true, true, true, false, false, true }) == "failed",
            "a failed Renderer was hidden behind a settled pause");
        require(display_runtime_state({ false, true, false, false, false, true }) == "applying",
            "an absent playback Renderer was reported as having presented a frame");
    }

    template <typename Require>
    inline void log_rotation_closes_reader_and_serializes_writers(
        std::filesystem::path const& root, Require require)
    {
        auto directory = root / L"log-rotation-regression";
        std::filesystem::create_directories(directory);
        auto path = directory / L"concurrent.log";
        {
            std::ofstream initial(path, std::ios::binary | std::ios::trunc);
            std::string line(1023, 'x');
            line += '\n';
            for (unsigned index = 0; index != 2050; ++index) initial << line;
            require(static_cast<bool>(initial), "failed to create bounded log rotation fixture");
        }
        std::vector<std::thread> writers;
        for (unsigned worker = 0; worker != 4; ++worker) {
            writers.emplace_back([path, worker] {
                for (unsigned entry = 0; entry != 8; ++entry) {
                    motion::append_utf8_log(path,
                        L"rotation-writer-" + std::to_wstring(worker) +
                        L"-entry-" + std::to_wstring(entry));
                }
            });
        }
        for (auto& writer : writers) writer.join();
        require(std::filesystem::file_size(path) < 2 * 1024 * 1024,
            "the input stream prevents log rotation or concurrent writers overwrite it");
        std::ifstream input(path, std::ios::binary);
        std::string contents((std::istreambuf_iterator<char>(input)), {});
        for (unsigned worker = 0; worker != 4; ++worker) {
            for (unsigned entry = 0; entry != 8; ++entry) {
                auto marker = "rotation-writer-" + std::to_string(worker) +
                    "-entry-" + std::to_string(entry) + "\n";
                auto first = contents.find(marker);
                require(first != std::string::npos &&
                        contents.find(marker, first + marker.size()) == std::string::npos,
                    "a concurrent rotation lost or duplicated a log line");
            }
        }
        auto temporary = path;
        temporary += L".rotate.tmp";
        require(!std::filesystem::exists(temporary),
            "a successful log rotation left its temporary file behind");
    }
}
