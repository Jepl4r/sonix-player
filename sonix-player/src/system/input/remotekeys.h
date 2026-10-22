#ifndef REMOTEKEYS_H
#define REMOTEKEYS_H

#include "src/gui/shell/gui.h"
#include "src/system/playback/audiobook.h"

// A loaded book includes paused playback; remotes use the screen's chapter path.
static inline gui_key_t remote_skip_key(bool next) {
	if (audiobook_is_playing()) {
		return next ? GUI_KEY_NEXT_CHAPTER : GUI_KEY_PREV_CHAPTER;
	}
	return next ? GUI_KEY_NEXT : GUI_KEY_PREV;
}

#endif /* REMOTEKEYS_H */
