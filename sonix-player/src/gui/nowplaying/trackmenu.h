#ifndef TRACKMENU_H
#define TRACKMENU_H

#include "src/gui/shell/gui.h"
#include "src/system/playback/playlist.h"

#include "lvgl/lvgl.h"

// The player's overflow menu (the ellipsis opposite the repeat button) and
// the two pages it opens: the queue (what is lined up after the current track)
// and the details page (everything known about the playing file).
extern lv_obj_t *queue_screen;
extern lv_obj_t *details_screen;

void trackmenu_init(gui_config_t *cfg);

// Opens the popover next to `anchor` with the queue and details entries.
void trackmenu_open(lv_obj_t *anchor);

// Opens the queue page directly, without the menu in front. For the player with
// a podcast playing: there the button is the episodes button, and the episodes
// are the queue.
void trackmenu_open_queue(void);

// Opens the details page for a specific file (from a library list's menu).
void trackmenu_details_open(const char *path);

// "Show artist" for the playing track, as the menu has it: what the artist
// line on the player opens when tapped. Quiet, unlike the menu line: false,
// with nothing opened and nothing said, when the track is not in the library --
// a radio station's line is tapped by accident far more often than on purpose.
// UI thread.
bool trackmenu_show_artist(void);

// The queue changed under the page (an undo, a list queued from elsewhere):
// bound again where it is, if it is the page on show. UI thread.
void trackmenu_queue_refresh(void);

// The same, only when something it shows moved (the queue's revision, the
// track, a source outside the queue): from the player's poll and every track
// change. Nothing at all while the page is not up. UI thread.
void trackmenu_queue_poll(void);

// The playing track changed: on the queue page (when it is up) the runs move
// round the new one -- trackmenu_queue_poll().
void trackmenu_notify_now_playing(void);

// Queueing from a menu, the one way every list does it: "Play next" or "Add to
// end of queue" (playlist_queue_paths), the queue written down to the card once
// for the whole set, and a toast saying which of the two happened -- or a
// notice when nothing could be queued. Returns how many tracks went in. UI
// thread. A streaming page can hand its tracks' cache paths in here too: the
// queue fetches a missing one when it comes up (device_state_add_prepare_cb).
int trackmenu_queue_paths(const char *const *paths, int count, playlist_queue_where_t where);

// The two choices as a popover by `anchor`, for a button that stands for
// "queue" as a whole (the selection bars): `chosen` gets the pick, after the
// popover has closed. UI thread.
typedef void (*trackmenu_queue_chosen_cb)(playlist_queue_where_t where, void *user);
void trackmenu_queue_choose(lv_obj_t *anchor, trackmenu_queue_chosen_cb chosen, void *user);

#endif /* TRACKMENU_H */
