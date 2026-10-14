#include "wiiuport/content/Yaz0.h"

#include "wiiuport/content/BigEndian.h"

#include <algorithm>
#include <array>

namespace wiiuport::content {
namespace {

constexpr size_t kHeaderSize = 16;
constexpr size_t kWindow = 0x1000;
constexpr size_t kLongestCopy = 0x111;
constexpr size_t kShortestCopy = 3;
constexpr size_t kHashSize = 1U << 15;
constexpr int kSearchDepth = 128;

uint32_t hashOf(std::span<const uint8_t> bytes, size_t at) {
    return ((uint32_t{bytes[at]} << 10) ^ (uint32_t{bytes[at + 1]} << 5) ^ bytes[at + 2]) &
           (kHashSize - 1);
}

} // namespace

std::optional<Yaz0::Decoded> Yaz0::decode(std::span<const uint8_t> encoded, std::string& refusal) {
    if (encoded.size() < kHeaderSize || encoded[0] != 'Y' || encoded[1] != 'a' ||
        encoded[2] != 'z' || encoded[3] != '0') {
        refusal = "not Yaz0";
        return std::nullopt;
    }
    Decoded decoded;
    const uint32_t size = readU32(encoded, 4);
    decoded.alignment = readU32(encoded, 8);
    decoded.bytes.reserve(size);
    size_t in = kHeaderSize;
    while (decoded.bytes.size() < size) {
        if (in >= encoded.size()) {
            refusal = "Yaz0 ends before its decoded size";
            return std::nullopt;
        }
        const uint8_t group = encoded[in++];
        for (int bit = 7; bit >= 0 && decoded.bytes.size() < size; --bit) {
            if ((group >> bit) & 1U) {
                if (in >= encoded.size()) {
                    refusal = "Yaz0 literal past the end";
                    return std::nullopt;
                }
                decoded.bytes.push_back(encoded[in++]);
                continue;
            }
            if (in + 2 > encoded.size()) {
                refusal = "Yaz0 copy past the end";
                return std::nullopt;
            }
            const uint32_t pair = (uint32_t{encoded[in]} << 8) | encoded[in + 1];
            in += 2;
            const size_t distance = (pair & 0xfffU) + 1;
            size_t count = pair >> 12;
            if (count == 0) {
                if (in >= encoded.size()) {
                    refusal = "Yaz0 copy length past the end";
                    return std::nullopt;
                }
                count = size_t{encoded[in++]} + 0x12;
            } else {
                count += 2;
            }
            if (distance > decoded.bytes.size()) {
                refusal = "Yaz0 copy from before the start";
                return std::nullopt;
            }
            for (size_t i = 0; i < count && decoded.bytes.size() < size; ++i) {
                decoded.bytes.push_back(decoded.bytes[decoded.bytes.size() - distance]);
            }
        }
    }
    return decoded;
}

std::vector<uint8_t> Yaz0::encode(std::span<const uint8_t> bytes, uint32_t alignment) {
    std::vector<uint8_t> out(kHeaderSize, 0);
    out[0] = 'Y';
    out[1] = 'a';
    out[2] = 'z';
    out[3] = '0';
    writeU32(out, 4, static_cast<uint32_t>(bytes.size()));
    writeU32(out, 8, alignment);
    std::vector<int64_t> head(kHashSize, -1);
    std::vector<int64_t> previous(bytes.size(), -1);
    auto insert = [&](size_t at) {
        if (at + kShortestCopy <= bytes.size()) {
            const uint32_t hash = hashOf(bytes, at);
            previous[at] = head[hash];
            head[hash] = static_cast<int64_t>(at);
        }
    };
    size_t at = 0;
    while (at < bytes.size()) {
        const size_t groupAt = out.size();
        out.push_back(0);
        for (int bit = 7; bit >= 0 && at < bytes.size(); --bit) {
            size_t bestLength = 0;
            size_t bestDistance = 0;
            if (at + kShortestCopy <= bytes.size()) {
                const size_t limit = std::min(kLongestCopy, bytes.size() - at);
                int64_t candidate = head[hashOf(bytes, at)];
                for (int depth = 0; candidate >= 0 && depth < kSearchDepth; ++depth) {
                    const auto from = static_cast<size_t>(candidate);
                    if (at - from > kWindow) {
                        break;
                    }
                    size_t length = 0;
                    while (length < limit && bytes[from + length] == bytes[at + length]) {
                        ++length;
                    }
                    if (length > bestLength) {
                        bestLength = length;
                        bestDistance = at - from;
                        if (length == limit) {
                            break;
                        }
                    }
                    candidate = previous[from];
                }
            }
            if (bestLength < kShortestCopy) {
                out[groupAt] |= static_cast<uint8_t>(1U << bit);
                out.push_back(bytes[at]);
                insert(at);
                ++at;
                continue;
            }
            const auto distance = static_cast<uint32_t>(bestDistance - 1);
            if (bestLength >= 0x12) {
                out.push_back(static_cast<uint8_t>(distance >> 8));
                out.push_back(static_cast<uint8_t>(distance));
                out.push_back(static_cast<uint8_t>(bestLength - 0x12));
            } else {
                out.push_back(static_cast<uint8_t>(((bestLength - 2) << 4) | (distance >> 8)));
                out.push_back(static_cast<uint8_t>(distance));
            }
            for (size_t i = 0; i < bestLength; ++i) {
                insert(at + i);
            }
            at += bestLength;
        }
    }
    return out;
}

} // namespace wiiuport::content
