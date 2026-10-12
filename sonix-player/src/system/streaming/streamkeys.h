#ifndef STREAMKEYS_H
#define STREAMKEYS_H

#include <stdbool.h>

// The streaming services' application credentials, read from a file on the
// device rather than written in here.
//
// Qobuz and Tidal offer no way to talk to their APIs without a pair of
// application keys, and the ones this device has are HiBy's: they live inside
// /usr/bin/hiby_player, Tidal's even assembled byte by byte at run time so it
// never appears as a string. They are HiBy's keys, not this project's, and open
// source that carries them publishes them.
//
// So they sit in a file on the rootfs, which whoever assembles the firmware puts
// alongside the other resource files. It is written as an INI:
//
//     [qobuz]
//     app_id = ...
//     app_secret = ...
//
//     [tidal]
//     client_id = ...
//     client_secret = ...
//
//     [podcast]
//     api_key = ...
//     api_secret = ...
//
//     [lastfm]
//     api_key = ...
//     api_secret = ...
//
//     [yandex]
//     client_id = ...
//     client_secret = ...
//     sign_key = ...
//     client_header = X-Yandex-Music-Client: ...
//
// and shipped sealed, as /usr/resource/sonix/components/streaming-keys.bin, by
// tools/seal_streamkeys.py. The key that opens it is not in the source: the
// tool keeps it in streaming-keys.key, outside git, and the Makefile compiles
// it into the player. That keeps the keys out of an unpacked image; it does not
// keep them from whoever takes the binary apart.
//
// A plain streaming-keys.ini next to it is still read when there is no .bin,
// for the simulator and for builds made without a key. Which of the two a file
// is, is decided by its first bytes, not its name.
//
// Without either, or without a section, the matching service disables itself
// and its menu entry says so instead of failing halfway through a login.
//
// The path can be changed with [streaming] keys_file in device_config.ini, to
// keep the keys on the card instead of the read-only rootfs -- the easy path
// the day a service changes its keys.

#include "src/system/core/respath.h"

#define STREAMKEYS_PATH SONIX_RESOURCE_DIR "/components/streaming-keys.bin"
#define STREAMKEYS_PLAIN_PATH SONIX_RESOURCE_DIR "/components/streaming-keys.ini"

// Reads the file. Call once at startup, after config_init(). A missing file is
// not an error.
void streamkeys_init(void);

// NULL when absent. The pointers stay valid for the life of the program.
const char *streamkeys_qobuz_app_id(void);
const char *streamkeys_qobuz_app_secret(void);
const char *streamkeys_tidal_client_id(void);
const char *streamkeys_tidal_client_secret(void);
// Podcast Index. This pair identifies this player rather than a HiBy
// application: it is requested free on podcastindex.org, and each request is
// signed with the SHA-1 of key + secret + time. Same rule as the other two:
// out of the source, in a file whoever assembles the firmware fills in.
const char *streamkeys_podcast_key(void);
const char *streamkeys_podcast_secret(void);
// Last.fm. An API account of this player, requested on last.fm/api: it signs
// every call with the MD5 of the sorted parameters plus the secret.
const char *streamkeys_lastfm_key(void);
const char *streamkeys_lastfm_secret(void);

// Yandex Music: the desktop application's OAuth client, the key that signs
// get-file-info, and the X-Yandex-Music-Client header line it sends ("Name:
// value", whole). Section [yandex]: client_id, client_secret, sign_key,
// client_header.
const char *streamkeys_yandex_client_id(void);
const char *streamkeys_yandex_client_secret(void);
const char *streamkeys_yandex_sign_key(void);
const char *streamkeys_yandex_client_header(void);

// Where they were read from, for the log and the information page. NULL when
// none were found.
const char *streamkeys_source(void);

#endif /* STREAMKEYS_H */
