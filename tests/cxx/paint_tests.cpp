#include "check.h"
#include "suites.h"
#include "wiiuport/title/WindWakerPaint.h"

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

void keepRegistration(uint32_t, uint32_t, GuestCallProbes::Probe& probe) {
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
    return WindWakerPaint(&keepRegistration, &allocateCode, &writeWord, &readWord);
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

void wiiuport::tests::runPaintTests() {
    // The words of the payload, checked against the title's own image: the
    // loop body lifted from 0x0274c020, and the two branches, whose only
    // difference from those words is a displacement.
    {
        const auto words = WindWakerPaint::payload(0x00e07000, WindWakerPaint::Mode::PassThrough);
        check::isTrue(words.has_value(), "a pass-through payload is built");
        if (words.has_value() && words->size() == 6) {
            check::isTrue(words->size() == 6, "a pass-through payload is six words");
            check::isTrue((*words)[0] == 0x819f0024, "it starts with the title's lwz r12");
            check::isTrue((*words)[4] == 0x4e800421, "it ends its body with the title's bctr");
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
        // The control mode reaches the frame by a plain branch, so its refusal
        // is about that one distance and nothing else.
        const auto direct = WindWakerPaint::payload(0x00e07000, WindWakerPaint::Mode::Direct);
        check::isTrue(direct.has_value() && direct->size() == 2,
                      "the control payload is two branches");
        check::isTrue(
            !WindWakerPaint::payload(0x40000000, WindWakerPaint::Mode::Direct).has_value(),
            "and it is refused when the frame is out of reach");
    }
    {
        check::isTrue(!WindWakerPaint::modeFrom(0).has_value(), "mode 0 names no stand-in");
        check::isTrue(!WindWakerPaint::modeFrom(5).has_value(), "mode 5 names no stand-in");
        check::isTrue(WindWakerPaint::modeFrom(4) == WindWakerPaint::Mode::Direct,
                      "mode 4 is the control that branches straight at the frame");
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
        check::isTrue(guest.block().size() == 6,
                      "the block holds exactly the stand-in's six words");
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
        check::isTrue(mod.json() ==
                          "{\"frame\":\"0x0274c264\",\"vtable\":\"0x10004e88\","
                          "\"patchedSlot\":\"0x00000000\",\"slot\":\"0xcc\","
                          "\"installed\":false,\"mode\":\"passThrough\",\"block\":\"0x00e07000\","
                          "\"swapIntervalAsked\":1,\"titleSwapInterval\":2,\"paints\":1,"
                          "\"probe\":\"installed\",\"display\":\"0x43e08af8\","
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
        FakeGuest guest = loadedTitle();
        WindWakerPaint mod = makeMod(guest);
        mod.install();
        linked();
        paintOnce(kDisplay);
        check::isTrue(mod.enable(WindWakerPaint::Mode::Twice).empty(), "two paints install");
        const auto twice = guest.block();
        check::isTrue(twice.size() == 11, "two paints are eleven words");
        mod.disable();
        check::isTrue(mod.enable(WindWakerPaint::Mode::TwiceAtSixty).empty(),
                      "two paints and one vblank install");
        const auto sixty = guest.block();
        // Guarded: an index into a block that was never written is a crash that
        // takes the whole report with it, which is the one thing a test suite
        // must never do to the run that reads it.
        check::isTrue(sixty.size() == 13, "and thirteen words, the interval call among them");
        if (sixty.size() != 13) {
            return;
        }
        check::isTrue(sixty[0] == 0x38600001, "starting with the title's own li r3,1");
        // The second body, and the branch back to the loop that ends it: with
        // the interval call the words are li, bl, then two bodies of five.
        check::isTrue(sixty[7] == 0x819f0024 && sixty[11] == 0x4e800421,
                      "the second body is the title's again");
        check::isTrue(sixty[12] == WindWakerPaint::branchTo(0x00e07000 + 4 * 12,
                                                            WindWakerPaint::kDisplayLoopTop, false),
                      "and it branches back to the top of the loop");
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
