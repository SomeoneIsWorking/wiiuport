#pragma once

#include <filesystem>
#include <string>

namespace wiiuport::shell {

// Where this host keeps the player's settings, saves, and caches, and the
// read-only data shipped beside the executable.
//
// This is the shell's one owner of environment-derived paths: nothing else in
// the shell reads XDG variables or guesses a directory. It publishes what it
// resolves to ActiveSettings once, before any configuration is loaded, which
// is the order the core requires.
class HostPaths {
  public:
    // Resolves and publishes the paths, then creates the directories the core
    // expects to already exist. Takes the running executable's path because
    // the data shipped beside it, and portable mode, are both relative to it.
    // Returns false and names the directory when one of them is not writable,
    // because a run that continues from there loses the player's saves and
    // settings without saying so.
    bool publish(const std::filesystem::path& executablePath);

    // Creates the emulated NAND layout. Separate from publish because the
    // NAND location can be overridden in the settings, so this must run after
    // the configuration is loaded.
    bool prepareNand();

    const std::string& lastError() const {
        return m_error;
    }

  private:
    bool createHostDirectories();

    std::string m_error;
};

} // namespace wiiuport::shell
