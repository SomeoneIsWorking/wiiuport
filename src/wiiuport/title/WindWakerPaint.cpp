#include "wiiuport/title/WindWakerPaint.h"

#include "Cafe/HW/Espresso/GuestPatching.h"

#include <lucent/log.h>

#include <array>
#include <cstdio>
#include <vector>

namespace wiiuport::title {

namespace {

// The display loop's dispatch at 0x0274c028-0x0274c030, lifted from the title's image.
constexpr uint32_t kLoopMoveToCounter = 0x7c0903a6;  // mtspr CTR,r0
constexpr uint32_t kLoopRestoreDisplay = 0x7fe3fb78; // or    r3,r31,r31
constexpr uint32_t kLoopCallFrame = 0x4e800421;      // bctrl

// `lis`/`ori` forms as the image spells them (3d80027f, 618c5890); only the immediate is computed.
constexpr uint32_t kAddisImmediate = 0x3C000000; // addis rD,rA,simm
constexpr uint32_t kOriImmediate = 0x60000000;   // ori rA,rS,uimm
constexpr uint32_t kTargetRegister = 0;          // r0, which `mtspr CTR,r0` consumes

constexpr uint32_t loadUpperImmediate(uint32_t address) {
    return kAddisImmediate | (static_cast<uint32_t>(kTargetRegister) << 21) |
           ((address >> 16) & 0xFFFF);
}

constexpr uint32_t orImmediate(uint32_t address) {
    return kOriImmediate | (static_cast<uint32_t>(kTargetRegister) << 21) |
           (static_cast<uint32_t>(kTargetRegister) << 16) | (address & 0xFFFF);
}

// The frame as a literal: slot 0xcc holds the stand-in once it is armed.
constexpr std::array<uint32_t, 5> kDispatch{
    loadUpperImmediate(WindWakerPaint::kDisplayFrame), orImmediate(WindWakerPaint::kDisplayFrame),
    kLoopMoveToCounter, kLoopRestoreDisplay, kLoopCallFrame};
constexpr size_t kPaintsPerPass = 2;
// The loader's arena is a bump allocator, so the block cannot grow once reserved.
constexpr size_t kReservedWords = (kPaintsPerPass * kDispatch.size()) + 1;

// A relative branch reaches 32 MiB either side.
constexpr int64_t kRelativeBranchReach = 0x02000000;
constexpr uint32_t kPrimaryBranch = 18;

std::string hex(uint32_t value) {
    std::array<char, 11> text{};
    std::snprintf(text.data(), text.size(), "0x%08x", value);
    return {text.data()};
}

std::string hexField(uint32_t offset) {
    std::array<char, 9> text{};
    std::snprintf(text.data(), text.size(), "0x%02x", offset);
    return {text.data()};
}

// A JSON object written one member at a time, owning the separators and the quoting.
class JsonBody {
  public:
    // A value that brings its own form: true, false, a number.
    void raw(const char* name, const std::string& value) {
        separate();
        m_body += '"';
        m_body += name;
        m_body += "\":";
        m_body += value;
    }

    void string(const char* name, const std::string& value) {
        raw(name, "\"" + value + "\"");
    }

    void number(const char* name, uint64_t value) {
        raw(name, std::to_string(value));
    }

    // `inner` is another body's `text`.
    void object(const char* name, const std::string& inner) {
        separate();
        m_body += '"';
        m_body += name;
        m_body += "\":";
        m_body += inner;
    }

    std::string finish() const {
        return text() + "\n";
    }

    std::string text() const {
        return m_body + "}";
    }

  private:
    void separate() {
        if (!m_first) {
            m_body += ',';
        }
        m_first = false;
    }

    std::string m_body = "{";
    bool m_first = true;
};

// The title's code image and the import thunks above it.
bool targetInImage(uint32_t address) {
    return address >= 0x02000020u && address < 0x028f88d4u;
}

bool targetsInImage(const std::array<uint32_t, WindWakerPaint::kRecentWords>& sample) {
    for (size_t word = 2; word < sample.size(); word++) {
        if (!targetInImage(sample[word])) {
            return false;
        }
    }
    return true;
}

} // namespace

bool WindWakerPaint::withinReach(uint32_t from, uint32_t to) {
    const int64_t displacement = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    return displacement >= -kRelativeBranchReach && displacement < kRelativeBranchReach;
}

uint32_t WindWakerPaint::branchTo(uint32_t from, uint32_t to, bool link) {
    return (kPrimaryBranch << 26) | (link ? 1u : 0u) | ((to - from) & 0x03fffffcu);
}

std::optional<std::vector<uint32_t>> WindWakerPaint::payload(uint32_t blockAddress) {
    std::vector<uint32_t> words;
    words.reserve(kReservedWords);
    for (size_t pass = 0; pass < kPaintsPerPass; pass++) {
        words.insert(words.end(), kDispatch.begin(), kDispatch.end());
    }
    // A branch, not `blr`: the stand-in's `bctrl` overwrote the link register.
    const uint32_t backAt = blockAddress + (4 * static_cast<uint32_t>(words.size()));
    if (!withinReach(backAt, kDisplayLoopTop)) {
        return std::nullopt;
    }
    words.push_back(branchTo(backAt, kDisplayLoopTop, false));
    return words;
}

WindWakerPaint::WindWakerPaint(Register registerProbe, AllocateCode allocateCode,
                               WriteWord writeWord, ReadWord readWord,
                               SetSwapInterval setSwapInterval, SwapInterval swapInterval)
    : m_register(registerProbe), m_allocateCode(allocateCode), m_writeWord(writeWord),
      m_readWord(readWord), m_setSwapInterval(setSwapInterval), m_swapInterval(swapInterval),
      m_frame(m_paints, readWord) {
}

void WindWakerPaint::install() {
    m_frame.setReservation([this](std::string& refusal) {
        reserve(refusal);
    });
    m_register(kDisplayFrameProbe, kDisplayFrameProbeFirst, m_frame, true, 0);
}

void WindWakerPaint::reserve(std::string& refusal) {
    std::scoped_lock lock(m_mutex);
    if (m_block != 0) {
        return;
    }
    // At link time: the loader's arena expects to be asked while it links.
    m_block = m_allocateCode(4 * kReservedWords);
    if (m_block == 0) {
        m_reservationRefusal = "the loader's arena had no " + std::to_string(4 * kReservedWords) +
                               " bytes of executable guest memory";
        refusal = m_reservationRefusal;
        return;
    }
    m_reservationRefusal.clear();
    lucent::info("paint", "stand-in memory reserved at {}", hex(m_block));
}

void WindWakerPaint::Frame::OnInstall(GuestCallProbes::Installation result) {
    {
        std::scoped_lock lock(mutex);
        installation = result;
    }
    // Outside the probe's lock: the reservation logs on the linking thread.
    if (m_reserve) {
        std::string refusal;
        m_reserve(refusal);
    }
}

void WindWakerPaint::Frame::OnCall(std::span<const uint32_t, 32> gpr, uint32_t /*returnAddress*/) {
    // Before the lock, so a report being written cannot stall the display thread.
    m_paints.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock(mutex);
    display = gpr[3];
    uint32_t flags = 0;
    uint32_t phase = 0;
    if (m_readWord(display + kFlagsOffset, flags)) {
        recent[0] = flags;
    }
    if (m_readWord(display + kPhaseOffset, phase)) {
        recent[1] = phase;
    }
    uint32_t base = 0;
    const bool haveBase = m_readWord(display + kFrameTargetBaseOffset, base) && base != 0;
    if (haveBase) {
        for (size_t index = 0; index < kFrameCallTargetOffsets.size(); index++) {
            uint32_t value = 0;
            if (m_readWord(base + kFrameCallTargetOffsets[index], value)) {
                callTargets[index] = value;
            }
        }
    }
    callTargetBase = haveBase ? base : 0;
    for (size_t index = kFrameSamples - 1; index > 0; index--) {
        for (size_t word = 0; word < kRecentWords; word++) {
            samples[index][word] = samples[index - 1][word];
        }
    }
    for (size_t word = 0; word < 2; word++) {
        samples[0][word] = recent[word];
    }
    for (size_t word = 0; word < kFrameCallTargetOffsets.size(); word++) {
        samples[0][2 + word] = callTargets[word];
    }
    samplesValid = m_paints.load(std::memory_order_relaxed) >= kFrameSamples;
    if (m_paintListener) {
        m_paintListener();
    }
}

bool WindWakerPaint::installed() const {
    std::scoped_lock lock(m_frame.mutex);
    return m_installed;
}

std::string WindWakerPaint::enable() {
    std::scoped_lock lock(m_mutex);
    if (m_installed) {
        return {};
    }
    // The vtable the running display holds; the image's address is only a check.
    uint32_t vtable = 0;
    uint32_t display = 0;
    {
        std::scoped_lock frameLock(m_frame.mutex);
        display = m_frame.display;
    }
    if (display == 0) {
        m_refusal = "the display thread has not painted yet, so which vtable it calls "
                    "is not known";
        return m_refusal;
    }
    if (!m_readWord(display + kVTableOffset, vtable) || vtable == 0) {
        m_refusal = "the display object at " + hex(display) + " would not give up its vtable";
        return m_refusal;
    }
    m_vtableFound = vtable;
    const uint32_t slot = vtable + kFrameSlot;
    uint32_t frame = 0;
    if (!m_readWord(slot, frame)) {
        m_refusal =
            "vtable slot " + hexField(kFrameSlot) + " of " + hex(vtable) + " is not guest memory";
        return m_refusal;
    }
    if (frame != kDisplayFrame) {
        m_refusal = "vtable slot " + hexField(kFrameSlot) + " of " + hex(vtable) + " holds " +
                    hex(frame) + ", not this title's display frame " + hex(kDisplayFrame);
        return m_refusal;
    }
    if (vtable != kDisplayVTable) {
        lucent::info("paint", "the display's vtable is {}, not the {} out of the image",
                     hex(vtable), hex(kDisplayVTable));
    }
    uint32_t first = 0;
    if (!m_readWord(kDisplayLoopTop, first) || first != kDisplayLoopTopFirst) {
        m_refusal = "the display thread's loop at " + hex(kDisplayLoopTop) + " holds " +
                    hex(first) + ", not this title's loop";
        return m_refusal;
    }
    m_original = frame;
    // The title's record of the interval; restored on disable.
    uint32_t field = 0;
    if (!m_readWord(display + kIntervalOffset, field)) {
        m_refusal = "the display's interval field at " + hex(kIntervalOffset) +
                    " would not give up its value";
        return m_refusal;
    }
    m_savedInterval = field;
    if (!m_writeWord(display + kIntervalOffset, kSwapInterval)) {
        m_refusal =
            "the display's interval field at " + hex(kIntervalOffset) + " would not take one";
        m_savedInterval = 0;
        return m_refusal;
    }
    m_wroteInterval = true;
    lucent::info("paint", "display interval field {} -> {}", hex(field), hex(kSwapInterval));
    // The emulator's flip pacing is what gates the display thread; the field alone does not.
    m_savedPacing = m_swapInterval();
    const uint32_t now = m_setSwapInterval(kSwapInterval);
    if (now != kSwapInterval) {
        m_refusal = "the flip pacing refused one vblank and is at " + hex(now);
        (void)m_writeWord(display + kIntervalOffset, m_savedInterval);
        m_wroteInterval = false;
        m_savedInterval = 0;
        return m_refusal;
    }
    m_wrotePacing = true;
    lucent::info("paint", "flip pacing {} -> {} vblank(s), and the title's field agrees",
                 hex(m_savedPacing), hex(kSwapInterval));
    if (m_block == 0) {
        m_refusal = m_reservationRefusal.empty()
                        ? "no executable guest memory was reserved for the stand-in"
                        : m_reservationRefusal;
        return m_refusal;
    }
    const std::optional<std::vector<uint32_t>> at = payload(m_block);
    if (!at.has_value()) {
        m_refusal = "the stand-in at " + hex(m_block) + " cannot reach the loop it returns to";
        return m_refusal;
    }
    // Held across the block and the slot so the display thread is not inside the stand-in.
    std::scoped_lock displayThread(m_frame.mutex);
    std::string written;
    for (size_t word = 0; word < at->size(); word++) {
        const uint32_t address = m_block + (4 * static_cast<uint32_t>(word));
        uint32_t held = 0;
        // A write discards the block's recompiled code, which a display thread may still be in.
        if (m_readWord(address, held) && held == (*at)[word]) {
            written += " " + hex(held);
            continue;
        }
        if (!m_writeWord(address, (*at)[word])) {
            m_refusal = "the stand-in's block at " + hex(m_block) + " would not take word " +
                        std::to_string(word);
            return m_refusal;
        }
        written += " " + hex((*at)[word]);
    }
    // Logged before arming: a fault right after leaves nothing alive to ask.
    lucent::info("paint", "stand-in at {} holds:{}", hex(m_block), written);
    if (!m_writeWord(slot, m_block)) {
        m_refusal = "vtable slot " + hexField(kFrameSlot) + " would not take the write";
        return m_refusal;
    }
    m_installed = true;
    m_patched = slot;
    m_refusal.clear();
    lucent::info("paint", "{} slot {:#04x} now {}", hex(vtable), kFrameSlot, hex(m_block));
    return {};
}

std::string WindWakerPaint::disable() {
    std::scoped_lock lock(m_mutex);
    if (!m_installed) {
        return {};
    }
    std::scoped_lock displayThread(m_frame.mutex);
    if (m_patched == 0 || !m_writeWord(m_patched, m_original)) {
        m_refusal = "vtable slot " + hexField(kFrameSlot) + " at " + hex(m_patched) +
                    " would not take the title's own " +
                    hex(m_original == 0 ? kDisplayFrame : m_original) + " back";
        return m_refusal;
    }
    m_installed = false;
    m_refusal.clear();
    if (m_wroteInterval) {
        const uint32_t display = m_frame.display;
        if (display != 0 && !m_writeWord(display + kIntervalOffset, m_savedInterval)) {
            m_refusal =
                "the display's interval field would not take " + hex(m_savedInterval) + " back";
            return m_refusal;
        }
        m_wroteInterval = false;
        m_savedInterval = 0;
    }
    if (m_wrotePacing) {
        (void)m_setSwapInterval(m_savedPacing);
        m_wrotePacing = false;
        m_savedPacing = 0;
    }
    lucent::info("paint", "vtable slot {:#04x} back to the title's own {}", kFrameSlot,
                 hex(m_original));
    m_patched = 0;
    return {};
}

std::string WindWakerPaint::json() const {
    std::scoped_lock lock(m_mutex);
    JsonBody body;
    body.string("frame", hex(kDisplayFrame));
    body.string("vtable", hex(m_vtableFound));
    body.string("patchedSlot", hex(m_patched));
    body.string("slot", hexField(kFrameSlot));
    body.raw("installed", m_installed ? "true" : "false");
    body.string("block", hex(m_block));
    body.number("swapIntervalAsked", kSwapInterval);
    const uint32_t pacing = m_swapInterval();
    if (pacing == GuestPatching::kSwapIntervalUnknown) {
        body.raw("pacing", "null");
        body.string("pacingWhy", "the graphics bring-up has not created its shared area yet");
    } else {
        body.number("pacing", pacing);
    }
    body.number("titleSwapInterval", kTitleSwapInterval);
    body.number("paints", m_paints.load());
    body.string("probe", probeName());
    {
        std::scoped_lock frameLock(m_frame.mutex);
        body.number("flagsAtLastPaint", m_frame.recent[0]);
        body.number("phaseAtLastPaint", m_frame.recent[1]);
        body.number("indirectBaseAtLastPaint", m_frame.callTargetBase);
        JsonBody targets;
        for (size_t index = 0; index < WindWakerPaint::kFrameCallTargetOffsets.size(); index++) {
            targets.number(std::to_string(WindWakerPaint::kFrameCallTargetOffsets[index]).c_str(),
                           m_frame.callTargets[index]);
        }

        body.object("callTargetsAtLastPaint", targets.text());
        body.raw("samplesValid", m_frame.samplesValid ? "true" : "false");
        JsonBody pair;
        for (size_t paint = 0; paint < WindWakerPaint::kFrameSamples; paint++) {
            JsonBody one;
            one.number("flags", m_frame.samples[paint][0]);
            one.number("phase", m_frame.samples[paint][1]);
            for (size_t index = 0; index < WindWakerPaint::kFrameCallTargetOffsets.size();
                 index++) {
                one.number(
                    ("target_" + std::to_string(WindWakerPaint::kFrameCallTargetOffsets[index]))
                        .c_str(),
                    m_frame.samples[paint][2 + index]);
            }
            one.string("allTargetsInImage",
                       targetsInImage(m_frame.samples[paint]) ? "true" : "false");
            pair.object(std::to_string(paint).c_str(), one.text());
        }
        body.object("lastTwoPaints", pair.text());
    }
    const DisplayFacts facts = displayFacts();
    body.string("display", hex(facts.display));
    body.string("liveVTable", hex(facts.vtable));
    body.object("fields", facts.fields);
    if (!m_refusal.empty()) {
        body.string("refusal", m_refusal);
    }
    return body.finish();
}

WindWakerPaint::DisplayFacts WindWakerPaint::displayFacts() const {
    std::scoped_lock frameLock(m_frame.mutex);
    DisplayFacts facts{.display = m_frame.display};
    if (facts.display == 0) {
        facts.fields = JsonBody().text();
        return facts;
    }
    (void)m_readWord(facts.display + kVTableOffset, facts.vtable);

    JsonBody fields;
    for (const DisplayField& field : kDisplayFields) {
        uint32_t value = 0;
        if (!m_readWord(facts.display + field.offset, value)) {
            continue;
        }
        fields.number(field.name, value);
    }
    facts.fields = fields.text();
    return facts;
}

std::string WindWakerPaint::probeName() const {
    std::scoped_lock lock(m_frame.mutex);
    if (!m_frame.installation.has_value()) {
        return "pending";
    }
    switch (*m_frame.installation) {
    case GuestCallProbes::Installation::Installed:
        return "installed";
    case GuestCallProbes::Installation::EntryHeldOther:
        return "entryHeldOther";
    case GuestCallProbes::Installation::EntryNotRelocatable:
        return "entryNotRelocatable";
    case GuestCallProbes::Installation::NoCodeSpace:
        return "noCodeSpace";
    }
    return "unknown";
}

} // namespace wiiuport::title
