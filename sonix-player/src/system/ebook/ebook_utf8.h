#ifndef EBOOK_UTF8_H
#define EBOOK_UTF8_H

#include <stdint.h>

// One UTF-8 character of `text` (`len` bytes) at `*at`, which is moved past
// it. A broken sequence reads as one byte, so the walk always moves forward.
//
// Shared by the vertical page layout and the one-word reader, which both walk
// a chapter's text a character at a time. No LVGL here, like the rest of the
// parser side.
uint32_t ebook_utf8_take(const char *text, uint32_t len, uint32_t *at);

#endif /* EBOOK_UTF8_H */
