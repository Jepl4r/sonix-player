#ifndef METADATA_H
#define METADATA_H

#include <stdbool.h>

typedef struct {
	char title[256];
	char artist[256];		// performer credited on this track
	char album_artist[256]; // artist the album as a whole is credited to (TPE2 / ALBUMARTIST)
	char album[256];
	char genre[128];
	int track_number; // 0 if unknown
	int year;		  // 0 if unknown
	bool has_tags;	  // true if any tag field was found

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
} song_metadata_t;

// Reads whatever tag metadata is available for the file (ID3v1/ID3v2 for MP3,
// Vorbis comments for FLAC/OGG, LIST/INFO chunk for WAV). Always fills every
// field of `out` (empty string / 0 if not found). Does not touch playback
// state or decode any audio samples.
void metadata_read(const char *filepath, song_metadata_t *out);

#endif // METADATA_H
