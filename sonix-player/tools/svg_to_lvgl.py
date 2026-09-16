#!/usr/bin/env python3
"""
Rasterises the SVG icons in assets/icons/ into a C source file LVGL can draw
(src/gui/shell/icons.c + icons.h).

The generated file is checked into the repo, so building the player does NOT
need Python, cairosvg or any of this. Re-run it only when an icon changes or a
new one is added:

    pip install cairosvg pillow
    python3 tools/svg_to_lvgl.py

Two kinds of icon come out of this:

  * SVGs are emitted as ARGB8888 *white* shapes. The colour comes from LVGL at
    draw time via `lv_obj_set_style_image_recolor()`, so the same bitmap serves
    an active and an inactive button.
  * PNGs are emitted with their own colours intact, for artwork that is not a
    single-colour glyph -- the main menu tiles.
"""

import os
import sys

try:
    import cairosvg
    from PIL import Image
except ImportError:
    sys.exit("needs cairosvg and pillow: pip install cairosvg pillow")

import io

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICON_DIR = os.path.join(REPO_ROOT, "assets", "icons")
# src/gui/shell/ e non src/gui/: le icone sono passate li' quando src e' stato
# diviso per funzione, e questo percorso e' rimasto indietro -- rigenerare
# scriveva un file nuovo accanto a quello vero, che nessuno compila.
OUT_C = os.path.join(REPO_ROOT, "src", "gui", "shell", "icons.c")
OUT_H = os.path.join(REPO_ROOT, "src", "gui", "shell", "icons.h")

# (svg file, C identifier, pixel size). Sizes are chosen for the R3 Pro II's
# 480x720 panel; they are drawn 1:1, LVGL does not scale them.
ICONS = [
    ("chevron-left.svg", "chevron_left", 36),
    ("chevron-right.svg", "chevron_right", 36), # list rows: same size as the back chevron
    # The tick that marks the chosen row in a single-choice list (the language
    # page). Same size as the chevron it stands in place of, so rows that have
    # one and rows that do not line up.
    ("check.svg", "check", 36),
    ("close.svg", "close", 36),
    ("x.svg", "clear", 30),                 # clears the search field
    ("eye.svg", "eye", 30),                 # mostra la password in chiaro
    ("eye-off.svg", "eye_off", 30),         # e la rinasconde -- stessa misura della x
    ("folder.svg", "folder", 32),
    # Il pulsante in alto a destra nel browser: riporta alla radice della
    # scheda. Misura da pulsante d'angolo (56 px di bottone), come le altre
    # icone che stanno li'.
    ("folder-root.svg", "folder_root", 34),
    ("file.svg", "file", 32),
    ("music-settings.svg", "music_settings", 34),
    ("repeat.svg", "repeat_all", 30),
    ("repeat-1.svg", "repeat_one", 30),
    ("repeat-off.svg", "repeat_off", 30),
    ("play.svg", "play", 40),
    ("pause.svg", "pause", 40),
    # A live stream cannot be paused -- there is no position to come back
    # to -- so on a radio the same button stops instead, and says so.
    # Drawn a touch smaller than play/pause: a solid square of the same
    # side reads heavier than either of them.
    ("stop.svg", "stop", 34),
    # The status bar's playback indicator, drawn at bar size from its own
    # outlined glyphs (the player's filled play/pause are too heavy up there).
    ("play-status.svg", "play_status", 26),
    ("pause-status.svg", "pause_status", 26),
    # Lo stesso posto, quando a suonare non e' il player ma un telefono: si
    # sostituisce a play/pause invece di aggiungersi, perche' quello che sta
    # uscendo dal DAC e' una cosa sola e il simbolo che la nomina deve essere
    # uno solo. Stessa misura degli altri due, se no il cambio si vede come
    # uno scatto della barra.
    ("airplay-status.svg", "airplay_status", 26),
    # Il logo di SonixLink, subito a destra di play/pausa: c'e' quando un
    # telefono e' collegato. Stessa misura degli altri glifi della barra.
    ("sonixlink-status.svg", "sonixlink_status", 26),
    ("skip-back.svg", "skip_back", 34),
    ("skip-forward.svg", "skip_forward", 34),
    # The control centre's transport, a size up from the player's: it is
    # reached by feel, with the sheet half-covering whatever was on screen, so
    # the targets there want to be bigger than the ones on a page being looked
    # at directly.
    ("play.svg", "play_large", 48),
    ("pause.svg", "pause_large", 48),
    ("skip-back.svg", "skip_back_large", 42),
    ("skip-forward.svg", "skip_forward_large", 42),
    ("music-note.svg", "music_note", 128),
    # The same placeholder for a radio station with no artwork: a note
    # would say "track", and a live stream is not one. Drawn at the music
    # note's size so the two swap in place, in the player and in the
    # screensaver alike.
    ("radio-player.svg", "radio_player", 128),
    # The AirPlay page's placeholder, where the artwork goes before a sender
    # has sent any. Lucide's own airplay glyph, drawn at the same size as the
    # player's music note, which is the other picture-shaped placeholder.
    ("airplay-page-icon.svg", "airplay_page", 128),
    # Lo stesso, per la pagina DLNA: il glifo "cast" di Lucide, alla stessa
    # misura, nel posto dove AirPlay mette il suo.
    ("dlna-page-icon.svg", "dlna_page", 128),
    # Status bar. The charging shell and its bolt are separate bitmaps so the
    # bolt can be yellow while the shell stays white.
    ("battery.svg", "battery", 38),
    ("battery-charging-body.svg", "battery_charging_body", 38),
    ("battery-charging-bolt.svg", "battery_charging_bolt", 38),
    # La stessa pila, piccola, per la striscia in fondo alla pagina di un libro.
    # Li' il testo e' a 16 e non a 24, e la pila della barra di stato accanto
    # sarebbe alta il doppio della riga su cui sta.
    ("battery.svg", "battery_small", 26),
    # Volume: three states of the same glyph, picked by level.
    ("volume.svg", "volume_mute", 30),
    ("headphones.svg", "headphones", 26), # jack indicator beside the volume
    # ...e quella che prende il suo posto quando il suono esce dalla porta
    # USB-C: la porta stessa, non il connettore ottico che il round precedente
    # aveva scambiato per la funzione giusta.
    ("usb-audio-out.svg", "usbaudioout", 26),
    ("volume-1.svg", "volume_low", 30),
    ("volume-2.svg", "volume_high", 30),
    # Power menu.
    ("power-off.svg", "power_off", 56),
    ("reboot.svg", "reboot", 56),
    # Library pages.
    ("music-2.svg", "music2", 32),
    # One glyph per index, so a list of names says what kind of names they
    # are without reading the title: a folder for everything was accurate and
    # told you nothing. Same 32 px as the folder they replace -- these ride
    # inside the 72 px thumbnail box on a library row and the 56 px one on a
    # search hit, in both cases as the stand-in for artwork that is not there.
    ("artist.svg", "artist", 32),
    # Il microfono che tiene il posto della copertina nelle liste dei podcast:
    # una nota musicale li' diceva la cosa sbagliata. E' l'icona mandata
    # dall'utente (Lucide "mic"), resa a 40 px per stare nella casella da
    # 60 px delle righe come fanno le altre icone segnaposto.
    ("podcast-list.svg", "podcast_list", 40),
    ("artist-album.svg", "artist_album", 32),
    ("genre.svg", "genre", 32),
    ("album.svg", "album", 32),
    # The corner buttons on the index pages: the sort direction, and the
    # "group this artist's tracks by album" switch.
    ("list-a-z.svg", "sort_az", 34),
    ("list-z-a.svg", "sort_za", 34),
    # Left in place, and left unused since the artist pages traded the album
    # grouping switch for the circle-play menu below.
    ("album.svg", "album_corner", 34),
    # Lo stesso disco alla misura che serve in Cover Flow, dove sta in mezzo a
    # un quadrato da 210 px al posto della copertina che manca. Prima veniva
    # ingrandito tre volte partendo da 32 px: un disegno di 96 px reso a 96 px
    # non ha i gradini che aveva quello.
    ("album.svg", "album_big", 96),
    # Il pannello laterale di Now Playing: il quadrante che porta ai VU-meter,
    # e il disco che riporta alla copertina. Stessa misura tutti e due, perche'
    # nel pannello si scambiano di posto.
    # The corner button that replaced it: one tap opens a small menu of ways to
    # start this artist, instead of toggling one setting.
    ("circle-play.svg", "circle_play", 34),
    ("ellipsis-vertical.svg", "ellipsis_vertical", 30),
    ("list-music.svg", "list_music", 34),
    ("search.svg", "search", 34),           # Musica corner + search page
    ("wifi.svg", "wifi", 46),               # quick panel: inside the 88 px circles
    ("bluetooth.svg", "bluetooth", 46),
    # Status bar radios. The wifi glyph is the same arc drawn with none, one,
    # two or three bars, picked by signal strength; bluetooth has the one rune.
    # Both are drawn at reduced opacity while their radio is on but not
    # connected to anything, so "acceso" and "collegato" read apart without
    # needing a second colour.
    ("wifi-zero.svg", "wifi_zero", 34),
    ("wifi-low.svg", "wifi_low", 34),
    ("wifi-high.svg", "wifi_high", 34),
    ("wifi-max.svg", "wifi_max", 34),
    ("bluetooth-status.svg", "bluetooth_status", 30),
    # The two "look for something" glyphs, in the corner of the Wi-Fi and
    # Bluetooth pages: the same arc and rune with the search motion added.
    ("wifi-search.svg", "wifi_search", 36),
    ("bluetooth-search.svg", "bluetooth_search", 36),
    # Il Ricevitore Bluetooth, accanto agli altri due nell'angolo della pagina
    # Bluetooth. Non e' una rune ma le onde che le arrivano addosso: qui il
    # suono entra invece di uscire, ed e' l'unica cosa che distingue questa
    # pagina da tutto il resto della sezione.
    ("bluetooth-receiver.svg", "bluetooth_receiver", 36),
    # The two glyphs the modal popups are built around, at the size a dialog
    # wants rather than a status bar's: the rune with its two link marks while
    # a pairing is in flight, and the headphones for "the volume is over
    # there, not here".
    ("bluetooth-connecting.svg", "bluetooth_connecting", 56),
    ("headphones.svg", "headphones_big", 56),
    ("sun.svg", "sun", 30),
    ("shift.svg", "shift", 30),           # keyboard
    # Il caps lock: stessa misura dello shift, perche' i due si scambiano
    # sullo stesso tasto (doppio tocco veloce dello shift).
    ("caps-lock.svg", "caps_lock", 30),
    ("delete.svg", "delete", 30),
    ("space.svg", "space", 34),
    ("chevron-up.svg", "chevron_up", 32), # (unused: the control centre's hint
                                          #  is the iOS-style line below)
    # The control centre's close hint: a single short bar, the way iOS draws
    # its home indicator. Big, because the glyph's line only spans the middle
    # 14 of its 24 units -- 56 px of bitmap is a ~33 px bar.
    ("control-center-line.svg", "control_center_line", 56),
    ("mseb.svg", "mseb", 46),               # control centre quick toggles
    ("equalizer.svg", "equalizer", 46),
    # Line out, in the same row of round toggles: a cable, because that is what
    # the mode is for -- the player feeding an amplifier instead of a pair of
    # headphones.
    ("lineout.svg", "lineout", 46),
    ("fade-track.svg", "fade_track", 46),
    ("low-gain.svg", "low_gain", 46),
    ("high-gain.svg", "high_gain", 46),
    # AirPlay fra i comandi rapidi, alla misura dei cerchi da 88. Lo stesso
    # disegno della barra di stato a un'altra misura, non un secondo disegno:
    # il tondo che lo accende e il simbolo che compare in alto devono essere
    # riconoscibilmente la stessa cosa.
    ("airplay-status.svg", "airplay_quick", 46),
    # Il parametrico fra i comandi rapidi, alla misura degli altri tondi.
    ("peq.svg", "peq_quick", 46),
    # Lo stesso ragionamento per SonixLink: il tondo che lo accende porta il
    # logo che compare nella barra di stato quando un telefono si collega.
    ("sonixlink-status.svg", "sonixlink_quick", 46),
    # E DLNA, che fra i tondi ci e' arrivato dopo. Qui il disegno e' un altro
    # dalla tessera del menu, che e' una piastrella colorata e ingrandita
    # sarebbe una piastrella finita nel posto sbagliato: questo e' il logo DLNA
    # a tratto, come gli altri tondi.
    ("dlna-quick.svg", "dlna_quick", 46),
    # I tre timer sonno fra i tondi: uno per tipo di ascolto, perche' i timer
    # sono tre e spegnerne uno dal control center non deve poter spegnere
    # quello di qualcun altro. Stessa misura degli altri tondi.
    ("sleep-timer-music.svg", "sleep_music_quick", 46),
    ("sleep-timer-audiobook.svg", "sleep_audiobook_quick", 46),
    ("sleep-timer-podcast.svg", "sleep_podcast_quick", 46),
    # I tipi di file di Gestione file e della pagina web: un glifo per famiglia,
    # alla misura della cartella che gli sta accanto nella stessa riga.
    ("files-music.svg", "files_music", 32),
    ("files-audiobook.svg", "files_audiobook", 32),
    ("files-playlist.svg", "files_playlist", 32),
    ("files-image.svg", "files_image", 32),
    ("files-text.svg", "files_text", 32),
    ("files-update.svg", "files_update", 32),
    ("files-gamepad.svg", "files_game", 32),
    ("files-book.svg", "files_book", 32),
    # Il cestino delle azioni su un file. Non icon_delete, che malgrado il nome
    # e' la freccia del tasto cancella della tastiera.
    ("trash.svg", "trash", 34),
    # Il tasto d'angolo di Gestione file. Misura d'angolo come gli altri.
    ("folder-new.svg", "folder_new", 34),
    # L'appiglio a destra di una riga che si trascina: la pagina della lingua
    # della tastiera lo usa per spostare un layout fra "In uso" e "Altri".
    ("grip-horizontal.svg", "grip", 36),
    # Il tasto d'angolo che accende il riordino nei brani di una playlist.
    # Misura d'angolo come circle-play, che gli sta subito a destra.
    ("move-vertical.svg", "reorder", 34),
    ("now-playing.svg", "now_playing", 32), # the playing row in the queue
    ("plus.svg", "plus", 36),               # "Nuova playlist"
    # The playlists page's corner button: bringing an .m3u dropped on the card
    # into the player's own folder. Corner-button size, like the rest of them.
    ("import.svg", "import", 34),
    ("reset.svg", "reset", 36),             # MSEB / equalizer: back to flat
    ("circle-check.svg", "circle_check", 64), # the "brano aggiunto" confirmation
    ("circle-alert.svg", "circle_alert", 64), # ...and what a failure shows instead
    ("book-headphones.svg", "book_headphones", 128),
    # The same book at thumbnail size, for the rows of the Audiolibri list
    # whose file carries no cover art.
    ("book-headphones.svg", "book_headphones_row", 64),
    # Player extras.
    ("shuffle.svg", "shuffle", 30),
    # Shuffle that comes round again: the mode after shuffle in the player's
    # cycle. Same size, since the two share a button.
    ("shuffle-repeat.svg", "shuffle_repeat", 30),
    # Preferiti: l'inverti-ordine, tasto d'angolo. Non e' la misura dello
    # shuffle che gli sta di fianco ma quella di list-a-z, che e' il tasto
    # d'angolo che fa la stessa identica cosa nella pagina Tutti i brani: due
    # tasti che ordinano una lista devono pesare uguale, e a 30 px questo si
    # vedeva piu' piccolo dell'altro.
    ("arrow-down-up.svg", "arrow_down_up", 34),
    ("star.svg", "star", 32),
    ("star-filled.svg", "star_filled", 32),
    ("star.svg", "star_corner", 34), # corner button size, beside list-music
    # The Radio page's own corner button: a clock turning back, for the
    # stations played most recently. Same 34 px as the star it sits beside.
    ("radio-recent.svg", "radio_recent", 34),
    # E quello accanto alla ricerca: le stazioni scritte a mano in radio.txt.
    # Stessa misura degli altri tasti d'angolo della stessa riga.
    ("custom-radio.svg", "radio_custom", 34),
    # L'aggiorna-a-comando della pagina Processi: stesso 34 px degli altri
    # tasti d'angolo.
    ("refresh.svg", "refresh", 34),

    # The DAC page: the charging toggle in the corner, and its own big glyph.
    # Audiobook transport: the four jumps the two buttons can be set to, and
    # the chapter list. Which pair is on screen follows Impostazioni ->
    # Audiolibri -> Cambia controlli, so all four are always built.
    ("prev_10.svg", "prev_10", 50),
    ("next_10.svg", "next_10", 50),
    ("prev_30.svg", "prev_30", 50),
    ("next_30.svg", "next_30", 50),
    ("prev_60.svg", "prev_60", 50),
    ("next_60.svg", "next_60", 50),
    ("chapter.svg", "chapter", 34),
    # Lo stesso posto, quando a suonare e' un podcast: apre la coda, che su un
    # podcast E' l'elenco degli episodi. Stessi 34 px del glifo dei capitoli
    # che sostituisce, cosi' passare da un libro a un podcast non fa ballare la
    # riga dei tasti.
    ("podcast-episodes.svg", "podcast_episodes", 34),
    # The playback-speed gauge, in the repeat button's place while a book is
    # loaded. Same 34 px as the repeat glyph it stands in for.
    ("play-speed.svg", "play_speed", 34),

    ("zap.svg", "zap", 34),
    ("zap-off.svg", "zap_off", 34),

    # I due tasti d'angolo della pagina Qobuz: la qualita' audio e l'uscita
    # dall'account. Stessi 34 px degli altri tasti d'angolo, cosi' stanno in
    # riga con l'ingranaggio di Musica.
    ("qobuz-quality.svg", "qobuz_quality", 34),
    ("log-out.svg", "log_out", 34),
    # Lo scambio fra i due fianchi nella pagina di rimappatura: stessi 34 px
    # degli altri tasti d'angolo.
    ("swap-remap.svg", "swap_remap", 34),
    # I due stati salvati di Gearboy, sul bollo tondo in alto a destra.
    ("save-state.svg", "save_state", 34),
    ("load-state.svg", "load_state", 34),
    # La rotella che gira: le liste che stanno cercando, e Qobuz che carica.
    # Due misure, perche' accanto a una riga e' un dettaglio e in mezzo allo
    # schermo e' la sola cosa che si guarda.
    ("loader-circle.svg", "loader_small", 26),
    ("loader-circle.svg", "loader_big", 56),
    # --- AirPods ---------------------------------------------------------
    #
    # Apple's own artwork, and the only icons here that are not square: an
    # earbud is twice as tall as it is wide and a charging case is wider than
    # it is tall, so every one of these carries its own (width, height), worked
    # out from the SVG's viewBox at the height wanted. Rendering them square
    # would squash them into something that is not an AirPod.
    #
    # Two sizes, because they are used in two places. The small ones sit at the
    # right of the AirPods row in the Bluetooth audio settings, where the whole
    # pair has to read at a glance in the height of a line of text; the taller
    # ones head the three columns of the battery page, above a ring and a
    # percentage that need room of their own.
    ("airpods.svg", "airpods_hero", (31, 30)),
    ("airpods-gen3.svg", "airpods_gen3_hero", (42, 30)),
    ("airpods-gen4.svg", "airpods_gen4_hero", (40, 30)),
    ("airpods-pro.svg", "airpods_pro_hero", (45, 30)),
    ("airpods-max.svg", "airpods_max_hero", (28, 30)),

    ("airpod-left.svg", "airpod_left", (32, 72)),
    ("airpod-right.svg", "airpod_right", (32, 72)),
    ("airpod-gen3-left.svg", "airpod_gen3_left", (45, 72)),
    ("airpod-gen3-right.svg", "airpod_gen3_right", (45, 72)),
    ("airpods-gen4-left.svg", "airpod_gen4_left", (43, 72)),
    ("airpods-gen4-right.svg", "airpod_gen4_right", (43, 72)),
    ("airpods-pro-left.svg", "airpod_pro_left", (55, 72)),
    ("airpods-pro-right.svg", "airpod_pro_right", (55, 72)),

    # The first two generations came with either case, and nothing the
    # headphones say tells us which one is in the room -- so the wireless one
    # is an option the user sets, and both have to be here.
    ("airpods-chargingcase-fill.svg", "airpods_case", (59, 72)),
    ("airpods-chargingcase-wireless-fill.svg", "airpods_case_wireless", (59, 72)),
    ("airpods-gen3-chargingcase-wireless-fill.svg", "airpods_gen3_case", (85, 72)),
    ("airpods-gen4-chargingcase-wireless-fill.svg", "airpods_gen4_case", (78, 72)),
    ("airpods-pro-chargingcase-wireless-fill.svg", "airpods_pro_case", (94, 72)),

    # The Max are one headset with one battery and no case, so their big icon
    # stands alone in the middle of the page.
    ("airpods-max.svg", "airpods_max_big", (66, 72)),

    # The battery ring's middle, and the four noise-control modes on the tab
    # bar under the pictures. Lucide glyphs, square like every other icon here.
    ("zap.svg", "airpods_charge", 20),
    ("noise-control-off.svg", "airpods_noise_off", 32),
    ("noise-cancellation.svg", "airpods_anc", 32),
    ("transparency.svg", "airpods_transparency", 32),
    ("adaptive.svg", "airpods_adaptive", 32),

    # Il lettore di EPUB. Le prime tre sono la barra che si apre tenendo premuto
    # al centro della pagina: 44 px perche' sono bersagli da dito e non decori.
    # Le tre dopo sono i modi di girare pagina, dentro le impostazioni del tema,
    # e stanno accanto a un nome: 34 px, la misura di una riga di testo.
    # chapter.svg e' gia' sopra a 34 px per la lista dei capitoli degli
    # audiolibri; qui serve alla misura degli altri due della barra, e una barra
    # con un'icona piu' piccola delle sue vicine si vede.
    ("chapter.svg", "ebook_chapter", 44),
    ("font-settings.svg", "ebook_font", 44),
    ("book-theme.svg", "ebook_theme", 44),
    ("fast-turn.svg", "ebook_turn_fast", 34),
    ("scroll-turn.svg", "ebook_turn_slide", 34),
    ("vertical-turn.svg", "ebook_turn_vertical", 34),
    # Il segnaposto delle caselle dello scaffale finche' la copertina non
    # arriva, e per i libri che non ne hanno una: grande, perche' riempie una
    # casella alta trecento pixel.
    ("ebook-cover.svg", "ebook_cover", 96),
    # Lo stesso segnaposto accanto a una riga invece che dentro una casella
    # della griglia. Una seconda misura e non lo stesso disegno rimpicciolito:
    # LVGL non scala le icone, quindi quello da 96 px in una casella da 56x84
    # veniva semplicemente tagliato.
    ("ebook-cover.svg", "ebook_cover_small", 40),
    # I segnalibri. Il primo e' il pulsante nell'angolo della pagina Libri,
    # alla misura degli altri pulsanti d'angolo; il secondo sta dentro il
    # pop-up che conferma il salvataggio, alla misura dei glifi dei pop-up.
    ("bookmark.svg", "bookmark", 36),
    ("bookmark-check.svg", "bookmark_check", 64),
    # Il cuore che salta fuori quando si chiude il codice QR del caffe'. Grande
    # perche' l'animazione lo ingrandisce fino a 1:1 e non oltre: LVGL disegna
    # le icone alla loro misura, quindi questa e' la misura a cui si vede.
    ("heart.svg", "heart", 160),
]

# Main menu tiles. These keep their own colours, so they are listed separately
# from the recolourable glyphs above. 96 px is what the 2x3 grid draws them at;
# they are stored at exactly that size because LVGL does not scale them and a
# larger bitmap would be pure weight in the binary.
# Two tile sizes: the main menu's six tiles grew to 128 px; the Musica
# section's own grid keeps the original 112 px.
MAIN_MENU_ICON_SIZE = 128
SECTION_ICON_SIZE = 112

COLOR_ICONS = [
    # The quality badges under a track's title, when Musica > Opzioni di
    # visualizzazione asks for them. Small: they sit under the title and are a
    # glance, not a label. Here rather than above because the four are the same
    # shape in four colours -- teal, olive, amber, magenta -- and the white
    # rendering above would make them one icon repeated.
    ("lossy-quality.svg", "quality_lossy", 26),
    ("cd-quality.svg", "quality_cd", 26),
    ("hifi-quality.svg", "quality_hifi", 26),
    ("dsd-quality.svg", "quality_dsd", 26),

    ("dac-icon-page.png", "dac_page", 200),
    ("sonixlink-icon-page.png", "sonixlink_page", 200),
    # Stessa misura e stessa costruzione delle altre due immagini grandi: la
    # pagina del Ricevitore Bluetooth ha la stessa forma di quella del DAC,
    # cioe' un modo in cui il lettore smette di essere un lettore.
    ("bluetooth-receiver-icon-page.png", "bluetooth_receiver_page", 200),
    ("music.png", "menu_music", MAIN_MENU_ICON_SIZE),
    # The Musica section's own grid.
    ("all.png", "menu_all", SECTION_ICON_SIZE),
    ("album.png", "menu_album", SECTION_ICON_SIZE),
    ("artist.png", "menu_artist", SECTION_ICON_SIZE),
    ("album_artist.png", "menu_album_artist", SECTION_ICON_SIZE),
    ("genre.png", "menu_genre", SECTION_ICON_SIZE),
    ("explorer.png", "menu_explorer", SECTION_ICON_SIZE),
    ("streaming.png", "menu_streaming", MAIN_MENU_ICON_SIZE),
    ("wireless.png", "menu_wireless", MAIN_MENU_ICON_SIZE),
    # The Wireless section's own grid, same size as the Musica one.
    ("wifi-settings.png", "menu_wifi_settings", SECTION_ICON_SIZE),
    ("bluetooth.png", "menu_bluetooth", SECTION_ICON_SIZE),
    ("airplay.png", "menu_airplay", SECTION_ICON_SIZE),
    ("wifi-transfer.png", "menu_wifi_transfer", SECTION_ICON_SIZE),
    ("sonixlink.png", "menu_sonixlink", SECTION_ICON_SIZE),
    ("dlna.png", "menu_dlna", SECTION_ICON_SIZE),
    # The Streaming section's own grid, same size as the others.
    ("tidal.png", "menu_tidal", SECTION_ICON_SIZE),
    ("qobuz.png", "menu_qobuz", SECTION_ICON_SIZE),
    ("radio.png", "menu_radio", SECTION_ICON_SIZE),
    ("podcast.png", "menu_podcast", SECTION_ICON_SIZE),
    ("audiobooks.png", "menu_audiobooks", MAIN_MENU_ICON_SIZE),
    ("settings.png", "menu_settings", MAIN_MENU_ICON_SIZE),
    # "Altro": la casella del menu principale che era del DAC. Stessa
    # costruzione delle altre -- due quadrati arrotondati, uno ruotato dietro e
    # uno smerigliato davanti, con la sfumatura di un colore solo -- e i tre
    # puntini che ovunque vogliono dire "c'e' dell'altro qui sotto".
    ("more.png", "menu_more", MAIN_MENU_ICON_SIZE),
    # Il DAC adesso e' una voce DENTRO Altro, quindi gli serve anche la misura
    # delle griglie di sezione, non solo quella del menu principale.
    ("dac.png", "menu_dac", SECTION_ICON_SIZE),
    # L'emulatore Game Boy, dentro "Altro" come il DAC.
    ("gearboy.png", "menu_gearboy", SECTION_ICON_SIZE),
    # I libri, terza voce di "Altro": stessa costruzione e stessa misura delle
    # altre caselle di sezione.
    ("ebook.png", "menu_books", SECTION_ICON_SIZE),
    # Il gestore file, quarta voce di "Altro". Per ora la casella c'e' e basta:
    # e' disegnata spenta finche' non ha una pagina dove portare.
    ("file-explorer.png", "menu_file_explorer", SECTION_ICON_SIZE),

    # Il marchio Qobuz in alto a destra del player, dove la radio mette
    # "DIRETTA". Sta SOPRA la copertina, cioe' sopra qualunque colore: per
    # questo e' quello con il contorno bianco e non quello del menu, che su una
    # copertina scura sparirebbe. Colori suoi, niente ricolorazione.
    ("qobuz-badge.png", "qobuz_badge", 52),

    # E quello di Tidal, stessa misura e stesso angolo: i due marchi si
    # sostituiscono nello stesso posto, quindi devono occupare lo stesso
    # spazio -- se no cambiare servizio sposta la copertina sotto.
    ("tidal-badge.png", "tidal_badge", 52),

    # E quello dei podcast, che sta nello stesso angolo degli altri due.
    ("podcast-badge.png", "podcast_badge", 52),

]


def render(svg_path, size):
    """SVG -> list of BGRA bytes, forced to white so recolouring works.

    `size` is a side in pixels for the square glyphs, which is what almost
    every icon here is. The AirPods artwork is not square -- an earbud is
    twice as tall as it is wide, a charging case is wider than it is tall --
    so those entries give an explicit (width, height) instead, and stretching
    them into a square would be the difference between an AirPod and a bean.
    """
    width, height = size if isinstance(size, tuple) else (size, size)
    png = cairosvg.svg2png(url=svg_path, output_width=width, output_height=height)
    img = Image.open(io.BytesIO(png)).convert("RGBA")

    raw = img.tobytes()  # RGBA, 4 bytes per pixel
    out = bytearray()
    for i in range(0, len(raw), 4):
        # LVGL's ARGB8888 is stored blue, green, red, alpha in memory.
        out += bytes((255, 255, 255, raw[i + 3]))
    return bytes(out), img.width, img.height


def render_color(path, size):
    """PNG or SVG -> list of BGRA bytes with the original colours kept."""
    if path.lower().endswith(".svg"):
        png = cairosvg.svg2png(url=path, output_width=size, output_height=size)
        img = Image.open(io.BytesIO(png)).convert("RGBA")
    else:
        img = Image.open(path).convert("RGBA")
    if img.size != (size, size):
        img = img.resize((size, size), Image.LANCZOS)

    raw = img.tobytes()  # RGBA, 4 bytes per pixel
    out = bytearray()
    for i in range(0, len(raw), 4):
        # LVGL's ARGB8888 is stored blue, green, red, alpha in memory.
        out += bytes((raw[i + 2], raw[i + 1], raw[i], raw[i + 3]))
    return bytes(out), img.width, img.height


def c_array(data, per_line=12):
    lines = []
    for i in range(0, len(data), per_line):
        chunk = ", ".join("0x%02x" % b for b in data[i:i + per_line])
        lines.append("\t" + chunk + ",")
    return "\n".join(lines)


def main():
    parts_c = []
    parts_h = []

    entries = [(f, n, s, False) for f, n, s in ICONS]
    entries += [(f, n, size, True) for f, n, size in COLOR_ICONS]

    for filename, name, size, keep_colour in entries:
        path = os.path.join(ICON_DIR, filename)
        if not os.path.exists(path):
            sys.exit("missing icon: %s" % path)

        data, w, h = render_color(path, size) if keep_colour else render(path, size)

        parts_c.append(
            "// %s, %dx%d\n"
            "static const uint8_t icon_%s_data[] = {\n%s\n};\n\n"
            "const lv_image_dsc_t icon_%s = {\n"
            "\t.header = {\n"
            "\t\t.magic = LV_IMAGE_HEADER_MAGIC,\n"
            "\t\t.cf = LV_COLOR_FORMAT_ARGB8888,\n"
            "\t\t.w = %d,\n"
            "\t\t.h = %d,\n"
            "\t\t.stride = %d,\n"
            "\t},\n"
            "\t.data_size = sizeof(icon_%s_data),\n"
            "\t.data = icon_%s_data,\n"
            "};\n"
            % (filename, w, h, name, c_array(data), name, w, h, w * 4, name, name)
        )
        parts_h.append("extern const lv_image_dsc_t icon_%s;" % name)

    header = (
        "/*\n"
        " * GENERATED FILE -- do not edit by hand.\n"
        " * Produced from the SVGs in assets/icons by tools/svg_to_lvgl.py.\n"
        " *\n"
        " * White ARGB8888 bitmaps: tint them with lv_obj_set_style_image_recolor().\n"
        " */\n\n"
    )

    with open(OUT_C, "w") as f:
        f.write(header)
        f.write('#include "icons.h"\n\n#include <stdint.h>\n\n')
        f.write("\n".join(parts_c))

    with open(OUT_H, "w") as f:
        f.write(header)
        f.write("#ifndef ICONS_H\n#define ICONS_H\n\n")
        f.write('#include "lvgl/lvgl.h"\n\n')
        f.write("\n".join(parts_h))
        f.write("\n\n#endif // ICONS_H\n")

    print("wrote %s and %s" % (OUT_C, OUT_H))


if __name__ == "__main__":
    main()
