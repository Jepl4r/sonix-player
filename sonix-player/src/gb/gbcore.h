#ifndef GBCORE_H
#define GBCORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The gate between the player, which is C, and the Game Boy core, which is C++.
//
// The core is Gearboy (Ignacio Sanchez, GPLv3+), dropped under src/gb/core
// unmodified -- see src/gb/README.md for why this one and not another. Only
// what is needed to run it is here: load a ROM, run a frame, hand back pixels
// and samples, take the keys, save the battery RAM.
//
// The core already produces RGB565, the framebuffer format of this device:
// between the emulator and the panel there is no conversion, only the 3x
// upscale.

#ifdef __cplusplus
extern "C" {
#endif

// The Game Boy screen size. Times 3 it is 480x432, exactly the panel width and
// 432 of its 720 rows.
#define GB_WIDTH 160
#define GB_HEIGHT 144

// How large the pixel buffer must be, which is not GB_WIDTH*GB_HEIGHT.
//
// This line cost a device that had to be reset by hand. Gearboy can also do
// Super Game Boy, and an "SGB enhanced" cartridge switches it on by itself via
// byte 0x146 of the ROM: from then on RenderSGBFrame() draws 256x224 -- the
// Super Game Boy border around the picture -- into the caller's buffer. That is
// 57,344 pixels where 23,040 fit: 68 KB written past the end, every frame. The
// heap falls apart, the audio emits random frequencies and the device locks up.
//
// SGB is now disabled (gb_core_create tells the core so), so 160x144 would be
// enough in theory -- and "in theory" is exactly what was assumed before. The
// buffer is allocated at the worst-case size, 66 KB, so the problem cannot come
// back even if someone re-enables the border one day.
#define GB_FRAME_MAX_PIXELS (256 * 224)

// How many audio samples one frame can produce. At 44100 Hz a 59.7 Hz frame
// makes 739 per channel; more than double that leaves margin for long frames (a
// disabled LCD stretches the loop).
#define GB_AUDIO_MAX_SAMPLES 4096

// The keys, with the same values Gearboy uses internally (definitions.h): they
// are already a bit mask, so the state of all eight fits in one integer and
// nothing has to be translated in between.
#define GB_KEY_RIGHT 0x01
#define GB_KEY_LEFT 0x02
#define GB_KEY_UP 0x04
#define GB_KEY_DOWN 0x08
#define GB_KEY_A 0x10
#define GB_KEY_B 0x20
#define GB_KEY_SELECT 0x40
#define GB_KEY_START 0x80

typedef struct gb_core gb_core_t;

// Creates a core. NULL when memory runs out.
gb_core_t *gb_core_create(void);
void gb_core_destroy(gb_core_t *core);

// Loads a ROM from its file. A path rather than a buffer on purpose: this way
// the core knows where the cartridge came from, and SaveRam()/LoadRam() write
// the .sav next to the ROM by themselves, which is where anyone expects to find
// it.
//
// `force_dmg` forces original Game Boy mode even for a colour cartridge.
// Normally false: the cartridge header already says.
bool gb_core_load_file(gb_core_t *core, const char *path, bool force_dmg);

// The cartridge's internal name, for the log. Empty string with no ROM loaded.
const char *gb_core_rom_name(gb_core_t *core);

// True if the cartridge runs in Game Boy Color mode.
bool gb_core_is_color(gb_core_t *core);

// The battery-backed RAM, that is, the game's saves.
//
// `dir` is the directory to put the file in; it is named after the ROM with the
// extension changed to `.sav`. NULL means "next to the ROM", which is what
// Gearboy does by itself; the player always passes <card>/Games/Saves, so the
// ROM folders stay exactly as whoever copied them left them and the saves can
// be carried off together.
//
// On a cartridge without a battery they do nothing, which is fine: the core
// knows and needs no asking.
void gb_core_load_ram(gb_core_t *core, const char *dir);
void gb_core_save_ram(gb_core_t *core, const char *dir);

// Save states: the whole machine frozen, not just the cartridge battery. Same
// `dir`, and the file is named after the ROM with extension `.state<index>`.
//
// One slot for now (index 1), because the menu offers one. The number is there
// anyway: adding more is a line in the menu, not a change to the format of
// files already written.
//
// Must be called on the emulator thread: they read and write the core state
// while no frame is running.
//
// They go through an in-memory buffer and a single fwrite/fread instead of the
// std::ofstream Gearboy would use: serialization is made of hundreds of small
// writes, and hundreds of round trips to a microSD are noticeable. See
// gbcore.cpp.
bool gb_core_save_state(gb_core_t *core, const char *dir, int index);
bool gb_core_load_state(gb_core_t *core, const char *dir, int index);

// The state of all keys at once, as a mask of GB_KEY_*. The whole state rather
// than individual events because the other side is a sheet of glass: fingers
// slide from one key to the next without lifting, and reconstructing
// press/release from that movement is exactly the mistake that leaves a
// direction stuck. Here the core works out the difference.
void gb_core_set_keys(gb_core_t *core, uint16_t mask);

// Runs one frame. `pixels` receives GB_WIDTH*GB_HEIGHT RGB565 values.
//
// `audio` may be NULL: the samples are still generated -- the APU sits inside
// the CPU loop and cannot be switched off from outside without distorting the
// timing -- but are not copied out. `audio_samples` always receives how many
// were produced (interleaved, two channels).
void gb_core_run_frame(gb_core_t *core, uint16_t *pixels, int16_t *audio, int *audio_samples);

// The rate at which the APU produces samples. Must be set before the first
// frame runs.
void gb_core_set_sample_rate(gb_core_t *core, int rate);

// How the core was built, for the log: says whether it is in accurate or fast
// mode, the only thing that really changes the numbers.
const char *gb_core_build(void);

// The four-grey palette of the original Game Boy (no effect on colour games):
// four RGB colours, lightest to darkest. Can be called at any time, including
// mid-game.
void gb_core_set_dmg_palette(gb_core_t *core, const uint8_t colors[4][3]);

// Game Boy Color colour correction: the real screen is less saturated than the
// numbers suggest, and games are drawn for that screen.
void gb_core_set_color_correction(gb_core_t *core, bool enabled);

// The real boot ROMs (the scrolling Nintendo logo): loaded and enabled before
// gb_core_load_file, which is where the reset decides whether to use them.
// False if either file cannot be read.
bool gb_core_set_bootroms(gb_core_t *core, const char *dmg_path, const char *cgb_path);

#ifdef __cplusplus
}
#endif

#endif /* GBCORE_H */
