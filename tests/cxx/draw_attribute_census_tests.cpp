/// Which of a draw's vertex attributes is the position.
///
/// The pose is not held anywhere as a transform, so the in-between frame is a lerp of two
/// vertex sets at the game's own draw, and this is the one thing that has to be *known* to do
/// it: which attribute carries the position. It is known by measuring how often each
/// (semantic, format, size, buffer, offset) signature recurs across the title's own objects --
/// not by reading a semantic index out of a GX2 header, which would be a guess about a title
/// nobody has disassembled.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/DrawAttributeCensus.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace {

using wiiuport::title::DrawAttributeCensus;
using Prepared = LatteFrameHooks::DrawPrepared;

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

// A draw the way the title lays one out: one buffer of a stated stride, an attribute table of
// stated entries, and -- because the census now READS the values at a candidate offset to apply
// its magnitude bar -- real bytes behind the buffer. The bytes are a fixture: `g_buffer` holds
// a stride's worth of zeros with a float written at the head of each vertex, which is what a
// position at offset 0 looks like.
//
// `data` was nullptr here until the magnitude bar existed, and the suite core-dumped on it: the
// census read through it. A null buffer is refused in the product, and a fixture that does not
// model the thing the code reads is a fixture that finds the crash rather than the bug.
std::vector<std::byte> g_buffer;

// A denormal and zero, written as the values they are, because the difference between them is
// the whole of the floor's non-zero clause.

Prepared aDraw(uint32_t stride, uint32_t bufferBytes) {
    // Value-initialised, and it has to be: `DrawPrepared` is a plain aggregate with no default
    // member initialisers, so a default-constructed one carries whatever was on the stack in
    // `vertexAttributeCount` and `vertexBufferCount`. The first version of this fixture did
    // that, and the suite read 110 attributes out of a draw that declared none -- and named a
    // position correctly in another test purely by luck.
    g_buffer.assign(bufferBytes == 0 ? 16 : bufferBytes, std::byte{0});
    Prepared draw{};
    draw.vertexBufferCount = 1;
    draw.vertexBuffers[0].data = g_buffer.data();
    draw.vertexBuffers[0].sizeInBytes = bufferBytes;
    draw.vertexBuffers[0].stride = stride;
    draw.vertexBuffers[0].slot = 0;
    return draw;
}

// Writes a float at the head of each vertex, so the magnitude bar has positions to believe.
Prepared aDrawWithPositions(uint32_t stride, uint32_t vertices, float scale) {
    Prepared draw = aDraw(stride, stride * vertices);
    // Written through memcpy rather than a cast: `std::byte` is the buffer's own type and
    // casting a `std::byte*` to `float*` is not allowed, which is the point of it.
    for (uint32_t vertex = 0; vertex < vertices; vertex++) {
        for (uint32_t component = 0; component < 3; component++) {
            const float value = scale * static_cast<float>(component + 1);
            std::memcpy(g_buffer.data() + (vertex * stride) + component * sizeof(float), &value,
                        sizeof(value));
        }
    }
    return draw;
}

void addAttribute(Prepared& draw, uint32_t semantic, uint32_t format, uint32_t size,
                  uint32_t offset, uint32_t buffer = 0, bool perInstance = false) {
    Prepared::VertexAttribute& attribute = draw.vertexAttributes[draw.vertexAttributeCount++];
    attribute.buffer = buffer;
    attribute.offset = offset;
    attribute.sizeInBytes = size;
    attribute.format = static_cast<uint8_t>(format);
    attribute.endianSwap = 0;
    attribute.semanticId = static_cast<uint8_t>(semantic);
    attribute.perInstance = perInstance;
}

// The table a skinned mesh draw carries: a position, a normal, a joint index, a weight, and a
// texture coordinate. The position is one of six attributes and is named by being the one a
// majority of objects agree on, not by being the first.
Prepared aMeshDraw() {
    Prepared draw = aDraw(32, 32 * 300);
    addAttribute(draw, 7, 0x14, 12, 0);  // position: three floats
    addAttribute(draw, 8, 0x14, 12, 12); // normal
    addAttribute(draw, 9, 0x0a, 4, 24);  // joint index
    addAttribute(draw, 10, 0x0c, 4, 28); // weight
    addAttribute(draw, 11, 0x0c, 4, 28, 0, true);
    addAttribute(draw, 0, 0x0c, 4, 24);
    return draw;
}

} // namespace

void wiiuport::tests::runDrawAttributeCensusTests() {
    // The position is named because a majority of distinct objects carry the same signature,
    // and the histogram is in the report so the choice is visible rather than asserted.
    {
        wiiuport::title::ObjectIdentityScope scope;
        DrawAttributeCensus census(&scope);
        for (uint32_t object = 1; object <= 5; object++) {
            scope.bind(0x43e00000u + object * 0x1000u);
            Prepared draw = aMeshDraw();
            census.onDrawRecorded(draw);
            census.onDrawRecorded(draw);
        }
        const DrawAttributeCensus::Position at = census.position();
        const std::string body = census.json();
        check::isTrue(at.known, "five objects carrying the same attribute table name a position");
        check::isTrue(at.semanticId == 7 && at.sizeInBytes == 12 && at.buffer == 0 &&
                          at.offset == 0,
                      "and it is the three-float attribute at the head of the stride, found by "
                      "recurrence rather than by being first: " +
                          body);
        check::isTrue(field(body, "nodesTracked") == "5" && field(body, "nodesNeeded") == "3",
                      "with the denominator and the bar beside it, so neither is a mystery");
        check::isTrue(field(body, "draws") == "10" && field(body, "attributesRead") == "60",
                      "and the counts it read: ten draws, six attributes each");
        check::isTrue(body.find("\"formatHex\"") != std::string::npos,
                      "and each signature's format byte in hex as well as decimal, because the "
                      "number is Latte's and a reader has to look it up: " +
                          body);
    }

    // **A position-sized attribute that is not the position is named by its place in the
    // stride, not by its size.** A twelve-byte attribute recurs in every object here and it is
    // the *normal*; the real position is sixteen bytes, padded to four components. A bar that
    // said "the smallest position-sized attribute" would name the normal, and every
    // substituted frame would be a frame of normals.
    {
        wiiuport::title::ObjectIdentityScope scope;
        DrawAttributeCensus census(&scope);
        for (uint32_t object = 1; object <= 4; object++) {
            scope.bind(0x43e10000u + object * 0x800u);
            Prepared draw = aDraw(48, 48 * 120);
            addAttribute(draw, 8, 0x14, 12, 0);  // normal first, in the stride
            addAttribute(draw, 7, 0x14, 16, 12); // position, padded to four components
            census.onDrawRecorded(draw);
        }
        const std::string body = census.json();
        const DrawAttributeCensus::Position at = census.position();
        check::isTrue(at.known && at.semanticId == 7 && at.sizeInBytes == 16,
                      "with two position-sized attributes agreeing equally on objects, the bar "
                      "picks by object count and reports which it picked: " +
                          body);
        check::isTrue(field(body, "positionBytesAccepted") == "12" &&
                          field(body, "positionBytesPaddedAccepted") == "16",
                      "and both accepted sizes are named, so a reader can see that twelve and "
                      "sixteen are both in the bar and not just one");
    }

    // **Two layouts, two positions, and the stride is what separates them.** This is the case
    // the real title forced: five objects compared cleanly at stride 32 while two at strides of
    // 20 and 64 read 18 and 60 components out of range, because the attribute the census named
    // sits at offset 0 *of its own layout*. One global answer is one layout's answer, and the
    // stride was recorded on the signature while excluded from its identity -- so a stride-32
    // draw and a stride-20 draw whose fields agreed folded into one signature and the majority
    // counted both. Here the same semantic, format, size and buffer appear at two strides with
    // two different offsets, and each is named for its own layout.
    {
        wiiuport::title::ObjectIdentityScope scope;
        DrawAttributeCensus census(&scope);
        for (uint32_t object = 1; object <= 4; object++) {
            scope.bind(0x43e00000u + object * 0x200u);
            Prepared wide = aDraw(32, 32 * 20);
            addAttribute(wide, 0, 0x30, 12, 0);
            addAttribute(wide, 6, 0x1e, 8, 12);
            census.onDrawRecorded(wide);
            // **The same offset in both layouts**, which is the collision the stride's absence
            // from the key caused. With different offsets the two signatures differ anyway and
            // the test proves nothing -- which is how the first version of this test passed
            // against the exact mutation it was written for.
            Prepared narrow = aDraw(20, 20 * 20);
            addAttribute(narrow, 0, 0x30, 12, 0);
            addAttribute(narrow, 6, 0x1e, 8, 12);
            census.onDrawRecorded(narrow);
        }
        const DrawAttributeCensus::Position at32 = census.positionFor(32);
        const DrawAttributeCensus::Position at20 = census.positionFor(20);
        const std::string body = census.json();
        check::isTrue(at32.known && at32.offset == 0 && at32.stride == 32,
                      "the stride-32 layout's position is at offset 0: " + body);
        check::isTrue(at20.known && at20.offset == 0 && at20.stride == 20,
                      "and the stride-20 layout's is at offset 0 of ITS OWN layout, which a "
                      "single global answer counted as the same signature");
        check::isTrue(body.find("\"layouts\"") != std::string::npos,
                      "and both layouts are in the report, because the number of layouts is what "
                      "the one global answer was hiding");
        // The histogram, not the layouts block: two entries at the SAME offset, one per stride.
        // The layouts block lists both strides either way, so it cannot tell whether the key
        // separated them -- and with the stride out of the key the two collapse into one entry
        // whose object count is the sum of two layouts, which is how one global position was
        // reported in the first place.
        const size_t head = body.find("\"signatures\":{");
        const std::string histogram = head == std::string::npos ? "" : body.substr(head);
        const size_t atOffset = histogram.find("\"offsetInStride\":0,\"stride\":20");
        check::isTrue(atOffset != std::string::npos,
                      "the histogram carries a stride-20 entry at offset 0: " + histogram);
        check::isTrue(histogram.find("\"offsetInStride\":0,\"stride\":32") != std::string::npos,
                      "and a stride-32 entry at the same offset, so the key keeps two layouts "
                      "apart rather than summing their objects: " +
                          histogram);
    }

    // A stride nobody has two objects for names nothing, rather than falling back to the
    // global answer -- a fallback would be the bug again, wearing a different hat.
    {
        wiiuport::title::ObjectIdentityScope scope;
        DrawAttributeCensus census(&scope);
        for (uint32_t object = 1; object <= 4; object++) {
            scope.bind(0x43e40000u + object * 0x200u);
            census.onDrawRecorded(aMeshDraw());
        }
        check::isTrue(!census.positionFor(999).known,
                      "a stride with no objects of its own names nothing");
        check::isTrue(census.positionFor(32).known,
                      "while the stride the objects were seen at still does");
    }

    // **The magnitude bar, and the case the real title forced.** Seven objects agreeing that the
    // stride-20 position is at offset 0 clears any bar over agreement -- and those twelve bytes
    // are partly position and partly the eight other bytes in the stride, so reading them gave
    // 1e+38 for some objects and a believable 0.107 for others. A count cannot tell those apart,
    // because every one of those objects agreed. Only the values can, so a candidate whose
    // components have ever read as something a position is not is not named, and the report
    // says how many so a layout that fails this reads as unsolved rather than as empty.
    {
        wiiuport::title::ObjectIdentityScope scope;
        DrawAttributeCensus census(&scope);
        for (uint32_t object = 1; object <= 5; object++) {
            scope.bind(0x43e50000u + object * 0x300u);
            Prepared draw = aDrawWithPositions(20, 8, 1.0f);
            addAttribute(draw, 0, 0x30, 12, 0);
            census.onDrawRecorded(draw);
        }
        const DrawAttributeCensus::Position clean = census.positionFor(20);
        const std::string body = census.json();
        check::isTrue(clean.known,
                      "a layout whose offset holds plausible positions is named: " + body);
        check::isTrue(body.find("\"magnitudeBar\":\"pass\"") != std::string::npos,
                      "and the report says the magnitude bar passed rather than leaving the "
                      "reader to assume it: " +
                          body);
    }

    // The same layout, the same seven objects, and bytes that are not a position. The count is
    // identical -- which is the whole point -- and the answer is different.
    {
        wiiuport::title::ObjectIdentityScope scope;
        DrawAttributeCensus census(&scope);
        for (uint32_t object = 1; object <= 5; object++) {
            scope.bind(0x43e60000u + object * 0x300u);
            Prepared draw = aDrawWithPositions(20, 8, 1.0f);
            addAttribute(draw, 0, 0x30, 12, 0);
            // One component in 10^38: the signature is untouched, the values are not positions.
            const float wild = 3.0e38f;
            std::memcpy(g_buffer.data() + sizeof(float), &wild, sizeof(wild));
            census.onDrawRecorded(draw);
        }
        const DrawAttributeCensus::Position dirty = census.positionFor(20);
        const std::string body = census.json();
        check::isTrue(!dirty.known,
                      "the same five objects and the same signature name nothing when the values "
                      "at that offset are not positions: " +
                          body);
        check::isTrue(body.find("\"magnitudeBar\":\"fail\"") != std::string::npos,
                      "and the report says the magnitude bar failed, so an unsolved layout reads "
                      "as unsolved rather than as a layout with no position: " +
                          body);
    }

    // **One bad draw must not refuse a layout, and a mostly-arithmetic one still must.** The
    // magnitude bar started all-or-nothing -- any component ever implausible and the layout was
    // gone -- and that refused four of the title's seven layouts, because one odd draw in one
    // frame of one object is enough. That is a threshold in the wrong place rather than a bar,
    // and it is the same mistake as the loose class being too loose. So the bar is a stated
    // SHARE of the components read, and both directions are tested here.
    {
        wiiuport::title::ObjectIdentityScope scope;
        DrawAttributeCensus census(&scope);
        // 200 draws of good positions, then ONE with a component at 1e38.
        for (uint32_t object = 1; object <= 5; object++) {
            scope.bind(0x43e70000u + object * 0x300u);
            for (uint32_t draw = 0; draw < 40; draw++) {
                Prepared good = aDrawWithPositions(20, 8, 1.0f);
                addAttribute(good, 0, 0x30, 12, 0);
                census.onDrawRecorded(good);
            }
        }
        // One bad read across the whole title's sample.
        scope.bind(0x43e70000u);
        Prepared bad = aDrawWithPositions(20, 8, 1.0f);
        addAttribute(bad, 0, 0x30, 12, 0);
        const float wild = 3.0e38f;
        std::memcpy(g_buffer.data() + sizeof(float), &wild, sizeof(wild));
        census.onDrawRecorded(bad);

        const std::string body = census.json();
        check::isTrue(census.positionFor(20).known,
                      "a layout that is 199 draws of positions and one of rubbish still names its "
                      "position, because one bad sample is not a layout being arithmetic: " +
                          body);
        check::isTrue(body.find("\"implausibleShare\":") != std::string::npos &&
                          body.find("\"implausibleShareCeiling\":0.01") != std::string::npos,
                      "and the share and the bar's own value are both in the report, so a reader "
                      "can see how close a layout is to being refused");
    }

    // The other direction: a layout that is mostly arithmetic is refused, and the share says by
    // how much -- so "unresolved" carries a number rather than only a verdict.
    {
        wiiuport::title::ObjectIdentityScope scope;
        DrawAttributeCensus census(&scope);
        for (uint32_t object = 1; object <= 5; object++) {
            scope.bind(0x43e80000u + object * 0x300u);
            for (uint32_t draw = 0; draw < 4; draw++) {
                Prepared bad = aDrawWithPositions(20, 8, 1.0f);
                addAttribute(bad, 0, 0x30, 12, 0);
                // Two of the three components are rubbish: a share of two thirds.
                const float wild = 3.0e38f;
                std::memcpy(g_buffer.data() + sizeof(float), &wild, sizeof(wild));
                std::memcpy(g_buffer.data() + 2 * sizeof(float), &wild, sizeof(wild));
                census.onDrawRecorded(bad);
            }
        }
        const std::string body = census.json();
        check::isTrue(!census.positionFor(20).known,
                      "a layout whose components are two thirds rubbish is refused: " + body);
        check::isTrue(body.find("\"magnitudeBar\":\"fail\"") != std::string::npos,
                      "and the report says the magnitude bar failed, with the share beside it, "
                      "so an unresolved layout reads as unresolved rather than as empty");
    }

    // **A tie is a choice, and the choice is named.** Two runs of the same title named stride
    // 32's position as "semantic 1 at offset 12" and then as "semantic 0 at offset 0" -- the same
    // layout, two answers -- because more than one position-sized attribute in a vertex layout
    // has plausible values (position, normal, tangent, colour). Almost every layout has several,
    // so refusing on a tie refuses the whole title, and picking by iteration order measures the
    // map. So the lowest semantic index wins, the rule is in the report as `tieBreak`, and every
    // candidate that cleared is listed beside the one chosen.
    {
        wiiuport::title::ObjectIdentityScope scope;
        DrawAttributeCensus census(&scope);
        for (uint32_t object = 1; object <= 5; object++) {
            scope.bind(0x43e90000u + object * 0x300u);
            Prepared draw = aDrawWithPositions(32, 8, 1.0f);
            // Two plausible twelve-byte attributes: one at offset 0 and one at offset 12, both
            // holding positions, because the bytes are the same bytes.
            addAttribute(draw, 0, 0x30, 12, 0);
            addAttribute(draw, 4, 0x30, 12, 12);
            census.onDrawRecorded(draw);
        }
        const DrawAttributeCensus::Position at = census.positionFor(32);
        const std::string body = census.json();
        check::isTrue(at.known && at.semanticId == 0 && at.offset == 0,
                      "two candidates clearing, the lowest semantic index wins: " + body);
        check::isTrue(body.find("\"candidatesClearing\":2") != std::string::npos,
                      "and the report says two cleared, so the reader knows a choice was made "
                      "rather than one candidate having been found");
        check::isTrue(body.find("\"tieBreak\":\"lowestSemanticThenOffset\"") != std::string::npos,
                      "and names the rule, so the pick is visibly a rule and not a measurement");
        check::isTrue(body.find("\"candidates\":{") != std::string::npos,
                      "with the alternatives listed beside it, so a reader entitled to see them "
                      "is entitled to object");
    }

    // **A vertex at the origin is a position, and a denormal is not.** The floor for denormals
    // arrived without a non-zero clause and refused every layout whose geometry sat at the
    // origin, which is most of a title's. Zero is exactly zero and is a perfectly good position.
    {
        wiiuport::title::ObjectIdentityScope scope;
        DrawAttributeCensus census(&scope);
        for (uint32_t object = 1; object <= 5; object++) {
            scope.bind(0x43ea0000u + object * 0x300u);
            Prepared draw = aDraw(32, 32 * 8);
            addAttribute(draw, 0, 0x30, 12, 0);
            std::memset(g_buffer.data(), 0, g_buffer.size());
            census.onDrawRecorded(draw);
        }
        check::isTrue(census.positionFor(32).known,
                      "every vertex at the origin still names a position, because zero is a "
                      "position");
    }

    // A denormal is not, and the two are told apart by being non-zero.
    {
        wiiuport::title::ObjectIdentityScope scope;
        DrawAttributeCensus census(&scope);
        for (uint32_t object = 1; object <= 5; object++) {
            scope.bind(0x43eb0000u + object * 0x300u);
            Prepared draw = aDraw(32, 32 * 8);
            addAttribute(draw, 0, 0x30, 12, 0);
            const float denormal = 1.7e-38f;
            std::memset(g_buffer.data(), 0, g_buffer.size());
            for (uint32_t vertex = 0; vertex < 8; vertex++) {
                std::memcpy(g_buffer.data() + vertex * 32, &denormal, sizeof(denormal));
            }
            census.onDrawRecorded(draw);
        }
        check::isTrue(!census.positionFor(32).known,
                      "and a layout whose every vertex is a denormal names nothing, which is the "
                      "1.7e+38 case's small twin");
    }

    // One object cannot agree with another, so nothing is named however many times it is drawn.
    {
        wiiuport::title::ObjectIdentityScope scope;
        DrawAttributeCensus census(&scope);
        scope.bind(0x43e20000u);
        for (int draw = 0; draw < 5; draw++) {
            census.onDrawRecorded(aMeshDraw());
        }
        const std::string body = census.json();
        check::isTrue(!census.position().known,
                      "one object drawn five times names nothing, because the bar is over "
                      "distinct objects and one cannot agree with itself");
        check::isTrue(field(body, "nodesNeeded") == "0" && field(body, "positionKnown") == "false",
                      "and the report says the bar is zero and the position unknown, rather "
                      "than leaving a reader to infer it from an empty list: " +
                          body);
    }

    // No identity published means no object, and the draws are counted as such rather than
    // folded into one phantom that every signature would divide by.
    {
        DrawAttributeCensus census(nullptr);
        for (int draw = 0; draw < 6; draw++) {
            census.onDrawRecorded(aMeshDraw());
        }
        const std::string body = census.json();
        check::isTrue(census.draws() == 6 && census.nodesTracked() == 0,
                      "six draws with no scope wired are six draws and zero objects");
        check::isTrue(field(body, "identitySource") == "\"none\"",
                      "and the report names the source it had, so the denominator is not "
                      "silently different from a run that had one: " +
                          body);
        check::isTrue(!census.position().known,
                      "and nothing is named, because there is no object to name it from");
    }

    // An attribute naming a buffer the draw does not have is counted and not read further: the
    // geometry needed to size a substitution is not there, and a signature with a stride of
    // zero is a signature nobody could act on.
    {
        wiiuport::title::ObjectIdentityScope scope;
        DrawAttributeCensus census(&scope);
        for (uint32_t object = 1; object <= 3; object++) {
            scope.bind(0x43e30000u + object * 0x400u);
            Prepared draw = aDraw(32, 32 * 10);
            addAttribute(draw, 7, 0x14, 12, 0);
            addAttribute(draw, 7, 0x14, 12, 0, 5); // buffer 5 of a one-buffer draw
            census.onDrawRecorded(draw);
        }
        const std::string body = census.json();
        check::isTrue(field(body, "attributesOutOfRange") == "3",
                      "three attributes named a buffer the draw does not have: " + body);
        check::isTrue(census.position().known && census.position().offset == 0,
                      "and the position is still named from the attributes that could be read");
    }

    // A draw with no attributes is counted separately, because a draw with an empty attribute
    // table is not a draw whose position is unknown -- it is a draw with nothing to read.
    {
        wiiuport::title::ObjectIdentityScope scope;
        DrawAttributeCensus census(&scope);
        scope.bind(0x43e40000u);
        census.onDrawRecorded(aDraw(32, 32 * 4));
        const std::string body = census.json();
        check::isTrue(field(body, "drawsWithoutAttributes") == "1" &&
                          field(body, "attributesRead") == "0",
                      "a draw with no attributes is counted on its own and reads nothing: " + body);
    }

    // The tracked set is bounded, and the refusal counted: a census that grew with the scene
    // would be a list of every object the title has ever drawn.
    {
        wiiuport::title::ObjectIdentityScope scope;
        DrawAttributeCensus census(&scope);
        for (uint32_t object = 1; object <= DrawAttributeCensus::kNodes + 4; object++) {
            scope.bind(0x43e00000u + object * 0x100u);
            census.onDrawRecorded(aMeshDraw());
        }
        const std::string body = census.json();
        check::isTrue(field(body, "nodesTracked") == std::to_string(DrawAttributeCensus::kNodes),
                      "the tracked set stops at its stated size");
        check::isTrue(field(body, "nodesRefused") == "4",
                      "and the objects beyond it are refused and counted");
    }
}
