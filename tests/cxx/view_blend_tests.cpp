// The camera's view, blended on the in-between paint at the register file.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/ViewBlend.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <span>

namespace {

using wiiuport::title::CommandPosition;
using wiiuport::title::ViewBlend;

using Registers = std::array<uint32_t, ViewBlend::kWords>;

void noRegistration(uint32_t /*entry*/, uint32_t /*firstInstruction*/,
                    GuestCallProbes::Probe& /*probe*/, bool /*holdsEntry*/, uint32_t /*resume*/) {
}

// The display thread's side: contexts with their uploaded flag, GX2's write pointer and the paint.
class Guest {
  public:
    static constexpr uintptr_t kBufferStart = 0x10000;
    static constexpr uintptr_t kBufferEnd = 0x20000;
    static constexpr uint32_t kContext = 0x3b740520;
    static constexpr uint32_t kOtherContext = 0x3b758f08;
    static constexpr uint32_t kVertexView = 0x400 + 16;

    std::map<uint32_t, uint8_t> uploadedFlags{{kContext, 0}, {kOtherContext, 0}};
    uintptr_t write = kBufferStart;
    bool inBetween = false;

    ViewBlend blend() {
        return ViewBlend{&noRegistration,
                         [this](uint32_t address, uint32_t /*size*/) -> const void* {
                             auto flag = uploadedFlags.find(address - ViewBlend::kUploadedFlag);
                             return flag == uploadedFlags.end() ? nullptr : &flag->second;
                         },
                         [this] {
                             return CommandPosition{kBufferStart, kBufferEnd, write};
                         },
                         [this] {
                             return inBetween;
                         }};
    }

    // The title calls uploadView(context); returns the packet the upload will be executed as.
    uintptr_t upload(ViewBlend& blend, uint32_t context) {
        std::array<uint32_t, 32> gpr{};
        gpr[ViewBlend::kContextRegister] = context;
        blend.OnCall(gpr, 0);
        uintptr_t packet = write + ViewBlend::kPacketHeaderBytes;
        write += sizeof(uint32_t) * (2 + ViewBlend::kWords);
        return packet;
    }
};

Registers view(float translation) {
    Registers words{};
    std::array<float, ViewBlend::kWords> rows{1, 0, 0, translation, 0, 1, 0, 2, 0, 0, 1, 3};
    for (size_t i = 0; i < words.size(); i++) {
        words[i] = std::bit_cast<uint32_t>(rows[i]);
    }
    return words;
}

float wordAt(const Registers& words, size_t index) {
    return std::bit_cast<float>(words[index]);
}

// The Latte thread executes the packet: its values reach the register file.
bool execute(ViewBlend& blend, uintptr_t packet, Registers& words) {
    return blend.onAluConstants({packet, Guest::kVertexView, words.data(), ViewBlend::kWords});
}

void theInBetweenPaintGetsTheMidpointOfTwoTicks() {
    Guest guest;
    ViewBlend blend = guest.blend();
    Registers tickOne = view(10.0f);
    execute(blend, guest.upload(blend, Guest::kContext), tickOne);
    guest.inBetween = true;
    Registers between = view(20.0f);
    const bool rewritten = execute(blend, guest.upload(blend, Guest::kContext), between);
    check::isTrue(rewritten, "the in-between paint's view is rewritten");
    check::near(wordAt(between, 3), 15.0f, 0.0f, "to the midpoint of tick N-1's and tick N's");
    check::near(wordAt(between, 0), 1.0f, 0.0f, "and the rest of the matrix is the title's");
    guest.inBetween = false;
    Registers own = view(20.0f);
    check::isTrue(!execute(blend, guest.upload(blend, Guest::kContext), own) &&
                      wordAt(own, 3) == 20.0f,
                  "the tick's own paint is left as the title wrote it");
    check::isTrue(tickOne[3] == view(10.0f)[3], "and the own paint before was not touched either");
    const ViewBlend::Report r = blend.report();
    check::isTrue(r.calls == 3 && r.matched == 3 && r.held == 2 && r.lerped == 1,
                  "three uploads matched: two held, one lerped");
    check::isTrue(r.moved == 1, "and the lerp is counted as a move");
}

void aStillCameraIsLerpedButDoesNotMove() {
    Guest guest;
    ViewBlend blend = guest.blend();
    Registers tickOne = view(10.0f);
    execute(blend, guest.upload(blend, Guest::kContext), tickOne);
    guest.inBetween = true;
    Registers between = view(10.0f);
    execute(blend, guest.upload(blend, Guest::kContext), between);
    const ViewBlend::Report r = blend.report();
    check::isTrue(r.lerped == 1 && r.moved == 0 && wordAt(between, 3) == 10.0f,
                  "the same view at both ticks is its own midpoint and no move");
}

// The paint advances on the display thread while Latte is still executing the paint before, so the
// parity is the one the probe saw, not the one current when the packet executes.
void theParityIsTheProbesNotTheExecutors() {
    Guest guest;
    ViewBlend blend = guest.blend();
    Registers tickOne = view(10.0f);
    execute(blend, guest.upload(blend, Guest::kContext), tickOne);
    guest.inBetween = true;
    const uintptr_t inBetweenPacket = guest.upload(blend, Guest::kContext);
    guest.inBetween = false;
    const uintptr_t ownPacket = guest.upload(blend, Guest::kContext);
    Registers between = view(20.0f);
    Registers own = view(20.0f);
    execute(blend, inBetweenPacket, between);
    execute(blend, ownPacket, own);
    check::near(wordAt(between, 3), 15.0f, 0.0f,
                "a packet written on the in-between paint is blended after the paint moved on");
    check::near(wordAt(own, 3), 20.0f, 0.0f, "and one written on the own paint is not");
}

void theFirstInBetweenHasNothingToBlendFrom() {
    Guest guest;
    ViewBlend blend = guest.blend();
    guest.inBetween = true;
    Registers between = view(20.0f);
    check::isTrue(!execute(blend, guest.upload(blend, Guest::kContext), between) &&
                      wordAt(between, 3) == 20.0f,
                  "with no tick before, the view is left alone");
    check::isTrue(blend.report().firstSight == 1, "and counted as first sight");
}

void contextsAreHeldSeparately() {
    Guest guest;
    ViewBlend blend = guest.blend();
    Registers one = view(10.0f);
    Registers other = view(100.0f);
    execute(blend, guest.upload(blend, Guest::kContext), one);
    execute(blend, guest.upload(blend, Guest::kOtherContext), other);
    guest.inBetween = true;
    Registers between = view(20.0f);
    Registers otherBetween = view(200.0f);
    execute(blend, guest.upload(blend, Guest::kContext), between);
    execute(blend, guest.upload(blend, Guest::kOtherContext), otherBetween);
    check::near(wordAt(between, 3), 15.0f, 0.0f, "each context blends from its own last view");
    check::near(wordAt(otherBetween, 3), 150.0f, 0.0f, "the other from its own");
    check::isTrue(blend.report().contexts == 2, "two contexts held");
}

void anUploadTheProgramAlreadyHasSendsNoPacket() {
    Guest guest;
    ViewBlend blend = guest.blend();
    guest.uploadedFlags[Guest::kContext] = 1;
    std::array<uint32_t, 32> gpr{};
    gpr[ViewBlend::kContextRegister] = Guest::kContext;
    blend.OnCall(gpr, 0);
    // The next packet at that position is someone else's constants.
    Registers other = view(5.0f);
    check::isTrue(!execute(blend, guest.write + ViewBlend::kPacketHeaderBytes, other),
                  "with the uploaded flag set nothing is expected at the write position");
    const ViewBlend::Report r = blend.report();
    check::isTrue(r.alreadyUploaded == 1 && r.matched == 0, "and the call is counted as such");
}

void otherConstantsAreLeftAlone() {
    Guest guest;
    ViewBlend blend = guest.blend();
    Registers material = view(7.0f);
    check::isTrue(!execute(blend, Guest::kBufferStart + 0x400, material) &&
                      wordAt(material, 3) == 7.0f,
                  "a packet no upload named is not the view's");
    check::isTrue(blend.report().matched == 0, "and matches nothing");
}

void aPacketOfAnotherSizeIsNotTheView() {
    Guest guest;
    ViewBlend blend = guest.blend();
    const uintptr_t packet = guest.upload(blend, Guest::kContext);
    std::array<uint32_t, 16> words{};
    check::isTrue(!blend.onAluConstants({packet, Guest::kVertexView, words.data(), 16}),
                  "sixteen words where twelve were expected are left alone");
    check::isTrue(blend.report().wrongSize == 1, "and counted");
}

void anUnblendableViewIsLeftAlone() {
    Guest guest;
    ViewBlend blend = guest.blend();
    Registers tickOne = view(10.0f);
    tickOne[5] = std::bit_cast<uint32_t>(std::numeric_limits<float>::quiet_NaN());
    execute(blend, guest.upload(blend, Guest::kContext), tickOne);
    guest.inBetween = true;
    Registers between = view(20.0f);
    check::isTrue(!execute(blend, guest.upload(blend, Guest::kContext), between) &&
                      wordAt(between, 3) == 20.0f,
                  "a held view with a NaN is not blended from");
    check::isTrue(blend.report().refusedUnblendable == 1, "and the refusal is counted");
}

void anUploadOutsideABufferIsCounted() {
    Guest guest;
    ViewBlend blend = guest.blend();
    guest.write = 0;
    std::array<uint32_t, 32> gpr{};
    gpr[ViewBlend::kContextRegister] = Guest::kContext;
    blend.OnCall(gpr, 0);
    check::isTrue(blend.report().withoutBuffer == 1, "no command buffer open, nothing expected");
}

} // namespace

void wiiuport::tests::runViewBlendTests() {
    theInBetweenPaintGetsTheMidpointOfTwoTicks();
    aStillCameraIsLerpedButDoesNotMove();
    theParityIsTheProbesNotTheExecutors();
    theFirstInBetweenHasNothingToBlendFrom();
    contextsAreHeldSeparately();
    anUploadTheProgramAlreadyHasSendsNoPacket();
    otherConstantsAreLeftAlone();
    aPacketOfAnotherSizeIsNotTheView();
    anUnblendableViewIsLeftAlone();
    anUploadOutsideABufferIsCounted();
}
