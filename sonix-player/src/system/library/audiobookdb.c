#include "audiobookdb.h"
#include "src/system/library/id3chap.h"
#include "src/system/playback/playlist.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/db/sqlite3.h"
#include "src/system/library/library.h" // library_collate_listorder: the shared ordering
#include "src/system/core/utils.h"

#define SCAN_MAX_DEPTH 12
#define SCAN_COMMIT_EVERY 50

static sqlite3 *db;
static pthread_mutex_t db_lock = PTHREAD_MUTEX_INITIALIZER;

// Bumped whenever the row ids of AUDIOBOOK_TABLE may have moved: a scan empties
// and refills it, a different card is a different set of books, and marking one
// finished changes which rows the finished list names. A handle built before
// the bump is reading somebody else's rows, so it is made to say so instead.
static unsigned generation = 1;

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
// every open after the first, which is not news worth printing at each boot.
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

	// Same journal choices as the music library, for the same reasons: a slow
	// card that may be pulled at any moment.
	exec("PRAGMA synchronous=NORMAL");
	exec("PRAGMA journal_mode=TRUNCATE");
	exec("PRAGMA cache_size=-128");

	exec("CREATE TABLE IF NOT EXISTS AUDIOBOOK_TABLE("
		 "path TEXT PRIMARY KEY, name TEXT COLLATE NOCASE, size INT, mtime INT,"
		 "last_played INT DEFAULT 0, resume_file TEXT, resume_pos REAL)");

	// Where the listener got to. The two columns are in the CREATE above for a
	// card that has never been scanned, and added by hand here for a database
	// written before they existed -- ALTER TABLE is the only way to reach that
	// one, and it fails harmlessly once they are there. It must run after the
	// CREATE, or on a fresh card there is no table to alter.
	exec_quiet("ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN resume_file TEXT");
	exec_quiet("ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN resume_pos REAL");

	// The books heard to the end. Deliberately not a column: a rescan wipes
	// AUDIOBOOK_TABLE and refills it, and having read a book is not something
	// a rescan may forget.
	exec("CREATE TABLE IF NOT EXISTS AUDIOBOOK_FINISHED("
		 "path TEXT PRIMARY KEY, finished_at INT DEFAULT 0)");

	// A different card is a different set of books, and the row ids that named
	// the last one's are now this one's.
	pthread_mutex_lock(&db_lock);
	bump_generation();
	pthread_mutex_unlock(&db_lock);

	printf("audiobooks: %s open, %d books indexed\n", db_path, audiobookdb_count());
	return true;
}

void audiobookdb_close(void) {
	// Waited on, not just asked: a scan still running holds the database open,
	// sqlite3_close() then answers SQLITE_BUSY without closing the file, and
	// an open file on the card is what stops the card being unmounted -- and
	// with it, the next card being mounted in its place.
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
	pthread_mutex_unlock(&db_lock);
}

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

// The ordered query behind a list. `select` is what to ask for, so the same
// builder serves the streaming reader and the row-id pass the handle makes.
static const char *list_sql(audiobook_list_t kind, bool ids) {
	switch (kind) {
	case AUDIOBOOK_LIST_RECENT:
		return ids ? "SELECT rowid FROM AUDIOBOOK_TABLE WHERE last_played > 0 ORDER BY last_played DESC LIMIT 10"
				   : "SELECT name, path FROM AUDIOBOOK_TABLE WHERE last_played > 0"
					 " ORDER BY last_played DESC LIMIT 10";
	case AUDIOBOOK_LIST_FINISHED:
		// The join is what keeps a book that has left the card out of the
		// list without ever deleting the fact that it was finished: put the
		// file back and it is there again.
		return ids ? "SELECT t.rowid FROM AUDIOBOOK_TABLE t JOIN AUDIOBOOK_FINISHED f ON f.path = t.path"
					 " ORDER BY f.finished_at DESC"
				   : "SELECT t.name, t.path FROM AUDIOBOOK_TABLE t"
					 " JOIN AUDIOBOOK_FINISHED f ON f.path = t.path"
					 " ORDER BY f.finished_at DESC";
	default:
		return ids ? "SELECT rowid FROM AUDIOBOOK_TABLE ORDER BY name COLLATE listorder"
				   : "SELECT name, path FROM AUDIOBOOK_TABLE ORDER BY name COLLATE listorder";
	}
}

int audiobookdb_for_each(audiobook_list_t kind, audiobook_row_cb cb, void *user) {
	if (!cb) {
		return 0;
	}

	const char *sql = list_sql(kind, false);

	pthread_mutex_lock(&db_lock);

	int count = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *name = (const char *)sqlite3_column_text(stmt, 0);
			const char *path = (const char *)sqlite3_column_text(stmt, 1);
			if (!cb(name ? name : "", path ? path : "", user)) {
				break;
			}
			count++;
		}
		sqlite3_finalize(stmt);
	}

	pthread_mutex_unlock(&db_lock);
	return count;
}

// ---------------------------------------------------------------------------
// List handles
//
// See audiobookdb.h. The one ordered pass happens at open and its result is
// four bytes a row; the rows themselves are read back a windowful at a time,
// by row id, which needs none of the ordering or filtering the pass did.
// ---------------------------------------------------------------------------

struct audiobookdb_index {
	int32_t *rows;
	int count;
	unsigned generation;
};

audiobookdb_index_t *audiobookdb_index_open(audiobook_list_t kind) {
	struct audiobookdb_index *ix = calloc(1, sizeof(*ix));
	if (!ix) {
		return NULL;
	}

	pthread_mutex_lock(&db_lock);
	ix->generation = generation;

	int capacity = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, list_sql(kind, true), -1, &stmt, NULL) == SQLITE_OK) {
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
	if (db && sqlite3_prepare_v2(db, "SELECT name, path FROM AUDIOBOOK_TABLE WHERE rowid=?", -1, &stmt, NULL) ==
				  SQLITE_OK) {
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
// Where the listener got to
// ---------------------------------------------------------------------------
//
// A book is not a song: coming back to it means coming back to the second it
// was left at, in the chapter it was left in. Both halves are remembered, and
// both are needed -- the chapter alone would restart a forty-minute file from
// the top.

void audiobookdb_save_position(const char *book_path, const char *file, double seconds) {
	if (!book_path || !book_path[0] || !file || !file[0]) {
		return;
	}

	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db,
								 "UPDATE AUDIOBOOK_TABLE SET resume_file=?, resume_pos=?, last_played=? WHERE path=?",
								 -1, &stmt, NULL) == SQLITE_OK) {
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
	if (db && sqlite3_prepare_v2(db, "SELECT resume_file, resume_pos FROM AUDIOBOOK_TABLE WHERE path=?", -1, &stmt,
								 NULL) == SQLITE_OK) {
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

// Which book a playing file is, if it is one at all.
//
// Asked of the path rather than of a flag set when playback started: a flag has
// to be maintained through every way a track can change -- the queue, the next
// button, a restore at boot, a track tapped in the file browser -- and the
// first one missed leaves the player wearing audiobook controls over a song.
//
// A book here is one .m4b file, not a folder of them: that is what the scan
// indexes and what the chapters live inside. So the answer is simply whether
// this exact path is in the table -- one query per track change, with nothing
// cached to go stale.
bool audiobookdb_book_for_file(const char *file_path, char *book_out, size_t book_size) {
	if (book_out && book_size) {
		book_out[0] = '\0';
	}
	if (!db || !file_path || !file_path[0]) {
		return false;
	}

	pthread_mutex_lock(&db_lock);

	bool hit = false;
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT path FROM AUDIOBOOK_TABLE WHERE path=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, file_path, -1, SQLITE_TRANSIENT);
		hit = sqlite3_step(stmt) == SQLITE_ROW;
		sqlite3_finalize(stmt);
	}

	pthread_mutex_unlock(&db_lock);

	if (hit && book_out && book_size) {
		snprintf(book_out, book_size, "%s", file_path);
	}
	return hit;
}

void audiobookdb_mark_finished(const char *path) {
	if (!path || !path[0]) {
		return;
	}

	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO AUDIOBOOK_FINISHED(path, finished_at) VALUES(?,?)", -1,
								 &stmt, NULL) == SQLITE_OK) {
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
	if (db &&
		sqlite3_prepare_v2(db, "UPDATE AUDIOBOOK_TABLE SET last_played=? WHERE path=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, (sqlite3_int64)time(NULL));
		sqlite3_bind_text(stmt, 2, path, -1, SQLITE_TRANSIENT);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	// A book has just joined Recent, which a handle over that list has to be
	// told. Deliberately not done in audiobookdb_save_position(), which writes
	// the same column every ten seconds for as long as a book is playing: a
	// handle scrapped that often would take the page's decoded jackets with it,
	// and the only thing moving is the book already at the top.
	bump_generation();
	pthread_mutex_unlock(&db_lock);
}

// ---------------------------------------------------------------------------
// scanning
//
// Only the Audiobooks folder at the root of the card, and only two kinds of
// file: .m4b, and .mp3 that actually carries chapter marks.
//
// The folder rather than the whole card because a scan of the whole card finds
// every podcast episode, every long mix and every lecture recording and calls
// them books -- and because on a full card it takes minutes to do it. One named
// folder is a rule a reader can hold in their head, which is why it is written
// under the button that starts the scan.
//
// An .mp3 is asked whether it has chapters rather than taken on its extension:
// the folder is where someone puts books, but a book split into one file per
// chapter is a folder of plain MP3s, and those are not one book each. Reading
// the ID3 header of each candidate costs one open and a few kilobytes.
// ---------------------------------------------------------------------------

static void insert_book(const char *path, const char *filename, const struct stat *st) {
	char name[256];
	snprintf(name, sizeof(name), "%s", filename);
	char *dot = strrchr(name, '.');
	if (dot) {
		*dot = '\0';
	}

	sqlite3_stmt *stmt = NULL;
	// last_played survives a rescan: the scan stashes the old values in a temp
	// table before wiping, and COALESCE picks them back up here.
	if (sqlite3_prepare_v2(db,
						   "INSERT OR REPLACE INTO AUDIOBOOK_TABLE(path,name,size,mtime,last_played)"
						   " VALUES(?,?,?,?,COALESCE((SELECT last_played FROM old_played WHERE path=?),0))",
						   -1, &stmt, NULL) != SQLITE_OK) {
		return;
	}
	sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, (sqlite3_int64)st->st_size);
	sqlite3_bind_int64(stmt, 4, (sqlite3_int64)st->st_mtime);
	sqlite3_bind_text(stmt, 5, path, -1, SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

static void scan_directory(const char *path, int depth) {
	if (scan_cancel || depth > SCAN_MAX_DEPTH) {
		return;
	}

	DIR *dir = opendir(path);
	if (!dir) {
		return;
	}

	struct dirent *de;
	while (!scan_cancel && (de = readdir(dir)) != NULL) {
		// Dot files, and the folders a desktop leaves behind without one.
		if (de->d_name[0] == '.' || playlist_is_junk_name(de->d_name)) {
			continue;
		}

		char child[512];
		if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >= (int)sizeof(child)) {
			continue;
		}

		struct stat st;
		if (stat(child, &st) != 0) {
			continue;
		}

		if (S_ISDIR(st.st_mode)) {
			scan_directory(child, depth + 1);
			continue;
		}

		if (!S_ISREG(st.st_mode)) {
			continue;
		}
		if (!has_extension(de->d_name, ".m4b") && !(has_extension(de->d_name, ".mp3") && id3chap_present(child))) {
			continue;
		}

		pthread_mutex_lock(&db_lock);
		if (db) {
			insert_book(child, de->d_name, &st);
			scan_found++;
			if (scan_found % SCAN_COMMIT_EVERY == 0) {
				exec("COMMIT");
				sqlite3_db_release_memory(db);
				exec("BEGIN");
			}
		}
		pthread_mutex_unlock(&db_lock);
	}

	closedir(dir);
}

static void *scan_thread_func(void *arg) {
	(void)arg;
	thread_be_background("audiobook scan");

	printf("audiobooks: scanning %s\n", scan_root);

	pthread_mutex_lock(&db_lock);
	// The rescan is wipe-and-refill, like the music library's, so books whose
	// files are gone simply never come back. What must survive the wipe is
	// last_played -- the "Recenti" view is built on it -- so the old values
	// are stashed in a temp table first and each insert copies its own back.
	exec("CREATE TEMP TABLE IF NOT EXISTS old_played(path TEXT PRIMARY KEY, last_played INT)");
	exec("DELETE FROM old_played");
	exec("INSERT INTO old_played SELECT path, last_played FROM AUDIOBOOK_TABLE WHERE last_played > 0");
	exec("DELETE FROM AUDIOBOOK_TABLE");
	// The row ids restart at one, so anything holding them is pointing at
	// other people's books.
	bump_generation();
	exec("BEGIN");
	pthread_mutex_unlock(&db_lock);

	scan_directory(scan_root, 0);

	pthread_mutex_lock(&db_lock);
	exec("COMMIT");
	if (db) {
		sqlite3_db_release_memory(db);
	}
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
