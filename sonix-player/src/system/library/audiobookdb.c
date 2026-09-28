#define _GNU_SOURCE 1 // strcasestr
#include "audiobookdb.h"
#include "src/system/library/id3chap.h"
#include "src/system/library/vorbischap.h"
#include "src/system/library/metadata.h"
#include "src/system/playback/playlist.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/utils.h"
#include "src/system/db/sqlite3.h"
#include "src/system/library/library.h" // library_collate_listorder: the shared ordering

#define SCAN_MAX_DEPTH 12
#define SCAN_COMMIT_EVERY 50

// Entries read from one folder at most, files and subfolders together.
#define SCAN_MAX_ENTRIES 4096

// PRAGMA user_version of an index whose rows carry authors, series and folder
// books. Anything lower was written by an older scan.
#define SCHEMA_VERSION 2

static sqlite3 *db;
static pthread_mutex_t db_lock = PTHREAD_MUTEX_INITIALIZER;

// Bumped whenever the row ids of AUDIOBOOK_TABLE may have moved: a scan empties
// and refills it, a different card is a different set of books, and marking one
// finished changes which rows the finished list names. A handle built before
// the bump is reading somebody else's rows, so it is made to say so instead.
static unsigned generation = 1;

static bool outdated;

// Call with db_lock held.
static void bump_generation(void) { generation++; }

unsigned audiobookdb_revision(void) {
	pthread_mutex_lock(&db_lock);
	unsigned value = generation;
	pthread_mutex_unlock(&db_lock);
	return value;
}

static pthread_t scan_thread;
static volatile bool scan_running;
static volatile bool scan_cancel;
static volatile int scan_found;
static char scan_root[512];

// For statements that are expected to fail once the schema is already right:
// the ALTER TABLEs below run on every open and say "duplicate column name" on
// every open after the first.
static bool exec_quiet(const char *sql) {
	char *error = NULL;
	bool ok = sqlite3_exec(db, sql, NULL, NULL, &error) == SQLITE_OK;
	sqlite3_free(error);
	return ok;
}

static bool exec(const char *sql) {
	char *error = NULL;
	if (sqlite3_exec(db, sql, NULL, NULL, &error) != SQLITE_OK) {
		fprintf(stderr, "audiobooks: %s\n", error ? error : "unknown error");
		sqlite3_free(error);
		return false;
	}
	return true;
}

static int user_version(void) {
	int version = 0;
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			version = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	return version;
}

bool audiobookdb_open(const char *db_path) {
	if (db) {
		return true;
	}
	if (!db_path || !db_path[0]) {
		return false;
	}

	if (sqlite3_open(db_path, &db) != SQLITE_OK) {
		fprintf(stderr, "audiobooks: cannot open %s: %s\n", db_path, sqlite3_errmsg(db));
		sqlite3_close(db);
		db = NULL;
		return false;
	}

	sqlite3_create_collation(db, "listorder", SQLITE_UTF8, NULL, library_collate_listorder);

	// Same journal choices as the music library: a slow card that may be
	// pulled at any moment.
	exec("PRAGMA synchronous=NORMAL");
	exec("PRAGMA journal_mode=TRUNCATE");
	exec("PRAGMA cache_size=-128");

	exec("CREATE TABLE IF NOT EXISTS AUDIOBOOK_TABLE("
		 "path TEXT PRIMARY KEY, name TEXT COLLATE NOCASE, size INT, mtime INT,"
		 "last_played INT DEFAULT 0, resume_file TEXT, resume_pos REAL,"
		 "author TEXT DEFAULT '', series TEXT DEFAULT '', series_part REAL, added INT DEFAULT 0,"
		 "folder INT DEFAULT 0)");

	// The columns a database written by an older scan does not have. They fail
	// harmlessly once they are there, and must run after the CREATE, or on a
	// fresh card there is no table to alter.
	exec_quiet("ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN resume_file TEXT");
	exec_quiet("ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN resume_pos REAL");
	exec_quiet("ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN author TEXT DEFAULT ''");
	exec_quiet("ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN series TEXT DEFAULT ''");
	exec_quiet("ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN series_part REAL");
	exec_quiet("ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN added INT DEFAULT 0");
	exec_quiet("ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN folder INT DEFAULT 0");

	// The files of the folder books, a row each, in the book's order.
	exec("CREATE TABLE IF NOT EXISTS AUDIOBOOK_PARTS(path TEXT PRIMARY KEY, book TEXT, idx INT, title TEXT)");
	exec("CREATE INDEX IF NOT EXISTS AUDIOBOOK_PARTS_BOOK ON AUDIOBOOK_PARTS(book, idx)");

	// The books heard to the end. Deliberately not a column: a rescan wipes
	// AUDIOBOOK_TABLE and refills it, and having read a book is not something
	// a rescan may forget.
	exec("CREATE TABLE IF NOT EXISTS AUDIOBOOK_FINISHED("
		 "path TEXT PRIMARY KEY, finished_at INT DEFAULT 0)");

	int books = 0;
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM AUDIOBOOK_TABLE", -1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			books = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	outdated = books > 0 && user_version() < SCHEMA_VERSION;

	// A different card is a different set of books, and the row ids that named
	// the last one's are now this one's.
	pthread_mutex_lock(&db_lock);
	bump_generation();
	pthread_mutex_unlock(&db_lock);

	printf("audiobooks: %s open, %d books indexed%s\n", db_path, books, outdated ? " (an older scan)" : "");
	return true;
}

void audiobookdb_close(void) {
	// Waited on, not just asked: a scan still running holds the database open,
	// sqlite3_close() then answers SQLITE_BUSY without closing the file, and
	// an open file on the card stops the card being unmounted.
	audiobookdb_scan_stop();
	for (int i = 0; i < 400 && audiobookdb_scan_running(); i++) {
		usleep(10 * 1000);
	}

	pthread_mutex_lock(&db_lock);
	if (db) {
		int rc = sqlite3_close(db);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "audiobooks: sqlite3_close returned %d; the handle may be leaking\n", rc);
		}
		db = NULL;
	}
	outdated = false;
	pthread_mutex_unlock(&db_lock);
}

bool audiobookdb_needs_rescan(void) { return db && outdated && !scan_running; }

int audiobookdb_count(void) {
	pthread_mutex_lock(&db_lock);

	int count = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM AUDIOBOOK_TABLE", -1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			count = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}

	pthread_mutex_unlock(&db_lock);
	return count;
}

// ---------------------------------------------------------------------------
// List handles
//
// The one ordered pass happens at open and its result is four bytes a row; the
// rows themselves are read back a windowful at a time, by row id.
// ---------------------------------------------------------------------------

struct audiobookdb_index {
	int32_t *rows;
	int count;
	unsigned generation;
	bool with_part; // the names carry the book's number in its series
};

// The ORDER BY of a list. `t` is the book table's alias, `when` the column the
// date order reads.
static void order_clause(char *out, size_t size, audiobook_order_t order, bool desc, const char *when) {
	const char *dir = desc ? "DESC" : "ASC";
	switch (order) {
	// Books from the same moment, or never played, keep A-Z among themselves.
	case AUDIOBOOK_ORDER_ADDED:
		snprintf(out, size, "%s %s, t.name COLLATE listorder", when, dir);
		break;
	case AUDIOBOOK_ORDER_PLAYED:
		snprintf(out, size, "t.last_played %s, t.name COLLATE listorder", dir);
		break;
	case AUDIOBOOK_ORDER_SERIES:
		// The books with no number go after the numbered ones.
		snprintf(out, size, "t.series_part IS NULL, t.series_part %s, t.name COLLATE listorder %s", dir, dir);
		break;
	default:
		snprintf(out, size, "t.name COLLATE listorder %s", dir);
		break;
	}
}

audiobookdb_index_t *audiobookdb_index_open(audiobook_list_t kind, const char *value, audiobook_order_t order, bool desc) {
	struct audiobookdb_index *ix = calloc(1, sizeof(*ix));
	if (!ix) {
		return NULL;
	}
	ix->with_part = kind == AUDIOBOOK_LIST_SERIES;

	char orderby[160];
	char sql[640];
	switch (kind) {
	case AUDIOBOOK_LIST_FINISHED:
		// The join keeps a book that has left the card out of the list without
		// deleting the fact that it was finished: put the file back and it is
		// there again.
		order_clause(orderby, sizeof(orderby), order, desc, "f.finished_at");
		snprintf(sql, sizeof(sql), "SELECT t.rowid FROM AUDIOBOOK_TABLE t JOIN AUDIOBOOK_FINISHED f ON f.path = t.path ORDER BY %s", orderby);
		break;
	case AUDIOBOOK_LIST_CONTINUE:
		// Listened to, and left somewhere past the start of the first file: a
		// book finished goes back to that start, and so leaves this list.
		order_clause(orderby, sizeof(orderby), order, desc, "t.added");
		snprintf(sql, sizeof(sql),
				 "SELECT t.rowid FROM AUDIOBOOK_TABLE t WHERE t.last_played > 0 AND t.resume_file IS NOT NULL AND"
				 " (t.resume_pos > 1 OR t.resume_file != COALESCE((SELECT p.path FROM AUDIOBOOK_PARTS p"
				 " WHERE p.book = t.path ORDER BY p.idx LIMIT 1), t.path)) ORDER BY %s",
				 orderby);
		break;
	case AUDIOBOOK_LIST_AUTHOR:
		order_clause(orderby, sizeof(orderby), order, desc, "t.added");
		snprintf(sql, sizeof(sql), "SELECT t.rowid FROM AUDIOBOOK_TABLE t WHERE t.author = ?1 COLLATE NOCASE ORDER BY %s", orderby);
		break;
	case AUDIOBOOK_LIST_SERIES:
		order_clause(orderby, sizeof(orderby), order, desc, "t.added");
		snprintf(sql, sizeof(sql), "SELECT t.rowid FROM AUDIOBOOK_TABLE t WHERE t.series = ?1 COLLATE NOCASE ORDER BY %s", orderby);
		break;
	default:
		order_clause(orderby, sizeof(orderby), order, desc, "t.added");
		snprintf(sql, sizeof(sql), "SELECT t.rowid FROM AUDIOBOOK_TABLE t ORDER BY %s", orderby);
		break;
	}

	pthread_mutex_lock(&db_lock);
	ix->generation = generation;

	int capacity = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		if (kind == AUDIOBOOK_LIST_AUTHOR || kind == AUDIOBOOK_LIST_SERIES) {
			sqlite3_bind_text(stmt, 1, value ? value : "", -1, SQLITE_TRANSIENT);
		}
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			if (ix->count == capacity) {
				int grown = capacity ? capacity * 2 : 64;
				int32_t *bigger = realloc(ix->rows, (size_t)grown * sizeof(*bigger));
				if (!bigger) {
					break;
				}
				ix->rows = bigger;
				capacity = grown;
			}
			ix->rows[ix->count++] = (int32_t)sqlite3_column_int64(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return ix;
}

void audiobookdb_index_close(audiobookdb_index_t *ix) {
	if (!ix) {
		return;
	}
	free(ix->rows);
	free(ix);
}

int audiobookdb_index_count(const audiobookdb_index_t *ix) { return ix ? ix->count : 0; }

bool audiobookdb_index_stale(const audiobookdb_index_t *ix) {
	if (!ix) {
		return true;
	}
	pthread_mutex_lock(&db_lock);
	bool stale = ix->generation != generation;
	pthread_mutex_unlock(&db_lock);
	return stale;
}

int audiobookdb_index_window(const audiobookdb_index_t *ix, int offset, int count, audiobook_row_cb cb, void *user) {
	if (!ix || !cb || offset < 0 || count <= 0 || offset >= ix->count) {
		return 0;
	}
	if (offset + count > ix->count) {
		count = ix->count - offset;
	}

	pthread_mutex_lock(&db_lock);

	// A handle from before a rescan or a card change names rows that belong to
	// somebody else now. Answering nothing is what makes the caller reload.
	if (ix->generation != generation) {
		pthread_mutex_unlock(&db_lock);
		return 0;
	}

	int delivered = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT name, path, series_part FROM AUDIOBOOK_TABLE WHERE rowid=?", -1, &stmt, NULL) == SQLITE_OK) {
		for (int i = 0; i < count; i++) {
			sqlite3_reset(stmt);
			sqlite3_bind_int64(stmt, 1, ix->rows[offset + i]);
			if (sqlite3_step(stmt) != SQLITE_ROW) {
				// The row went while the window was being read. The window is
				// positional, so it gets a blank rather than the next row
				// shifted up into its place.
				if (!cb("", "", user)) {
					break;
				}
				delivered++;
				continue;
			}
			const char *name = (const char *)sqlite3_column_text(stmt, 0);
			const char *path = (const char *)sqlite3_column_text(stmt, 1);
			char numbered[300];
			if (ix->with_part && sqlite3_column_type(stmt, 2) != SQLITE_NULL) {
				snprintf(numbered, sizeof(numbered), "%g. %s", sqlite3_column_double(stmt, 2), name ? name : "");
				name = numbered;
			}
			if (!cb(name ? name : "", path ? path : "", user)) {
				break;
			}
			delivered++;
		}
		sqlite3_finalize(stmt);
	}

	pthread_mutex_unlock(&db_lock);
	return delivered;
}

// ---------------------------------------------------------------------------
// Authors and series
// ---------------------------------------------------------------------------

int audiobookdb_names_for_each(audiobook_names_t kind, bool by_added, bool desc, audiobook_name_cb cb, void *user) {
	if (!cb) {
		return 0;
	}
	const char *column = kind == AUDIOBOOK_NAMES_SERIES ? "series" : "author";
	const char *where = kind == AUDIOBOOK_NAMES_SERIES ? "WHERE series != ''" : "";
	const char *dir = desc ? "DESC" : "ASC";
	char orderby[128];
	if (by_added) {
		snprintf(orderby, sizeof(orderby), "MAX(added) %s, %s COLLATE listorder", dir, column);
	} else {
		snprintf(orderby, sizeof(orderby), "%s COLLATE listorder %s", column, dir);
	}
	// The books with no author are one group, and it goes last.
	char sql[400];
	snprintf(sql, sizeof(sql), "SELECT %s, COUNT(*) FROM AUDIOBOOK_TABLE %s GROUP BY %s COLLATE NOCASE ORDER BY %s = '', %s", column, where, column, column, orderby);

	pthread_mutex_lock(&db_lock);
	int delivered = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *name = (const char *)sqlite3_column_text(stmt, 0);
			if (!cb(name ? name : "", sqlite3_column_int(stmt, 1), user)) {
				break;
			}
			delivered++;
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return delivered;
}

// ---------------------------------------------------------------------------
// The parts of a folder book
// ---------------------------------------------------------------------------

bool audiobookdb_is_folder_book(const char *book) {
	if (!book || !book[0]) {
		return false;
	}
	pthread_mutex_lock(&db_lock);
	bool folder = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT folder FROM AUDIOBOOK_TABLE WHERE path=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, book, -1, SQLITE_TRANSIENT);
		folder = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) != 0;
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return folder;
}

int audiobookdb_parts_for_each(const char *book, audiobook_part_cb cb, void *user) {
	if (!book || !book[0] || !cb) {
		return 0;
	}
	pthread_mutex_lock(&db_lock);
	int delivered = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT path, title FROM AUDIOBOOK_PARTS WHERE book=? ORDER BY idx", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, book, -1, SQLITE_TRANSIENT);
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *path = (const char *)sqlite3_column_text(stmt, 0);
			const char *title = (const char *)sqlite3_column_text(stmt, 1);
			if (!cb(path ? path : "", title ? title : "", user)) {
				break;
			}
			delivered++;
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return delivered;
}

// ---------------------------------------------------------------------------
// Where the listener got to
// ---------------------------------------------------------------------------

void audiobookdb_save_position(const char *book_path, const char *file, double seconds) {
	if (!book_path || !book_path[0] || !file || !file[0]) {
		return;
	}

	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "UPDATE AUDIOBOOK_TABLE SET resume_file=?, resume_pos=?, last_played=? WHERE path=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, file, -1, SQLITE_TRANSIENT);
		sqlite3_bind_double(stmt, 2, seconds);
		sqlite3_bind_int64(stmt, 3, (sqlite3_int64)time(NULL));
		sqlite3_bind_text(stmt, 4, book_path, -1, SQLITE_TRANSIENT);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
}

bool audiobookdb_get_position(const char *book_path, char *file_out, size_t file_size, double *seconds_out) {
	if (file_out && file_size) {
		file_out[0] = '\0';
	}
	if (seconds_out) {
		*seconds_out = 0;
	}
	if (!book_path || !book_path[0]) {
		return false;
	}

	pthread_mutex_lock(&db_lock);

	bool found = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT resume_file, resume_pos FROM AUDIOBOOK_TABLE WHERE path=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, book_path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *file = (const char *)sqlite3_column_text(stmt, 0);
			if (file && file[0]) {
				if (file_out && file_size) {
					snprintf(file_out, file_size, "%s", file);
				}
				if (seconds_out) {
					*seconds_out = sqlite3_column_double(stmt, 1);
				}
				found = true;
			}
		}
		sqlite3_finalize(stmt);
	}

	pthread_mutex_unlock(&db_lock);
	return found;
}

// One or two primary-key lookups per track change, with nothing cached to go
// stale: the file as a book of its own, then as a part of a folder book.
bool audiobookdb_book_for_file(const char *file_path, char *book_out, size_t book_size) {
	if (book_out && book_size) {
		book_out[0] = '\0';
	}
	if (!db || !file_path || !file_path[0]) {
		return false;
	}

	pthread_mutex_lock(&db_lock);

	char book[512] = "";
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT path FROM AUDIOBOOK_TABLE WHERE path=? AND folder=0", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, file_path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			snprintf(book, sizeof(book), "%s", file_path);
		}
		sqlite3_finalize(stmt);
	}
	if (!book[0] && sqlite3_prepare_v2(db, "SELECT book FROM AUDIOBOOK_PARTS WHERE path=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, file_path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *owner = (const char *)sqlite3_column_text(stmt, 0);
			snprintf(book, sizeof(book), "%s", owner ? owner : "");
		}
		sqlite3_finalize(stmt);
	}

	pthread_mutex_unlock(&db_lock);

	if (book[0] && book_out && book_size) {
		snprintf(book_out, book_size, "%s", book);
	}
	return book[0] != '\0';
}

void audiobookdb_mark_finished(const char *path) {
	if (!path || !path[0]) {
		return;
	}

	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO AUDIOBOOK_FINISHED(path, finished_at) VALUES(?,?)", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 2, (sqlite3_int64)time(NULL));
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	// Not the row ids, but which rows the finished list names -- a handle over
	// it is describing a list that has just gained a book.
	bump_generation();
	pthread_mutex_unlock(&db_lock);
}

bool audiobookdb_is_finished(const char *path) {
	if (!path || !path[0]) {
		return false;
	}

	pthread_mutex_lock(&db_lock);
	bool hit = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT 1 FROM AUDIOBOOK_FINISHED WHERE path=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		hit = sqlite3_step(stmt) == SQLITE_ROW;
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return hit;
}

void audiobookdb_touch(const char *path) {
	if (!path || !path[0]) {
		return;
	}

	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "UPDATE AUDIOBOOK_TABLE SET last_played=? WHERE path=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, (sqlite3_int64)time(NULL));
		sqlite3_bind_text(stmt, 2, path, -1, SQLITE_TRANSIENT);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	// The order by last listened has changed, which a handle over that list
	// has to be told. Not done in audiobookdb_save_position(), which writes
	// the same column every ten seconds while a book plays.
	bump_generation();
	pthread_mutex_unlock(&db_lock);
}

// ---------------------------------------------------------------------------
// scanning
//
// Only the Audiobooks folder at the root of the card, walked to any depth.
//
// A folder is read whole before anything in it is indexed, and closed before
// its subfolders are walked, so one directory is open at a time whatever the
// depth. In each folder:
//
//   an .m4b, or an .mp3, .opus, .ogg or .flac with chapter marks, is a book
//   of its own;
//
//   the other audio files, together with those of its "CD 1" / "Disc 2"
//   subfolders, are grouped by album tag: a group of two or more is a folder
//   book, a group of one a book of its own. Loose files directly in the
//   Audiobooks folder are always a book each.
// ---------------------------------------------------------------------------

typedef struct {
	char **items;
	int count;
	int capacity;
} namelist_t;

static bool namelist_add(namelist_t *l, const char *name) {
	if (l->count >= SCAN_MAX_ENTRIES) {
		return false;
	}
	if (l->count == l->capacity) {
		int grown = l->capacity ? l->capacity * 2 : 32;
		char **bigger = realloc(l->items, (size_t)grown * sizeof(*bigger));
		if (!bigger) {
			return false;
		}
		l->items = bigger;
		l->capacity = grown;
	}
	char *copy = strdup(name);
	if (!copy) {
		return false;
	}
	l->items[l->count++] = copy;
	return true;
}

static void namelist_free(namelist_t *l) {
	for (int i = 0; i < l->count; i++) {
		free(l->items[i]);
	}
	free(l->items);
	l->items = NULL;
	l->count = 0;
	l->capacity = 0;
}

// Case-insensitive, with runs of digits compared as numbers: "2 x" before
// "10 x", which is how chapter files are numbered.
static int natural_compare(const char *a, const char *b) {
	while (*a && *b) {
		if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			while (*a == '0') {
				a++;
			}
			while (*b == '0') {
				b++;
			}
			size_t la = 0;
			size_t lb = 0;
			while (isdigit((unsigned char)a[la])) {
				la++;
			}
			while (isdigit((unsigned char)b[lb])) {
				lb++;
			}
			if (la != lb) {
				return la < lb ? -1 : 1;
			}
			int c = strncmp(a, b, la);
			if (c) {
				return c;
			}
			a += la;
			b += lb;
			continue;
		}
		int ca = tolower((unsigned char)*a);
		int cb = tolower((unsigned char)*b);
		if (ca != cb) {
			return ca < cb ? -1 : 1;
		}
		a++;
		b++;
	}
	return *a ? 1 : (*b ? -1 : 0);
}

static int natural_qsort(const void *a, const void *b) { return natural_compare(*(const char *const *)a, *(const char *const *)b); }

// Reads the names in `path`: files into `files`, subfolders into `dirs`, each
// in natural order. Dot files and the folders a desktop leaves behind are
// skipped, and so are links to folders -- a link to an ancestor would walk the
// same branch at every level.
static bool read_folder(const char *path, namelist_t *files, namelist_t *dirs) {
	DIR *dir = opendir(path);
	if (!dir) {
		return false;
	}
	struct dirent *de;
	while (!scan_cancel && (de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.' || playlist_is_junk_name(de->d_name)) {
			continue;
		}
		char child[768];
		if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >= (int)sizeof(child)) {
			continue;
		}
		struct stat st;
		if (lstat(child, &st) != 0) {
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			namelist_add(dirs, de->d_name);
		} else if (S_ISREG(st.st_mode) || (S_ISLNK(st.st_mode) && stat(child, &st) == 0 && S_ISREG(st.st_mode))) {
			namelist_add(files, de->d_name);
		}
	}
	closedir(dir);
	if (files->count > 1) {
		qsort(files->items, (size_t)files->count, sizeof(char *), natural_qsort);
	}
	if (dirs->count > 1) {
		qsort(dirs->items, (size_t)dirs->count, sizeof(char *), natural_qsort);
	}
	return true;
}

// The audio a folder book can be made of.
static bool is_audio(const char *name) {
	static const char *const EXTENSIONS[] = {".mp3", ".m4a", ".m4b", ".aac", ".flac", ".ogg", ".opus", ".wav", ".wv", ".ape", ".mp4"};
	for (size_t i = 0; i < sizeof(EXTENSIONS) / sizeof(EXTENSIONS[0]); i++) {
		if (has_extension(name, EXTENSIONS[i])) {
			return true;
		}
	}
	return false;
}

// A book in one file whatever its neighbours: an .m4b, or an .mp3, .opus, .ogg
// or .flac whose chapters are marked inside it.
static bool is_book_file(const char *path, const char *name) {
	if (has_extension(name, ".m4b")) {
		return true;
	}
	if (has_extension(name, ".mp3")) {
		return id3chap_present(path);
	}
	if (has_extension(name, ".opus") || has_extension(name, ".ogg") || has_extension(name, ".flac")) {
		return vorbischap_present(path);
	}
	return false;
}

// "CD 1", "CD01", "Disc 2", "Disk 3": the halves of one book split the way a
// set of discs was.
static bool is_disc_folder(const char *name) {
	const char *rest = NULL;
	if (strncasecmp(name, "cd", 2) == 0) {
		rest = name + 2;
	} else if (strncasecmp(name, "disc", 4) == 0 || strncasecmp(name, "disk", 4) == 0) {
		rest = name + 4;
	} else {
		return false;
	}
	while (*rest == ' ' || *rest == '_' || *rest == '-' || *rest == '.') {
		rest++;
	}
	if (!isdigit((unsigned char)*rest)) {
		return false;
	}
	while (isdigit((unsigned char)*rest)) {
		rest++;
	}
	return *rest == '\0';
}

static void strip_extension(char *name) {
	char *dot = strrchr(name, '.');
	if (dot && dot != name) {
		*dot = '\0';
	}
}

// A book's number in its series, from the tag ("3", "2.5", "3/12") or, when
// there is none, from the front of its name: "3 - Title", "03. Title", "Book 3
// - Title", "Vol. 2", "#4", or the series' own name and then the number,
// "Mistborn 1 The Final Empire". -1 when there is no number.
static double series_number(const char *text, bool from_name, const char *series) {
	if (!text || !text[0]) {
		return -1;
	}
	const char *p = text;
	while (*p == ' ') {
		p++;
	}
	size_t series_len = series ? strlen(series) : 0;
	if (from_name && series_len && strncasecmp(p, series, series_len) == 0) {
		p += series_len;
		while (*p == ' ' || *p == '-' || *p == ',' || *p == ':' || *p == '_') {
			p++;
		}
	}
	if (from_name) {
		static const char *const PREFIXES[] = {"book", "vol.", "vol", "volume", "part", "tome", "band", "libro", "#"};
		for (size_t i = 0; i < sizeof(PREFIXES) / sizeof(PREFIXES[0]); i++) {
			size_t n = strlen(PREFIXES[i]);
			if (strncasecmp(p, PREFIXES[i], n) == 0 && (isdigit((unsigned char)p[n]) || p[n] == ' ')) {
				p += n;
				while (*p == ' ') {
					p++;
				}
				break;
			}
		}
	}
	if (!isdigit((unsigned char)*p)) {
		return -1;
	}
	char *end = NULL;
	double value = strtod(p, &end);
	if (end == p || value < 0 || value > 100000) {
		return -1;
	}
	// From a name the number has to stand apart from the title: "1984" is a
	// title, not book 1984 of anything.
	if (from_name && *end && *end != ' ' && *end != '.' && *end != '-' && *end != ')' && *end != ':' && *end != '_') {
		return -1;
	}
	if (from_name && value >= 1000) {
		return -1;
	}
	return value;
}

typedef struct {
	char title[256];
	char author[256];
	char series[256];
	double series_part; // -1: none
	long long added;
	long long size;
	long long mtime;
	bool folder;
} book_t;

// Fills what the tags left empty from the folders the book sits in, read as
// Audiobooks/Author/Series. `rel` is the path of those folders under the
// Audiobooks folder, without the book's own folder.
static void fill_from_folders(book_t *b, const char *rel) {
	char first[256] = "";
	char second[256] = "";
	const char *slash = rel ? strchr(rel, '/') : NULL;
	if (rel && rel[0]) {
		size_t n = slash ? (size_t)(slash - rel) : strlen(rel);
		if (n >= sizeof(first)) {
			n = sizeof(first) - 1;
		}
		memcpy(first, rel, n);
		first[n] = '\0';
	}
	if (slash) {
		const char *next = slash + 1;
		const char *end = strchr(next, '/');
		size_t n = end ? (size_t)(end - next) : strlen(next);
		if (n >= sizeof(second)) {
			n = sizeof(second) - 1;
		}
		memcpy(second, next, n);
		second[n] = '\0';
	}
	if (!b->author[0]) {
		snprintf(b->author, sizeof(b->author), "%s", first);
	}
	if (!b->series[0]) {
		snprintf(b->series, sizeof(b->series), "%s", second);
	}
}

static void book_from_tags(book_t *b, const song_metadata_t *tags) {
	snprintf(b->author, sizeof(b->author), "%s", tags->album_artist[0] ? tags->album_artist : tags->artist);
	snprintf(b->series, sizeof(b->series), "%s", tags->series);
	b->series_part = series_number(tags->series_part, false, NULL);
}

// Under db_lock, inside the scan's transaction.
static void insert_book(const char *path, const book_t *b) {
	sqlite3_stmt *stmt = NULL;
	// Where the listener was survives a rescan: the scan stashes it in a temp
	// table before wiping, and each insert copies its own back.
	if (sqlite3_prepare_v2(db,
						   "INSERT OR REPLACE INTO AUDIOBOOK_TABLE"
						   "(path,name,size,mtime,author,series,series_part,added,folder,"
						   "last_played,resume_file,resume_pos)"
						   " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,"
						   "COALESCE((SELECT last_played FROM old_state WHERE path=?1),0),"
						   "(SELECT resume_file FROM old_state WHERE path=?1),"
						   "(SELECT resume_pos FROM old_state WHERE path=?1))",
						   -1, &stmt, NULL) != SQLITE_OK) {
		return;
	}
	sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, b->title, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, b->size);
	sqlite3_bind_int64(stmt, 4, b->mtime);
	sqlite3_bind_text(stmt, 5, b->author, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, b->series, -1, SQLITE_TRANSIENT);
	if (b->series[0] && b->series_part >= 0) {
		sqlite3_bind_double(stmt, 7, b->series_part);
	} else {
		sqlite3_bind_null(stmt, 7);
	}
	sqlite3_bind_int64(stmt, 8, b->added);
	sqlite3_bind_int(stmt, 9, b->folder ? 1 : 0);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	scan_found++;
	if (scan_found % SCAN_COMMIT_EVERY == 0) {
		exec("COMMIT");
		sqlite3_db_release_memory(db);
		exec("BEGIN");
	}
}

static void insert_part(const char *path, const char *book, int index, const char *title) {
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO AUDIOBOOK_PARTS(path,book,idx,title) VALUES(?,?,?,?)", -1, &stmt, NULL) != SQLITE_OK) {
		return;
	}
	sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, book, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 3, index);
	sqlite3_bind_text(stmt, 4, title, -1, SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

// The path of `dir` under the Audiobooks folder, "" for the folder itself.
static const char *relative(const char *dir) {
	size_t root = strlen(scan_root);
	if (strncmp(dir, scan_root, root) != 0) {
		return "";
	}
	dir += root;
	return *dir == '/' ? dir + 1 : dir;
}

// The folder above `rel`, into `out`: "A/S/B" -> "A/S".
static void parent_of(const char *rel, char *out, size_t size) {
	snprintf(out, size, "%s", rel);
	char *slash = strrchr(out, '/');
	if (slash) {
		*slash = '\0';
	} else {
		out[0] = '\0';
	}
}

// Whether the folder `rel` ends in is the book's own: named like the file, or
// like the title, one inside the other.
static bool named_like(const char *rel, const char *stem, const char *title) {
	const char *folder = strrchr(rel, '/');
	folder = folder ? folder + 1 : rel;
	if (!folder[0]) {
		return false;
	}
	return strcasestr(folder, stem) || strcasestr(stem, folder) || (title[0] && (strcasestr(folder, title) || strcasestr(title, folder)));
}

// A book in one file, in the folder `rel` under the Audiobooks folder. `alone`
// says it is the only thing in that folder, which is then its own folder when
// it is named like it, and the folders above it are its author and series.
static void index_single(const char *path, const char *name, const char *rel, bool alone) {
	struct stat st;
	if (stat(path, &st) != 0) {
		return;
	}
	song_metadata_t *tags = calloc(1, sizeof(*tags));
	book_t *b = calloc(1, sizeof(*b));
	if (!tags || !b) {
		free(tags);
		free(b);
		return;
	}
	metadata_read(path, tags);

	book_from_tags(b, tags);
	if (tags->title[0]) {
		snprintf(b->title, sizeof(b->title), "%s", tags->title);
	} else if (tags->album[0]) {
		snprintf(b->title, sizeof(b->title), "%s", tags->album);
	} else {
		snprintf(b->title, sizeof(b->title), "%s", name);
		strip_extension(b->title);
	}
	char stem[256];
	snprintf(stem, sizeof(stem), "%s", name);
	strip_extension(stem);
	char above[512];
	if (alone && named_like(rel, stem, b->title)) {
		parent_of(rel, above, sizeof(above));
	} else {
		snprintf(above, sizeof(above), "%s", rel);
	}
	fill_from_folders(b, above);
	if (b->series[0] && b->series_part < 0) {
		b->series_part = series_number(stem, true, b->series);
		if (b->series_part < 0) {
			b->series_part = series_number(b->title, true, b->series);
		}
	}
	b->size = (long long)st.st_size;
	b->mtime = (long long)st.st_mtime;
	b->added = (long long)st.st_ctime;
	b->folder = false;

	pthread_mutex_lock(&db_lock);
	if (db) {
		insert_book(path, b);
	}
	pthread_mutex_unlock(&db_lock);

	free(tags);
	free(b);
}

// A book made of the files in `parts`, which sit in folder `dir` and its disc
// folders; `key` is the path the book is indexed under. `own_folder` says the
// folder holds this book alone, so it is the book's folder and the folders
// above it are its author and series; otherwise the folder is the series of
// several books.
static void index_folder(const char *dir, const char *key, const namelist_t *parts, bool own_folder) {
	song_metadata_t *tags = calloc(1, sizeof(*tags));
	book_t *b = calloc(1, sizeof(*b));
	if (!tags || !b) {
		free(tags);
		free(b);
		return;
	}

	const char *folder_name = strrchr(dir, '/');
	folder_name = folder_name ? folder_name + 1 : dir;

	b->folder = true;
	b->series_part = -1;
	for (int i = 0; i < parts->count && !scan_cancel; i++) {
		const char *path = parts->items[i];
		struct stat st;
		if (stat(path, &st) != 0) {
			continue;
		}
		memset(tags, 0, sizeof(*tags));
		metadata_read(path, tags);
		if (i == 0) {
			book_from_tags(b, tags);
			snprintf(b->title, sizeof(b->title), "%s", tags->album[0] ? tags->album : folder_name);
			b->added = (long long)st.st_ctime;
		}
		b->size += (long long)st.st_size;
		if ((long long)st.st_mtime > b->mtime) {
			b->mtime = (long long)st.st_mtime;
		}

		char title[256];
		if (tags->title[0]) {
			snprintf(title, sizeof(title), "%s", tags->title);
		} else {
			const char *base = strrchr(path, '/');
			snprintf(title, sizeof(title), "%s", base ? base + 1 : path);
			strip_extension(title);
		}
		pthread_mutex_lock(&db_lock);
		if (db) {
			insert_part(path, key, i, title);
		}
		pthread_mutex_unlock(&db_lock);
	}

	char above[512];
	if (own_folder) {
		parent_of(relative(dir), above, sizeof(above));
	} else {
		snprintf(above, sizeof(above), "%s", relative(dir));
	}
	fill_from_folders(b, above);
	if (b->series[0] && b->series_part < 0) {
		char stem[256] = "";
		if (parts->count > 0) {
			const char *base = strrchr(parts->items[0], '/');
			snprintf(stem, sizeof(stem), "%s", base ? base + 1 : parts->items[0]);
			strip_extension(stem);
		}
		b->series_part = series_number(own_folder ? folder_name : stem, true, b->series);
		if (b->series_part < 0) {
			b->series_part = series_number(b->title, true, b->series);
		}
	}

	pthread_mutex_lock(&db_lock);
	if (db) {
		insert_book(key, b);
	}
	pthread_mutex_unlock(&db_lock);

	free(tags);
	free(b);
}

// Adds the loose audio of folder `dir` (its `files`) to `parts`, as full
// paths, and says how many single-file books sit beside them.
static int collect(const char *dir, const namelist_t *files, namelist_t *parts, namelist_t *singles) {
	int books = 0;
	for (int i = 0; i < files->count && !scan_cancel; i++) {
		const char *name = files->items[i];
		if (!is_audio(name)) {
			continue;
		}
		char path[768];
		if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path)) {
			continue;
		}
		if (is_book_file(path, name)) {
			if (singles) {
				namelist_add(singles, path);
			}
			books++;
		} else {
			namelist_add(parts, path);
		}
	}
	return books;
}

static void scan_directory(const char *path, int depth) {
	if (scan_cancel || depth > SCAN_MAX_DEPTH) {
		return;
	}

	namelist_t files = {0};
	namelist_t dirs = {0};
	if (!read_folder(path, &files, &dirs)) {
		return;
	}

	namelist_t parts = {0};
	namelist_t singles = {0};
	collect(path, &files, &parts, &singles);

	// The disc folders belong to this folder's book: their files follow its
	// own, disc by disc. Everything else is walked on its own below.
	namelist_t walk = {0};
	for (int i = 0; i < dirs.count && !scan_cancel; i++) {
		char child[768];
		if (snprintf(child, sizeof(child), "%s/%s", path, dirs.items[i]) >= (int)sizeof(child)) {
			continue;
		}
		if (is_disc_folder(dirs.items[i])) {
			namelist_t disc_files = {0};
			namelist_t disc_dirs = {0};
			if (read_folder(child, &disc_files, &disc_dirs)) {
				collect(child, &disc_files, &parts, NULL);
			}
			namelist_free(&disc_files);
			namelist_free(&disc_dirs);
		} else {
			namelist_add(&walk, child);
		}
	}
	namelist_free(&files);
	namelist_free(&dirs);

	bool root = depth == 0;

	// The loose files, by book. In the Audiobooks folder itself each is a book
	// of its own; anywhere else they are grouped by their album tag, the
	// book's title in files split by chapter, and the files with none form one
	// group together.
	namelist_t albums = {0};
	int *group_of = calloc((size_t)(parts.count ? parts.count : 1), sizeof(int));
	int groups = 0;
	if (group_of) {
		song_metadata_t *tags = root ? NULL : calloc(1, sizeof(*tags));
		for (int i = 0; i < parts.count && !scan_cancel; i++) {
			if (!tags) {
				group_of[i] = groups++;
				continue;
			}
			memset(tags, 0, sizeof(*tags));
			metadata_read(parts.items[i], tags);
			int found = -1;
			for (int g = 0; g < albums.count; g++) {
				if (strcasecmp(albums.items[g], tags->album) == 0) {
					found = g;
					break;
				}
			}
			if (found < 0 && namelist_add(&albums, tags->album)) {
				found = albums.count - 1;
				groups++;
			}
			group_of[i] = found < 0 ? 0 : found;
		}
		free(tags);
	}

	// How many books the folder holds, counting a group of one file as a book
	// of its own.
	int books = singles.count;
	for (int g = 0; g < groups; g++) {
		int members = 0;
		for (int i = 0; i < parts.count; i++) {
			members += group_of && group_of[i] == g;
		}
		books += members >= 2 ? 1 : members;
	}

	const char *rel = relative(path);
	bool alone = !root && books == 1 && walk.count == 0;

	for (int i = 0; i < singles.count && !scan_cancel; i++) {
		const char *file = singles.items[i];
		const char *base = strrchr(file, '/');
		index_single(file, base ? base + 1 : file, rel, alone);
	}
	for (int g = 0; g < groups && group_of && !scan_cancel; g++) {
		namelist_t members = {0};
		for (int i = 0; i < parts.count; i++) {
			if (group_of[i] == g) {
				namelist_add(&members, parts.items[i]);
			}
		}
		if (members.count >= 2) {
			// One book per folder is indexed under the folder. Beside others,
			// each is indexed under the folder and its album: a path no file
			// has, since it only has to be unique.
			char key[768];
			if (groups == 1 || !albums.items[g][0]) {
				snprintf(key, sizeof(key), "%s", path);
			} else {
				snprintf(key, sizeof(key), "%s/\x1f%s", path, albums.items[g]);
			}
			index_folder(path, key, &members, books == 1);
		} else {
			for (int i = 0; i < members.count; i++) {
				const char *base = strrchr(members.items[i], '/');
				index_single(members.items[i], base ? base + 1 : members.items[i], rel, alone);
			}
		}
		namelist_free(&members);
	}
	free(group_of);
	namelist_free(&albums);
	namelist_free(&parts);
	namelist_free(&singles);

	for (int i = 0; i < walk.count && !scan_cancel; i++) {
		scan_directory(walk.items[i], depth + 1);
	}
	namelist_free(&walk);
}

static void *scan_thread_func(void *arg) {
	(void)arg;
	thread_be_background("audiobook scan");

	printf("audiobooks: scanning %s\n", scan_root);

	pthread_mutex_lock(&db_lock);
	// The rescan is wipe-and-refill, like the music library's, so books whose
	// files are gone simply never come back. What must survive the wipe is
	// where each book was left and when, so it is stashed in a temp table
	// first and each insert copies its own back.
	exec("CREATE TEMP TABLE IF NOT EXISTS old_state(path TEXT PRIMARY KEY, last_played INT, resume_file TEXT,"
		 " resume_pos REAL)");
	exec("DELETE FROM old_state");
	exec("INSERT INTO old_state SELECT path, last_played, resume_file, resume_pos FROM AUDIOBOOK_TABLE"
		 " WHERE last_played > 0 OR resume_file IS NOT NULL");
	exec("DELETE FROM AUDIOBOOK_TABLE");
	exec("DELETE FROM AUDIOBOOK_PARTS");
	// The row ids restart at one, so anything holding them is pointing at
	// other people's books.
	bump_generation();
	exec("BEGIN");
	pthread_mutex_unlock(&db_lock);

	scan_directory(scan_root, 0);

	pthread_mutex_lock(&db_lock);
	exec("COMMIT");
	if (db) {
		if (!scan_cancel) {
			char sql[48];
			snprintf(sql, sizeof(sql), "PRAGMA user_version=%d", SCHEMA_VERSION);
			exec(sql);
			outdated = false;
		}
		sqlite3_db_release_memory(db);
	}
	bump_generation();
	pthread_mutex_unlock(&db_lock);

	printf("audiobooks: scan finished, %d books%s\n", scan_found, scan_cancel ? " (stopped early)" : "");
	scan_running = false;
	return NULL;
}

bool audiobookdb_scan_start(const char *root) {
	if (!db || scan_running || !root || !root[0]) {
		return false;
	}

	snprintf(scan_root, sizeof(scan_root), "%s", root);
	scan_found = 0;
	scan_cancel = false;
	scan_running = true;

	if (pthread_create(&scan_thread, NULL, scan_thread_func, NULL) != 0) {
		scan_running = false;
		fprintf(stderr, "audiobooks: could not start the scan thread\n");
		return false;
	}

	pthread_detach(scan_thread);
	return true;
}

bool audiobookdb_scan_running(void) { return scan_running; }

int audiobookdb_scan_found(void) { return scan_found; }

void audiobookdb_scan_stop(void) {
	if (scan_running) {
		scan_cancel = true;
	}
}
