#ifndef LANG_H
#define LANG_H

#include <stdbool.h>
#include <stddef.h>

// The interface in more than one language.
//
// The stock firmware keeps a folder per language under /usr/resource/str, each
// holding forty-odd .ini files that are really UTF-16 XML. That shape exists
// because its UI is built from resource ids; ours is built from C, so one flat
// file per language is both simpler to read and simpler to edit:
//
//     /usr/resource/sonix/language/Italiano.ini
//     /usr/resource/sonix/language/English.ini
//
// Next to the firmware's own fonts and strings, on the read-only rootfs, which
// is where they belong: they are part of the build, not part of what the user
// accumulates. Nothing here ever writes -- the language that was chosen is a
// line in device_config.ini on the writable partition -- so read-only costs
// nothing. Adding a fourth language later does mean remounting rw.
//
// The format is a plain UTF-8 INI, one entry per line, split at the FIRST '='
// so a translation may contain one:
//
//     [testi]
//     Alimentazione = Power
//     %d brani = %d tracks
//
// The key is the Italian text as it is written in the source. That is a
// deliberate choice over invented ids like `settings.power`:
//
//   * nothing can ever show a raw key to the user -- when a line is missing,
//     tr() hands back what it was given, which is correct Italian;
//   * Italiano.ini is generated from the source itself, so it cannot drift out
//     of step with the strings that actually exist;
//   * a translator reads the file and sees sentences, not identifiers.
//
// The cost is that two Italian words that happen to be spelled the same must
// take the same translation. Nothing in this interface needs otherwise.

#include "src/system/core/respath.h"

#define LANG_DIR SONIX_RESOURCE_DIR "/language"

// The language used on first boot, until someone chooses: the welcome screen,
// the language list and the date and time come out in it. The matching file
// must exist in the directory above; without it the interface stays on the
// source's Italian.
#define LANG_FIRST_BOOT "English"

// Reads the language named in the settings (or Italiano) out of `dir`. A
// missing directory or file is not an error: every tr() then returns its
// argument and the interface is in Italian, exactly as if this module did not
// exist.
void lang_init(const char *dir);

// The translation of `text`, or `text` itself when there is none. The returned
// pointer stays valid until the language changes.
const char *tr(const char *text);

// Which language is loaded ("Italiano", "English", ...).
const char *lang_current(void);

// The languages found in the directory, for the settings page. `names` is
// filled with pointers that stay valid; returns how many were written.
int lang_list(const char **names, int max);

// A language's name as it is spelled, rather than as its file is named.
//
// The .ini files may spell their names the Windows way -- `Fran#U00e7ais.ini`,
// `Espa#U00f1ol.ini` -- where `#Uxxxx` is a Unicode code point in hex, four hex
// digits, so the basic multilingual plane only. The filename stays as it is,
// since it is needed to open the file and to store the choice in config; this
// derives the display form, with the cedilla and the tilded n restored.
//
// `out` receives UTF-8. With nothing to decode it is an exact copy.
void lang_display_name(const char *name, char *out, size_t size);

// Loads another language, saves the choice, and re-labels what is already on
// screen -- every page is built once at startup, so without that a switch
// would only show through on pages built afterwards. Returns false when the
// file could not be read, in which case nothing changed.
bool lang_set(const char *name);

#endif /* LANG_H */
