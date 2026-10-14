#ifndef SHUFFLECARD_H
#define SHUFFLECARD_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// "Shuffle all music" (UI 2.0, the empty state)
//
// While nothing has played since the player was set up there is no mini
// player, and the home and Music pages put this card in its place: a round
// shuffle button in the accent, "Shuffle all music" and how many tracks and
// records there are. A tap plays the whole library in a random order
// (listacts_shuffle_all) and the page stays where it is: the mini player
// takes the card's place.
//
// The frame (sx_frame_set_idle_card) shows it exactly while its page is on
// screen and the player is idle. The counts are read when the page comes up
// and only when the card is going to show, and again only after the library
// has changed (library_revision). An empty library leaves the slot empty.
//
// UI thread only. Nothing runs while the card is not on screen.
// ---------------------------------------------------------------------------

// Builds the card on `screen` (the home page, the Music page) and hands it to
// the frame.
void shufflecard_attach(lv_obj_t *screen, gui_config_t *cfg);

#endif /* SHUFFLECARD_H */
