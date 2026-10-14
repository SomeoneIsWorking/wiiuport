# Controllers

Player 1 is an emulated Pro Controller, and Wind Waker HD's Controller Selection screen answers
itself with it.

## Owners

- `shell/ControllerAutoMap`: owns SDL's event queue; sets player 1 to `EmulatedController::Type::Pro`
  (WPAD channel 0) at start and attaches the first SDL gamepad to it with 25 positional bindings.
  `GET /controllers`.
- `input/InputDriver`: the control channel's presses and sticks, through the fork's `InputHooks`
  source. Hold bits are GamePad bits; the fork's `ProController::s_hold_from_hook_buttons`
  translates them in `KPADRead`.
- `title/ProControllerChoice`: the Controller Selection bypass. `GET /controller-choice`.

## Controller Selection in the title

- File select always enters ControllerSelect; its enter `FUN_0270aaac` forces mode 3. No native skip
  exists.
- The screen's StateID_Display exec `FUN_026d440c` (screen in r3) on a Pro choice stores 1 at
  `screen+0x54`, asks `FUN_026d43dc` whether a Pro Controller is on KPAD channel 0, and if so calls
  `changeState(screen+0x18, StateID_OutAll 0x1049aaa4)` through `FUN_020063c0`. The scene's exec
  `FUN_0270ab10` then sets mode 0 (`FUN_02618094`) and moves to SaveDataSelect.
- Mode lives at `*(0x101f5088)+0x1d0`: 0 Pro, 1 GamePad TV, 2 Off-TV, 3 any, 4 initial.

## The bypass

At link time a momentary probe on `0x026d440c` allocates a 29-word block and a standing probe sends
every Display call there. The probe's stub runs the exec's `stwu`; the block runs the exec's own
prologue, the presence check, and the Pro path above with the exec's epilogue. With no Pro
Controller it restores the registers and link and branches to `0x026d4410`, so the frame runs as the
title wrote it. `changeState` must run in the guest: it calls the old state's exit and the new
state's enter through virtual calls immediately.

Pressing B on File Selection returns to Controller Selection, which confirms Pro again on its next
frame.

## Testing

- `tests/cxx/pro_controller_choice_tests.cpp`: every block word, the branch targets, the
  registration sequence and the out-of-reach refusal.
- Live: boot headless with a save and press only A; the run goes title, File Selection, gameplay,
  and `/controller-choice` reports `"probe":"installed"` with `displayFrames` 1.

## Button glyphs

The game shows Xbox-layout glyphs by position, matching the bindings above: the right face button
shows B, the bottom A, the top Y, the left X; R reads RB, ZR RT; L/ZL, the sticks and +/- are
LB/LT, LS/RS and Menu/View in the message font.

- Where the title keeps them: `content/Common/Pack/permanent_2d_<UsEnglish|UsFrench|UsSpanish>.pack`.
  - `CKingPic_bffnt.szs`: the picture font messages draw buttons from (`E000`-`E003` A B X Y,
    `E045`/`E046` + -, `E080`-`E086` sticks and shoulders, `E0A0`-`E0A7` a stick with the way to
    push it, `E0A8` the A burst). One 256x1024 RGBA8 sheet, 54x54 cells one pixel apart, GX2 2D
    tiled with swizzle 0, bottom row first.
  - The HUD's letters are text panes (`T_A_00`, `T_R_00`, ...) whose strings the title sets from the
    layouts' message files by label: `CommandGuide_00_msbt` (including `L_CommandA_00-T_A_00`),
    `MenuItem_00_msbt` and `SlideAssistTV_00_msbt` (`L_IconSlideArea_00-T_Y_00`). The text in the
    `.bflyt` itself is not what is shown. A letter's colour is the white of the material named after
    its pane (dark `1e1e1e` on the disc); the face letters get Xbox's colours there, in
    `CommandA_00`, `CommandGuide_00` and `IconSlideArea_00`.
  - `T_R_00` is 30 wide and wraps "RB"; it is widened to `T_ZR_00`'s 38 in `CommandGuide_00` and
    `IconSlideArea_00`.
- `title/XboxGlyphs` rebuilds a pack; `shell/GlyphPacks` builds each pack once into
  `<cache>/glyphs/<sha256 of the disc's pack>-r<kRevision>.pack` and redirects the title's path to
  it at priority 0, so a graphic pack replacing the same file wins. A pack that does not match the
  plan is logged and the disc's is used.
- The art is drawn by `glyphs/XboxGlyphArt`; no community mod's assets are used (they are NC-ND).

Testing: `tests/cxx/content_tests.cpp` (each format) and `tests/cxx/xbox_glyphs_tests.cpp` (a pack
rebuilt through the plan). Live: a headless run to Hyrule shows B on the right, A at the bottom, RB
and RT on the shoulders; decoding the built pack's sheet shows every replaced cell in place.

Not covered yet: the File Selection's and pause screens' prompts were not inspected one by one, and
the item buttons' X/Y letters were not seen on screen.
