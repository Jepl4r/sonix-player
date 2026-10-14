#ifndef ONSCREEN_H
#define ONSCREEN_H

#include "lvgl/lvgl.h"

// Runs an endless animation only while its screen is the one being shown.
//
// LVGL's animation timer ticks every 16 ms for as long as any animation is
// alive, whether or not what it moves can be seen, and an endless one never
// ends: a spinner left on a page that was navigated away from, or the caret of
// a search field on a screen built at start-up and not shown since, keeps the
// whole interface waking sixty times a second for nothing. Pausing an
// animation does not help -- the timer still runs for a paused one -- so the
// animation is deleted when its screen goes and made again when it comes back.
//
// `show` runs when the object's screen starts loading (so the animation is
// already moving as the page slides in) and at once if that screen is the
// active one, or a layer, which is always shown. `hide` runs once the screen
// has been unloaded, and at once when the object is bound on a screen that is
// not shown. Both get the object.
//
// Only screen changes are followed, not the object's own hidden flag: a
// spinner hidden on the page being looked at keeps turning, as before.
// The binding ends with the object; a screen deleted first takes it along.
// Binding an object a second time does nothing.
//
// Thread contract: UI thread only.
void onscreen_bind(lv_obj_t *obj, void (*show)(lv_obj_t *obj), void (*hide)(lv_obj_t *obj));

#endif /* ONSCREEN_H */
