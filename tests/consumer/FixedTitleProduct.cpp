// A title project's entry point, in full: what it fixes, handed to the host.
//
// The title is a placeholder; a real project names its own. Nothing here may
// need more than wiiuport/shell/RunProduct.h, which is the point of building
// it.

#include "wiiuport/shell/RunProduct.h"

int main(int argc, char* argv[]) {
    wiiuport::shell::Product product{.name = "Fixed title product",
                                     .title = wiiuport::shell::TitleIdentity(0x0005000010143500)};
    return wiiuport::shell::runProduct(product, argc, argv);
}
