#ifndef COVERTONE_H
#define COVERTONE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

// ===========================================================================
// Album tone and thumbnails (Sonix UI 2.0 cover pipeline)
//
// INTEGRATION
//
// Palette engine. Every screen of UI 2.0 is painted from one OKLCH hue: the
// playing track's album, or the album a page shows. Hand the engine the
// adapter below once at startup:
//
//     sx_palette_set_tone_provider(covertone_tone_provider);
//
// covertone_tone_provider() is covertone_for_path() without the lightness: a
// lookup in memory and one indexed SQLite query, never a decode, so it is safe
// on the UI thread at every track change. When it answers false the album has
// not been looked at yet; paint with covertone_hash_hue(album title) -- the
// same hue the pipeline itself falls back to for an album without a picture,
// so nothing changes colour when the job catches up with it -- and repaint
// when covertone_revision() moves (or from the listener, see
// covertone_set_listener()).
//
// List rows. An album row knows its album value (the `name` of a
// LIBRARY_LIST_ALBUMS row, library.h); covertone_album_id(name) is its id.
//   1. Draw the row at once with a plate of SX_T_COVER_TONE in the album's hue:
//      covertone_for_album(id, ...), or covertone_hash_hue(title) when false.
//   2. covertone_thumb(id, COVERTONE_THUMB_SMALL, &dsc, buf, sizeof buf) with a
//      buffer the row owns (COVERTONE_THUMB_BYTES(56), 6272 bytes): a single
//      6 KB read from the card, fine on the UI thread. On true, lv_image_set_src
//      (img, &dsc) -- dsc points into buf, which must live as long as the image.
//   3. On false, covertone_request(id) for the rows on screen (first visible
//      rows first: later requests go to the front) and try again when the
//      revision moves.
// The 240 px picture (album page head, the player before its full cover is
// decoded) is 115 KB: never read it on the UI thread -- use
// covertone_thumb_async(), which reads (or makes) it on the worker and hands
// it back through gui_post().
//
// While a list scrolls or a screen animates, nothing here touches the card:
// the UI watcher (covertone_ui_init) notices a finger down, a scroll in
// flight and a screen load in progress by itself. Anything else that should
// keep the worker off the card and the CPU (a long custom animation) wraps
// itself in covertone_pause(true) ... covertone_pause(false).
//
// WHAT IS KEPT, AND WHERE
//
// Per album, in the library index (library.db, table ALBUM_COVER, keyed by
// covertone_album_id()): cover_h (hue, degrees), cover_c (chroma * 510),
// cover_l (lightness * 255), cover_mtime (the picture's file's mtime), what the
// tone was made from (the track, the cover file, their mtimes and sizes) so a
// rescan only decodes what changed, and which thumbnails exist.
//
// The thumbnails are raw RGB565, one fixed-size slot per album in one file per
// size beside the index: .local/covers/thumbs56.sxt and thumbs240.sxt. See
// covertone.c for the layout. One file per size rather than one per album: a
// 6 KB file takes a whole cluster on the card -- 32 or 128 KB on an exFAT card
// -- and thousands of names in one FAT directory make every lookup slow.
//
// WHO DOES THE WORK
//
// A full scan and Detect changes analyse the albums they read at the end of
// their run, on the scan thread, within a time budget of about a seventh of
// the run (so a scan gets no more than ~15% longer); what does not fit, the
// 240 px pictures, an index written before this existed and progressive JPEGs
// are left to a background thread at idle priority (SCHED_IDLE), which stays
// off the card while the interface is busy, while a track is starting and
// while a scan runs.
// ===========================================================================

// Thumbnail sides, in pixels.
#define COVERTONE_THUMB_SMALL 56
#define COVERTONE_THUMB_LARGE 240
#define COVERTONE_THUMB_BYTES(side) ((size_t)(side) * (size_t)(side) * 2u)

// An album's id: a 63-bit hash of its value (the name, LIBRARY_ALBUM_KEY_SEP
// and the key, as library.h describes), folded to lower case in ASCII as the
// library compares names. Stable across rescans and reboots for as long as
// the album keeps its name and key. Never 0 for a non-empty value; 0 for NULL
// or "". Any thread, no locks.
int64_t covertone_album_id(const char *album_value);

// The same from the two parts, for code that has them apart.
int64_t covertone_album_id_parts(const char *album, const char *album_key);

// The tone of the album `track_path` belongs to: hue in degrees (0..360),
// OKLCH chroma (0..~0.37) and lightness (0..1) of the sleeve's most saturated
// noticeable colour. For an album whose picture is grey, missing or cannot be
// decoded the answer is still true: the hue is covertone_hash_hue() of its
// title and the chroma is what the picture has (0 without one), so a caller
// that wants to tell the two apart compares chroma with a threshold of its own
// (0.04 is about where a hue stops being visible).
//
// False when the album is not known yet (the job has not reached it, the track
// is not in the index or has no album tag): use covertone_hash_hue(title).
//
// Any thread. Memory, plus one indexed query of the library index for the
// track's album (cached for the last few paths): no decode, no file read
// beyond SQLite's. Any output may be NULL.
//
// Not from inside a library row callback (library_for_each(),
// library_index_window() ...): those run with the index locked, and this asks
// the index -- the thread would wait for itself. covertone_for_album() is
// memory only and safe there.
bool covertone_for_path(const char *track_path, float *hue_deg, float *chroma, float *lightness);

// The same by album id: memory only.
bool covertone_for_album(int64_t album_id, float *hue_deg, float *chroma, float *lightness);

// covertone_for_path() in the shape the palette engine's provider takes
// (sx_palette_set_tone_provider). UI thread or any other.
bool covertone_tone_provider(const char *track_path, float *hue_deg, float *chroma);

// The fallback hue for an album with no usable picture, or not looked at yet:
// a hash of its title (the name a person reads, library_album_title()),
// ASCII-folded, 0..359. Any thread, pure.
uint16_t covertone_hash_hue(const char *album_title);

// The album's thumbnail of `size` (COVERTONE_THUMB_SMALL or _LARGE) into the
// caller's buffer: `buf` holds at least COVERTONE_THUMB_BYTES(size) bytes and
// must outlive every use of `dsc`, which is filled in to point into it (RGB565,
// size x size, ready for lv_image_set_src). Nothing is allocated, nothing to
// free.
//
// False when the thumbnail is not made yet (ask covertone_request()), when the
// album has no picture, and for the large size on the UI thread -- 115 KB off
// the card is a stall there; use covertone_thumb_async(). Reads the card: the
// small size costs one ~6 KB read, which is fine on the UI thread.
bool covertone_thumb(int64_t album_id, int size, lv_image_dsc_t *dsc, void *buf, size_t buf_size);

// The asynchronous form, for the large size on the UI thread. The thumbnail is
// read on the worker -- or made there first, decoding the picture, when it does
// not exist yet -- and `cb` runs on the UI thread through gui_post().
//
// On success `dsc` describes a buffer the callback now owns: copy *dsc into
// storage of your own and give the pixels back with covertone_thumb_free()
// when the image is gone. On failure (no picture, no card) `dsc` is NULL. The
// callback is not run after covertone_thumb_cancel(ticket), which a page calls
// when it goes away. Returns the ticket, 0 when the request could not be
// queued (then `cb` never runs). At most 16 requests wait at a time; the
// oldest is cancelled -- its callback runs with NULL -- to make room.
typedef void (*covertone_thumb_cb)(int64_t album_id, int size, const lv_image_dsc_t *dsc, void *user);
uint32_t covertone_thumb_async(int64_t album_id, int size, covertone_thumb_cb cb, void *user);
void covertone_thumb_cancel(uint32_t ticket);
void covertone_thumb_free(lv_image_dsc_t *dsc);

// Asks for this album's tone and thumbnails ahead of the background order --
// what a list does for the rows it is showing. Requests go to the front of a
// short queue (32; the oldest fall off), so the last row asked for is the
// first one made. Any thread, returns at once.
void covertone_request(int64_t album_id);

// Keeps the worker off the card and the processor while the interface does
// something smooth. Counted: every pause(true) needs its pause(false). A pause
// left on by mistake lapses after 30 seconds, so a forgotten call costs a
// late picture and not a job that never runs again. Any thread.
void covertone_pause(bool paused);

// Bumped whenever a tone or a thumbnail lands, and when the set is reloaded
// (another card, a rescan pruned it). A list compares it with the value it
// last drew with. Any thread.
uint32_t covertone_revision(void);

// Called on the UI thread, at most about four times a second, after new tones
// or thumbnails have landed. One listener; NULL removes it.
void covertone_set_listener(void (*changed)(void));

// ---------------------------------------------------------------------------
// Plumbing: called by main.c and library.c, not by pages.
// ---------------------------------------------------------------------------

// UI thread, once, after LVGL and its input devices are up: notes which thread
// the UI is, and sets up the watcher (a 100 ms LVGL timer, started by a touch
// and stopping itself after) that pauses the worker while a finger is down, a
// list scrolls or a screen is being loaded.
void covertone_ui_init(void);

// The library index at `db_path` has just been opened (library_open): the
// tones are loaded into memory, the thumbnail files are looked for beside it,
// and the worker starts if there is work. Any thread, without the library's
// lock held.
void covertone_attach(const char *db_path);

// The index is about to close (library_close): the worker finishes the album
// it is on, lets go of the card and waits. Returns when no file of ours is open.
void covertone_detach(void);

// The end of a scan, on the scan thread (library.c): analyses the albums the
// run read, within `budget_ms`, and leaves the rest to the worker.
// `id_base` > 0 limits it to albums that have a track with a MEDIA_TABLE id
// above it (what Detect changes added); 0 means every album, checked against
// what is kept so that only changed pictures are decoded. `cancel` is the
// scan's stop flag; `progress` names the folder being looked at.
void covertone_scan_pass(int id_base, uint32_t budget_ms, volatile bool *cancel, void (*progress)(const char *folder));

// The albums may have changed (a scan, Detect changes or filing the index
// again finished): the worker prunes the tones of albums that are gone and
// looks for new work. Any thread.
void covertone_library_changed(void);

#endif // COVERTONE_H
