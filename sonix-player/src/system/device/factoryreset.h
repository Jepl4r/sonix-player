#ifndef FACTORYRESET_H
#define FACTORYRESET_H

#include <stdbool.h>

// Factory reset.
//
// Nothing is erased on the spot, and this is not a shortcut: it is the stock
// firmware's own mechanism, which leaves the real work to a rootfs component
// that runs at boot. See factoryreset.c for why, and for the non-obvious reason
// it is the only safe way to do it.
//
// Does not return: it reboots.
void factoryreset_run(void);

// Whether the rootfs component that does the erasing is actually present. When
// it is missing the reset degrades to clearing this player's own configuration,
// which is worth saying in the log rather than glossing over.
bool factoryreset_supported(void);

#endif /* FACTORYRESET_H */
