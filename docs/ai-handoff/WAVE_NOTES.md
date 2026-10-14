# Wave work notes (shared by all agents)

Repo: Sonix Player (LVGL music-player firmware for HiBy R1 / R3 Pro II). Upstream base commit 1743e5b.
Each agent works ONLY in its own git worktree (given in its prompt) on its own branch. Do not touch other worktrees,
/home/claude/sonix (integration tree, toolchain is building there — never run make in it), or push anything.

## Constraints
- Device: single-core MIPS X1600, 64 MB RAM (~10 MB free at runtime), microSD. Everything you add must cost ~0 CPU/RAM when unused.
- LVGL is single-threaded: only the UI thread touches lv_* ; workers hand results back with gui_post().
- Style: follow .clang-format (tabs, width 4), module_verb prefixes, British spelling (favourites, colour),
  long explanatory comments saying WHY, header comments stating thread contract. Match surrounding code.
- Settings: config getters/setters in the existing sections ([library], [music], [wireless], [bluetooth], ...).
- Every new user-visible string = a translation key added to ALL 11 language .ini files in BOTH
  sonix-packer/assets/R1/usr/resource/sonix/language/ and sonix-packer/assets/R3PII/usr/resource/sonix/language/.
  Provide a real translation for each language (English, Italiano, Deutsch, Español, Français, Russian, Chinese,
  Japanese, Polski, Português BR, Nederlands — check actual file names). Put your keys right after the existing
  section of the .c file they belong to (the files have `; --- file.c` comments) to keep merges clean.
  Labels used in popover_item_t initialisers must be static initialisers (see trackmenu.c ~752).
- Target build is not available yet (toolchain building). Avoid new glibc symbols newer than 2.22 (see glibc_compat.h).
- Keep the host build warning-free for files you touch.

## Host build & emulator
    cd <worktree>/sonix-player && make host -j2        # first run clones LVGL 9.6.0 (git proxy ok); needs python3
    # resources:
    mkdir -p usr && cp -r ../sonix-packer/assets/R1/usr/resource usr/
    # run headless with screenshots (use YOUR display number from the prompt):
    Xvfb :NN -screen 0 480x800x24 &  export DISPLAY=:NN
    SONIX_PANEL=480x800 SONIX_SD_ROOT=<your copy of /home/claude/testcard> SONIX_CONFIG=<tmp>/device_config.ini \
      SONIX_NO_MOUNT=1 SONIX_NO_SYSSERVER=1 SONIX_LOG=<tmp>/log.txt ./sonix_player_host &
    xwd -root -display :NN | convert xwd:- shot.png        # screenshot, view it with the Read tool
    xdotool mousemove X Y click 1 ; xdotool key n            # tap / buttons: p power, u/i vol, b n m transport
    Copy /home/claude/testcard to your own temp dir first (the player writes its index into the card).
- Test card (/home/claude/testcard/Music): Pink Floyd albums from 1967/1973/1979 (names chosen so alphabetical !=
  chronological), a compilation "Best of 80s" with COMPILATION=1 and NO albumartist (3 different artists), Кино (Cyrillic),
  an untagged file (Untagged/raw.flac, loud), RG/01.flac with REPLAYGAIN tags (-8 dB).
- CPU is shared with a toolchain build: use -j2 at most, don't rebuild from clean unnecessarily.

## Deliverable
Commit on your branch (one commit per feature, maintainer-style message: subject = short feature list,
body grouped by area in user-facing prose; end with the two trailer lines given in your prompt).
Final message: per item — done / partial / not done, files touched, how verified (emulator screenshots paths), risks,
anything needing on-device verification.

## Lessons from wave 1 (READ)
- Base for wave 2 is branch sonix-r1 (wave 1 merged). Your worktree already contains lvgl/, build_host/, build_gen/, usr/ and a built
  sonix_player_host copied from the integration build: `make host -j1` is incremental. Use -j1: the cross toolchain is building on the same 2 CPUs.
- NEVER `pkill -f sonix_player_host` or `pkill sonix_player_host` — it kills other agents' emulators (and pkill -f can kill your own shell).
  Copy the binary to your temp dir under a unique name (e.g. sp_w2lib) and stop it with `pkill -x <name>` (process names are truncated to 15 chars!).
- With SONIX_NO_MOUNT the host build puts library.db in /tmp/.local which is SHARED between agents. Run the player isolated:
    unshare -m --propagation private sh -c 'mount --bind <yourtmp>/dotlocal /tmp/.local && exec <yourbinary>'
  (see /home/claude/tmp-lib/run.sh). A pre-configured device_config.ini (welcome done, Russian UI) is at /home/claude/tmp-int/device_config.ini;
  a scanned card copy at /home/claude/tmp-int/card (copy both to your temp dir).

## Wave 3 notes
- Base: sonix-r1 (waves 1+2 merged; a device image 1.2.1 was built from it). Prebuilt host objects are in your worktree (touched newer than sources) — `make host -j2` is incremental.
- The cross toolchain now exists at /home/claude/sonix/sonix-player/rockbox-toolchain (bin/mipsel-rockbox-linux-gnu-gcc, sysroot with glibc 2.27, alsa-lib, dbus, glib, expat, zlib, libffi). You may USE it read-only (e.g. to cross-compile a test or a library) but never run make in /home/claude/sonix.
- You may also check your code compiles for the target: in your worktree `make target -j2 ROCKBOX_TOOLCHAIN_DIR=/home/claude/sonix/sonix-player/rockbox-toolchain` after copying /home/claude/sonix/sonix-player/{freetype-target,audio-target} into your worktree's sonix-player/ (they are prebuilt static libs). This must end with "check-abi: no symbol beyond glibc 2.22".

## 1.2.3 notes
- Base: sonix-r1 at 5987ee5 (all waves + upstream main merged: card notice, Podcast rework). Prebuilt host objects in your worktree are fresh: `make host -j2` is incremental.
- freetype-target/ and audio-target/ are already copied (cp -a) into your worktree: `make target -j2 ROCKBOX_TOOLCHAIN_DIR=/home/claude/sonix/sonix-player/rockbox-toolchain` must end with "check-abi: no symbol beyond glibc 2.22".
- The user is away; nobody can answer questions. Make conservative decisions, document them in the commit body and the report.

## 1.2.4 notes
- Base: sonix-r1 at d094956 (1.2.3 + changelog). Worktrees /home/claude/w6-ya, w6-ui, w6-audit. build_host there may be stale: the first `make host -j2` can be a full rebuild — that's fine.
- freetype-target/ and audio-target/ are copied into each worktree; target check as in 1.2.3 notes.
- Real device logs from 1.2.3 are in /home/claude/devlogs/ (1.2.3-sonix_player.log, 1.2.3-bluetooth.log, 1.2.3-library.db). Read them (read-only).
- Emulator display numbers: w6-ya :61, w6-ui :62, w6-audit :63. Temp dirs: /home/claude/tmp-w6ya, tmp-w6ui, tmp-w6audit.
- The user is away; make conservative decisions, document them. Defaults that change must only affect users who never set the key explicitly (config_get_* with new fallback is fine — check the key isn't written at first boot / welcome).
- Commit trailers:
    Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
    Claude-Session: https://claude.ai/code/session_01K21tweGmc5xuSmQjL98HRk

## 1.2.5 notes
- Base: sonix-r1 at 2a70896 (1.2.4 = Yandex lossless/likes/save, streaming "+", audiobook folder, new defaults, audio audit). Read CHANGELOG-rekkod.md section 1.2.4 first.
- Worktrees /home/claude/w7-{queue,stream,ya,ui,audio} on branches r125-*. Temp dirs /home/claude/tmp-w7{queue,stream,ya,ui,audio}.
  Emulator displays: queue :71, stream :72, ya :73, ui :74, audio :75.
- Five agents run at once on 2 CPUs: ALWAYS build with -j1. First `make host -j1` is a full build (build_host absent) — start it early in the background
  and read code meanwhile. build_target is prebuilt for the base (touched), so `make target -j1 ROCKBOX_TOOLCHAIN_DIR=/home/claude/sonix/sonix-player/rockbox-toolchain` is incremental.
- File ownership to avoid conflicts (edit others' files only minimally, and say so in the report):
  queue → playback queue/player/trackmenu/nowplaying menus/buttons mapping; stream → streaming common layer, provider.h, *sync.c of qobuz/tidal/subsonic, new downloads page;
  ya → yandex*.c/yandexpage.c; ui → shared widgets (header, pills, list rows), nowplaying status line, scan/index-version, .nomedia; audio → audio.c, resample, decimate, dsd, usb audio, input polling, cover.c, sonixlink.
- Language files: add your keys in a block right after the `; --- file.c` comment of the file they belong to; for a NEW file add a new `; --- newfile.c` section at the END of the file. All 11 languages, R1 and R3PII.
- The user (Vladimir) is away while you work; decide conservatively and document.

## 1.3.0 notes (UI 2.0 + Yandex)
- Base: sonix-r1 at d7400c6 (1.2.5). Worktrees /home/claude/w8-*; build_host and build_target are prebuilt for the base (touched), so `make host -j2` / `make target -j2 ROCKBOX_TOOLCHAIN_DIR=/home/claude/sonix/sonix-player/rockbox-toolchain` are incremental.
- Resources for the emulator: <worktree>/sonix-player/usr/resource is already copied. Run the binary with cwd = a dir containing usr/ (see /home/claude/tmp-int125/run.sh, mocks.sh, shot.sh for a working setup with Yandex + Subsonic mocks and fault proxies; copy to your own temp dir and change display/ports/names).
- A demo card with good tags and generated covers: /home/claude/tmp-ds/card (copy it). Metaflac needs LC_ALL=C.UTF-8 --no-utf8-convert for Cyrillic.
- The Sonix UI 2.0 design: /home/claude/ui2 (prototype "Sonix UI 2.0.dc.html", renders with shoot.py; screenshots in /home/claude/ui2/shots/d*.png) and the spec /home/claude/ui2/SPEC.md once written.
- Disk is limited (~16 GB free): don't copy lvgl/build dirs more than needed; delete your temp dirs' large files when done.
- THE DESIGN HANDOFF (source of truth): /home/claude/ui2/handoff/README.md (principles, gesture matrix, queue model, screens, components, animations, cover pipeline, icon mapping), /home/claude/ui2/handoff/sonix_ui_tokens.h (all sizes/timings/thresholds/colours incl. OKLCH tone palette), /home/claude/ui2/handoff/prototype/ (same as /home/claude/ui2/"Sonix UI 2.0.dc.html"). Rendered dark screenshots: /home/claude/ui2/shots/d*.png (shoot.py renders it; React is served locally from /home/claude/ui2/vendor).
- Owner's instruction: keep every current function working (nothing silently dropped) and fix small glitches you meet along the way (list them in the report).
- Phase 1 agents: core (w8-core, display :92), cover (w8-cover, :93), ya (w8-ya, :91). Phase 2 agents will build screens on core's API.
- PHASE 2: base sonix-r1 f298cb7 (Phase 1 merged). Read /home/claude/ui2/PHASE1_API.md first. Worktrees /home/claude/w9-{lists,player,svc,sys}; displays lists :101, player :102, svc :103, sys :104; temp dirs /home/claude/tmp-w9*. Emulator resources: copy usr/ from the worktree into a run dir (the integration run setup: /home/claude/tmp-ds/{run.sh,mocks.sh,shot.sh,root/} — copy and rename binary/display/ports; mock ports must differ per agent: lists 5101/5102, player 5201/5202, svc 5301/5302 (+fault proxies +1), sys 5401/5402 — edit device_config.ini base_url/server accordingly).

## 1.3.0 wave 3 (after beta2)
- Base: sonix-r1 at the merge of UI 2.0 phases 1–2 (beta2 shipped from it). Worktrees /home/claude/w10-{qa,perf,zvuk}; displays qa :111, perf :112, zvuk :113; temp dirs /home/claude/tmp-w10*; mock ports qa 6101.., perf 6201.., zvuk 6301...
- What beta2 contains: read CHANGELOG-rekkod.md (1.2.x) + /home/claude/ui2/PHASE1_API.md + `git log --oneline d7400c6..HEAD` (the UI 2.0 commits are well described). Two queue-undo APIs exist (playlist_queue_paths_undoable/playlist_queue_undo from lists, queueundo.h from player); covertone listeners must go through covertonefan_add (covertone_watch forwards to it).
- Owner's direction: keep going in waves — polish, performance, UI smoothness, battery; keep every function; fix small glitches. Beta testers on 4pda will use it.
