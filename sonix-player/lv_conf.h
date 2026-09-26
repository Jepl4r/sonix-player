#ifndef LV_CONF_H
#define LV_CONF_H

/* Defining this at all tells lv_conf_internal.h to skip this file, and it
 * tests for the name rather than the value. Harmless here only because
 * lv_conf_internal.h runs its test before including this file. */
#define LV_CONF_SKIP 0

/* RGB565, which is what the panel scans out. */
#define LV_COLOR_DEPTH 16

/* malloc, string and sprintf from the C library rather than LVGL's own. */
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_CLIB

/* LVGL's log is off by default, and its assert handler is while(1). A failed
 * allocation -- LV_USE_ASSERT_MALLOC is on -- therefore stops the interface
 * dead without a word and without exiting, which on a device with no console
 * cannot be told apart from a lock-up. Turning the log on leaves the halting
 * as it is and gets the line that names it into the log the player already
 * writes. Warnings and errors only. */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1
/* The player's log puts the time on every line (src/system/core/logging.c);
 * LVGL's own "(373.404, +373404)" would only repeat it. */
#define LV_LOG_USE_TIMESTAMP 0
#define LV_USE_STDLIB_STRING  LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

/* No font is built into the binary: every string is rendered through FreeType
 * from the faces in /usr/resource/sonix/fonts (see src/gui/fonts/fonts.c). */
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_MONTSERRAT_16 0
#define LV_FONT_MONTSERRAT_28 0

/* Declared by hand rather than through LV_FONT_DECLARE, which would add
 * `const`: the objects are filled in at startup by fonts_init(). Keep this
 * list in step with src/gui/fonts/fonts.h: same symbols, same types. */
#define LV_FONT_CUSTOM_DECLARE                                                                                         \
	extern lv_font_t font_ui_14;                                                                                       \
	extern lv_font_t font_ui_16;                                                                                       \
	extern lv_font_t font_ui_18;                                                                                       \
	extern lv_font_t font_ui_20;                                                                                       \
	extern lv_font_t font_ui_22;                                                                                       \
	extern lv_font_t font_ui_24;                                                                                       \
	extern lv_font_t font_ui_24_bold;                                                                                  \
	extern lv_font_t font_ui_26;                                                                                       \
	extern lv_font_t font_ui_28;                                                                                       \
	extern lv_font_t font_ui_32;

/* Used by every widget that does not ask for a specific font. */
#define LV_FONT_DEFAULT &font_ui_14

/* FT_New_Face maps the font files and decodes nothing up front; rendered
 * glyphs sit in the shared FTC cache sized below, so text costs a bounded
 * amount of memory however much of it is on screen. */
#define LV_USE_FREETYPE 1
#define LV_FREETYPE_USE_LVGL_PORT 0
#define LV_FREETYPE_CACHE_FT_GLYPH_CNT 256

/* The power menu photographs the screen behind it for its blurred backdrop. */
#define LV_USE_SNAPSHOT 1

/* The search field. The keyboard under it is this player's own, built out of
 * plain buttons (src/gui/shell/keyboard.c), so lv_keyboard stays off. */
#define LV_USE_TEXTAREA 1

/* Sixty frames a second. This is also the period of every input device's read
 * timer: lv_indev.c creates it with LV_DEF_REFR_PERIOD, so the touch controller
 * is polled at the same rate the screen is drawn.
 *
 * It costs CPU only during animations and scrolling, and on this device that is
 * time the audio decoder does not get. Put it back to 33 if a scroll ever costs
 * dropouts. */
#define LV_DEF_REFR_PERIOD 16

/* No press animation: the default theme grows a button and fades its colour
 * over 80 ms, which on this hardware reads as lag. */
#define LV_THEME_DEFAULT_GROW 0
#define LV_THEME_DEFAULT_TRANSITION_TIME 0

/* The QR code on the Tidal sign-in page, which carries a phone straight to
 * link.tidal.com with the code already filled in. */
#define LV_USE_QRCODE 1

/* The widgets that are never built. LVGL turns most of them on by default and
 * the Makefile links every source in the library, so without --gc-sections an
 * unused widget is still carried. Switching off one that is used fails to link.
 *
 * Kept on: arc (the AirPods battery rings), bar, button, chart (the parametric
 * equaliser's curve), image, label, roller (the date and time page), slider,
 * switch, textarea, flex, and the default theme. */
#define LV_USE_ARCLABEL 0

#define LV_USE_ANIMIMG 0
#define LV_USE_CALENDAR 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LED 0
#define LV_USE_LINE 0
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_MSGBOX 0
#define LV_USE_SCALE 0
/* Runs of different faces flowing as one wrapped paragraph, which a label
 * cannot do. The ebook reader paginates against the height lv_spangroup
 * reports before drawing (lv_spangroup_get_expand_height). */
#define LV_USE_SPAN 1
#define LV_USE_SPINBOX 0
/* The spinner on screen is this player's own: src/gui/shell/spinner.c turns
 * one image with lv_image_set_rotation. */
#define LV_USE_SPINNER 0
#define LV_USE_TABLE 0
#define LV_USE_TABVIEW 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0

/* On although nothing here calls them: lv_qrcode is built on a canvas, and
 * lv_api_map_v8.h names lv_buttonmatrix_ctrl_t unguarded, so the type must
 * exist for any file that includes lvgl.h. */
#define LV_USE_CANVAS 1
#define LV_USE_BUTTONMATRIX 1

/* Every page is built with flex. Nothing calls lv_obj_set_grid_*. */
#define LV_USE_GRID 0

/* The data-binding layer (lv_subject/lv_observer). Nothing uses it. */
#define LV_USE_OBSERVER 0

/* Alternatives to the default theme, which nothing here selects. */
#define LV_USE_THEME_MONO 0
#define LV_USE_THEME_SIMPLE 0

/* The display and input drivers: SDL in the simulator, framebuffer and evdev
 * on the device. */
#ifdef HOST_BUILD
  #define LV_USE_SDL 1
  #define LV_SDL_INCLUDE_PATH <SDL2/SDL.h>
  #define LV_USE_LINUX_FBDEV 0
  #define LV_USE_EVDEV 0
#else
  #define LV_USE_SDL 0
  #define LV_USE_LINUX_FBDEV 1
  #define LV_USE_EVDEV 1
#endif

#endif /* LV_CONF_H */
