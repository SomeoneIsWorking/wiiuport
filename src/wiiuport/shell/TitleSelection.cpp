#include "wiiuport/shell/TitleSelection.h"

#include <fstream>
#include <system_error>
#include <utility>

namespace wiiuport::shell {
namespace {

// One line of text rather than a settings key, because this is the one answer
// the product needs before it has anywhere to keep settings.
constexpr const char* kRecordName = "selected-title.txt";

} // namespace

TitleSelection::TitleSelection(std::filesystem::path configDirectory, Validator validator)
    : m_configDirectory(std::move(configDirectory)), m_validator(std::move(validator)) {
}

std::filesystem::path TitleSelection::recordPath() const {
    return m_configDirectory / kRecordName;
}

std::string TitleSelection::validate(const std::filesystem::path& title) const {
    if (title.empty()) {
        return "no file was chosen";
    }
    return m_validator(title);
}

std::filesystem::path TitleSelection::remembered() {
    std::filesystem::path record = recordPath();
    std::error_code status;
    if (!std::filesystem::is_regular_file(record, status)) {
        m_error = "no title has been chosen yet (" + record.string() + " does not exist)";
        return {};
    }
    std::ifstream stream(record);
    std::string line;
    if (!std::getline(stream, line) || line.empty()) {
        m_error = record.string() + " is empty, so the remembered title cannot be read";
        return {};
    }
    std::filesystem::path title(line);
    std::string problem = validate(title);
    if (!problem.empty()) {
        m_error = "the remembered title " + title.string() + " cannot be used: " + problem;
        return {};
    }
    m_error.clear();
    return title;
}

bool TitleSelection::remember(const std::filesystem::path& title) {
    std::string problem = validate(title);
    if (!problem.empty()) {
        m_error = problem;
        return false;
    }
    std::error_code status;
    std::filesystem::create_directories(m_configDirectory, status);
    if (status) {
        m_error = "the chosen title cannot be remembered: " + m_configDirectory.string() + " (" +
                  status.message() + ")";
        return false;
    }
    std::filesystem::path record = recordPath();
    std::ofstream stream(record, std::ios::trunc);
    stream << title.string() << '\n';
    stream.flush();
    if (!stream) {
        m_error = "the chosen title cannot be written to " + record.string();
        return false;
    }
    m_error.clear();
    return true;
}

} // namespace wiiuport::shell
