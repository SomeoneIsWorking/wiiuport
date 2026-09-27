#include "check.h"
#include "suites.h"
#include "wiiuport/title/WindWakerPaint.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <span>
#include <string>
#include <vector>

using wiiuport::title::WindWakerPaint;

namespace {

// A guest the mod can be pointed at: a sparse map of guest words, in the
// guest's own order, so the tests exercise the byte swapping rather than
// trusting it. Reads of a word nothing wrote are refused, which is what the
// emulator does for a range that is not mapped.
class FakeGuest {
  public:
    uint32_t code = 0x00e07000;

    bool writeWord(uint32_t address, uint32_t value) {
        if (address == 0) {
            return false;
        }
        m_words[address] = value;
        return true;
    }

    bool readWord(uint32_t address, uint32_t& value) const {
        const auto found = m_words.find(address);
        if (found == m_words.end()) {
            return false;
        }
        value = found->second;
        return true;
    }

    std::vector<uint32_t> block() const {
        std::vector<uint32_t> words;
        for (auto it = m_words.lower_bound(code); it != m_words.end(); it++) {
            if (it->first < code + 256) {
                words.push_back(it->second);
            }
        }
        return words;
    }

    uint32_t nextCode() const {
        return code;
    }

  private:
    std::map<uint32_t, uint32_t> m_words;
};

WindWakerPaint* g_guest = nullptr;
FakeGuest* g_fake = nullptr;
uint32_t g_lastBlock = 0;

// The probe the mod registered, kept so a test can call the title's draw the way
// the title does -- through the probe -- instead of reaching inside the mod.
GuestCallProbes::Probe* g_probe = nullptr;

void keepRegistration(uint32_t, uint32_t, GuestCallProbes::Probe& probe, bool /*holdsEntry*/,
                      uint32_t /*resume*/) {
    g_probe = &probe;
}

// One paint, as the display thread's entry makes it: the frame's arguments.
void paintOnce(uint32_t display) {
    std::array<uint32_t, 32> gpr{};
    gpr[3] = display;
    g_probe->OnCall(std::span<const uint32_t, 32>(gpr.data(), gpr.size()), 0);
}

// The fork's report that the title's modules are linked and the probe is in
// place, which is when the mod takes its memory. A test that skipped it would be
// testing a mod that was never given any, and every check after would be about
// the refusal instead of about the stand-in.
void linked() {
    g_probe->OnInstall(GuestCallProbes::Installation::Installed);
}

// The flip pacing this test pretends the emulator has, and whether a write to it
// took: a fake that refuses is how the refusal path is reached.
uint32_t g_pacing = 2;
bool g_pacingTakes = true;
// What the emulator's shared area holds before the graphics bring-up has made
// it. The real one is a null dereference if the accessor does not check, and a
// control channel that asks what the pacing is before a title has a surface is
// exactly that case.
constexpr uint32_t kNoSharedArea = 0xffffffffu;
bool g_sharedAreaExists = true;

uint32_t setPacing(uint32_t vblanks) {
    if (g_pacingTakes) {
        g_pacing = vblanks;
    }
    return g_pacing;
}

uint32_t pacing() {
    return g_sharedAreaExists ? g_pacing : kNoSharedArea;
}

uint32_t allocateCode(uint32_t sizeInBytes) {
    // The block is empty and writable, as a fresh allocation out of the
    // loader's arena is.
    (void)sizeInBytes;
    g_lastBlock = g_fake->nextCode();
    return g_lastBlock;
}

bool writeWord(uint32_t address, uint32_t value) {
    return g_fake->writeWord(address, value);
}

bool readWord(uint32_t address, uint32_t& value) {
    return g_fake->readWord(address, value);
}

WindWakerPaint makeMod(FakeGuest& guest) {
    g_fake = &guest;
    g_probe = nullptr;
    g_pacing = 2;
    g_pacingTakes = true;
    g_sharedAreaExists = true;
    return WindWakerPaint(&keepRegistration, &allocateCode, &writeWord, &readWord, &setPacing,
                          &pacing);
}

// A display object at a fixed address, holding this title's vtable, and the
// vtable's frame slot filled in -- the state the title is in once it has
// painted anything.
constexpr uint32_t kDisplay = 0x43e08af8;

FakeGuest loadedTitle() {
    FakeGuest guest;
    guest.writeWord(kDisplay + WindWakerPaint::kVTableOffset, WindWakerPaint::kDisplayVTable);
    guest.writeWord(WindWakerPaint::kDisplayVTable + WindWakerPaint::kFrameSlot,
                    WindWakerPaint::kDisplayFrame);
    guest.writeWord(WindWakerPaint::kDisplayLoopTop, WindWakerPaint::kDisplayLoopTopFirst);
    return guest;
}

} // namespace

// A report read before the graphics bring-up has made its shared area. The
// pacing is not an interval then, and the two ways of saying so are not equal:
// a number nobody can interpret reads as a bug, and 0xffffffff in a report reads
// as a bug too. It says so in words, and the product does not fault asking.
namespace {

void thePacingIsReportedAsNotYetThereRatherThanAsANumber() {
    FakeGuest guest = loadedTitle();
    auto mod = makeMod(guest);
    g_sharedAreaExists = false;
    const std::string body = mod.json();
    g_sharedAreaExists = true;
    check::isTrue(body.find("\"pacing\":null") != std::string::npos,
                  "no interval is in force and the report says so");
    check::isTrue(body.find("pacingWhy") != std::string::npos,
                  "and says why, which is the graphics bring-up");
    check::isTrue(body.find("\"pacing\":2") == std::string::npos,
                  "and does not print a number that would read as one");
}

} // namespace

void wiiuport::tests::runPaintTests() {
    thePacingIsReportedAsNotYetThereRatherThanAsANumber();
    // The words of the payload, checked against the title's own image: the
    // The payload, and the branch encoding its computed words depend on. The
    // only word not lifted from the title is `li r3,1`; every other word is a
    // branch, and a branch's displacement is worked out from where it lands.
    {
        const auto words = WindWakerPaint::payload(0x00e07000, WindWakerPaint::Mode::PassThrough);
        check::isTrue(words.has_value(), "a pass-through payload is built");
        if (words.has_value() && words->size() == 2) {
            check::isTrue(words->size() == 2, "a pass-through payload is two branches");
            check::isTrue((*words)[0] == WindWakerPaint::branchTo(0x00e07000, 0x0274c264, false),
                          "the first branches at the frame");
            check::isTrue((*words)[1] == WindWakerPaint::branchTo(0x00e07004, 0x0274c020, false),
                          "and the second back to the top of the display thread's loop");
        }
    }
    {
        // The branch encoding, against a real instruction from the title: the
        // call at 0x0274bafc to the gx2 import at 0x028fad2c is 0x481af231.
        check::isTrue(WindWakerPaint::branchTo(0x0274bafc, 0x028fad2c, true) == 0x481af231,
                      "branchTo reproduces the title's own bl to the swap-interval import");
        check::isTrue(WindWakerPaint::branchTo(0x0274bafc, 0x028fad2c, false) == 0x481af230,
                      "the same branch without the link is that word less one");
    }
    {
        // Reach is refused rather than written wrong: a block far from the loop
        // cannot branch back to it, and the caller is told instead.
        check::isTrue(WindWakerPaint::withinReach(0x00e07000, WindWakerPaint::kDisplayLoopTop),
                      "a block in the loader's arena reaches the display thread's loop");
        check::isTrue(!WindWakerPaint::withinReach(0x40000000, WindWakerPaint::kDisplayLoopTop),
                      "a block 32 MiB away does not");
        const auto far = WindWakerPaint::payload(0x40000000, WindWakerPaint::Mode::PassThrough);
        check::isTrue(!far.has_value(), "and a payload that cannot branch back is refused");
        // The variant kept as the falsifier: the title's own loop body, which
        // is the re-read and the indirect call this design gave up.
        const auto indirect =
            WindWakerPaint::payload(0x00e07000, WindWakerPaint::Mode::IndirectOnce);
        check::isTrue(indirect.has_value() && indirect->size() == 6,
                      "the indirect variant is the title's own five instructions and a branch");
        if (indirect.has_value() && indirect->size() == 6) {
            check::isTrue((*indirect)[0] == 0x819f0024 && (*indirect)[4] == 0x4e800421,
                          "of which the fifth is the title's bctr, which is what does not run");
        }
        // Every direct variant reaches the frame by a branch whose displacement
        // is worked out, so each refuses when that frame is out of reach rather
        // than writing a branch that lands elsewhere.
        for (const WindWakerPaint::Mode mode :
             {WindWakerPaint::Mode::PassThrough, WindWakerPaint::Mode::Twice,
              WindWakerPaint::Mode::TwiceAtSixty}) {
            check::isTrue(WindWakerPaint::payload(0x00e07000, mode).has_value(),
                          "a direct payload is built where the frame is in reach");
            check::isTrue(!WindWakerPaint::payload(0x40000000, mode).has_value(),
                          "and refused where the frame is not");
        }
    }
    {
        // **Mode 8 differs from mode 3 in the link bit of its second frame branch, and nothing
        // else.** Not "the payloads are different words" -- that would pass if the whole thing were
        // different, which is not what is being claimed. The claim is narrow: same number of words,
        // same targets, and the last frame branch has its link bit clear where mode 3's has it set.
        //
        // The expected words are built by the same `branchTo` the payload uses, which is weak on
        // its own; what makes it a test is the word-for-word comparison *and* the link-bit check
        // read out of the words themselves, so a builder that returned the same two payloads twice
        // would fail the bit check and not the arithmetic one.
        const auto three = WindWakerPaint::payload(0x00e07000, WindWakerPaint::Mode::TwiceAtSixty);
        const auto eight =
            WindWakerPaint::payload(0x00e07000, WindWakerPaint::Mode::TailTwiceAtSixty);
        check::isTrue(three.has_value() && eight.has_value(),
                      "both two-paint payloads are built where the frame is in reach");
        if (three.has_value() && eight.has_value()) {
            check::isTrue(three->size() == eight->size(),
                          "and they are the same length, so the difference is not a word count: " +
                              std::to_string(three->size()) + " against " +
                              std::to_string(eight->size()));
            size_t differing = 0;
            size_t where = three->size();
            for (size_t index = 0; index < three->size() && index < eight->size(); index++) {
                if ((*three)[index] != (*eight)[index]) {
                    differing++;
                    where = index;
                }
            }
            check::isTrue(differing == 1,
                          "exactly one word differs, so mode 8 changes the kind of one branch and "
                          "nothing else: " +
                              std::to_string(differing) + " differing");
            // The link bit, read out of the words rather than out of the builder: a `bl` has AA=1
            // in the opcode's LI field, so the two words differ by exactly one.
            // The differing word is the *second* frame branch, not the last word: the last word of
            // both payloads is the branch back to the display thread's loop, which is the same in
            // both. An earlier version of this compared the last words and found them equal, which
            // says nothing about the branch the mode exists to change.
            if (where < three->size() && where < eight->size()) {
                const uint32_t called = (*three)[where];
                const uint32_t branched = (*eight)[where];
                check::isTrue(called == branched + 1,
                              "and the second frame branch is the same branch with the link bit "
                              "clear, which is the whole difference between the two modes");
            }
        }
    }
    {
        // **Mode 9's middle word is the title's own, and it is the fix.** Read out of the display
        // frame: the frame is `mfspr r0,LR; stwu r1,-0x18(r1); stw r30,...; or r30,r3,r3`, so it
        // takes the display pointer into `r30` and dereferences *`r30`* -- `display+0x74` is
        // `lwz r0, 0x74(r30)` -- while `r3` is scratch. The word between the two paints puts the
        // pointer back, and it is `0x7fc3f378`, which the frame itself uses five times in its own
        // body, once before each of its own `bctrl` calls.
        const auto nine =
            WindWakerPaint::payload(0x00e07000, WindWakerPaint::Mode::RestoreDisplayTwice);
        check::isTrue(
            nine.has_value(),
            "the two-paints-with-the-pointer-restored payload is built where the frame is "
            "in reach");
        if (nine.has_value()) {
            check::isTrue(nine->size() == 4,
                          "and it is four words -- paint, restore, paint, back to the loop: " +
                              std::to_string(nine->size()));
            check::isTrue(
                (*nine)[1] == 0x7fc3f378,
                "with the display pointer put back between the two paints, by the title's "
                "own word and not one worked out here");
            check::isTrue((*nine)[0] == WindWakerPaint::branchTo(0x00e07000, 0x0274c264, true),
                          "the first word calls the frame");
            check::isTrue((*nine)[2] == WindWakerPaint::branchTo(0x00e07008, 0x0274c264, true),
                          "and the third calls it again, measured from where that word stands");
        }
    }
    {
        // **Mode 10 is built from the frame's own words and nothing else.** The load and the store
        // are 0x0274c2c4 and 0x0274c38c verbatim, and the test asserts the exact words -- not "the
        // payload differs from mode 9", which a payload that invented its own encoding would also
        // satisfy.
        const auto ten = WindWakerPaint::payload(0x00e07000, WindWakerPaint::Mode::SamePhaseTwice);
        check::isTrue(ten.has_value(),
                      "the same-phase two-paint payload is built where the frame is in reach");
        if (ten.has_value()) {
            check::isTrue(
                ten->size() == 5,
                "and it is five words -- read the flag, paint, write it back, paint, back "
                "to the loop: " +
                    std::to_string(ten->size()));
            check::isTrue((*ten)[0] == 0x801e0074,
                          "the first word is the frame's own `lwz r0,0x74(r30)` at 0x0274c2c4, "
                          "verbatim");
            check::isTrue(
                (*ten)[2] == 0x901e0074,
                "the third is its `stw r0,0x74(r30)` at 0x0274c38c, verbatim -- the two "
                "words the payload needs are the two the frame already uses on this field");
            check::isTrue(
                // **From each word's own address, not from the block's.** A first version checked
                // word 1 against `branchTo(block, frame)` -- word *0*'s address -- which makes a
                // displacement four bytes long and lands four bytes past the frame. The same
                // mistake the gate's counters were read for, in a payload rather than a test: a
                // displacement is measured from the branch, not from the start of the block the
                // branch happens to live in.
                (*ten)[1] == WindWakerPaint::branchTo(0x00e07004, 0x0274c264, true) &&
                    (*ten)[3] == WindWakerPaint::branchTo(0x00e0700c, 0x0274c264, true),
                "and the two paints are calls at the frame, each displacement measured from its "
                "own "
                "word: " +
                    std::to_string((*ten)[1]) + " then " + std::to_string((*ten)[3]));
        }
    }
    {
        // **Mode 11 is the objective's eleven words, in its order, with one computed branch.** The
        // test asserts the ten verbatim words exactly -- not "the payload differs from mode 9",
        // which a payload that invented its own encoding would also satisfy -- and the eleventh is
        // checked as the branch back to the title's loop.
        const auto eleven =
            WindWakerPaint::payload(0x00e07000, WindWakerPaint::Mode::ObjectivePayload);
        check::isTrue(eleven.has_value(),
                      "the objective's payload is built where the loop is in reach");
        if (eleven.has_value()) {
            check::isTrue(eleven->size() == 11,
                          "and it is eleven words, as the objective specifies: " +
                              std::to_string(eleven->size()));
            const std::array<uint32_t, 5> group{0x819f0024, 0x800c00cc, 0x7c0903a6, 0x7fe3fb78,
                                                0x4e800421};
            bool verbatim = true;
            for (size_t index = 0; index < group.size(); index++) {
                verbatim = verbatim && (*eleven)[index] == group[index] &&
                           (*eleven)[group.size() + index] == group[index];
            }
            check::isTrue(verbatim,
                          "the two groups are the objective's five words each, verbatim and in its "
                          "order -- the only mode that reaches the frame by loading it out of the "
                          "vtable rather than by a literal branch");
            check::isTrue((*eleven)[10] ==
                              WindWakerPaint::branchTo(0x00e07000 + 40, 0x0274c020, false),
                          "and the eleventh is the branch back to the display thread's loop, its "
                          "displacement measured from its own word: " +
                              std::to_string((*eleven)[10]));
        }
    }
    {
        check::isTrue(!WindWakerPaint::modeFrom(0).has_value(), "mode 0 names no stand-in");
        check::isTrue(!WindWakerPaint::modeFrom(12).has_value(), "mode 12 names no stand-in");
        // Mode 8 is the tail-branch twin, and the pair is the discriminator: 3 calls the frame
        // twice and faults, 8 paints twice with the second paint returning through the title's own
        // loop. Both exist, and the number reaches the payload builder.
        check::isTrue(WindWakerPaint::modeFrom(8) == WindWakerPaint::Mode::TailTwiceAtSixty,
                      "mode 8 is the tail-branch twin, and it is reachable by number");
        check::isTrue(WindWakerPaint::modeFrom(9) == WindWakerPaint::Mode::RestoreDisplayTwice,
                      "mode 9 is the one that restores the display pointer, and it is reachable by "
                      "number");
        check::isTrue(WindWakerPaint::modeFrom(10) == WindWakerPaint::Mode::SamePhaseTwice,
                      "mode 10 is the one that holds the display's per-pass flag, and it is "
                      "reachable by number");
        check::isTrue(WindWakerPaint::modeFrom(11) == WindWakerPaint::Mode::ObjectivePayload,
                      "mode 11 is the objective's own payload, and it is reachable by number");
        // Mode 7 is the branch-entry control: the same payload, reached by a
        // direct branch at the frame instead of through the vtable, so that the
        // one difference between it and mode 1 is the kind of branch.
        check::isTrue(WindWakerPaint::modeFrom(7).has_value() &&
                          *WindWakerPaint::modeFrom(7) == WindWakerPaint::Mode::BranchEntry,
                      "mode 7 is the branch-entry control");
        check::isTrue(
            WindWakerPaint::payload(0x00e07000, WindWakerPaint::Mode::BranchEntry).has_value(),
            "and it builds a payload");
        check::isTrue(WindWakerPaint::modeFrom(6) == WindWakerPaint::Mode::OneAtSixty,
                      "mode 6 is one paint at one vblank a flip");
        check::isTrue(WindWakerPaint::modeFrom(5) == WindWakerPaint::Mode::IntervalField,
                      "mode 5 writes the title's own interval field and nothing else");
        check::isTrue(WindWakerPaint::modeFrom(4) == WindWakerPaint::Mode::IndirectOnce,
                      "mode 4 is the variant that re-reads the frame through CTR");
        check::isTrue(WindWakerPaint::modeFrom(1) == WindWakerPaint::Mode::PassThrough,
                      "mode 1 is the redirect alone");
        check::isTrue(WindWakerPaint::modeFrom(3) == WindWakerPaint::Mode::TwiceAtSixty,
                      "mode 3 is two paints at one vblank a flip");
    }

    // Installing, and refusing: every refusal names what it found instead.
    {
        FakeGuest guest = loadedTitle();
        WindWakerPaint mod = makeMod(guest);
        mod.install();
        linked();
        // Before the display thread has painted, which vtable it calls is not
        // known, and patching an address out of the image instead is exactly
        // the mistake worth refusing.
        check::isTrue(mod.enable(WindWakerPaint::Mode::PassThrough).find("has not painted") !=
                          std::string::npos,
                      "with no paint seen yet there is no vtable to patch, and it says so");
        paintOnce(kDisplay);
        check::isTrue(mod.enable(WindWakerPaint::Mode::PassThrough).empty(),
                      "the stand-in installs over this title's own frame");
        uint32_t slot = 0;
        check::isTrue(readWord(WindWakerPaint::kDisplayVTable + WindWakerPaint::kFrameSlot, slot) &&
                          slot == g_lastBlock,
                      "and the vtable slot points at it");
        check::isTrue(mod.paints() == 1, "one paint counted through the probe");
        check::isTrue(guest.block().size() == 2,
                      "the block holds exactly the stand-in's two branches");
        check::isTrue(mod.json().find("\"installed\":true") != std::string::npos,
                      "the report says it is installed");
        check::isTrue(mod.json().find("\"mode\":\"passThrough\"") != std::string::npos,
                      "and which stand-in it is");
        std::printf("  paint json: %s", mod.json().c_str());
        check::isTrue(mod.disable().empty(), "and it comes back out");
        std::printf("  paint json after disable: %s", mod.json().c_str());
        // The whole body, exactly: a report no JSON parser will take is
        // indistinguishable from a product that is not reporting, and this is
        // the one place that catches a stray quote or a doubled comma.
        check::isTrue(
            mod.json() ==
                "{\"frame\":\"0x0274c264\",\"vtable\":\"0x10004e88\","
                "\"patchedSlot\":\"0x00000000\",\"slot\":\"0xcc\","
                "\"installed\":false,\"mode\":\"passThrough\",\"block\":\"0x00e07000\","
                "\"swapIntervalAsked\":1,\"pacing\":2,\"titleSwapInterval\":2,\"paints\":1,"
                "\"probe\":\"installed\",\"flagsAtLastPaint\":0,"
                "\"phaseAtLastPaint\":0,"
                "\"indirectBaseAtLastPaint\":268455560,"
                "\"callTargetsAtLastPaint\":{\"108\":0,\"212\":0,\"220\":0,"
                "\"236\":0,\"228\":0},"
                "\"samplesValid\":false,"
                "\"lastTwoPaints\":{\"0\":{\"flags\":0,\"phase\":0,\"target_108\":0,"
                "\"target_212\":0,\"target_220\":0,\"target_236\":0,"
                "\"target_228\":0,\"allTargetsInImage\":\"false\"},"
                "\"1\":{\"flags\":0,\"phase\":0,\"target_108\":0,\"target_212\":0,"
                "\"target_220\":0,\"target_236\":0,"
                "\"target_228\":0,\"allTargetsInImage\":\"false\"}},"
                "\"display\":\"0x43e08af8\","
                "\"liveVTable\":\"0x10004e88\",\"fields\":{}}\n",
            "the report is one JSON object, spelled out");
        check::isTrue(readWord(WindWakerPaint::kDisplayVTable + WindWakerPaint::kFrameSlot, slot) &&
                          slot == WindWakerPaint::kDisplayFrame,
                      "leaving the title's own frame in the slot");
    }
    {
        // Another title: the slot holds something else, and the mod says so
        // rather than writing a stand-in for one frame over another.
        FakeGuest guest = loadedTitle();
        guest.writeWord(WindWakerPaint::kDisplayVTable + WindWakerPaint::kFrameSlot, 0xDEADBEEF);
        WindWakerPaint mod = makeMod(guest);
        mod.install();
        linked();
        paintOnce(kDisplay);
        const std::string refusal = mod.enable(WindWakerPaint::Mode::PassThrough);
        check::isTrue(refusal.find("0xdeadbeef") != std::string::npos,
                      "a foreign frame is refused by what the slot holds");
        check::isTrue(mod.json().find("\"installed\":false") != std::string::npos,
                      "and nothing is installed");
    }
    {
        // A display on a vtable of its own -- the title's other display class --
        // so the word patched and the word restored are both the live one's.
        // Restoring to the address out of the image instead would rewrite a word
        // nothing patched and leave the stand-in installed, which is the kind of
        // failure that only shows up as "the mod cannot be turned off".
        FakeGuest guest = loadedTitle();
        constexpr uint32_t otherVTable = 0x10145000;
        guest.writeWord(kDisplay + WindWakerPaint::kVTableOffset, otherVTable);
        guest.writeWord(otherVTable + WindWakerPaint::kFrameSlot, WindWakerPaint::kDisplayFrame);
        WindWakerPaint mod = makeMod(guest);
        mod.install();
        linked();
        paintOnce(kDisplay);
        check::isTrue(mod.enable(WindWakerPaint::Mode::PassThrough).empty(),
                      "a display on another vtable installs against the one it holds");
        uint32_t live = 0;
        uint32_t image = 0;
        check::isTrue(readWord(otherVTable + WindWakerPaint::kFrameSlot, live) &&
                          live == g_lastBlock,
                      "and the vtable it holds is the one patched");
        check::isTrue(
            readWord(WindWakerPaint::kDisplayVTable + WindWakerPaint::kFrameSlot, image) &&
                image == WindWakerPaint::kDisplayFrame,
            "while the address out of the image is left alone");
        check::isTrue(mod.disable().empty(), "and it comes back out");
        check::isTrue(readWord(otherVTable + WindWakerPaint::kFrameSlot, live) &&
                          live == WindWakerPaint::kDisplayFrame,
                      "restoring the word it actually wrote");
    }
    {
        // The loop the stand-in branches back to is read and checked, not
        // assumed: branching to the thread's entry instead re-frames the stack
        // every paint and takes the display pointer with it.
        FakeGuest guest = loadedTitle();
        guest.writeWord(WindWakerPaint::kDisplayLoopTop, 0x60000000);
        WindWakerPaint mod = makeMod(guest);
        mod.install();
        linked();
        paintOnce(kDisplay);
        const std::string refusal = mod.enable(WindWakerPaint::Mode::PassThrough);
        check::isTrue(refusal.find("display thread's loop") != std::string::npos,
                      "a foreign loop is refused by name");
    }
    {
        // A second paint and the interval are separable, so a failure says
        // which of the two the title would not take.
        //
        // The interval field is seeded with the two the title asks for, because the two-paint shape
        // now **writes** it -- the fix that removed the call into the zero-filled hole -- and a
        // field with nothing in it is not something the mod can save and restore.
        FakeGuest guest = loadedTitle();
        guest.writeWord(kDisplay + WindWakerPaint::kIntervalOffset, 2);
        WindWakerPaint mod = makeMod(guest);
        mod.install();
        linked();
        paintOnce(kDisplay);
        check::isTrue(mod.enable(WindWakerPaint::Mode::Twice).empty(), "two paints install");
        const auto twice = guest.block();
        check::isTrue(twice.size() == 3, "two paints are three words: two calls and a branch");
        mod.disable();
        // The refusal is in the message, so a failure says *why* rather than only that: a check
        // whose text is "it installed" cannot distinguish a wrong payload from a refused field.
        const std::string refusal = mod.enable(WindWakerPaint::Mode::TwiceAtSixty);
        check::isTrue(refusal.empty(),
                      "two paints and one vblank install" +
                          (refusal.empty() ? std::string() : " -- refused: " + refusal));
        const auto sixty = guest.block();
        // Guarded: an index into a block that was never written is a crash that
        // takes the whole report with it, which is the one thing a test suite
        // must never do to the run that reads it.
        // **Four words, and none of them is a call into the hole.** This used to be five, the
        // second of them a `bl` at 0x028fad2c, and that call was the double-paint fault: the
        // address holds four zero words in the title's own image, so the branch ran off the end of
        // the hole, the two `bl`s at the display frame were never reached, and the guest ended up
        // executing host pointer bytes in the loader arena. The interval is set by writing the
        // display's own field, which is what the shape that measures sixty a second has always
        // done.
        check::isTrue(sixty.size() == 3,
                      "and three words: the frame twice and a branch back to the loop, with no "
                      "interval call among them -- " +
                          std::to_string(sixty.size()));
        if (sixty.size() != 3) {
            return;
        }
        check::isTrue(sixty[0] == WindWakerPaint::branchTo(0x00e07000, 0x0274c264, true) &&
                          sixty[1] == WindWakerPaint::branchTo(0x00e07004, 0x0274c264, true),
                      "the frame, called twice");
        check::isTrue(sixty[2] == WindWakerPaint::branchTo(0x00e07008, 0x0274c020, false),
                      "and a branch back to the top of the loop");
        // The field is the title's own record of the interval it asked for, and the two-paint shape
        // sets it the same way the one-vblank shape does rather than by calling into a hole.
        uint32_t interval = 0;
        check::isTrue(readWord(kDisplay + WindWakerPaint::kIntervalOffset, interval) &&
                          interval == 1,
                      "and the title's own interval field reads one, set by writing it");
        // **No word of any payload may be a branch to the address that is not code.** Asserted over
        // the whole word list rather than by position, because a payload that grew a call anywhere
        // is the failure and a position check would not see it.
        bool callsTheHole = false;
        for (uint32_t word : sixty) {
            // A `bl` to 0x028fad2c has AA=0 and LK=1, so the top six bits are 0b010010 and bit 1 is
            // set; the low 24 bits are the displacement. Reconstructing the target from the word
            // catches the call whatever position it was written at.
            if ((word >> 26) == 0x12 && ((word >> 1) & 1) == 1) {
                int32_t displacement = static_cast<int32_t>(word << 2) >> 2;
                if (displacement + 4 == 0x028fad2c) {
                    callsTheHole = true;
                }
            }
        }
        check::isTrue(!callsTheHole,
                      "and no word of it is a call to 0x028fad2c, which holds four zero words in "
                      "the image and was the fault");
    }
    {
        // **The probe's word is the frame's sixth, not its first, and that is load-bearing.** The
        // frame's first word is `mfspr r0, LR` and the frame returns through what that instruction
        // produced; a probe's stub begins with an HLE *call*, and a call sets the link register, so
        // a probe on the first word made the frame save the stub's return address and return into
        // the loader's arena. The sixth word is `or r30,r3,r3`, which reads no special register,
        // and `r3` is still the display there -- which is what the probe reports.
        //
        // Asserted on the words, not on a flag: a probe that moved back to the first word would
        // still report installed and would still be wrong.
        check::isTrue(WindWakerPaint::kDisplayFrameProbe == 0x0274c278,
                      "the probe sits on the frame's sixth word, 0x0274c278");
        check::isTrue(WindWakerPaint::kDisplayFrameProbeFirst == 0x7c7e1b78,
                      "whose instruction is or r30,r3,r3, 0x7c7e1b78");
        check::isTrue(WindWakerPaint::kDisplayFrameProbe != WindWakerPaint::kDisplayFrame,
                      "and it is not the frame's entry");
        // The two facts that make the sixth word the right one, decoded rather than asserted as
        // constants: the entry reads the link register, and the probe word does not.
        auto reads_link_register = [](uint32_t word) {
            return (word >> 26) == 31 && ((word >> 1) & 0x3FF) == 339 && ((word >> 16) & 0x1F) == 8;
        };
        check::isTrue(reads_link_register(WindWakerPaint::kDisplayFrameFirst),
                      "the frame's first word does read the link register, which is why the probe "
                      "cannot displace it");
        check::isTrue(!reads_link_register(WindWakerPaint::kDisplayFrameProbeFirst),
                      "and the word the probe does displace does not");
    }
    {
        // The one-vblank payload is a single branch. The pacing is the
        // emulator's and the title's own record of it is a field, so nothing in
        // the guest is called and the display register arrives untouched.
        const auto words = WindWakerPaint::payload(0x00e07000, WindWakerPaint::Mode::OneAtSixty);
        check::isTrue(words.has_value() && words->size() == 1, "it is one branch");
        if (words.has_value() && words->size() == 1) {
            check::isTrue((*words)[0] == WindWakerPaint::branchTo(0x00e07000, 0x0274c264, false),
                          "at the frame, so its return is the title's loop's");
        }
    }
    {
        // One vblank: the title's own field says one, the emulator's pacing says
        // one, and both are put back on the way out.
        FakeGuest guest = loadedTitle();
        guest.writeWord(kDisplay + WindWakerPaint::kIntervalOffset, 2);
        WindWakerPaint mod = makeMod(guest);
        mod.install();
        linked();
        paintOnce(kDisplay);
        check::isTrue(mod.enable(WindWakerPaint::Mode::OneAtSixty).empty(),
                      "the one-vblank mode installs");
        uint32_t field = 0;
        check::isTrue(readWord(kDisplay + WindWakerPaint::kIntervalOffset, field) && field == 1,
                      "the title's own field reads one");
        check::isTrue(g_pacing == 1, "and the emulator's pacing is one vblank a flip");
        check::isTrue(mod.json().find("\"pacing\":1") != std::string::npos,
                      "and the report says so");
        check::isTrue(mod.disable().empty(), "and it comes back out");
        check::isTrue(readWord(kDisplay + WindWakerPaint::kIntervalOffset, field) && field == 2,
                      "with the title's field back at two");
        check::isTrue(g_pacing == 2, "and the pacing back at two");
    }
    {
        // A pacing the emulator refuses is a refusal, and the title's field is
        // not left saying one when the pacing is not.
        FakeGuest guest = loadedTitle();
        guest.writeWord(kDisplay + WindWakerPaint::kIntervalOffset, 2);
        WindWakerPaint mod = makeMod(guest);
        mod.install();
        linked();
        paintOnce(kDisplay);
        g_pacingTakes = false;
        const std::string refusal = mod.enable(WindWakerPaint::Mode::OneAtSixty);
        check::isTrue(refusal.find("pacing refused") != std::string::npos,
                      "a refused pacing is refused by name");
        uint32_t field = 0;
        check::isTrue(readWord(kDisplay + WindWakerPaint::kIntervalOffset, field) && field == 2,
                      "and the title's field is left as it was");
    }
    {
        // The interval field is the title's own state, so the mode that writes it
        // has to put it back, and a field it cannot read is a refusal rather than
        // a write of a value it guessed.
        FakeGuest guest = loadedTitle();
        guest.writeWord(kDisplay + WindWakerPaint::kIntervalOffset, 2);
        WindWakerPaint mod = makeMod(guest);
        mod.install();
        linked();
        paintOnce(kDisplay);
        check::isTrue(mod.enable(WindWakerPaint::Mode::IntervalField).empty(),
                      "the interval-field mode installs");
        uint32_t field = 0;
        check::isTrue(readWord(kDisplay + WindWakerPaint::kIntervalOffset, field) && field == 1,
                      "and the display's own field now reads one");
        check::isTrue(mod.disable().empty(), "and comes back out");
        check::isTrue(readWord(kDisplay + WindWakerPaint::kIntervalOffset, field) && field == 2,
                      "with the title's own value put back");
    }
    {
        // A display whose interval field cannot be read: refused by name, and
        // the field left as it was rather than written blind.
        FakeGuest guest = loadedTitle();
        WindWakerPaint mod = makeMod(guest);
        mod.install();
        linked();
        paintOnce(kDisplay);
        const std::string refusal = mod.enable(WindWakerPaint::Mode::IntervalField);
        check::isTrue(refusal.find("interval field") != std::string::npos,
                      "an unreadable interval field is refused by name");
    }
    {
        // A stand-in already in, asked for a different one: the title's own
        // frame goes back first, so a refusal leaves it running.
        FakeGuest guest = loadedTitle();
        WindWakerPaint mod = makeMod(guest);
        mod.install();
        linked();
        paintOnce(kDisplay);
        mod.enable(WindWakerPaint::Mode::PassThrough);
        check::isTrue(mod.enable(WindWakerPaint::Mode::Twice).empty(), "a second mode replaces it");
        uint32_t slot = 0;
        readWord(WindWakerPaint::kDisplayVTable + WindWakerPaint::kFrameSlot, slot);
        check::isTrue(slot == g_lastBlock, "and the slot points at the new block");
        check::isTrue(mod.json().find("\"mode\":\"twice\"") != std::string::npos,
                      "the report names the stand-in now in");
    }
}
