#pragma once

#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wiiuport::guest {

// Which call sites and call chains reach a title function, and how often: the instrument for
// a function reached only through a pointer, which leaves Ghidra no
// reference to follow. Configured, not compiled in: WIIUPORT_CALLER_CENSUS
// names up to kMaxEntries functions as `entry:firstInstruction` in hex,
// comma separated. Changes nothing; reports on GET /callers.
class CallerCensus {
  public:
    static constexpr size_t kMaxEntries = 8;
    // The fork's registration, injected so the census is testable without it.
    // The flag says whether the probe keeps the entry. Every registration here is
    // a standing one -- each exists to count calls for the whole run -- so each
    // passes true. The parameter is here so that one `GuestCallProbes::Register`
    // address fits every seam the product hands it to.
    // `resume` is where the call continues; zero is the instruction after the
    // entry, which is what an observer wants. It is in the signature so that one
    // `GuestCallProbes::Register` address fits every seam the product hands it to.
    using Register = void (*)(uint32_t entry, uint32_t firstInstruction,
                              GuestCallProbes::Probe& probe, bool holdsEntry, uint32_t resume);

    // The fork's guest memory read: the bytes at a guest address, or null where unmapped.
    using GuestBytes = const void* (*)(uint32_t address, uint32_t size);

    // Frames of a call's chain: its return address, then each caller's saved link register.
    static constexpr size_t kChainDepth = 6;
    using Chain = std::array<uint32_t, kChainDepth>;
    static constexpr size_t kReportedChains = 24;

    CallerCensus(Register registerProbe, GuestBytes guestBytes)
        : m_register(registerProbe), m_guestBytes(guestBytes) {
    }

    // The chain a call entering a function with registers `gpr` and `returnAddress` came by,
    // walked up the EABI back chain from r1; frames past an unreadable one are zero.
    static Chain chainOf(std::span<const uint32_t, 32> gpr, uint32_t returnAddress,
                         GuestBytes guestBytes);

    struct Target {
        uint32_t entry = 0;
        uint32_t firstInstruction = 0;
    };

    // The targets `text` names, or why it names none. Empty names none.
    static std::optional<std::vector<Target>> parse(std::string_view text, std::string& refusal);

    // Registers each target with the fork, before the title is linked.
    void install(std::span<const Target> targets);

    std::string json() const;

  private:
    class Entry final : public GuestCallProbes::Probe {
      public:
        Entry(Target target, GuestBytes guestBytes) : m_target(target), m_guestBytes(guestBytes) {
        }

        void OnInstall(GuestCallProbes::Installation installation) override;
        void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) override;
        std::string json() const;

        Target target() const {
            return m_target;
        }

      private:
        Target m_target;
        GuestBytes m_guestBytes;
        mutable std::mutex m_mutex;
        std::optional<GuestCallProbes::Installation> m_installation;
        std::unordered_map<uint32_t, uint64_t> m_callsByReturn;
        std::map<Chain, uint64_t> m_callsByChain;
    };

    Register m_register;
    GuestBytes m_guestBytes;
    std::vector<std::unique_ptr<Entry>> m_entries;
};

} // namespace wiiuport::guest
