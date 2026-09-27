/// The base the title's relative uniform-block offset is relative to.
///
/// A binding's descriptor entry names its block by a *relative* offset, and the census's own
/// `blockOf` says that taking that word for a length once "asked the product for a gigabyte and
/// the product died". The block's size is directly usable; the address is not, and the ring test
/// is a re-read of a place.
///
/// The base is measured rather than looked up: the fork's uniform assembly hands over the draw's
/// real guest block addresses in the same draw and immediately after the binder, so
/// `address - offset` is a candidate base, and the one the title actually uses is the one that
/// recurs.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/UniformBlockBase.h"

#include <string>
#include <vector>

namespace {

using wiiuport::title::UniformBlockBase;

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

// A draw's block addresses, as `(bufferId, physicalAddress)` pairs. The true block is the
// second pair; the first is a decoy that also contributes a candidate, which is the point.
std::vector<uint32_t> twoBlocksAt(uint32_t offset, uint32_t trueBase) {
    return {0, offset + trueBase + 0x100, 1, offset + trueBase};
}

} // namespace

void wiiuport::tests::runUniformBlockBaseTests() {
    // **A base that recurs is named, with its share.** The real base appears once per binding
    // whichever pairing produced it; the decoy's arithmetic does not repeat.
    {
        UniformBlockBase base;
        for (int round = 0; round < 20; round++) {
            base.publish(1, 0x40, 0x40);
            base.observe(twoBlocksAt(0x40, 0x1000));
        }
        uint64_t count = 0;
        const uint32_t best = base.bestBase(&count);
        const std::string body = base.json();
        check::isTrue(best == 0x1000, "a base that appears in every binding is named: " + body);
        check::isTrue(count == 20,
                      "with the count it was seen, which is 20 -- one per binding, so the decoy's "
                      "arithmetic cannot be what matched");
        check::isTrue(field(body, "baseShare") != "0",
                      "and its share, because a base is a property of a distribution and not of a "
                      "maximum");
    }

    // **A base that barely leads is not named.** The wrong pairings dominate a corpus that is
    // mostly noise, so the largest count alone is a guess with a number on it. This is the case
    // that decides it: a leader under the share is reported as null, with its share beside it, so
    // "not identified" carries a margin rather than being an absence.
    {
        UniformBlockBase base;
        // Every binding generates a candidate from a different address, so nothing repeats.
        for (int round = 0; round < 20; round++) {
            base.publish(1, 0x40, 0x40);
            base.observe({0, 0x100000u + static_cast<uint32_t>(round) * 0x10});
        }
        uint64_t count = 0;
        const uint32_t best = base.bestBase(&count);
        const std::string body = base.json();
        check::isTrue(best == 0 && field(body, "base") == "null",
                      "no base at all is reported as null, not as the best candidate seen: " +
                          body);
        check::isTrue(field(body, "candidates") == "20" && field(body, "distinctBases") == "20",
                      "with every candidate counted, so a null is a statement about a set rather "
                      "than an absence");
    }

    // A leader that is under the share is refused even though it leads, and its share is
    // reported so a reader can see how near it came.
    {
        UniformBlockBase base;
        for (int round = 0; round < 10; round++) {
            base.publish(1, 0x40, 0x40);
            // A tenth of the candidates are the real base; the rest are noise.
            base.observe(round == 0 ? twoBlocksAt(0x40, 0x2000)
                                    : std::vector<uint32_t>{
                                          0, 0x300000u + static_cast<uint32_t>(round) * 0x4});
        }
        uint64_t count = 0;
        const uint32_t best = base.bestBase(&count);
        const std::string body = base.json();
        check::isTrue(best == 0,
                      "a leader under the belief share is refused, because a title's base "
                      "recurs and a coincidence leads: " +
                          body);
        check::isTrue(field(body, "beliefShare") != "0" && field(body, "baseShare") != "null",
                      "and the share it reached is in the report, so a refusal carries a margin");
    }

    // **A binding with no assembly after it is counted, not silently dropped.** The census
    // publishes on every binding and the assembly hook fires only for draws that upload
    // uniforms, so the slot is overwritten whenever a draw has none -- and a pairing that quietly
    // went wrong is worse than one that is counted.
    {
        UniformBlockBase base;
        base.publish(1, 0x40, 0x40);
        base.publish(1, 0x40, 0x40);
        base.observe(twoBlocksAt(0x40, 0x1000));
        const std::string body = base.json();
        check::isTrue(field(body, "bindingsOverwrittenBeforeAssembly") == "1",
                      "one binding was overwritten before an assembly arrived, and it is "
                      "counted: " +
                          body);
        check::isTrue(field(body, "assemblies") == "1" &&
                          field(body, "assembliesWithABinding") == "1",
                      "and the assembly paired with the binding that preceded it, which is the "
                      "one the title's own draw order says");
    }

    // An assembly with no binding before it pairs with nothing and claims nothing.
    {
        UniformBlockBase base;
        base.observe(twoBlocksAt(0x40, 0x1000));
        const std::string body = base.json();
        check::isTrue(field(body, "assemblies") == "1" &&
                          field(body, "assembliesWithABinding") == "0" &&
                          field(body, "candidates") == "0",
                      "an assembly with nothing bound before it contributes no candidate, "
                      "because there is no offset to subtract: " +
                          body);
    }

    // The histogram is in the report, because a base that recurs is a fact about a distribution
    // and a reader must be able to see whether the leader is far ahead or barely ahead.
    {
        UniformBlockBase base;
        for (int round = 0; round < 8; round++) {
            base.publish(1, 0x40, 0x40);
            base.observe(twoBlocksAt(0x40, 0x1000));
        }
        const std::string body = base.json();
        check::isTrue(body.find("\"candidatesByBase\":{") != std::string::npos,
                      "the histogram of bases is in the report: " + body);
        check::isTrue(body.find("\"seen\":") != std::string::npos &&
                          body.find("\"share\":") != std::string::npos,
                      "with each entry's count and share, so the leader's margin is visible");
    }
}
