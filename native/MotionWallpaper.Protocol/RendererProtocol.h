#pragma once

#include <cstdint>
#include <charconv>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace motion::protocol
{
    enum class AckChannel { Unknown, Target, Control };

    struct Ack
    {
        AckChannel channel{ AckChannel::Unknown };
        uint64_t revision{};
        std::string state;
    };

    struct DecodeStatus
    {
        std::string path;
        std::string reason;
    };

    struct PlaybackHeartbeat
    {
        uint64_t revision{}, serial{}, period100ns{};
    };

    [[nodiscard]] inline std::optional<PlaybackHeartbeat> parse_playback_heartbeat(std::string_view value)
    {
        if (!value.empty() && value.back() == '\r') value.remove_suffix(1);
        constexpr std::string_view prefix = "status playback ";
        if (!value.starts_with(prefix) || value.size() > 128) return std::nullopt;
        value.remove_prefix(prefix.size());
        PlaybackHeartbeat result;
        uint64_t* fields[]{ &result.revision, &result.serial, &result.period100ns };
        for (size_t index = 0; index < 3; ++index) {
            auto end = value.find(' ');
            auto token = value.substr(0, end);
            auto parsed = std::from_chars(token.data(), token.data() + token.size(), *fields[index]);
            if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) return std::nullopt;
            if (index == 2) { if (end != std::string_view::npos) return std::nullopt; }
            else { if (end == std::string_view::npos) return std::nullopt; value.remove_prefix(end + 1); }
        }
        if (!result.revision || !result.period100ns || result.period100ns > 3'000'000'000ULL) return std::nullopt;
        return result;
    }

    struct RendererError
    {
        uint64_t revision{};
        std::string operation;
        uint32_t code{};
    };

    [[nodiscard]] inline std::optional<RendererError> parse_renderer_error(std::string_view value)
    {
        if (value.size() > 256) return std::nullopt;
        std::istringstream input{ std::string(value) };
        std::string tag, revision, code, trailing;
        RendererError result;
        if (!(input >> tag >> revision >> result.operation >> code) || tag != "error" ||
            (input >> trailing) || result.operation.empty() || result.operation.size() > 64 ||
            !code.starts_with("0x") || code.size() < 3 || code.size() > 10) return std::nullopt;
        for (auto character : result.operation) {
            if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                    (character >= '0' && character <= '9') || character == '-' || character == '_')) {
                return std::nullopt;
            }
        }
        auto parsedRevision = std::from_chars(revision.data(), revision.data() + revision.size(), result.revision);
        auto parsedCode = std::from_chars(code.data() + 2, code.data() + code.size(), result.code, 16);
        if (parsedRevision.ec != std::errc{} || parsedRevision.ptr != revision.data() + revision.size() ||
            parsedCode.ec != std::errc{} || parsedCode.ptr != code.data() + code.size()) return std::nullopt;
        return result;
    }

    [[nodiscard]] inline std::string format_renderer_error(std::string_view operation, uint32_t code)
    {
        std::ostringstream output;
        output << operation << " 0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << code;
        return output.str();
    }

    [[nodiscard]] inline Ack parse_ack(std::string_view value)
    {
        std::istringstream input{ std::string(value) };
        std::string tag;
        std::string channel;
        Ack result;
        if (!(input >> tag >> channel >> result.revision >> result.state) || tag != "ack" || !result.revision) return {};
        if (channel == "target") result.channel = AckChannel::Target;
        else if (channel == "control") result.channel = AckChannel::Control;
        else return {};
        return result;
    }

    [[nodiscard]] inline DecodeStatus parse_decode_status(std::string_view value)
    {
        std::istringstream input{ std::string(value) };
        std::string tag;
        std::string channel;
        DecodeStatus result;
        if (!(input >> tag >> channel >> result.path >> result.reason) ||
            tag != "status" || channel != "decode") return {};
        std::string trailing;
        if (input >> trailing) return {};
        return result;
    }
}
