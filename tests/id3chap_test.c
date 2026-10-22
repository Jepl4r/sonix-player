#include "../sonix-player/src/system/library/id3chap.c"

#include <assert.h>

static uint8_t bytes[10 + 27 * (ID3CHAP_MAX_CHAPTERS + 1)];
static size_t used;
static int failures;

static void check(bool condition, int line, const char *expression) {
	if (!condition) {
		fprintf(stderr, "FAIL line %d: %s\n", line, expression);
		failures++;
	}
}

#define CHECK(condition) check((condition), __LINE__, #condition)

static void put32(size_t at, uint32_t value) {
	bytes[at] = (uint8_t)(value >> 24);
	bytes[at + 1] = (uint8_t)(value >> 16);
	bytes[at + 2] = (uint8_t)(value >> 8);
	bytes[at + 3] = (uint8_t)value;
}

static void begin_tag(void) {
	memset(bytes, 0, sizeof(bytes));
	memcpy(bytes, "ID3\x03", 4);
	used = 10;
}

static void add_chapter(uint32_t start, const uint8_t *title, size_t length) {
	assert(used + 38 + length <= sizeof(bytes));
	size_t frame = used;
	memcpy(bytes + frame, "CHAP", 4);
	used += 10;
	bytes[used++] = 'c';
	bytes[used++] = 0;
	put32(used, start);
	put32(used + 4, start + 1000);
	put32(used + 8, UINT32_MAX);
	put32(used + 12, UINT32_MAX);
	used += 16;
	memcpy(bytes + used, "TIT2", 4);
	put32(used + 4, (uint32_t)length);
	used += 10;
	memcpy(bytes + used, title, length);
	used += length;
	put32(frame + 4, (uint32_t)(used - frame - 10));
}

static void write_tag(const char *path) {
	uint32_t length = (uint32_t)(used - 10);
	for (int i = 9; i >= 6; i--) {
		bytes[i] = (uint8_t)(length & 0x7f);
		length >>= 7;
	}
	FILE *f = fopen(path, "wb");
	assert(f);
	assert(fwrite(bytes, 1, used, f) == used);
	assert(fclose(f) == 0);
}

static void count_contract(const char *path) {
	begin_tag();
	add_chapter(0, (const uint8_t *)"\0First", 6);
	add_chapter(1250, (const uint8_t *)"\0Second", 7);
	add_chapter(2500, (const uint8_t *)"\0Third", 6);
	write_tag(path);
	int count = id3chap_read(path, NULL, 0);
	fprintf(stderr, "count: expected 3, got %d\n", count);
	CHECK(count == 3);
	CHECK(id3chap_read(path, NULL, 2) == 3);
	CHECK(id3chap_present(path));
	id3chap_t *out = calloc(2, sizeof(*out));
	assert(out);
	count = id3chap_read(path, out, 2);
	fprintf(stderr, "max 2: expected 2, got %d\n", count);
	CHECK(count == 2);
	CHECK(out[0].start == 0 && strcmp(out[0].title, "First") == 0);
	CHECK(out[1].start == 1.25 && strcmp(out[1].title, "Second") == 0);
	free(out);
	id3chap_t all[8] = {0};
	CHECK(id3chap_read(path, all, 8) == 3);
	CHECK(all[2].start == 2.5 && strcmp(all[2].title, "Third") == 0);
	CHECK(id3chap_read(path, all, 0) == 0);
	CHECK(id3chap_read(path, all, -1) == 0);
	CHECK(all[0].start == 0 && strcmp(all[0].title, "First") == 0);
	// Exercise the allocation and copy pattern used by audiobook.c.
	count = id3chap_read(path, NULL, 0);
	out = calloc((size_t)count, sizeof(*out));
	assert(out);
	int filled = id3chap_read(path, out, count);
	CHECK(filled == count);
	if (filled <= count) {
		for (int i = 0; i < filled; i++) {
			CHECK(out[i].title[0] != '\0');
		}
	}
	free(out);
}

static void chapter_limit(const char *path) {
	// Reuse a minimal CHAP: empty element ID, four zero times/offsets, no title.
	static const uint8_t chapter[27] = {'C', 'H', 'A', 'P', 0, 0, 0, 17};
	begin_tag();
	for (int i = 0; i < ID3CHAP_MAX_CHAPTERS + 1; i++) {
		memcpy(bytes + used, chapter, sizeof(chapter));
		used += sizeof(chapter);
	}
	write_tag(path);
	CHECK(id3chap_read(path, NULL, 0) == ID3CHAP_MAX_CHAPTERS);
	id3chap_t *out = calloc(ID3CHAP_MAX_CHAPTERS + 1, sizeof(*out));
	assert(out);
	out[ID3CHAP_MAX_CHAPTERS].start = -1;
	CHECK(id3chap_read(path, out, ID3CHAP_MAX_CHAPTERS + 1) == ID3CHAP_MAX_CHAPTERS);
	CHECK(out[ID3CHAP_MAX_CHAPTERS - 1].start == 0);
	CHECK(out[ID3CHAP_MAX_CHAPTERS - 1].title[0] == '\0');
	CHECK(out[ID3CHAP_MAX_CHAPTERS].start == -1);
	free(out);
}

static void title_encodings(const char *path) {
	static const uint8_t le[] = {1, 0xff, 0xfe, 0x13, 4, 0x3b, 4, 0x30, 4, 0x32, 4, 0x30, 4, 0x20, 0, 0x31, 0, 0, 0};
	static const uint8_t be[] = {1, 0xfe, 0xff, 4, 0x13, 4, 0x3b, 4, 0x30, 4, 0x32, 4, 0x30, 0, 0x20, 0, 0x31, 0, 0};
	static const uint8_t be_no_bom[] = {2, 4, 0x13, 4, 0x3b, 4, 0x30, 4, 0x32, 4, 0x30, 0, 0x20, 0, 0x31};
	static const char cyrillic[] = "\xd0\x93\xd0\xbb\xd0\xb0\xd0\xb2\xd0\xb0 1";
	static const uint8_t utf8[] = "\x03\xd0\x93\xd0\xbb\xd0\xb0\xd0\xb2\xd0\xb0 1";
	static const uint8_t latin1[] = {0, 'C', 'a', 'f', 0xe9, ' ', '\r', '\n', 0};
	static const uint8_t bmp[] = {1, 0xfe, 0xff, 0x4e, 0x2d, 0, 0};
	static const uint8_t surrogate[] = {1, 0xff, 0xfe, 0, 0xd8, 0, 0};
	static const uint8_t odd_byte[] = {1, 0xff, 0xfe, 0x13, 4, 0x3b};
	static const uint8_t encoding1_no_bom[] = {1, 4, 0x13, 4, 0x3b};
	static const uint8_t after_nul[] = {1, 0xff, 0xfe, 0x13, 4, 0, 0, 0x3b, 4};
	const struct {
		const uint8_t *data;
		size_t length;
		const char *want;
	} cases[] = {
		{le, sizeof(le), cyrillic}, {be, sizeof(be), cyrillic}, {be_no_bom, sizeof(be_no_bom), cyrillic}, {utf8, sizeof(utf8), cyrillic}, {latin1, sizeof(latin1), "Caf\xc3\xa9"}, {bmp, sizeof(bmp), "\xe4\xb8\xad"}, {surrogate, sizeof(surrogate), "\xef\xbf\xbd"}, {odd_byte, sizeof(odd_byte), "\xd0\x93"}, {encoding1_no_bom, sizeof(encoding1_no_bom), "\xd0\x93\xd0\xbb"}, {after_nul, sizeof(after_nul), "\xd0\x93"},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		begin_tag();
		add_chapter(1000, cases[i].data, cases[i].length);
		write_tag(path);
		id3chap_t out;
		CHECK(id3chap_read(path, &out, 1) == 1);
		if (strcmp(out.title, cases[i].want) != 0) {
			fprintf(stderr, "title case %zu: expected '%s', got '%s'\n", i, cases[i].want, out.title);
		}
		CHECK(strcmp(out.title, cases[i].want) == 0);
	}
	// A multibyte character must fit with its terminator or stay out entirely.
	uint8_t long_title[260] = {1, 0xff, 0xfe};
	for (int i = 0; i < 128; i++) {
		long_title[3 + i * 2] = 0x13;
		long_title[4 + i * 2] = 4;
	}
	begin_tag();
	add_chapter(0, long_title, 259);
	write_tag(path);
	id3chap_t out;
	CHECK(id3chap_read(path, &out, 1) == 1);
	CHECK(strlen(out.title) == 126);
	for (int i = 0; i < 126; i += 2) {
		CHECK((uint8_t)out.title[i] == 0xd0 && (uint8_t)out.title[i + 1] == 0x93);
	}
	for (int encoding = 0; encoding <= 3; encoding += 3) {
		long_title[0] = (uint8_t)encoding;
		for (int i = 1; i < 257; i++) {
			long_title[i] = encoding == 0 ? 0xe9 : (i % 2 ? 0xc3 : 0xa9);
		}
		begin_tag();
		add_chapter(0, long_title, 257);
		write_tag(path);
		CHECK(id3chap_read(path, &out, 1) == 1);
		CHECK(strlen(out.title) == 126);
		for (int i = 0; i < 126; i += 2) {
			CHECK((uint8_t)out.title[i] == 0xc3 && (uint8_t)out.title[i + 1] == 0xa9);
		}
	}
}

int main(int argc, char **argv) {
	assert(argc == 2);
	count_contract(argv[1]);
	chapter_limit(argv[1]);
	title_encodings(argv[1]);
	begin_tag();
	bytes[used++] = 0;
	write_tag(argv[1]);
	id3chap_t out;
	CHECK(id3chap_read(argv[1], NULL, 0) == 0);
	CHECK(id3chap_read(argv[1], &out, 1) == 0);
	CHECK(!id3chap_present(argv[1]));
	return failures ? 1 : 0;
}
