#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wiiuport::content {

// A big-endian SARC archive: named files in hash order, each placed at the alignment it held.
class SarcArchive {
  public:
    struct File {
        std::string name;
        uint32_t nameHash = 0;
        // Largest power of two (up to kMaxAlignment) the file's start held in the source archive.
        uint32_t alignment = 0;
        std::vector<uint8_t> data;
    };

    static constexpr uint32_t kMaxAlignment = 0x2000;

    static std::optional<SarcArchive> parse(std::span<const uint8_t> bytes, std::string& refusal);

    std::vector<uint8_t> write() const;

    File* find(std::string_view name);

    const std::vector<File>& files() const {
        return m_files;
    }

  private:
    uint32_t m_hashKey = 0;
    std::vector<File> m_files;
};

} // namespace wiiuport::content
