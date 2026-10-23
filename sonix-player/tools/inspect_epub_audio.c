// Read-only target probe: inspect a real EPUB without launching/replacing the UI.
#include "src/system/ebook/epub_internal.h"
#include "src/system/image/miniz/tinfl_impl.inc"
#include <stdio.h>
int main(int argc, char **argv) {
	if (argc != 2 && argc != 3)
		return 2;
	setbuf(stdout, NULL);
	ebook_t *book = ebook_open(argv[1]);
	if (!book) {
		puts("Cannot open EPUB");
		return 1;
	}
	unsigned chapters = 0, passages = 0, unavailable = 0, cues = 0, mapped = 0;
	size_t peak = 0;
	for (uint32_t i = 0; i < ebook_spine_count(book); i++) {
		if (!ebook_open_chapter(book, i)) {
			ebook_close(book);
			return 1;
		}
		uint32_t count = ebook_audio_count(book);
		if (argc == 3)
			printf("chapter %u: %s; passages=%u cues=%u\n", i, book->spine[i].path, count, ebook_audio_cue_count(book));
		if (count) {
			chapters++;
			passages += count;
			cues += ebook_audio_cue_count(book);
			for (uint32_t c = 0; c < ebook_audio_cue_count(book); c++) {
				const ebook_audio_cue_t *cue = ebook_audio_cue(book, c);
				if (cue->text_begin != UINT32_MAX && cue->text_end > cue->text_begin)
					mapped++;
			}
			if (chapters == 1) {
				const ebook_audio_clip_t *clip = ebook_audio_clip(book, 0);
				printf("first audio: %s (%.3f to %.3f s)\n", clip->path, clip->begin, clip->end);
			}
		}
		if (ebook_audio_unavailable(book))
			unavailable++;
		size_t bytes = ebook_book_bytes(book) + ebook_chapter_bytes(book);
		if (bytes > peak)
			peak = bytes;
	}
	printf("PASS: %u chapters, %u with audio, %u passages, %u unavailable; peak arenas %zu KiB\n", ebook_spine_count(book), chapters, passages, unavailable, peak / 1024);
	printf("Sentence timings: %u, mapped text ranges: %u\n", cues, mapped);
	ebook_close(book);
	return unavailable ? 1 : 0;
}
