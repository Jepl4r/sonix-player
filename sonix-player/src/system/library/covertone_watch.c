#include "covertone_watch.h"

#include "src/system/library/covertonefan.h"

// Two fan-outs over covertone's single listener slot were written at the same
// time (this one for the lists, covertonefan for the player); each took the
// slot for itself, so whichever registered last silenced the other. This one
// now forwards to covertonefan, which owns the slot, so lists, player, mini
// player and queue page all hear about new tones and thumbnails.

void covertone_watch_add(void (*changed)(void)) {
	if (changed) {
		covertonefan_add(changed);
	}
}

void covertone_watch_remove(void (*changed)(void)) {
	if (changed) {
		covertonefan_remove(changed);
	}
}
