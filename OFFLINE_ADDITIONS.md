# Offline audiobook additions

These changes selectively adapt work from
[yetisoldier/sonix-audiobooks at d93da3b](https://github.com/yetisoldier/sonix-audiobooks/tree/d93da3b3a7414bb549488be98b3ebbd82ef231d8),
under the repository's GPL-3.0 license. In particular, the sidecar parser,
`devcontrol.c/.h`, PowerShell device helper and simulator smoke are derived
from that project. Local changes harden command framing and adapt assertions
and navigation to this branch. No Audiobookshelf client, server configuration,
network synchronization or sync database schema is included.

## Add series and a description without retagging audio

For `Book.mp3`, create a UTF-8 file named `Book.mp3.tags` beside it:

```ini
series=The Example Cycle
series_part=2.5
```

The optional `Book.mp3.description` holds the description as text, including
multiple paragraphs. Simple HTML is cleaned like embedded descriptions.
Sidecar values override embedded values; descriptions are bounded to 8 KiB.
Remove the sidecar to use embedded metadata again. Rescan Audiobooks after
editing these files. The same naming applies to M4B and other audio files;
for multipart books, place consistent metadata beside their parts.

## Run the real interface in CI or locally

```sh
bash tests/run-metadata-sidecar-test.sh
tools/audiobook-host-smoke.sh
```

The smoke builds the host player unless `SONIX_SMOKE_SKIP_BUILD=1`, generates
its own card and database under `build/audiobook-host-smoke`, drives the GUI,
and verifies catalog/description indexing, bookmarks, exact checkpoint resume
after restart and gapless multipart advance. It saves screenshots and logs.
Dependencies: the normal host build dependencies plus ffmpeg, sqlite3, Xvfb,
xdotool and ImageMagick. CI installs these and uploads the visual evidence.
Playback goes to ALSA null; this is not an acoustic or physical hardware test.

## Drive the player over ADB

With ADB enabled and the device connected to the computer running the helper:

```sh
tools/sonix-device-control.sh status
tools/sonix-device-control.sh tap 240 746
tools/sonix-device-control.sh swipe 240 650 240 250 500
tools/sonix-device-control.sh wake
tools/sonix-device-control.sh shot
```

`shot` asks the player's existing screenshot service to save a screenshot.
Windows users can use `tools/sonix-device-control.ps1`; its framebuffer export
is specifically sized for R1 (480x800).

The device channel is the root-owned, mode-0600 `/tmp/sonix-control` FIFO;
status is `/tmp/sonix-control.status`. It is local, not a network listener,
and does not enable ADB or Wi-Fi. A second LVGL pointer leaves the physical
touchscreen available. Commands are newline-delimited; malformed/oversized
lines are rejected, and each GUI iteration reads at most 4096 bytes. Hosts
can isolate parallel simulators with `SONIX_CONTROL_FIFO` and
`SONIX_CONTROL_STATUS`; device builds keep the fixed paths.
