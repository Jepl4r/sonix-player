#ifndef LIBRARYCOVERS_H
#define LIBRARYCOVERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The library index's side of the cover pipeline (covertone.h): the
// ALBUM_COVER table and the album queries the pipeline walks. Only covertone.c
// calls these; they live in library.c because the index's connection and its
// lock do.
//
// Every function takes the library's lock for one statement or one batch and
// gives it back: none of them is held across a decode.

// What kind of answer a row holds.
typedef enum {
	LIBRARY_COVER_NONE = 0,		// no picture anywhere: the title's hash
	LIBRARY_COVER_PICTURE = 1,	// the picture's own hue
	LIBRARY_COVER_GREY = 2,		// a picture without a noticeable hue: the title's hash
	LIBRARY_COVER_FAILED = 3,	// a picture that would not decode: the title's hash
	LIBRARY_COVER_DEFERRED = 4, // left by a scan for the worker (a progressive JPEG): the title's hash for now
} library_cover_kind_t;

// Which thumbnails a row's slot holds.
#define LIBRARY_COVER_THUMB_SMALL 1
#define LIBRARY_COVER_THUMB_LARGE 2

// One row of ALBUM_COVER as the pipeline keeps it in memory.
typedef struct {
	int64_t id;
	uint16_t hue;	// cover_h, degrees
	uint8_t chroma; // cover_c, OKLCH chroma * 510
	uint8_t light;	// cover_l, OKLCH lightness * 255
	uint8_t kind;	// library_cover_kind_t
	uint8_t thumbs; // LIBRARY_COVER_THUMB_*
	int32_t slot;	// in the thumbnail files, -1 for none
	int version;	// the analysis the row was made under
} library_cover_brief_t;

// One album to look at: who it is, the track its picture is taken from now,
// and what was kept about it, if anything.
typedef struct {
	int64_t group_rowid; // where the walk is, for the next batch
	int64_t id;
	char album[256];
	char album_key[128];
	char track[512];	 // its first track by disc and number, as the Albums list shows its cover
	int64_t track_mtime; // that track's mtime as the index has it
	bool kept;			 // the fields below are a stored row
	library_cover_brief_t brief;
	char kept_track[512];
	int64_t kept_track_mtime;
	char kept_src[512]; // the file the picture was read from ("" for none)
	int64_t kept_src_mtime;
	int64_t kept_src_size;
	int64_t kept_dir_mtime;
} library_cover_album_t;

// Which albums a walk returns.
typedef enum {
	LIBRARY_COVERS_ALL,		// every album, with its row if it has one
	LIBRARY_COVERS_STALE,	// without a row made from its current first track under `version`, or deferred
	LIBRARY_COVERS_THUMBS,	// current, with a picture, and a thumbnail missing
} library_covers_select_t;

// Up to `max` albums with a rowid above `after`, in rowid order. `version` is
// the current analysis; `id_base` > 0 keeps only albums with a track whose id
// is above it. Returns how many were written.
int library_covers_albums(library_covers_select_t which, int version, int64_t after, int id_base,
						  library_cover_album_t *out, int max);

// The album with this id, as above. False when no album has it.
bool library_covers_album(int64_t id, int version, library_cover_album_t *out);

// Every row, one callback each, with the lock held: copy, do not call back in.
int library_covers_load(void (*cb)(const library_cover_brief_t *row, void *user), void *user);

// Writes the row for `album`, with what it was made from.
bool library_cover_store(const library_cover_album_t *album, const library_cover_brief_t *row, const char *src,
						 int64_t src_mtime, int64_t src_size, int64_t dir_mtime);

// Updates a row's thumbnails and slot.
void library_cover_set_thumbs(int64_t id, int thumbs, int32_t slot);

// Another row made from the same track at the same mtime under `version`, for
// an album whose key changed (the index filed again): its id, or 0.
int64_t library_cover_find_by_track(const char *track, int64_t track_mtime, int version);

// Gives a row a new id (and album), keeping everything else.
bool library_cover_rekey(int64_t old_id, int64_t new_id, const char *album, const char *album_key);

// Deletes the rows of albums no longer in the index. Returns how many went.
int library_covers_prune(void);

// Clears one thumbnail bit on every row: its file was started again.
void library_covers_forget_thumbs(int bit);

// The full scan feature bit of the index (LIBRARY_INDEX_COVERS) is earned
// without a scan once every album has a row: written, and the notice it
// raised goes.
void library_covers_complete(void);

// Where the open index lives, so the thumbnail files can sit beside it. False
// with none open.
bool library_db_dir(char *out, size_t size);

#endif // LIBRARYCOVERS_H
