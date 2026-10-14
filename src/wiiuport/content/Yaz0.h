#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wiiuport::content {

// Nintendo's Yaz0 container: a 16-byte header (magic, decoded size, data alignment) and groups of
// eight chunks, each a literal byte or a back reference.
class Yaz0 {
  public:
    struct Decoded {
        std::vector<uint8_t> bytes;
        // What the loader aligns the decoded data to; Wii U titles rely on it for textures.
        uint32_t alignment = 0;
    };

    static std::optional<Decoded> decode(std::span<const uint8_t> encoded, std::string& refusal);
    static std::vector<uint8_t> encode(std::span<const uint8_t> bytes, uint32_t alignment);
};

} // namespace wiiuport::content
