# Sonix Player 1.3.0 (UI 2.0) — QA pass after beta2

Branch `r131-qa` (worktree /home/claude/w10-qa), base 2cf4f4a (beta2). 19 commits, one fix each
(the first 14 by the first QA run, the rest by the resumed run). Screenshots: /home/claude/tmp-w10qa/shots
(`NN_*.png` R1 Russian, `r3_*.png` R3 Pro II German, `v*.png` resumed run).

Host build `make host -j2`: no warnings. Target `make target -j2`: "check-abi: no symbol beyond
glibc 2.22". Translation files: all 22 (11 languages × R1/R3PII) have the same key set, and every
`tr("…")` key in the sources exists in all of them.

## What was tested

Emulator (Xvfb :111, isolated `/tmp/.local`, mocks on 6101–6108: Subsonic + fault proxy, Yandex + fault
proxy, radio, Qobuz, Tidal, Podcast Index). Panels R1 480×800 and R3 Pro II 480×720; dark and light;
Russian, German, French (first start), English; accents: cover, yellow, orange; demo card
(/home/claude/tmp-ds/card copy) and a synthetic 3 000-track card (`card_big`, Cyrillic/umlaut names,
albums without sleeves, 3 s tracks for fast queue movement).

- First start: welcome → language → date/time → home; scan from the empty Music page; idle "Shuffle all
  music" card on Home/Music; restart keeps the paused last track in the mini player.
- Home, Music tiles, All tracks (A–Z strip + bubble, every sort incl. "by artist", gesture hint),
  Albums, album page (gradient, edge back, play/shuffle/⋮, track sheet), Artists, Genres, Years, Search
  (big library, pills, clear), Files, Playlists, More.
- Gesture matrix: row → Play next / ← Add to end with undo toast; queue ← remove (not the current) +
  undo; hold → sheet (library, Qobuz, Yandex, Wi-Fi, Bluetooth rows); ☆ in rows; left-edge back on every
  page incl. the album page; mini player tap / ← next / → previous / ↑ player; player cover tap = pause,
  ← / → skip, ↓ collapse; sheets drag-closed; toast undo, dismiss, and its place after the player
  collapses; control centre open (status-bar tap) / close (swipe up); volume bar (hardware keys).
- Player: Next pill, chain expand, queue position after Play next, ⋮ sheet (stop after track, sleep timer
  15/30/60/end of track with ☾ and toast), star with toast, queue page (groups, ⋮: save, shuffle rest,
  clear next).
- Streaming: Radio (genres, stations, live in mini player, ☆, swipe), Subsonic (albums, play, fault
  proxy errors), Yandex (My Wave, likes, swipes, Play next during the Wave, restart), Qobuz and Tidal
  (favourites, swipes, sheet), Podcasts (trending, feed, episode play stays on page, multi-select +
  download), Downloads page.
- Audiobooks: empty state, scan (starts at once, toast), shelf, tap to play.
- Settings: Appearance (theme, clock, accents, tint, large rows), Music (scan options, scan with progress
  card and Stop, folders, playback), language switch at run time (Russian → German, no restart),
  Date and time from Settings, Wi-Fi and Bluetooth pages with `SONIX_WIFI_FAKE` / `SONIX_BT_FAKE`
  (connect, sheets, rename with keyboard, codec/LDAC pills, "More").
- Seams named in the brief: two queue-undo APIs, two "Shuffle all" implementations, podcast
  `show_player()`, toast wrap/height, status-bar glyph, scan buttons' contrast, album-page mini-player
  band, podcast rows.

No crash, hang or raw translation key was seen.

## Broken and fixed

| Commit | Fix |
|---|---|
| 786c589 | Toast: a short name no longer wraps beside "Отменить" (one line, "…" when long); a toast shown over the player follows it down to the mini player when the player collapses, instead of hanging mid-page. |
| 8f5ae89 | Queue undo unified: every addition (lists, services, player sheets) goes through `queueundo.h` (built on `playlist_queue_paths_undoable/undo`); a list's Undo could refuse ("queue has changed") where the player's worked. Services' additions update "Up next" at once; Play next on the playing track says "Already playing"; podcasts' Play next follows the 2.0 queue with toast + Undo. |
| f369cae | Player showed a wrong queue position after Play next (6/6 while 2nd); mini player kept the previous stream's sleeve when the next out-of-library track had none. |
| 001df35 | Yandex My Wave: Play next / Add to end / Undo on Wave tracks made the wave report the playing track as skipped and stop fetching (ran dry a few tracks later). |
| 55afb35 | "Shuffle all music": one implementation (`listacts_shuffle_all`) for the idle cards and the queue page. |
| 8a8b3b9 | Podcasts: tapping the playing episode no longer jumps to the player (only "Now listening" opens it); dates in the genitive ("9 октября") and lengths in the UI language. |
| e7790fb | Last accent buttons with white text on pale accents (podcast download FAB, OPML, playlists Import, Files Move here, Tidal Retry) take the accent's ink. |
| e2ee650 | Album page: no darker band behind the mini player. |
| 7dd1ab6 | Album page: left-edge swipe goes back (the floating ‹ had disabled it). |
| 1ee4924 | First start in Russian: Home tile "Ещё" was relabelled "Дополнительно" (and wrapped) by the by-value key lookup. |
| 3625879 | All tracks: "By artist" sort, as in the handoff. |
| 8f65ab7 | Sheets: "Play/Слушать" used the big filled player triangle; now the outline glyph like the other lines. |
| fd9b5fa | Tiles: a caption that becomes two lines after the language is chosen (German "Album-Interpreten" on R3) is no longer cut. |
| 57ddb48 | Albums without any sleeve get a plaque/page in their title's hue (as the handoff says) instead of grey; a short album says "1 min", never "0 min". |
| 8b23fde | Rename/name dialogs (Bluetooth, EQ/MSEB/PEQ presets, playlists, folders, artist exceptions) took the mini player away for good (keyboard in a hidden layer counted as shown). Hold on paired headphones also connected them, hold on a saved network joined it, hold on the current network opened its sheet twice (lift arrived as a tap). |
| 645c6aa | Status bar: showed ▶ while playing and ❚❚ while paused — the opposite of the mini player's button under it. Now ❚❚ while playing, nothing when paused/idle (handoff `sbL`). AirPlay glyph and BT blue unchanged. |
| e371f6f | Audiobooks: tapping a book opened the full player; now it plays in the mini player and the shelf stays, the book marked as playing (handoff: "Радио, книга — тап: играть"). Bookmarks still open the player. |
| 65ac900 | Date and time (first start on R1, also from Settings): "Confirm" was below the fold at the end of the scroller; now pinned under the scrolling cards like the language page. |
| 606e3d9 | Light theme + yellow (also green/orange) preset: accent used as text (playing row title, "· 24/96") was ~2:1 on white; text-only accent is now darkened to ≥ 3:1. Fills/pills/icons unchanged. |

Checked, no change needed: scan page Stop button and progress card contrast (dark + light);
libraryscan has only that one button. Static check of long-press handlers: quick panel, bookmarks,
medialist, streaming tiles, podcasts already swallow the lift.

## Remaining (not fixed)

| Severity | Issue |
|---|---|
| Medium (design) | Podcast episode rows are still the 1.2.5-style custom rows: no → Play next / ← Add to end swipes, no "playing" plaque. Hold is multi-select and ⋮ is the episode menu, so moving them to `sx_row` (hold → sheet) needs a decision on where multi-select goes (e.g. a "Select" line in the sheet, as the library track sheet has). |
| Low | Status-bar change (645c6aa) follows the handoff but changes 1.2.5 behaviour; worth a line in the release notes. |
| Low | Mini player plaque is grey for a track whose album has no sleeve, while its list row shows the title hue (screens stay neutral for such tracks on purpose). |
| Low | Audiobook files without tags show the file name ("01.mp3") in the player and mini player and an empty artist line; a one-file book shows its tag title on the shelf with the folder as subtitle. |
| Low | A scan started from Settings → Music → Scan options ends on the Music page, not back on Scan options. |
| Low | Dark theme: the scan/progress card's bar track (press colour on surface) is faint. |
| Low | Qobuz/Tidal track sheet: "Show album" uses a chevron glyph instead of a disc. |
| Low | Search runs on submit only (no results while typing); results don't show which field matched. Probably intended for CPU. |
| Info | Target-only `-Wformat-truncation` warnings in search.c:203 and medialist.c:3351 (beta2 code, intentional truncation); host build is clean. |
| Info | Needs the device: status-bar glyph with real AirPlay/Bluetooth, hold timing on the real touch panel, contrast of the darkened yellow outdoors. |

## Известные проблемы (для бета-тестеров)

- Строки эпизодов подкастов пока старого вида: нет свайпов «Следующим / В конец очереди» и подсветки играющего эпизода. Меню — через ⋮, выбор нескольких — удержанием.
- В строке состояния значок ❚❚ теперь означает «играет»; на паузе значка нет (как в мини-плеере).
- Книги и треки без тегов показываются в плеере по имени файла («01.mp3»), строка исполнителя пустая.
- У трека из альбома без обложки мини-плеер показывает серую плашку, хотя в списке плашка цветная.
- Сканирование, запущенное из «Настройки → Музыка → Параметры сканирования», заканчивается на странице «Музыка».
- Поиск ищет после нажатия кнопки поиска на клавиатуре, а не по мере ввода.
