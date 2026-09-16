#include "kblayout.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "src/system/core/config.h"
#include "src/system/core/lang.h"

// One alphabet. The rows are written out in both cases rather than folded from
// one to the other: the letters are UTF-8 and the mapping is not arithmetic --
// Cyrillic sits a whole block away from its capitals, and a rule that works for
// ASCII would silently mangle everything else. Two strings per row and the
// question does not arise.
//
// The strings are split into letters at the code point, so every letter of a
// row must be one code point, which all of these are.
typedef struct {
	const char *name;
	const char *lower[KB_LAYOUT_ROWS];
	const char *upper[KB_LAYOUT_ROWS];
	// The same alphabet shared over the nine keys of a phone keypad. Key 0 is
	// the punctuation one and is the same everywhere.
	const char *t9_lower[KB_LAYOUT_T9_KEYS];
	const char *t9_upper[KB_LAYOUT_T9_KEYS];
	const char *language; // the interface language whose default this is
} layout_info_t;

// The Latin keypad, which three of the four Latin layouts share exactly: T9
// groups letters by alphabet, and AZERTY rearranges keys rather than letters.
#define T9_LATIN_LOWER ".,?!\'", "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz"
#define T9_LATIN_UPPER ".,?!\'", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ"

static const layout_info_t layouts[KB_LAYOUT_COUNT] = {
	[KB_LAYOUT_ENGLISH] = {"English",
						   {"qwertyuiop", "asdfghjkl", "zxcvbnm"},
						   {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"},
						   {T9_LATIN_LOWER},
						   {T9_LATIN_UPPER},
						   "English"},
	// The same letters as English, which is what an Italian phone keyboard
	// has: the accented vowels are not on the letter rows there either.
	[KB_LAYOUT_ITALIAN] = {"Italiano",
						   {"qwertyuiop", "asdfghjkl", "zxcvbnm"},
						   {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"},
						   {T9_LATIN_LOWER},
						   {T9_LATIN_UPPER},
						   "Italiano"},
	// QWERTZ, with the umlauts on the rows where a German keyboard has them and
	// on the keys of the letters they are: a phone keypad shares them out that
	// way rather than giving them keys of their own.
	[KB_LAYOUT_GERMAN] = {"Deutsch",
						  {"qwertzuiop\xc3\xbc", "asdfghjkl\xc3\xb6\xc3\xa4", "yxcvbnm"},
						  {"QWERTZUIOP\xc3\x9c", "ASDFGHJKL\xc3\x96\xc3\x84", "YXCVBNM"},
						  {".,?!\'", "abc\xc3\xa4", "def", "ghi", "jkl", "mno\xc3\xb6", "pqrs\xc3\x9f", "tuv\xc3\xbc",
						   "wxyz"},
						  {".,?!\'", "ABC\xc3\x84", "DEF", "GHI", "JKL", "MNO\xc3\x96", "PQRS\xc3\x9f", "TUV\xc3\x9c",
						   "WXYZ"},
						  "Deutsch"},
	[KB_LAYOUT_SPANISH] = {"Espa\xc3\xb1ol",
						   {"qwertyuiop", "asdfghjkl\xc3\xb1", "zxcvbnm"},
						   {"QWERTYUIOP", "ASDFGHJKL\xc3\x91", "ZXCVBNM"},
						   // The only Latin keypad that differs: the enye rides
						   // the key its neighbours are on.
						   {".,?!\'", "abc", "def", "ghi", "jkl", "mno\xc3\xb1", "pqrs", "tuv", "wxyz"},
						   {".,?!\'", "ABC", "DEF", "GHI", "JKL", "MNO\xc3\x91", "PQRS", "TUV", "WXYZ"},
						   "Espa\xc3\xb1ol"},
	[KB_LAYOUT_FRENCH] = {"Fran\xc3\xa7\x61is",
						  {"azertyuiop", "qsdfghjklm", "wxcvbn"},
						  {"AZERTYUIOP", "QSDFGHJKLM", "WXCVBN"},
						  {T9_LATIN_LOWER},
						  {T9_LATIN_UPPER},
						  "Fran\xc3\xa7\x61is"},
	[KB_LAYOUT_RUSSIAN] = {"\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9",
						   {"\xd0\xb9\xd1\x86\xd1\x83\xd0\xba\xd0\xb5\xd0\xbd\xd0\xb3\xd1\x88\xd1\x89\xd0\xb7"
							"\xd1\x85\xd1\x8a",
							"\xd1\x84\xd1\x8b\xd0\xb2\xd0\xb0\xd0\xbf\xd1\x80\xd0\xbe\xd0\xbb\xd0\xb4\xd0\xb6"
							"\xd1\x8d",
							"\xd1\x8f\xd1\x87\xd1\x81\xd0\xbc\xd0\xb8\xd1\x82\xd1\x8c\xd0\xb1\xd1\x8e"},
						   {"\xd0\x99\xd0\xa6\xd0\xa3\xd0\x9a\xd0\x95\xd0\x9d\xd0\x93\xd0\xa8\xd0\xa9\xd0\x97"
							"\xd0\xa5\xd0\xaa",
							"\xd0\xa4\xd0\xab\xd0\x92\xd0\x90\xd0\x9f\xd0\xa0\xd0\x9e\xd0\x9b\xd0\x94\xd0\x96"
							"\xd0\xad",
							"\xd0\xaf\xd0\xa7\xd0\xa1\xd0\x9c\xd0\x98\xd0\xa2\xd0\xac\xd0\x91\xd0\xae"},
						   // The Cyrillic keypad: thirty-two letters four to a key,
						   // in alphabetical order, which is how a Russian phone
						   // shares them out.
						   {".,?!\'",
							"\xd0\xb0\xd0\xb1\xd0\xb2\xd0\xb3", "\xd0\xb4\xd0\xb5\xd0\xb6\xd0\xb7",
							"\xd0\xb8\xd0\xb9\xd0\xba\xd0\xbb", "\xd0\xbc\xd0\xbd\xd0\xbe\xd0\xbf",
							"\xd1\x80\xd1\x81\xd1\x82\xd1\x83", "\xd1\x84\xd1\x85\xd1\x86\xd1\x87",
							"\xd1\x88\xd1\x89\xd1\x8a\xd1\x8b", "\xd1\x8c\xd1\x8d\xd1\x8e\xd1\x8f"},
						   {".,?!\'",
							"\xd0\x90\xd0\x91\xd0\x92\xd0\x93", "\xd0\x94\xd0\x95\xd0\x96\xd0\x97",
							"\xd0\x98\xd0\x99\xd0\x9a\xd0\x9b", "\xd0\x9c\xd0\x9d\xd0\x9e\xd0\x9f",
							"\xd0\xa0\xd0\xa1\xd0\xa2\xd0\xa3", "\xd0\xa4\xd0\xa5\xd0\xa6\xd0\xa7",
							"\xd0\xa8\xd0\xa9\xd0\xaa\xd0\xab", "\xd0\xac\xd0\xad\xd0\xae\xd0\xaf"},
						   "Russian"},
};

// The config file keeps names, not indices.
static const char *const layout_keys[KB_LAYOUT_COUNT] = {"english", "italian", "german",
														 "spanish", "french",  "russian"};

static uint8_t in_use[KB_LAYOUT_COUNT];
static uint8_t in_use_n;
static uint8_t others[KB_LAYOUT_COUNT];
static uint8_t others_n;
static bool loaded;

// ---------------------------------------------------------------------------
// The letters
// ---------------------------------------------------------------------------

// How many bytes the UTF-8 code point starting at `p` occupies.
static int utf8_len(const char *p) {
	unsigned char c = (unsigned char)*p;
	if (c < 0x80) {
		return 1;
	}
	if ((c & 0xE0) == 0xC0) {
		return 2;
	}
	if ((c & 0xF0) == 0xE0) {
		return 3;
	}
	return 4;
}

static const char *row_string(kblayout_t layout, int row, bool upper) {
	if (layout < 0 || layout >= KB_LAYOUT_COUNT || row < 0 || row >= KB_LAYOUT_ROWS) {
		return "";
	}
	return upper ? layouts[layout].upper[row] : layouts[layout].lower[row];
}

int kblayout_row_len(kblayout_t layout, int row) {
	const char *p = row_string(layout, row, false);
	int n = 0;
	while (*p) {
		p += utf8_len(p);
		n++;
	}
	return n;
}

const char *kblayout_letter(kblayout_t layout, int row, int index, bool upper) {
	// Copied out into a small rotation of buffers rather than returned as a
	// pointer into the table: the caller wants one letter, and the table holds
	// a whole row with no terminator between its letters.
	static char slots[4][8];
	static int at;

	const char *p = row_string(layout, row, upper);
	for (int i = 0; i < index && *p; i++) {
		p += utf8_len(p);
	}
	if (!*p) {
		return "";
	}

	int len = utf8_len(p);
	char *out = slots[at];
	at = (at + 1) % 4;
	memcpy(out, p, (size_t)len);
	out[len] = '\0';
	return out;
}

static const char *t9_string(kblayout_t layout, int key, bool upper) {
	if (layout < 0 || layout >= KB_LAYOUT_COUNT || key < 0 || key >= KB_LAYOUT_T9_KEYS) {
		return "";
	}
	return upper ? layouts[layout].t9_upper[key] : layouts[layout].t9_lower[key];
}

int kblayout_t9_len(kblayout_t layout, int key) {
	const char *p = t9_string(layout, key, false);
	int n = 0;
	while (*p) {
		p += utf8_len(p);
		n++;
	}
	return n;
}

const char *kblayout_t9_letter(kblayout_t layout, int key, int index, bool upper) {
	static char slots[4][8];
	static int at;

	const char *p = t9_string(layout, key, upper);
	for (int i = 0; i < index && *p; i++) {
		p += utf8_len(p);
	}
	if (!*p) {
		return "";
	}

	int len = utf8_len(p);
	char *out = slots[at];
	at = (at + 1) % 4;
	memcpy(out, p, (size_t)len);
	out[len] = '\0';
	return out;
}

const char *kblayout_name(kblayout_t layout) {
	if (layout < 0 || layout >= KB_LAYOUT_COUNT) {
		return "";
	}
	return layouts[layout].name;
}

// ---------------------------------------------------------------------------
// The two lists
// ---------------------------------------------------------------------------

static uint8_t parse_list(const char *saved, uint8_t *out, bool *placed) {
	uint8_t n = 0;
	const char *p = saved ? saved : "";

	while (*p && n < KB_LAYOUT_COUNT) {
		while (*p == ',' || *p == ' ') {
			p++;
		}
		const char *start = p;
		while (*p && *p != ',') {
			p++;
		}
		size_t len = (size_t)(p - start);
		while (len > 0 && start[len - 1] == ' ') {
			len--;
		}
		for (int i = 0; i < KB_LAYOUT_COUNT && len; i++) {
			if (placed[i] || strlen(layout_keys[i]) != len || strncmp(layout_keys[i], start, len) != 0) {
				continue;
			}
			placed[i] = true;
			out[n++] = (uint8_t)i;
			break;
		}
	}
	return n;
}

// The layout the interface language asks for, which is the one a player that
// has never been told otherwise types in.
static kblayout_t layout_for_language(void) {
	const char *now = lang_current();
	for (int i = 0; i < KB_LAYOUT_COUNT && now; i++) {
		if (strcmp(layouts[i].language, now) == 0) {
			return (kblayout_t)i;
		}
	}
	return KB_LAYOUT_ENGLISH;
}

static void load(void) {
	if (loaded) {
		return;
	}
	loaded = true;

	bool placed[KB_LAYOUT_COUNT] = {false};
	in_use_n = parse_list(config_get("keyboard", "layouts", ""), in_use, placed);
	others_n = parse_list(config_get("keyboard", "layouts_off", ""), others, placed);

	// Nothing saved, or a layout that did not exist when it was saved. The
	// interface language's own goes in front of the ones in use; everything
	// else waits in the other list, because five alphabets on one page is a
	// list to choose from, not a default.
	kblayout_t mine = layout_for_language();
	if (!placed[mine] && in_use_n == 0) {
		placed[mine] = true;
		in_use[in_use_n++] = (uint8_t)mine;
	}
	for (int i = 0; i < KB_LAYOUT_COUNT; i++) {
		if (!placed[i]) {
			others[others_n++] = (uint8_t)i;
		}
	}

	// A hand-edited file that emptied the first list: something has to be
	// typeable, so the first of the others is drafted.
	if (in_use_n == 0 && others_n > 0) {
		in_use[in_use_n++] = others[0];
		memmove(&others[0], &others[1], (size_t)(others_n - 1));
		others_n--;
	}
}

static void save(void) {
	char text[KB_LAYOUT_COUNT * 10];

	for (int list = 0; list < 2; list++) {
		const uint8_t *items = list ? others : in_use;
		int count = list ? others_n : in_use_n;
		size_t used = 0;
		text[0] = '\0';
		for (int i = 0; i < count; i++) {
			int wrote = snprintf(text + used, sizeof(text) - used, "%s%s", i ? "," : "", layout_keys[items[i]]);
			if (wrote <= 0 || (size_t)wrote >= sizeof(text) - used) {
				break;
			}
			used += (size_t)wrote;
		}
		config_set("keyboard", list ? "layouts_off" : "layouts", text);
	}
	config_save();
}

int kblayout_in_use_count(void) {
	load();
	return in_use_n;
}

kblayout_t kblayout_in_use_at(int position) {
	load();
	if (position < 0 || position >= in_use_n) {
		return KB_LAYOUT_ENGLISH;
	}
	return (kblayout_t)in_use[position];
}

int kblayout_others_count(void) {
	load();
	return others_n;
}

kblayout_t kblayout_others_at(int position) {
	load();
	if (position < 0 || position >= others_n) {
		return KB_LAYOUT_ENGLISH;
	}
	return (kblayout_t)others[position];
}

kblayout_t kblayout_default(void) {
	load();
	return in_use_n ? (kblayout_t)in_use[0] : KB_LAYOUT_ENGLISH;
}

bool kblayout_is_in_use(kblayout_t layout) {
	load();
	for (int i = 0; i < in_use_n; i++) {
		if (in_use[i] == layout) {
			return true;
		}
	}
	return false;
}

static void list_remove(uint8_t *items, uint8_t *count, kblayout_t layout) {
	for (int i = 0; i < *count; i++) {
		if (items[i] != layout) {
			continue;
		}
		memmove(&items[i], &items[i + 1], (size_t)(*count - i - 1));
		(*count)--;
		return;
	}
}

static void list_insert(uint8_t *items, uint8_t *count, kblayout_t layout, int position) {
	if (position < 0 || position > *count) {
		position = *count;
	}
	memmove(&items[position + 1], &items[position], (size_t)(*count - position));
	items[position] = (uint8_t)layout;
	(*count)++;
}

void kblayout_move_in_use(kblayout_t layout, int position) {
	load();
	if (layout < 0 || layout >= KB_LAYOUT_COUNT) {
		return;
	}
	if (kblayout_is_in_use(layout)) {
		list_remove(in_use, &in_use_n, layout);
	} else {
		list_remove(others, &others_n, layout);
	}
	list_insert(in_use, &in_use_n, layout, position);
	save();
}

void kblayout_move_others(kblayout_t layout, int position) {
	load();
	if (layout < 0 || layout >= KB_LAYOUT_COUNT) {
		return;
	}
	// The last one in use cannot be sent away: the keyboard would have no
	// letters to draw.
	if (kblayout_is_in_use(layout)) {
		if (in_use_n <= 1) {
			return;
		}
		list_remove(in_use, &in_use_n, layout);
	} else {
		list_remove(others, &others_n, layout);
	}
	list_insert(others, &others_n, layout, position);
	save();
}
