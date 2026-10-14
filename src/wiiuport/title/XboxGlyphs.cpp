#include "wiiuport/title/XboxGlyphs.h"

#include "wiiuport/content/FontSheets.h"
#include "wiiuport/content/LayoutPanes.h"
#include "wiiuport/content/MessageText.h"
#include "wiiuport/content/SarcArchive.h"
#include "wiiuport/content/Yaz0.h"

#include <utility>

namespace wiiuport::title {
namespace {

using Art = glyphs::XboxGlyphArt;

constexpr std::string_view kFontFile = "CKingPic_bffnt.szs";
constexpr std::string_view kFontInner = "CKingPic.bffnt";
// Layouts with lettered button panes.
constexpr std::array<std::string_view, 3> kLayouts{"CommandA_00", "CommandGuide_00",
                                                   "IconSlideArea_00"};
// Message files whose labels name a lettered pane, alone or after the layout ("L_<layout>-<pane>").
constexpr std::array<std::string_view, 3> kMessages{"CommandGuide_00", "MenuItem_00",
                                                    "SlideAssistTV_00"};

// Decodes a Yaz0 SARC, lets `edit` change it, and encodes it again with its alignment.
template <typename Edit>
bool rewriteArchive(std::vector<uint8_t>& file, std::string_view name, std::string& refusal,
                    Edit edit) {
    auto decoded = content::Yaz0::decode(file, refusal);
    if (!decoded) {
        refusal = std::string(name) + ": " + refusal;
        return false;
    }
    auto archive = content::SarcArchive::parse(decoded->bytes, refusal);
    if (!archive || !edit(*archive)) {
        refusal = std::string(name) + ": " + refusal;
        return false;
    }
    file = content::Yaz0::encode(archive->write(), decoded->alignment);
    return true;
}

} // namespace

XboxGlyphs::XboxGlyphs(content::SurfaceTiling tiling) : m_tiling(std::move(tiling)) {
}

bool XboxGlyphs::patchFont(std::vector<uint8_t>& archive, std::string& refusal) const {
    return rewriteArchive(archive, kFontFile, refusal, [&](content::SarcArchive& sarc) {
        content::SarcArchive::File* font = sarc.find(kFontInner);
        if (font == nullptr) {
            refusal = "no " + std::string(kFontInner);
            return false;
        }
        auto sheets = content::FontSheets::parse(font->data, refusal);
        if (!sheets) {
            return false;
        }
        for (const FontGlyph& glyph : kFontGlyphs) {
            auto index = sheets->glyphIndex(glyph.codePoint);
            if (!index) {
                refusal =
                    "no glyph for U+" + std::to_string(static_cast<uint32_t>(glyph.codePoint));
                return false;
            }
            const content::RgbaImage cell =
                Art::draw(glyph.art, sheets->cellWidth(), sheets->cellHeight());
            if (!sheets->writeCell(*index, cell, m_tiling, refusal)) {
                return false;
            }
        }
        return true;
    });
}

bool XboxGlyphs::patchMessages(std::string_view messages, std::vector<uint8_t>& archive,
                               std::string& refusal) {
    return rewriteArchive(archive, messages, refusal, [&](content::SarcArchive& sarc) {
        const std::string path = std::string(messages) + ".msbt";
        content::SarcArchive::File* file = sarc.find(path);
        if (file == nullptr) {
            refusal = "no " + path;
            return false;
        }
        auto text = content::MessageText::parse(file->data, refusal);
        if (!text) {
            return false;
        }
        int changed = 0;
        for (const std::string& label : text->labels()) {
            const size_t dash = label.rfind('-');
            const std::string_view pane = dash == std::string::npos
                                              ? std::string_view(label)
                                              : std::string_view(label).substr(dash + 1);
            for (const PaneLetter& letter : kPaneLetters) {
                if (letter.pane == pane && text->text(label) == letter.wiiU) {
                    text->setText(label, letter.text);
                    ++changed;
                }
            }
        }
        if (changed == 0) {
            refusal = "no button letter";
            return false;
        }
        file->data = text->bytes();
        return true;
    });
}

bool XboxGlyphs::patchLayout(std::string_view layout, std::vector<uint8_t>& archive,
                             std::string& refusal) {
    return rewriteArchive(archive, layout, refusal, [&](content::SarcArchive& sarc) {
        const std::string path = "blyt/" + std::string(layout) + ".bflyt";
        content::SarcArchive::File* file = sarc.find(path);
        if (file == nullptr) {
            refusal = "no " + path;
            return false;
        }
        auto panes = content::LayoutPanes::parse(file->data, refusal);
        if (!panes) {
            return false;
        }
        int changed = 0;
        for (const PaneLetter& letter : kPaneLetters) {
            if (letter.width > 0) {
                changed += panes->setWidth(letter.pane, letter.width);
            }
            if (letter.color[3] != 0) {
                changed += panes->setWhite(letter.pane, letter.color);
            }
        }
        if (changed == 0) {
            refusal = "no lettered pane";
            return false;
        }
        file->data = panes->bytes();
        return true;
    });
}

std::optional<std::vector<uint8_t>> XboxGlyphs::patchPack(std::span<const uint8_t> pack,
                                                          std::string& refusal) const {
    auto archive = content::SarcArchive::parse(pack, refusal);
    if (!archive) {
        return std::nullopt;
    }
    content::SarcArchive::File* font = archive->find(kFontFile);
    if (font == nullptr) {
        refusal = "no " + std::string(kFontFile);
        return std::nullopt;
    }
    if (!patchFont(font->data, refusal)) {
        return std::nullopt;
    }
    for (std::string_view layout : kLayouts) {
        content::SarcArchive::File* file = archive->find(std::string(layout) + ".szs");
        if (file == nullptr) {
            refusal = "no " + std::string(layout) + ".szs";
            return std::nullopt;
        }
        if (!patchLayout(layout, file->data, refusal)) {
            return std::nullopt;
        }
    }
    for (std::string_view messages : kMessages) {
        content::SarcArchive::File* file = archive->find(std::string(messages) + "_msbt.szs");
        if (file == nullptr) {
            refusal = "no " + std::string(messages) + "_msbt.szs";
            return std::nullopt;
        }
        if (!patchMessages(messages, file->data, refusal)) {
            return std::nullopt;
        }
    }
    return archive->write();
}

} // namespace wiiuport::title
