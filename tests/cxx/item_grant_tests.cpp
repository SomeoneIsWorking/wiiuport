// An item given as its item-get function gives it, and put on a button.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/ItemGrant.h"

#include <cstdint>
#include <map>
#include <string>

namespace {

using wiiuport::title::ItemGrant;

constexpr uint32_t kInfo = 0x145b7b60;

class Guest {
  public:
    std::map<uint32_t, uint32_t> words;

    ItemGrant grant() {
        return ItemGrant{{.readWords =
                              [this](uint32_t address, uint32_t* out, uint32_t count) {
                                  for (uint32_t i = 0; i < count; ++i) {
                                      out[i] = words[address + (4 * i)];
                                  }
                                  return true;
                              },
                          .writeWords =
                              [this](uint32_t address, const uint32_t* in, uint32_t count) {
                                  for (uint32_t i = 0; i < count; ++i) {
                                      words[address + (4 * i)] = in[i];
                                  }
                                  return true;
                              }}};
    }

    uint8_t byte(uint32_t offset) {
        uint32_t address = kInfo + offset;
        return static_cast<uint8_t>(words[address & ~3U] >> (24 - (8 * (address & 3U))));
    }
};

// A fresh file: every slot and button empty (0xff), no item got.
Guest freshFile() {
    Guest guest;
    guest.words[ItemGrant::kSaveInfo] = kInfo;
    for (uint32_t offset = 0x28; offset < 0x34; offset += 4) {
        guest.words[kInfo + offset] = 0x00ffffff;
    }
    for (uint32_t offset = 0x5c; offset < 0x70; offset += 4) {
        guest.words[kInfo + offset] = 0xffffffff;
    }
    guest.words[kInfo + 0x70] = 0xff000000;
    guest.words[kInfo + 0x74] = 0x00000100;
    return guest;
}

void theBombBagIsGivenAsItsItemGetFunctionGivesIt() {
    Guest guest = freshFile();
    ItemGrant grant = guest.grant();
    std::string refusal;
    auto request = check::present(ItemGrant::parse("item=bomb_bag&button=y", refusal),
                                  "a known item and button parse");
    check::isTrue(grant.grant(request, refusal), "and are given");
    check::equal(guest.byte(0x69), 0x31, "the bomb bag in slot 13");
    check::equal(guest.byte(0x71 + 13), 1, "got");
    check::equal(guest.byte(0x8a), 30, "thirty bombs");
    check::equal(guest.byte(0x90), 30, "of thirty");
    check::equal(guest.byte(0x2a), 13, "on Y");
    check::equal(guest.byte(0x29), 0xff, "X untouched");
    check::equal(guest.byte(0x68), 0xff, "the slot before untouched");
    check::equal(guest.byte(0x76), 1, "another got flag in the same word kept");
}

void theGrapplingHookTakesItsSlot() {
    Guest guest = freshFile();
    ItemGrant grant = guest.grant();
    std::string refusal;
    check::isTrue(
        grant.grant(
            check::present(ItemGrant::parse("item=grappling_hook&button=x", refusal), "parsed"),
            refusal),
        "the grappling hook is given");
    check::equal(guest.byte(0x5f), 0x25, "in slot 3");
    check::equal(guest.byte(0x71 + 3), 1, "got");
    check::equal(guest.byte(0x29), 3, "on X");
}

void theTelescopeAndPictoBoxTakeTheirSlots() {
    Guest guest = freshFile();
    ItemGrant grant = guest.grant();
    std::string refusal;
    check::isTrue(
        grant.grant(check::present(ItemGrant::parse("item=telescope&button=y", refusal), "parsed"),
                    refusal),
        "the telescope is given");
    check::equal(guest.byte(0x5c), 0x20, "in slot 0");
    check::equal(guest.byte(0x71), 1, "got");
    check::isTrue(
        grant.grant(check::present(ItemGrant::parse("item=picto_box&button=z", refusal), "parsed"),
                    refusal),
        "the picto box is given");
    check::equal(guest.byte(0x64), 0x23, "in slot 8");
    check::equal(guest.byte(0x71 + 8), 1, "got");
    check::equal(guest.byte(0x2b), 8, "on Z");
}

void unknownItemsAndButtonsAreRefused() {
    std::string refusal;
    check::isTrue(!ItemGrant::parse("item=master_sword&button=x", refusal), "an item not listed");
    check::isTrue(!ItemGrant::parse("item=bomb_bag&button=a", refusal), "a button that holds none");
    check::isTrue(!ItemGrant::parse("item=bomb_bag", refusal), "no button");
    Guest guest;
    ItemGrant grant = guest.grant();
    check::isTrue(
        !grant.grant(check::present(ItemGrant::parse("item=bomb_bag&button=x", refusal), "parsed"),
                     refusal),
        "no save info yet");
}

} // namespace

void wiiuport::tests::runItemGrantTests() {
    theBombBagIsGivenAsItsItemGetFunctionGivesIt();
    theGrapplingHookTakesItsSlot();
    theTelescopeAndPictoBoxTakeTheirSlots();
    unknownItemsAndButtonsAreRefused();
}
