#pragma once

#include <filesystem>
#include <string>

namespace wiiuport::shell {

// Puts the title's 2D language packs with Xbox-layout glyphs in front of the disc's.
//
// Each pack is read from the mounted title, rebuilt once into the cache (named by the source's
// hash and the plan's revision), and redirected at the lowest priority, so a graphic pack that
// replaces the same file still wins.
class GlyphPacks {
  public:
    // After the title is prepared and before it launches. A pack that cannot be built is logged
    // and left as the disc has it.
    void install(const std::filesystem::path& cache);
};

} // namespace wiiuport::shell
