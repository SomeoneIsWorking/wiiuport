// The whole of Cemu's front-end interface, answered by this shell.
//
// Cemu's core reaches a host window only through these functions, so this
// file is the entire surface a replacement front end has to satisfy. Most of
// them are reads of the shared WindowInfo the window keeps current; the few
// that are not say plainly that this shell has no such thing rather than
// pretending to have done something.

#include "gui/interface/WindowSystem.h"

#include <lucent/log.h>

#include <SDL3/SDL.h>

namespace {

// Cemu's core takes a reference to this and keeps it for the process's life.
WindowSystem::WindowInfo g_windowInfo{};

} // namespace

namespace WindowSystem {

WindowInfo& GetWindowInfo() {
    return g_windowInfo;
}

void Create() {
    // Upstream's front end starts its own toolkit here and never returns.
    // This shell owns its lifecycle from main instead, so reaching this is a
    // wiring mistake and saying so beats doing nothing.
    lucent::error("shell", "WindowSystem::Create was called; this shell runs from main");
}

void ShowErrorDialog(std::string_view message, std::string_view title,
                     std::optional<ErrorCategory> /*category*/) {
    // No modal dialog: a run nobody is watching must not stop on one. The
    // message still has to arrive, so it goes where every other failure goes.
    lucent::error("shell", "{}{}{}", title, title.empty() ? "" : ": ", message);
}

void UpdateWindowTitles(bool /*isIdle*/, bool /*isLoading*/, double /*fps*/) {
    // The window's title is set once, by the shell. Rewriting it every frame
    // is a front-end flourish the product does not need.
}

// Cemu's interface returns each size through two int references. The
// signature is the fork's upstream contract, not this shell's to reshape.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void GetWindowSize(int& w, int& h) {
    w = g_windowInfo.width;
    h = g_windowInfo.height;
}

void GetPadWindowSize(int& w, int& h) {
    w = g_windowInfo.pad_width;
    h = g_windowInfo.pad_height;
}

void GetWindowPhysSize(int& w, int& h) {
    w = g_windowInfo.phys_width;
    h = g_windowInfo.phys_height;
}

void GetPadWindowPhysSize(int& w, int& h) {
    w = g_windowInfo.phys_pad_width;
    h = g_windowInfo.phys_pad_height;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

double GetWindowDPIScale() {
    return g_windowInfo.dpi_scale;
}

double GetPadDPIScale() {
    return g_windowInfo.pad_dpi_scale;
}

bool IsPadWindowOpen() {
    return g_windowInfo.pad_open;
}

bool IsKeyDown(uint32 key) {
    return g_windowInfo.get_keystate(key);
}

bool IsKeyDown(PlatformKeyCodes key) {
    switch (key) {
    case PlatformKeyCodes::LCONTROL:
        return g_windowInfo.get_keystate(static_cast<uint32>(SDL_SCANCODE_LCTRL));
    case PlatformKeyCodes::RCONTROL:
        return g_windowInfo.get_keystate(static_cast<uint32>(SDL_SCANCODE_RCTRL));
    case PlatformKeyCodes::TAB:
        return g_windowInfo.get_keystate(static_cast<uint32>(SDL_SCANCODE_TAB));
    case PlatformKeyCodes::ESCAPE:
        return g_windowInfo.get_keystate(static_cast<uint32>(SDL_SCANCODE_ESCAPE));
    }
    return false;
}

std::string GetKeyCodeName(uint32 key) {
    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(key));
    return name != nullptr ? std::string(name) : std::string();
}

bool InputConfigWindowHasFocus() {
    // There is no input configuration window; bindings are resolved by the
    // shell. Claiming focus would make the core swallow input for a window
    // that does not exist.
    return false;
}

void NotifyGameLoaded() {
}

void NotifyGameExited() {
}

void RefreshGameList() {
    // There is no game list: the shell is given one title to run.
}

bool IsFullScreen() {
    return g_windowInfo.is_fullscreen;
}

void CaptureInput(const ControllerState& /*currentState*/, const ControllerState& /*lastState*/) {
    // Only an input-configuration UI captures a raw controller state to bind
    // it. This shell binds by identity instead, so there is nothing to catch.
}

} // namespace WindowSystem
