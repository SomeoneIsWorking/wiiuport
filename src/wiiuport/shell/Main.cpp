// The product's entry point.
//
// A player runs this; Cemu's own front end is not built. It takes one
// argument -- the disc image -- because a shell that guesses which game to
// run is a shell that silently runs the wrong one.

#include "wiiuport/Runtime.h"
#include "wiiuport/shell/ShellHost.h"

#include <lucent/log.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

void printUsage(const char* program) {
    lucent::error("shell", "usage: {} <disc image> [--hidden] [--width N] [--height N]", program);
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
    if (options.title.empty()) {
        printUsage(argv[0]);
        return 2;
    }

    // Installed before the system exists, so the first frame the guest
    // submits is already observed.
    wiiuport::Runtime::instance().installHooks();
    wiiuport::shell::ShellHost host;
    return host.run(options);
}
