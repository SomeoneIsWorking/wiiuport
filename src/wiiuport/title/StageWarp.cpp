#include "wiiuport/title/StageWarp.h"

#include <array>
#include <charconv>
#include <system_error>
#include <utility>

namespace wiiuport::title {

namespace {

std::optional<int> integer(std::string_view text) {
    int value = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

} // namespace

StageWarp::StageWarp(Seams seams)
    : m_readWords(std::move(seams.readWords)), m_writeWords(std::move(seams.writeWords)) {
}

std::optional<StageWarp::Request> StageWarp::parse(std::string_view query, std::string& refusal) {
    Request request;
    std::optional<int> point;
    std::optional<int> room;
    std::optional<int> layer = -1;
    while (!query.empty()) {
        size_t end = query.find('&');
        std::string_view pair = query.substr(0, end);
        query = end == std::string_view::npos ? std::string_view{} : query.substr(end + 1);
        size_t equals = pair.find('=');
        std::string_view key = pair.substr(0, equals);
        std::string_view value =
            equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);
        if (key == "stage") {
            request.stage = std::string(value);
        } else if (key == "point") {
            point = integer(value);
        } else if (key == "room") {
            room = integer(value);
        } else if (key == "layer") {
            layer = integer(value);
        } else {
            refusal = "unknown parameter " + std::string(key) + ".\n";
            return std::nullopt;
        }
    }
    if (request.stage.empty() || request.stage.size() > kMaxName) {
        refusal = "stage names 1 to 7 characters, e.g. stage=sea.\n";
        return std::nullopt;
    }
    if (!point || *point < INT16_MIN || *point > INT16_MAX) {
        refusal = "point is a spawn point number (s16).\n";
        return std::nullopt;
    }
    if (!room || *room < INT8_MIN || *room > INT8_MAX || !layer || *layer < INT8_MIN ||
        *layer > INT8_MAX) {
        refusal = "room and layer are s8; layer -1 picks the stage's own.\n";
        return std::nullopt;
    }
    request.point = static_cast<int16_t>(*point);
    request.room = static_cast<int8_t>(*room);
    request.layer = static_cast<int8_t>(*layer);
    return request;
}

bool StageWarp::warp(const Request& request, std::string& refusal) {
    uint32_t enable = 0;
    uint32_t saveInfo = 0;
    uint32_t restart = 0;
    uint32_t player = 0;
    if (!m_readWords(kEnableWord, &enable, 1) || !m_readWords(kSaveInfo, &saveInfo, 1) ||
        saveInfo == 0 || !m_readWords(saveInfo + kRestartPoint, &restart, 1) ||
        !m_readWords(kPlayer, &player, 1)) {
        refusal = "the stage request does not read; is the title in play?\n";
        return false;
    }
    if ((enable >> 24) != 0) {
        refusal = "a stage change is already pending.\n";
        return false;
    }
    uint32_t mode = 0;
    if (player != 0) {
        uint32_t flags = 0;
        uint32_t tinkle = 0;
        if (!m_readWords(player + kPlayerFlags, &flags, 1) ||
            !m_readWords(player + (kTinkleShield & ~3U), &tinkle, 1)) {
            refusal = "the player does not read.\n";
            return false;
        }
        auto tinkleHalf =
            static_cast<int16_t>((kTinkleShield & 2U) != 0 ? tinkle & 0xffff : tinkle >> 16);
        mode = ((flags & 1U) != 0 ? 0x8000U : 0U) | ((flags & 0x8000U) != 0 ? 0x4000U : 0U) |
               (static_cast<uint32_t>(static_cast<int32_t>(tinkleHalf)) << 16);
    }
    std::array<uint32_t, kNameWords> name{};
    for (size_t i = 0; i < request.stage.size(); ++i) {
        name.at(i / 4) |= uint32_t{static_cast<uint8_t>(request.stage[i])} << (24 - (8 * (i % 4)));
    }
    uint32_t place = (uint32_t{static_cast<uint16_t>(request.point)} << 16) |
                     (uint32_t{static_cast<uint8_t>(request.room)} << 8) |
                     static_cast<uint8_t>(request.layer);
    uint32_t restartPoint =
        (uint32_t{static_cast<uint16_t>(request.point)} << 16) | (restart & 0xffff);
    uint32_t lastSpeed = 0;
    // Wipe 0 and the bytes after the request kept; enable last, so the request is whole when seen.
    uint32_t enabled = (1U << 24) | (enable & 0xffff);
    if (!m_writeWords(kNextStage, name.data(), kNameWords) ||
        !m_writeWords(kPlaceWord, &place, 1) ||
        !m_writeWords(saveInfo + kLastSpeed, &lastSpeed, 1) ||
        !m_writeWords(saveInfo + kLastMode, &mode, 1) ||
        !m_writeWords(saveInfo + kRestartPoint, &restartPoint, 1) ||
        !m_writeWords(kEnableWord, &enabled, 1)) {
        refusal = "the stage request did not write.\n";
        return false;
    }
    return true;
}

} // namespace wiiuport::title
