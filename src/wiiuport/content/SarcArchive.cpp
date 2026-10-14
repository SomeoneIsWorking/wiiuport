#include "wiiuport/content/SarcArchive.h"

#include "wiiuport/content/BigEndian.h"

#include <algorithm>

namespace wiiuport::content {
namespace {

constexpr size_t kHeaderSize = 0x14;
constexpr size_t kFatHeaderSize = 0xc;
constexpr size_t kNodeSize = 0x10;
constexpr size_t kFntHeaderSize = 8;
constexpr uint32_t kNamed = 0x01000000;

bool magicAt(std::span<const uint8_t> bytes, size_t at, std::string_view magic) {
    return at + magic.size() <= bytes.size() &&
           std::equal(magic.begin(), magic.end(), bytes.begin() + static_cast<ptrdiff_t>(at));
}

uint32_t alignmentOf(uint32_t offset) {
    uint32_t alignment = 1;
    while (alignment < SarcArchive::kMaxAlignment && (offset % (alignment * 2)) == 0) {
        alignment *= 2;
    }
    return alignment;
}

size_t alignUp(size_t value, size_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

} // namespace

std::optional<SarcArchive> SarcArchive::parse(std::span<const uint8_t> bytes,
                                              std::string& refusal) {
    if (!magicAt(bytes, 0, "SARC") || bytes.size() < kHeaderSize + kFatHeaderSize) {
        refusal = "not a SARC archive";
        return std::nullopt;
    }
    if (readU16(bytes, 6) != 0xfeff) {
        refusal = "a little-endian SARC archive";
        return std::nullopt;
    }
    const uint32_t dataOffset = readU32(bytes, 0xc);
    const size_t fat = readU16(bytes, 4);
    if (!magicAt(bytes, fat, "SFAT")) {
        refusal = "no SFAT block";
        return std::nullopt;
    }
    const uint16_t count = readU16(bytes, fat + 6);
    SarcArchive archive;
    archive.m_hashKey = readU32(bytes, fat + 8);
    const size_t nodes = fat + kFatHeaderSize;
    const size_t fnt = nodes + (kNodeSize * count);
    if (!magicAt(bytes, fnt, "SFNT")) {
        refusal = "no SFNT block";
        return std::nullopt;
    }
    const size_t names = fnt + kFntHeaderSize;
    for (uint16_t index = 0; index < count; ++index) {
        const size_t node = nodes + (kNodeSize * index);
        File file;
        file.nameHash = readU32(bytes, node);
        const uint32_t attributes = readU32(bytes, node + 4);
        const uint32_t start = readU32(bytes, node + 8);
        const uint32_t end = readU32(bytes, node + 12);
        if (end < start || dataOffset + size_t{end} > bytes.size()) {
            refusal = "a file outside the archive";
            return std::nullopt;
        }
        if ((attributes & 0xff000000U) != 0) {
            size_t at = names + (size_t{attributes & 0xffffffU} * 4);
            while (at < dataOffset && bytes[at] != 0) {
                file.name.push_back(static_cast<char>(bytes[at++]));
            }
        }
        file.alignment = alignmentOf(dataOffset + start);
        file.data.assign(bytes.begin() + dataOffset + start, bytes.begin() + dataOffset + end);
        archive.m_files.push_back(std::move(file));
    }
    return archive;
}

std::vector<uint8_t> SarcArchive::write() const {
    std::vector<uint8_t> names;
    std::vector<uint32_t> nameOffsets;
    for (const File& file : m_files) {
        nameOffsets.push_back(static_cast<uint32_t>(names.size()));
        names.insert(names.end(), file.name.begin(), file.name.end());
        names.resize(alignUp(names.size() + 1, 4), 0);
    }
    const size_t fnt = kHeaderSize + kFatHeaderSize + (kNodeSize * m_files.size());
    size_t dataOffset = fnt + kFntHeaderSize + names.size();
    uint32_t widest = 1;
    for (const File& file : m_files) {
        widest = std::max(widest, file.alignment);
    }
    dataOffset = alignUp(dataOffset, widest);
    std::vector<size_t> starts;
    size_t cursor = dataOffset;
    for (const File& file : m_files) {
        cursor = alignUp(cursor, file.alignment);
        starts.push_back(cursor);
        cursor += file.data.size();
    }
    std::vector<uint8_t> out(cursor, 0);
    std::copy_n("SARC", 4, out.begin());
    writeU16(out, 4, kHeaderSize);
    writeU16(out, 6, 0xfeff);
    writeU32(out, 8, static_cast<uint32_t>(out.size()));
    writeU32(out, 0xc, static_cast<uint32_t>(dataOffset));
    writeU16(out, 0x10, 0x0100);
    std::copy_n("SFAT", 4, out.begin() + kHeaderSize);
    writeU16(out, kHeaderSize + 4, kFatHeaderSize);
    writeU16(out, kHeaderSize + 6, static_cast<uint16_t>(m_files.size()));
    writeU32(out, kHeaderSize + 8, m_hashKey);
    for (size_t index = 0; index < m_files.size(); ++index) {
        const File& file = m_files[index];
        const size_t node = kHeaderSize + kFatHeaderSize + (kNodeSize * index);
        writeU32(out, node, file.nameHash);
        writeU32(out, node + 4, file.name.empty() ? 0 : kNamed | (nameOffsets[index] / 4));
        writeU32(out, node + 8, static_cast<uint32_t>(starts[index] - dataOffset));
        writeU32(out, node + 12,
                 static_cast<uint32_t>(starts[index] - dataOffset + file.data.size()));
        std::copy(file.data.begin(), file.data.end(),
                  out.begin() + static_cast<ptrdiff_t>(starts[index]));
    }
    std::copy_n("SFNT", 4, out.begin() + static_cast<ptrdiff_t>(fnt));
    writeU16(out, fnt + 4, kFntHeaderSize);
    std::copy(names.begin(), names.end(),
              out.begin() + static_cast<ptrdiff_t>(fnt + kFntHeaderSize));
    return out;
}

SarcArchive::File* SarcArchive::find(std::string_view name) {
    for (File& file : m_files) {
        if (file.name == name) {
            return &file;
        }
    }
    return nullptr;
}

} // namespace wiiuport::content
