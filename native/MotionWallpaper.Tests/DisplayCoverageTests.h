#pragma once

#include "../MotionWallpaper.Agent/CoveragePolicy.h"
#include "../MotionWallpaper.Agent/RuntimePolicy.h"
#include "../MotionWallpaper.Common/DisplayTopology.h"
#include "../MotionWallpaper.Protocol/RendererProtocol.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace motion::tests
{
    inline std::vector<agent::DisplayCoverage> unequal_coverage_displays()
    {
        return {
            { L"\\\\.\\DISPLAY1", { -1920, 240, 0, 1320 }, { -1920, 240, 0, 1280 } },
            { L"\\\\.\\DISPLAY2", { 0, 0, 2560, 1440 }, { 0, 0, 2560, 1400 } },
            { L"\\\\.\\DISPLAY3", { 2560, -1080, 4480, 0 }, { 2560, -1080, 4480, -40 } }
        };
    }

    template <typename Require>
    void display_coverage_tracks_physical_bounds_and_fresh_snapshots(Require require)
    {
        using namespace agent;
        std::vector<std::wstring> targets{ L"\\\\.\\DISPLAY1", L"\\\\.\\DISPLAY2" };
        auto displays = unequal_coverage_displays();
        auto selection = select_display_coverage(displays, targets);
        require(selection.coveredDevices.empty() && !selection.allCovered,
            "uncovered outputs started in a globally paused state");

        // The left output has negative X, an offset Y, and a taskbar. A
        // maximized window covers its work area but not the right output.
        mark_covered_displays({ -1920, 240, 0, 1280 }, displays);
        selection = select_display_coverage(displays, targets);
        require(selection.coveredDevices == std::vector<std::wstring>{ targets[0] } && !selection.allCovered,
            "maximizing on the smaller negative-coordinate monitor paused its visible sibling");

        mark_covered_displays({ 0, 0, 2560, 1440 }, displays);
        selection = select_display_coverage(displays, targets);
        require(selection.coveredDevices == targets && selection.allCovered,
            "independent fullscreen windows failed to pause both covered outputs");

        displays = unequal_coverage_displays();
        mark_covered_displays({ -1920, 0, 2560, 1440 }, displays);
        selection = select_display_coverage(displays, targets);
        require(selection.coveredDevices == targets && selection.allCovered && !displays[2].covered,
            "a spanning window did not cover exactly the physical monitors within its bounds");

        // Window enumeration creates a new snapshot every time. Closing or
        // minimizing a fullscreen window must not retain last tick's flags.
        displays = unequal_coverage_displays();
        mark_covered_displays({ 400, 300, 1500, 1000 }, displays);
        selection = select_display_coverage(displays, targets);
        require(selection.coveredDevices.empty() && !selection.allCovered,
            "restoring a normal window retained the prior all-covered snapshot");

        mark_covered_displays({ 2560, -1080, 4480, 0 }, displays);
        selection = select_display_coverage(displays, targets);
        require(selection.coveredDevices.empty() && !selection.allCovered,
            "a fullscreen app on an untargeted display paused wallpaper elsewhere");

        displays = unequal_coverage_displays();
        mark_covered_displays({ 0, 0, 0, 1440 }, displays);
        mark_covered_displays({ 2560, 1440, 0, 0 }, displays);
        require(select_display_coverage(displays, targets).coveredDevices.empty(),
            "empty or inverted window bounds were treated as coverage");
        require(!covers_display({ -100, -100, 100, 100 }, {}) &&
            !covers_display({ -100, -100, 100, 100 }, { 10, 10, -10, -10 }),
            "invalid display bounds were treated as a covered monitor");
        require(covers_display({ 2, 2, 2558, 1438 }, { 0, 0, 2560, 1440 }) &&
            !covers_display({ 3, 3, 2557, 1437 }, { 0, 0, 2560, 1440 }),
            "physical coverage tolerance swallowed a genuinely uncovered border");
        require(!select_display_coverage(displays, {}).allCovered &&
            !select_display_coverage(displays, { L"removed-monitor" }).allCovered,
            "empty or stale target sets were treated as globally covered");
    }

    template <typename Require>
    void coverage_masks_preserve_user_and_screensaver_policy(Require require)
    {
        using namespace agent;
        Settings settings;
        settings.desktopPlayback = true;
        settings.activePlaybackEnabled = true;
        settings.continueWhenCovered = false;
        settings.screensaverEnabled = true;
        settings.idleTimeoutSeconds = 30;
        auto displays = unequal_coverage_displays();
        std::vector<std::wstring> targets{ displays[0].deviceName, displays[1].deviceName };
        mark_covered_displays(displays[0].bounds, displays);
        RuntimeSignals signals;
        signals.hasMedia = true;
        signals.covered = select_display_coverage(displays, targets).allCovered;
        require(reduce_runtime_action(settings, signals) == RuntimeAction::DesktopPlay,
            "partial coverage stopped the shared decoder required by a visible sibling");

        mark_covered_displays(displays[1].bounds, displays);
        signals.covered = select_display_coverage(displays, targets).allCovered;
        require(reduce_runtime_action(settings, signals) == RuntimeAction::DesktopPaused,
            "a fully covered desktop kept decoding video");
        settings.continueWhenCovered = true;
        require(reduce_runtime_action(settings, signals) == RuntimeAction::DesktopPlay,
            "the explicit continue-when-covered setting did not bypass coverage");
        settings.continueWhenCovered = false;
        signals.idleSeconds = 30;
        require(reduce_runtime_action(settings, signals) == RuntimeAction::ScreensaverPlay,
            "desktop coverage suppressed the independent idle screensaver");
        settings.desktopPlayback = false;
        require(reduce_runtime_action(settings, signals) == RuntimeAction::ScreensaverPlay,
            "turning off desktop playback also disabled the requested screensaver");
        signals.idleSeconds = 0;
        require(reduce_runtime_action(settings, signals) == RuntimeAction::Stopped,
            "desktop coverage overrode the user's disabled desktop preference");
        settings.desktopPlayback = true;
        settings.activePlaybackEnabled = false;
        signals.covered = false;
        require(reduce_runtime_action(settings, signals) == RuntimeAction::DesktopFrozen,
            "uncovering a monitor silently re-enabled user-paused playback");
        settings.activePlaybackEnabled = true;
        require(reduce_runtime_action(settings, signals) == RuntimeAction::DesktopPlay,
            "restoring a visible monitor failed to resume enabled playback");
        signals.sessionLocked = true;
        signals.idleSeconds = 30;
        require(reduce_runtime_action(settings, signals) == RuntimeAction::Locked,
            "screensaver or coverage overrode session locking");
        signals.displayOn = false;
        require(reduce_runtime_action(settings, signals) == RuntimeAction::DisplayOff,
            "coverage kept rendering while display power was off");
    }

    // Callbacks reuse the existing job-isolated hidden Renderer fixture. No
    // wallpaper assignment, monitor topology, or app setting is changed.
    template <typename Require, typename Launch, typename ReadLine, typename Stop>
    void real_renderer_partial_coverage_keeps_shared_decode_alive(
        std::filesystem::path const& root, Require require, Launch launch, ReadLine readLine, Stop stop)
    {
        auto displays = enumerate_displays();
        if (displays.size() < 2 || displays[0].deviceName == displays[1].deviceName) {
            std::cout << "    SKIP physical mixed-coverage integration: two active monitors required\n";
            return;
        }
        auto media = root / L"decode-probe-h264.mp4";
        require(std::filesystem::is_regular_file(media), "coverage integration video fixture is missing");
        std::vector<std::wstring> monitors{ displays[0].deviceName, displays[1].deviceName };
        auto renderer = launch(media, monitors);
        auto originalId = renderer.id;
        auto coveredDevice = motion::wide_to_utf8(monitors[0]);
        bool forbidRelease{};
        auto read = [&]() {
            auto line = readLine(renderer.output.get(), std::chrono::milliseconds(250));
            if (line.starts_with("error ")) std::cerr << line << '\n';
            require(!line.starts_with("error "), "partial coverage caused a Renderer error");
            require(!forbidRelease || !line.starts_with("decoder released "),
                "covering only one output released its sibling's shared decoder");
            require(WaitForSingleObject(renderer.process.get(), 0) == WAIT_TIMEOUT && renderer.id == originalId,
                "coverage transition retired the existing shared Renderer");
            return line;
        };
        auto apply = [&](std::string const& command, uint64_t revision, std::string const& state) {
            renderer.Send(command + " " + std::to_string(revision) + "\n");
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            bool acknowledged{};
            while (std::chrono::steady_clock::now() < deadline) {
                auto ack = protocol::parse_ack(read());
                if (ack.channel == protocol::AckChannel::Target && ack.revision == revision && ack.state == state) {
                    acknowledged = true;
                    break;
                }
            }
            require(acknowledged, "coverage transition did not acknowledge the requested target");
        };
        auto progress = [&](uint64_t revision) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            uint64_t first{}, last{};
            while (std::chrono::steady_clock::now() < deadline) {
                auto heartbeat = protocol::parse_playback_heartbeat(read());
                if (!heartbeat || heartbeat->revision != revision || !heartbeat->serial) continue;
                if (!first) first = heartbeat->serial;
                last = heartbeat->serial;
                if (last > first) break;
            }
            require(first && last > first,
                "visible output stopped advancing when its shared-decoder sibling was covered");
        };
        apply("desktop-play", 701, "playing");
        progress(701);
        forbidRelease = true;
        // The wire format puts coverage devices after the revision.
        renderer.Send("desktop-play 702 " + coveredDevice + "\n");
        bool maskedAck{};
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            auto ack = protocol::parse_ack(read());
            if (ack.channel == protocol::AckChannel::Target && ack.revision == 702 && ack.state == "playing") {
                maskedAck = true;
                break;
            }
        }
        require(maskedAck, "covered output blocked the visible output's first-frame acknowledgement");
        progress(702);
        apply("desktop-play", 703, "playing");
        progress(703);
        // Reapply the mask, then enter a screensaver. Screensaver presentation
        // must clear the desktop-only mask, and must preserve this process.
        renderer.Send("desktop-play 704 " + coveredDevice + "\n");
        progress(704);
        apply("screensaver-play", 705, "playing");
        progress(705);
        forbidRelease = false;
        apply("pause", 706, "paused");
        apply("desktop-play", 707, "playing");
        progress(707);
        stop(renderer, 708);
    }
}
