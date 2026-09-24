#pragma once

#include "wiiuport/shell/ProductCommandLine.h"

namespace wiiuport::shell {

// The whole interface a title project builds its product through: its `main`
// is one call to this with what it fixes, and the runtime does the rest --
// hooks, emulated system, window, setup screen and title. The exit code is
// the process's, and every non-zero one has been reported before it returns.
int runProduct(const Product& product, int argc, char** argv);

} // namespace wiiuport::shell
