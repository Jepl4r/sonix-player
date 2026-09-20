# Sonix Player

A replacement player for the **HiBy R3 Pro II**, written on LVGL.

**Other devices**: only the R3 Pro II is supported today. Support for the R1 is planned.

There are two builds from one tree:

| | binary | runs on |
|---|---|---|
| **host** | `sonix_player_host` | your PC, in an SDL window |
| **target** | `sonix_player` | the device |

---

## Building for the host

The simulator draws the real interface in an SDL window, using the same source
as the device. Most work happens here.

### What you need

Compiler, make, git, and four libraries.

**Fedora**

```bash
sudo dnf install gcc gcc-c++ make git pkgconf-pkg-config \
                 SDL2-devel freetype-devel opusfile-devel \
                 wavpack-devel alsa-lib-devel
```

**Debian / Ubuntu**

```bash
sudo apt install build-essential git pkg-config \
                 libsdl2-dev libfreetype-dev libopusfile-dev \
                 libwavpack-dev libasound2-dev
```

**Arch**

```bash
sudo pacman -S base-devel git sdl2 freetype2 opusfile wavpack alsa-lib
```

`opusfile` pulls in `opus` and `libogg` by itself.

### Build

```bash
make host -j$(nproc)
```

### Resources

The player reads its language files, its streaming keys and some of its gui assets
from a resource tree:

```
sonix_player_host
usr/
└── resource/
    └── sonix/
        ├── language/                    the 7 .ini files
        ├── components/
        │   └── streaming-keys.ini       Tidal / Qobuz / Podcast Index keys - Need to provide your own.
        └── gui/                         some of the .png assets the UI loads at runtime - the rest are inside the binary.
```

Without the language files the interface draws raw tags instead of words, and says so on the first line of the log. Without `streaming-keys.ini` everything works except Tidal, Qobuz and podcasts.

Fonts are looked for in `usr/resource/sonix/fonts` first and fall back to
`assets/fonts` in the repo, so a tree without a resource folder still has text.

### Run

```bash
./sonix_player_host
```

The window is 480x720, the mouse is the finger, and dragging scrolls.

The folder that stands in for the memory card is the **documents folder**,
found from the desktop's own XDG setting. Point it elsewhere with:

```bash
SONIX_SD_ROOT=/path/to/a/card ./sonix_player_host
```

Music goes in there, and so does everything the player writes: the index, the
cover cache, the playlists.

### Keyboard

The device's buttons are on the keyboard:

| key | button |
|---|---|
| `p` | power — a tap toggles the screen, held opens the power menu |
| `u` | volume up |
| `i` | volume down |
| `b` | previous track |
| `n` | play / pause |
| `m` | next track |


### Other environment variables

| variable | what it does |
|---|---|
| `SONIX_SD_ROOT` | the folder standing in for the card |
| `SONIX_CONFIG` | where the settings file lives |
| `SONIX_EBOOK_CONFIG` | the ebook reader's own settings file |
| `SONIX_LANG_DIR` | the language directory, instead of the resource tree |
| `SONIX_LOG` | write the log to a file |
| `SONIX_BATTERY_CAPACITY`, `SONIX_BATTERY_STATUS` | two files to fake a battery |
| `SONIX_NO_MOUNT`, `SONIX_NO_SYSSERVER` | leave the host's own system alone |


## Building for the device

### What you need

The host requirements above, plus `wget`, `texinfo`, `bison`, `flex` and about
2 GB of disk: the first target build compiles a MIPS cross toolchain from
source.

### Build

```bash
make target -j$(nproc)
```

The first run takes a while and does four things by itself:

1. builds the Rockbox MIPS toolchain into `rockbox-toolchain/`
2. downloads and cross-builds FreeType, static, into `freetype-target/`
3. downloads and cross-builds libogg, libopus, opusfile and libwavpack, static,
   into `audio-target/`
4. compiles and links `sonix_player`

Steps 1 to 3 happen once. Later builds go straight to step 4.

Everything the device does not already carry is linked statically, so the
result is one file to copy across with nothing to install beside it.

### The ABI check

The link is followed by a `readelf` pass that fails the build if the binary
asks for a glibc symbol newer than the device's 2.22.

This is not decoration. A binary that asks for a newer symbol links without a
word and then refuses to start, and `hiby_player.sh` runs `sleep 1; reboot` as
soon as the player exits - so the only symptom on the device is a boot loop.


## Creating the firmware image

The packer looks only next to itself:

```
sonix-packer/
├── sonix_firmware_packer.sh
├── r3proii_original.upt     the stock firmware, from HiBy
├── sonix_player             the binary from `make target`
└── assets/                  an overlay copied onto the root of the rootfs
```

`assets/` mirrors the rootfs from its root, so a file goes to the path it has
inside the folder. That is how the resource tree gets installed:

```
assets/
├── usr/
│   ├── resource/
│   │   └── sonix/
│   │       ├── language/          the 7 .ini files
│   │       ├── components/
│   │       │   ├── streaming-keys.ini	 Tidal / Qobuz / Podcast Index keys - Need to provide your own.
│   │       │   └── system-info.json     required: the packer writes to it
│   │       ├── fonts/             		 default.otf, bold.otf, Korean.ttf, Thai.ttf
│   │       └── gui/               		 some of the .png assets the UI loads at runtime - the rest are inside the binary.
│   └── data/                      		 anything else to ship on the device
└── module_driver/                 		 patched drivers, if there are any
```

`system-info.json` has to be there. The packer writes the build stamp into its
`build_version` key and stops if the file is missing.

### What it needs installed

```bash
# Debian / Ubuntu
sudo apt install p7zip-full squashfs-tools genisoimage

# Fedora
sudo dnf install p7zip squashfs-tools genisoimage
```

### Build

```bash
./sonix_firmware_packer.sh
```

It runs through without asking anything:

1. unpacks the `.upt`, joins the rootfs chunks and extracts the squashfs
2. deletes `usr/bin/hiby_player` and installs `usr/bin/sonix_player`
3. renames `hiby_player.sh` to `sonix_player.sh` and rewrites the name inside it
4. points `etc/init.d/S92_03_start_music_player` at the new launcher
5. copies `assets/` over the rootfs
6. deletes the stock interface's own resources — `litegui`, `layout`, `str`,
   `fonts`.
7. writes the build stamp into `system-info.json`
8. repacks the squashfs, splits it into 512 KB chunks and rebuilds the md5
   chain the recovery kernel checks
9. writes `r3proii.upt`

The kernel is carried across untouched, size and md5 copied from the original
rather than recomputed.

### Patches

They go in **before** the repack, and the easiest way is through
the overlay - any other file need to be in the respective folder mirroring the rootfs structure.

### Flashing

1. Copy `r3proii.upt` to the **root of the microSD card.**
2.  Insert the SD card into your HiBy R3 Pro II.
3.  Hold **Volume Up** and press **Power** to enter the updater.
4.  Let it flash - it will say "Upgrading..." then "Succeeded" and reboot by itself.

> It is recommended to **charge above 30%** first. 
**Recovery from a failed flash:** If something goes wrong, you can always restore by flashing the original stock firmware from HiBy's website using the same procedure.


## Generated files

Three files in the tree are produced by a script and committed, so an ordinary
build needs no Python at all. Re-run the script only when its input changes.

| generated | from | script |
|---|---|---|
| `src/gui/shell/icons.c`, `icons.h` | `assets/icons/*.svg`, `*.png` | `tools/svg_to_lvgl.py` |
| `src/system/net/webpage.h` | `web/index.html`, `web/icons`, `web/img` | `tools/web_to_c.py` |

```bash
pip install cairosvg pillow
python3 tools/svg_to_lvgl.py

python3 tools/web_to_c.py
```


## Source code tree

```
sonix-player/
│
├── sonix-packer/
│   ├── assets/
│   │   ├── etc/					 bootlogo, sonix-player.conf, cert.pem, modified S80_bt_init
│   │   ├── module_driver/			 patched gt9xx_touch.ko, gt9xx_touch.sh and leds_sgm31324_add.sh with 3 added LED registers
│   │   └── usr/					 bluealsa 4.3.1, resources required by Sonix Player                      
│   └── sonix_firmware_packer.sh     
│
│
├── sonix-player/                    
│   ├── assets/                      
│   │   ├── gui/                     some of the .png assets the UI loads at runtime - the rest are inside the binary.
│   │   ├── fonts/ 					 the four faces: default, bold, Korean, Thai
│   │   └── icons/                   191 SVGs and PNGs, baked into src/gui/shell/icons.c
│   │   
│   │
│   ├── rockboxdev/                  builds the MIPS cross toolchain, first `make target` only
│   │   ├── toolchain-patches/       three patches gcc and binutils needed on a modern host
│   │   └── rockboxdev.sh
│   │
│   ├── src/
│   │   ├── gb/                      Gearboy, upstream and untouched
│   │   │   ├── core/                78 files: the emulator itself
│   │   │   └── miniz/               reads a ROM straight out of a .zip
│   │   │
│   │   ├── gui/                     
│   │   │   ├── audio/               EQ, PEQ, MSEB and the DAC page
│   │   │   ├── bluetooth/           pairing, codec, AirPods, receiver mode
│   │   │   ├── ebook/               EPUB reader: pages, shelf, bar, bookmarks, themes
│   │   │   ├── fonts/               the lv_font_t objects FreeType fills in at startup
│   │   │   ├── gearboy/             game list, screen and button pad
│   │   │   ├── library/             file browser, media lists, playlists, search
│   │   │   ├── nowplaying/          player screen, cover art, cover flow, waveform, queue
│   │   │   ├── settings/            26 files, one page each
│   │   │   ├── shell/               40 files: theme, screen switcher, status bar, control
│   │   │   │                        centre, keyboard, popups, toasts, settings rows, icons.c
│   │   │   ├── streaming/           Tidal, Qobuz, podcast and radio pages
│   │   │   └── wireless/            Wi-Fi, file transfer, AirPlay, DLNA, SonixLink
│   │   │
│   │   ├── system/                  
│   │   │   ├── audio/               ALSA, volume, filter chain, USB audio, headset
│   │   │   ├── bluetooth/           the whole stack, run by the player and not by the firmware
│   │   │   ├── core/                config, language, logging, paths, utils
│   │   │   ├── db/                  SQLite
│   │   │   ├── decode/              one file per format, plus the dispatcher and CUE handling
│   │   │   ├── device/              power, screen, buttons, USB, storage, ADB, firmware, LED
│   │   │   ├── ebook/               EPUB parsing: zip, XML, XHTML, CSS, OPF, arenas
│   │   │   ├── gearboy/             the emulator's glue: input, ROM database, save states
│   │   │   ├── image/               JPEG and PNG decoder
│   │   │   ├── input/               key mapping
│   │   │   ├── library/             index, tag reading, cover art, playlists, audiobooks
│   │   │   ├── net/                 HTTP, TLS, HLS, mDNS, transfer page
│   │   │   ├── playback/            queue, device state, audiobooks, sleep timers
│   │   │   ├── remote/              AirPlay, DLNA, SonixLink
│   │   │   └── streaming/           service clients and their caches
│   │   │
│   │   └── main.c                   entry point: display, input, and the startup order
│   │
│   ├── tools/                       generators, patches
│   │
│   ├── web/                         Wi-Fi transfer page, source of src/system/net/webpage.h
│   │   ├── icons/                   26 Lucide glyphs, inlined as <symbol>
│   │   ├── img/                     favicon-web.png, logo-web.png
│   │   └── index.html               the template
│   │
│   ├── generate_compile_commands.py 
│   ├── lv_conf.h                    
│   └── Makefile                     
│
├── FEATURES.md                      
├── LICENSE                          
├── PATCHES.md                       
└── README.md
```



## Special thanks to:
[@Tartarus6](https://github.com/Tartarus6)

[@noisetta](https://github.com/noisetta)

and all the members of this fantastic community!!
