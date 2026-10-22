#ifndef DEVCONTROL_H
#define DEVCONTROL_H

#include "lvgl/lvgl.h"

// ADB-only UI automation for hardware testing. Commands are written to the
// root-owned /tmp/sonix-control FIFO and become a second LVGL pointer, leaving
// the real touchscreen open and usable. The FIFO is serviced by the main
// poll() loop, without a separate FIFO worker thread.
void devcontrol_init(lv_display_t *display, int width, int height);

// File descriptor included in the main event loop's poll set. -1 when the
// control channel could not be created.
int devcontrol_fd(void);

// Reads and applies every complete command waiting in the FIFO. Must run on
// the GUI thread because commands can wake the display and update LVGL input.
void devcontrol_service(void);

#endif /* DEVCONTROL_H */
