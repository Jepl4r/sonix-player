#ifndef STREAMERROR_H
#define STREAMERROR_H

#include "lvgl/lvgl.h"

#include "src/system/streaming/streamerr.h"

// A failed load, said where the list would have been.
//
// The streaming pages used to raise a full-screen veil for as long as a
// request took -- twenty seconds against a server that did not answer -- and
// then put the service's last error up in a popup, often a bare HTTP status.
// Now the request is short (http_set_thread_timeout_cap) and the failure stays
// on the page that asked: what went wrong in a sentence (streamerr.h maps it),
// a picture for the kind of trouble, and a button that asks again -- the same
// load, from the same place. For an expired session the button offers to sign
// in instead, when the page has a sign-in to offer.
//
// Shared by every service's page, so the same failure reads the same way
// everywhere. GUI thread only. Nothing runs while it is hidden: it is a few
// objects and no timer.

// Builds the panel, hidden, inside `parent` (usually the list's container or
// its screen). It sizes itself to the parent's width.
lv_obj_t *streamerror_create(lv_obj_t *parent);

// Shows the panel saying `text` -- the sentence streamerr_describe() wrote --
// with the picture for `cls`. `retry` repeats the failed load; `sign_in`, when
// given, replaces it for STREAM_ERR_SIGN_IN. Either may be NULL (no button).
void streamerror_show(lv_obj_t *panel, stream_err_t cls, const char *text, void (*retry)(void),
					  void (*sign_in)(void));

void streamerror_hide(lv_obj_t *panel);

bool streamerror_visible(lv_obj_t *panel);

#endif /* STREAMERROR_H */
