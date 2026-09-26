#ifndef LOGGING_H
#define LOGGING_H

#include <stdbool.h>

// Where the player's output goes.
//
// The device has no console of its own: without ADB, everything printed is
// lost. So stdout and stderr are redirected to a file. That file starts on the
// tmpfs, because at the moment the player begins talking the card is not
// mounted yet, and moves onto the card once there is one -- next to the other
// things the player generates, under .local.
//
// Every line starts with the local time it was printed, to the millisecond,
// and a line with the date goes out first and whenever the day changes. Only
// what goes through stdio is stamped: the crash handler and child processes
// write to the descriptors directly and come out as they are.
//
// Writing to the card can be turned off (Settings > Developer options): it is
// a constant trickle of writes to the user's music card, worth having only
// while something is being debugged.

// Starts the stamping and redirects output to the tmpfs. Call first thing in
// main(), before anything can print: it replaces stdout and stderr.
void logging_init(void);

// Moves the log onto the card, if that is what the settings ask for. Call once
// the card is mounted and the config has been read.
void logging_attach_sd(const char *sd_root);

// Turns card logging on or off now, and remembers the choice.
void logging_set_to_sd(bool enabled);
bool logging_to_sd(void);

// The file being written, for the settings page to show.
const char *logging_path(void);

// The USB mass-storage export unmounts the card the log lives on. Suspend
// releases the file (output is held in memory meanwhile -- an open descriptor
// on the card would make the unmount fail); resume reopens the file and
// appends everything said in between. Both are safe to call when the log was
// never on the card.
void logging_suspend_for_usb(void);
void logging_resume_after_usb(void);

// Copies into the log whatever the kernel said about a process being killed.
//
// A process killed with SIGKILL -- which is what the out-of-memory killer
// sends -- runs no handler and leaves nothing behind: the log simply restarts
// mid-sentence. The kernel does say so, in its own ring buffer, and that buffer
// survives the process. Read
// at every start, it turns "it died and nobody knows why" into a line naming
// the killer and the victim.
//
// Says nothing when the buffer holds nothing of interest, which is the normal
// case. Call once, after the log is on the card.
void logging_report_previous_run(void);

#endif /* LOGGING_H */
