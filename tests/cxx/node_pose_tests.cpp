/// Which field of a node holds its pose.
///
/// Two measurements in a row put the pose outside a uniform, so this looks in the node's own
/// memory at the node's own draw. What makes the answer a locator rather than a coincidence
/// is the cross-node test: a pose field is at one offset in *every* node, while a colour
/// triple that happens to be near unit length is at a different offset in each. Every test
/// here is about that, or about the probe site being somewhere the title actually enters.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/NodePoseLocator.h"

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

using wiiuport::title::NodePoseLocator;

std::map<uint32_t, std::vector<float>>* g_nodes = nullptr;
GuestCallProbes::Probe* g_probe = nullptr;
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
    return NodePoseLocator(&keepRegistration, &readWords);
}

NodePoseLocator* g_locator = nullptr;

// The route the product uses: a probe on the sub-object's binder, whose argument is the
// sub-object, one fixed subtraction from the node. The probe is exercised too, because the
// two paths are the same code reached two ways and the entry's being uncalled is a
// measurement the report carries.
void draw(uint32_t node) {
    g_locator->observe(node);
}

// The other path, kept and counted: the probed entry, which installs and is never called
// because the title dispatches the draw through the vtable's target. Exercised so the
// report's `calls` is a number something tested rather than a field nothing reaches.
void drawThroughProbe(uint32_t node) {
    std::array<uint32_t, 32> gpr{};
    gpr[NodePoseLocator::kNodeRegister] = node;
    g_probe->OnCall(std::span<const uint32_t, 32>(gpr.data(), gpr.size()), 0);
}

// A node whose memory holds a rigid transform at a byte offset, and zeros elsewhere.
std::vector<float> nodeWithPoseAt(size_t floatOffset, float spin) {
    std::vector<float> words(NodePoseLocator::kScanWords, 0.0f);
    const float s = std::sin(spin);
    const float c = std::cos(spin);
    const float pose[12] = {c, s, 0.0f, -s, c, 0.0f, 0.0f, 0.0f, 1.0f, 10.0f * c, 10.0f * s, -5.0f};
    for (size_t word = 0; word < 12; word++) {
        words[floatOffset + word] = pose[word];
    }
    return words;
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
    }

    // A pose at the same offset in several nodes is named, and the count of nodes is what
    // names it.
    {
        std::map<uint32_t, std::vector<float>> nodes;
        for (uint32_t node = 1; node <= 5; node++) {
            nodes[node] = nodeWithPoseAt(20, 0.1f * static_cast<float>(node));
        }
        NodePoseLocator locator = makeLocator();
        g_nodes = &nodes;
        locator.install();
        g_locator = &locator;
        for (uint32_t node = 1; node <= 5; node++) {
            for (int scan = 0; scan < 3; scan++) {
                nodes[node] = nodeWithPoseAt(20, 0.1f * static_cast<float>(node) + 0.05f * scan);
                draw(node);
            }
        }
        const std::string body = locator.json();
        g_nodes = nullptr;
        check::isTrue(NodePoseLocator::kSubObjectOffset == 0xa1c,
                      "and the node's sub-object sits 0xa1c from the node, which is the draw's "
                      "own `addi r3,r28,0xa1c` -- so the binder's argument is the node");
        check::isTrue(field(body, "nodesTracked") == "5", "five distinct nodes tracked");
        check::isTrue(locator.bestOffset() == 80,
                      "and the offset five nodes agree on is named -- 20 floats is 80 bytes: " +
                          body);
        check::isTrue(field(body, "nodesNeeded") == "3",
                      "with the number of nodes needed stated, so the bar is not a mystery");
        check::isTrue(field(body, "moved") != "0",
                      "and the value moved between scans of a node, which is what separates a "
                      "pose from a constant: " +
                          body);
    }

    // The same shape at a *different* offset in each node is not a field. Three nodes, three
    // offsets, nothing named -- which is the case a "found a transform" report would call a
    // pass and a cross-node locator calls nothing.
    {
        std::map<uint32_t, std::vector<float>> nodes;
        for (uint32_t node = 1; node <= 3; node++) {
            nodes[node] = nodeWithPoseAt(static_cast<size_t>(node) * 7, 0.2f);
        }
        NodePoseLocator locator = makeLocator();
        g_nodes = &nodes;
        locator.install();
        g_locator = &locator;
        for (uint32_t node = 1; node <= 3; node++) {
            for (int scan = 0; scan < 3; scan++) {
                draw(node);
            }
        }
        const std::string body = locator.json();
        g_nodes = nullptr;
        check::isTrue(field(body, "calls") == "0",
                      "and nothing came through the probed entry, because the title dispatches "
                      "the draw through the vtable's target instead");
        check::isTrue(field(body, "nodesNeeded") == "2", "two nodes would have been enough");
        check::isTrue(locator.bestOffset() == 0,
                      "and nothing is named, because no offset is held by two nodes");
        check::isTrue(field(body, "bestOffset") == "null",
                      "and the report says null rather than the best candidate it saw");
    }

    // One node cannot agree with another, so it names nothing however many times it is
    // scanned. Reported, not silent.
    {
        std::map<uint32_t, std::vector<float>> nodes;
        nodes[1] = nodeWithPoseAt(20, 0.3f);
        NodePoseLocator locator = makeLocator();
        g_nodes = &nodes;
        locator.install();
        g_locator = &locator;
        for (int scan = 0; scan < NodePoseLocator::kScansPerNode; scan++) {
            draw(1);
        }
        const std::string body = locator.json();
        g_nodes = nullptr;
        check::isTrue(field(body, "nodesTracked") == "1", "one node tracked");
        check::isTrue(field(body, "nodesNeeded") == "0",
                      "and the nodes needed is zero, because a single node cannot settle a "
                      "cross-node question");
        check::isTrue(locator.bestOffset() == 0, "so nothing is named");
    }

    // The probed entry does reach the locator when something calls it, so `calls` is a real
    // count and not a field nothing writes.
    {
        std::map<uint32_t, std::vector<float>> nodes;
        nodes[1] = nodeWithPoseAt(20, 0.3f);
        NodePoseLocator locator = makeLocator();
        g_nodes = &nodes;
        locator.install();
        g_locator = &locator;
        drawThroughProbe(1);
        const std::string body = locator.json();
        g_nodes = nullptr;
        check::isTrue(field(body, "calls") == "1", "a call through the probed entry is counted");
        check::isTrue(field(body, "nodesTracked") == "1", "and the node in r3 is tracked");
    }

    // A node whose memory does not read is counted, and contributes nothing.
    {
        std::map<uint32_t, std::vector<float>> nodes;
        nodes[1] = nodeWithPoseAt(20, 0.3f);
        NodePoseLocator locator = makeLocator();
        g_nodes = &nodes;
        locator.install();
        draw(1);
        draw(0x7fffffff); // not in the map
        const std::string body = locator.json();
        g_nodes = nullptr;
        check::isTrue(field(body, "readsUnreadable") == "1",
                      "a node whose memory does not read is counted as unreadable: " + body);
    }

    // The scan is bounded, and the refusal is counted: a node list that grew with the scene
    // would be a list of every object the title has ever drawn.
    {
        std::map<uint32_t, std::vector<float>> nodes;
        for (uint32_t node = 1; node <= NodePoseLocator::kNodes + 3; node++) {
            nodes[node] = nodeWithPoseAt(20, 0.1f);
        }
        NodePoseLocator locator = makeLocator();
        g_nodes = &nodes;
        locator.install();
        g_locator = &locator;
        for (uint32_t node = 1; node <= NodePoseLocator::kNodes + 3; node++) {
            draw(node);
        }
        const std::string body = locator.json();
        g_nodes = nullptr;
        check::isTrue(field(body, "nodesTracked") == std::to_string(NodePoseLocator::kNodes),
                      "the tracked set stops at its stated size");
        check::isTrue(field(body, "nodesRefused") == "3",
                      "and the nodes beyond it are refused and counted");
    }

    // The report is one object that ends, because a client parses it.
    {
        NodePoseLocator locator = makeLocator();
        locator.install();
        const std::string body = locator.json();
        check::isTrue(body.front() == '{' && body.find("}\n") != std::string::npos,
                      "and the body is one object that ends");
        check::isTrue(body.find("\"vtableTargetNotProbed\"") != std::string::npos,
                      "and it names the vtable's target as well as the probed entry, because "
                      "they are different addresses and a reader should not have to find that "
                      "out from the code");
    }
}
