#pragma once

#include "../MotionWallpaper.Agent/RuntimeEventPolicy.h"
#include "../MotionWallpaper.Protocol/RendererProtocol.h"

namespace motion::tests
{
    template <typename Require>
    void session_connection_never_implies_unlock(Require require)
    {
        motion::agent::RuntimeSessionState state;
        require(state.Inactive(), "unknown startup lock state allowed presentation");
        state.Observe(true, false);
        require(!state.Inactive(), "a verified active unlocked session stayed paused");
        state.Notify(0x7);
        state.Observe(true, std::nullopt);
        require(state.Inactive(), "connection polling cleared a real lock notification");
        state.Notify(0x4);
        state.Notify(0x3);
        require(state.Inactive(), "remote reconnect was mistaken for unlock");
        state.Notify(0x8);
        require(!state.Inactive(), "unlock on a connected session did not restore eligibility");
        state.Notify(0x2);
        state.Observe(std::nullopt, std::nullopt);
        require(state.Inactive(), "failed polling resumed a disconnected session");
        state.Notify(0x8);
        require(state.Inactive(), "unlock alone resumed a disconnected session");
        state.Observe(true, false);
        require(!state.Inactive(), "a verified console return did not restore eligibility");
        state.Observe(true, true);
        require(state.Inactive(), "query reported locked but presentation remained active");
    }

    template <typename Require>
    void renderer_error_details_are_bounded_and_exact(Require require)
    {
        using namespace motion::protocol;
        auto error = parse_renderer_error("error 19 present 0x887a0005");
        require(error && error->revision == 19 && error->operation == "present" &&
            error->code == 0x887a0005u, "renderer device-removed HRESULT was lost");
        require(format_renderer_error(error->operation, error->code) == "present 0x887A0005",
            "renderer HRESULT was formatted ambiguously");
        require(parse_renderer_error("error 0 media 0xc00d36b4").has_value(),
            "startup diagnostics without a target revision were discarded");
        for (auto malformed : { "error 1 media 0x100000000", "error -1 media 0x1",
                "error 1 media 0x", "error 1 media 0xZZ", "error 1 media 0x1 trailing",
                "error 1 media:unsafe 0x1", "error 18446744073709551616 media 0x1" }) {
            require(!parse_renderer_error(malformed), "invalid renderer diagnostic was accepted");
        }
        require(!parse_renderer_error(std::string(300, 'x')), "unbounded renderer diagnostic was accepted");
    }
}
