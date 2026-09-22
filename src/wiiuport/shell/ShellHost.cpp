#include "wiiuport/shell/ShellHost.h"

#include "wiiuport/Runtime.h"
#include "wiiuport/shell/SetupScreen.h"

#include "Boot/SystemBringup.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/TitleList/TitleList.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "input/api/SDL/SDLControllerProvider.h"
#include "util/helpers/helpers.h"

#include <lucent/log.h>

#include <chrono>
#include <thread>

namespace wiiuport::shell {
namespace {

// How long the event loop sleeps between pumps. The guest runs on its own
// threads; this one only forwards host events, so it costs nothing to leave
// the core alone between them.
constexpr std::chrono::milliseconds kEventPollInterval{4};

// What makes a file a title this system can mount. One owner, because the
// setup screen and the launch path must agree: a file the screen accepted and
// the launcher then refused would be a dead end the player cannot escape.
std::string describeTitleProblem(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path)) {
        return path.string() + " is not a file";
    }
    TitleInfo title(path);
    if (!title.IsValid()) {
        return path.string() + " is not a title this system can mount";
    }
    return {};
}

} // namespace

int ShellHost::run(const Options& options) {
    m_executable = options.executable;
    if (!bringUpSystem()) {
        lucent::error("shell", "{}", m_failure);
        return 1;
    }
    std::filesystem::path title = resolveTitle(options);
    if (title.empty()) {
        if (!m_failure.empty()) {
            lucent::error("shell", "{}", m_failure);
            shutdown();
            return 1;
        }
        lucent::info("shell", "the player chose no title; nothing to run");
        shutdown();
        return 0;
    }
    if (!m_window.open(options.window)) {
        lucent::error("shell", "{}", m_window.lastError());
        shutdown();
        return 1;
    }
    if (!bringUpRenderer()) {
        lucent::error("shell", "{}", m_failure);
        shutdown();
        return 1;
    }
    if (!launchTitle(title)) {
        lucent::error("shell", "{}", m_failure);
        shutdown();
        return 1;
    }
    // The channel can now answer what is plugged in, and says so explicitly
    // while nothing is.
    Runtime::instance().control().setControllerStatus(&m_controllers);
    m_controllers.start();
    lucent::info("shell", "running {}", title.string());
    while (m_window.pumpEvents(*this)) {
        std::this_thread::sleep_for(kEventPollInterval);
    }
    lucent::info("shell", "host asked to stop");
    shutdown();
    return 0;
}

void ShellHost::onHostEvent(SDL_Event& event) {
    // This thread owns SDL's queue, so the controller provider is handed the
    // events it would otherwise have waited for on a thread of its own.
    SDLControllerProvider::HandleHostEvent(event);
    m_controllers.handleEvent(event);
}

bool ShellHost::bringUpSystem() {
    // This host has an SDL window, so it drains SDL's queue itself; the
    // controller provider must not start a second loop over the same queue.
    SDLControllerProvider::SetHostOwnsEventLoop(true);
    SDLControllerProvider::InitSDL();
    if (!m_paths.publish(m_executable)) {
        m_failure = m_paths.lastError();
        return false;
    }
    // Everything the core needs, in the order it needs it, from the one owner
    // both front ends share.
    SystemBringup::Run();
    if (!m_paths.prepareNand()) {
        m_failure = m_paths.lastError();
        return false;
    }
    ActiveSettings::Init();
    LatteOverlay_init();
    return true;
}

std::filesystem::path ShellHost::resolveTitle(const Options& options) {
    TitleSelection selection(ActiveSettings::GetConfigPath(), describeTitleProblem);
    if (!options.title.empty()) {
        // An explicitly given title is still the player's answer, so it is
        // remembered; a refusal to keep it is reported but does not stop this
        // run, which can still launch what was asked for.
        if (!selection.remember(options.title)) {
            lucent::warn("shell", "{}", selection.lastError());
        }
        return options.title;
    }
    std::filesystem::path remembered = selection.remembered();
    if (!remembered.empty()) {
        return remembered;
    }
    lucent::info("shell", "{}", selection.lastError());
    SetupScreen screen(selection);
    SetupScreen::Options screenOptions;
    screenOptions.stagingRoot = ActiveSettings::GetCachePath("setup");
    screenOptions.hidden = options.window.hidden;
    // The screen blocks this thread until the player answers, so an agent
    // asks the channel what it is waiting for rather than watching a window.
    Runtime::instance().control().setSetupStatus(&screen);
    std::filesystem::path chosen = screen.run(screenOptions);
    Runtime::instance().control().setSetupStatus(nullptr);
    if (chosen.empty() && !screen.lastError().empty()) {
        m_failure = screen.lastError();
    }
    return chosen;
}

bool ShellHost::bringUpRenderer() {
    // The window's native handle is already published, which is the only
    // thing the renderer needs from a front end.
    int width = 0;
    int height = 0;
    WindowSystem::GetWindowPhysSize(width, height);
    // Loading the Vulkan entry points is the front end's job. Without this the
    // global function pointers stay null and the first call into the loader is
    // a jump to address zero rather than a diagnosable failure.
    if (!InitializeGlobalVulkan() || !g_vulkan_available) {
        m_failure = "the Vulkan loader would not initialise: no usable Vulkan driver was found";
        return false;
    }
    // Interpolation presents twice per title frame and leaves the spacing to
    // the display: FIFO puts each present on its own vblank. Immediate or
    // mailbox presentation would show the two back to back and drop one.
    if (Runtime::instance().continuous().enabled()) {
        GetConfig().vsync = static_cast<int>(SwapchainInfoVk::VSync::FIFO);
    }
    try {
        g_renderer = std::make_unique<VulkanRenderer>();
        VulkanRenderer::GetInstance()->InitializeSurface({width, height}, true);
    } catch (const std::exception& failure) {
        m_failure = std::string("the Vulkan renderer would not start: ") + failure.what();
        return false;
    }
    return true;
}

bool ShellHost::launchTitle(const std::filesystem::path& path) {
    std::string problem = describeTitleProblem(path);
    if (!problem.empty()) {
        m_failure = problem;
        return false;
    }
    CafeTitleList::AddTitleFromPath(path);
    TitleInfo title(path);
    TitleId baseTitleId;
    if (!CafeTitleList::FindBaseTitleId(title.GetAppTitleId(), baseTitleId)) {
        m_failure = path.string() + " has no base title; an update or DLC cannot be launched alone";
        return false;
    }
    CafeSystem::PREPARE_STATUS_CODE status = CafeSystem::PrepareForegroundTitle(baseTitleId);
    if (status != CafeSystem::PREPARE_STATUS_CODE::SUCCESS) {
        // The code names which of several unrelated failures happened, and
        // dropping it would make them one indistinguishable "did not launch".
        m_failure = path.string() + " would not prepare (status " +
                    std::to_string(static_cast<int>(status)) + ")";
        return false;
    }
    CafeSystem::LaunchForegroundTitle();
    m_titleRunning = true;
    return true;
}

void ShellHost::shutdown() {
    if (m_titleRunning) {
        CafeSystem::ShutdownTitle();
        m_titleRunning = false;
    }
    m_window.close();
    g_renderer.reset();
}

} // namespace wiiuport::shell
