#include "check.h"
#include "suites.h"
#include "wiiuport/shell/TitleIdentity.h"
#include "wiiuport/shell/TitleSelection.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

using wiiuport::shell::TitleIdentity;
using wiiuport::shell::TitleSelection;

namespace {

// A directory of this test's own, emptied first so a previous run's record
// cannot be mistaken for this one's.
std::filesystem::path freshConfigDirectory() {
    std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "wiiuport-title-selection-tests";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return directory;
}

std::filesystem::path writeFile(const std::filesystem::path& path) {
    std::ofstream stream(path, std::ios::trunc);
    stream << "not really a disc image";
    return path;
}

TitleSelection::Validator accepting() {
    return [](const std::filesystem::path&) {
        return std::string();
    };
}

TitleSelection::Validator refusing(std::string reason) {
    return [reason](const std::filesystem::path&) {
        return reason;
    };
}

void nothingIsRememberedUntilSomethingIsChosen() {
    std::filesystem::path directory = freshConfigDirectory();
    TitleSelection selection(directory, accepting());
    check::isTrue(selection.remembered().empty(),
                  "a product that has never been set up has no title");
    // The negative has to say which of the several nothings it is, or the
    // player is left with a setup screen and no reason for it.
    check::isTrue(selection.lastError().find("no title has been chosen") != std::string::npos,
                  "and says that nothing was ever chosen");
    check::isTrue(selection.lastError().find(selection.recordPath().string()) != std::string::npos,
                  "naming where it looked");
}

void aChosenTitleSurvivesTheProcess() {
    std::filesystem::path directory = freshConfigDirectory();
    std::filesystem::path title = writeFile(directory / "game.wux");
    TitleSelection first(directory, accepting());
    check::isTrue(first.remember(title), "a validated title is kept");
    TitleSelection second(directory, accepting());
    check::equal(second.remembered().string(), title.string(),
                 "and the next launch runs it without asking");
}

void aRefusedTitleIsNotKept() {
    std::filesystem::path directory = freshConfigDirectory();
    std::filesystem::path good = writeFile(directory / "good.wux");
    TitleSelection keeper(directory, accepting());
    keeper.remember(good);

    std::filesystem::path bad = writeFile(directory / "bad.txt");
    TitleSelection refuser(directory, refusing("that is not a title"));
    check::isTrue(!refuser.remember(bad), "a title the host refuses is not kept");
    check::equal(refuser.lastError(), std::string("that is not a title"),
                 "and the host's reason is what the player is shown");

    TitleSelection reader(directory, accepting());
    check::equal(reader.remembered().string(), good.string(),
                 "and the title that already worked is still the one remembered");
}

void aTitleThatStoppedWorkingIsForgottenOutLoud() {
    std::filesystem::path directory = freshConfigDirectory();
    std::filesystem::path title = writeFile(directory / "game.wux");
    TitleSelection keeper(directory, accepting());
    keeper.remember(title);

    // The disc image moved, or the keys it needs went away: the record is
    // still there and still unusable.
    TitleSelection reader(directory, refusing("the file has gone"));
    check::isTrue(reader.remembered().empty(),
                  "a remembered title that no longer works is not run");
    check::isTrue(reader.lastError().find("the file has gone") != std::string::npos,
                  "and setup says why it is asking again");
}

void anEmptyRecordIsAFailureAndNotAnAnswer() {
    std::filesystem::path directory = freshConfigDirectory();
    TitleSelection selection(directory, accepting());
    std::ofstream(selection.recordPath(), std::ios::trunc);
    check::isTrue(selection.remembered().empty(), "an empty record remembers nothing");
    check::isTrue(selection.lastError().find("is empty") != std::string::npos,
                  "and is reported as a record that could not be read");
}

void aTitleIdIsSixteenHexDigits() {
    std::optional<TitleIdentity> lower = TitleIdentity::parse("0005000010143500");
    check::isTrue(lower.has_value(), "an ID as it is written is read");
    check::equal(lower->id(), uint64_t{0x0005000010143500}, "as the number it names");
    check::equal(TitleIdentity::parse("0005000010143AbC")->id(), uint64_t{0x0005000010143abc},
                 "in either case");
    check::isTrue(!TitleIdentity::parse("000500001014350").has_value(), "fifteen digits are not");
    check::isTrue(!TitleIdentity::parse("0x05000010143500").has_value(), "nor a prefixed one");
    check::isTrue(!TitleIdentity::parse("000500001014350g").has_value(), "nor one not in hex");
    check::isTrue(!TitleIdentity::parse("").has_value(), "nor nothing");
}

void onlyTheProductsOwnTitleIsAccepted() {
    TitleIdentity windWaker(0x0005000010143500);
    check::isTrue(windWaker.refusal(0x0005000010143500, "Wind Waker HD").empty(),
                  "its own title is accepted");
    std::string refused = windWaker.refusal(0x000500001010ec00, "Mario Kart 8");
    check::isTrue(refused.find("Mario Kart 8") != std::string::npos,
                  "another is refused by what it holds");
    check::isTrue(refused.find("000500001010ec00") != std::string::npos &&
                      refused.find("0005000010143500") != std::string::npos,
                  "naming both IDs");
    check::isTrue(windWaker.refusal(0x000500001010ec00, "").find("a title") != std::string::npos,
                  "and one without a name is still refused");
}

} // namespace

namespace wiiuport::tests {

void runSelectionTests() {
    nothingIsRememberedUntilSomethingIsChosen();
    aChosenTitleSurvivesTheProcess();
    aRefusedTitleIsNotKept();
    aTitleThatStoppedWorkingIsForgottenOutLoud();
    anEmptyRecordIsAFailureAndNotAnAnswer();
    aTitleIdIsSixteenHexDigits();
    onlyTheProductsOwnTitleIsAccepted();
    std::filesystem::remove_all(std::filesystem::temp_directory_path() /
                                "wiiuport-title-selection-tests");
}

} // namespace wiiuport::tests
