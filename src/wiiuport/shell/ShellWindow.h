#pragma once

#include "gui/interface/WindowSystem.h"

#include <cstdint>
#include <string>

struct SDL_Window;
union SDL_Event;

namespace wiiuport::shell {

// Something else that needs the host's events. The window owns the loop
// because only one thread may drain SDL's queue, so anything else that needs
// an event is handed it here.
class HostEventObserver {
  public:
    virtual ~HostEventObserver() = default;
    virtual void onHostEvent(SDL_Event& event) = 0;
};

// The one window the product has, and the native handles the renderer needs
// from it.
//
// Cemu reaches the host window through the WindowSystem interface only, so
// this is the whole surface a front end has to satisfy. Owning it here rather
// than in a toolkit's widget tree is what lets the product run without one.
class ShellWindow {
  public:
    struct Options {
        std::string title{"wiiuport"};
        int width{1280};
        int height{720};
        // A window nobody is watching must not take the desktop's focus, and
        // an agent run must not depend on one being mapped at all.
        bool hidden{false};
    };

    ShellWindow() = default;
    ~ShellWindow();

    ShellWindow(const ShellWindow&) = delete;
    ShellWindow& operator=(const ShellWindow&) = delete;

    // False with a reason in lastError(): a window that failed to open must
    // not be reported as one nobody looked at.
    bool open(const Options& options);
    void close();

    bool isOpen() const {
        return m_window != nullptr;
    }

    const std::string& lastError() const {
        return m_lastError;
    }

    // Drains the host's events into the shared window state, handing each one
    // to the observer as well. False once the host has asked the product to
    // quit.
    bool pumpEvents(HostEventObserver& observer);

    void setTitle(const std::string& title);
    bool isFullscreen() const;
    void setFullscreen(bool fullscreen);

    SDL_Window* handle() const {
        return m_window;
    }

  private:
    // Fills the size, scale and native-handle fields Cemu's renderer reads.
    // Separate from opening because a resize has to redo exactly this.
    bool publishGeometry();
    bool publishNativeHandles();

    SDL_Window* m_window{nullptr};
    std::string m_lastError;
    bool m_quitRequested{false};
};

} // namespace wiiuport::shell
