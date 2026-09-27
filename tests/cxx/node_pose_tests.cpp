/// Which field of an object holds its pose.
///
/// Three measurements in a row put the pose outside a uniform and outside the node, so this
/// looks in the memory of the two things a binding names: the node and the sub-object at
/// `node + 0xa1c`. What makes the answer a locator rather than a coincidence is the
/// cross-object test: a pose field is at one offset in *every* object, while a colour triple
/// that happens to be near unit length is at a different offset in each. Every test here is
/// about that, about the two kinds being scored apart, or about the probe site being
/// somewhere the title actually enters -- which measurement says it does not.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/NodePoseLocator.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

using wiiuport::title::NodePoseLocator;

std::map<uint32_t, std::vector<float>>* g_nodes = nullptr;
GuestCallProbes::Probe* g_probe = nullptr;
NodePoseLocator* g_locator = nullptr;
uint32_t g_entry = 0;
uint32_t g_first = 0;

bool readWords(uint32_t address, uint32_t* values, uint32_t count) {
    if (g_nodes == nullptr) {
        return false;
    }
    auto found = g_nodes->find(address);
    if (found == g_nodes->end()) {
        return false;
    }
    for (uint32_t word = 0; word < count; word++) {
        if (word >= found->second.size()) {
            return false;
        }
        const float& value = found->second[word];
        std::memcpy(&values[word], &value, sizeof(value));
    }
    return true;
}

void keepRegistration(uint32_t entry, uint32_t firstInstruction, GuestCallProbes::Probe& probe,
                      bool /*holdsEntry*/, uint32_t /*resume*/) {
    g_entry = entry;
    g_first = firstInstruction;
    g_probe = &probe;
}

NodePoseLocator makeLocator() {
    g_nodes = nullptr;
    g_probe = nullptr;
    g_locator = nullptr;
    return NodePoseLocator(&keepRegistration, &readWords);
}

// An object whose memory holds a transform at a float offset, and zeros elsewhere. The
// scale is a multiplier on the rotation's rows, so the value stays a transform at any scale
// -- a scaled one is still a transform, which is the whole of the loose class.
std::vector<float> withPoseAt(size_t floatOffset, float spin, float scale = 1.0f) {
    std::vector<float> words(NodePoseLocator::kScanWords, 0.0f);
    const float s = std::sin(spin);
    const float c = std::cos(spin);
    const float pose[12] = {scale * c, scale * s, 0.0f,  -scale * s, scale * c, 0.0f,
                            0.0f,      0.0f,      scale, 10.0f * c,  10.0f * s, -5.0f};
    for (size_t word = 0; word < 12; word++) {
        words[floatOffset + word] = pose[word];
    }
    return words;
}

std::vector<float> withNothing() {
    return std::vector<float>(NodePoseLocator::kScanWords, 0.0f);
}

// A scalar field, read up to the next comma or brace. An object-valued field has to be
// found whole, with `find`, because a value that begins with `{` ends at that brace.
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

// One nested object out of the report, so a field can be read inside it and not confused
// with the same name in the other kind. Both kinds carry `objectsTracked` and `bestOffset`,
// and a reader that cannot tell them apart reads whichever it finds first.
std::string section(const std::string& body, const std::string& name) {
    const std::string key = "\"" + name + "\":{";
    const size_t at = body.find(key);
    if (at == std::string::npos) {
        return "";
    }
    size_t depth = 0;
    size_t index = at + name.size() + 2;
    for (; index < body.size(); index++) {
        if (body[index] == '{') {
            depth++;
        } else if (body[index] == '}') {
            depth--;
            if (depth == 0) {
                return body.substr(at + name.size() + 3, index - (at + name.size() + 3));
            }
        }
    }
    return "";
}

} // namespace

void wiiuport::tests::runNodePoseLocatorTests() {
    // The probe is at the function's entry, not at the address the vtable holds. The
    // vtable's target is 0x168 bytes in and its first instruction is a conditional branch,
    // and a probe resumes at "the instruction after the entry" -- so a taken branch there
    // would make the stub re-run the code the branch was there to skip.
    {
        NodePoseLocator locator = makeLocator();
        locator.install();
        check::isTrue(g_entry == NodePoseLocator::kDraw, "the probe is on the node draw's entry");
        check::isTrue(g_first == NodePoseLocator::kFirstInstruction,
                      "with that entry's own first word, lifted from the image and not "
                      "assembled: 0x9421feb8 is stwu r1,-0x148(r1)");
        check::isTrue(NodePoseLocator::kVtableTarget > NodePoseLocator::kDraw,
                      "and the vtable's target is inside the function rather than at its entry, "
                      "which is why it is not the probe site");
        check::isTrue(NodePoseLocator::kNodeRegister == 3,
                      "the node is in r3 at the entry, where the prologue has not yet copied it "
                      "to r28");
        check::isTrue(NodePoseLocator::kSubObjectOffset == 0xa1c,
                      "and the node's sub-object sits 0xa1c from the node, which is the draw's "
                      "own `addi r3,r28,0xa1c` -- so one subtraction off a binding's argument "
                      "is the node");
    }

    // A pose at the same offset in several objects is named, and the count of objects is
    // what names it.
    {
        std::map<uint32_t, std::vector<float>> nodes;
        for (uint32_t node = 1; node <= 5; node++) {
            nodes[node] = withPoseAt(20, 0.1f * static_cast<float>(node));
        }
        NodePoseLocator locator = makeLocator();
        g_nodes = &nodes;
        locator.install();
        g_locator = &locator;
        for (uint32_t node = 1; node <= 5; node++) {
            for (int scan = 0; scan < 3; scan++) {
                nodes[node] = withPoseAt(20, 0.1f * static_cast<float>(node) + 0.05f * scan);
                locator.observe(node);
            }
        }
        const std::string body = locator.json();
        const std::string mine = section(body, "node");
        g_nodes = nullptr;
        check::isTrue(field(mine, "objectsTracked") == "5", "five distinct objects tracked");
        check::isTrue(locator.bestOffset() == 80,
                      "and the offset five objects agree on is named -- 20 floats is 80 bytes");
        check::isTrue(field(mine, "objectsNeeded") == "3",
                      "with the number of objects needed stated, so the bar is not a mystery");
        check::isTrue(field(mine, "moved") != "0",
                      "and the value moved between scans of an object, which is what separates a "
                      "pose from a constant: " +
                          mine);
    }

    // The same shape at a *different* offset in each object is not a field. Three objects,
    // three offsets, nothing named -- which is the case a "found a transform" report would
    // call a pass and a cross-object locator calls nothing.
    {
        std::map<uint32_t, std::vector<float>> nodes;
        for (uint32_t node = 1; node <= 3; node++) {
            nodes[node] = withPoseAt(static_cast<size_t>(node) * 7, 0.2f);
        }
        NodePoseLocator locator = makeLocator();
        g_nodes = &nodes;
        locator.install();
        g_locator = &locator;
        for (uint32_t node = 1; node <= 3; node++) {
            for (int scan = 0; scan < 3; scan++) {
                locator.observe(node);
            }
        }
        const std::string body = locator.json();
        g_nodes = nullptr;
        check::isTrue(field(body, "calls") == "0",
                      "nothing came through the probed entry, because the title dispatches the "
                      "draw through the vtable's target instead");
        check::isTrue(field(section(body, "node"), "objectsNeeded") == "2",
                      "two objects would have been enough");
        check::isTrue(locator.bestOffset() == 0,
                      "and nothing is named, because no offset is held by two objects");
        check::isTrue(field(section(body, "node"), "bestOffset") == "null",
                      "and the report says null rather than the best candidate it saw");
    }

    // **The two kinds are scored apart.** Four sub-objects each hold a transform at 80
    // bytes; four nodes hold nothing. Named in the sub-object table, nothing in the node
    // table. A single table over both would have seen 4 of 8 and needed 5, and named
    // nothing -- so this is the difference between a measurement and a diluted one, and it
    // is why the two tables exist.
    {
        std::map<uint32_t, std::vector<float>> memory;
        for (uint32_t sub = 1; sub <= 4; sub++) {
            // The node and its sub-object are the two addresses a binding names, one
            // kSubObjectOffset apart, which is how the product reaches both.
            const uint32_t node = 0x1000u * sub;
            memory[node] = withNothing();
            memory[node + NodePoseLocator::kSubObjectOffset] = withPoseAt(20, 0.3f * sub);
        }
        NodePoseLocator locator = makeLocator();
        g_nodes = &memory;
        locator.install();
        g_locator = &locator;
        for (uint32_t sub = 1; sub <= 4; sub++) {
            const uint32_t node = 0x1000u * sub;
            for (int scan = 0; scan < 3; scan++) {
                // The pose changes between draws of the same object, because that is what a
                // pose does and because the bar requires it: a rigid triple at the same
                // offset in every object that never moves is a basis, and the locator says so.
                memory[node + NodePoseLocator::kSubObjectOffset] =
                    withPoseAt(20, 0.3f * sub + 0.05f * scan);
                locator.observe(node + NodePoseLocator::kSubObjectOffset,
                                NodePoseLocator::Kind::SubObject);
                locator.observe(node, NodePoseLocator::Kind::Node);
            }
        }
        const std::string body = locator.json();
        g_nodes = nullptr;
        const std::string asNode = section(body, "node");
        const std::string asSub = section(body, "subObject");
        check::isTrue(field(asSub, "objectsTracked") == "4" &&
                          field(asNode, "objectsTracked") == "4",
                      "both kinds tracked four objects each");
        check::isTrue(field(asSub, "bestOffset") == "80" &&
                          locator.bestOffset(NodePoseLocator::Kind::SubObject) == 80,
                      "and the sub-object's field is named: four sub-objects hold it");
        check::isTrue(field(asNode, "bestOffset") == "null" && locator.bestOffset() == 0,
                      "while the node's own table names nothing, because no node held it: " +
                          asNode);
        check::isTrue(field(asNode, "objectsNeeded") == "3" && field(asSub, "objectsNeeded") == "3",
                      "and both tables state their own denominator, so neither bar is a mystery");
    }

    // One object cannot agree with another, so it names nothing however many times it is
    // scanned. Reported, not silent.
    {
        std::map<uint32_t, std::vector<float>> nodes;
        nodes[1] = withPoseAt(20, 0.3f);
        NodePoseLocator locator = makeLocator();
        g_nodes = &nodes;
        locator.install();
        g_locator = &locator;
        for (int scan = 0; scan < static_cast<int>(NodePoseLocator::kScansPerObject); scan++) {
            locator.observe(1);
        }
        const std::string body = locator.json();
        g_nodes = nullptr;
        check::isTrue(field(section(body, "node"), "objectsTracked") == "1", "one object tracked");
        check::isTrue(field(section(body, "node"), "objectsNeeded") == "0",
                      "and the objects needed is zero, because a single object cannot settle a "
                      "cross-object question");
        check::isTrue(locator.bestOffset() == 0, "so nothing is named");
    }

    // The probed entry does reach the locator when something calls it, so `calls` is a real
    // count and not a field nothing writes.
    {
        std::map<uint32_t, std::vector<float>> nodes;
        nodes[1] = withPoseAt(20, 0.3f);
        NodePoseLocator locator = makeLocator();
        g_nodes = &nodes;
        locator.install();
        g_locator = &locator;
        std::array<uint32_t, 32> gpr{};
        gpr[NodePoseLocator::kNodeRegister] = 1;
        g_probe->OnCall(std::span<const uint32_t, 32>(gpr.data(), gpr.size()), 0);
        const std::string body = locator.json();
        g_nodes = nullptr;
        check::isTrue(field(body, "calls") == "1", "a call through the probed entry is counted");
        check::isTrue(field(section(body, "node"), "objectsTracked") == "1",
                      "and the node in r3 is tracked as a node, not as a sub-object");
    }

    // An object whose memory does not read is counted, and contributes nothing.
    {
        std::map<uint32_t, std::vector<float>> nodes;
        nodes[1] = withPoseAt(20, 0.3f);
        NodePoseLocator locator = makeLocator();
        g_nodes = &nodes;
        locator.install();
        g_locator = &locator;
        locator.observe(1);
        locator.observe(0x7fffffff); // not in the map
        const std::string body = locator.json();
        g_nodes = nullptr;
        check::isTrue(field(body, "readsUnreadable") == "1",
                      "an object whose memory does not read is counted as unreadable: " + body);
    }

    // The scan is bounded per kind, and the refusals are counted per kind: a tracked list
    // that grew with the scene would be a list of every object the title has ever drawn,
    // and a refusal counted only in total would hide which kind filled up.
    {
        std::map<uint32_t, std::vector<float>> memory;
        for (uint32_t sub = 1; sub <= NodePoseLocator::kObjects + 3; sub++) {
            const uint32_t node = 0x1000u * sub;
            memory[node] = withPoseAt(20, 0.1f);
            memory[node + NodePoseLocator::kSubObjectOffset] = withPoseAt(20, 0.1f);
        }
        NodePoseLocator locator = makeLocator();
        g_nodes = &memory;
        locator.install();
        g_locator = &locator;
        for (uint32_t sub = 1; sub <= NodePoseLocator::kObjects + 3; sub++) {
            const uint32_t node = 0x1000u * sub;
            locator.observe(node + NodePoseLocator::kSubObjectOffset,
                            NodePoseLocator::Kind::SubObject);
            locator.observe(node, NodePoseLocator::Kind::Node);
        }
        const std::string body = locator.json();
        g_nodes = nullptr;
        const std::string asNode = section(body, "node");
        const std::string asSub = section(body, "subObject");
        check::isTrue(
            field(asNode, "objectsTracked") == std::to_string(NodePoseLocator::kObjects) &&
                field(asSub, "objectsTracked") == std::to_string(NodePoseLocator::kObjects),
            "each kind's tracked set stops at its stated size");
        check::isTrue(field(asNode, "objectsRefused") == "3" &&
                          field(asSub, "objectsRefused") == "3",
                      "and the objects beyond it are refused and counted, per kind: " + asNode);
    }

    // **A rigid triple at the same offset in every object that never moves is not a pose.**
    // This is the case the first run of the instrument got wrong: it reported `moved 18`
    // beside `biggest delta 0.000000` -- values differing in the last mantissa bit -- and
    // named a static triple at three offsets 1020 bytes apart. The bar now needs a change
    // bigger than `kMotionEpsilon` between two draws, and this is the test for it: a
    // bitwise-different but immovable triple is reported, counted as `still`, and named
    // nothing.
    {
        std::map<uint32_t, std::vector<float>> memory;
        for (uint32_t object = 1; object <= 6; object++) {
            memory[object] = withPoseAt(20, 0.4f);
        }
        NodePoseLocator locator = makeLocator();
        g_nodes = &memory;
        locator.install();
        g_locator = &locator;
        for (uint32_t object = 1; object <= 6; object++) {
            for (int scan = 0; scan < 3; scan++) {
                // One part in 10^7 of a unit: different as bits, identical as a pose.
                memory[object] = withPoseAt(20, 0.4f + 1e-7f * static_cast<float>(scan + 1));
                locator.observe(object);
            }
        }
        const std::string body = locator.json();
        g_nodes = nullptr;
        const std::string mine = section(body, "node");
        check::isTrue(mine.find("\"objects\":6") != std::string::npos,
                      "six objects hold a rigid triple at the same offset: " + mine);
        check::isTrue(mine.find("\"moving\":false") != std::string::npos,
                      "and the report says it does not move");
        check::isTrue(mine.find("\"moved\":0") != std::string::npos &&
                          mine.find("\"still\":") != std::string::npos,
                      "with nothing counted as moved and every comparison counted as still, "
                      "which is the difference a bitwise test could not see");
        check::isTrue(locator.bestOffset() == 0 && field(mine, "bestOffset") == "null",
                      "and nothing is named, because a basis is the same shape as a pose and "
                      "is not one");
    }

    // A pose that moves is named, and the delta says by how much rather than printing as
    // zero: the first run's `biggest delta 0.000000` beside `moved 18` was the tell that the
    // bar was a bit test.
    {
        std::map<uint32_t, std::vector<float>> memory;
        for (uint32_t object = 1; object <= 4; object++) {
            memory[object] = withPoseAt(20, 0.1f * object);
        }
        NodePoseLocator locator = makeLocator();
        g_nodes = &memory;
        locator.install();
        g_locator = &locator;
        for (uint32_t object = 1; object <= 4; object++) {
            for (int scan = 0; scan < 3; scan++) {
                memory[object] = withPoseAt(20, 0.1f * object + 0.2f * scan);
                locator.observe(object);
            }
        }
        const std::string body = locator.json();
        g_nodes = nullptr;
        const std::string mine = section(body, "node");
        check::isTrue(field(mine, "moved") == "8",
                      "four objects saw their pose move, twice each over three scans, which is "
                      "the eight the denominator implies: " +
                          mine);
        check::isTrue(field(mine, "biggestDelta") != "0",
                      "and the delta is a number a reader can see, not a zero beside a "
                      "movement count");
        check::isTrue(locator.bestOffset() == 80, "so the offset is named");
    }

    // **The two windows are disjoint, and that is a fix, not a detail.** The sub-object is a
    // field *of* the node, at `+0xa1c`, so a node window wide enough to be useful reaches
    // into the sub-object's memory and a pose there is reported at the node's offset too --
    // one measurement counted twice, which is what the separate tables exist to prevent. The
    // first run did exactly that: the sub-object's `+80` appeared at the node's `+2668`.
    // So the node's window ends where the sub-object begins, and this is the test: a pose in
    // the sub-object must not appear in the node's table at all.
    {
        std::map<uint32_t, std::vector<float>> memory;
        for (uint32_t sub = 1; sub <= 3; sub++) {
            const uint32_t node = 0x2000u * sub;
            memory[node] = withNothing();
            memory[node + NodePoseLocator::kSubObjectOffset] = withPoseAt(20, 0.5f);
        }
        NodePoseLocator locator = makeLocator();
        g_nodes = &memory;
        locator.install();
        g_locator = &locator;
        for (uint32_t sub = 1; sub <= 3; sub++) {
            const uint32_t node = 0x2000u * sub;
            for (int scan = 0; scan < 3; scan++) {
                memory[node + NodePoseLocator::kSubObjectOffset] =
                    withPoseAt(20, 0.5f + 0.2f * scan);
                locator.observe(node + NodePoseLocator::kSubObjectOffset,
                                NodePoseLocator::Kind::SubObject);
                locator.observe(node, NodePoseLocator::Kind::Node);
            }
        }
        const std::string body = locator.json();
        g_nodes = nullptr;
        const std::string asNode = section(body, "node");
        const std::string asSub = section(body, "subObject");
        check::isTrue(field(asSub, "bestOffset") == "80",
                      "the sub-object's field is at its own offset, 80: " + asSub);
        check::isTrue(asNode.find("\"offsets\":{}") != std::string::npos,
                      "and the node's table is empty, because its window stops where the "
                      "sub-object begins: " +
                          asNode);
        check::isTrue(
            field(asNode, "scanBytes") == std::to_string(NodePoseLocator::kSubObjectOffset) &&
                field(asSub, "scanBytes") == std::to_string(NodePoseLocator::kScanWords * 4),
            "and each table states its own window, so a reader can see the two are "
            "apart rather than being told it");
    }

    // **The sample schedule is per frame, and this is the test for it.** An object is bound
    // several times per frame, so a locator that samples once per *binding* can take all
    // four of an object's samples inside one frame -- microseconds apart, where no pose has
    // moved by a thousandth of a unit. That is indistinguishable, in the report, from a
    // genuinely static field, and the first run with a real movement bar returned `0 moved,
    // 18 still, delta 0` for every candidate with no way to say which it was looking at.
    {
        std::map<uint32_t, std::vector<float>> memory;
        for (uint32_t object = 1; object <= 3; object++) {
            memory[object] = withPoseAt(20, 0.2f);
        }
        std::atomic<uint64_t> frame{1};
        NodePoseLocator locator = makeLocator();
        g_nodes = &memory;
        locator.install();
        g_locator = &locator;
        locator.setFrameCounter(&frame);

        // Four bindings of each object, all inside frame 1, all showing the same value. Only
        // the first of each is sampled; the other three are the same frame and say nothing.
        for (int bind = 0; bind < 4; bind++) {
            for (uint32_t object = 1; object <= 3; object++) {
                locator.observe(object);
            }
        }
        const std::string sameFrame = locator.json();
        g_nodes = nullptr;
        check::isTrue(field(section(sameFrame, "node"), "scans") == "3",
                      "twelve bindings inside one frame take three samples, one per object, so "
                      "no pose is compared with itself: " +
                          section(sameFrame, "node"));
        check::isTrue(sameFrame.find("\"schedule\":\"perFrame\"") != std::string::npos &&
                          sameFrame.find("\"frameCounter\":\"0x00000001\"") != std::string::npos,
                      "and the report says the schedule was per frame and which frame it was "
                      "on, rather than leaving a negative to be read as a fact: " +
                          sameFrame);

        // Three further frames, with each pose turning between them.
        g_nodes = &memory;
        for (int pass = 0; pass < 3; pass++) {
            frame.store(static_cast<uint64_t>(pass) + 2);
            for (uint32_t object = 1; object <= 3; object++) {
                memory[object] = withPoseAt(20, 0.2f + 0.3f * static_cast<float>(pass + 1));
                locator.observe(object);
            }
        }
        const std::string across = locator.json();
        g_nodes = nullptr;
        const std::string mine = section(across, "node");
        check::isTrue(field(mine, "scans") == "12" && field(mine, "moved") == "9",
                      "four samples each of three objects, so nine comparisons and all nine "
                      "moved, which is what the denominator says: " +
                          mine);
        check::isTrue(locator.bestOffset() == 80,
                      "and the offset is named, because the schedule could see it move");
    }

    // Without a frame counter the schedule is per binding, and the report says so. A locator
    // that cannot say how it sampled is a locator whose negatives cannot be believed.
    {
        std::map<uint32_t, std::vector<float>> memory;
        for (uint32_t object = 1; object <= 3; object++) {
            memory[object] = withPoseAt(20, 0.2f);
        }
        NodePoseLocator locator = makeLocator();
        g_nodes = &memory;
        locator.install();
        g_locator = &locator;
        for (uint32_t object = 1; object <= 3; object++) {
            for (int bind = 0; bind < 3; bind++) {
                locator.observe(object);
            }
        }
        const std::string body = locator.json();
        g_nodes = nullptr;
        check::isTrue(body.find("\"schedule\":\"perBind\"") != std::string::npos &&
                          body.find("\"frameCounter\":null") != std::string::npos,
                      "with no counter wired the schedule is per bind and named as such: " + body);
        check::isTrue(field(section(body, "node"), "scans") == "9",
                      "and every binding is a sample, three objects three times each: " +
                          section(body, "node"));
    }

    // **A scaled transform is a transform, and the strict bar will not count it.** This is
    // the measurement that decides between two different answers, so it is the one that
    // matters: a node whose own transform carries scale has its field *here*, and the parent
    // chain is needed only to compose with it; a node with no transform here has none, and
    // the transform a renderer multiplies -- the world matrix -- has to be read off the
    // parent instead. A rigid-only bar cannot tell those apart, because it reports nothing
    // for both. So the loose class counts any non-singular 3x3, the scale is reported, and
    // the two tables are both in the report.
    {
        std::map<uint32_t, std::vector<float>> memory;
        for (uint32_t object = 1; object <= 5; object++) {
            // Rows 2.5 long: a scale, and a pose that turns between draws.
            memory[object] = withPoseAt(20, 0.1f * object, 2.5f);
        }
        NodePoseLocator locator = makeLocator();
        g_nodes = &memory;
        locator.install();
        g_locator = &locator;
        for (uint32_t object = 1; object <= 5; object++) {
            for (int scan = 0; scan < 3; scan++) {
                memory[object] = withPoseAt(20, 0.1f * object + 0.2f * scan, 2.5f);
                locator.observe(object);
            }
        }
        const std::string body = locator.json();
        g_nodes = nullptr;
        const std::string mine = section(body, "node");
        const std::string rigid = section(mine, "rigid");
        const std::string affine = section(mine, "affine");
        check::isTrue(rigid.find("\"bestOffset\":null") != std::string::npos,
                      "the strict bar names nothing, because a scaled transform is not a "
                      "rigid one: " +
                          rigid);
        check::isTrue(affine.find("\"bestOffset\":80") != std::string::npos &&
                          locator.bestAffineOffset() == 80,
                      "and the loose bar names the same offset, so the field is here and "
                      "carries scale: " +
                          affine);
        check::isTrue(affine.find("\"scale\":1.5") != std::string::npos,
                      "with the scale reported -- 1.5 is how far the row lengths are from "
                      "unit -- so a scaled transform is not mistaken for a rigid one: " +
                          affine);
        check::isTrue(affine.find("\"moving\":true") != std::string::npos,
                      "and it moves, which is what the loose bar also requires");
    }

    // The loose class has a floor, or it is a bar that cannot fail: a plane of near-zero
    // numbers has a determinant near zero and must not read as a matrix.
    {
        std::map<uint32_t, std::vector<float>> memory;
        for (uint32_t object = 1; object <= 4; object++) {
            std::vector<float> words = withNothing();
            // Rows of 1e-5: non-zero, non-parallel in the loosest sense, and a
            // determinant of 1e-15, far below the floor.
            for (size_t word = 0; word < 12; word++) {
                words[20 + word] = 1e-5f * static_cast<float>(1 + (word % 4));
            }
            memory[object] = words;
        }
        NodePoseLocator locator = makeLocator();
        g_nodes = &memory;
        locator.install();
        g_locator = &locator;
        for (uint32_t object = 1; object <= 4; object++) {
            for (int scan = 0; scan < 3; scan++) {
                locator.observe(object);
            }
        }
        const std::string body = locator.json();
        g_nodes = nullptr;
        const std::string affine = section(section(body, "node"), "affine");
        check::isTrue(affine.find("\"offsets\":{}") != std::string::npos,
                      "a near-degenerate triple is not a transform, so the loose bar finds "
                      "nothing where a floorless one would find something at every offset: " +
                          affine);
        check::isTrue(NodePoseLocator::kDeterminantFloor > 0.0,
                      "and the floor is a positive number in the report, so a reader can see "
                      "what the loose bar is willing to call a matrix");
    }

    // The report is one object that ends, because a client parses it, and it names both
    // kinds and the vtable's target -- three addresses a reader would otherwise have to
    // find in the code.
    {
        NodePoseLocator locator = makeLocator();
        locator.install();
        const std::string body = locator.json();
        check::isTrue(body.front() == '{' && body.find("}\n") != std::string::npos,
                      "the body is one object that ends");
        check::isTrue(body.find("\"vtableTargetNotProbed\"") != std::string::npos &&
                          body.find("\"subObject\"") != std::string::npos &&
                          body.find("\"subObjectOffset\"") != std::string::npos,
                      "and it names the vtable's target and both kinds, because they are "
                      "different addresses and a reader should not have to find that out from "
                      "the code");
        check::isTrue(field(body, "entryWordMatches") == "false",
                      "and it says the entry's word did not match the image's, which is the "
                      "reason a refused install is reported -- the fake guest has no code at "
                      "the draw, and a report that hid the mismatch would hide the refusal's "
                      "cause: " +
                          body);
    }
}
