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

// A draw the way the title lays one out: one buffer of a stated stride, and an attribute table
// of stated entries. The numbers are a fixture, and the census's job is to say what it would
// make of whatever it is handed.
Prepared aDraw(uint32_t stride, uint32_t bufferBytes) {
    // Value-initialised, and it has to be: `DrawPrepared` is a plain aggregate with no default
    // member initialisers, so a default-constructed one carries whatever was on the stack in
    // `vertexAttributeCount` and `vertexBufferCount`. The first version of this fixture did
    // that, and the suite read 110 attributes out of a draw that declared none -- and named a
    // position correctly in another test purely by luck.
    Prepared draw{};
    draw.vertexBufferCount = 1;
    draw.vertexBuffers[0].data = nullptr;
    draw.vertexBuffers[0].sizeInBytes = bufferBytes;
    draw.vertexBuffers[0].stride = stride;
    draw.vertexBuffers[0].slot = 0;
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
