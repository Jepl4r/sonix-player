#ifndef AUDIOBOOKDB_H
#define AUDIOBOOKDB_H

#include <stdbool.h>
#include <stddef.h>

// The audiobook index: a small SQLite database of the books in the Audiobooks
// folder, separate from the music library (which ignores .m4b -- an audiobook
// in the middle of an album listing helps nobody).
//
// A book is one of two things:
//
//   a single file: an .m4b, an .mp3 carrying ID3v2 chapter marks, an .opus,
//   .ogg or .flac carrying Vorbis-comment chapter marks, or a loose audio
//   file whose album tag no other file of its folder shares;
//
//   a folder: two or more loose audio files of one folder with the same album
//   tag -- one file per chapter. They are the book's parts, in natural name
//   order, and the files of "CD 1", "Disc 2" subfolders count as the folder's
//   own. Files with no album tag are one book together.
//
// A row per book, keyed by its path: the file, the folder, or -- for one of
// several folder books in the same folder -- the folder and the album, a path
// no file has. Title, author
// and series come from the tags; where the tags say nothing, the folders the
// book sits in do, read as Audiobooks/Author/Series/... . Sorting uses the
// same collation as the music library, so leading articles are skipped ("The
// Hobbit" files under H).

// Opens (creating if needed) the database. Safe to call again; false when the
// file cannot be opened, in which case everything below is a no-op.
bool audiobookdb_open(const char *db_path);
void audiobookdb_close(void);

// How many audiobooks the index holds.
int audiobookdb_count(void);

// True when the index was written by a scan that did not read authors, series
// or folder books yet. The page rescans once when it sees this.
bool audiobookdb_needs_rescan(void);

// --- Scanning (same shape as the music library's) ---
bool audiobookdb_scan_start(const char *root);
bool audiobookdb_scan_running(void);
int audiobookdb_scan_found(void);
void audiobookdb_scan_stop(void);

// --- Reading ---
typedef enum {
	AUDIOBOOK_LIST_ALL,		 // every book
	AUDIOBOOK_LIST_FINISHED, // the ones heard to the end
	AUDIOBOOK_LIST_CONTINUE, // the ones started and not back at their beginning
	AUDIOBOOK_LIST_AUTHOR,	 // the books of one author; "" is the books with none
	AUDIOBOOK_LIST_SERIES,	 // the books of one series
} audiobook_list_t;

// The orders a list can come in. Each can also be read backwards.
typedef enum {
	AUDIOBOOK_ORDER_NAME,	// by title
	AUDIOBOOK_ORDER_ADDED,	// by when the book landed on the card, oldest first;
							// on the finished list, by when it was finished
	AUDIOBOOK_ORDER_PLAYED, // by when it was last listened to, oldest first
	AUDIOBOOK_ORDER_SERIES, // by the place in the series, then by title
} audiobook_order_t;

// A book as a list row. `name` is the title, prefixed with the book's number
// in its series on a series list ("3. The Title"); `path` is the file or the
// folder.
typedef bool (*audiobook_row_cb)(const char *name, const char *path, void *user);

// ---------------------------------------------------------------------------
// List handles
//
// Reading a whole list in means a name and a path allocated per row, where a
// handle holds the SQLite row id and reads a windowful of real rows back as the
// viewport moves. A handle goes stale when the rows underneath it move.
// ---------------------------------------------------------------------------

typedef struct audiobookdb_index audiobookdb_index_t;

// `value` is the author or the series for the two filtered lists, ignored for
// the others. NULL when the list cannot be built.
audiobookdb_index_t *audiobookdb_index_open(audiobook_list_t kind, const char *value, audiobook_order_t order, bool desc);
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

// ---------------------------------------------------------------------------
// Authors and series
// ---------------------------------------------------------------------------

typedef enum {
	AUDIOBOOK_NAMES_AUTHORS,
	AUDIOBOOK_NAMES_SERIES,
} audiobook_names_t;

// Streams the authors or the series, one callback each with the name and how
// many books carry it. By name, or with `by_added` by the newest book of each,
// oldest first; `desc` reverses either. Authors include "" for the books with
// none, always last. Returns how many were delivered.
typedef bool (*audiobook_name_cb)(const char *name, int books, void *user);
int audiobookdb_names_for_each(audiobook_names_t kind, bool by_added, bool desc, audiobook_name_cb cb, void *user);

// ---------------------------------------------------------------------------
// The parts of a folder book
// ---------------------------------------------------------------------------

// True when `book` is a folder book.
bool audiobookdb_is_folder_book(const char *book);

// Streams the parts of a folder book in their order: the file and the name it
// is shown under. Returns how many were delivered; 0 for a single-file book.
typedef bool (*audiobook_part_cb)(const char *path, const char *title, void *user);
int audiobookdb_parts_for_each(const char *book, audiobook_part_cb cb, void *user);

// Stamps a book as listened right now.
void audiobookdb_touch(const char *path);

// ---------------------------------------------------------------------------
// Books that have been heard to the end
// ---------------------------------------------------------------------------
//
// Their own table rather than a column: a rescan is a wipe-and-refill of
// AUDIOBOOK_TABLE, and "this book has been read" is not something a rescan is
// entitled to forget. A row for a book that has since left the card never
// shows: the listing joins the two.
void audiobookdb_mark_finished(const char *path);
bool audiobookdb_is_finished(const char *path);

// ---------------------------------------------------------------------------
// Where the listener got to
// ---------------------------------------------------------------------------
//
// A book is not a song. Coming back to one means coming back to the second it
// was left at, in the file it was left in: the chapter file of a folder book,
// or the book's own file.
void audiobookdb_save_position(const char *book_path, const char *file, double seconds);
bool audiobookdb_get_position(const char *book_path, char *file_out, size_t file_size, double *seconds_out);

// Which book a file belongs to, copying the book's path into `book_out`: the
// file itself for a single-file book, the folder for a part of a folder book.
// False for a file that is no book's.
//
// Asked of the path rather than of a flag set when playback began: a flag has
// to survive every way a track can change and the first one missed puts
// audiobook controls over a song. The path is still true after a reboot.
bool audiobookdb_book_for_file(const char *file_path, char *book_out, size_t book_size);

#endif /* AUDIOBOOKDB_H */
