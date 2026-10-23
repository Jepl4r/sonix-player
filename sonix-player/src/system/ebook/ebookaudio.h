#ifndef EBOOK_AUDIO_H
#define EBOOK_AUDIO_H

#include "src/system/ebook/ebook.h"

// GUI thread only, apart from the private extraction worker. Attach after a
// chapter opens; detach BEFORE its arena is closed. Opening never starts sound.
void ebookaudio_attach(ebook_t *book, const char *sd_root);
void ebookaudio_detach(void);
void ebookaudio_leave_chapter(void); // stop, retaining one disk resource for the next chapter
bool ebookaudio_take_chapter_completion(void);
void ebookaudio_tick(uint32_t now_ms);

bool ebookaudio_active(void);
bool ebookaudio_owns_path(const char *path);
bool ebookaudio_loading(void);
bool ebookaudio_playing(void);
const char *ebookaudio_error(void); // translation tag, or NULL
uint32_t ebookaudio_index(void);
void ebookaudio_progress(double *position, double *duration); // relative to this clip

void ebookaudio_toggle(void);
void ebookaudio_step(int direction);
void ebookaudio_seek(double seconds);		 // relative to this clip, clamped
void ebookaudio_seek_source(double seconds); // source time; thread-safe input request
void ebookaudio_stop(void);					 // relinquish playback; keep the book attached
uint32_t ebookaudio_cue_index(void);
bool ebookaudio_highlight(uint32_t *begin, uint32_t *end, uint32_t *reading);
bool ebookaudio_seek_text(uint32_t text_offset);
int ebookaudio_speed(void); // tenths: 5..30, default 10
void ebookaudio_set_speed(int tenths);

#endif
