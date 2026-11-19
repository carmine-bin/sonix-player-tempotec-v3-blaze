#!/usr/bin/env python3
"""
Rasterises the SVG icons in assets/icons/ into a C source file LVGL can draw
(src/gui/shell/icons.c + icons.h).

The generated file is checked into the repo, so building the player does NOT
need Python, cairosvg or any of this. Re-run it only when an icon changes or a
new one is added:

    pip install cairosvg pillow
    python3 tools/svg_to_lvgl.py

For a panel that is not 480 px wide, --width W draws every icon at the size
ui_px() gives (src/gui/shell/uiscale.h, same rounding) and --out DIR writes
icons.c/icons.h there instead; the Makefile does this when PANEL_WIDTH is set:

    python3 tools/svg_to_lvgl.py --width 320 --out build_gen

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
OUT_C = os.path.join(REPO_ROOT, "src", "gui", "shell", "icons.c")
OUT_H = os.path.join(REPO_ROOT, "src", "gui", "shell", "icons.h")

# (svg file, C identifier, pixel size).
#
# LVGL does not scale these: the size here is the size on the 480x720 panel, so
# the same drawing needed at two sizes appears twice under two names.
#
# The recurring sizes: 26-30 px for a status bar or a row, 34 px for a corner
# button, 46 px inside the control centre's 88 px circles, 56 px for a dialog
# glyph, 96-160 px for a placeholder standing in for missing artwork.
ICONS = [
    ("chevron-left.svg", "chevron_left", 36),
    ("chevron-right.svg", "chevron_right", 36),
    # Marks the chosen row in a single-choice list, in place of the chevron.
    ("check.svg", "check", 36),
    ("close.svg", "close", 36),
    ("x.svg", "clear", 30),                 # clears the search field
    ("eye.svg", "eye", 30),                 # shows the password in clear
    ("eye-off.svg", "eye_off", 30),
    ("folder.svg", "folder", 32),
    # The Music page corner button when Playlists has taken the tile.
    ("folder.svg", "folder_corner", 34),
    ("folder-root.svg", "folder_root", 34), # browser corner: back to the card root
    ("file.svg", "file", 32),
    ("music-settings.svg", "music_settings", 34),
    ("book-finished.svg", "book_finished", 34),  # the Audiobooks page's corner: the finished books
    ("audio-waveform.svg", "audio_waveform", 34),
    # AutoEq search corner: download the headphone index again.
    ("autoeq-update.svg", "autoeq_update", 34),
    ("repeat.svg", "repeat_all", 30),
    ("repeat-1.svg", "repeat_one", 30),
    ("repeat-off.svg", "repeat_off", 30),
    ("play.svg", "play", 40),
    ("pause.svg", "pause", 40),
    # A live stream has no position to come back to, so on a radio the same
    # button stops. Smaller than play/pause: a solid square of the same side
    # reads heavier than either of them.
    ("stop.svg", "stop", 34),
    # The status bar's playback indicator, from its own outlined glyphs: the
    # player's filled play/pause are too heavy at bar size.
    ("play-status.svg", "play_status", 26),
    ("pause-status.svg", "pause_status", 26),
    # The same place when a phone is playing rather than the player. It
    # replaces play/pause instead of joining them, and shares their size.
    ("airplay-status.svg", "airplay_status", 26),
    ("sonixlink-status.svg", "sonixlink_status", 26),
    # Detect changes, beside play/pause: looking over the card, then done.
    ("database-search.svg", "library_checking", 26),
    ("database-check.svg", "library_checked", 26),
    ("skip-back.svg", "skip_back", 34),
    ("skip-forward.svg", "skip_forward", 34),
    # The control centre's transport, a size up from the player's: the sheet is
    # reached by feel over whatever was on screen.
    ("play.svg", "play_large", 48),
    ("pause.svg", "pause_large", 48),
    ("skip-back.svg", "skip_back_large", 42),
    ("skip-forward.svg", "skip_forward_large", 42),
    ("music-note.svg", "music_note", 128),
    # Placeholders that swap in for the music note in the same box: a radio
    # station, an AirPlay sender and a DLNA renderer are not tracks.
    ("radio-player.svg", "radio_player", 128),
    ("airplay-page-icon.svg", "airplay_page", 128),
    ("dlna-page-icon.svg", "dlna_page", 128),
    # Status bar. The charging shell and its bolt are separate bitmaps so the
    # bolt can be yellow while the shell stays white.
    ("battery.svg", "battery", 38),
    ("battery-charging-body.svg", "battery_charging_body", 38),
    ("battery-charging-bolt.svg", "battery_charging_bolt", 38),
    # The same cell for the ebook reader's footer strip, where the text is 16
    # and the status bar's cell would be twice the height of its line.
    ("battery.svg", "battery_small", 26),
    # Volume: three states of the same glyph, picked by level.
    ("volume.svg", "volume_mute", 30),
    ("headphones.svg", "headphones", 26), # jack indicator beside the volume
    ("usb-audio-out.svg", "usbaudioout", 26), # and when the sound leaves over USB-C
    ("lineout.svg", "lineout_status", 26), # and while line out holds the jack
    ("volume-1.svg", "volume_low", 30),
    ("volume-2.svg", "volume_high", 30),
    # In place of whichever of the three is showing while the volume lock is on.
    ("volume-lock.svg", "volume_locked", 30),
    # Power menu.
    ("power-off.svg", "power_off", 56),
    ("reboot.svg", "reboot", 56),
    # The low battery warning and the countdown to the shutdown at 1 %.
    ("battery-warning.svg", "battery_warning", 56),
    # Library pages. One glyph per index, and the stand-in for artwork that is
    # not there: they ride inside the 72 px thumbnail box on a library row and
    # the 56 px one on a search hit.
    ("music-2.svg", "music2", 32),
    ("artist.svg", "artist", 32),
    ("podcast-list.svg", "podcast_list", 40), # 40 px for the 60 px podcast rows
    ("artist-album.svg", "artist_album", 32),
    ("genre.svg", "genre", 32),
    ("album.svg", "album", 32),
    # Corner buttons on the index pages: the sort direction, and the menu of
    # ways to start an artist.
    ("list-a-z.svg", "sort_az", 34),
    ("list-z-a.svg", "sort_za", 34),
    # The same button when All tracks or Albums runs by when the files arrived:
    # the arrow points the way the list reads, as it does on A-Z and Z-A.
    ("calendar-arrow-up.svg", "sort_date_old", 34),
    ("calendar-arrow-down.svg", "sort_date_new", 34),
    # By release year, the same size: arrow down while the years go up, arrow
    # up while they come down.
    ("year-arrow-down.svg", "sort_year_asc", 34),
    ("year-arrow-up.svg", "sort_year_desc", 34),
    # The audiobook lists also run by when a book was last listened to, and a
    # series by the number of each book in it.
    ("history.svg", "sort_played", 34),
    ("list-ordered.svg", "sort_series", 34),
    ("album.svg", "album_corner", 34),  # built, and currently unused
    # Cover Flow draws this in a 210 px square where a cover is missing.
    ("album.svg", "album_big", 96),
    ("circle-play.svg", "circle_play", 34),
    ("ellipsis-vertical.svg", "ellipsis_vertical", 30),
    ("list-music.svg", "list_music", 34),
    # Selection mode on the library lists and the file browser: its corner
    # buttons that add the chosen rows to a playlist, to the favourites and to
    # the queue, and inside a playlist or the favourites take them out.
    ("list-plus.svg", "list_plus", 34),
    ("star-plus.svg", "star_plus", 34),
    ("list-queue.svg", "list_queue", 34),
    ("list-x.svg", "list_x", 34),
    ("star-x.svg", "star_x", 34),
    ("search.svg", "search", 34),
    ("wifi.svg", "wifi", 46),
    ("bluetooth.svg", "bluetooth", 46),
    # Status bar radios. The wifi glyph is the same arc with none, one, two or
    # three bars, picked by signal strength. Both are drawn at reduced opacity
    # while the radio is on but connected to nothing.
    ("wifi-zero.svg", "wifi_zero", 34),
    ("wifi-low.svg", "wifi_low", 34),
    ("wifi-high.svg", "wifi_high", 34),
    ("wifi-max.svg", "wifi_max", 34),
    ("bluetooth-status.svg", "bluetooth_status", 30),
    # The corner buttons of the Wi-Fi and Bluetooth pages: the same arc and
    # rune with the search motion added, and the receiver, which is waves
    # arriving rather than a rune because there the sound comes in.
    ("wifi-search.svg", "wifi_search", 36),
    ("bluetooth-search.svg", "bluetooth_search", 36),
    ("bluetooth-receiver.svg", "bluetooth_receiver", 36),
    # The receiver page corner button: the codec the phone is sending on.
    ("change_codec_receiver.svg", "change_codec", 34),
    # Dialog glyphs: a pairing in flight, and "the volume is over there".
    ("bluetooth-connecting.svg", "bluetooth_connecting", 56),
    ("headphones.svg", "headphones_big", 56),
    ("sun.svg", "sun", 30),
    # Keyboard. Shift and caps lock share one key, so they share a size.
    ("shift.svg", "shift", 30),
    ("caps-lock.svg", "caps_lock", 30),
    ("delete.svg", "delete", 30),
    ("space.svg", "space", 34),
    ("chevron-up.svg", "chevron_up", 32), # built, and currently unused
    # The Music page's "more below", shown for a moment when the page opens.
    ("chevron-down.svg", "chevron_down", 36),
    # The control centre's close hint, drawn the way iOS draws its home
    # indicator. Big because the glyph's line spans only the middle 14 of its
    # 24 units, so 56 px of bitmap is a ~33 px bar.
    ("control-center-line.svg", "control_center_line", 56),
    # The control centre's round toggles.
    ("mseb.svg", "mseb", 46),
    ("equalizer.svg", "equalizer", 46),
    ("lineout.svg", "lineout", 46),
    ("fade-track.svg", "fade_track", 46),
    ("low-gain.svg", "low_gain", 46),
    ("mid-gain.svg", "mid_gain", 46),
    ("high-gain.svg", "high_gain", 46),
    # AirPlay and SonixLink carry the same drawing as their status bar glyph at
    # circle size: the toggle and the symbol it raises must read as one thing.
    ("airplay-status.svg", "airplay_quick", 46),
    ("peq.svg", "peq_quick", 46),
    ("sonixlink-status.svg", "sonixlink_quick", 46),
    # DLNA's menu tile is a coloured square, so the circle carries the line
    # drawing of the logo instead.
    ("dlna-quick.svg", "dlna_quick", 46),
    # Wi-Fi transfer: a folder with the two arrows of a sync, since the tile's
    # own drawing is a coloured square.
    ("wifi-transfer-quick.svg", "wifi_transfer_quick", 46),
    # One sleep timer per kind of listening: there are three of them, and
    # stopping one from the control centre must not stop another.
    ("sleep-timer-music.svg", "sleep_music_quick", 46),
    ("sleep-timer-audiobook.svg", "sleep_audiobook_quick", 46),
    ("sleep-timer-podcast.svg", "sleep_podcast_quick", 46),
    # Gapless: two drawings and not one recoloured, because what changes is
    # the gap itself -- the two halves apart while off, closed up while on.
    ("gapless-on-quick.svg", "gapless_on_quick", 46),
    ("gapless-off-quick.svg", "gapless_off_quick", 46),
    # The volume lock: the status bar's own drawing at circle size, like AirPlay.
    ("volume-lock.svg", "volume_lock_quick", 46),
    # Direct: straight through, nothing on the way.
    ("direct-quick.svg", "direct_quick", 46),
    # The scheduled shutdown: the power glyph with a stopwatch's crown.
    ("scheduled-off-quick.svg", "scheduled_off_quick", 46),
    # File types, in the file manager and on the web page: one glyph per
    # family, at the size of the folder beside them.
    ("files-music.svg", "files_music", 32),
    ("files-audiobook.svg", "files_audiobook", 32),
    ("files-playlist.svg", "files_playlist", 32),
    # On an import row whose name a playlist already has: importing it
    # replaces that playlist.
    ("playlist-overwrite-alert.svg", "playlist_overwrite_alert", 32),
    ("files-image.svg", "files_image", 32),
    ("files-text.svg", "files_text", 32),
    ("files-update.svg", "files_update", 32),
    ("files-gamepad.svg", "files_game", 32),
    ("files-book.svg", "files_book", 32),
    # The bin in a file's action menu. Not icon_delete, which despite the name
    # is the keyboard's backspace arrow.
    ("trash.svg", "trash", 34),
    ("folder-new.svg", "folder_new", 34),
    # The grip at the right of a draggable row, in the keyboard layout page.
    ("grip-horizontal.svg", "grip", 36),
    ("move-vertical.svg", "reorder", 34), # turns on reordering in a playlist
    ("now-playing.svg", "now_playing", 32), # the playing row in the queue
    # The queue's corner, after a long press on a row: choose rows to take out.
    # Moving them by hand is arrow_down_up, next to it.
    ("circle-dashed-check.svg", "select_mode", 34),
    ("plus.svg", "plus", 36),
    ("minus.svg", "minus", 36),             # the metronome: one beat per minute slower
    ("import.svg", "import", 34), # brings an .m3u from the card into the player's folder
    ("reset.svg", "reset", 36),             # MSEB / equalizer: back to flat
    ("circle-check.svg", "circle_check", 64), # the confirmation popup
    ("circle-alert.svg", "circle_alert", 64), # ...and what a failure shows instead
    ("book-headphones.svg", "book_headphones", 128),
    ("book-headphones.svg", "book_headphones_row", 64), # audiobook rows with no cover
    # Music and Audiobooks with no card in the slot, or the card at a computer.
    ("sd-card.svg", "sd_card", 128),
    # Podcasts on the card: the badge on a cover (copied in, or downloaded by
    # Sonix), the line without a network, the empty folder, the OPML files.
    ("sd-card.svg", "sd_card_badge", 16),
    ("arrow-down-to-line.svg", "download_badge", 16),
    # Downloading the chosen episodes, on the disc of a podcast's page.
    ("arrow-down-to-line.svg", "download", 34),
    ("wifi-off.svg", "wifi_off", 30),
    ("folder-open.svg", "folder_open", 80),
    ("file-down.svg", "file_down", 34),
    # An episode's state in its row, and the play and pause buttons of the podcast pages.
    ("check.svg", "check_small", 22),
    ("play.svg", "play_small", 26),
    ("pause.svg", "pause_small", 26),
    # The same glyph the podcast rows use, at the size the three the player
    # stands in with are drawn: an episode with no artwork gets this one.
    ("podcast-list.svg", "podcast_cover", 128),
    # Player extras. Shuffle and shuffle-repeat share a button.
    ("shuffle.svg", "shuffle", 30),
    ("shuffle-repeat.svg", "shuffle_repeat", 30),
    # The favourites page's reverse-order button. 34 px and not shuffle's 30:
    # it does the same job as list-a-z on the all-tracks page, and two buttons
    # that sort a list should weigh the same.
    ("arrow-down-up.svg", "arrow_down_up", 34),
    ("star.svg", "star", 32),
    ("star-filled.svg", "star_filled", 32),
    ("star.svg", "star_corner", 34),
    # Over the lyrics, beside the star or the ellipsis: look the words up again
    # on LRCLIB, to replace them. The star's size, since the two sit together.
    ("lyrics-search.svg", "lyrics_search", 32),
    ("radio-recent.svg", "radio_recent", 34), # stations played most recently
    ("custom-radio.svg", "radio_custom", 34), # the stations written into radio.txt
    # Under a station's name: the stream's quality, one glyph in four colours
    # that follow the theme, so white like the rest of this list and tinted by
    # the page (radiopage.c). The size of the track rows' quality badges.
    ("radio-quality.svg", "radio_quality", 26),
    ("refresh.svg", "refresh", 34), # the Processes page's refresh
    # Audiobook transport: the four jumps the two buttons can be set to, plus
    # the chapter list. Which pair is on screen follows Settings -> Audiobooks
    # -> Change controls, so all of them are built.
    ("prev_10.svg", "prev_10", 50),
    ("next_10.svg", "next_10", 50),
    ("prev_30.svg", "prev_30", 50),
    ("next_30.svg", "next_30", 50),
    ("prev_60.svg", "prev_60", 50),
    ("next_60.svg", "next_60", 50),
    ("chapter.svg", "chapter", 34),
    # The same place on a podcast, where the queue is the episode list.
    ("podcast-episodes.svg", "podcast_episodes", 34),
    # The playback-speed gauge, in the repeat button's place while a book plays.
    ("play-speed.svg", "play_speed", 34),
    # The DAC page's charging toggle.
    ("zap.svg", "zap", 34),
    ("zap-off.svg", "zap_off", 34),
    # Corner buttons: Qobuz quality and sign-out, the remap page's side swap,
    # and Gearboy's two save states.
    ("qobuz-quality.svg", "qobuz_quality", 34),
    ("log-out.svg", "log_out", 34),
    ("swap-remap.svg", "swap_remap", 34),
    ("save-state.svg", "save_state", 34),
    ("load-state.svg", "load_state", 34),
    # The turning wheel, at two sizes: beside a row it is a detail, in the
    # middle of the screen it is the only thing being looked at.
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
    # headphones say tells which one is in the room, so the wireless one is an
    # option the user sets and both have to be here.
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

    # The EPUB reader. The first three are the bar that opens on a long press
    # in the middle of the page, at 44 px because they are finger targets. The
    # three after are the page-turn styles in the theme settings, beside a name
    # at the size of a line of text.
    ("chapter.svg", "ebook_chapter", 44),
    ("font-settings.svg", "ebook_font", 44),
    ("book-theme.svg", "ebook_theme", 44),
    ("fast-turn.svg", "ebook_turn_fast", 34),
    ("scroll-turn.svg", "ebook_turn_slide", 34),
    ("vertical-turn.svg", "ebook_turn_vertical", 34),
    # The shelf's placeholder, in a 300 px tall tile and beside a row.
    ("ebook-cover.svg", "ebook_cover", 96),
    ("ebook-cover.svg", "ebook_cover_small", 40),
    # Bookmarks: the Books page's corner button, and the glyph in the popup
    # that confirms a save.
    ("bookmark.svg", "bookmark", 36),
    ("bookmark-check.svg", "bookmark_check", 64),
    # The heart that jumps out when the coffee QR code is closed. 160 px
    # because the animation grows it to 1:1 and no further.
    ("heart.svg", "heart", 160),
]

# Main menu and section tiles, which keep their own colours and are therefore
# listed separately from the recolourable glyphs above. Stored at exactly the
# size they are drawn at: LVGL does not scale them, so a larger bitmap would be
# weight in the binary and nothing else.
MAIN_MENU_ICON_SIZE = 128
SECTION_ICON_SIZE = 112

COLOR_ICONS = [
    # The quality badges under a track's title. Here rather than above because
    # the four are one shape in four colours -- teal, olive, amber, magenta --
    # and the white rendering would make them one icon repeated.
    ("lossy-quality.svg", "quality_lossy", 26),
    ("cd-quality.svg", "quality_cd", 26),
    ("hifi-quality.svg", "quality_hifi", 26),
    ("dsd-quality.svg", "quality_dsd", 26),

    # The three pages where the player stops being a player.
    ("dac-icon-page.png", "dac_page", 200),
    ("sonixlink-icon-page.png", "sonixlink_page", 200),
    ("bluetooth-receiver-icon-page.png", "bluetooth_receiver_page", 200),

    ("music.png", "menu_music", MAIN_MENU_ICON_SIZE),
    # The Musica section's own grid.
    ("all.png", "menu_all", SECTION_ICON_SIZE),
    ("album.png", "menu_album", SECTION_ICON_SIZE),
    ("artist.png", "menu_artist", SECTION_ICON_SIZE),
    ("album_artist.png", "menu_album_artist", SECTION_ICON_SIZE),
    ("genre.png", "menu_genre", SECTION_ICON_SIZE),
    ("year.png", "menu_year", SECTION_ICON_SIZE),
    ("favourites.png", "menu_favourites", SECTION_ICON_SIZE),
    ("most-played.png", "menu_most_played", SECTION_ICON_SIZE),
    ("explorer.png", "menu_explorer", SECTION_ICON_SIZE),
    ("playlist.png", "menu_playlist", SECTION_ICON_SIZE),
    ("streaming.png", "menu_streaming", MAIN_MENU_ICON_SIZE),
    ("wireless.png", "menu_wireless", MAIN_MENU_ICON_SIZE),
    # The Wireless section's own grid.
    ("wifi-settings.png", "menu_wifi_settings", SECTION_ICON_SIZE),
    ("bluetooth.png", "menu_bluetooth", SECTION_ICON_SIZE),
    ("airplay.png", "menu_airplay", SECTION_ICON_SIZE),
    ("wifi-transfer.png", "menu_wifi_transfer", SECTION_ICON_SIZE),
    ("sonixlink.png", "menu_sonixlink", SECTION_ICON_SIZE),
    ("dlna.png", "menu_dlna", SECTION_ICON_SIZE),
    # The Streaming section's own grid.
    ("tidal.png", "menu_tidal", SECTION_ICON_SIZE),
    ("qobuz.png", "menu_qobuz", SECTION_ICON_SIZE),
    ("radio.png", "menu_radio", SECTION_ICON_SIZE),
    ("podcast.png", "menu_podcast", SECTION_ICON_SIZE),
    ("audiobooks.png", "menu_audiobooks", MAIN_MENU_ICON_SIZE),
    ("audiobook-library.png", "menu_audiobook_library", SECTION_ICON_SIZE),
    ("audiobook-series.png", "menu_audiobook_series", SECTION_ICON_SIZE),
    ("audiobook-author.png", "menu_audiobook_author", SECTION_ICON_SIZE),
    ("audiobook-continue.png", "menu_audiobook_continue", SECTION_ICON_SIZE),
    ("settings.png", "menu_settings", MAIN_MENU_ICON_SIZE),
    ("more.png", "menu_more", MAIN_MENU_ICON_SIZE),
    # What lives inside "More", at section size.
    ("dac.png", "menu_dac", SECTION_ICON_SIZE),
    ("gearboy.png", "menu_gearboy", SECTION_ICON_SIZE),
    ("flappy-bird.png", "menu_flappy_bird", SECTION_ICON_SIZE),
    ("ebook.png", "menu_books", SECTION_ICON_SIZE),
    ("file-explorer.png", "menu_file_explorer", SECTION_ICON_SIZE),
    ("metronome.png", "menu_metronome", SECTION_ICON_SIZE),

    # The service badges in the player's top right corner, where the radio puts
    # its LIVE mark. They sit over the cover art, so these are the outlined
    # versions rather than the menu ones, which would vanish on a dark cover.
    # One size for all three: they replace each other in the same place.
    ("qobuz-badge.png", "qobuz_badge", 52),
    ("tidal-badge.png", "tidal_badge", 52),
    ("podcast-badge.png", "podcast_badge", 52),
]


def render(svg_path, size):
    """SVG -> list of BGRA bytes, forced to white so recolouring works.

    `size` is a side in pixels for the square glyphs, or an explicit
    (width, height) for the AirPods artwork, which is not square.
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


def ui_px(n, width):
    """uiscale.h's ui_px() for a positive size."""
    return (n * width + 240) // 480


def main():
    args = sys.argv[1:]
    width = int(args[args.index("--width") + 1]) if "--width" in args else 480
    out_c, out_h = OUT_C, OUT_H
    if "--out" in args:
        out_dir = args[args.index("--out") + 1]
        out_c, out_h = os.path.join(out_dir, "icons.c"), os.path.join(out_dir, "icons.h")

    def scaled(size):
        return tuple(ui_px(n, width) for n in size) if isinstance(size, tuple) else ui_px(size, width)

    parts_c = []
    parts_h = []

    entries = [(f, n, scaled(s), False) for f, n, s in ICONS]
    entries += [(f, n, scaled(size), True) for f, n, size in COLOR_ICONS]

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

    with open(out_c, "w") as f:
        f.write(header)
        f.write('#include "icons.h"\n\n#include <stdint.h>\n\n')
        f.write("\n".join(parts_c))

    with open(out_h, "w") as f:
        f.write(header)
        f.write("#ifndef ICONS_H\n#define ICONS_H\n\n")
        f.write('#include "lvgl/lvgl.h"\n\n')
        f.write("\n".join(parts_h))
        f.write("\n\n#endif // ICONS_H\n")

    print("wrote %s and %s" % (out_c, out_h))


if __name__ == "__main__":
    main()
