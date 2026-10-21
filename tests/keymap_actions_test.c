#include "../sonix-player/src/system/input/keymap.c"
#include <assert.h>

// An in-memory config: enough for keymap.c, which only reads and writes "keys".
#define STUB_MAX 16
static char stub_key[STUB_MAX][64];
static char stub_value[STUB_MAX][64];
static int stub_count;

const char *config_get(const char *section, const char *key, const char *fallback) {
	(void)section;
	for (int i = 0; i < stub_count; i++) {
		if (strcmp(stub_key[i], key) == 0) {
			return stub_value[i];
		}
	}
	return fallback;
}

void config_set(const char *section, const char *key, const char *value) {
	(void)section;
	for (int i = 0; i < stub_count; i++) {
		if (strcmp(stub_key[i], key) == 0) {
			snprintf(stub_value[i], sizeof(stub_value[i]), "%s", value);
			return;
		}
	}
	assert(stub_count < STUB_MAX);
	snprintf(stub_key[stub_count], sizeof(stub_key[0]), "%s", key);
	snprintf(stub_value[stub_count], sizeof(stub_value[0]), "%s", value);
	stub_count++;
}

bool config_save(void) { return true; }

int main(void) {
	assert(action_from_key("next_chapter", KEYMAP_ACTION_NONE) == KEYMAP_ACTION_NEXT_CHAPTER);
	assert(action_from_key("prev_chapter", KEYMAP_ACTION_NONE) == KEYMAP_ACTION_PREV_CHAPTER);
	assert(strcmp(keymap_action_name(KEYMAP_ACTION_PREV_CHAPTER), "keymap_previous_chapter") == 0);
	assert(strcmp(keymap_action_name(KEYMAP_ACTION_NEXT_CHAPTER), "keymap_next_chapter") == 0);

	// Every action has a distinct config key and a name.
	for (int i = 0; i < KEYMAP_ACTION_COUNT; i++) {
		assert(action_from_key(ACTION_KEYS[i], KEYMAP_ACTION_COUNT) == (keymap_action_t)i);
		assert(keymap_action_name((keymap_action_t)i)[0] != '\0');
	}

	// The defaults do not change.
	keymap_init();
	assert(keymap_get(KEYMAP_BTN_PREV) == KEYMAP_ACTION_PREV);
	assert(keymap_get(KEYMAP_BTN_PLAY) == KEYMAP_ACTION_PLAY_PAUSE);
	assert(keymap_get(KEYMAP_BTN_NEXT) == KEYMAP_ACTION_NEXT);
	assert(keymap_get(KEYMAP_BTN_VOL_UP) == KEYMAP_ACTION_VOLUME_UP);
	assert(keymap_get(KEYMAP_BTN_VOL_DOWN) == KEYMAP_ACTION_VOLUME_DOWN);
	assert(!keymap_double_enabled());

	// A chapter action survives a save and a fresh start, on a button and on
	// the double click.
	keymap_set(KEYMAP_BTN_PREV, KEYMAP_ACTION_PREV_CHAPTER);
	keymap_set_double(KEYMAP_BTN_NEXT, KEYMAP_ACTION_NEXT_CHAPTER);
	keymap_set(KEYMAP_BTN_PREV, KEYMAP_ACTION_NONE);
	keymap_init();
	assert(keymap_get(KEYMAP_BTN_PREV) == KEYMAP_ACTION_NONE);
	assert(keymap_double_button() == KEYMAP_BTN_NEXT);
	assert(keymap_double_action() == KEYMAP_ACTION_NEXT_CHAPTER);
	assert(keymap_double_enabled());

	keymap_set(KEYMAP_BTN_VOL_UP, KEYMAP_ACTION_PREV_CHAPTER);
	keymap_init();
	assert(keymap_get(KEYMAP_BTN_VOL_UP) == KEYMAP_ACTION_PREV_CHAPTER);
	return 0;
}
