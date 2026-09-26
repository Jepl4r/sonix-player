#include "library.h"
#include "src/system/playback/playlist.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/db/sqlite3.h"
#include "src/system/library/cue.h"
#include "src/system/decode/mp4.h"
#include "src/system/decode/apedec.h"
#include "src/system/library/metadata.h"
#include "src/system/core/utils.h"

// The schema.
//
// Close to the stock player's, with the `sortkey` columns added: the ordering
// the stock schema implies cannot be indexed, so without them every list sorts
// the whole table on every open. See library_sort_key(). The two databases are
// separate -- this index lives under .local, the stock one under .temp -- so
// neither player reads the other's.
//
// Only the tables a scan actually fills are created here; the rest of the
// firmware's tables (history, collections, bookmarks) are its own business.

// The `format` column's codes. The first five are the stock player's; the rest
// are local additions, and extending the set breaks nothing because the column
// is read only in here.
#define FORMAT_WAV 0
#define FORMAT_MP3 1
#define FORMAT_FLAC 2
#define FORMAT_OGG 3
#define FORMAT_DSD 4
#define FORMAT_AAC 5
#define FORMAT_OPUS 6
#define FORMAT_WAVPACK 7
#define FORMAT_ALAC 8
#define FORMAT_APE 9

static const char *const SCHEMA[] = {
	"CREATE TABLE IF NOT EXISTS MEDIA_TABLE(id INT,path TEXT COLLATE NOCASE,name TEXT COLLATE NOCASE,"
	"album TEXT COLLATE NOCASE,artist TEXT COLLATE NOCASE,genre TEXT COLLATE NOCASE,year INT,dis_id INT,ck_id INT,"
	"has_child_file INT,begin_time INT,end_time INT,cue_id INT,character TEXT COLLATE NOCASE,size INT,"
	"sample_rate INT,bit_rate INT,bit INT,channel INT,format INT,quality TEXT COLLATE NOCASE,"
	"album_pic_path TEXT COLLATE NOCASE,lrc_path TEXT COLLATE NOCASE,track_gain REAL,track_peak REAL,"
	"album_artist TEXT COLLATE NOCASE,ctime INT,mtime INT,sortkey TEXT,disc INT,PRIMARY KEY(id, path, cue_id))",

	"CREATE TABLE IF NOT EXISTS ALBUM_TABLE(id INT, album TEXT COLLATE NOCASE,character TEXT COLLATE NOCASE,"
	" cn INT, sortkey TEXT, PRIMARY KEY(album))",

	"CREATE TABLE IF NOT EXISTS ARTIST_TABLE(id INT, artist TEXT COLLATE NOCASE,character TEXT COLLATE NOCASE,"
	" cn INT, sortkey TEXT, PRIMARY KEY(artist))",

	"CREATE TABLE IF NOT EXISTS ALBUM_ARTIST_TABLE(id INT, album_artist TEXT COLLATE NOCASE,"
	// pinyin_charater is the stock schema's, misspelling and all: nothing here
	// reads it and the scan writes an empty string into it. Kept because
	// dropping a column means rebuilding the table on every card that already
	// has one, for nothing.
	"character TEXT COLLATE NOCASE, cn INT, ctime INT, mtime INT,mqa INT,pinyin_charater TEXT,sortkey TEXT,"
	"PRIMARY KEY(album_artist))",

	"CREATE TABLE IF NOT EXISTS GENRE_TABLE( id INT,genre TEXT COLLATE NOCASE,character TEXT COLLATE NOCASE,"
	" cn INT, sortkey TEXT, PRIMARY KEY(genre))",

	// Not in the stock schema: the starred tracks, in order of starring.
	// Deliberately independent of MEDIA_TABLE so favourites survive rescans
	// and can point at files the index has never seen.
	"CREATE TABLE IF NOT EXISTS FAVOURITES(path TEXT PRIMARY KEY, name TEXT, artist TEXT, added_at INT)",

	// Also local: where playback was left (the "Ricorda traccia" option). One
	// row only; id is always 0.
	"CREATE TABLE IF NOT EXISTS PLAYBACK_STATE(id INT PRIMARY KEY, path TEXT, position REAL, updated_at INT)",

	// Also local: the playback queue as it will play, so a reboot comes back
	// with next and prev still meaning something. One row per entry, plus a
	// single row of state (the current position, whether it is a library list).
	// `slot` is where the row sits in the playing order. It matters only for the
	// query form below, where these rows are what "add to queue" put beside a
	// library list: without it the restore has nowhere to put them but after the
	// current track. -1, or the column missing on an older card, means no slot
	// was recorded.
	"CREATE TABLE IF NOT EXISTS PLAYBACK_QUEUE(idx INT PRIMARY KEY, path TEXT, slot INT)",
	"CREATE TABLE IF NOT EXISTS PLAYBACK_QUEUE_STATE(id INT PRIMARY KEY, pos INT, custom INT, updated_at INT)",

	// A queue that is a library list is written down as the question, not the
	// answer: which list, ordered how, and where playback had got to. Two
	// hundred thousand rows of PLAYBACK_QUEUE for a queue that one query
	// rebuilds is minutes of card writes on every track change, and it is the
	// same list either way -- unless the library was rescanned in between, in
	// which case re-running the query is the more correct of the two.
	"CREATE TABLE IF NOT EXISTS PLAYBACK_QUEUE_QUERY(id INT PRIMARY KEY, kind INT, filter INT, ord INT, desc INT,"
	" value TEXT, pos INT, updated_at INT)",

	// Which record the queue IS, when it is one. Neither of the two forms above
	// carries it: the rows are paths and the query is a filter, and a queue
	// built from Cover Flow or the Albums page is a record in a way that neither
	// spells out. "Play albums back to back" reads it after a reboot. Its own
	// table because a new table needs no migration on a card that already holds
	// the others.
	"CREATE TABLE IF NOT EXISTS PLAYBACK_QUEUE_ALBUM(id INT PRIMARY KEY, album TEXT)",

	// Local: how many tracks each playlist holds, so the playlists page does not
	// have to walk every entry against the card to draw a subtitle. Keyed by
	// the file's own path, with the modification time and size it had when the
	// number was counted -- edit the playlist and the row stops matching.
	"CREATE TABLE IF NOT EXISTS PLAYLIST_COUNTS(path TEXT PRIMARY KEY, mtime INT, size INT, tracks INT, mount INT)",

	// Local: which mount of this card the counts above were checked against.
	// One row, bumped every time the card is mounted. See next_mount_serial().
	"CREATE TABLE IF NOT EXISTS MOUNT_SERIAL(id INT PRIMARY KEY, serial INT)",

	// Local: whether the card's .m3u playlist files have already been taken into
	// tables of their own. One row, set once per card.
	"CREATE TABLE IF NOT EXISTS PLAYLIST_MIGRATION(id INT PRIMARY KEY, done INT)",

	"CREATE TABLE IF NOT EXISTS VERSION_TABLE( version_id INT,PRIMARY KEY(version_id))",
	"CREATE TABLE IF NOT EXISTS COUNT_TABLE( cn INT)",
};

// Added to a database that predates them. CREATE TABLE IF NOT EXISTS leaves an
// existing table alone, columns and all, so a new column has to be asked for
// separately; "duplicate column name" is the ordinary answer here and means the
// column is already there.
static const char *const SCHEMA_COLUMNS[] = {
	"ALTER TABLE MEDIA_TABLE ADD COLUMN sortkey TEXT",
	// Unlike the sort keys, this one cannot be worked out from anything already
	// in the table: it is in the file's tags and nowhere else. An index written
	// before the column existed therefore carries NULL until the card is
	// scanned again. See TRACK_ORDER_IN_ALBUM.
	"ALTER TABLE MEDIA_TABLE ADD COLUMN disc INT",
	"ALTER TABLE ALBUM_TABLE ADD COLUMN sortkey TEXT",
	"ALTER TABLE ARTIST_TABLE ADD COLUMN sortkey TEXT",
	"ALTER TABLE ALBUM_ARTIST_TABLE ADD COLUMN sortkey TEXT",
	"ALTER TABLE GENRE_TABLE ADD COLUMN sortkey TEXT",
	"ALTER TABLE PLAYLIST_COUNTS ADD COLUMN mount INT",
	"ALTER TABLE PLAYBACK_QUEUE ADD COLUMN slot INT",
};

// After the columns exist, never before: an index on a column the table does
// not have yet is an error, and on an old database that is exactly the order
// things happen in.
static const char *const SCHEMA_INDEXES[] = {
	// By path. The primary key is (id, path, cue_id), and SQLite can only use a
	// composite index from its leftmost column, so without this "WHERE path=?"
	// is a full table scan. One such scan goes unnoticed; a playlist asking for
	// a name per entry is one per row.
	"CREATE INDEX IF NOT EXISTS media_path_idx ON MEDIA_TABLE(path)",
	"CREATE INDEX IF NOT EXISTS media_album_idx ON MEDIA_TABLE(album)",
	"CREATE INDEX IF NOT EXISTS media_artist_idx ON MEDIA_TABLE(artist)",
	"CREATE INDEX IF NOT EXISTS media_album_artist_idx ON MEDIA_TABLE(album_artist)",
};

// Built only once every row has a key: an index on a column that is still half
// empty is a b-tree updated a row at a time for nothing. On a database that
// already has its keys these run at open; on one that does not, they run when
// the backfill has been through it, as one bulk build instead of two hundred
// thousand insertions.
static const char *const SORT_INDEXES[] = {
	// By sort key: what makes a list open without sorting anything.
	//
	// The row id rides in every index entry, so "SELECT rowid, sortkey ...
	// ORDER BY sortkey" is answered from the index alone -- a sequential walk,
	// no table lookups, no temporary b-tree, nothing spilled to the card. The
	// A-Z strip is counted from the key rather than from the name, which is why
	// the name does not have to be in here (see letter_from_key).
	"CREATE INDEX IF NOT EXISTS media_sort_idx ON MEDIA_TABLE(sortkey)",

	// The same for one genre's tracks, which on a real library is a large
	// slice of it. Albums and artists keep their plain single-column index:
	// those lists are tens or hundreds of rows, and sorting that many costs
	// less than the wider index would cost the scan.
	"CREATE INDEX IF NOT EXISTS media_genre_sort_idx ON MEDIA_TABLE(genre, sortkey)",
	// Which makes the plain one redundant: a two-column index serves a lookup
	// on its first column just as well, and a scan pays for every index it has
	// to keep.
	"DROP INDEX IF EXISTS media_genre_idx",

	// The lookup tables carry their name along in the index as well. Their
	// query leaves out the rows with no name at all ("WHERE album <> ''"), and
	// a filter on a column the index does not hold means reading the row to
	// answer it -- which is the table lookup per row that the index was there
	// to avoid. They hold thousands of rows, not hundreds of thousands, so the
	// extra width costs nothing worth counting.
	"CREATE INDEX IF NOT EXISTS album_sort_idx ON ALBUM_TABLE(sortkey, album)",
	"CREATE INDEX IF NOT EXISTS artist_sort_idx ON ARTIST_TABLE(sortkey, artist)",
	"CREATE INDEX IF NOT EXISTS album_artist_sort_idx ON ALBUM_ARTIST_TABLE(sortkey, album_artist)",
	"CREATE INDEX IF NOT EXISTS genre_sort_idx ON GENRE_TABLE(sortkey, genre)",
};

// A record's running order: the disc, then the track number inside it, so a
// two-disc album gives 1-1, 1-2, ... then 2-1, 2-2, and not the two track
// ones together.
//
// COALESCE because `disc` is NULL on every row of an index written before the
// column existed, and in SQLite NULL sorts ahead of every number: a table
// where some rows have been scanned since and some have not would otherwise
// deal each record out in two. NULL means disc one here, which is the order
// those rows had anyway.
#define TRACK_ORDER_IN_ALBUM "COALESCE(disc,1), dis_id"

// Rows are committed in small batches. This is not a tuning knob: an open
// transaction holds its dirty pages in memory, and on a device with ten
// megabytes free a scan that only commits at the end is a scan that gets the
// player killed. Fifty rows is frequent enough that the index on disk is never
// far behind what the counter says, and each commit is followed by handing the
// page cache back.
#define SCAN_COMMIT_EVERY 50

// Deepest directory nesting the walk will follow. Guards against a symlink
// loop turning into an infinite scan.
#define SCAN_MAX_DEPTH 12

// How many subdirectories of one level are held aside while it is walked. Not a
// limit on the library -- every file in that directory is still read -- but the
// memory ceiling of the walk. A directory with more than four thousand
// subdirectories is an accident, not a discography.
#define SCAN_MAX_SUBDIRS 4096

// How often the scan logs a line with the free memory. If it ever dies partway
// through, the log says where it got to and how much RAM was left.
#define SCAN_LOG_EVERY 200

static sqlite3 *db;
static pthread_mutex_t db_lock = PTHREAD_MUTEX_INITIALIZER;

// Bumped whenever a row's identity can have changed underneath a list that is
// already open.
//
// The lists hold SQLite row ids rather than copies of the rows, and a row id is
// only a promise for as long as nobody rewrites the table. It is not: INSERT OR
// REPLACE is a delete and an insert, so a re-scanned track comes back with a
// different one; a scan begins by emptying the tables, and with no AUTOINCREMENT
// the numbering starts again at one. A list built before that and read after it
// would show the wrong tracks rather than none, which is the kind of wrong
// nobody notices.
//
// One counter per group of tables that move together, not one for everything:
// favourites are a table of their own precisely so that starring a track cannot
// touch the index (see the FAVOURITES schema), and a single counter would let
// the star button invalidate the handle the playback queue was built on.
typedef enum {
	GEN_MEDIA = 0, // MEDIA_TABLE and the four lookup tables the scan fills
	GEN_FAVOURITES,
	GEN_PLAYLISTS, // the M3U_ tables: their own, so editing one does not
				   // invalidate a list of tracks and vice versa
	GEN_COUNT,
} gen_domain_t;

static unsigned generation[GEN_COUNT] = {1, 1, 1};

static gen_domain_t domain_of(library_list_t kind) {
	switch (kind) {
	case LIBRARY_LIST_FAVOURITES:
		return GEN_FAVOURITES;
	case LIBRARY_LIST_PLAYLIST:
		return GEN_PLAYLISTS;
	default:
		return GEN_MEDIA;
	}
}

// Call with db_lock held, or from somewhere no query can be in flight.
static void bump_generation(gen_domain_t domain) { generation[domain]++; }

static void bump_all_generations(void) {
	for (int i = 0; i < GEN_COUNT; i++) {
		generation[i]++;
	}
}

unsigned library_revision(library_list_t kind) {
	pthread_mutex_lock(&db_lock);
	unsigned value = generation[domain_of(kind)];
	pthread_mutex_unlock(&db_lock);
	return value;
}

static pthread_t scan_thread;
static volatile bool scan_running;
static volatile bool scan_cancel;
// Set when the database itself stops answering -- a failed prepare, a
// transaction that will not commit. The walk stops on scan_cancel, and this
// says the stop was a fault rather than the user pressing back.
static volatile bool scan_db_failed;
static volatile int scan_found;
static char scan_root[512];
static char scan_folder[512];
static pthread_mutex_t scan_folder_lock = PTHREAD_MUTEX_INITIALIZER;

// Whether every file is named in the log as it is read. Developer options has
// the switch; see library_set_log_database.
static bool scan_log_files;

// ---------------------------------------------------------------------------
// collation
// ---------------------------------------------------------------------------

// Groups, in the order they sort. A string is placed by its first character,
// so "…" comes before "9 Crimes" comes before "Zappa" comes before a title in
// kana, which comes before one in Chinese.
typedef enum {
	SORT_GROUP_EMPTY = 0,
	SORT_GROUP_SYMBOL,
	SORT_GROUP_DIGIT,
	SORT_GROUP_LATIN,
	SORT_GROUP_GREEK,
	SORT_GROUP_CYRILLIC,
	SORT_GROUP_KANA,	// Japanese hiragana/katakana
	SORT_GROUP_CJK,		// Chinese (and Japanese kanji, which share the block)
	SORT_GROUP_HANGUL,	// Korean
	SORT_GROUP_OTHER,
} sort_group_t;

// Decodes one UTF-8 character. Advances *pos past it. Invalid bytes are
// returned as themselves so a mis-encoded tag still sorts somewhere sensible
// instead of stopping the comparison.
static uint32_t utf8_next(const char *text, int length, int *pos) {
	if (*pos >= length) {
		return 0;
	}

	unsigned char first = (unsigned char)text[*pos];
	int extra;
	uint32_t code;

	if (first < 0x80) {
		(*pos)++;
		return first;
	} else if ((first & 0xE0) == 0xC0) {
		extra = 1;
		code = first & 0x1F;
	} else if ((first & 0xF0) == 0xE0) {
		extra = 2;
		code = first & 0x0F;
	} else if ((first & 0xF8) == 0xF0) {
		extra = 3;
		code = first & 0x07;
	} else {
		(*pos)++;
		return first;
	}

	if (*pos + extra >= length) {
		(*pos)++;
		return first;
	}

	for (int i = 1; i <= extra; i++) {
		unsigned char continuation = (unsigned char)text[*pos + i];
		if ((continuation & 0xC0) != 0x80) {
			(*pos)++;
			return first; // truncated sequence
		}
		code = (code << 6) | (continuation & 0x3F);
	}

	*pos += extra + 1;
	return code;
}

// The other direction: one code point into `out`, which needs four bytes.
// Returns how many were written.
//
// Encoded by hand rather than through a library: what has to hold is that
// these bytes compare the way the code points do, and that is a property of
// this encoder, not of somebody else's. The sort key leans on it (see
// library_sort_key), and utf8_next never returns more than four bytes' worth,
// so the last branch is the top of the range and nothing is lost.
static int utf8_encode(uint32_t code, char *out) {
	if (code < 0x80) {
		out[0] = (char)code;
		return 1;
	}
	if (code < 0x800) {
		out[0] = (char)(0xC0 | (code >> 6));
		out[1] = (char)(0x80 | (code & 0x3F));
		return 2;
	}
	if (code < 0x10000) {
		out[0] = (char)(0xE0 | (code >> 12));
		out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
		out[2] = (char)(0x80 | (code & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | ((code >> 18) & 0x07));
	out[1] = (char)(0x80 | ((code >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((code >> 6) & 0x3F));
	out[3] = (char)(0x80 | (code & 0x3F));
	return 4;
}

static sort_group_t sort_group_of(uint32_t code) {
	if (code == 0) {
		return SORT_GROUP_EMPTY;
	}
	if (code >= '0' && code <= '9') {
		return SORT_GROUP_DIGIT;
	}
	if ((code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z')) {
		return SORT_GROUP_LATIN;
	}
	if (code < 0x80) {
		return SORT_GROUP_SYMBOL; // the rest of ASCII: punctuation and space
	}
	if (code >= 0xC0 && code <= 0x24F) {
		return SORT_GROUP_LATIN; // accented Latin, Latin Extended-A/B
	}
	if (code >= 0x370 && code <= 0x3FF) {
		return SORT_GROUP_GREEK;
	}
	if (code >= 0x400 && code <= 0x4FF) {
		return SORT_GROUP_CYRILLIC;
	}
	if ((code >= 0x3040 && code <= 0x30FF) || (code >= 0x31F0 && code <= 0x31FF) ||
		(code >= 0xFF66 && code <= 0xFF9D)) {
		return SORT_GROUP_KANA;
	}
	if ((code >= 0x3400 && code <= 0x4DBF) || (code >= 0x4E00 && code <= 0x9FFF) ||
		(code >= 0xF900 && code <= 0xFAFF)) {
		return SORT_GROUP_CJK;
	}
	if ((code >= 0x1100 && code <= 0x11FF) || (code >= 0xAC00 && code <= 0xD7AF)) {
		return SORT_GROUP_HANGUL;
	}
	if (code >= 0x2000 && code <= 0x2BFF) {
		return SORT_GROUP_SYMBOL; // punctuation, arrows, symbols
	}
	return SORT_GROUP_OTHER;
}

// Accented Latin folded to the letter underneath, so "Evolution" and
// "Evolution" with an acute land next to each other instead of the accented
// one being exiled past Z. Covers Latin-1 Supplement and Latin Extended-A,
// which is every accent a European tag realistically carries.
static uint32_t fold_latin_accent(uint32_t code) {
	static const char LATIN1[] = "aaaaaaaceeeeiiii" // C0-CF
								 "dnooooo\0ouuuuyps"  // D0-DF
								 "aaaaaaaceeeeiiii" // E0-EF
								 "dnooooo\0ouuuuypy"; // F0-FF

	if (code >= 0xC0 && code <= 0xFF) {
		char folded = LATIN1[code - 0xC0];
		return folded ? (uint32_t)folded : code; // D7/F7 are the maths signs
	}

	// Latin Extended-A, one entry per code point from 0x100 to 0x17F. Written
	// out run by run rather than counted by hand: the table this replaces was
	// one letter short at U+0134 and every entry after it was shifted, which
	// put S-caron among the Ts and L-stroke among the Ns -- in the sort and,
	// once there was one, on the A-Z strip.
	if (code >= 0x100 && code <= 0x17F) {
		static const char EXT_A[] = "aaaaaa"		// 100-105 A with macron, breve, ogonek
									"cccccccc"		// 106-10D
									"dddd"			// 10E-111
									"eeeeeeeeee"	// 112-11B
									"gggggggg"		// 11C-123
									"hhhh"			// 124-127
									"iiiiiiiiiiii"	// 128-133, the IJ ligature included
									"jj"			// 134-135
									"kkk"			// 136-138
									"llllllllll"	// 139-142, L with stroke included
									"nnnnnnnnn"		// 143-14B
									"oooooooo"		// 14C-153, the OE ligature included
									"rrrrrr"		// 154-159
									"ssssssss"		// 15A-161
									"tttttt"		// 162-167
									"uuuuuuuuuuuu"	// 168-173
									"ww"			// 174-175
									"yyy"			// 176-178
									"zzzzzz"		// 179-17E
									"s";			// 17F the long s
		size_t index = code - 0x100;
		if (index < sizeof(EXT_A) - 1) {
			return (uint32_t)EXT_A[index];
		}
	}

	return code;
}

// Case folding, far enough for the alphabets that have a case at all.
static uint32_t fold_case(uint32_t code) {
	if (code >= 'A' && code <= 'Z') {
		return code + 32;
	}
	if (code >= 0xC0 && code <= 0x17F) {
		return fold_latin_accent(code);
	}
	if (code >= 0x391 && code <= 0x3A9) {
		return code + 32; // Greek
	}
	if (code >= 0x410 && code <= 0x42F) {
		return code + 32; // Cyrillic
	}
	if (code == 0x401 || code == 0x451) {
		return 0x435; // Ё files under Е, the way an accent files under its letter
	}
	if (code >= 0x400 && code <= 0x40F) {
		return code + 80; // Cyrillic Ё and friends
	}
	return code;
}

// Whether a leading article is skipped when a name is filed and compared. On by
// default, which is what puts "The Beatles" under B.
//
// It reaches the lists through the sort key, which is written at scan time, so
// turning it off only changes what is on screen after the next scan -- and that
// is the honest behaviour to promise, because the alternative is a strip of
// letters that no longer matches the order of the rows beside it. The
// comparison itself follows the switch straight away, which is what the
// audiobook index (no stored key, see audiobookdb.c) needs.
static bool skip_articles = true;

void library_set_skip_articles(bool on) { skip_articles = on; }
bool library_skip_articles(void) { return skip_articles; }

// Skips a leading article so "The Beatles" files under B. Only English and
// Italian, which is what a tag on this device is most likely to be in.
static int skip_article(const char *text, int length) {
	if (!skip_articles) {
		return 0;
	}
	static const char *const ARTICLES[] = {"the ", "a ",  "an ", "il ",  "lo ", "la ", "i ",
										   "gli ", "le ", "l'",  "un ", "uno ", "una "};

	for (size_t i = 0; i < sizeof(ARTICLES) / sizeof(ARTICLES[0]); i++) {
		int article_length = (int)strlen(ARTICLES[i]);
		if (length > article_length && strncasecmp(text, ARTICLES[i], (size_t)article_length) == 0) {
			return article_length;
		}
	}
	return 0;
}

// The collation the schema names. Compares group first, then folded code
// points, then length -- so the result is a total order and SQLite is happy.
// Non-static: the audiobook database registers the very same ordering (see
// library.h), so "The Hobbit" files under H there just as "The Beatles" files
// under B here.
int library_collate_listorder(void *unused, int len_a, const void *a, int len_b, const void *b) {
	(void)unused;

	const char *text_a = a;
	const char *text_b = b;

	int pos_a = skip_article(text_a, len_a);
	int pos_b = skip_article(text_b, len_b);

	uint32_t first_a = 0, first_b = 0;
	int peek_a = pos_a, peek_b = pos_b;
	first_a = utf8_next(text_a, len_a, &peek_a);
	first_b = utf8_next(text_b, len_b, &peek_b);

	sort_group_t group_a = sort_group_of(first_a);
	sort_group_t group_b = sort_group_of(first_b);

	if (group_a != group_b) {
		return group_a < group_b ? -1 : 1;
	}

	for (;;) {
		uint32_t code_a = utf8_next(text_a, len_a, &pos_a);
		uint32_t code_b = utf8_next(text_b, len_b, &pos_b);

		if (code_a == 0 || code_b == 0) {
			if (code_a == code_b) {
				return 0;
			}
			return code_a == 0 ? -1 : 1;
		}

		code_a = fold_case(code_a);
		code_b = fold_case(code_b);

		if (code_a != code_b) {
			return code_a < code_b ? -1 : 1;
		}
	}
}

// The collation's order, written down as bytes.
//
// library_collate_listorder() above compares two things, in order: the script
// group of the first character, then the folded code points one at a time.
// Both go into a string whose plain byte order is that same order -- the group
// as one printable digit, then the folded code points re-encoded as UTF-8,
// whose byte order is code point order, which is exactly what the collation
// compares. Where one string runs out first, memcmp over the shorter length
// and then the lengths is the same answer the collation gives, and that is what
// SQLite's BINARY collation does.
//
// So "ORDER BY sortkey" answers what "ORDER BY name COLLATE listorder" answers,
// and an ordinary index can serve it. That is the whole point: the collation is
// registered at runtime, so no index can be built on it, and a list ordered by
// it has to sort the entire table into a temporary b-tree before drawing a row.
//
// The key holds no zero byte and is never empty, so it stores as TEXT.
void library_sort_key(const char *text, char *out, size_t out_size) {
	if (!out || out_size < 2) {
		return;
	}
	if (!text) {
		text = "";
	}

	int length = (int)strlen(text);
	int pos = skip_article(text, length);

	// The group of the first character, unfolded -- the collation looks at it
	// before it folds anything. The enum's numbering is the order the groups
	// sort in, and there are ten of them, so one digit says it and compares the
	// right way round.
	int peek = pos;
	uint32_t first = utf8_next(text, length, &peek);
	size_t at = 0;
	out[at++] = (char)('0' + (int)sort_group_of(first));

	for (;;) {
		uint32_t code = utf8_next(text, length, &pos);
		if (code == 0) {
			break;
		}
		code = fold_case(code);

		char encoded[4];
		int bytes = utf8_encode(code, encoded);

		if (at + (size_t)bytes >= out_size) {
			// Two names sharing five hundred bytes of prefix get the same key
			// and land next to each other in an order nothing else decides.
			break;
		}
		memcpy(out + at, encoded, (size_t)bytes);
		at += (size_t)bytes;
	}

	out[at] = '\0';
}

// sortkey(x) inside SQL, so a database written before the column existed can be
// filled in with one statement per batch instead of a round trip per row.
static void sortkey_sql(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
	const char *text = argc > 0 ? (const char *)sqlite3_value_text(argv[0]) : NULL;
	char key[LIBRARY_SORT_KEY_MAX];
	library_sort_key(text, key, sizeof(key));
	sqlite3_result_text(ctx, key, -1, SQLITE_TRANSIENT);
}

// The same folding the collation applies, over a whole string: case dropped and
// an accented Latin letter reduced to the letter underneath. Truncates rather
// than overflows. Returns the bytes written.
//
// Folding never lengthens a character -- an accented letter shrinks to one
// byte, Cyrillic and Greek keep their two -- so a buffer the size of the input
// is always enough.
static size_t fold_text(const char *text, char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return 0;
	}
	if (!text) {
		text = "";
	}

	int length = (int)strlen(text);
	int pos = 0;
	size_t at = 0;

	for (;;) {
		uint32_t code = utf8_next(text, length, &pos);
		if (code == 0) {
			break;
		}
		char encoded[4];
		int bytes = utf8_encode(fold_case(code), encoded);
		if (at + (size_t)bytes >= out_size) {
			break;
		}
		memcpy(out + at, encoded, (size_t)bytes);
		at += (size_t)bytes;
	}

	out[at] = '\0';
	return at;
}

// foldcase(x) inside SQL, which is what makes the search case-insensitive.
//
// SQLite's LIKE folds case for ASCII and for ASCII only, so on its own "кино"
// does not match "Кино" and "BJORK" does not match "Björk". The COLLATE
// NOCASE on the columns does not help: LIKE ignores collations. So both sides
// are folded instead, the row here and the query in library_search(), and LIKE
// is left comparing two strings that have already had the question of case
// taken out of them.
//
// It costs a call per row on a scan that was already reading every row: the
// pattern starts with a wildcard, so no index could serve it either way.
static void foldcase_sql(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
	const char *text = argc > 0 ? (const char *)sqlite3_value_text(argv[0]) : NULL;
	char folded[LIBRARY_SORT_KEY_MAX];
	fold_text(text, folded, sizeof(folded));
	sqlite3_result_text(ctx, folded, -1, SQLITE_TRANSIENT);
}

// The single character a name is filed under in an A-Z index, and the group it
// belongs to. Latin letters come back upper-cased; everything else is stored
// as-is.
static void index_character(const char *text, char *out, size_t out_size, int *group_out) {
	snprintf(out, out_size, "%s", "#");
	*group_out = SORT_GROUP_SYMBOL;

	if (!text || !text[0]) {
		*group_out = SORT_GROUP_EMPTY;
		return;
	}

	int length = (int)strlen(text);
	int pos = skip_article(text, length);
	int start = pos;

	uint32_t code = utf8_next(text, length, &pos);
	sort_group_t group = sort_group_of(code);
	*group_out = group;

	if (group == SORT_GROUP_LATIN && code < 0x80) {
		char letter = (char)(code >= 'a' && code <= 'z' ? code - 32 : code);
		snprintf(out, out_size, "%c", letter);
		return;
	}

	if (group == SORT_GROUP_DIGIT) {
		snprintf(out, out_size, "%s", "0-9");
		return;
	}

	// Everything else is filed under the character itself.
	int bytes = pos - start;
	if (bytes > 0 && (size_t)bytes < out_size) {
		memcpy(out, text + start, (size_t)bytes);
		out[bytes] = '\0';
	}
}

// The A-Z letter a sort key stands for.
//
// The key already holds the answer: its first byte is the script group and the
// byte after it is the first folded character, which for a Latin name is the
// lower-case letter itself. So the strip can be counted from the key the list
// is ordered by, and the name never has to be read -- which is what lets the
// sortkey index answer the whole ordered pass on its own.
//
// It has to agree with library_index_letter() on every name there is.
char library_index_letter_of_key(const char *key) {
	if (!key || !key[0]) {
		return '#';
	}

	int group = key[0] - '0';
	if (group > (int)SORT_GROUP_LATIN) {
		return LIBRARY_INDEX_PAST_Z;
	}
	if (group != (int)SORT_GROUP_LATIN) {
		return '#';
	}

	unsigned char folded = (unsigned char)key[1];
	if (folded >= 'a' && folded <= 'z') {
		return (char)(folded - 32);
	}
	// A Latin letter with no plain form to fold to sorts past every a-z, which
	// is the answer library_index_letter() gives it too.
	return LIBRARY_INDEX_PAST_Z;
}

int library_index_slot(char letter) {
	if (letter >= 'A' && letter <= 'Z') {
		return letter - 'A' + 1;
	}
	if (letter == LIBRARY_INDEX_PAST_Z) {
		return LIBRARY_INDEX_PAST_Z_SLOT;
	}
	return 0;
}

char library_index_letter(const char *name) {
	if (!name || !name[0]) {
		return '#';
	}

	int length = (int)strlen(name);
	int pos = skip_article(name, length);
	uint32_t code = utf8_next(name, length, &pos);

	// '#' is the head of the list -- the empty names, the punctuation and the
	// digits, which the collation files above A. What it is NOT is a bin for
	// everything unusual: Greek, Cyrillic, kana, Hangul and CJK are filed BELOW
	// Z, and lumping them in with the digits would make one bucket stand for
	// two ends of the same list. They get '~' instead, which says "past Z" and
	// which an index leaves out rather than pointing at the wrong end.
	sort_group_t group = sort_group_of(code);
	if (group > SORT_GROUP_LATIN) {
		return LIBRARY_INDEX_PAST_Z;
	}
	if (group != SORT_GROUP_LATIN) {
		return '#';
	}

	code = fold_case(code); // an accented letter files under the plain one
	if (code >= 'a' && code <= 'z') {
		return (char)(code - 32);
	}
	// A Latin letter with no plain form to fold to (Latin Extended-B and the
	// odd unmapped sign) sorts by its own code point, which is past every
	// a-z: same answer as the other scripts.
	return LIBRARY_INDEX_PAST_Z;
}

// ---------------------------------------------------------------------------
// database
// ---------------------------------------------------------------------------

static bool exec(const char *sql) {
	char *error = NULL;
	if (sqlite3_exec(db, sql, NULL, NULL, &error) != SQLITE_OK) {
		fprintf(stderr, "library: %s\n", error ? error : "unknown error");
		sqlite3_free(error);
		return false;
	}
	return true;
}

// The same, for a statement whose failure is the expected answer: adding a
// column that is already there. Logging that on every boot would be five lines
// of noise saying nothing went wrong.
static bool exec_quiet(const char *sql) {
	return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
}

// A monotonic millisecond reading, only ever used as a difference.
static uint32_t now_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)(now.tv_sec * 1000u) + (uint32_t)(now.tv_nsec / 1000000);
}

// See the call site. Points SQLite's scratch files at the folder the database
// lives in. Held in a static because sqlite3_temp_directory is a bare pointer
// SQLite keeps and never copies.
static char temp_dir[512];

static void set_temp_directory(const char *db_path) {
	// Built somewhere else first. This runs again at every card change, and
	// rewriting the buffer SQLite is holding would move the ground under it;
	// and a failure has to put the pointer back, not leave it on the folder of
	// a card that has gone.
	char wanted[sizeof(temp_dir)];
	snprintf(wanted, sizeof(wanted), "%s", db_path);
	char *slash = strrchr(wanted, '/');
	if (!slash) {
		sqlite3_temp_directory = NULL; // a bare name: no folder to point at
		return;
	}
	*slash = '\0';
	if (wanted[0] == '\0') {
		wanted[0] = '/';
		wanted[1] = '\0';
	}

	// A card that cannot be written to would turn every sort into an error
	// instead of a slow sort, so the default stands where the card is
	// read-only.
	if (access(wanted, W_OK) != 0) {
		fprintf(stderr, "library: %s is not writable; sorts will spill to the default temp folder\n", wanted);
		sqlite3_temp_directory = NULL;
		return;
	}

	snprintf(temp_dir, sizeof(temp_dir), "%s", wanted);
	sqlite3_temp_directory = temp_dir;
}

// ---------------------------------------------------------------------------
// Filling in the sort keys
//
// A database written before the column existed gains the column empty. A list
// ordered by a column full of nulls is not ordered, so the lists name the key
// only once every row has one; until then they order by the collation, which is
// correct and slow.
//
// Filling it in is one UPDATE per slice of row ids rather than a round trip per
// row, on a thread of its own at the lowest priority there is. The card is
// slow, the user is looking at a library that already works, and this is the
// one job in the player nobody is waiting for.
// ---------------------------------------------------------------------------

// True once every row of the media tables carries a key. Read without the lock
// while a query is being built: the SQL that comes out is right either way, and
// the worst a stale read can do is order one list by the collation instead.
static volatile bool sortkeys_ready;

static pthread_t backfill_thread;
static bool backfill_started;
static volatile bool backfill_stop;

// Row ids one UPDATE covers. The same reasoning as the scan's commit batch: an
// open transaction holds its dirty pages, and this device has ten megabytes.
#define BACKFILL_BATCH 2000

// The tables that have the column, with the name each key is made from.
static const struct {
	const char *table;
	const char *column;
} KEYED_TABLES[] = {
	{"MEDIA_TABLE", "name"},
	{"ALBUM_TABLE", "album"},
	{"ARTIST_TABLE", "artist"},
	{"ALBUM_ARTIST_TABLE", "album_artist"},
	{"GENRE_TABLE", "genre"},
};

// Whether anything is still without a key. Cheap whatever the library's size:
// nulls sort at the head of the sortkey index, so this is an index seek and not
// a walk of the table.
static int count_rows(const char *sql);

static bool sortkeys_missing(void) {
	bool missing = false;
	pthread_mutex_lock(&db_lock);
	for (size_t i = 0; i < sizeof(KEYED_TABLES) / sizeof(KEYED_TABLES[0]) && !missing; i++) {
		char sql[128];
		snprintf(sql, sizeof(sql), "SELECT 1 FROM %s WHERE sortkey IS NULL LIMIT 1", KEYED_TABLES[i].table);
		missing = count_rows(sql) > 0;
	}
	pthread_mutex_unlock(&db_lock);
	return missing;
}

// The indexes over the sort key. Separate from the rest because they are worth
// having only once every row has a key: kept up to date through a backfill they
// would be one b-tree insertion per row instead of a single bulk build, an
// order of magnitude slower on a large library.
static void build_sort_indexes(void) {
	pthread_mutex_lock(&db_lock);
	if (!db) {
		pthread_mutex_unlock(&db_lock);
		return;
	}
	for (size_t i = 0; i < sizeof(SORT_INDEXES) / sizeof(SORT_INDEXES[0]); i++) {
		if (!exec(SORT_INDEXES[i])) {
			fprintf(stderr, "library: sort index step %zu failed\n", i);
		}
	}
	pthread_mutex_unlock(&db_lock);
}

static void *backfill_worker(void *arg) {
	(void)arg;
	thread_be_background("sortkeys");

	uint32_t started_ms = now_ms();
	int filled = 0;

	// The four lookup tables in one statement each: a card holds thousands of
	// albums, not hundreds of thousands.
	for (size_t i = 1; i < sizeof(KEYED_TABLES) / sizeof(KEYED_TABLES[0]) && !backfill_stop; i++) {
		char sql[192];
		snprintf(sql, sizeof(sql), "UPDATE %s SET sortkey=sortkey(%s) WHERE sortkey IS NULL", KEYED_TABLES[i].table,
				 KEYED_TABLES[i].column);
		pthread_mutex_lock(&db_lock);
		if (db && exec(sql)) {
			filled += sqlite3_changes(db);
		}
		pthread_mutex_unlock(&db_lock);
	}

	// And the tracks a slice of row ids at a time, so the lock is never held
	// long and no transaction grows past a few hundred rows.
	sqlite3_int64 highest = 0;
	pthread_mutex_lock(&db_lock);
	highest = db ? (sqlite3_int64)count_rows("SELECT IFNULL(MAX(rowid),0) FROM MEDIA_TABLE") : 0;
	pthread_mutex_unlock(&db_lock);

	for (sqlite3_int64 from = 0; from < highest && !backfill_stop; from += BACKFILL_BATCH) {
		// A scan writes the key with the row, so it does this job better than
		// this does; and it empties the tables first, which would leave this
		// walking row ids that mean something else now.
		if (scan_running) {
			break;
		}
		pthread_mutex_lock(&db_lock);
		sqlite3_stmt *stmt = NULL;
		// By row id and nothing else. Adding "AND sortkey IS NULL" reads well
		// and costs dearly: SQLite then has the choice of the sortkey index,
		// whose null entries are all at its head, and walks them for every
		// batch. Rewriting a key that is already right is free by comparison.
		if (db && sqlite3_prepare_v2(db,
									 "UPDATE MEDIA_TABLE SET sortkey=sortkey(name)"
									 " WHERE rowid > ? AND rowid <= ?",
									 -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, from);
			sqlite3_bind_int64(stmt, 2, from + BACKFILL_BATCH);
			sqlite3_step(stmt);
			sqlite3_finalize(stmt);
			filled += sqlite3_changes(db);
		}
		pthread_mutex_unlock(&db_lock);
	}

	if (!backfill_stop && !sortkeys_missing()) {
		build_sort_indexes();
		sortkeys_ready = true;
		printf("library: %d sort keys filled in in %u ms; the lists are indexed from now on\n", filled,
			   now_ms() - started_ms);
	}
	return NULL;
}

static void sortkeys_start_backfill(void) {
	// The ordinary case: every boot but the first one after the upgrade.
	if (!sortkeys_missing()) {
		build_sort_indexes();
		sortkeys_ready = true;
		return;
	}

	printf("library: the index predates the sort keys; filling them in the background\n");
	backfill_stop = false;
	if (pthread_create(&backfill_thread, NULL, backfill_worker, NULL) == 0) {
		backfill_started = true;
	} else {
		fprintf(stderr, "library: cannot fill in the sort keys; the lists keep the old ordering\n");
	}
}

static void backfill_stop_and_wait(void) {
	if (!backfill_started) {
		return;
	}
	backfill_stop = true;
	pthread_join(backfill_thread, NULL);
	backfill_started = false;
}

// Whether the ordered query may name the key. Favourites never may: they have
// no such column and are ordered by when they were starred, not by name.
static bool list_uses_sortkey(library_list_t kind) {
	// Favourites run by when they were starred and a playlist by the order it
	// was built, so neither has a sort key to read.
	return sortkeys_ready && kind != LIBRARY_LIST_FAVOURITES && kind != LIBRARY_LIST_PLAYLIST;
}

static void next_mount_serial(void);

bool library_open(const char *db_path) {
	if (db) {
		return true;
	}
	if (!db_path || !db_path[0]) {
		return false;
	}

	// A ceiling SQLite will stay under by recycling instead of growing. The
	// scan is the only thing here that touches a lot of rows and it has no
	// business holding megabytes on a device with ten of them.
	sqlite3_soft_heap_limit64(1536 * 1024);

	if (sqlite3_open(db_path, &db) != SQLITE_OK) {
		fprintf(stderr, "library: cannot open %s: %s\n", db_path, sqlite3_errmsg(db));
		sqlite3_close(db);
		db = NULL;
		return false;
	}

	// Every ORDER BY in the schema names this collation; without it the
	// queries fail outright rather than sorting badly.
	sqlite3_create_collation(db, "listorder", SQLITE_UTF8, NULL, library_collate_listorder);

	// And the same ordering as a value the SQL can compute, for filling in a
	// database written before the column existed.
	sqlite3_create_function(db, "sortkey", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, sortkey_sql, NULL, NULL);

	// And the folding on its own, which the search matches through.
	sqlite3_create_function(db, "foldcase", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, foldcase_sql, NULL,
							NULL);

	// Where SQLite spills a sort it cannot hold in memory.
	//
	// The lists themselves read the sortkey index in order, but the queries
	// that do sort (one album by disc position, one artist's records) go
	// through a temporary b-tree, and anything past about a megabyte of it goes
	// to a scratch file. SQLITE_TEMP_STORE is 1 (file, not memory) precisely so
	// it does not sit in RAM, but with nothing set the unix VFS falls back to
	// /tmp, which on this device is tmpfs -- the RAM it was avoiding, by another
	// name. Next to the database instead: the card is slow, but a sort that
	// reaches here is already the slow part.
	set_temp_directory(db_path);

	// A card is slow and may be pulled out at any moment. WAL needs shared
	// memory the FAT driver does not provide, so stay on the default journal
	// and just stop waiting for the platter on every commit.
	exec("PRAGMA synchronous=NORMAL");
	exec("PRAGMA journal_mode=TRUNCATE");
	exec("PRAGMA cache_size=-256"); // 256 KB, not the 2 MB default

	for (size_t i = 0; i < sizeof(SCHEMA) / sizeof(SCHEMA[0]); i++) {
		if (!exec(SCHEMA[i])) {
			fprintf(stderr, "library: schema step %zu failed\n", i);
		}
	}
	// Quietly: on every database but the first one opened after the upgrade,
	// each of these is "duplicate column name", which is the answer that says
	// there is nothing to do.
	for (size_t i = 0; i < sizeof(SCHEMA_COLUMNS) / sizeof(SCHEMA_COLUMNS[0]); i++) {
		exec_quiet(SCHEMA_COLUMNS[i]);
	}
	for (size_t i = 0; i < sizeof(SCHEMA_INDEXES) / sizeof(SCHEMA_INDEXES[0]); i++) {
		if (!exec(SCHEMA_INDEXES[i])) {
			fprintf(stderr, "library: index step %zu failed\n", i);
		}
	}

	pthread_mutex_lock(&db_lock);
	bump_all_generations(); // a different card is a different set of row ids
	pthread_mutex_unlock(&db_lock);

	next_mount_serial();

	printf("library: %s open, %d tracks indexed\n", db_path, library_track_count());

	sortkeys_start_backfill();
	return true;
}

static void library_scan_stop_and_wait(void);
static void finalize_statements(void);

void library_close(void) {
	// Waits, rather than just asking: a scan still in flight owns prepared
	// statements, sqlite3_close() then answers SQLITE_BUSY and leaves the file
	// open -- and on this device an open file on the card makes the unmount
	// fail, so the next card cannot be mounted in its place.
	library_scan_stop_and_wait();
	// And the same for the backfill, which holds the lock in short bursts and
	// would be writing to a handle that had just been closed.
	backfill_stop_and_wait();
	sortkeys_ready = false;

	pthread_mutex_lock(&db_lock);
	if (db) {
		finalize_statements();
		int rc = sqlite3_close(db);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "library: sqlite3_close returned %d; the handle may be leaking\n", rc);
		}
		db = NULL;
		bump_all_generations(); // every open list is now looking at nothing
	}
	pthread_mutex_unlock(&db_lock);
}

static int count_rows(const char *sql) {
	if (!db) {
		return 0;
	}

	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		return 0;
	}

	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		count = sqlite3_column_int(stmt, 0);
	}
	sqlite3_finalize(stmt);
	return count;
}

int library_track_count(void) {
	pthread_mutex_lock(&db_lock);
	int count = count_rows("SELECT COUNT(*) FROM MEDIA_TABLE");
	pthread_mutex_unlock(&db_lock);
	return count;
}

int library_list(library_list_t kind, char out[][256], int max) {
	static const struct {
		const char *table;
		const char *column;
	} SOURCES[] = {
		{"MEDIA_TABLE", "name"},
		{"ALBUM_TABLE", "album"},
		{"ARTIST_TABLE", "artist"},
		{"ALBUM_ARTIST_TABLE", "album_artist"},
		{"GENRE_TABLE", "genre"},
	};

	if (kind < 0 || kind >= (int)(sizeof(SOURCES) / sizeof(SOURCES[0])) || max <= 0) {
		return 0;
	}

	char sql[256];
	snprintf(sql, sizeof(sql), "SELECT %s FROM %s WHERE %s <> '' ORDER BY %s COLLATE listorder LIMIT %d",
			 SOURCES[kind].column, SOURCES[kind].table, SOURCES[kind].column, SOURCES[kind].column, max);

	pthread_mutex_lock(&db_lock);

	int count = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		while (count < max && sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *text = sqlite3_column_text(stmt, 0);
			snprintf(out[count], 256, "%s", text ? (const char *)text : "");
			count++;
		}
		sqlite3_finalize(stmt);
	}

	pthread_mutex_unlock(&db_lock);
	return count;
}

// ---------------------------------------------------------------------------
// single-row lookups, favourites and row iteration for the Musica pages
// ---------------------------------------------------------------------------

// Whitelisted SQL fragments per kind/filter -- nothing user-controlled is ever
// pasted into the statement text; the filter *value* travels as a bound
// parameter.
static const char *filter_column(library_filter_t filter) {
	switch (filter) {
	case LIBRARY_FILTER_ALBUM:
		return "album";
	case LIBRARY_FILTER_ARTIST:
		return "artist";
	case LIBRARY_FILTER_ALBUM_ARTIST:
		return "album_artist";
	case LIBRARY_FILTER_GENRE:
		return "genre";
	default:
		return NULL;
	}
}

bool library_track_title(const char *path, char *out, size_t out_size) {
	if (!path || !path[0] || !out) {
		return false;
	}

	pthread_mutex_lock(&db_lock);
	bool found = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT name FROM MEDIA_TABLE WHERE path=? LIMIT 1", -1, &stmt, NULL) ==
				  SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *name = sqlite3_column_text(stmt, 0);
			if (name && name[0]) {
				snprintf(out, out_size, "%s", (const char *)name);
				found = true;
			}
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return found;
}

// Title and artist in one go, for a list built from paths rather than from a
// query -- a playlist. The scan already put both in the row, so a lookup here
// costs one indexed SELECT instead of opening the file and parsing its tags.
// Either output may be left empty: a track can be indexed without an artist.
bool library_track_names(const char *path, char *title_out, size_t title_size, char *artist_out, size_t artist_size) {
	if (!path || !path[0]) {
		return false;
	}
	if (title_out && title_size) {
		title_out[0] = '\0';
	}
	if (artist_out && artist_size) {
		artist_out[0] = '\0';
	}

	pthread_mutex_lock(&db_lock);
	bool found = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT name, artist FROM MEDIA_TABLE WHERE path=? LIMIT 1", -1, &stmt, NULL) ==
				  SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *name = sqlite3_column_text(stmt, 0);
			const unsigned char *artist = sqlite3_column_text(stmt, 1);
			if (title_out && title_size && name && name[0]) {
				snprintf(title_out, title_size, "%s", (const char *)name);
				found = true;
			}
			if (artist_out && artist_size && artist && artist[0]) {
				snprintf(artist_out, artist_size, "%s", (const char *)artist);
			}
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return found;
}

library_quality_t library_track_quality(const char *path, int *rate_out, int *bits_out) {
	if (rate_out) {
		*rate_out = 0;
	}
	if (bits_out) {
		*bits_out = 0;
	}
	if (!path || !path[0]) {
		return LIBRARY_QUALITY_NONE;
	}

	int format = -1, rate = 0, bits = 0;
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT format, sample_rate, bit FROM MEDIA_TABLE WHERE path=? LIMIT 1", -1,
								 &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			format = sqlite3_column_int(stmt, 0);
			rate = sqlite3_column_int(stmt, 1);
			bits = sqlite3_column_int(stmt, 2);
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);

	if (rate_out) {
		*rate_out = rate;
	}
	if (bits_out) {
		*bits_out = bits;
	}

	switch (format) {
	case FORMAT_DSD:
		return LIBRARY_QUALITY_DSD;
	case FORMAT_MP3:
	case FORMAT_OGG:
	case FORMAT_AAC:
	case FORMAT_OPUS:
		return LIBRARY_QUALITY_LOSSY;
	case FORMAT_WAV:
	case FORMAT_FLAC:
	case FORMAT_WAVPACK:
	case FORMAT_ALAC:
	case FORMAT_APE:
		break;
	default:
		return LIBRARY_QUALITY_NONE; // no row, or a code from an older index
	}

	// Lossless, so the numbers decide. WavPack has none -- nothing reads its
	// header -- and neither does a card indexed before the scan started
	// storing them, and both are better off with no badge.
	if (rate <= 0 || bits <= 0) {
		return LIBRARY_QUALITY_NONE;
	}
	return (bits <= 16 && rate <= 44100) ? LIBRARY_QUALITY_CD : LIBRARY_QUALITY_HIFI;
}

// One name and the slot it came from, for the batch below.
typedef struct {
	const char *name;
	int slot;
} name_slot_t;

static int name_slot_cmp(const void *a, const void *b) {
	const name_slot_t *x = a;
	const name_slot_t *y = b;
	int order = strcasecmp(x->name, y->name);
	return order ? order : (x->slot - y->slot);
}

// Finds the tracks whose file names are in `names`, in one pass.
//
// The index stores whole paths and nothing else, so asking for a file name
// means `LIKE '%/name'` -- a leading wildcard, which no index can serve, so one
// name costs a scan of the table. A playlist of four hundred entries would cost
// four hundred scans. This turns it around: the names are sorted once and every
// row of the table is looked for among them, so the batch costs one scan
// whatever its size.
//
// Matching is case-insensitive by byte, which is how the card itself compares
// file names.
int library_match_basenames(const char *const *names, int count, library_basename_cb cb, void *user) {
	if (!names || count <= 0 || !cb) {
		return 0;
	}

	name_slot_t *sorted = calloc((size_t)count, sizeof(*sorted));
	if (!sorted) {
		return 0;
	}
	int wanted = 0;
	for (int i = 0; i < count; i++) {
		if (names[i] && names[i][0]) {
			sorted[wanted].name = names[i];
			sorted[wanted].slot = i;
			wanted++;
		}
	}
	if (wanted == 0) {
		free(sorted);
		return 0;
	}
	qsort(sorted, (size_t)wanted, sizeof(*sorted), name_slot_cmp);

	int matched = 0;
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT path FROM MEDIA_TABLE", -1, &stmt, NULL) == SQLITE_OK) {
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *path = (const char *)sqlite3_column_text(stmt, 0);
			if (!path || !path[0]) {
				continue;
			}
			const char *slash = strrchr(path, '/');
			const char *base = slash ? slash + 1 : path;

			int low = 0, high = wanted;
			while (low < high) {
				int mid = low + (high - low) / 2;
				if (strcasecmp(sorted[mid].name, base) < 0) {
					low = mid + 1;
				} else {
					high = mid;
				}
			}
			for (int i = low; i < wanted && strcasecmp(sorted[i].name, base) == 0; i++) {
				cb(sorted[i].slot, path, user);
				matched++;
			}
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);

	free(sorted);
	return matched;
}

// The album a track belongs to, for "Mostra album": the row is already there,
// this only asks a different column of it.
bool library_track_album(const char *path, char *out, size_t out_size) {
	if (!path || !path[0] || !out) {
		return false;
	}

	pthread_mutex_lock(&db_lock);
	bool found = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT album FROM MEDIA_TABLE WHERE path=? LIMIT 1", -1, &stmt, NULL) ==
				  SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *album = sqlite3_column_text(stmt, 0);
			if (album && album[0]) {
				snprintf(out, out_size, "%s", (const char *)album);
				found = true;
			}
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return found;
}

bool library_fav_contains(const char *path) {
	if (!path || !path[0]) {
		return false;
	}

	pthread_mutex_lock(&db_lock);
	bool found = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT 1 FROM FAVOURITES WHERE path=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		found = sqlite3_step(stmt) == SQLITE_ROW;
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return found;
}

bool library_fav_toggle(const char *path, const char *name, const char *artist) {
	if (!path || !path[0]) {
		return false;
	}

	bool now_favourite = !library_fav_contains(path);

	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db) {
		if (now_favourite) {
			if (sqlite3_prepare_v2(db,
								   "INSERT OR REPLACE INTO FAVOURITES(path,name,artist,added_at)"
								   " VALUES(?,?,?,strftime('%s','now'))",
								   -1, &stmt, NULL) == SQLITE_OK) {
				sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
				sqlite3_bind_text(stmt, 2, name ? name : "", -1, SQLITE_TRANSIENT);
				sqlite3_bind_text(stmt, 3, artist ? artist : "", -1, SQLITE_TRANSIENT);
				sqlite3_step(stmt);
				sqlite3_finalize(stmt);
			}
		} else {
			if (sqlite3_prepare_v2(db, "DELETE FROM FAVOURITES WHERE path=?", -1, &stmt, NULL) == SQLITE_OK) {
				sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
				sqlite3_step(stmt);
				sqlite3_finalize(stmt);
			}
		}
		// Starring rewrites the row, which on this table means a new row id --
		// and only on this table, so a list of tracks is untouched.
		bump_generation(GEN_FAVOURITES);
	}
	pthread_mutex_unlock(&db_lock);
	return now_favourite;
}

int library_forget_under(const char *prefix) {
	if (!prefix || !prefix[0]) {
		return 0;
	}

	char pattern[600];
	snprintf(pattern, sizeof(pattern), "%.500s%%", prefix);

	pthread_mutex_lock(&db_lock);
	int removed = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "DELETE FROM FAVOURITES WHERE path LIKE ?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_DONE) {
			removed = sqlite3_changes(db);
		}
		sqlite3_finalize(stmt);
	}
	// And the remembered playback position, if it pointed at a cache file.
	if (db && sqlite3_prepare_v2(db, "DELETE FROM PLAYBACK_STATE WHERE path LIKE ?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	bump_generation(GEN_FAVOURITES);
	pthread_mutex_unlock(&db_lock);

	if (removed > 0) {
		printf("library: removed %d favourites that pointed at the Qobuz cache\n", removed);
	}
	return removed;
}

int library_for_each(library_list_t kind, library_filter_t filter, const char *value, library_row_cb cb, void *user) {
	return library_for_each_ordered(kind, filter, value, LIBRARY_ORDER_DEFAULT, cb, user);
}

int library_for_each_ordered(library_list_t kind, library_filter_t filter, const char *value, library_order_t order,
							 library_row_cb cb, void *user) {
	if (!cb) {
		return 0;
	}

	char sql[512];
	const char *col = filter_column(filter);
	// Same choice the handles make, for the same reason (see list_sql): the two
	// expressions order identically, one of them with an index behind it.
	const char *by_name = list_uses_sortkey(kind) ? "sortkey" : "name COLLATE listorder";

	if (kind == LIBRARY_LIST_FAVOURITES) {
		snprintf(sql, sizeof(sql), "SELECT name, path, artist FROM FAVOURITES ORDER BY added_at");
	} else if (kind == LIBRARY_LIST_TRACKS) {
		// Inside one album the disc order is the natural one; everywhere else
		// the titles read best alphabetically.
		//
		// LIBRARY_ORDER_ALBUM is the third case: an artist's tracks with each
		// record kept together and in its own running order, which is how a
		// person thinks about an artist's work and not how an alphabetical list
		// of titles presents it.
		char order_sql[128];
		if (order == LIBRARY_ORDER_ALBUM) {
			snprintf(order_sql, sizeof(order_sql), "album COLLATE listorder, " TRACK_ORDER_IN_ALBUM ", %s", by_name);
		} else if (col && value && filter == LIBRARY_FILTER_ALBUM) {
			snprintf(order_sql, sizeof(order_sql), TRACK_ORDER_IN_ALBUM ", %s", by_name);
		} else {
			snprintf(order_sql, sizeof(order_sql), "%s", by_name);
		}
		if (col && value) {
			snprintf(sql, sizeof(sql), "SELECT name, path, artist FROM MEDIA_TABLE WHERE %s=? ORDER BY %s", col,
					 order_sql);
		} else {
			snprintf(sql, sizeof(sql), "SELECT name, path, artist FROM MEDIA_TABLE ORDER BY %s", order_sql);
		}
	} else {
		static const struct {
			const char *table;
			const char *column;
		} SOURCES[] = {
			{"MEDIA_TABLE", "name"},
			{"ALBUM_TABLE", "album"},
			{"ARTIST_TABLE", "artist"},
			{"ALBUM_ARTIST_TABLE", "album_artist"},
			{"GENRE_TABLE", "genre"},
		};
		if (kind < 0 || kind >= (int)(sizeof(SOURCES) / sizeof(SOURCES[0]))) {
			return 0;
		}
		if (kind == LIBRARY_LIST_ALBUMS) {
			// An album row wants its cover, and covers hang off files: hand the
			// caller one representative track per album to load the art from.
			snprintf(sql, sizeof(sql),
					 "SELECT album, (SELECT path FROM MEDIA_TABLE m WHERE m.album = ALBUM_TABLE.album"
					 " ORDER BY COALESCE(m.disc,1), m.dis_id LIMIT 1), NULL FROM ALBUM_TABLE WHERE album <> '' ORDER BY %s",
					 list_uses_sortkey(kind) ? "sortkey" : "album COLLATE listorder");
		} else if (list_uses_sortkey(kind)) {
			snprintf(sql, sizeof(sql), "SELECT %s, NULL, NULL FROM %s WHERE %s <> '' ORDER BY sortkey",
					 SOURCES[kind].column, SOURCES[kind].table, SOURCES[kind].column);
		} else {
			snprintf(sql, sizeof(sql), "SELECT %s, NULL, NULL FROM %s WHERE %s <> '' ORDER BY %s COLLATE listorder",
					 SOURCES[kind].column, SOURCES[kind].table, SOURCES[kind].column, SOURCES[kind].column);
		}
	}

	pthread_mutex_lock(&db_lock);

	int count = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		if (col && value && kind == LIBRARY_LIST_TRACKS) {
			sqlite3_bind_text(stmt, 1, value, -1, SQLITE_TRANSIENT);
		}
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *name = (const char *)sqlite3_column_text(stmt, 0);
			const char *path = (const char *)sqlite3_column_text(stmt, 1);
			const char *artist = (const char *)sqlite3_column_text(stmt, 2);
			if (!cb(name ? name : "", path, artist, user)) {
				break; // the caller has all it can hold
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
// Reading a list into RAM in full costs a name and a path strdup'd per row,
// about a hundred bytes each: two hundred thousand tracks is nineteen
// megabytes, on a device with ten free, to draw the twelve rows that fit on the
// screen.
//
// A handle holds the row ids instead -- four bytes a row, eight hundred
// kilobytes for the same library -- and the rows themselves are read back a
// windowful at a time, by row id, which is a b-tree descent and costs nothing.
//
// Row ids rather than LIMIT/OFFSET paging because they are what lets a row be
// reached by position (the A-Z strip lands on one) and what the queue is
// written down as.
// ---------------------------------------------------------------------------

struct library_index {
	// What it was built from, kept so a queue made out of a handle can be
	// written down as the question instead of the two hundred thousand rows of
	// its answer.
	library_list_t kind;
	library_filter_t filter;
	library_order_t order;
	char value[256];

	bool desc; // the list reads backwards; the rows are always stored ascending
	gen_domain_t domain;
	unsigned generation;

	int32_t *rows;
	int count;

	int buckets[LIBRARY_INDEX_BUCKETS];
	bool buckets_valid;
};

// ---------------------------------------------------------------------------
// A playlist is a table
//
// See library.h for what the shape is and why. What follows is the mechanics of
// naming one: a table name cannot be bound like a value, so it is written into
// the statement, and the only safe way to do that with a name the user typed is
// SQLite's own quoting -- double quotes around it, and any double quote inside
// it doubled. Nothing else is trusted about the name; it goes in exactly as
// typed so that the table is called what the playlist is called.
// ---------------------------------------------------------------------------

// "M3U_<name>", quoted. False when the name is too long to be one, which is the
// only thing that can go wrong here.
static bool playlist_table(const char *name, char *out, size_t out_size) {
	if (!name || !name[0] || !out || out_size < 8) {
		return false;
	}
	size_t at = 0;
	out[at++] = '"';
	const char *prefix = LIBRARY_PLAYLIST_PREFIX;
	while (*prefix && at + 2 < out_size) {
		out[at++] = *prefix++;
	}
	for (const char *c = name; *c; c++) {
		if (at + 3 >= out_size) {
			return false;
		}
		if (*c == '"') {
			out[at++] = '"'; // doubled, which is how a quote lives inside one
		}
		out[at++] = *c;
	}
	if (at + 2 > out_size) {
		return false;
	}
	out[at++] = '"';
	out[at] = '\0';
	return true;
}

#define PLAYLIST_TABLE_MAX 512

// The ordered query behind a list, as SQL. `select` is what to ask for, so the
// same builder serves both the row-id pass and the streaming reader.
static void list_sql(char *sql, size_t size, const char *select, library_list_t kind, library_filter_t filter,
					 const char *value, library_order_t order) {
	const char *col = filter_column(filter);

	if (kind == LIBRARY_LIST_FAVOURITES) {
		snprintf(sql, size, "SELECT %s FROM FAVOURITES ORDER BY added_at", select);
		return;
	}

	// A playlist is a table of its own, so the name of the table is the value
	// the caller passed. It runs in the order it was built -- `idx` -- and
	// leaves out the entries whose file was not there at the last look.
	if (kind == LIBRARY_LIST_PLAYLIST) {
		char quoted[PLAYLIST_TABLE_MAX];
		if (!playlist_table(value, quoted, sizeof(quoted))) {
			sql[0] = '\0';
			return;
		}
		// A truncated statement is not a slower statement, it is a different
		// one, so it is refused rather than run.
		if (snprintf(sql, size, "SELECT %s FROM %s WHERE present<>0 ORDER BY idx", select, quoted) >= (int)size) {
			sql[0] = '\0';
		}
		return;
	}

	// The sort key where every row has one, the collation where they do not.
	// The two give the same order (see sort_key) -- one of them off an index,
	// the other by sorting the whole table into a temporary b-tree.
	const char *by_name = list_uses_sortkey(kind) ? "sortkey" : "name COLLATE listorder";

	if (kind == LIBRARY_LIST_TRACKS) {
		// Inside one album the disc order is the natural one; everywhere else
		// the titles read best alphabetically. LIBRARY_ORDER_ALBUM is the third
		// case: an artist's tracks with each record kept together and in its
		// own running order.
		char order_sql[128];
		if (order == LIBRARY_ORDER_ALBUM) {
			snprintf(order_sql, sizeof(order_sql), "album COLLATE listorder, " TRACK_ORDER_IN_ALBUM ", %s", by_name);
		} else if (col && value && filter == LIBRARY_FILTER_ALBUM) {
			snprintf(order_sql, sizeof(order_sql), TRACK_ORDER_IN_ALBUM ", %s", by_name);
		} else {
			snprintf(order_sql, sizeof(order_sql), "%s", by_name);
		}
		if (col && value) {
			snprintf(sql, size, "SELECT %s FROM MEDIA_TABLE WHERE %s=? ORDER BY %s", select, col, order_sql);
		} else {
			snprintf(sql, size, "SELECT %s FROM MEDIA_TABLE ORDER BY %s", select, order_sql);
		}
		return;
	}

	// One artist's records, rather than one artist's tracks: the album list
	// narrowed to the albums that artist appears on. ALBUM_TABLE holds the
	// distinct names and the sort keys, and which of them belong to an artist
	// is a question for MEDIA_TABLE -- answered off media_artist_idx, so the
	// subquery is a lookup and not a scan.
	if (kind == LIBRARY_LIST_ALBUMS && col && value &&
		(filter == LIBRARY_FILTER_ARTIST || filter == LIBRARY_FILTER_ALBUM_ARTIST)) {
		snprintf(sql, size,
				 "SELECT %s FROM ALBUM_TABLE WHERE album <> ''"
				 " AND album IN (SELECT album FROM MEDIA_TABLE WHERE %s=?) ORDER BY %s",
				 select, col, list_uses_sortkey(kind) ? "sortkey" : "album COLLATE listorder");
		return;
	}

	// One artist's records, rather than one artist's tracks: the album list
	// narrowed to the albums that artist appears on. ALBUM_TABLE holds the
	// distinct names and the sort keys, and which of them belong to an artist
	// is a question for MEDIA_TABLE -- answered off media_artist_idx, so the
	// subquery is a lookup and not a scan.
	if (kind == LIBRARY_LIST_ALBUMS && col && value &&
		(filter == LIBRARY_FILTER_ARTIST || filter == LIBRARY_FILTER_ALBUM_ARTIST)) {
		snprintf(sql, size,
				 "SELECT %s FROM ALBUM_TABLE WHERE album <> ''"
				 " AND album IN (SELECT album FROM MEDIA_TABLE WHERE %s=?) ORDER BY %s",
				 select, col, list_uses_sortkey(kind) ? "sortkey" : "album COLLATE listorder");
		return;
	}

	static const struct {
		const char *table;
		const char *column;
	} SOURCES[] = {
		{"MEDIA_TABLE", "name"},
		{"ALBUM_TABLE", "album"},
		{"ARTIST_TABLE", "artist"},
		{"ALBUM_ARTIST_TABLE", "album_artist"},
		{"GENRE_TABLE", "genre"},
	};
	if (kind < 0 || kind >= (int)(sizeof(SOURCES) / sizeof(SOURCES[0]))) {
		sql[0] = '\0';
		return;
	}
	if (list_uses_sortkey(kind)) {
		snprintf(sql, size, "SELECT %s FROM %s WHERE %s <> '' ORDER BY sortkey", select, SOURCES[kind].table,
				 SOURCES[kind].column);
	} else {
		snprintf(sql, size, "SELECT %s FROM %s WHERE %s <> '' ORDER BY %s COLLATE listorder", select,
				 SOURCES[kind].table, SOURCES[kind].column, SOURCES[kind].column);
	}
}

// One row of a list, by row id. The row id already says which row, so none of
// the filtering the ordered query does is needed here.
//
// `out` is only written for the kinds whose table is named by the list itself,
// which today is the playlists; everything else answers with a literal.
static const char *row_by_id_sql(library_list_t kind, const char *value, char *out, size_t out_size) {
	if (kind == LIBRARY_LIST_PLAYLIST) {
		char quoted[PLAYLIST_TABLE_MAX];
		if (!playlist_table(value, quoted, sizeof(quoted))) {
			return NULL;
		}
		if (snprintf(out, out_size, "SELECT title, path, artist FROM %s WHERE rowid=?", quoted) >= (int)out_size) {
			return NULL;
		}
		return out;
	}
	switch (kind) {
	case LIBRARY_LIST_TRACKS:
		return "SELECT name, path, artist FROM MEDIA_TABLE WHERE rowid=?";
	case LIBRARY_LIST_ALBUMS:
		// An album row wants its cover, and covers hang off files: hand the
		// caller one representative track per album to load the art from. This
		// is why it is not in the ordered pass -- there it would run the
		// subquery for every album on the card, to be thrown away.
		//
		// The same first track names the album's artist: its album artist, or
		// its artist on a file that has none. Lists that show it read it from
		// here; a window of rows is a dozen lookups on the album index.
		return "SELECT album, (SELECT path FROM MEDIA_TABLE m WHERE m.album = ALBUM_TABLE.album"
			   " ORDER BY COALESCE(m.disc,1), m.dis_id LIMIT 1),"
			   " (SELECT COALESCE(NULLIF(m.album_artist,''), m.artist) FROM MEDIA_TABLE m"
			   " WHERE m.album = ALBUM_TABLE.album ORDER BY COALESCE(m.disc,1), m.dis_id LIMIT 1)"
			   " FROM ALBUM_TABLE WHERE rowid=?";
	case LIBRARY_LIST_ARTISTS:
		return "SELECT artist, NULL, NULL FROM ARTIST_TABLE WHERE rowid=?";
	case LIBRARY_LIST_ALBUM_ARTISTS:
		return "SELECT album_artist, NULL, NULL FROM ALBUM_ARTIST_TABLE WHERE rowid=?";
	case LIBRARY_LIST_GENRES:
		return "SELECT genre, NULL, NULL FROM GENRE_TABLE WHERE rowid=?";
	case LIBRARY_LIST_FAVOURITES:
		return "SELECT name, path, artist FROM FAVOURITES WHERE rowid=?";
	default:
		return NULL;
	}
}

// The column a list is named by, for the A-Z buckets.
static const char *name_column(library_list_t kind) {
	switch (kind) {
	case LIBRARY_LIST_ALBUMS:
		return "album";
	case LIBRARY_LIST_PLAYLIST:
		return "title";
	case LIBRARY_LIST_ARTISTS:
		return "artist";
	case LIBRARY_LIST_ALBUM_ARTISTS:
		return "album_artist";
	case LIBRARY_LIST_GENRES:
		return "genre";
	default:
		return "name";
	}
}

library_index_t *library_index_open(library_list_t kind, library_filter_t filter, const char *value,
									library_order_t order, bool desc) {
	struct library_index *ix = calloc(1, sizeof(*ix));
	if (!ix) {
		return NULL;
	}
	ix->kind = kind;
	ix->filter = filter;
	ix->order = order;
	ix->desc = desc;
	snprintf(ix->value, sizeof(ix->value), "%s", value ? value : "");

	const char *col = filter_column(filter);
	// The filtered lists take a bound value: a track list narrowed to one
	// album/artist/genre, and an album list narrowed to one artist.
	bool bound = col && value &&
				 (kind == LIBRARY_LIST_TRACKS ||
				  (kind == LIBRARY_LIST_ALBUMS &&
				   (filter == LIBRARY_FILTER_ARTIST || filter == LIBRARY_FILTER_ALBUM_ARTIST)));

	char count_sql[512];
	char rows_sql[512];
	char rows_select[64];
	// A second column, for the A-Z buckets counted in this same pass.
	//
	// The sort key where there is one: the query is ordered by it, so the index
	// holds it and the whole pass is answered without ever reading a table row.
	// It also says which letter the row files under, because the byte after the
	// group is the first character folded -- see letter_from_key.
	//
	// The name otherwise. The `character` column the scan writes would be
	// cheaper still, but on a database written by the stock player it holds a
	// pinyin initial, which files a Chinese title under Z while this collation
	// sorts it past Z: the counts and the list would disagree and the strip
	// would land on the wrong rows, with the totals still adding up.
	bool keyed = list_uses_sortkey(kind);
	snprintf(rows_select, sizeof(rows_select), "rowid, %s", keyed ? "sortkey" : name_column(kind));
	list_sql(count_sql, sizeof(count_sql), "COUNT(*)", kind, filter, value, order);
	list_sql(rows_sql, sizeof(rows_sql), rows_select, kind, filter, value, order);
	if (!rows_sql[0]) {
		free(ix);
		return NULL;
	}
	// COUNT(*) has no business sorting: the ORDER BY the builder appended is
	// dead weight on a query that returns one row, and SQLite would still build
	// the sorter for it.
	char *order_by = strstr(count_sql, " ORDER BY ");
	if (order_by) {
		*order_by = '\0';
	}

	pthread_mutex_lock(&db_lock);

	// No database to ask. It happens for real: exporting the card to a computer
	// closes the index (usb.c, storage_export) and library_close() sets the
	// handle to NULL, so every list opened while the card is over there arrives
	// here with nothing behind it. Answering NULL is what the callers already
	// expect from a list that cannot be built -- medialist shows its empty
	// label -- and it also skips the whole prologue below, which has no meaning
	// without a handle.
	if (!db) {
		pthread_mutex_unlock(&db_lock);
		free(ix);
		return NULL;
	}

	ix->domain = domain_of(kind);
	ix->generation = generation[ix->domain];

	// Room to work in, for the length of this one query.
	//
	// A list ordered by the sort key reads an index in order and sorts nothing,
	// so this is mostly page cache for that walk. It still matters for the
	// queries that do sort -- one album by disc position, one artist's records,
	// and every list at all while an upgraded database is still having its keys
	// filled in -- where the whole result goes through a temporary b-tree and
	// what does not fit spills to a scratch file on the card. The budget is the
	// page cache, held at 256 KB for the rest of the player's life. It is given
	// back on the way out.
	exec("PRAGMA cache_size=-2048");
	uint32_t started_ms = now_ms();

	// The count first, so the row ids land in one allocation of the right size
	// rather than a dozen doublings with a copy each. Both queries run under
	// the same lock, so nothing can change between them.
	int expected = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, count_sql, -1, &stmt, NULL) == SQLITE_OK) {
		if (bound) {
			sqlite3_bind_text(stmt, 1, value, -1, SQLITE_TRANSIENT);
		}
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			expected = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}

	if (expected > 0) {
		ix->rows = malloc((size_t)expected * sizeof(*ix->rows));
		if (!ix->rows) {
			pthread_mutex_unlock(&db_lock);
			free(ix);
			return NULL;
		}
	}

	stmt = NULL;
	if (ix->rows && db && sqlite3_prepare_v2(db, rows_sql, -1, &stmt, NULL) == SQLITE_OK) {
		if (bound) {
			sqlite3_bind_text(stmt, 1, value, -1, SQLITE_TRANSIENT);
		}
		while (ix->count < expected && sqlite3_step(stmt) == SQLITE_ROW) {
			ix->rows[ix->count++] = (int32_t)sqlite3_column_int64(stmt, 0);
			const char *second = (const char *)sqlite3_column_text(stmt, 1);
			char letter = keyed ? library_index_letter_of_key(second) : library_index_letter(second ? second : "");
			ix->buckets[library_index_slot(letter)]++;
		}
		sqlite3_finalize(stmt);

		// The counts say where each letter begins, which is only true of a list
		// ordered by that letter. Favourites run by when they were starred, an
		// album by disc position, an artist's records by album -- three lists
		// whose order has nothing to do with the alphabet.
		bool by_name = kind != LIBRARY_LIST_FAVOURITES && kind != LIBRARY_LIST_PLAYLIST &&
					   order != LIBRARY_ORDER_ALBUM &&
					   !(kind == LIBRARY_LIST_TRACKS && filter == LIBRARY_FILTER_ALBUM);
		ix->buckets_valid = by_name && ix->count > 0;
	}

	// Back to what the rest of the player runs on. Whatever the sorter took is
	// released with it.
	exec("PRAGMA cache_size=-256");
	// Null-checked, unlike the calls above it. sqlite3_exec() and
	// sqlite3_prepare_v2() check the handle and answer SQLITE_MISUSE on a
	// closed one; sqlite3_db_release_memory() does not -- its check is behind
	// SQLITE_ENABLE_API_ARMOR, which this build does not define -- so it walks
	// straight into db->mutex and segfaults. The crash handler parks the
	// process instead of dying, so that fault on the interface thread shows up
	// as a hang with db_lock held for ever.
	if (db) {
		sqlite3_db_release_memory(db);
	}

	uint32_t elapsed = now_ms() - started_ms;
	pthread_mutex_unlock(&db_lock);

	// The one line that says whether opening a list is the slow part. It holds
	// the database lock for its whole length, on the interface thread, so it is
	// also how long everything else waited.
	if (elapsed > 250) {
		fprintf(stderr, "library: list of %d rows built in %u ms\n", ix->count, elapsed);
	}

	return ix;
}

int library_index_count(const library_index_t *ix) { return ix ? ix->count : 0; }

bool library_index_stale(const library_index_t *ix) {
	if (!ix) {
		return true;
	}
	pthread_mutex_lock(&db_lock);
	bool stale = ix->generation != generation[ix->domain];
	pthread_mutex_unlock(&db_lock);
	return stale;
}

bool library_index_buckets(const library_index_t *ix, int counts[LIBRARY_INDEX_BUCKETS]) {
	if (!ix || !counts || !ix->buckets_valid) {
		return false;
	}
	memcpy(counts, ix->buckets, sizeof(ix->buckets));
	return true;
}

// A descending list is the same row ids read from the other end. No DESC query
// is ever issued: the collation would have to sort the table a second time to
// answer one.
static int row_slot(const struct library_index *ix, int index) {
	return ix->desc ? ix->count - 1 - index : index;
}

int library_index_window(const library_index_t *ix, int offset, int count, library_row_cb cb, void *user) {
	if (!ix || !cb || offset < 0 || count <= 0 || offset >= ix->count) {
		return 0;
	}
	if (offset + count > ix->count) {
		count = ix->count - offset;
	}

	char sql_buf[PLAYLIST_TABLE_MAX + 96];
	const char *sql = row_by_id_sql(ix->kind, ix->value, sql_buf, sizeof(sql_buf));
	if (!sql) {
		return 0;
	}

	pthread_mutex_lock(&db_lock);

	// A handle from before a rescan or a card change names rows that are now
	// somebody else's. Answering nothing is what makes the caller reload.
	if (ix->generation != generation[ix->domain]) {
		pthread_mutex_unlock(&db_lock);
		return 0;
	}

	int delivered = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		for (int i = 0; i < count; i++) {
			sqlite3_reset(stmt);
			sqlite3_bind_int64(stmt, 1, ix->rows[row_slot(ix, offset + i)]);
			if (sqlite3_step(stmt) != SQLITE_ROW) {
				// The row went while the window was being read. The window is
				// positional, so it gets a blank rather than the next row
				// shifted into its place.
				if (!cb("", NULL, NULL, user)) {
					break;
				}
				delivered++;
				continue;
			}
			const char *name = (const char *)sqlite3_column_text(stmt, 0);
			const char *path = (const char *)sqlite3_column_text(stmt, 1);
			const char *artist = (const char *)sqlite3_column_text(stmt, 2);
			if (!cb(name ? name : "", path, artist, user)) {
				break;
			}
			delivered++;
		}
		sqlite3_finalize(stmt);
	}

	pthread_mutex_unlock(&db_lock);
	return delivered;
}

library_index_t *library_index_clone(const library_index_t *ix) {
	if (!ix) {
		return NULL;
	}
	struct library_index *copy = malloc(sizeof(*copy));
	if (!copy) {
		return NULL;
	}
	*copy = *ix;
	copy->rows = NULL;
	if (ix->count > 0) {
		copy->rows = malloc((size_t)ix->count * sizeof(*copy->rows));
		if (!copy->rows) {
			free(copy);
			return NULL;
		}
		memcpy(copy->rows, ix->rows, (size_t)ix->count * sizeof(*copy->rows));
	}
	return copy;
}

// What the handle was built from, so a queue made out of one can be written
// down as a question rather than as its answer.
void library_index_describe(const library_index_t *ix, library_index_spec_t *out) {
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	if (!ix) {
		return;
	}
	out->kind = ix->kind;
	out->filter = ix->filter;
	out->order = ix->order;
	out->desc = ix->desc;
	snprintf(out->value, sizeof(out->value), "%s", ix->value);
	out->valid = true;
}

int library_index_find_path(const library_index_t *ix, const char *path) {
	if (!ix || !path || !path[0] || ix->kind == LIBRARY_LIST_ALBUMS) {
		return -1;
	}

	// Which table the handle's row ids belong to. A playlist's rows are the
	// playlist's own table: looking the path up in MEDIA_TABLE and comparing
	// that row id against them matches by coincidence, and the queue then jumps
	// to whatever track happens to sit at that number.
	char sql[PLAYLIST_TABLE_MAX + 64];
	if (ix->kind == LIBRARY_LIST_PLAYLIST) {
		char quoted[PLAYLIST_TABLE_MAX];
		if (!playlist_table(ix->value, quoted, sizeof(quoted))) {
			return -1;
		}
		if (snprintf(sql, sizeof(sql), "SELECT rowid FROM %s WHERE path = ? AND present<>0", quoted) >=
			(int)sizeof(sql)) {
			return -1;
		}
	} else {
		const char *table = ix->kind == LIBRARY_LIST_FAVOURITES ? "FAVOURITES" : "MEDIA_TABLE";
		snprintf(sql, sizeof(sql), "SELECT rowid FROM %s WHERE path = ?", table);
	}

	pthread_mutex_lock(&db_lock);
	int32_t rowid = -1;
	sqlite3_stmt *stmt = NULL;
	if (db && ix->generation == generation[ix->domain] && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			rowid = (int32_t)sqlite3_column_int64(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);

	if (rowid < 0) {
		return -1;
	}
	// One indexed lookup for the row id, then a walk of an array of ints: two
	// hundred thousand comparisons is microseconds, where asking the database
	// for each position in turn would be two hundred thousand queries.
	for (int i = 0; i < ix->count; i++) {
		if (ix->rows[i] == rowid) {
			return ix->desc ? ix->count - 1 - i : i;
		}
	}
	return -1;
}

void library_index_close(library_index_t *ix) {
	if (!ix) {
		return;
	}
	free(ix->rows);
	free(ix);
}

// ---------------------------------------------------------------------------
// scanning
// ---------------------------------------------------------------------------

// Free memory, for the scan log. -1 where /proc/meminfo does not exist (the
// simulator on a system without it).
static long mem_available_kb(void) {
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) {
		return -1;
	}
	char line[160];
	long kb = -1;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) {
			break;
		}
	}
	fclose(f);
	return kb;
}

// A growable list of names: what the walk holds while it reads one level (see
// scan_directory).
typedef struct {
	char **names;
	int count;
	int capacity;
} namelist_t;

static void namelist_init(namelist_t *l) {
	l->names = NULL;
	l->count = 0;
	l->capacity = 0;
}

static bool namelist_add(namelist_t *l, const char *name) {
	if (l->count >= SCAN_MAX_SUBDIRS) {
		return false;
	}
	if (l->count == l->capacity) {
		int grown = l->capacity ? l->capacity * 2 : 16;
		if (grown > SCAN_MAX_SUBDIRS) {
			grown = SCAN_MAX_SUBDIRS;
		}
		char **bigger = realloc(l->names, (size_t)grown * sizeof(*bigger));
		if (!bigger) {
			return false; // out of memory: index whatever was collected so far
		}
		l->names = bigger;
		l->capacity = grown;
	}

	char *copy = strdup(name);
	if (!copy) {
		return false;
	}
	l->names[l->count++] = copy;
	return true;
}

static void namelist_free(namelist_t *l) {
	for (int i = 0; i < l->count; i++) {
		free(l->names[i]);
	}
	free(l->names);
	namelist_init(l);
}

// Everything the player can play, with two deliberate exclusions.
//
// This list must stay in step with decode_detect_format() plus WAV (which has
// its own path in audio.c): a format that plays but does not index is an album
// that exists in Explore and not in Albums, Artists, Genres or search -- which
// is exactly what happened to AAC and ALAC, both inside a .m4a.
//
// .m4b is excluded: it is an audiobook and has its own scanner in
// audiobookdb.c, feeding the Audiobooks page. Indexing it here too would put
// ten hours of novel in among the tracks.
//
// .mp4 is excluded as well. The decoder can read it -- same container as a
// .m4a -- but a file named .mp4 is a film, and the exceptions are not worth the
// certainty of finding an episode among the albums. Music in this container is
// named .m4a: that is indexed, and still inspected (see is_video_file) because
// the extension alone proves nothing.
static bool is_playable(const char *name) {
	static const char *const EXTENSIONS[] = {".wav",  ".mp3", ".flac", ".ogg",  ".dsf",  ".dff",
											 ".aif",  ".aiff", ".aifc", ".caf", ".opus", ".wv",
											 ".m4a",  ".alac", ".aac",  ".ape",  ".cue"};

	for (size_t i = 0; i < sizeof(EXTENSIONS) / sizeof(EXTENSIONS[0]); i++) {
		if (has_extension(name, EXTENSIONS[i])) {
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// What a file actually is
//
// The quality badge in the lists needs three things about a track: whether it
// is DSD, whether it is lossy, and -- for the lossless ones -- whether it is CD
// or better than CD. The first two are the container; the third is the sample
// rate and the bit depth, which nothing was writing down.
//
// Opening a decoder to ask costs an order of magnitude more per file than
// reading the tags, which is minutes on a large library for two integers. So
// the header is read directly instead -- a few bytes at a known place -- and
// only for the containers where the answer is not already settled by the
// extension:
//
//   FLAC  STREAMINFO, the first metadata block, at a fixed offset
//   WAV   the `fmt ` chunk, found by walking the chunk list
//   DSF   the `fmt ` chunk of a DSD stream, at a fixed offset
//   MP4   through the parser the player already has (rate, and ALAC's depth)
//   APE   the Monkey's Audio header, through apedec_probe()
//
// Anything else keeps zeros, and a row with zeros gets no badge: a wrong badge
// is worse than none. That is mp3, ogg, opus, raw aac -- all lossy, so the
// badge is decided by the container anyway -- and WavPack, which is the one
// lossless format left without an answer.
// ---------------------------------------------------------------------------


static uint32_t le32(const unsigned char *p) {
	return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

// STREAMINFO packs the rate in 20 bits and the depth in 5, straddling bytes.
static void probe_flac(FILE *f, int *rate, int *bits) {
	unsigned char head[4];
	if (fread(head, 1, 4, f) != 4) {
		return;
	}
	// An ID3 tag before the stream is legal and common: skip it and look again.
	if (head[0] == 'I' && head[1] == 'D' && head[2] == '3') {
		unsigned char rest[6];
		if (fread(rest, 1, 6, f) != 6) {
			return;
		}
		long skip = ((long)(rest[2] & 0x7f) << 21) | ((long)(rest[3] & 0x7f) << 14) | ((long)(rest[4] & 0x7f) << 7) |
					(rest[5] & 0x7f);
		if (fseek(f, 10 + skip, SEEK_SET) != 0 || fread(head, 1, 4, f) != 4) {
			return;
		}
	}
	if (memcmp(head, "fLaC", 4) != 0) {
		return;
	}

	unsigned char block[38]; // 4 bytes of block header, then 34 of STREAMINFO
	if (fread(block, 1, sizeof(block), f) != sizeof(block)) {
		return;
	}
	if ((block[0] & 0x7f) != 0) {
		return; // the first block is meant to be STREAMINFO
	}
	const unsigned char *si = block + 4;
	*rate = (int)(((uint32_t)si[10] << 12) | ((uint32_t)si[11] << 4) | (uint32_t)(si[12] >> 4));
	*bits = (int)((((si[12] & 0x01) << 4) | (si[13] >> 4)) + 1);
}

static void probe_wav(FILE *f, int *rate, int *bits) {
	unsigned char head[12];
	if (fread(head, 1, sizeof(head), f) != sizeof(head) || memcmp(head, "RIFF", 4) != 0 ||
		memcmp(head + 8, "WAVE", 4) != 0) {
		return;
	}
	// The chunks, until `fmt `. Bounded: a file that is not one would otherwise
	// be walked to its end.
	for (int i = 0; i < 32; i++) {
		unsigned char chunk[8];
		if (fread(chunk, 1, sizeof(chunk), f) != sizeof(chunk)) {
			return;
		}
		uint32_t size = le32(chunk + 4);
		if (memcmp(chunk, "fmt ", 4) == 0) {
			unsigned char fmt[16];
			if (size < sizeof(fmt) || fread(fmt, 1, sizeof(fmt), f) != sizeof(fmt)) {
				return;
			}
			*rate = (int)le32(fmt + 4);
			*bits = (int)(fmt[14] | ((uint32_t)fmt[15] << 8));
			return;
		}
		if (fseek(f, (long)size + (size & 1), SEEK_CUR) != 0) {
			return;
		}
	}
}

// DSF: a 28-byte "DSD " chunk, then a `fmt ` chunk carrying the rate 28 bytes
// in. The same offsets parse_dsf() in the decoder reads.
static void probe_dsf(FILE *f, int *rate, int *bits) {
	unsigned char head[80]; // 28 of DSD chunk, then the head of the fmt chunk
	if (fread(head, 1, sizeof(head), f) != sizeof(head) || memcmp(head, "DSD ", 4) != 0 ||
		memcmp(head + 28, "fmt ", 4) != 0) {
		return;
	}
	*rate = (int)le32(head + 28 + 28);
	*bits = 1; // one bit per sample is what DSD is
}

// Fills the sample rate and bit depth of a file, or leaves them at zero when
// they cannot be had cheaply. `alac` says the file is lossless MP4, which the
// extension cannot say: a .m4a is AAC or ALAC and only the sample entry knows
// which.
static void probe_stream_format(const char *path, const char *filename, int *rate, int *bits, bool *alac) {
	*rate = 0;
	*bits = 0;
	*alac = false;

	bool flac = has_extension(filename, ".flac");
	bool wav = has_extension(filename, ".wav");
	bool dsf = has_extension(filename, ".dsf");

	if (has_extension(filename, ".ape")) {
		if (!apedec_probe(path, rate, bits)) {
			*rate = 0;
			*bits = 0;
		}
		return;
	}
	bool mp4 = has_extension(filename, ".m4a") || has_extension(filename, ".alac") || has_extension(filename, ".mp4");

	if (mp4) {
		// The player's own parser: it is already opening these files to find out
		// whether they are video, and it knows the rate.
		mp4_file_t *m = mp4_open(path);
		if (m) {
			*rate = mp4_audio_sample_rate(m);
			if (mp4_audio_codec(m) == MP4_CODEC_ALAC) {
				*alac = true;
				// ALACSpecificConfig: frameLength(4), compatibleVersion(1),
				// then the depth. AAC has no depth to read -- it is lossy and
				// the number would describe the decoder, not the recording.
				int len = 0;
				const unsigned char *cfg = mp4_audio_config(m, &len);
				if (cfg && len >= 6) {
					*bits = cfg[5];
				}
			}
			mp4_close(m);
		}
		return;
	}
	if (!flac && !wav && !dsf) {
		return;
	}

	FILE *f = fopen(path, "rb");
	if (!f) {
		return;
	}
	if (flac) {
		probe_flac(f, rate, bits);
	} else if (wav) {
		probe_wav(f, rate, bits);
	} else {
		probe_dsf(f, rate, bits);
	}
	fclose(f);
}

// Which format code the row records, matching the stock numbering where it has
// one. The column is informational: nothing outside this file reads it.
static int format_of(const char *name) {
	if (has_extension(name, ".flac")) {
		return FORMAT_FLAC;
	}
	if (has_extension(name, ".mp3")) {
		return FORMAT_MP3;
	}
	if (has_extension(name, ".ogg")) {
		return FORMAT_OGG;
	}
	if (has_extension(name, ".dsf") || has_extension(name, ".dff")) {
		return FORMAT_DSD;
	}
	// Codes the stock numbering never had. Without them half the library is
	// reported as WAV.
	if (has_extension(name, ".m4a") || has_extension(name, ".alac")) {
		return FORMAT_AAC; // overridden to FORMAT_ALAC when the container says so
	}
	if (has_extension(name, ".opus")) {
		return FORMAT_OPUS;
	}
	if (has_extension(name, ".wv")) {
		return FORMAT_WAVPACK;
	}
	if (has_extension(name, ".ape")) {
		return FORMAT_APE;
	}
	return FORMAT_WAV;
}

static void set_scan_folder(const char *path) {
	pthread_mutex_lock(&scan_folder_lock);
	snprintf(scan_folder, sizeof(scan_folder), "%s", path);
	pthread_mutex_unlock(&scan_folder_lock);
}

// The statements a scan runs over and over. Prepared once when the scan starts
// and reused for every row: re-parsing the same INSERT for each of ten thousand
// tracks costs both time and, because each parse allocates, memory churn.
static sqlite3_stmt *stmt_track;
static sqlite3_stmt *stmt_album;
static sqlite3_stmt *stmt_artist;
static sqlite3_stmt *stmt_genre;
static sqlite3_stmt *stmt_album_artist;

static void finalize_statements(void) {
	sqlite3_finalize(stmt_track);
	sqlite3_finalize(stmt_album);
	sqlite3_finalize(stmt_artist);
	sqlite3_finalize(stmt_genre);
	sqlite3_finalize(stmt_album_artist);
	stmt_track = stmt_album = stmt_artist = stmt_genre = stmt_album_artist = NULL;
}

static bool prepare_statements(void) {
	finalize_statements();

	static const char *const LOOKUP_SQL[] = {
		"INSERT OR IGNORE INTO ALBUM_TABLE(id,album,character,cn,sortkey) VALUES(0,?,?,?,?)",
		"INSERT OR IGNORE INTO ARTIST_TABLE(id,artist,character,cn,sortkey) VALUES(0,?,?,?,?)",
		"INSERT OR IGNORE INTO GENRE_TABLE(id,genre,character,cn,sortkey) VALUES(0,?,?,?,?)",
	};

	sqlite3_stmt **targets[] = {&stmt_album, &stmt_artist, &stmt_genre};
	for (int i = 0; i < 3; i++) {
		if (sqlite3_prepare_v2(db, LOOKUP_SQL[i], -1, targets[i], NULL) != SQLITE_OK) {
			fprintf(stderr, "library: prepare failed: %s\n", sqlite3_errmsg(db));
			return false;
		}
	}

	if (sqlite3_prepare_v2(db,
						   "INSERT OR IGNORE INTO ALBUM_ARTIST_TABLE"
						   "(id,album_artist,character,cn,ctime,mtime,mqa,pinyin_charater,sortkey)"
						   " VALUES(0,?,?,?,0,0,0,'',?)",
						   -1, &stmt_album_artist, NULL) != SQLITE_OK) {
		return false;
	}

	if (sqlite3_prepare_v2(db,
						   "INSERT OR REPLACE INTO MEDIA_TABLE"
						   "(id,path,name,album,artist,genre,year,dis_id,ck_id,has_child_file,begin_time,end_time,"
						   "cue_id,character,size,sample_rate,bit_rate,bit,channel,format,quality,album_pic_path,"
						   "lrc_path,track_gain,track_peak,album_artist,ctime,mtime,sortkey,disc)"
						   " VALUES(?,?,?,?,?,?,?,?,0,0,0,0,0,?,?,?,0,?,0,?,\'\',NULL,NULL,0,0,?,?,?,?,?)",
						   -1, &stmt_track, NULL) != SQLITE_OK) {
		fprintf(stderr, "library: prepare failed: %s\n", sqlite3_errmsg(db));
		return false;
	}

	return true;
}

// Adds a name to one of the lookup tables, if it isn't there already.
static void run_lookup(sqlite3_stmt *stmt, const char *value) {
	if (!stmt || !value || !value[0]) {
		return;
	}

	char character[16];
	int group = 0;
	index_character(value, character, sizeof(character), &group);

	char key[LIBRARY_SORT_KEY_MAX];
	library_sort_key(value, key, sizeof(key));

	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_bind_text(stmt, 1, value, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, character, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 3, group);
	sqlite3_bind_text(stmt, 4, key, -1, SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_reset(stmt);
}



static void insert_track(const char *path, const char *filename, const song_metadata_t *tags, const struct stat *st) {
	if (!stmt_track) {
		return;
	}

	// A file with no title tag is filed under its own name, minus the
	// extension: an untitled track still has to be findable.
	char title[256];
	if (tags->title[0]) {
		snprintf(title, sizeof(title), "%s", tags->title);
	} else {
		snprintf(title, sizeof(title), "%s", filename);
		char *dot = strrchr(title, '.');
		if (dot) {
			*dot = '\0';
		}
	}

	const char *album_artist = tags->album_artist[0] ? tags->album_artist : tags->artist;

	char character[16];
	int group = 0;
	index_character(title, character, sizeof(character), &group);

	sqlite3_reset(stmt_track);
	sqlite3_clear_bindings(stmt_track);

	int column = 1;
	sqlite3_bind_int(stmt_track, column++, scan_found + 1);						 // id
	sqlite3_bind_text(stmt_track, column++, path, -1, SQLITE_TRANSIENT);		 // path
	sqlite3_bind_text(stmt_track, column++, title, -1, SQLITE_TRANSIENT);		 // name
	sqlite3_bind_text(stmt_track, column++, tags->album, -1, SQLITE_TRANSIENT);	 // album
	sqlite3_bind_text(stmt_track, column++, tags->artist, -1, SQLITE_TRANSIENT); // artist
	sqlite3_bind_text(stmt_track, column++, tags->genre, -1, SQLITE_TRANSIENT);	 // genre
	sqlite3_bind_int(stmt_track, column++, tags->year);							 // year
	sqlite3_bind_int(stmt_track, column++, tags->track_number);					 // dis_id: track number
	sqlite3_bind_text(stmt_track, column++, character, -1, SQLITE_TRANSIENT);	 // character
	sqlite3_bind_int64(stmt_track, column++, (sqlite3_int64)st->st_size);		 // size

	// The two the quality badge is worked out from. Zero when the container
	// does not give them up cheaply -- see probe_stream_format().
	int rate = 0, bits = 0;
	bool alac = false;
	probe_stream_format(path, filename, &rate, &bits, &alac);
	sqlite3_bind_int(stmt_track, column++, rate);									 // sample_rate
	sqlite3_bind_int(stmt_track, column++, bits);									 // bit
	sqlite3_bind_int(stmt_track, column++, alac ? FORMAT_ALAC : format_of(filename)); // format
	sqlite3_bind_text(stmt_track, column++, album_artist, -1, SQLITE_TRANSIENT); // album_artist
	sqlite3_bind_int64(stmt_track, column++, (sqlite3_int64)st->st_ctime);		 // ctime
	sqlite3_bind_int64(stmt_track, column++, (sqlite3_int64)st->st_mtime);		 // mtime

	char key[LIBRARY_SORT_KEY_MAX];
	library_sort_key(title, key, sizeof(key));
	sqlite3_bind_text(stmt_track, column++, key, -1, SQLITE_TRANSIENT); // sortkey

	// A file with no disc tag is disc one. A record where the tagger wrote the
	// number on some files and not on others is the common case, and writing 0
	// for those would put them on a shelf of their own, ahead of the disc they
	// belong to.
	sqlite3_bind_int(stmt_track, column++, tags->disc_number > 0 ? tags->disc_number : 1); // disc

	if (sqlite3_step(stmt_track) != SQLITE_DONE) {
		fprintf(stderr, "library: insert failed: %s\n", sqlite3_errmsg(db));
	}
	sqlite3_reset(stmt_track);

	run_lookup(stmt_album, tags->album);
	run_lookup(stmt_artist, tags->artist);
	run_lookup(stmt_genre, tags->genre);
	run_lookup(stmt_album_artist, album_artist);
}

// Keeps video out of the music library.
//
// .mp4 never reaches here (it is not in the list above), but a .m4a is the same
// container: renaming a film would have put it in Albums, Artists and Genres as
// a track, with its audio stream playable. Only what is inside the container
// can decide.
//
// Costs one container open -- a few reads, no decoding -- and only for this
// format.
static bool is_video_file(const char *path, const char *name) {
	if (!has_extension(name, ".m4a") && !has_extension(name, ".alac")) {
		return false;
	}
	// An .alac in a CAF is not an MP4 and answers "no video" here, which is
	// right: mp4_file_has_video() opens the container, and on something that is
	// not a readable MP4 there is nothing to keep out.
	return mp4_file_has_video(path);
}

// Indexes one file: read the tags, write the row, commit every so often. Split
// out of the walk, which has two phases with this sitting between them.
static void scan_indexed(const char *path, const char *name, const song_metadata_t *tags, const struct stat *st) {
	pthread_mutex_lock(&db_lock);
	if (db) {
		insert_track(path, name, tags, st);
		scan_found++;

		if (scan_found % SCAN_COMMIT_EVERY == 0) {
			bool ok = exec("COMMIT");
			// Give the pages back rather than letting the cache grow to
			// its limit and stay there for the rest of the scan.
			if (db) {
				sqlite3_db_release_memory(db);
			}
			ok = exec("BEGIN") && ok;
			// A card that has stopped taking writes -- the usual reason a
			// commit fails -- will not start again on the next thousand
			// files, and every one of those would be read for nothing.
			if (!ok) {
				// Whatever the commit did, the transaction must not be left
				// open: the next unrelated COMMIT anywhere would otherwise
				// write out half an abandoned scan.
				exec("ROLLBACK");
				fprintf(stderr, "library: database write failed, stopping the scan\n");
				scan_db_failed = true;
				scan_cancel = true;
			}
		}

		if (scan_found % SCAN_LOG_EVERY == 0) {
			long free_kb = mem_available_kb();
			printf("library: %d tracks, %s (%ld kB free)\n", scan_found, path, free_kb);
			fflush(stdout);
		}
	}
	pthread_mutex_unlock(&db_lock);
}

// The audio files claimed by the CUE sheets of the directory being walked.
//
// A disc must appear once. When a sheet claims a file, that file itself is not
// a track: it is the container the sheet cuts up. The list is reset for each
// directory and only consulted inside it, which is safe because the walk
// finishes a directory's files before descending (see scan_directory).
#define CUE_CLAIMED_MAX 16
static char cue_claimed[CUE_CLAIMED_MAX][512];
static int cue_claimed_count;

static void cue_claim(const char *audio_path) {
	if (cue_claimed_count >= CUE_CLAIMED_MAX) {
		return;
	}
	snprintf(cue_claimed[cue_claimed_count], sizeof(cue_claimed[0]), "%s", audio_path);
	cue_claimed_count++;

	// The sheet may have been read after the file it claims was already
	// indexed, so the row is removed as well as skipped later.
	pthread_mutex_lock(&db_lock);
	if (db) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db, "DELETE FROM MEDIA_TABLE WHERE path = ?1", -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, audio_path, -1, SQLITE_STATIC);
			sqlite3_step(stmt);
			sqlite3_finalize(stmt);
			// The counter on the progress page must lose the row too.
			scan_found -= sqlite3_changes(db);
			bump_generation(GEN_MEDIA);
		}
	}
	pthread_mutex_unlock(&db_lock);
}

static bool cue_claims(const char *path) {
	for (int i = 0; i < cue_claimed_count; i++) {
		if (strcmp(cue_claimed[i], path) == 0) {
			return true;
		}
	}
	return false;
}

// A whole disc in one file, split by a CUE sheet.
//
// Each TRACK becomes its own row, pointing at a virtual path -- the sheet plus
// a track number -- that the decoder turns back into a window onto the audio
// file. Without this the file is one row and one hour-long track, which is what
// it looked like before.
//
// Returns false when the sheet is unusable, and the caller then treats it as
// an ordinary file it cannot index.
static bool scan_cue_sheet(const char *path, const struct stat *st) {
	// A sheet is thirty-five kilobytes. This runs several frames deep in a
	// directory walk, on the scan thread, so it goes on the heap.
	cue_sheet_t *cue = malloc(sizeof(*cue));
	if (!cue) {
		return false;
	}
	if (!cue_parse(path, cue)) {
		free(cue);
		return false;
	}

	for (int i = 0; i < cue->track_count; i++) {
		const cue_track_t *t = &cue->tracks[i];

		char vpath[600];
		cue_virtual_path(path, t->number, vpath, sizeof(vpath));

		song_metadata_t tags;
		memset(&tags, 0, sizeof(tags));
		snprintf(tags.title, sizeof(tags.title), "%s", t->title);
		snprintf(tags.artist, sizeof(tags.artist), "%s", t->performer[0] ? t->performer : cue->performer);
		snprintf(tags.album, sizeof(tags.album), "%s", cue->album);
		snprintf(tags.album_artist, sizeof(tags.album_artist), "%s", cue->performer);
		snprintf(tags.genre, sizeof(tags.genre), "%s", cue->genre);
		tags.year = cue->year;
		tags.track_number = t->number;
		tags.has_tags = tags.title[0] || tags.artist[0] || tags.album[0];

		// The name shown when the sheet gave no title: better a track number
		// than the sheet's file name repeated for every entry.
		char shown[64];
		if (!tags.title[0]) {
			snprintf(shown, sizeof(shown), "%02d", t->number);
			snprintf(tags.title, sizeof(tags.title), "%s", shown);
		}

		scan_indexed(vpath, tags.title, &tags, st);
	}

	if (cue->track_count > 0) {
		cue_claim(cue->audio_path);
	}
	printf("library: %s holds %d tracks\n", path, cue->track_count);
	bool indexed = cue->track_count > 0;
	free(cue);
	return indexed;
}

static void scan_one_file(const char *child, const char *name, const struct stat *st) {
	// Before the file is touched, not after: what is wanted is the name of the
	// file the player was on when it died, and by then no code here runs.
	crumb_set(child);
	if (scan_log_files) {
		printf("library: reading %s\n", child);
		fflush(stdout);
	}

	if (cue_is_sheet(name)) {
		scan_cue_sheet(child, st);
		return;
	}
	if (cue_claims(child)) {
		return; // a sheet in this folder already indexed it, track by track
	}

	// A WAV can hold its own track list, in the RIFF markers a vinyl rip
	// leaves behind. It is read the same way a sheet is, with the file
	// standing in for the sheet; when there are no markers this returns false
	// and the file is indexed whole. A .cue beside it wins: it was checked
	// above and has already claimed the audio.
	if (cue_wav_has_markers(name) && scan_cue_sheet(child, st)) {
		return;
	}

	song_metadata_t tags;
	metadata_read(child, &tags);
	scan_indexed(child, name, &tags, st);
}

// One directory open at a time.
//
// An open DIR is not just a pointer: the C library allocates it a read buffer,
// 32 KB here. Recursing from inside the readdir() loop would hold one of those,
// and one file descriptor, per level of nesting -- over four hundred kilobytes
// at the twelve-level limit.
//
// So the walk is in two passes: read the directory to the end (indexing files
// as they come, which costs no memory) keeping only the names of the
// subdirectories, close it, and only then descend. One directory is open at any
// depth, and what is carried down is names, a few dozen bytes each.
static void scan_directory(const char *path, int depth) {
	if (scan_cancel || depth > SCAN_MAX_DEPTH) {
		return;
	}

	DIR *dir = opendir(path);
	if (!dir) {
		return;
	}

	set_scan_folder(path);
	cue_claimed_count = 0; // the claims below belong to this directory only

	namelist_t subdirs;
	namelist_init(&subdirs);

	// WAVs are held back to a second pass. A WAV can carry its own track list
	// in its markers, and a .cue in the same folder overrules it -- but
	// readdir returns names in the order the filesystem kept them, and a
	// ripper writes the audio before the sheet, so the sheet is usually the
	// later of the two. Deciding while the directory is still being read would
	// index the disc twice.
	namelist_t wavs;
	namelist_init(&wavs);

	struct dirent *de;
	while (!scan_cancel && (de = readdir(dir)) != NULL) {
		// ".", "..", the player's own hidden folders -- and the folders a
		// desktop leaves behind, which do not begin with a dot.
		if (de->d_name[0] == '.' || playlist_is_junk_name(de->d_name)) {
			continue;
		}

		char child[512];
		if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >= (int)sizeof(child)) {
			continue; // path too long to be useful
		}

		// lstat() rather than stat(): a symlink pointing at an ancestor
		// directory would re-read the same branch at every level down to the
		// twelfth -- not an infinite loop, but an explosion of work and
		// duplicate rows. Symlinks to files are followed, which is harmless:
		// the stat() below resolves them.
		struct stat st;
		if (lstat(child, &st) != 0) {
			continue;
		}
		if (S_ISLNK(st.st_mode)) {
			if (stat(child, &st) != 0 || !S_ISREG(st.st_mode)) {
				continue; // broken, or points at a directory: leave it alone
			}
		}

		if (S_ISDIR(st.st_mode)) {
			if (!namelist_add(&subdirs, de->d_name) && subdirs.count == SCAN_MAX_SUBDIRS) {
				fprintf(stderr, "library: %s has more than %d subfolders; the rest are skipped\n", path,
						SCAN_MAX_SUBDIRS);
			}
			continue;
		}

		if (!S_ISREG(st.st_mode) || !is_playable(de->d_name)) {
			continue;
		}

		if (is_video_file(child, de->d_name)) {
			printf("library: %s is a video; it does not go into the music library\n", child);
			continue;
		}

		// Held back unless the list is full, in which case indexing it now is
		// better than dropping it.
		if (cue_wav_has_markers(de->d_name) && namelist_add(&wavs, de->d_name)) {
			continue;
		}

		scan_one_file(child, de->d_name, &st);
	}

	// The 32 KB buffer goes back to the system before descending a level.
	closedir(dir);

	// Every sheet in this directory has claimed its audio by now.
	for (int i = 0; i < wavs.count && !scan_cancel; i++) {
		char child[512];
		if (snprintf(child, sizeof(child), "%s/%s", path, wavs.names[i]) >= (int)sizeof(child)) {
			continue;
		}
		struct stat st;
		if (stat(child, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		scan_one_file(child, wavs.names[i], &st);
	}
	namelist_free(&wavs);

	for (int i = 0; i < subdirs.count && !scan_cancel; i++) {
		char child[512];
		if (snprintf(child, sizeof(child), "%s/%s", path, subdirs.names[i]) >= (int)sizeof(child)) {
			continue;
		}
		scan_directory(child, depth + 1);
		set_scan_folder(path); // back to this level for the progress display
	}

	namelist_free(&subdirs);
}

static void *scan_thread_func(void *arg) {
	(void)arg;

	// One core: the scan reads thousands of files; at normal priority it
	// would starve the interface for its whole duration.
	thread_be_background("library scan");

	printf("library: scanning %s\n", scan_root);

	pthread_mutex_lock(&db_lock);
	exec("DELETE FROM MEDIA_TABLE");
	exec("DELETE FROM ALBUM_TABLE");
	exec("DELETE FROM ARTIST_TABLE");
	exec("DELETE FROM ALBUM_ARTIST_TABLE");
	exec("DELETE FROM GENRE_TABLE");
	// Emptying the tables restarts the row ids at one, so anything holding
	// them is now pointing at other people's tracks.
	bump_generation(GEN_MEDIA);
	// Without the statements every insert is a silent no-op, and the scan
	// would walk the whole card to produce an empty library.
	if (!prepare_statements() || !exec("BEGIN")) {
		fprintf(stderr, "library: cannot start the scan, the database is not writable\n");
		scan_db_failed = true;
		scan_cancel = true;
	}
	pthread_mutex_unlock(&db_lock);

	if (!scan_cancel) {
		scan_directory(scan_root, 0);
	}

	pthread_mutex_lock(&db_lock);
	if (scan_db_failed) {
		exec("ROLLBACK"); // no-op when the BEGIN itself never took
	} else {
		exec("COMMIT");
	}
	finalize_statements();

	// One row so the count is readable without walking the table.
	exec("DELETE FROM COUNT_TABLE");
	char sql[128];
	snprintf(sql, sizeof(sql), "INSERT INTO COUNT_TABLE(cn) VALUES(%d)", scan_found);
	exec(sql);

	// Nothing is going to read the index for a while; give the cache back
	// rather than sitting on it until the player is killed for it.
	if (db) {
		sqlite3_db_release_memory(db);
	}
	pthread_mutex_unlock(&db_lock);

	pthread_mutex_lock(&db_lock);
	bump_generation(GEN_MEDIA); // and again at the end, so a list opened mid-scan reloads
	pthread_mutex_unlock(&db_lock);

	// Every row a scan writes carries its sort key, and a scan starts by
	// emptying the tables, so after one there is nothing left to fill in --
	// whatever the database looked like before.
	if (!scan_db_failed) {
		build_sort_indexes(); // no-op unless the upgrade never got to them
		sortkeys_ready = true;
	}

	printf("library: scan finished, %d tracks%s\n", scan_found,
		   scan_db_failed ? " (database error)" : scan_cancel ? " (stopped early)" : "");

	set_scan_folder("");
	crumb_set(NULL); // the scan is over: a later crash must not blame its last file
	scan_running = false;
	return NULL;
}

bool library_scan_start(const char *root) {
	if (!db || scan_running || !root || !root[0]) {
		return false;
	}

	snprintf(scan_root, sizeof(scan_root), "%s", root);
	scan_found = 0;
	scan_cancel = false;
	scan_db_failed = false;
	scan_running = true;
	set_scan_folder(root);

	// Off by default: a line per file is a write to the card per file, which
	// slows the scan and wears the card. It is turned on to catch a death that
	// leaves nothing behind -- a process killed from outside runs no handler,
	// so the last line written is the only evidence of which file it was on.
	scan_log_files = library_log_database();
	if (scan_log_files) {
		printf("library: naming every file (Developer options > Database log)\n");
		fflush(stdout);
	}

	if (pthread_create(&scan_thread, NULL, scan_thread_func, NULL) != 0) {
		scan_running = false;
		fprintf(stderr, "library: could not start the scan thread\n");
		return false;
	}

	pthread_detach(scan_thread);
	return true;
}

bool library_log_database(void) { return config_get_bool("library", "log_database", false); }

void library_set_log_database(bool enabled) {
	config_set_bool("library", "log_database", enabled);
	config_save();
	// A scan already under way starts naming files at once, rather than at the
	// next one: the switch is thrown precisely while trying to catch a crash,
	// and asking for a restart of the very scan that crashes is asking a lot.
	scan_log_files = enabled;
}

// Whether there is an index to ask at all. False while the card is exported to
// a computer, while it is out, and before the first open.
bool library_is_open(void) {
	pthread_mutex_lock(&db_lock);
	bool open = db != NULL;
	pthread_mutex_unlock(&db_lock);
	return open;
}

bool library_scan_running(void) { return scan_running; }

int library_scan_found(void) { return scan_found; }

void library_scan_current_folder(char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	pthread_mutex_lock(&scan_folder_lock);
	snprintf(out, size, "%s", scan_folder);
	pthread_mutex_unlock(&scan_folder_lock);
}

void library_scan_stop(void) {
	if (!scan_running) {
		return;
	}
	scan_cancel = true;
}

// The same, but it does not return until the scan thread has actually let go.
// The thread tests the flag once per directory entry, so this is a handful of
// milliseconds in practice; the ceiling is there so a wedged card reader can
// never hold the interface hostage.
static void library_scan_stop_and_wait(void) {
	if (!scan_running) {
		return;
	}
	scan_cancel = true;

	for (int i = 0; i < 400 && scan_running; i++) {
		usleep(10 * 1000);
	}
	if (scan_running) {
		fprintf(stderr, "library: the scan did not stop in time; closing anyway\n");
	}
}

// ---------------------------------------------------------------------------
// Playback state: the single remembered track + position ("Ricorda traccia").
// ---------------------------------------------------------------------------

void library_queue_save_query(const library_index_spec_t *spec, int position, const char *const *extra,
							  const int *extra_slots, int extra_count) {
	pthread_mutex_lock(&db_lock);
	if (db) {
		// One transaction: this runs on every track change, and four separate
		// statements is four journal round trips to a card. A savepoint and not
		// BEGIN, because a scan holds a transaction open across the whole card
		// and a nested BEGIN would fail -- silently ending the scan's batch on
		// the COMMIT below.
		exec("SAVEPOINT queue_save");
		// The two ways of writing a queue down are exclusive: whichever is
		// stored last is the one the next boot restores, so the other has to go.
		exec("DELETE FROM PLAYBACK_QUEUE");
		exec("DELETE FROM PLAYBACK_QUEUE_STATE");
		exec("DELETE FROM PLAYBACK_QUEUE_QUERY");

		sqlite3_stmt *stmt = NULL;
		if (spec && spec->valid &&
			sqlite3_prepare_v2(db,
							   "INSERT INTO PLAYBACK_QUEUE_QUERY(id,kind,filter,ord,desc,value,pos,updated_at)"
							   " VALUES(0,?,?,?,?,?,?,strftime('%s','now'))",
							   -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_int(stmt, 1, (int)spec->kind);
			sqlite3_bind_int(stmt, 2, (int)spec->filter);
			sqlite3_bind_int(stmt, 3, (int)spec->order);
			sqlite3_bind_int(stmt, 4, spec->desc ? 1 : 0);
			sqlite3_bind_text(stmt, 5, spec->value, -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(stmt, 6, position);
			sqlite3_step(stmt);
			sqlite3_finalize(stmt);
		}

		// Whatever "add to queue" put there after the fact. A handful of rows
		// beside the query, rather than the two hundred thousand the query
		// stands for.
		stmt = NULL;
		if (extra && extra_count > 0 &&
			sqlite3_prepare_v2(db, "INSERT INTO PLAYBACK_QUEUE(idx,path,slot) VALUES(?,?,?)", -1, &stmt, NULL) ==
				SQLITE_OK) {
			for (int i = 0; i < extra_count; i++) {
				if (!extra[i] || !extra[i][0]) {
					continue;
				}
				sqlite3_reset(stmt);
				sqlite3_bind_int(stmt, 1, i);
				sqlite3_bind_text(stmt, 2, extra[i], -1, SQLITE_TRANSIENT);
				// Where it actually sits, so the restore puts it back there
				// instead of in front of playback.
				sqlite3_bind_int(stmt, 3, extra_slots ? extra_slots[i] : -1);
				sqlite3_step(stmt);
			}
			sqlite3_finalize(stmt);
		}
		exec("RELEASE queue_save");
	}
	pthread_mutex_unlock(&db_lock);
}

bool library_queue_load_query(library_index_spec_t *spec, int *position_out) {
	if (!spec) {
		return false;
	}
	memset(spec, 0, sizeof(*spec));

	bool found = false;
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT kind, filter, ord, desc, value, pos FROM PLAYBACK_QUEUE_QUERY WHERE id=0",
								 -1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			spec->kind = (library_list_t)sqlite3_column_int(stmt, 0);
			spec->filter = (library_filter_t)sqlite3_column_int(stmt, 1);
			spec->order = (library_order_t)sqlite3_column_int(stmt, 2);
			spec->desc = sqlite3_column_int(stmt, 3) != 0;
			const char *value = (const char *)sqlite3_column_text(stmt, 4);
			snprintf(spec->value, sizeof(spec->value), "%s", value ? value : "");
			if (position_out) {
				*position_out = sqlite3_column_int(stmt, 5);
			}
			spec->valid = true;
			found = true;
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return found;
}

// ---------------------------------------------------------------------------
// Playlist counts, and what makes one worth re-counting
//
// The number under a playlist's name is how many of its entries are actually on
// the card, and finding it out is one path lookup per entry -- several thousand
// for a page of full playlists, which on a microSD is seconds of the only core
// there is. So it is cached, keyed by the playlist file's own path, size and
// modification time: edit the playlist and the row stops matching.
//
// The stamp alone is not enough on its own, because deleting a track does not
// touch the .m3u that names it. What settles it is that the player cannot
// delete a track from the card: files go missing when the card is taken to a
// computer, and the card then comes back mounted again -- which is what this
// counter tracks. A row therefore needs re-checking when its stamp does not
// match, or when it was checked against an earlier mount than the one in the
// reader now.
//
// The counter lives on the card and not in a variable, because a variable
// starting from zero on every boot would let the second boot's first mount
// match rows written by the first boot's -- verified against a card that has
// been out of the device since.
// ---------------------------------------------------------------------------

static long long mount_serial;

static void next_mount_serial(void) {
	long long serial = 0;
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT serial FROM MOUNT_SERIAL WHERE id=0", -1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			serial = sqlite3_column_int64(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	serial++;
	stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO MOUNT_SERIAL(id,serial) VALUES(0,?)", -1, &stmt, NULL) ==
				  SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, serial);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	mount_serial = serial;
	pthread_mutex_unlock(&db_lock);
}


// The statement a playlist's table is made with. `idx` orders the rows and is
// what the user sees as the order of the list.
static bool playlist_create_locked(const char *quoted) {
	char sql[PLAYLIST_TABLE_MAX + 200];
	snprintf(sql, sizeof(sql),
			 "CREATE TABLE IF NOT EXISTS %s(idx INTEGER PRIMARY KEY, path TEXT, title TEXT, artist TEXT,"
			 " seconds INT, present INT, mount INT)",
			 quoted);
	return exec(sql);
}

static bool playlist_table_exists_locked(const char *name) {
	char table[PLAYLIST_TABLE_MAX];
	if (!db) {
		return false;
	}
	// By value, not by name in the statement: this one can be bound.
	snprintf(table, sizeof(table), LIBRARY_PLAYLIST_PREFIX "%s", name);
	sqlite3_stmt *stmt = NULL;
	bool found = false;
	if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1, &stmt, NULL) ==
		SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, table, -1, SQLITE_TRANSIENT);
		found = sqlite3_step(stmt) == SQLITE_ROW;
		sqlite3_finalize(stmt);
	}
	return found;
}

bool library_playlist_exists(const char *name) {
	if (!name || !name[0]) {
		return false;
	}
	pthread_mutex_lock(&db_lock);
	bool found = playlist_table_exists_locked(name);
	pthread_mutex_unlock(&db_lock);
	return found;
}

int library_playlist_names(char ***names_out) {
	if (names_out) {
		*names_out = NULL;
	}
	char **names = NULL;
	int count = 0, capacity = 0;

	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	// The underscore in the prefix is a wildcard to LIKE, hence the escape:
	// without it "M3U_" would also match "M3UX...".
	if (db && sqlite3_prepare_v2(db,
								 "SELECT name FROM sqlite_master WHERE type='table'"
								 " AND name LIKE 'M3U\\_%' ESCAPE '\\' ORDER BY name COLLATE listorder",
								 -1, &stmt, NULL) == SQLITE_OK) {
		size_t prefix_len = strlen(LIBRARY_PLAYLIST_PREFIX);
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *table = (const char *)sqlite3_column_text(stmt, 0);
			if (!table || strlen(table) <= prefix_len) {
				continue;
			}
			if (count == capacity) {
				int grown = capacity ? capacity * 2 : 32;
				char **bigger = realloc(names, (size_t)grown * sizeof(*names));
				if (!bigger) {
					break;
				}
				names = bigger;
				capacity = grown;
			}
			names[count] = strdup(table + prefix_len);
			if (!names[count]) {
				break;
			}
			count++;
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);

	if (names_out) {
		*names_out = names;
	} else {
		library_playlist_names_free(names, count);
	}
	return count;
}

void library_playlist_names_free(char **names, int count) {
	if (!names) {
		return;
	}
	for (int i = 0; i < count; i++) {
		free(names[i]);
	}
	free(names);
}

bool library_playlist_create(const char *name) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (!playlist_table(name, quoted, sizeof(quoted))) {
		return false;
	}
	pthread_mutex_lock(&db_lock);
	bool ok = false;
	if (db && !playlist_table_exists_locked(name)) {
		ok = playlist_create_locked(quoted);
	}
	if (ok) {
		bump_generation(GEN_PLAYLISTS);
	}
	pthread_mutex_unlock(&db_lock);
	return ok;
}

bool library_playlist_drop(const char *name) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (!playlist_table(name, quoted, sizeof(quoted))) {
		return false;
	}
	pthread_mutex_lock(&db_lock);
	bool ok = false;
	if (db && playlist_table_exists_locked(name)) {
		char sql[PLAYLIST_TABLE_MAX + 32];
		snprintf(sql, sizeof(sql), "DROP TABLE %s", quoted);
		ok = exec(sql);
	}
	if (ok) {
		bump_generation(GEN_PLAYLISTS);
	}
	pthread_mutex_unlock(&db_lock);
	return ok;
}

bool library_playlist_rename(const char *name, const char *new_name) {
	char from[PLAYLIST_TABLE_MAX], to[PLAYLIST_TABLE_MAX];
	if (!playlist_table(name, from, sizeof(from)) || !playlist_table(new_name, to, sizeof(to))) {
		return false;
	}
	if (strcmp(name, new_name) == 0) {
		return true;
	}
	pthread_mutex_lock(&db_lock);
	bool ok = false;
	if (db && playlist_table_exists_locked(name) && !playlist_table_exists_locked(new_name)) {
		char sql[2 * PLAYLIST_TABLE_MAX + 32];
		snprintf(sql, sizeof(sql), "ALTER TABLE %s RENAME TO %s", from, to);
		ok = exec(sql);
	}
	if (ok) {
		bump_generation(GEN_PLAYLISTS);
	}
	pthread_mutex_unlock(&db_lock);
	return ok;
}

// One row in, with the table already made and the lock already held.
static bool playlist_append_locked(const char *quoted, const library_playlist_row_t *row) {
	char sql[PLAYLIST_TABLE_MAX + 160];
	snprintf(sql, sizeof(sql), "INSERT INTO %s(path,title,artist,seconds,present,mount) VALUES(?,?,?,?,1,?)", quoted);

	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		return false;
	}
	sqlite3_bind_text(stmt, 1, row->path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, row->title, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, row->artist, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 4, row->seconds);
	sqlite3_bind_int64(stmt, 5, mount_serial);
	bool ok = sqlite3_step(stmt) == SQLITE_DONE;
	sqlite3_finalize(stmt);
	return ok;
}

bool library_playlist_append(const char *name, const library_playlist_row_t *row) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (!row || !playlist_table(name, quoted, sizeof(quoted))) {
		return false;
	}
	pthread_mutex_lock(&db_lock);
	bool ok = db && playlist_create_locked(quoted) && playlist_append_locked(quoted, row);
	if (ok) {
		bump_generation(GEN_PLAYLISTS);
	}
	pthread_mutex_unlock(&db_lock);
	return ok;
}

// One row out of a statement that selected path,title,artist,seconds,present,mount.
static void playlist_row_read(sqlite3_stmt *stmt, library_playlist_row_t *row) {
	const char *text = (const char *)sqlite3_column_text(stmt, 0);
	snprintf(row->path, sizeof(row->path), "%s", text ? text : "");
	text = (const char *)sqlite3_column_text(stmt, 1);
	snprintf(row->title, sizeof(row->title), "%s", text ? text : "");
	text = (const char *)sqlite3_column_text(stmt, 2);
	snprintf(row->artist, sizeof(row->artist), "%s", text ? text : "");
	row->seconds = (long)sqlite3_column_int64(stmt, 3);
	row->present = sqlite3_column_int(stmt, 4) != 0;
	row->checked = sqlite3_column_int64(stmt, 5) == mount_serial;
}

int library_playlist_page(const char *name, int offset, int count, library_playlist_row_t *out) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (!out || offset < 0 || count <= 0 || !playlist_table(name, quoted, sizeof(quoted))) {
		return 0;
	}

	char sql[PLAYLIST_TABLE_MAX + 128];
	if (snprintf(sql, sizeof(sql),
				 "SELECT path,title,artist,seconds,present,mount FROM %s ORDER BY idx LIMIT ? OFFSET ?",
				 quoted) >= (int)sizeof(sql)) {
		return 0;
	}

	int got = 0;
	pthread_mutex_lock(&db_lock);
	if (db && playlist_table_exists_locked(name)) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_int(stmt, 1, count);
			sqlite3_bind_int(stmt, 2, offset);
			while (got < count && sqlite3_step(stmt) == SQLITE_ROW) {
				playlist_row_read(stmt, &out[got]);
				got++;
			}
			sqlite3_finalize(stmt);
		}
	}
	pthread_mutex_unlock(&db_lock);
	return got;
}

bool library_playlist_has_unchecked(const char *name) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (!playlist_table(name, quoted, sizeof(quoted))) {
		return false;
	}
	char sql[PLAYLIST_TABLE_MAX + 96];
	// "IS NOT" and not "<>": a row written before the column existed holds
	// NULL, and NULL <> anything is NULL, which is not true and would leave
	// those rows never looked at.
	if (snprintf(sql, sizeof(sql), "SELECT 1 FROM %s WHERE mount IS NOT ? LIMIT 1", quoted) >= (int)sizeof(sql)) {
		return false;
	}

	bool any = false;
	pthread_mutex_lock(&db_lock);
	if (db && playlist_table_exists_locked(name)) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, mount_serial);
			any = sqlite3_step(stmt) == SQLITE_ROW;
			sqlite3_finalize(stmt);
		}
	}
	pthread_mutex_unlock(&db_lock);
	return any;
}

// How many rows there are at all, present or not: what a full read has to make
// room for, and not the number under the playlist's name.
static int playlist_row_total(const char *name) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (!playlist_table(name, quoted, sizeof(quoted))) {
		return 0;
	}
	pthread_mutex_lock(&db_lock);
	int count = 0;
	if (db && playlist_table_exists_locked(name)) {
		char sql[PLAYLIST_TABLE_MAX + 64];
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", quoted);
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				count = sqlite3_column_int(stmt, 0);
			}
			sqlite3_finalize(stmt);
		}
	}
	pthread_mutex_unlock(&db_lock);
	return count;
}

int library_playlist_rows(const char *name, library_playlist_row_t **rows_out) {
	if (rows_out) {
		*rows_out = NULL;
	}
	int total = playlist_row_total(name);
	if (total <= 0) {
		return 0;
	}
	library_playlist_row_t *rows = malloc((size_t)total * sizeof(*rows));
	if (!rows) {
		return 0;
	}
	int got = library_playlist_page(name, 0, total, rows);
	if (rows_out) {
		*rows_out = rows;
	} else {
		free(rows);
	}
	return got;
}

// ---------------------------------------------------------------------------
// Writing one, a row at a time
//
// An import resolves its entries one by one and a backup reads them back one by
// one, so neither has any reason to hold the whole playlist -- and at two
// kilobytes a row, holding five thousand of them is nine megabytes on a device
// with ten. The transaction stays: one commit for the lot, because a commit
// each would be one card write each.
//
// The lock is taken per row and given back between them, not held for the
// length of the write. That is not tidiness: what a caller does between two
// rows is work out what the next one says, and working that out means asking
// the index for a track's name -- which takes this same lock, which is not
// recursive. Held across the write, the first such question would be a deadlock.
// The scan works the same way for the same reason.
//
// A transaction that will not start -- a scan has one open, and this connection
// allows one at a time -- is not a failure: the rows go in one at a time
// instead, which is slower and just as correct.
// ---------------------------------------------------------------------------

struct library_playlist_writer {
	sqlite3_stmt *stmt;
	bool failed;
	bool owns_transaction;
};

library_playlist_writer_t *library_playlist_write_begin(const char *name) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (!playlist_table(name, quoted, sizeof(quoted))) {
		return NULL;
	}
	library_playlist_writer_t *writer = calloc(1, sizeof(*writer));
	if (!writer) {
		return NULL;
	}

	char sql[PLAYLIST_TABLE_MAX + 160];
	snprintf(sql, sizeof(sql), "INSERT INTO %s(path,title,artist,seconds,present,mount) VALUES(?,?,?,?,1,?)", quoted);

	pthread_mutex_lock(&db_lock);
	if (!db || !playlist_create_locked(quoted) || sqlite3_prepare_v2(db, sql, -1, &writer->stmt, NULL) != SQLITE_OK) {
		pthread_mutex_unlock(&db_lock);
		free(writer);
		return NULL;
	}
	writer->owns_transaction = exec_quiet("BEGIN");
	pthread_mutex_unlock(&db_lock);
	return writer;
}

bool library_playlist_write_row(library_playlist_writer_t *writer, const library_playlist_row_t *row) {
	if (!writer || !row || writer->failed) {
		return false;
	}
	pthread_mutex_lock(&db_lock);
	sqlite3_reset(writer->stmt);
	sqlite3_bind_text(writer->stmt, 1, row->path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(writer->stmt, 2, row->title, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(writer->stmt, 3, row->artist, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(writer->stmt, 4, row->seconds);
	sqlite3_bind_int64(writer->stmt, 5, mount_serial);
	bool ok = sqlite3_step(writer->stmt) == SQLITE_DONE;
	pthread_mutex_unlock(&db_lock);

	if (!ok) {
		writer->failed = true;
	}
	return ok;
}

bool library_playlist_write_end(library_playlist_writer_t *writer, bool keep) {
	if (!writer) {
		return false;
	}
	bool ok = keep && !writer->failed;
	pthread_mutex_lock(&db_lock);
	sqlite3_finalize(writer->stmt);
	if (writer->owns_transaction) {
		exec(ok ? "COMMIT" : "ROLLBACK");
	} else if (!ok) {
		// Nothing to roll back to, since every row went in on its own. The
		// half-written playlist is dropped instead, which is what a rollback
		// would have left behind.
		ok = false;
	}
	bump_generation(GEN_PLAYLISTS);
	pthread_mutex_unlock(&db_lock);
	free(writer);
	return ok;
}

bool library_playlist_append_all(const char *name, const library_playlist_row_t *rows, int count) {
	if (!rows || count < 0) {
		return false;
	}
	library_playlist_writer_t *writer = library_playlist_write_begin(name);
	if (!writer) {
		return false;
	}
	bool ok = true;
	for (int i = 0; i < count && ok; i++) {
		ok = library_playlist_write_row(writer, &rows[i]);
	}
	return library_playlist_write_end(writer, ok);
}

bool library_playlist_set_presence(const char *name, const library_playlist_presence_t *marks, int count) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (!marks || count <= 0 || !playlist_table(name, quoted, sizeof(quoted))) {
		return false;
	}

	// By position in the ordered read, which is what the caller walked, turned
	// back into the row's own idx by the same ORDER BY.
	char sql[PLAYLIST_TABLE_MAX * 2 + 200];
	snprintf(sql, sizeof(sql),
			 "UPDATE %s SET present=?, mount=? WHERE idx=(SELECT idx FROM %s ORDER BY idx LIMIT 1 OFFSET ?)", quoted,
			 quoted);

	pthread_mutex_lock(&db_lock);
	bool ok = db != NULL;
	if (ok) {
		exec("BEGIN");
		sqlite3_stmt *stmt = NULL;
		ok = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK;
		for (int i = 0; i < count && ok; i++) {
			sqlite3_bind_int(stmt, 1, marks[i].present ? 1 : 0);
			sqlite3_bind_int64(stmt, 2, mount_serial);
			sqlite3_bind_int(stmt, 3, marks[i].index);
			ok = sqlite3_step(stmt) == SQLITE_DONE;
			sqlite3_reset(stmt);
		}
		sqlite3_finalize(stmt);
		exec(ok ? "COMMIT" : "ROLLBACK");
		// The list a page is showing leaves out what is not there, so a row
		// that has just changed its mind about that changes the list.
		bump_generation(GEN_PLAYLISTS);
	}
	pthread_mutex_unlock(&db_lock);
	return ok;
}

// Moves one entry of a playlist from one position to another.
//
// Positions, not row ids: `idx` is the primary key and orders the table, but it
// is not a rank. Removing an entry leaves a hole, so the entry at position 7 can
// perfectly well have idx 11, and the page the user is dragging on counts rows
// from the top.
//
// The set of idx values in the stretch between the two positions does not
// change -- only which row sits on which of them -- so the work is proportional
// to the distance dragged and not to the length of the list. The dragged row is
// parked on a key nothing else uses first, because updating straight onto an
// occupied primary key is a constraint failure, and the rows in between are
// shifted in the direction that always writes into a slot just vacated.
bool library_playlist_move(const char *name, int from, int to) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (from < 0 || to < 0 || from == to || !playlist_table(name, quoted, sizeof(quoted))) {
		return false;
	}

	char read_sql[PLAYLIST_TABLE_MAX + 80];
	char write_sql[PLAYLIST_TABLE_MAX + 80];
	// present<>0, exactly as the list the user is dragging on is built
	// (library_list_sql). The page leaves out entries whose file is not on the
	// card, so its positions count only those; reading them back unfiltered
	// would count the missing ones too and move the wrong row.
	//
	// An entry that is not shown keeps its idx, so it is never moved and never
	// lost -- but it does not keep its neighbours either, because the shown ones
	// are permuted among the idx values they occupied and a hidden entry between
	// two of them can end up on the other side. There is no better answer: it is
	// an order between rows nobody is looking at.
	snprintf(read_sql, sizeof(read_sql), "SELECT idx FROM %s WHERE present<>0 ORDER BY idx LIMIT ? OFFSET ?",
			 quoted);
	snprintf(write_sql, sizeof(write_sql), "UPDATE %s SET idx=? WHERE idx=?", quoted);

	int low = from < to ? from : to;
	int high = from < to ? to : from;
	int span = high - low + 1;

	pthread_mutex_lock(&db_lock);
	bool ok = db != NULL && playlist_table_exists_locked(name);

	// Only the stretch that moves is read back, which is what keeps a drag of
	// two rows from touching a list of ten thousand.
	int64_t *keys = ok ? calloc((size_t)span, sizeof(int64_t)) : NULL;
	int got = 0;
	if (keys) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db, read_sql, -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_int(stmt, 1, span);
			sqlite3_bind_int(stmt, 2, low);
			while (got < span && sqlite3_step(stmt) == SQLITE_ROW) {
				keys[got++] = sqlite3_column_int64(stmt, 0);
			}
			sqlite3_finalize(stmt);
		}
	}
	ok = keys != NULL && got == span;

	if (ok) {
		exec("BEGIN");
		sqlite3_stmt *stmt = NULL;
		ok = sqlite3_prepare_v2(db, write_sql, -1, &stmt, NULL) == SQLITE_OK;

		// A key below every real one: idx is INTEGER PRIMARY KEY and the table
		// is written with positive values only, so nothing can be sitting here.
		const int64_t parked = -1;
		int64_t moving = keys[from - low];

		if (ok) {
			sqlite3_bind_int64(stmt, 1, parked);
			sqlite3_bind_int64(stmt, 2, moving);
			ok = sqlite3_step(stmt) == SQLITE_DONE;
			sqlite3_reset(stmt);
		}

		// Dragged down: everything under it comes up one slot, taken in the
		// order that leaves the destination free. Dragged up: the mirror.
		if (from < to) {
			for (int i = from + 1; i <= to && ok; i++) {
				sqlite3_bind_int64(stmt, 1, keys[i - 1 - low]);
				sqlite3_bind_int64(stmt, 2, keys[i - low]);
				ok = sqlite3_step(stmt) == SQLITE_DONE;
				sqlite3_reset(stmt);
			}
		} else {
			for (int i = from - 1; i >= to && ok; i--) {
				sqlite3_bind_int64(stmt, 1, keys[i + 1 - low]);
				sqlite3_bind_int64(stmt, 2, keys[i - low]);
				ok = sqlite3_step(stmt) == SQLITE_DONE;
				sqlite3_reset(stmt);
			}
		}

		if (ok) {
			sqlite3_bind_int64(stmt, 1, keys[to - low]);
			sqlite3_bind_int64(stmt, 2, parked);
			ok = sqlite3_step(stmt) == SQLITE_DONE;
			sqlite3_reset(stmt);
		}

		sqlite3_finalize(stmt);
		exec(ok ? "COMMIT" : "ROLLBACK");
		if (ok) {
			bump_generation(GEN_PLAYLISTS);
		}
	}

	free(keys);
	pthread_mutex_unlock(&db_lock);
	return ok;
}

bool library_playlist_remove_path(const char *name, const char *path) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (!path || !path[0] || !playlist_table(name, quoted, sizeof(quoted))) {
		return false;
	}
	char sql[PLAYLIST_TABLE_MAX * 2 + 160];
	snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE idx=(SELECT idx FROM %s WHERE path=? ORDER BY idx LIMIT 1)",
			 quoted, quoted);

	pthread_mutex_lock(&db_lock);
	bool ok = false;
	if (db && playlist_table_exists_locked(name)) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
			ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0;
			sqlite3_finalize(stmt);
		}
	}
	if (ok) {
		bump_generation(GEN_PLAYLISTS);
	}
	pthread_mutex_unlock(&db_lock);
	return ok;
}

int library_playlist_count(const char *name) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (!playlist_table(name, quoted, sizeof(quoted))) {
		return 0;
	}
	pthread_mutex_lock(&db_lock);
	int count = 0;
	if (db && playlist_table_exists_locked(name)) {
		char sql[PLAYLIST_TABLE_MAX + 64];
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE present<>0", quoted);
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				count = sqlite3_column_int(stmt, 0);
			}
			sqlite3_finalize(stmt);
		}
	}
	pthread_mutex_unlock(&db_lock);
	return count;
}

bool library_playlists_migrated(void) {
	pthread_mutex_lock(&db_lock);
	bool done = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT done FROM PLAYLIST_MIGRATION WHERE id=0", -1, &stmt, NULL) == SQLITE_OK) {
		done = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) != 0;
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return done;
}

void library_playlists_set_migrated(void) {
	pthread_mutex_lock(&db_lock);
	if (db) {
		exec("INSERT OR REPLACE INTO PLAYLIST_MIGRATION(id,done) VALUES(0,1)");
	}
	pthread_mutex_unlock(&db_lock);
}

int library_playlist_count_get(const char *path, long mtime, long size) {
	if (!path || !path[0]) {
		return -1;
	}
	int tracks = -1;
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT tracks FROM PLAYLIST_COUNTS WHERE path=? AND mtime=? AND size=?", -1,
								 &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 2, mtime);
		sqlite3_bind_int64(stmt, 3, size);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			tracks = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return tracks;
}

bool library_playlist_count_is_verified(const char *path, long mtime, long size) {
	if (!path || !path[0]) {
		return false;
	}
	bool verified = false;
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT 1 FROM PLAYLIST_COUNTS WHERE path=? AND mtime=? AND size=? AND mount=?",
								 -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 2, mtime);
		sqlite3_bind_int64(stmt, 3, size);
		sqlite3_bind_int64(stmt, 4, mount_serial);
		verified = sqlite3_step(stmt) == SQLITE_ROW;
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return verified;
}

void library_playlist_count_save(const char *path, long mtime, long size, int tracks) {
	if (!path || !path[0] || tracks < 0) {
		return;
	}
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db &&
		sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO PLAYLIST_COUNTS(path,mtime,size,tracks,mount) VALUES(?,?,?,?,?)",
						   -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 2, mtime);
		sqlite3_bind_int64(stmt, 3, size);
		sqlite3_bind_int(stmt, 4, tracks);
		sqlite3_bind_int64(stmt, 5, mount_serial);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
}

void library_playback_state_save(const char *path, double position) {
	if (!path || !path[0]) {
		return;
	}
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db,
								 "INSERT OR REPLACE INTO PLAYBACK_STATE(id,path,position,updated_at)"
								 " VALUES(0,?,?,strftime('%s','now'))",
								 -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		sqlite3_bind_double(stmt, 2, position);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
}

bool library_playback_state_load(char *path_out, size_t path_size, double *position_out) {
	bool found = false;
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT path, position FROM PLAYBACK_STATE WHERE id=0", -1, &stmt,
								 NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *path = sqlite3_column_text(stmt, 0);
			if (path && path[0] && path_out) {
				snprintf(path_out, path_size, "%s", (const char *)path);
				if (position_out) {
					*position_out = sqlite3_column_double(stmt, 1);
				}
				found = true;
			}
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return found;
}

// ---------------------------------------------------------------------------
// The playback queue, mirrored on disk. A full rewrite is one transaction --
// thousands of separate inserts on a class-10 card would take seconds -- and
// the common case (the position moved) only touches the one state row.
// ---------------------------------------------------------------------------

void library_queue_save(const char *const *paths, int count, int current_index, bool custom) {
	pthread_mutex_lock(&db_lock);
	if (!db) {
		pthread_mutex_unlock(&db_lock);
		return;
	}

	sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
	sqlite3_exec(db, "DELETE FROM PLAYBACK_QUEUE", NULL, NULL, NULL);
	// The two forms are exclusive: a queue written out as rows has to retire
	// whichever query was standing, or the next boot would restore that instead
	// -- it is the one main.c looks at first.
	sqlite3_exec(db, "DELETE FROM PLAYBACK_QUEUE_QUERY", NULL, NULL, NULL);

	sqlite3_stmt *stmt = NULL;
	if (paths && count > 0 &&
		sqlite3_prepare_v2(db, "INSERT INTO PLAYBACK_QUEUE(idx,path) VALUES(?,?)", -1, &stmt, NULL) == SQLITE_OK) {
		for (int i = 0; i < count; i++) {
			if (!paths[i] || !paths[i][0]) {
				continue;
			}
			sqlite3_bind_int(stmt, 1, i);
			sqlite3_bind_text(stmt, 2, paths[i], -1, SQLITE_TRANSIENT);
			sqlite3_step(stmt);
			sqlite3_reset(stmt);
		}
		sqlite3_finalize(stmt);
	}

	stmt = NULL;
	if (sqlite3_prepare_v2(db,
						   "INSERT OR REPLACE INTO PLAYBACK_QUEUE_STATE(id,pos,custom,updated_at)"
						   " VALUES(0,?,?,strftime('%s','now'))",
						   -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int(stmt, 1, current_index);
		sqlite3_bind_int(stmt, 2, custom ? 1 : 0);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}

	sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
	pthread_mutex_unlock(&db_lock);
}

// Written beside whichever form holds the queue, and cleared when the queue
// stops being a record. Its own statement rather than a column in either table:
// the two saves delete each other's rows, and this outlives both.
void library_queue_save_album(const char *album) {
	pthread_mutex_lock(&db_lock);
	if (db) {
		if (album && album[0]) {
			sqlite3_stmt *stmt = NULL;
			if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO PLAYBACK_QUEUE_ALBUM(id,album) VALUES(0,?)", -1, &stmt,
								   NULL) == SQLITE_OK) {
				sqlite3_bind_text(stmt, 1, album, -1, SQLITE_TRANSIENT);
				sqlite3_step(stmt);
				sqlite3_finalize(stmt);
			}
		} else {
			sqlite3_exec(db, "DELETE FROM PLAYBACK_QUEUE_ALBUM", NULL, NULL, NULL);
		}
	}
	pthread_mutex_unlock(&db_lock);
}

bool library_queue_load_album(char *out, int size) {
	if (!out || size <= 0) {
		return false;
	}
	out[0] = '\0';

	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT album FROM PLAYBACK_QUEUE_ALBUM WHERE id=0", -1, &stmt, NULL) ==
				  SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *text = sqlite3_column_text(stmt, 0);
			if (text) {
				snprintf(out, (size_t)size, "%s", (const char *)text);
			}
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return out[0] != '\0';
}

void library_queue_save_index(int current_index) {
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "UPDATE PLAYBACK_QUEUE_STATE SET pos=?, updated_at=strftime('%s','now') WHERE id=0",
								 -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int(stmt, 1, current_index);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
}

int library_queue_load(char ***paths_out, int *index_out, bool *custom_out) {
	if (paths_out) {
		*paths_out = NULL;
	}
	if (index_out) {
		*index_out = 0;
	}
	if (custom_out) {
		*custom_out = false;
	}

	pthread_mutex_lock(&db_lock);
	if (!db) {
		pthread_mutex_unlock(&db_lock);
		return 0;
	}

	int count = 0;
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM PLAYBACK_QUEUE", -1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			count = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	if (count <= 0) {
		pthread_mutex_unlock(&db_lock);
		return 0;
	}

	char **list = calloc((size_t)count, sizeof(*list));
	if (!list) {
		pthread_mutex_unlock(&db_lock);
		return 0;
	}

	int filled = 0;
	stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT path FROM PLAYBACK_QUEUE ORDER BY idx", -1, &stmt, NULL) == SQLITE_OK) {
		while (filled < count && sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *path = sqlite3_column_text(stmt, 0);
			if (path && path[0]) {
				list[filled] = strdup((const char *)path);
				if (!list[filled]) {
					break;
				}
				filled++;
			}
		}
		sqlite3_finalize(stmt);
	}

	stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT pos, custom FROM PLAYBACK_QUEUE_STATE WHERE id=0", -1, &stmt, NULL) ==
		SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			if (index_out) {
				*index_out = sqlite3_column_int(stmt, 0);
			}
			if (custom_out) {
				*custom_out = sqlite3_column_int(stmt, 1) != 0;
			}
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);

	if (filled == 0) {
		free(list);
		return 0;
	}

	if (paths_out) {
		*paths_out = list;
	} else {
		library_queue_free(list, filled);
	}
	return filled;
}

void library_queue_free(char **paths, int count) {
	if (!paths) {
		return;
	}
	for (int i = 0; i < count; i++) {
		free(paths[i]);
	}
	free(paths);
}

int library_queue_load_extra(char ***paths_out, int **slots_out) {
	if (paths_out) {
		*paths_out = NULL;
	}
	if (slots_out) {
		*slots_out = NULL;
	}

	pthread_mutex_lock(&db_lock);
	if (!db) {
		pthread_mutex_unlock(&db_lock);
		return 0;
	}

	int count = 0;
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM PLAYBACK_QUEUE", -1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			count = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	if (count <= 0) {
		pthread_mutex_unlock(&db_lock);
		return 0;
	}

	char **list = calloc((size_t)count, sizeof(*list));
	int *slots = calloc((size_t)count, sizeof(*slots));
	if (!list || !slots) {
		free(list);
		free(slots);
		pthread_mutex_unlock(&db_lock);
		return 0;
	}

	// By slot, not by idx: they go back into the queue in the order they appear
	// in it, and each insert shifts everything after it up by one -- so the
	// later ones are only right if the earlier ones are already in place. A row
	// with no slot (a card written before the column existed) sorts last and is
	// appended after the current track.
	int filled = 0;
	stmt = NULL;
	if (sqlite3_prepare_v2(db,
						   "SELECT path, IFNULL(slot,-1) FROM PLAYBACK_QUEUE"
						   " ORDER BY (IFNULL(slot,-1) < 0), IFNULL(slot,-1), idx",
						   -1, &stmt, NULL) == SQLITE_OK) {
		while (filled < count && sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *path = sqlite3_column_text(stmt, 0);
			if (!path || !path[0]) {
				continue;
			}
			list[filled] = strdup((const char *)path);
			if (!list[filled]) {
				break;
			}
			slots[filled] = sqlite3_column_int(stmt, 1);
			filled++;
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);

	if (filled == 0) {
		free(list);
		free(slots);
		return 0;
	}

	if (paths_out) {
		*paths_out = list;
	} else {
		library_queue_free(list, filled);
	}
	if (slots_out) {
		*slots_out = slots;
	} else {
		free(slots);
	}
	return filled;
}

// ---------------------------------------------------------------------------
// Search: names LIKE %query% across tracks, albums and artists, capped per
// category -- the search page's data source.
// ---------------------------------------------------------------------------

int library_search(const char *query, int per_category, library_search_cb_t cb, void *user) {
	if (!query || !query[0] || !cb) {
		return 0;
	}

	// The pattern, folded the way foldcase() folds the rows, and with LIKE's own
	// two wildcards escaped: a query is a piece of a name, so a user typing "_"
	// means an underscore and not "any character".
	char folded[256];
	if (fold_text(query, folded, sizeof(folded)) == 0) {
		return 0;
	}

	char like[2 * sizeof(folded) + 3];
	size_t at = 0;
	like[at++] = '%';
	for (size_t i = 0; folded[i]; i++) {
		if (folded[i] == '%' || folded[i] == '_' || folded[i] == '\\') {
			like[at++] = '\\';
		}
		like[at++] = folded[i];
	}
	like[at++] = '%';
	like[at] = '\0';

	int total = 0;

	pthread_mutex_lock(&db_lock);
	if (db) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db,
							   "SELECT name, path FROM MEDIA_TABLE WHERE foldcase(name) LIKE ? ESCAPE '\\'"
							   " ORDER BY name LIMIT ?",
							   -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, like, -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(stmt, 2, per_category);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *name = (const char *)sqlite3_column_text(stmt, 0);
				const char *path = (const char *)sqlite3_column_text(stmt, 1);
				cb(LIBRARY_SEARCH_TRACK, name ? name : "", path ? path : "", user);
				total++;
			}
			sqlite3_finalize(stmt);
		}
		// The album's first track rides along as the `path`, so the search page
		// can load its artwork the same way the album list does.
		if (sqlite3_prepare_v2(db,
							   "SELECT album, (SELECT path FROM MEDIA_TABLE m WHERE m.album = ALBUM_TABLE.album"
							   " ORDER BY COALESCE(m.disc,1), m.dis_id LIMIT 1) FROM ALBUM_TABLE WHERE foldcase(album) LIKE ?"
							   " ESCAPE '\\' ORDER BY album LIMIT ?",
							   -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, like, -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(stmt, 2, per_category);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *name = (const char *)sqlite3_column_text(stmt, 0);
				const char *path = (const char *)sqlite3_column_text(stmt, 1);
				if (name && name[0]) {
					cb(LIBRARY_SEARCH_ALBUM, name, path ? path : "", user);
					total++;
				}
			}
			sqlite3_finalize(stmt);
		}
		if (sqlite3_prepare_v2(db,
							   "SELECT artist FROM ARTIST_TABLE WHERE foldcase(artist) LIKE ? ESCAPE '\\'"
							   " ORDER BY artist LIMIT ?",
							   -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, like, -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(stmt, 2, per_category);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *name = (const char *)sqlite3_column_text(stmt, 0);
				if (name && name[0]) {
					cb(LIBRARY_SEARCH_ARTIST, name, NULL, user);
					total++;
				}
			}
			sqlite3_finalize(stmt);
		}
	}
	pthread_mutex_unlock(&db_lock);
	return total;
}
