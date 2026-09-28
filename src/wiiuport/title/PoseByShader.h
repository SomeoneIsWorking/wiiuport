#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wiiuport::title {

// **Where a draw's pose is, in words: one byte offset per shader, and a refusal for a shader it has
// not seen.** This is the blend's lookup, and it is a lookup rather than a search because the
// measurement says a pose's offset in an assembly is a property of the shader that read it.
//
// **Measured, on Wind Waker HD with the camera moving.** The camera is twelve consecutive words in
// a flat run of floats in the title's assembled uniforms, and where that run lands differs by
// shader:
//
// | shader | offset | same value as other objects | movements |
// |---|---|---|---|
// | `0x1557c18f92f3bcb9` | 12 | **0 of 15** -- every object holds its own | 21,730 of 21,879 |
// | `0x8cecd19741c6c1c7` |  4 | **0 of 15** | 202 of 21,950 |
// | `0x1557c18f92f3bcb9` | 60 | 3 of 15 -- a translation column of the same array | 21,708 of
// 21,857 | | `0x8cecd19741c6c1c7` | 84 | 8 of 15 | 80 of 21,864 | | `0xb7252004aba21c10` | 76 | 12
// of 15 -- a pass's view, not an object's | 98 of 235 |
//
// A camera's view matrix reads 15 of 15 because every draw of the scene pass holds the same one.
// These read **0 of 15**: at offset 12 and offset 4 every object holds its own twelve words, and
// the two offsets hold *the same* twelve words in two different shaders' layouts -- one array, read
// by fifteen objects, each getting its own copy.
//
// **So the offset is per shader, and the table is keyed on the shader's base *and* aux hashes.**
// The base alone is not a shader: keyed on it, this class's own denominator summed three shaders
// into one and reported 45,475 assemblies for a "shader" that is three, which is a bar nothing
// clears honestly.
//
// **What a refusal is for, and why this is not a default.** A shader this table has never seen, a
// shader whose evidence was below the bar, and a shader whose value is a pass's rather than an
// object's are three different facts and the caller may do something different with each. A default
// offset would answer all three with one number, and a blend that wrote twelve words at a wrong
// offset would corrupt a value that is not its own.
class PoseByShader {
  public:
    // **How many other objects must hold a different value before an offset is taken as an object's
    // own.** Measured: the per-object pose is 0 of 15 and the camera's is 12 of 15 or more, so any
    // bar between 1 and 12 separates them on this title. Twelve is the middle of the measured gap,
    // not a round number, and the bar is reported beside every refusal so a reader can see which of
    // the two a refusal was.
    static constexpr uint32_t kShareBar = 12;

    // The float count of the pose: three rows of four, a 3x4.
    static constexpr size_t kWords = 12;

    struct Entry {
        uint32_t byteOffset = 0;
        // Of how many other objects, how many read the same twelve words. A pass's value reads most
        // of them and an object's reads none.
        uint32_t otherObjects = 0;
        uint32_t otherObjectsSame = 0;
        // Of this shader's own assemblies holding the value at this offset, how many found it
        // changed. A value that never changes is a basis, not a pose.
        uint64_t moved = 0;
        uint64_t compared = 0;
    };

    // One candidate offered from the census, and the reason for it being refused is in `refused()`.
    // **A refusal is returned rather than thrown**: the caller is on a rendering path, and a table
    // that has not seen a shader yet is the ordinary state of one that is still filling.
    void offer(uint64_t shaderBaseHash, uint64_t shaderAuxHash, const Entry& entry,
               std::string& refusal);

    // The offset in bytes for a draw of this shader, or nullopt with `refusal` saying why not.
    //
    // **Not const, and the reason is the counters.** "Found 1 of 240 lookups" is what says a table
    // is being used and how often it is refusing, and a const method cannot keep that. A caller
    // holding `const PoseByShader&` gets a lookup it cannot count, which is the reading this class
    // exists to stop.
    std::optional<uint32_t> offsetFor(uint64_t shaderBaseHash, uint64_t shaderAuxHash,
                                      std::string& refusal);

    // How many of a pose's twelve words are numbers a lerp can combine. Zero means the entry is a
    // colour triple that looked like a transform, and a blend that treated it as a pose would
    // average three colours and call it a matrix.
    static uint32_t blendableWords(const float* words);

    // The offset is only usable for a blend if the value at it is a pose rather than a shape: a
    // candidate that never moved is a basis matrix and a value shared by most objects is a pass's.
    // Both refusals are in `refused()` with the counts beside them.
    std::string refused(const Entry& entry) const;

    std::string json() const;

    // **What a census hands over.** The same numbers, as the structure, so the table is fed from
    // what the measurement holds rather than from the report's own text -- reading a JSON report
    // back into a class is a second implementation of the fields the report already has.
    struct Offered {
        uint64_t shaderBaseHash = 0;
        uint64_t shaderAuxHash = 0;
        uint32_t byteOffset = 0;
        uint32_t otherObjects = 0;
        uint32_t otherObjectsSame = 0;
        uint64_t moved = 0;
        uint64_t compared = 0;
    };

    // How many candidates the last feed offered, took and refused, with the first refusal's text.
    //
    // **Every candidate is offered, including the ones expected to be refused**, so the table's
    // denominators are the census's: a table that silently dropped what it refused would report
    // "3 of 12" where the census says 12, and a reader dividing one by the other would get a number
    // neither said.
    //
    // **These are held rather than returned**, so the report carrying them is written by the class
    // that counted them. A caller that spliced them onto the end of the table's JSON would be a
    // second hand assembling one document, and a report that has grown two is exactly the fault
    // this project has already paid for once.
    struct Feed {
        uint64_t offered = 0;
        uint64_t accepted = 0;
        uint64_t refused = 0;
        std::string firstRefusal;
    };

    void feed(const Offered* candidates, size_t count);

    struct Tally {
        uint64_t offered = 0;
        uint64_t accepted = 0;
        uint64_t refusedShared = 0;   // most other objects read the same value: a pass's
        uint64_t refusedStill = 0;    // it never moved: a basis matrix
        uint64_t refusedNoOthers = 0; // nothing to compare it against
        uint64_t lookedUp = 0;
        uint64_t found = 0;
    };

    Tally tally() const {
        return m_tally;
    }

  private:
    // The key is the pair, for the reason the class comment gives.
    std::map<std::pair<uint64_t, uint64_t>, Entry> m_byShader;
    Tally m_tally;
    Feed m_lastFeed;
};

} // namespace wiiuport::title
