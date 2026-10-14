#include "wiiuport/shell/GlyphPacks.h"

#include "wiiuport/title/XboxGlyphs.h"

#include "Cafe/Filesystem/fsc.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"

#include <lucent/content.h>
#include <lucent/log.h>

#include <fstream>
#include <vector>

namespace wiiuport::shell {
namespace {

constexpr sint32 kRedirectPriority = 0;

content::ElementOffset latteTiling(const content::SurfaceShape& shape) {
    LatteAddrLib::AddrSurfaceInfo_OUT info{};
    LatteAddrLib::GX2CalculateSurfaceInfo(
        static_cast<Latte::E_GX2SURFFMT>(shape.format), shape.width, shape.height, 1,
        Latte::E_DIM::DIM_2D, static_cast<Latte::E_GX2TILEMODE>(shape.tileMode), 0, 0, &info);
    const uint32 pitch = info.pitch;
    const uint32 height = info.height;
    const uint32 bpp = info.bpp;
    const Latte::E_HWTILEMODE tileMode = info.hwTileMode;
    const uint32 pipeSwizzle = (shape.swizzle >> 8) & 1;
    const uint32 bankSwizzle = (shape.swizzle >> 9) & 3;
    return [=](uint32_t x, uint32_t y) {
        return LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiled(
            x, y, 0, 0, bpp, pitch, height, 1, tileMode, false, pipeSwizzle, bankSwizzle);
    };
}

bool writeFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const std::filesystem::path partial = path.string() + ".partial";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            return false;
        }
    }
    std::filesystem::rename(partial, path, error);
    return !error;
}

} // namespace

void GlyphPacks::install(const std::filesystem::path& cache) {
    const title::XboxGlyphs glyphs(&latteTiling);
    int installed = 0;
    for (std::string_view pack : title::XboxGlyphs::kPacks) {
        auto source = fsc_extractFile(("/vol/" + std::string(pack)).c_str());
        if (!source) {
            lucent::warn("glyphs", "{} is not in the title; left as it is", pack);
            continue;
        }
        const std::string digest =
            lucent::content::sha256_hex(lucent::content::sha256(std::as_bytes(std::span(*source))));
        const std::filesystem::path built =
            cache / "glyphs" /
            (digest + "-r" + std::to_string(title::XboxGlyphs::kRevision) + ".pack");
        std::error_code error;
        if (!std::filesystem::exists(built, error)) {
            std::string refusal;
            auto patched = glyphs.patchPack(*source, refusal);
            if (!patched) {
                lucent::error("glyphs", "{}: {}; the disc's glyphs stay", pack, refusal);
                continue;
            }
            if (!writeFile(built, *patched)) {
                lucent::error("glyphs", "{} would not write; the disc's glyphs stay",
                              built.string());
                continue;
            }
            lucent::info("glyphs", "built {} ({} bytes)", built.string(), patched->size());
        }
        fscDeviceRedirect_map();
        fscDeviceRedirect_add("vol/" + std::string(pack), std::filesystem::file_size(built, error),
                              built, kRedirectPriority);
        ++installed;
    }
    lucent::info("glyphs", "Xbox glyphs in {} of {} language packs", installed,
                 title::XboxGlyphs::kPacks.size());
}

} // namespace wiiuport::shell
