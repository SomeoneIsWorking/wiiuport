#include "check.h"
#include "suites.h"
#include "wiiuport/shell/PadRoster.h"

#include <string>
#include <vector>

using wiiuport::shell::PadRoster;

void wiiuport::tests::runPadRosterTests() {
    const std::vector<std::string> nothing;
    const std::vector<std::string> pad{"pad"};
    {
        const PadRoster::Change change = PadRoster(pad).against(pad);
        check::isTrue(change.keep && change.lost.empty(), "a pad still plugged in stays mapped");
    }
    {
        const PadRoster::Change change = PadRoster(pad).against(nothing);
        check::isTrue(!change.keep && change.lost == pad, "an unplugged pad is lost");
    }
    {
        const std::vector<std::string> mapped{"gone", "pad"};
        const PadRoster::Change change = PadRoster(mapped).against(pad);
        check::isTrue(change.keep && change.lost == std::vector<std::string>{"gone"},
                      "a lost pad is named even when another mapped pad stays");
    }
    check::isTrue(PadRoster(nothing).against(nothing).lost.empty(), "nothing mapped loses nothing");
}
