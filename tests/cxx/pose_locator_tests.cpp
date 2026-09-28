/// Where in an assembled uniform buffer the pose is, found by shape and then constant.
///
/// The blend needs one number, and every test here is about whether this reports that
/// number honestly: a transform seen once is a coincidence, a transform that never moves is
/// a colour triple that looked like one, and a buffer too large to scan is not a buffer
/// with no pose in it.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/ObjectPoseLocator.h"

#include <limits>
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
    std::string value = body.substr(start, end - start);
    // **A JSON string's value comes back with its quotes**, and a comparison against a C++ literal
    // that forgets them fails for a reason no message shows. The same reader in
    // `global_pose_tests.cpp` strips them; this is the same fix in the second place it was needed,
    // which is a duplication worth naming rather than a coincidence.
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
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

// A field's value out of a JSON object, braces counted so a nested object's field of the same name
// is not mistaken for this one's.
std::string nested(const std::string& body, const std::string& name) {
    return field(body, name);
}

bool mentions(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

RecordedUniformAssembly assembly(std::vector<float> data, std::vector<uint32_t> sources,
                                 uint32_t object = 0) {
    RecordedUniformAssembly one;
    one.data = std::move(data);
    one.blockSources = std::move(sources);
    one.objectAddress = object;
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

// **One matrix is one candidate, however many alignments it is affine at.** This is the defect the
// real title exposed: eight of the twelve held offsets were one flat array of seventeen words seen
// at eight alignments, laid over each other with no contradiction, and each alignment had become a
// candidate with its own counts -- so `bestOffset` was the maximum over a run of offsets that are
// the same matrix, and it moved between runs for that reason alone.
//
// The fixture writes a 3x4 into a flat run of floats and asks for the count, and the negative is
// beside it: a *second* matrix elsewhere in the same assembly is a second candidate, because the
// collapse folds windows on one value and not two.
void oneMatrixIsOneCandidateHoweverManyAlignmentsItIsAffineAt() {
    const size_t at = 8;
    std::vector<float> words(64, 0.0f);
    putPose(words, at, 0.3f);
    // A second matrix, far enough away that the collapse must not fold it in: a span is twelve
    // words, so anything past one 3x4 apart is its own value.
    putPose(words, 40, 0.9f, 2.0f);

    ObjectPoseLocator locator;
    for (int round = 0; round < 30; round++) {
        locator.onAssemblyRecorded(assembly(words, {0x3e000000u}, 0x1000u));
    }
    const std::string body = locator.json();
    // The two matrices, each once.
    const size_t first = body.find("\"offset\":" + std::to_string(at * sizeof(float)));
    const size_t second = body.find("\"offset\":" + std::to_string(40 * sizeof(float)));
    check::isTrue(first != std::string::npos && second != std::string::npos,
                  "both matrices are candidates: " + body.substr(0, 200));
    // And no third: every affine window inside either matrix was folded into one of them, so the
    // table's length is the number of matrices and not the number of alignments.
    //
    // **Counted in the affine table alone.** The report has a rigid table and an affine table, and
    // an offset that is rigid in some assemblies and affine in others is in both -- so counting the
    // whole body counts one matrix twice and reports three candidates for two. That is the same
    // class of mistake as the one being tested: a count that is a property of the report's shape
    // rather than of the data.
    const size_t affineAt = body.find("\"affine\":");
    check::isTrue(affineAt != std::string::npos, "the affine table is in the report");
    size_t offsets = 0;
    for (size_t at_ = body.find("\"offset\":", affineAt); at_ != std::string::npos;
         at_ = body.find("\"offset\":", at_ + 1)) {
        ++offsets;
    }
    check::isTrue(offsets == 2,
                  "two matrices and no more, though each is affine at up to twelve offsets: " +
                      std::to_string(offsets) + " candidates for 2 matrices");

    // The first matrix's entry carries the over-count it absorbed, so a reader can see what the
    // collapse removed rather than having to trust that something was.
    const size_t next = body.find("\"offset\":", first + 1);
    const std::string entry =
        body.substr(first, (next == std::string::npos ? body.size() : next) - first);
    check::isTrue(mentions(entry, "\"foldedFromAlignments\":"),
                  "the entry says how many alignments were folded into it: " + entry);
    check::isTrue(mentions(entry, "\"alignmentsSeen\":"),
                  "and how many places the matrix was seen: " + entry);
}

void wiiuport::tests::runObjectPoseLocatorTests() {
    oneMatrixIsOneCandidateHoweverManyAlignmentsItIsAffineAt();
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

    // **The node is the identity, and this is the measurement that says so.** The fallback --
    // the guest addresses the draw sourced its uniforms from -- was measured to match exactly
    // one identity across 438,872 assemblies, because the uniform block is re-uploaded at a new
    // address each frame. So here the *same object* is given a *different* block source on every
    // assembly, exactly as the title does, and the pose moves between them. A locator that keys
    // on the block source sees ten sightings of ten different objects and no comparisons at
    // all; a locator that keys on the node sees one object, nine comparisons, nine movements.
    // That difference is the whole reason `objectAddress` exists, and this is the test for it.
    {
        ObjectPoseLocator locator;
        for (int seen = 0; seen < 10; seen++) {
            std::vector<float> words(64, 0.0f);
            putPose(words, 16, 0.1f * static_cast<float>(seen));
            // A new address every assembly, as the title produces: the same object, re-uploaded.
            locator.onAssemblyRecorded(assembly(
                words, {0x3e000000u + static_cast<uint32_t>(seen) * 0x1000u}, 0x43e01000u));
        }
        const std::string body = locator.json();
        const std::string rigid = section(body, "rigid");
        check::isTrue(field(rigid, "compared") == "9",
                      "nine repeat comparisons, because the ten assemblies were one object "
                      "despite ten different block addresses: " +
                          rigid);
        check::isTrue(field(rigid, "moved") == "9" && locator.bestOffset() == 64,
                      "and nine movements, so the pose is found in the buffer the title "
                      "assembled for it -- which the address-keyed version of this could not do "
                      "at all");
        check::isTrue(field(body, "identitiesSeen") == "1",
                      "and exactly one identity, which is the number the address key would have "
                      "reached had it been used: " +
                          body);
    }

    // Without a published object the fallback still works, and the report says which source it
    // used -- a reader must not have to read the code to know whether a number came from the
    // node or from an address.
    {
        ObjectPoseLocator locator;
        for (int seen = 0; seen < 10; seen++) {
            std::vector<float> words(64, 0.0f);
            putPose(words, 16, 0.1f * static_cast<float>(seen));
            locator.onAssemblyRecorded(assembly(words, {0x3e000000u}, 0));
        }
        const std::string body = locator.json();
        check::isTrue(field(body, "identitiesSeen") == "1",
                      "with the same block source every time, the fallback also yields one "
                      "identity -- which is why the real run matched one: " +
                          body);
        check::isTrue(field(body, "identitySource") == "blockSources",
                      "and the report names the source it used, so a number is never silently "
                      "from the weaker of two keys");
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

    // **A window holding a non-finite value is not a transform at all**, and saying so beats
    // counting it and reporting an infinite scale. Guest memory holds values large enough to
    // overflow a float, and the loose class used to admit them: the first run of it named an
    // offset in the assembled buffers with "rows off unit by 364193" -- arithmetic wearing a
    // 3x3's shape. A non-finite window is refused, and the reason is a classification rather
    // than a number nobody can read.
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
        const std::string affine = section(body, "affine");
        check::isTrue(field(affine, "bestOffset") == "null",
                      "an offset whose rows are 364193 times unit is not named, because that is "
                      "a projection constant and not a pose: " +
                          affine);
        check::isTrue(field(affine, "candidates") == "0",
                      "and the loose class counts no such window at all, rather than counting "
                      "it and reporting a scale no reader can use");
    }

    // A scale beyond the ceiling is refused, and one inside it is not. The ceiling is
    // generous on purpose -- a hundredfold -- because the point is to exclude the numbers that
    // are arithmetic, not to find a transform at exactly one scale.
    {
        std::vector<float> scaled(12, 0.0f);
        putPose(scaled, 0, 0.0f, 2.5f);
        std::vector<float> huge(12, 0.0f);
        putPose(huge, 0, 0.0f, ObjectPoseLocator::Shape::kScaleCeiling * 2.0f);
        check::isTrue(ObjectPoseLocator::Shape::isAffine(scaled.data()),
                      "a transform with rows 2.5 long is in the loose class");
        check::isTrue(!ObjectPoseLocator::Shape::isRigid(scaled.data()),
                      "and is not in the strict one, which is the whole point of having both");
        check::isTrue(ObjectPoseLocator::Shape::classify(huge.data()) ==
                          ObjectPoseLocator::Shape::Affine::TooLarge,
                      "and a row length past the ceiling is refused with the reason given, not "
                      "merely refused");
        // 3e19 is a finite float -- 3e38 is the ceiling, and it was worth checking rather than
        // assuming -- so the case above is `TooLarge`, not `NotFinite`. A real infinity has to
        // be put in deliberately, and guest memory can hold one: a value divided to overflow,
        // or a half-written register.
        std::vector<float> overflowing(12, 0.0f);
        const float big[12] = {3.0e19f, 0.0f, 0.0f,    0.0f, 2.0e19f, 0.0f,
                               0.0f,    0.0f, 1.0e19f, 5.0f, 6.0f,    7.0f};
        for (size_t word = 0; word < 12; word++) {
            overflowing[word] = big[word];
        }
        check::isTrue(ObjectPoseLocator::Shape::classify(overflowing.data()) ==
                          ObjectPoseLocator::Shape::Affine::TooLarge,
                      "so rows of 3e19 -- finite, and far past the ceiling -- are refused as too "
                      "large rather than as unreadable");
        overflowing[0] = std::numeric_limits<float>::infinity();
        check::isTrue(ObjectPoseLocator::Shape::classify(overflowing.data()) ==
                          ObjectPoseLocator::Shape::Affine::NotFinite,
                      "and a window that really does hold an infinity is a different refusal "
                      "again, because \"rows off unit by inf\" is not a number anybody can "
                      "read");
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
    {
        // **A value every object shares is a global, and a value each object has is not.** This is
        // the question the blend turns on: a camera's view matrix is written once a frame and read
        // by every shader, so two objects hold the *same* twelve floats at the same offset, while a
        // static prop's world matrix is that object's own and two objects hold different ones. The
        // share of assemblies an offset appears in cannot tell those apart -- a quarter of the
        // frame's draws being objects fits a per-object pose exactly as well as it fits a global --
        // so the discriminator is the value, compared across identities.
        //
        // Both halves in one place, because a test with only the agreeing case would pass against
        // a counter that never counted.
        // **A frame's worth of objects sharing one value, and that value advancing per frame** --
        // which is the shape of a camera's view matrix and the only shape the question can be asked
        // of. A single frame of six identical assemblies cannot tell a global from a per-object
        // value, because there is nothing for the value to differ from; and the first version of
        // this fixture was exactly that, and passed with the instrument's retained value never
        // advancing. **A test that cannot fail is not a test**, and this one had a mutation in it
        // that nothing noticed.
        auto globalFrame = [](uint32_t object, float spin) {
            std::vector<float> words(64, 0.0f);
            putPose(words, 8, spin);
            return assembly(words, {0x3e000000u}, object);
        };
        auto perObjectValue = [](uint32_t object) {
            std::vector<float> words(64, 0.0f);
            // A different spin per object, which is what two props at different places hold.
            putPose(words, 8, 0.4f + static_cast<float>(object) * 0.37f);
            return assembly(words, {0x3e000000u}, object);
        };
        const size_t offsetBytes = 8 * sizeof(float);

        auto countsFor = [](const std::string& body, uint32_t offset, const char* other,
                            const char* same, const char* different) {
            // The offset's own entry in the report, found by its offset rather than by its
            // position: the table is ordered by how many assemblies held each offset, so a test
            // that read the first entry would be testing the fixture's luck.
            const std::string marker = "\"offset\":" + std::to_string(offset);
            const size_t at = body.find(marker);
            if (at == std::string::npos) {
                return std::string("no entry at offset ") + std::to_string(offset);
            }
            const size_t brace = body.find('{', at);
            const size_t end = body.find('}', brace);
            const std::string entry = body.substr(brace, end - brace);
            return std::string(other) + "=" + nested(entry, other) + " " + same + "=" +
                   nested(entry, same) + " " + different + "=" + nested(entry, different);
        };

        {
            ObjectPoseLocator locator;
            for (int frame = 0; frame < 3; frame++) {
                for (uint32_t object = 1; object <= 6; object++) {
                    // The same twelve floats for every object in a frame, and a different set each
                    // frame: shared across objects, moving over time, which is the whole of it.
                    locator.onAssemblyRecorded(
                        globalFrame(object, 0.61f + 0.2f * static_cast<float>(frame)));
                }
            }
            const std::string body = locator.json();
            const std::string counts =
                countsFor(body, offsetBytes, "otherIdentities", "otherIdentitiesSameValue",
                          "otherIdentitiesDifferentValue");
            // **The offset the test wrote is the offset the report carries, found by its offset and
            // not by its position.** The table is ordered by how many assemblies held each offset,
            // so a test that read the first entry would be testing its own luck rather than the
            // locator. `countsFor` returns empty when no entry sits at that offset, and every
            // assertion below reads through it, so this is the check that they are about the right
            // one rather than about a mixture of offsets.
            check::isTrue(mentions(counts, "otherIdentities="),
                          "and the report carries an entry at the offset the test wrote to: " +
                              counts);
            // **Five, not fifteen**: six distinct objects over three frames, and a repeat of an
            // object is not a new object -- so five comparisons, all agreeing. The three frames are
            // what make the value move; the six objects are what make it shared, and it is the
            // sharing the question is about.
            check::isTrue(mentions(counts, "otherIdentities=5") &&
                              mentions(counts, "otherIdentitiesSameValue=5") &&
                              mentions(counts, "otherIdentitiesDifferentValue=0"),
                          "six objects over three frames, and the same value in every comparison, "
                          "which is what a camera's view matrix looks like -- " +
                              counts);
            check::isTrue(nested(body, "identitySamplesCapped") == "no",
                          "and six identities is under the bound, so nothing was capped and the "
                          "five is all of them: " +
                              nested(body, "identitySamplesCapped"));
        }
        {
            // The per-object case over the same frame loop, so the two fixtures differ only in
            // whether the value is shared: a pair that differed in shape as well as in value
            // would let a counter that only noticed the shape pass both.
            ObjectPoseLocator locator;
            for (int frame = 0; frame < 3; frame++) {
                for (uint32_t object = 1; object <= 6; object++) {
                    locator.onAssemblyRecorded(
                        perObjectValue(object + static_cast<uint32_t>(frame) * 10));
                }
            }
            const std::string body = locator.json();
            const std::string counts =
                countsFor(body, offsetBytes, "otherIdentities", "otherIdentitiesSameValue",
                          "otherIdentitiesDifferentValue");
            // Fifteen, not five: the frame loop makes eighteen distinct objects and the identity
            // sample is capped at sixteen, so fifteen comparisons is every one the cap allows.
            // **And the cap is reported**, so "compared against fifteen of eighteen" is visible
            // rather than reading as "compared against all of them".
            check::isTrue(mentions(counts, "otherIdentities=15") &&
                              mentions(counts, "otherIdentitiesSameValue=0") &&
                              mentions(counts, "otherIdentitiesDifferentValue=15"),
                          "and the same frame loop with a different value in every one of fifteen "
                          "comparisons, which is what a prop's world matrix looks like -- " +
                              counts);
            check::isTrue(nested(body, "identitySamplesCapped") == "yes",
                          "and the identity sample says it was capped, so the fifteen is the bound "
                          "and not the field's opinion: " +
                              nested(body, "identitySamplesCapped"));
        }
        {
            // **The distinct-identity count is a measurement.** It was `= 1` on the candidate's
            // first appearance and never touched again, so every run reported `identities: 1` at
            // every offset and a reader sorting offsets by it sorted them by nothing. A field that
            // is a constant cannot be told from a field that is telling the truth by reading it --
            // only by looking at two offsets in the same report and seeing whether they differ.
            auto twoObjects = [](uint32_t object, float spin) {
                std::vector<float> words(64, 0.0f);
                putPose(words, 8, spin);
                return assembly(words, {0x3e000000u}, object);
            };
            ObjectPoseLocator locator;
            for (int round = 0; round < 40; round++) {
                locator.onAssemblyRecorded(twoObjects(0x1000u, 0.10f * static_cast<float>(round)));
                locator.onAssemblyRecorded(twoObjects(0x2000u, 0.20f * static_cast<float>(round)));
            }
            const std::string body = locator.json();
            const std::string marker = "\"offset\":" + std::to_string(8 * sizeof(float));
            const size_t at = body.find(marker);
            check::isTrue(at != std::string::npos, "the offset is in the report: " + body);
            // The entry is read up to the next offset's marker rather than to a brace: the report
            // nests, and a window closed on the wrong brace reads a field that is not there as one
            // that is absent -- which is a different failure with the same symptom.
            const size_t next = body.find("\"offset\":", at + 1);
            const std::string entry =
                body.substr(at, (next == std::string::npos ? body.size() : next) - at);
            // Two objects each held it, so the count is two. A constant of one fails here and a
            // counter that only ever fires once fails here too.
            check::isTrue(mentions(entry, "\"identities\":2"),
                          "two objects held this offset, so the report says two and not one: " +
                              entry);
        }
        {
            // **The case that tells "compared against the last value" from "compared against the
            // first".** Three objects in a row holding A, B, B. Compared against the value just
            // seen, the second differs and the third agrees -- one difference and one agreement.
            // Compared against the first value ever, both differ, and the answer is a different
            // count of the same data.
            //
            // **This is the only fixture that can tell them apart**, and the two above cannot: they
            // compare every object against a value that is either always equal or never equal, so
            // advancing the retained value or not gives the same number either way. A mutation that
            // stopped the retained value advancing passed both of them, which is how this one was
            // found.
            ObjectPoseLocator locator;
            locator.onAssemblyRecorded(globalFrame(1, 0.31f)); // A
            locator.onAssemblyRecorded(globalFrame(2, 0.52f)); // B, differs from A
            locator.onAssemblyRecorded(globalFrame(3, 0.52f)); // B, agrees with the one before
            locator.onAssemblyRecorded(globalFrame(4, 0.73f)); // C, differs again
            const std::string counts =
                countsFor(locator.json(), offsetBytes, "otherIdentities",
                          "otherIdentitiesSameValue", "otherIdentitiesDifferentValue");
            check::isTrue(mentions(counts, "otherIdentities=3") &&
                              mentions(counts, "otherIdentitiesSameValue=1") &&
                              mentions(counts, "otherIdentitiesDifferentValue=2"),
                          "A, B, B, C against the value just seen is one agreement and two "
                          "differences, which is the count the last-value rule gives and the "
                          "first-value rule does not -- " +
                              counts);
        }
        {
            // **A repeat of one object is not a second object.** Without this, a per-object pose
            // would be counted as differing from itself on every frame after the first, and the
            // "different" column would be a frame count wearing a disguise.
            ObjectPoseLocator locator;
            for (int repeat = 0; repeat < 20; repeat++) {
                locator.onAssemblyRecorded(perObjectValue(1));
            }
            const std::string counts = countsFor(locator.json(), offsetBytes, "otherIdentities",
                                                 "sameValue", "differentValue");
            check::isTrue(mentions(counts, "otherIdentities=0"),
                          "twenty assemblies of one object compare it against nobody: " + counts);
        }
    }
}
