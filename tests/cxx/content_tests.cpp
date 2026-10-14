// The Wii U content formats the glyph packs are rebuilt through.
#include "check.h"
#include "content_fixtures.h"
#include "suites.h"
#include "wiiuport/content/BigEndian.h"
#include "wiiuport/content/FontSheets.h"
#include "wiiuport/content/LayoutPanes.h"
#include "wiiuport/content/MessageText.h"
#include "wiiuport/content/SarcArchive.h"
#include "wiiuport/content/Yaz0.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace wiiuport::content;
using wiiuport::tests::fixtures::linearTiling;

void yaz0RoundTripsAndCompresses() {
    std::vector<uint8_t> bytes;
    bytes.reserve(20000);
    for (int i = 0; i < 20000; ++i) {
        bytes.push_back(static_cast<uint8_t>((i % 97) < 50 ? i % 7 : (i * 31) & 0xff));
    }
    const std::vector<uint8_t> encoded = Yaz0::encode(bytes, 0x2000);
    std::string refusal;
    auto decoded = Yaz0::decode(encoded, refusal);
    check::isTrue(decoded.has_value() && decoded->bytes == bytes, "Yaz0 decodes what it encoded");
    check::equal(decoded ? decoded->alignment : 0, 0x2000, "the alignment is kept");
    check::isTrue(encoded.size() < bytes.size() / 2, "repeats are copied, not stored");
    check::isTrue(!Yaz0::decode(std::vector<uint8_t>{'Y', 'a', 'z'}, refusal), "a short header");
}

void yaz0ReadsLongCopies() {
    // One literal, then a copy of 0x12 + 0x10 bytes from one back.
    const std::vector<uint8_t> encoded{'Y', 'a', 'z', '0', 0, 0,    0, 0x23, 0,    0,   0,
                                       0,   0,   0,   0,   0, 0x80, 7, 0x00, 0x00, 0x10};
    std::string refusal;
    auto decoded = Yaz0::decode(encoded, refusal);
    check::isTrue(decoded && decoded->bytes == std::vector<uint8_t>(0x23, 7), "a long copy");
}

void sarcKeepsNamesDataAndAlignment() {
    SarcArchive archive = wiiuport::tests::fixtures::archiveOf(
        {{"b.bin", std::vector<uint8_t>(5, 2)}, {"a.bin", std::vector<uint8_t>(0x30, 1)}}, 0x100);
    std::string refusal;
    SarcArchive::File* b = archive.find("b.bin");
    check::isTrue(b != nullptr, "found by name");
    b->data.assign(9, 3);
    auto again = SarcArchive::parse(archive.write(), refusal);
    check::isTrue(again.has_value(), "the written archive parses");
    if (!again) {
        return;
    }
    check::equal(again->files().size(), 2, "both files");
    check::isTrue(again->find("b.bin")->data == std::vector<uint8_t>(9, 3), "the new data");
    check::isTrue(again->find("a.bin")->data == std::vector<uint8_t>(0x30, 1), "the other data");
    check::isTrue(again->find("a.bin")->alignment >= 0x100 &&
                      again->find("b.bin")->alignment >= 0x100,
                  "each file where its alignment puts it");
    check::isTrue(!SarcArchive::parse(std::vector<uint8_t>(0x40, 0), refusal), "not SARC");
}

void fontCellsAreStoredBottomRowFirst() {
    std::vector<uint8_t> font = wiiuport::tests::fixtures::pictureFont(4, 2, 2);
    std::string refusal;
    auto sheets = FontSheets::parse(font, refusal);
    check::isTrue(sheets.has_value(), "the font parses: " + refusal);
    if (!sheets) {
        return;
    }
    check::equal(sheets->glyphIndex(U'').value_or(99), 1, "the code point's glyph");
    check::isTrue(!sheets->glyphIndex(U'Q'), "a code point the font lacks");
    RgbaImage cell(4, 4);
    cell.at(0, 0)[0] = 0xaa;
    cell.at(3, 3)[1] = 0xbb;
    check::isTrue(sheets->writeCell(1, cell, linearTiling(), refusal), "the cell is written");
    // Glyph 1 is column 1 of an 11x11 sheet: x 6, top row y 1, stored bottom row first.
    const size_t sheet = wiiuport::tests::fixtures::kSheetData;
    check::equal(font[sheet + ((((size_t{11} - 1 - 1) * 11) + 6) * 4)], 0xaa,
                 "its top left, stored low");
    check::equal(font[sheet + ((((size_t{11} - 1 - 4) * 11) + 9) * 4) + 1], 0xbb,
                 "its bottom right");
    const RgbaImage back = sheets->readCell(1, linearTiling());
    check::isTrue(back.pixels == cell.pixels, "read back as written");
    check::isTrue(!sheets->writeCell(1, RgbaImage(3, 4), linearTiling(), refusal),
                  "a cell of another size");
}

void layoutBoxesAreWidenedByName() {
    std::string refusal;
    auto panes = LayoutPanes::parse(
        wiiuport::tests::fixtures::layoutOf({{"T_R_00", u"R"}, {"T_A_00", u"A"}, {"T_R_00", u"R"}}),
        refusal);
    check::isTrue(panes.has_value(), "the layout parses: " + refusal);
    if (!panes) {
        return;
    }
    check::near(panes->width("T_R_00").value_or(0), 30, 0, "the box's width");
    check::equal(panes->setWidth("T_R_00", 38), 2, "every pane of the name");
    check::near(panes->width("T_R_00").value_or(0), 38, 0, "widened");
    check::near(panes->width("T_A_00").value_or(0), 30, 0, "another pane is left");
    check::equal(panes->setWidth("T_None", 38), 0, "no pane of that name");
    check::equal(panes->setWhite("T_A_00", {1, 2, 3, 4}), 1, "a material's white");
    check::isTrue(wiiuport::tests::fixtures::whiteOf(panes->bytes(), "T_A_00") ==
                      LayoutPanes::Color{1, 2, 3, 4},
                  "is written");
    check::isTrue(wiiuport::tests::fixtures::whiteOf(panes->bytes(), "T_R_00") ==
                      LayoutPanes::Color{255, 255, 255, 255},
                  "another is left");
    check::isTrue(!LayoutPanes::parse(std::vector<uint8_t>(0x20, 0), refusal), "not FLYT");
}

void messagesAreRewrittenByLabel() {
    std::string refusal;
    auto text =
        MessageText::parse(wiiuport::tests::fixtures::messagesOf(
                               {{"T_R_00", u"R"}, {"L_X-T_Y_00", u"Y"}, {"T_C", u"Crouch"}}),
                           refusal);
    check::isTrue(text.has_value(), "the message file parses: " + refusal);
    if (!text) {
        return;
    }
    check::equal(text->labels().size(), 3, "every label");
    check::isTrue(text->text("L_X-T_Y_00") == u"Y", "a message by label");
    check::isTrue(text->setText("T_R_00", u"RB"), "a longer message");
    check::isTrue(!text->setText("T_None", u"Q"), "no message of that label");
    auto again = MessageText::parse(text->bytes(), refusal);
    check::isTrue(again && again->text("T_R_00") == u"RB", "read back");
    check::isTrue(again && again->text("T_C") == u"Crouch", "the messages after it moved intact");
    check::equal(readU32(text->bytes(), 0x12), text->bytes().size(), "the file size follows");
    check::equal(text->bytes().size() % 16, 0, "sections stay padded to sixteen");
}

} // namespace

void wiiuport::tests::runContentTests() {
    messagesAreRewrittenByLabel();
    yaz0RoundTripsAndCompresses();
    yaz0ReadsLongCopies();
    sarcKeepsNamesDataAndAlignment();
    fontCellsAreStoredBottomRowFirst();
    layoutBoxesAreWidenedByName();
}
