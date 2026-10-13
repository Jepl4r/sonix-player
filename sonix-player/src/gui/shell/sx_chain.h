#ifndef GUI_SX_CHAIN_H
#define GUI_SX_CHAIN_H

#include <stdbool.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Sonix UI 2.0: the signal chain in one line
//
// Principle 8, "the sound is always visible": the mini player (and, in phase
// 2, the player) says what the music goes out as. With Bluetooth headphones it
// is the link -- "LDAC 990 · 96 kHz", or "LDAC 990 · 44.1 → 48 kHz" when the
// track is converted -- the same words as the Now Playing Bluetooth line
// (btline.h); on the jack, the file and its rate -- "FLAC · 44.1 kHz".
//
// `warn` is the handoff's amber: the headphones were asked for a codec (the
// Bluetooth page's choice, remembered per device) and the link runs another
// one -- AAC where LDAC was wanted. Without a choice there is nothing to fall
// short of, and it stays false.
//
// Cache reads and string work, no I/O: cheap enough for the player's poll.
// UI thread.
// ---------------------------------------------------------------------------

// Fills `out` and returns true; "" and false when nothing is loaded.
bool sx_chain_short(char *out, size_t size, bool *warn);

#endif /* GUI_SX_CHAIN_H */
