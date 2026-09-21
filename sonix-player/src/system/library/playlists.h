#ifndef PLAYLISTS_H
#define PLAYLISTS_H

#include <stdbool.h>
#include <stddef.h>

// User playlists.
//
// A playlist is a table in the music index, named "M3U_" followed by the
// playlist's own name -- "Sera" is the table "M3U_Sera". Each row carries the
// track's path and the title, artist and duration to show for it, so drawing a
// playlist, or counting one, is a read of one table and touches the card not at
// all.
//
// The first time a card is seen, the marked .m3u files in its Playlist folder
// are taken into tables, so a card written by an older version needs nothing
// imported by hand.
//
// The folder itself is where Backup writes and where Import reads. A backup is
// an ordinary extended M3U, the same thing the stock player reads and writes:
//
//   #EXTM3U
//   #[Sonix Playlist]
//   #EXTINF:213,Artist - Title
//   /mnt/sd/Music/Album/01 Title.flac
//
// Absolute paths, LF line endings, UTF-8 -- no BOM. The second line marks the
// file as written by this player. It is a comment, so every other player still
// reads the file, and it is read back either way, so a marker added by hand
// without the '#' is still understood and never counted as a track.
#define PLAYLISTS_MARKER "[Sonix Playlist]"

// The card, and with it where backups live: <sd_root>/Playlist. Called once at
// startup, and again whenever a card is mounted.
void playlists_init(const char *sd_root);
const char *playlists_dir(void);

// Every playlist, by name, alphabetically. Return false from the callback to
// stop early. Returns how many were delivered. The first call on a card is also
// what moves the old .m3u files into the index.
typedef bool (*playlists_name_cb_t)(const char *name, void *user);
int playlists_for_each(playlists_name_cb_t cb, void *user);

// True if a playlist by that name already exists.
bool playlists_exists(const char *name);

// Creates an empty playlist. Fails if the name is unusable or it exists.
bool playlists_create(const char *name);

// Appends a track, creating the playlist if it is not there yet. Duration and
// display name come from the file's tags, like the stock player's entries.
bool playlists_add_track(const char *name, const char *track_path);

// Reads a playlist. `paths_out` receives the resolved paths; `titles_out` and
// `artists_out` (either may be NULL) receive the name each row should show,
// taken from the track's own tags -- the #EXTINF line, when there is one, is
// only a fallback, and the stock player's playlists carry none at all. An entry
// in `artists_out` may be NULL where the track has no artist. Every returned
// array is freed with playlists_free_list().
int playlists_load(const char *name, char ***paths_out, char ***titles_out, char ***artists_out);
void playlists_free_list(char **list, int count);

// Drops the playlist. Its backup, if it has one, is left alone.
bool playlists_delete(const char *name);

// Renames the playlist, which is to say its table. Fails when either name is
// unusable or when another playlist already answers to the new one; renaming a
// playlist to what it is already called succeeds and does nothing. An existing
// backup keeps the old name until the next Backup.
bool playlists_rename(const char *name, const char *new_name);

// Removes the first entry whose path matches, and the #EXTINF line above it if
// there is one. Returns false when the playlist or the entry is not there.
bool playlists_remove_track(const char *name, const char *track_path);

// How many tracks a playlist holds: a COUNT over its table, so no card is
// touched and nothing is read. The two are the same question, and
// playlists_count_lines() is kept only as the older name for it.
int playlists_count_tracks(const char *name);
int playlists_count_lines(const char *name);

// The backup file's path, modification time and size. False when there is no
// backup.
bool playlists_file_stamp(const char *name, char *path_out, size_t path_size, long *mtime_out, long *size_out);

// Whether any of a playlist's entries have not been looked for on the card
// since it was last mounted. The list draws from what is written down either
// way; this says whether playlists_verify() has anything to do.
bool playlists_needs_verify(const char *name);

// Looks for the entries above and writes down what it found. One card lookup
// each, bounded, so this belongs on a thread and not in front of a user: what
// it is for is the page opening at once and correcting itself a moment later.
// True when something changed, which is also when the open list reloads.
bool playlists_verify(const char *name);

// Writes the playlist out as <card>/Playlist/<name>.m3u, replacing an earlier
// backup of the same name. Every entry goes in, missing files included: a
// backup is what the playlist holds, and a track off the card today can be back
// on it tomorrow. `path_out` (may be NULL) receives where it was written.
bool playlists_backup(const char *name, char *path_out, size_t path_size);

// ---------------------------------------------------------------------------
// Import
//
// What the page's import button works on. Two places: loose .m3u/.m3u8 files at
// the root of the card, which is where a list copied off a computer lands, and
// the Playlist folder, which is where Backup writes. The second is what brings
// a deleted playlist back -- its backup is still there. A backup of a playlist
// that still exists is not offered, since importing it would only make a second
// copy of something already on the page.
//
// Importing reads a list once and writes it into the index as a playlist of
// this player's own: every entry resolved to a path it can open, the ones the
// card does not have at that path looked up in the index by file name, and a
// title beside each. What comes out opens without touching the card at all.
// ---------------------------------------------------------------------------

enum PlaylistLocation {
	PLAYLIST_LOCATION_SD_ROOT,
	PLAYLIST_LOCATION_PLAYLIST,
	PLAYLIST_LOCATION_PLAYLIST_DATA,
};

typedef struct {
	char name[201]; // what it would be called: the file name, without extension
	char path[512]; // the file itself
	enum PlaylistLocation playlist_location; // a backup, rather than a loose file at the root
} playlists_candidate_t;

// Fills `out` with at most `max` importable playlists, by name. Returns how
// many were written.
int playlists_importable(playlists_candidate_t *out, int max);

// Up to this many of a playlist's unresolved entries are named in its report.
// A list that matches nothing would otherwise be a page of file names.
#define PLAYLISTS_IMPORT_MISSING_MAX 20

typedef struct {
	char name[201]; // what the imported playlist ended up called
	int total;		// entries the file named
	int found;		// of those, the ones that became rows
	int by_name;	// of those, the ones the index found rather than the card
	int missing;	// entries that are neither on the card nor in the index
	char missing_names[PLAYLISTS_IMPORT_MISSING_MAX][96];
	int missing_listed;
} playlists_import_result_t;

// Imports one playlist. Slow: a card lookup per entry, then one pass over the
// index -- not something to call from the interface thread. False when the file
// cannot be read or nothing in it could be resolved, and then nothing was
// written. The source file is never touched.
bool playlists_import(const char *source_path, playlists_import_result_t *out);

#endif // PLAYLISTS_H
