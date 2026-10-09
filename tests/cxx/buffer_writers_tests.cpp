#include "check.h"
#include "suites.h"
#include "wiiuport/guest/BufferWriters.h"
#include "wiiuport/interp/GuestObject.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using wiiuport::guest::BufferWriters;
using wiiuport::interp::GuestObject;

namespace wiiuport::tests {

namespace {

// A quad as the title's particle writers leave it, one byte at a time so the
// test does not depend on the value, only on the bytes being what was written.
BufferWriters::Leading quadBytes(uint8_t seed) {
    BufferWriters::Leading leading{};
    for (size_t index = 0; index < leading.size(); ++index) {
        leading[index] = static_cast<std::byte>(seed + static_cast<uint8_t>(index));
    }
    return leading;
}

std::vector<std::byte> bytesOf(const BufferWriters::Leading& leading) {
    return {leading.begin(), leading.end()};
}

// The buffers the tests write into, named so a reader can tell one address from another, and
// declared here rather than inside a function: a named constant in a function body is a second
// place to look for a number that the type already carries.
constexpr int kWrittenBuffer = 1;
constexpr int kNeverWrittenBuffer = 2;

} // namespace

// **The writer's own behaviour, which the retired vertex blend's tests covered only as a
// fixture.** A draw from a buffer a particle wrote, reading the bytes it wrote there, is that
// particle; anything else is refused, and both answers are counted with a denominator so a
// refusal cannot be told from a table that was never filled.
void aDrawFromABufferAWriterUsedIsTheObjectItWroteThere() {
    BufferWriters writers;
    int source = kWrittenBuffer;
    GuestObject particle{0x0274a100, 3.0f};
    auto leading = quadBytes(0x40);
    check::equal(writers.calls(), uint64_t{0}, "nothing is recorded before a writer runs");

    writers.record(&source, particle, leading);
    check::equal(writers.calls(), uint64_t{1}, "the write is counted");
    // A recording and an installation are different things: a probe being installed is what makes
    // a recording possible, so the two counters are kept apart and asserted apart.
    check::equal(writers.installed(), 0u, "a recording does not count as an installed probe");
    writers.noteInstalled();
    check::equal(writers.installed(), 1u, "and the installed count is the probes' own");
    check::equal(writers.calls(), uint64_t{1}, "which recorded nothing on its own");

    auto drawn = writers.written(&source, bytesOf(leading));
    check::isTrue(drawn.has_value(), "the draw from that buffer is identified");
    check::isTrue(drawn.has_value() && drawn->object == particle,
                  "and it is the object that wrote there");
    check::equal(writers.identified(), uint64_t{1}, "one draw identified");
    check::equal(writers.rewritten(), uint64_t{0}, "and none refused as rewritten");
}

// A buffer handed to another object's draw since is the one refusal the class has: the bytes no
// longer say what was written there, so naming the writer would be a guess.
void aDrawWhoseBytesAreNotWhatTheWriterLeftIsRefusedAndCounted() {
    BufferWriters writers;
    int source = kWrittenBuffer;
    GuestObject particle{0x0274a100, 3.0f};
    auto leading = quadBytes(0x40);
    writers.record(&source, particle, leading);

    auto other = quadBytes(0x90);
    auto drawn = writers.written(&source, bytesOf(other));
    check::isTrue(!drawn.has_value(), "a draw reading other bytes is not named");
    check::equal(writers.rewritten(), uint64_t{1}, "and is counted as rewritten");
    check::equal(writers.identified(), uint64_t{0}, "no draw is identified in its place");
}

// The negative first, and both of its faces: a buffer nothing wrote, and a draw too short to be
// checked. Neither may be answered from the table's other rows.
void aDrawFromAnUnknownOrShortBufferIsRefusedWithoutNamingAnObject() {
    BufferWriters writers;
    int written = kWrittenBuffer;
    int never = kNeverWrittenBuffer;
    writers.record(&written, GuestObject{0x0274a100, 3.0f}, quadBytes(0x40));

    check::isTrue(!writers.written(&never, bytesOf(quadBytes(0x40))).has_value(),
                  "a buffer no writer used names nothing");
    check::isTrue(!writers.written(nullptr, bytesOf(quadBytes(0x40))).has_value(),
                  "and a null source names nothing either");
    check::equal(writers.rewritten(), uint64_t{0}, "an unwritten buffer is not a rewritten one");

    // A draw of fewer bytes than the check reads cannot be shown to match, so it is refused; the
    // class counts it as rewritten because that is the refusal's reason, not because a writer
    // changed anything.
    auto shortDraw = std::vector<std::byte>(BufferWriters::kLeadingBytes - 1, std::byte{0x40});
    check::isTrue(!writers.written(&written, shortDraw).has_value(),
                  "a draw too short to check names nothing");
    check::equal(writers.rewritten(), uint64_t{1}, "and the refusal is counted with its reason");
    check::equal(writers.identified(), uint64_t{0}, "nothing was identified from it");

    // A write whose object or buffer could not be read is counted separately from a write that
    // was recorded: the two have different causes, and a table that only counts the second would
    // hide a probe reading the wrong address.
    writers.noteUnreadable();
    check::equal(writers.calls(), uint64_t{2}, "an unreadable write is still a call");
    check::equal(writers.unreadable(), uint64_t{1}, "and is counted as unreadable");
    // A write that could not be read records nothing, and so cannot displace what a reader did
    // record: a probe reading the wrong address must not make every later draw unidentifiable.
    check::isTrue(writers.written(&written, bytesOf(quadBytes(0x40))).has_value(),
                  "so the good record still names its object after an unreadable write");
    check::equal(writers.identified(), uint64_t{1}, "and that draw is identified");
}

// Each write carries the same object's write before it: the tick before's quad, in the object's
// other buffer.
void aWriteCarriesTheObjectsWriteBefore() {
    BufferWriters writers;
    int first = kWrittenBuffer;
    int second = kNeverWrittenBuffer;
    writers.record(&first, GuestObject{0x0274a100, 3.0f}, quadBytes(0x40));
    writers.record(&second, GuestObject{0x0274a100, 4.0f}, quadBytes(0x50));

    auto earlier = writers.written(&first, bytesOf(quadBytes(0x40)));
    check::isTrue(earlier.has_value() && !earlier->before.has_value(),
                  "the object's first write has nothing before it");
    auto later = writers.written(&second, bytesOf(quadBytes(0x50)));
    check::isTrue(later.has_value() && later->before == quadBytes(0x40),
                  "the next carries the same object's quad before it");
}

// A pool slot given to a new particle: the bytes before are another particle's.
void aRebornObjectHasNothingBefore() {
    BufferWriters writers;
    int first = kWrittenBuffer;
    int second = kNeverWrittenBuffer;
    writers.record(&first, GuestObject{0x0274a100, 9.0f}, quadBytes(0x40));
    writers.record(&second, GuestObject{0x0274a100, 1.0f}, quadBytes(0x50));
    auto reborn = writers.written(&second, bytesOf(quadBytes(0x50)));
    check::isTrue(reborn.has_value() && !reborn->before.has_value(),
                  "a younger object at the same address does not continue the older one");
}

void runBufferWritersTests() {
    aWriteCarriesTheObjectsWriteBefore();
    aRebornObjectHasNothingBefore();
    aDrawFromABufferAWriterUsedIsTheObjectItWroteThere();
    aDrawWhoseBytesAreNotWhatTheWriterLeftIsRefusedAndCounted();
    aDrawFromAnUnknownOrShortBufferIsRefusedWithoutNamingAnObject();
}

} // namespace wiiuport::tests
