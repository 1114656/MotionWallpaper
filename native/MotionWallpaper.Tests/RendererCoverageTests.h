#pragma once

#include "../MotionWallpaper.Renderer/RendererCommand.h"
#include "../MotionWallpaper.Renderer/OutputFramePolicy.h"

#include <array>
#include <stdexcept>

namespace motion::tests
{
    inline void renderer_coverage_command_tests()
    {
        using namespace motion::renderer;
        auto check = [](bool value, char const* message) {if (!value) throw std::runtime_error(message);};
        RendererCommand request;
        check(parse_renderer_command(R"(desktop-play 21 \\.\DISPLAY2 \\.\DISPLAY1 \\.\DISPLAY2)", request) ==
            CommandParseError::None, "covered display command rejected");
        check(request.command == Command::DesktopPlay && request.revision == 21 && request.coveredDisplays ==
            std::vector<std::wstring>{LR"(\\.\DISPLAY1)", LR"(\\.\DISPLAY2)"},
            "coverage mask was not transported atomically and normalized");
        RendererCommandMailbox mailbox;
        bool notify{};
        check(mailbox.Put(request, &notify) && notify, "first covered request rejected or not dispatched");
        check(parse_renderer_command("desktop-play 22", request) == CommandParseError::None &&
            request.coveredDisplays.empty() && mailbox.Put(request, &notify) && !notify,
            "uncover request retained stale display mask or queued a redundant window message");
        check(!mailbox.Put({Command::DesktopPlay, 20, {LR"(\\.\DISPLAY1)"}}),
            "stale covered request replaced latest uncover request");
        auto pending = mailbox.Take();
        check(pending && pending->revision == 22 && pending->coveredDisplays.empty() && !mailbox.Take(),
            "mailbox did not coalesce the complete latest request");
        check(mailbox.Put({Command::Pause, 23, {}}) && mailbox.Put({Command::Stop, 24, {}}) &&
            !mailbox.Put({Command::DesktopPlay, 25, {}}), "shutdown command was overwritten");
        pending = mailbox.Take();
        check(pending && pending->command == Command::Stop, "stop command was lost");
        check(parse_renderer_command(R"(pause 30 \\.\DISPLAY1)", request) == CommandParseError::InvalidPayload,
            "pause command accepted a stale desktop-only mask");
        check(parse_renderer_command(R"(screensaver-play 31 \\.\DISPLAY1)", request) == CommandParseError::InvalidPayload,
            "screen saver command accepted desktop coverage");
        check(parse_renderer_command(R"(desktop-play 32 \\.\DISPLAY)", request) == CommandParseError::InvalidPayload &&
            parse_renderer_command(R"(desktop-play 32 \\.\DISPLAY1x)", request) == CommandParseError::InvalidPayload &&
            parse_renderer_command("desktop-play -1", request) == CommandParseError::MissingRevision,
            "malformed coverage command accepted");
        check(parse_renderer_command("desktop-play 18446744073709551615", request) == CommandParseError::None &&
            request.revision == UINT64_MAX, "64-bit revision was truncated in command transport");
        std::string excessive = "desktop-play 33";
        for (unsigned index = 0; index != 65; ++index) excessive += R"( \\.\DISPLAY)" + std::to_string(index);
        check(parse_renderer_command(excessive, request) == CommandParseError::InvalidPayload,
            "unbounded display mask accepted");
    }

    inline void renderer_coverage_output_tests()
    {
        using namespace motion::renderer;
        auto check = [](bool value, char const* message) {if (!value) throw std::runtime_error(message);};
        struct Output {DesktopOutputPolicy policy; OutputFrameProgress progress;};
        std::array<Output, 2> outputs{{{{true}, {}}, {{false}, {}}}};
        // The covered swap chain never becomes ready. Its visible sibling must
        // continue presenting one shared decode timeline and satisfy startup.
        for (uint64_t serial = 1; serial <= 240; ++serial) {
            for (auto& output : outputs)
                if (output.policy.ShouldPresent(false)) output.progress.Presented(serial, false);
        }
        check(outputs[0].progress.presentedSerial == 0 && outputs[1].progress.presentedSerial == 240,
            "covered output consumed shared frames or stopped visible sibling");
        bool ready = true;
        uint64_t heartbeat = UINT64_MAX;
        for (auto const& output : outputs) {
            ready = ready && (!output.policy.ShouldPresent(false) || output.progress.Ready(false));
            if (output.policy.ContributesProgress()) heartbeat = (std::min)(heartbeat, output.progress.presentedSerial);
        }
        check(ready && heartbeat == 240, "covered output stalled first-frame acknowledgement or watchdog progress");
        outputs[0].policy.covered = false;
        check(outputs[0].progress.NeedsFrame(241, false), "uncovered output cannot join current timeline");
        for (auto& output : outputs) output.progress.Presented(241, false);
        check(outputs[0].progress.presentedSerial == outputs[1].progress.presentedSerial,
            "uncover restarted a separate video timeline");
        // Whole-route freeze still captures every surface so decoder release
        // and static residency do not depend on occluded swap-chain readiness.
        outputs[0].policy.covered = true;
        for (auto& output : outputs) {
            output.progress.BeginFreeze();
            check(output.policy.ShouldPresent(true), "coverage suppressed required freeze capture");
            output.progress.Presented(242, true);
            check(output.progress.Ready(true), "freeze could not compact every shared output");
        }
    }
}
