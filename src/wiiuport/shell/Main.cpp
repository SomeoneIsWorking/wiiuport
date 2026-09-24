// The maintainer's runtime: a product that fixes no title.
//
// The disc image is an optional argument: given one, that is what runs, and
// given none the runtime runs what the player chose last time, or asks on its
// setup screen. It never guesses which game to run. A title project builds its
// own product through runProduct instead of copying this entry point.

#include "wiiuport/shell/RunProduct.h"

int main(int argc, char* argv[]) {
    return wiiuport::shell::runProduct({}, argc, argv);
}
