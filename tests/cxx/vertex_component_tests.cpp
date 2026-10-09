// A vertex float component decoded in the byte order its fetch declares.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/VertexComponent.h"

#include <array>
#include <cstdint>

using wiiuport::title::VertexComponent;

void wiiuport::tests::runVertexComponentTests() {
    // 1.5f is 0x3fc00000.
    std::array<uint8_t, 4> little{0x00, 0x00, 0xc0, 0x3f};
    std::array<uint8_t, 4> big{0x3f, 0xc0, 0x00, 0x00};
    std::array<uint8_t, 4> halves{0x00, 0x00, 0x3f, 0xc0};
    check::isTrue(VertexComponent(VertexComponent::kSwapNone).read(little.data()) == 1.5f,
                  "SWAP_NONE reads little-endian bytes as they are");
    check::isTrue(VertexComponent(VertexComponent::kSwapU32).read(big.data()) == 1.5f,
                  "SWAP_U32 reads a big-endian word");
    check::isTrue(VertexComponent(VertexComponent::kSwapU16).read(halves.data()) == 1.5f,
                  "SWAP_U16 swaps the bytes of each half");
    std::array<uint8_t, 4> written{};
    VertexComponent(VertexComponent::kSwapU32).write(written.data(), 1.5f);
    check::isTrue(written == big, "write puts a value back in the byte order it was read in");
    VertexComponent(VertexComponent::kSwapU16).write(written.data(), 1.5f);
    check::isTrue(written == halves, "for each mode");
}
