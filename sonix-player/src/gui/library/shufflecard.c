#include "shufflecard.h"

#include <stdio.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/listacts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_frame.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"

// The round button at the card's left (the prototype's 52 px disc).
#define DISC_SIZE 52
#define CARD_PAD_L 10
#define CARD_GAP 14

#define CARDS_MAX 2

typedef struct {
	lv_obj_t *screen;
	lv_obj_t *band; // the 88 px slot, in the page's colour
	lv_obj_t *card; // the 72 px card in it
	lv_obj_t *sub;	// "Tracks: N · albums: M"
} card_t;

static card_t cards[CARDS_MAX];
static int card_count;

// The counts, shared by both cards, and the library state they were read in:
// read again only when it has changed since.
static int tracks_n = -1;
static int albums_n;
static unsigned counted_tracks_rev;
static unsigned counted_albums_rev;
static bool counted_open;

static void counts_refresh(void) {
	bool open = library_is_open();
	unsigned tracks_rev = library_revision(LIBRARY_LIST_TRACKS);
	unsigned albums_rev = library_revision(LIBRARY_LIST_ALBUMS);
	if (tracks_n >= 0 && open == counted_open && tracks_rev == counted_tracks_rev &&
		albums_rev == counted_albums_rev) {
		return;
	}
	counted_open = open;
	counted_tracks_rev = tracks_rev;
	counted_albums_rev = albums_rev;
	tracks_n = open ? library_track_count() : 0;
	albums_n = 0;
	if (tracks_n > 0) {
		// One handle over the albums' row ids: four bytes a record, let go of
		// at once.
		library_index_t *ix =
			library_index_open(LIBRARY_LIST_ALBUMS, LIBRARY_FILTER_NONE, NULL, LIBRARY_ORDER_DEFAULT, false);
		albums_n = ix ? library_index_count(ix) : 0;
		library_index_close(ix);
	}
}

static void card_fill(card_t *c) {
	counts_refresh();
	// Nothing to shuffle: the slot stays empty rather than offering a button
	// that can only say so (the Music page has its scan panel for that).
	lv_obj_set_hidden(c->card, tracks_n <= 0);
	if (tracks_n <= 0) {
		return;
	}
	char text[96];
	if (albums_n > 0) {
		snprintf(text, sizeof(text), tr("lists_shuffle_counts"), tracks_n, albums_n);
	} else {
		snprintf(text, sizeof(text), tr("lists_tracks_n"), tracks_n);
	}
	lv_label_set_text(c->sub, text);
}

static void load_start_cb(lv_event_t *e) {
	card_t *c = lv_event_get_user_data(e);
	// Only when it is going to show: a page visited while music plays never
	// counts anything.
	if (c && sx_frame_idle()) {
		card_fill(c);
	}
}

static void card_clicked_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return; // a swipe that ended here, not a tap
	}
	listacts_shuffle_all();
}

void shufflecard_attach(lv_obj_t *screen, gui_config_t *cfg) {
	if (!screen || !cfg || card_count >= CARDS_MAX) {
		return;
	}
	card_t *c = &cards[card_count++];
	c->screen = screen;

	int w = (int)cfg->screen_width;
	int h = (int)cfg->screen_height;

	// The band: the whole slot, in the page's colour, so a page scrolled under
	// it (the Music tiles) does not show beside the card -- the same as the
	// mini player's slot. It takes the presses in its margins too.
	c->band = sx_box(screen);
	lv_obj_add_style(c->band, &sx_style_page, 0);
	lv_obj_set_clickable(c->band, true);
	lv_obj_set_event_bubble(c->band, false);
	lv_obj_set_style_pad_top(c->band, SX_MINI_MARGIN_TOP, 0);
	lv_obj_set_style_pad_hor(c->band, SX_MINI_MARGIN_SIDE, 0);
	lv_obj_set_style_pad_bottom(c->band, SX_MINI_MARGIN_BOTTOM, 0);

	c->card = sx_box(c->band);
	lv_obj_set_size(c->card, lv_pct(100), SX_MINI_H);
	lv_obj_add_style(c->card, &sx_style_surface, 0);
	lv_obj_add_style(c->card, &sx_style_press, LV_STATE_PRESSED);
	lv_obj_set_style_radius(c->card, SX_MINI_R, 0);
	lv_obj_set_style_pad_left(c->card, CARD_PAD_L, 0);
	lv_obj_set_style_pad_right(c->card, CARD_PAD_L, 0);
	lv_obj_set_style_pad_column(c->card, CARD_GAP, 0);
	lv_obj_set_flex_flow(c->card, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(c->card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_clickable(c->card, true);
	lv_obj_set_event_bubble(c->card, false);
	lv_obj_add_event_cb(c->card, card_clicked_cb, LV_EVENT_CLICKED, NULL);

	// The line along its top, as on the mini player and the sheet: a layer
	// shown by a lighter surface and a line, not by a shadow. Inset by the
	// radius so it does not stick out past the rounded corners.
	lv_obj_t *hairline = sx_box(c->card);
	lv_obj_set_ignore_layout(hairline, true);
	lv_obj_set_size(hairline, w - 2 * SX_MINI_MARGIN_SIDE - 2 * SX_MINI_R, 1);
	lv_obj_set_pos(hairline, SX_MINI_R - CARD_PAD_L, 0);
	lv_obj_add_style(hairline, &sx_style_hairline, 0);

	lv_obj_t *disc = sx_box(c->card);
	lv_obj_set_size(disc, DISC_SIZE, DISC_SIZE);
	lv_obj_add_style(disc, &sx_style_accent, 0);
	lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
	lv_obj_t *glyph = lv_image_create(disc);
	lv_image_set_src(glyph, &icon_shuffle);
	lv_obj_add_style(glyph, &sx_style_accent, 0);
	lv_obj_set_style_bg_opa(glyph, LV_OPA_TRANSP, 0);
	lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
	lv_obj_center(glyph);

	lv_obj_t *text = sx_box(c->card);
	lv_obj_set_height(text, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(text, 1);
	lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(text, 2, 0);

	lv_obj_t *title = sx_label(text, &font_ui_24_bold, &sx_style_text);
	lv_obj_set_width(title, lv_pct(100));
	lv_label_set_text(title, tr("lists_shuffle_all_music"));

	c->sub = sx_label(text, &font_ui_18, &sx_style_text2);
	lv_obj_set_width(c->sub, lv_pct(100));
	lv_label_set_text(c->sub, "");

	lv_obj_set_hidden(c->card, true); // until the counts say there is music
	lv_obj_add_event_cb(screen, load_start_cb, LV_EVENT_SCREEN_LOAD_START, c);

	// The frame sizes and places what it is given at the card's 72 px; the band
	// is the whole slot instead, set again here after it has done so.
	sx_frame_set_idle_card(screen, c->band);
	lv_obj_set_size(c->band, w, SX_MINI_SLOT_H);
	lv_obj_set_pos(c->band, 0, h - SX_MINI_SLOT_H);
}
