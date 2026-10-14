// A language pack rebuilt with Xbox-layout glyphs, through the shipping plan and art.
#include "check.h"
#include "content_fixtures.h"
#include "suites.h"
#include "wiiuport/content/FontSheets.h"
#include "wiiuport/content/LayoutPanes.h"
#include "wiiuport/content/MessageText.h"
#include "wiiuport/content/SarcArchive.h"
#include "wiiuport/content/Yaz0.h"
#include "wiiuport/glyphs/XboxGlyphArt.h"
#include "wiiuport/title/XboxGlyphs.h"

#include <string>
#include <vector>

namespace {

using namespace wiiuport::content;
using wiiuport::glyphs::XboxGlyphArt;
using wiiuport::title::XboxGlyphs;
namespace fixtures = wiiuport::tests::fixtures;

constexpr uint32_t kCell = 54;

std::vector<uint8_t> titleFont() {
    std::vector<char32_t> codes;
    codes.reserve(XboxGlyphs::kFontGlyphs.size());
    for (const XboxGlyphs::FontGlyph& glyph : XboxGlyphs::kFontGlyphs) {
        codes.push_back(glyph.codePoint);
    }
    return fixtures::pictureFont(kCell, 4, 6, codes);
}

std::vector<uint8_t> layoutArchive(const std::string& name,
                                   const std::vector<fixtures::LayoutText>& texts) {
    return fixtures::yaz0Sarc({{"blyt/" + name + ".bflyt", fixtures::layoutOf(texts)}});
}

std::vector<uint8_t> languagePack(bool withIconSlideArea) {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files{
        {"CKingPic_bffnt.szs", fixtures::yaz0Sarc({{"CKingPic.bffnt", titleFont()}})},
        {"CommandA_00.szs", layoutArchive("CommandA_00", {{"T_A_01", u"A"}, {"T_A_00", u"A"}})},
        {"CommandGuide_00.szs", layoutArchive("CommandGuide_00", {{"T_Y_00", u"Y"},
                                                                  {"T_R_00", u"R"},
                                                                  {"T_B_00", u"B"},
                                                                  {"T_A_00", u"A"},
                                                                  {"T_X_00", u"X"},
                                                                  {"T_ZR_00", u"ZR"}})},
        {"Other_00.szs", std::vector<uint8_t>(40, 9)},
        {"CommandGuide_00_msbt.szs",
         fixtures::yaz0Sarc(
             {{"CommandGuide_00.msbt", fixtures::messagesOf({{"T_A_00", u"A"},
                                                             {"T_ZR_00", u"ZR"},
                                                             {"T_ACT_Command_14", u"Crouch"},
                                                             {"L_CommandA_00-T_A_00", u"A"}})}})},
        {"SlideAssistTV_00_msbt.szs",
         fixtures::yaz0Sarc({{"SlideAssistTV_00.msbt",
                              fixtures::messagesOf({{"L_IconSlideArea_00-T_X_00", u"X"}})}})},
        {"MenuItem_00_msbt.szs",
         fixtures::yaz0Sarc(
             {{"MenuItem_00.msbt", fixtures::messagesOf({{"L_IconSlideArea_00-T_Y_00", u"Y"},
                                                         {"L_IconSlideArea_00-T_R_00", u"R"}})}})},
    };
    if (withIconSlideArea) {
        files.emplace_back("IconSlideArea_00.szs",
                           layoutArchive("IconSlideArea_00",
                                         {{"T_X_00", u"X"}, {"T_R_00", u"R"}, {"T_Y_00", u"Y"}}));
    }
    return fixtures::sarcBytes(files, 0x80);
}

std::vector<uint8_t> inner(SarcArchive& pack, const std::string& name, const std::string& file) {
    std::string refusal;
    SarcArchive::File* packed = pack.find(name);
    if (packed == nullptr) {
        check::isTrue(false, name + " is in the pack");
        return {};
    }
    auto decoded = check::present(Yaz0::decode(packed->data, refusal), name + " decodes");
    auto archive = check::present(SarcArchive::parse(decoded.bytes, refusal), name + " parses");
    SarcArchive::File* found = archive.find(file);
    check::isTrue(found != nullptr, file + " is in " + name);
    return found == nullptr ? std::vector<uint8_t>{} : found->data;
}

void thePackIsRebuiltByPosition() {
    const XboxGlyphs glyphs(fixtures::linearTiling());
    std::string refusal;
    auto patched = glyphs.patchPack(languagePack(true), refusal);
    check::isTrue(patched.has_value(), "the pack is rebuilt: " + refusal);
    if (!patched) {
        return;
    }
    auto pack = check::present(SarcArchive::parse(*patched, refusal), "the pack parses");
    SarcArchive::File* other = pack.find("Other_00.szs");
    check::isTrue(other != nullptr && other->data == std::vector<uint8_t>(40, 9),
                  "a file without glyphs is copied");

    std::vector<uint8_t> font = inner(pack, "CKingPic_bffnt.szs", "CKingPic.bffnt");
    auto sheets = check::present(FontSheets::parse(font, refusal), "the font parses");
    const RgbaImage right =
        sheets.readCell(sheets.glyphIndex(U'').value_or(0), fixtures::linearTiling());
    const RgbaImage expected = XboxGlyphArt::draw(XboxGlyphs::kFontGlyphs[0].art, kCell, kCell);
    check::isTrue(right.pixels == expected.pixels, "the Wii U A cell holds the art for B");
    check::equal(right.at(27, 27)[3], 255, "the glyph is drawn");
    check::equal(right.at(0, 0)[3], 0, "outside the button is clear");
    bool red = false;
    for (size_t i = 0; i < right.pixels.size(); i += 4) {
        red = red || (right.pixels[i] > 200 && right.pixels[i + 1] < 90);
    }
    check::isTrue(red, "the B is red");

    auto guide = check::present(
        LayoutPanes::parse(inner(pack, "CommandGuide_00.szs", "blyt/CommandGuide_00.bflyt"),
                           refusal),
        "the guide layout parses");
    check::near(guide.width("T_R_00").value_or(0), XboxGlyphs::kTwoLetterWidth, 0,
                "R's box holds two letters");
    check::near(guide.width("T_A_00").value_or(0), 30, 0, "a one-letter box is left");
    const std::vector<uint8_t> guideLayout =
        inner(pack, "CommandGuide_00.szs", "blyt/CommandGuide_00.bflyt");
    check::isTrue(fixtures::whiteOf(guideLayout, "T_A_00") == XboxGlyphArt::kRed,
                  "the right face button's B is red");
    check::isTrue(fixtures::whiteOf(guideLayout, "T_B_00") == XboxGlyphArt::kGreen,
                  "the bottom's A green");
    check::isTrue(fixtures::whiteOf(guideLayout, "T_X_00") == XboxGlyphArt::kYellow,
                  "the top's Y yellow");
    check::isTrue(fixtures::whiteOf(guideLayout, "T_Y_00") == XboxGlyphArt::kBlue,
                  "the left's X blue");
    check::isTrue(fixtures::whiteOf(guideLayout, "T_R_00") ==
                      LayoutPanes::Color{255, 255, 255, 255},
                  "a shoulder keeps its colour");
    check::isTrue(fixtures::whiteOf(inner(pack, "CommandA_00.szs", "blyt/CommandA_00.bflyt"),
                                    "T_A_00") == XboxGlyphArt::kRed,
                  "the A prompt's layout too");
    auto slide = check::present(
        LayoutPanes::parse(inner(pack, "IconSlideArea_00.szs", "blyt/IconSlideArea_00.bflyt"),
                           refusal),
        "the item layout parses");
    check::near(slide.width("T_R_00").value_or(0), XboxGlyphs::kTwoLetterWidth, 0,
                "the item buttons' too");

    auto guideText = check::present(
        MessageText::parse(inner(pack, "CommandGuide_00_msbt.szs", "CommandGuide_00.msbt"),
                           refusal),
        "the guide messages parse");
    check::isTrue(guideText.text("T_A_00") == u"B", "the letter the title sets reads B");
    check::isTrue(guideText.text("T_ZR_00") == u"RT", "and ZR RT");
    check::isTrue(guideText.text("L_CommandA_00-T_A_00") == u"B", "a label after its layout");
    check::isTrue(guideText.text("T_ACT_Command_14") == u"Crouch", "an action is left as it was");
    auto itemText = check::present(
        MessageText::parse(inner(pack, "MenuItem_00_msbt.szs", "MenuItem_00.msbt"), refusal),
        "the item messages parse");
    check::isTrue(itemText.text("L_IconSlideArea_00-T_Y_00") == u"X", "the item button Y reads X");
    check::isTrue(itemText.text("L_IconSlideArea_00-T_R_00") == u"RB", "and R RB");
}

void aPackWithoutALayoutIsRefused() {
    const XboxGlyphs glyphs(fixtures::linearTiling());
    std::string refusal;
    check::isTrue(!glyphs.patchPack(languagePack(false), refusal), "refused");
    check::isTrue(refusal.find("IconSlideArea_00") != std::string::npos, "naming the layout");
}

} // namespace

void wiiuport::tests::runXboxGlyphsTests() {
    thePackIsRebuiltByPosition();
    aPackWithoutALayoutIsRefused();
}
