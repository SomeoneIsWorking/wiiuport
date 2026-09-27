/// Where in an assembled uniform buffer the pose is, found by shape and then constant.
///
/// The blend needs one number, and every test here is about whether this reports that
/// number honestly: a transform seen once is a coincidence, a transform that never moves is
/// a colour triple that looked like one, and a buffer too large to scan is not a buffer
/// with no pose in it.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/ObjectPoseLocator.h"

#include <string>
#include <vector>

namespace {

using wiiuport::frame::RecordedUniformAssembly;
using wiiuport::title::ObjectPoseLocator;

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

RecordedUniformAssembly assembly(std::vector<float> data, std::vector<uint32_t> sources) {
    RecordedUniformAssembly one;
    one.data = std::move(data);
    one.blockSources = std::move(sources);
    one.stageIndex = 0;
    return one;
}

// A rigid transform: three orthonormal rows and a translation in the fourth group.
void putPose(std::vector<float>& words, size_t at, float spin) {
    const float s = std::sin(spin);
    const float c = std::cos(spin);
    const float pose[12] = {c, s, 0.0f, -s, c, 0.0f, 0.0f, 0.0f, 1.0f, 10.0f * c, 10.0f * s, -5.0f};
    for (size_t word = 0; word < 12; word++) {
        words[at + word] = pose[word];
    }
}

} // namespace

void wiiuport::tests::runObjectPoseLocatorTests() {
    // A pose that recurs in most assemblies is named, with the count that named it. A
    // transform seen in one assembly of ten is not an offset.
    {
        ObjectPoseLocator locator;
        for (int seen = 0; seen < 10; seen++) {
            std::vector<float> words(64, 0.0f);
            putPose(words, 16, 0.1f * static_cast<float>(seen));
            // The same block sources every time, so the readings are one object's and
            // the movement count means something.
            locator.onAssemblyRecorded(assembly(words, {0x3e000000u}));
        }
        const std::string body = locator.json();
        check::isTrue(field(body, "assemblies") == "10", "ten assemblies seen");
        check::isTrue(field(body, "believedOffsets") == "1", "one offset was held often enough");
        check::isTrue(field(body, "bestOffset") == "64",
                      "and it is reported in bytes -- 16 floats is 64 bytes -- because a "
                      "substitution writes at a byte offset");
        check::isTrue(field(body, "candidates") == "1",
                      "and exactly one candidate offset was ever a pose, so the scan is not "
                      "manufacturing hits out of noise");
        check::isTrue(field(body, "moved") != "0",
                      "and the value moved between assemblies of one identity, which is what "
                      "separates a pose from a colour triple: " +
                          body);
    }

    // Ten assemblies, one of them with a pose, names nothing. This is the case a
    // "found a transform" report would call a pass.
    {
        ObjectPoseLocator locator;
        for (int seen = 0; seen < 10; seen++) {
            std::vector<float> words(64, 0.0f);
            if (seen == 3) {
                putPose(words, 16, 0.5f);
            }
            locator.onAssemblyRecorded(assembly(words, {0x3e000000u}));
        }
        const std::string body = locator.json();
        check::isTrue(field(body, "candidates") == "1", "one candidate was seen");
        check::isTrue(field(body, "believedOffsets") == "0",
                      "and it was not believed, because one assembly in ten is a coincidence and "
                      "the bar is stated");
        check::isTrue(field(body, "bestOffset") == "null",
                      "and the believed offset is null rather than the candidate, so a caller "
                      "cannot mistake a coincidence for an answer");
        check::isTrue(locator.bestOffset() == 0, "and bestOffset agrees, as zero");
    }

    // A transform that never moves is a colour triple that looked like one: it is
    // compared and it does not move, and both counts are reported.
    {
        ObjectPoseLocator locator;
        for (int seen = 0; seen < 10; seen++) {
            std::vector<float> words(64, 0.0f);
            putPose(words, 16, 0.25f);
            locator.onAssemblyRecorded(assembly(words, {0x3e000000u}));
        }
        const std::string body = locator.json();
        check::isTrue(field(body, "assemblies") == "10", "ten assemblies of one identity");
        check::isTrue(field(body, "compared") == "9", "nine repeat comparisons");
        check::isTrue(field(body, "moved") == "0",
                      "and none of them moved, which is what a standing object looks like and "
                      "what a pose nobody writes also looks like: " +
                          body);
    }

    // A buffer larger than the scan's bound is reported unscanned, not scanned in part: a
    // partial scan's "no pose here" is about the part it looked at.
    {
        ObjectPoseLocator locator;
        std::vector<float> words(ObjectPoseLocator::kMaxScanBytes / sizeof(float) + 64, 0.0f);
        putPose(words, 8, 0.5f);
        locator.onAssemblyRecorded(assembly(words, {0x3e000000u}));
        const std::string body = locator.json();
        check::isTrue(field(body, "unscannedBuffers") == "1",
                      "a buffer past the bound is counted as unscanned");
        check::isTrue(field(body, "candidates") == "0",
                      "and contributes no candidate, rather than contributing one from the part "
                      "that was looked at");
    }

    // An assembly with no block sources has no identity, so it is counted and skipped
    // rather than merged with whatever identity came before it.
    {
        ObjectPoseLocator locator;
        for (int seen = 0; seen < 4; seen++) {
            std::vector<float> words(64, 0.0f);
            putPose(words, 16, 0.2f);
            locator.onAssemblyRecorded(assembly(
                words, seen == 0 ? std::vector<uint32_t>{} : std::vector<uint32_t>{0x3e000000u}));
        }
        const std::string body = locator.json();
        check::isTrue(field(body, "buffersWithoutSources") == "1",
                      "an assembly with no block sources is counted on its own");
    }

    // A buffer too short to hold a pose is not a buffer with no pose in it, and is
    // neither scanned nor reported as a candidate.
    {
        ObjectPoseLocator locator;
        std::vector<float> words(4, 1.0f);
        locator.onAssemblyRecorded(assembly(words, {0x3e000000u}));
        const std::string body = locator.json();
        check::isTrue(field(body, "assemblies") == "1", "it was still an assembly");
        check::isTrue(field(body, "candidates") == "0", "and it contributed no candidate");
    }

    // The report is one object that ends, because a client parses it.
    {
        ObjectPoseLocator locator;
        const std::string body = locator.json();
        check::isTrue(body.front() == '{' && body.find("}\n") != std::string::npos,
                      "and the body is one object that ends");
        check::isTrue(field(body, "bestOffset") == "null",
                      "and with no assemblies at all the believed offset is null, not zero");
    }
}
