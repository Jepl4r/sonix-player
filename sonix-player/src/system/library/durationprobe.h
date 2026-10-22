#ifndef DURATIONPROBE_H
#define DURATIONPROBE_H
// Seconds of audio in `path` from its headers, or 0 when that cannot be
// known cheaply. MP4 through mp4_duration_seconds; MP3 from the first
// 256 KiB (Xing/Info frame count, else CBR estimate from the first frame
// and the file size minus tags); FLAC, Opus and Vorbis through
// decoder_open_info. Never reads a whole MP3.
double durationprobe_seconds(const char *path);
#endif
