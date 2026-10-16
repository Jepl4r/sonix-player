# Music sync: rsync (PC -> player) + lsyncd (optional live sync)

One-way music sync from a computer to the player over Wi-Fi. The player
runs an `rsync` daemon (port 873, LAN only, no encryption); the computer
pushes with `rsync`, optionally continuously via `lsyncd`.

## What's in the firmware (R1)

- `usr/bin/rsync` — static MIPS build, no runtime dependencies.
- `etc/rsyncd.conf` — one module, `[music]`, rooted at the SD card
  (`/mnt/sd_0`), password taken from `/usr/data/rsyncd.secrets`.
- `etc/init.d/S51rsyncd` — starts the daemon at boot only when enabled.
- Developer options -> **Rsync** toggle — off by default. It writes
  `/usr/data/rsyncd.enabled` (what the init script checks) and remembers
  the choice across reboots.

## Player setup (once)

1. Connect the player to Wi-Fi and note its IP (Wi-Fi settings).
2. Developer options -> ADB on, then from the computer:

   ```bash
   adb shell "echo 'music:YOURPASSWORD' > /usr/data/rsyncd.secrets && chmod 600 /usr/data/rsyncd.secrets"
   ```

   One line, `music:` + password (letters/numbers, no `:`). Mode **must**
   be 600 — the daemon refuses looser permissions and the client just
   reports `auth failed`.
3. Developer options -> **Rsync** on. Check it's alive:

   ```bash
   adb shell "ps | grep -i rsync | grep -v grep"
   ```

4. Keep the player **awake** during syncs: standby powers the Wi-Fi radio
   down, which surfaces on the computer as `No route to host`. Raise the
   screen-off timeout or disable standby while syncing.

## Pushing music from the computer

List the card (password via environment):

```bash
RSYNC_PASSWORD=YOURPASSWORD rsync rsync://music@<PLAYER_IP>/music/
```

Push a library into the `Music` folder (trailing slashes matter):

```bash
RSYNC_PASSWORD=YOURPASSWORD rsync -avh --progress \
  --no-owner --no-group --modify-window=1 \
  ~/Music/ rsync://music@<PLAYER_IP>/music/Music/
```

- `--no-owner --no-group`: the card (FAT/exFAT) has no owners; without
  these every file errors `chown ... Operation not permitted`.
- `--modify-window=1`: FAT timestamps are coarse; without it every file
  looks changed on each run and incremental syncs re-transfer everything.
- Always target `.../music/Music/` (or another subfolder) when using
  `--delete`. The module root is the whole card (index, covers,
  playlists live there too); `--delete` at the root wipes those.
- After pushing, rescan the library on the player.

A second run with nothing changed should move only kilobytes — that's
your proof the delta works.

## Ignore list

Keep a filter file on the computer (it can live anywhere *outside* the
music folder, or self-exclude if inside it):

```
.Trash-*/
.Spotlight-V100/
*.tmp
*.part
/Podcasts/
```

Use it with `--exclude-from=/path/to/excludes.txt`. Excluded paths are
also protected from `--delete` (unless `--delete-excluded` is passed).

## Continuous sync with lsyncd (computer side)

`lsyncd` watches the library with inotify and pushes seconds after each
change. It runs on the **computer**, never on the player.

1. Install: `sudo apt install lsyncd` / `sudo dnf install lsyncd`.
2. Store the password in a 600 file (the daemon won't inherit your shell
   variables):

   ```bash
   printf '%s' 'YOURPASSWORD' > ~/.config/sonix-rsync-pass
   chmod 600 ~/.config/sonix-rsync-pass
   ```

3. `~/.config/sonix-lsyncd.lua`:

   ```lua
   settings { logfile = "/tmp/lsyncd-sonix.log", statusFile = "/tmp/lsyncd-sonix.status" }
   sync { default.rsync,
     source = "/home/YOU/Music/",
     target = "rsync://music@<PLAYER_IP>/music/Music/",
     rsync = { archive = true, compress = false,
       _extra = { "--delete",
         "--no-owner", "--no-group", "--modify-window=1",
         "--password-file=/home/YOU/.config/sonix-rsync-pass",
         "--exclude-from=/home/YOU/.config/sonix-excludes.txt" } } }
   ```

   (`compress = false`: music is already compressed; it only costs CPU.)
4. Test in the foreground first:

   ```bash
   lsyncd -nodaemon ~/.config/sonix-lsyncd.lua
   ```

   (Ctrl+C stops it), then daemonize: `lsyncd ~/.config/sonix-lsyncd.lua`.
5. Exactly one instance: check with `ps aux | grep '[l]syncd'` (count
   must be 1); stop extras with `pkill -x lsyncd`. Every start launches a
   new daemon — never start a second.
6. The player's IP is DHCP: re-check it each session or give the player a
   router-side address reservation.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `auth failed on module music` | Wrong password, or secrets file not mode 600 (`ls -l /usr/data/rsyncd.secrets`), or trailing whitespace/CR in it (`cat -A` should show one `music:...$` line). |
| `No route to host` / `Connection reset` | Player asleep (radio off in standby) or IP changed. Wake it, verify Wi-Fi + IP. |
| `chown ... Operation not permitted` | Missing `--no-owner --no-group`. |
| `mkdir ... failed: Invalid argument` | Folder name with trailing dots/spaces — illegal on FAT/exFAT. Rename it on the computer. |
| Exit code 23 | See previous errors; usually one of the above. |
| Daemon not running after toggle | `/etc/init.d/S51rsyncd start` prints the exact missing piece (flag/conf/secrets). |
| `lsyncd` never fires | Source on a FUSE/network mount without inotify events, or no daemon running. Check `/tmp/lsyncd-sonix.log`. |

## Steady routine

Syncs: player awake + Wi-Fi + Rsync on -> push (or leave `lsyncd`
running) -> rescan -> Rsync off, standby back on. The daemon listens on
every network the player joins while enabled, so don't leave it on.

## For developers (rebuilding this)

- `rsync` 3.2.7, cross-built static with the rockbox toolchain
  (`-march=mips32r2 -mplt`, `--with-included-popt --with-included-zlib`,
  `--disable-zstd --disable-lz4 --disable-xxhash --disable-openssl
  --disable-md2man`), ABI-gated like the player itself.
- Player side mirrors the ADB toggle: `src/system/device/rsyncd.c/.h`,
  the row in `src/gui/settings/devoptions.c`, `rsyncd_apply_saved_state()`
  in `src/main.c`, keys in the overlay `English.ini`.
- Ships via the `assets/R1/` overlay; no packer script changes.
