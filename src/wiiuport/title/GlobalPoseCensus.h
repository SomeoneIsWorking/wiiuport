#pragma once

#include "wiiuport/title/TransformShape.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wiiuport::title {

// Which 4-aligned offsets in the guest's data area hold a transform that changes, read as two
// snapshots of the same memory a frame apart.
//
// **The gap this closes, and it is a measured one.** The per-object census reads the uniform buffer
// each draw assembles, and it finds transform-shaped values that move: the best offset is in
// 206,388 of 823,431 assemblies, 25.1%. **That share identifies nothing.** A per-object world
// matrix is in every object draw and nowhere else, so 25% is exactly what a per-object pose looks
// like if a quarter of the frame's draws are objects -- and it is also what a quarter-of-the-time
// global looks like. Deciding between them from a share is a guess with a denominator attached,
// which is the shape of the mistake this project is retiring.
//
// What separates them is *how often they change*. A static prop's world matrix is the same for
// ever. A camera's view matrix is different in every frame. So the question is not "is this a
// transform at this offset" but "is this a transform here **and did it move between two readings a
// frame apart**", and that is a question about the *data area* -- where a global uniform lives --
// rather than about the per-draw assembly.
//
// **The range is the title's own `.data` and `.bss`, contiguous.** Taken from the converted ELF's
// section table rather than guessed: `.data` at 0x1018c0c0 size 0x70c78, `.bss` at 0x101fce00 size
// 0x2dd3c8, which abut, so one range of 0x2ee108 bytes covers both and there is no gap to reason
// about. `.rodata` is excluded on purpose: it is the name table and the constants, and a transform
// that changes per frame does not live in read-only data.
//
// **The class is `TransformShape`'s, not a new one.** `isAffine` accepts a scaled pose and refuses
// a singular or arbitrarily large one; `moved` is a magnitude test with a stated epsilon, because
// the first version of this project's movement test called a last-mantissa-bit difference movement
// and named three static matrices. One implementation of each rule, and this file asks rather than
// defines.
//
// **This runs on the channel thread, never the display thread.** It holds about 6 MB for the two
// snapshots, so doing it on the thread that paints would be a frame-time spike measured in
// milliseconds of a sixty-hertz budget.
class GlobalPoseCensus {
  public:
    // Bulk guest read: the same seam the block scans use, and refused by reason rather than
    // returning something plausible.
    using ReadWords = std::function<bool(uint32_t address, uint32_t* values, uint32_t count)>;
    // A counter that changes once per presented frame, so the two snapshots are a frame apart
    // rather than however long the first read happened to take.
    using FrameCounter = std::function<uint64_t()>;

    explicit GlobalPoseCensus(ReadWords readWords, FrameCounter frame)
        : m_readWords(std::move(readWords)), m_frame(std::move(frame)) {
    }

    // One pass. Returns the number of offsets named, or zero with `refusal` saying why -- a scan
    // that could not read its range has scanned nothing and must not report a zero as a finding.
    size_t scan(std::string& refusal);

    // How long to wait for the frame counter to move, in milliseconds, and how many times to try
    // before giving up on the second snapshot being a frame later rather than the same instant.
    static constexpr uint32_t kWaitMs = 4000;
    static constexpr uint32_t kWaitSteps = 40;

    // The title's `.data` and `.bss`, from the ELF's section table. Contiguous, and asserted to be.
    static constexpr uint32_t kStart = 0x1018C0C0;
    static constexpr uint32_t kEnd = 0x1047A1C8;
    static constexpr size_t kWords = (kEnd - kStart) / 4;

    std::string json() const;

  private:
    struct Hit {
        uint32_t offset = 0;  // bytes from kStart
        uint32_t address = 0; // the guest address
        float biggestDelta = 0.0f;
        float scale = 0.0f;
        bool rigid = false;
    };

    // Reported, capped, and the cap is in the report: a run that names more than this is a run
    // whose predicate is too loose, and saying so is better than printing the first few as though
    // they were all of them.
    static constexpr size_t kReported = 24;
    // How many raw hits are kept before the collapse, which is generous: the collapse needs room
    // to drop a pose's own shifted fragments, and a bound too tight would collapse two real poses
    // that happen to be near each other.
    static constexpr size_t kKeep = 256;

    ReadWords m_readWords;
    FrameCounter m_frame;
    std::vector<Hit> m_hits;
    size_t m_scans = 0;
    size_t m_wordsTested = 0;
    size_t m_classified = 0;
    size_t m_rigidHits = 0;
    size_t m_collapsed = 0;
    size_t m_moved = 0;
    size_t m_refusedScans = 0;
    std::string m_lastRefusal;
};

} // namespace wiiuport::title
