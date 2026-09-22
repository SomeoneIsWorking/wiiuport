#include "wiiuport/shell/ShellWindow.h"

#include <lucent/log.h>

#include <SDL3/SDL.h>

namespace wiiuport::shell {
namespace {

// Cemu stores an X11 window id in a void*, which is how its own front end
// passes it. Kept in one place so the cast is described once.
void* asHandle(uint64_t id) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(id));
}

} // namespace

ShellWindow::~ShellWindow() {
    close();
}

bool ShellWindow::open(const Options& options) {
    if (m_window != nullptr) {
        m_lastError = "a window is already open";
        return false;
    }
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        m_lastError = std::string("SDL video would not start: ") + SDL_GetError();
        return false;
    }
    SDL_WindowFlags flags =
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (options.hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    m_window = SDL_CreateWindow(options.title.c_str(), options.width, options.height, flags);
    if (m_window == nullptr) {
        m_lastError = std::string("SDL would not create a window: ") + SDL_GetError();
        return false;
    }
    if (!publishNativeHandles()) {
        close();
        return false;
    }
    if (!publishGeometry()) {
        close();
        return false;
    }
    WindowSystem::GetWindowInfo().app_active = true;
    lucent::info("shell", "window open: {}x{} {}", options.width, options.height,
                 options.hidden ? "hidden" : "shown");
    return true;
}

void ShellWindow::close() {
    if (m_window != nullptr) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    WindowSystem::GetWindowInfo().app_active = false;
}

bool ShellWindow::publishNativeHandles() {
    SDL_PropertiesID properties = SDL_GetWindowProperties(m_window);
    if (properties == 0) {
        m_lastError = std::string("SDL exposed no window properties: ") + SDL_GetError();
        return false;
    }
    auto& info = WindowSystem::GetWindowInfo();
    const char* driver = SDL_GetCurrentVideoDriver();
    std::string_view name = driver != nullptr ? driver : "";

    WindowSystem::WindowHandleInfo handle{};
    if (name == "x11") {
        handle.backend = WindowSystem::WindowHandleInfo::Backend::X11;
        handle.display =
            SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        handle.surface = asHandle(static_cast<uint64_t>(
            SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)));
    } else if (name == "wayland") {
        handle.backend = WindowSystem::WindowHandleInfo::Backend::Wayland;
        handle.display =
            SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        handle.surface =
            SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        // Naming the driver matters: "unsupported" without it sends the
        // reader looking in the wrong place.
        m_lastError = "the SDL video driver '" + std::string(name) +
                      "' is not one this shell can hand to the renderer; it knows x11 and wayland";
        return false;
    }
    if (handle.display == nullptr || handle.surface == nullptr) {
        m_lastError = "the " + std::string(name) +
                      " window gave no native handle, so the renderer would draw nowhere";
        return false;
    }
    info.window_main = handle;
    info.canvas_main = handle;
    return true;
}

bool ShellWindow::publishGeometry() {
    int width = 0;
    int height = 0;
    int physicalWidth = 0;
    int physicalHeight = 0;
    if (!SDL_GetWindowSize(m_window, &width, &height) ||
        !SDL_GetWindowSizeInPixels(m_window, &physicalWidth, &physicalHeight)) {
        m_lastError = std::string("SDL would not report the window size: ") + SDL_GetError();
        return false;
    }
    auto& info = WindowSystem::GetWindowInfo();
    info.width = width;
    info.height = height;
    info.phys_width = physicalWidth;
    info.phys_height = physicalHeight;
    float scale = SDL_GetWindowDisplayScale(m_window);
    info.dpi_scale = scale > 0.0f ? static_cast<double>(scale) : 1.0;
    return true;
}

bool ShellWindow::pumpEvents(HostEventObserver& observer) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        observer.onHostEvent(event);
        switch (event.type) {
        case SDL_EVENT_QUIT:
            m_quitRequested = true;
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            m_quitRequested = true;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED:
            publishGeometry();
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            WindowSystem::GetWindowInfo().app_active = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            WindowSystem::GetWindowInfo().app_active = false;
            break;
        case SDL_EVENT_KEY_DOWN:
            WindowSystem::GetWindowInfo().set_keystate(event.key.scancode, true);
            break;
        case SDL_EVENT_KEY_UP:
            WindowSystem::GetWindowInfo().set_keystate(event.key.scancode, false);
            break;
        default:
            break;
        }
    }
    return !m_quitRequested;
}

void ShellWindow::setTitle(const std::string& title) {
    if (m_window != nullptr) {
        SDL_SetWindowTitle(m_window, title.c_str());
    }
}

bool ShellWindow::isFullscreen() const {
    if (m_window == nullptr) {
        return false;
    }
    return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_FULLSCREEN) != 0;
}

void ShellWindow::setFullscreen(bool fullscreen) {
    if (m_window != nullptr) {
        SDL_SetWindowFullscreen(m_window, fullscreen);
        WindowSystem::GetWindowInfo().is_fullscreen = fullscreen;
    }
}

} // namespace wiiuport::shell
