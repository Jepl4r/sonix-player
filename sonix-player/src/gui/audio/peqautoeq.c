#include "peqautoeq.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "src/gui/audio/peqpage.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/audio/eq.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/net/http.h"

#define RESULT_MAX 20
#define INDEX_MAX (4u * 1024u * 1024u)
#define PROFILE_MAX (128u * 1024u)

#define INDEX_URL "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/refs/heads/master/results/INDEX.md"
#define RESULTS_URL "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/refs/heads/master/results/"

typedef struct {
	char name[128];
	char path[512];
	char source[128];
	char display[300];
	bool duplicate_name;
} profile_t;

typedef enum {
	JOB_SEARCH,
	JOB_UPDATE_DATABASE,
	JOB_DOWNLOAD_PROFILE,
} job_action_t;

typedef struct {
	job_action_t action;
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
static char local_dir[512];
static char autoeq_dir[512];
static char profiles_dir[512];
static char index_file[512];
static lv_obj_t *search_screen;
static lv_obj_t *search_field;
static lv_obj_t *search_list;
static lv_obj_t *results_screen;
static lv_obj_t *results_empty;
static keyboard_t *search_keyboard;
static profile_t result_items[RESULT_MAX];
static int result_count;
static void (*reload_cb)(void);
static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static bool job_busy;

static void job_done(void *user);
static void result_clicked(lv_event_t *event);

static bool path_join(char *out, size_t size, const char *parent, const char *name) {
	int n = snprintf(out, size, "%s/%s", parent, name);
	return n > 0 && (size_t)n < size;
}

static bool make_dir(const char *path) {
	struct stat st;
	if (mkdir(path, 0777) == 0) {
		return true;
	}
	return errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool set_paths(void) {
	if (!cfg || !cfg->sd_root_path || !cfg->sd_root_path[0]) {
		return false;
	}
	return path_join(local_dir, sizeof(local_dir), cfg->sd_root_path, ".local") &&
		   path_join(autoeq_dir, sizeof(autoeq_dir), local_dir, "autoeq") &&
		   path_join(profiles_dir, sizeof(profiles_dir), autoeq_dir, "profiles") &&
		   path_join(index_file, sizeof(index_file), autoeq_dir, "INDEX.md");
}

static bool ensure_dirs(void) {
	return set_paths() && make_dir(local_dir) && make_dir(autoeq_dir) && make_dir(profiles_dir);
}

static bool write_file(const char *path, const char *data, size_t size) {
	char temp[800];
	int n = snprintf(temp, sizeof(temp), "%s.tmp", path);
	if (n < 0 || (size_t)n >= sizeof(temp)) {
		return false;
	}

	FILE *file = fopen(temp, "wb");
	if (!file) {
		return false;
	}
	bool ok = fwrite(data, 1, size, file) == size;
	if (fclose(file) != 0) {
		ok = false;
	}
	if (!ok || rename(temp, path) != 0) {
		remove(temp);
		return false;
	}
	return true;
}

static char *read_file(const char *path, size_t max_size, size_t *size_out) {
	FILE *file = fopen(path, "rb");
	if (!file) {
		return NULL;
	}
	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return NULL;
	}
	long size = ftell(file);
	if (size < 0 || (size_t)size > max_size || fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return NULL;
	}

	char *data = malloc((size_t)size + 1);
	if (!data) {
		fclose(file);
		return NULL;
	}
	size_t got = fread(data, 1, (size_t)size, file);
	fclose(file);
	if (got != (size_t)size) {
		free(data);
		return NULL;
	}
	data[got] = '\0';
	if (size_out) {
		*size_out = got;
	}
	return data;
}

static bool begin_job(void) {
	pthread_mutex_lock(&job_lock);
	bool ok = !job_busy;
	if (ok) {
		job_busy = true;
	}
	pthread_mutex_unlock(&job_lock);
	return ok;
}

static void end_job(void) {
	pthread_mutex_lock(&job_lock);
	job_busy = false;
	pthread_mutex_unlock(&job_lock);
}

static bool is_busy(void) {
	pthread_mutex_lock(&job_lock);
	bool busy = job_busy;
	pthread_mutex_unlock(&job_lock);
	return busy;
}

static uint32_t hash_text(const char *text) {
	uint32_t hash = 2166136261u;
	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		hash = (hash ^ *p) * 16777619u;
	}
	return hash;
}

static bool has_text(const char *text, const char *query) {
	if (!query[0]) {
		return true;
	}
	for (const char *p = text; *p; p++) {
		const char *a = p;
		const char *b = query;
		while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
			a++;
			b++;
		}
		if (!*b) {
			return true;
		}
	}
	return false;
}

static void copy_trimmed(char *out, size_t size, const char *text, size_t length) {
	while (length && isspace((unsigned char)text[length - 1])) {
		length--;
	}
	if (length >= size) {
		length = size - 1;
	}
	memcpy(out, text, length);
	out[length] = '\0';
}

static void profile_source(profile_t *item, const char *meta, const char *path) {
	while (*meta == ' ' || *meta == '\t') {
		meta++;
	}
	if (strncmp(meta, "by ", 3) == 0) {
		meta += 3;
	}
	if (*meta == '[') {
		const char *end = strchr(meta + 1, ']');
		if (end) {
			copy_trimmed(item->source, sizeof(item->source), meta + 1, (size_t)(end - meta - 1));
		}
	} else {
		copy_trimmed(item->source, sizeof(item->source), meta, strlen(meta));
	}

	if (!item->source[0]) {
		const char *last = strrchr(path, '/');
		if (last && last > path) {
			const char *parent_end = last;
			const char *parent_start = parent_end;
			while (parent_start > path && parent_start[-1] != '/') {
				parent_start--;
			}
			copy_trimmed(item->source, sizeof(item->source), parent_start,
						 (size_t)(parent_end - parent_start));
		}
	}
	if (!item->source[0]) {
		snprintf(item->source, sizeof(item->source), "%s", "AutoEq");
	}
}

static void finish_result_names(job_t *job) {
	for (int i = 0; i < job->result_count; i++) {
		int matches = 0;
		for (int j = 0; j < job->result_count; j++) {
			if (strcasecmp(job->results[i].name, job->results[j].name) == 0) {
				matches++;
			}
		}
		job->results[i].duplicate_name = job->results[i].duplicate_name || matches > 1;
		if (job->results[i].duplicate_name) {
			snprintf(job->results[i].display, sizeof(job->results[i].display), "%s [%s]",
					 job->results[i].name, job->results[i].source);
		} else {
			snprintf(job->results[i].display, sizeof(job->results[i].display), "%s", job->results[i].name);
		}
	}
}

static void parse_index(const char *text, const char *query, job_t *job) {
	for (const char *line = text; *line;) {
		const char *end = strchr(line, '\n');
		size_t length = end ? (size_t)(end - line) : strlen(line);
		if (length < 2048) {
			char row[2048];
			memcpy(row, line, length);
			row[length] = '\0';
			while (length && isspace((unsigned char)row[length - 1])) {
				row[--length] = '\0';
			}

			char *start = strstr(row, "- [");
			char *name_end = start ? strstr(start + 3, "](") : NULL;
			char *path_end = NULL;
			if (name_end) {
				int depth = 1;
				for (char *p = name_end + 2; *p; p++) {
					if (*p == '(') {
						depth++;
					} else if (*p == ')' && --depth == 0) {
						path_end = p;
						break;
					}
				}
			}

			if (start && name_end && path_end && has_text(start + 3, query)) {
				size_t name_len = (size_t)(name_end - start - 3);
				size_t path_len = (size_t)(path_end - name_end - 2);
				if (name_len && name_len < sizeof(job->results[0].name) && path_len &&
					path_len < sizeof(job->results[0].path)) {
					profile_t item = {0};
					memcpy(item.name, start + 3, name_len);
					memcpy(item.path, name_end + 2, path_len);
					profile_source(&item, path_end + 1, item.path);

					bool duplicate = false;
					for (int i = 0; i < job->result_count; i++) {
						if (!strcasecmp(job->results[i].name, item.name) &&
							!strcmp(job->results[i].path, item.path)) {
							duplicate = true;
							break;
						}
						if (!strcasecmp(job->results[i].name, item.name)) {
							job->results[i].duplicate_name = true;
							item.duplicate_name = true;
						}
					}
					if (!duplicate && job->result_count < RESULT_MAX) {
						job->results[job->result_count++] = item;
					}
				}
			}
		}
		if (!end) {
			break;
		}
		line = end + 1;
	}
	finish_result_names(job);
}

static bool encode_path(const char *input, char *output, size_t capacity) {
	static const char hex[] = "0123456789ABCDEF";
	size_t used = 0;
	for (const unsigned char *p = (const unsigned char *)input; *p; p++) {
		unsigned char c = *p;
		if (c == '/' || isalnum(c) || strchr("-_.~", c)) {
			if (used + 1 >= capacity) {
				return false;
			}
			output[used++] = (char)c;
		} else if (c == '%' && isxdigit(p[1]) && isxdigit(p[2])) {
			if (used + 3 >= capacity) {
				return false;
			}
			output[used++] = '%';
			output[used++] = (char)p[1];
			output[used++] = (char)p[2];
			p += 2;
		} else {
			if (used + 3 >= capacity || c == '?' || c == '#' || c == '\\') {
				return false;
			}
			output[used++] = '%';
			output[used++] = hex[c >> 4];
			output[used++] = hex[c & 15];
		}
	}
	output[used] = '\0';
	return true;
}

static bool make_profile_url(const profile_t *item, char *url, size_t capacity) {
	const char *path = item->path;
	if (!strncmp(path, "./", 2)) {
		path += 2;
	}
	char encoded_path[1600];
	char file[768];
	char name[180];
	if (!encode_path(path, encoded_path, sizeof(encoded_path))) {
		return false;
	}
	snprintf(name, sizeof(name), "%s ParametricEQ.txt", item->name);
	http_url_encode(name, file, sizeof(file));
	int n = snprintf(url, capacity, "%s%s/%s", RESULTS_URL, encoded_path, file);
	return n > 0 && (size_t)n < capacity;
}

static size_t copy_name_part(char *out, size_t capacity, const char *source, size_t limit) {
	size_t used = 0;
	while (*source && used < limit && used + 1 < capacity) {
		unsigned char c = (unsigned char)*source;
		size_t width = 1;
		if ((c & 0xe0) == 0xc0) {
			width = 2;
		} else if ((c & 0xf0) == 0xe0) {
			width = 3;
		} else if ((c & 0xf8) == 0xf0) {
			width = 4;
		}
		if (used + width > limit || used + width >= capacity) {
			break;
		}
		if (width == 1 && strchr("/\\:*?\"<>|", c)) {
			out[used++] = '-';
			source++;
			continue;
		}
		memcpy(out + used, source, width);
		used += width;
		source += width;
	}
	out[used] = '\0';
	return used;
}

static void preset_name(const profile_t *item, char *out, size_t size) {
	char model[101];
	char source[31];
	copy_name_part(model, sizeof(model), item->name, item->duplicate_name ? 66 : 100);
	if (!item->duplicate_name) {
		snprintf(out, size, "%s", model);
		return;
	}
	copy_name_part(source, sizeof(source), item->source, 24);
	if (!source[0]) {
		snprintf(source, sizeof(source), "%s", "AutoEq");
	}
	snprintf(out, size, "%s [%s]", model, source);
}

static void cache_path(const profile_t *item, char *out, size_t size) {
	uint32_t hash = hash_text(item->name) ^ hash_text(item->path);
	snprintf(out, size, "%s/%08x.txt", profiles_dir, hash);
}

static bool download_index(job_t *job) {
	char temp[600];
	if (!path_join(temp, sizeof(temp), autoeq_dir, "INDEX.md.tmp")) {
		snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_error_database_save"));
		return false;
	}

	http_stream_t stream;
	if (!http_stream_open(&stream, INDEX_URL, 35)) {
		const char *error = http_last_error();
		snprintf(job->error, sizeof(job->error), "%s",
				 error && error[0] ? error : tr("peq_autoeq_error_index_download"));
		return false;
	}
	if (stream.content_length > (long)INDEX_MAX) {
		http_stream_close(&stream);
		snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_error_index_download"));
		return false;
	}
	long expected_size = stream.content_length;

	FILE *file = fopen(temp, "wb");
	if (!file) {
		http_stream_close(&stream);
		snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_error_database_save"));
		return false;
	}

	char buffer[8192];
	size_t total = 0;
	bool too_large = false;
	bool read_failed = false;
	bool write_failed = false;
	for (;;) {
		int count = http_stream_read(&stream, buffer, (int)sizeof(buffer));
		if (count < 0) {
			read_failed = true;
			break;
		}
		if (count == 0) {
			break;
		}
		if (total + (size_t)count > INDEX_MAX) {
			too_large = true;
			break;
		}
		if (fwrite(buffer, 1, (size_t)count, file) != (size_t)count) {
			write_failed = true;
			break;
		}
		total += (size_t)count;
	}
	http_stream_close(&stream);
	if (fclose(file) != 0) {
		write_failed = true;
	}
	if (!write_failed && expected_size > 0 && total != (size_t)expected_size) {
		read_failed = true;
	}
	if (total == 0) {
		read_failed = true;
	}
	if (!too_large && !read_failed && !write_failed && rename(temp, index_file) == 0) {
		return true;
	}

	remove(temp);
	if (too_large || read_failed) {
		snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_error_index_download"));
	} else {
		snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_error_database_save"));
	}
	return false;
}

static bool load_search_results(job_t *job) {
	struct stat st;
	bool cached = stat(index_file, &st) == 0 && st.st_size > 0 && (size_t)st.st_size <= INDEX_MAX;
	if (!cached && !download_index(job)) {
		return false;
	}

	char *index = read_file(index_file, INDEX_MAX, NULL);
	if (!index) {
		snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_error_database_save"));
		return false;
	}
	parse_index(index, job->query, job);
	free(index);
	return true;
}

static bool download_profile(job_t *job) {
	size_t size = 0;
	char *text = read_file(job->cache, PROFILE_MAX, &size);
	if (text) {
		job->profile_text = text;
		job->profile_size = size;
		return true;
	}

	char url[3072];
	if (!make_profile_url(&job->selected, url, sizeof(url))) {
		snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_error_profile_url"));
		return false;
	}
	if (!http_get(url, &text, &size, PROFILE_MAX, 30)) {
		const char *error = http_last_error();
		snprintf(job->error, sizeof(job->error), "%s",
				 error && error[0] ? error : tr("peq_autoeq_error_profile_download"));
		free(text);
		return false;
	}
	write_file(job->cache, text, size);
	job->profile_text = text;
	job->profile_size = size;
	return true;
}

static void *worker(void *user) {
	job_t *job = user;
	thread_be_background("autoeq");

	if (!ensure_dirs()) {
		snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_need_card"));
	} else if (job->action == JOB_SEARCH) {
		load_search_results(job);
	} else if (job->action == JOB_UPDATE_DATABASE) {
		download_index(job);
	} else {
		download_profile(job);
	}

	if (!gui_post(job_done, job)) {
		free(job->profile_text);
		free(job);
		end_job();
	}
	return NULL;
}

static bool start_job(job_t *job, const char *busy_text) {
	if (!begin_job()) {
		free(job);
		return false;
	}
	toast_busy(busy_text);

	pthread_t thread;
	if (pthread_create(&thread, NULL, worker, job) != 0) {
		end_job();
		free(job);
		toast_busy_end();
		toast_error(tr("peq_autoeq_error_start"));
		return false;
	}
	pthread_detach(thread);
	return true;
}

static bool save_and_apply(job_t *job) {
	char peq_dir[600];
	char stage_name[40];
	char stage_path[700];
	char preset_path[700];
	if (!cfg || !cfg->sd_root_path || !cfg->sd_root_path[0] ||
		!path_join(peq_dir, sizeof(peq_dir), cfg->sd_root_path, "PEQ") || !make_dir(peq_dir)) {
		toast_error(tr("peq_autoeq_error_peq_folder"));
		return false;
	}

	uint32_t id = hash_text(job->preset) ^ hash_text(job->profile_text);
	snprintf(stage_name, sizeof(stage_name), "AutoEq Stage %08x", id);
	int n = snprintf(stage_path, sizeof(stage_path), "%s/%s.txt", peq_dir, stage_name);
	int p = snprintf(preset_path, sizeof(preset_path), "%s/%s.txt", peq_dir, job->preset);
	if (n < 0 || (size_t)n >= sizeof(stage_path) || p < 0 || (size_t)p >= sizeof(preset_path) ||
		!write_file(stage_path, job->profile_text, job->profile_size)) {
		toast_error(tr("peq_autoeq_error_save_profile"));
		return false;
	}

	int ignored = 0;
	if (!peq_preset_load(stage_name, &ignored)) {
		remove(stage_path);
		toast_error(tr("peq_autoeq_error_unsupported_profile"));
		return false;
	}
	if (rename(stage_path, preset_path) != 0) {
		remove(stage_path);
		toast_error(tr("peq_autoeq_error_save_preset"));
		return false;
	}

	peq_set_enabled(true);
	if (reload_cb) {
		reload_cb();
	}
	if (ignored) {
		toast_success(tr("peq_autoeq_unsupported_filters"));
	} else {
		toast_success(tr("peq_autoeq_profile_applied"));
	}
	switch_screen(peqpage_screen());
	return true;
}

static void rebuild_results(job_t *job) {
	result_count = job->result_count;
	memcpy(result_items, job->results, sizeof(result_items));
	lv_obj_clean(search_list);
	for (int i = 0; i < result_count; i++) {
		lv_obj_t *row = settingsrow_add(search_list, result_items[i].display, NULL, result_clicked,
									(void *)(intptr_t)i);
		lv_obj_update_layout(row);
		lv_obj_t *label = settingsrow_name_label(row);
		lv_obj_t *chevron = lv_obj_get_child(row, 1);
		int32_t width = lv_obj_get_content_width(row) - (chevron ? lv_obj_get_width(chevron) + 12 : 0);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_width(label, width);
		lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
	}
	if (result_count) {
		lv_obj_add_flag(results_empty, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_remove_flag(results_empty, LV_OBJ_FLAG_HIDDEN);
	}
	switch_screen(results_screen);
}

static void job_done(void *user) {
	job_t *job = user;
	end_job();
	toast_busy_end();

	if (job->error[0]) {
		toast_error(job->error);
	} else if (job->action == JOB_SEARCH) {
		rebuild_results(job);
	} else if (job->action == JOB_UPDATE_DATABASE) {
		toast_success(tr("peq_autoeq_database_updated"));
	} else {
		save_and_apply(job);
	}
	free(job->profile_text);
	free(job);
}

static void result_clicked(lv_event_t *event) {
	if (switcher_back_drag_active() || is_busy()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(event);
	if (index < 0 || index >= result_count) {
		return;
	}

	job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		toast_error(tr("out_of_memory"));
		return;
	}
	job->action = JOB_DOWNLOAD_PROFILE;
	job->selected = result_items[index];
	preset_name(&job->selected, job->preset, sizeof(job->preset));
	cache_path(&job->selected, job->cache, sizeof(job->cache));
	start_job(job, tr("peq_autoeq_downloading_profile"));
}

static void search_accept(lv_event_t *event) {
	(void)event;
	char query[128];
	snprintf(query, sizeof(query), "%s", lv_textarea_get_text(search_field));
	char *start = query;
	while (isspace((unsigned char)*start)) {
		start++;
	}
	size_t length = strlen(start);
	while (length && isspace((unsigned char)start[length - 1])) {
		start[--length] = '\0';
	}
	if (!*start) {
		toast_error(tr("peq_autoeq_enter_name"));
		return;
	}

	job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		toast_error(tr("out_of_memory"));
		return;
	}
	job->action = JOB_SEARCH;
	snprintf(job->query, sizeof(job->query), "%s", start);
	start_job(job, tr("peq_autoeq_downloading_database"));
}

static void update_database(lv_event_t *event) {
	(void)event;
	if (is_busy()) {
		return;
	}
	job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		toast_error(tr("out_of_memory"));
		return;
	}
	job->action = JOB_UPDATE_DATABASE;
	start_job(job, tr("peq_autoeq_downloading_database"));
}

static void build_pages(gui_config_t *config) {
	search_screen = lv_obj_create(NULL);
	lv_obj_t *search_container = settingsrow_page(search_screen, config, "peq_autoeq_search");
	lv_obj_set_height(search_container, config->screen_height - settingsrow_content_top(config) - 316);

	search_field = lv_textarea_create(search_container);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_max_length(search_field, 120);
	lv_textarea_set_placeholder_text(search_field, tr("peq_autoeq_headphones"));
	lv_obj_set_size(search_field, lv_pct(100), 62);
	lv_obj_add_style(search_field, &theme_style_card, 0);
	lv_obj_set_style_radius(search_field, 12, 0);
	lv_obj_set_style_border_width(search_field, 0, 0);
	lv_obj_set_style_pad_all(search_field, 14, 0);
	lv_obj_set_style_text_font(search_field, &font_ui_24, 0);
	keyboard_style_caret(search_field);
	settingsrow_action(search_container, "peq_autoeq_update_database", update_database, NULL);
	search_keyboard = keyboard_create(search_screen, config->screen_width, 316, search_field, NULL,
									 "peq_autoeq_search_button", search_accept, NULL);

	results_screen = lv_obj_create(NULL);
	search_list = settingsrow_page(results_screen, config, "peq_autoeq_results");
	results_empty = lv_label_create(results_screen);
	lv_label_set_text(results_empty, tr("peq_autoeq_no_results"));
	lv_obj_add_style(results_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(results_empty, &font_ui_24, 0);
	lv_obj_set_width(results_empty, config->screen_width - 2 * config->padding);
	lv_obj_set_style_text_align(results_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(results_empty, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(config) + 100);
	lv_obj_add_flag(results_empty, LV_OBJ_FLAG_HIDDEN);
}

void peqautoeq_init(gui_config_t *config) {
	cfg = config;
	set_paths();
	build_pages(config);
}

lv_obj_t *peqautoeq_screen(void) {
	return search_screen;
}

void peqautoeq_set_reload_cb(void (*cb)(void)) {
	reload_cb = cb;
}

void peqautoeq_open(void) {
	lv_textarea_set_text(search_field, "");
	keyboard_reset(search_keyboard);
	switch_screen(search_screen);
	lv_obj_add_state(search_field, LV_STATE_FOCUSED);
	lv_obj_send_event(search_field, LV_EVENT_FOCUSED, NULL);
}
