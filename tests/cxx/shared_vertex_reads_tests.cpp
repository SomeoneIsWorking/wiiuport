#include "check.h"
#include "suites.h"
#include "wiiuport/interp/SharedVertexReads.h"

#include <cstdint>
#include <vector>

using wiiuport::interp::groupVertexReads;
using wiiuport::interp::VertexRead;
using wiiuport::interp::VertexReadGroups;

namespace {

void meshesReadingTheirOwnBytesAreGroupsOfTheirOwn() {
    VertexReadGroups groups = groupVertexReads(
        {{.begin = 0x1000, .size = 0x100, .mesh = 0}, {.begin = 0x1100, .size = 0x100, .mesh = 1}},
        2);
    check::equal(groups.groupOf[0], uint32_t{0}, "the first mesh is its own group");
    check::equal(groups.groupOf[1], uint32_t{1}, "and so is the one right after it");
    check::equal(groups.readUnredirected[0], uint8_t{0}, "which only the vertex blend reads");
}

void aMeshWhoseBytesAnotherReadsFromPartWayInIsOneGroupWithIt() {
    // The hair reads positions, normals and texture coordinates; its outline
    // only positions and normals, from part way into the same buffer.
    VertexReadGroups groups = groupVertexReads({{.begin = 0x5000, .size = 0x10, .mesh = 0},
                                                {.begin = 0x1000, .size = 0x300, .mesh = 1},
                                                {.begin = 0x1200, .size = 0x40, .mesh = 2}},
                                               3);
    check::equal(groups.groupOf[2], uint32_t{1}, "the outline is the hair's group");
    check::equal(groups.groupOf[1], uint32_t{1}, "named by the hair, the lower mesh");
    check::equal(groups.groupOf[0], uint32_t{0}, "and a mesh elsewhere is a group of its own");
}

void meshesJoinedThroughAThirdAreOneGroup() {
    // One mesh reads two buffers, each shared with another mesh.
    VertexReadGroups groups = groupVertexReads({{.begin = 0x1000, .size = 0x100, .mesh = 0},
                                                {.begin = 0x1000, .size = 0x100, .mesh = 1},
                                                {.begin = 0x2000, .size = 0x100, .mesh = 1},
                                                {.begin = 0x2000, .size = 0x80, .mesh = 2}},
                                               3);
    check::equal(groups.groupOf[2], uint32_t{0},
                 "the third is the first's group through the second");
}

void aGroupADrawTheReplayCannotRedirectReadsIsMarked() {
    VertexReadGroups groups = groupVertexReads({{.begin = 0x1000, .size = 0x100, .mesh = 0},
                                                {.begin = 0x3000, .size = 0x100, .mesh = 0},
                                                {.begin = 0x3000, .size = 0x100, .mesh = 1},
                                                {.begin = 0x1040, .size = 0x10},
                                                {.begin = 0x9000, .size = 0x10, .mesh = 2}},
                                               3);
    check::equal(groups.readUnredirected[1], uint8_t{1},
                 "a mesh whose group a draw of the title's bytes reads is marked");
    check::equal(groups.readUnredirected[0], uint8_t{1}, "as is the mesh it read");
    check::equal(groups.readUnredirected[2], uint8_t{0}, "and a mesh elsewhere is not");
}

} // namespace

namespace wiiuport::tests {

void runSharedVertexReadsTests() {
    meshesReadingTheirOwnBytesAreGroupsOfTheirOwn();
    aMeshWhoseBytesAnotherReadsFromPartWayInIsOneGroupWithIt();
    meshesJoinedThroughAThirdAreOneGroup();
    aGroupADrawTheReplayCannotRedirectReadsIsMarked();
}

} // namespace wiiuport::tests
