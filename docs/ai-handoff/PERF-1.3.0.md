# Sonix Player 1.3.0 — performance, smoothness, battery (wave 3, branch r131-perf)

Base: sonix-r1 at 2cf4f4a (UI 2.0 phases 1–2 merged, beta2). Worktree /home/claude/w10-perf, branch `r131-perf`.
Host build (`make host -j2`) warning-free for every touched file; target build `make target -j2` ends with
"check-abi: no symbol beyond glibc 2.22" (the remaining target warnings are in search.c, medialist.c:3364 and
the sqlite amalgamation, all from before this wave).

## How it was measured

* `[debug] perf_stats = N` / `SONIX_PERF=N` (new, perfstat.c): every N s a `perf:` block in the log — main-loop
  passes and what woke them (timer due / gui_post / touch fd), every LVGL timer with runs/s and CPU/s, animations
  alive, frames with cost split into layout / draw / flush, median and p95 of the draw, pixels, every thread's
  context switches, process CPU, RSS, heap. `kill -USR2 <pid>` writes a report at once and opens a new window
  (two of them around one gesture measure that gesture). `SONIX_PERF_AREAS=1` logs each frame's areas.
  Off by default: each hook is then one flag test.
* Emulator (Xvfb :112, 480×800, test card with 3016 tracks). SDL's event pump and the screenshot poll are left out
  of the figures, and "without flush" frame times leave out SDL presenting the window, so they read like the device.
* callgrind (instructions per frame, deterministic) for the drawing hot spots: `cgrun.sh` / `cgtour.sh` /
  `cgframes.py` in /home/claude/tmp-w10perf.
* Device scale: the brief's ~14× (host ms → device ms), used for every "device estimate" below. Instruction
  counts are given next to it because they are exact and compare builds fairly; the two do not always agree
  (fixed per-frame overheads, memory traffic), and only the device can settle it — what to collect is listed
  at the end.

## 1. Wake-ups and CPU at rest

Main loop: `lv_timer_handler()` then `poll()` on the gui_post eventfd (+ the touch fd while its read timer is
parked) with the timeout = time to the next due timer, capped at 250 ms. It does not poll at a fixed rate: with
nothing due it sleeps until the next timer or a post. What remained was timers and animations that ran with
nothing to do.

Emulator, 5 s windows, same tour (`baseline.sh`): before = 2cf4f4a, after = HEAD.

| State | LVGL timer runs/s before → after | UI-thread CPU (host ms/s) before → after | frames/s before → after |
|---|---|---|---|
| Home, idle, screen on | 89.6 → **17.4** | 5.23 → **0.69** | 0.6 → 0 |
| All tracks list, idle | 90.8 → **17.0** | 5.79 → **1.17** | 1.0 → 0.2 |
| List, playing (EQ bars + mini bar) | 104.2 → **38.4** | 27.3 → **20.4** | 10.6 → 9.4 |
| Player up, playing | 111.0 → 129.4 | 33.9 → 67.2 | 11.8 → 30.4 (*) |
| Player up, paused | 88.0 → 116.4 | 4.4 → 62.5 | 0.4 → 27.8 (*) |
| Screen off, playing | 19.4 → **6.6** | 2.95 → **0.24** | 0.6 → **0** |
| Screen off, paused | 18.0 → **5.6** | 2.40 → **0.50** | 0.4 → **0** |

Threads (switches/s): the Bluetooth volume bridge `btvolume` 1.6 → **0** with no BT stream (wired, BT off,
dark screen). `playback` stays at ~5/s idle and 10–12/s paused (its pause loop polls every 50/200 ms, see
recommendations), `watchdog` 1/s, `logwriter` ~2/s (mostly the perf log itself).

Device reading (×14): the UI thread resting on Home went from ~7 % of the core (the LVGL animation timer at
60 Hz for spinners/carets/titles nobody could see) to ~1 %; in the dark while playing from ~4 % to ~0.3 %.
On the device add the touch read timer: 60 wake-ups/s whenever the screen is on, unless `[display]
touch_events = 1` (see recommendations).

(*) The player rows are not a regression. On the base build the long test title showed cut ("Long Tone") and
did not scroll in this tour; since adbeb04 (titles parked when unseen and restarted when drawn) it scrolls as
designed — 2 px steps, ~30 frames/s for as long as the player is up with a title wider than the panel. That
made the per-frame cost of the player matter, which led to fix 2a below: each step now costs 0.79 M
instructions instead of 4.1 M. QA may want to confirm the base behaviour was a glitch (screenshots
/home/claude/tmp-w10perf/shots/before_player.png vs final_player.png).

What still runs at rest (screen on, Home, after): power tick 4/s, jack poll 4/s, five streaming "queue
watchers" (Tidal, Qobuz, Subsonic, Yandex, Podcasts) 1/s each, Last.fm/play-count tick 1/s, gui_post drain 1/s,
BT routing watch 1/s, DLNA/SonixLink pumps 0.5/s each, radio 0.5/s, clock 0.1/s. In the dark: power 1/s, jack
0.5/s, the 1 Hz watchers. Each costs microseconds; together ~6 separate wake-ups/s in the dark because their
phases differ (see recommendations).

## 2. Rendering and smoothness

Emulator, one window per gesture (`gestures.sh`, HEAD). "Draw" = frame cost without the SDL flush.

| Gesture | frames | draw p50 / p95 / max (host ms) | px/frame | callgrind M instr/frame | device estimate (draw, ×14) |
|---|---|---|---|---|---|
| Home → Music (MOVE 180 ms) | 15 | 0.83 / 1.14 / 1.14 | 355 k | 3.5 | 12 / 16 ms |
| Music → All tracks (page build + slide) | 12 | 1.12 / 1.75 / 1.75 | 384 k | 4.9 | 16 / 24 ms |
| All tracks drag (3016 rows) | 153 | 0.76 / 1.07 / 8.3 | 321 k | 3.4–4.0 | 11 / 15 ms |
| All tracks fling | 57 | 0.77 / 1.11 / 1.22 | 316 k | — | 11 / 16 ms |
| A–Z rail drag | 29 | 1.11 / 1.67 / 1.72 | 230 k | — | 16 / 23 ms |
| Sheet open (long press) | 15 | 1.04 / 2.10 / 2.10 | 192 k | 4.2 | 15 / 29 ms |
| Sheet close | 13 | 0.75 / 1.67 | 163 k | 4.8 | 10 / 23 ms |
| Player open | 47 | 0.27 / 2.00 / 3.85 | 98 k | 1.4 avg | 4 / 28 ms |
| Player collapse | 37 | 0.21 / 1.93 / 4.20 | 70 k | 0.4 avg | 3 / 27 ms |
| Back (button) | 23 | 0.82 / 1.08 / 1.72 | 234 k | 1.1 | 11 / 15 ms |
| Back (edge swipe) | 33 | 0.45 / 1.30 / 1.34 | 134 k | — | 6 / 18 ms |
| Player title scrolling, before → after | 30/s | 0.45 → **0.33** p50 | 18 k | **4.1 → 0.79** | 6.3 → 4.6 ms/step |
| List playing (EQ bars) | 9.4/s | 0.10 | 0.2 k | 0.09 | 1.4 ms |

Page transitions draw both screens in full for every frame (355–384 k px); the 180 ms MOVE is 11 frames at
60 Hz. No transition or sheet leaves a frame over 16 ms on the host; on the device the p95s of the slides and the
sheet land around 25–30 ms, i.e. those gestures run at 35–60 fps there, the list scroll near 60.

Where a list-scroll frame goes (callgrind, All tracks, ~4 M instructions/frame): text 60 % (glyph lookup and
blending; LVGL label draw 1.7 M, label event overhead 0.7 M), style lookups ~20 % (inside the above), row
re-binding as rows recycle (`scroll_cb`, set_title/subtitle) 11 %, layout 9 %, solid fills 13 % (one fill of the
area — no overdraw), ARGB icon recolour + image blend ~6 % (chevrons, stars), radius masks ~1 %. The mini player
on lv_layer_top floats over the list, so every scroll frame redraws the band behind it too (frames are
0,120–479,799): ~12 % of the pixels. Opening All tracks costs ~25 M instructions to build the page and ~27 M for
its first layout before the slide starts (≈ 70–100 ms on the device).

### 2a. Fixed: the player panel's gradient (c2a8e8d)

The panel under the title has rounded top corners and its dithered gradient was a tiled background image. LVGL
draws a tiled image under a radius through its radius mask line by line over every tile of the *whole object*
(not the area being redrawn) for any frame that touches it: ~10,000 masked lines per step of the scrolling title
(img_draw_core 3.3 M of the 4.1 M instructions per frame). `sx_gradient_paint()` now draws the strip from the
panel's draw event, clipped to the redrawn area, through the mask only in the two corner bands and as plain
copies elsewhere (the layer clip is narrowed as well: LVGL's plain path blends whole tiles up to the clip).
Pixel-identical to before (screenshot diff empty); 4.1 → 0.79 M instructions per title step, host p50
0.45 → 0.33 ms. Also helps every other partial redraw of the panel (clocks, bar, chain, buttons) and the
player sheet's open/collapse. The album page and the player page gradients have no radius and take LVGL's cheap
path; they are unchanged.

### 2b. Checked, nothing to fix

* EQ bars: the timer is ~18 k instructions a run (eq_any_shown + three set_height); its frames are ~90 k
  instructions plus a ~65 k layout pass of the screen each (the bars are flex children). Cheap; a one-object
  custom-drawn EQ would save the layout pass (~0.1 ms/frame on the device) — not worth the churn now.
* Event lists: no handler build-up (max 9 event callbacks on one object, 11 on the display after a tour).
* Fills: one opaque fill per area (rows are transparent unless pressed/lifted) — no overdraw to remove.
* Opacity/transform layers: only the scrolling title (bitmap mask for the edge fade) uses a layer; small.
* Image decode on the UI thread: covers decode in the cover loader thread; 56 px thumbs are slot-file reads
  (~23 k instructions/frame in a scroll); 240 px are refused on the UI thread.

## 3. Memory

50 rounds of: album page open/back, track sheet open/close, a "play next" swipe with its toast, player
open/collapse, search open/back (`cycle.sh`, `leak.sh`):

| | heap in use | RSS |
|---|---|---|
| after 2 warm-up rounds | 57 747 kB | 140 648 kB |
| after 25 rounds | 57 780 kB | 140 704 kB |
| after 50 rounds | 57 803 kB | 140 728 kB |
| + 20 rounds without the "play next" swipe | 57 804 kB | 140 732 kB |

+56 kB over 50 rounds, all from the 50 tracks queued by the "play next" swipe (queue entries, order array,
undo snapshot); without that step +1 kB over 20 rounds. No leak found in sheets, toasts, player, album page or
search. (Absolute host figures include SDL/llvmpipe; device RSS comes from the device log.)

Caches are bounded: FreeType glyphs 1024 per face, title fade masks 8, covertone path cache 8 and 16 queued
thumb requests, EQ registry 96, cover thumbs in slot files. The interface itself is the large resident item:
8 311 LVGL objects on 122 screens built at start (pages kept built), unchanged by this wave.

## 4. Battery: system settings (investigated; nothing changed in the system)

* **cpufreq**: the stock rootfs (/home/claude/verify7/rfs) has no cpufreq tooling or governor set-up and the
  player never touches /sys/devices/system/cpu; whether the X1600 kernel exposes cpufreq is unknown. Check on
  the device: `ls /sys/devices/system/cpu/cpu0/cpufreq/` and `cat scaling_available_governors
  scaling_cur_freq`. If `ondemand`/`schedutil` exist, a lower `scaling_max_freq` while the screen is off could be
  worth a test against the audio health line (decode 7 % + bluealsa 10–12 % of the core at full clock in
  1.2.3 logs — there is headroom, but LDAC + Wi-Fi downloads left only 5 % idle).
* **Screen-off behaviour** (done in this wave): nothing is drawn in the dark (refresh timer held down,
  animation timer kept paused), the power tick drops to 1 Hz, idle watchers stop, and suspend-to-RAM after
  60 s of silence already exists ([power] standby_mem).
* **Wi-Fi power save while streaming**: brcmfmac; the rootfs has no `iw`/`wl`, the player sets nothing, so the
  kernel default applies (CONFIG_CFG80211_DEFAULT_PS, normally on = PM mode). Verify on the device with
  `cat /sys/module/brcmfmac/parameters/*` and the dmesg at association. Streaming with PM on is fine for the
  buffered services (whole files/segments are fetched ahead); if radio stutters appear it is the first thing
  to look at. Setting it from the player would need nl80211 — not done.
* **Bluetooth sniff / LPM**: the stock bring-up runs `brcm_patchram_plus ... --enable_lpm` (BT_WAKE/HOST_WAKE
  UART sleep). Sniff is negotiated per link by BlueZ and the headset; it does not apply while A2DP streams.
  Check `hciconfig hci0 lp` (expect RSWITCH SNIFF) and `hcitool lp <addr>` on the device; nothing to change
  blind.

## Commits on r131-perf (2cf4f4a..HEAD)

| Commit | What |
|---|---|
| 88efb87 | Debug: wake-up and frame statistics in the log ([debug] perf_stats) |
| adbeb04 | Idle: spinners, carets and scrolling titles stop when they cannot be seen (anim timer no longer 60 Hz at rest) |
| 3bff69b | Screen off: nothing drawn in the dark, animation timer kept down, power tick 1 Hz |
| 3e3145b | Status bar and player: only values that change are repainted (sx_set_text/src/recolor) |
| e943660 | Idle: cover worker's touch watcher, A–Z strip timers, EQ timer run only while needed |
| fadd677 | Debug: perf_stats per-timer CPU, frames without their flush, SONIX_PERF_AREAS |
| deb4573 | Debug: perf_stats frame p50/p95, report on SIGUSR2 |
| c2a8e8d | Player: the panel's gradient costs a fifth of what it did (sx_gradient_paint) |
| 42f0f00 | Idle: the Bluetooth volume bridge sleeps while there is no stream |

## Recommendations (not done — need the device or a decision)

1. **`[display] cached_render = 1` — measure first, likely the biggest single win on the device.** The
   framebuffer is mapped uncached (jzfb_mmap); LVGL blends straight into it, so every read-modify-write pixel
   (text, icons, the title fade, gradients) and every refr_sync_areas copy is a DRAM round trip. cached_render
   draws into a cached RAM page and copies the damage out with write-only memcpy, for +1 page of RAM
   (~750 KB at 480×800). Compare scroll/transition smoothness and the `perf:` draw p50/p95 with it on and off.
2. **`[display] touch_events = 1`**: parks the touch read timer between touches — 60 fewer wake-ups/s
   whenever the screen is on (Home at rest would go from ~77 to ~17 wake-ups/s). Needs a check that taps,
   long presses, flings and the edge swipe behave the same.
3. Single-colour Lucide icons as A8 instead of ARGB8888 + recolour (svg_to_lvgl.py emits ARGB for all 282):
   ~5 % of a list-scroll frame, more on icon-heavy pages.
4. Coalesce the 1 Hz watchers (five streaming queue watchers, the Last.fm/play-count tick, gui_post drain, BT
   routing) into one 1 Hz tick or align their phases: ~6 fewer wake-ups/s in the dark. Microseconds of CPU,
   only worth it if the device shows idle wake-ups matter.
5. audio.c pause loop: a condition wait instead of the 50/200 ms usleep poll (5–12 wake-ups/s while paused).
   Audio-thread code; leave to the audio owner.
6. Jack poll at 4 Hz with the screen on: evdev switch events (SW_HEADPHONE_INSERT) if the codec driver reports
   them.
7. The scrolling player title runs at ~30 frames/s while the player is up: by the ×14 rule ~14 % of the core
   after 2a (~19 % before); its instruction count fell 5×, so the real share is probably lower. If battery with the player open matters, scroll a long title twice per track and stop at its start
   (tap to scroll again) — a design call.
8. Page building: All tracks costs ~50 M instructions before its slide starts (build + first layout); the
   list could be created with the first screenful only and the rest bound after the first frame.

## What to measure on the device (1.3.0 beta with this branch)

Turn on `[debug] perf_stats = 10` in device_config.ini, then for ~60 s each:

1. Home, screen on, nothing touched; same with `touch_events = 1`.
2. All tracks open, idle; then playing with the EQ bars visible.
3. Player up, playing, with a long title; same paused.
4. Screen off playing local FLAC over wired; over Bluetooth (LDAC and SBC); screen off paused.
5. Fling All tracks a few times; open/close the player; open a sheet — with `cached_render = 0` and `= 1`.

From each `perf:` block note: `timers/s`, `woke: due/post/touch`, `handler cpu ms/s`, `frames`, the draw
`p50/p95/max`, `process cpu %`, `rss`; and from the audio health line `the core over the same span: idle %,
interface %, bluealsa %`. Battery: drain per hour screen-off playing (wired / BT) from the battery
percentage in the log, before (beta2) and after.
