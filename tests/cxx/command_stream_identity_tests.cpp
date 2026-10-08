// Which object a draw belongs to, joined through the command stream rather than through time.
#include "check.h"
#include "command_stream.h"
#include "suites.h"
#include "wiiuport/title/CommandStreamIdentity.h"

#include <string>

namespace {

using wiiuport::tests::CommandStream;
using wiiuport::title::CommandPosition;
using wiiuport::title::CommandStreamIdentity;

// A position the test moves by hand, for buffers other than CommandStream's one.
class ManualPosition {
  public:
    CommandPosition at;

    CommandStreamIdentity identity() {
        return CommandStreamIdentity{[this] {
            return at;
        }};
    }
};

void nothingBoundNamesNothing() {
    CommandStream stream;
    check::isTrue(stream.identity().objectAt(stream.packet()) == 0,
                  "a packet in a stream with no bind is named by nothing");
    check::isTrue(stream.identity().objectAt(0) == 0, "and a draw outside a packet by nothing");
    const CommandStreamIdentity::Report r = stream.identity().report();
    check::isTrue(r.lookups == 2 && r.lookupsNamed == 0 && r.lookupsWithoutBuffer == 1 &&
                      r.lookupsWithoutPacket == 1,
                  "and both are counted by why");
}

void eachDrawIsNamedByTheBindWrittenBeforeIt() {
    CommandStream stream;
    stream.bind(0xa000u);
    const uintptr_t first = stream.packet();
    const uintptr_t second = stream.packet();
    check::isTrue(stream.identity().objectAt(first) == 0xa000u &&
                      stream.identity().objectAt(second) == 0xa000u,
                  "every packet after a bind belongs to it until the next bind");
}

// The defect this replaces: the binder runs while the guest writes the stream and the draw runs
// when Latte executes it, so by then later objects have been bound. A slot holding the last bind
// named every draw here 0xc000.
void drawsExecutedAfterLaterBindsKeepTheirOwnObject() {
    CommandStream stream;
    stream.bind(0xa000u);
    const uintptr_t a = stream.packet();
    stream.bind(0xb000u);
    const uintptr_t b = stream.packet();
    stream.bind(0xc000u);
    check::isTrue(stream.identity().objectAt(a) == 0xa000u,
                  "the first draw is the first object's after two more binds");
    check::isTrue(stream.identity().objectAt(b) == 0xb000u, "and the second the second's");
}

void aBindThatWroteNothingIsReplaced() {
    ManualPosition position;
    position.at = {0x1000, 0x2000, 0x1100};
    CommandStreamIdentity identity = position.identity();
    identity.bind(0xa000u);
    identity.bind(0xb000u);
    check::isTrue(identity.objectAt(0x1110) == 0xb000u,
                  "two binds at one write position: the packet after them is the later one's");
}

void aRestartedBufferForgetsItsOldBinds() {
    ManualPosition position;
    position.at = {0x1000, 0x2000, 0x1800};
    CommandStreamIdentity identity = position.identity();
    identity.bind(0xa000u);
    position.at.write = 0x1100;
    identity.bind(0xb000u);
    check::isTrue(identity.objectAt(0x1900) == 0xb000u,
                  "a buffer written from its start again does not keep the old stream's binds");
    check::isTrue(identity.report().buffersRestarted == 1, "and the restart is counted");
}

void buffersAreSeparate() {
    ManualPosition position;
    position.at = {0x1000, 0x2000, 0x1100};
    CommandStreamIdentity identity = position.identity();
    identity.bind(0xa000u);
    position.at = {0x4000, 0x5000, 0x4100};
    identity.bind(0xb000u);
    check::isTrue(identity.objectAt(0x1200) == 0xa000u && identity.objectAt(0x4200) == 0xb000u,
                  "a packet is named from its own buffer's binds");
    check::isTrue(identity.objectAt(0x3000) == 0 && identity.objectAt(0x4050) == 0,
                  "and nothing names a packet between buffers or before a buffer's first bind");
    const CommandStreamIdentity::Report r = identity.report();
    check::isTrue(r.lookupsWithoutBuffer == 1 && r.lookupsBeforeFirstBind == 1,
                  "each refusal counted by why");
}

void aReusedRangeDropsTheOldBuffer() {
    ManualPosition position;
    position.at = {0x1000, 0x2000, 0x1100};
    CommandStreamIdentity identity = position.identity();
    identity.bind(0xa000u);
    position.at = {0x1800, 0x2800, 0x1900};
    identity.bind(0xb000u);
    check::isTrue(identity.objectAt(0x1200) == 0,
                  "a chunk that overlaps an older one retires it rather than mixing streams");
    check::isTrue(identity.report().buffersDropped == 1, "and the drop is counted");
}

void aBindWithNoBufferIsCounted() {
    ManualPosition position;
    CommandStreamIdentity identity = position.identity();
    identity.bind(0xa000u);
    const CommandStreamIdentity::Report r = identity.report();
    check::isTrue(r.binds == 1 && r.bindsWithoutBuffer == 1 && r.buffersTracked == 0,
                  "a bind with no command buffer open records nothing and says so");
}

} // namespace

void wiiuport::tests::runCommandStreamIdentityTests() {
    nothingBoundNamesNothing();
    eachDrawIsNamedByTheBindWrittenBeforeIt();
    drawsExecutedAfterLaterBindsKeepTheirOwnObject();
    aBindThatWroteNothingIsReplaced();
    aRestartedBufferForgetsItsOldBinds();
    buffersAreSeparate();
    aReusedRangeDropsTheOldBuffer();
    aBindWithNoBufferIsCounted();
}
