#pragma once

#include "wiiuport/control/HostStop.h"
#include "wiiuport/shell/ControllerAutoMap.h"
#include "wiiuport/shell/HostPaths.h"
#include "wiiuport/shell/ShellWindow.h"
#include "wiiuport/shell/TitleIdentity.h"
#include "wiiuport/shell/TitleSelection.h"

#include <filesystem>
#include <optional>
#include <string>

namespace wiiuport::shell {

// Brings up the emulated system, the window, the renderer and the title, runs
// until the host asks to stop, and takes it all down again.
//
// It composes; it does not implement. Window handling belongs to ShellWindow,
// guest execution to Cemu's core, and everything first-party answers for
// itself over the control channel.
class ShellHost final : public HostEventObserver, public control::HostStopTarget {
  public:
    struct Options {
        // The running executable, as the host invoked it: the data shipped
        // beside it and portable mode are both found relative to it.
        std::filesystem::path executable;
        // Which title to run. Empty asks the remembered one, and then the
        // player: a packaged product has no command line.
        std::filesystem::path title;
        // The one title the launching product runs; none accepts any.
        std::optional<TitleIdentity> expectedTitle;
        ShellWindow::Options window;
    };

    // The exit code the process returns. Every failure path reports what it
    // was before returning, so a non-zero exit is never unexplained.
    int run(const Options& options);

    // Forwards controller arrivals, departures and input to the emulated
    // controllers, which have no event loop of their own.
    void onHostEvent(SDL_Event& event) override;

    // POST /quit: leaves the event loop as closing the window does.
    void requestStop() override;

  private:
    bool bringUpSystem();
    // The title to launch: the one asked for, the one remembered, or the one
    // the player chooses on the setup screen. Empty when the player asked for
    // none; m_failure is set only when something failed.
    std::filesystem::path resolveTitle(const Options& options);
    bool bringUpRenderer();
    bool launchTitle(const std::filesystem::path& path);
    // Why `path` cannot be run, or empty: not a mountable title, or not the
    // one expected.
    std::string titleProblem(const std::filesystem::path& path) const;
    void shutdown();

    HostPaths m_paths;
    ControllerAutoMap m_controllers;
    std::filesystem::path m_executable;
    std::optional<TitleIdentity> m_expectedTitle;
    ShellWindow m_window;
    bool m_titleRunning{false};
    std::string m_failure;
};

} // namespace wiiuport::shell
