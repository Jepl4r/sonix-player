#ifndef TIMESET_H
#define TIMESET_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

// Date and time setter, shown over whatever is on screen.
//
// It comes up by itself the first time the player runs, because a device with
// nothing to sync against cannot know what time it is and a wrong clock is
// worse than an obviously empty one. It can be opened again from Settings.
void timeset_init(gui_config_t *cfg);

// Brings the panel up, pre-filled with the current time.
void timeset_show(void);

// True when the clock has never been set, i.e. the panel should come up on its
// own at startup.
bool timeset_needed(void);

#endif /* TIMESET_H */
