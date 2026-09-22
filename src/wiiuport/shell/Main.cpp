// The product's entry point.
//
// A player runs this; Cemu's own front end is not built. The disc image is an
// optional argument: given one, that is what runs, and given none the product
// runs what the player chose last time, or asks them on its setup screen. It
// never guesses which game to run.

#include "wiiuport/Runtime.h"
#include "wiiuport/shell/ShellHost.h"

#include <lucent/log.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

void printUsage(const char* program) {
    lucent::error("shell", "usage: {} [disc image] [--hidden] [--width N] [--height N]", program);
}

} // namespace

int main(int argc, char* argv[]) {
    wiiuport::shell::ShellHost::Options options;
    options.executable = argv[0];
    std::vector<std::string> arguments(argv + 1, argv + argc);
    for (size_t i = 0; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];
        if (argument == "--hidden") {
            options.window.hidden = true;
            continue;
        }
        if ((argument == "--width" || argument == "--height") && i + 1 < arguments.size()) {
            int value = std::stoi(arguments[++i]);
            if (argument == "--width") {
                options.window.width = value;
            } else {
                options.window.height = value;
            }
            continue;
        }
        if (!argument.empty() && argument.front() == '-') {
            lucent::error("shell", "unknown option {}", argument);
            printUsage(argv[0]);
            return 2;
        }
        if (!options.title.empty()) {
            lucent::error("shell", "more than one disc image given; this shell runs one title");
            return 2;
        }
        options.title = argument;
    }

    // Installed before the system exists, so the first frame the guest
    // submits is already observed.
    wiiuport::Runtime::instance().installHooks();
    wiiuport::shell::ShellHost host;
    return host.run(options);
}
