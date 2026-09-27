#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace wiiuport::title {

// Which object the title is binding, for a uniform assembly that arrives without one.
//
// **The gap this exists to close, and it is a measured one.** The fork's assembly hook hands
// over `blockSources` -- the guest addresses the draw sourced its uniforms from -- and
// `FrameRecording.h` calls that "the engine's own storage for the object, and the only identity
// a recorded draw carries". Measured over 836,990 assembled buffers it matched exactly **one**
// identity across the 438,872 that had sources: the uniform block is re-uploaded at a new guest
// address each frame, so the set of addresses is nearly unique per draw and the same object's
// assemblies never meet. Every "did this value change between two draws of one object"
// comparison in `ObjectPoseLocator` therefore happened 63 times, and a movement count over 63
// comparisons is not a measurement.
//
// The identity the objective names is the **node**, and the node is not in the assembly record.
// But it is one step away: the node's own draw calls its sub-object at `node + 0xa1c`, and the
// binder probe on that sub-object is already installed and already firing 137,489 times a run.
// So the binder publishes the object, and an assembly reads whichever object was bound last.
//
// **A single slot, and that is a limitation rather than a design choice.** The binder runs on
// the display thread inside the draw, and the draw's own uniform uploads follow it, so last-bound
// is the right answer for them. Whether anything else binds in between is not assumed -- it is
// measured. `assemblyQueries` counts every read, `assemblyQueriesWithObject` how many found a
// slot filled, and `bindsSinceLastQuery` how many *different* objects were bound between
// consecutive reads. That last one is the number of identities that might be wrong, and a
// correlation whose error rate is not reported is a correlation nobody can trust. It is
// reported, and a run where it is not small says so instead of being believed.
class ObjectIdentityScope {
  public:
    // The object a binding named, published by the binder probe. Zero is never a valid object
    // address, so zero is the "nothing bound" answer and needs no separate flag.
    void bind(uint32_t object);

    // The object bound most recently, or zero. Read once per assembly.
    uint32_t current() const;

    // How many bindings were published, how many reads found a filled slot, and how many reads
    // saw a different object bound since the previous read -- the possible-error count.
    struct Report {
        uint64_t binds = 0;
        uint64_t assemblyQueries = 0;
        uint64_t assemblyQueriesWithObject = 0;
        uint64_t bindsSinceLastQuery = 0;
        uint32_t lastObject = 0;
    };

    Report report() const;

    std::string json() const;

  private:
    // Mutable because reading is itself measured. `current()` counts the read and records
    // whether the slot was filled, so a const query is not a query that can be optimised into
    // nothing -- and a coverage number that a compiler is free to drop is not a coverage
    // number. The identity itself is not mutable: binding changes it, reading does not.
    mutable std::atomic<uint32_t> m_object{0};
    mutable std::atomic<uint64_t> m_binds{0};
    mutable std::atomic<uint64_t> m_queries{0};
    mutable std::atomic<uint64_t> m_queriesWithObject{0};
    // Distinct objects bound between consecutive reads, and the last one published, both
    // published together so the pair is consistent: a reader needs to know whether the slot it
    // read was written once or several times since.
    // Binds since the last read, and the value the last read observed. Two fields because they
    // are two moments: one is in progress and one is the answer, and a single field asked to be
    // both reports the count *after* the reset -- always zero, which is a number that looks like
    // a good result and is the absence of a measurement.
    mutable std::atomic<uint64_t> m_bindsInProgress{0};
    mutable std::atomic<uint64_t> m_bindsAtLastQuery{0};
    mutable std::atomic<uint32_t> m_objectAtQuery{0};
};

} // namespace wiiuport::title
