#include "wiiuport/title/WindWakerPaint.h"

#include <lucent/log.h>

#include <array>
#include <cstdio>
#include <vector>

namespace wiiuport::title {

namespace {

// The display loop's body, lifted out of the title's own image at 0x0274c020
// where those five instructions stand. Nothing here is hand-assembled, which is
// the point: a word worked out by hand is a word nobody checked.
constexpr std::array<uint32_t, 5> kLoopBody = {
    0x819f0024, // lwz  r12,0x24(r31)   the display's vtable
    0x800c00cc, // lwz  r0,0xcc(r12)    the frame
    0x7c0903a6, // mtctr r0
    0x7fe3fb78, // or   r3,r31,r31      the display, as the original call passed it
    0x4e800421, // bctr
};
constexpr uint32_t kLoadOne = 0x38600001; // li r3,1          from 0x025f094c

// A relative branch reaches 32 MiB either side of where it stands, which is
// the fork's own rule for the same instruction.
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

// A JSON object written one member at a time, owning the separators and the
// quoting. Written as a long chain of additions this report once came out with
// a stray quote and a doubled comma -- joins in an expression that reads as one
// thing are exactly the joins nobody checks -- so every member goes through
// here, and `finish` is the only way to close it.
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

    // An object as a member: `inner` is another body's `text`, unterminated,
    // because this one closes the whole thing.
    void object(const char* name, const std::string& inner) {
        separate();
        m_body += '"';
        m_body += name;
        m_body += "\":";
        m_body += inner;
    }

    // The body closed, with the newline a report ends on.
    std::string finish() const {
        return text() + "\n";
    }

    // The body closed, without it: for a member of another body.
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

} // namespace

bool WindWakerPaint::withinReach(uint32_t from, uint32_t to) {
    const int64_t displacement = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    return displacement >= -kRelativeBranchReach && displacement < kRelativeBranchReach;
}

uint32_t WindWakerPaint::branchTo(uint32_t from, uint32_t to, bool link) {
    return (kPrimaryBranch << 26) | (link ? 1u : 0u) | ((to - from) & 0x03fffffcu);
}

std::string_view WindWakerPaint::modeName(Mode mode) {
    switch (mode) {
    case Mode::PassThrough:
        return "passThrough";
    case Mode::Twice:
        return "twice";
    case Mode::TwiceAtSixty:
        return "twiceAtSixty";
    }
    return "unknown";
}

std::optional<WindWakerPaint::Mode> WindWakerPaint::modeFrom(long long number) {
    switch (number) {
    case 1:
        return Mode::PassThrough;
    case 2:
        return Mode::Twice;
    case 3:
        return Mode::TwiceAtSixty;
    default:
        return std::nullopt;
    }
}

std::optional<std::vector<uint32_t>> WindWakerPaint::payload(uint32_t blockAddress, Mode mode) {
    const bool twice = mode != Mode::PassThrough;
    const bool interval = mode == Mode::TwiceAtSixty;
    // The call, when there is one, sits at the second word; the branch back is
    // last. Each displacement is measured from where its own word stands.
    const uint32_t callAt = blockAddress + 4;
    const uint32_t bodies = twice ? 2 : 1;
    // The branch back is the last word, so it stands after everything before
    // it and nothing else: measuring from one word further sends the display
    // thread wherever that lands.
    const uint32_t backAt = blockAddress + 4 * ((interval ? 2u : 0u) + kLoopBody.size() * bodies);
    if (interval && !withinReach(callAt, kSetSwapInterval)) {
        return std::nullopt;
    }
    if (!withinReach(backAt, kDisplayLoopTop)) {
        return std::nullopt;
    }
    std::vector<uint32_t> words;
    words.reserve(backAt / 4 - blockAddress / 4);
    if (interval) {
        words.push_back(kLoadOne);
        words.push_back(branchTo(callAt, kSetSwapInterval, true));
    }
    for (uint32_t body = 0; body < bodies; body++) {
        words.insert(words.end(), kLoopBody.begin(), kLoopBody.end());
    }
    words.push_back(branchTo(backAt, kDisplayLoopTop, false));
    return words;
}

WindWakerPaint::WindWakerPaint(Register registerProbe, AllocateCode allocateCode,
                               WriteWord writeWord, ReadWord readWord)
    : m_register(registerProbe), m_allocateCode(allocateCode), m_writeWord(writeWord),
      m_readWord(readWord), m_frame(m_paints) {
}

void WindWakerPaint::install() {
    m_register(kDisplayFrame, kDisplayFrameFirst, m_frame);
}

void WindWakerPaint::Frame::OnInstall(GuestCallProbes::Installation result) {
    std::scoped_lock lock(mutex);
    installation = result;
}

void WindWakerPaint::Frame::OnCall(std::span<const uint32_t, 32> gpr, uint32_t /*returnAddress*/) {
    {
        // Counted before the lock: a paint must not be able to block the
        // display thread behind a report being written.
        m_paints.fetch_add(1, std::memory_order_relaxed);
    }
    std::scoped_lock lock(mutex);
    display = gpr[3];
}

std::string WindWakerPaint::enable(Mode mode) {
    std::scoped_lock lock(m_mutex);
    if (m_installed) {
        if (m_mode == mode) {
            return {};
        }
        // A different stand-in is a different patch: put the title's own back
        // before writing another, so a refusal leaves the title running rather
        // than running on the last thing installed.
        const std::string refusal = disableLocked();
        if (!refusal.empty()) {
            return refusal;
        }
    }
    m_mode = mode;
    const uint32_t slot = kDisplayVTable + kFrameSlot;
    uint32_t frame = 0;
    if (!m_readWord(slot, frame)) {
        m_refusal = "the display vtable at " + hex(kDisplayVTable) + " is not guest memory yet";
        return m_refusal;
    }
    if (frame != kDisplayFrame) {
        // Named, because a stand-in written for one frame and pointed at
        // another is a crash with nothing to read.
        m_refusal = "vtable slot " + hexField(kFrameSlot) + " holds " + hex(frame) +
                    ", not this title's display frame " + hex(kDisplayFrame);
        return m_refusal;
    }
    // The stand-in branches back to the top of the display thread's loop, so
    // that address is read and checked rather than assumed: a revision whose
    // loop starts on something else would be sent somewhere else entirely.
    uint32_t first = 0;
    if (!m_readWord(kDisplayLoopTop, first) || first != kDisplayLoopTopFirst) {
        m_refusal = "the display thread's loop at " + hex(kDisplayLoopTop) + " holds " +
                    hex(first) + ", not this title's loop";
        return m_refusal;
    }
    m_original = frame;
    m_block = m_allocateCode(4 * (2 + kLoopBody.size() * 2 + 1));
    if (m_block == 0) {
        m_refusal = "no executable guest memory for the stand-in";
        return m_refusal;
    }
    const std::optional<std::vector<uint32_t>> at = payload(m_block, mode);
    if (!at.has_value()) {
        m_refusal = "the stand-in at " + hex(m_block) +
                    " cannot reach the swap-interval call or the loop it returns to";
        m_block = 0;
        return m_refusal;
    }
    lucent::info("paint", "stand-in {} at {}: {} words", modeName(mode), hex(m_block),
                 std::to_string(at->size()));
    // A word at a time, through the seam that owns the guest's order. Written
    // as bytes from a host word array, every instruction in the block would
    // reach the guest with its halves exchanged -- which is a display thread
    // branching into noise, and no report anywhere near the cause.
    for (size_t word = 0; word < at->size(); word++) {
        if (!m_writeWord(m_block + 4 * static_cast<uint32_t>(word), (*at)[word])) {
            m_refusal = "the stand-in's block at " + hex(m_block) + " would not take word " +
                        std::to_string(word);
            m_block = 0;
            return m_refusal;
        }
    }
    if (!m_writeWord(slot, m_block)) {
        m_refusal = "vtable slot " + hexField(kFrameSlot) + " would not take the write";
        m_block = 0;
        return m_refusal;
    }
    m_installed = true;
    m_refusal.clear();
    lucent::info("paint", "vtable slot {:#04x} now {}; the display thread paints {}", kFrameSlot,
                 hex(m_block), modeName(mode));
    return {};
}

std::string WindWakerPaint::disable() {
    std::scoped_lock lock(m_mutex);
    return disableLocked();
}

// The write-back, with the lock the callers already hold: a scoped_lock is not
// recursive, so a disable() from inside enable() would wait on itself.
std::string WindWakerPaint::disableLocked() {
    if (!m_installed) {
        // Already the title's own frame: putting it back is what was asked
        // for, and refusing would make the unmodded window -- the control
        // every comparison is made against -- unmeasurable.
        return {};
    }
    const uint32_t slot = kDisplayVTable + kFrameSlot;
    if (!m_writeWord(slot, m_original)) {
        m_refusal = "vtable slot " + hexField(kFrameSlot) + " would not take the write back";
        return m_refusal;
    }
    m_installed = false;
    m_refusal.clear();
    lucent::info("paint", "vtable slot {:#04x} back to the title's own {}", kFrameSlot,
                 hex(m_original));
    return {};
}

std::string WindWakerPaint::json() const {
    std::scoped_lock lock(m_mutex);
    JsonBody body;
    body.string("frame", hex(kDisplayFrame));
    body.string("vtable", hex(kDisplayVTable));
    body.string("slot", hexField(kFrameSlot));
    body.raw("installed", m_installed ? "true" : "false");
    body.string("mode", std::string(modeName(m_mode)));
    body.string("block", hex(m_block));
    body.number("swapIntervalAsked", kSwapInterval);
    body.number("titleSwapInterval", kTitleSwapInterval);
    body.number("paints", m_paints.load());
    body.string("probe", probeName());
    body.string("display", hex(m_frame.display));
    body.object("fields", displayFields());
    if (!m_refusal.empty()) {
        body.string("refusal", m_refusal);
    }
    return body.finish();
}

// The display object's own fields, or an empty object before the display thread
// has been seen: a value read as a number or not read at all, never a guess.
std::string WindWakerPaint::displayFields() const {
    std::scoped_lock frameLock(m_frame.mutex);
    if (m_frame.display == 0) {
        return JsonBody().text();
    }

    struct Field {
        uint32_t offset;
        const char* name;
    };

    static constexpr Field kFields[] = {
        {kPhaseOffset, "phase"},
        {kIntervalOffset, "interval"},
        {kFlagsOffset, "flags"},
        {kCounterOffset, "counter"},
    };
    JsonBody fields;
    for (const Field& field : kFields) {
        uint32_t value = 0;
        if (!m_readWord(m_frame.display + field.offset, value)) {
            continue;
        }
        fields.number(field.name, value);
    }
    return fields.text();
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
