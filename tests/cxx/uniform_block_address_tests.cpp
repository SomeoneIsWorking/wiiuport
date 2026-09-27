/// Where the uniform block a draw sources actually is, and which word of the record says so.
///
/// The binder at 0x027ff88c / 0x027ff9c0 was decompiled to find the base its relative offset was
/// relative to, and the decompilation is what killed the base: `GX2SetVertexUniformBlock(iVar5,
/// uVar4, uVar6)` with `uVar4 = record[0x0c]` and `uVar6 = record[0x04]`, and the fork's
/// `_GX2SubmitUniformBlock` writes one of those two straight into the uniform block register with
/// no base added. So one of them is an address and the other is a size, and the arithmetic built
/// on the other reading measured the difference of a size and an address.
///
/// Which is which is not taken from the decompilation -- the fork's export maps `gpr[4]` to its
/// `size` and `gpr[5]` to its `virtualAddress`, which is the reverse of the documented GX2 order,
/// and both readings fit the observations. It is decided here, by counting: the word that is the
/// address is the one whose value the draw's real addresses match, and if none of them is, the
/// histogram of `address - word` says which one is an offset into a pool and what the pool's base
/// is.
///
/// The pairs are by the title's own object, not by adjacency, and the case below is what that buys:
/// two objects bound in an order that adjacency would mis-pair, and the right word found anyway.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/UniformBlockAddress.h"

#include <array>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using wiiuport::title::UniformBlockAddress;

constexpr uint32_t kOne = 0x02160000u;
constexpr uint32_t kTwo = 0x02161000u;

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

// A seven-word descriptor record, as the binder read it: a per-object head, the word that names
// the block, and a tail that is the same in every record.
std::array<uint32_t, 7> recordWith(uint32_t addressWord) {
    return {0x11111111u, 0x40u, 0x33333333u, addressWord, 0x55555555u, 0x66666666u, 0x77777777u};
}

// The same record with a word 3 that names nothing, so a test about another word is not passed by
// word 3 matching everything -- the confound that made the first version of the "no majority" case
// pass for the wrong reason.
std::array<uint32_t, 7> recordNamingNothing() {
    return recordWith(0xdeadbeefu);
}

// The `seen` count of the histogram entry whose `base` is `value`, or 0 when there is none.
// A space-saving sketch's counts are upper bounds -- a newcomer inherits the least frequent entry's
// count plus one -- so a caller comparing against a truth has to allow for that, and a test that
// asserted equality would be asserting that the sketch does not carry.
uint64_t seenForBase(const std::string& body, uint32_t value) {
    const std::string key = "\"base\":" + std::to_string(value) + ",\"seen\":";
    size_t at = body.find(key);
    while (at != std::string::npos) {
        const size_t start = at + key.size();
        size_t end = start;
        while (end < body.size() && std::isdigit(static_cast<unsigned char>(body[end]))) {
            end++;
        }
        return std::strtoull(body.substr(start, end - start).c_str(), nullptr, 10);
    }
    return 0;
}

// The draw sourced one block, at `address`.
std::vector<uint32_t> sourcedAt(uint32_t address) {
    return {0, address};
}

} // namespace

void wiiuport::tests::runUniformBlockAddressTests() {
    // **The word the draw's real addresses match is the one that names the address.** Word 3
    // (`+0x0c`) is given the address in every draw, so it hits every time; the other six are
    // constants that appear in no address list at all.
    {
        UniformBlockAddress address;
        for (int round = 0; round < 20; round++) {
            const auto record = recordWith(0x01800000u + static_cast<uint32_t>(round) * 0x100u);
            address.publish(kOne, record);
            address.observe(kOne, sourcedAt(record[3]), {});
        }
        check::isTrue(address.addressWord() == 3,
                      "word 3 is named as the address word, because the draw's real addresses "
                      "matched it in every paired draw: " +
                          address.json());
        const std::string body = address.json();
        check::isTrue(field(body, "addressWordOffset") == "12" &&
                          field(body, "addressWordHits") == "20",
                      "with the offset it came from and the count that named it, both read from "
                      "the report rather than restated here");
        check::isTrue(body.find("\"wordHits\":{") != std::string::npos &&
                          body.find("\"hits\":0") == std::string::npos,
                      "and no word that hit nothing is in the report, so the seven words are not "
                      "all dressed up as candidates");
    }

    // **Pairs are by object, not by adjacency.** Two objects bound in the order that adjacency
    // would mis-pair -- object one's record, then object two's, then a draw of object one again --
    // and the word is found anyway. With an adjacency slot the third draw would be compared with
    // object two's record and every word's hit rate would collapse to nothing.
    {
        UniformBlockAddress address;
        for (int round = 0; round < 6; round++) {
            const auto record = recordWith(0x01800000u + static_cast<uint32_t>(round) * 0x100u);
            address.publish(kOne, record);
            // Object two is bound in between, with a record naming somewhere else entirely.
            address.publish(kTwo, recordWith(0x02ff0000u));
            address.observe(kOne, sourcedAt(record[3]), {});
        }
        check::isTrue(address.addressWord() == 3,
                      "the pair is found despite another object being bound in between, because "
                      "the draw names the object it is in the middle of: " +
                          address.json());
        const std::string body = address.json();
        check::isTrue(field(body, "assembliesWithARecord") == "6" &&
                          field(body, "recordsHeld") == "2",
                      "with all six draws paired and both objects' records held, which is the "
                      "denominator the hit count rests on");
    }

    // **A draw of an object that was never bound pairs with nothing.** Its absence is counted,
    // because an assembly that claimed a record it did not have would be a fabricated comparison.
    {
        UniformBlockAddress address;
        address.publish(kOne, recordWith(0x01800000u));
        address.observe(0x0badf00du, sourcedAt(0x01800000u), {});
        address.observe(kOne, sourcedAt(0x01800000u), {});
        const std::string body = address.json();
        check::isTrue(field(body, "assembliesWithARecord") == "1" &&
                          field(body, "assemblies") == "2",
                      "one of the two draws paired, and the one for an object the binder never "
                      "named claimed nothing: " +
                          body);
    }

    // **A word that hits without a majority is not the address.** The bar is a majority of the
    // paired draws, not a lead: every word is compared against the same address list, so the seven
    // are competing on one corpus and a word that hits 3 times out of 20 is a coincidence with a
    // name on it. This is the case that decides the instrument, and it is the one a "largest
    // count" bar would pass.
    {
        UniformBlockAddress address;
        for (int round = 0; round < 20; round++) {
            auto record = recordNamingNothing();
            // In a quarter of the draws word 1 happens to equal the address, which a real title
            // would not do and a coincidence readily does.
            record[1] = (round % 4 == 0) ? 0x01800000u : 0x40u;
            address.publish(kOne, record);
            address.observe(kOne, sourcedAt(0x01800000u), {});
        }
        check::isTrue(address.addressWord() == -1,
                      "no word is named the address word, because nothing held a majority: " +
                          address.json());
        const std::string body = address.json();
        check::isTrue(field(body, "addressWord") == "null" && field(body, "bestWordShare") != "0",
                      "and the report is a null with the share the leader reached, so the refusal "
                      "carries a margin rather than being an absence");
    }

    // **A word at exactly the bar is named**, so the bar is a threshold and not a mood: the
    // half-and-half corpus is the boundary the previous case sits below.
    {
        UniformBlockAddress address;
        for (int round = 0; round < 20; round++) {
            auto record = recordNamingNothing();
            record[1] = (round % 2 == 0) ? 0x01800000u : 0x40u;
            address.publish(kOne, record);
            address.observe(kOne, sourcedAt(0x01800000u), {});
        }
        check::isTrue(address.addressWord() == 1,
                      "a word holding exactly half the paired draws is named, because the bar is "
                      "a majority and half of a half is not: " +
                          address.json());
    }

    // **No word is the address, so the histogram of `address - word` is the live route -- and a
    // real offset into a pool falls out of it with the word it came from.** Word 1 is an offset and
    // the pool's base is `0x01800000`, so `address - word` is the base in every draw; the other
    // words produce arithmetic that does not repeat. This is the case the whole measurement exists
    // for, and a run where no word matched is exactly the case that makes it necessary.
    {
        UniformBlockAddress address;
        for (int round = 0; round < 20; round++) {
            auto record = recordNamingNothing();
            record[1] = static_cast<uint32_t>(round) * 0x40u;
            address.publish(kOne, record);
            address.observe(kOne, sourcedAt(0x01800000u + record[1]), {});
        }
        const std::string body = address.json();
        check::isTrue(address.addressWord() == -1,
                      "no word is the address, because none of them was -- the offset and the "
                      "address are different words' business: " +
                          body);
        check::isTrue(field(body, "base") == "25165824" && field(body, "baseWord") == "1",
                      "and the base and the word it came from are named: " + body);
        // The share's denominator is *every* candidate -- all seven words, every draw -- and not
        // the twenty that matched, so a real base is a minority of a corpus that is mostly the
        // wrong word. Asserted as the two counts rather than as the share, because the share is
        // `JsonBody::real`'s nine digits and a test that spelled those out would be asserting the
        // formatter instead of the finding.
        check::isTrue(field(body, "baseSeen") == "20" && field(body, "baseCandidates") == "140",
                      "with the count and the corpus it is a share of, because a base that recurs "
                      "is a fact about a distribution and a reader has to see the denominator: " +
                          body);
        check::isTrue(body.find("\"candidatesByBase\":{") != std::string::npos,
                      "and the histogram is in the report, so the leader can be compared with what "
                      "it beat");
    }

    // **A corpus where nothing recurs names no base**, with the leader's share beside it. The bar
    // is a share of a corpus that is mostly the wrong pairing, and the largest count alone would
    // name a coincidence.
    {
        UniformBlockAddress address;
        for (int round = 0; round < 20; round++) {
            address.publish(kOne, recordNamingNothing());
            address.observe(kOne, sourcedAt(0x30000000u + static_cast<uint32_t>(round) * 0x10u),
                            {});
        }
        const std::string body = address.json();
        check::isTrue(field(body, "base") == "null" && field(body, "baseSeenShare") != "null",
                      "no base is named when nothing recurs, and the share the leader reached is "
                      "in the report: " +
                          body);
        check::isTrue(field(body, "baseCandidates") == "140",
                      "with every candidate counted -- seven words times twenty draws -- so a null "
                      "is a statement about a set rather than an absence");
    }

    // **The address histogram is reported, because a constant address is a fact about a
    // distribution.** One address in every draw is a finding this measurement exists for, and a
    // report that named it without the count beside it would be an assertion.
    {
        UniformBlockAddress address;
        for (int round = 0; round < 10; round++) {
            address.publish(kOne, recordWith(0x40u));
            address.observe(kOne, sourcedAt(0x40u), {});
        }
        const std::string body = address.json();
        check::isTrue(
            field(body, "leadingAddress") == "64" && field(body, "leadingAddressSeen") == "10",
            "the leading address is reported with the number of draws that sourced it: " + body);
        check::isTrue(field(body, "distinctAddresses") == "1" && field(body, "addresses") == "10",
                      "and it is one distinct address out of ten sourced, which is what a constant "
                      "looks like and what would be invisible without the denominator");
    }

    // **The record is quoted raw.** A report that carried only a conclusion about the words would
    // be asking to be believed, and the words are what the conclusion is about.
    {
        UniformBlockAddress address;
        address.publish(kOne, recordWith(0x01801000u));
        address.observe(kOne, sourcedAt(0x01801000u), {});
        const std::string body = address.json();
        check::isTrue(body.find("\"sampleRecords\":{") != std::string::npos,
                      "the sample records are in the report: " + body);
        check::isTrue(body.find("25169920") != std::string::npos,
                      "carrying the words as they were read -- 0x01801000 is one of them, so a "
                      "reader can check the finding against the record rather than against this "
                      "file's account of it");
    }

    // **A heavy hitter survives a corpus that overflows the sketch, and is still not named until it
    // clears the share.** Two things are being separated here, and conflating them is how a
    // truncated histogram becomes a claimed base: *surviving* the sketch and *being named* by the
    // bar. The first is what the space-saving structure buys over a plain bounded map; the second
    // is a separate claim with its own denominator.
    {
        UniformBlockAddress address;
        constexpr uint32_t kBase = 0x01800000u;
        // **The noise first, and the base after it.** The order is the whole case: a base inserted
        // before the sketch fills survives under either policy, so a test that put it first would
        // pass a plain bounded map that refuses newcomers and displace nothing. Arriving into a
        // full sketch is the only situation where the two policies differ.
        for (int round = 0; round < 12000; round++) {
            auto record = recordNamingNothing();
            record[1] = 0x40u;
            address.publish(kOne, record);
            address.observe(kOne, sourcedAt(0x50000000u + static_cast<uint32_t>(round) * 4u), {});
        }
        // Ten draws that source the base, into a sketch that is already full of noise.
        for (int round = 0; round < 10; round++) {
            auto record = recordNamingNothing();
            record[1] = static_cast<uint32_t>(round + 1) * 0x40u;
            address.publish(kOne, record);
            address.observe(kOne, sourcedAt(kBase + record[1]), {});
        }
        const std::string body = address.json();
        check::isTrue(field(body, "candidatesEvicted") != "0",
                      "the sketch overflowed, which is the case: over 65,536 distinct candidates "
                      "into 65,536 slots, and the evictions are counted because a sketch's counts "
                      "are upper bounds: " +
                          body);
        check::isTrue(field(body, "base") == "null",
                      "and no base is named, because ten sightings out of more than 84,000 "
                      "candidates is not a share of the corpus -- surviving the sketch is not "
                      "being named, and only the second is a claim");
        check::isTrue(seenForBase(body, 25165824) >= 10,
                      "but the base is still in the histogram, with a count no lower than the ten "
                      "draws that sourced it, which is what the space-saving structure bought: a "
                      "plain bounded map refusing newcomers would have dropped it and this report "
                      "would have nothing to show");
    }

    // **The same corpus with the base recurring in a large share does name it**, which is the
    // payoff: a real base is a heavy hitter *and* a share, and this is the case the previous one
    // cannot reach.
    {
        UniformBlockAddress address;
        constexpr uint32_t kBase = 0x01800000u;
        for (int round = 0; round < 2000; round++) {
            auto record = recordNamingNothing();
            // Two draws in five source the base; the rest are noise that never repeats.
            if (round % 5 < 2) {
                record[1] = static_cast<uint32_t>(round) * 0x40u;
                address.publish(kOne, record);
                address.observe(kOne, sourcedAt(kBase + record[1]), {});
            } else {
                record[1] = 0x40u;
                address.publish(kOne, record);
                address.observe(kOne, sourcedAt(0x50000000u + static_cast<uint32_t>(round) * 4u),
                                {});
            }
        }
        const std::string body = address.json();
        check::isTrue(field(body, "base") == "25165824" && field(body, "baseWord") == "1",
                      "the base is named through a corpus that overflows the sketch, because it is "
                      "both a heavy hitter and a share of the corpus: " +
                          body);
        check::isTrue(
            field(body, "baseSeen") == "800",
            "with the count it was seen, which is 800 -- two draws in five of 2,000 -- so "
            "the number is a count and not a share pretending to be one");
    }

    // **A full record map drops the least recently bound, and does not refuse.** Measured: 179,285
    // of 388,000 publishes were refused outright under a plain bound, so 46% of the bindings
    // contributed nothing and the pair rate was capped by the map rather than by the title. The
    // newest record is the one a following draw needs, so the oldest is what goes.
    {
        UniformBlockAddress address;
        for (size_t index = 0; index < UniformBlockAddress::kMaxObjects + 8; index++) {
            const auto record = recordWith(0x01800000u + static_cast<uint32_t>(index) * 4u);
            address.publish(0x1000u + static_cast<uint32_t>(index), record);
        }
        // The object bound first is long gone; the one bound last is still here, and pairs.
        address.observe(0x1000u, sourcedAt(0x01800000u), {});
        const std::string body = address.json();
        check::isTrue(field(body, "recordsEvicted") == "8" && field(body, "recordsRefused") == "0",
                      "the eight oldest records were dropped and none refused, so a full map costs "
                      "eight bindings rather than every binding after the bound: " +
                          body);
        check::isTrue(field(body, "assembliesWithARecord") == "0",
                      "and the draw of the object bound first pairs with nothing, which is the "
                      "honest consequence of having dropped its record");
        // The object bound last is still held, so its draw pairs -- which is the point of dropping
        // the oldest rather than refusing the newest.
        address.observe(0x1000u + static_cast<uint32_t>(UniformBlockAddress::kMaxObjects + 7),
                        sourcedAt(0x01800000u +
                                  static_cast<uint32_t>(UniformBlockAddress::kMaxObjects + 7) * 4u),
                        {});
        const std::string after = address.json();
        check::isTrue(field(after, "assembliesWithARecord") == "1" &&
                          field(after, "recordsHeld") == "4096",
                      "the draw of the object bound last pairs, while the map stays bounded at its "
                      "stated size -- a map that grows with the objects is a map of the scene");
    }

    // **A register slot the guest wrote is told from one it did not, by its size word.** Word 0 of
    // a uniform block register is whatever last held the slot -- the guest and the shader index
    // those registers differently -- so word 0 alone cannot say the title put a block there. Word 1
    // is `size - 1` as the guest wrote it, and the record says the block is 64 bytes, so a slot
    // holding 0x3f is one the title filled. Two slots: one the guest wrote, one it did not.
    {
        UniformBlockAddress address;
        address.setExpectedSize(0x40);
        address.publish(kOne, recordWith(0x40u));
        for (int round = 0; round < 10; round++) {
            // Slot 0: 0x01800000 with size word 0x3f -- the guest wrote this one. Slot 1:
            // 0x7ffff000 with size word 0 -- left over, never written by the title.
            address.observe(kOne, {0, 0x01800000u, 1, 0x7ffff000u}, {0x3f, 0});
        }
        const std::string body = address.json();
        check::isTrue(field(body, "expectedSize") == "64" && field(body, "sizeWords") == "20",
                      "the expected size and the number of size words read are both reported, so "
                      "the filter has a denominator: " +
                          body);
        check::isTrue(field(body, "writtenSlots") == "10" &&
                          field(body, "distinctWrittenAddresses") == "1",
                      "only the slot whose size word is 0x3f counts, so the one the guest never "
                      "wrote is not mistaken for a block it did");
        check::isTrue(field(body, "leadingWrittenAddress") == "25165824" &&
                          field(body, "leadingWrittenAddressSeen") == "10",
                      "and the address it then holds is the one the title meant -- which is the "
                      "thing word 0 alone could not say");
    }

    // **A slot whose size word is not the record's size is not the title's block**, and the
    // negative half matters as much as the positive: without it, a filter that matched everything
    // would pass the case above.
    {
        UniformBlockAddress address;
        address.setExpectedSize(0x40);
        address.publish(kOne, recordWith(0x40u));
        for (int round = 0; round < 10; round++) {
            address.observe(kOne, {0, 0x01800000u}, {0x40});
        }
        const std::string body = address.json();
        check::isTrue(field(body, "writtenSlots") == "0" &&
                          field(body, "leadingWrittenAddress") == "null",
                      "a size word of 0x40 is not `size - 1` for a 64-byte block, so nothing is "
                      "claimed -- the register holds 63, not 64: " +
                          body);
    }

    // **No expected size means no slots claimed.** A filter with nothing to filter against is not a
    // filter, and guessing 0x3f would be the assumption this route exists to avoid.
    {
        UniformBlockAddress address;
        address.publish(kOne, recordWith(0x40u));
        for (int round = 0; round < 10; round++) {
            address.observe(kOne, {0, 0x01800000u}, {0x3f});
        }
        const std::string body = address.json();
        check::isTrue(field(body, "writtenSlots") == "0" && field(body, "expectedSize") == "0",
                      "with no size from the record, no slot is claimed however inviting its size "
                      "word looks: " +
                          body);
    }

    // A record longer than the words compared is refused rather than truncated into an answer that
    // looks whole.
    {
        UniformBlockAddress address;
        const std::vector<uint32_t> tooLong(UniformBlockAddress::kSampleRecords + 40u, 0x40u);
        address.publish(kOne, tooLong);
        address.observe(kOne, sourcedAt(0x40u), {});
        const std::string body = address.json();
        check::isTrue(field(body, "recordsRefused") == "1" &&
                          field(body, "assembliesWithARecord") == "0",
                      "a record past the words compared is counted as refused and paired with "
                      "nothing: " +
                          body);
    }
}
