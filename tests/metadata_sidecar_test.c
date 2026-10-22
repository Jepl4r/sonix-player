#include "src/system/library/metadata.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_file(const char *path, const char *text) {
	FILE *f = fopen(path, "wb");
	assert(f);
	assert(fwrite(text, 1, strlen(text), f) == strlen(text));
	assert(fclose(f) == 0);
}
int main(int argc, char **argv) {
	assert(argc == 2);
	char audio[1024], tags[1100], description[1100];
	snprintf(audio, sizeof(audio), "%s/book.mp3", argv[1]);
	snprintf(tags, sizeof(tags), "%s.tags", audio);
	snprintf(description, sizeof(description), "%s.description", audio);
	write_file(audio, "");
	write_file(tags, "title=Sidecar title\nseries=The Cycle\nseries_part=2.5\n");
	write_file(description, "<p>Line one &amp; two.</p>\n<p>Line three.</p>");
	song_metadata_t m;
	char *summary = metadata_read_with_description(audio, &m);
	assert(strcmp(m.title, "Sidecar title") == 0);
	assert(strcmp(m.series, "The Cycle") == 0);
	assert(strcmp(m.series_part, "2.5") == 0);
	assert(summary && strstr(summary, "Line one & two.") && strstr(summary, "Line three."));
	assert(!strstr(summary, "<p>"));
	free(summary);
	metadata_read(audio, &m);
	assert(strcmp(m.series, "The Cycle") == 0);
	unlink(tags);
	unlink(description);
	summary = metadata_read_with_description(audio, &m);
	assert(!summary && !m.series[0] && !m.series_part[0]);
	write_file(description, "");
	summary = metadata_read_with_description(audio, &m);
	assert(!summary);
	FILE *large = fopen(description, "wb");
	assert(large);
	for (int i = 0; i < 16384; i++) {
		fputc('A', large);
	}
	fclose(large);
	summary = metadata_read_with_description(audio, &m);
	assert(summary && strlen(summary) == 8192);
	free(summary);
	puts("PASS metadata-sidecar-series-description-and-isolation");
	return 0;
}
