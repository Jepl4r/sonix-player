#ifndef QUICKPANEL_H
#define QUICKPANEL_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The pull-down control centre: dragging down from the status bar slides in a
// sheet with the wifi and bluetooth buttons (tap to toggle, long press to open
// the page), the transport controls and the brightness slider. Tap outside or
// swipe up to put it away.
void quickpanel_init(gui_config_t *cfg);
void quickpanel_open(void);

// True while something else owns the device (USB DAC mode, Wi-Fi transfer) and
// the control centre must stay shut.
bool quickpanel_blocked(void);
void quickpanel_close(void);
bool quickpanel_is_open(void);

// Interactive open/close: the sheet follows the finger instead of jumping the
// moment a drag starts. The status bar drives the downward gesture, the panel
// itself the upward one; both call the same three.
//   begin  -- take the sheet wherever it currently is (open or parked)
//   update -- dy is the finger's travel since the press
//   end    -- animate to whichever end the release is closest to
void quickpanel_drag_begin(void);
void quickpanel_drag_update(int dy);
void quickpanel_drag_end(void);

// ---------------------------------------------------------------------------
// Which round buttons the panel carries, and where
//
// The panel holds eight, in a grid of eight places. A place can also be empty:
// the panel simply draws one button fewer, but the settings page shows the gap
// where the button was taken from, so putting it back is a matter of putting it
// where it was rather than of finding it a new home.
//
// Both the grid and the buttons left out of it live in [quickpanel] as lists of
// names -- `order`, with "-" for an empty place, and `hidden`. Names and not
// indices, so a button added to the enum later joins a saved layout instead of
// invalidating it. Settings > More > Control centre is the page that edits it.
// ---------------------------------------------------------------------------

#define QP_SLOT_COUNT 8

typedef enum {
	QP_BTN_WIFI = 0,
	QP_BTN_BLUETOOTH,
	QP_BTN_AIRPLAY,
	QP_BTN_MSEB,
	QP_BTN_EQ,
	QP_BTN_FADE,
	QP_BTN_GAIN,
	QP_BTN_LINEOUT,
	QP_BTN_SONIXLINK,
	QP_BTN_PEQ,
	QP_BTN_DLNA,
	// One per sleep timer, because there are three of them and switching off
	// "the sleep timer" from here must not switch off somebody else's: a book
	// counting down in the evening has nothing to do with the album that was
	// playing at lunchtime.
	QP_BTN_SLEEP_MUSIC,
	QP_BTN_SLEEP_AUDIOBOOK,
	QP_BTN_SLEEP_PODCAST,
	QP_BTN_COUNT,
	QP_BTN_NONE = QP_BTN_COUNT, // an empty place in the grid
} quickpanel_button_t;

// What is in grid place `slot`, or QP_BTN_NONE if nothing is.
quickpanel_button_t quickpanel_slot_at(int slot);

// The buttons that are not in the grid, in the order they were taken out.
int quickpanel_hidden_count(void);
quickpanel_button_t quickpanel_hidden_at(int position);

// The row's name (a translation tag) and the glyph it carries. The gain button
// draws whichever of the two glyphs matches the current setting; this is the
// low one, which is what the list should show.
const char *quickpanel_button_tag(quickpanel_button_t button);
const lv_image_dsc_t *quickpanel_button_icon(quickpanel_button_t button);

// The three things the settings page can do, each of which saves the layout and
// rearranges the panel:
//
//   move  one grid place to another. An empty destination takes the button and
//         leaves the source empty; an occupied one swaps the two.
//   place a button from outside the grid into a place. Whatever was there goes
//         out, so the grid never holds more than eight.
//   clear a place, sending its button out of the grid.
void quickpanel_slot_move(int from_slot, int to_slot);
void quickpanel_slot_place(quickpanel_button_t button, int slot);
void quickpanel_slot_clear(int slot);

#endif // QUICKPANEL_H
