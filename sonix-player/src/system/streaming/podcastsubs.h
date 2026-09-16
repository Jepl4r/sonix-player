#ifndef PODCASTSUBS_H
#define PODCASTSUBS_H

#include <stdbool.h>

#include "src/system/streaming/podcast.h"

// Followed podcasts, kept on the card.
//
// Not held by the service, unlike Qobuz and Tidal favourites: Podcast Index has
// no accounts. It is only a catalogue -- searched and read, never logged into
// -- so the list of what is followed belongs to this device and lives on the
// card with everything else.
//
// The list is <card>/.local/podcast.db, an SQLite database like the library and
// audiobook ones; an older podcast-subs.ini, if still present, is imported on
// first start and set aside. The feed id is stored rather than the name, so a
// podcast that changes its title stays the right one; the title and artwork
// held here exist only to avoid querying the catalogue before the list can be
// drawn.
//
// The maximum is a real cap, not a convenience: the list stays in memory for
// the life of the process, and a corrupt or hand-inflated file must not be able
// to eat the RAM playback needs.

#define PODCASTSUBS_MAX 100

// Sets the card root and reloads the list. NULL or empty clears it.
void podcastsubs_set_root(const char *sd_root);

// How many are followed, and the list itself: most recently followed first.
int podcastsubs_count(void);
int podcastsubs_list(podcast_feed_t *out, int max);

bool podcastsubs_is_followed(long long feed_id);

// Follow or unfollow. Saves immediately: powering the player off an instant
// after tapping the star must not lose the change.
void podcastsubs_follow(const podcast_feed_t *feed);
void podcastsubs_unfollow(long long feed_id);

#endif /* PODCASTSUBS_H */
