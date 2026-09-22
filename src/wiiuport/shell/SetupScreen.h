#pragma once

#include "wiiuport/control/SetupStatus.h"
#include "wiiuport/shell/TitleSelection.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wiiuport::shell {

// The screen a player meets when no title is configured.
//
// It owns this product's side of the shared setup screen: what the screen
// says, which files the platform picker offers, and what a chosen file has to
// be. Presentation, layout and the staged-file state machine belong to
// setup-ui; whether a file is a title belongs to TitleSelection's validator.
class SetupScreen final : public control::SetupStatusSource {
  public:
    // What the screen is waiting for. Its own, rather than the shared
    // screen's type, because this is what the control channel promises a
    // client and it must not change underneath one.
    enum class State : std::uint8_t {
        Collecting,
        Ready,
        Rejected,
        Accepted,
    };

    struct Options {
        // A private directory the shared screen may use for its own
        // bookkeeping. Nothing is copied into it: a disc image is adopted
        // where the player keeps it.
        std::filesystem::path stagingRoot;
        bool hidden{false};
    };

    explicit SetupScreen(TitleSelection& selection);

    // Runs until the player starts from an accepted title or dismisses the
    // screen. Returns the chosen title, and an empty path when the player
    // dismissed it -- with lastError() empty in that case and set when the
    // screen could not be shown at all. The two are different outcomes and a
    // caller must be able to tell them apart.
    std::filesystem::path run(const Options& options);

    const std::string& lastError() const {
        return m_error;
    }

    // Answered on the channel's thread while this one draws, so everything
    // behind them is atomic.
    bool setupShown() const override;
    std::string setupState() const override;
    size_t setupSelectionsOffered() const override;

  private:
    // The platform picker's answer, delivered on this thread by SDL while the
    // screen is drawing.
    static void onFilesChosen(void* userdata, const char* const* files, int filter);

    TitleSelection& m_selection;
    std::vector<std::filesystem::path> m_chosen;
    std::filesystem::path m_accepted;
    bool m_browsing{false};
    std::string m_error;
    std::atomic_bool m_shown{false};
    std::atomic<State> m_state{State::Collecting};
    std::atomic_size_t m_selectionsOffered{0};
};

} // namespace wiiuport::shell
