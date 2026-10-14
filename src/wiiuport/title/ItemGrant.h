#pragma once

#include "wiiuport/title/DrawInterpolation.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wiiuport::title {

// Gives the player an item the way Wind Waker HD's own item-get functions do, and puts it on a
// button: a tool's way to the actors only an item reaches (the grappling hook's rope, what bombs
// break).
//
// Writes what the item number's entry in the item-get table (0x101e3a98) writes: the got flag, the
// inventory slot and any count, then the button's select slot (dSv_player_status_a_c, 0x20 later
// in HD's save info than TWW's).
class ItemGrant {
  public:
    // Holds the save info.
    static constexpr uint32_t kSaveInfo = 0x101f84dc;
    // X, Y and Z: the inventory slot each button uses.
    static constexpr uint32_t kSelectItem = 0x29;
    // dSv_player_item_c: the item number in each slot.
    static constexpr uint32_t kItems = 0x5c;
    // dSv_player_get_item_c: bit 0 of a slot's byte, as onGetItem (0x025b5ccc) sets it.
    static constexpr uint32_t kGotItems = 0x71;

    struct Item {
        std::string_view name;
        uint8_t slot;
        uint8_t number;
        // A count and its maximum the item-get function sets, at these save info offsets (0: none).
        uint32_t count;
        uint32_t maximum;
        uint8_t amount;
    };

    // item_func_rope (0x0254e3a8) and item_func_bomb_bag (0x0254e888).
    static constexpr std::array<Item, 2> kItemTable{{
        {.name = "grappling_hook",
         .slot = 3,
         .number = 0x25,
         .count = 0,
         .maximum = 0,
         .amount = 0},
        {.name = "bomb_bag",
         .slot = 13,
         .number = 0x31,
         .count = 0x8a,
         .maximum = 0x90,
         .amount = 30},
    }};
    static constexpr std::array<std::string_view, 3> kButtons{"x", "y", "z"};

    struct Request {
        Item item;
        uint8_t button = 0;
    };

    struct Seams {
        DrawInterpolation::ReadWords readWords;
        DrawInterpolation::WriteWords writeWords;
    };

    explicit ItemGrant(Seams seams);

    // `item=<name>&button=<x|y|z>`, or a refusal naming what is wrong.
    static std::optional<Request> parse(std::string_view query, std::string& refusal);

    // Gives the item; a refusal names why it was not given.
    bool grant(const Request& request, std::string& refusal);

  private:
    // One byte of a big-endian word, written over or merged into what is there.
    struct ByteWrite {
        uint32_t address;
        uint8_t value;
        bool merge;
    };

    bool setByte(ByteWrite write);

    DrawInterpolation::ReadWords m_readWords;
    DrawInterpolation::WriteWords m_writeWords;
};

} // namespace wiiuport::title
