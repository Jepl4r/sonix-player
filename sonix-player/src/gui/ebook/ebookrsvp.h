#ifndef EBOOKRSVP_H
#define EBOOKRSVP_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/gui.h"
#include "src/system/ebook/ebook.h"

// ---------------------------------------------------------------------------
// Reading one word at a time
//
// Rapid serial visual presentation: the words of the chapter are shown one
// after another at one fixed point on the screen, so the eyes never move and
// the pace is set by a timer rather than by the reader. One letter of each
// word -- the pivot, a little left of the word's middle -- is always drawn at
// exactly the same spot and in the accent colour, with a tick above and below
// it, so a long word and a short one are looked at in the same place. The
// second mode shows one line's worth of words at a time instead.
//
// The reader owns the book, the position and the settings; this file owns the
// view, the ticker and the tokeniser. It is told where the reading is and
// tells the reader every time the word on screen moves, so the position the
// reader writes to the config and into a bookmark is the word being shown.
// ---------------------------------------------------------------------------

typedef enum {
	RSVP_MODE_PAGES = 0, // the ordinary pages: nothing in here is used
	RSVP_MODE_WORD,		 // one word at a time at the focal point
	RSVP_MODE_LINE,		 // one line at a time
} rsvp_mode_t;

// Words per minute. The steps are what the stepper in the menu and the
// vertical swipe on the page move by.
#define RSVP_WPM_MIN 100
#define RSVP_WPM_MAX 800
#define RSVP_WPM_DEFAULT 250
#define RSVP_WPM_STEP 25

// What the reader does when the ticker asks it to.
typedef struct {
	// The word on screen is now the one starting at `block`/`offset` of the
	// chapter. The reader keeps it as its position, so resuming the book and
	// saving a bookmark land on that word.
	void (*moved)(uint32_t block, uint32_t offset);
	// Opens another chapter at `block`/`offset` -- UINT32_MAX for its last
	// block -- and shows it, which calls ebookrsvp_show() again. `direction`
	// is +1 reading on, -1 going back. False when the chapter cannot be read.
	bool (*load_chapter)(uint32_t spine, uint32_t block, uint32_t offset, int direction);
	// Writes the position to the config: on a pause and on a skip, not on
	// every word.
	void (*save)(void);
} ebookrsvp_hooks_t;

// How the page looks right now, handed over at every show.
typedef struct {
	lv_color_t ink;	   // the page's text colour
	lv_color_t accent; // the interface accent, for the pivot and the ticks
	int text_size;	   // the page's font size in pixels
	int line_space;	   // the extra pixels between the page's lines
	bool no_pivot;	   // a vertical book: the word is centred instead
} ebookrsvp_look_t;

// Builds the ticker, paused. `hooks` is kept by pointer and must outlive
// this.
void ebookrsvp_init(gui_config_t *cfg, const ebookrsvp_hooks_t *hooks);

// Which of the two views, or neither. Changing it does not redraw: the reader
// calls ebookrsvp_show() after.
void ebookrsvp_set_mode(rsvp_mode_t mode);
rsvp_mode_t ebookrsvp_mode(void);

// Words per minute, clamped to the range above. `flash` shows the new value on
// the page for a moment, which a swipe wants and the menu's stepper does not.
void ebookrsvp_set_wpm(int wpm, bool flash);

// Puts the word at or after `block`/`offset` of `book`'s chapter on screen,
// inside `box`, which is the reader's page box. The view is built from
// scratch each time, so it is also how a theme or size change is applied. The
// ticker keeps running if it was.
void ebookrsvp_show(lv_obj_t *box, ebook_t *book, uint32_t block, uint32_t offset, const ebookrsvp_look_t *look);

// For the reader to call once it has put a chapter up by itself -- the book
// opened, a chapter picked, the mode changed: a chapter with no word in it (a
// cover page, a chapter of illustrations) is passed over for the next one
// that has, so the view never opens on nothing.
void ebookrsvp_settle(void);

// The reader is leaving the view -- another mode, or the book is closed. The
// ticker stops and the book is forgotten; the reader deletes the objects.
void ebookrsvp_hide(void);

// The ticker.
void ebookrsvp_play(void);
void ebookrsvp_pause(void);
void ebookrsvp_toggle(void);
bool ebookrsvp_playing(void);

// To the start of the next sentence (+1) or of this one, or the one before it
// when this one has only just begun (-1). Crosses chapter boundaries.
void ebookrsvp_skip_sentence(int direction);

#endif /* EBOOKRSVP_H */
