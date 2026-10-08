// A vertex float component decoded in the byte order its fetch declares.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/VertexComponent.h"

#include <array>
#include <cstdint>

using wiiuport::title::VertexComponent;

void wiiuport::tests::runVertexComponentTests() {
    // 1.5f is 0x3fc00000.
    const std::array<uint8_t, 4> little{0x00, 0x00, 0xc0, 0x3f};
    const std::array<uint8_t, 4> big{0x3f, 0xc0, 0x00, 0x00};
    const std::array<uint8_t, 4> halves{0x00, 0x00, 0x3f, 0xc0};
    check::isTrue(VertexComponent::read(little.data(), VertexComponent::kSwapNone) == 1.5f,
                  "SWAP_NONE reads little-endian bytes as they are");
    check::isTrue(VertexComponent::read(big.data(), VertexComponent::kSwapU32) == 1.5f,
                  "SWAP_U32 reads a big-endian word");
    check::isTrue(VertexComponent::read(halves.data(), VertexComponent::kSwapU16) == 1.5f,
                  "SWAP_U16 swaps the bytes of each half");
}
