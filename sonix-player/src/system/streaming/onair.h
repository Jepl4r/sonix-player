#ifndef ONAIR_H
#define ONAIR_H

#include <stddef.h>

// The on-air text of a radio stream (its ICY StreamTitle) reduced to what the
// player shows under the station name: "Artist - Title", a title alone, or the
// name of the programme -- one line, nothing else.
//
// What stations add around that, and what is taken off:
//
//   line breaks, tabs, control characters     one space
//   a JSON document instead of the song       its artist and title, or nothing
//   fields between tildes (Mediaset radios)   the first two made of words,
//   or between asterisks, two or more           every number, date and code
//                                             dropped
//   codes between " - " ("A1B2C3D4")          dropped
//   key="value" fields (iHeart, ad servers)   title=/text=/song= and artist=
//                                             kept, with the text before the
//                                             first field as the artist
//   web addresses                             dropped
//   HTML tags, &amp; &quot; &#39; ...         tags dropped, entities decoded
//   "-", "|" left dangling at either end      dropped
//   "Unknown", "Unknown - Unknown", "-"       nothing
//
// Works in place in a buffer of `size` bytes. The result can be a little
// longer than the input ("A~B" becomes "A - B") and is cut to fit.
void onair_clean(char *text, size_t size);

#endif /* ONAIR_H */
