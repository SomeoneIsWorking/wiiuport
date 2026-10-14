#pragma once

#include "wiiuport/content/SurfaceTiling.h"
#include "wiiuport/glyphs/XboxGlyphArt.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wiiuport::title {

// Wind Waker HD's button glyphs, redrawn for an Xbox-layout pad by position: the right face
// button shows B, the bottom A, the top Y, the left X (the bindings in shell/ControllerAutoMap).
//
// The glyphs are in the 2D language packs: the message font's picture glyphs (CKingPic.bffnt)
// and the HUD's lettered text panes, whose letters the title sets from the layouts' message files
// by pane name and whose colour is the material named after the pane.
class XboxGlyphs {
  public:
    // Bumped whenever the art or the plan changes, so built packs are rebuilt.
    static constexpr int kRevision = 6;

    static constexpr std::array<std::string_view, 3> kPacks{
        "content/Common/Pack/permanent_2d_UsEnglish.pack",
        "content/Common/Pack/permanent_2d_UsFrench.pack",
        "content/Common/Pack/permanent_2d_UsSpanish.pack",
    };

    struct FontGlyph {
        char32_t codePoint;
        glyphs::XboxGlyphArt::Glyph art;
    };

    struct PaneLetter {
        std::string_view pane;
        std::u16string_view wiiU;
        std::u16string_view text;
        // The text box's width when the letter needs a wider one than the layout gives (0: kept).
        float width;
        // The letter's colour, in the material of the pane's name (alpha 0: kept).
        glyphs::XboxGlyphArt::Color color;
    };

    // By code point in CKingPic: E000-E003 A B X Y, E045/E046 + -, E080-E082 sticks, E083-E086
    // L R ZL ZR, E0A0-E0A7 a stick and the way to push it, E0A8 the A burst.
    static constexpr std::array<FontGlyph, 22> kFontGlyphs{{
        {U'',
         {.shape = glyphs::XboxGlyphArt::Shape::Face,
          .label = "B",
          .labelColor = glyphs::XboxGlyphArt::kRed}},
        {U'',
         {.shape = glyphs::XboxGlyphArt::Shape::Face,
          .label = "A",
          .labelColor = glyphs::XboxGlyphArt::kGreen}},
        {U'',
         {.shape = glyphs::XboxGlyphArt::Shape::Face,
          .label = "Y",
          .labelColor = glyphs::XboxGlyphArt::kYellow}},
        {U'',
         {.shape = glyphs::XboxGlyphArt::Shape::Face,
          .label = "X",
          .labelColor = glyphs::XboxGlyphArt::kBlue}},
        {U'',
         {.shape = glyphs::XboxGlyphArt::Shape::Menu, .labelColor = glyphs::XboxGlyphArt::kWhite}},
        {U'',
         {.shape = glyphs::XboxGlyphArt::Shape::View, .labelColor = glyphs::XboxGlyphArt::kWhite}},
        {U'', {.shape = glyphs::XboxGlyphArt::Shape::Stick}},
        {U'',
         {.shape = glyphs::XboxGlyphArt::Shape::Stick,
          .label = "LS",
          .labelColor = glyphs::XboxGlyphArt::kWhite}},
        {U'',
         {.shape = glyphs::XboxGlyphArt::Shape::Stick,
          .label = "RS",
          .labelColor = glyphs::XboxGlyphArt::kWhite}},
        {U'',
         {.shape = glyphs::XboxGlyphArt::Shape::Bumper,
          .label = "LB",
          .labelColor = glyphs::XboxGlyphArt::kWhite}},
        {U'',
         {.shape = glyphs::XboxGlyphArt::Shape::Bumper,
          .label = "RB",
          .labelColor = glyphs::XboxGlyphArt::kWhite}},
        {U'',
         {.shape = glyphs::XboxGlyphArt::Shape::Trigger,
          .label = "LT",
          .labelColor = glyphs::XboxGlyphArt::kWhite}},
        {U'',
         {.shape = glyphs::XboxGlyphArt::Shape::Trigger,
          .label = "RT",
          .labelColor = glyphs::XboxGlyphArt::kWhite}},
        {U'',
         {.shape = glyphs::XboxGlyphArt::Shape::Stick,
          .label = "LS",
          .labelColor = glyphs::XboxGlyphArt::kWhite,
          .push = glyphs::XboxGlyphArt::Push::Up}},
        {U'',
         {.shape = glyphs::XboxGlyphArt::Shape::Stick,
          .label = "LS",
          .labelColor = glyphs::XboxGlyphArt::kWhite,
          .push = glyphs::XboxGlyphArt::Push::Down}},
        {U'',
         {.shape = glyphs::XboxGlyphArt::Shape::Stick,
          .label = "LS",
          .labelColor = glyphs::XboxGlyphArt::kWhite,
          .push = glyphs::XboxGlyphArt::Push::Left}},
        {U'',
         {.shape = glyphs::XboxGlyphArt::Shape::Stick,
          .label = "LS",
          .labelColor = glyphs::XboxGlyphArt::kWhite,
          .push = glyphs::XboxGlyphArt::Push::Right}},
        {U'',
         {.shape = glyphs::XboxGlyphArt::Shape::Stick,
          .label = "RS",
          .labelColor = glyphs::XboxGlyphArt::kWhite,
          .push = glyphs::XboxGlyphArt::Push::Up}},
        {U'',
         {.shape = glyphs::XboxGlyphArt::Shape::Stick,
          .label = "RS",
          .labelColor = glyphs::XboxGlyphArt::kWhite,
          .push = glyphs::XboxGlyphArt::Push::Down}},
        {U'',
         {.shape = glyphs::XboxGlyphArt::Shape::Stick,
          .label = "RS",
          .labelColor = glyphs::XboxGlyphArt::kWhite,
          .push = glyphs::XboxGlyphArt::Push::Left}},
        {U'',
         {.shape = glyphs::XboxGlyphArt::Shape::Stick,
          .label = "RS",
          .labelColor = glyphs::XboxGlyphArt::kWhite,
          .push = glyphs::XboxGlyphArt::Push::Right}},
        {U'',
         {.shape = glyphs::XboxGlyphArt::Shape::Burst,
          .label = "B",
          .labelColor = glyphs::XboxGlyphArt::kRed}},
    }};

    // T_ZR_00's box, which holds two letters.
    static constexpr float kTwoLetterWidth = 38;

    static constexpr std::array<PaneLetter, 7> kPaneLetters{{
        {"T_A_00", u"A", u"B", 0, glyphs::XboxGlyphArt::kRed},
        {"T_A_01", u"A", u"B", 0, glyphs::XboxGlyphArt::kRed},
        {"T_B_00", u"B", u"A", 0, glyphs::XboxGlyphArt::kGreen},
        {"T_X_00", u"X", u"Y", 0, glyphs::XboxGlyphArt::kYellow},
        {"T_Y_00", u"Y", u"X", 0, glyphs::XboxGlyphArt::kBlue},
        {"T_R_00", u"R", u"RB", kTwoLetterWidth, {}},
        {"T_ZR_00", u"ZR", u"RT", 0, {}},
    }};

    explicit XboxGlyphs(content::SurfaceTiling tiling);

    // The pack with every glyph replaced, or a refusal naming the first file that did not match.
    std::optional<std::vector<uint8_t>> patchPack(std::span<const uint8_t> pack,
                                                  std::string& refusal) const;

  private:
    bool patchFont(std::vector<uint8_t>& archive, std::string& refusal) const;
    static bool patchMessages(std::string_view messages, std::vector<uint8_t>& archive,
                              std::string& refusal);
    static bool patchLayout(std::string_view layout, std::vector<uint8_t>& archive,
                            std::string& refusal);

    content::SurfaceTiling m_tiling;
};

} // namespace wiiuport::title
