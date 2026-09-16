#ifndef EBOOKBAR_H
#define EBOOKBAR_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// The strip along the bottom of a page of a book
//
// What is on it is the reader's choice -- chapter, a progress line, the page,
// the battery, the clock -- so it is built here rather than inside the reader,
// and the settings page builds one of its own as its preview.
//
// That is the whole reason this is a file: a preview drawn by different code
// from the thing it previews is a preview that lies as soon as one of the two
// changes. Both call ebookbar_refresh(), so there is nothing to keep in step.
// ---------------------------------------------------------------------------

typedef struct {
	lv_obj_t *root;	  // the strip itself, to place or hide
	lv_obj_t *line;	  // the progress line
	lv_obj_t *facts;  // the row the text and the battery sit in
	lv_obj_t *text;	  // chapter, page and the battery level, in one label
	lv_obj_t *battery;	 // the battery shell, after the level it belongs to
	lv_obj_t *charge;	 // how full it is, showing through the shell
	lv_obj_t *tail;		 // the clock, which comes after the battery
} ebookbar_t;

// Where the reader is, as the strip needs to know it. Two percentages because
// the line can measure either, and both are cheap for the reader to work out
// while only it knows how.
typedef struct {
	uint32_t chapter;	   // 0-based
	uint32_t chapters;	   // 0 when the book does not say
	int book_percent;	   // how far through the whole book
	int chapter_percent;   // and through this chapter
	int page;			   // page within the chapter, 1-based; 0 when unknown
	lv_color_t ink;		   // the page's text colour: the strip belongs to the paper
} ebookbar_state_t;

// Builds one inside `parent`, at the bottom. The caller positions `root`.
void ebookbar_create(ebookbar_t *bar, lv_obj_t *parent, int32_t width);

// Redraws it with the reader's current options and this position.
void ebookbar_refresh(ebookbar_t *bar, const ebookbar_state_t *state);

// How tall the strip needs to be with the options as they are, so the page
// above it knows how much room it has left. Zero when nothing is switched on.
int32_t ebookbar_height(void);

// The options, read from and written to the reader's own config file.
typedef enum {
	EBOOKBAR_CHAPTER = 0,
	EBOOKBAR_PROGRESS,
	EBOOKBAR_PAGE,
	EBOOKBAR_BATTERY,
	EBOOKBAR_CLOCK,
	EBOOKBAR_COUNT,
} ebookbar_option_t;

bool ebookbar_option(ebookbar_option_t which);
void ebookbar_set_option(ebookbar_option_t which, bool on);

// The translation tag naming each one, for the settings page.
const char *ebookbar_option_tag(ebookbar_option_t which);

// What the progress line measures. A line that fills over a chapter says how
// far the chapter has to go; one that fills over the book says how much book
// is left. Neither is the other, so it is asked rather than assumed.
typedef enum {
	EBOOKBAR_SCOPE_BOOK = 0,
	EBOOKBAR_SCOPE_CHAPTER,
	EBOOKBAR_SCOPE_COUNT,
} ebookbar_scope_t;

ebookbar_scope_t ebookbar_scope(void);
void ebookbar_set_scope(ebookbar_scope_t scope);
const char *ebookbar_scope_tag(ebookbar_scope_t scope);

#endif /* EBOOKBAR_H */
