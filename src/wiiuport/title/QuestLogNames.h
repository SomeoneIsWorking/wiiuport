#pragma once

#include "Cafe/OS/libs/swkbd/SwkbdHooks.h"
#include "wiiuport/title/DrawInterpolation.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wiiuport::title {

// Fills Wind Waker HD's name entry with Link, or Lunk when a quest log is already named Link, or
// Lonk when both are taken; the player can still edit it.
//
// The names come from the save `cking.sav`: three quest logs of kSlotBytes, each in use once its
// save time is set, then a table of the three names in UTF-16BE.
class QuestLogNames final : public SwkbdHooks::InitialText {
  public:
    static constexpr size_t kSlots = 3;
    static constexpr size_t kSlotBytes = 0xa94;
    // dSv_player_status_b_c's save time (u64); zero in a quest log never saved.
    static constexpr size_t kSaveTime = 0x18;
    static constexpr size_t kNames = 0x2034;
    static constexpr size_t kNameBytes = 0x12;
    static constexpr size_t kSaveBytes = kNames + (kSlots * kNameBytes);
    // g_dComIfG_gameInfo.play's start stage, "Name" from File Selection on.
    static constexpr uint32_t kStartStage = 0x1046f0b0 + 0x5134;
    static constexpr std::array<uint32_t, 2> kNameStage{0x4e616d65, 0}; // "Name"
    static constexpr std::string_view kSaveFile = "cking.sav";
    static constexpr std::array<std::u16string_view, 3> kChoices{u"Link", u"Lunk", u"Lonk"};

    struct QuestLog {
        bool inUse = false;
        std::u16string name;
    };

    // The save's bytes, or none when the title has no save yet.
    using ReadSave = std::function<std::optional<std::vector<uint8_t>>()>;

    struct Seams {
        DrawInterpolation::ReadWords readWords;
        ReadSave readSave;
    };

    explicit QuestLogNames(Seams seams);

    // The three quest logs, or none when the save is too short to hold them.
    static std::optional<std::array<QuestLog, kSlots>> parse(std::span<const uint8_t> save);

    // The first of kChoices no quest log in use is named; the last when all are.
    static std::u16string_view choose(std::span<const QuestLog> logs);

    // The name for the keyboard the title shows on its Name stage; empty on any other.
    std::u16string Provide() override;

  private:
    DrawInterpolation::ReadWords m_readWords;
    ReadSave m_readSave;
};

} // namespace wiiuport::title
