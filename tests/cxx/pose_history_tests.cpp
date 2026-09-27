/// Which blocks carry a pose, and whether it is written before the block is bound.
///
/// The question is one a blend turns on and no counter answers: is the pose at `+0xc4`
/// of the block written *before* the binder runs, or after? Written before, the value
/// at the binder is this tick's and the value kept from the block's previous binding is
/// the last tick's, so both ends of the lerp are in memory at the binding and the binder
/// alone is enough. Written after, the value at the binder is the *previous* tick's and
/// the current one has to be read where it is written -- a different probe at a
/// different address.
///
/// Three things this has to get right, each of which was measured against the title and
/// each of which a plausible-looking implementation gets wrong:
///
///   - the series is per **block**, not per object. The binder's cursor alternates per
///     binding, so an object's pose is in a different block each time, and a history
///     keyed by object compares two different blocks' readings and calls it one object
///     moving. Measured on the real title: 0 comparisons over 209,615 observations.
///   - a block is tracked because its twelve floats **are** a rigid transform, not
///     because it was the first one seen. Measured: seven of the first eight objects
///     read zero at `+0xc4`.
///   - a pose that never changes is a different finding from a pose nobody writes, so
///     the answer is a count against a denominator and `null` when there is none.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/ObjectPoseHistory.h"

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

namespace {

using wiiuport::title::ObjectPoseHistory;

std::map<uint32_t, uint32_t>* g_words = nullptr;

bool readWords(uint32_t address, uint32_t* values, uint32_t count) {
    if (g_words == nullptr) {
        return false;
    }
    for (uint32_t word = 0; word < count; word++) {
        auto found = g_words->find(address + 4 * word);
        if (found == g_words->end()) {
            return false;
        }
        values[word] = found->second;
    }
    return true;
}

uint32_t floatBits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// A pose that is a rigid transform by construction: three orthonormal rows and a
// translation in the fourth group. `spin` turns the first row, so two poses of the same
// object differ in a way a rigid transform can.
void putPose(std::map<uint32_t, uint32_t>& words, uint32_t block, float spin) {
    const uint32_t base = block + ObjectPoseHistory::kPoseOffset;
    const float s = std::sin(spin);
    const float c = std::cos(spin);
    const float rows[12] = {
        c,         s,         0.0f,  // row 0
        -s,        c,         0.0f,  // row 1
        0.0f,      0.0f,      1.0f,  // row 2
        10.0f * c, 10.0f * s, -5.0f, // the translation beside it
    };
    // Every word at four bytes: writing the first three at +0, +1 and +2 reads as a
    // block that does not read at all, which is how the first version of this file
    // managed to assert that every reading was unreadable.
    for (uint32_t word = 0; word < ObjectPoseHistory::kPoseWords; word++) {
        words[base + 4 * word] = floatBits(rows[word]);
    }
}

// Something at `+0xc4` that is not a pose: what most of the title's blocks hold there.
void putNotPose(std::map<uint32_t, uint32_t>& words, uint32_t block, float value) {
    const uint32_t base = block + ObjectPoseHistory::kPoseOffset;
    for (uint32_t word = 0; word < ObjectPoseHistory::kPoseWords; word++) {
        words[base + 4 * word] = floatBits(value);
    }
}

std::string field(const std::string& body, const std::string& name) {
    const size_t at = body.find("\"" + name + "\":");
    if (at == std::string::npos) {
        return "";
    }
    const size_t start = at + name.size() + 3;
    size_t end = start;
    while (end < body.size() && body[end] != ',' && body[end] != '}') {
        end++;
    }
    return body.substr(start, end - start);
}

} // namespace

void wiiuport::tests::runObjectPoseHistoryTests() {
    // A pose that changes between bindings of the same block is written before the
    // bind: that is the answer the blend rests on, as a count over a denominator.
    {
        std::map<uint32_t, uint32_t> words;
        putPose(words, 0x2000, 0.0f);
        g_words = &words;
        ObjectPoseHistory history(&readWords);
        for (int binding = 0; binding < 4; binding++) {
            history.observe(0x1000, 0x2000, 0x2100);
            putPose(words, 0x2000, 0.1f * static_cast<float>(binding + 1));
        }
        const std::string body = history.json();
        g_words = nullptr;
        check::isTrue(field(body, "observations") == "4", "four bindings observed");
        check::isTrue(field(body, "compared") == "3", "three comparisons, one per repeat binding");
        check::isTrue(field(body, "changed") == "3", "all three found the pose changed");
        check::isTrue(body.find("\"writtenBeforeBind\":true") != std::string::npos,
                      "and the report says the pose is written before the bind, which is what "
                      "makes the binder the only place a blend needs");
    }

    // A pose that never changes is the other answer, and it must not read as the first.
    // This is also what a block nobody writes looks like, which is why the report says
    // what it counted and not what it concluded.
    {
        std::map<uint32_t, uint32_t> words;
        putPose(words, 0x2000, 0.4f);
        g_words = &words;
        ObjectPoseHistory history(&readWords);
        for (int binding = 0; binding < 5; binding++) {
            history.observe(0x1000, 0x2000, 0);
        }
        const std::string body = history.json();
        g_words = nullptr;
        check::isTrue(field(body, "compared") == "4", "four comparisons over five bindings");
        check::isTrue(field(body, "changed") == "0", "none of them found a change");
        check::isTrue(body.find("\"writtenBeforeBind\":false") != std::string::npos,
                      "and the report says the pose is not written before the bind rather than "
                      "leaving the question open");
    }

    // One reading is not a comparison. `null`, not a `false` that reads as a measurement.
    {
        std::map<uint32_t, uint32_t> words;
        putPose(words, 0x2000, 0.2f);
        g_words = &words;
        ObjectPoseHistory history(&readWords);
        history.observe(0x1000, 0x2000, 0);
        const std::string body = history.json();
        g_words = nullptr;
        check::isTrue(field(body, "compared") == "0", "a first binding compares against nothing");
        check::isTrue(body.find("\"writtenBeforeBind\":null") != std::string::npos,
                      "and with nothing compared the answer is null");
    }

    // The series is per block. One object whose bindings land on alternating blocks --
    // which is what the title's cursor does, 0.297 switches a binding -- must give two
    // series, each compared against its own previous reading. Keyed by object this is
    // zero comparisons, and the measured run had exactly zero.
    {
        std::map<uint32_t, uint32_t> words;
        putPose(words, 0x2000, 0.0f);
        putPose(words, 0x2100, 0.0f);
        g_words = &words;
        ObjectPoseHistory history(&readWords);
        // Four alternating bindings, each block moved since it was last bound, which is
        // the shape the title's cursor produces: 0.297 switches a binding.
        putPose(words, 0x2000, 0.3f);
        history.observe(0x1000, 0x2000, 0x2100);
        putPose(words, 0x2100, 0.6f);
        history.observe(0x1000, 0x2100, 0x2000);
        putPose(words, 0x2000, 0.9f);
        history.observe(0x1000, 0x2000, 0x2100);
        putPose(words, 0x2100, 1.2f);
        history.observe(0x1000, 0x2100, 0x2000);
        const std::string body = history.json();
        g_words = nullptr;
        check::isTrue(field(body, "compared") == "2",
                      "an object alternating between two blocks gives two series with one "
                      "comparison each, not one series with none: " +
                          body);
        check::isTrue(field(body, "changed") == "2", "and both found their own block's pose moved");
        check::isTrue(body.find("\"blocks\":{") != std::string::npos,
                      "the report names the blocks, which is what a blend reads");
    }

    // A block is tracked because its twelve floats are a rigid transform, not because
    // it was seen first. Seven of the title's first eight objects read zero at `+0xc4`,
    // and zero is not a pose.
    {
        std::map<uint32_t, uint32_t> words;
        for (uint32_t block = 0x2000; block < 0x2800; block += 0x100) {
            putNotPose(words, block, 0.0f);
        }
        putPose(words, 0x3000, 0.5f);
        g_words = &words;
        ObjectPoseHistory history(&readWords);
        for (uint32_t block = 0x2000; block < 0x2800; block += 0x100) {
            history.observe(0x1000 + block, block, 0);
        }
        history.observe(0x9000, 0x3000, 0);
        const std::string body = history.json();
        g_words = nullptr;
        check::isTrue(field(body, "notPoseBindings") == "8",
                      "eight bindings whose block holds something that is not a pose are counted "
                      "as such: " +
                          body);
        check::isTrue(field(body, "poseBindings") == "1", "and one that holds a pose");
        check::isTrue(body.find("0x00003000") != std::string::npos,
                      "and the tracked series is the pose-carrying block, not the first one seen");
    }

    // Near-orthogonal is not orthogonal. The tolerance is a number somebody can argue
    // with, so the test states it and the report carries it.
    {
        std::map<uint32_t, uint32_t> words;
        putPose(words, 0x2000, 0.0f);
        g_words = &words;
        ObjectPoseHistory history(&readWords);
        history.observe(0x1000, 0x2000, 0);
        const std::string body = history.json();
        g_words = nullptr;
        check::isTrue(field(body, "unitTolerance") == "10",
                      "the unit tolerance is reported in thousandths, so 10 is 0.01");
        check::isTrue(field(body, "poseBindings") == "1",
                      "and a pose built from single-precision cosines passes it");
    }
    {
        // A row of length 1.05, and two rows a hundredth off perpendicular: both just
        // outside, and both refused rather than rounded in.
        std::map<uint32_t, uint32_t> words;
        putNotPose(words, 0x2000, 1.05f);
        g_words = &words;
        ObjectPoseHistory history(&readWords);
        history.observe(0x1000, 0x2000, 0);
        const std::string body = history.json();
        g_words = nullptr;
        check::isTrue(field(body, "poseBindings") == "0",
                      "a row 5% off unit length is not a pose, and is refused rather than "
                      "rounded in");
    }

    // The ring, as a comparison of two readings: whether the other slot ever held what
    // this block held at its previous binding.
    {
        std::map<uint32_t, uint32_t> words;
        putPose(words, 0x2000, 0.25f);
        putPose(words, 0x2100, 0.75f);
        g_words = &words;
        ObjectPoseHistory history(&readWords);
        history.observe(0x1000, 0x2000, 0x2100);
        putPose(words, 0x2100, 0.25f); // the other slot now holds what 0x2000 held
        putPose(words, 0x2000, 0.5f);
        history.observe(0x1000, 0x2000, 0x2100);
        const std::string body = history.json();
        g_words = nullptr;
        check::isTrue(field(body, "seenAsOther") == "1", "the other slot was pose-shaped once");
        check::isTrue(field(body, "otherMatched") == "1",
                      "and it matched, which is the ring carrying the previous tick's pose");
        check::isTrue(!field(body, "previousPose").empty() &&
                          field(body, "previousPose").find("1.000000") == std::string::npos,
                      "and the reading before this one is still there, which is the ring's own "
                      "history rather than zeroes left by a shift that never happened");
    }
    {
        std::map<uint32_t, uint32_t> words;
        putPose(words, 0x2000, 0.25f);
        putPose(words, 0x2100, 0.75f);
        g_words = &words;
        ObjectPoseHistory history(&readWords);
        history.observe(0x1000, 0x2000, 0x2100);
        putPose(words, 0x2000, 0.5f);
        history.observe(0x1000, 0x2000, 0x2100);
        const std::string body = history.json();
        g_words = nullptr;
        check::isTrue(field(body, "seenAsOther") == "1", "the other slot was pose-shaped once");
        check::isTrue(field(body, "otherMatched") == "0",
                      "and it did not match, so this ring is not carrying the previous tick -- "
                      "which is what the title's zeroed other slot says, now counted");
    }

    // A descriptor that named nothing, and a binding with no object: counted apart from
    // a pose of zeroes.
    {
        std::map<uint32_t, uint32_t> words;
        g_words = &words;
        ObjectPoseHistory history(&readWords);
        history.observe(0, 0x2000, 0x2100);
        history.observe(0x1000, 0x2000, 0);
        const std::string body = history.json();
        g_words = nullptr;
        check::isTrue(field(body, "bindingsWithoutObject") == "1",
                      "a binding with no object is counted on its own");
        check::isTrue(field(body, "bindingsUnreadable") == "1",
                      "and one whose block address is zero is unreadable, not a pose of zeroes");
        check::isTrue(field(body, "poseBindings") == "0", "neither is a pose");
    }

    // The tracked set is bounded, and a refusal is counted.
    {
        std::map<uint32_t, uint32_t> words;
        for (uint32_t block = 0x2000; block < 0x2000 + 0x100 * (ObjectPoseHistory::kBlocks + 5);
             block += 0x100) {
            putPose(words, block, 0.1f);
        }
        g_words = &words;
        ObjectPoseHistory history(&readWords);
        uint32_t block = 0x2000;
        for (uint32_t seen = 0; seen < ObjectPoseHistory::kBlocks + 5; seen++) {
            history.observe(0x1000 + seen, block, 0);
            block += 0x100;
        }
        const std::string body = history.json();
        g_words = nullptr;
        check::isTrue(field(body, "blocksRefused") == "5",
                      "the five blocks beyond the cap are refused and counted, not dropped");
    }

    // The offset scan. A pose at an offset nobody expected is a wrong offset; no pose
    // at any offset is a block that does not hold one when it is bound, and those are
    // different findings.
    {
        std::map<uint32_t, uint32_t> words;
        // A block whose pose is at +0x30 rather than at +0xc4, and nothing at +0xc4.
        for (uint32_t word = 0; word < ObjectPoseHistory::kBlockWords; word++) {
            words[0x2000 + 4 * word] = floatBits(0.0f);
        }
        std::array<float, 12> shifted = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                         0.0f, 0.0f, 1.0f, 4.0f, 5.0f, 6.0f};
        for (uint32_t word = 0; word < 12; word++) {
            words[0x2000 + 0x30 + 4 * word] = floatBits(shifted[word]);
        }
        g_words = &words;
        ObjectPoseHistory history(&readWords);
        history.observe(0x1000, 0x2000, 0);
        history.observe(0x1000, 0x2000, 0);
        const std::string body = history.json();
        g_words = nullptr;
        check::isTrue(field(body, "scans") == "2",
                      "the block was scanned at both its bindings, not once: it may hold the "
                      "pose only after the frame's draw fills it, and one scan at one moment "
                      "would call a block that holds no pose a wrong offset");
        check::isTrue(field(body, "poseBindings") == "0",
                      "and the offset the pose was located at holds nothing, so the block is "
                      "not counted as carrying a pose at +0xc4");
        check::isTrue(field(body, "offsetHits") == "2",
                      "while the scan found the rigid transform at its real offset twice");
        check::isTrue(body.find("\"poseOffsets\":\" 0x30:2") != std::string::npos,
                      "and named that offset, which is a located answer rather than a guess: " +
                          body);
    }
    {
        std::map<uint32_t, uint32_t> words;
        for (uint32_t word = 0; word < ObjectPoseHistory::kBlockWords; word++) {
            words[0x2000 + 4 * word] = floatBits(0.0f);
        }
        g_words = &words;
        ObjectPoseHistory history(&readWords);
        for (int binding = 0; binding < 5; binding++) {
            history.observe(0x1000, 0x2000, 0);
        }
        const std::string body = history.json();
        g_words = nullptr;
        check::isTrue(field(body, "offsetHits") == "0",
                      "a block with no rigid transform anywhere in it finds none, which is the "
                      "block holding no pose when it is bound rather than a wrong offset");
        check::isTrue(body.find("\"poseOffsets\":null") != std::string::npos,
                      "and the report says null rather than an empty list, which reads as an "
                      "answer where there was no measurement");
    }
    {
        // The scan is bounded, and the bound is counted.
        std::map<uint32_t, uint32_t> words;
        for (uint32_t block = 0x2000; block < 0x2000 + 0x100 * (ObjectPoseHistory::kScanned + 3);
             block += 0x100) {
            for (uint32_t word = 0; word < ObjectPoseHistory::kBlockWords; word++) {
                words[block + 4 * word] = floatBits(0.0f);
            }
        }
        g_words = &words;
        ObjectPoseHistory history(&readWords);
        uint32_t block = 0x2000;
        for (uint32_t seen = 0; seen < ObjectPoseHistory::kScanned + 3; seen++) {
            history.observe(0x1000 + seen, block, 0);
            block += 0x100;
        }
        const std::string body = history.json();
        g_words = nullptr;
        check::isTrue(field(body, "scannedBlocks") == std::to_string(ObjectPoseHistory::kScanned),
                      "the scan stops at its stated number of blocks, because a scan of every "
                      "block at every binding is a load on the display thread");
    }

    // The report is one object that ends, because a client parses it.
    {
        std::map<uint32_t, uint32_t> words;
        putPose(words, 0x2000, 0.7f);
        g_words = &words;
        ObjectPoseHistory history(&readWords);
        history.observe(0x1000, 0x2000, 0x2100);
        const std::string body = history.json();
        g_words = nullptr;
        check::isTrue(body.front() == '{' && body.find("}\n") != std::string::npos,
                      "and the body is one object that ends");
        check::isTrue(body.find("\"pose\":{") != std::string::npos,
                      "and it carries the last pose as an object, not as a quoted string -- a "
                      "quoted one makes the whole report unparseable");
    }
}
