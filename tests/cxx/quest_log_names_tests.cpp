#include "check.h"
#include "suites.h"
#include "wiiuport/title/QuestLogNames.h"

#include <optional>
#include <string>
#include <vector>

using wiiuport::title::QuestLogNames;

namespace {

// A save with each quest log's save time set or not and its name in the names table.
std::vector<uint8_t> saveOf(const std::vector<std::pair<bool, std::u16string>>& logs) {
    std::vector<uint8_t> save(QuestLogNames::kSaveBytes + 0xcc, 0);
    for (size_t slot = 0; slot < logs.size(); ++slot) {
        if (logs[slot].first) {
            save.at((slot * QuestLogNames::kSlotBytes) + QuestLogNames::kSaveTime + 7) = 0x2a;
        }
        size_t at = QuestLogNames::kNames + (slot * QuestLogNames::kNameBytes);
        for (char16_t unit : logs[slot].second) {
            save.at(at) = static_cast<uint8_t>(unit >> 8);
            save.at(at + 1) = static_cast<uint8_t>(unit);
            at += 2;
        }
    }
    return save;
}

std::u16string chosen(const std::vector<std::pair<bool, std::u16string>>& logs) {
    const auto parsed = QuestLogNames::parse(saveOf(logs));
    return parsed ? std::u16string(QuestLogNames::choose(*parsed)) : u"(unparsed)";
}

struct FakeTitle {
    std::array<uint32_t, 2> stage{};
    std::optional<std::vector<uint8_t>> save;
};

QuestLogNames namesFor(FakeTitle& title) {
    return QuestLogNames({.readWords =
                              [&title](uint32_t address, uint32_t* words, uint32_t count) {
                                  if (address != QuestLogNames::kStartStage || count != 2) {
                                      return false;
                                  }
                                  words[0] = title.stage[0];
                                  words[1] = title.stage[1];
                                  return true;
                              },
                          .readSave =
                              [&title] {
                                  return title.save;
                              }});
}

} // namespace

void wiiuport::tests::runQuestLogNamesTests() {
    check::isTrue(chosen({{false, u"Link"}, {false, u"Link"}, {false, u"Link"}}) == u"Link",
                  "empty quest logs carry Link and do not take it");
    check::isTrue(chosen({{true, u"T"}, {false, u"Link"}, {false, u"Link"}}) == u"Link",
                  "a quest log with another name leaves Link free");
    check::isTrue(chosen({{false, u"Link"}, {true, u"Link"}, {false, u"Link"}}) == u"Lunk",
                  "a quest log named Link gives Lunk");
    check::isTrue(chosen({{true, u"Lunk"}, {true, u"Link"}, {false, u"Link"}}) == u"Lonk",
                  "Link and Lunk taken give Lonk");
    check::isTrue(chosen({{true, u"Lunk"}, {false, u"Link"}, {false, u"Link"}}) == u"Link",
                  "Lunk alone leaves Link free");
    check::isTrue(chosen({{true, u"Lonk"}, {true, u"Lunk"}, {true, u"Link"}}) == u"Lonk",
                  "all three taken still gives Lonk");
    check::isTrue(chosen({{true, u"link"}, {false, u""}, {false, u""}}) == u"Link",
                  "names compare exactly");
    {
        const auto parsed =
            QuestLogNames::parse(saveOf({{true, u"Tetra123"}, {false, u""}, {false, u""}}));
        check::isTrue(parsed && (*parsed)[0].inUse && (*parsed)[0].name == u"Tetra123" &&
                          !(*parsed)[1].inUse,
                      "an eight-letter name reads whole and the save time marks the log in use");
    }
    check::isTrue(!QuestLogNames::parse(std::vector<uint8_t>(QuestLogNames::kSaveBytes - 1, 0)),
                  "a save too short for the names table is not read");
    {
        FakeTitle title;
        title.stage = QuestLogNames::kNameStage;
        title.save = saveOf({{true, u"Link"}, {false, u"Link"}, {false, u"Link"}});
        QuestLogNames names = namesFor(title);
        check::isTrue(names.Provide() == u"Lunk", "on the Name stage the keyboard gets the name");
        title.save.reset();
        check::isTrue(names.Provide() == u"Link", "with no save yet it gets Link");
        title.save = std::vector<uint8_t>(16, 0);
        check::isTrue(names.Provide().empty(), "a save it cannot read fills nothing");
        title.stage = {0x73656100, 0}; // "sea"
        title.save.reset();
        check::isTrue(names.Provide().empty(), "on any other stage the keyboard stays empty");
        title.stage = {0x4e616d65, 0x32000000}; // "Name2"
        check::isTrue(names.Provide().empty(), "and the stage name must be Name exactly");
    }
}
