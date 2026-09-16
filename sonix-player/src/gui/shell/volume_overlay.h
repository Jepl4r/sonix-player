#ifndef VOLUME_OVERLAY_H
#define VOLUME_OVERLAY_H

#include "src/gui/shell/gui.h"

// The heads-up volume bar.
//
// The status bar already carries the level as a number, which is the right
// answer everywhere except inside the player: there the artwork fills the
// screen and the status bar is hidden, so pressing a volume key would give no
// feedback at all. This is the panel that appears over the artwork while the
// keys are being held, and fades out again once they stop.
void volume_overlay_init(gui_config_t *cfg);

// Shows the bar at `percent` and restarts its dismiss timer. Does nothing
// unless the player is on screen -- elsewhere the status bar covers it.
void volume_overlay_show(int percent);

// Lets the bar appear outside the player while this stays on.
//
// The normal rule is player-only, because everywhere else the status bar
// already carries the number. Gearboy is the other place without a status bar,
// so it turns this on for the length of a session.
void volume_overlay_allow_outside_player(bool allow);

#endif /* VOLUME_OVERLAY_H */
