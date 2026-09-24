#include "textview.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

lv_obj_t *textview_screen;

// The text goes into many labels of about this size rather than one: LVGL
// walks a label's text from the start on every redraw, and a page of a hundred
// kilobytes in one label would be slow to scroll.
#define CHUNK_BYTES 1500

static lv_obj_t *title_label;
static lv_obj_t *page;

// ---------------------------------------------------------------------------
// decoding
// ---------------------------------------------------------------------------

// Windows-1252 from 0x80 to 0x9F; 0 where the code page leaves a hole. From
// 0xA0 up it is Latin-1, the same numbers as Unicode.
static const uint16_t CP1252_HIGH[32] = {
	0x20AC, 0,		0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
	0x2039, 0x0152, 0,		0x017D, 0,		0,		0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
	0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,		0x017E, 0x0178,
};

// Appends code point `cp` as UTF-8. Returns the bytes written.
static size_t put_utf8(char *out, uint32_t cp) {
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

// The length of the valid UTF-8 sequence at `p`, 0 when there is none. A
// sequence cut by the end of the buffer counts as valid: the file was read up
// to a limit, not to its end.
static size_t utf8_len(const unsigned char *p, size_t left) {
	size_t need = p[0] < 0x80 ? 1 : (p[0] & 0xE0) == 0xC0 ? 2 : (p[0] & 0xF0) == 0xE0 ? 3 : (p[0] & 0xF8) == 0xF0 ? 4 : 0;
	if (need == 0 || (need == 2 && p[0] < 0xC2)) {
		return 0;
	}
	for (size_t i = 1; i < need; i++) {
		if (i >= left) {
			return left;
		}
		if ((p[i] & 0xC0) != 0x80) {
			return 0;
		}
	}
	return need;
}

static bool is_utf8(const unsigned char *p, size_t len) {
	for (size_t i = 0; i < len;) {
		size_t n = utf8_len(p + i, len - i);
		if (n == 0) {
			return false;
		}
		i += n;
	}
	return true;
}

// Adds one code point to `out`, with line endings made "\n", tabs made four
// spaces and other control characters left out.
static size_t put_char(char *out, uint32_t cp, uint32_t *prev) {
	uint32_t before = *prev;
	*prev = cp;
	if (cp == '\r') {
		out[0] = '\n';
		return 1;
	}
	if (cp == '\n') {
		if (before == '\r') {
			return 0; // the second half of "\r\n"
		}
		out[0] = '\n';
		return 1;
	}
	if (cp == '\t') {
		memcpy(out, "    ", 4);
		return 4;
	}
	if (cp < 0x20 || cp == 0x7F || cp == 0xFEFF) {
		return 0;
	}
	return put_utf8(out, cp);
}

// `raw` as UTF-8 text, newly allocated. Every input byte becomes at most four
// output bytes (a tab, or a Windows-1252 byte that needs three).
static char *decode(const unsigned char *raw, size_t len) {
	char *out = malloc(len * 4 + 1);
	if (!out) {
		return NULL;
	}
	size_t o = 0;
	uint32_t prev = 0;

	bool le = len >= 2 && raw[0] == 0xFF && raw[1] == 0xFE;
	bool be = len >= 2 && raw[0] == 0xFE && raw[1] == 0xFF;
	if (le || be) {
		for (size_t i = 2; i + 1 < len; i += 2) {
			uint32_t unit = le ? (uint32_t)(raw[i] | (raw[i + 1] << 8)) : (uint32_t)((raw[i] << 8) | raw[i + 1]);
			if (unit >= 0xD800 && unit < 0xDC00 && i + 3 < len) {
				uint32_t low = le ? (uint32_t)(raw[i + 2] | (raw[i + 3] << 8))
								  : (uint32_t)((raw[i + 2] << 8) | raw[i + 3]);
				if (low >= 0xDC00 && low < 0xE000) {
					unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
					i += 2;
				}
			}
			if (unit >= 0xD800 && unit < 0xE000) {
				continue;
			}
			o += put_char(out + o, unit, &prev);
		}
	} else if (is_utf8(raw, len)) {
		for (size_t i = 0; i < len;) {
			size_t n = utf8_len(raw + i, len - i);
			if (raw[i] < 0x80) {
				o += put_char(out + o, raw[i], &prev);
			} else if (n == 3 && raw[i] == 0xEF && raw[i + 1] == 0xBB && raw[i + 2] == 0xBF) {
				prev = 0; // a byte order mark
			} else {
				memcpy(out + o, raw + i, n);
				o += n;
				prev = 0x80;
			}
			i += n;
		}
	} else {
		for (size_t i = 0; i < len; i++) {
			uint32_t cp = raw[i] < 0x80 ? raw[i] : raw[i] < 0xA0 ? CP1252_HIGH[raw[i] - 0x80] : raw[i];
			if (cp) {
				o += put_char(out + o, cp, &prev);
			}
		}
	}

	out[o] = '\0';
	return out;
}

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

static lv_obj_t *add_label(const char *text, bool dim) {
	lv_obj_t *label = lv_label_create(page);
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_add_style(label, dim ? &theme_style_text_dim : &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_label_set_text(label, text);
	return label;
}

// Cuts `text` into labels of about CHUNK_BYTES, at a line break when there is
// one, otherwise at a space, otherwise at the start of a character.
static void fill(char *text) {
	char *p = text;
	while (*p) {
		size_t left = strlen(p);
		size_t cut = left;
		if (left > CHUNK_BYTES) {
			cut = CHUNK_BYTES;
			size_t i = cut;
			while (i > CHUNK_BYTES / 2 && p[i - 1] != '\n') {
				i--;
			}
			if (p[i - 1] != '\n') {
				i = cut;
				while (i > CHUNK_BYTES / 2 && p[i - 1] != ' ') {
					i--;
				}
				if (p[i - 1] != ' ') {
					i = cut;
					while (i > 0 && ((unsigned char)p[i] & 0xC0) == 0x80) {
						i--;
					}
				}
			}
			cut = i;
		}

		// The label gets the piece without the line break it ends on: the next
		// label starts on a line of its own anyway.
		char saved = p[cut];
		p[cut] = '\0';
		size_t shown = cut;
		if (shown > 0 && p[shown - 1] == '\n') {
			p[shown - 1] = '\0';
		}
		add_label(p, false);
		p[cut] = saved;
		p += cut;
	}
}

bool textview_open(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	unsigned char *raw = malloc(TEXTVIEW_MAX_BYTES);
	if (!raw) {
		fclose(f);
		return false;
	}
	size_t len = fread(raw, 1, TEXTVIEW_MAX_BYTES, f);
	bool more = fgetc(f) != EOF;
	bool failed = ferror(f) != 0;
	fclose(f);
	if (failed) {
		free(raw);
		return false;
	}

	char *text = decode(raw, len);
	free(raw);
	if (!text) {
		return false;
	}

	const char *slash = strrchr(path, '/');
	lv_label_set_text(title_label, slash ? slash + 1 : path);

	lv_obj_clean(page);
	if (text[0]) {
		fill(text);
	} else {
		add_label(tr("files_text_empty"), true);
	}
	if (more) {
		add_label(tr("files_text_truncated"), true);
	}
	free(text);

	lv_obj_scroll_to_y(page, 0, LV_ANIM_OFF);
	switch_screen(textview_screen);
	return true;
}

void textview_init(gui_config_t *cfg) {
	lv_obj_add_style(textview_screen, &theme_style_screen, 0);
	title_label = settingsrow_title(textview_screen, cfg, "");
	settingsrow_title_corner_slots(title_label, cfg, 0);

	int content_top = settingsrow_content_top(cfg);

	page = lv_obj_create(textview_screen);
	lv_obj_set_size(page, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(page, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(page, 0, 0);
	lv_obj_set_style_border_width(page, 0, 0);
	lv_obj_set_style_radius(page, 0, 0);
	lv_obj_set_style_pad_hor(page, cfg->padding, 0);
	lv_obj_set_style_pad_top(page, 0, 0);
	lv_obj_set_style_pad_bottom(page, cfg->padding * 2, 0);
	lv_obj_set_style_pad_row(page, 0, 0);
	lv_obj_set_scroll_dir(page, LV_DIR_VER);
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
	lv_obj_add_flag(page, LV_OBJ_FLAG_EVENT_BUBBLE);

	switcher_attach_back_gesture(textview_screen);
}
