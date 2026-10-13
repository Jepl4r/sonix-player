#ifndef GUI_SX_CARD_H
#define GUI_SX_CARD_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// Sonix UI 2.0: the settings card
//
// The shape of every settings page, the Bluetooth and Wi-Fi pages, the
// streaming services' pages: a rounded surface (SX_CARD_R) with 12 / 20 px
// margins, one after another SX_CARD_GAP apart. Its head row, at least 60 px
// tall, carries the title (24) and under it an optional subtitle -- 21 px
// secondary, or the "mono" form, 17 px in the accent, for a codec or a
// network's figures -- and at the right a value (22, secondary), a chevron or
// a switch. Under the head, as needed: a row of pills (one choice of several),
// a row of colour swatches, a fill bar, an action button.
//
// A card can be marked active -- the handoff's plaque for "on now": the
// connected headphones, the current network -- which mixes 16 % of the accent
// into its surface; and dimmed, for something not available.
//
// Two ways to use it. New pages build with sx_card_create() and the parts
// below; they get the 2.0 layout as a whole. The existing settings rows
// (settingsrow.h) keep their own construction -- pages reach into their
// children by position -- and take only the look from here: sx_card_style(),
// sx_pill_style(), sx_switch_style().
//
// Every part follows the palette by itself (shared styles), except what is
// noted. Strings are shown as given: translate first. UI thread only.
// ---------------------------------------------------------------------------

// A card, flex column, full width of `parent`, its height its content's.
lv_obj_t *sx_card_create(lv_obj_t *parent);

// Makes the whole card a button: pressed fill, `cb` on a tap (with `user` as
// the event's user data). A tap at the end of a swipe or a scroll is not one.
void sx_card_on_click(lv_obj_t *card, lv_event_cb_t cb, void *user);

// The head row's parts. Each creates its part on first call and sets the text
// on every call, so a page updates a card by calling them again.
lv_obj_t *sx_card_title(lv_obj_t *card, const char *text);
lv_obj_t *sx_card_subtitle(lv_obj_t *card, const char *text, bool mono);
lv_obj_t *sx_card_value(lv_obj_t *card, const char *text);
lv_obj_t *sx_card_chevron(lv_obj_t *card);
// A switch at the right; `cb` gets LV_EVENT_VALUE_CHANGED with the state
// already flipped, on a tap only (as settingsrow_switch).
lv_obj_t *sx_card_switch(lv_obj_t *card, lv_event_cb_t cb);

// Under the head. Pills and swatches go into the row these return.
lv_obj_t *sx_card_pills(lv_obj_t *card);
lv_obj_t *sx_card_swatches(lv_obj_t *card);
lv_obj_t *sx_card_bar(lv_obj_t *card, int percent);
void sx_card_bar_set(lv_obj_t *bar, int percent);
lv_obj_t *sx_card_action(lv_obj_t *card, const char *text, lv_event_cb_t cb, void *user);

// The plaque for "on now"; and dimmed (45 %) for "not available here".
void sx_card_set_active(lv_obj_t *card, bool active);
void sx_card_set_dim(lv_obj_t *card, bool dim);

// A pill: SX_PILL_H tall, round, the accent when chosen. `value` reaches `cb`
// as its user data. Several in a pills row are one choice; the page marks the
// chosen one with sx_pill_set_active().
lv_obj_t *sx_pill(lv_obj_t *row, const char *text, intptr_t value, lv_event_cb_t cb);
void sx_pill_set_active(lv_obj_t *pill, bool active);

// A colour swatch, SX_SWATCH round; the chosen one wears a 3 px ring in the
// text colour. Its colour is set by hand (it is the choice itself).
lv_obj_t *sx_swatch(lv_obj_t *row, lv_color_t colour, intptr_t value, lv_event_cb_t cb);
void sx_swatch_set_active(lv_obj_t *swatch, bool active);

// The look alone, for objects built elsewhere: the card (radius, surface,
// margins, pressed fill), a pill, a switch (68 x 36, 28 px knob, the accent
// when on).
void sx_card_style(lv_obj_t *obj);
void sx_pill_style(lv_obj_t *pill);
void sx_switch_style(lv_obj_t *sw);

#endif /* GUI_SX_CARD_H */
