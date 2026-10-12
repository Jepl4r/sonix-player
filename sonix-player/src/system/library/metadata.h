#ifndef METADATA_H
#define METADATA_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	char title[256];
	char artist[256];		// performer credited on this track
	char album_artist[256]; // artist the album as a whole is credited to (TPE2 / ALBUMARTIST)
	char album[256];
	char genre[128];
	int track_number; // 0 if unknown
	int disc_number;  // 0 if unknown, which is read as disc one; see below
	int year;		  // 0 if unknown
	bool has_tags;	  // true if any tag field was found

	// Marked as a compilation: COMPILATION (or ITUNESCOMPILATION) in Vorbis
	// and APE tags, TCMP or TXXX:COMPILATION in ID3v2, cpil in MP4. Each track
	// of such a record has its own performer, and that is the name to show.
	bool compilation;

	// The series a book belongs to and its place in it, for the audiobook
	// index: SERIES / SERIES-PART and MOVEMENTNAME / MOVEMENT in Vorbis and APE
	// tags, TXXX:SERIES / TXXX:SERIES-PART, MVNM and MVIN in ID3v2, the same
	// freeform names and the movement atoms in MP4. The part is kept as the
	// text the tag holds ("3", "2.5", "3/12").
	char series[128];
	char series_part[32];

	// ReplayGain, when the file carries it: the loudness the tagger measured,
	// as a correction in dB, and the sample peak that correction has to stay
	// clear of. Track and album are separate numbers on purpose -- normalising
	// a whole record by one figure is what keeps a quiet passage quiet.
	//
	// Written by every tagger as text ("-7.50 dB", "0.988525"), which is why
	// they arrive here as floats rather than as tag strings.
	bool has_track_gain;
	bool has_album_gain;
	float track_gain_db;
	float album_gain_db;
	float track_peak; // 0 when the file does not say
	float album_peak;

	// The stream itself, filled only by the readers that have it in hand while
	// they look for the tags anyway: FLAC's STREAMINFO is the block in front of
	// the comments, and an MP4's sample entry and movie header are parsed with
	// its atoms. The library scan takes them from here instead of opening the
	// file a second time to ask. `has_stream` says they were filled; with it
	// false every one of them is zero and means nothing.
	bool has_stream;
	bool alac; // an MP4 whose audio is Apple Lossless
	int sample_rate;
	int bits_per_sample; // 0 for a lossy stream, which has no depth of its own
	uint32_t duration_ms; // 0 when the header does not say
} song_metadata_t;

// Reads whatever tag metadata is available for the file (ID3v1/ID3v2 for MP3,
// Vorbis comments for FLAC/OGG, LIST/INFO chunk for WAV). Always fills every
// field of `out` (empty string / 0 if not found). Does not touch playback
// state or decode any audio samples.
//
// disc_number is 0 when the file carries no disc tag, which is what a
// single-disc release looks like. Whoever orders by it treats that as disc
// one: a record where only some of the files carry the tag must not split
// into two shelves.
void metadata_read(const char *filepath, song_metadata_t *out);

// The artist to show for the track. By default the album's artist when the
// tags have one, since that is the name the record is filed under -- except on
// a compilation, where the album is credited to "Various Artists" or the like
// and the track's own performer says more. With "Use track artist" on, the
// track's own artist tag, whole -- every value of a tag that holds several, as
// the details show it -- and the album's artist only when the track has none.
const char *metadata_shown_artist(const song_metadata_t *m);

// "Use track artist", kept as [music] track_artist. Off by default.
bool metadata_track_artist(void);
void metadata_set_track_artist(bool on);

// The lyrics the file carries, as text: a Vorbis or APE LYRICS or
// UNSYNCEDLYRICS field, an ID3v2 USLT frame, an SYLT frame turned into LRC, or
// an MP4 ©lyr atom. Timed text wins over plain when a file has both. malloc'd,
// NULL when there are none.
char *metadata_read_lyrics(const char *filepath);

// metadata_read(), and the description the file carries as plain text, for
// an audiobook's summary: a Vorbis or APE DESCRIPTION, SUMMARY or COMMENT, an
// ID3v2 COMM frame or TXXX:DESCRIPTION, an MP4 ldes, desc or comment atom.
// HTML is reduced to text and the length capped at 8 KB. malloc'd, NULL when
// the file has none.
char *metadata_read_with_description(const char *filepath, song_metadata_t *out);

#endif // METADATA_H
