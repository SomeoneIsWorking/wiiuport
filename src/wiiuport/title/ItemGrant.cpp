#include "wiiuport/title/ItemGrant.h"

#include <utility>

namespace wiiuport::title {

ItemGrant::ItemGrant(Seams seams)
    : m_readWords(std::move(seams.readWords)), m_writeWords(std::move(seams.writeWords)) {
}

std::optional<ItemGrant::Request> ItemGrant::parse(std::string_view query, std::string& refusal) {
    std::optional<Item> item;
    std::optional<uint8_t> button;
    while (!query.empty()) {
        size_t end = query.find('&');
        std::string_view pair = query.substr(0, end);
        query = end == std::string_view::npos ? std::string_view{} : query.substr(end + 1);
        size_t equals = pair.find('=');
        std::string_view key = pair.substr(0, equals);
        std::string_view value =
            equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);
        if (key == "item") {
            for (const Item& known : kItemTable) {
                if (known.name == value) {
                    item = known;
                }
            }
        } else if (key == "button") {
            for (size_t index = 0; index < kButtons.size(); ++index) {
                if (kButtons.at(index) == value) {
                    button = static_cast<uint8_t>(index);
                }
            }
        } else {
            refusal = "unknown parameter " + std::string(key) + ".\n";
            return std::nullopt;
        }
    }
    if (!item) {
        refusal = "item is telescope, picto_box, grappling_hook or bomb_bag.\n";
        return std::nullopt;
    }
    if (!button) {
        refusal = "button is x, y or z.\n";
        return std::nullopt;
    }
    return Request{.item = *item, .button = *button};
}

bool ItemGrant::setByte(ByteWrite write) {
    const uint32_t address = write.address;
    uint32_t word = 0;
    const uint32_t aligned = address & ~3U;
    const uint32_t shift = 24 - (8 * (address & 3U));
    if (!m_readWords(aligned, &word, 1)) {
        return false;
    }
    if (!write.merge) {
        word &= ~(0xffU << shift);
    }
    word |= uint32_t{write.value} << shift;
    return m_writeWords(aligned, &word, 1);
}

bool ItemGrant::grant(const Request& request, std::string& refusal) {
    uint32_t saveInfo = 0;
    if (!m_readWords(kSaveInfo, &saveInfo, 1) || saveInfo == 0) {
        refusal = "the save info does not read; is the title in play?\n";
        return false;
    }
    const Item& item = request.item;
    bool written =
        setByte({.address = saveInfo + kGotItems + item.slot, .value = 1, .merge = true}) &&
        setByte({.address = saveInfo + kItems + item.slot, .value = item.number, .merge = false}) &&
        setByte({.address = saveInfo + kSelectItem + request.button,
                 .value = item.slot,
                 .merge = false});
    if (written && item.count != 0) {
        written =
            setByte({.address = saveInfo + item.count, .value = item.amount, .merge = false}) &&
            setByte({.address = saveInfo + item.maximum, .value = item.amount, .merge = false});
    }
    if (!written) {
        refusal = "the save info did not write.\n";
        return false;
    }
    return true;
}

} // namespace wiiuport::title
