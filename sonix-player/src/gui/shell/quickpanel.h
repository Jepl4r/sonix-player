#ifndef QUICKPANEL_H
#define QUICKPANEL_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The pull-down control centre (2.0): dragging down from the status bar, or a
// tap on it, brings a card down under the bar with the round buttons -- a tap
// switches what one stands for, a hold opens its page -- and the brightness
// slider. A tap outside or a swipe up of SX_CC_CLOSE_PX puts it away.
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
// The panel holds up to sixteen, in a grid of sixteen places. A place can also
// be empty: the panel simply draws one button fewer. The card shows the first
// eight; past them a handle appears under the brightness slider, and dragging
// the card down (or tapping the handle) stretches it to show the rest.
//
// Both the grid and the buttons left out of it live in [quickpanel] as lists of
// names -- `order`, with "-" for an empty place, and `hidden`. Names and not
// indices, so a button added to the enum later joins a saved layout instead of
// invalidating it. Settings > More > Control centre is the page that edits it,
// as two lists (see quickpanel_move_in_use).
// ---------------------------------------------------------------------------

#define QP_SLOT_COUNT 16
#define QP_SLOT_VISIBLE 8 // the ones the card shows before it is stretched

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
	QP_BTN_WIFI_TRANSFER,
	QP_BTN_GAPLESS,
	// 2.0: the volume limit (a switch), USB DAC mode (the way to its page) and
	// the DAC's filter (a choice of four, in a sheet).
	QP_BTN_VOLUME_LIMIT,
	QP_BTN_DAC,
	QP_BTN_DAC_FILTER,
	QP_BTN_COUNT,
	QP_BTN_NONE = QP_BTN_COUNT, // an empty place in the grid
} quickpanel_button_t;

// The row's name (a translation tag) and the glyph it carries. The gain and
// gapless buttons draw whichever of their two glyphs matches the current
// setting; the list shows low gain and gapless on, the drawings that say what
// the button is for.
const char *quickpanel_button_tag(quickpanel_button_t button);
const lv_image_dsc_t *quickpanel_button_icon(quickpanel_button_t button);

// The layout as two lists, the way the settings page shows it: the buttons in
// the panel, in order with the empty places left out, and the ones left out of
// it. Moving a button to a position in either list saves the layout and closes
// the gaps in the panel; a button past the last place pushes the last one out,
// to the top of the other list.
int quickpanel_in_use_count(void);
quickpanel_button_t quickpanel_in_use_at(int position);
int quickpanel_hidden_count(void);
quickpanel_button_t quickpanel_hidden_at(int position);
void quickpanel_move_in_use(quickpanel_button_t button, int position);
void quickpanel_move_hidden(quickpanel_button_t button, int position);

#endif // QUICKPANEL_H
