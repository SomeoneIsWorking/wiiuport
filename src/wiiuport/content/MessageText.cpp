#include "wiiuport/content/MessageText.h"

#include "wiiuport/content/BigEndian.h"

#include <cstring>

namespace wiiuport::content {
namespace {

constexpr size_t kHeaderSize = 0x20;
constexpr size_t kSectionHeader = 0x10;
constexpr uint8_t kPadding = 0xab;

size_t alignSixteen(size_t value) {
    return (value + 15) & ~size_t{15};
}

} // namespace

std::optional<MessageText> MessageText::parse(std::vector<uint8_t> file, std::string& refusal) {
    if (file.size() < kHeaderSize || std::memcmp(file.data(), "MsgStdBn", 8) != 0 ||
        readU16(file, 8) != 0xfeff) {
        refusal = "not a big-endian MSBT";
        return std::nullopt;
    }
    MessageText message;
    message.m_bytes = std::move(file);
    size_t at = kHeaderSize;
    const uint16_t count = readU16(message.m_bytes, 0xe);
    for (uint16_t index = 0; index < count; ++index) {
        if (at + kSectionHeader > message.m_bytes.size() ||
            at + kSectionHeader + readU32(message.m_bytes, at + 4) > message.m_bytes.size()) {
            refusal = "a section past the end of the message file";
            return std::nullopt;
        }
        at = alignSixteen(at + kSectionHeader + readU32(message.m_bytes, at + 4));
    }
    auto labels = message.section("LBL1");
    if (!labels || !message.section("TXT2")) {
        refusal = "no LBL1 or TXT2 section";
        return std::nullopt;
    }
    const size_t body = labels->at + kSectionHeader;
    const uint32_t groups = readU32(message.m_bytes, body);
    for (uint32_t group = 0; group < groups; ++group) {
        const uint32_t entries = readU32(message.m_bytes, body + 4 + (8 * size_t{group}));
        size_t entry = body + readU32(message.m_bytes, body + 8 + (8 * size_t{group}));
        for (uint32_t i = 0; i < entries; ++i) {
            const uint8_t length = message.m_bytes[entry];
            message.m_labels.emplace_back(
                reinterpret_cast<const char*>(&message.m_bytes[entry + 1]), length);
            message.m_labelIndex.push_back(readU32(message.m_bytes, entry + 1 + length));
            entry += 5 + size_t{length};
        }
    }
    return message;
}

std::optional<MessageText::Section> MessageText::section(const char* magic) const {
    size_t at = kHeaderSize;
    const uint16_t count = readU16(m_bytes, 0xe);
    for (uint16_t index = 0; index < count; ++index) {
        const uint32_t size = readU32(m_bytes, at + 4);
        if (std::memcmp(&m_bytes[at], magic, 4) == 0) {
            return Section{.at = at, .size = size};
        }
        at = alignSixteen(at + kSectionHeader + size);
    }
    return std::nullopt;
}

std::optional<uint32_t> MessageText::indexOf(std::string_view label) const {
    for (size_t i = 0; i < m_labels.size(); ++i) {
        if (m_labels[i] == label) {
            return m_labelIndex[i];
        }
    }
    return std::nullopt;
}

std::optional<std::u16string> MessageText::text(std::string_view label) const {
    auto index = indexOf(label);
    auto texts = section("TXT2");
    if (!index || !texts) {
        return std::nullopt;
    }
    const size_t body = texts->at + kSectionHeader;
    if (*index >= readU32(m_bytes, body)) {
        return std::nullopt;
    }
    size_t at = body + readU32(m_bytes, body + 4 + (4 * size_t{*index}));
    std::u16string text;
    while (at + 1 < body + texts->size && readU16(m_bytes, at) != 0) {
        text.push_back(static_cast<char16_t>(readU16(m_bytes, at)));
        at += 2;
    }
    return text;
}

bool MessageText::setText(std::string_view label, std::u16string_view text) {
    auto index = indexOf(label);
    auto found = section("TXT2");
    if (!index || !found) {
        return false;
    }
    const Section texts = *found;
    const size_t body = texts.at + kSectionHeader;
    const uint32_t count = readU32(m_bytes, body);
    if (*index >= count) {
        return false;
    }
    std::vector<std::vector<uint8_t>> strings;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t start = readU32(m_bytes, body + 4 + (4 * size_t{i}));
        const uint32_t end =
            i + 1 < count ? readU32(m_bytes, body + 8 + (4 * size_t{i})) : texts.size;
        strings.emplace_back(m_bytes.begin() + static_cast<ptrdiff_t>(body + start),
                             m_bytes.begin() + static_cast<ptrdiff_t>(body + end));
    }
    std::vector<uint8_t> replaced((text.size() + 1) * 2, 0);
    for (size_t i = 0; i < text.size(); ++i) {
        writeU16(replaced, 2 * i, static_cast<uint16_t>(text[i]));
    }
    strings[*index] = std::move(replaced);
    std::vector<uint8_t> section(kSectionHeader + 4 + (4 * size_t{count}), 0);
    std::memcpy(section.data(), "TXT2", 4);
    writeU32(section, kSectionHeader, count);
    for (uint32_t i = 0; i < count; ++i) {
        writeU32(section, kSectionHeader + 4 + (4 * size_t{i}),
                 static_cast<uint32_t>(section.size() - kSectionHeader));
        section.insert(section.end(), strings[i].begin(), strings[i].end());
    }
    writeU32(section, 4, static_cast<uint32_t>(section.size() - kSectionHeader));
    section.resize(alignSixteen(section.size()), kPadding);
    const auto from = m_bytes.begin() + static_cast<ptrdiff_t>(texts.at);
    const auto to = m_bytes.begin() +
                    static_cast<ptrdiff_t>(alignSixteen(texts.at + kSectionHeader + texts.size));
    m_bytes.erase(from, to);
    m_bytes.insert(m_bytes.begin() + static_cast<ptrdiff_t>(texts.at), section.begin(),
                   section.end());
    writeU32(m_bytes, 0x12, static_cast<uint32_t>(m_bytes.size()));
    return true;
}

} // namespace wiiuport::content
