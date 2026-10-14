#include "wiiuport/title/QuestLogNames.h"

#include "wiiuport/content/BigEndian.h"

#include <lucent/log.h>

#include <algorithm>
#include <utility>

namespace wiiuport::title {

QuestLogNames::QuestLogNames(Seams seams)
    : m_readWords(std::move(seams.readWords)), m_readSave(std::move(seams.readSave)) {
}

std::optional<std::array<QuestLogNames::QuestLog, QuestLogNames::kSlots>>
QuestLogNames::parse(std::span<const uint8_t> save) {
    if (save.size() < kSaveBytes) {
        return std::nullopt;
    }
    std::array<QuestLog, kSlots> logs;
    for (size_t slot = 0; slot < kSlots; ++slot) {
        const auto time = save.subspan((slot * kSlotBytes) + kSaveTime, 8);
        logs.at(slot).inUse = std::any_of(time.begin(), time.end(), [](uint8_t byte) {
            return byte != 0;
        });
        for (size_t at = kNames + (slot * kNameBytes); at + 1 < kNames + ((slot + 1) * kNameBytes);
             at += 2) {
            const auto unit = static_cast<char16_t>(content::readU16(save, at));
            if (unit == 0) {
                break;
            }
            logs.at(slot).name.push_back(unit);
        }
    }
    return logs;
}

std::u16string_view QuestLogNames::choose(std::span<const QuestLog> logs) {
    for (std::u16string_view choice : kChoices) {
        const bool taken = std::any_of(logs.begin(), logs.end(), [choice](const QuestLog& log) {
            return log.inUse && log.name == choice;
        });
        if (!taken) {
            return choice;
        }
    }
    return kChoices.back();
}

std::u16string QuestLogNames::Provide() {
    std::array<uint32_t, 2> stage{};
    if (!m_readWords(kStartStage, stage.data(), static_cast<uint32_t>(stage.size())) ||
        stage[0] != kNameStage[0] || (stage[1] >> 24) != kNameStage[1]) {
        return {};
    }
    const std::optional<std::vector<uint8_t>> save = m_readSave();
    if (!save) {
        return std::u16string(kChoices.front());
    }
    const auto logs = parse(*save);
    if (!logs) {
        lucent::error("name",
                      "the save is {} bytes, too short for three quest logs; no name filled",
                      save->size());
        return {};
    }
    return std::u16string(choose(*logs));
}

} // namespace wiiuport::title
