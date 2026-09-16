#ifndef WEBCGI_H
#define WEBCGI_H

#include <stdbool.h>

// The player, wearing a second hat: a CGI program that lists a directory.
//
// Why this exists at all. The firmware's own list.cgi refuses to show anything
// whose name starts with a dot. It is not configurable and it is not a mistake
// on their side, it is simply what the code does -- from the MIPS disassembly
// of the readdir loop, at 0x400f34:
//
//     lw    v0,48(s8)      ; entry->d_name
//     lbu   v1,0(v0)       ; d_name[0]
//     lbu   v0,0(v0)       ; '.'  (the literal at 0x409738)
//     beq   v1,v0,401194   ; equal -> straight on to the next readdir
//
// So "show hidden files" cannot be done in the page: the entries never leave
// the device. The listing has to come from something else, and the only C
// program on the device this project controls is the player itself.
//
// So the player symlinks itself into thttpd's document root as "sonix-list" and
// answers that one URL, busybox-style: main() checks whether it was run as a
// CGI before it does anything else, and if it was, prints a listing and exits
// without ever opening a display.
//
//     GET sonix-list?path=<dir>[&hidden=1]
//         -> [{"path","name","ctime"[,"size"]}, ...]
//
// which is the shape the firmware's list.cgi returns, so the page can fall
// back to the stock one if this is somehow not there.

// True when this process was started by a web server rather than by the user.
// Call it first thing in main(); everything else in the player assumes it is
// the player.
bool webcgi_should_run(int argc, char **argv);

// Answers the request on stdout and returns the process exit code. Only
// meaningful when webcgi_should_run() said yes.
int webcgi_main(void);

#endif /* WEBCGI_H */
