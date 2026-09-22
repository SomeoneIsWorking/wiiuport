#pragma once

#include "wiiuport/shell/ControllerAutoMap.h"
#include "wiiuport/shell/HostPaths.h"
#include "wiiuport/shell/ShellWindow.h"

#include <filesystem>
#include <string>

namespace wiiuport::shell {

// Brings up the emulated system, the window, the renderer and the title, runs
// until the host asks to stop, and takes it all down again.
//
// It composes; it does not implement. Window handling belongs to ShellWindow,
// guest execution to Cemu's core, and everything first-party answers for
// itself over the control channel.
class ShellHost final : public HostEventObserver {
  public:
    struct Options {
        // The running executable, as the host invoked it: the data shipped
        // beside it and portable mode are both found relative to it.
        std::filesystem::path executable;
        std::filesystem::path title;
        ShellWindow::Options window;
    };

    // The exit code the process returns. Every failure path reports what it
    // was before returning, so a non-zero exit is never unexplained.
    int run(const Options& options);

    // Forwards controller arrivals, departures and input to the emulated
    // controllers, which have no event loop of their own.
    void onHostEvent(SDL_Event& event) override;

  private:
    bool bringUpSystem();
    bool bringUpRenderer();
    bool launchTitle(const std::filesystem::path& path);
    void shutdown();

    HostPaths m_paths;
    ControllerAutoMap m_controllers;
    std::filesystem::path m_executable;
    ShellWindow m_window;
    bool m_titleRunning{false};
    std::string m_failure;
};

} // namespace wiiuport::shell
