#pragma once

#include <array>
#include <cstdint>

namespace wiiuport::guest::ppc {

// PowerPC words for guest code blocks, each lifted from the title's own image. The three that
// carry a register are derived from two instructions each that differ only in
// that field, which is what makes the field a fact about this title's encoding
// rather than a guess:
//
//   `lis r0,0x1000` 0x3c001000 and `lis r12,0x1019` 0x3d801019 -> rS at 21-25
//   `ori r0,r0,0x7431` 0x60007431                               -> rS, rD at 21-25
//   `lwz r12,0x24(r31)` 0x819f0024 and `lwz r0,0xcc(r12)` 0x800c00cc
//   `addi` from `li r3,1` 0x38600001
//   `stw r31,0xc(r1)` 0x93e1000c and `stw r0,0x14(r1)` 0x981f0014
//   `andi. r0,r7,0xfd` 0x70e000fd
//   `bne 0x0200004c` 0x40820014, without its absolute bit 0x40820000
//   `blr` 0x4e800020 from 0x025f0950
inline constexpr uint32_t kLoadUpper = 0x3c000000;    // lis rD, hi
inline constexpr uint32_t kOrImmediate = 0x60000000;  // ori rS, rS, lo
inline constexpr uint32_t kLoadWord = 0x80000000;     // lwz rD, disp(rA)
inline constexpr uint32_t kAddImmediate = 0x38000000; // addi rD, rA, simm
inline constexpr uint32_t kStoreWord = 0x90000000;    // stw rS, disp(rA)
inline constexpr uint32_t kAndImmediate = 0x70000000; // andi. rS, rA, K
// `bne` is 0x40820000, and it was 0x40800000 here while the comment above named
// 0x40820014 as the instruction it came from. A conditional branch is one opcode
// with its condition in BO (bits 6-10) and BI (bits 11-15): BO=4 branches when the
// bit BI names is *false*, and `andi.` sets bit 2 (which is BI=2) when its result
// is not zero. So `bne` is BO=4 with BI=2, and BO=4 with BI=0 is a different
// branch over the same displacement. Four `bne` in this title's own image agree,
// and were read out of it rather than argued:
//
//   0x025d4398  bne 0x025d43ac  0x40820014
//   0x025d4678  bne 0x025d4740  0x408200c8
//   0x025d46a4  bne 0x025d4708  0x40820064
//   0x0274c964  bne 0x0274c938  0x4082ffd4
//
// and no instruction in the image carries 0x40800000 as a `bne`.
inline constexpr uint32_t kBranchNotEqual = 0x40820000;   // bne, relative
inline constexpr uint32_t kBranchEqual = 0x41820000;      // beq, relative (0x02035a04)
inline constexpr uint32_t kCompareImmediate = 0x2c000000; // cmpwi rA, simm (0x025b020c)
inline constexpr uint32_t kReturn = 0x4e800020;           // blr
inline constexpr uint32_t kMoveToCounter = 0x7c0903a6;    // mtctr rS
inline constexpr uint32_t kBranchCount = 0x4e800420;      // bctr
// The skipped draw's frame, each word thousands of times in the image.
inline constexpr uint32_t kOpenFrame = 0x9421fff0;       // stwu r1,-0x10(r1)
inline constexpr uint32_t kSaveLink = 0x90010014;        // stw  r0,0x14(r1)
inline constexpr uint32_t kLoadLink = 0x80010014;        // lwz  r0,0x14(r1)
inline constexpr uint32_t kMoveToLink = 0x7c0803a6;      // mtlr r0
inline constexpr uint32_t kCloseFrame = 0x38210010;      // addi r1,r1,0x10
inline constexpr uint32_t kBranchCountLink = 0x4e800421; // bctrl
inline constexpr uint32_t kPrimaryBranch = 18;
inline constexpr int64_t kRelativeBranchReach = 0x02000000;

// `b`/`bl` from `from` to `to`.
inline constexpr uint32_t branchTo(uint32_t from, uint32_t to, bool link) {
    return (kPrimaryBranch << 26) | (link ? 1u : 0u) | ((to - from) & 0x03fffffcu);
}

inline constexpr bool withinReach(uint32_t from, uint32_t to) {
    const int64_t displacement = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    return displacement >= -kRelativeBranchReach && displacement < kRelativeBranchReach;
}

// `lis reg, hi; ori reg, reg, lo`.
inline constexpr std::array<uint32_t, 2> loadAddress(uint32_t reg, uint32_t address) {
    return {kLoadUpper | (reg << 21) | ((address >> 16) & 0xffff),
            kOrImmediate | (reg << 21) | (reg << 16) | (address & 0xffff)};
}

} // namespace wiiuport::guest::ppc
