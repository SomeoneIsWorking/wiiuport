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
