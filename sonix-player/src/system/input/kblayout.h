#ifndef KBLAYOUT_H
#define KBLAYOUT_H

#include <stdbool.h>

// The alphabets the on-screen keyboard can be laid out in, and which of them
// the user has put within reach.
//
// Two lists, the same shape as the control centre's: the ones in use, in order,
// and the rest. The first of the ones in use is the default -- the layout every
// keyboard opens in -- and the others are what the "123" key offers on a long
// press, for one keyboard and until it is next reset. At least one layout is
// always in use.
//
// Both lists live in [keyboard] as lists of names, `layouts` and `layouts_off`,
// so a layout added later joins a saved choice instead of invalidating it. The
// first time round there is nothing saved and the interface language decides:
// its own layout is the one in use and the rest wait in the other list.
//
// The letters are the layouts real keyboards have. English and Italian carry
// the same ones, as they do on a phone; what differs is German (QWERTZ and the
// umlauts), Spanish (the ñ), French (AZERTY) and Russian (ЙЦУКЕН).

typedef enum {
	KB_LAYOUT_ENGLISH = 0,
	KB_LAYOUT_ITALIAN,
	KB_LAYOUT_GERMAN,
	KB_LAYOUT_SPANISH,
	KB_LAYOUT_FRENCH,
	KB_LAYOUT_RUSSIAN,
	KB_LAYOUT_COUNT,
} kblayout_t;

// The most letter keys any layout has (Russian: 12 + 11 + 9).
#define KB_LAYOUT_MAX_KEYS 32
#define KB_LAYOUT_ROWS 3

// What the settings page calls it. Not a translation tag: a layout is named in
// its own language, the way the language list names languages.
const char *kblayout_name(kblayout_t layout);

// How many letters row `row` (0..2) has, and letter `index` of it, lower or
// upper case, as a NUL-terminated UTF-8 string.
int kblayout_row_len(kblayout_t layout, int row);
const char *kblayout_letter(kblayout_t layout, int row, int index, bool upper);

// The nine-key phone keypad, for the same alphabet. Key 0 is the punctuation
// key; the rest carry the letters, four or five to a key.
#define KB_LAYOUT_T9_KEYS 9

int kblayout_t9_len(kblayout_t layout, int key);
const char *kblayout_t9_letter(kblayout_t layout, int key, int index, bool upper);

// The lists. `in_use_at(0)` is the default layout.
int kblayout_in_use_count(void);
kblayout_t kblayout_in_use_at(int position);
int kblayout_others_count(void);
kblayout_t kblayout_others_at(int position);
kblayout_t kblayout_default(void);

// True while `layout` is one of the ones in use.
bool kblayout_is_in_use(kblayout_t layout);

// The two moves the settings page makes. Taking the last layout out of use is
// refused: something has to be typeable.
void kblayout_move_in_use(kblayout_t layout, int position);
void kblayout_move_others(kblayout_t layout, int position);

#endif /* KBLAYOUT_H */
