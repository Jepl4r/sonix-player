#define _GNU_SOURCE 1

#include "lastfm.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/core/md5.h"
#include "src/system/library/metadata.h"
#include "src/system/net/http.h"

#define LASTFM_API_URL "https://ws.audioscrobbler.com/2.0/"
#define LASTFM_CONFIG_SECTION "lastfm"
#define LASTFM_QUEUE_MAX_AGE (13LL * 24LL * 60LL * 60LL)
#define LASTFM_POLL_MS 250
#define LASTFM_API_TIMEOUT 15
#define LASTFM_BODY_LIMIT (256U * 1024U)
#define LASTFM_TEXT_MAX 256
#define LASTFM_PASSWORD_MAX 512
#define LASTFM_FORM_MAX 8192
#define LASTFM_QUEUE_LINE_MAX 4096

#define CFG_ENABLED "enabled"
#define CFG_API_KEY "api_key"
#define CFG_API_SECRET "api_secret"

#define LASTFM_STATE_FILE ".local/lastfm.state"
#define LASTFM_QUEUE_FILE ".local/lastfm.queue"
#define LASTFM_QUEUE_TMP ".local/lastfm.queue.tmp"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
	time_t timestamp;
	int duration;
	char artist[LASTFM_TEXT_MAX];
	char title[LASTFM_TEXT_MAX];
	char album[LASTFM_TEXT_MAX];
} lastfm_track_t;

typedef struct {
	const char *name;
	const char *value;
} param_t;

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_cond = PTHREAD_COND_INITIALIZER;
static pthread_t worker_thread;
static bool worker_started;
static bool worker_stop;

static bool enabled;
static bool logging_in;
static char api_key[128];
static char api_secret[128];
static char session_key[128];
static char username[LASTFM_TEXT_MAX];

static char sd_root[512];
static char state_path[600];
static char queue_path[600];
static char queue_tmp_path[600];

static char login_username[LASTFM_TEXT_MAX];
static char login_password[LASTFM_PASSWORD_MAX];
static bool login_pending;
static bool clear_state_pending;

static char last_message[192];
static char status_text[192];
static uint64_t message_serial;

static lastfm_track_t current_track;
static bool current_track_valid;
static bool scrobbled_this_track;
static bool scrobble_pending;
static bool scrobble_in_flight;
static bool now_playing_pending;
static uint64_t track_generation;
static time_t next_queue_sync;
static char current_audio_file[512];
static audio_status_t previous_audio_status = AUDIO_STATUS_STOPPED;

static void set_message_locked(const char *text) {
	snprintf(last_message, sizeof(last_message), "%s", text ? text : "");
	message_serial++;
}

static void set_status_locked(const char *text) {
	snprintf(status_text, sizeof(status_text), "%s", text ? text : "");
}

static void ensure_local_dir(void) {
	if (sd_root[0]) {
		char path[560];
		snprintf(path, sizeof(path), "%s/.local", sd_root);
		(void)mkdir(path, 0755);
	}
}

static void build_paths(const char *root) {
	if (root && root[0]) {
		snprintf(sd_root, sizeof(sd_root), "%s", root);
	} else {
		snprintf(sd_root, sizeof(sd_root), "%s", "/tmp");
	}

	ensure_local_dir();
	snprintf(state_path, sizeof(state_path), "%s/%s", sd_root, LASTFM_STATE_FILE);
	snprintf(queue_path, sizeof(queue_path), "%s/%s", sd_root, LASTFM_QUEUE_FILE);
	snprintf(queue_tmp_path, sizeof(queue_tmp_path), "%s/%s", sd_root, LASTFM_QUEUE_TMP);

	// A host run can point sd_root at a folder that is not writable. Keep the
	// service useful for tests rather than failing every state/queue write.
	FILE *probe = fopen(state_path, "a");
	if (!probe) {
		snprintf(sd_root, sizeof(sd_root), "%s", "/tmp");
		ensure_local_dir();
		snprintf(state_path, sizeof(state_path), "%s/%s", sd_root, LASTFM_STATE_FILE);
		snprintf(queue_path, sizeof(queue_path), "%s/%s", sd_root, LASTFM_QUEUE_FILE);
		snprintf(queue_tmp_path, sizeof(queue_tmp_path), "%s/%s", sd_root, LASTFM_QUEUE_TMP);
	} else {
		fclose(probe);
	}
}

static void load_session_state(void) {
	FILE *f = fopen(state_path, "r");
	if (!f) {
		return;
	}

	char line[512];
	if (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		snprintf(session_key, sizeof(session_key), "%s", line);
	}
	if (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		snprintf(username, sizeof(username), "%s", line);
	}
	fclose(f);
}

static void save_session_state(void) {
	if (!session_key[0]) {
		unlink(state_path);
		return;
	}

	char tmp[620];
	snprintf(tmp, sizeof(tmp), "%s.tmp", state_path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return;
	}
	fprintf(f, "%s\n%s\n", session_key, username);
	if (fflush(f) != 0 || fclose(f) != 0 || rename(tmp, state_path) != 0) {
		unlink(tmp);
	}
}

static char *url_encode(const char *value, char *out, size_t out_size) {
	static const char hex[] = "0123456789ABCDEF";
	if (!out || out_size == 0) {
		return out;
	}
	if (!value) {
		value = "";
	}

	size_t p = 0;
	for (const unsigned char *s = (const unsigned char *)value; *s; ++s) {
		bool safe = (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') ||
				   *s == '-' || *s == '_' || *s == '.' || *s == '~';
		size_t need = safe ? 1 : 3;
		if (p + need + 1 >= out_size) {
			break;
		}
		if (safe) {
			out[p++] = (char)*s;
		} else {
			out[p++] = '%';
			out[p++] = hex[*s >> 4];
			out[p++] = hex[*s & 0x0f];
		}
	}
	out[p] = '\0';
	return out;
}

static int param_cmp(const void *a, const void *b) {
	const param_t *pa = a;
	const param_t *pb = b;
	return strcmp(pa->name, pb->name);
}

static bool build_form(const param_t *input, size_t count, char *out, size_t out_size) {
	param_t params[16];
	if (count > ARRAY_LEN(params)) {
		return false;
	}
	memcpy(params, input, count * sizeof(params[0]));
	qsort(params, count, sizeof(params[0]), param_cmp);

	size_t used = 0;
	for (size_t i = 0; i < count; i++) {
		char ek[512];
		char ev[2048];
		url_encode(params[i].name, ek, sizeof(ek));
		url_encode(params[i].value, ev, sizeof(ev));

		int n = snprintf(out + used, out_size - used, "%s%s=%s", i ? "&" : "", ek, ev);
		if (n < 0 || (size_t)n >= out_size - used) {
			return false;
		}
		used += (size_t)n;
	}
	return true;
}

static bool add_sig_and_build(const param_t *input, size_t count, const char *request_api_key,
					const char *request_api_secret, char *out, size_t out_size) {
	param_t signed_params[16];
	if (count + 1 > ARRAY_LEN(signed_params)) {
		return false;
	}
	memcpy(signed_params, input, count * sizeof(signed_params[0]));
	signed_params[count].name = "api_key";
	signed_params[count].value = request_api_key;

	param_t sig_params[16];
	memcpy(sig_params, signed_params, (count + 1) * sizeof(sig_params[0]));
	qsort(sig_params, count + 1, sizeof(sig_params[0]), param_cmp);

	char sign_text[LASTFM_FORM_MAX];
	size_t used = 0;
	for (size_t i = 0; i < count + 1; i++) {
		int n = snprintf(sign_text + used, sizeof(sign_text) - used, "%s%s", sig_params[i].name,
						 sig_params[i].value ? sig_params[i].value : "");
		if (n < 0 || (size_t)n >= sizeof(sign_text) - used) {
			return false;
		}
		used += (size_t)n;
	}
	if (used + strlen(request_api_secret) + 1 >= sizeof(sign_text)) {
		return false;
	}
	memcpy(sign_text + used, request_api_secret, strlen(request_api_secret) + 1);

	char sig[MD5_HEX_LEN];
	md5_hex(sign_text, strlen(sign_text), sig);

	param_t request_params[16];
	memcpy(request_params, signed_params, (count + 1) * sizeof(request_params[0]));
	request_params[count + 1].name = "api_sig";
	request_params[count + 1].value = sig;
	return build_form(request_params, count + 2, out, out_size);
}

static bool api_call(const param_t *params, size_t count, const char *request_api_key, const char *request_api_secret,
				 const char *request_session_key, char **body_out, size_t *body_len, int *status_out) {
	if (!request_api_key || !request_api_secret || !request_api_key[0] || !request_api_secret[0]) {
		return false;
	}
	char form[LASTFM_FORM_MAX];
	if (!add_sig_and_build(params, count, request_api_key, request_api_secret, form, sizeof(form))) {
		return false;
	}

	(void)request_session_key;
	http_req_t req = {
		.method = "POST",
		.extra_headers = NULL,
		.body = form,
		.body_len = strlen(form),
		.content_type = "application/x-www-form-urlencoded",
		.want_error_body = true,
		.allow_empty_body = false,
	};

	return http_request(LASTFM_API_URL, &req, body_out, body_len, LASTFM_BODY_LIMIT, LASTFM_API_TIMEOUT, status_out);
}

static bool response_ok(const char *body) {
	return body && strstr(body, "status=\"ok\"") != NULL;
}

static const char *response_error(const char *body) {
	if (!body) {
		return NULL;
	}
	const char *p = strstr(body, "<error");
	if (!p) {
		return NULL;
	}
	p = strchr(p, '>');
	if (!p) {
		return NULL;
	}
	p++;
	static char error[160];
	size_t i = 0;
	while (p[i] && p[i] != '<' && i + 1 < sizeof(error)) {
		error[i] = p[i];
		i++;
	}
	error[i] = '\0';
	return error[0] ? error : NULL;
}

static bool extract_tag(const char *body, const char *tag, char *out, size_t out_size) {
	if (!body || !tag || !out || out_size == 0) {
		return false;
	}
	char open[64];
	snprintf(open, sizeof(open), "<%s>", tag);
	const char *start = strstr(body, open);
	if (!start) {
		return false;
	}
	start += strlen(open);
	char close[64];
	snprintf(close, sizeof(close), "</%s>", tag);
	const char *end = strstr(start, close);
	if (!end || end <= start) {
		return false;
	}
	size_t len = (size_t)(end - start);
	if (len >= out_size) {
		len = out_size - 1;
	}
	memcpy(out, start, len);
	out[len] = '\0';
	return true;
}

static bool do_login_request(const char *user, const char *password, const char *request_api_key,
						  const char *request_api_secret, char *new_session, size_t session_size,
						  char *new_user, size_t user_size, char *error_out, size_t error_size) {
	param_t params[] = {
		{"method", "auth.getMobileSession"},
		{"username", user},
		{"password", password},
	};

	char *body = NULL;
	size_t body_len = 0;
	int status = 0;
	bool ok = api_call(params, ARRAY_LEN(params), request_api_key, request_api_secret, NULL, &body, &body_len, &status);
	if (!ok || !response_ok(body)) {
		const char *err = response_error(body);
		if (err) {
			snprintf(error_out, error_size, "%s", err);
		} else if (http_last_error()) {
			snprintf(error_out, error_size, "%s", http_last_error());
		} else {
			snprintf(error_out, error_size, "HTTP %d", status);
		}
		free(body);
		return false;
	}

	bool got_key = extract_tag(body, "key", new_session, session_size);
	bool got_name = extract_tag(body, "name", new_user, user_size);
	free(body);
	return got_key && got_name;
}

static bool do_now_playing(const lastfm_track_t *track, const char *request_api_key, const char *request_api_secret,
					const char *request_session_key) {
	char duration[32];
	snprintf(duration, sizeof(duration), "%d", track->duration > 0 ? track->duration : 0);
	param_t request[6] = {
		{"method", "track.updateNowPlaying"},
		{"sk", request_session_key},
		{"track", track->title},
		{"artist", track->artist},
		{"album", track->album},
		{"duration", duration},
	};

	char *body = NULL;
	size_t body_len = 0;
	int status = 0;
	size_t count = track->album[0] ? 6 : 5;
	bool ok = api_call(request, count, request_api_key, request_api_secret, request_session_key, &body, &body_len, &status) &&
			response_ok(body);
	free(body);
	return ok;
}

static bool do_scrobble(const lastfm_track_t *track, const char *request_api_key, const char *request_api_secret,
					const char *request_session_key) {
	char timestamp[32];
	char duration[32];
	snprintf(timestamp, sizeof(timestamp), "%ld", (long)track->timestamp);
	snprintf(duration, sizeof(duration), "%d", track->duration > 0 ? track->duration : 0);

	param_t request[7];
	size_t count = 0;
	request[count++] = (param_t){"method", "track.scrobble"};
	request[count++] = (param_t){"sk", request_session_key};
	request[count++] = (param_t){"track", track->title};
	request[count++] = (param_t){"artist", track->artist};
	if (track->album[0]) {
		request[count++] = (param_t){"album", track->album};
	}
	request[count++] = (param_t){"timestamp", timestamp};
	request[count++] = (param_t){"duration", duration};

	char *body = NULL;
	size_t body_len = 0;
	int status = 0;
	bool ok = api_call(request, count, request_api_key, request_api_secret, request_session_key, &body, &body_len, &status) &&
			response_ok(body);
	free(body);
	return ok;
}

static bool queue_encode(const char *value, char *out, size_t out_size) {
	url_encode(value ? value : "", out, out_size);
	return out[0] != '\0' || !value || !value[0];
}

static bool parse_queue_line(const char *line, lastfm_track_t *out) {
	if (!line || !out) {
		return false;
	}
	char ts[32], dur[32], artist[768], title[768], album[768];
	if (sscanf(line, "%31[^\t]\t%31[^\t]\t%767[^\t]\t%767[^\t]\t%767[^\r\n]", ts, dur, artist, title, album) != 5) {
		return false;
	}
	char *end = NULL;
	long timestamp = strtol(ts, &end, 10);
	if (!end || *end != '\0' || timestamp <= 0) {
		return false;
	}
	long duration = strtol(dur, &end, 10);
	if (!end || *end != '\0' || duration < 0) {
		duration = 0;
	}

	// Decode the same %HH representation used by the writer.
	char *fields[] = {artist, title, album};
	for (size_t j = 0; j < ARRAY_LEN(fields); j++) {
		char decoded[768];
		size_t p = 0;
		for (size_t i = 0; fields[j][i] && p + 1 < sizeof(decoded); i++) {
			if (fields[j][i] == '%' && fields[j][i + 1] && fields[j][i + 2]) {
				unsigned value = 0;
				if (sscanf(&fields[j][i + 1], "%2x", &value) == 1) {
					decoded[p++] = (char)value;
					i += 2;
					continue;
				}
			}
			decoded[p++] = fields[j][i];
		}
		decoded[p] = '\0';
		snprintf(fields[j], 768, "%s", decoded);
	}

	memset(out, 0, sizeof(*out));
	out->timestamp = (time_t)timestamp;
	out->duration = (int)duration;
	snprintf(out->artist, sizeof(out->artist), "%s", artist);
	snprintf(out->title, sizeof(out->title), "%s", title);
	snprintf(out->album, sizeof(out->album), "%s", album);
	return out->artist[0] != '\0' && out->title[0] != '\0';
}

static bool write_queue_item(FILE *f, const lastfm_track_t *item) {
	char artist[768], title[768], album[768];
	if (!queue_encode(item->artist, artist, sizeof(artist)) || !queue_encode(item->title, title, sizeof(title)) ||
		!queue_encode(item->album, album, sizeof(album))) {
		return false;
	}
	return fprintf(f, "%ld\t%d\t%s\t%s\t%s\n", (long)item->timestamp, item->duration, artist, title, album) > 0;
}

static bool prune_queue(void) {
	FILE *in = fopen(queue_path, "r");
	if (!in) {
		return true;
	}
	FILE *out = fopen(queue_tmp_path, "w");
	if (!out) {
		fclose(in);
		return false;
	}

	time_t cutoff = time(NULL) - LASTFM_QUEUE_MAX_AGE;
	char line[LASTFM_QUEUE_LINE_MAX];
	while (fgets(line, sizeof(line), in)) {
		lastfm_track_t item;
		if (parse_queue_line(line, &item) && item.timestamp >= cutoff) {
			write_queue_item(out, &item);
		}
	}
	fclose(in);
	if (fclose(out) != 0) {
		unlink(queue_tmp_path);
		return false;
	}
	if (rename(queue_tmp_path, queue_path) != 0) {
		unlink(queue_tmp_path);
		return false;
	}
	return true;
}

static bool enqueue_scrobble(const lastfm_track_t *item) {
	if (!item || !item->artist[0] || !item->title[0] || item->timestamp <= 0) {
		return false;
	}
	prune_queue();
	FILE *f = fopen(queue_path, "a");
	if (!f) {
		return false;
	}
	bool ok = write_queue_item(f, item);
	fclose(f);
	return ok;
}

static bool read_first_queued(lastfm_track_t *out) {
	FILE *f = fopen(queue_path, "r");
	if (!f) {
		return false;
	}
	char line[LASTFM_QUEUE_LINE_MAX];
	while (fgets(line, sizeof(line), f)) {
		if (parse_queue_line(line, out)) {
			fclose(f);
			return true;
		}
	}
	fclose(f);
	return false;
}

static bool remove_first_queued(void) {
	FILE *in = fopen(queue_path, "r");
	if (!in) {
		return true;
	}
	FILE *out = fopen(queue_tmp_path, "w");
	if (!out) {
		fclose(in);
		return false;
	}
	bool removed = false;
	char line[LASTFM_QUEUE_LINE_MAX];
	while (fgets(line, sizeof(line), in)) {
		if (!removed) {
			lastfm_track_t item;
			if (parse_queue_line(line, &item)) {
				removed = true;
				continue;
			}
		}
		fputs(line, out);
	}
	fclose(in);
	if (fclose(out) != 0) {
		unlink(queue_tmp_path);
		return false;
	}
	if (rename(queue_tmp_path, queue_path) != 0) {
		unlink(queue_tmp_path);
		return false;
	}
	return true;
}

static void clear_session_locked(void) {
	session_key[0] = '\0';
	username[0] = '\0';
	unlink(state_path);
}

static bool credentials_ready_locked(void) {
	return api_key[0] && api_secret[0] && session_key[0];
}

static void read_track_metadata(lastfm_track_t *track, const char *file, double duration, time_t stamp) {
	song_metadata_t metadata;
	memset(&metadata, 0, sizeof(metadata));
	metadata_read(file, &metadata);
	memset(track, 0, sizeof(*track));
	track->timestamp = stamp;
	track->duration = duration > 0 ? (int)(duration + 0.5) : 0;
	snprintf(track->artist, sizeof(track->artist), "%s", metadata.artist);
	snprintf(track->title, sizeof(track->title), "%s", metadata.title);
	snprintf(track->album, sizeof(track->album), "%s", metadata.album);
}

static void handle_audio_poll(void) {
	char file[512];
	audio_get_current_file(file, sizeof(file));
	audio_status_t st = audio_get_status();
	double current = 0;
	double total = 0;
	audio_get_progress(&current, &total);
	bool playing = st == AUDIO_STATUS_PLAYING;

	// A path change starts a new Last.fm play. A stopped->playing transition on
	// the same path also starts a new play (pressing Play again), while paused->
	// playing is only a resume of the same play.
	bool same_file = current_audio_file[0] && file[0] && strcmp(current_audio_file, file) == 0;
	bool new_play = playing && file[0] && (!same_file || !current_track_valid || previous_audio_status == AUDIO_STATUS_STOPPED);
	if (new_play) {
		snprintf(current_audio_file, sizeof(current_audio_file), "%s", file);
		lastfm_track_t next;
		read_track_metadata(&next, file, total, time(NULL));

		pthread_mutex_lock(&state_mutex);
		current_track = next;
		current_track_valid = next.artist[0] && next.title[0];
		scrobbled_this_track = false;
		scrobble_pending = false;
		now_playing_pending = current_track_valid;
		track_generation++;
		pthread_mutex_unlock(&state_mutex);
		previous_audio_status = st;
		return;
	}

	if (!playing && !file[0]) {
		current_audio_file[0] = '\0';
	}

	pthread_mutex_lock(&state_mutex);
	if (current_track_valid && total > 0) {
		current_track.duration = (int)(total + 0.5);
	}

	if (enabled && session_key[0] && current_track_valid && !scrobbled_this_track && !scrobble_pending &&
		total >= 30.0 && current >= (total < 480.0 ? total / 2.0 : 240.0)) {
		scrobble_pending = true;
	}
	pthread_mutex_unlock(&state_mutex);
	previous_audio_status = st;
}

static void wait_250ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += LASTFM_POLL_MS * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&state_mutex);
	if (!worker_stop) {
		pthread_cond_timedwait(&state_cond, &state_mutex, &ts);
	}
	pthread_mutex_unlock(&state_mutex);
}

static void *lastfm_worker(void *arg) {
	(void)arg;
	next_queue_sync = 0;

	for (;;) {
		char login_user[LASTFM_TEXT_MAX] = {0};
		char login_pass[LASTFM_PASSWORD_MAX] = {0};
		char request_key[128] = {0};
		char request_secret[128] = {0};
		char request_session[128] = {0};
		bool do_login = false;
		bool do_logout = false;
		bool send_now_playing = false;
		bool send_scrobble = false;
		bool sync_queue = false;
		lastfm_track_t track;
		uint64_t generation = 0;

		pthread_mutex_lock(&state_mutex);
		if (worker_stop) {
			pthread_mutex_unlock(&state_mutex);
			break;
		}

		if (clear_state_pending) {
			clear_state_pending = false;
			do_logout = true;
		}
		if (login_pending) {
			login_pending = false;
			do_login = true;
			snprintf(login_user, sizeof(login_user), "%s", login_username);
			snprintf(login_pass, sizeof(login_pass), "%s", login_password);
			memset(login_password, 0, sizeof(login_password));
			strncpy(request_key, api_key, sizeof(request_key) - 1);
			strncpy(request_secret, api_secret, sizeof(request_secret) - 1);
		}

		if (!do_logout && now_playing_pending && credentials_ready_locked()) {
			now_playing_pending = false;
			send_now_playing = true;
			track = current_track;
			strncpy(request_key, api_key, sizeof(request_key) - 1);
			strncpy(request_secret, api_secret, sizeof(request_secret) - 1);
			strncpy(request_session, session_key, sizeof(request_session) - 1);
		}
		if (!do_logout && scrobble_pending && credentials_ready_locked() && !scrobble_in_flight) {
			scrobble_pending = false;
			scrobble_in_flight = true;
			send_scrobble = true;
			track = current_track;
			generation = track_generation;
			strncpy(request_key, api_key, sizeof(request_key) - 1);
			strncpy(request_secret, api_secret, sizeof(request_secret) - 1);
			strncpy(request_session, session_key, sizeof(request_session) - 1);
		}
		if (!do_logout && enabled && session_key[0] && time(NULL) >= next_queue_sync) {
			sync_queue = true;
			next_queue_sync = time(NULL) + 15;
			strncpy(request_key, api_key, sizeof(request_key) - 1);
			strncpy(request_secret, api_secret, sizeof(request_secret) - 1);
			strncpy(request_session, session_key, sizeof(request_session) - 1);
		}
		pthread_mutex_unlock(&state_mutex);

		if (do_logout) {
			pthread_mutex_lock(&state_mutex);
			clear_session_locked();
			set_status_locked(enabled && api_key[0] && api_secret[0] ? "Not logged in" : "API key and secret required");
			set_message_locked("Logged out of Last.fm");
			pthread_mutex_unlock(&state_mutex);
		}

		if (do_login) {
			char new_session[128] = {0};
			char new_username[LASTFM_TEXT_MAX] = {0};
			char error[160] = {0};
			bool ok = do_login_request(login_user, login_pass, request_key, request_secret, new_session,
								  sizeof(new_session), new_username, sizeof(new_username), error, sizeof(error));
			memset(login_pass, 0, sizeof(login_pass));
			pthread_mutex_lock(&state_mutex);
			logging_in = false;
			if (ok) {
				snprintf(session_key, sizeof(session_key), "%s", new_session);
				snprintf(username, sizeof(username), "%s", new_username);
				save_session_state();
				set_status_locked("Connected");
				set_message_locked("Logged in to Last.fm");
				next_queue_sync = 0;
			} else {
				set_status_locked(error[0] ? error : "Login failed");
				char msg[192];
				snprintf(msg, sizeof(msg), "Last.fm login failed: %s", error[0] ? error : "unknown error");
				set_message_locked(msg);
			}
			pthread_mutex_unlock(&state_mutex);
		}

		if (send_now_playing && request_session[0]) {
			(void)do_now_playing(&track, request_key, request_secret, request_session);
		}

		if (send_scrobble && request_session[0]) {
			bool ok = do_scrobble(&track, request_key, request_secret, request_session);
			pthread_mutex_lock(&state_mutex);
			if (generation == track_generation && ok) {
				scrobbled_this_track = true;
			} else if (generation == track_generation && !ok) {
				if (enqueue_scrobble(&track)) {
					scrobbled_this_track = true;
				}
			}
			scrobble_in_flight = false;
			pthread_mutex_unlock(&state_mutex);
		}

		if (sync_queue && request_session[0]) {
			lastfm_track_t queued;
			if (read_first_queued(&queued) && do_scrobble(&queued, request_key, request_secret, request_session)) {
				remove_first_queued();
			}
			prune_queue();
		}

		handle_audio_poll();
		wait_250ms();
	}
	return NULL;
}

void lastfm_init(const char *root) {
	pthread_mutex_lock(&state_mutex);
	if (worker_started) {
		pthread_mutex_unlock(&state_mutex);
		return;
	}

	build_paths(root);
	load_session_state();
	previous_audio_status = AUDIO_STATUS_STOPPED;
	current_audio_file[0] = '\0';
	enabled = config_get_bool(LASTFM_CONFIG_SECTION, CFG_ENABLED, false);
	snprintf(api_key, sizeof(api_key), "%s", config_get(LASTFM_CONFIG_SECTION, CFG_API_KEY, ""));
	snprintf(api_secret, sizeof(api_secret), "%s", config_get(LASTFM_CONFIG_SECTION, CFG_API_SECRET, ""));
	if (!enabled) {
		set_status_locked("Disabled");
	} else if (!api_key[0] || !api_secret[0]) {
		set_status_locked("API key and secret required");
	} else if (!session_key[0]) {
		set_status_locked("Not logged in");
	} else {
		set_status_locked("Connected");
	}

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 64 * 1024);
	int rc = pthread_create(&worker_thread, &attr, lastfm_worker, NULL);
	pthread_attr_destroy(&attr);
	if (rc != 0) {
		set_status_locked("Worker unavailable");
		set_message_locked("Could not start Last.fm worker");
		pthread_mutex_unlock(&state_mutex);
		return;
	}
	worker_started = true;
	pthread_mutex_unlock(&state_mutex);
}

void lastfm_set_enabled(bool value) {
	pthread_mutex_lock(&state_mutex);
	enabled = value;
	config_set_bool(LASTFM_CONFIG_SECTION, CFG_ENABLED, enabled);
	config_save();
	if (!enabled) {
		set_status_locked("Disabled");
	} else if (!api_key[0] || !api_secret[0]) {
		set_status_locked("API key and secret required");
	} else if (!session_key[0]) {
		set_status_locked("Not logged in");
	} else {
		set_status_locked("Connected");
		next_queue_sync = 0;
	}
	pthread_cond_signal(&state_cond);
	pthread_mutex_unlock(&state_mutex);
}

static void set_credential(const char *key, char *dst, size_t dst_size, const char *value) {
	pthread_mutex_lock(&state_mutex);
	snprintf(dst, dst_size, "%s", value ? value : "");
	config_set(LASTFM_CONFIG_SECTION, key, dst);
	config_save();
	clear_session_locked();
	if (!api_key[0] || !api_secret[0]) {
		set_status_locked("API key and secret required");
	} else {
		set_status_locked("Not logged in");
	}
	set_message_locked("Last.fm credentials updated; please log in again");
	pthread_mutex_unlock(&state_mutex);
}

void lastfm_set_api_key(const char *value) { set_credential(CFG_API_KEY, api_key, sizeof(api_key), value); }

void lastfm_set_api_secret(const char *value) { set_credential(CFG_API_SECRET, api_secret, sizeof(api_secret), value); }

void lastfm_login(const char *user, const char *password) {
	pthread_mutex_lock(&state_mutex);
	if (!api_key[0] || !api_secret[0]) {
		set_message_locked("Set the Last.fm API key and API secret first");
		pthread_mutex_unlock(&state_mutex);
		return;
	}
	if (!user || !user[0] || !password || !password[0] || logging_in) {
		pthread_mutex_unlock(&state_mutex);
		return;
	}
	snprintf(login_username, sizeof(login_username), "%s", user);
	snprintf(login_password, sizeof(login_password), "%s", password);
	logging_in = true;
	login_pending = true;
	set_status_locked("Logging in...");
	set_message_locked("Logging in to Last.fm...");
	pthread_cond_signal(&state_cond);
	pthread_mutex_unlock(&state_mutex);
}

void lastfm_logout(void) {
	pthread_mutex_lock(&state_mutex);
	logging_in = false;
	login_pending = false;
	clear_state_pending = true;
	set_status_locked("Not logged in");
	pthread_cond_signal(&state_cond);
	pthread_mutex_unlock(&state_mutex);
}

void lastfm_get_api_key(char *out, size_t out_size) {
	if (!out || out_size == 0) return;
	pthread_mutex_lock(&state_mutex);
	snprintf(out, out_size, "%s", api_key);
	pthread_mutex_unlock(&state_mutex);
}

void lastfm_get_api_secret(char *out, size_t out_size) {
	if (!out || out_size == 0) return;
	pthread_mutex_lock(&state_mutex);
	snprintf(out, out_size, "%s", api_secret);
	pthread_mutex_unlock(&state_mutex);
}

bool lastfm_network_wanted(void) {
	pthread_mutex_lock(&state_mutex);
	bool wanted = enabled && (session_key[0] != '\0' || logging_in || login_pending ||
		now_playing_pending || scrobble_pending);
	pthread_mutex_unlock(&state_mutex);
	return wanted;
}

void lastfm_get_snapshot(lastfm_snapshot_t *out) {
	if (!out) return;
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&state_mutex);
	out->enabled = enabled;
	out->logged_in = session_key[0] != '\0';
	out->logging_in = logging_in;
	out->api_key_configured = api_key[0] != '\0';
	out->api_secret_configured = api_secret[0] != '\0';
	snprintf(out->username, sizeof(out->username), "%s", username);
	snprintf(out->status, sizeof(out->status), "%s", status_text);
	snprintf(out->last_message, sizeof(out->last_message), "%s", last_message);
	out->message_serial = message_serial;
	pthread_mutex_unlock(&state_mutex);
}
