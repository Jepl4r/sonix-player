#ifndef RSYNCD_H
#define RSYNCD_H

#include <stdbool.h>

// Rsync daemon for music sync (PC -> player), daemon mode, LAN only.
//
// Off by default. The Developer toggle writes /usr/data/rsyncd.enabled
// (what S51rsyncd checks at boot) and remembers the choice in the config.
// The password itself lives in /usr/data/rsyncd.secrets and is never in git.

bool rsyncd_is_running(void);
bool rsyncd_set_enabled(bool enabled);
bool rsyncd_switched_on(void);
void rsyncd_apply_saved_state(void);

#endif /* RSYNCD_H */
