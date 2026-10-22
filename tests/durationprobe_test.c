#define _GNU_SOURCE
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Instrument only the probe's stdio, leaving the real container/decoder opens
// alone.
static size_t probe_bytes;
static bool enforce_mp3_bounds;
static size_t counted_read(void *ptr, size_t size, size_t count, FILE *file) {
  long begin = ftell(file);
  if (enforce_mp3_bounds) {
    struct stat st;
    assert(fstat(fileno(file), &st) == 0);
    assert((begin >= 0 && begin + size * count <= 256 * 1024) ||
           (begin == st.st_size - 128 && size * count <= 128));
  }
  size_t n = fread(ptr, size, count, file);
  probe_bytes += n * size;
  return n;
}
#define fread counted_read
#include "../sonix-player/src/system/library/durationprobe.c"
#undef fread

void thread_be_background(const char *name) { (void)name; }

bool has_extension(const char *name, const char *ext) {
  size_t a = strlen(name), b = strlen(ext);
  return a >= b && !strcasecmp(name + a - b, ext);
}
static double milliseconds(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static void fixture(const char *dir, const char *name, const char *options) {
  char command[2048];
  snprintf(command, sizeof(command),
           "ffmpeg -nostdin -v error -y -f lavfi -i "
           "'sine=frequency=440:duration=10' %s '%s/%s'",
           options, dir, name);
  assert(system(command) == 0);
}
int main(int argc, char **argv) {
  if (system("command -v ffmpeg >/dev/null 2>&1") != 0) {
    puts("PASS durationprobe (skipped: no ffmpeg)");
    return 0;
  }
  assert(argc == 2);
  fixture(argv[1], "cbr.mp3", "-c:a libmp3lame -b:a 128k -write_xing 0");
  fixture(argv[1], "vbr.mp3", "-c:a libmp3lame -q:a 3");
  fixture(argv[1], "alac.m4b", "-c:a alac");
  fixture(argv[1], "audio.flac", "-c:a flac");
  fixture(argv[1], "audio.ogg", "-c:a libvorbis");
  fixture(argv[1], "audio.opus", "-c:a libopus");
  const char *names[] = {"cbr.mp3",    "vbr.mp3",   "alac.m4b",
                         "audio.flac", "audio.ogg", "audio.opus"};
  char path[1024];
  for (int i = 0; i < 6; i++) {
    snprintf(path, sizeof(path), "%s/%s", argv[1], names[i]);
    probe_bytes = 0;
    double start = milliseconds(), seconds = durationprobe_seconds(path),
           elapsed = milliseconds() - start;
    printf("probe %s: %.6f s, %.3f ms, %zu probe bytes\n", names[i], seconds,
           elapsed, probe_bytes);
    assert(fabs(seconds - 10) < 0.5);
  }
  // Remove the complete first MPEG frame (including Xing), then pad to 2 MiB.
  snprintf(path, sizeof(path), "%s/vbr.mp3", argv[1]);
  FILE *file = fopen(path, "r+b");
  assert(file);
  unsigned char prefix[4096];
  size_t n = fread(prefix, 1, sizeof(prefix), file);
  size_t xing = 0;
  for (size_t i = 0; i + 4 < n; i++)
    if (!memcmp(prefix + i, "Xing", 4)) {
      xing = i;
      break;
    }
  assert(xing > 0);
  size_t frame = xing;
  while (frame > 0 &&
         !(prefix[frame] == 0xff && (prefix[frame + 1] & 0xe0) == 0xe0))
    frame--;
  size_t next = xing + 4;
  while (next + 1 < n &&
         !(prefix[next] == 0xff && (prefix[next + 1] & 0xe0) == 0xe0))
    next++;
  assert(frame > 0 && next + 1 < n);
  memset(prefix + frame, 0, next - frame);
  rewind(file);
  assert(fwrite(prefix, 1, n, file) == n);
  assert(ftruncate(fileno(file), 2 * 1024 * 1024) == 0);
  fclose(file);
  probe_bytes = 0;
  enforce_mp3_bounds = true;
  double start = milliseconds(), seconds = durationprobe_seconds(path),
         elapsed = milliseconds() - start;
  printf("probe headerless-2MiB.mp3: %.6f s, %.3f ms, %zu probe bytes\n",
         seconds, elapsed, probe_bytes);
  assert(seconds == 0 ||
         seconds > 10); // file-size CBR estimate, not a whole-file frame count
  assert(probe_bytes <= 256 * 1024 + 128);
  assert(elapsed < 50);
  assert(durationprobe_seconds("/does/not/exist.mp3") == 0);
  puts("PASS durationprobe");
  return 0;
}
