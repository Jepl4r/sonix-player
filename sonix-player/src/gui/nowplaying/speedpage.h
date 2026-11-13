#ifndef SPEEDPAGE_H
#define SPEEDPAGE_H

#include "src/gui/shell/gui.h"

// The playback-speed page: a fine 0.05x slider from 0.25x to 4.00x, with the
// everyday presets and a pair of single-step buttons beside it. Opened from
// the speed gauge on the player while a book or an episode is playing.
void speedpage_init(gui_config_t *cfg);
void speedpage_open(void);

#endif /* SPEEDPAGE_H */
