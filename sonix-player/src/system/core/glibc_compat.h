#ifndef GLIBC_COMPAT_H
#define GLIBC_COMPAT_H

// The device has glibc 2.22; the toolchain has 2.27, and the difference is not
// harmless. When glibc changes a function's implementation it keeps the old
// symbol beside the new one and tells them apart by version. Code built
// against 2.27 asks for the new version, the device's loader looks for that
// name, does not find it, and the program does not start:
//
//     /lib/libm.so.6: version `GLIBC_2.27' not found
//
// The link succeeds, so nothing shows at build time, and on the device nothing
// shows at run time either, because sonix_launch does `sleep 1; reboot` as
// soon as the player exits: the only symptom is a bootloop.
//
// glibc 2.27 reimplemented five single-precision functions (powf, expf, logf,
// exp2f, log2f) and 2.29 the double-precision ones. The old implementations
// are still there under their old version names, and those are exactly the ones
// the device knows, so they are requested by name below.
//
// The file is pulled into every target compilation unit by the Makefile's
// -include: a .symver only applies to the unit it sits in, and adding it by
// hand thirty-nine times invites forgetting the fortieth.
//
// It covers the whole binary and not one call site, so an expf appearing in
// the equaliser or in a decoder needs nothing added here.
//
// ---------------------------------------------------------------------------
// No #include may ever go in this file.
//
// Because of the -include hook this file is the first thing every compilation
// unit sees, before the first line of the .c. Feature-test macros
// (_GNU_SOURCE, _XOPEN_SOURCE, _FILE_OFFSET_BITS...) must be defined before any
// system header: <features.h> reads them once and then closes behind its own
// guard. Pulling it in here turns every `#define _GNU_SOURCE` at the top of a
// .c into a no-op.
//
// sqlite3_impl.c, http.c, usbdac.c and tls.c all define _GNU_SOURCE at their
// own top and need declarations that only appear under it -- mremap among
// them. An <features.h> pulled in from here would close the door before any of
// those lines is read.
//
// __mips__ and HOST_BUILD suffice and cost nothing: the compiler predefines the
// first, the Makefile passes the second with -D. Neither needs a header, which
// is why the guard is on those and not on __GLIBC__.
//
// The versions below are mipsel's; another architecture uses different names,
// which is the other reason for the __mips__ guard.
// ---------------------------------------------------------------------------

#if defined(__mips__) && !defined(HOST_BUILD)

// Reimplemented in glibc 2.27.
__asm__(".symver powf,powf@GLIBC_2.0");
__asm__(".symver expf,expf@GLIBC_2.0");
__asm__(".symver logf,logf@GLIBC_2.0");
__asm__(".symver exp2f,exp2f@GLIBC_2.2");
__asm__(".symver log2f,log2f@GLIBC_2.2");

// Reimplemented in glibc 2.29. With the current toolchain (2.27) these lines do
// nothing, because the default version of these functions is already the old
// one. They are here for the day the toolchain moves up, when they would
// otherwise break silently exactly as powf did.
__asm__(".symver pow,pow@GLIBC_2.0");
__asm__(".symver exp,exp@GLIBC_2.0");
__asm__(".symver log,log@GLIBC_2.0");
__asm__(".symver exp2,exp2@GLIBC_2.2");
__asm__(".symver log2,log2@GLIBC_2.2");

#endif

#endif /* GLIBC_COMPAT_H */
