#include "player.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "src/gui/library/browser.h"
#include "src/gui/library/audiobookextras.h"
#include "src/gui/nowplaying/chapters.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/nowplaying/meterview.h"
#include "src/gui/nowplaying/lyricsearch.h"
#include "src/gui/library/medialist.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/doubletap.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/quickpanel.h"
#include "src/gui/shell/scrolltext.h"
#include "src/gui/nowplaying/trackmenu.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/topbar.h"
#include "src/system/device/power.h"
#include "src/system/playback/sleeptimer.h"
#include "src/system/gearboy/gearboy.h"
#include "src/system/streaming/radio.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/readahead.h"
#include "src/system/playback/audiobook.h"
#include "src/system/playback/device_state.h"
#include "src/system/remote/airplay.h"
#include "src/system/remote/dlna.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/library/lrclib.h"
#include "src/system/library/lyrics.h"
#include "src/system/device/led.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcastsubs.h"
#include "src/system/streaming/podcaststate.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/qobuzsync.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/streaming/tidalsync.h"
#include "src/system/core/utils.h"
#include "src/system/core/frameprof.h"
#include "src/system/audio/waveform.h"
#include "src/system/net/wifi.h"

#include "lvgl/src/core/lv_obj_draw_private.h"
#include "lvgl/src/core/lv_obj_event_private.h"
#include "lvgl/src/font/lv_font_private.h"
#include "lvgl/src/misc/lv_text_private.h"
#include "lvgl/src/widgets/slider/lv_slider_private.h"
#include "lvgl/src/misc/lv_text_ap.h"
#include "src/gui/shell/unchanged.h"
#include "lvgl/lvgl.h"
#include "src/gui/shell/uiscale.h"

lv_obj_t *player_screen;

static lv_obj_t *player_menu;
static lv_obj_t *play_btn;
static lv_obj_t *play_btn_icon;
static lv_obj_t *song_title_label;
static lv_obj_t *song_artist_label;
static lv_obj_t *format_label; // "16/44.1 FLAC", right of the artist line
static lv_obj_t *fav_btn_obj;  // the button it sits on, hidden on a book
static lv_obj_t *controls_row; // transport row: where the ellipsis lives, and the star in Studio
static lv_obj_t *fav_btn_icon; // the star above it

static void update_fav_button(void);
static void update_speed_button(void);
static void update_format_label(const device_state_t *state);
static void update_repeat_button(void);

// The colour the chrome on this page is drawn in: the theme's accent normally,
// and the sleeve's own while the alternative layout is up. Everything else on
// that layout takes its colour from the record -- the pills, the star's disc,
// the shape of the track -- and a blue play button in the middle of it was the
// one thing that still belonged to some other page.
static lv_color_t chrome_accent(void);
static lv_obj_t *progress_slider;
static lv_obj_t *elapsed_label; // start of the track, under the left end of the bar
static lv_obj_t *remaining_label; // end of the track, under the right end
// Where this track sits in the queue, between the two clocks: the row under the
// bar had an empty middle and the question -- how much of this is left -- is the
// same one the clocks answer.
static lv_obj_t *queue_position_label;
static lv_obj_t *repeat_btn_icon;
static lv_obj_t *prev_icon; // static so the over-cover recolor can reach them
static lv_obj_t *next_icon;
static lv_timer_t *progress_slider_timer;

// Runs only while a sleep timer is counting, which is the only time there is
// anything to ask. Paused otherwise, so an option nobody switched on does not
// wake the main loop once a second all night.
static lv_timer_t *sleep_timer;

// Where playback was when the poll last asked, and when that was. The tick is
// lv_tick_get()'s, which is the same clock the timer is driven by.
static lv_timer_t *smooth_timer;
static bool sheet_open; // the player panel is the page on show

// "Always show the status bar" (Settings > Appearance): the bar stays up over
// the player too. Over the picture that runs to the top edge -- Standard and
// Waveform -- it blurs and dims what lies under it as it is drawn, like the
// block behind the controls; Studio, whose whole background is that blurred
// picture already, moves down under it instead, the chevron and the heading
// with it (back_btn_player_below_bar).
static bool bar_always;
static int bar_height; // the status bar's, from the config
static void bar_over_player_update(void);
static double progress_anchor_secs;
static uint32_t progress_anchor_tick;
static bool progress_running;

// The smoothing runs only while there is a bar moving on a sheet that can be
// seen: sixteen wake-ups a second for a panel that is closed or dark are pure
// drain. Asked again by every poll, which is how it comes back after a wake,
// and at every opening and closing of the sheet.
static void smooth_timer_sync(void) {
	if (!smooth_timer) {
		return;
	}
	if (progress_running && sheet_open && power_screen_is_on()) {
		lv_timer_resume(smooth_timer);
	} else {
		lv_timer_pause(smooth_timer);
	}
}

// How often the UI reconciles itself against the playback thread. It polls
// slowly rather than not at all while stopped: the audio thread can report
// STOPPED for a moment in the middle of a track change, and a UI that stopped
// polling on that reading would stay frozen until the next touch.
#define POLL_PERIOD_PLAYING_MS 500
#define POLL_PERIOD_IDLE_MS 2000

// How far past their 56 px the buttons at the two ends of the transport row
// take a tap. Upwards and downwards the 84 px row still clips it; sideways the
// previous and next buttons start about 50 px further in, so 20 reaches
// neither.
#define CORNER_BTN_EXT_CLICK ui_px(20)

// How often the progress display is carried forward between polls.
//
// The poll asks the playback thread where it is, and half a second is as often
// as that is worth asking. But half a second is also how long the bar and the
// shape of the track stood still before jumping, and a bar that moves twice a
// second reads as a bar that is broken.
//
// So the position between polls is worked out from the clock instead of being
// asked for -- playback runs in real time, so the arithmetic is exact -- and
// the drawing happens only when it would actually differ by a pixel. On a
// four-minute track that is about twice a second for each of the two, out of
// sixteen chances; the rest of the ticks cost a subtraction and a comparison.
#define SMOOTH_PERIOD_MS 60

// How many fast polls to keep running after playback stops, so the queue gets
// its chance to start the next track before the timer relaxes.
#define POLLS_FAST_AFTER_STOP 4

static int polls_since_stop = POLLS_FAST_AFTER_STOP;

// Height the controls block needs for its two lines of text, the bar, the
// clocks and the buttons. Only used as a floor on screens too short to fit a
// full-width square cover on top of it.
#define PLAYER_MENU_MIN_HEIGHT ui_px(210)

// The controls block as the R3 Pro II has it, 720 - 480. A taller panel (the
// R1's 800) spreads what it has over the rows rather than leaving it empty
// above and below them: a fifth to each end and a fifth to each of the three
// gaps between the four rows.
#define PLAYER_MENU_REF_HEIGHT ui_px(240)
#define PLAYER_MENU_PAD_VER ui_px(10)
#define PLAYER_MENU_GAP ui_px(12)

// Studio keeps only the bar, the clocks and the transport in the controls
// block, pushed to the bottom, with the gaps between them the standard
// arrangement has; the panel with the sleeve takes the rest of the screen
// above them. This is their height at PLAYER_MENU_GAP; a taller panel adds
// what its wider gaps take.
#define STUDIO_CONTROLS_H ui_px(172)

// Album art: an image on top of a placeholder panel. The panel is always
// there, so the layout doesn't jump between a track that has a cover and one
// that doesn't -- the image is simply hidden in the latter case.
static lv_obj_t *cover_panel;
static lv_obj_t *cover_img;
static lv_obj_t *cover_placeholder_icon;

// Live mode (internet radio). A stream is not a track: it cannot be paused,
// skipped or seeked, and it has no position and no length. Rather than leave
// controls on screen that would do nothing, the ones that make no sense are
// hidden and the play button becomes a stop button. These are kept so
// apply_live_mode() can reach them.
static lv_obj_t *prev_btn_obj;
static lv_obj_t *next_btn_obj;
static lv_obj_t *more_btn_obj;
static lv_obj_t *more_btn_icon; // ellipsis on a track, chapters on a book
static lv_obj_t *repeat_btn_obj;
static lv_obj_t *speed_btn_obj;	 // stands in the repeat button's place on a book
static lv_obj_t *speed_btn_icon;
static lv_obj_t *below_slider_obj; // the two clocks under the bar
// How far the four source marks keep from the corner of the artwork they
// share. One number, because they replace each other in that corner and a
// difference between them would read as the mark jumping.
#define BADGE_INSET ui_px(14)

// Where the marks sit over the artwork: in its corner, under the status bar
// when the bar stays up.
static int badge_top(void) { return BADGE_INSET + (bar_always ? bar_height : 0); }

static lv_obj_t *live_badge;	   // live indicator, top right over the artwork
static lv_obj_t *qobuz_badge;	   // the Qobuz mark, in the same corner
static lv_obj_t *tidal_badge;	   // and the Tidal one, over it: only one ever shows
static lv_obj_t *podcast_badge;	   // and the podcast one, third in the same place
static bool live_mode;
static bool live_transport_hidden; // prev/next taken away on a stream
static bool live_custom_nav;	   // on a station from a list, where they change station

// Audiobook mode. A book is one long file, not a queue of songs: previous and
// next have nothing to move to, and what a listener wants from those two
// buttons is to go back ten seconds because they missed a sentence. So the
// transport becomes a pair of ten-second jumps, the overflow menu becomes the
// chapter list, and the star and the repeat mode -- both of which belong to
// the music library and its queue -- go away rather than sit there doing
// something invisible to a book.
static bool audiobook_mode;
// A podcast is not an audiobook, but the two skip buttons are the same. See
// apply_audiobook_mode(): the rest of the book dressing -- chapters, speed,
// book icon -- stays with books only.
static bool podcast_mode;
static bool audiobook_has_chapters;		 // whether the loaded book is marked up
static int audiobook_skip_shown_back;	 // the two jump sizes the buttons are drawn
static int audiobook_skip_shown_forward; // for, so changing the setting repaints
static bool audiobook_mode_valid;   // false until the first apply, so it paints once

static cover_image_t current_cover;	   // pixels currently referenced by cover_img
static cover_image_t current_backdrop; // upside-down blurred copy behind the controls
static int cover_box_w, cover_box_h;   // the artwork spans the full screen width

// ---------------------------------------------------------------------------
// The two arrangements
//
// Standard is sleeve on top, and under it a block with the title, the star, the
// format, a bar and the transport.
//
// Alternative moves the track's own things onto the sleeve -- the title and
// artist in a pill in one bottom corner, the star in a disc in the other -- and
// turns the progress bar into the shape of the track. Everything it draws takes
// its colour from the sleeve rather than from the theme, which is why it only
// looks like anything at all over artwork.
//
// One set of widgets, moved between parents, and not two pages: the title, the
// star and the transport all have behaviour behind them, and a second copy of
// each would be a second place for that behaviour to be wrong.
// ---------------------------------------------------------------------------

// The progress bar: a thin track, and a knob that is a white ring around a
// smaller circle of the accent colour. The ring is a wide white border over an
// accent-coloured body rather than a second object, so it costs nothing and
// follows the knob on its own.
#define PROGRESS_TRACK_HEIGHT ui_px(10) // as thick as every other slider
#define PROGRESS_KNOB_GROW ui_px(9)	 // how far the knob grows past the track
#define PROGRESS_KNOB_RING ui_px(5)

// How the shape of the track is drawn, and how much room it gets. The bars are
// wide and well apart with rounded ends rather than a comb of hairlines: at
// this size a hairline is one pixel of a colour that is half background, and
// the whole thing reads as noise.
#define WAVE_HEIGHT ui_px(64)
#define WAVE_BAR_GAP ui_px(3)
#define WAVE_MIN_BAR ui_px(3)	  // a silent column is still a mark, not a hole
#define WAVE_PAST_OPA 255 // the part already played
#define WAVE_TODO_OPA 80  // and the part still to come

// How far the pills and the disc sit in from the corner of the sleeve, and how
// big the disc is.
#define ALT_PAD ui_px(16)
#define ALT_FAV_SIZE ui_px(64)
#define ALT_PILL_PAD_H ui_px(18) // what a pill keeps to the left and right of its text
#define ALT_PILL_PAD_V ui_px(8)
// How far past its own edges a transport button takes a press in this
// arrangement: into the gaps between them. The star takes the whole of its disc
// instead (apply_layout()).
#define ALT_EXT_CLICK ui_px(14)
#define FAV_BTN_W ui_px(48)
#define FAV_BTN_H ui_px(44)

// What the sleeve's colour becomes once it has to be painted rather than
// thrown.
//
// album_tone is stretched to full strength (cover_tone_stretched), the way
// cover_dominant_tone() answers for Cover Flow, where it is a light behind the
// record and a light that is not strong is not seen. Poured into a solid pill the same number is a poster
// colour -- which is the complaint. So the hue is kept and everything else is
// pinned: the surface keeps a third of its distance from grey and sits at a
// fixed low brightness, the ink on it is always the same white, and the mark
// the waveform draws keeps rather more colour and a fixed high brightness so it
// still reads over a dark blurred sleeve. Two roles, one hue -- every album
// then looks like the same player rather than like a different skin.
#define ALT_SURFACE_SAT 34	// per cent of the distance from grey kept
#define ALT_SURFACE_LUMA 58 // and where it sits on 0..255
#define ALT_MARK_SAT 72
#define ALT_MARK_LUMA 196

// The setting, and whether it is in force right now.
//
// They are two different things because the alternative arrangement is built
// around a file on the card: the shape of the track comes from decoding it, the
// colour from its sleeve, and the pill in the corner from tags. A radio station
// has no length to draw, a stream from a phone has no file to read ahead of,
// and a Qobuz track is a cache entry that will be gone tomorrow. So the setting
// says what the user wants for their music, and `layout_alt_now` says whether
// what is playing is music of that kind. Everything that draws asks the second.
static player_layout_t layout_choice; // the setting, saved in the config
static bool layout_alt_now;		// and whether the current source can use it
static bool layout_studio_now;	// likewise for the third arrangement
// The shape of the track and the sleeve's colour: up with the alternative
// layout, and kept when the words are shown over it.
static bool layout_wave_now;

static uint32_t album_tone; // the sleeve's colour, 0 when there is no sleeve
// The setting that has the Standard and Studio arrangements wear the sleeve's
// colour as their accent too, as Waveform always does.
static bool cover_accent;

static lv_obj_t *song_text_obj; // the title and artist, wherever they live now
static lv_obj_t *song_side_obj; // the star and the format, likewise
static void align_title_with_star(void);
static lv_obj_t *alt_text_col;	// the two pills, stacked at the foot of the sleeve
static lv_obj_t *alt_title_pill;
static lv_obj_t *alt_artist_pill;
static lv_obj_t *alt_fav_circle;
static lv_obj_t *wave_box;
static lv_obj_t *wave_canvas;
static uint8_t *wave_buf;
static int wave_w; // the buffer's width, which is what the painting must use
static uint8_t wave_bars[WAVEFORM_BARS];
// How many columns of this track's shape are known, from the left: all of them
// once it has been worked out, fewer while the worker is still reading the
// file, and the rest are drawn flat until they arrive.
static int wave_ready;
static int wave_drawn = -1; // where the playhead was, in pixels, when it was drawn
static uint32_t wave_drawn_tone = 1;
static int backdrop_w, backdrop_h;	   // size of the controls block it sits behind
// Studio shows the same blurred copy behind the whole screen, so there it is
// asked for at that shape instead: a picture made for the block behind the
// controls is a third of the height and stretching it up is both distorted and
// coarse. `backdrop_is_studio` is the shape the picture on hand was made at.
static int backdrop_studio_h;
static bool backdrop_is_studio;

static double current_total_length = 0; // cached from the last device_state snapshot, so slider math works between polls
static char progress_label_text[32];

// The stretch of the file the progress bar and its two clocks stand for. The
// whole file, except on a book with chapters when Audiobooks -> Show duration
// is set to the chapter: then only the chapter being listened to, so the bar
// starts empty at every chapter and the right clock is the chapter's length.
// `view_length` is 0 while the length of the file is not known.
static double view_start;
static double view_length;

// True while the controls sit on a loaded cover's dark backdrop. The play
// button wears its white disc there (it has to stand out against artwork);
// on the plain no-cover panel it is flat, like prev and next.
static bool chrome_over_cover;

// While a station connects, stop, previous and next are greyed out and do
// nothing: the connection is on its way, and pulling it down or starting the
// next one in the middle of it is a tap that only makes the wait longer. The
// side keys and the control centre ask radio_is_connecting() themselves.
static bool connecting_shown;

static void apply_connecting(void) {
	bool connecting = radio_is_connecting();
	if (connecting == connecting_shown) {
		return;
	}
	connecting_shown = connecting;
	lv_obj_t *const buttons[] = {play_btn, prev_btn_obj, next_btn_obj};
	for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		if (!buttons[i]) {
			continue;
		}
		if (connecting) {
			lv_obj_add_state(buttons[i], LV_STATE_DISABLED);
		} else {
			lv_obj_remove_state(buttons[i], LV_STATE_DISABLED);
		}
	}
}

// Switches the transport between a track and a live stream. Cheap and
// idempotent: called from every now-playing refresh, and does nothing at all
// unless the mode actually changed.
static void apply_live_mode(bool live) {
	// Previous and next go away on a stream, playing or stopped: they would
	// start a local track in place of the station. On a station started from a
	// list -- radio.txt, a country, a search, the starred -- they stay and move
	// along that list, and the bearing under the bar says where in it the
	// station is (see update_queue_position()).
	bool custom_nav = live && radio_can_step();
	bool transport_hidden = live && !custom_nav;

	if (live == live_mode && transport_hidden == live_transport_hidden && custom_nav == live_custom_nav &&
		cover_placeholder_icon) {
		return;
	}
	live_mode = live;
	live_transport_hidden = transport_hidden;
	live_custom_nav = custom_nav;

	// A stream has no queue to look at, no length to scrub and no repeat mode
	// that means anything.
	//
	// Repeat/shuffle goes for a reason worth stating: it is not merely useless
	// on a stream, it is misleading. The mode it cycles belongs to the track
	// queue, so a tap there would silently change what happens to the music
	// the radio interrupted.
	lv_obj_t *const hidden_when_live[] = {more_btn_obj, repeat_btn_obj, progress_slider, elapsed_label,
										  remaining_label};
	for (size_t i = 0; i < sizeof(hidden_when_live) / sizeof(hidden_when_live[0]); i++) {
		if (!hidden_when_live[i]) {
			continue;
		}
		if (live) {
			lv_obj_set_hidden(hidden_when_live[i], true);
		} else {
			lv_obj_set_hidden(hidden_when_live[i], false);
		}
	}
	// The row under the bar stays for a station from a list, for its place in
	// the list and nothing else.
	if (below_slider_obj) {
		if (live && !custom_nav) {
			lv_obj_set_hidden(below_slider_obj, true);
		} else {
			lv_obj_set_hidden(below_slider_obj, false);
		}
	}

	// The shape of the track goes with the bar, and comes back only if the
	// alternative layout is the one up -- it is not in the list above because
	// that list has no idea which arrangement is on screen, and unhiding it in
	// the standard one would leave an empty strip under the title.
	if (wave_box) {
		if (live || !layout_wave_now) {
			lv_obj_set_hidden(wave_box, true);
		} else {
			lv_obj_set_hidden(wave_box, false);
		}
	}

	lv_obj_t *const transport[] = {prev_btn_obj, next_btn_obj};
	for (size_t i = 0; i < sizeof(transport) / sizeof(transport[0]); i++) {
		if (!transport[i]) {
			continue;
		}
		if (transport_hidden) {
			lv_obj_set_hidden(transport[i], true);
		} else {
			lv_obj_set_hidden(transport[i], false);
		}
	}

	if (live_badge) {
		if (live) {
			lv_obj_set_hidden(live_badge, false);
			lv_obj_move_foreground(live_badge);
		} else {
			lv_obj_set_hidden(live_badge, true);
		}
	}
	// The marks share one corner and can never go there together: a live
	// stream does not come from Qobuz.
	if (tidal_badge && live) {
		lv_obj_set_hidden(tidal_badge, true);
	}
	if (podcast_badge && live) {
		lv_obj_set_hidden(podcast_badge, true);
	}
	if (qobuz_badge && live) {
		lv_obj_set_hidden(qobuz_badge, true);
	}

	// A note means "track", a book means a book, and a station with no artwork
	// gets the radio. Coming back off a stream has to restore whichever of the
	// first two is right, not assume the note.
	if (cover_placeholder_icon) {
		lv_image_set_src(cover_placeholder_icon, live ? &icon_radio_player
									: (audiobook_mode ? &icon_book_headphones : &icon_music_note));
	}
}

// Dresses the player for a book or for a track. Cheap and idempotent, like
// apply_live_mode(): called from every now-playing refresh and from the poll,
// and does nothing at all unless the answer changed.
static void apply_audiobook_mode(bool book, bool podcast) {
	// Whether this book is marked up decides what the chapter button does, not
	// whether it is there. A folder book's files are its chapters.
	bool chapters = book && (audiobook_chapter_count() > 0 || audiobook_part_count() > 1);

	// Books and podcasts both get the second-jump buttons, but not the same
	// pair of sizes: a book's forward jump steps over a pause, a podcast's
	// over a half-minute sponsor read. They are two separate settings on two
	// separate pages (podcast settings, and Audiobooks -> change controls).
	//
	// Speed carries over too, as a setting of its own. Nothing else does: a
	// podcast has no chapters, is not a single file, and sits in a real queue
	// of episodes. Chapters and the book icon stay with books.
	bool skips = book || podcast;
	int back = podcast && !book ? podcast_skip_back() : audiobook_skip_back();
	int forward = podcast && !book ? podcast_skip_forward() : audiobook_skip_forward();

	if (audiobook_mode_valid && book == audiobook_mode && podcast == podcast_mode &&
		chapters == audiobook_has_chapters && back == audiobook_skip_shown_back &&
		forward == audiobook_skip_shown_forward) {
		return;
	}
	audiobook_mode = book;
	podcast_mode = podcast;
	audiobook_has_chapters = chapters;
	audiobook_skip_shown_back = back;
	audiobook_skip_shown_forward = forward;
	audiobook_mode_valid = true;

	if (prev_icon) {
		lv_image_set_src(prev_icon, !skips                            ? &icon_skip_back
									: back == AUDIOBOOK_SKIP_HUGE ? &icon_prev_60
									: back == AUDIOBOOK_SKIP_LONG ? &icon_prev_30
																  : &icon_prev_10);
	}
	if (next_icon) {
		lv_image_set_src(next_icon, !skips                               ? &icon_skip_forward
									: forward == AUDIOBOOK_SKIP_HUGE ? &icon_next_60
									: forward == AUDIOBOOK_SKIP_LONG ? &icon_next_30
																	 : &icon_next_10);
	}
	// A book has a menu of its own behind the usual glyph: chapters,
	// bookmarks, summary. On a podcast the same button goes straight to the
	// episodes, the only thing wanted from it, rather than to a menu whose one
	// useful entry would always be picked.
	if (more_btn_icon) {
		lv_image_set_src(more_btn_icon, podcast && !book ? &icon_podcast_episodes : &icon_ellipsis_vertical);
	}

	// The star and the repeat mode are about the music library and its queue.
	// On a book they are not merely useless: starring would write a row into
	// the music favourites for a file the music library does not index, and
	// the repeat mode would silently change what happens to the queue the book
	// interrupted.
	//
	// On a podcast the star stays but means something else: it follows the
	// podcast, putting it under the subscribed feeds -- the same promise the
	// star makes everywhere, "find this again without searching". Repeat still
	// goes: a queue of episodes is not an album, and looping the last twenty
	// instalments is nothing anybody asks for.
	if (fav_btn_obj) {
		if (book) {
			lv_obj_set_hidden(fav_btn_obj, true);
		} else {
			lv_obj_set_hidden(fav_btn_obj, false);
		}
	}
	if (repeat_btn_obj) {
		if (book || podcast) {
			lv_obj_set_hidden(repeat_btn_obj, true);
		} else {
			lv_obj_set_hidden(repeat_btn_obj, false);
		}
	}

	if (speed_btn_obj) {
		if (book || podcast) {
			lv_obj_set_hidden(speed_btn_obj, false);
			update_speed_button();
		} else {
			lv_obj_set_hidden(speed_btn_obj, true);
		}
	}

	// A book with no artwork gets the book and an episode the microphone, not
	// a musical note -- each the glyph its own page uses, drawn at the size
	// this one is, which is why the podcast row's 40 px copy is not the one
	// named here. Only touched when a station is not up: live mode owns this
	// icon while it is on.
	if (cover_placeholder_icon && !live_mode) {
		const lv_image_dsc_t *mark = book ? &icon_book_headphones : podcast ? &icon_podcast_cover : &icon_music_note;
		lv_image_set_src(cover_placeholder_icon, mark);
	}
}

// ---------------------------------------------------------------------------
// Stopping on its own: at the end of a chapter, or after a while
//
// Both live here, in the poll that already runs twice a second, because both
// are questions about where playback has got to -- and neither wants a timer
// of its own that could fire while the book is paused, or after it has been
// swapped for a song.
// ---------------------------------------------------------------------------

// Where the chapter being listened to ends, or 0 when the book has no chapters
// or is on its last one. Worked out per poll rather than remembered, so a jump
// to another chapter is picked up without anything having to be told.
static double current_chapter_end(double position) {
	int index = audiobook_chapter_at(position);
	if (index < 0) {
		return 0;
	}
	double start = 0;
	if (!audiobook_chapter(index + 1, NULL, 0, &start)) {
		return 0; // the last chapter ends where the file does
	}
	return start;
}

static void pause_book_at(double seconds) {
	device_state_seek(seconds);
	if (audio_get_status() == AUDIO_STATUS_PLAYING) {
		audiobook_suppress_rewind_once();
		device_state_toggle_play_pause();
	}
	// Written through: stopping on purpose is exactly the moment somebody puts
	// the player down.
	audiobook_note_position(seconds, 0, true);
}

// Runs on every poll while a book is loaded.
//
// Only the chapter boundary: the sleep timers live in sleeptimer.c, where all
// three behave the same way.
static bool audiobook_auto_stop(const device_state_t *state) {
	if (state->status != AUDIO_STATUS_PLAYING || !audiobook_stop_at_chapter_end()) {
		return false;
	}
	double end = current_chapter_end(state->progress_current_secs);
	if (end > 0 && state->progress_current_secs >= end - 0.5) {
		pause_book_at(end);
		return true;
	}
	return false;
}

// The jump both the on-screen buttons and the side keys make. Clamped at both
// ends: asking for -10 s in the first second means the beginning, and running
// off the end of the file would stop the book instead of moving in it.
static void audiobook_seek_by(double delta, const device_state_t *state) {
	double target = state->progress_current_secs + delta;
	if (target < 0) {
		target = 0;
	}
	double length = state->progress_total_secs > 0 ? state->progress_total_secs : current_total_length;
	if (length > 1.0 && target > length - 1.0) {
		target = length - 1.0;
	}

	device_state_seek(target);
	// Written through immediately: a jump is a deliberate move, and the ten
	// second throttle would lose it if the player were switched off next.
	audiobook_note_position(target, length, true);
}

// Applies a playback status to the play/pause button and the progress timer.
// Called both optimistically (right after a button press, before the audio
// thread has caught up) and from update_progress() every poll, which is what
// keeps the UI from going stale if the track stops on its own.
// Which sleep timer, if any, owns what is playing.
//
// Books and podcasts have their own because they are listened to differently: a
// stretch that is right for falling asleep to music is not the one somebody
// chose for a chapter, and a podcast timer set at bedtime should not be counting
// down over an album the next morning. Everything else the transport plays --
// the card, Tidal, Qobuz -- is one thing and gets one timer.
//
// Three sources are none of them, and for the same reason: they do not go
// through the player's transport at all. The radio, the Game Boy and the
// Bluetooth receiver push their sound out through the external path, and there
// is no pause to give them -- stopping a station is a disconnection, stopping
// the emulator is stopping the game, and stopping the receiver is silence in a
// room where somebody else's phone is still playing. Only the radio ever
// reports playing here; the other two are written down so that it stays true if
// they ever start to.
static sleeptimer_kind_t sleeptimer_kind_playing(void) {
	if (radio_is_active() || gearboy_running() || btreceiver_is_active()) {
		return SLEEPTIMER_COUNT;
	}
	if (audiobook_is_playing()) {
		return SLEEPTIMER_AUDIOBOOK;
	}
	char path[512];
	audio_get_current_file(path, sizeof(path));
	if (podcastcache_is_episode(path)) {
		return SLEEPTIMER_PODCAST;
	}
	return SLEEPTIMER_MUSIC;
}

static void apply_playback_status(audio_status_t status) {
	bool playing = (status == AUDIO_STATUS_PLAYING);

	// The glyph is the accent colour, always: play and pause are the same
	// control in two states. Only the disc under it changes (see
	// set_over_cover): white over artwork and in the dark theme, dark in the
	// light theme when there is no cover to stand out against.
	//
	// On a live stream there is no pause -- stopping and starting again does
	// not resume, it reconnects -- so the button says stop while it is on.
	// Stopped, it goes back to play: the station stays loaded, and pressing
	// it opens a fresh connection to the same one.
	// Each of the three only when it would change: this runs on every poll, and
	// a button redrawn twice a second for nothing is a refresh of the whole
	// interface twice a second (see unchanged.h).
	gui_image_set_src(play_btn_icon,
					  live_mode ? (playing ? &icon_stop : &icon_play) : (playing ? &icon_pause : &icon_play));
	lv_color_t accent = chrome_accent();
	if (!lv_color_eq(lv_obj_get_style_image_recolor(play_btn_icon, 0), accent)) {
		lv_obj_set_style_image_recolor(play_btn_icon, accent, 0);
	}
	if (lv_obj_get_style_image_recolor_opa(play_btn_icon, 0) != LV_OPA_COVER) {
		lv_obj_set_style_image_recolor_opa(play_btn_icon, LV_OPA_COVER, 0);
	}

	// Keep polling either way, just less often when nothing is playing. The
	// timer must never be paused here: a track change is briefly reported as
	// STOPPED, and a timer paused on that reading leaves nothing running to
	// correct the progress bar and the play/pause icon.
	//
	// Dropping straight to the slow period on STOPPED is wrong too: the end of
	// a track is reported as STOPPED and the queue only advances on the next
	// poll, so the gap between two tracks would become the slow period, seconds
	// of silence reading as a player that stopped instead of going on. Stay on
	// the fast period for a moment after playback stops, which is exactly the
	// window the handover needs.
	if (playing) {
		polls_since_stop = 0;
	} else if (polls_since_stop < POLLS_FAST_AFTER_STOP) {
		polls_since_stop++;
	}

	bool settling = polls_since_stop < POLLS_FAST_AFTER_STOP;
	lv_timer_set_period(progress_slider_timer, (playing || settling) ? POLL_PERIOD_PLAYING_MS : POLL_PERIOD_IDLE_MS);
	lv_timer_resume(progress_slider_timer);

	// The status bar shows the same state, and told here rather than left to
	// wait for its own five-second poll.
	topbar_refresh_playback();

	// Every route into and out of playing comes through here -- the button, the
	// side keys, the end of a track, a remote, the poll putting the screen back
	// in step with the engine -- so it is the one place the sleep timers can be
	// told without being told twice. It is also the one place that knows what
	// is playing well enough to say which of the three it belongs to.
	sleeptimer_note_playing(playing ? sleeptimer_kind_playing() : SLEEPTIMER_COUNT);
	if (sleep_timer) {
		if (sleeptimer_any_running()) {
			lv_timer_resume(sleep_timer);
		} else {
			lv_timer_pause(sleep_timer);
		}
	}
}

// "3/24": which track of the queue is playing.
//
// It reads the queue itself, so it says the same thing whatever filled it --
// tracks off the card, a podcast's episodes, a Tidal or Qobuz album.
//
// The number is the track's place in the LIST, not in the dealt order: under
// shuffle those are two different things, and the list is the one that means
// something. Counting the deal gives a number that climbs by one at every
// track, which reads as a position in an album but is not one, since the
// tracks are picked at random from all over the card. Outside shuffle the two
// orders are the same array and the reading is identical.
//
// Blank rather than "1/1" when there is nothing to be lost in: a single track,
// a book (one long file), a stream (no queue at all), or playback that belongs
// to a phone over DLNA.
bool player_queue_position(int *position, int *count) {
	*position = 0;
	*count = 0;
	if (live_mode) {
		int index;
		int total;
		if (live_custom_nav && radio_list_position(&index, &total)) {
			*position = index + 1;
			*count = total;
			return true;
		}
		return false;
	}
	int total = playlist_count();
	int index = playlist_current_index();
	if (total <= 1 || index < 0 || audiobook_mode || dlna_owns_playback()) {
		return false;
	}
	*position = (int)playlist_current_entry() + 1;
	*count = total;
	return true;
}

static void update_queue_position(void) {
	if (!queue_position_label) {
		return;
	}

	if (live_mode) {
		int index;
		int count;
		if (live_custom_nav && radio_list_position(&index, &count)) {
			gui_label_set_text_fmt(queue_position_label, "%d/%d", index + 1, count);
		} else {
			gui_label_set_text(queue_position_label, "");
		}
		return;
	}

	int count = playlist_count();
	int index = playlist_current_index();
	bool worth_saying = count > 1 && index >= 0 && !audiobook_mode && !dlna_owns_playback();

	if (!worth_saying) {
		gui_label_set_text(queue_position_label, "");
		return;
	}
	gui_label_set_text_fmt(queue_position_label, "%d/%d", (int)playlist_current_entry() + 1, count);
}

// Works out view_start and view_length for playback at `position` in a file
// `total` seconds long. Asked on every poll rather than at a chapter change:
// a seek, a skip or the book simply playing on moves into another chapter, and
// nothing has to be told.
static void view_update(double position, double total) {
	view_start = 0;
	view_length = total > 0 ? total : 0;
	if (view_length <= 0 || !audiobook_mode || !audiobook_has_chapters || !audiobook_duration_per_chapter()) {
		return;
	}
	int index = audiobook_chapter_at(position);
	double start = 0;
	if (index < 0 || !audiobook_chapter(index, NULL, 0, &start)) {
		return;
	}
	double end = total;
	if (!audiobook_chapter(index + 1, NULL, 0, &end) || end > total) {
		end = total; // the last chapter ends where the file does
	}
	if (start < 0 || end - start < 1.0) {
		return; // a mark with nothing after it: the whole file is the better answer
	}
	view_start = start;
	view_length = end - start;
}

// Where `position` sits along the bar, 0 to 1000, the slider's range.
static int view_value(double position) {
	if (view_length <= 0) {
		return 0;
	}
	double fraction = (position - view_start) / view_length;
	if (fraction < 0) {
		fraction = 0;
	}
	if (fraction > 1) {
		fraction = 1;
	}
	return (int)(fraction * 1000);
}

// view_value() snapped to the bar's own pixels: the smallest value at which the
// indicator ends on the same pixel as at `position`. The range is 1000 and the
// bar is some 450 layout pixels long, so most steps of the raw value move
// nothing on screen -- and every step the slider is told about invalidates the
// whole strip it sits in, knob and both clocks' row, 480x54. On a three-minute
// track that was five redraws a second for two pixels of travel. Snapped, a
// tick that would draw the same picture leaves the value as it is, and the
// callers' "has it changed" test skips it. The length is worked out the way
// lv_bar does it (its width less the background's padding); a stray pixel of
// difference only moves where the snap falls, not how often.
static int bar_value(double position) {
	int value = view_value(position);
	int32_t len = lv_obj_get_width(progress_slider) - lv_obj_get_style_pad_left(progress_slider, LV_PART_MAIN) -
				  lv_obj_get_style_pad_right(progress_slider, LV_PART_MAIN);
	if (len <= 0 || len >= 1000) {
		return value;
	}
	int32_t px = value * len / 1000;
	return (int)((px * 1000 + len - 1) / len);
}

// lv_slider_set_value() invalidates the whole slider with its knob's margin --
// 480x54, the row with both clocks -- for a knob that moved a pixel or two.
// Here only the strip from the knob as last drawn to where it will be is
// invalidated, the same margin wide on each side; the whole-slider path is
// kept for when there is no drawn knob yet or LVGL is animating the value.
static void progress_slider_set(int value) {
	lv_slider_t *slider = (lv_slider_t *)progress_slider;
	lv_bar_t *bar = &slider->bar;
	value = LV_CLAMP(bar->min_value, value, bar->max_value);
	if (value == bar->cur_value) {
		return;
	}
	lv_area_t drawn = slider->right_knob_area;
	if (lv_anim_get(&bar->cur_value_anim, NULL) || lv_area_get_width(&drawn) <= 1 ||
		bar->max_value <= bar->min_value) {
		lv_slider_set_value(progress_slider, value, LV_ANIM_OFF);
		return;
	}
	lv_area_t coords;
	lv_obj_get_coords(progress_slider, &coords);
	int32_t left = coords.x1 + lv_obj_get_style_pad_left(progress_slider, LV_PART_MAIN);
	int32_t len = lv_area_get_width(&coords) - lv_obj_get_style_pad_left(progress_slider, LV_PART_MAIN) -
				  lv_obj_get_style_pad_right(progress_slider, LV_PART_MAIN);
	int32_t end = left + (int32_t)((int64_t)(value - bar->min_value) * len / (bar->max_value - bar->min_value));
	int32_t half = lv_area_get_width(&drawn) / 2 + ui_px(2); // the knob, and a rounding pixel either way
	int32_t ext = lv_obj_get_ext_draw_size(progress_slider);
	lv_area_t dirty = {
		.x1 = LV_MIN(drawn.x1, end - half) - ext,
		.y1 = coords.y1 - ext,
		.x2 = LV_MAX(drawn.x2, end + half) + ext,
		.y2 = coords.y2 + ext,
	};
	bar->cur_value = value;
	lv_obj_invalidate_area(progress_slider, &dirty);
}

// The other way round: the second of the file that slider value `value` is.
static double view_seconds(int value) { return view_start + view_length * value / 1000.0; }

// Writes the two clocks that sit under the ends of the progress bar: elapsed
// on the left, total length on the right -- both of the file, or both of the
// chapter (see view_start).
static void set_progress_label(double current_secs, double total_secs) {
	view_update(current_secs, total_secs);
	double shown = 0;
	if (view_length > 0) {
		progress_slider_set(bar_value(current_secs));
		shown = current_secs - view_start;
		if (shown < 0) {
			shown = 0;
		}
	}

	formatDoubleSeconds(shown, progress_label_text, sizeof(progress_label_text));
	gui_label_set_text(elapsed_label, progress_label_text);

	formatDoubleSeconds(view_length, progress_label_text, sizeof(progress_label_text));
	gui_label_set_text(remaining_label, progress_label_text);

	// Written from here because this is the one place that runs on every poll
	// and on every track change, which is exactly when the answer can move.
	update_queue_position();
}

// True while a decode is in flight at the worker.
static bool cover_request_outstanding;
// When the track last changed, for the log line that says how long its
// picture took to be on screen -- the delay a skip is felt by.
static uint32_t track_change_tick;
// The sizes the last request used, which the next track's warm-up repeats so
// its cache entry is the one that request will look for.
static int warm_cover_w, warm_cover_h, warm_backdrop_h;

// What the artwork on screen IS, as cover_source_id() reports it, so the next
// track can be asked "the same picture?" before anything is thrown away. 0 when
// it is not known -- the answer has not come back yet, or there is no artwork.
static uint64_t cover_shown_id;

// The id of the picture now being decoded, when it was already known before the
// decode was asked for. Saves hashing the same file twice.
static uint64_t cover_incoming_id;

// Hashing never happens on this thread: it reads the picture out of the file,
// which is the work the loader exists to keep off the interface. So there is a
// question outstanding, and two reasons to have asked it.
static enum {
	COVER_ID_IDLE,
	COVER_ID_NEW_TRACK, // is the next track's picture the one already up?
	COVER_ID_MEASURE,   // what is the picture that just went up?
} cover_id_state;

// How often the loader's answers are looked for while one is outstanding. The
// progress poll comes round only every half second (two seconds when paused),
// and a track change asks twice -- what the picture is, then the picture -- so
// waiting for it added up to a second, or four, on top of the work itself.
// This timer runs only while a question is out and stops itself after.
#define COVER_POLL_MS 30
static lv_timer_t *cover_poll_timer;
static void cover_poll_start(void);

// With artwork loaded the controls sit on the always-dark blurred backdrop,
// so the chrome -- titles, clocks, transport glyphs, the chevron -- must be
// light in both themes. Local style props are laid over the theme styles
// while the cover is up, and removed again when it goes, so the plain
// no-cover panel keeps following the theme.
// The picture at the width of the screen, and the note that stands in for it.
//
// One place, because Studio hides both -- the sleeve there is a smaller picture
// of its own over the blurred copy -- and the artwork arrives on a worker, so
// whatever the arrangement did when it was set up would be undone the moment a
// cover landed.
static void cover_show(bool have_cover) {
	if (!cover_img) {
		return;
	}
	bool studio = layout_studio_now;
	if (have_cover && !studio) {
		lv_obj_set_hidden(cover_img, false);
	} else {
		lv_obj_set_hidden(cover_img, true);
	}
	if (!cover_placeholder_icon) {
		return;
	}
	if (!have_cover && !studio) {
		lv_obj_set_hidden(cover_placeholder_icon, false);
	} else {
		lv_obj_set_hidden(cover_placeholder_icon, true);
	}
}

static void set_over_cover(bool on) {
	chrome_over_cover = on;

	lv_obj_t *const text_objs[] = {song_title_label, elapsed_label, remaining_label};
	lv_obj_t *const dim_objs[] = {song_artist_label, format_label, queue_position_label};
	lv_obj_t *const icon_objs[] = {prev_icon, next_icon};

	for (size_t i = 0; i < sizeof(text_objs) / sizeof(text_objs[0]); i++) {
		if (!text_objs[i]) {
			continue;
		}
		if (on) {
			lv_obj_set_style_text_color(text_objs[i], lv_color_white(), 0);
		} else {
			lv_obj_remove_local_style_prop(text_objs[i], LV_STYLE_TEXT_COLOR, 0);
		}
	}
	for (size_t i = 0; i < sizeof(dim_objs) / sizeof(dim_objs[0]); i++) {
		if (!dim_objs[i]) {
			continue;
		}
		if (on) {
			lv_obj_set_style_text_color(dim_objs[i], lv_color_make(200, 200, 200), 0);
		} else {
			lv_obj_remove_local_style_prop(dim_objs[i], LV_STYLE_TEXT_COLOR, 0);
		}
	}
	for (size_t i = 0; i < sizeof(icon_objs) / sizeof(icon_objs[0]); i++) {
		if (!icon_objs[i]) {
			continue;
		}
		if (on) {
			lv_obj_set_style_image_recolor(icon_objs[i], lv_color_white(), 0);
			lv_obj_set_style_image_recolor_opa(icon_objs[i], LV_OPA_COVER, 0);
		} else {
			lv_obj_remove_local_style_prop(icon_objs[i], LV_STYLE_IMAGE_RECOLOR, 0);
			lv_obj_remove_local_style_prop(icon_objs[i], LV_STYLE_IMAGE_RECOLOR_OPA, 0);
		}
	}

	// The disc: white over artwork and in the dark theme, where white reads
	// as the brightest thing on screen. The one case it has to go dark is the
	// light theme with no cover -- a white disc on a near-white panel is
	// invisible.
	if (play_btn) {
		bool white_disc = on || theme()->dark;
		lv_obj_set_style_bg_color(play_btn, white_disc ? lv_color_white() : theme()->surface_pressed, 0);
		lv_obj_set_style_bg_opa(play_btn, LV_OPA_COVER, 0);
	}
	if (play_btn_icon) {
		lv_obj_set_style_image_recolor(play_btn_icon, chrome_accent(), 0);
		lv_obj_set_style_image_recolor_opa(play_btn_icon, LV_OPA_COVER, 0);
	}

	// The floating chevron parks over the artwork's top-left corner.
	back_btn_over_cover(on);
}

// Told before the loaded track's artwork is freed. The screensaver borrows
// those pixels and can still be up behind a blanked panel when a track
// changes, so it has to let go first.
static void (*cover_release_cb)(void);

void player_set_cover_release_cb(void (*cb)(void)) { cover_release_cb = cb; }

void player_cover_box(int *w, int *h) {
	if (w) {
		*w = cover_box_w;
	}
	if (h) {
		*h = cover_box_h;
	}
}

bool player_cover_copy(int size, cover_image_t *out, uint64_t *id) {
	*id = 0;
	// Only a settled cover: while one is coming, or a track change is still
	// asking whether its picture is the same, what is up may be the last
	// track's.
	if (!current_cover.pixels || cover_request_outstanding || cover_id_state != COVER_ID_IDLE) {
		return false;
	}
	if (!cover_scaled_copy(&current_cover, size, out)) {
		return false;
	}
	*id = cover_shown_id;
	return true;
}

// The path whose artwork is currently up. A live stream refreshes its text
// several times a minute (every new on-air title), and re-decoding the same
// station icon each time would be pointless work on the UI thread.
static char cover_shown_path[512];

static void reload_cover(const char *filepath);

// Shows the placeholder and asks the worker for the album art of `filepath`.
// Nothing is read or decoded here: on the X1600E that work is seconds long,
// and doing it on this thread at every track change -- including the automatic
// one when a song ends -- froze whatever the user was doing at that moment,
// the file browser most of all. The finished pictures are collected by
// apply_cover_result(), from cover_poll_cb() and the progress timer tick.
// Rec. 601 luma, which is what "how bright does this look" means to an eye and
// not to a sum of three channels.
static int luma_of(int r, int g, int b) { return (r * 299 + g * 587 + b * 114) / 1000; }

// The sleeve's colour with its strength cut to `sat` per cent of the distance
// from grey and its brightness moved to `want`. Darkening is a plain ratio, so
// no channel can clip; brightening walks each channel the same fraction of the
// way to white, which keeps the hue where it was.
static lv_color_t tone_shaped(int sat, int want) {
	uint32_t raw = album_tone;
	if (!raw) {
		lv_color_t accent = theme()->accent;
		raw = ((uint32_t)accent.red << 16) | ((uint32_t)accent.green << 8) | accent.blue;
	}
	int r = (int)((raw >> 16) & 0xFF), g = (int)((raw >> 8) & 0xFF), b = (int)(raw & 0xFF);

	int grey = luma_of(r, g, b);
	r = grey + (r - grey) * sat / 100;
	g = grey + (g - grey) * sat / 100;
	b = grey + (b - grey) * sat / 100;

	int luma = luma_of(r, g, b);
	if (luma > want && luma > 0) {
		r = r * want / luma;
		g = g * want / luma;
		b = b * want / luma;
	} else if (luma < want && luma < 255) {
		int step = (want - luma) * 255 / (255 - luma);
		r += (255 - r) * step / 255;
		g += (255 - g) * step / 255;
		b += (255 - b) * step / 255;
	}
	return lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

// What the pills and the disc are filled with, and what the waveform is drawn
// in. Cover Flow throws the same hue under the record in the middle, which is
// what makes the two pages look like they belong to the same player.
static lv_color_t alt_surface(void) { return tone_shaped(ALT_SURFACE_SAT, ALT_SURFACE_LUMA); }
static lv_color_t alt_mark(void) { return tone_shaped(ALT_MARK_SAT, ALT_MARK_LUMA); }

// The accent the transport, the progress bar and the sung line wear: the
// sleeve's colour in the Waveform arrangement, and in the other two as well
// when the setting asks for it and there is a sleeve to take it from. The same
// shade in all three, light enough to read over the dark backdrop every
// arrangement puts behind a sleeve.
static lv_color_t chrome_accent(void) {
	if (layout_wave_now || (cover_accent && album_tone)) {
		return alt_mark();
	}
	return theme()->accent;
}

// The type on that surface. Worked out rather than fixed so that a change to
// ALT_SURFACE_LUMA cannot quietly leave the title unreadable.
static lv_color_t alt_ink(void) {
	lv_color_t s = alt_surface();
	return luma_of(s.red, s.green, s.blue) > 140 ? lv_color_hex(0x1A1A1A) : lv_color_white();
}

static void lyrics_repaint_lit(void);

// The two glyphs that wear the accent, repainted whenever the accent of the
// moment can have moved: the play/pause disc and the repeat/shuffle mode.
static void paint_transport_tint(void) {
	if (play_btn_icon) {
		lv_obj_set_style_image_recolor(play_btn_icon, chrome_accent(), 0);
		lv_obj_set_style_image_recolor_opa(play_btn_icon, LV_OPA_COVER, 0);
	}
	if (repeat_btn_icon) {
		update_repeat_button();
	}
	update_speed_button();
	// The progress bar, which Waveform hides under the shape of the track.
	if (progress_slider) {
		lv_obj_set_style_bg_color(progress_slider, chrome_accent(), LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(progress_slider, chrome_accent(), LV_PART_KNOB);
	}
	lyrics_repaint_lit(); // the sung line wears it too
}

// Hides the artist's pill when there is no artist. An empty pill is a blank
// lozenge sitting on the sleeve for no reason, and a stream between titles has
// no artist for seconds at a time.
static void alt_pills_sync(void) {
	if (!alt_artist_pill || !song_artist_label) {
		return;
	}
	const char *artist = lv_label_get_text(song_artist_label);
	if (artist && artist[0]) {
		lv_obj_set_hidden(alt_artist_pill, false);
	} else {
		lv_obj_set_hidden(alt_artist_pill, true);
	}
}

// Puts the sleeve's colour on everything the alternative layout tints -- and,
// in the standard layout, takes it off again. That second half is the point:
// these are local style properties on widgets the two arrangements share, so
// leaving them behind painted the standard page in the last sleeve's ink.
static void paint_alt_tint(void) {
	if (!alt_title_pill) {
		return;
	}

	if (!layout_alt_now) {
		if (song_artist_label) {
			lv_obj_remove_local_style_prop(song_artist_label, LV_STYLE_TEXT_OPA, 0);
		}
		// And the title and the artist go back to being whatever the page they
		// are on says they are. Asked of the one function that knows -- the
		// colour depends on whether there is artwork behind them, not on the
		// theme alone, and a second copy of that rule here would be a second
		// place for it to go out of date.
		set_over_cover(chrome_over_cover);
		update_fav_button(); // which owns the star's colour in both arrangements
		paint_transport_tint();
		wave_drawn = -1;
		return;
	}

	lv_color_t surface = alt_surface();
	lv_color_t ink = alt_ink();

	lv_obj_set_style_bg_color(alt_title_pill, surface, 0);
	lv_obj_set_style_bg_color(alt_artist_pill, surface, 0);
	lv_obj_set_style_bg_color(alt_fav_circle, surface, 0);

	if (song_title_label) {
		lv_obj_set_style_text_color(song_title_label, ink, 0);
	}
	if (song_artist_label) {
		lv_obj_set_style_text_color(song_artist_label, ink, 0);
		lv_obj_set_style_text_opa(song_artist_label, LV_OPA_80, 0);
	}
	update_fav_button();
	alt_pills_sync();
	paint_transport_tint();

	wave_drawn = -1; // the bars are the old colour
}

// Whether what is playing is an ordinary file on the card.
//
// Asked of the state and the caches rather than of a flag raised at the start,
// for the reason written at apply_audiobook_mode(): a flag has to be cleared on
// every route by which the track can change, and the first route forgotten
// leaves the player dressed as the wrong thing.
//
// Everything that is not a file on the card is somebody else's audio passing
// through: the radio has no length, Qobuz, Tidal, a podcast and a phone pushing
// over DLNA all play out of a cache folder that is emptied behind them, and
// AirPlay does not go through the decoder at all -- during an AirPlay session
// `current_file` still names whatever local track was loaded last, so it has to
// be asked separately rather than inferred from the path.
//
// An audiobook is the one exception that IS a file on the card. It is left out
// anyway: the page is already dressed as a book -- chapters instead of a queue,
// skips instead of tracks, no star -- and none of what the alternative
// arrangement adds means anything to one. A shape drawn across nine hours of
// speech is a flat stripe, and the nine hours would have to be decoded to find
// that out.
static bool playing_local_file(const device_state_t *state) {
	if (state->live || !state->current_file[0] || audiobook_is_playing()) {
		return false;
	}
	airplay_state_t air;
	airplay_get_state(&air);
	if (air.playing) {
		return false;
	}
	const char *f = state->current_file;
	return !qobuzcache_owns(f) && !tidalcache_owns(f) && !podcastcache_is_episode(f) && !dlna_owns_path(f) &&
		   !dlna_owns_playback();
}

// Puts the progress slider over the waveform, or back where it belongs.
//
// The slider is the only thing on the page that knows how to turn a finger into
// a seek -- the waveform is a picture and nothing more -- so the alternative
// layout must not hide it. It moves inside the waveform's box instead,
// stretched over the whole of it so that a press anywhere on the bars is a
// press on the slider, with every part of it drawn as nothing. What is seen is
// the waveform; what is dragged is the slider.
// Leaves the shape of the track, where the panel above it overlaps it, to the
// slider laid over it.
static void cover_panel_hit_test_cb(lv_event_t *e) {
	lv_hit_test_info_t *info = lv_event_get_hit_test_info(e);
	if (!info || !wave_box || lv_obj_is_hidden(wave_box)) {
		return;
	}
	lv_area_t wave;
	lv_obj_get_coords(wave_box, &wave);
	const lv_point_t *p = info->point;
	if (p->x >= wave.x1 && p->x <= wave.x2 && p->y >= wave.y1 && p->y <= wave.y2) {
		info->res = false;
	}
}

static void slider_over_waveform(bool over) {
	if (!progress_slider) {
		return;
	}

	if (over && wave_box) {
		lv_obj_set_parent(progress_slider, wave_box);
		lv_obj_set_size(progress_slider, lv_pct(100), WAVE_HEIGHT);
		lv_obj_align(progress_slider, LV_ALIGN_CENTER, 0, 0);
		lv_obj_set_style_bg_opa(progress_slider, LV_OPA_TRANSP, LV_PART_MAIN);
		lv_obj_set_style_bg_opa(progress_slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
		lv_obj_set_style_opa(progress_slider, LV_OPA_TRANSP, LV_PART_KNOB);
		lv_obj_move_foreground(progress_slider); // over the canvas, which takes no presses anyway
		return;
	}

	if (lv_obj_get_parent(progress_slider) == wave_box) {
		lv_obj_set_parent(progress_slider, player_menu);
		lv_obj_move_to_index(progress_slider, lv_obj_get_index(wave_box) + 1);
	}
	lv_obj_set_size(progress_slider, lv_pct(100), PROGRESS_TRACK_HEIGHT);
	lv_obj_remove_local_style_prop(progress_slider, LV_STYLE_BG_OPA, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(progress_slider, LV_OPA_30, LV_PART_MAIN);
	lv_obj_remove_local_style_prop(progress_slider, LV_STYLE_BG_OPA, LV_PART_INDICATOR);
	lv_obj_remove_local_style_prop(progress_slider, LV_STYLE_OPA, LV_PART_KNOB);
}

// ---------------------------------------------------------------------------
// Studio: the sleeve in the middle, the blurred artwork behind the whole screen
//
// Nothing here is drawn by hand. The sleeve is the picture the page already
// decoded, shown at the size this arrangement wants, and the background is the
// blurred copy the page already makes for behind the controls, asked for at the
// size of the whole screen instead. What this section owns is where they go and
// when.
// ---------------------------------------------------------------------------

#define STUDIO_MARGIN ui_px(14)	  // what the sleeve keeps to the side edges
#define STUDIO_HEAD_H ui_px(78)	  // the title, the artist and the ellipsis
#define STUDIO_COVER_GAP ui_px(24)	  // between the head and the top of the sleeve
#define STUDIO_BADGE_GAP ui_px(6)	  // between the head and the source mark under the ellipsis
#define STUDIO_QUALITY_GAP ui_px(10) // between the sleeve and the line under it
#define STUDIO_QUALITY_H ui_px(30)
#define STUDIO_BOTTOM ui_px(10) // under that line, before the controls begin
#define STUDIO_COVER_MAX_PCT 83

static lv_obj_t *studio_bg;		  // the blurred sleeve, the size of the screen
static lv_obj_t *studio_box;	  // what everything else is laid out on
static lv_obj_t *studio_head;	  // title and artist across the top
static lv_obj_t *studio_text_col; // the two of them, stacked and centred
static lv_obj_t *studio_cover;
static lv_obj_t *studio_empty;		  // the square shown where a track has no artwork
static lv_obj_t *studio_empty_icon;
static lv_obj_t *studio_quality;	  // the icon and the format line under the sleeve
static lv_obj_t *studio_quality_icon;
static bool studio_up;
static int studio_box_w, studio_box_h; // the panel this arrangement is laid out on
static int menu_pad_ver; // the controls block's spacing, see PLAYER_MENU_REF_HEIGHT
static int menu_gap;
static int studio_cover_size;

// ---------------------------------------------------------------------------
// Lyrics
//
// For a file on the card, a page of words beside the sleeve: a finger
// drawing the artwork from right to left pulls the words in as the sleeve
// goes out, and drawing them back from left to right brings the sleeve back.
// The bar and the buttons stay exactly where they are. Everything else turns
// into Studio's look as the words come in: the blurred sleeve behind the whole
// screen, and the title and artist at the top. That blur is made in the
// background as soon as the artwork arrives, so it is there before the first
// swipe; Studio already has it, and there the words only take the sleeve's
// place.
//
// Timed lyrics follow the music, the line being sung lit in the accent colour
// and held in the middle of the column, the words moving up under it one line
// at a time; a finger scrolling them takes over for a few seconds, and a tap
// on a line plays from there. The column fades out at its top and bottom
// edges the way a scrolling title does at its ends.
//
// The swipe is there on every file, words or not. On one without, it asks
// whether to look for them on LRCLIB (lrclib.h); what is found is listed to
// pick from (lyricsearch.h), the one picked is saved beside the file, and the
// page comes in with it.
// ---------------------------------------------------------------------------

#define LYRICS_FADE_PX ui_px(56)
#define LYRICS_LINE_GAP ui_px(16)
#define LYRICS_DIM_OPA LV_OPA_40
#define LYRICS_USER_HOLD_MS 4000
#define LYRICS_STEP_MS 320 // one line moving up into the middle
#define LYRICS_SLIDE_MS 220
// The page comes or goes once the finger has carried it this fraction of the
// way; short of it, it goes back.
#define LYRICS_COMMIT_FRACTION 4
#define LYRICS_DRAG_PX ui_px(10) // sideways past this, a swipe and not a tap
// Behind the words on a track with no artwork: dark in either theme, since
// the words are white.
#define LYRICS_FULL 1000 // lyrics_progress with the page all the way in

static bool lyrics_wanted;  // pulled in by a swipe
static bool lyrics_allowed; // wanted, and a file on the card playing
static bool lyrics_now;		// on screen: allowed, and the file has words
static lv_obj_t *lyrics_pane; // the page: the column and the note
static lv_obj_t *lyrics_head; // the title and artist at the top while the words are up
static lv_obj_t *lyrics_head_col;
static lv_obj_t *lyrics_head_title;
static lv_obj_t *lyrics_head_artist;
static cover_image_t lyrics_screen_pic; // the blurred sleeve at the shape of the screen
static unsigned lyrics_bg_generation;
static int32_t lyrics_progress; // 0 the sleeve, LYRICS_FULL the words
// A line tapped: where it plays from, and until when the column waits for the
// player to get there rather than follow the position from before the jump.
static int32_t lyrics_tap_ms = -1;
static uint32_t lyrics_tap_until;
#define LYRICS_TAP_WAIT_MS 2000
static int lyrics_pane_x;		// where the page rests
static lv_obj_t *lyrics_view;
static lv_obj_t *lyrics_note; // "no lyrics", in the middle of the column
static lyrics_t lyrics_cur;
static bool lyrics_loaded;		   // lyrics_cur belongs to lyrics_path, found or not
static char lyrics_path[1024];	   // the file the view is for
static unsigned lyrics_generation; // stale answers from the loader are dropped
static int lyrics_lit = -1;		   // the line drawn as being sung
static uint32_t lyrics_user_until; // no following until then: a finger is reading
static bool lyrics_centred;		   // the lit line is where it belongs
static lv_image_dsc_t lyrics_mask;
static uint8_t *lyrics_mask_pixels;
static int lyrics_view_h;

// Timed lines have room above the first and below the last, so either can be
// brought to the middle; untimed ones read from the top.
static void lyrics_pad(void) {
	int around = lyrics_cur.synced ? lyrics_view_h / 2 - ui_px(20) : LYRICS_FADE_PX / 2;
	lv_obj_set_style_pad_top(lyrics_view, around, 0);
	lv_obj_set_style_pad_bottom(lyrics_view, lyrics_cur.synced ? around : LYRICS_FADE_PX, 0);
}

// An A8 mask the size of the column, opaque but for a ramp at the top and at
// the bottom. Built again only when the size changes.
// The fade is on when the page wants it and nothing slides over the column:
// under a moving control centre or screensaver the masked layer would be
// rebuilt on every frame of the slide (see lyrics_fade).
static bool lyrics_fade_wanted = true;
static unsigned lyrics_fade_covers;

static void lyrics_fade_apply(void) {
	if (lyrics_view && lyrics_mask_pixels) {
		lv_obj_set_style_bitmap_mask_src(lyrics_view, lyrics_fade_wanted && !lyrics_fade_covers ? &lyrics_mask : NULL, 0);
	}
}

void player_lyrics_covered(unsigned who, bool covered) {
	lyrics_fade_covers = covered ? lyrics_fade_covers | who : lyrics_fade_covers & ~who;
	lyrics_fade_apply();
}

static void lyrics_mask_build(int w, int h) {
	if (lyrics_mask_pixels && (int)lyrics_mask.header.w == w && (int)lyrics_mask.header.h == h) {
		return;
	}
	if (lyrics_view) {
		lv_obj_set_style_bitmap_mask_src(lyrics_view, NULL, 0);
	}
	free(lyrics_mask_pixels);
	lyrics_mask_pixels = NULL;
	if (w <= 0 || h <= 2 * LYRICS_FADE_PX) {
		return;
	}
	lyrics_mask_pixels = malloc((size_t)w * (size_t)h);
	if (!lyrics_mask_pixels) {
		return;
	}
	for (int y = 0; y < h; y++) {
		int edge = y < h - 1 - y ? y : h - 1 - y;
		uint8_t a = edge >= LYRICS_FADE_PX ? 255 : (uint8_t)(edge * 255 / LYRICS_FADE_PX);
		memset(lyrics_mask_pixels + (size_t)y * (size_t)w, a, (size_t)w);
	}
	memset(&lyrics_mask, 0, sizeof(lyrics_mask));
	lyrics_mask.header.magic = LV_IMAGE_HEADER_MAGIC;
	lyrics_mask.header.cf = LV_COLOR_FORMAT_A8;
	lyrics_mask.header.w = (uint32_t)w;
	lyrics_mask.header.h = (uint32_t)h;
	lyrics_mask.header.stride = (uint32_t)w;
	lyrics_mask.data = lyrics_mask_pixels;
	lyrics_mask.data_size = (uint32_t)((size_t)w * (size_t)h);
	lyrics_fade_apply();
}

static void lyrics_note_show(const char *tag) {
	if (!lyrics_note) {
		return;
	}
	if (tag) {
		lv_label_set_text(lyrics_note, tr(tag));
		lv_obj_set_hidden(lyrics_note, false);
	} else {
		lv_obj_set_hidden(lyrics_note, true);
	}
}

// The column's scroll position, for its step animation.
static void lyrics_scroll_exec(void *obj, int32_t y) { lv_obj_scroll_to_y(obj, y, LV_ANIM_OFF); }

// The column's lines, one label each, from lyrics_cur.
static void lyrics_fill(void) {
	if (!lyrics_view) {
		return;
	}
	lv_obj_clean(lyrics_view);
	lyrics_pad();
	lyrics_lit = -1;
	lyrics_user_until = 0;
	lyrics_centred = false;
	lv_anim_delete(lyrics_view, lyrics_scroll_exec);
	lv_obj_scroll_to_y(lyrics_view, 0, LV_ANIM_OFF);

	if (!lyrics_loaded) {
		lyrics_note_show(NULL); // still being read
		return;
	}
	if (lyrics_cur.count == 0) {
		lyrics_note_show("lyrics_none");
		return;
	}
	lyrics_note_show(NULL);

	for (int i = 0; i < lyrics_cur.count; i++) {
		lv_obj_t *line = lv_label_create(lyrics_view);
		lv_label_set_text(line, lyrics_cur.lines[i].text[0] ? lyrics_cur.lines[i].text : " ");
		lv_label_set_long_mode(line, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(line, lv_pct(100));
		lv_obj_set_style_text_align(line, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_font(line, &font_ui_26, 0);
		lv_obj_set_style_text_opa(line, lyrics_cur.synced ? LYRICS_DIM_OPA : LV_OPA_COVER, 0);
		lv_obj_set_event_bubble(line, true);
		lv_obj_set_clickable(line, false);
	}
}

typedef struct {
	unsigned generation;
	char path[1024];
	char title[256];
	lyrics_t lyrics;
	bool found;
} lyrics_job_t;

static void lyrics_settle(void);

// The column is built when it is about to be seen, not when the words arrive.
// They arrive with every track change, while the player is animating the new
// sleeve in, and a label per line -- forty for a song, each with its styles
// and its share of a relayout -- made one of the longest frames of the whole
// interface on the device, for a page that is mostly never opened. So the
// fill waits for a quiet moment, LYRICS_FILL_DELAY_MS on, or happens at once
// if the page starts to come in first (lyrics_progress_apply).
#define LYRICS_FILL_DELAY_MS 1200

static bool lyrics_fill_pending;
static lv_timer_t *lyrics_fill_timer;

static void lyrics_fill_now(void) {
	lyrics_fill_pending = false;
	if (lyrics_fill_timer) {
		lv_timer_pause(lyrics_fill_timer);
	}
	lyrics_fill();
	lyrics_settle();
}

static void lyrics_fill_timer_cb(lv_timer_t *timer) {
	(void)timer;
	if (lyrics_fill_pending) {
		lyrics_fill_now();
	}
}

static void lyrics_fill_soon(void) {
	if (lyrics_progress > 0) {
		lyrics_fill_now(); // the page is up: no waiting
		return;
	}
	lyrics_fill_pending = true;
	if (!lyrics_fill_timer) {
		lyrics_fill_timer = lv_timer_create(lyrics_fill_timer_cb, LYRICS_FILL_DELAY_MS, NULL);
	}
	lv_timer_set_period(lyrics_fill_timer, LYRICS_FILL_DELAY_MS);
	lv_timer_reset(lyrics_fill_timer);
	lv_timer_resume(lyrics_fill_timer);
}

static void lyrics_arrived(void *user) {
	lyrics_job_t *job = user;
	if (job->generation == lyrics_generation && strcmp(job->path, lyrics_path) == 0) {
		lyrics_free(&lyrics_cur);
		lyrics_cur = job->lyrics; // handed over, found or empty
		lyrics_loaded = true;
		lyrics_fill_soon();
	} else {
		lyrics_free(&job->lyrics);
	}
	free(job);
}

static void *lyrics_worker(void *arg) {
	lyrics_job_t *job = arg;
	job->found = lyrics_load(job->path, job->title, &job->lyrics);
	if (!gui_post(lyrics_arrived, job)) {
		lyrics_free(&job->lyrics);
		free(job);
	}
	return NULL;
}

// Only a file on the card has words to show.
static bool lyrics_available(void) {
	device_state_t state;
	device_state_get(&state);
	return playing_local_file(&state);
}

// The page is up, or on its way in or out under a finger.
static bool lyrics_on_screen(void) { return lyrics_pane && !lv_obj_is_hidden(lyrics_pane); }

// Words being read are not an idle screen: while the lyrics are up on an open
// player, the idle timer does not blank the panel under them. Asked again
// wherever either half changes; the hold itself ignores a call that changes
// nothing.
static void lyrics_screen_hold_update(void) { power_hold_screen_for_view(sheet_open && lyrics_on_screen()); }

// The words of the file playing are known before any swipe: on a track with
// none the swipe asks to download them, and the page goes away by itself on
// one.
static bool lyrics_has_words(void) { return lyrics_loaded && lyrics_cur.count > 0; }

// The lyrics of the file playing, read on a thread of their own as soon as it
// starts: the tags of a FLAC are read whole, pictures included, and that is
// not the interface's to wait for. Asked again only when the file changes.
static void lyrics_request(const device_state_t *state) {
	if (!state->current_file[0] || !playing_local_file(state)) {
		return;
	}
	if (strcmp(state->current_file, lyrics_path) == 0) {
		return;
	}
	snprintf(lyrics_path, sizeof(lyrics_path), "%s", state->current_file);
	lyrics_free(&lyrics_cur);
	lyrics_loaded = false;
	lyrics_generation++;
	lyrics_fill_soon();

	lyrics_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	job->generation = lyrics_generation;
	snprintf(job->path, sizeof(job->path), "%s", state->current_file);
	snprintf(job->title, sizeof(job->title), "%s", state->metadata.title);
	pthread_t thread;
	if (pthread_create(&thread, NULL, lyrics_worker, job) == 0) {
		pthread_detach(thread);
	} else {
		free(job);
	}
}

// Brings `label` to the middle of the column, always in the same short time
// whatever the distance, so the words step up like a typewriter's paper.
static void lyrics_centre(lv_obj_t *label) {
	lv_obj_update_layout(lyrics_view);
	lv_area_t line_area;
	lv_area_t view_area;
	lv_obj_get_coords(label, &line_area);
	lv_obj_get_coords(lyrics_view, &view_area);
	int32_t from = lv_obj_get_scroll_y(lyrics_view);
	int32_t to = from + (line_area.y1 + line_area.y2) / 2 - (view_area.y1 + view_area.y2) / 2;
	if (to < 0) {
		to = 0;
	}

	lv_anim_delete(lyrics_view, lyrics_scroll_exec);
	if (to == from) {
		return;
	}
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, lyrics_view);
	lv_anim_set_exec_cb(&a, lyrics_scroll_exec);
	lv_anim_set_values(&a, from, to);
	lv_anim_set_duration(&a, LYRICS_STEP_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

static void lyrics_paint_line(int index, bool lit) {
	if (!lyrics_view || index < 0 || index >= (int)lv_obj_get_child_count(lyrics_view)) {
		return;
	}
	lv_obj_t *label = lv_obj_get_child(lyrics_view, index);
	// Unlit, the line wears the column's colour (see lyrics_look()).
	if (lit) {
		lv_obj_set_style_text_color(label, chrome_accent(), 0);
	} else {
		lv_obj_remove_local_style_prop(label, LV_STYLE_TEXT_COLOR, 0);
	}
	lv_obj_set_style_text_opa(label, lit ? LV_OPA_COVER : LYRICS_DIM_OPA, 0);
}

static void lyrics_repaint_lit(void) { lyrics_paint_line(lyrics_lit, true); }

// The line being sung, lit and held in the middle of the column. Once a
// finger has scrolled the words, the column comes back to it a few seconds
// after the finger stops -- or, with the music paused, once it plays again:
// until then the words stay where the finger left them.
static void lyrics_follow(double seconds) {
	if (!lyrics_on_screen() || !lyrics_view || !lyrics_cur.synced || lyrics_cur.count == 0) {
		return;
	}
	int32_t ms = (int32_t)(seconds * 1000.0);
	if (lyrics_tap_ms >= 0) {
		// Until the player reports a position near the tapped line, the one
		// it reports is from before the jump: following it would send the
		// column back there and then forward again.
		if ((int32_t)(lv_tick_get() - lyrics_tap_until) < 0 && LV_ABS(ms - lyrics_tap_ms) > 1500) {
			return;
		}
		lyrics_tap_ms = -1;
	}
	int line = lyrics_line_at(&lyrics_cur, ms);
	if (line != lyrics_lit) {
		lyrics_paint_line(lyrics_lit, false);
		lyrics_lit = line;
		lyrics_paint_line(line, true);
		lyrics_centred = false;
	}

	if (lyrics_centred) {
		return;
	}
	if (lyrics_user_until &&
		((int32_t)(lv_tick_get() - lyrics_user_until) < 0 || audio_get_status() != AUDIO_STATUS_PLAYING)) {
		return;
	}
	lyrics_user_until = 0;
	lyrics_centred = true;
	if (line >= 0 && line < (int)lv_obj_get_child_count(lyrics_view)) {
		lyrics_centre(lv_obj_get_child(lyrics_view, line));
	} else {
		lyrics_centre(lv_obj_get_child(lyrics_view, 0));
	}
}

static void lyrics_scroll_cb(lv_event_t *e) {
	(void)e;
	// A finger, not the column following the music: that scrolls from a timer.
	if (lv_indev_active()) {
		lv_anim_delete(lyrics_view, lyrics_scroll_exec);
		lyrics_user_until = lv_tick_get() + LYRICS_USER_HOLD_MS;
		lyrics_centred = false;
	}
}

// The blurred sleeve the words sit on: Studio's own, or the one made for the
// other two arrangements. NULL while there is none.
static const lv_image_dsc_t *lyrics_backdrop(void) {
	if (layout_studio_now) {
		return current_backdrop.pixels ? &current_backdrop.dsc : NULL;
	}
	return lyrics_screen_pic.pixels ? &lyrics_screen_pic.dsc : NULL;
}

// Keeps the title and artist at the top saying what the row below says.
static void lyrics_head_sync(void) {
	if (!lyrics_head || !song_title_label || !song_artist_label) {
		return;
	}
	scrolltext_set(lyrics_head_title, lv_label_get_text(song_title_label));
	scrolltext_set(lyrics_head_artist, lv_label_get_text(song_artist_label));
}

// Studio's look for the arrangements that are not Studio: its blurred
// sleeve behind the whole screen, and the surfaces that would cover it --
// the sleeve's panel and the controls' own blurred block -- made clear. Put on
// in one step when the words start to come in, not faded with the finger: a
// screen-sized blend redrawn on every frame of the swipe is what this
// processor cannot keep up with. Only with a picture to show; without one the
// page keeps its own background, whole, and the words take the theme's
// colours.
static bool lyrics_look_on;

static void lyrics_look(bool on) {
	if (layout_studio_now) {
		on = false; // Studio is the look already
	}
	const lv_image_dsc_t *backdrop = lyrics_backdrop();
	if (on && !backdrop) {
		on = false;
	}
	if (on) {
		lv_image_set_src(studio_bg, backdrop);
		lv_obj_set_hidden(studio_bg, false);
		lv_obj_move_background(studio_bg);
		lv_obj_set_style_bg_opa(cover_panel, LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_opa(player_menu, LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_image_opa(player_menu, LV_OPA_TRANSP, 0);
	} else if (lyrics_look_on && !layout_studio_now) {
		lv_obj_set_hidden(studio_bg, true);
		lv_obj_remove_local_style_prop(cover_panel, LV_STYLE_BG_OPA, 0);
		lv_obj_remove_local_style_prop(player_menu, LV_STYLE_BG_OPA, 0);
		lv_obj_remove_local_style_prop(player_menu, LV_STYLE_BG_IMAGE_OPA, 0);
	}
	lyrics_look_on = on;
	// The words are white over a blurred sleeve, and in the theme's own
	// colour over the page's own background where there is no picture.
	lv_color_t ink = lyrics_backdrop() ? lv_color_white() : theme()->text_primary;
	lv_obj_set_style_text_color(lyrics_view, ink, 0);
	lv_obj_set_style_text_color(lyrics_note, ink, 0);
	lv_obj_set_style_text_color(lyrics_head_title, ink, 0);
	lv_obj_set_style_text_color(lyrics_head_artist, lyrics_backdrop() ? lv_color_make(200, 200, 200) : theme()->text_secondary, 0);
}

// What travels with the sleeve: it goes out to the left as the words come in.
static void lyrics_shift(lv_obj_t *obj, int32_t x) {
	if (!obj) {
		return;
	}
	if (x == 0) {
		lv_obj_remove_local_style_prop(obj, LV_STYLE_TRANSLATE_X, 0);
	} else {
		lv_obj_set_style_translate_x(obj, x, 0);
	}
}

// The star of the arrangement with the shape of the track, which lives in a
// disc on the sleeve: while the words are up it moves to the head, at the
// right of the names, and back into its disc when they go.
static bool lyrics_star_up;

static void lyrics_star_to_head(bool up) {
	if (up == lyrics_star_up || !fav_btn_obj || !alt_fav_circle) {
		return;
	}
	lyrics_star_up = up;
	if (up) {
		lv_obj_set_parent(fav_btn_obj, lyrics_head);
		lv_obj_align(fav_btn_obj, LV_ALIGN_RIGHT_MID, 0, 0);
	} else {
		lv_obj_set_parent(fav_btn_obj, alt_fav_circle);
		lv_obj_center(fav_btn_obj);
	}
}

// The button at the top right while the words are all the way in: it looks them
// up on LRCLIB again, to pick others in their place. Beside the star or the
// ellipsis where an arrangement keeps one up there, in the corner otherwise.
#define LYRICS_FIND_BTN ui_px(56)
static lv_obj_t *lyrics_find_btn;
static lv_obj_t *lyrics_find_icon;
static lv_obj_t *lyrics_find_narrowed; // the names' column, made room in

// The names at the top give up the button's width on both sides, so a long
// title stays centred and stops short of it.
static void lyrics_find_make_room(lv_obj_t *col) {
	if (col == lyrics_find_narrowed) {
		return;
	}
	if (lyrics_find_narrowed) {
		int32_t w = lv_obj_get_style_width(lyrics_find_narrowed, 0);
		lv_obj_set_width(lyrics_find_narrowed, w + 2 * LYRICS_FIND_BTN);
	}
	if (col) {
		int32_t w = lv_obj_get_style_width(col, 0);
		lv_obj_set_width(col, w - 2 * LYRICS_FIND_BTN);
	}
	lyrics_find_narrowed = col;
}

static void lyrics_find_sync(bool full) {
	if (!lyrics_find_btn) {
		return;
	}
	bool show = full && lyrics_now;
	if (!show) {
		lv_obj_set_hidden(lyrics_find_btn, true);
		lyrics_find_make_room(NULL);
		return;
	}
	int width = layout_studio_now ? studio_box_w : cover_box_w;
	int beside = 0;
	if (layout_studio_now && more_btn_obj) {
		beside = lv_obj_get_width(more_btn_obj);
	} else if (layout_alt_now && fav_btn_obj) {
		beside = lv_obj_get_width(fav_btn_obj);
	}
	lv_obj_set_pos(lyrics_find_btn, width - STUDIO_MARGIN - beside - LYRICS_FIND_BTN, back_btn_centre_y() - LYRICS_FIND_BTN / 2);
	lyrics_find_make_room(layout_studio_now ? studio_text_col : layout_alt_now ? lyrics_head_col : NULL);
	// The ink of the names beside it: white over the blurred sleeve, the
	// theme's own over the page's background.
	lv_obj_set_style_image_recolor(lyrics_find_icon, lyrics_backdrop() ? lv_color_white() : theme()->text_primary, 0);
	lv_obj_set_hidden(lyrics_find_btn, false);
	lv_obj_move_foreground(lyrics_find_btn);
}

// Everything the swipe moves, at `progress` of the way from the sleeve (0)
// to the words (LYRICS_FULL). Only positions change from one frame to the
// next; at 0 every style touched is handed back, so the arrangement is
// exactly what it was before the first swipe.
static void lyrics_progress_apply(int32_t progress) {
	if (!lyrics_pane) {
		return;
	}
	if (progress < 0) {
		progress = 0;
	}
	if (progress > LYRICS_FULL) {
		progress = LYRICS_FULL;
	}
	if (progress > 0 && lyrics_fill_pending) {
		lyrics_progress = progress; // so the fill does not defer itself again
		lyrics_fill_now();
	}
	lyrics_progress = progress;
	lyrics_find_sync(progress == LYRICS_FULL);
	bool rest = progress == 0;
	int32_t travel = cover_box_w - lyrics_pane_x;

	if (rest) {
		lv_obj_set_hidden(lyrics_pane, true);
	} else if (lv_obj_is_hidden(lyrics_pane)) {
		lv_obj_set_hidden(lyrics_pane, false);
		lv_obj_move_foreground(lyrics_pane);
	}
	lyrics_screen_hold_update();
	lv_obj_set_x(lyrics_pane, lyrics_pane_x + travel * (LYRICS_FULL - progress) / LYRICS_FULL);
	if (rest || !lyrics_look_on) {
		lyrics_look(!rest);
	}

	int32_t away = -(int32_t)cover_box_w * progress / LYRICS_FULL;
	if (layout_studio_now) {
		lyrics_shift(studio_cover, away);
		lyrics_shift(studio_empty, away);
		lyrics_shift(studio_quality, away);
		return;
	}
	lyrics_shift(cover_img, away);
	lyrics_shift(cover_placeholder_icon, away);
	if (!layout_alt_now) {
		return; // the names stay in their row below the sleeve
	}

	// With the shape of the track the names are pills on the sleeve, so they
	// go with it, and come up at the top once the words are in.
	lyrics_shift(alt_text_col, away);
	lyrics_shift(alt_fav_circle, away);
	bool full = progress == LYRICS_FULL;
	lyrics_star_to_head(full);
	if (full) {
		lyrics_head_sync();
		lv_obj_set_hidden(lyrics_head, false);
		lv_obj_move_foreground(lyrics_head);
		if (lyrics_find_btn && !lv_obj_is_hidden(lyrics_find_btn)) {
			lv_obj_move_foreground(lyrics_find_btn);
		}
	} else {
		lv_obj_set_hidden(lyrics_head, true);
	}
}

// While the page moves, the fade at its edges is left off: it is drawn by
// rendering the column into a layer of its own and masking it, which is fine
// once a scroll step and far too much for every frame of a swipe.
static void lyrics_fade(bool on) {
	lyrics_fade_wanted = on;
	lyrics_fade_apply();
}

static void lyrics_progress_exec(void *obj, int32_t value) {
	(void)obj;
	lyrics_progress_apply(value);
}

static void lyrics_slid_cb(lv_anim_t *a) {
	(void)a;
	lyrics_fade(true);
}

static void lyrics_slide_to(int32_t progress) {
	lv_anim_delete(lyrics_pane, lyrics_progress_exec);
	if (lyrics_progress == progress) {
		lyrics_progress_apply(progress);
		lyrics_fade(true);
		return;
	}
	lyrics_fade(false);
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, lyrics_pane);
	lv_anim_set_exec_cb(&a, lyrics_progress_exec);
	lv_anim_set_values(&a, lyrics_progress, progress);
	lv_anim_set_duration(&a, LYRICS_SLIDE_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, lyrics_slid_cb);
	lv_anim_start(&a);
}

// The head's place: Studio's, between the chevron and the right edge.
static int lyrics_head_top(void) {
	int head_top = back_btn_centre_y() - STUDIO_HEAD_H / 2;
	return head_top < 2 ? 2 : head_top;
}

// Where the page goes in the arrangement on show: in Studio from under the
// head to above the controls, over the sleeve; with the shape of the track
// from under the head that takes the names; in the standard arrangement from
// under the chevron. Outside Studio the page covers the whole square.
static void lyrics_layout(void) {
	if (!lyrics_pane) {
		return;
	}
	int head_bottom = lyrics_head_top() + STUDIO_HEAD_H + STUDIO_BADGE_GAP;
	int top = layout_alt_now || layout_studio_now ? head_bottom : back_btn_centre_y() + ui_px(28);
	int pane_y = 0;
	int x = 0, w = cover_box_w, h = cover_box_h - top;
	if (layout_studio_now) {
		pane_y = top;
		int bottom = studio_box_h - STUDIO_BOTTOM;
		if (layout_wave_now && wave_canvas) {
			bottom -= WAVE_HEIGHT - PROGRESS_TRACK_HEIGHT;
		}
		x = STUDIO_MARGIN;
		w = studio_box_w - 2 * STUDIO_MARGIN;
		h = bottom - top;
	}
	lyrics_pane_x = x;
	int view_x = layout_studio_now ? 0 : STUDIO_MARGIN;
	int view_y = top - pane_y;
	int view_w = w - 2 * view_x;

	// Into the new arrangement with the styles of the old one handed back.
	lv_anim_delete(lyrics_pane, lyrics_progress_exec);
	lyrics_progress_apply(0);
	// apply_layout() has just put the star where this arrangement keeps it,
	// and the head belongs to the one before.
	lyrics_star_up = false;
	if (lyrics_head) {
		lv_obj_set_hidden(lyrics_head, true);
	}

	lv_obj_set_y(lyrics_pane, pane_y);
	lv_obj_set_size(lyrics_pane, w, h + view_y);
	lv_obj_set_pos(lyrics_view, view_x, view_y);
	lv_obj_set_size(lyrics_view, view_w, h);
	lyrics_view_h = h;
	lyrics_pad();
	lyrics_mask_build(view_w, h);
	lv_obj_set_size(lyrics_note, view_w, LV_SIZE_CONTENT);
	lv_obj_set_pos(lyrics_note, view_x, view_y + h / 2 - ui_px(20));

	if (lyrics_head) {
		lv_obj_set_pos(lyrics_head, STUDIO_MARGIN, lyrics_head_top());
		lv_obj_set_size(lyrics_head, cover_box_w - 2 * STUDIO_MARGIN, STUDIO_HEAD_H);
	}
	lyrics_progress_apply(lyrics_now ? LYRICS_FULL : 0);
}

// Brings the page up, or puts it away, and remembers it either way: it stays
// up from one track to the next.
static void lyrics_commit(bool on) {
	lyrics_wanted = on;
	device_state_t state;
	device_state_get(&state);
	lyrics_allowed = on && playing_local_file(&state);
	lyrics_now = lyrics_allowed && lyrics_has_words();
	if (lyrics_now) {
		lyrics_slide_to(LYRICS_FULL);
		lyrics_request(&state);
		lyrics_follow(state.progress_current_secs);
	} else {
		lyrics_slide_to(0);
	}
}

// The words for the file playing have been read: a page wanted and up stays
// up if there are any and goes if there are none; one wanted and put away for
// a track without words comes back on the first track that has some.
static void lyrics_settle(void) {
	bool now = lyrics_allowed && lyrics_has_words();
	if (now == lyrics_now || !lyrics_pane) {
		return;
	}
	lyrics_now = now;
	lyrics_slide_to(now ? LYRICS_FULL : 0);
	if (now) {
		device_state_t state;
		device_state_get(&state);
		lyrics_follow(state.progress_current_secs);
	}
}

// ---------------------------------------------------------------------------
// Words from LRCLIB, for a file with none
// ---------------------------------------------------------------------------

typedef struct {
	char path[1024];
	char title[256];
	char artist[256];
	char album[256];
	int duration;
	lrclib_result_t result;
	lrclib_record_t *records;
	int count;
} lyrics_fetch_t;

static bool lyrics_fetching; // one search at a time

void player_lyrics_reload(const char *path) {
	device_state_t state;
	device_state_get(&state);
	if (!path || strcmp(state.current_file, path) != 0) {
		return;
	}
	// Read again, from the .lrc now beside the file. Words that were up stay up
	// and change in place; otherwise the page is wanted, and comes in when the
	// words arrive (lyrics_settle()).
	bool up = lyrics_now;
	lyrics_path[0] = '\0';
	lyrics_request(&state);
	if (!up) {
		lyrics_commit(true);
	}
}

static void lyrics_fetched(void *user) {
	lyrics_fetch_t *job = user;
	lyrics_fetching = false;
	toast_busy_end();

	switch (job->result) {
	case LRCLIB_OK: {
		// The list is for the track it was asked about, over the player it was
		// asked from; with either gone it is no longer wanted.
		device_state_t state;
		device_state_get(&state);
		if (sheet_open && strcmp(state.current_file, job->path) == 0) {
			lyricsearch_open(job->path, job->records, job->count);
			job->records = NULL;
		}
		break;
	}
	case LRCLIB_INSTRUMENTAL:
		toast_plain("lyrics_instrumental");
		break;
	case LRCLIB_NOT_FOUND:
		toast_plain("lyrics_not_found");
		break;
	case LRCLIB_NO_TAGS:
		toast_error("lyrics_no_tags");
		break;
	default:
		toast_error("lyrics_download_failed");
		break;
	}
	lrclib_free(job->records, job->count);
	free(job);
}

static void *lyrics_fetch_worker(void *arg) {
	lyrics_fetch_t *job = arg;
	job->result = lrclib_search(job->title, job->artist, job->album, job->duration, &job->records, &job->count);
	if (!gui_post(lyrics_fetched, job)) {
		lrclib_free(job->records, job->count);
		free(job);
	}
	return NULL;
}

static void lyrics_search_run(void);

// ---------------------------------------------------------------------------
// Wi-Fi switched on for a search
//
// With Wi-Fi off the question offers to switch it on instead of searching; the
// search then starts by itself once the link has an address, for the file that
// was playing when the question was answered.
// ---------------------------------------------------------------------------

#define LYRICS_WIFI_POLL_MS 500
#define LYRICS_WIFI_WAIT_MS 30000

static lv_timer_t *lyrics_wifi_timer;
static uint32_t lyrics_wifi_since;
static char lyrics_wifi_path[1024];

static bool lyrics_wifi_online(void) {
	wifi_status_t wifi;
	wifi_get_status(&wifi);
	return wifi.state == WIFI_STATE_CONNECTED && wifi.ip[0] != '\0';
}

static void lyrics_wifi_wait_end(void) {
	if (lyrics_wifi_timer) {
		lv_timer_delete(lyrics_wifi_timer);
		lyrics_wifi_timer = NULL;
	}
}

static void lyrics_wifi_tick_cb(lv_timer_t *t) {
	(void)t;
	device_state_t state;
	device_state_get(&state);
	// Another track, or Wi-Fi switched off again: the search is no longer wanted.
	if (!wifi_get_enabled() || strcmp(state.current_file, lyrics_wifi_path) != 0) {
		lyrics_wifi_wait_end();
		toast_busy_end();
		return;
	}
	if (lyrics_wifi_online()) {
		lyrics_wifi_wait_end();
		toast_busy_end();
		lyrics_search_run();
		return;
	}
	if (lv_tick_elaps(lyrics_wifi_since) >= LYRICS_WIFI_WAIT_MS) {
		lyrics_wifi_wait_end();
		toast_busy_end();
		toast_error("lyrics_wifi_failed");
	}
}

static void lyrics_enable_wifi_cb(void *user) {
	(void)user;
	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0] || !playing_local_file(&state)) {
		return;
	}
	snprintf(lyrics_wifi_path, sizeof(lyrics_wifi_path), "%s", state.current_file);
	wifi_set_enabled(true);
	lyrics_wifi_since = lv_tick_get();
	if (!lyrics_wifi_timer) {
		lyrics_wifi_timer = lv_timer_create(lyrics_wifi_tick_cb, LYRICS_WIFI_POLL_MS, NULL);
	}
	toast_busy_dismissable("lyrics_wifi_connecting");
}

// Wi-Fi is off and can be switched on: the question offers that in place of
// the search.
static bool lyrics_offer_wifi(void) {
	if (lyrics_wifi_timer) {
		toast_busy_dismissable("lyrics_wifi_connecting");
		return true;
	}
	if (wifi_get_enabled() || !wifi_available()) {
		return false;
	}
	confirm_show("lyrics_download_title", "lyrics_wifi_off_note", "lyrics_enable_wifi", lyrics_enable_wifi_cb, NULL);
	return true;
}

// The file playing looked up on LRCLIB, on a thread of its own; what is found
// is listed to pick from.
static void lyrics_search_start(void) {
	if (!lyrics_fetching && lyrics_offer_wifi()) {
		return;
	}
	lyrics_search_run();
}

static void lyrics_search_run(void) {
	if (lyrics_fetching) {
		toast_busy_dismissable("lyrics_downloading");
		return;
	}
	wifi_status_t wifi;
	wifi_get_status(&wifi);
	if (wifi.state != WIFI_STATE_CONNECTED) {
		toast_error("lyrics_no_wifi");
		return;
	}
	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0] || !playing_local_file(&state)) {
		return;
	}

	lyrics_fetch_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	snprintf(job->path, sizeof(job->path), "%s", state.current_file);
	snprintf(job->title, sizeof(job->title), "%s", state.metadata.title);
	// The track's own performer is who LRCLIB files it under; the album's
	// artist only when the track names none.
	snprintf(job->artist, sizeof(job->artist), "%s", state.metadata.artist[0] ? state.metadata.artist : state.metadata.album_artist);
	snprintf(job->album, sizeof(job->album), "%s", state.metadata.album);
	job->duration = (int)(state.progress_total_secs + 0.5);

	pthread_t thread;
	if (pthread_create(&thread, NULL, lyrics_fetch_worker, job) != 0) {
		free(job);
		return;
	}
	pthread_detach(thread);
	lyrics_fetching = true;
	// A tap puts the spinner away while LRCLIB is asked again after a failure;
	// the list still opens when the answer comes.
	toast_busy_dismissable("lyrics_downloading");
}

// The answer to the question below.
static void lyrics_download_cb(void *user) {
	(void)user;
	lyrics_search_start();
}

// The button over the words: other words for the same file, in their place.
static void lyrics_find_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active()) {
		return;
	}
	lyrics_search_start();
}

// A swipe to the words of a file that has none.
static void lyrics_offer_download(void) {
	if (lyrics_fetching) {
		toast_busy_dismissable("lyrics_downloading");
		return;
	}
	if (lyrics_offer_wifi()) {
		return;
	}
	confirm_show("lyrics_download_title", "lyrics_download_note", "lyrics_download", lyrics_download_cb, NULL);
}

// The swipe that pulls the page in (on the sleeve, right to left) or pushes it
// out (on the words, left to right). `opening` is the user data. Sideways
// only: a movement that reads as vertical is the column being scrolled, and
// a swipe the other way on the sleeve is the player being put away.
static bool lyrics_swiped; // the release that ends a swipe is not a tap on a line

static void lyrics_swiped_clear_cb(void *unused) {
	(void)unused;
	lyrics_swiped = false;
}

static void lyrics_drag_cb(lv_event_t *e) {
	static lv_point_t start;
	static bool tracking;
	static bool engaged;
	static bool asking; // no words to pull in: the swipe offers to download them

	bool opening = (bool)(uintptr_t)lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_active();
	if (!indev || !lyrics_pane) {
		return;
	}

	if (code == LV_EVENT_PRESSED) {
		tracking = opening ? !lyrics_now && lyrics_available() && lyrics_loaded : lyrics_now;
		asking = opening && !lyrics_has_words();
		engaged = false;
		lv_indev_get_point(indev, &start);
		return;
	}
	if (!tracking) {
		return;
	}

	lv_point_t p;
	lv_indev_get_point(indev, &p);
	int dx = p.x - start.x;
	int dy = p.y - start.y;

	if (code == LV_EVENT_PRESSING) {
		if (!engaged) {
			if (lv_indev_get_scroll_obj(indev)) {
				tracking = false; // the column is being scrolled
				return;
			}
			if (LV_ABS(dy) >= LYRICS_DRAG_PX && LV_ABS(dy) > LV_ABS(dx)) {
				tracking = false;
				return;
			}
			if (LV_ABS(dx) < LYRICS_DRAG_PX || LV_ABS(dx) < 2 * LV_ABS(dy)) {
				return;
			}
			if ((opening && dx > 0) || (!opening && dx < 0)) {
				tracking = false; // the other way: not this gesture
				return;
			}
			engaged = true;
			lyrics_swiped = true;
			if (asking) {
				return; // the sleeve stays: there is no page to bring
			}
			lv_anim_delete(lyrics_pane, lyrics_progress_exec);
			lyrics_fade(false);
			if (opening) {
				lyrics_progress_apply(1); // up, so the request below finds it on screen
				device_state_t state;
				device_state_get(&state);
				lyrics_request(&state);
				lyrics_follow(state.progress_current_secs);
			}
		}
		if (asking) {
			return;
		}
		int32_t moved = (int32_t)dx * LYRICS_FULL / cover_box_w;
		lyrics_progress_apply(opening ? -moved : LYRICS_FULL - moved);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		tracking = false;
		if (!engaged) {
			return;
		}
		engaged = false;
		bool far = LV_ABS(dx) > cover_box_w / LYRICS_COMMIT_FRACTION;
		if (asking) {
			if (far) {
				lyrics_offer_download();
			}
		} else {
			lyrics_commit(opening ? far : !far);
		}
		// Cleared after this gesture's CLICKED has been seen, as for the sheet.
		lv_async_call(lyrics_swiped_clear_cb, NULL);
	}
}

// A tap on a timed line plays from where it is sung.
static void lyrics_tap_cb(lv_event_t *e) {
	(void)e;
	if (lyrics_swiped || !lyrics_cur.synced || !lyrics_view) {
		return;
	}
	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}
	lv_point_t p;
	lv_indev_get_point(indev, &p);
	uint32_t count = lv_obj_get_child_count(lyrics_view);
	for (uint32_t i = 0; i < count && i < (uint32_t)lyrics_cur.count; i++) {
		lv_area_t area;
		lv_obj_get_coords(lv_obj_get_child(lyrics_view, i), &area);
		// The gap between two lines belongs to the one above it.
		if (p.y >= area.y1 && p.y <= area.y2 + LYRICS_LINE_GAP) {
			int32_t ms = lyrics_cur.lines[i].ms;
			if (ms < 0) {
				return;
			}
			device_state_seek((double)ms / 1000.0);
			// Back to following at once: the finger chose this line.
			lyrics_user_until = 0;
			lyrics_centred = false;
			lyrics_follow((double)ms / 1000.0);
			lyrics_tap_ms = ms;
			lyrics_tap_until = lv_tick_get() + LYRICS_TAP_WAIT_MS;
			return;
		}
	}
}

static void lyrics_build(void) {
	lyrics_pane = lv_obj_create(cover_panel);
	lv_obj_remove_style_all(lyrics_pane);
	lv_obj_set_ignore_layout(lyrics_pane, true);
	lv_obj_set_scrollable(lyrics_pane, false);
	// Presses on the page outside the column go to the sleeve under it.
	lv_obj_set_clickable(lyrics_pane, false);
	lv_obj_set_hidden(lyrics_pane, true);

	lyrics_view = lv_obj_create(lyrics_pane);
	lv_obj_remove_style_all(lyrics_view);
	lv_obj_set_ignore_layout(lyrics_view, true);
	lv_obj_set_flex_flow(lyrics_view, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(lyrics_view, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(lyrics_view, LYRICS_LINE_GAP, 0);
	lv_obj_set_style_pad_hor(lyrics_view, ui_px(8), 0);
	lv_obj_set_scroll_dir(lyrics_view, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(lyrics_view, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(lyrics_view, lyrics_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
	lv_obj_add_event_cb(lyrics_view, lyrics_tap_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(lyrics_view, lyrics_drag_cb, LV_EVENT_PRESSED, (void *)(uintptr_t) false);
	lv_obj_add_event_cb(lyrics_view, lyrics_drag_cb, LV_EVENT_PRESSING, (void *)(uintptr_t) false);
	lv_obj_add_event_cb(lyrics_view, lyrics_drag_cb, LV_EVENT_RELEASED, (void *)(uintptr_t) false);
	lv_obj_add_event_cb(lyrics_view, lyrics_drag_cb, LV_EVENT_PRESS_LOST, (void *)(uintptr_t) false);

	lyrics_note = lv_label_create(lyrics_pane);
	lv_obj_set_ignore_layout(lyrics_note, true);
	lv_label_set_long_mode(lyrics_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(lyrics_note, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(lyrics_note, &font_ui_24, 0);
	lv_obj_set_style_text_color(lyrics_note, lv_color_white(), 0);
	lv_obj_set_style_text_opa(lyrics_note, LV_OPA_70, 0);
	lv_obj_set_hidden(lyrics_note, true);

	// The title and artist at the top, Studio's head, over the whole screen and
	// above the sleeve. Copies of the two in the row below, which keeps its
	// place so the controls do not move.
	lyrics_head = lv_obj_create(player_screen);
	lv_obj_remove_style_all(lyrics_head);
	lv_obj_set_ignore_layout(lyrics_head, true);
	lv_obj_set_scrollable(lyrics_head, false);
	lv_obj_set_clickable(lyrics_head, false);
	lv_obj_set_hidden(lyrics_head, true);

	lv_obj_t *col = lv_obj_create(lyrics_head);
	lyrics_head_col = col;
	lv_obj_remove_style_all(col);
	lv_obj_set_size(col, cover_box_w - 2 * STUDIO_MARGIN - 2 * ui_px(68), LV_SIZE_CONTENT);
	lv_obj_align(col, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(col, ui_px(4), 0);
	lv_obj_set_scrollable(col, false);
	lv_obj_set_clickable(col, false);

	lyrics_head_title = lv_label_create(col);
	lv_obj_set_width(lyrics_head_title, lv_pct(100));
	lv_obj_set_style_text_align(lyrics_head_title, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(lyrics_head_title, lv_obj_get_style_text_font(song_title_label, 0), 0);
	lv_obj_set_style_text_color(lyrics_head_title, lv_color_white(), 0);
	scrolltext_apply(lyrics_head_title);

	lyrics_head_artist = lv_label_create(col);
	lv_obj_set_width(lyrics_head_artist, lv_pct(100));
	lv_obj_set_style_text_align(lyrics_head_artist, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(lyrics_head_artist, lv_obj_get_style_text_font(song_artist_label, 0), 0);
	lv_obj_set_style_text_color(lyrics_head_artist, lv_color_make(200, 200, 200), 0);
	scrolltext_apply(lyrics_head_artist);

	lyrics_find_btn = lv_btn_create(player_screen);
	lv_obj_set_ignore_layout(lyrics_find_btn, true);
	lv_obj_set_size(lyrics_find_btn, LYRICS_FIND_BTN, LYRICS_FIND_BTN);
	lv_obj_set_style_bg_opa(lyrics_find_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(lyrics_find_btn, 0, 0);
	lv_obj_set_style_border_width(lyrics_find_btn, 0, 0);
	lv_obj_set_ext_click_area(lyrics_find_btn, CORNER_BTN_EXT_CLICK);
	lv_obj_add_event_cb(lyrics_find_btn, lyrics_find_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_hidden(lyrics_find_btn, true);
	lyrics_find_icon = lv_image_create(lyrics_find_btn);
	lv_image_set_src(lyrics_find_icon, &icon_lyrics_search);
	lv_obj_add_style(lyrics_find_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(lyrics_find_icon, LV_OPA_COVER, 0);
	lv_obj_center(lyrics_find_icon);

	// The sleeve is where the page is pulled in from.
	lv_obj_add_event_cb(cover_panel, lyrics_drag_cb, LV_EVENT_PRESSED, (void *)(uintptr_t) true);
	lv_obj_add_event_cb(cover_panel, lyrics_drag_cb, LV_EVENT_PRESSING, (void *)(uintptr_t) true);
	lv_obj_add_event_cb(cover_panel, lyrics_drag_cb, LV_EVENT_RELEASED, (void *)(uintptr_t) true);
	lv_obj_add_event_cb(cover_panel, lyrics_drag_cb, LV_EVENT_PRESS_LOST, (void *)(uintptr_t) true);
	doubletap_sleep_attach(cover_panel);
}

// Studio's background for the other two arrangements -- the sleeve blurred at
// the shape of the screen, the right way up -- made off the interface thread
// from a copy of the sleeve on screen: a few hundred milliseconds of this
// processor, and it has to be ready before the words are first pulled in.
typedef struct {
	unsigned generation;
	int w, h; // the screen
	cover_image_t src;
	cover_image_t out;
} lyrics_bg_job_t;

static void lyrics_bg_arrived(void *user) {
	lyrics_bg_job_t *job = user;
	if (job->generation == lyrics_bg_generation && job->out.pixels) {
		cover_free(&lyrics_screen_pic);
		lyrics_screen_pic = job->out;
		lyrics_screen_pic.dsc.data = lyrics_screen_pic.pixels; // struct moved, buffer didn't
		if (lyrics_progress > 0) {
			lyrics_look(true);
		}
	} else {
		cover_free(&job->out);
	}
	free(job);
}

static void *lyrics_bg_worker(void *arg) {
	lyrics_bg_job_t *job = arg;
	// Idle class: this starts in the same moment the player slides in and the
	// sleeve arrives, and at the interface's own priority its quarter of a
	// second of blur was taken out of that animation. Nobody is waiting for it
	// yet; it only has to be done before the words are pulled in.
	// Quietly: one of these runs per sleeve, and the named call logs a line.
	thread_set_background_level(false);
	cover_blur_fill(&job->src, job->w, job->h, &job->out);
	cover_free(&job->src);
	if (!gui_post(lyrics_bg_arrived, job)) {
		cover_free(&job->out);
		free(job);
	}
	return NULL;
}

// Drops the blur that belongs to the sleeve going away, and Studio's look
// with it while the words are up.
static void lyrics_bg_forget(void) {
	lyrics_bg_generation++;
	if (lyrics_look_on && !layout_studio_now) {
		lyrics_look(false);
	}
	if (!layout_studio_now && studio_bg && lv_image_get_src(studio_bg) == &lyrics_screen_pic.dsc) {
		lv_image_set_src(studio_bg, NULL);
	}
	cover_free(&lyrics_screen_pic);
}

// Starts the blur of the sleeve just put up. Not in Studio, which has its own
// blurred copy behind the whole screen already.
static void lyrics_bg_prepare(void) {
	if (layout_studio_now || !current_cover.pixels) {
		return;
	}
	lyrics_bg_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	size_t size = current_cover.dsc.data_size;
	job->src = current_cover;
	job->src.pixels = malloc(size);
	if (!job->src.pixels) {
		free(job);
		return;
	}
	memcpy(job->src.pixels, current_cover.pixels, size);
	job->src.dsc.data = job->src.pixels;
	job->generation = lyrics_bg_generation;
	job->w = (int)lv_obj_get_width(player_screen);
	job->h = (int)lv_obj_get_height(player_screen);

	pthread_t thread;
	if (pthread_create(&thread, NULL, lyrics_bg_worker, job) == 0) {
		pthread_detach(thread);
	} else {
		cover_free(&job->src);
		free(job);
	}
}

// Which of the four quality marks belongs to what is playing.
//
// Asked of the stream and not of the library, so a radio station and a track
// still downloading are answered as well as a file that has been indexed.
//
// A podcast gets none of them. The marks rank an encoding against what music
// needs, and an episode is speech at whatever bitrate the publisher chose:
// "lossy" beside it is true and says nothing anyone would act on.
static const lv_image_dsc_t *studio_quality_mark(const device_state_t *state) {
	if (!state->live && podcastcache_is_episode(state->current_file)) {
		return NULL;
	}
	if (audio_get_dsd_multiple() > 0) {
		return &icon_quality_dsd;
	}
	if (state->live || audio_stream_is_lossy()) {
		return &icon_quality_lossy;
	}
	int rate = 0, channels = 0;
	audio_get_stream_info(&rate, &channels);
	int bits = audio_get_stream_bits();
	if (rate > 48000 || bits > 16) {
		return &icon_quality_hifi;
	}
	if (rate > 0) {
		return &icon_quality_cd;
	}
	return NULL;
}

// The top of the sleeve, and how large it is.
//
// The sleeve is as large as the narrower of the two constraints allows: the
// width left between the margins, and the height left between the head and the
// line under it, which on this panel is what decides.
//
// Worked out from the numbers rather than from the widgets, so it can be asked
// before the panel has been laid out -- which is where the artwork is ordered
// at the size this arrangement will draw it.
static int studio_cover_geometry(int *top_out) {
	int head_top = back_btn_centre_y() - STUDIO_HEAD_H / 2;
	if (head_top < 2) {
		head_top = 2;
	}

	int top = head_top + STUDIO_HEAD_H + STUDIO_COVER_GAP;
	// Never wider than STUDIO_COVER_MAX_PCT of the panel: where the height
	// allows the full width (the R1), a sleeve from edge to edge crowds the
	// title above it and reads as the standard arrangement.
	int size = studio_box_w * STUDIO_COVER_MAX_PCT / 100;
	int room = studio_box_h - top - STUDIO_QUALITY_GAP - STUDIO_QUALITY_H - STUDIO_BOTTOM;
	if (size > room) {
		size = room;
	}
	if (size < 64) {
		size = 64;
	}
	if (top_out) {
		*top_out = top + (room - size) / 2;
	}
	return size;
}

// The four marks that say where a track comes from -- Qobuz, Tidal, podcast,
// and the red one for a live stream -- share the top right corner of the
// artwork, which in this arrangement is where the ellipsis went. They move
// down under it and keep its right edge, so the two read as one column.
//
// `parent` of NULL leaves them where they are and only re-places them, which
// is what studio_place() wants on a relayout.
static void studio_badges_move(lv_obj_t *parent, int y) {
	lv_obj_t *const marks[] = {live_badge, qobuz_badge, tidal_badge, podcast_badge};
	for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++) {
		if (!marks[i]) {
			continue;
		}
		if (parent) {
			lv_obj_set_parent(marks[i], parent);
		}
		lv_obj_align(marks[i], LV_ALIGN_TOP_RIGHT, -STUDIO_MARGIN, y);
	}
}

// Where everything goes.
static void studio_place(void) {
	if (!studio_box) {
		return;
	}

	int screen_w = studio_box_w;
	int head_top = back_btn_centre_y() - STUDIO_HEAD_H / 2;
	if (head_top < 2) {
		head_top = 2;
	}

	int cover_y = 0;
	int size = studio_cover_geometry(&cover_y);
	studio_cover_size = size;

	int cover_x = (screen_w - size) / 2;

	lv_obj_set_pos(studio_head, STUDIO_MARGIN, head_top);
	lv_obj_set_size(studio_head, screen_w - 2 * STUDIO_MARGIN, STUDIO_HEAD_H);

	studio_badges_move(NULL, head_top + STUDIO_HEAD_H + STUDIO_BADGE_GAP);

	lv_obj_set_size(studio_cover, size, size);
	lv_obj_set_pos(studio_cover, cover_x, cover_y);

	// The same square, so a track with no artwork keeps the arrangement rather
	// than leaving a bare screen.
	lv_obj_set_size(studio_empty, size, size);
	lv_obj_set_pos(studio_empty, cover_x, cover_y);

	lv_obj_set_size(studio_quality, size, STUDIO_QUALITY_H);
	lv_obj_set_pos(studio_quality, cover_x, cover_y + size + STUDIO_QUALITY_GAP);

	lyrics_layout();
}

// Points the sleeve and the background at the pictures the page already holds.
static void studio_refresh_cover(void) {
	if (!studio_cover) {
		return;
	}
	bool have = current_cover.pixels != NULL;
	lv_image_set_src(studio_cover, have ? &current_cover.dsc : NULL);
	if (have) {
		lv_obj_set_hidden(studio_cover, false);
	} else {
		lv_obj_set_hidden(studio_cover, true);
	}

	// One of the two squares is always up. Without this a track with no artwork
	// left the arrangement with nothing in it at all: no sleeve, no blurred
	// background behind it, and the page's own colour across the whole screen.
	if (studio_empty) {
		if (have) {
			lv_obj_set_hidden(studio_empty, true);
		} else {
			lv_obj_set_hidden(studio_empty, false);
		}
	}
	if (studio_empty_icon) {
		// The same mark the full-width cover panel stands in with: the note, the
		// headphones for a book, the aerial for a station.
		lv_image_set_src(studio_empty_icon, lv_image_get_src(cover_placeholder_icon));
		lv_obj_set_style_image_recolor(studio_empty_icon, theme()->text_secondary, 0);
	}

	if (studio_bg) {
		lv_image_set_src(studio_bg, current_backdrop.pixels ? &current_backdrop.dsc : NULL);
		if (current_backdrop.pixels) {
			lv_obj_set_hidden(studio_bg, false);
		} else {
			lv_obj_set_hidden(studio_bg, true);
		}
	}
}

// In the standard arrangement the title is lowered until the middle of its
// capitals meets the middle of the star beside it. Only the title moves: the
// star, the format under it and the artist stay where the row puts them, and a
// translation is not layout, so nothing else on the page moves either. The
// middle of the capitals is read from the font, so it holds at either text
// size. In the pills, in Studio, and beside no star (a book), the title is not
// moved.
static void align_title_with_star(void) {
	if (!song_title_label) {
		return;
	}
	int32_t shift = 0;
	if (song_text_obj && song_side_obj && fav_btn_obj && lv_obj_get_parent(song_title_label) == song_text_obj &&
		lv_obj_get_parent(fav_btn_obj) == song_side_obj && !lv_obj_is_hidden(fav_btn_obj) &&
		!lv_obj_is_hidden(song_side_obj)) {
		const lv_font_t *font = lv_obj_get_style_text_font(song_title_label, 0);
		int32_t line = lv_font_get_line_height(font);
		int32_t title_mid = line / 2;
		lv_font_glyph_dsc_t g;
		if (lv_font_get_glyph_dsc(font, &g, 'H', 0) && g.box_h > 0) {
			// A glyph's box is placed box_h + ofs_y above the baseline, which is
			// base_line up from the bottom of the line.
			int32_t cap_top = line - font->base_line - g.box_h - g.ofs_y;
			title_mid = cap_top + g.box_h / 2;
		}
		// Where the row put the title, without the shift already on it.
		lv_area_t title, star;
		lv_obj_get_coords(song_title_label, &title);
		lv_obj_get_coords(fav_btn_obj, &star);
		int32_t title_top = title.y1 - lv_obj_get_style_translate_y(song_title_label, 0) +
							lv_obj_get_style_pad_top(song_title_label, 0);
		shift = (star.y1 + star.y2) / 2 - (title_top + title_mid);
	}
	// Only when it changes: a new translation lays the column out again, and
	// that comes back here.
	if (lv_obj_get_style_translate_y(song_title_label, 0) != shift) {
		lv_obj_set_style_translate_y(song_title_label, shift, 0);
	}
}

static void song_info_layout_cb(lv_event_t *e) {
	(void)e;
	align_title_with_star();
}

// Puts the ellipsis and the star back where every other arrangement keeps them,
// and takes the panel down. Called before each arrangement is laid out, so the
// two that know nothing about Studio find the page as they left it.
static void studio_take_back(void) {
	if (!studio_box) {
		return;
	}
	lv_obj_set_hidden(studio_box, true);
	if (studio_bg) {
		lv_obj_set_hidden(studio_bg, true);
	}
	if (studio_empty) {
		lv_obj_set_hidden(studio_empty, true);
	}

	// The surfaces this arrangement made transparent, and the light chrome that
	// goes with the blurred sleeve. Only put back by the arrangement that took
	// them away: every other path leaves the cover to decide, as it always has.
	if (studio_up) {
		studio_up = false;
		lv_obj_remove_local_style_prop(cover_panel, LV_STYLE_BG_OPA, 0);
		lv_obj_set_height(cover_panel, cover_box_h);
		if (player_menu) {
			lv_obj_remove_local_style_prop(player_menu, LV_STYLE_BG_OPA, 0);
			lv_obj_remove_local_style_prop(player_menu, LV_STYLE_BG_IMAGE_OPA, 0);
			lv_obj_set_flex_align(player_menu, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
			lv_obj_set_style_pad_ver(player_menu, menu_pad_ver, 0);
			lv_obj_set_style_pad_gap(player_menu, menu_gap, 0);
		}
		// The picture at full width comes back, and the note with it when there
		// is no picture. studio_up is already down, so cover_show() agrees.
		cover_show(current_cover.pixels != NULL);
		if (format_label && song_side_obj) {
			lv_obj_set_parent(format_label, song_side_obj);
			lv_obj_set_style_text_align(format_label, LV_TEXT_ALIGN_RIGHT, 0);
		}
		// The marks go back to the corner of the artwork they share with every
		// other arrangement.
		lv_obj_t *const marks[] = {live_badge, qobuz_badge, tidal_badge, podcast_badge};
		for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++) {
			if (marks[i]) {
				lv_obj_set_parent(marks[i], cover_panel);
				lv_obj_align(marks[i], LV_ALIGN_TOP_RIGHT, -BADGE_INSET, badge_top());
			}
		}
		set_over_cover(current_cover.pixels != NULL);
	}

	if (more_btn_obj && controls_row) {
		lv_obj_set_parent(more_btn_obj, controls_row);
		lv_obj_set_ignore_layout(more_btn_obj, true);
		lv_obj_align(more_btn_obj, LV_ALIGN_RIGHT_MID, 0, 0);
	}
	if (fav_btn_obj) {
		// Laid out by its row again: the next step decides which row that is.
		lv_obj_set_ignore_layout(fav_btn_obj, false);
	}
}

static void studio_put(void) {
	if (!studio_box) {
		return;
	}

	if (song_title_label) {
		lv_obj_set_parent(song_title_label, studio_text_col);
		lv_obj_move_to_index(song_title_label, 0);
		lv_obj_set_width(song_title_label, lv_pct(100));
		lv_obj_set_style_max_width(song_title_label, LV_COORD_MAX, 0);
	}
	if (song_artist_label) {
		lv_obj_set_parent(song_artist_label, studio_text_col);
		lv_obj_move_to_index(song_artist_label, 1);
		lv_obj_set_width(song_artist_label, lv_pct(100));
		lv_obj_set_style_max_width(song_artist_label, LV_COORD_MAX, 0);
	}
	lv_obj_set_hidden(song_text_obj, true);
	if (song_side_obj) {
		lv_obj_set_hidden(song_side_obj, true);
	}

	// The line under the sleeve is the same label the standard arrangement
	// keeps beside the artist -- it already knows how to say "16/44.1 FLAC",
	// "320 kbps MP3" and "DSD256", a station included. Moved, not copied.
	if (format_label) {
		lv_obj_set_parent(format_label, studio_quality);
		lv_obj_set_hidden(format_label, false);
		lv_obj_set_width(format_label, LV_SIZE_CONTENT);
		lv_obj_set_style_text_align(format_label, LV_TEXT_ALIGN_LEFT, 0);
	}

	// The two buttons change places: the ellipsis belongs beside the title it
	// is about, and the star takes the corner of the transport row it left.
	if (more_btn_obj && studio_head) {
		lv_obj_set_parent(more_btn_obj, studio_head);
		lv_obj_set_ignore_layout(more_btn_obj, true);
		lv_obj_align(more_btn_obj, LV_ALIGN_RIGHT_MID, 0, 0);
	}
	if (fav_btn_obj && controls_row) {
		lv_obj_set_parent(fav_btn_obj, controls_row);
		lv_obj_set_ignore_layout(fav_btn_obj, true);
		lv_obj_align(fav_btn_obj, LV_ALIGN_RIGHT_MID, 0, 0);
	}

	// Onto the panel, so they draw over the sleeve rather than under it, and
	// below the ellipsis. studio_place() settles where.
	studio_badges_move(studio_box, 0);

	// The blurred sleeve is the whole background here, so everything that would
	// otherwise cover it gets out of the way: the two panels' own fills, the
	// picture at full width, and the note that stands in for it.
	studio_up = true;
	lv_obj_set_style_bg_opa(cover_panel, LV_OPA_TRANSP, 0);
	// The panel with the sleeve reaches down over the top of the controls
	// block, which in this arrangement holds its rows at the bottom. Grown
	// rather than left to overflow: a band of the screen redrawn below the
	// panel's own area would skip it and cut the sleeve off.
	lv_obj_set_height(cover_panel, studio_box_h);
	if (player_menu) {
		lv_obj_set_flex_align(player_menu, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_ver(player_menu, PLAYER_MENU_PAD_VER, 0);
		lv_obj_set_style_pad_gap(player_menu, menu_gap, 0);
		lv_obj_set_style_bg_opa(player_menu, LV_OPA_TRANSP, 0);
		// Its own blurred copy too: it is the same picture at a different crop,
		// and two of them meeting at the controls is a seam across the screen.
		lv_obj_set_style_bg_image_opa(player_menu, LV_OPA_TRANSP, 0);
	}
	cover_show(current_cover.pixels != NULL);

	lv_obj_set_hidden(studio_box, false);
	lv_obj_move_foreground(studio_box);
	if (studio_bg) {
		lv_obj_move_background(studio_bg);
	}
	studio_place();
	studio_refresh_cover();
}

// Moves the pieces between the two arrangements.
//
// Reparenting and not rebuilding: the title label carries its scrolling, the
// star carries what it does when pressed, and copying either of them would be
// copying behaviour.
static void apply_layout(void) {
	if (!alt_title_pill || !song_text_obj) {
		return;
	}

	// Always first: the two arrangements below know nothing about Studio, so
	// they have to find the ellipsis and the star where they left them.
	studio_take_back();

	if (layout_alt_now) {
		// A pill each, and not one pill with two lines in it: the title and the
		// album's artist are two different things, a single box around both
		// pads the shorter one out to the length of the longer, and each has to
		// be able to scroll on its own.
		//
		// The labels are a percentage of their parent in the standard layout,
		// and a percentage of a parent that is itself sized by its contents is
		// nothing at all. Here they size themselves -- so each pill ends where
		// its own text ends -- up to what is left of the sleeve once the star's
		// disc and the margins are taken off.
		int room = cover_box_w - 2 * ALT_PAD - ALT_FAV_SIZE - 2 * ALT_PILL_PAD_H - ui_px(24);
		if (song_title_label) {
			lv_obj_set_parent(song_title_label, alt_title_pill);
			lv_obj_set_width(song_title_label, LV_SIZE_CONTENT);
			lv_obj_set_style_max_width(song_title_label, room, 0);
		}
		if (song_artist_label) {
			lv_obj_set_parent(song_artist_label, alt_artist_pill);
			lv_obj_set_width(song_artist_label, LV_SIZE_CONTENT);
			lv_obj_set_style_max_width(song_artist_label, room, 0);
		}

		if (fav_btn_obj && alt_fav_circle) {
			lv_obj_set_parent(fav_btn_obj, alt_fav_circle);
			lv_obj_center(fav_btn_obj);
		}
		if (format_label) {
			lv_obj_set_hidden(format_label, true); // the sleeve is not the place for a bitrate
		}
		if (song_side_obj) {
			lv_obj_set_hidden(song_side_obj, true);
		}
		lv_obj_set_hidden(song_text_obj, true); // empty now, and it would still take a row

		lv_obj_set_hidden(alt_text_col, false);
		lv_obj_set_hidden(alt_fav_circle, false);
		// Over the sleeve and not under it: both were built before the picture
		// so that the picture would not have to be rebuilt around them.
		lv_obj_move_foreground(alt_text_col);
		lv_obj_move_foreground(alt_fav_circle);
		if (wave_canvas) {
			lv_obj_set_hidden(wave_box, false);
			lv_obj_move_to_index(wave_box, lv_obj_get_index(progress_slider));
			slider_over_waveform(true);
		}
	} else {
		if (song_title_label) {
			lv_obj_set_parent(song_title_label, song_text_obj);
			lv_obj_move_to_index(song_title_label, 0);
			lv_obj_set_width(song_title_label, lv_pct(100));
			lv_obj_set_style_max_width(song_title_label, LV_COORD_MAX, 0);
		}
		if (song_artist_label) {
			lv_obj_set_parent(song_artist_label, song_text_obj);
			lv_obj_move_to_index(song_artist_label, 1);
			lv_obj_set_width(song_artist_label, lv_pct(100));
			lv_obj_set_style_max_width(song_artist_label, LV_COORD_MAX, 0);
		}
		lv_obj_set_hidden(song_text_obj, false);

		if (fav_btn_obj && song_side_obj) {
			lv_obj_set_parent(fav_btn_obj, song_side_obj);
			lv_obj_move_to_index(fav_btn_obj, 0);
		}
		if (format_label) {
			lv_obj_set_hidden(format_label, false);
		}
		if (song_side_obj) {
			lv_obj_set_hidden(song_side_obj, false);
		}

		lv_obj_set_hidden(alt_text_col, true);
		lv_obj_set_hidden(alt_fav_circle, true);
		if (layout_wave_now && wave_canvas) {
			lv_obj_set_hidden(wave_box, false);
			lv_obj_move_to_index(wave_box, lv_obj_get_index(progress_slider));
			slider_over_waveform(true);
		} else {
			slider_over_waveform(false);
			if (wave_box) {
				lv_obj_set_hidden(wave_box, true);
			}
		}
	}

	// Bigger targets in this arrangement. The star's button is as big as the
	// disc it sits in, which is what a finger aims at; a press beyond the disc
	// could not reach it anyway, since LVGL looks for a child only inside its
	// parent.
	if (fav_btn_obj) {
		lv_obj_set_size(fav_btn_obj, layout_alt_now ? ALT_FAV_SIZE : FAV_BTN_W, layout_alt_now ? ALT_FAV_SIZE : FAV_BTN_H);
	}
	lv_obj_t *const transport[] = {prev_btn_obj, play_btn, next_btn_obj};
	for (size_t i = 0; i < sizeof(transport) / sizeof(transport[0]); i++) {
		if (transport[i]) {
			lv_obj_set_ext_click_area(transport[i], layout_alt_now ? ALT_EXT_CLICK : 0);
		}
	}

	if (layout_studio_now) {
		studio_put();
	} else {
		lyrics_layout(); // studio_put() places the page itself
	}
	align_title_with_star(); // the title may have just left its row, or come back to it

	paint_alt_tint();
	bar_over_player_update(); // Studio's bar blurs nothing of its own
}

// Decides which arrangement the thing now playing gets, and moves the pieces
// only if the answer changed.
//
// Called from every now-playing refresh and from the live-mode watch in the
// poll, which is what makes the arrangement follow the source instead of being
// settled once at startup. It has to run on the way out of a stream as well as
// on the way in, or the ordinary progress bar is left behind on top of the
// waveform: the bar's parent, size and transparency are decided here and
// nowhere else.
static void update_layout(const device_state_t *state) {
	// Studio only wants a picture and a name, and takes whatever is playing:
	// a file, a cached Qobuz or Tidal track, a podcast, a book, a station.
	bool local = playing_local_file(state);
	bool lyrics = lyrics_wanted && local;
	bool studio = layout_choice == PLAYER_LAYOUT_STUDIO;
	bool wave = layout_choice == PLAYER_LAYOUT_ALTERNATIVE && local;
	bool alt = wave;

	if (studio == layout_studio_now && alt == layout_alt_now && wave == layout_wave_now && lyrics == lyrics_allowed &&
		alt_title_pill) {
		return;
	}
	layout_studio_now = studio;
	layout_alt_now = alt;
	layout_wave_now = wave;
	lyrics_allowed = lyrics;
	// Up only with words for this very file; while they are still being read,
	// as they were, so a track change does not take the page down and up again.
	bool same_file = strcmp(state->current_file, lyrics_path) == 0;
	if (!lyrics) {
		lyrics_now = false;
	} else if (same_file && lyrics_loaded) {
		lyrics_now = lyrics_cur.count > 0;
	}
	apply_layout();
	lyrics_request(state);

	// The blurred copy is made at the shape of whatever shows it, so moving in
	// or out of Studio means the one on hand is the wrong shape and the picture
	// is asked for again. Only on a change of arrangement, which is a setting
	// the user has just touched.
	if (studio != backdrop_is_studio && cover_shown_path[0]) {
		reload_cover(cover_shown_path);
	}
}

// ---------------------------------------------------------------------------
// the shape of the track
// ---------------------------------------------------------------------------

// Paints the bars into the canvas buffer by hand.
//
// RGB565A8, and written directly rather than through the drawing pipeline: the
// gaps between the bars have to let the blurred sleeve through, an RGB565
// canvas would lay an opaque rectangle over it, and one LVGL object per bar is
// one more thing to lay out on every track change.
//
// The buffer is the colour plane followed by the alpha plane, which is what
// LVGL means by RGB565A8.
//
// Each bar is a capsule rather than a rectangle: `cap` is how far the drawn
// column is pulled in at that horizontal offset, from the circle of diameter
// `solid` that rounds the ends. A silent stretch is then a row of dots rather
// than a row of ticks, which is the shape this player's buttons and pills are
// already drawn in.
// The bars all start at the same place and are all the same width, and two
// callers besides the painter need to know where: one to work out how far along
// the playhead is in pixels, the other to compare that with what is on screen.
static void wave_geometry(int *bar_w, int *left) {
	int w = wave_w;
	int b = w / WAVEFORM_BARS;
	if (b < 1) {
		b = 1;
	}
	*bar_w = b;
	*left = (w - b * WAVEFORM_BARS) / 2;
}

// How far along the strip the playhead is, in pixels of the canvas.
//
// Pixels and not columns: a column is a forty-eighth of the track, so a column
// boundary stands still for several seconds and then jumps. In pixels the
// playhead moves by one every half second or so, and the bar it is inside is
// lit up to the playhead rather than all at once.
//
// Along the same stretch as the bar laid over it (see view_start), so a finger
// on the strip lands where the lit part says it will.
static int wave_played_px(double position) {
	int bar_w, left;
	wave_geometry(&bar_w, &left);
	double fraction = view_length > 0.1 ? view_value(position) / 1000.0 : 0.0;
	return left + (int)(fraction * bar_w * WAVEFORM_BARS);
}

static void wave_paint(int played_px) {
	if (!wave_buf || !wave_canvas) {
		return;
	}

	// The buffer's own width, and never the object's: an object that has not
	// been laid out yet answers with something else, and painting to that
	// number writes past the end of the buffer.
	int w = wave_w;
	int h = WAVE_HEIGHT;
	if (w <= 0) {
		return;
	}

	uint16_t *colour = (uint16_t *)wave_buf;
	uint8_t *alpha = wave_buf + (size_t)w * h * 2;
	memset(alpha, 0, (size_t)w * h);

	lv_color_t tint = alt_mark();
	uint16_t packed = (uint16_t)(((tint.red & 0xF8) << 8) | ((tint.green & 0xFC) << 3) | (tint.blue >> 3));

	int bar_w, left;
	wave_geometry(&bar_w, &left);
	int solid = bar_w - WAVE_BAR_GAP;
	if (solid < 1) {
		solid = 1;
	}
	int middle = h / 2;

	// The tallest a bar may be drawn, with the cap and a pixel of air left over
	// so that a loud passage does not end flat against the top edge -- which is
	// exactly what reads as clipped audio.
	int reach = h / 2 - 1 - solid / 2;
	if (reach < WAVE_MIN_BAR) {
		reach = WAVE_MIN_BAR;
	}

	for (int i = 0; i < WAVEFORM_BARS; i++) {
		int value = i < wave_ready ? wave_bars[i] : 0;
		int half = value * reach / 255;
		if (half < WAVE_MIN_BAR) {
			half = WAVE_MIN_BAR;
		}
		int x0 = left + i * bar_w;
		for (int dx = 0; dx < solid; dx++) {
			int x = x0 + dx;
			if (x < 0 || x >= w) {
				continue;
			}
			// Decided per column of pixels rather than per bar, which is what
			// lets the playhead stand in the middle of a bar.
			uint8_t opa = x < played_px ? WAVE_PAST_OPA : WAVE_TODO_OPA;
			// Doubled coordinates, so that an even-width bar can have its
			// centre between two pixels without any rounding.
			int d2 = 2 * dx - (solid - 1);
			int cap = (solid - (int)utils_isqrt32((uint32_t)(solid * solid - d2 * d2))) / 2;

			for (int y = middle - half + cap; y <= middle + half - cap; y++) {
				if (y < 0 || y >= h) {
					continue;
				}
				size_t at = (size_t)y * w + x;
				colour[at] = packed;
				alpha[at] = opa;
			}
		}
	}

	lv_obj_invalidate(wave_canvas);
}

// Moves the played part of the shape to `seconds`, if that has moved it at all.
//
// The guard is what keeps this cheap enough to call at fifteen frames a second:
// repainting is eighty kilobytes of buffer and an invalidate, and on a track of
// any length the answer changes about twice a second.
static void wave_preview(double seconds) {
	if (!layout_wave_now || !wave_canvas || view_length <= 0.1) {
		return;
	}
	int px = wave_played_px(seconds);
	if (px == wave_drawn) {
		return;
	}
	wave_drawn = px;
	wave_paint(px);
}

// Asks for this track's shape and repaints when either the shape or the
// playhead has moved. Called from the progress poll, so it runs twice a second
// while something is playing and once every two seconds otherwise.
static void wave_refresh(const char *path, double position) {
	if (!layout_wave_now || !wave_canvas) {
		return;
	}

	// Compared with what is on screen as well as counted: while the shape is
	// being worked out the columns already shown are scaled again whenever a
	// louder one arrives.
	int had = wave_ready;
	uint8_t before[WAVEFORM_BARS];
	memcpy(before, wave_bars, sizeof(before));
	if (path && path[0]) {
		wave_ready = waveform_get(path, wave_bars);
	} else {
		wave_ready = 0;
	}
	bool same_shape = wave_ready == had && memcmp(before, wave_bars, (size_t)wave_ready) == 0;

	int px = wave_played_px(position);
	uint32_t tone = album_tone;
	if (px == wave_drawn && same_shape && tone == wave_drawn_tone) {
		return;
	}
	wave_drawn = px;
	wave_drawn_tone = tone;
	wave_paint(px);
}

// The size the sleeve is asked for in the arrangement in force (see
// reload_cover()).
static void cover_request_size(int *w, int *h) {
	*w = layout_studio_now ? studio_cover_geometry(NULL) : cover_box_w;
	*h = layout_studio_now ? *w : cover_box_h;
}

// Throws away what is on screen and asks for the artwork of `filepath`. The
// half of refresh_cover() that runs once it is known that the picture really is
// changing.
// Takes the pictures on screen down and frees them. With `placeholder` the
// empty sleeve goes up in their place and the chrome follows the theme again;
// without, the caller is about to put the next pictures up in this same pass.
static void cover_drop_current(bool placeholder) {
	if (cover_release_cb) {
		cover_release_cb();
	}

	// Detach the old pixels before freeing them, so nothing can draw from a
	// buffer that's already gone.
	lv_image_set_src(cover_img, NULL);
	lv_obj_set_style_bg_image_src(player_menu, NULL, 0);
	if (layout_studio_now && studio_bg) {
		lv_image_set_src(studio_bg, NULL); // Studio's points at the backdrop freed below
	}
	cover_free(&current_cover);
	cover_free(&current_backdrop);
	bar_over_player_update();

	if (!placeholder) {
		return;
	}
	cover_show(false);
	// Studio holds pointers to both of those, so it is told in the same breath:
	// its sleeve and its background come down and the empty square goes up.
	if (layout_studio_now) {
		studio_refresh_cover();
	}

	// Backdrop gone: the chrome goes back to following the theme.
	set_over_cover(false);
	album_tone = 0;
	paint_alt_tint();

	lv_obj_invalidate(cover_panel);
	lv_obj_invalidate(player_menu);
}

static void reload_cover(const char *filepath) {
	// The blur of the picture going away goes with it, from behind the words
	// too: until the new one is made they are on a plain dark page.
	lyrics_bg_forget();

	// The pictures on screen stay up until the next ones are ready, and are
	// swapped for them in one pass (apply_cover_result). Taking them down here
	// put the empty sleeve on screen for the length of the load -- a blink at
	// every skip, even with the next picture already in the cache. Only with
	// nothing to load do they come down now.
	if (!(filepath && filepath[0] && current_cover.pixels)) {
		cover_drop_current(true);
	}

	if (filepath && filepath[0]) {
		backdrop_is_studio = layout_studio_now;
		cover_set_backdrop_upright(backdrop_is_studio);

		// The sleeve is asked for at the size the arrangement in force will
		// draw it. A picture that does not match its widget is resampled by
		// LVGL on every frame it is drawn, and every frame is what a sheet
		// being dragged, or the control centre coming down over it, means: in
		// Studio the picture is decoded at the width of the screen and shown
		// at two thirds of it, and that resampling is the interface's core
		// being spent again and again on an answer that never changes.
		int cover_w, cover_h;
		cover_request_size(&cover_w, &cover_h);
		coverloader_request_player(filepath, cover_w, cover_h, backdrop_w,
								   backdrop_is_studio ? backdrop_studio_h : backdrop_h);
		cover_request_outstanding = true;
		warm_cover_w = cover_w;
		warm_cover_h = cover_h;
		warm_backdrop_h = backdrop_is_studio ? backdrop_studio_h : backdrop_h;

		// The collector rides on the progress timer too; make sure it is
		// ticking even when nothing is playing (paused player, theme change).
		lv_timer_resume(progress_slider_timer);
		cover_poll_start();
	} else {
		cover_request_outstanding = false;
	}
}

// A track change.
//
// The next track of a record carries the record's cover, and so does the next
// file in a folder that keeps its picture beside the music. Decoding it again
// is a second of this processor's time for a picture that does not change, and
// the cover blinking out to the placeholder and back is the part the user sees.
//
// So nothing is thrown away yet: the loader is asked what the new track's
// artwork is -- which it answers by reading the picture, not by decoding it --
// and only an answer that differs from what is on screen costs anything. When
// it does differ the old cover stays up for the one poll that takes, which
// still reads better than a placeholder in the gap.
static void refresh_cover(const char *filepath) {
	if (!cover_img)
		return;

	snprintf(cover_shown_path, sizeof(cover_shown_path), "%s", filepath ? filepath : "");

	if (cover_shown_id != 0 && current_cover.pixels && filepath && filepath[0]) {
		cover_id_state = COVER_ID_NEW_TRACK;
		coverloader_request_player_id(filepath);
		lv_timer_resume(progress_slider_timer);
		cover_poll_start();
		return;
	}

	cover_id_state = COVER_ID_IDLE;
	cover_incoming_id = 0;
	reload_cover(filepath);
}

// ---------------------------------------------------------------------------
// Glyph warm-up
//
// The first time a letter is drawn at a size, FreeType has to find it in the
// font file (on the device a squashfs block to unpack), hint and rasterize it,
// and LVGL measures it once more when the label is laid out. For a Latin title
// the letters are nearly all in the caches already; a title in kanji is mostly
// new ones, and that cost used to land in the skip itself (np_layout_texts up
// to 141 ms, plus the first repaint). The tags of the tracks a skip can go to
// are read ahead (device_state_prefetch), so their text is known a few seconds
// early: while the player is idle its glyphs are asked for here, at the fonts
// the title and artist labels use, a few milliseconds at a time. FreeType and
// LVGL are not thread-safe, so this is the interface thread and nothing else.
// ---------------------------------------------------------------------------
#define GLYPH_WARM_PATHS 4
#define GLYPH_WARM_PERIOD_MS 40
#define GLYPH_WARM_SLICE_US 5000   // per tick, so a press that lands in one is not kept waiting
#define GLYPH_WARM_QUIET_MS 300    // after the last touch or button
#define GLYPH_WARM_GIVE_UP_MS 4000 // the tags did not turn up (not read ahead: a stream, a file the card refused)
#define GLYPH_WARM_LABELS 4
#define GLYPH_WARM_FONTS 2 // title and the lyrics page's copy of it, artist and its copy

static const char *const PROF_GLYPH_WARM = "glyph_warm";

typedef struct {
	char path[512];
	char text[2][512]; // title, artist
	bool have_text;
	bool done;
} glyph_job_t;

static glyph_job_t glyph_jobs[GLYPH_WARM_PATHS];
static lv_timer_t *glyph_warm_timer;
static uint32_t glyph_warm_started;
static int glyph_job;       // the job in hand
static int glyph_string;    // 0 title, 1 artist
static int glyph_font;      // which font of the string
static uint32_t glyph_pos;  // byte offset in the string

// A label keeps Arabic in its joined (presentation) forms, so those are the
// letters that get drawn and measured; the title as the tags have it is not.
// Put in the same forms here, or the warm-up would fetch the wrong glyphs.
static void glyph_shape(char *text, size_t capacity) {
#if LV_USE_ARABIC_PERSIAN_CHARS
	const unsigned char *p = (const unsigned char *)text;
	while (*p && (*p < 0xD8 || *p > 0xDB)) {
		p++;
	}
	if (!*p) {
		return;
	}
	char *joined = malloc(lv_text_ap_strlen(text) + 1);
	if (!joined) {
		return;
	}
	lv_text_ap_proc(text, joined);
	snprintf(text, capacity, "%s", joined);
	free(joined);
#else
	(void)text;
	(void)capacity;
#endif
}

static uint64_t glyph_now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

// The font a string is drawn in on the n-th label that shows it, or NULL.
static const lv_font_t *glyph_font_for(int string, int n) {
	lv_obj_t *label;
	if (string == 0) {
		label = n == 0 ? song_title_label : lyrics_head_title;
	} else {
		label = n == 0 ? song_artist_label : lyrics_head_artist;
	}
	return label ? lv_obj_get_style_text_font(label, LV_PART_MAIN) : NULL;
}

// Asks for one letter at one size the way drawing would: the metrics (what the
// layout measures with), then the bitmap (what the repaint needs).
static void glyph_warm_letter(const lv_font_t *font, uint32_t letter) {
	lv_font_glyph_dsc_t g;
	if (!lv_font_get_glyph_dsc(font, &g, letter, 0) || g.is_placeholder || g.box_w == 0 || g.box_h == 0) {
		return;
	}
	if (lv_font_get_glyph_bitmap_internal(&g, NULL)) {
		lv_font_glyph_release_draw_data_internal(&g);
	}
}

static void glyph_warm_tick(lv_timer_t *timer) {
	if (!power_screen_is_on()) {
		lv_timer_pause(timer); // nobody is going to skip with the panel dark; the next track change asks again
		return;
	}
	if (lv_display_get_inactive_time(NULL) < GLYPH_WARM_QUIET_MS) {
		return; // somebody is using it
	}
	uint64_t begin = glyph_now_us();
	frameprof_begin(PROF_GLYPH_WARM);

	bool waiting = false; // a job whose tags have not arrived yet
	bool budget_left = true;
	while (budget_left && glyph_job < GLYPH_WARM_PATHS) {
		glyph_job_t *job = &glyph_jobs[glyph_job];
		if (job->done || !job->path[0]) {
			glyph_job++;
			continue;
		}
		if (!job->have_text) {
			song_metadata_t meta;
			if (!device_state_prefetch_peek(job->path, &meta)) {
				waiting = true;
				glyph_job++; // the next job may be ready; this one is looked at again from the top
				continue;
			}
			const char *slash = strrchr(job->path, '/');
			snprintf(job->text[0], sizeof(job->text[0]), "%s", meta.title[0] ? meta.title : (slash ? slash + 1 : job->path));
			snprintf(job->text[1], sizeof(job->text[1]), "%s", metadata_shown_artist(&meta));
			glyph_shape(job->text[0], sizeof(job->text[0]));
			glyph_shape(job->text[1], sizeof(job->text[1]));
			job->have_text = true;
			glyph_string = 0;
			glyph_font = 0;
			glyph_pos = 0;
		}

		const char *text = job->text[glyph_string];
		const lv_font_t *font = glyph_font_for(glyph_string, glyph_font);
		const lv_font_t *first = glyph_font_for(glyph_string, 0);
		if (glyph_font >= GLYPH_WARM_FONTS || !text[0] || !font || (glyph_font > 0 && font == first) ||
			text[glyph_pos] == '\0') {
			// This font is through (or there is none to do): the next one, the next string, the next job.
			glyph_pos = 0;
			if (++glyph_font >= GLYPH_WARM_FONTS) {
				glyph_font = 0;
				if (++glyph_string >= 2) {
					glyph_string = 0;
					job->done = true;
					glyph_job++;
					continue;
				}
			}
			continue;
		}

		uint32_t letter = lv_text_encoded_next(text, &glyph_pos);
		if (letter > ' ') {
			glyph_warm_letter(font, letter);
		}
		budget_left = glyph_now_us() - begin < GLYPH_WARM_SLICE_US;
	}

	frameprof_end(PROF_GLYPH_WARM);

	if (waiting && glyph_job >= GLYPH_WARM_PATHS && lv_tick_elaps(glyph_warm_started) < GLYPH_WARM_GIVE_UP_MS) {
		glyph_job = 0; // everything that could be done is: look at the waiting ones again next time
		return;
	}
	if (glyph_job >= GLYPH_WARM_PATHS) {
		lv_timer_pause(timer);
	}
}

// A new set of tracks to get ready (the two ahead and the two behind).
static void glyph_warm_request(const char *const *paths, int count) {
	if (!power_screen_is_on()) {
		return;
	}
	if (!glyph_warm_timer) {
		glyph_warm_timer = lv_timer_create(glyph_warm_tick, GLYPH_WARM_PERIOD_MS, NULL);
	}
	for (int i = 0; i < GLYPH_WARM_PATHS; i++) {
		glyph_job_t *job = &glyph_jobs[i];
		const char *path = i < count && paths[i] ? paths[i] : "";
		if (strcmp(job->path, path) == 0 && job->done) {
			continue; // asked for before and done (the letters are cached, not lost)
		}
		snprintf(job->path, sizeof(job->path), "%s", path);
		job->have_text = false;
		job->done = false;
	}
	glyph_job = 0;
	glyph_string = 0;
	glyph_font = 0;
	glyph_pos = 0;
	glyph_warm_started = lv_tick_get();
	lv_timer_resume(glyph_warm_timer);
}

// This track's pictures are up (or were kept); the next one's go into the disk
// cache in the background, so that skipping to it is a read rather than a
// decode. At the sizes of the last request, which is what the next one will
// ask for.
static void warm_next_cover(void) {
	// The tags and the star of the same tracks are read ahead too
	// (device_state_prefetch), whether or not there is a cover to warm, and
	// their text gets its glyphs ready once the tags are in.
	char paths[GLYPH_WARM_PATHS][512] = {"", "", "", ""};
	const char *list[GLYPH_WARM_PATHS];
	int here = playlist_current_index();
	int count = playlist_count();
	// Most likely first: next, previous, then one further each way.
	const int offsets[GLYPH_WARM_PATHS] = {1, -1, 2, -2};
	for (int i = 0; i < GLYPH_WARM_PATHS; i++) {
		int index = here + offsets[i];
		if (index >= 0 && index < count) {
			playlist_path_at(index, paths[i], sizeof(paths[i]));
		}
		list[i] = paths[i];
	}
	device_state_prefetch(list, GLYPH_WARM_PATHS);
	glyph_warm_request(list, GLYPH_WARM_PATHS);

	if (warm_cover_w <= 0 || warm_cover_h <= 0) {
		return;
	}
	if (paths[0][0]) {
		coverloader_warm_player(paths[0], warm_cover_w, warm_cover_h, backdrop_w, warm_backdrop_h);
	}
	// And the one before, for "previous" -- after the next one, on the loader.
	if (paths[1][0]) {
		coverloader_warm_player_back(paths[1], warm_cover_w, warm_cover_h, backdrop_w, warm_backdrop_h);
	}
}

// The answer to whichever question was asked, picked up on the progress poll.
static void apply_cover_id_result(void) {
	if (cover_id_state == COVER_ID_IDLE) {
		return;
	}
	uint64_t id = 0;
	bool finished = false;
	coverloader_take_player_id(&id, &finished);
	if (!finished) {
		return;
	}

	if (cover_id_state == COVER_ID_MEASURE) {
		cover_id_state = COVER_ID_IDLE;
		cover_shown_id = id;
		return;
	}

	cover_id_state = COVER_ID_IDLE;

	// The same picture: the cover, the backdrop, the sleeve colour and the
	// chrome all stay exactly as they are. This is the whole point.
	if (id != 0 && id == cover_shown_id) {
		printf("cover: the same picture, kept, %u ms after the track change\n",
			   (unsigned)lv_tick_elaps(track_change_tick));
		warm_next_cover();
		return;
	}

	// Different, and its id is already known: the decode does not have to be
	// asked what it produced.
	cover_incoming_id = id;
	reload_cover(cover_shown_path);
}

// Picks up the worker's finished artwork, if any. Runs on the UI thread from
// the progress timer, so touching widgets here is allowed.
static void apply_cover_result(void) {
	if (!cover_request_outstanding) {
		return;
	}

	cover_image_t new_cover, new_backdrop;
	bool finished = false;
	bool has_cover = coverloader_take_player(&new_cover, &new_backdrop, &finished);

	if (!finished) {
		return; // still decoding; what was on screen stays up
	}
	cover_request_outstanding = false;
	printf("cover: on screen %u ms after the track change\n", (unsigned)lv_tick_elaps(track_change_tick));

	if (has_cover) {
		// The pictures of the track before, held up until now, go in the same
		// pass the new ones come in: no frame in between shows the gap.
		if (current_cover.pixels) {
			cover_drop_current(false);
		}

		// What is now on screen, so the next track can be asked whether its
		// artwork is this same picture. Usually known already; when it is not
		// -- the first cover after the player opens, a station's icon -- the
		// loader is asked, because working the id out reads the file and this
		// is the interface thread.
		cover_shown_id = cover_incoming_id;
		if (cover_shown_id == 0 && cover_shown_path[0]) {
			cover_id_state = COVER_ID_MEASURE;
			coverloader_request_player_id(cover_shown_path);
			cover_poll_start();
		}

		current_cover = new_cover;
		current_cover.dsc.data = current_cover.pixels; // struct moved, buffer didn't

		lv_image_set_src(cover_img, &current_cover.dsc);
		lv_obj_set_size(cover_img, current_cover.dsc.header.w, current_cover.dsc.header.h);
		lv_obj_center(cover_img);

		cover_show(true);

		if (new_backdrop.pixels) {
			current_backdrop = new_backdrop;
			current_backdrop.dsc.data = current_backdrop.pixels;
			lv_obj_set_style_bg_image_src(player_menu, &current_backdrop.dsc, 0);
		}

		// The dark backdrop is up: light chrome, in both themes.
		set_over_cover(true);

		// And the sleeve's own colour, which is what the alternative layout is
		// drawn in: the one an album's page is painted in (cover_page_tone),
		// so the record looks the same colour on both. Kept with the picture by
		// the loader, so a cover seen before is not worked out again, and taken
		// to full strength here for marks drawn over a dark backdrop.
		uint32_t tone = 0;
		int luma = 0;
		bool light = false;
		if (current_cover.has_tone) {
			tone = current_cover.tone;
		} else if (!cover_page_tone(&current_cover, COVER_PAGE_LIGHT_FROM, &tone, &luma, &light)) {
			tone = 0;
		}
		album_tone = tone ? cover_tone_stretched(tone) : 0;
		paint_alt_tint();

		// The words' background, ready before they are pulled in.
		lyrics_bg_prepare();

		lv_obj_invalidate(cover_panel);
		lv_obj_invalidate(player_menu);
	}
	if (!has_cover) {
		cover_shown_id = 0; // nothing to compare the next track against
		lyrics_bg_forget(); // no picture, nothing to blur
		if (current_cover.pixels) {
			cover_drop_current(true); // the previous track's, held up until now
		}
	}
	bar_over_player_update();
	if (lyrics_progress > 0) {
		lyrics_look(true); // Studio's backdrop has arrived, or there is none
	}
	cover_incoming_id = 0;

	warm_next_cover();

	// Studio draws both of these itself -- the sleeve smaller and the blurred
	// copy across the whole screen -- so it is told here, where they land,
	// rather than asked on a timer.
	if (layout_studio_now) {
		studio_refresh_cover();
	}
	// No artwork: the placeholder is already showing.
	// Upstream's prefetch_next_cover() is not called: warm_next_cover() above
	// already decodes the next track (and the previous) into both caches, and a
	// second decode of the same picture is a second of the one core.
}

// Collects whatever the loader has answered, quickly, for as long as anything
// is outstanding; then the timer stops until the next question.
static void cover_poll_cb(lv_timer_t *timer) {
	apply_cover_id_result();
	apply_cover_result();
	if (!cover_request_outstanding && cover_id_state == COVER_ID_IDLE) {
		lv_timer_pause(timer);
	}
}

static void cover_poll_start(void) {
	if (!cover_poll_timer) {
		cover_poll_timer = lv_timer_create(cover_poll_cb, COVER_POLL_MS, NULL);
	}
	lv_timer_resume(cover_poll_timer);
}

// The artwork the player has already decoded for the loaded track, lent to
// anything else that wants to draw it (the screensaver). NULL while a track
// has no cover or its decode is still in flight -- never a stale pointer:
// refresh_cover() zeroes both before the buffers are freed.
const lv_image_dsc_t *player_cover_image(void) { return current_cover.pixels ? &current_cover.dsc : NULL; }

const lv_image_dsc_t *player_backdrop_image(void) {
	return current_backdrop.pixels ? &current_backdrop.dsc : NULL;
}

static void update_repeat_button(void);

// Repaints the bits that are coloured by hand, and rebuilds the blurred
// artwork behind the controls -- it is dimmed towards the page background, so
// it has to be regenerated when that background changes.
static void player_refresh_theme(void) {
	if (!cover_panel)
		return;

	if (studio_box) {
		lv_obj_set_style_bg_color(studio_box, theme()->screen_bg, 0);
	}

	lv_obj_set_style_bg_color(cover_panel, theme()->cover_bg, 0);
	lv_obj_set_style_image_recolor(cover_placeholder_icon, theme()->text_secondary, 0);
	// The backdrop over a loaded cover is always the dark treatment -- same
	// look in both themes; only the no-cover panel follows the theme.
	cover_set_backdrop_light(false);
	lv_obj_set_style_bg_color(progress_slider, chrome_accent(), LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(progress_slider, chrome_accent(), LV_PART_KNOB);
	set_over_cover(chrome_over_cover); // repaints the play disc for the new palette
	update_repeat_button();
	update_speed_button();

	device_state_t state;
	device_state_get(&state);
	refresh_cover(state.current_file);
	if (lyrics_pane) {
		lyrics_look(lyrics_look_on); // the words in the new theme's colours
	}
}

static void update_qobuz_badge(void);

// Refreshes the now-playing info (title + artist) from the current
// device state and resets the per-track length cache. Used both when the user
// picks a file and when playback auto-advances to a new track.
// Marks inside the repaint for a new track (frameprof.h; only logged when the
// profiler is on), so a slow skip can be pinned on a step.
static const char *const PROF_NP_LISTS = "np_notify_lists";
static const char *const PROF_NP_LAYOUT = "np_layout_texts";
static const char *const PROF_NP_TITLE = "np_lt_title";
static const char *const PROF_NP_ARTIST = "np_lt_artist";
static const char *const PROF_NP_HEAD = "np_lt_lyrics_head";
static const char *const PROF_NP_FAV = "np_fav_button";
static const char *const PROF_NP_BADGES = "np_format_badges";
static const char *const PROF_NP_LYRICS = "np_lyrics_request";
static const char *const PROF_NP_COVER = "np_refresh_cover";

static void refresh_now_playing(void) {
	track_change_tick = lv_tick_get();
	current_total_length = 0; // unknown until the playback thread reports it

	// The library lists carry an accent mark on the playing track (and on the
	// album and artist it belongs to). This is the one place every track
	// change goes through, so it is where they are told.
	frameprof_begin(PROF_NP_LISTS);
	medialist_notify_now_playing();
	browser_notify_now_playing();
	trackmenu_notify_now_playing();
	frameprof_end(PROF_NP_LISTS);

	// Wind the progress display back to the start straight away, so the bar
	// never sits at the previous track's position while the new one loads.
	lv_slider_set_value(progress_slider, 0, LV_ANIM_OFF);
	set_progress_label(0, 0);

	device_state_t state;
	device_state_get(&state);

	apply_live_mode(state.live);
	apply_connecting();
	// Before anything is drawn: which arrangement this source gets decides
	// where the title goes and what colour everything is.
	update_layout(&state);
	// Decided from the path every time, as for books: a flag raised at start
	// would have to be cleared on every route by which the track can change,
	// and the first one forgotten leaves the player dressed as a podcast over
	// a song.
	apply_audiobook_mode(!state.live && audiobook_is_playing(),
						 !state.live && podcastcache_is_episode(state.current_file));

	if (state.live) {
		// The station on top, what it says is on air underneath -- the same
		// two lines as a track, carrying the two things a radio knows.
		scrolltext_set(song_title_label, state.metadata.title);
		scrolltext_set(song_artist_label, state.metadata.artist);
		lyrics_head_sync();
		alt_pills_sync();

		update_fav_button();
		update_format_label(&state);

		// The artwork is the station's own icon, downloaded when it started.
		// An empty path simply leaves the radio placeholder up.
		refresh_cover(state.live_cover);
		return;
	}

	frameprof_begin(PROF_NP_LAYOUT);
	const char *file = state.current_file;
	const char *slash = strrchr(file, '/');
	frameprof_begin(PROF_NP_TITLE);
	scrolltext_set(song_title_label, state.metadata.title[0] ? state.metadata.title : (slash ? slash + 1 : file));
	frameprof_end(PROF_NP_TITLE);

	// Whose name goes under the title: see metadata_shown_artist().
	const char *artist = metadata_shown_artist(&state.metadata);
	frameprof_begin(PROF_NP_ARTIST);
	scrolltext_set(song_artist_label, artist);
	frameprof_end(PROF_NP_ARTIST);
	frameprof_begin(PROF_NP_HEAD);
	lyrics_head_sync();
	frameprof_end(PROF_NP_HEAD);
	alt_pills_sync();
	frameprof_end(PROF_NP_LAYOUT);

	frameprof_begin(PROF_NP_FAV);
	update_fav_button();
	frameprof_end(PROF_NP_FAV);
	frameprof_begin(PROF_NP_BADGES);
	update_format_label(&state);
	update_qobuz_badge();
	frameprof_end(PROF_NP_BADGES);

	frameprof_begin(PROF_NP_LYRICS);
	lyrics_request(&state);
	frameprof_end(PROF_NP_LYRICS);

	frameprof_begin(PROF_NP_COVER);
	refresh_cover(file);
	frameprof_end(PROF_NP_COVER);
}

// The parts of the now-playing display a live stream changes while it runs:
// the station name (the stream's own name arrives with the connection), what
// is on air (an ICY title, every few minutes), the bitrate, and the artwork
// once the icon has been downloaded. Deliberately not a full
// refresh_now_playing(): that throws the decoded cover away and asks for it
// again, which for a station whose icon has not changed is work for nothing.
static void refresh_live_texts(const device_state_t *state) {
	scrolltext_set(song_title_label, state->metadata.title);
	scrolltext_set(song_artist_label, state->metadata.artist);
	lyrics_head_sync();
	alt_pills_sync();
	update_fav_button();
	update_format_label(state);

	if (strcmp(cover_shown_path, state->live_cover) != 0) {
		refresh_cover(state->live_cover);
	}
}

// The mirror of the Qobuz favourites has arrived, so the star may need to
// change face. It comes from the qobuzsync worker, hence the bounce onto the
// UI thread.
static void update_fav_button(void);

static void fav_refresh_async(void *user) {
	(void)user;
	update_fav_button();
}

static void qobuz_favorites_changed(void) { gui_post(fav_refresh_async, NULL); }
static void tidal_favorites_changed(void) { gui_post(fav_refresh_async, NULL); }

// The Qobuz id of the loaded track, 0 when what is playing does not come from
// Qobuz. This is the question that splits the star and "add to playlist" in
// two: a user's own file is written to the local database, a Qobuz track to
// the account.
static long current_qobuz_track(void) {
	device_state_t state;
	device_state_get(&state);
	return state.current_file[0] ? qobuzcache_track_id(state.current_file) : 0;
}

// The podcast the playing episode belongs to, plus what following it needs:
// title, author and cover. All from the sidecar written when the episode was
// downloaded -- no network, which listening to an already cached episode may
// well not have.
static bool podcast_current_feed(long long *id_out, podcast_feed_t *feed_out) {
	device_state_t state;
	device_state_get(&state);
	if (state.live || !state.current_file[0]) {
		return false;
	}
	long long id = 0;
	if (!podcastcache_feed_id(state.current_file, &id) || id <= 0) {
		return false;
	}
	if (id_out) {
		*id_out = id;
	}
	if (feed_out) {
		memset(feed_out, 0, sizeof(*feed_out));
		feed_out->id = id;
		// The podcast name is also the episode's album tag: see
		// podcastcache_write_sidecars(), which writes it to both places on
		// purpose.
		snprintf(feed_out->title, sizeof(feed_out->title), "%.*s", (int)sizeof(feed_out->title) - 1,
				 state.metadata.album);
		podcastcache_tag(state.current_file, "feed_author", feed_out->author, sizeof(feed_out->author));
		podcastcache_tag(state.current_file, "feed_image", feed_out->image, sizeof(feed_out->image));
	}
	return true;
}

static bool current_podcast_episode(void) {
	device_state_t state;
	device_state_get(&state);
	return state.current_file[0] && podcastcache_is_episode(state.current_file);
}

// The playing episode's podcast, for callers outside the player (the control
// centre): the same lookup the player uses for its own star, exposed so both
// stars say the same thing.
bool player_current_podcast_feed(long long *id_out, podcast_feed_t *feed_out) {
	return podcast_current_feed(id_out, feed_out);
}

// The same for Tidal. Two separate questions rather than one returning "which
// service": the two caches live in different directories, so a path can answer
// yes to at most one, which makes it impossible by construction for the player
// to believe a track comes from both.
static long current_tidal_track(void) {
	device_state_t state;
	device_state_get(&state);
	return state.current_file[0] ? tidalcache_track_id(state.current_file) : 0;
}

// The Qobuz mark, top right, when what is playing comes from there. Same place
// and same purpose as the live badge for radio: say where the music comes from
// without taking space from anything else.
//
// The question is the star's own -- is the file in the Qobuz cache? -- so
// there is no extra state to keep in step.
static void update_qobuz_badge(void) {
	// The marks share one corner and can never appear together: a track comes
	// from one service only. So which one shows is decided here, rather than
	// leaving each badge to hide itself -- which is how they would eventually
	// have overlapped.
	bool from_qobuz = !live_mode && current_qobuz_track() != 0;
	bool from_tidal = !live_mode && !from_qobuz && current_tidal_track() != 0;
	bool from_podcast = !live_mode && !from_qobuz && !from_tidal && current_podcast_episode();

	if (qobuz_badge) {
		if (from_qobuz) {
			lv_obj_set_hidden(qobuz_badge, false);
			lv_obj_move_foreground(qobuz_badge);
		} else {
			lv_obj_set_hidden(qobuz_badge, true);
		}
	}
	if (tidal_badge) {
		if (from_tidal) {
			lv_obj_set_hidden(tidal_badge, false);
			lv_obj_move_foreground(tidal_badge);
		} else {
			lv_obj_set_hidden(tidal_badge, true);
		}
	}
	if (podcast_badge) {
		if (from_podcast) {
			lv_obj_set_hidden(podcast_badge, false);
			lv_obj_move_foreground(podcast_badge);
		} else {
			lv_obj_set_hidden(podcast_badge, true);
		}
	}
}

// Repaints the star from the database: filled yellow when the loaded track
// is a favourite, quiet outline otherwise.
static void update_fav_button(void) {
	if (!fav_btn_icon) {
		return;
	}

	// A station is starred in the radio favourites, not the music library:
	// there is no file to put in the database, and the list the star feeds is
	// the one on the Radio page.
	radio_station_t station;
	bool starred = false;
	long qobuz_id = 0;
	long tidal_id = 0;
	long long feed_id = 0;
	if (radio_current_station(&station)) {
		starred = radio_fav_contains(station.uuid);
	} else if (podcast_current_feed(&feed_id, NULL)) {
		// On a podcast the star does not mark a liked track: it follows the
		// podcast, so it turns up in the followed list without searching. The
		// list is on the card, so the answer is immediate and nothing has to
		// be asked of the network.
		starred = podcastsubs_is_followed(feed_id);
	} else if ((qobuz_id = current_qobuz_track()) > 0) {
		// A Qobuz track: the star reports what is on the account, not what is
		// in the local database. Until the mirror arrives it is drawn empty
		// and asked for -- one request per session, not one per song.
		starred = qobuzsync_is_favorite(qobuz_id);
		qobuzsync_favorites_refresh(qobuz_favorites_changed);
	} else if ((tidal_id = current_tidal_track()) > 0) {
		starred = tidalsync_is_favorite(tidal_id);
		tidalsync_favorites_refresh(tidal_favorites_changed);
	} else {
		// device_state, not audio_get_current_file(): the star is drawn from
		// one source and toggled from another, and when the two disagreed the
		// star never filled in -- the tap wrote the right track to the
		// database while the drawing looked up a different one.
		device_state_t state;
		device_state_get(&state);
		// Read ahead while the previous track played when it can be (see
		// device_state_prefetch); the database otherwise.
		if (!state.current_file[0]) {
			starred = false;
		} else if (!device_state_prefetched_fav(state.current_file, &starred)) {
			starred = library_fav_contains(state.current_file);
		}
	}

	// The star's colour is decided here and nowhere else. This function runs
	// last on every track change, so anything that tints the star elsewhere is
	// overwritten.
	//
	// An empty star is drawn in the disc's own ink so that it reads on it; a
	// filled one keeps the yellow, which is the same on every page of the
	// player and is what says "favourite" rather than "here is the album's
	// colour again".
	if (starred) {
		lv_image_set_src(fav_btn_icon, &icon_star_filled);
		lv_obj_set_style_image_recolor(fav_btn_icon, lv_color_make(246, 211, 45), 0); // Adwaita yellow
	} else {
		lv_image_set_src(fav_btn_icon, &icon_star);
		lv_obj_set_style_image_recolor(fav_btn_icon, layout_alt_now ? alt_ink() : lv_color_make(150, 150, 150), 0);
	}
	lv_obj_set_style_image_recolor_opa(fav_btn_icon, LV_OPA_COVER, 0);
}

// The outcome of a Qobuz star, from the qobuzsync worker. Success says
// nothing -- the filled star already did; failure has to be said, because the
// star had filled in optimistically and is about to empty again.
static char fav_error[192];

static void fav_error_async(void *user) {
	(void)user;
	if (fav_error[0]) {
		gui_notify_popup(fav_error);
	}
	update_fav_button();
}

static void qobuz_fav_done(bool ok, const char *error, void *user) {
	(void)user;
	snprintf(fav_error, sizeof(fav_error), "%s", ok ? "" : (error && error[0] ? error : tr("qobuz_favourites_failed")));
	gui_post(fav_error_async, NULL);
}

static void tidal_fav_done(bool ok, const char *error, void *user) {
	(void)user;
	snprintf(fav_error, sizeof(fav_error), "%s", ok ? "" : (error && error[0] ? error : tr("tidal_favourites_failed")));
	gui_post(fav_error_async, NULL);
}

static void fav_btn_event_cb(lv_event_t *e) {
	(void)e;

	radio_station_t station;
	if (radio_current_station(&station)) {
		if (radio_fav_contains(station.uuid)) {
			radio_fav_remove(station.uuid);
		} else {
			radio_fav_add(&station);
		}
		update_fav_button();
		return;
	}

	// A podcast: the star follows or unfollows it. Written to the card at
	// once, so switching the player off a moment later does not lose it.
	long long feed_id = 0;
	podcast_feed_t feed;
	if (podcast_current_feed(&feed_id, &feed)) {
		if (podcastsubs_is_followed(feed_id)) {
			podcastsubs_unfollow(feed_id);
		} else {
			podcastsubs_follow(&feed);
		}
		update_fav_button();
		return;
	}

	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0]) {
		return;
	}

	// A Qobuz track goes into the account's favourites. Saving it in the local
	// database would mean keeping the path of a cache file that will be gone
	// twenty tracks from now, and invisible from any other device.
	long qobuz_id = qobuzcache_track_id(state.current_file);
	if (qobuz_id > 0) {
		bool want = !qobuzsync_is_favorite(qobuz_id);
		qobuzsync_favorite_toggle(qobuz_id, want, qobuz_fav_done, NULL);
		update_fav_button(); // the mirror has already flipped: shows at once
		return;
	}

	long tidal_id = tidalcache_track_id(state.current_file);
	if (tidal_id > 0) {
		bool want = !tidalsync_is_favorite(tidal_id);
		tidalsync_favorite_toggle(tidal_id, want, tidal_fav_done, NULL);
		update_fav_button();
		return;
	}

	const char *title = state.metadata.title[0] ? state.metadata.title : strrchr(state.current_file, '/') + 1;
	// No notification popup: the star filling in is the whole feedback.
	library_fav_toggle(state.current_file, title, state.metadata.artist);
	update_fav_button();
}

// The format line, the way the stock player writes it: "16/44.1 FLAC".
static void update_format_label(const device_state_t *state) {
	// The mark beside it in Studio, which is the same question asked of the
	// stream rather than of the library. Here because this is where the format
	// line is worked out, and the two say one thing between them.
	if (layout_studio_now && studio_quality_icon) {
		const lv_image_dsc_t *mark = studio_quality_mark(state);
		if (mark) {
			gui_image_set_src(studio_quality_icon, mark);
			lv_obj_set_hidden(studio_quality_icon, false);
		} else {
			lv_obj_set_hidden(studio_quality_icon, true);
		}
	}

	if (!format_label) {
		return;
	}

	if (state->live) {
		// A lossy station is quoted by its bitrate, a lossless one by depth and
		// rate like a track from the card ("16/44.1 FLAC"). The codec is the
		// decoder's, not the directory's claim. Nothing at all until the first
		// frame has been decoded.
		radio_now_t now;
		radio_get_now(&now);
		if (now.bits > 0 && now.sample_rate > 0 && now.codec[0]) {
			gui_label_set_text_fmt(format_label, "%d/%g %s", now.bits, now.sample_rate / 1000.0, now.codec);
		} else if (now.bitrate > 0) {
			gui_label_set_text_fmt(format_label, "%d kbps %s", now.bitrate, now.codec[0] ? now.codec : "MP3");
		} else if (now.codec[0]) {
			gui_label_set_text(format_label, now.codec);
		} else {
			gui_label_set_text(format_label, "");
		}
		return;
	}

	if (state->stream_sample_rate <= 0 || !state->current_file[0]) {
		gui_label_set_text(format_label, "");
		return;
	}

	// Nothing on a podcast. "16/44.1 MP3" is true and useless: an episode is
	// not chosen for its audio quality, there is no better version to buy, and
	// the number takes the line under the title without saying anything worth
	// knowing.
	if (podcastcache_is_episode(state->current_file)) {
		gui_label_set_text(format_label, "");
		return;
	}

	const char *dot = strrchr(state->current_file, '.');
	char kind[8] = "";
	if (dot) {
		size_t k = 0;
		for (const char *c = dot + 1; *c && k < sizeof(kind) - 1; c++) {
			kind[k++] = (char)((*c >= 'a' && *c <= 'z') ? *c - 32 : *c);
		}
		kind[k] = '\0';
	}

	// A DSD track says what it is rather than what carries it: "24/176.4 DSF"
	// is true of a DSD64 file and of a DSD256 one, and tells the listener
	// nothing. How it gets there is not said: DoP is the only way it does.
	int dsd = audio_get_dsd_multiple();
	if (dsd > 0) {
		gui_label_set_text_fmt(format_label, "DSD%d", dsd);
		return;
	}

	// The codec name comes from the decoder, not the extension: an .m4a holds
	// either AAC or ALAC, and those read as two different things under a
	// cover. When the decoder has no name to give -- the libsndfile formats --
	// the extension still serves.
	char codec[16] = "";
	audio_get_stream_codec(codec, sizeof(codec));
	const char *name = codec[0] ? codec : kind;

	// A lossy format has no bit depth of its own: "16/44.1 MP3" describes the
	// PCM leaving the decoder, not the file, and is equally true of a 96 kbps
	// MP3 and a 320 kbps one. What an MP3, an AAC or an Opus is -- exactly as
	// for a radio station -- is its bitrate.
	if (audio_stream_is_lossy()) {
		// The nominal figure first, the one written in the file. It is the
		// only one showable on a Qobuz or Tidal track: the cache file is still
		// growing while it plays, so the bytes-over-seconds average climbed on
		// screen until the download finished and it settled.
		int kbps = audio_get_stream_bitrate_kbps();

		// When the file does not declare one (a variable-bitrate MP3, a silent
		// AAC) the real average stands in -- but only once the download has
		// finished, for the reason just given.
		struct stat st;
		if (kbps <= 0 && !qobuzcache_owns(state->current_file) && !tidalcache_owns(state->current_file) &&
			state->progress_total_secs > 0.5 && stat(state->current_file, &st) == 0 && st.st_size > 0) {
			kbps = (int)((double)st.st_size * 8.0 / state->progress_total_secs / 1000.0 + 0.5);
		}

		if (kbps > 0) {
			gui_label_set_text_fmt(format_label, "%d kbps %s", kbps, name);
			return;
		}
		// Neither one: the format name alone beats quoting a bit depth the
		// file does not have.
		gui_label_set_text(format_label, name);
		return;
	}

	int bits = audio_get_stream_bits();
	gui_label_set_text_fmt(format_label, "%d/%g %s", bits > 0 ? bits : 16, state->stream_sample_rate / 1000.0, name);
}

// The speed setting of what is playing: an episode has its own, apart from the
// books'.
static bool speed_is_podcast(void) { return podcast_mode && !audiobook_mode; }

static int speed_current_permille(void) {
	return speed_is_podcast() ? podcast_speed_permille() : audiobook_speed_permille();
}

// The gauge wears the accent colour whenever the book or episode is not at
// normal speed, the way the repeat glyph it stands in for marks a mode that is
// on.
static void update_speed_button(void) {
	if (!speed_btn_icon) {
		return;
	}
	bool normal = speed_current_permille() == AUDIOBOOK_SPEED_NORMAL;
	lv_obj_set_style_image_recolor(speed_btn_icon, normal ? lv_color_make(100, 100, 100) : chrome_accent(), 0);
	lv_obj_set_style_image_recolor_opa(speed_btn_icon, LV_OPA_COVER, 0);
}

static void speed_pick(void *user) {
	int permille = (int)(intptr_t)user;
	if (speed_is_podcast()) {
		podcast_set_speed_permille(permille);
		permille = podcast_speed_permille();
	} else {
		audiobook_set_speed_permille(permille);
		permille = audiobook_speed_permille();
	}
	// Straight through to the playback thread: it picks the factor up on the
	// next block, so the change is heard within a period rather than at the
	// next track.
	audio_set_speed((double)permille / 1000.0);
	update_speed_button();
}

static void speed_btn_event_cb(lv_event_t *e) {
	static const int CHOICES[] = {500, 750, 1000, 1250, 1500, 1750, 2000};
	static const char *const LABELS[] = {"0.5x", "0.75x", "1.0x", "1.25x", "1.5x", "1.75x", "2.0x"};
	enum { CHOICE_COUNT = sizeof(CHOICES) / sizeof(CHOICES[0]) };

	// Zeroed, not just filled field by field: popover_item_t also has
	// `checked`, and stack garbage in it puts a tick on a random entry -- along
	// with the different row layout a tick brings.
	//
	// The tick belongs here for the same reason it does in the Qobuz quality
	// menu: this is a choice between alternatives, not a list of actions, so
	// the current one has to be visible.
	popover_item_t items[CHOICE_COUNT] = {0};
	int current = speed_current_permille();
	for (int i = 0; i < CHOICE_COUNT; i++) {
		items[i].label = LABELS[i];
		items[i].action = speed_pick;
		items[i].user = (void *)(intptr_t)CHOICES[i];
		items[i].checked = CHOICES[i] == current;
	}
	popover_show(lv_event_get_current_target(e), items, CHOICE_COUNT);
}

// Updates the repeat-mode button's icon and colour to reflect the active mode.
// The icons are the white ARGB bitmaps generated from assets/icons/*.svg;
// recolouring them is what marks the mode as active or not.
static void update_repeat_button(void) {
	// The active mode wears the accent colour of the moment, read each time
	// rather than frozen, so it follows a change of accent.
	lv_color_t active = chrome_accent();
	lv_color_t inactive = lv_color_make(100, 100, 100);

	const lv_image_dsc_t *icon;
	lv_color_t color;

	switch (playlist_get_mode()) {
	case PLAYBACK_MODE_REPEAT_ONE:
		icon = &icon_repeat_one;
		color = active;
		break;
	case PLAYBACK_MODE_REPEAT_ALL:
		icon = &icon_repeat_all;
		color = active;
		break;
	case PLAYBACK_MODE_SHUFFLE:
		icon = &icon_shuffle;
		color = active;
		break;
	case PLAYBACK_MODE_SHUFFLE_REPEAT:
		icon = &icon_shuffle_repeat;
		color = active;
		break;
	case PLAYBACK_MODE_NORMAL:
	default:
		icon = &icon_repeat_off;
		color = inactive;
		break;
	}

	lv_image_set_src(repeat_btn_icon, icon);
	lv_obj_set_style_image_recolor(repeat_btn_icon, color, 0);
	lv_obj_set_style_image_recolor_opa(repeat_btn_icon, LV_OPA_COVER, 0);
}

// Called when the current track finished on its own: advances the folder queue
// per the active playback mode. If a next track starts, refresh the UI to it;
// otherwise playback simply stays stopped (end of folder in normal mode).
static void handle_track_finished(void) {
	// What is playing came from DLNA: the queue belongs to the phone, not to
	// the player. Tell it the track finished and wait for the next URL,
	// instead of drawing from a local queue one entry long.
	if (dlna_owns_playback()) {
		dlna_notify_finished();
		return;
	}

	// "Stop at the end of the episode": the queue is left exactly where it is,
	// which is what the player already does when a queue simply runs out. Asked
	// here and not at a boundary inside the file, because for a podcast the end
	// of the episode IS the end of the track -- there is no chapter to watch
	// for, which is what makes this the cheaper half of the audiobook's version.
	char path[512];
	audio_get_current_file(path, sizeof(path));
	if (podcast_stop_at_episode_end() && podcastcache_is_episode(path)) {
		fprintf(stderr, "player: end of the episode; not starting the next\n");
		refresh_now_playing();
		return;
	}

	if (device_state_advance_auto(NULL, 0)) {
		refresh_now_playing();
	} else {
		// The queue ran out: the track it ended on went into the history.
		trackmenu_notify_now_playing();
	}
}

// A track ended on its own: the next one is started now rather than at the
// next poll, while the PCM held for gapless still has its queue to play.
static void track_end_async(void *user) {
	(void)user;
	if (device_state_take_completion()) {
		handle_track_finished();
	}
}

// Playback thread.
static void track_end_hook(void) { gui_post(track_end_async, NULL); }

// Reads the current device state and reconciles the UI against it. This is the
// single point that keeps the play/pause button and the progress bar from
// going stale once a track finishes on its own.
static const char *const PROF_UP_UI = "up_progress_ui";
static const char *const PROF_UP_LED = "up_led";
static const char *const PROF_UP_REMEMBER = "up_remember_note";

static void update_progress(void) {
	// Auto-advance or loop the folder when the track has played through.
	if (device_state_take_completion()) {
		handle_track_finished();
	}

	device_state_t state;
	device_state_get(&state);

	// The live source moves under the player: the station's own name comes
	// with the connection, the on-air title arrives later and keeps changing,
	// the artwork lands when its download finishes -- and the whole thing
	// ends by itself when a station drops for good. None of that goes through
	// a track change, so it is followed here.
	{
		static bool was_live;
		static unsigned last_serial;
		unsigned serial = radio_now_serial();

		if (state.live != was_live) {
			was_live = state.live;
			last_serial = serial;
			// In or out of live: the whole display changes hands.
			refresh_now_playing();
			device_state_get(&state);
		} else if (state.live && serial != last_serial) {
			last_serial = serial;
			refresh_live_texts(&state);
		}
	}

	// Asked on every poll and not only at a track change, for the routes that
	// are not one: a phone claiming the output over AirPlay does not change the
	// track -- it takes the DAC away under the one that is loaded -- and the
	// answer here has to follow it. Two string compares and a struct copy,
	// twice a second.
	update_layout(&state);
	// Likewise the transport: radio.txt can be reloaded under a station, which
	// decides whether previous and next are there for it.
	apply_live_mode(state.live);
	apply_connecting();

	// The bar itself is set by set_progress_label() below, which knows
	// whether it stands for the file or for the chapter.
	if (state.progress_total_secs > 0) {
		current_total_length = state.progress_total_secs;
	}

	// The truth the smoothing works from, and the moment it was true. Taken
	// here and not in the smoothing itself, so the two can never drift: every
	// poll puts the interpolation back on the playback thread's own figure.
	progress_anchor_secs = state.progress_current_secs;
	progress_anchor_tick = lv_tick_get();
	progress_running = state.status == AUDIO_STATUS_PLAYING && state.progress_total_secs > 0;
	smooth_timer_sync();

	// With the panel dark the poll is still the heartbeat of playback (queue
	// advance, LED, remembered position), but nothing it would paint can be
	// seen: the labels, the slider, the waveform, the lyrics and their worker.
	// The first poll after the wake (readied by power.c) does all of it. Books
	// are left alone: their chapter view is worked out in set_progress_label().
	bool paint = power_screen_is_on() || audiobook_mode || state.live;
	frameprof_begin(PROF_UP_UI);
	if (paint) {
		set_progress_label(state.progress_current_secs, current_total_length);
		wave_refresh(cover_shown_path, state.progress_current_secs);
	}
	// Playback's own read-ahead, not painting: it runs with the panel dark too.
	if (state.status == AUDIO_STATUS_PLAYING && !state.live) {
		device_state_read_ahead(state.progress_current_secs, state.progress_total_secs);
	}
	if (paint) {
		lyrics_request(&state);
		lyrics_follow(state.progress_current_secs);
	}
	apply_playback_status(state.status);
	// Asked every poll rather than only on a track change: the answer comes
	// from the audiobook index, which a scan finishing can change under a
	// track that is already playing. Both answers come from the path every
	// time -- a flag raised at start would have to be cleared on every route
	// by which the track can change, and the first one forgotten leaves the
	// player dressed as a podcast over a song.
	apply_audiobook_mode(!state.live && audiobook_is_playing(),
						 !state.live && podcastcache_is_episode(state.current_file));
	if (paint) {
		update_format_label(&state); // the rate/bits settle shortly after the start
	}
	frameprof_end(PROF_UP_UI);

	// The status LED follows playback promptly from here (the battery poll
	// only comes round once a minute). Only writes when the colour changes.
	// A podcast lights purple instead of the sample-rate colour.
	frameprof_begin(PROF_UP_LED);
	led_update_playback(state.status == AUDIO_STATUS_PLAYING, state.stream_sample_rate,
						!state.live && podcastcache_is_episode(state.current_file),
						!state.live && audio_get_dsd_multiple() > 0);
	frameprof_end(PROF_UP_LED);

	// Where the book got to. Not the same thing as "remember track" below and
	// not subject to its switch: that setting is about which track comes back
	// on the next boot, while this is the whole point of a book -- it is
	// remembered per book, it survives listening to something else in between,
	// and turning it off would make the feature meaningless. Throttled inside
	// audiobook_note_position(); forced whenever playback stops or pauses, so
	// a shutdown loses at most a few seconds.
	if (!state.live && audiobook_mode) {
		bool stopped_itself = audiobook_auto_stop(&state);

		static audio_status_t book_last_status = AUDIO_STATUS_STOPPED;
		bool changed = state.status != book_last_status;
		book_last_status = state.status;
		// Not when it has just stopped itself: that path already wrote the
		// position it stopped at, and this snapshot is from before the seek.
		if (!stopped_itself) {
			audiobook_note_position(state.progress_current_secs, state.progress_total_secs,
									changed || state.status != AUDIO_STATUS_PLAYING);
		}
	}

	// Where a podcast episode got to, kept per episode in the same spirit:
	// throttled inside podcaststate_note(), forced when the playback state
	// changes.
	if (!state.live && podcastcache_is_episode(state.current_file)) {
		static audio_status_t episode_last_status = AUDIO_STATUS_STOPPED;
		bool episode_changed = state.status != episode_last_status;
		episode_last_status = state.status;
		podcaststate_note(state.current_file, state.progress_current_secs, state.progress_total_secs,
						  episode_changed);
	}

	// "Remember track". The decision, and the writing, live in device_state.c:
	// the power menu and the automatic shutdown have to do the same thing on
	// their way out, and neither of them comes through here.
	frameprof_begin(PROF_UP_REMEMBER);
	device_state_remember_note();
	frameprof_end(PROF_UP_REMEMBER);
}

static void play_btn_event_cb(lv_event_t *e) {
	(void)e;

	// A live stream has no position to come back to, so this is a stop, not a
	// pause: the connection goes, the station stays on screen, and pressing
	// play again reconnects to it. Not while it is still connecting.
	if (radio_is_active()) {
		if (radio_is_connecting()) {
			return;
		}
		if (radio_is_playing()) {
			radio_stop();
		} else {
			radio_resume();
		}
		player_refresh_now_playing();
		return;
	}

	audio_status_t new_status = device_state_toggle_play_pause();
	apply_playback_status(new_status); // optimistic UI update; next poll reconciles against the real status
}

// What the on-screen prev/next buttons do. On a book or a podcast they are
// jumps of seconds; on a track they are previous/next, Spotify style -- within
// the first few seconds of a track prev moves to the previous one, later it
// restarts the current one. Public so the control centre can do exactly this:
// the same buttons cannot mean two different things in the two places. (The
// physical side keys stay episode changes on a podcast: see
// player_key_prev/next.)
// Previous and next on a stream. True when the press was the stream's: it
// changed station along the list the station came from, or there was nowhere
// to go, or the station is still connecting (see apply_connecting()), and it
// did nothing. False when no station is loaded and the press is the queue's.
static bool live_step(int step) {
	if (!radio_is_active()) {
		return false;
	}
	if (radio_is_connecting()) {
		return true;
	}
	if (radio_step(step)) {
		refresh_now_playing();
	}
	return true;
}

void player_screen_prev(void) {
	if (live_step(-1)) {
		return;
	}

	device_state_t state;
	device_state_get(&state);

	// On a book these two are not "previous chapter" and "next chapter": they
	// are ten seconds back and ten seconds on, which is what somebody who
	// missed a sentence reaches for. Chapters have their own list.
	if (audiobook_mode || podcast_mode) {
		int back = podcast_mode && !audiobook_mode ? podcast_skip_back() : audiobook_skip_back();
		audiobook_seek_by(-(double)back, &state);
		update_progress();
		return;
	}

	// Past the first three seconds, prev restarts the track instead of leaving
	// it: the same rule every player follows, so a double press is what goes
	// back.
	if (state.progress_current_secs > 3.0) {
		device_state_seek(0); // far enough in -> restart current track
	} else if (device_state_prev(NULL, 0)) {
		refresh_now_playing(); // near the start -> go to previous track
	} else {
		device_state_seek(0); // no queue -> just restart
	}

	update_progress();
}

// Profiling marks for a skip from the player (frameprof.h): the queue step and
// track load, the repaint of the player, and the progress bar.
static const char *const PROF_STEP = "skip_queue_and_load";
static const char *const PROF_REFRESH = "skip_refresh_now_playing";
static const char *const PROF_PROGRESS = "skip_update_progress";

void player_screen_next(void) {
	if (live_step(1)) {
		return;
	}

	if (audiobook_mode || podcast_mode) {
		device_state_t state;
		device_state_get(&state);
		int fwd = podcast_mode && !audiobook_mode ? podcast_skip_forward() : audiobook_skip_forward();
		audiobook_seek_by((double)fwd, &state);
		update_progress();
		return;
	}

	frameprof_begin(PROF_STEP);
	bool moved = device_state_next(NULL, 0);
	frameprof_end(PROF_STEP);
	if (moved) {
		frameprof_begin(PROF_REFRESH);
		refresh_now_playing();
		frameprof_end(PROF_REFRESH);
	}
	frameprof_begin(PROF_PROGRESS);
	update_progress();
	frameprof_end(PROF_PROGRESS);
}

// Held, << and >> seek like the transport keys do (system.c seek_step), a step
// every quarter second that grows with the hold. The release is not also a skip.
#define TOUCH_SEEK_REPEAT_MS 250
static bool touch_seeked;
static uint32_t touch_seek_last;
static uint32_t touch_seek_start;

// The step grows with the hold, as for the keys: keep in step with
// seek_step_seconds() in system.c.
static double touch_seek_step_seconds(uint32_t held_ms, int total_secs) {
	double step = held_ms < 1000 ? 2.0 : held_ms < 2000 ? 5.0 : held_ms < 4000 ? 10.0 : held_ms < 7000 ? 20.0 : 30.0;
	double cap = total_secs / 30.0;
	if (cap < 2.0) {
		cap = 2.0;
	}
	return step < cap ? step : cap;
}

static void touch_seek_event_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_PRESSED) {
		touch_seeked = false;
		touch_seek_start = lv_tick_get();
		return;
	}
	// The scrub only moves a number until it is committed, as the keys do on release.
	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		if (touch_seeked) {
			device_state_scrub_commit();
		}
		return;
	}
	// LONG_PRESSED_REPEAT comes every 100 ms; the step is paced here.
	if (touch_seeked && lv_tick_elaps(touch_seek_last) < TOUCH_SEEK_REPEAT_MS) {
		return;
	}
	device_state_t state;
	device_state_get(&state);
	if (state.live || state.progress_total_secs <= 0) {
		return;
	}
	touch_seeked = true;
	touch_seek_last = lv_tick_get();
	double step = touch_seek_step_seconds(lv_tick_elaps(touch_seek_start), state.progress_total_secs);
	device_state_scrub_by(lv_event_get_user_data(e) ? step : -step);
}

static void prev_btn_event_cb(lv_event_t *e) {
	(void)e;
	if (touch_seeked) {
		touch_seeked = false;
		return;
	}
	player_screen_prev();
}

static void next_btn_event_cb(lv_event_t *e) {
	(void)e;
	if (touch_seeked) {
		touch_seeked = false;
		return;
	}
	player_screen_next();
}

// --- hardware transport keys ---
//
// They do exactly what the on-screen buttons do, whichever page is showing:
// the player does not have to be open for the keys on the side of the device
// to work.

void player_key_play_pause(void) {
	apply_playback_status(device_state_toggle_play_pause());
}

void player_key_next(void) {
	if (live_step(1)) {
		return;
	}

	// On a track coming from DLNA, "next" is a question for the phone: the
	// queue is its own.
	if (dlna_owns_playback()) {
		dlna_request_next();
		return;
	}

	// The side keys stay previous/next track on a podcast, deliberately. A
	// book is a single file, so "next track" means nothing there and the keys
	// become jumps. A podcast sits in a real queue of episodes: if the side
	// keys also jumped thirty seconds, there would be no way to reach the next
	// episode without opening the queue. This way both exist -- jumps on
	// screen, episode change under the finger.
	if (audiobook_mode) {
		device_state_t state;
		device_state_get(&state);
		audiobook_seek_by((double)audiobook_skip_forward(), &state);
		update_progress();
		return;
	}

	if (device_state_next(NULL, 0)) {
		refresh_now_playing();
	}
	update_progress();
}

void player_key_prev(void) {
	if (live_step(-1)) {
		return;
	}

	device_state_t state;
	device_state_get(&state);

	if (dlna_owns_playback()) {
		// As elsewhere: within the first few seconds go back to the start,
		// later ask for the previous track -- only here the phone is asked.
		if (state.progress_current_secs > 3.0) {
			device_state_seek(0);
		} else {
			dlna_request_prev();
		}
		return;
	}

	if (audiobook_mode) {
		audiobook_seek_by(-(double)audiobook_skip_back(), &state);
		update_progress();
		return;
	}

	if (state.progress_current_secs > 3.0) {
		device_state_seek(0);
	} else if (device_state_prev(NULL, 0)) {
		refresh_now_playing();
	} else {
		device_state_seek(0);
	}
	update_progress();
}

// The book's menu. A book with no chapters still lists them, and says so when
// asked: a plainer answer than an entry that comes and goes between books.
static void book_chapters_action(void *user) {
	(void)user;
	if (audiobook_has_chapters) {
		chapters_open();
	} else {
		gui_notify_popup("player_this_audiobook_has_no_chapters_2");
	}
}

static void book_add_bookmark_action(void *user) {
	(void)user;
	audiobookextras_add_bookmark();
}

static void book_bookmarks_action(void *user) {
	(void)user;
	audiobookextras_open_current_bookmarks();
}

static void book_summary_action(void *user) {
	(void)user;
	audiobookextras_open_current_summary();
}

// The overflow button: the book's menu on a book, the episode queue on a
// podcast, the track menu on anything else.
static void more_btn_event_cb(lv_event_t *e) {
	if (audiobook_mode) {
		static const popover_item_t BOOK_ITEMS[] = {
			{"chapters", book_chapters_action, NULL},
			{"audiobook_add_bookmark", book_add_bookmark_action, NULL},
			{"bookmarks", book_bookmarks_action, NULL},
			{"audiobook_summary", book_summary_action, NULL},
		};
		popover_show(lv_event_get_current_target(e), BOOK_ITEMS, (int)(sizeof(BOOK_ITEMS) / sizeof(BOOK_ITEMS[0])));
		return;
	}
	// A podcast has no chapters: it has episodes, and the episodes are already
	// the queue. The button goes straight there.
	if (podcast_mode) {
		trackmenu_open_queue();
		return;
	}
	trackmenu_open(lv_event_get_current_target(e));
}

// Steps to the next playback mode: normal, loop the queue, loop the track,
// shuffle once through, shuffle round and round.
static void repeat_btn_event_cb(lv_event_t *e) {
	(void)e;
	playlist_cycle_mode();
	update_repeat_button();
}

static void progress_slider_event_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);

	if (code == LV_EVENT_PRESSED) {
		lv_timer_pause(progress_slider_timer); // the poll must not fight the finger
		if (smooth_timer) {
			lv_timer_pause(smooth_timer); // nor the smoothing, which would drag it back
		}
	}

	if (code == LV_EVENT_RELEASED) {
		lv_timer_resume(progress_slider_timer);

		int value = lv_slider_get_value(progress_slider);

		double seconds = view_seconds(value);

		device_state_seek(seconds);

		lv_timer_resume(progress_slider_timer);
	}

	if (code == LV_EVENT_VALUE_CHANGED) {
		// While dragging, the left clock previews where the knob would land.
		int value = lv_slider_get_value(progress_slider);

		double seconds = view_seconds(value);

		formatDoubleSeconds(seconds - view_start, progress_label_text, sizeof(progress_label_text));
		lv_label_set_text(elapsed_label, progress_label_text);

		// And so does the waveform, when that is what is being dragged. The
		// poll is paused for the duration of the press, so without this the
		// bars would stand still under the finger and only jump at the release.
		wave_preview(seconds);
	}
}

// The poll: collects finished artwork and reconciles the UI with playback.
// Carries the bar and the shape of the track forward between polls.
//
// Draws nothing unless the picture would change: the slider is told a value
// only when its own scale says a different one, and the waveform only when the
// playhead has crossed into another pixel. Both are compared against what was
// last drawn rather than against what was last computed, so a tick that changes
// nothing costs nothing.
static void smooth_timer_cb(lv_timer_t *timer) {
	if (!progress_running || view_length <= 0.1) {
		return;
	}
	// Nothing is drawn for a screen nobody is looking at, or for a page that is
	// not the one on show. Drawing a dark panel is work this device cannot
	// spare and has paid for before. And the timer itself is not left running
	// for it either: sixteen wake-ups a second to find that out. It is started
	// again by smooth_timer_sync() when the sheet opens or the screen comes back.
	if (!sheet_open || !power_screen_is_on()) {
		lv_timer_pause(timer);
		return;
	}

	double seconds = progress_anchor_secs + (double)lv_tick_elaps(progress_anchor_tick) / 1000.0;
	if (seconds > view_start + view_length) {
		seconds = view_start + view_length; // the poll will say what happens next
	}

	int value = bar_value(seconds);
	progress_slider_set(value);
	wave_preview(seconds);
	lyrics_follow(seconds);
}

// The worker's answers, picked up the moment they exist. They used to wait for
// the progress poll, every half second, and a track change asks two questions
// in turn -- the picture's id, then the pictures -- so a cover already in the
// cache could take up to a second to appear. The poll still picks them up too;
// both are no-ops with nothing new.
static void cover_answer_arrived(void *user) {
	(void)user;
	apply_cover_id_result();
	apply_cover_result();
}

static void cover_answer_ready(void) { gui_post(cover_answer_arrived, NULL); }

static void progress_slider_timer_cb(lv_timer_t *timer) {
	(void)timer;
	apply_cover_id_result(); // "is the new track's picture the one already up?"
	apply_cover_result();	 // artwork the worker has finished since last tick
	update_progress();
}

// The boot-time restore: the remembered track, paused at its position.
void player_restore_track(const char *filepath, double position) {
	device_state_restore_file(filepath, position);
	player_refresh_now_playing();
}

// The same, with the queue that was saved alongside it.
void player_restore_index(library_index_t *ix, int start_index, const char *const *extra, const int *extra_slots,
						  int extra_count, const char *filepath, double position) {
	device_state_restore_index(ix, start_index, extra, extra_slots, extra_count, filepath, position);
	player_refresh_now_playing();
}

void player_restore_list(const char *const *list, int count, int start_index, bool custom, const char *filepath,
						 double position) {
	device_state_restore_list(list, count, start_index, custom, filepath, position);
	player_refresh_now_playing();
}

// Loads and starts a new file, updating the now-playing info. Also rebuilds
// the folder queue inside device_state, so playback carries on to the
// following tracks when this one finishes.
void player_play_file(const char *filepath) {
	device_state_play_file(filepath); // builds folder queue, loads metadata, starts playback

	player_refresh_now_playing();
}

// audio_pause() rather than device_state_toggle_play_pause(): this is a pause,
// not a toggle, and the toggle would start a stopped radio station playing
// again. The button glyph is corrected here rather than left to the next poll
// because half a second of a pause button over a paused track reads as a
// control that did not take.
player_layout_t player_layout_get(void) { return layout_choice; }

bool player_cover_accent(void) { return cover_accent; }

void player_cover_accent_set(bool on) {
	if (on == cover_accent) {
		return;
	}
	cover_accent = on;
	config_set_bool("screen", "cover_accent", on);
	config_save();
	paint_transport_tint();
}

void player_layout_set(player_layout_t layout) {
	if (layout == layout_choice) {
		return;
	}
	layout_choice = layout;
	config_set_int("screen", "player_layout_alt", (int)layout);
	config_save();

	// Not apply_layout() directly: choosing Alternative while a radio station
	// is on must not put the alternative arrangement up. What the setting does
	// is change the answer update_layout() will give, so it is asked again.
	device_state_t state;
	device_state_get(&state);
	update_layout(&state);
}

void player_output_unplugged(const char *what) {
	if (audio_get_status() != AUDIO_STATUS_PLAYING) {
		return;
	}
	printf("player: %s unplugged; pausing\n", what ? what : "the output");
	audio_pause();
	apply_playback_status(AUDIO_STATUS_PAUSED);
}

// A pause and not a stop, and for the same reason as the one above: the point
// of falling asleep to something is finding it where it was in the morning.
static void sleep_timer_cb(lv_timer_t *timer) {
	(void)timer;
	bool expired = false;
	for (int i = 0; i < SLEEPTIMER_COUNT; i++) {
		expired = sleeptimer_expired((sleeptimer_kind_t)i) || expired;
	}
	if (!expired || audio_get_status() != AUDIO_STATUS_PLAYING) {
		return;
	}

	// A book being put down has two things to settle that a track does not: the
	// rewind that the next play would otherwise add -- it is for a sentence
	// somebody interrupted, not for one the player stopped on purpose -- and
	// the position, written through rather than left to the ten second throttle.
	if (audiobook_is_playing()) {
		device_state_t state;
		device_state_get(&state);
		audiobook_suppress_rewind_once();
		audio_pause();
		apply_playback_status(AUDIO_STATUS_PAUSED);
		audiobook_note_position(state.progress_current_secs, 0, true);
		return;
	}

	audio_pause();
	apply_playback_status(AUDIO_STATUS_PAUSED);
}

// The artist line again, after "Use track artist" has been switched. Not a
// whole refresh_now_playing(): nothing else on the page has changed, and that
// would decode the cover and ask for the lyrics all over again.
void player_refresh_artist(void) {
	if (!song_artist_label) {
		return;
	}
	device_state_t state;
	device_state_get(&state);
	if (state.live || !state.current_file[0]) {
		return;
	}
	scrolltext_set(song_artist_label, metadata_shown_artist(&state.metadata));
	lyrics_head_sync();
	alt_pills_sync();
}

// Public face of refresh_now_playing, for callers that started playback
// through device_state themselves (the library lists hand over a whole
// queue rather than a single file).
void player_refresh_now_playing(void) {
	refresh_now_playing();

	// Playback may have been started by something that also set the mode --
	// the favourites shuffle button forces shuffle on its way in -- so the
	// repeat/shuffle glyph is refreshed here too, not only when it is tapped.
	update_repeat_button();

	device_state_t state;
	device_state_get(&state);
	apply_playback_status(state.status);
}


// ---------------------------------------------------------------------------
// the sheet: dragging the player in and out
// ---------------------------------------------------------------------------

// How far across the screen the drag has to get before letting go opens (or
// closes) it rather than snapping back. A third is enough to feel deliberate
// without demanding the whole width.
#define SHEET_COMMIT_FRACTION 3

#define SHEET_ANIM_MS 220

// Sideways movement past this is a drag, not a tap.
#define DRAG_COMMIT_PX ui_px(10)

static int sheet_width;
static lv_obj_t *sheet_under; // the page the sheet first slid over

// Where the sheet was when the finger went down, and where the finger was.
static int drag_origin_x;
static lv_point_t drag_start_point;
static bool dragging;

// True once a gesture has moved far enough sideways to count as a drag. Read
// by the tap handlers on the pages behind, so that swiping across a menu tile
// does not also select it.
static bool drag_committed;

static void sheet_anim_x_cb(void *obj, int32_t value) {
	lv_obj_set_x((lv_obj_t *)obj, (int)value);
	// The chevron belongs to the sheet while it moves: ride its edge, and
	// snap back into the page's corner once the sheet is fully away.
	back_btn_translate(value >= sheet_width ? 0 : value);
}

// Moves the sheet to `x` (0 = fully open, sheet_width = fully out of the way).
static void sheet_move_to(int x, bool animate) {
	if (x < 0) {
		x = 0;
	}
	if (x > sheet_width) {
		x = sheet_width;
	}

	// Never race a slide that is still running. A slide left to finish after a
	// new one has started parks the sheet off screen while everything else
	// believes it is open -- the page underneath showing, no status bar, and no
	// way back to the player.
	lv_anim_delete(player_screen, sheet_anim_x_cb);

	if (!animate) {
		lv_obj_set_x(player_screen, x);
		back_btn_translate(x >= sheet_width ? 0 : x);
		return;
	}

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, player_screen);
	lv_anim_set_exec_cb(&a, sheet_anim_x_cb);
	lv_anim_set_values(&a, lv_obj_get_x(player_screen), x);
	lv_anim_set_duration(&a, SHEET_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

bool player_sheet_is_open(void) { return sheet_open; }

bool player_sheet_drag_active(void) { return drag_committed; }

// The status bar over the open player, when it stays up there: blurring the
// cover under it, on nothing in Studio -- the blurred picture behind
// everything is already there -- and in the page's own colours while there is
// no picture at all, where white would be lost on a light theme.
static void bar_over_player_update(void) {
	if (!sheet_open || !bar_always) {
		return;
	}
	topbar_set_hidden(false);
	if (!current_cover.pixels) {
		topbar_set_over_artwork(false, false);
		return;
	}
	topbar_set_over_artwork(true, !layout_studio_now);
}

bool player_statusbar_always(void) { return bar_always; }

void player_set_statusbar_always(bool on) {
	if (on == bar_always) {
		return;
	}
	bar_always = on;
	back_btn_player_below_bar(on);

	// The marks in the artwork's corner, and everything lined up with the
	// chevron, follow it down or back up.
	if (!studio_up) {
		lv_obj_t *const marks[] = {live_badge, qobuz_badge, tidal_badge, podcast_badge};
		for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++) {
			if (marks[i]) {
				lv_obj_align(marks[i], LV_ALIGN_TOP_RIGHT, -BADGE_INSET, badge_top());
			}
		}
	}
	if (layout_studio_now) {
		studio_place();
	}
	lyrics_layout();

	if (!on) {
		if (sheet_open) {
			topbar_set_over_artwork(false, false);
			topbar_set_hidden(true);
		}
		return;
	}
	bar_over_player_update();
}

void player_sheet_open(bool animate) {
	// Nothing to show while another mode owns the sound: the sheet is about a
	// track this player is playing, and in Bluetooth receiver mode the track is
	// on somebody else's machine. Refusing to open is the honest answer -- an
	// opened sheet would show the last thing played here, paused, over music
	// that is coming out of the jack right now.
	if (btreceiver_is_active()) {
		return;
	}

	if (!sheet_open) {
		// Remember the opener: leaving the player must always land back here,
		// whatever the pages under the sheet did in the meantime.
		sheet_under = lv_screen_active();
	}
	sheet_open = true;
	smooth_timer_sync();
	back_btn_player_mode(true);
	lyrics_screen_hold_update();

	// Explicitly the bottom-most thing on the top layer: it covers the page it
	// slides over, while the status bar and the floating back button stay drawn
	// above it, exactly where the page underneath put them. Nothing shared is
	// moved or hidden, so nothing shared slides around during the drag.
	lv_obj_set_hidden(player_screen, false);
	lv_obj_move_background(player_screen);

	// Artwork runs to the top edge here, so the bar steps aside. It is hidden
	// rather than moved, which is why nothing slides during the drag. Unless
	// it is to stay up: then it goes over the artwork.
	if (bar_always) {
		bar_over_player_update();
	} else {
		topbar_set_hidden(true);
	}

	sheet_move_to(0, animate);
}

static void sheet_hidden_anim_ready_cb(lv_anim_t *a) {
	(void)a;
	if (!sheet_open) {
		lv_obj_set_hidden(player_screen, true);
	}
	// Whatever page the close landed on, the chevron's visibility is settled
	// once more now that the last animation frame has run -- the fix for the
	// chevron flashing on the main menu for a frame on the way back to it.
	back_btn_sync_visibility();
}

void player_sheet_close(bool animate) {
	bool was_open = sheet_open;
	sheet_open = false;
	smooth_timer_sync();
	back_btn_player_mode(false);
	lyrics_screen_hold_update();
	topbar_set_over_artwork(false, false);
	topbar_set_hidden(false);
	// The tab does not travel with the page: coming back to a player with a
	// panel hanging open is not what leaving it looked like.

	// Back where the player was opened from, always: if anything switched the
	// page underneath while the sheet was up, put the opener back.
	if (was_open && sheet_under && lv_screen_active() != sheet_under) {
		switch_screen_no_history(sheet_under);
	}
	// The page underneath stays loaded while the sheet covers it, so nothing
	// tells it to look at its list again.
	medialist_refresh_visible();

	if (!animate) {
		lv_obj_set_x(player_screen, sheet_width);
		back_btn_translate(0);
		lv_obj_set_hidden(player_screen, true);
		back_btn_sync_visibility();
		return;
	}

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, player_screen);
	lv_anim_set_exec_cb(&a, sheet_anim_x_cb);
	lv_anim_set_values(&a, lv_obj_get_x(player_screen), sheet_width);
	lv_anim_set_duration(&a, SHEET_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, sheet_hidden_anim_ready_cb);
	lv_anim_start(&a);
}

// Runs from lv_async_call, one tick after the gesture's own events (RELEASED,
// then CLICKED) have all been delivered: the suppression the flag exists for
// has happened by then, and nothing later may still be blocked by it.
static void drag_committed_clear_cb(void *unused) {
	(void)unused;
	drag_committed = false;
}

// One handler for both directions. `opening` is passed as the user data: true
// when the drag starts on something behind the sheet, false when it starts on
// the sheet itself.
static void sheet_drag_cb(lv_event_t *e) {
	bool opening = (bool)(uintptr_t)lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}

	if (code == LV_EVENT_PRESSED) {
		if (opening == sheet_open) {
			return; // dragging the wrong way for the current state
		}

		lv_indev_get_point(indev, &drag_start_point);
		drag_origin_x = opening ? sheet_width : 0;
		dragging = true;

		drag_committed = false;

		if (opening) {
			// It has to be on screen to be dragged, even if only by a pixel.
			lv_obj_set_hidden(player_screen, false);
			lv_obj_move_background(player_screen);
			lv_obj_set_x(player_screen, sheet_width);
		}
		return;
	}

	if (!dragging) {
		return;
	}

	lv_point_t point;
	lv_indev_get_point(indev, &point);
	int dx = point.x - drag_start_point.x;

	if (code == LV_EVENT_PRESSING) {
		// Vertical scrolling must still work: the moment LVGL is scrolling
		// something this press is a scroll, and once the movement reads as
		// vertical it stays a scroll -- no more sideways shimmy of the page
		// during a fast flick.
		if (!drag_committed) {
			if (lv_indev_get_scroll_obj(indev)) {
				dragging = false;
				return;
			}
			int dy = point.y - drag_start_point.y;
			if (LV_ABS(dy) >= DRAG_COMMIT_PX && LV_ABS(dy) > LV_ABS(dx)) {
				dragging = false;
				return;
			}
			if (LV_ABS(dx) < DRAG_COMMIT_PX || LV_ABS(dx) < 2 * LV_ABS(dy)) {
				return;
			}
			drag_committed = true;
		}

		sheet_move_to(drag_origin_x + dx, false);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		dragging = false;

		int travelled = opening ? -dx : dx;
		bool commit = travelled > (sheet_width / SHEET_COMMIT_FRACTION);

		if (opening) {
			commit ? player_sheet_open(true) : player_sheet_close(true);
		} else {
			commit ? player_sheet_close(true) : player_sheet_open(true);
		}

		// The flag must not outlive the gesture. Waiting for the next PRESSED on
		// an attached object is not enough -- the browser list is not one, so
		// every tap on a row after a swipe would be discarded as part of a drag.
		// Deferred rather than cleared here, because the CLICKED event of this
		// same gesture still has to see it.
		lv_async_call(drag_committed_clear_cb, NULL);
	}
}

// The control centre, from the player too.
//
// Every other page pulls it down from the status bar, and this page hides the
// status bar, so there is nothing to pull. This strip stands in for it:
// the same gesture, in the same place, on a piece of the artwork that has
// nothing else to do. The bar comes back for as long as the sheet is up (see
// quickpanel.c), so what the finger lands on afterwards is what it expected.
//
// Vertical dominance is required before it engages: a sideways drag across the
// top of the artwork is somebody putting the player away, not somebody
// reaching for the brightness.
static void panel_edge_drag_cb(lv_event_t *e) {
	static lv_point_t start;
	static bool tracking;
	static bool engaged;

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}

	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_PRESSED) {
		lv_indev_get_point(indev, &start);
		tracking = !quickpanel_is_open();
		engaged = false;
		return;
	}
	if (!tracking) {
		return;
	}

	lv_point_t p;
	lv_indev_get_point(indev, &p);
	int dy = p.y - start.y;
	int dx = p.x - start.x;

	if (code == LV_EVENT_PRESSING) {
		if (!engaged) {
			int adx = dx < 0 ? -dx : dx;
			if (dy < 8 || dy <= adx) {
				return; // still just a press, or a sideways one
			}
			engaged = true;
			quickpanel_drag_begin();
		}
		quickpanel_drag_update(dy);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		tracking = false;
		if (engaged) {
			engaged = false;
			quickpanel_drag_end();
		}
	}
}

void player_sheet_attach_drag(lv_obj_t *obj, bool opening) {
	if (!obj) {
		return;
	}

	lv_obj_set_clickable(obj, true);
	lv_obj_add_event_cb(obj, sheet_drag_cb, LV_EVENT_PRESSED, (void *)(uintptr_t)opening);
	lv_obj_add_event_cb(obj, sheet_drag_cb, LV_EVENT_PRESSING, (void *)(uintptr_t)opening);
	lv_obj_add_event_cb(obj, sheet_drag_cb, LV_EVENT_RELEASED, (void *)(uintptr_t)opening);
	lv_obj_add_event_cb(obj, sheet_drag_cb, LV_EVENT_PRESS_LOST, (void *)(uintptr_t)opening);
}

// Builds the player sheet: artwork, controls and the poll that drives them.
// Where the level meter goes (meterview.h): across the foot of the sleeve as
// it is drawn now, above Waveform's title and star where they sit on it. Not
// while the player is away or the lyrics are over the sleeve.
static bool meter_where(lv_area_t *rel) {
	if (!sheet_open || lyrics_now || !cover_panel) {
		return false;
	}
	lv_area_t panel, base;
	lv_obj_get_coords(cover_panel, &panel);
	if (cover_img && !lv_obj_is_hidden(cover_img) && lv_area_get_width(&cover_img->coords) > ui_px(100)) {
		lv_obj_get_coords(cover_img, &base);
	} else {
		base = panel;
		base.y2 = base.y1 + lv_area_get_width(&panel) - 1; // the sleeve's square
	}
	if (base.y2 > panel.y2) {
		base.y2 = panel.y2;
	}
	int32_t bottom = base.y2;
	if (layout_alt_now && alt_text_col && !lv_obj_is_hidden(alt_text_col)) {
		lv_area_t col;
		lv_obj_get_coords(alt_text_col, &col);
		if (col.y1 > base.y1 && col.y1 <= bottom) {
			bottom = col.y1 - ui_px(6);
		}
	}
	int32_t h = lv_area_get_width(&base) * 30 / 100;
	if (h < ui_px(110)) {
		h = ui_px(110);
	}
	rel->x1 = base.x1 - panel.x1;
	rel->x2 = base.x2 - panel.x1;
	rel->y2 = bottom - panel.y1;
	rel->y1 = rel->y2 - h + 1;
	return rel->y1 >= 0;
}

void player_init(gui_config_t *cfg) {
	// The artwork decodes on the shared worker; make sure it exists before
	// the first track can be started. Safe to call more than once.
	coverloader_start();
	coverloader_set_player_ready_cb(cover_answer_ready);
	// The next track's picture is not read ahead once the cover side has no
	// use for it.
	readahead_set_picture_known(cover_picture_known);

	lv_obj_add_style(player_screen, &theme_style_screen, 0);

	// The player is a panel parked just off the right edge (see player.h).
	sheet_width = (int)cfg->screen_width;
	lv_obj_set_size(player_screen, cfg->screen_width, cfg->screen_height);
	lv_obj_set_style_pad_all(player_screen, 0, 0);
	lv_obj_set_style_border_width(player_screen, 0, 0);
	lv_obj_set_style_radius(player_screen, 0, 0);
	lv_obj_set_scrollable(player_screen, false);
	lv_obj_set_pos(player_screen, sheet_width, 0);
	lv_obj_set_hidden(player_screen, true);


	// Geometry first, because everything else hangs off it: the artwork is a
	// square as wide as the screen, sitting flush at the top (this page hides
	// the status bar), and the controls get whatever height is left. On a panel
	// too short for that, the artwork gives up height rather than the controls.
	int cover_size = (int)cfg->screen_width;
	int menu_height = (int)cfg->screen_height - cover_size;
	if (menu_height < PLAYER_MENU_MIN_HEIGHT) {
		menu_height = PLAYER_MENU_MIN_HEIGHT;
		cover_size = (int)cfg->screen_height - menu_height;
		if (cover_size < 64)
			cover_size = 64;
	}

	cover_box_w = cover_size;
	cover_box_h = cover_size;
	backdrop_w = (int)cfg->screen_width;
	backdrop_h = menu_height;
	backdrop_studio_h = (int)cfg->screen_height;

	// The controls block. Its background is the current track's artwork,
	// flipped and blurred (set per track in refresh_cover); the panel colour is
	// what shows through when there is no artwork.
	player_menu = lv_obj_create(player_screen);
	lv_obj_set_size(player_menu, cfg->screen_width, menu_height);
	lv_obj_align(player_menu, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_add_style(player_menu, &theme_style_panel, 0);
	lv_obj_set_flex_flow(player_menu, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(player_menu, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_border_width(player_menu, 0, 0);
	lv_obj_set_style_radius(player_menu, 0, 0);
	lv_obj_set_style_pad_hor(player_menu, cfg->padding, 0);
	int spare = menu_height - PLAYER_MENU_REF_HEIGHT;
	menu_pad_ver = PLAYER_MENU_PAD_VER;
	menu_gap = PLAYER_MENU_GAP;
	if (spare > 0) {
		menu_pad_ver = PLAYER_MENU_PAD_VER + spare / 5;
		menu_gap = PLAYER_MENU_GAP + spare / 5;
	}
	lv_obj_set_style_pad_ver(player_menu, menu_pad_ver, 0);
	lv_obj_set_style_pad_gap(player_menu, menu_gap, 0);
	lv_obj_set_scrollable(player_menu, false);

	// Track info: the text on the left, the star and the format on the right.
	lv_obj_t *song_info = lv_obj_create(player_menu);
	lv_obj_set_size(song_info, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(song_info, 0, 0);
	lv_obj_set_style_border_width(song_info, 0, 0);
	lv_obj_set_style_radius(song_info, 0, 0);
	lv_obj_set_style_pad_all(song_info, 0, 0);
	lv_obj_set_style_pad_column(song_info, ui_px(10), 0);
	lv_obj_set_flex_flow(song_info, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(song_info, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(song_info, false);
	// Every time the row is laid out (a new text size, the star shown or
	// hidden, the row coming back from another arrangement) the title is lined
	// up with the star again.
	lv_obj_add_event_cb(song_info, song_info_layout_cb, LV_EVENT_LAYOUT_CHANGED, NULL);

	lv_obj_t *song_text = lv_obj_create(song_info);
	song_text_obj = song_text;
	lv_obj_set_height(song_text, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(song_text, 1);
	lv_obj_set_style_bg_opa(song_text, 0, 0);
	lv_obj_set_style_border_width(song_text, 0, 0);
	lv_obj_set_style_pad_all(song_text, 0, 0);
	lv_obj_set_flex_flow(song_text, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(song_text, false);

	// Long titles scroll rather than end in an ellipsis: on a 480 px panel a
	// good half of real album titles do not fit, and the tail is usually the
	// part that tells them apart.
	song_title_label = lv_label_create(song_text);
	lv_label_set_text(song_title_label, tr("player_no_track_loaded"));
	lv_obj_set_width(song_title_label, lv_pct(100));
	lv_obj_add_style(song_title_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(song_title_label, &font_ui_26, 0);
	scrolltext_apply(song_title_label);

	// The artist line -- see refresh_now_playing().
	song_artist_label = lv_label_create(song_text);
	lv_label_set_text(song_artist_label, "");
	lv_obj_set_width(song_artist_label, lv_pct(100));
	lv_obj_add_style(song_artist_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(song_artist_label, &font_ui_24, 0);
	scrolltext_apply(song_artist_label);

	// Right side: the favourite star over the stream format.
	lv_obj_t *song_side = lv_obj_create(song_info);
	song_side_obj = song_side;
	lv_obj_set_size(song_side, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(song_side, 0, 0);
	lv_obj_set_style_border_width(song_side, 0, 0);
	lv_obj_set_style_pad_all(song_side, 0, 0);
	lv_obj_set_style_pad_gap(song_side, ui_px(2), 0);
	lv_obj_set_flex_flow(song_side, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(song_side, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(song_side, false);

	lv_obj_t *fav_btn = lv_btn_create(song_side);
	fav_btn_obj = fav_btn;
	lv_obj_set_size(fav_btn, FAV_BTN_W, FAV_BTN_H);
	lv_obj_set_style_bg_opa(fav_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(fav_btn, 0, 0);
	lv_obj_set_style_shadow_width(fav_btn, 0, 0);
	lv_obj_set_style_pad_all(fav_btn, 0, 0);
	lv_obj_add_event_cb(fav_btn, fav_btn_event_cb, LV_EVENT_CLICKED, NULL);

	fav_btn_icon = lv_image_create(fav_btn);
	lv_obj_center(fav_btn_icon);
	update_fav_button();

	format_label = lv_label_create(song_side);
	lv_label_set_text(format_label, "");
	lv_obj_add_style(format_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(format_label, &font_ui_18, 0);

	// The shape of the track, and the slider laid over it that still does the
	// seeking. Two things in the same place rather than one: drawing a
	// waveform is one job and reading a drag is another, and the slider
	// already knows how to do the second.
	wave_box = lv_obj_create(player_menu);
	lv_obj_remove_style_all(wave_box);
	lv_obj_set_size(wave_box, lv_pct(100), WAVE_HEIGHT);
	lv_obj_set_scrollable(wave_box, false);
	lv_obj_set_hidden(wave_box, true);

	{
		wave_w = (int)cfg->screen_width - 2 * cfg->padding;
		if (wave_w < WAVEFORM_BARS) {
			wave_w = WAVEFORM_BARS;
		}
		wave_buf = malloc((size_t)wave_w * WAVE_HEIGHT * 3);
		if (wave_buf) {
			memset(wave_buf, 0, (size_t)wave_w * WAVE_HEIGHT * 3);
			wave_canvas = lv_canvas_create(wave_box);
			lv_canvas_set_buffer(wave_canvas, wave_buf, wave_w, WAVE_HEIGHT, LV_COLOR_FORMAT_RGB565A8);
			lv_obj_set_size(wave_canvas, wave_w, WAVE_HEIGHT);
			lv_obj_align(wave_canvas, LV_ALIGN_CENTER, 0, 0);
			lv_obj_set_clickable(wave_canvas, false);
		} else {
			fprintf(stderr, "player: no room for the waveform; the alternative layout keeps the bar\n");
		}
	}

	// Playback progress, drawn as described at PROGRESS_TRACK_HEIGHT.
	progress_slider = lv_slider_create(player_menu);
	lv_obj_set_width(progress_slider, lv_pct(100));
	lv_obj_set_height(progress_slider, PROGRESS_TRACK_HEIGHT);
	lv_slider_set_range(progress_slider, 0, 1000);

	lv_obj_set_style_radius(progress_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_bg_color(progress_slider, lv_color_make(255, 255, 255), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(progress_slider, LV_OPA_30, LV_PART_MAIN);

	lv_obj_set_style_radius(progress_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(progress_slider, theme()->accent, LV_PART_INDICATOR);

	lv_obj_set_style_radius(progress_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
	lv_obj_set_style_bg_color(progress_slider, theme()->accent, LV_PART_KNOB);
	lv_obj_set_style_border_color(progress_slider, lv_color_white(), LV_PART_KNOB);
	lv_obj_set_style_border_opa(progress_slider, LV_OPA_COVER, LV_PART_KNOB);
	lv_obj_set_style_border_width(progress_slider, PROGRESS_KNOB_RING, LV_PART_KNOB);
	lv_obj_set_style_pad_all(progress_slider, PROGRESS_KNOB_GROW, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(progress_slider, ui_px(8), LV_PART_KNOB);
	lv_obj_set_style_shadow_opa(progress_slider, LV_OPA_40, LV_PART_KNOB);
	lv_obj_set_style_shadow_color(progress_slider, lv_color_black(), LV_PART_KNOB);
	lv_obj_set_style_shadow_offset_y(progress_slider, ui_px(1), LV_PART_KNOB);

	lv_obj_add_event_cb(progress_slider, progress_slider_event_cb, LV_EVENT_ALL, NULL);

	// Polls playback to keep the slider, the clock and the play/pause icon in
	// sync. It runs from here on, slowing down rather than stopping when
	// nothing is playing (see apply_playback_status).
	smooth_timer = lv_timer_create(smooth_timer_cb, SMOOTH_PERIOD_MS, NULL);
	lv_timer_pause(smooth_timer); // the poll starts it when something is playing

	progress_slider_timer = lv_timer_create(progress_slider_timer_cb, POLL_PERIOD_IDLE_MS, NULL);
	audio_set_completion_hook(track_end_hook);
	// Slowed in standby, never stopped: this timer does more than paint --
	// update_progress() calls device_state_take_completion(), the engine that
	// advances the queue when a track ends on its own. Stopping it when the
	// screen goes off (on the grounds that nobody can see the slider) meant no
	// next track ever started with the screen dark, local files included,
	// until the screen came back on and consumed the completion left waiting.
	// The period is still apply_playback_status's to decide (fast while
	// playing, slow when stopped), in the dark too.
	power_slow_in_standby(progress_slider_timer, POLL_PERIOD_PLAYING_MS);

	// The sleep timer's own tick. Slowed in standby and not stopped, because
	// the screen being dark is the normal case for it -- and slowing it costs
	// nothing in accuracy: the countdown is against the clock, so a late tick
	// stops the music late by however late it was, not by a tick.
	sleep_timer = lv_timer_create(sleep_timer_cb, 1000, NULL);
	lv_timer_pause(sleep_timer); // apply_playback_status starts it when there is a stretch to count
	power_slow_in_standby(sleep_timer, 5000);

	lv_obj_t *below_slider_group = lv_obj_create(player_menu);
	below_slider_obj = below_slider_group;
	lv_obj_set_size(below_slider_group, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(below_slider_group, 0, 0);
	lv_obj_set_style_border_width(below_slider_group, 0, 0);
	lv_obj_set_style_radius(below_slider_group, 0, 0);
	lv_obj_set_style_pad_all(below_slider_group, 0, 0);
	lv_obj_set_scrollable(below_slider_group, false);

	// Elapsed and total time, tucked under the two ends of the bar.
	elapsed_label = lv_label_create(below_slider_group);
	lv_label_set_text(elapsed_label, "0:00");
	lv_obj_add_style(elapsed_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(elapsed_label, &font_ui_22, 0);
	lv_obj_set_align(elapsed_label, LV_ALIGN_LEFT_MID);

	remaining_label = lv_label_create(below_slider_group);
	lv_label_set_text(remaining_label, "0:00");
	lv_obj_add_style(remaining_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(remaining_label, &font_ui_22, 0);
	lv_obj_set_align(remaining_label, LV_ALIGN_RIGHT_MID);

	// Between them, in the middle the two clocks left empty. Dimmer than they
	// are: it is a bearing, not a reading.
	queue_position_label = lv_label_create(below_slider_group);
	lv_label_set_text(queue_position_label, "");
	lv_obj_add_style(queue_position_label, &theme_style_text_dim, 0);
	// A step below the clocks either side of it, as well as dimmer: it is a
	// bearing and they are the reading, and at the same size the three read as
	// one row of equals.
	lv_obj_set_style_text_font(queue_position_label, &font_ui_18, 0);
	lv_obj_set_align(queue_position_label, LV_ALIGN_CENTER);

	// Transport: prev, play/pause, next -- with the repeat toggle parked on
	// the left, out of the way of the transport controls.
	lv_obj_t *player_controls_buttons = lv_obj_create(player_menu);
	controls_row = player_controls_buttons;
	lv_obj_set_size(player_controls_buttons, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(player_controls_buttons, 0, 0);
	lv_obj_set_style_border_width(player_controls_buttons, 0, 0);
	lv_obj_set_style_radius(player_controls_buttons, 0, 0);
	lv_obj_set_style_pad_all(player_controls_buttons, 0, 0);
	lv_obj_set_flex_flow(player_controls_buttons, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(player_controls_buttons, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	// Lifted a touch off the screen edge.
	lv_obj_set_style_translate_y(player_controls_buttons, -ui_px(8), 0);

	// The repeat/shuffle button, kept out of the flex row so the transport
	// stays centred on the screen.
	lv_obj_t *repeat_btn = lv_btn_create(player_controls_buttons);
	repeat_btn_obj = repeat_btn;
	lv_obj_set_ignore_layout(repeat_btn, true);
	lv_obj_set_size(repeat_btn, ui_px(56), ui_px(56));
	lv_obj_set_style_bg_opa(repeat_btn, 0, 0);
	lv_obj_set_style_shadow_width(repeat_btn, 0, 0);
	lv_obj_add_event_cb(repeat_btn, repeat_btn_event_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_align(repeat_btn, LV_ALIGN_LEFT_MID, 0, 0);
	// The glyph stays 56 px; the area that takes the tap does not, out to the
	// height of the row and short of the previous button.
	lv_obj_set_ext_click_area(repeat_btn, CORNER_BTN_EXT_CLICK);

	repeat_btn_icon = lv_image_create(repeat_btn);
	lv_obj_center(repeat_btn_icon);
	update_repeat_button(); // the icon for the mode already in force

	// The playback-speed gauge, in exactly the same corner: on a book the
	// repeat mode is meaningless and the speed is the thing worth reaching,
	// so one hides and the other appears in its place.
	lv_obj_t *speed_btn = lv_btn_create(player_controls_buttons);
	speed_btn_obj = speed_btn;
	lv_obj_set_ignore_layout(speed_btn, true);
	lv_obj_set_size(speed_btn, ui_px(56), ui_px(56));
	lv_obj_set_style_bg_opa(speed_btn, 0, 0);
	lv_obj_set_style_shadow_width(speed_btn, 0, 0);
	lv_obj_add_event_cb(speed_btn, speed_btn_event_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_align(speed_btn, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_set_ext_click_area(speed_btn, CORNER_BTN_EXT_CLICK);
	lv_obj_set_hidden(speed_btn, true);

	speed_btn_icon = lv_image_create(speed_btn);
	lv_image_set_src(speed_btn_icon, &icon_play_speed);
	lv_obj_add_style(speed_btn_icon, &theme_style_icon, 0);
	lv_obj_center(speed_btn_icon);
	update_speed_button();

	// The overflow menu, mirroring the repeat button on the right: queue and
	// details in an iOS-style popover next to the button.
	lv_obj_t *more_btn = lv_btn_create(player_controls_buttons);
	more_btn_obj = more_btn;
	lv_obj_set_ignore_layout(more_btn, true);
	lv_obj_set_size(more_btn, ui_px(56), ui_px(56));
	lv_obj_set_style_bg_opa(more_btn, 0, 0);
	lv_obj_set_style_shadow_width(more_btn, 0, 0);
	lv_obj_add_event_cb(more_btn, more_btn_event_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_align(more_btn, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_ext_click_area(more_btn, CORNER_BTN_EXT_CLICK);

	lv_obj_t *more_icon = lv_image_create(more_btn);
	more_btn_icon = more_icon;
	lv_image_set_src(more_icon, &icon_ellipsis_vertical);
	lv_obj_add_style(more_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor(more_icon, lv_color_make(150, 150, 150), 0);
	lv_obj_set_style_image_recolor_opa(more_icon, LV_OPA_COVER, 0);
	lv_obj_center(more_icon);

	// No button chrome on prev/next: they sit on top of the blurred artwork,
	// and plain glyphs read better there than filled boxes.
	lv_obj_t *prev_btn = lv_btn_create(player_controls_buttons);
	prev_btn_obj = prev_btn;
	lv_obj_set_size(prev_btn, ui_px(76), ui_px(76));
	lv_obj_set_style_bg_opa(prev_btn, 0, 0);
	lv_obj_set_style_shadow_width(prev_btn, 0, 0);
	prev_icon = lv_image_create(prev_btn);
	lv_image_set_src(prev_icon, &icon_skip_back);
	lv_obj_add_style(prev_icon, &theme_style_icon, 0);
	lv_obj_center(prev_icon);

	lv_obj_add_event_cb(prev_btn, prev_btn_event_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(prev_btn, touch_seek_event_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(prev_btn, touch_seek_event_cb, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
	lv_obj_add_event_cb(prev_btn, touch_seek_event_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(prev_btn, touch_seek_event_cb, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_set_style_opa(prev_btn, LV_OPA_40, LV_STATE_DISABLED); // see apply_connecting()

	// Play/pause: a white disc, with the glyph carrying the colour.
	play_btn = lv_btn_create(player_controls_buttons);
	lv_obj_set_size(play_btn, ui_px(84), ui_px(84));
	lv_obj_set_style_radius(play_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(play_btn, lv_color_white(), 0); // repainted by set_over_cover
	lv_obj_set_style_bg_opa(play_btn, LV_OPA_COVER, 0);
	lv_obj_set_style_shadow_width(play_btn, 0, 0);
	lv_obj_add_event_cb(play_btn, play_btn_event_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_style_opa(play_btn, LV_OPA_40, LV_STATE_DISABLED);

	play_btn_icon = lv_image_create(play_btn);
	lv_obj_center(play_btn_icon);
	apply_playback_status(AUDIO_STATUS_STOPPED); // start on the play glyph

	lv_obj_t *next_btn = lv_btn_create(player_controls_buttons);
	next_btn_obj = next_btn;
	lv_obj_set_size(next_btn, ui_px(76), ui_px(76));
	lv_obj_set_style_bg_opa(next_btn, 0, 0);
	lv_obj_set_style_shadow_width(next_btn, 0, 0);
	next_icon = lv_image_create(next_btn);
	lv_image_set_src(next_icon, &icon_skip_forward);
	lv_obj_add_style(next_icon, &theme_style_icon, 0);
	lv_obj_center(next_icon);

	lv_obj_add_event_cb(next_btn, next_btn_event_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(next_btn, touch_seek_event_cb, LV_EVENT_PRESSED, (void *)1);
	lv_obj_add_event_cb(next_btn, touch_seek_event_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)1);
	lv_obj_add_event_cb(next_btn, touch_seek_event_cb, LV_EVENT_RELEASED, (void *)1);
	lv_obj_add_event_cb(next_btn, touch_seek_event_cb, LV_EVENT_PRESS_LOST, (void *)1);
	lv_obj_set_style_opa(next_btn, LV_OPA_40, LV_STATE_DISABLED);

	// Album art: a square as wide as the screen, flush against the top edge
	// (this page hides the status bar, see switch_screen). The picture is
	// centre-cropped to fill it rather than letterboxed. Sizes were worked out
	// at the top of this function, before the controls were laid out.
	cover_panel = lv_obj_create(player_screen);
	// Dragging the artwork to the right pushes the player back off screen.
	player_sheet_attach_drag(cover_panel, false);
	// Studio stretches the panel down over the top of the controls, where the
	// taller shape of the track can reach: presses there go to the slider.
	lv_obj_set_adv_hittest(cover_panel, true);
	lv_obj_add_event_cb(cover_panel, cover_panel_hit_test_cb, LV_EVENT_HIT_TEST, NULL);
	lv_obj_set_size(cover_panel, cover_size, cover_size);
	lv_obj_align(cover_panel, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_set_style_bg_color(cover_panel, theme()->cover_bg, 0);
	lv_obj_set_style_border_width(cover_panel, 0, 0);
	lv_obj_set_style_radius(cover_panel, 0, 0);
	lv_obj_set_style_pad_all(cover_panel, 0, 0);
	lv_obj_set_scrollable(cover_panel, false);

	// The live badge, top right over the artwork: the one thing that has to be
	// legible at a glance is that this is not a file being played but a
	// broadcast going past. Red because that is what a live indicator is, and
	// deliberately not the accent colour -- it says something about the
	// stream, not about the theme.
	live_badge = lv_label_create(cover_panel);
	lv_label_set_text(live_badge, tr("player_live"));
	lv_obj_set_style_text_font(live_badge, &font_ui_18, 0);
	lv_obj_set_style_text_color(live_badge, lv_color_white(), 0);
	lv_obj_set_style_bg_color(live_badge, lv_color_make(224, 27, 36), 0); // Adwaita red
	lv_obj_set_style_bg_opa(live_badge, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(live_badge, ui_px(6), 0);
	lv_obj_set_style_pad_hor(live_badge, ui_px(10), 0);
	lv_obj_set_style_pad_ver(live_badge, ui_px(5), 0);
	lv_obj_align(live_badge, LV_ALIGN_TOP_RIGHT, -BADGE_INSET, badge_top());
	lv_obj_set_hidden(live_badge, true);

	// The Qobuz mark, in the same corner and for the same reason: say where
	// what is playing comes from. It is the white-outlined image, legible even
	// over a black cover, and is not recoloured (theme_style_icon would turn
	// it into a solid square).
	qobuz_badge = lv_image_create(cover_panel);
	lv_image_set_src(qobuz_badge, &icon_qobuz_badge);
	lv_obj_align(qobuz_badge, LV_ALIGN_TOP_RIGHT, -BADGE_INSET, badge_top());
	lv_obj_set_hidden(qobuz_badge, true);

	// Same corner, same size: the marks replace each other instead of sitting
	// side by side, and update_qobuz_badge() decides which.
	tidal_badge = lv_image_create(cover_panel);
	lv_image_set_src(tidal_badge, &icon_tidal_badge);
	lv_obj_align(tidal_badge, LV_ALIGN_TOP_RIGHT, -BADGE_INSET, badge_top());
	lv_obj_set_hidden(tidal_badge, true);

	// And the third, same corner and same size as the other two.
	podcast_badge = lv_image_create(cover_panel);
	lv_image_set_src(podcast_badge, &icon_podcast_badge);
	lv_obj_align(podcast_badge, LV_ALIGN_TOP_RIGHT, -BADGE_INSET, badge_top());
	lv_obj_set_hidden(podcast_badge, true);

	// Shown while there is no artwork for the current track.
	cover_placeholder_icon = lv_image_create(cover_panel);
	lv_image_set_src(cover_placeholder_icon, &icon_music_note);
	lv_obj_add_style(cover_placeholder_icon, &theme_style_text_dim, 0);
	lv_obj_set_style_image_recolor(cover_placeholder_icon, theme()->text_secondary, 0);
	lv_obj_set_style_image_recolor_opa(cover_placeholder_icon, LV_OPA_COVER, 0);
	lv_obj_center(cover_placeholder_icon);

	// Where the title and the star go in the alternative layout: two pills in
	// one bottom corner of the sleeve and a disc in the other. Built empty and
	// hidden; apply_layout() moves the real labels into them.
	//
	// Children of cover_panel so they sit on the sleeve and travel with it.
	// The column is what keeps the two pills stacked and both left-aligned
	// while each is only as wide as its own words.
	alt_text_col = lv_obj_create(cover_panel);
	lv_obj_remove_style_all(alt_text_col);
	lv_obj_set_size(alt_text_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_align(alt_text_col, LV_ALIGN_BOTTOM_LEFT, ALT_PAD, -ALT_PAD);
	lv_obj_set_flex_flow(alt_text_col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(alt_text_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(alt_text_col, ui_px(6), 0);
	lv_obj_set_scrollable(alt_text_col, false);
	lv_obj_set_hidden(alt_text_col, true);
	// None of this takes presses. A plain object is clickable the moment it is
	// made, and these lie on the artwork -- which is the surface the player is
	// dragged shut by. Left clickable they swallowed every gesture that began
	// on the title, so putting the player away by pushing it off the right of
	// the screen worked everywhere except on the words naming the track.
	lv_obj_set_clickable(alt_text_col, false);

	for (int i = 0; i < 2; i++) {
		lv_obj_t *pill = lv_obj_create(alt_text_col);
		lv_obj_remove_style_all(pill);
		lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
		lv_obj_set_style_pad_hor(pill, ALT_PILL_PAD_H, 0);
		lv_obj_set_style_pad_ver(pill, ALT_PILL_PAD_V, 0);
		lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		// No shadow. There was one, to keep a pill from disappearing into a
		// patch of sleeve the same colour as itself -- but that was when the
		// fill was the sleeve's colour at full strength. It is now a dark
		// surface of fixed weight, which separates from artwork on its own, and
		// the shadow under it only made the pill look pasted on.
		lv_obj_set_scrollable(pill, false);
		lv_obj_set_clickable(pill, false);
		if (i == 0) {
			alt_title_pill = pill;
		} else {
			alt_artist_pill = pill;
		}
	}

	alt_fav_circle = lv_obj_create(cover_panel);
	lv_obj_remove_style_all(alt_fav_circle);
	lv_obj_set_size(alt_fav_circle, ALT_FAV_SIZE, ALT_FAV_SIZE);
	lv_obj_set_style_radius(alt_fav_circle, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(alt_fav_circle, LV_OPA_COVER, 0);
	lv_obj_align(alt_fav_circle, LV_ALIGN_BOTTOM_RIGHT, -ALT_PAD, -ALT_PAD);
	lv_obj_set_scrollable(alt_fav_circle, false);
	// Likewise: the star's own button is a child of this and takes its own
	// presses, so the disc around it has no reason to take any.
	lv_obj_set_clickable(alt_fav_circle, false);
	lv_obj_set_hidden(alt_fav_circle, true);

	cover_img = lv_image_create(cover_panel);
	lv_obj_center(cover_img);
	lv_obj_set_hidden(cover_img, true);

	// ---------------------------------------------------------------------
	// Studio: the blurred sleeve behind the whole screen, and everything else
	// laid out over it.
	//
	// The background is a child of the page rather than of the artwork, so it
	// reaches under the controls as well; the panel that carries the rest is a
	// child of the artwork, so it travels with the sheet. Nothing on either
	// takes a press: the surface under them is what the player is pushed shut
	// by, and the buttons that do take presses are moved here from elsewhere.
	// ---------------------------------------------------------------------
	studio_bg = lv_image_create(player_screen);
	lv_obj_set_ignore_layout(studio_bg, true);
	lv_obj_set_size(studio_bg, cfg->screen_width, cfg->screen_height);
	lv_obj_set_pos(studio_bg, 0, 0);
	// COVER and not STRETCH: the blurred copy is asked for at the shape of the
	// screen, but between a change of arrangement and the picture that follows
	// it the one on hand is still the old shape, and stretching that is a
	// visibly squashed sleeve.
	lv_image_set_inner_align(studio_bg, LV_IMAGE_ALIGN_COVER);
	lv_obj_set_clickable(studio_bg, false);
	lv_obj_set_hidden(studio_bg, true);
	lv_obj_move_background(studio_bg);

	studio_box_w = cover_size;
	studio_box_h = (int)cfg->screen_height - STUDIO_CONTROLS_H - 2 * (menu_gap - PLAYER_MENU_GAP);
	if (studio_box_h < cover_size) {
		studio_box_h = cover_size;
	}
	studio_box = lv_obj_create(cover_panel);
	lv_obj_remove_style_all(studio_box);
	lv_obj_set_size(studio_box, studio_box_w, studio_box_h);
	lv_obj_set_pos(studio_box, 0, 0);
	lv_obj_set_scrollable(studio_box, false);
	lv_obj_set_clickable(studio_box, false);
	lv_obj_set_hidden(studio_box, true);

	studio_head = lv_obj_create(studio_box);
	lv_obj_remove_style_all(studio_head);
	lv_obj_set_size(studio_head, cover_size - 2 * STUDIO_MARGIN, STUDIO_HEAD_H);
	lv_obj_set_scrollable(studio_head, false);
	lv_obj_set_clickable(studio_head, false);

	// Centred in the head and not filling it: the ellipsis sits at the right
	// edge and the chevron at the left, and the two names have to stop before
	// they reach either -- on both sides, or a long title would be centred
	// against one of them.
	studio_text_col = lv_obj_create(studio_head);
	lv_obj_remove_style_all(studio_text_col);
	lv_obj_set_size(studio_text_col, cover_size - 2 * STUDIO_MARGIN - 2 * ui_px(68), LV_SIZE_CONTENT);
	lv_obj_align(studio_text_col, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_flex_flow(studio_text_col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(studio_text_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(studio_text_col, ui_px(4), 0);
	lv_obj_set_style_text_align(studio_text_col, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_scrollable(studio_text_col, false);
	lv_obj_set_clickable(studio_text_col, false);

	// The sleeve at whatever size studio_place() settles on. The picture is
	// decoded at the width of the screen for the standard arrangement, so it is
	// scaled down here rather than decoded twice.
	studio_cover = lv_image_create(studio_box);
	lv_obj_set_ignore_layout(studio_cover, true);
	lv_image_set_inner_align(studio_cover, LV_IMAGE_ALIGN_STRETCH);
	lv_obj_set_clickable(studio_cover, false);

	// What stands in the sleeve's place when the track has no artwork: the same
	// surface and the same mark the full-width cover panel uses, at the size
	// this arrangement gives the sleeve.
	studio_empty = lv_obj_create(studio_box);
	lv_obj_set_ignore_layout(studio_empty, true);
	lv_obj_add_style(studio_empty, &theme_style_panel, 0);
	lv_obj_set_style_border_width(studio_empty, 0, 0);
	lv_obj_set_style_radius(studio_empty, ui_px(8), 0);
	lv_obj_set_scrollable(studio_empty, false);
	lv_obj_set_clickable(studio_empty, false);
	lv_obj_set_hidden(studio_empty, true);

	studio_empty_icon = lv_image_create(studio_empty);
	lv_obj_set_style_image_recolor_opa(studio_empty_icon, LV_OPA_COVER, 0);
	lv_obj_center(studio_empty_icon);
	lv_obj_set_clickable(studio_empty_icon, false);

	studio_quality = lv_obj_create(studio_box);
	lv_obj_remove_style_all(studio_quality);
	lv_obj_set_flex_flow(studio_quality, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(studio_quality, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(studio_quality, ui_px(8), 0);
	lv_obj_set_scrollable(studio_quality, false);
	lv_obj_set_clickable(studio_quality, false);

	// No theme_style_icon here: the four quality marks carry their own colours,
	// which is how the track lists draw them, and a recolour to the text colour
	// flattens all four into the same grey glyph.
	studio_quality_icon = lv_image_create(studio_quality);

	lyrics_build();

	// Last, so it is above the artwork: an invisible strip exactly where the
	// status bar would be, owning the pull-down that opens the control centre.
	lv_obj_t *panel_edge = lv_obj_create(player_screen);
	lv_obj_remove_style_all(panel_edge);
	lv_obj_set_size(panel_edge, cfg->screen_width, cfg->top_bar_height);
	lv_obj_set_pos(panel_edge, 0, 0);
	lv_obj_set_scrollable(panel_edge, false);
	lv_obj_set_clickable(panel_edge, true);
	lv_obj_add_event_cb(panel_edge, panel_edge_drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(panel_edge, panel_edge_drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(panel_edge, panel_edge_drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(panel_edge, panel_edge_drag_cb, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_move_foreground(panel_edge);

	// The backdrop over a loaded cover is always the dark treatment -- same
	// look in both themes; only the no-cover panel follows the theme.
	cover_set_backdrop_light(false);

	// The status bar over the player, as it was left.
	bar_height = (int)cfg->top_bar_height;
	player_set_statusbar_always(config_get_int("screen", "statusbar_always", 0) != 0);

	// Whichever arrangement was left selected, now that every widget it moves
	// exists.
	int saved = (int)config_get_int("screen", "player_layout_alt", 0);
	layout_choice = (saved == (int)PLAYER_LAYOUT_ALTERNATIVE)	? PLAYER_LAYOUT_ALTERNATIVE
					: (saved == (int)PLAYER_LAYOUT_STUDIO)		? PLAYER_LAYOUT_STUDIO
																: PLAYER_LAYOUT_STANDARD;
	cover_accent = config_get_bool("screen", "cover_accent", false);
	layout_alt_now = false; // apply_layout() below puts the standard one up first
	layout_studio_now = false;
	apply_layout();

	meterview_create(cover_panel, meter_where);

	theme_register_refresh(player_refresh_theme);
}


