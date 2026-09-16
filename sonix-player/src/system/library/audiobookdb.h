#ifndef AUDIOBOOKDB_H
#define AUDIOBOOKDB_H

#include <stdbool.h>
#include <stddef.h>

// The audiobook index: a small SQLite database of every .m4b on the card,
// separate from the music library (which deliberately ignores .m4b -- an
// audiobook in the middle of an album listing helps nobody).
//
// Kept intentionally simple: one table, one row per file, plus a last_played
// timestamp updated when a book starts, so the "Recent" view can list the ten
// most recently listened books. Sorting uses the same
// collation as the music library, so leading articles are skipped ("The
// Hobbit" files under H).

// Opens (creating if needed) the database. Safe to call again; false when the
// file cannot be opened, in which case everything below is a no-op.
bool audiobookdb_open(const char *db_path);
void audiobookdb_close(void);

// How many audiobooks the index holds.
int audiobookdb_count(void);

// --- Scanning (same shape as the music library's) ---
bool audiobookdb_scan_start(const char *root);
bool audiobookdb_scan_running(void);
int audiobookdb_scan_found(void);
void audiobookdb_scan_stop(void);

// --- Reading ---
typedef enum {
	AUDIOBOOK_LIST_ALL,		 // everything, alphabetical (articles skipped)
	AUDIOBOOK_LIST_RECENT,	 // the 10 most recently listened, newest first
	AUDIOBOOK_LIST_FINISHED, // the ones heard to the end, latest first
} audiobook_list_t;

// Streams the rows: `name` is the display title, `path` the file. Return
// false from the callback to stop early. Returns rows delivered.
//
// For a caller that wants every row and can hold them. The books page does not:
// it uses the handle below, which keeps four bytes a row instead of the row.
typedef bool (*audiobook_row_cb)(const char *name, const char *path, void *user);
int audiobookdb_for_each(audiobook_list_t kind, audiobook_row_cb cb, void *user);

// ---------------------------------------------------------------------------
// List handles
//
// The same arrangement the music index uses, and for the same reason: reading a
// whole list in means a name and a path allocated per row, where a handle holds
// the SQLite row id and reads a windowful of real rows back as the viewport
// moves. Fewer books than tracks, so the memory saved is smaller; the two pages
// work the same way, down to the handle going stale when the rows underneath it
// move.
// ---------------------------------------------------------------------------

typedef struct audiobookdb_index audiobookdb_index_t;

// Builds a handle over the same list audiobookdb_for_each() would stream. NULL
// when the list cannot be built.
audiobookdb_index_t *audiobookdb_index_open(audiobook_list_t kind);
void audiobookdb_index_close(audiobookdb_index_t *ix);

int audiobookdb_index_count(const audiobookdb_index_t *ix);

// True once the rows the handle names may no longer be the rows it was built
// over -- a rescan, a card change, a book marked finished. Row ids are reused,
// so a stale handle reads other books rather than none: the caller has to
// rebuild rather than carry on.
bool audiobookdb_index_stale(const audiobookdb_index_t *ix);

// Reads `count` rows from `offset`, in the handle's order, one callback each.
// Returns how many were delivered: fewer than asked means the end of the list,
// and zero on a stale handle.
int audiobookdb_index_window(const audiobookdb_index_t *ix, int offset, int count, audiobook_row_cb cb, void *user);

// Bumped whenever the row ids may have moved.
unsigned audiobookdb_revision(void);

// Stamps a book as listened right now.
void audiobookdb_touch(const char *path);

// ---------------------------------------------------------------------------
// Books that have been heard to the end
// ---------------------------------------------------------------------------
//
// Their own table rather than a column: a rescan is a wipe-and-refill of
// AUDIOBOOK_TABLE, and "this book has been read" is not something a rescan is
// entitled to forget. In a separate table it survives without the temp-table
// dance last_played needs. A row for a book that has since left the card never
// shows: the listing joins the two.
void audiobookdb_mark_finished(const char *path);
bool audiobookdb_is_finished(const char *path);

// ---------------------------------------------------------------------------
// Where the listener got to
// ---------------------------------------------------------------------------
//
// A book is not a song. Coming back to one means coming back to the second it
// was left at, in the chapter it was left in -- the chapter alone would
// restart a forty-minute file from the top.
void audiobookdb_save_position(const char *book_path, const char *file, double seconds);
bool audiobookdb_get_position(const char *book_path, char *file_out, size_t file_size, double *seconds_out);

// Whether this exact path is one of the indexed books, copying it into
// `book_out` when it is. A book here is one .m4b file -- that is what the scan
// finds and where the chapters live -- so this is a lookup, not a walk up the
// directory tree.
//
// Asked of the path rather than of a flag set when playback began: a flag has
// to survive every way a track can change and the first one missed puts
// audiobook controls over a song. The path is still true after a reboot.
bool audiobookdb_book_for_file(const char *file_path, char *book_out, size_t book_size);

#endif /* AUDIOBOOKDB_H */
