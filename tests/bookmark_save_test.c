#include "../sonix-player/src/gui/library/audiobookextras.c"
#include <assert.h>
static uint32_t tick;
static int writes;
static bool fail;
static int units = 1;
static char shown[320];
uint32_t lv_tick_get(void) { return tick; }
bool audiobook_current_book(char *book, size_t bn, char *file, size_t fn) {
	snprintf(book, bn, "book");
	snprintf(file, fn, "file");
	return true;
}
void device_state_get(device_state_t *s) {
	memset(s, 0, sizeof(*s));
	s->progress_current_secs = 10;
}
void audiobook_place_label(double seconds, char *out, size_t n) { snprintf(out, n, "Chapter 1"); }
int audiobook_unit_count(void) { return units; }
audiobook_bookmark_result_t audiobookdb_bookmark_add(const char *book, const char *file, double seconds, const char *label) {
	writes++;
	return fail ? AUDIOBOOK_BOOKMARK_FAILED : AUDIOBOOK_BOOKMARK_SAVED;
}
const char *tr(const char *tag) { return !strcmp(tag, "audiobook_bookmark_saved_at") ? "Bookmark: %s, %s" : "Bookmark: %s"; }
void toast_glyph(const lv_image_dsc_t *glyph, const char *text) { snprintf(shown, sizeof(shown), "%s", text); }
void toast_error(const char *text) {}
const lv_image_dsc_t icon_bookmark_check = {0};
int main(void) {
	fail = true;
	audiobookextras_add_bookmark();
	assert(writes == 1);
	fail = false;
	audiobookextras_add_bookmark();
	assert(writes == 2);
	assert(!strcmp(shown, "Bookmark: Chapter 1, 0:10"));
	audiobookextras_add_bookmark();
	assert(writes == 2);
	tick = 2000;
	audiobookextras_add_bookmark();
	assert(writes == 3);
	units = 0;
	tick = 4000;
	audiobookextras_add_bookmark();
	assert(!strcmp(shown, "Bookmark: 0:10"));
	puts("PASS bookmark-save-retry");
}
