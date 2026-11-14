#include "lang.h"

// LVGL 9.2 made lv_roller_t opaque, and there is no lv_roller_get_mode():
// re-setting the options needs the mode the roller already has.
#include "lvgl/src/widgets/roller/lv_roller_private.h"

#include "src/system/core/config.h"

#include "lvgl/lvgl.h"
#include "lvgl/src/display/lv_display_private.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// A language is a table: one arena holding all the text, one array of entries
// pointing into it, and a hash index over the keys. Two of these exist for a
// moment during a switch, which is why it is a struct rather than a set of
// globals -- what is on screen is in the language being left and can only be
// read with its table, while the new text has to come from the other.
#define ARENA_STEP (32 * 1024)
#define MAX_ENTRIES 2048
#define BUCKETS 1024

typedef struct {
	uint32_t key_at; // offsets into the arena, not pointers: it moves as it grows
	uint32_t value_at;
	int next;		// index of the next entry in this bucket, -1 at the end
	int value_next; // the same chain for the by-value index below
} entry_t;

typedef struct {
	char *arena;
	size_t used, size;
	entry_t entries[MAX_ENTRIES];
	int count;
	int buckets[BUCKETS];
	// The same index the other way round, keyed on the translation rather than
	// on the tag. The re-labelling below asks nine hundred-odd questions of it
	// for every label on every page at once, and walking the whole table for
	// each of them is the language change taking seconds rather than a moment.
	int value_buckets[BUCKETS];
} table_t;

static table_t live;
static char current[64] = "Italiano";
static char dir_path[256] = LANG_DIR;

// ---------------------------------------------------------------------------
// the table
// ---------------------------------------------------------------------------

static unsigned hash_of(const char *s) {
	unsigned h = 2166136261u; // FNV-1a
	while (*s) {
		h ^= (unsigned char)*s++;
		h *= 16777619u;
	}
	return h;
}

static void table_reset(table_t *t) {
	free(t->arena);
	t->arena = NULL;
	t->used = t->size = 0;
	t->count = 0;
	for (int i = 0; i < BUCKETS; i++) {
		t->buckets[i] = -1;
		t->value_buckets[i] = -1;
	}
}

static const char *at(const table_t *t, uint32_t off) { return t->arena + off; }
static const char *entry_key(const table_t *t, int i) { return at(t, t->entries[i].key_at); }
static const char *entry_value(const table_t *t, int i) { return at(t, t->entries[i].value_at); }

// Offsets rather than pointers: realloc moves the arena, and an entry holding
// a pointer into the old block would be left dangling with nothing to say so.
static bool arena_put(table_t *t, const char *s, uint32_t *out) {
	size_t len = strlen(s);
	if (t->used + len + 1 > t->size) {
		size_t want = t->size;
		while (want < t->used + len + 1) {
			want += ARENA_STEP;
		}
		char *grown = realloc(t->arena, want);
		if (!grown) {
			return false;
		}
		t->arena = grown;
		t->size = want;
	}
	*out = (uint32_t)t->used;
	memcpy(t->arena + t->used, s, len + 1);
	t->used += len + 1;
	return true;
}

static void table_add(table_t *t, const char *key, const char *value) {
	// An empty translation means "not done yet", not "show nothing".
	if (t->count >= MAX_ENTRIES || !key[0] || !value[0]) {
		return;
	}

	uint32_t k, v;
	if (!arena_put(t, key, &k) || !arena_put(t, value, &v)) {
		return;
	}

	unsigned b = hash_of(at(t, k)) % BUCKETS;
	unsigned vb = hash_of(at(t, v)) % BUCKETS;
	t->entries[t->count].key_at = k;
	t->entries[t->count].value_at = v;
	t->entries[t->count].next = t->buckets[b];
	t->entries[t->count].value_next = t->value_buckets[vb];
	t->buckets[b] = t->count;
	t->value_buckets[vb] = t->count;
	t->count++;
}

static const char *table_find(const table_t *t, const char *key) {
	if (t->count == 0) {
		return NULL;
	}
	for (int i = t->buckets[hash_of(key) % BUCKETS]; i >= 0; i = t->entries[i].next) {
		if (strcmp(entry_key(t, i), key) == 0) {
			return entry_value(t, i);
		}
	}
	return NULL;
}

// The other direction, for the re-labelling below: what is on screen is the
// translation and the key has to be recovered from it.
//
// Two tags can carry the same text in one language and different text in
// another, so which of them answers has to be settled: the lowest entry, which
// is the one nearest the top of the file, exactly as a walk from the start
// would have found. The chain is built by prepending, so it is walked to the
// end rather than stopped at the first match.
static const char *table_find_by_value(const table_t *t, const char *value) {
	int best = -1;
	for (int i = t->value_buckets[hash_of(value) % BUCKETS]; i >= 0; i = t->entries[i].value_next) {
		if (strcmp(entry_value(t, i), value) == 0 && (best < 0 || i < best)) {
			best = i;
		}
	}
	return best >= 0 ? entry_key(t, best) : NULL;
}

// ---------------------------------------------------------------------------
// files
// ---------------------------------------------------------------------------

static char *trim(char *s) {
	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
		s++;
	}
	char *end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
		end--;
	}
	*end = '\0';
	return s;
}

// The escapes a one-line INI needs: a translation may want a line break, and
// it has to be able to write a backslash.
static void unescape(char *s) {
	char *w = s;
	for (const char *r = s; *r; r++) {
		if (*r != '\\' || !r[1]) {
			*w++ = *r;
			continue;
		}
		r++;
		switch (*r) {
		case 'n':
			*w++ = '\n';
			break;
		case 't':
			*w++ = '\t';
			break;
		default:
			*w++ = *r; // including a doubled backslash
			break;
		}
	}
	*w = '\0';
}

static bool load_file(table_t *t, const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	table_reset(t);

	char line[1024]; // twice the longest sentence in the interface
	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line);
		if (!s[0] || s[0] == '#' || s[0] == ';' || s[0] == '[') {
			continue;
		}
		// The FIRST '=' only: a translation is allowed to contain one, and no
		// key does -- every key here is a C string literal from the source.
		char *eq = strchr(s, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		char *key = trim(s);
		char *value = trim(eq + 1);
		unescape(key);
		unescape(value);
		table_add(t, key, value);
	}

	fclose(f);
	return true;
}

static void path_for(char *out, size_t out_size, const char *name) {
	snprintf(out, out_size, "%s/%s.ini", dir_path, name);
}

// ---------------------------------------------------------------------------
// re-labelling what is already drawn
//
// Every page is built once, at startup, and the text it was given is a copy
// held inside the label. There is therefore nothing to ask the pages to redo
// -- and asking fifty files to each grow a refresh callback would be fifty
// chances to forget one. Instead the object tree is walked and every label
// whose text is recognised is set again.
//
// Recognised means: it is a value in the table being left (so the key can be
// recovered from it), or it is already that key. Anything else -- a song title,
// a formatted count, a file name -- is left alone. A track really called
// "Album" would be re-labelled until the list is next filled from the database,
// and that is the price of recovering keys from the text on screen.
// ---------------------------------------------------------------------------

static const table_t *from_table;
static const table_t *to_table;

static const char *convert_text(const char *shown) {
	const char *key = table_find_by_value(from_table, shown);
	if (!key) {
		key = shown; // already the source language
	}
	const char *now = table_find(to_table, key);
	return now ? now : key;
}

static void relabel_tree(lv_obj_t *obj) {
	if (!obj) {
		return;
	}

	// A roller is not a label: its options are one multi-line string it parses
	// into rows, and it keeps a count and a selection derived from them. The
	// month names live in one, so setting the label a roller draws with would
	// change what is on screen and leave the roller believing something else.
	// It gets the whole option string translated at once -- which is exactly
	// how it is written in the source, and therefore how it is keyed -- and
	// the selected row put back, because set_options resets it to the first.
	if (lv_obj_check_type(obj, &lv_roller_class)) {
		const char *options = lv_roller_get_options(obj);
		if (options && options[0]) {
			const char *now = convert_text(options);
			if (strcmp(now, options) != 0) {
				uint32_t selected = lv_roller_get_selected(obj);
				lv_roller_set_options(obj, now, ((lv_roller_t *)obj)->mode);
				lv_roller_set_selected(obj, selected, LV_ANIM_OFF);
			}
		}
		return; // never descend: the label inside is the roller's own business
	}

	// A text field's placeholder ("Search...", "Name...", "Password") is neither
	// a label nor a child: it is a string inside the widget that the textarea
	// draws itself while the field is empty. The walk below never saw it, so it
	// stayed forever in the language the page was built in at startup.
	//
	// The typed text is not touched: somebody entered it, and a typed word that
	// happens to resemble an interface string is not an interface string.
	if (lv_obj_check_type(obj, &lv_textarea_class)) {
		const char *hint = lv_textarea_get_placeholder_text(obj);
		if (hint && hint[0]) {
			const char *now = convert_text(hint);
			if (strcmp(now, hint) != 0) {
				lv_textarea_set_placeholder_text(obj, now);
			}
		}
		// Never descend: the label inside a textarea holds the typed text, which
		// must never be translated. Searching for "Album" and then changing
		// language must not rewrite what was searched for.
		return;
	}

	if (lv_obj_check_type(obj, &lv_label_class)) {
		// A label set to LV_LABEL_LONG_DOT does not keep the text it was
		// given: once LVGL has decided the text is too long it overwrites the
		// tail with dots, remembers the characters it covered, and
		// lv_label_get_text() then hands back the shortened form. A page built
		// at startup has never been laid out at its real width, so the dots
		// start at the first character and the whole string reads as "..." --
		// which matches nothing, and the heading stays in the old language.
		//
		// lv_label_set_text(obj, NULL) is LVGL's own "put the characters back
		// and refresh": it reverts the dots in place and leaves the text
		// alone. Changing the long mode does not, whatever it once did --
		// lv_label_set_long_mode() only marks the label for a refresh that has
		// not happened yet by the time the text is read back.
		//
		// Asked only of the labels that can carry dots at all: this walk sees
		// every label on every page, and the call reallocates the string.
		if (lv_label_get_long_mode(obj) == LV_LABEL_LONG_DOT) {
			lv_label_set_text(obj, NULL);
		}

		const char *text = lv_label_get_text(obj);
		if (text && text[0]) {
			const char *now = convert_text(text);
			if (strcmp(now, text) != 0) {
				lv_label_set_text(obj, now);
			}
		}
	}
	uint32_t n = lv_obj_get_child_count(obj);
	for (uint32_t i = 0; i < n; i++) {
		relabel_tree(lv_obj_get_child(obj, i));
	}
}

static void relabel_everything(const table_t *from, const table_t *to) {
	from_table = from;
	to_table = to;

	lv_display_t *disp = lv_display_get_default();
	if (!disp) {
		return;
	}
	for (uint32_t i = 0; i < disp->screen_cnt; i++) {
		relabel_tree(disp->screens[i]);
	}
	// The overlays do not live on a screen: the top bar, the quick panel, the
	// pop-overs and the toasts are all on the layers.
	relabel_tree(lv_display_get_layer_bottom(disp));
	relabel_tree(lv_display_get_layer_top(disp));
	relabel_tree(lv_display_get_layer_sys(disp));

	from_table = to_table = NULL;
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

void lang_init(const char *dir) {
	if (dir && dir[0]) {
		snprintf(dir_path, sizeof(dir_path), "%s", dir);
	}
	table_reset(&live);

	// With no line in the config nobody has chosen yet: this is the first boot,
	// and it starts in English -- not because English is better, but because it
	// is the one language whoever meets the welcome screen has some chance of
	// reading whatever theirs is, and the first thing that screen asks is which
	// language to continue in.
	const char *chosen = config_get("ui", "language", NULL);
	snprintf(current, sizeof(current), "%s", chosen ? chosen : LANG_FIRST_BOOT);

	char path[512];
	path_for(path, sizeof(path), current);
	if (load_file(&live, path)) {
		printf("lang: %s, %d texts from %s\n", current, live.count, path);
	} else {
		// Since the move to tags the source holds no Italian: it holds tags
		// ("podcast_search_for_a_podcast"), and every language, Italian included, is a
		// file to load. Without its file the interface shows bare tags --
		// deliberately ugly, because that is the unmistakable sign the .ini
		// files were not installed, not a case to paper over. Italian is still
		// tried as a fallback, being the file most likely to be present.
		fprintf(stderr, "lang: no %s.ini in %s: THE TAGS WILL SHOW. Install the language files.\n", current,
				dir_path);
		if (strcmp(current, "Italiano") != 0) {
			path_for(path, sizeof(path), "Italiano");
			if (load_file(&live, path)) {
				snprintf(current, sizeof(current), "Italiano");
				printf("lang: falling back to Italiano, %d texts\n", live.count);
			}
		}
	}
}

const char *tr(const char *text) {
	if (!text) {
		return "";
	}
	const char *found = table_find(&live, text);
	return found ? found : text;
}

const char *lang_current(void) { return current; }

static bool read_declared_name(const char *name, char *out, size_t size);

// The order the list is shown in. The key is the file name, not the name the
// file declares: a declared name stays in its own script (CJK, Cyrillic) and
// has no place in an A-Z list, while the file names are the English names the
// set is organised by.
//
// A Latin letter written with a mark, a stroke or a ligature folds to the plain
// letter a reader looks for it under: the Czech name with a caron sorts with C,
// the Spanish one with a tilde with E, the Vietnamese one with a dot below with
// E, the OE ligature with O. The table below covers every Latin block a name can
// be written in -- the Supplement, the two Extended blocks and the Additional
// block the Vietnamese letters live in -- and leaves out only the code points
// that stand for two letters at once and so have no single letter to fold to
// (the clicks, "oi", "ou", "hv", the glottal stop, the multiplication and
// division signs). Those, and every script outside Latin, keep their code point
// and therefore follow the Latin names.
static uint32_t utf8_next(const char **p) {
	const unsigned char *s = (const unsigned char *)*p;
	uint32_t cp;

	if (s[0] < 0x80) {
		cp = s[0];
		*p += 1;
	} else if ((s[0] & 0xE0) == 0xC0 && s[1]) {
		cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
		*p += 2;
	} else if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) {
		cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
		*p += 3;
	} else if ((s[0] & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
		cp = ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) | ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
		*p += 4;
	} else {
		cp = s[0];
		*p += 1;
	}
	return cp;
}

// One entry per run of code points that fold to the same letter. The blocks lay
// each letter out as a capital and a small form, with its marks in between, so
// a run usually covers a whole letter and stops where the next one starts.
static const struct {
	uint32_t first;
	uint32_t last;
	char base;
} latin_folds[] = {
	// Latin-1 Supplement, U+00C0-U+00FF.
	{0x00C0, 0x00C6, 'a'}, // A grave .. AE ligature
	{0x00C7, 0x00C7, 'c'},
	{0x00C8, 0x00CB, 'e'},
	{0x00CC, 0x00CF, 'i'},
	{0x00D0, 0x00D0, 'd'}, // ETH
	{0x00D1, 0x00D1, 'n'},
	{0x00D2, 0x00D6, 'o'},
	{0x00D8, 0x00D8, 'o'}, // O stroke; U+00D7 is the multiplication sign
	{0x00D9, 0x00DC, 'u'},
	{0x00DD, 0x00DD, 'y'},
	{0x00DE, 0x00DE, 't'}, // THORN
	{0x00DF, 0x00DF, 's'}, // sharp s
	{0x00E0, 0x00E6, 'a'},
	{0x00E7, 0x00E7, 'c'},
	{0x00E8, 0x00EB, 'e'},
	{0x00EC, 0x00EF, 'i'},
	{0x00F0, 0x00F0, 'd'},
	{0x00F1, 0x00F1, 'n'},
	{0x00F2, 0x00F6, 'o'},
	{0x00F8, 0x00F8, 'o'}, // O stroke; U+00F7 is the division sign
	{0x00F9, 0x00FC, 'u'},
	{0x00FD, 0x00FD, 'y'},
	{0x00FE, 0x00FE, 't'},
	{0x00FF, 0x00FF, 'y'},

	// Latin Extended-A, U+0100-U+017F. The strokes and the IJ ligature have no
	// decomposition to the letter under them and are listed with the rest.
	{0x0100, 0x0105, 'a'},
	{0x0106, 0x010D, 'c'},
	{0x010E, 0x0111, 'd'}, // .. D stroke
	{0x0112, 0x011B, 'e'},
	{0x011C, 0x0123, 'g'},
	{0x0124, 0x0127, 'h'}, // .. H stroke
	{0x0128, 0x0133, 'i'}, // .. dotless I, IJ ligature
	{0x0134, 0x0135, 'j'},
	{0x0136, 0x0138, 'k'}, // .. kra
	{0x0139, 0x0142, 'l'}, // .. L and l with stroke
	{0x0143, 0x014B, 'n'}, // .. apostrophe N, eng
	{0x014C, 0x0153, 'o'}, // .. OE ligature
	{0x0154, 0x0159, 'r'},
	{0x015A, 0x0161, 's'},
	{0x0162, 0x0167, 't'}, // .. T stroke
	{0x0168, 0x0173, 'u'},
	{0x0174, 0x0175, 'w'},
	{0x0176, 0x0178, 'y'},
	{0x0179, 0x017E, 'z'},
	{0x017F, 0x017F, 's'}, // long s

	// Latin Extended-B, U+0180-U+024F. This block is mostly the phonetic
	// letters of the International Phonetic Alphabet; the ones that are a base
	// letter with a hook, a bar or a stroke rather than a shape of their own are
	// folded with it.
	{0x0184, 0x0185, 'b'},
	{0x0186, 0x0186, 'o'}, // open O
	{0x018E, 0x0190, 'e'}, // reversed E, schwa, open E
	{0x0196, 0x0197, 'i'}, // iota, barred I
	{0x019C, 0x019C, 'm'},
	{0x019D, 0x019E, 'n'},
	{0x01A0, 0x01A1, 'o'}, // O with horn
	{0x01A6, 0x01A6, 'r'},
	{0x01A9, 0x01A9, 's'}, // esh
	{0x01AF, 0x01B1, 'u'}, // U with horn, upsilon
	{0x01B2, 0x01B2, 'v'},
	{0x01B7, 0x01BA, 'z'}, // ezh
	{0x01BF, 0x01BF, 'w'}, // wynn
	{0x01C4, 0x01C6, 'd'}, // DZ ligature
	{0x01C7, 0x01C9, 'l'}, // LJ ligature
	{0x01CA, 0x01CC, 'n'}, // NJ ligature
	{0x01CD, 0x01CE, 'a'},
	{0x01CF, 0x01D0, 'i'},
	{0x01D1, 0x01D2, 'o'},
	{0x01D3, 0x01DC, 'u'},
	{0x01DD, 0x01DD, 'e'}, // turned e
	{0x01DE, 0x01E3, 'a'}, // .. AE with macron
	{0x01E4, 0x01E7, 'g'}, // G stroke, G caron
	{0x01E8, 0x01E9, 'k'},
	{0x01EA, 0x01ED, 'o'},
	{0x01EE, 0x01EF, 'z'}, // ezh with caron
	{0x01F0, 0x01F0, 'j'},
	{0x01F1, 0x01F3, 'd'}, // DZ ligature
	{0x01F4, 0x01F5, 'g'},
	{0x01F7, 0x01F7, 'w'}, // wynn
	{0x01F8, 0x01F9, 'n'},
	{0x01FA, 0x01FD, 'a'}, // .. AE with acute
	{0x01FE, 0x01FF, 'o'}, // O stroke with acute
	{0x0200, 0x0203, 'a'},
	{0x0204, 0x0207, 'e'},
	{0x0208, 0x020B, 'i'},
	{0x020C, 0x020F, 'o'},
	{0x0210, 0x0213, 'r'},
	{0x0214, 0x0217, 'u'},
	{0x0218, 0x0219, 's'}, // S with comma below
	{0x021A, 0x021B, 't'}, // T with comma below
	{0x021E, 0x021F, 'h'},
	{0x0220, 0x0220, 'n'},
	{0x0221, 0x0221, 'd'},
	{0x0226, 0x0227, 'a'},
	{0x0228, 0x0229, 'e'},
	{0x022A, 0x0231, 'o'},
	{0x0232, 0x0233, 'y'},
	{0x0234, 0x0234, 'l'},
	{0x0235, 0x0235, 'n'},
	{0x0236, 0x0236, 't'},
	{0x0237, 0x0237, 'j'}, // dotless j
	{0x023A, 0x023A, 'a'},
	{0x023B, 0x023C, 'c'}, // C with stroke
	{0x023D, 0x023D, 'l'},
	{0x023E, 0x023E, 't'},
	{0x023F, 0x023F, 's'},
	{0x0240, 0x0240, 'z'},
	{0x0243, 0x0243, 'b'}, // B with stroke
	{0x0244, 0x0244, 'u'},
	{0x0245, 0x0245, 'v'}, // turned V
	{0x0246, 0x0247, 'e'}, // E with stroke
	{0x0248, 0x0249, 'j'},
	{0x024A, 0x024B, 'q'},
	{0x024C, 0x024D, 'r'},
	{0x024E, 0x024F, 'y'},

	// Latin Extended Additional, U+1E00-U+1EFF. The runs from U+1EA0 on are the
	// Vietnamese letters: a base letter, a tone mark and a vowel mark together.
	{0x1E00, 0x1E01, 'a'},
	{0x1E02, 0x1E07, 'b'},
	{0x1E08, 0x1E09, 'c'},
	{0x1E0A, 0x1E13, 'd'},
	{0x1E14, 0x1E1D, 'e'},
	{0x1E1E, 0x1E1F, 'f'},
	{0x1E20, 0x1E21, 'g'},
	{0x1E22, 0x1E2B, 'h'},
	{0x1E2C, 0x1E2F, 'i'},
	{0x1E30, 0x1E35, 'k'},
	{0x1E36, 0x1E3D, 'l'},
	{0x1E3E, 0x1E43, 'm'},
	{0x1E44, 0x1E4B, 'n'},
	{0x1E4C, 0x1E53, 'o'},
	{0x1E54, 0x1E57, 'p'},
	{0x1E58, 0x1E5F, 'r'},
	{0x1E60, 0x1E69, 's'},
	{0x1E6A, 0x1E71, 't'},
	{0x1E72, 0x1E7B, 'u'},
	{0x1E7C, 0x1E7F, 'v'},
	{0x1E80, 0x1E89, 'w'},
	{0x1E8A, 0x1E8D, 'x'},
	{0x1E8E, 0x1E8F, 'y'},
	{0x1E90, 0x1E95, 'z'},
	{0x1E96, 0x1E96, 'h'},
	{0x1E97, 0x1E97, 't'},
	{0x1E98, 0x1E98, 'w'},
	{0x1E99, 0x1E99, 'y'},
	{0x1E9A, 0x1E9A, 'a'},
	{0x1E9B, 0x1E9E, 's'}, // long s with dot or stroke, sharp S
	{0x1E9F, 0x1E9F, 'd'}, // delta
	{0x1EA0, 0x1EB7, 'a'},
	{0x1EB8, 0x1EC7, 'e'},
	{0x1EC8, 0x1ECB, 'i'},
	{0x1ECC, 0x1EE3, 'o'},
	{0x1EE4, 0x1EF1, 'u'},
	{0x1EF2, 0x1EF9, 'y'},
	{0x1EFA, 0x1EFB, 'l'}, // middle-Welsh LL
	{0x1EFE, 0x1EFF, 'y'}, // Y with loop; U+1EFC/1EFD is middle-Welsh V
};

static uint32_t folded_letter(uint32_t cp) {
	if (cp >= 'A' && cp <= 'Z') {
		return cp + ('a' - 'A');
	}
	for (size_t i = 0; i < sizeof(latin_folds) / sizeof(latin_folds[0]); i++) {
		if (cp >= latin_folds[i].first && cp <= latin_folds[i].last) {
			return (uint32_t)latin_folds[i].base;
		}
	}
	return cp;
}

static int name_order(const char *a, const char *b) {
	const char *pa = a, *pb = b;
	for (;;) {
		uint32_t ca = folded_letter(utf8_next(&pa));
		uint32_t cb = folded_letter(utf8_next(&pb));
		if (ca != cb) {
			return ca < cb ? -1 : 1;
		}
		if (!ca) {
			return 0;
		}
	}
}

int lang_list(const char **names, int max) {
	// Whatever .ini files are in the folder, and nothing else. Dropping a new
	// one in is all it takes to add a language: the list is the directory.
	// Nothing is offered that has no file behind it -- the keys in the source
	// are tags, so a language with no file would show those tags rather than
	// any readable text.
	static char found[16][64];
	int count = 0;

	DIR *d = opendir(dir_path);
	if (!d) {
		return count;
	}
	struct dirent *e;
	while ((e = readdir(d)) != NULL && count < max && count < 16) {
		const char *dot = strrchr(e->d_name, '.');
		if (!dot || strcmp(dot, ".ini") != 0) {
			continue;
		}
		size_t len = (size_t)(dot - e->d_name);
		if (len == 0 || len >= sizeof(found[0])) {
			continue;
		}
		char name[64];
		memcpy(name, e->d_name, len);
		name[len] = '\0';

		// Two files can name the same language -- the shipped set carries both
		// `Francais.ini` and its `#U00e7` spelling -- and the list must not show
		// it twice. They are compared by what they declare, not by file name.
		char label[64];
		if (!read_declared_name(name, label, sizeof(label))) {
			snprintf(label, sizeof(label), "%s", name);
		}

		bool already = false;
		for (int i = 0; i < count; i++) {
			char other[64];
			if (!read_declared_name(found[i], other, sizeof(other))) {
				snprintf(other, sizeof(other), "%.*s", (int)sizeof(other) - 1, found[i]);
			}
			if (strcmp(other, label) == 0) {
				already = true;
				break;
			}
		}
		if (already) {
			continue;
		}
		snprintf(found[count], sizeof(found[0]), "%s", name);
		count++;
	}
	closedir(d);

	// readdir() returns the folder in whatever order it likes. The list is
	// for a reader, so the names are put in alphabetical order before they
	// leave this function.
	for (int i = 1; i < count; i++) {
		char key[64];
		memcpy(key, found[i], sizeof(key));
		int j = i - 1;
		while (j >= 0 && name_order(found[j], key) > 0) {
			memcpy(found[j + 1], found[j], sizeof(found[0]));
			j--;
		}
		memcpy(found[j + 1], key, sizeof(key));
	}
	for (int i = 0; i < count; i++) {
		names[i] = found[i];
	}
	return count;
}

static int hex_digit(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

// The name a file gives for itself, from its header line:
//
//     [Language: English]
//
// Read straight off the file rather than derived from its name, so the list
// shows what the translator wrote. Returns false when the file has no such
// line, and the caller falls back to the file name.
static bool read_declared_name(const char *name, char *out, size_t size) {
	char path[512];
	if ((size_t)snprintf(path, sizeof(path), "%s/%s.ini", dir_path, name) >= sizeof(path)) {
		return false;
	}
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}

	bool got = false;
	char line[256];
	while (!got && fgets(line, sizeof(line), f)) {
		char *s = trim(line);
		if (s[0] != '[') {
			continue;
		}
		char *close = strchr(s, ']');
		if (!close) {
			continue;
		}
		*close = '\0';
		if (strncasecmp(s + 1, "language", 8) != 0) {
			continue; // some other section
		}
		char *colon = strchr(s + 1, ':');
		if (!colon) {
			continue;
		}
		char *value = trim(colon + 1);
		if (value[0]) {
			snprintf(out, size, "%s", value);
			got = true;
		}
	}

	fclose(f);
	return got;
}

void lang_display_name(const char *name, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	if (!name) {
		out[0] = '\0';
		return;
	}

	if (read_declared_name(name, out, size)) {
		return;
	}

	// No header: fall back to the file name, decoding the Windows-style
	// `#Uxxxx` escapes some of the shipped files use. See lang.h -- basic
	// multilingual plane only, since `#Uxxxx` is four hex digits and no
	// existing language name needs more.

	size_t w = 0;
	for (const char *p = name; *p && w + 1 < size;) {
		int d0, d1, d2, d3;
		if (p[0] == '#' && (p[1] == 'U' || p[1] == 'u') && (d0 = hex_digit(p[2])) >= 0 &&
			(d1 = hex_digit(p[3])) >= 0 && (d2 = hex_digit(p[4])) >= 0 && (d3 = hex_digit(p[5])) >= 0) {
			unsigned cp = (unsigned)((d0 << 12) | (d1 << 8) | (d2 << 4) | d3);

			// Code point to UTF-8, and only if the whole sequence fits: half a
			// sequence would be worse than leaving it undecoded.
			char enc[4];
			size_t n = 0;
			if (cp < 0x80) {
				enc[n++] = (char)cp;
			} else if (cp < 0x800) {
				enc[n++] = (char)(0xC0 | (cp >> 6));
				enc[n++] = (char)(0x80 | (cp & 0x3F));
			} else {
				enc[n++] = (char)(0xE0 | (cp >> 12));
				enc[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
				enc[n++] = (char)(0x80 | (cp & 0x3F));
			}
			if (w + n + 1 > size) {
				break;
			}
			for (size_t i = 0; i < n; i++) {
				out[w++] = enc[i];
			}
			p += 6;
			continue;
		}
		out[w++] = *p++;
	}
	out[w] = '\0';
}

bool lang_set(const char *name) {
	if (!name || !name[0] || strcmp(name, current) == 0) {
		return true;
	}

	// Static rather than on the stack: the table carries two indexes now and is
	// forty kilobytes, which is more than a page's worth of stack to take for
	// the length of one call.
	static table_t next;
	memset(&next, 0, sizeof(next));
	table_reset(&next);

	char path[512];
	path_for(path, sizeof(path), name);
	if (!load_file(&next, path)) {
		// With tags in the source there is no free language: Italian is a file
		// too, and an unreadable file is a failure for any language, since an
		// empty table would show bare tags.
		table_reset(&next);
		free(next.arena);
		fprintf(stderr, "lang: %s not readable\n", path);
		return false;
	}

	relabel_everything(&live, &next);

	free(live.arena);
	live = next;

	snprintf(current, sizeof(current), "%s", name);
	config_set("ui", "language", current);
	config_save();
	printf("lang: switched to %s (%d texts)\n", current, live.count);
	return true;
}

