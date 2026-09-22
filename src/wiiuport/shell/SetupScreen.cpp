#include "wiiuport/shell/SetupScreen.h"

#include <array>

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <lucent/log.h>
#include <setup_ui/setup_ui.h>

namespace wiiuport::shell {
namespace {

// What the player is asked for, in their words. A Wii U title is one file the
// player already has; there is no second requirement to tell it apart from,
// so the requirement is nameless and the validator decides.
setup_ui::Config buildConfig() {
    setup_ui::Config config;
    config.title = "wiiuport";
    config.message = "Choose your own copy of the game. It is remembered for next time.";
    config.hint = "A Wii U disc image: .wux, .wud, .iso or .wua, or a title's .rpx.";
    config.footer = "The file stays where it is. Nothing is copied.";
    config.files.push_back({"title", "", "Game disc image"});
    config.placement = setup_ui::Placement::Adopt;
    config.browse_label = "Choose your game";
    config.accepted_message = "That is the game. Starting it now.";
    return config;
}

SetupScreen::State translate(setup_ui::Status status) {
    switch (status) {
    case setup_ui::Status::Collecting:
        return SetupScreen::State::Collecting;
    case setup_ui::Status::Ready:
        return SetupScreen::State::Ready;
    case setup_ui::Status::Rejected:
        return SetupScreen::State::Rejected;
    case setup_ui::Status::Accepted:
        return SetupScreen::State::Accepted;
    }
    return SetupScreen::State::Collecting;
}

// Offered by the picker, not enforced by it: the validator, not an extension,
// decides what may be launched.
constexpr std::array<SDL_DialogFileFilter, 2> kFilters{
    SDL_DialogFileFilter{"Wii U titles", "wux;wud;iso;wua;rpx"},
    SDL_DialogFileFilter{"All files", "*"},
};

} // namespace

SetupScreen::SetupScreen(TitleSelection& selection) : m_selection(selection) {
}

bool SetupScreen::setupShown() const {
    return m_shown.load();
}

std::string SetupScreen::setupState() const {
    switch (m_state.load()) {
    case State::Collecting:
        return "collecting";
    case State::Ready:
        return "ready";
    case State::Rejected:
        return "rejected";
    case State::Accepted:
        return "accepted";
    }
    return "collecting";
}

size_t SetupScreen::setupSelectionsOffered() const {
    return m_selectionsOffered.load();
}

void SetupScreen::onFilesChosen(void* userdata, const char* const* files, int filter) {
    (void)filter;
    auto* screen = static_cast<SetupScreen*>(userdata);
    screen->m_browsing = false;
    if (files == nullptr) {
        // The picker itself failed, which is not the same as choosing
        // nothing, and the player is left looking at a screen that did not
        // move unless this is said out loud.
        lucent::error("setup", "the file picker would not open: {}", SDL_GetError());
        return;
    }
    if (files[0] == nullptr) {
        lucent::info("setup", "the player closed the file picker without choosing");
        return;
    }
    for (const char* const* file = files; *file != nullptr; ++file) {
        screen->m_chosen.emplace_back(*file);
        screen->m_selectionsOffered.fetch_add(1);
    }
}

std::filesystem::path SetupScreen::run(const Options& options) {
    m_error.clear();
    if (options.hidden) {
        m_error = "no title is configured, and a hidden run has nobody to ask; "
                  "pass the title on the command line";
        return {};
    }

    setup_ui::SessionOptions sessionOptions;
    sessionOptions.staging_root = options.stagingRoot;
    setup_ui::Session session(buildConfig(), sessionOptions,
                              [this](const std::vector<setup_ui::StagedFile>& staged) {
                                  if (staged.empty()) {
                                      return std::string("no file was chosen");
                                  }
                                  std::string problem = m_selection.validate(staged.front().path);
                                  if (problem.empty()) {
                                      m_accepted = staged.front().path;
                                  }
                                  return problem;
                              });

    setup_ui::ViewOptions viewOptions;
    viewOptions.window_title = "wiiuport";
    setup_ui::View view(session, viewOptions);
    if (!view.open()) {
        m_error = "the setup screen would not open: " + view.last_error();
        return {};
    }
    m_shown.store(true);

    while (view.running()) {
        for (const setup_ui::Request& request : view.poll()) {
            switch (request.kind) {
            case setup_ui::RequestKind::Browse:
                if (!m_browsing) {
                    m_browsing = true;
                    SDL_ShowOpenFileDialog(&SetupScreen::onFilesChosen, this, nullptr,
                                           kFilters.data(), static_cast<int>(kFilters.size()),
                                           nullptr, false);
                }
                break;
            case setup_ui::RequestKind::Start:
                if (!m_accepted.empty() && m_selection.remember(m_accepted)) {
                    view.finish();
                    view.close();
                    m_shown.store(false);
                    return m_accepted;
                }
                // Refusing here rather than starting anyway: a title the
                // product cannot remember is one the player would be asked
                // for again next launch with no explanation.
                lucent::error("setup", "the chosen title was not kept: {}",
                              m_selection.lastError());
                session.reset();
                break;
            case setup_ui::RequestKind::Cancel:
                view.close();
                m_shown.store(false);
                return {};
            }
        }
        // The picker answers on this thread while the screen draws, so its
        // files are collected after the frame that may have delivered them.
        view.frame();
        m_state.store(translate(session.status()));
        if (!m_chosen.empty()) {
            std::vector<std::filesystem::path> chosen;
            chosen.swap(m_chosen);
            std::string failure;
            if (session.add_selected(chosen, failure) == 0) {
                lucent::error("setup", "the chosen file was not taken: {}",
                              failure.empty() ? "it did not match what is needed" : failure);
            }
            session.validate_if_ready();
        }
    }
    view.close();
    m_shown.store(false);
    return {};
}

} // namespace wiiuport::shell
