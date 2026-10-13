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
// and a button that asks again -- the same load, from the same place. For an
// expired session the button offers to sign in instead, when the page has a
// sign-in to offer.
//
// 2.0 draws it as the handoff's "service error" state: a round plaque with
// the mark of the trouble, a headline for its kind ("The service is not
// answering"), the reason in words, "Retry" in the accent and, where the page
// has something that plays without the network, a second button to it
// ("Saved tracks"). A retry that fails again says which attempt it was, so a
// press that changed nothing is not mistaken for one that did nothing.
//
// Shared by every service's page, so the same failure reads the same way
// everywhere. GUI thread only. Nothing runs while it is hidden: it is a few
// objects and no timer.

// Builds the panel, hidden, inside `parent` (usually the list's container or
// its screen). It sizes itself to the parent's width.
lv_obj_t *streamerror_create(lv_obj_t *parent);

// The service's name, for the sentence that says who did not answer.
void streamerror_set_service(lv_obj_t *panel, const char *name);

// The second button, to what plays offline ("Saved tracks"); NULL: none.
void streamerror_set_offline(lv_obj_t *panel, void (*open)(void));

// Shows the panel saying `text` -- the sentence streamerr_describe() wrote --
// under the headline for `cls`. `retry` repeats the failed load; `sign_in`,
// when given, replaces it for STREAM_ERR_SIGN_IN. Either may be NULL (no
// button).
void streamerror_show(lv_obj_t *panel, stream_err_t cls, const char *text, void (*retry)(void),
					  void (*sign_in)(void));

// Hides it, and forgets the attempts: the next failure is a first one again.
void streamerror_hide(lv_obj_t *panel);

bool streamerror_visible(lv_obj_t *panel);

#endif /* STREAMERROR_H */
