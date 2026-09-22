#include "wiiuport/shell/HostPaths.h"

#include "Cafe/Filesystem/MlcStorage.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <lucent/log.h>

#include <set>

namespace wiiuport::shell {
namespace {

// The runtime reads and writes the installation an existing Cemu on this
// machine already owns: the title keys, the emulated NAND, and the graphic
// packs all live there, and a private directory would silently start the
// player from nothing.
constexpr const char* kApplicationFolder = "Cemu";

// A directory of this name beside the executable makes the whole installation
// portable, as it does upstream. It is what a packaged build uses.
constexpr const char* kPortableFolder = "portable";

std::filesystem::path userDataDirectory() {
    // SDL resolves XDG_DATA_HOME on Linux, Application Support on macOS, and
    // the roaming app-data folder on Windows.
    char* preferences = SDL_GetPrefPath(nullptr, kApplicationFolder);
    if (preferences == nullptr) {
        return {};
    }
    std::filesystem::path path(preferences);
    SDL_free(preferences);
    return path;
}

// Linux keeps configuration and caches apart from data; the other hosts do
// not, and putting them together there is what the platform expects.
std::filesystem::path xdgDirectory(const char* variable, const char* fallback,
                                   const std::filesystem::path& userData) {
#if defined(__linux__) || defined(__FreeBSD__)
    const char* configured = SDL_getenv(variable);
    if (configured != nullptr && configured[0] != '\0') {
        return std::filesystem::path(configured) / kApplicationFolder;
    }
    const char* home = SDL_getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / fallback / kApplicationFolder;
    }
#else
    (void)variable;
    (void)fallback;
#endif
    return userData;
}

} // namespace

bool HostPaths::publish(const std::filesystem::path& executablePath) {
    std::error_code resolution;
    std::filesystem::path executable =
        std::filesystem::weakly_canonical(executablePath, resolution);
    std::filesystem::path directory = executable.parent_path();
    if (directory.empty()) {
        m_error = "cannot tell where " + executablePath.string() + " lives";
        return false;
    }
    std::filesystem::path portable = directory / kPortableFolder;
    bool isPortable = std::filesystem::is_directory(portable);

    std::filesystem::path userData = isPortable ? portable : userDataDirectory();
    if (userData.empty()) {
        m_error = "the host would not say where user data belongs";
        return false;
    }
    std::filesystem::path config =
        isPortable ? portable : xdgDirectory("XDG_CONFIG_HOME", ".config", userData);
    std::filesystem::path cache =
        isPortable ? portable : xdgDirectory("XDG_CACHE_HOME", ".cache", userData);

    std::set<std::filesystem::path> failedWriteAccess;
    ActiveSettings::SetPaths(isPortable, executable, userData, config, cache, directory,
                             failedWriteAccess);
    if (!failedWriteAccess.empty()) {
        m_error = "cannot write to " + failedWriteAccess.begin()->string();
        return false;
    }
    GetConfigHandle().SetFilename(ActiveSettings::GetConfigPath("settings.xml").generic_wstring());
    lucent::info("shell", "settings in {}, data in {}", config.string(), userData.string());
    return createHostDirectories();
}

bool HostPaths::prepareNand() {
    std::filesystem::path mlc = ActiveSettings::GetMlcPath();
    if (mlc.empty()) {
        m_error = "no emulated NAND path is configured";
        return false;
    }
    if (!MlcStorage::CreateDefaultFiles(mlc)) {
        m_error = "cannot prepare the emulated NAND at " + mlc.string();
        return false;
    }
    return true;
}

bool HostPaths::createHostDirectories() {
    std::error_code failure;
    for (const std::filesystem::path& directory :
         {ActiveSettings::GetConfigPath("controllerProfiles"),
          ActiveSettings::GetUserDataPath("memorySearcher")}) {
        if (std::filesystem::exists(directory, failure)) {
            continue;
        }
        if (!std::filesystem::create_directories(directory, failure)) {
            m_error = "cannot create " + directory.string() + ": " + failure.message();
            return false;
        }
    }
    return true;
}

} // namespace wiiuport::shell
