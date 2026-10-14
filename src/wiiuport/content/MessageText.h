#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wiiuport::content {

// A Wii U message file (MsgStdBn, MSBT): labelled UTF-16 strings.
class MessageText {
  public:
    static std::optional<MessageText> parse(std::vector<uint8_t> file, std::string& refusal);

    const std::vector<uint8_t>& bytes() const {
        return m_bytes;
    }

    const std::vector<std::string>& labels() const {
        return m_labels;
    }

    std::optional<std::u16string> text(std::string_view label) const;
    // Rewrites the TXT2 section; false when no message has that label.
    bool setText(std::string_view label, std::u16string_view text);

  private:
    struct Section {
        size_t at = 0;
        uint32_t size = 0;
    };

    std::optional<Section> section(const char* magic) const;
    std::optional<uint32_t> indexOf(std::string_view label) const;

    std::vector<uint8_t> m_bytes;
    std::vector<std::string> m_labels;
    std::vector<uint32_t> m_labelIndex;
};

} // namespace wiiuport::content
