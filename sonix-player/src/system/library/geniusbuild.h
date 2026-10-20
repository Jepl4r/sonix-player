#ifndef GENIUSBUILD_H
#define GENIUSBUILD_H

#include <stdatomic.h>
#include <stddef.h>

// One builder, on the library scan worker. These bounds also keep its output
// inside the reader's 4 MiB budget (including private request metadata).
// The track bound also counts malformed rows with an empty/missing path, so
// scanning past arbitrarily many excluded rows cannot evade the input budget.
#define GENIUSBUILD_MAX_TRACKS 4096
#define GENIUSBUILD_MAX_TEXT_BYTES ((size_t)1024 * 1024)
#define GENIUSBUILD_MAX_FIELD_BYTES 4096
#define GENIUSBUILD_MAX_RETAINED_BYTES ((size_t)4 * 1024 * 1024)
#define GENIUSBUILD_MAX_BLOB_BYTES ((size_t)2 * 1024 * 1024)
#define GENIUSBUILD_TOP 32

typedef enum {
	GENIUSBUILD_OK, // a complete generation was published
	GENIUSBUILD_CURRENT,
	GENIUSBUILD_CANCELLED,
	GENIUSBUILD_LIMIT,
	GENIUSBUILD_NOMEM,
	GENIUSBUILD_INVALID,
	GENIUSBUILD_IO,
	GENIUSBUILD_CHANGED,
} geniusbuild_result_t;

typedef enum {
	GENIUSBUILD_CHECKING,
	GENIUSBUILD_RANKING,
	GENIUSBUILD_PUBLISHING,
	GENIUSBUILD_PUBLISHED,
} geniusbuild_phase_t;
typedef void (*geniusbuild_progress_cb)(geniusbuild_phase_t phase, int completed, int total, void *user);

// Reads tags only through library_read_tags(). Owns .genius-build.db and its
// journal beside path, never the old genius.db. Cancellation, changed inputs,
// limits and I/O errors discard staging. Library close must stop and await this
// call before unmount/export; a physical removal produces cancellation or I/O
// failure. An orphan staging file from a power cut is discarded on the next run.
// No file descriptor is retained on return. Call only from the scan worker.
geniusbuild_result_t geniusbuild_run(const char *path, const atomic_bool *cancel, geniusbuild_progress_cb progress, void *user);

#endif
