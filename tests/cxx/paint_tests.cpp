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
    // Each write discards the recompiled block, so a re-arm must not write it again.
    size_t blockWrites = 0;

    bool writeWord(uint32_t address, uint32_t value) {
        if (address == 0) {
            return false;
        }
        m_words[address] = value;
        if (address >= code && address < code + 256) {
            ++blockWrites;
        }
        return true;
    }

    void erase(uint32_t address) {
        m_words.erase(address);
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
constexpr uint32_t kOtherVTable = 0x10145000;

FakeGuest loadedTitle() {
    FakeGuest guest;
    guest.writeWord(kDisplay + WindWakerPaint::kVTableOffset, WindWakerPaint::kDisplayVTable);
    guest.writeWord(WindWakerPaint::kDisplayVTable + WindWakerPaint::kFrameSlot,
                    WindWakerPaint::kDisplayFrame);
    guest.writeWord(WindWakerPaint::kDisplayLoopTop, WindWakerPaint::kDisplayLoopTopFirst);
    guest.writeWord(kDisplay + WindWakerPaint::kIntervalOffset, WindWakerPaint::kTitleSwapInterval);
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

// **The blend writes on the stand-in's in-between paint and on nothing else.** The stand-in's
// `installed()` is the guard, and this is the arm that matters: **a parity is only "half the
// paints" while the stand-in is doubling them.** With one paint per tick the counter still climbs
// and the parity still alternates, so without the guard every second of the game's *own* frame read
// as an in-between paint. That was measured on the title -- the falsifier arm of
// `pose_blend_run.py` caught the blend writing on paints it had been told were the tick's own --
// and this is the unit test for the fix, with the same fixture that installs the stand-in.
//
// The fixture lives here rather than beside the blend's own tests because installing a stand-in
// needs a guest to install it into, and there is one here and none there.

void wiiuport::tests::runPaintTests() {
    thePacingIsReportedAsNotYetThereRatherThanAsANumber();
    // The words of the payload, checked against the title's own image: the
    // The payload, and the branch encoding its computed words depend on. The
    // only word not lifted from the title is `li r3,1`; every other word is a
    // branch, and a branch's displacement is worked out from where it lands.
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
        check::isTrue(!WindWakerPaint::payload(0x40000000).has_value(),
                      "and a payload that cannot branch back is refused");
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
        check::isTrue(mod.enable().find("has not painted") != std::string::npos,
                      "with no paint seen yet there is no vtable to patch, and it says so");
        paintOnce(kDisplay);
        check::isTrue(mod.enable().empty(), "the stand-in installs over this title's own frame");
        uint32_t slot = 0;
        check::isTrue(readWord(WindWakerPaint::kDisplayVTable + WindWakerPaint::kFrameSlot, slot) &&
                          slot == g_lastBlock,
                      "and the vtable slot points at it");
        check::isTrue(mod.paints() == 1, "one paint counted through the probe");
        check::isTrue(guest.block().size() == 11, "the block holds exactly the stand-in's words");
        check::isTrue(mod.json().find("\"installed\":true") != std::string::npos,
                      "the report says it is installed");
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
                "\"installed\":false,\"block\":\"0x00e07000\","
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
                "\"liveVTable\":\"0x10004e88\",\"fields\":{\"interval\":2}}\n",
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
        const std::string refusal = mod.enable();
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
        guest.writeWord(kDisplay + WindWakerPaint::kVTableOffset, kOtherVTable);
        guest.writeWord(kOtherVTable + WindWakerPaint::kFrameSlot, WindWakerPaint::kDisplayFrame);
        WindWakerPaint mod = makeMod(guest);
        mod.install();
        linked();
        paintOnce(kDisplay);
        check::isTrue(mod.enable().empty(),
                      "a display on another vtable installs against the one it holds");
        uint32_t live = 0;
        uint32_t image = 0;
        check::isTrue(readWord(kOtherVTable + WindWakerPaint::kFrameSlot, live) &&
                          live == g_lastBlock,
                      "and the vtable it holds is the one patched");
        check::isTrue(
            readWord(WindWakerPaint::kDisplayVTable + WindWakerPaint::kFrameSlot, image) &&
                image == WindWakerPaint::kDisplayFrame,
            "while the address out of the image is left alone");
        check::isTrue(mod.disable().empty(), "and it comes back out");
        check::isTrue(readWord(kOtherVTable + WindWakerPaint::kFrameSlot, live) &&
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
        const std::string refusal = mod.enable();
        check::isTrue(refusal.find("display thread's loop") != std::string::npos,
                      "a foreign loop is refused by name");
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
        check::isTrue(reads_link_register(0x7c0802a6),
                      "the frame's first word does read the link register, which is why the probe "
                      "cannot displace it");
        check::isTrue(!reads_link_register(WindWakerPaint::kDisplayFrameProbeFirst),
                      "and the word the probe does displace does not");
    }
    {
        // **The frame carried as a literal, twice.** This is the shape that can call the frame
        // rather than whatever slot 0xcc holds, and slot 0xcc holds the stand-in once this mod is
        // installed.
        //
        // The two words that carry the address are checked by *reconstructing* it, not by asserting
        // two constants: a pair of encodings can be wrong in a way a constant check repeats, and
        // this project has measured what a hand-derived encoding costs twice.
        const auto words = WindWakerPaint::payload(0x00e07000);
        check::isTrue(words.has_value(),
                      "the literal dispatch is built where the loop is in reach");
        if (words.has_value()) {
            check::isTrue(words->size() == 11, "and it is eleven words, the objective's count -- " +
                                                   std::to_string(words->size()));
            // `lis`/`ori` are the upper and lower halves; together they must be the frame.
            auto rebuild = [](uint32_t upper, uint32_t lower) {
                // addis puts the immediate at 16.
                return ((upper & 0x7FFF) << 16) | (lower & 0xFFFF);
            };
            for (size_t pass = 0; pass < 2; pass++) {
                const size_t at = pass * 5;
                check::isTrue(
                    rebuild((*words)[at], (*words)[at + 1]) == WindWakerPaint::kDisplayFrame,
                    "group " + std::to_string(pass) + " carries the frame itself: " + [&] {
                        char buffer[32];
                        std::snprintf(buffer, sizeof(buffer), "%08x",
                                      rebuild((*words)[at], (*words)[at + 1]));
                        return std::string(buffer);
                    }());
                check::isTrue(
                    (*words)[at + 2] == 0x7c0903a6 && (*words)[at + 3] == 0x7fe3fb78 &&
                        (*words)[at + 4] == 0x4e800421,
                    "and then mtspr CTR,r0, or r3,r31,r31 and bctrl, the loop's own three");
            }
            // No word may load slot 0xcc: that is the word this mod rewrote, and a stand-in that
            // re-reads it calls itself.
            bool readsTheSlot = false;
            for (uint32_t word : *words) {
                if (((word >> 26) & 0x3F) == 34 && (word & 0xFFFF) == 0x00CC) {
                    readsTheSlot = true;
                }
            }
            check::isTrue(!readsTheSlot,
                          "and no word of it loads displacement 0xcc, the slot this mod rewrote");
        }
    }
    {
        // One vblank: the title's own field says one, the emulator's pacing says
        // one, and both are put back on the way out.
        FakeGuest guest = loadedTitle();
        WindWakerPaint mod = makeMod(guest);
        mod.install();
        linked();
        paintOnce(kDisplay);
        check::isTrue(mod.enable().empty(), "the stand-in installs");
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
        FakeGuest guest = loadedTitle();
        WindWakerPaint mod = makeMod(guest);
        mod.install();
        linked();
        paintOnce(kDisplay);
        check::isTrue(mod.enable().empty() && mod.disable().empty(), "armed and disarmed once");
        const size_t firstArm = guest.blockWrites;
        check::isTrue(mod.enable().empty(), "and armed again");
        check::isTrue(firstArm > 0 && guest.blockWrites == firstArm,
                      "re-arming leaves a block that already holds the stand-in unwritten");
    }
    {
        // A pacing the emulator refuses is a refusal, and the title's field is
        // not left saying one when the pacing is not.
        FakeGuest guest = loadedTitle();
        WindWakerPaint mod = makeMod(guest);
        mod.install();
        linked();
        paintOnce(kDisplay);
        g_pacingTakes = false;
        const std::string refusal = mod.enable();
        check::isTrue(refusal.find("pacing refused") != std::string::npos,
                      "a refused pacing is refused by name");
        uint32_t field = 0;
        check::isTrue(readWord(kDisplay + WindWakerPaint::kIntervalOffset, field) && field == 2,
                      "and the title's field is left as it was");
    }
    {
        // A display whose interval field cannot be read: refused by name, and
        // the field left as it was rather than written blind.
        FakeGuest guest = loadedTitle();
        guest.erase(kDisplay + WindWakerPaint::kIntervalOffset);
        WindWakerPaint mod = makeMod(guest);
        mod.install();
        linked();
        paintOnce(kDisplay);
        const std::string refusal = mod.enable();
        check::isTrue(refusal.find("interval field") != std::string::npos,
                      "an unreadable interval field is refused by name");
    }
}
