#include "autoeqsettings.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/audio/eq.h"
#include "src/system/core/config.h"
#include "src/system/net/http.h"

#define RESULT_MAX 20
#define INDEX_MAX (4u * 1024u * 1024u)
#define RANKING_MAX (2u * 1024u * 1024u)
#define PROFILE_MAX (128u * 1024u)

#define INDEX_URL "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/refs/heads/master/results/INDEX.md"
#define RANKING_URL "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/refs/heads/master/results/RANKING.md"
#define RESULTS_URL "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/refs/heads/master/results/"

typedef struct {
	char name[128];
	char path[512];
	char source[128];
	char display[300];
} profile_t;

typedef struct {
	bool update;
	bool search_after_update;
	char query[128];
	char error[192];
	profile_t selected;
	profile_t results[RESULT_MAX];
	int result_count;
	char preset[101];
	char cache[768];
	char *profile_text;
	size_t profile_size;
} job_t;

static gui_config_t *cfg;
static char root[512], profiles[512], imports[512], index_file[512], ranking_file[512];
static lv_obj_t *menu_screen, *enabled_switch, *current_value;
static lv_obj_t *search_screen, *search_field;
static keyboard_t *search_keyboard;
static lv_obj_t *results_screen, *results_list, *results_empty;
static lv_obj_t *saved_screen, *saved_list, *saved_empty;
static lv_obj_t *remove_screen, *remove_list, *remove_empty;
static lv_obj_t *import_screen, *import_list, *import_empty;
static profile_t result_items[RESULT_MAX];
static int result_count;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static bool job_busy;

static bool make_dir(const char *path) {
	struct stat st;
	return (mkdir(path, 0777) == 0 || errno == EEXIST) && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool ensure_dirs(void) {
	if (!cfg || !cfg->sd_root_path || !cfg->sd_root_path[0] || !gui_card_available()) return false;
	snprintf(root, sizeof(root), "%s/AutoEq", cfg->sd_root_path);
	snprintf(profiles, sizeof(profiles), "%s/Profiles", root);
	snprintf(imports, sizeof(imports), "%s/Imports", root);
	snprintf(index_file, sizeof(index_file), "%s/INDEX.md", root);
	snprintf(ranking_file, sizeof(ranking_file), "%s/RANKING.md", root);
	return make_dir(root) && make_dir(profiles) && make_dir(imports);
}

static bool write_file(const char *path, const char *data, size_t size) {
	char temp[800];
	if (snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp)) return false;
	FILE *file = fopen(temp, "wb");
	if (!file) return false;
	size_t written = fwrite(data, 1, size, file);
	bool ok = written == size;
	if (fclose(file) != 0) ok = false;
	if (!ok || rename(temp, path) != 0) {
		remove(temp);
		return false;
	}
	return true;
}

static char *read_file(const char *path, size_t max_size, size_t *size_out) {
	FILE *file = fopen(path, "rb");
	if (!file) return NULL;
	if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
	long size = ftell(file);
	if (size < 0 || (size_t)size > max_size || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
	char *data = malloc((size_t)size + 1);
	if (!data) { fclose(file); return NULL; }
	size_t got = fread(data, 1, (size_t)size, file);
	fclose(file);
	if (got != (size_t)size) { free(data); return NULL; }
	data[got] = '\0';
	if (size_out) *size_out = got;
	return data;
}

static bool begin_job(void) {
	pthread_mutex_lock(&state_lock);
	bool ok = !job_busy;
	if (ok) job_busy = true;
	pthread_mutex_unlock(&state_lock);
	return ok;
}

static void end_job(void) {
	pthread_mutex_lock(&state_lock);
	job_busy = false;
	pthread_mutex_unlock(&state_lock);
}

static bool is_busy(void) {
	pthread_mutex_lock(&state_lock);
	bool busy = job_busy;
	pthread_mutex_unlock(&state_lock);
	return busy;
}

static uint32_t hash_text(const char *text) {
	uint32_t hash = 2166136261u;
	for (const unsigned char *p = (const unsigned char *)text; *p; p++) hash = (hash ^ *p) * 16777619u;
	return hash;
}

static bool has_text(const char *text, const char *query) {
	if (!query[0]) return true;
	for (const char *p = text; *p; p++) {
		const char *a = p, *b = query;
		while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
		if (!*b) return true;
	}
	return false;
}

static void parse_index(const char *text, const char *query, job_t *job) {
	for (const char *line = text; *line && job->result_count < RESULT_MAX;) {
		const char *end = strchr(line, '\n');
		size_t length = end ? (size_t)(end - line) : strlen(line);
		if (length < 2048) {
			char row[2048];
			memcpy(row, line, length);
			row[length] = '\0';
			while (length && isspace((unsigned char)row[length - 1])) row[--length] = '\0';
			char *start = strstr(row, "- ["), *name_end = start ? strstr(start + 3, "](") : NULL;
			char *path_end = name_end ? strchr(name_end + 2, ')') : NULL;
			if (start && name_end && path_end) {
				size_t name_len = (size_t)(name_end - start - 3), path_len = (size_t)(path_end - name_end - 2);
				const char *path = name_end + 2;
				if (name_len && name_len < sizeof(job->results[0].name) && path_len < sizeof(job->results[0].path) &&
					strstr(path, "in-ear") && has_text(start + 3, query)) {
					profile_t item = {0};
					memcpy(item.name, start + 3, name_len);
					memcpy(item.path, path, path_len);
					const char *meta = path_end + 1;
					while (*meta == ' ' || *meta == '\t') meta++;
					if (strncmp(meta, "by ", 3) == 0) meta += 3;
					snprintf(item.source, sizeof(item.source), "%s", *meta ? meta : "AutoEq");
					snprintf(item.display, sizeof(item.display), "%s [%s]", item.name, item.source);
					bool duplicate = false;
					for (int i = 0; i < job->result_count; i++) {
						if (!strcmp(job->results[i].name, item.name) && !strcmp(job->results[i].path, item.path)) duplicate = true;
					}
					if (!duplicate) job->results[job->result_count++] = item;
				}
			}
		}
		if (!end) break;
		line = end + 1;
	}
}

static bool encode_path(const char *input, char *output, size_t capacity) {
	static const char hex[] = "0123456789ABCDEF";
	size_t used = 0;
	for (const unsigned char *p = (const unsigned char *)input; *p; p++) {
		unsigned char c = *p;
		if (c == '/' || isalnum(c) || strchr("-_.~", c)) {
			if (used + 1 >= capacity) return false;
			output[used++] = (char)c;
		} else if (c == '%' && isxdigit(p[1]) && isxdigit(p[2])) {
			if (used + 3 >= capacity) return false;
			output[used++] = '%'; output[used++] = (char)p[1]; output[used++] = (char)p[2]; p += 2;
		} else {
			if (used + 3 >= capacity || c == '?' || c == '#' || c == '\\') return false;
			output[used++] = '%'; output[used++] = hex[c >> 4]; output[used++] = hex[c & 15];
		}
	}
	output[used] = '\0';
	return true;
}

static bool make_profile_url(const profile_t *item, char *url, size_t capacity) {
	const char *path = item->path;
	if (!strncmp(path, "./", 2)) path += 2;
	char encoded_path[1600], file[768], name[180];
	if (!encode_path(path, encoded_path, sizeof(encoded_path))) return false;
	snprintf(name, sizeof(name), "%s ParametricEQ.txt", item->name);
	http_url_encode(name, file, sizeof(file));
	int n = snprintf(url, capacity, "%s%s/%s", RESULTS_URL, encoded_path, file);
	return n > 0 && (size_t)n < capacity;
}

static void preset_name(const profile_t *item, char *out, size_t size) {
	char name[68];
	snprintf(name, sizeof(name), "%.64s", item->name);
	for (char *p = name; *p; p++) if (strchr("/\\:*?\"<>|", *p)) *p = '-';
	snprintf(out, size, "AutoEq - %s-%08x", name, hash_text(item->path));
}

static void cache_path(const char *preset, char *out, size_t size) {
	char slug[64];
	size_t n = 0;
	for (const unsigned char *p = (const unsigned char *)preset; *p && n < sizeof(slug)-1; p++) {
		slug[n++] = isalnum(*p) ? (char)tolower(*p) : '_';
	}
	slug[n] = '\0';
	snprintf(out, size, "%s/%.54s_%08x.txt", profiles, slug, hash_text(preset));
}

static void update_current(const char *name, const char *label) {
	config_set("autoeq", "preset", name ? name : "");
	config_set("autoeq", "current", label ? label : "");
	config_set_bool("autoeq", "enabled", true);
	config_save();
	if (current_value) lv_label_set_text(current_value, label && label[0] ? label : tr("autoeq_none"));
}

static void refresh_menu(void) {
	if (!menu_screen) return;
	if (config_get_bool("autoeq", "enabled", false)) lv_obj_add_state(enabled_switch, LV_STATE_CHECKED);
	else lv_obj_remove_state(enabled_switch, LV_STATE_CHECKED);
	const char *name = config_get("autoeq", "current", "");
	lv_label_set_text(current_value, name && name[0] ? name : tr("autoeq_none"));
}

static bool apply_job(job_t *job, const char *label, const char *profile, size_t size) {
	char peq_dir[600], stage_name[64], stage_path[700], preset_path[700];
	if (!cfg || !cfg->sd_root_path || !profile || !size ||
		snprintf(peq_dir, sizeof(peq_dir), "%s/PEQ", cfg->sd_root_path) >= (int)sizeof(peq_dir) ||
		!make_dir(peq_dir)) {
		toast_error(tr("autoeq_error_peq_folder"));
		return false;
	}
	uint32_t stage_hash = hash_text(job->preset) ^ hash_text(profile);
	snprintf(stage_name, sizeof(stage_name), "AutoEq Stage %08x", stage_hash);
	if (snprintf(stage_path, sizeof(stage_path), "%s/%s.txt", peq_dir, stage_name) >= (int)sizeof(stage_path) ||
		snprintf(preset_path, sizeof(preset_path), "%s/%s.txt", peq_dir, job->preset) >= (int)sizeof(preset_path) ||
		!write_file(stage_path, profile, size)) {
		toast_error(tr("autoeq_error_save_profile"));
		return false;
	}
	int ignored = 0;
	if (!peq_preset_load(stage_name, &ignored)) {
		remove(stage_path);
		toast_error(tr("autoeq_error_unsupported_profile"));
		return false;
	}
	if (rename(stage_path, preset_path) != 0) {
		remove(stage_path);
		toast_error(tr("autoeq_error_save_preset"));
		return false;
	}
	peq_set_enabled(true);
	update_current(job->preset, label);
	if (ignored) {
		char message[96];
		snprintf(message, sizeof(message), tr("peq_preset_loaded_partly"), ignored);
		toast_success(message);
	} else toast_success(tr("autoeq_profile_applied"));
	return true;
}

static bool download_database(job_t *job) {
	char *index = NULL, *ranking = NULL;
	size_t index_size = 0, ranking_size = 0;
	if (!http_get(INDEX_URL, &index, &index_size, INDEX_MAX, 35)) {
		const char *error = http_last_error();
		snprintf(job->error, sizeof(job->error), "%s", error && error[0] ? error : tr("autoeq_error_index_download"));
		free(index);
		return false;
	}
	if (!http_get(RANKING_URL, &ranking, &ranking_size, RANKING_MAX, 35)) {
		const char *error = http_last_error();
		snprintf(job->error, sizeof(job->error), "%s", error && error[0] ? error : tr("autoeq_error_ranking_download"));
		free(index); free(ranking);
		return false;
	}
	bool ok = write_file(index_file, index, index_size) && write_file(ranking_file, ranking, ranking_size);
	if (!ok) snprintf(job->error, sizeof(job->error), tr("autoeq_error_database_save"));
	if (ok && job->search_after_update) parse_index(index, job->query, job);
	free(index); free(ranking);
	return ok;
}

static void job_done(void *user);
static void result_clicked(lv_event_t *event);
static void saved_clicked(lv_event_t *event);
static void remove_clicked(lv_event_t *event);

static void *worker(void *user) {
	job_t *job = user;
	if (job->update) {
		download_database(job);
	} else {
		size_t size = 0;
		char *text = read_file(job->cache, PROFILE_MAX, &size);
		if (!text) {
			char url[3072];
			if (!make_profile_url(&job->selected, url, sizeof(url))) {
				snprintf(job->error, sizeof(job->error), tr("autoeq_error_profile_url"));
			} else if (!http_get(url, &text, &size, PROFILE_MAX, 30)) {
				const char *error = http_last_error();
				snprintf(job->error, sizeof(job->error), "%s", error && error[0] ? error : tr("autoeq_error_profile_download"));
			} else if (!write_file(job->cache, text, size)) {
				snprintf(job->error, sizeof(job->error), tr("autoeq_error_profile_cache"));
			}
		}
		if (text && !job->error[0]) {
			job->profile_text = text;
			job->profile_size = size;
			text = NULL;
		}
		free(text);
	}
	if (!gui_post(job_done, job)) {
		free(job->profile_text);
		free(job);
		end_job();
	}
	return NULL;
}

static bool start_job(job_t *job, const char *busy_text) {
	if (!begin_job()) { free(job); return false; }
	toast_busy(busy_text);
	pthread_t thread;
	if (pthread_create(&thread, NULL, worker, job) != 0) {
		end_job(); free(job); toast_busy_end(); toast_error(tr("autoeq_error_start"));
		return false;
	}
	pthread_detach(thread);
	return true;
}

static void show_results(job_t *job) {
	result_count = job->result_count;
	memcpy(result_items, job->results, sizeof(result_items));
	lv_obj_clean(results_list);
	for (int i = 0; i < result_count; i++) {
		lv_obj_t *row = settingsrow_add(results_list, result_items[i].display, NULL, result_clicked,
									(void *)(intptr_t)i);
		lv_obj_update_layout(row);
		lv_obj_t *label = settingsrow_name_label(row);
		lv_obj_t *chevron = lv_obj_get_child(row, 1);
		int32_t width = lv_obj_get_content_width(row) - (chevron ? lv_obj_get_width(chevron) + 12 : 0);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_width(label, width);
		lv_obj_set_height(label, lv_font_get_line_height(
				lv_obj_get_style_text_font(label, LV_PART_MAIN)));
	}
	if (result_count) lv_obj_add_flag(results_empty, LV_OBJ_FLAG_HIDDEN);
	else lv_obj_remove_flag(results_empty, LV_OBJ_FLAG_HIDDEN);
	if (result_count) switch_screen(results_screen);
	else toast_plain(tr("autoeq_no_matching_profile"));
}

static void job_done(void *user) {
	job_t *job = user;
	end_job();
	toast_busy_end();
	if (job->error[0]) toast_error(job->error);
	else if (job->update) {
		if (job->search_after_update) show_results(job);
		else toast_success(tr("autoeq_database_updated"));
	} else apply_job(job, job->selected.display, job->profile_text, job->profile_size);
	free(job->profile_text);
	free(job);
}

static void result_clicked(lv_event_t *event) {
	if (switcher_back_drag_active() || is_busy() || !ensure_dirs()) return;
	int index = (int)(intptr_t)lv_event_get_user_data(event);
	if (index < 0 || index >= result_count) return;
	job_t *job = calloc(1, sizeof(*job));
	if (!job) { toast_error(tr("out_of_memory")); return; }
	job->selected = result_items[index];
	preset_name(&job->selected, job->preset, sizeof(job->preset));
	cache_path(job->preset, job->cache, sizeof(job->cache));
	start_job(job, tr("autoeq_downloading_profile"));
}

static void search_accept(lv_event_t *event) {
	(void)event;
	char query[128];
	snprintf(query, sizeof(query), "%s", lv_textarea_get_text(search_field));
	char *start = query;
	while (isspace((unsigned char)*start)) start++;
	size_t len = strlen(start);
	while (len && isspace((unsigned char)start[len-1])) start[--len] = '\0';
	if (!*start) { toast_error(tr("autoeq_enter_iem_name")); return; }
	if (!ensure_dirs()) { toast_error(tr("autoeq_need_card")); return; }
	char *index = read_file(index_file, INDEX_MAX, NULL);
	if (!index) {
		job_t *job = calloc(1, sizeof(*job));
		if (!job) { toast_error(tr("out_of_memory")); return; }
		job->update = job->search_after_update = true;
		snprintf(job->query, sizeof(job->query), "%s", start);
		start_job(job, tr("autoeq_downloading_database"));
		return;
	}
	job_t *job = calloc(1, sizeof(*job));
	if (!job) { free(index); toast_error(tr("out_of_memory")); return; }
	parse_index(index, start, job);
	free(index);
	show_results(job);
	free(job);
}

static void search_open(lv_event_t *event) {
	(void)event;
	lv_textarea_set_text(search_field, "");
	keyboard_reset(search_keyboard);
	switch_screen(search_screen);
	lv_obj_add_state(search_field, LV_STATE_FOCUSED);
	lv_obj_send_event(search_field, LV_EVENT_FOCUSED, NULL);
}

static void update_database(lv_event_t *event) {
	(void)event;
	if (!ensure_dirs()) { toast_error(tr("autoeq_need_card")); return; }
	job_t *job = calloc(1, sizeof(*job));
	if (!job) { toast_error(tr("out_of_memory")); return; }
	job->update = true;
	start_job(job, tr("autoeq_updating_database"));
}

static void row_name_delete(lv_event_t *event) { free(lv_event_get_user_data(event)); }

typedef struct { lv_obj_t *list; int count; bool remove; } list_state_t;

static bool add_saved(const char *name, void *user) {
	list_state_t *state = user;
	if (strncmp(name, "AutoEq - ", 9)) return true;
	char *copy = strdup(name);
	if (!copy) return true;
	lv_obj_t *row = settingsrow_add(state->list, name, NULL, state->remove ? remove_clicked : saved_clicked, copy);
	lv_obj_add_event_cb(row, row_name_delete, LV_EVENT_DELETE, copy);
	state->count++;
	return true;
}

static void rebuild_saved(bool remove_mode) {
	lv_obj_t *list = remove_mode ? remove_list : saved_list;
	lv_obj_t *empty = remove_mode ? remove_empty : saved_empty;
	lv_obj_clean(list);
	list_state_t state = {.list = list, .remove = remove_mode};
	peq_preset_for_each(add_saved, &state);
	if (state.count) lv_obj_add_flag(empty, LV_OBJ_FLAG_HIDDEN);
	else lv_obj_remove_flag(empty, LV_OBJ_FLAG_HIDDEN);
}

static void saved_open(lv_event_t *event) {
	(void)event;
	if (!ensure_dirs()) { toast_error(tr("autoeq_need_card")); return; }
	rebuild_saved(false);
	switch_screen(saved_screen);
}

static void remove_open(lv_event_t *event) {
	(void)event;
	if (!ensure_dirs()) { toast_error(tr("autoeq_need_card")); return; }
	rebuild_saved(true);
	switch_screen(remove_screen);
}

static void saved_clicked(lv_event_t *event) {
	if (switcher_back_drag_active()) return;
	const char *name = lv_event_get_user_data(event);
	if (!name) return;
	if (peq_preset_load(name, NULL)) {
		peq_set_enabled(true);
		update_current(name, name);
		refresh_menu();
		toast_success(tr("autoeq_preset_loaded"));
	} else toast_error(tr("autoeq_error_load_preset"));
}

static void remove_clicked(lv_event_t *event) {
	if (switcher_back_drag_active()) return;
	const char *name = lv_event_get_user_data(event);
	if (!name) return;
	char current[128];
	snprintf(current, sizeof(current), "%s", config_get("autoeq", "preset", ""));
	char cache[768]; cache_path(name, cache, sizeof(cache));
	bool removed = peq_preset_delete(name);
	remove(cache);
	if (removed && !strcmp(name, current)) {
		peq_reset(); peq_set_enabled(false);
		config_set_bool("autoeq", "enabled", false);
		config_set("autoeq", "preset", ""); config_set("autoeq", "current", ""); config_save();
		refresh_menu();
	}
	toast_plain(removed ? tr("autoeq_preset_removed") : tr("autoeq_error_remove_preset"));
	rebuild_saved(true);
}

static bool is_txt(const char *name) {
	size_t n = strlen(name);
	return n > 4 && !strcasecmp(name + n - 4, ".txt");
}

static void import_clicked(lv_event_t *event) {
	if (switcher_back_drag_active() || is_busy() || !ensure_dirs()) return;
	const char *filename = lv_event_get_user_data(event);
	char path[700];
	if (!filename || snprintf(path, sizeof(path), "%s/%s", imports, filename) >= (int)sizeof(path)) return;
	size_t size;
	char *text = read_file(path, PROFILE_MAX, &size);
	if (!text) { toast_error(tr("autoeq_error_read_parametric_eq")); return; }
	job_t *job = calloc(1, sizeof(*job));
	if (!job) { free(text); toast_error(tr("out_of_memory")); return; }
	profile_t item = {0};
	snprintf(item.name, sizeof(item.name), "%s", filename);
	char *dot = strrchr(item.name, '.'); if (dot) *dot = '\0';
	snprintf(item.path, sizeof(item.path), "%s", filename);
	preset_name(&item, job->preset, sizeof(job->preset));
	if (apply_job(job, item.name, text, size)) {
		cache_path(job->preset, job->cache, sizeof(job->cache));
		write_file(job->cache, text, size);
	}
	free(job); free(text);
}

static void rebuild_imports(void) {
	lv_obj_clean(import_list);
	DIR *dir = opendir(imports);
	int count = 0;
	if (dir) {
		struct dirent *entry;
		while ((entry = readdir(dir))) {
			if (!is_txt(entry->d_name)) continue;
			char *copy = strdup(entry->d_name);
			if (!copy) continue;
			lv_obj_t *row = settingsrow_add(import_list, entry->d_name, NULL, import_clicked, copy);
			lv_obj_add_event_cb(row, row_name_delete, LV_EVENT_DELETE, copy);
			count++;
		}
		closedir(dir);
	}
	if (count) lv_obj_add_flag(import_empty, LV_OBJ_FLAG_HIDDEN);
	else lv_obj_remove_flag(import_empty, LV_OBJ_FLAG_HIDDEN);
}

static void import_open(lv_event_t *event) {
	(void)event;
	if (!ensure_dirs()) { toast_error(tr("autoeq_need_card")); return; }
	rebuild_imports();
	switch_screen(import_screen);
}

static void current_cb(lv_event_t *event) {
	(void)event;
	char preset[128];
	snprintf(preset, sizeof(preset), "%s", config_get("autoeq", "preset", ""));
	if (!preset[0] || !peq_preset_load(preset, NULL)) { toast_plain(tr("autoeq_current_config_none")); return; }
	peq_set_enabled(true);
	char label[192];
	snprintf(label, sizeof(label), "%s", config_get("autoeq", "current", preset));
	update_current(preset, label);
	refresh_menu();
	toast_success(tr("autoeq_current_preset_loaded"));
}

static void enabled_cb(lv_event_t *event) {
	(void)event;
	if (!lv_obj_has_state(enabled_switch, LV_STATE_CHECKED)) {
		peq_set_enabled(false);
		config_set_bool("autoeq", "enabled", false); config_save();
		toast_plain(tr("autoeq_disabled"));
		return;
	}
	char preset[128];
	snprintf(preset, sizeof(preset), "%s", config_get("autoeq", "preset", ""));
	if (!preset[0] || !peq_preset_load(preset, NULL)) {
		lv_obj_remove_state(enabled_switch, LV_STATE_CHECKED);
		toast_error(tr("autoeq_choose_profile"));
		return;
	}
	peq_set_enabled(true);
	config_set_bool("autoeq", "enabled", true); config_save();
	toast_plain(tr("autoeq_enabled_toast"));
}

static void reset_cb(lv_event_t *event) {
	(void)event;
	peq_reset(); peq_set_enabled(false);
	config_set_bool("autoeq", "enabled", false);
	config_set("autoeq", "preset", ""); config_set("autoeq", "current", ""); config_save();
	refresh_menu();
	toast_success(tr("autoeq_reset_done"));
}

static void about_cb(lv_event_t *event) {
	(void)event;
	toast_plain(tr("autoeq_about_note"));
}

static void menu_loaded(lv_event_t *event) {
	(void)event;
	refresh_menu();
}

static lv_obj_t *simple_page(lv_obj_t **screen, lv_obj_t **list, lv_obj_t **empty, gui_config_t *g,
							 const char *title, const char *empty_text) {
	*screen = lv_obj_create(NULL);
	*list = settingsrow_page(*screen, g, title);
	*empty = lv_label_create(*screen);
	lv_label_set_text(*empty, tr(empty_text));
	lv_obj_add_style(*empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(*empty, &font_ui_24, 0);
	lv_obj_set_width(*empty, g->screen_width - 2 * g->padding);
	lv_obj_set_style_text_align(*empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(*empty, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(g) + 100);
	lv_obj_add_flag(*empty, LV_OBJ_FLAG_HIDDEN);
	switcher_attach_back_gesture(*screen);
	return *list;
}

static void build_pages(gui_config_t *g) {
	menu_screen = lv_obj_create(NULL);
	lv_obj_t *menu = settingsrow_page(menu_screen, g, "autoeq_page_title");
	settingsrow_toggle(menu, "autoeq_enabled", &enabled_switch, enabled_cb);
	settingsrow_add(menu, "autoeq_current_config", &current_value, current_cb, NULL);
	lv_label_set_long_mode(current_value, LV_LABEL_LONG_DOT);
	lv_obj_set_width(current_value, g->screen_width / 2 - g->padding);
	lv_obj_set_height(current_value, lv_font_get_line_height(&font_ui_24));
	settingsrow_add(menu, "autoeq_find_apply_iem", NULL, search_open, NULL);
	settingsrow_add(menu, "autoeq_saved_configs", NULL, saved_open, NULL);
	settingsrow_add(menu, "autoeq_remove_saved_config", NULL, remove_open, NULL);
	settingsrow_add(menu, "autoeq_import_parametric_eq", NULL, import_open, NULL);
	settingsrow_action(menu, "autoeq_update_database", update_database, NULL);
	settingsrow_action(menu, "autoeq_reset_to_flat", reset_cb, NULL);
	settingsrow_action(menu, "autoeq_about", about_cb, NULL);
	lv_obj_add_event_cb(menu_screen, menu_loaded, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(menu_screen);

	search_screen = lv_obj_create(NULL);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);
	settingsrow_title(search_screen, g, "autoeq_search_iem");
	search_field = lv_textarea_create(search_screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_max_length(search_field, 120);
	lv_textarea_set_placeholder_text(search_field, tr("autoeq_headphone_name"));
	lv_obj_set_size(search_field, g->screen_width - 2 * g->padding, 62);
	lv_obj_align(search_field, LV_ALIGN_TOP_LEFT, g->padding, settingsrow_content_top(g));
	lv_obj_add_style(search_field, &theme_style_card, 0);
	lv_obj_set_style_radius(search_field, 12, 0);
	lv_obj_set_style_border_width(search_field, 0, 0);
	lv_obj_set_style_pad_all(search_field, 14, 0);
	lv_obj_set_style_text_font(search_field, &font_ui_24, 0);
	keyboard_style_caret(search_field);
	search_keyboard = keyboard_create(search_screen, g->screen_width, 316, search_field, NULL, "autoeq_search_button", search_accept, NULL);
	switcher_attach_back_gesture(search_screen);

	results_screen = lv_obj_create(NULL);
	results_list = settingsrow_page(results_screen, g, "autoeq_results_title");
	results_empty = lv_label_create(results_screen);
	lv_label_set_text(results_empty, tr("autoeq_no_matching_profiles"));
	lv_obj_add_style(results_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(results_empty, &font_ui_24, 0);
	lv_obj_align(results_empty, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(g) + 100);
	lv_obj_add_flag(results_empty, LV_OBJ_FLAG_HIDDEN);
	switcher_attach_back_gesture(results_screen);

	simple_page(&saved_screen, &saved_list, &saved_empty, g, "autoeq_saved_configs", "autoeq_no_saved_configs");
	simple_page(&remove_screen, &remove_list, &remove_empty, g, "autoeq_remove_saved_config", "autoeq_no_saved_configs");
	simple_page(&import_screen, &import_list, &import_empty, g, "autoeq_import_parametric_eq",
				"autoeq_import_note");
}

void autoeqsettings_init(gui_config_t *config) {
	cfg = config;
	ensure_dirs();
	if (config_get_bool("autoeq", "enabled", false)) {
		const char *preset = config_get("autoeq", "preset", "");
		if (preset[0] && peq_preset_load(preset, NULL)) peq_set_enabled(true);
		else {
			peq_set_enabled(false);
			config_set_bool("autoeq", "enabled", false);
			config_set("autoeq", "preset", ""); config_set("autoeq", "current", ""); config_save();
		}
	}
	build_pages(config);
	refresh_menu();
}

lv_obj_t *autoeqsettings_screen(void) { return menu_screen; }
