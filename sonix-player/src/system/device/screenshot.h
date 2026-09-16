#ifndef SCREENSHOT_H
#define SCREENSHOT_H

#include <stdbool.h>

// Screenshots: volume up plus the power key, held together.
//
// Files land in a "Screenshots" directory at the root of the microSD -- the
// only writable place large enough, and the only one they can be retrieved from
// by plugging the device into a computer.
//
// Off by default: the combination is easy to hit by accident while turning the
// volume up with the device in a pocket, and someone who does not know it
// exists should not find the card full of images.

bool screenshot_enabled(void);
void screenshot_set_enabled(bool enabled);

// The directory name, for the interface.
const char *screenshot_folder_name(void);

// Requested from the key-reading thread. It does not do the work: it hands it
// to the UI thread, the only one that can copy the framebuffer without risking
// half of one frame and half of the next, which in turn hands it to a thread
// that writes the file. Returns immediately.
void screenshot_request(void);

#endif /* SCREENSHOT_H */
