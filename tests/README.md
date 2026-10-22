# Audiobook and update regression tests

Run from the repository root:

```sh
tests/run-audiobook-tests.sh
```

Requirements: a C compiler, POSIX shell, pthreads and SQLite development headers/library.
GUI-related checks also need the player's LVGL headers. On a clean checkout,
fetch the same version as the player Makefile before running the suite:

```sh
lvgl_version=$(sed -n 's/^LVGL_VER = //p' sonix-player/Makefile)
git clone --depth 1 --branch "v$lvgl_version" https://github.com/lvgl/lvgl.git sonix-player/lvgl
```

An existing `make host` checkout already has this directory. Install `pkg-config`,
`libopusfile-dev` and `libwavpack-dev` to run the optional decoder-duration checks;
GitHub Actions installs these so those checks are not skipped.

On Debian/Ubuntu: `apt-get install build-essential libsqlite3-dev`.
On macOS the system compiler and SQLite library suffice.

The runner creates fixtures under a unique temporary directory and removes them on exit.
It never reads a real music library, card, player or network service.

The database checks compile the real `audiobookdb.c` and the shared case-folding
implementation from `library.c`. Only metadata/chapter readers, file-identity
notifications and background-priority setup are stubbed. They cover:

- cancellation before scanning and after 55 of 60 files;
- failed directory reads and a file disappearing between listing and indexing;
- process termination mid-scan followed by reopening the persistent database;
- a live position save during scanning;
- successful pruning and rollback when publication fails after old rows were deleted;
- stale writes interleaved across two books, missing-row retry and a legitimate
  delayed write belonging to another book;
- Cyrillic album/author grouping, natural chapter ordering and Russian disc folders.

The playback check executes the real paused-track loading path with a decoder
and storage boundary stubbed; it verifies that outgoing progress is forced out
before the book association changes and before a new decoder is started.
The update checks ensure that a missing/exported card is rejected before
network/download or firmware lookup, and a read-only card before download.
The small ALSA include stub is only for host tests: `audio.h` exposes no ALSA types.

These checks are not a complete firmware/driver test suite. They do not verify
MIPS ABI, audio output, physical removal, recovery, or flash the device.
