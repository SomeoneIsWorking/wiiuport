#pragma once

#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace wiiuport::title {

// Wind Waker HD's display thread, painting each tick's world twice at one vblank a flip.
// The display vtable's frame slot is pointed at a stand-in that runs the display loop's own
// dispatch twice with the frame carried as a literal, then branches back to the loop.
// Nothing here blends; the second paint draws the same world as the first.
class WindWakerPaint {
  public:
    // Slot 0xcc of the display vtable holds the frame.
    static constexpr uint32_t kDisplayVTable = 0x10004e88;
    static constexpr uint32_t kFrameSlot = 0xcc;
    static constexpr uint32_t kVTableOffset = 0x24;
    static constexpr uint32_t kDisplayFrame = 0x0274c264;
    // The display loop's dispatch; the thread's entry at 0x0274c00c is a prologue and re-frames
    // the stack.
    static constexpr uint32_t kDisplayLoopTop = 0x0274c020;
    static constexpr uint32_t kDisplayLoopTopFirst = 0x819f0024; // lwz r12,0x24(r31)
    // The frame's sixth word: its first is `mfspr r0,LR`, which a probe's HLE call would corrupt.
    static constexpr uint32_t kDisplayFrameProbe = 0x0274c278;
    static constexpr uint32_t kDisplayFrameProbeFirst = 0x7c7e1b78; // or r30,r3,r3
    static constexpr uint32_t kSwapInterval = 1;
    static constexpr uint32_t kTitleSwapInterval = 2;
    // The frame's five `bctrl` targets are loaded from `*(display+0x24)`, not from the display.
    static constexpr uint32_t kFrameTargetBaseOffset = 0x24;
    static constexpr std::array<uint32_t, 5> kFrameCallTargetOffsets{0x6c, 0xd4, 0xdc, 0xec, 0xe4};
    static constexpr size_t kFrameSamples = 2;
    // Flags, phase, and the five call targets.
    static constexpr size_t kRecentWords = 2 + kFrameCallTargetOffsets.size();

    static constexpr uint32_t kPhaseOffset = 0x28;
    static constexpr uint32_t kIntervalOffset = 0x50;
    static constexpr uint32_t kFlagsOffset = 0x74;
    static constexpr uint32_t kCounterOffset = 0x78;

    struct DisplayField {
        uint32_t offset;
        const char* name;
    };

    static constexpr std::array<DisplayField, 4> kDisplayFields{{
        {kPhaseOffset, "phase"},
        {kIntervalOffset, "interval"},
        {kFlagsOffset, "flags"},
        {kCounterOffset, "counter"},
    }};

    using Register = void (*)(uint32_t entry, uint32_t firstInstruction,
                              GuestCallProbes::Probe& probe, bool holdsEntry, uint32_t resume);
    using AllocateCode = uint32_t (*)(uint32_t sizeInBytes);
    // Words in the guest's byte order.
    using WriteWord = bool (*)(uint32_t guestAddress, uint32_t value);
    using ReadWord = bool (*)(uint32_t guestAddress, uint32_t& value);
    // Returns the pacing in force after the call.
    using SetSwapInterval = uint32_t (*)(uint32_t vblanksPerFlip);
    using SwapInterval = uint32_t (*)();

    WindWakerPaint(Register registerProbe, AllocateCode allocateCode, WriteWord writeWord,
                   ReadWord readWord, SetSwapInterval setSwapInterval, SwapInterval swapInterval);

    // Registers the frame probe; the stand-in's memory is reserved when the probe reports in.
    void install();

    // Told of each paint, on the display thread; set before install.
    void setPaintListener(std::function<void()> listener) {
        m_frame.setPaintListener(std::move(listener));
    }

    bool installed() const;

    // Writes the stand-in and points the vtable slot at it. Empty on success, otherwise the
    // refusal.
    std::string enable();

    const std::string& reservationRefusal() const {
        return m_reservationRefusal;
    }

    // Takes the stand-in's memory once the title is linked.
    void reserve(std::string& refusal);

    // Puts the title's own frame back. Empty on success, otherwise the refusal.
    std::string disable();

    std::string json() const;

    uint64_t paints() const {
        return m_paints;
    }

    const std::atomic<uint64_t>& paintCounter() const {
        return m_paints;
    }

    // The stand-in's words for a block at `blockAddress`, or nothing when its branch back to the
    // loop cannot reach.
    static std::optional<std::vector<uint32_t>> payload(uint32_t blockAddress);

    // A PC-relative branch to `to` standing at `from`.
    static uint32_t branchTo(uint32_t from, uint32_t to, bool link);
    static bool withinReach(uint32_t from, uint32_t to);

  private:
    std::string probeName() const;

    struct DisplayFacts {
        uint32_t display = 0;
        uint32_t vtable = 0;
        std::string fields = "{}";
    };

    DisplayFacts displayFacts() const;

    class Frame final : public GuestCallProbes::Probe {
      public:
        Frame(std::atomic<uint64_t>& paints, ReadWord readWord)
            : m_paints(paints), m_readWord(readWord) {
        }

        void OnInstall(GuestCallProbes::Installation installation) override;

        void setReservation(std::function<void(std::string&)> reserve) {
            m_reserve = std::move(reserve);
        }

        void setPaintListener(std::function<void()> listener) {
            m_paintListener = std::move(listener);
        }

        void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) override;

        // Flags and phase at the last paint.
        std::array<uint32_t, 2> recent{0, 0};
        std::array<uint32_t, kFrameCallTargetOffsets.size()> callTargets{0};
        uint32_t callTargetBase = 0;
        // Newest paint first.
        std::array<std::array<uint32_t, kRecentWords>, kFrameSamples> samples{};
        bool samplesValid = false;
        // Held while the display thread is in the frame; patching takes it so the thread is not
        // inside code being rewritten.
        mutable std::mutex mutex;
        std::optional<GuestCallProbes::Installation> installation;
        uint32_t display = 0;

      private:
        std::atomic<uint64_t>& m_paints;
        ReadWord m_readWord;
        std::function<void()> m_paintListener;
        std::function<void(std::string&)> m_reserve;
    };

    Register m_register;
    AllocateCode m_allocateCode;
    WriteWord m_writeWord;
    ReadWord m_readWord;
    SetSwapInterval m_setSwapInterval;
    SwapInterval m_swapInterval;
    uint32_t m_savedPacing = 0;
    bool m_wrotePacing = false;
    std::atomic<uint64_t> m_paints{0};
    Frame m_frame;
    mutable std::mutex m_mutex;
    uint32_t m_block = 0;
    uint32_t m_original = 0;
    // The live vtable's slot, which need not be the image's.
    uint32_t m_patched = 0;
    uint32_t m_vtableFound = 0;
    uint32_t m_savedInterval = 0;
    bool m_wroteInterval = false;
    std::string m_reservationRefusal;
    bool m_installed = false;
    std::string m_refusal;
};

} // namespace wiiuport::title
