// mtcircles -- circles that follow your fingers, to see whether multitouch is there.
//
// Why it exists
// -------------
// The R3 Pro II's panel is a Goodix GT967 and it reports five contacts
// (verified by reading 0x814E with the module unloaded: 0x81..0x85 with 1..5
// fingers). The stock driver lets exactly one out, and gtp_max_touch_number is
// not to blame: inside goodix_ts_work_func's loop over the contacts there is a
// jiffies comparison that skips the report of every contact after the first of
// the same frame -- and jiffies does not advance in the microseconds that loop
// takes. See tools/gt9xx_multitouch_patch.py.
//
// The R1 is a different panel with the same symptom: a Hynitron CST8xx
// (cst8xx_touch.ko, "hyn_ts"), where the cap is not a bug but a load-time
// parameter. cst_max_touch_number sizes the very I2C read the driver does per
// interrupt -- max_touch * 6 + 3 bytes -- so loaded with 1 the driver never
// even asks the controller for a second finger. Nothing to patch there: raise
// the number in the insmod line.
//
// This program is here to LOOK at the result instead of deducing it from logs:
// it reads /dev/input/event1 raw, draws a coloured circle per contact and makes
// them follow the fingers. With a capped driver you always see exactly one;
// with an uncapped one you see up to five.
//
// It does not use LVGL, it does not use the player, it installs nothing: it
// opens /dev/fb0 and /dev/input/eventN and that is all.
//
// Nothing in here is tied to one device. The panel size comes from
// FBIOGET_VSCREENINFO, protocol A or B is detected from EVIOCGBIT(EV_ABS), and
// the coordinate ranges from EVIOCGABS -- so a 480x800 panel works as well as a
// 480x720 one, with no edit. The one hard requirement is a 16-bit RGB565
// framebuffer.
//
// How to read it
// --------------
//   * one disc per contact, with its position in the packet written inside it;
//   * at the top, for every contact: tracking id, coordinates and width,
//     exactly as the kernel sends them;
//   * "MAX TOGETHER" is the highest number of contacts ever seen in a single
//     packet since the program started. That is THE line that answers the
//     question: 1 = the cap is still there, 2 or more = it is gone.
//   * "PACKETS" and "PER SEC" say whether the panel is reporting at its own
//     rate (~100 Hz) or whether something is throttling it.
//
// To quit: the red box with the X in the top right corner, or Ctrl-C from the
// adb shell.
//
// BEFORE running it, stop the player -- STOP it, do not kill it:
// /usr/bin/hiby_player.sh does `sleep 1; reboot` as soon as the player exits,
// so a kill reboots the device.
//
//   adb shell 'kill -STOP $(pidof sonix_player)'
//   adb shell /usr/data/mtcircles
//   adb shell 'kill -CONT $(pidof sonix_player)'
//
// The framebuffer's contents are saved at startup and put back on exit, so the
// stopped player finds the screen the way it left it. /dev/input events are
// duplicated to every reader, so reading them here takes nothing away from
// anybody else.
//
// Building, from the repo root:
//   rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc -O2 -std=gnu99 -o mtcircles tools/mtcircles.c
//   adb push mtcircles /usr/data/ && adb shell chmod +x /usr/data/mtcircles
//
// The -std=gnu99 is not a flourish: some builds of the rockbox toolchain ship a
// gcc that without it is still at C89, and there are declarations inside loops
// here. Nothing else is needed -- no -lm, no libraries.
//
// Options
// -------
//   -d <node>   the input device (default /dev/input/event1)
//   -f <node>   the framebuffer (default /dev/fb0)
//   -t          leave a trail: every contact seeds dots where it goes
//   -r          panel rotated 180 degrees, like the player's rotated mode
//               (flips the touch mapping; the text stays oriented with the
//               framebuffer)
//   -v          print every packet on stdout, to read it over adb
//   -s <sec>    quit on its own after that many seconds (0 = never, default)
//
// Returns 0 if it exited on request, 1 if it could not open something.
//
// Trying it without a device
// --------------------------
// If -f is not a framebuffer, the program falls back to test mode on a regular
// file: two 480x720 RGB565 pages inside that file, like a real panel. And -d
// can be a fifo (or a file) of raw struct input_event. That is how the drawing
// and the parser were verified on a PC before handing this over:
//
//   mkfifo /tmp/ev
//   ./mtcircles -f /tmp/screen.bin -d /tmp/ev
//   # ...and somebody on the other end writing events into the fifo

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

// Old kernels do not define everything that is needed here.
#ifndef ABS_MT_SLOT
#define ABS_MT_SLOT 0x2f
#endif
#ifndef SYN_MT_REPORT
#define SYN_MT_REPORT 2
#endif
#ifndef EVIOCGBIT
#define EVIOCGBIT(ev, len) _IOC(_IOC_READ, 'E', 0x20 + (ev), len)
#endif

#define MAX_CONTACTS 10

// 7x10, ASCII 0x20..0x7E. One unsigned short per column, bit 0 = top row.
// Traced from DejaVuSansMono at 11 px with a threshold: it is a diagnostic
// font, and the size was chosen because at 6x8 the O, the 0, the D and the 8
// all became the same drawing -- which on a line of coordinates is not a
// detail.
#define GLYPH_W 7
#define GLYPH_H 10
static const unsigned short FONT7X10[95][GLYPH_W] = {
	{0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000}, // 0x20 space
	{0x000, 0x000, 0x000, 0x17e, 0x000, 0x000, 0x000}, // 0x21 !
	{0x000, 0x000, 0x00e, 0x000, 0x00e, 0x000, 0x000}, // 0x22 "
	{0x040, 0x1c8, 0x07c, 0x1ca, 0x078, 0x04e, 0x008}, // 0x23 #
	{0x000, 0x118, 0x124, 0x3fe, 0x124, 0x0c4, 0x000}, // 0x24 $
	{0x004, 0x04a, 0x02a, 0x1c4, 0x150, 0x1c8, 0x000}, // 0x25 %
	{0x0c0, 0x1bc, 0x11a, 0x122, 0x1c2, 0x1c0, 0x000}, // 0x26 &
	{0x000, 0x000, 0x000, 0x00e, 0x000, 0x000, 0x000}, // 0x27 apostrophe
	{0x000, 0x000, 0x078, 0x1ce, 0x201, 0x000, 0x000}, // 0x28 (
	{0x000, 0x000, 0x301, 0x1fe, 0x000, 0x000, 0x000}, // 0x29 )
	{0x000, 0x024, 0x018, 0x07e, 0x018, 0x024, 0x000}, // 0x2a *
	{0x020, 0x020, 0x020, 0x0f8, 0x020, 0x020, 0x000}, // 0x2b +
	{0x000, 0x000, 0x200, 0x180, 0x000, 0x000, 0x000}, // 0x2c ,
	{0x000, 0x000, 0x020, 0x020, 0x020, 0x000, 0x000}, // 0x2d -
	{0x000, 0x000, 0x000, 0x180, 0x000, 0x000, 0x000}, // 0x2e .
	{0x000, 0x300, 0x0c0, 0x030, 0x00e, 0x002, 0x000}, // 0x2f /
	{0x000, 0x0fc, 0x186, 0x122, 0x186, 0x0fc, 0x000}, // 0x30 0
	{0x000, 0x102, 0x102, 0x1fe, 0x100, 0x100, 0x000}, // 0x31 1
	{0x000, 0x186, 0x1c2, 0x162, 0x11e, 0x10c, 0x000}, // 0x32 2
	{0x000, 0x184, 0x112, 0x112, 0x1be, 0x0ec, 0x000}, // 0x33 3
	{0x040, 0x070, 0x048, 0x046, 0x1fe, 0x040, 0x000}, // 0x34 4
	{0x000, 0x11e, 0x112, 0x112, 0x1b2, 0x0e0, 0x000}, // 0x35 5
	{0x000, 0x0fc, 0x116, 0x112, 0x1b2, 0x0e0, 0x000}, // 0x36 6
	{0x000, 0x002, 0x182, 0x0e2, 0x01e, 0x002, 0x000}, // 0x37 7
	{0x000, 0x0ec, 0x112, 0x112, 0x1be, 0x0ec, 0x000}, // 0x38 8
	{0x000, 0x11c, 0x122, 0x122, 0x1b6, 0x07c, 0x000}, // 0x39 9
	{0x000, 0x000, 0x000, 0x198, 0x000, 0x000, 0x000}, // 0x3a :
	{0x000, 0x000, 0x200, 0x198, 0x000, 0x000, 0x000}, // 0x3b ;
	{0x020, 0x020, 0x050, 0x050, 0x050, 0x088, 0x000}, // 0x3c <
	{0x050, 0x050, 0x050, 0x050, 0x050, 0x050, 0x000}, // 0x3d =
	{0x088, 0x088, 0x050, 0x050, 0x070, 0x020, 0x000}, // 0x3e >
	{0x000, 0x002, 0x002, 0x172, 0x00e, 0x004, 0x000}, // 0x3f ?
	{0x0f0, 0x30c, 0x262, 0x092, 0x092, 0x0fc, 0x000}, // 0x40 @
	{0x100, 0x1e0, 0x05e, 0x046, 0x078, 0x1c0, 0x000}, // 0x41 A
	{0x000, 0x1fe, 0x112, 0x112, 0x11e, 0x0ec, 0x000}, // 0x42 B
	{0x000, 0x0fc, 0x186, 0x102, 0x102, 0x084, 0x000}, // 0x43 C
	{0x000, 0x1fe, 0x102, 0x102, 0x084, 0x078, 0x000}, // 0x44 D
	{0x000, 0x1fe, 0x112, 0x112, 0x112, 0x112, 0x000}, // 0x45 E
	{0x000, 0x1fe, 0x012, 0x012, 0x012, 0x012, 0x000}, // 0x46 F
	{0x000, 0x0fc, 0x186, 0x102, 0x122, 0x0e4, 0x000}, // 0x47 G
	{0x000, 0x1fe, 0x010, 0x010, 0x010, 0x1fe, 0x000}, // 0x48 H
	{0x000, 0x102, 0x102, 0x1fe, 0x102, 0x102, 0x000}, // 0x49 I
	{0x000, 0x100, 0x102, 0x102, 0x0fe, 0x000, 0x000}, // 0x4a J
	{0x000, 0x1fe, 0x010, 0x038, 0x0c4, 0x182, 0x000}, // 0x4b K
	{0x000, 0x1fe, 0x100, 0x100, 0x100, 0x100, 0x000}, // 0x4c L
	{0x1fe, 0x1fe, 0x01c, 0x030, 0x00e, 0x1fe, 0x000}, // 0x4d M
	{0x000, 0x1fe, 0x00c, 0x070, 0x1c0, 0x1fe, 0x000}, // 0x4e N
	{0x000, 0x0fc, 0x102, 0x102, 0x186, 0x0fc, 0x000}, // 0x4f O
	{0x000, 0x1fe, 0x022, 0x022, 0x032, 0x01c, 0x000}, // 0x50 P
	{0x000, 0x0fc, 0x102, 0x102, 0x386, 0x0fc, 0x000}, // 0x51 Q
	{0x000, 0x1fe, 0x022, 0x022, 0x07e, 0x19c, 0x000}, // 0x52 R
	{0x000, 0x09c, 0x112, 0x122, 0x1a2, 0x0c0, 0x000}, // 0x53 S
	{0x002, 0x002, 0x002, 0x1fe, 0x002, 0x002, 0x000}, // 0x54 T
	{0x000, 0x0fe, 0x100, 0x100, 0x180, 0x0fe, 0x000}, // 0x55 U
	{0x002, 0x01e, 0x1e0, 0x180, 0x078, 0x00e, 0x000}, // 0x56 V
	{0x01e, 0x1f0, 0x0f0, 0x018, 0x1c0, 0x1fe, 0x002}, // 0x57 W
	{0x100, 0x186, 0x06c, 0x030, 0x0cc, 0x182, 0x000}, // 0x58 X
	{0x002, 0x006, 0x018, 0x1f0, 0x00c, 0x006, 0x000}, // 0x59 Y
	{0x000, 0x182, 0x142, 0x132, 0x10a, 0x106, 0x000}, // 0x5a Z
	{0x000, 0x000, 0x3ff, 0x3ff, 0x201, 0x000, 0x000}, // 0x5b [
	{0x000, 0x006, 0x018, 0x060, 0x380, 0x200, 0x000}, // 0x5c backslash
	{0x000, 0x000, 0x201, 0x3ff, 0x000, 0x000, 0x000}, // 0x5d ]
	{0x000, 0x008, 0x006, 0x002, 0x004, 0x008, 0x000}, // 0x5e ^
	{0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000}, // 0x5f _
	{0x000, 0x000, 0x001, 0x002, 0x000, 0x000, 0x000}, // 0x60 `
	{0x000, 0x1e8, 0x128, 0x128, 0x1b8, 0x1f0, 0x000}, // 0x61 a
	{0x000, 0x1ff, 0x198, 0x108, 0x198, 0x0f0, 0x000}, // 0x62 b
	{0x000, 0x0f0, 0x198, 0x108, 0x108, 0x108, 0x000}, // 0x63 c
	{0x000, 0x0f0, 0x108, 0x108, 0x1f8, 0x1ff, 0x000}, // 0x64 d
	{0x000, 0x0f0, 0x128, 0x128, 0x128, 0x130, 0x000}, // 0x65 e
	{0x000, 0x008, 0x008, 0x1ff, 0x009, 0x009, 0x000}, // 0x66 f
	{0x000, 0x0f0, 0x108, 0x108, 0x3f8, 0x1f8, 0x000}, // 0x67 g
	{0x000, 0x1ff, 0x018, 0x008, 0x018, 0x1f0, 0x000}, // 0x68 h
	{0x000, 0x108, 0x108, 0x1f9, 0x100, 0x100, 0x000}, // 0x69 i
	{0x000, 0x008, 0x008, 0x3f9, 0x000, 0x000, 0x000}, // 0x6a j
	{0x000, 0x1ff, 0x060, 0x070, 0x0d8, 0x108, 0x000}, // 0x6b k
	{0x000, 0x001, 0x0ff, 0x1ff, 0x100, 0x100, 0x000}, // 0x6c l
	{0x000, 0x1f8, 0x008, 0x1f8, 0x008, 0x1f8, 0x000}, // 0x6d m
	{0x000, 0x1f8, 0x018, 0x008, 0x018, 0x1f0, 0x000}, // 0x6e n
	{0x000, 0x0f0, 0x108, 0x108, 0x198, 0x0f0, 0x000}, // 0x6f o
	{0x000, 0x3f8, 0x198, 0x108, 0x198, 0x0f0, 0x000}, // 0x70 p
	{0x000, 0x0f0, 0x108, 0x108, 0x198, 0x3f8, 0x000}, // 0x71 q
	{0x000, 0x000, 0x1f8, 0x018, 0x008, 0x008, 0x000}, // 0x72 r
	{0x000, 0x130, 0x128, 0x168, 0x1c8, 0x080, 0x000}, // 0x73 s
	{0x000, 0x008, 0x0fe, 0x188, 0x108, 0x108, 0x000}, // 0x74 t
	{0x000, 0x0f8, 0x100, 0x100, 0x180, 0x1f8, 0x000}, // 0x75 u
	{0x000, 0x038, 0x1c0, 0x180, 0x0f0, 0x018, 0x000}, // 0x76 v
	{0x018, 0x1e0, 0x1c0, 0x060, 0x180, 0x0f8, 0x008}, // 0x77 w
	{0x000, 0x188, 0x0f0, 0x060, 0x098, 0x108, 0x000}, // 0x78 x
	{0x000, 0x018, 0x2e0, 0x380, 0x060, 0x018, 0x000}, // 0x79 y
	{0x000, 0x108, 0x1c8, 0x128, 0x118, 0x108, 0x000}, // 0x7a z
	{0x000, 0x010, 0x030, 0x3ef, 0x201, 0x201, 0x000}, // 0x7b {
	{0x000, 0x000, 0x000, 0x3ff, 0x000, 0x000, 0x000}, // 0x7c |
	{0x000, 0x201, 0x201, 0x1fe, 0x010, 0x010, 0x000}, // 0x7d }
	{0x000, 0x020, 0x020, 0x060, 0x040, 0x040, 0x000}, // 0x7e ~
};

// ---------------------------------------------------------------------------
// the framebuffer
// ---------------------------------------------------------------------------

static int fb_fd = -1;
static uint8_t *fb_mem;			  // the mapped pages
static size_t fb_page_bytes;	  // how much ONE page takes
static int fb_pages = 1;		  // 1 or 2
static int fb_w, fb_h, fb_stride; // pixels, pixels, bytes per row
static struct fb_var_screeninfo fb_var, fb_var_saved;
static uint8_t *fb_backup;		  // how it looked before we turned up
static int fb_draw_page;		  // the page being drawn into
static int fb_visible_page;		  // the page the panel is actually showing
static bool fb_fake;			  // a regular file standing in for the framebuffer

// Defined down in the drawing section; fb_show() needs it when it has to fall
// back to a single page halfway through.
static void dirty_reset(void);

static uint16_t rgb565(int r, int g, int b) {
	return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static uint8_t *page_base(int page) { return fb_mem + (size_t)page * fb_page_bytes; }

static bool fb_open(const char *path) {
	fb_fd = open(path, O_RDWR);
	if (fb_fd < 0 && errno == ENOENT) {
		fb_fd = open(path, O_RDWR | O_CREAT, 0644); // test mode
	}
	if (fb_fd < 0) {
		fprintf(stderr, "mtcircles: %s: %s\n", path, strerror(errno));
		return false;
	}

	struct fb_fix_screeninfo fix;
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_var) < 0 || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		// Not a framebuffer: test mode on a regular file, to verify the
		// program on a PC. Two 480x720 RGB565 pages, like a real panel;
		// afterwards the file can be looked at with whatever you like. The
		// same thing fbbench does.
		fb_fake = true;
		memset(&fb_var, 0, sizeof(fb_var));
		memset(&fix, 0, sizeof(fix));
		fb_var.xres = fb_var.xres_virtual = 480;
		fb_var.yres = 720;
		fb_var.yres_virtual = 1440;
		fb_var.bits_per_pixel = 16;
		fix.line_length = 480 * 2;
		fix.smem_len = 480 * 2 * 1440;
		if (ftruncate(fb_fd, (off_t)fix.smem_len) < 0) {
			perror("mtcircles: ftruncate");
			return false;
		}
		printf("mtcircles: %s is not a framebuffer, falling back to file test mode\n", path);
	}
	fb_var_saved = fb_var;

	if (fb_var.bits_per_pixel != 16) {
		fprintf(stderr, "mtcircles: the framebuffer is %u bpp, this program only knows RGB565\n",
				fb_var.bits_per_pixel);
		return false;
	}

	// Two pages if there are two: draw into the hidden one and flip with
	// FBIOPAN_DISPLAY, the way the player does. If the panel only has one,
	// draw into the visible one and watch the picture form -- it works just
	// the same, it is only less tidy.
	if (!fb_fake) {
		struct fb_var_screeninfo want = fb_var;
		want.yres_virtual = fb_var.yres * 2;
		want.xres_virtual = fb_var.xres;
		want.yoffset = 0;
		if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &want) == 0) {
			fb_var = want;
			ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix);
		}
	}

	fb_w = (int)fb_var.xres;
	fb_h = (int)fb_var.yres;
	fb_stride = (int)fix.line_length;
	fb_page_bytes = (size_t)fb_stride * (size_t)fb_h;
	fb_pages = ((size_t)fix.smem_len >= fb_page_bytes * 2 && fb_var.yres_virtual >= fb_var.yres * 2) ? 2 : 1;

	fb_mem = mmap(NULL, fb_page_bytes * (size_t)fb_pages, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb_mem == MAP_FAILED) {
		perror("mtcircles: mmap");
		fb_mem = NULL;
		return false;
	}

	// What was there before. If there is no memory we carry on anyway: it is
	// a courtesy to the stopped player, not a requirement.
	fb_backup = fb_fake ? NULL : malloc(fb_page_bytes * (size_t)fb_pages);
	if (fb_backup) {
		memcpy(fb_backup, fb_mem, fb_page_bytes * (size_t)fb_pages);
	}

	// WHICH page the panel is showing right now, asked rather than assumed.
	//
	// It matters when the flip turns out to be refused (see fb_show): the
	// fallback is to draw straight into the visible page, and "the visible
	// page" is not always page zero. A player that was stopped mid-frame can
	// have left either one on screen, and drawing into the other would put the
	// picture in the half nobody is looking at -- which looks exactly like the
	// program not working at all.
	if (!fb_fake) {
		struct fb_var_screeninfo now;
		if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &now) == 0 && now.yres > 0) {
			fb_visible_page = (int)(now.yoffset / now.yres);
		}
	}
	if (fb_visible_page < 0 || fb_visible_page >= fb_pages) {
		fb_visible_page = 0;
	}
	fb_draw_page = (fb_pages == 2) ? (fb_visible_page ^ 1) : fb_visible_page;

	printf("mtcircles: %s %dx%d, %d bytes per row, %d page%s, showing page %d\n", path, fb_w, fb_h, fb_stride,
		   fb_pages, fb_pages == 1 ? "" : "s", fb_visible_page);
	return true;
}

// Puts a page on screen. If the panel refuses to flip, this is where the
// program stops trying and settles for one page.
//
// FBIOPAN_DISPLAY coming back EBUSY -- "Device or resource busy" -- means
// somebody else owns the panning: on these players that is the player itself,
// still running, or stopped with a flip of its own still outstanding. Some
// Ingenic panels also refuse it outright when they were set up single-buffered,
// however much video memory /dev/fb0 reports.
//
// Complaining once and carrying on, which is what this used to do, was the
// worst of the three possible behaviours: every drawing went into the page
// nobody was looking at, so the screen sat there unchanged and the program
// looked broken rather than limited. Falling back is honest and it works: from
// here on we draw straight into the page that IS on screen. The picture forms
// under your eyes instead of appearing whole, and that is all that is lost.
static void fb_show(int page) {
	if (fb_pages < 2 || fb_fake) {
		return;
	}

	fb_var.yoffset = (uint32_t)page * fb_var.yres;
	if (ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var) == 0) {
		fb_visible_page = page;
		return;
	}

	perror("mtcircles: FBIOPAN_DISPLAY");
	fprintf(stderr, "mtcircles: page flipping refused, drawing straight into the visible page\n"
					"           (is the player still running? it has to be STOPped, see the header)\n");

	fb_pages = 1;
	fb_draw_page = fb_visible_page;

	// Everything the two pages had dirty is void: from now on there is one
	// page and it starts clean, so no stale rectangle survives the switch.
	dirty_reset();
	memset(page_base(fb_draw_page), 0, fb_page_bytes);
}

static void fb_close(void) {
	if (fb_mem) {
		if (fb_backup) {
			memcpy(fb_mem, fb_backup, fb_page_bytes * (size_t)fb_pages);
			free(fb_backup);
			fb_backup = NULL;
		}
		munmap(fb_mem, fb_page_bytes * (size_t)fb_pages);
		fb_mem = NULL;
	}
	if (fb_fd >= 0) {
		if (!fb_fake) {
			ioctl(fb_fd, FBIOPUT_VSCREENINFO, &fb_var_saved);
		}
		close(fb_fd);
		fb_fd = -1;
	}
}

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------

static uint8_t *trail; // the trail, when it is on: one page of RAM

typedef struct {
	int x1, y1, x2, y2;
} rect_t;

// What each page has dirty from the previous round. More than the maximum
// number of contacts, because the status panel and the quit box go in too.
#define MAX_DIRTY (MAX_CONTACTS + 4)
static rect_t dirty[2][MAX_DIRTY];
static int dirty_count[2];

static void dirty_reset(void) {
	dirty_count[0] = 0;
	dirty_count[1] = 0;
}

static void dirty_add(int page, int x1, int y1, int x2, int y2) {
	if (x1 < 0) x1 = 0;
	if (y1 < 0) y1 = 0;
	if (x2 >= fb_w) x2 = fb_w - 1;
	if (y2 >= fb_h) y2 = fb_h - 1;
	if (x1 > x2 || y1 > y2) {
		return;
	}
	if (dirty_count[page] >= MAX_DIRTY) {
		// Past the ceiling the last one is widened instead of losing one:
		// better to clean too much than leave a stale circle on the screen.
		rect_t *r = &dirty[page][MAX_DIRTY - 1];
		if (x1 < r->x1) r->x1 = x1;
		if (y1 < r->y1) r->y1 = y1;
		if (x2 > r->x2) r->x2 = x2;
		if (y2 > r->y2) r->y2 = y2;
		return;
	}
	dirty[page][dirty_count[page]++] = (rect_t){x1, y1, x2, y2};
}

// Puts back the part of the page the previous round dirtied: black, or the
// trail when it is on.
static void restore_dirty(int page) {
	uint8_t *base = page_base(page);
	for (int i = 0; i < dirty_count[page]; i++) {
		rect_t r = dirty[page][i];
		int bytes = (r.x2 - r.x1 + 1) * 2;
		for (int y = r.y1; y <= r.y2; y++) {
			uint8_t *dst = base + (size_t)y * fb_stride + (size_t)r.x1 * 2;
			if (trail) {
				memcpy(dst, trail + (size_t)y * fb_stride + (size_t)r.x1 * 2, (size_t)bytes);
			} else {
				memset(dst, 0, (size_t)bytes);
			}
		}
	}
	dirty_count[page] = 0;
}

static void fill_rect(uint8_t *base, int x, int y, int w, int h, uint16_t c) {
	for (int j = 0; j < h; j++) {
		int yy = y + j;
		if (yy < 0 || yy >= fb_h) {
			continue;
		}
		uint16_t *row = (uint16_t *)(base + (size_t)yy * fb_stride);
		for (int i = 0; i < w; i++) {
			int xx = x + i;
			if (xx >= 0 && xx < fb_w) {
				row[xx] = c;
			}
		}
	}
}

// Filled disc. The comparison is between squares, so no square root needed.
static void fill_disc(uint8_t *base, int cx, int cy, int r, uint16_t c) {
	int rr = r * r;
	for (int dy = -r; dy <= r; dy++) {
		int y = cy + dy;
		if (y < 0 || y >= fb_h) {
			continue;
		}
		int half = 0;
		while ((half + 1) * (half + 1) + dy * dy <= rr) {
			half++;
		}
		uint16_t *row = (uint16_t *)(base + (size_t)y * fb_stride);
		int x1 = cx - half, x2 = cx + half;
		if (x1 < 0) x1 = 0;
		if (x2 >= fb_w) x2 = fb_w - 1;
		for (int x = x1; x <= x2; x++) {
			row[x] = c;
		}
	}
}

// Ring: the outer disc minus the inner one, row by row, without redrawing
// the middle.
static void draw_ring(uint8_t *base, int cx, int cy, int r_out, int r_in, uint16_t c) {
	int ro = r_out * r_out, ri = r_in * r_in;
	for (int dy = -r_out; dy <= r_out; dy++) {
		int y = cy + dy;
		if (y < 0 || y >= fb_h) {
			continue;
		}
		int outer = 0;
		while ((outer + 1) * (outer + 1) + dy * dy <= ro) {
			outer++;
		}
		uint16_t *row = (uint16_t *)(base + (size_t)y * fb_stride);
		for (int x = cx - outer; x <= cx + outer; x++) {
			if (x < 0 || x >= fb_w) {
				continue;
			}
			int dx = x - cx;
			if (dx * dx + dy * dy > ri) {
				row[x] = c;
			}
		}
	}
}

static void draw_char(uint8_t *base, int x, int y, char ch, int scale, uint16_t c) {
	if (ch < 0x20 || ch > 0x7e) {
		ch = '?';
	}
	const unsigned short *g = FONT7X10[(int)ch - 0x20];
	for (int col = 0; col < GLYPH_W; col++) {
		for (int row = 0; row < GLYPH_H; row++) {
			if (g[col] & (1u << row)) {
				fill_rect(base, x + col * scale, y + row * scale, scale, scale, c);
			}
		}
	}
}

static void draw_text(uint8_t *base, int x, int y, const char *s, int scale, uint16_t c) {
	for (; *s; s++, x += GLYPH_W * scale) {
		draw_char(base, x, y, *s, scale, c);
	}
}

// How tall a line of text is, so we know how far to step down.
static int text_h(int scale) { return GLYPH_H * scale; }

// ---------------------------------------------------------------------------
// the panel
// ---------------------------------------------------------------------------

typedef struct {
	int id;	 // ABS_MT_TRACKING_ID, exactly as the kernel sends it
	int x, y;	 // already converted to screen coordinates
	int raw_x, raw_y;
	int major, width;
} contact_t;

static int ev_fd = -1;
static bool proto_b;			  // the driver uses slots instead of SYN_MT_REPORT
static int ax_min, ax_max;		  // the range of ABS_MT_POSITION_X
static int ay_min, ay_max;
static bool rotate180;

static bool axis_range(int code, int *lo, int *hi, int fallback_hi) {
	struct input_absinfo abs;
	if (ioctl(ev_fd, EVIOCGABS(code), &abs) == 0 && abs.maximum > abs.minimum) {
		*lo = abs.minimum;
		*hi = abs.maximum;
		return true;
	}
	*lo = 0;
	*hi = fallback_hi;
	return false;
}

static bool ev_open(const char *path) {
	ev_fd = open(path, O_RDONLY);
	if (ev_fd < 0) {
		fprintf(stderr, "mtcircles: %s: %s\n", path, strerror(errno));
		return false;
	}

	char name[128] = "?";
	ioctl(ev_fd, EVIOCGNAME(sizeof(name)), name);

	unsigned long bits[(ABS_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];
	memset(bits, 0, sizeof(bits));
	if (ioctl(ev_fd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits) >= 0) {
		proto_b = (bits[ABS_MT_SLOT / (8 * sizeof(long))] >> (ABS_MT_SLOT % (8 * sizeof(long)))) & 1;
	}

	bool got_x = axis_range(ABS_MT_POSITION_X, &ax_min, &ax_max, fb_w - 1);
	bool got_y = axis_range(ABS_MT_POSITION_Y, &ay_min, &ay_max, fb_h - 1);

	printf("mtcircles: %s -> \"%s\", protocol %s\n", path, name, proto_b ? "B (slots)" : "A (SYN_MT_REPORT)");
	printf("mtcircles: X %d..%d%s, Y %d..%d%s\n", ax_min, ax_max, got_x ? "" : " (guessed)", ay_min, ay_max,
		   got_y ? "" : " (guessed)");
	return true;
}

// From the panel's range to the screen's. With -r everything turns 180
// degrees, which is what the player does by inverting the evdev calibration.
static void map_point(int rx, int ry, int *sx, int *sy) {
	int dx = ax_max - ax_min, dy = ay_max - ay_min;
	int x = dx > 0 ? (rx - ax_min) * (fb_w - 1) / dx : rx;
	int y = dy > 0 ? (ry - ay_min) * (fb_h - 1) / dy : ry;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x >= fb_w) x = fb_w - 1;
	if (y >= fb_h) y = fb_h - 1;
	if (rotate180) {
		x = fb_w - 1 - x;
		y = fb_h - 1 - y;
	}
	*sx = x;
	*sy = y;
}

// ---------------------------------------------------------------------------
// the program
// ---------------------------------------------------------------------------

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) {
	(void)sig;
	running = 0;
}

static int64_t now_ms(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// One colour per contact, by position in the packet. Five is enough: neither
// of the two panels reports more than that.
static uint16_t contact_color(int i) {
	static const uint8_t RGB[5][3] = {
		{255, 59, 48},	 // red
		{52, 199, 89},	 // green
		{10, 132, 255},	 // blue
		{255, 214, 10},	 // yellow
		{191, 90, 242},	 // purple
	};
	const uint8_t *c = RGB[i % 5];
	return rgb565(c[0], c[1], c[2]);
}

// The quit box, top right. Big enough to hit with a finger and far from where
// multitouch is being tried.
#define QUIT_SIZE 64
#define QUIT_MARGIN 6

static bool in_quit_box(int x, int y) {
	return x >= fb_w - QUIT_SIZE - QUIT_MARGIN && y <= QUIT_SIZE + QUIT_MARGIN;
}

static void draw_quit_box(uint8_t *base) {
	int x = fb_w - QUIT_SIZE - QUIT_MARGIN, y = QUIT_MARGIN;
	uint16_t red = rgb565(200, 40, 40), white = rgb565(255, 255, 255);
	fill_rect(base, x, y, QUIT_SIZE, QUIT_SIZE, red);
	for (int i = 12; i < QUIT_SIZE - 12; i++) {
		fill_rect(base, x + i, y + i, 4, 4, white);
		fill_rect(base, x + QUIT_SIZE - 4 - i, y + i, 4, 4, white);
	}
	dirty_add(fb_draw_page, x, y, x + QUIT_SIZE - 1, y + QUIT_SIZE - 1);
}

int main(int argc, char **argv) {
	const char *ev_path = "/dev/input/event1";
	const char *fb_path = "/dev/fb0";
	bool want_trail = false, verbose = false;
	int stop_after = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") && i + 1 < argc) {
			ev_path = argv[++i];
		} else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
			fb_path = argv[++i];
		} else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
			stop_after = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-t")) {
			want_trail = true;
		} else if (!strcmp(argv[i], "-r")) {
			rotate180 = true;
		} else if (!strcmp(argv[i], "-v")) {
			verbose = true;
		} else {
			fprintf(stderr, "usage: %s [-d /dev/input/eventN] [-f /dev/fb0] [-t] [-r] [-v] [-s seconds]\n", argv[0]);
			return 1;
		}
	}

	// sigaction and not signal(): glibc puts SA_RESTART under signal(), and
	// with that the read() on the panel restarts by itself after the signal --
	// which means Ctrl-C would not be felt until a touch arrived.
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	if (!fb_open(fb_path)) {
		fb_close();
		return 1;
	}
	if (!ev_open(ev_path)) {
		fb_close();
		return 1;
	}

	if (want_trail) {
		trail = calloc(1, fb_page_bytes);
		if (!trail) {
			fprintf(stderr, "mtcircles: no memory for the trail, going without\n");
		}
	}

	// Both pages start black, so the first flip does not show whatever was
	// there before.
	for (int p = 0; p < fb_pages; p++) {
		memset(page_base(p), 0, fb_page_bytes);
		dirty_count[p] = 0;
	}

	contact_t live[MAX_CONTACTS];  // what is being shown
	contact_t build[MAX_CONTACTS]; // the packet being built
	int live_n = 0, build_n = 0;
	contact_t slot[MAX_CONTACTS];  // protocol B
	bool slot_used[MAX_CONTACTS];
	int cur_slot = 0;
	memset(live, 0, sizeof(live));
	memset(build, 0, sizeof(build));
	memset(slot, 0, sizeof(slot));
	memset(slot_used, 0, sizeof(slot_used));

	contact_t *pending = &build[0];
	bool pending_dirty = false;

	int max_together = 0;
	unsigned long packets = 0, packets_at_mark = 0;
	int packets_per_sec = 0;
	int64_t started = now_ms(), mark = started, last_draw = 0;
	bool need_draw = true;

	printf("mtcircles: running. To quit, the red box top right or Ctrl-C.\n");

	while (running) {
		// poll and not a blocking read: if nobody touches the screen we still
		// have to update the per-second counter, honour -s and notice the
		// signal.
		struct pollfd pfd = {.fd = ev_fd, .events = POLLIN, .revents = 0};
		int ready = poll(&pfd, 1, 100);
		if (ready < 0 && errno != EINTR) {
			perror("mtcircles: poll");
			break;
		}

		int n = 0;
		struct input_event ev[64];
		if (ready > 0) {
			ssize_t got = read(ev_fd, ev, sizeof(ev));
			if (got < 0) {
				if (errno != EINTR) {
					perror("mtcircles: read");
					break;
				}
			} else if (got == 0) {
				break; // a test stream read from a file has ended
			} else {
				n = (int)(got / (ssize_t)sizeof(ev[0]));
			}
		}

		for (int i = 0; i < n; i++) {
			struct input_event *e = &ev[i];

			if (e->type == EV_ABS) {
				switch (e->code) {
				case ABS_MT_SLOT:
					cur_slot = (e->value >= 0 && e->value < MAX_CONTACTS) ? e->value : 0;
					break;
				case ABS_MT_TRACKING_ID:
					if (proto_b) {
						if (e->value < 0) {
							slot_used[cur_slot] = false;
						} else {
							slot_used[cur_slot] = true;
							slot[cur_slot].id = e->value;
						}
					} else {
						pending->id = e->value;
						pending_dirty = true;
					}
					break;
				case ABS_MT_POSITION_X:
					if (proto_b) {
						slot[cur_slot].raw_x = e->value;
					} else {
						pending->raw_x = e->value;
						pending_dirty = true;
					}
					break;
				case ABS_MT_POSITION_Y:
					if (proto_b) {
						slot[cur_slot].raw_y = e->value;
					} else {
						pending->raw_y = e->value;
						pending_dirty = true;
					}
					break;
				case ABS_MT_TOUCH_MAJOR:
					if (proto_b) {
						slot[cur_slot].major = e->value;
					} else {
						pending->major = e->value;
						pending_dirty = true;
					}
					break;
				case ABS_MT_WIDTH_MAJOR:
					if (proto_b) {
						slot[cur_slot].width = e->value;
					} else {
						pending->width = e->value;
						pending_dirty = true;
					}
					break;
				default:
					break;
				}
				continue;
			}

			if (e->type != EV_SYN) {
				continue;
			}

			// Protocol A: a SYN_MT_REPORT closes one contact and opens the
			// next. Both drivers send one per finger, then a SYN_REPORT to
			// close the packet.
			if (e->code == SYN_MT_REPORT) {
				if (pending_dirty && build_n < MAX_CONTACTS) {
					build_n++;
					pending = &build[build_n < MAX_CONTACTS ? build_n : MAX_CONTACTS - 1];
					memset(pending, 0, sizeof(*pending));
				}
				pending_dirty = false;
				continue;
			}

			if (e->code != SYN_REPORT) {
				continue;
			}

			// Packet closed: what is inside it is the state right now.
			if (proto_b) {
				build_n = 0;
				for (int s = 0; s < MAX_CONTACTS; s++) {
					if (slot_used[s] && build_n < MAX_CONTACTS) {
						build[build_n++] = slot[s];
					}
				}
			} else if (pending_dirty && build_n < MAX_CONTACTS) {
				// A driver that does not send SYN_MT_REPORT after the last finger.
				build_n++;
				pending_dirty = false;
			}

			for (int c = 0; c < build_n; c++) {
				map_point(build[c].raw_x, build[c].raw_y, &build[c].x, &build[c].y);
			}

			memcpy(live, build, sizeof(live));
			live_n = build_n;
			if (live_n > max_together) {
				max_together = live_n;
			}
			packets++;
			need_draw = true;

			if (verbose) {
				printf("%6lu  n=%d ", packets, live_n);
				for (int c = 0; c < live_n; c++) {
					printf(" [%d id=%d %d,%d w=%d]", c, live[c].id, live[c].raw_x, live[c].raw_y, live[c].width);
				}
				printf("\n");
				fflush(stdout);
			}

			// The quit box: any contact inside the rectangle.
			for (int c = 0; c < live_n; c++) {
				if (in_quit_box(live[c].x, live[c].y)) {
					running = 0;
				}
			}

			build_n = 0;
			pending = &build[0];
			memset(build, 0, sizeof(build));
			pending_dirty = false;
		}

		int64_t t = now_ms();
		if (t - mark >= 1000) {
			packets_per_sec = (int)(packets - packets_at_mark);
			packets_at_mark = packets;
			mark = t;
			need_draw = true;
		}
		if (stop_after > 0 && (t - started) / 1000 >= stop_after) {
			running = 0;
		}

		// No more than sixty times a second: the panel reports at ~100 Hz, and
		// redrawing at that rate inside a mapping that may not be cached costs
		// more than it gives.
		if (!need_draw || t - last_draw < 16) {
			continue;
		}
		last_draw = t;
		need_draw = false;

		uint8_t *base = page_base(fb_draw_page);
		restore_dirty(fb_draw_page);

		for (int c = 0; c < live_n; c++) {
			// The radius follows the reported width, within sane limits: a
			// finger pressing harder makes a bigger circle.
			int r = 30 + live[c].width / 2;
			if (r < 24) r = 24;
			if (r > 60) r = 60;

			uint16_t col = contact_color(c);
			fill_disc(base, live[c].x, live[c].y, r, col);
			draw_ring(base, live[c].x, live[c].y, r + 5, r + 2, rgb565(255, 255, 255));

			// Its position in the packet, written inside the disc, as large as
			// the disc allows: it says which circle is which line of coordinates.
			char num[2] = {(char)('0' + (c % 10)), 0};
			int gs = (r >= 34) ? 4 : 3;
			draw_text(base, live[c].x - (GLYPH_W * gs) / 2, live[c].y - (GLYPH_H * gs) / 2, num, gs, rgb565(0, 0, 0));

			dirty_add(fb_draw_page, live[c].x - r - 6, live[c].y - r - 6, live[c].x + r + 6, live[c].y + r + 6);

			if (trail) {
				fill_disc(trail, live[c].x, live[c].y, 3, col);
			}
		}

		// The status panel, top left. On solid black and at a fixed height: a
		// finger resting on it ends up UNDER it instead of making it
		// unreadable, and one dirty area covers everything -- including the
		// contact lines that were there last round and are not now.
		{
			char line[96];
			const int scale = 2, step = text_h(scale) + 3;
			const int panel_w = 26 * GLYPH_W * scale + 16;
			const int lines = 3 + (live_n < 5 ? live_n : 5);
			const int panel_h = lines * step + 4 + 12;
			const int panel_max_h = (3 + 5) * step + 4 + 12;

			// Filled as far as needed, but dirtied as far as it can ever get:
			// that way when the fingers drop the panel really shrinks, instead
			// of leaving last round's lines underneath.
			fill_rect(base, 0, 0, panel_w, panel_h, rgb565(0, 0, 0));
			dirty_add(fb_draw_page, 0, 0, panel_w - 1, panel_max_h - 1);

			int y = 8;
			snprintf(line, sizeof(line), "MTCIRCLES PROTO %c %dx%d", proto_b ? 'B' : 'A', fb_w, fb_h);
			draw_text(base, 8, y, line, scale, rgb565(160, 160, 160));
			y += step;

			snprintf(line, sizeof(line), "NOW %d  MAX TOGETHER %d", live_n, max_together);
			draw_text(base, 8, y, line, scale, max_together > 1 ? rgb565(52, 199, 89) : rgb565(255, 214, 10));
			y += step;

			snprintf(line, sizeof(line), "PACKETS %lu  PER SEC %d", packets, packets_per_sec);
			draw_text(base, 8, y, line, scale, rgb565(160, 160, 160));
			y += step + 4;

			for (int c = 0; c < live_n && c < 5; c++) {
				snprintf(line, sizeof(line), "%d ID%-2d %3d,%-3d W%-2d", c, live[c].id, live[c].raw_x, live[c].raw_y,
						 live[c].width);
				draw_text(base, 8, y, line, scale, contact_color(c));
				y += step;
			}
		}

		draw_quit_box(base);

		fb_show(fb_draw_page);
		if (fb_pages == 2) {
			fb_draw_page ^= 1;
		}
	}

	printf("mtcircles: %lu packets, most contacts at once: %d\n", packets, max_together);
	if (max_together <= 1) {
		printf("mtcircles: one contact only. The cap is still in place --\n"
			   "           Goodix: the driver was not patched, see\n"
			   "           tools/gt9xx_multitouch_patch.py; Hynitron: raise\n"
			   "           cst_max_touch_number in the insmod line.\n");
	} else {
		printf("mtcircles: multitouch is working.\n");
	}

	free(trail);
	if (ev_fd >= 0) {
		close(ev_fd);
	}
	fb_close();
	return 0;
}
