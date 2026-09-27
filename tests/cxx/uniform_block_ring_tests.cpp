/// Whether tick N-1's uniform block contents are still there when tick N paints.
///
/// The objective's own second question, and it does not need to know where the pose is. A
/// mechanism that reads tick N-1's pose at tick N's draw has to find those bytes still resident;
/// a title that overwrites them in place has no such place to read, and one that alternates
/// between two addresses is double-buffering, which does preserve them.
///
/// The measurement is a re-read, not an inference: at each binding the block's bytes are hashed,
/// and when the next binding of the same object arrives the *earlier* address is read again and
/// compared with the hash taken then.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/UniformBlockRing.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

using wiiuport::title::UniformBlockRing;

std::map<uint32_t, std::vector<uint32_t>>* g_blocks = nullptr;
uint64_t g_frame = 1;

bool readWords(uint32_t address, uint32_t* values, uint32_t count) {
    if (g_blocks == nullptr) {
        return false;
    }
    const auto found = g_blocks->find(address);
    if (found == g_blocks->end()) {
        return false;
    }
    if (found->second.size() < count) {
        return false;
    }
    for (uint32_t word = 0; word < count; word++) {
        values[word] = found->second[word];
    }
    return true;
}

uint64_t now() {
    return g_frame;
}

// Sixteen words, written with one value changed, so a hash difference means a content
// difference and nothing else.
std::vector<uint32_t> blockFilledWith(uint32_t value) {
    return std::vector<uint32_t>(16, value);
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

void wiiuport::tests::runUniformBlockRingTests() {
    // **Overwritten in place: the previous tick's contents are gone.** The same address, new
    // bytes. This is the answer that closes the door on reading N-1 at N, and it has to be
    // distinguishable from a block that could not be read.
    {
        g_blocks = new std::map<uint32_t, std::vector<uint32_t>>();
        g_frame = 1;
        (*g_blocks)[0x2000] = blockFilledWith(0x11111111u);
        UniformBlockRing ring(&readWords, &now);
        ring.bind(1, 0x2000, 64);
        g_frame = 2;
        (*g_blocks)[0x2000] = blockFilledWith(0x22222222u);
        ring.bind(1, 0x2000, 64);
        const std::string body = ring.json();
        check::isTrue(field(body, "pairsCompared") == "1", "one pair compared: " + body);
        check::isTrue(field(body, "previousTickStillPresent") == "0" &&
                          field(body, "previousTickOverwritten") == "1",
                      "and the previous tick's contents were overwritten, because the address "
                      "the earlier tick used now holds the later tick's bytes");
        check::isTrue(field(body, "comparisonsUnreadable") == "0",
                      "with nothing unreadable, so the overwrite is a measurement and not a "
                      "failed read");
    }

    // **Still present: the block the earlier tick used was not written again.** Either the title
    // left it alone or it moved elsewhere -- and either way the bytes are there to be read,
    // which is the answer a blend needs.
    {
        g_blocks = new std::map<uint32_t, std::vector<uint32_t>>();
        g_frame = 1;
        (*g_blocks)[0x2000] = blockFilledWith(0x11111111u);
        (*g_blocks)[0x3000] = blockFilledWith(0x33333333u);
        UniformBlockRing ring(&readWords, &now);
        ring.bind(1, 0x2000, 64);
        g_frame = 2;
        // The second tick writes somewhere else and leaves the first address alone.
        (*g_blocks)[0x3000] = blockFilledWith(0x44444444u);
        ring.bind(1, 0x3000, 64);
        const std::string body = ring.json();
        check::isTrue(field(body, "previousTickStillPresent") == "1",
                      "the previous tick's bytes are still at the address it used: " + body);
        check::isTrue(field(body, "consecutivePairsWithDifferentAddress") == "1",
                      "and the two samples used different addresses, which is double buffering "
                      "measured rather than assumed of GX2");
    }

    // **A block that cannot be read is not an overwrite.** This is the distinction the whole
    // class exists to keep: "the bytes changed" and "I could not look" are different answers and
    // a report that merges them turns a read failure into a finding.
    {
        g_blocks = new std::map<uint32_t, std::vector<uint32_t>>();
        g_frame = 1;
        (*g_blocks)[0x2000] = blockFilledWith(0x11111111u);
        UniformBlockRing ring(&readWords, &now);
        ring.bind(1, 0x2000, 64);
        g_frame = 2;
        // The earlier address is unmapped by the time the second binding arrives.
        g_blocks->erase(0x2000);
        (*g_blocks)[0x3000] = blockFilledWith(0x33333333u);
        ring.bind(1, 0x3000, 64);
        const std::string body = ring.json();
        check::isTrue(field(body, "previousTickOverwritten") == "0" &&
                          field(body, "previousTickStillPresent") == "0",
                      "neither still present nor overwritten, because the comparison could not "
                      "be made: " +
                          body);
        check::isTrue(field(body, "comparisonsUnreadable") == "1",
                      "and the comparison is counted as unreadable, which is a third answer and "
                      "not a second: " +
                          body);
        check::isTrue(body.find("\"previousStillPresent\":\"unreadable\"") != std::string::npos,
                      "and the sample says so in words, so a reader can tell a failed "
                      "comparison from a first sample with nothing to compare");
    }

    // **Two binds inside one tick are one tick's contents seen twice.** Comparing them would
    // answer "does a block change within a tick", which is not the question, and a title that
    // binds an object four times a frame would make every object look overwritten.
    {
        g_blocks = new std::map<uint32_t, std::vector<uint32_t>>();
        g_frame = 7;
        (*g_blocks)[0x2000] = blockFilledWith(0x11111111u);
        UniformBlockRing ring(&readWords, &now);
        ring.bind(1, 0x2000, 64);
        ring.bind(1, 0x2000, 64);
        ring.bind(1, 0x2000, 64);
        const std::string body = ring.json();
        check::isTrue(field(body, "bindings") == "3" && field(body, "pairsCompared") == "0",
                      "three bindings inside one frame take one sample and compare nothing: " +
                          body);
        check::isTrue(field(body, "schedule") == "\"perFrame\"",
                      "and the report says the schedule was per frame, so a negative cannot be "
                      "read as a fact about the title");
    }

    // A block past the bound is counted and not hashed: a hash of a megabyte per binding is a
    // census that costs more than the frame it watches.
    {
        g_blocks = new std::map<uint32_t, std::vector<uint32_t>>();
        g_frame = 1;
        UniformBlockRing ring(&readWords, &now);
        ring.bind(1, 0x2000, UniformBlockRing::kMaxBlockBytes + 4);
        const std::string body = ring.json();
        check::isTrue(field(body, "blocksOversize") == "1" && field(body, "objectsTracked") == "0",
                      "a block past the bound is counted and contributes nothing: " + body);
    }

    // The tracked set is bounded, and the refusals counted.
    {
        g_blocks = new std::map<uint32_t, std::vector<uint32_t>>();
        g_frame = 1;
        UniformBlockRing ring(&readWords, &now);
        for (uint32_t object = 1; object <= UniformBlockRing::kObjects + 3; object++) {
            const uint32_t address = 0x2000u + object * 0x100u;
            (*g_blocks)[address] = blockFilledWith(object);
            ring.bind(object, address, 64);
        }
        const std::string body = ring.json();
        check::isTrue(field(body, "objectsTracked") == std::to_string(UniformBlockRing::kObjects),
                      "the tracked set stops at its stated size");
        check::isTrue(field(body, "objectsRefused") == "3",
                      "and the objects beyond it are refused and counted");
    }

    delete g_blocks;
    g_blocks = nullptr;
}
