/*
 * SQLite, built into the player.
 *
 * The stock hiby_player keeps its music index in SQLite and this port reuses
 * its schema, so the same database can be read by both. The firmware links it
 * statically and ships no libsqlite3, so it has to be carried along.
 *
 * The amalgamation is vendored as a .h (sqlite3_amalgamation.h) and included
 * here rather than being a .c of its own, for the same reason stb_image is:
 * the Makefile compiles every .c under src/, and the build options below have
 * to be set before the source is seen.
 */

// Only one connection, used from the UI thread and the scan thread, never at
// the same moment -- but serialized mode costs little and removes the question.
#define SQLITE_THREADSAFE 1

// A music index needs none of this, and every omission is binary left behind
// on a device with ten megabytes free.
#define SQLITE_OMIT_LOAD_EXTENSION 1
#define SQLITE_OMIT_DEPRECATED 1
#define SQLITE_OMIT_PROGRESS_CALLBACK 1
#define SQLITE_OMIT_AUTHORIZATION 1
#define SQLITE_OMIT_TRACE 1
#define SQLITE_OMIT_UTF16 1
#define SQLITE_OMIT_SHARED_CACHE 1
#define SQLITE_OMIT_COMPLETE 1
#define SQLITE_OMIT_TCL_VARIABLE 1
// Memory statistics stay on deliberately, even though disabling them would
// save an increment per malloc. SQLite only enforces the soft limit library.c
// sets with sqlite3_soft_heap_limit64() when memstatus is enabled; without it
// that call does nothing and the scan runs with no cap at all.
#define SQLITE_DEFAULT_MEMSTATUS 1
#define SQLITE_LIKE_DOESNT_MATCH_BLOBS 1
#define SQLITE_DQS 0

// Temporary tables on disk: the card has room, the RAM does not.
#define SQLITE_TEMP_STORE 1

// The card is FAT; there is no ownership to inherit and no unix-dotfile
// locking to negotiate.
#define SQLITE_DEFAULT_FILE_PERMISSIONS 0666

#include "sqlite3_amalgamation.h"
