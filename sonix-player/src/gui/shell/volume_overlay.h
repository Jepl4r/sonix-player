#ifndef VOLUME_OVERLAY_H
#define VOLUME_OVERLAY_H

#include "src/gui/shell/gui.h"

// The heads-up volume bar.
//
// Since 2.0 (sx_tokens.h): a 64 x 300 bar against the right edge of every
// page, 1.5 s after the last press of a volume key, sliding in and out. The
// scale can be dragged with a finger while it is up.
void volume_overlay_init(gui_config_t *cfg);

// Shows the bar at `percent` and restarts its dismiss timer.
void volume_overlay_show(int percent);

// Before 2.0 the bar showed only on the player unless this allowed it
// elsewhere (Gearboy did); it shows everywhere now, and this is a no-op kept
// for its callers.
void volume_overlay_allow_outside_player(bool allow);

#endif /* VOLUME_OVERLAY_H */
