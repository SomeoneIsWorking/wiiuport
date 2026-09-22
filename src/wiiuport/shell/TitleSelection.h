#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace wiiuport::shell {

// Which title this product runs, remembered between launches.
//
// A packaged product asks the player for their copy of the game once. This
// owns that answer: where it is written, whether what was written is still
// usable, and the reason when it is not. It does not know what makes a file a
// title -- that is the host's policy, injected as a validator -- so the
// remembered answer can be checked without an emulated system existing.
class TitleSelection {
  public:
    // Returns an empty string to accept the path, or the reason a player
    // should be shown to reject it.
    using Validator = std::function<std::string(const std::filesystem::path&)>;

    TitleSelection(std::filesystem::path configDirectory, Validator validator);

    // The remembered title, or an empty path when nothing usable is
    // remembered. lastError() then says which it was: never chosen, the
    // record unreadable, or the title no longer acceptable. A caller that
    // silently showed setup again would leave the player guessing why.
    std::filesystem::path remembered();

    // Validates and writes. False names the reason in lastError(), and the
    // previous record is left alone, because a rejected choice must not cost
    // the player the one that already worked.
    bool remember(const std::filesystem::path& title);

    std::string validate(const std::filesystem::path& title) const;

    // Where the answer is written. Public because a player who wants to
    // choose again is told this path.
    std::filesystem::path recordPath() const;

    const std::string& lastError() const {
        return m_error;
    }

  private:
    std::filesystem::path m_configDirectory;
    Validator m_validator;
    std::string m_error;
};

} // namespace wiiuport::shell
