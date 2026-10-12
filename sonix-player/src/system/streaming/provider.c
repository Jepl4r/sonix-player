#include "provider.h"

#include <stdio.h>
#include <string.h>

// Filled at start-up, read-only afterwards (see provider.h): no lock needed,
// which matters because the power loop asks it several times a second.
static stream_provider_t *providers[STREAM_PROVIDER_MAX];
static int provider_count;

void stream_provider_register(stream_provider_t *provider) {
	if (!provider || !provider->id) {
		return;
	}
	for (int i = 0; i < provider_count; i++) {
		if (providers[i] == provider) {
			return; // init called twice: harmless
		}
	}
	if (provider_count >= STREAM_PROVIDER_MAX) {
		fprintf(stderr, "provider: no room for %s (raise STREAM_PROVIDER_MAX)\n", provider->id);
		return;
	}
	providers[provider_count++] = provider;
}

void stream_provider_attach_gui(const char *id, const stream_provider_gui_t *gui) {
	if (!id || !gui) {
		return;
	}
	for (int i = 0; i < provider_count; i++) {
		if (strcmp(providers[i]->id, id) == 0) {
			providers[i]->gui = *gui;
			return;
		}
	}
}

int stream_provider_count(void) { return provider_count; }

const stream_provider_t *stream_provider_at(int index) {
	return index >= 0 && index < provider_count ? providers[index] : NULL;
}

const stream_provider_t *stream_provider_for_path(const char *path) {
	if (!path || !path[0]) {
		return NULL;
	}
	for (int i = 0; i < provider_count; i++) {
		if (providers[i]->owns_path && providers[i]->owns_path(path)) {
			return providers[i];
		}
	}
	return NULL;
}

const stream_provider_t *stream_provider_for_track(const char *path) {
	const stream_provider_t *p = stream_provider_for_path(path);
	return p && p->is_track && p->is_track(path) ? p : NULL;
}

bool stream_provider_owns(const char *path) { return stream_provider_for_path(path) != NULL; }

bool stream_provider_any_downloading(void) {
	for (int i = 0; i < provider_count; i++) {
		if (providers[i]->downloading && providers[i]->downloading()) {
			return true;
		}
	}
	return false;
}

bool stream_provider_any_network_wanted(void) {
	for (int i = 0; i < provider_count; i++) {
		if ((providers[i]->downloading && providers[i]->downloading()) ||
			(providers[i]->network_wanted && providers[i]->network_wanted())) {
			return true;
		}
	}
	return false;
}

void stream_provider_set_root_all(const char *sd_root) {
	for (int i = 0; i < provider_count; i++) {
		if (providers[i]->set_root) {
			providers[i]->set_root(sd_root);
		}
	}
}

void stream_provider_clear_on_exit_all(void) {
	for (int i = 0; i < provider_count; i++) {
		if (providers[i]->clear_on_exit) {
			providers[i]->clear_on_exit();
		}
	}
}
