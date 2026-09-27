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

// One nested object out of the report. Both shape tables carry `bestOffset` and
// `believedOffsets`, and a reader that cannot tell them apart reads whichever comes first --
// which is the whole difference between "a pose" and "a transform with scale on it".
std::string section(const std::string& body, const std::string& name) {
    const std::string key = "\"" + name + "\":{";
    const size_t at = body.find(key);
    if (at == std::string::npos) {
        return "";
    }
    size_t depth = 0;
    for (size_t index = at + name.size() + 2; index < body.size(); index++) {
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

RecordedUniformAssembly assembly(std::vector<float> data, std::vector<uint32_t> sources) {
    RecordedUniformAssembly one;
    one.data = std::move(data);
    one.blockSources = std::move(sources);
    one.stageIndex = 0;
    return one;
}

// A transform: three rows and a translation, with a scale on the rows so a caller can ask
// for the scaled case the strict class cannot see. Rigid at scale 1.
void putPose(std::vector<float>& words, size_t at, float spin, float scale = 1.0f) {
    const float s = std::sin(spin);
    const float c = std::cos(spin);
    const float pose[12] = {scale * c, scale * s, 0.0f,  -scale * s, scale * c, 0.0f,
                            0.0f,      0.0f,      scale, 10.0f * c,  10.0f * s, -5.0f};
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
        const std::string rigid = section(body, "rigid");
        const std::string affine = section(body, "affine");
        check::isTrue(field(body, "assemblies") == "10", "ten assemblies seen");
        check::isTrue(field(rigid, "believedOffsets") == "1", "one offset was held often enough");
        check::isTrue(field(rigid, "bestOffset") == "64",
                      "and it is reported in bytes -- 16 floats is 64 bytes -- because a "
                      "substitution writes at a byte offset");
        check::isTrue(field(rigid, "candidates") == "1",
                      "and exactly one offset was ever a RIGID transform, so the strict scan is "
                      "not manufacturing hits out of noise -- the loose class legitimately finds "
                      "more, which is why this says rigid: " +
                          rigid);
        check::isTrue(field(body, "candidates") != "" && field(body, "candidates") != "0",
                      "and the loose class found candidates of its own, so the two are not the "
                      "same measurement reported twice");
        check::isTrue(field(rigid, "moved") != "0",
                      "and the value moved between assemblies of one identity, which is what "
                      "separates a pose from a colour triple: " +
                          rigid);
        check::isTrue(field(affine, "bestOffset") == "64",
                      "and the loose class names the same offset, because a rigid transform is "
                      "also a non-singular one: " +
                          affine);
        check::isTrue(locator.bestOffset() == 64 && locator.bestAffineOffset() == 64,
                      "and both accessors agree with their tables");
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
        const std::string rigid = section(body, "rigid");
        check::isTrue(field(rigid, "candidates") == "1", "one rigid candidate was seen");
        check::isTrue(field(rigid, "believedOffsets") == "0",
                      "and it was not believed, because one assembly in ten is a coincidence and "
                      "the bar is stated");
        check::isTrue(field(rigid, "bestOffset") == "null",
                      "and the believed offset is null rather than the candidate, so a caller "
                      "cannot mistake a coincidence for an answer");
        check::isTrue(locator.bestOffset() == 0, "and bestOffset agrees, as zero");
        check::isTrue(locator.bestAffineOffset() == 0,
                      "and the loose class names nothing either, so a single sighting is a "
                      "coincidence in both readings and not a scaled pose");
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
        const std::string rigid = section(body, "rigid");
        check::isTrue(field(body, "assemblies") == "10", "ten assemblies of one identity");
        check::isTrue(field(rigid, "compared") == "9", "nine repeat comparisons");
        check::isTrue(field(rigid, "moved") == "0" && field(rigid, "still") == "9",
                      "and none of them moved while all nine comparisons were counted, which is "
                      "what a standing object looks like and what a pose nobody writes also "
                      "looks like -- so a bar that counts movement without counting the "
                      "comparisons it had could not tell them apart: " +
                          rigid);
        check::isTrue(field(rigid, "bestOffset") == "null",
                      "and a value that holds a rigid shape in every assembly and never changes "
                      "is named nothing, so the answer is a null rather than a basis matrix");
    }

    // **A scaled pose is a pose, and the strict class cannot see it.** This is the case the
    // two bars exist for. The node scan found a dozen non-singular 3x3s that no rigid test
    // would count, every one of them static, and the lesson was that "no rigid transform" and
    // "no transform" are different sentences. Here the scaled value is the one that moves, so
    // the loose bar names it and the strict bar names nothing -- and a report with only the
    // strict bar would have said the pose was not in the assembled buffers.
    {
        ObjectPoseLocator locator;
        for (int seen = 0; seen < 10; seen++) {
            std::vector<float> words(64, 0.0f);
            putPose(words, 16, 0.1f * static_cast<float>(seen), 2.5f);
            locator.onAssemblyRecorded(assembly(words, {0x3e000000u}));
        }
        const std::string body = locator.json();
        const std::string rigid = section(body, "rigid");
        const std::string affine = section(body, "affine");
        check::isTrue(field(rigid, "bestOffset") == "null",
                      "the strict bar names nothing, because a scaled transform is not a rigid "
                      "one: " +
                          rigid);
        check::isTrue(field(affine, "bestOffset") == "64" && locator.bestAffineOffset() == 64,
                      "and the loose bar names it, so the pose IS in the assembled buffers: " +
                          affine);
        check::isTrue(field(affine, "moved") == "9",
                      "with all nine comparisons counted as movement, which is the number of "
                      "repeat assemblies of one identity");
        check::isTrue(field(affine, "scale") == "1.5",
                      "and the scale reported -- 1.5 is how far the row lengths are from unit -- "
                      "so a scaled transform is not passed off as a rigid one: " +
                          affine);
        check::isTrue(locator.bestOffset() == 0,
                      "while the strict accessor still says zero, because the two accessors "
                      "answer different questions and neither is standing in for the other");
    }

    // **A report a client can parse, whatever the guest's memory holds.** The loose class
    // admits any non-singular 3x3, which includes rows long enough to overflow a float to
    // infinity; a numeric formatter writes those as `inf`, JSON allows neither, and a parser
    // stops at the first one. It happened here first -- `GET /blocks` came back as something
    // no client could read and the run called it an I/O failure. The fix is one formatter,
    // shared, which is what makes it hold in both locators rather than in whichever was
    // fixed last.
    {
        ObjectPoseLocator locator;
        for (int seen = 0; seen < 6; seen++) {
            std::vector<float> words(64, 0.0f);
            const float huge[12] = {3.0e19f, 0.0f, 0.0f,    0.0f, 2.0e19f, 0.0f,
                                    0.0f,    0.0f, 1.0e19f, 5.0f, 6.0f,    7.0f};
            for (size_t word = 0; word < 12; word++) {
                words[16 + word] = huge[word] * (1.0f + 0.1f * static_cast<float>(seen));
            }
            locator.onAssemblyRecorded(assembly(words, {0x3e000000u}));
        }
        const std::string body = locator.json();
        check::isTrue(body.find(":inf") == std::string::npos &&
                          body.find(":nan") == std::string::npos,
                      "no bare inf or nan appears as a value, because a JSON parser stops at "
                      "the first one and the whole report is then unreadable: " +
                          body);
        check::isTrue(body.find("\"inf\"") != std::string::npos,
                      "and the fact appears as a quoted string instead, so it survives as a fact");
        check::isTrue(body.front() == '{' && body.find("}\n") != std::string::npos,
                      "and the body is still one object that ends");
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
        check::isTrue(field(section(body, "rigid"), "bestOffset") == "null" &&
                          field(section(body, "affine"), "bestOffset") == "null",
                      "and with no assemblies at all both believed offsets are null, not zero");
    }
}
