#include "quickpanel.h"

#include <stdio.h>
#include <string.h>

#include "src/gui/wireless/airplay.h"
#include "src/gui/bluetooth/btsettings.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/settings/musicsettings.h"
#include "src/gui/audio/peqpage.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/scrolltext.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/gui/wireless/dlna.h"
#include "src/gui/wireless/sonixlink.h"
#include "src/gui/wireless/wifitransfer.h"
#include "src/gui/shell/theme.h"
#include "src/system/playback/audiobook.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcastsubs.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/usbdac.h"
#include "src/system/net/wifitransfer.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/remote/airplay.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/core/config.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/audio/eq.h"
#include "src/system/playback/playlist.h"
#include "src/system/playback/sleeptimer.h"
#include "src/system/device/power.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/qobuzsync.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/streaming/tidalsync.h"
#include "src/system/streaming/radio.h"
#include "src/system/remote/dlna.h"
#include "src/system/remote/sonixlink.h"
#include "src/system/net/wifi.h"
#include "src/gui/shell/uiscale.h"

#define PANEL_ANIM_MS 180
#define PANEL_POLL_MS 700

// Four round quick controls to a line. Four at 88 px plus their gaps is 394,
// against the roughly 400 px the card leaves inside its padding, so a fifth
// does not fit however tight the gap gets: the extra controls wrap onto a
// second line rather than shrinking.
#define CIRCLE_BUTTON_SIZE ui_px(88)
#define CIRCLE_BUTTON_GAP ui_px(14)
#define QP_BUTTONS_PER_LINE 4

// How the sheet divides the screen up: a strip for the close hint at the
// bottom, small gaps at the top and between the cards, and the rest split
// evenly between them. The leftover space goes to the cards rather than around
// them, or on a 720 px panel everything sits high.
#define CARD_PADDING ui_px(20)
#define HINT_STRIP_H ui_px(52) // room under the cards for the close line
#define CARD_TOP_GAP ui_px(12) // between the status bar and the first card
#define CARD_GAP ui_px(16)	   // between the two cards
#define CARD_ROW_GAP ui_px(12) // between the blocks inside the controls card
#define HANDLE_ROOM ui_px(24)	// added under the slider when the card has a handle
#define BRIGHT_ROW_H ui_px(44)
// The dimmest backlight level on the slider: 0 would put the backlight out.
#define BRIGHTNESS_FLOOR 1

// The sheet's own padding. The cards are aligned inside the content area, so
// every y below is measured from there and not from the top of the screen;
// measuring from the screen puts the close line over the second card.
#define PANEL_PAD_TOP(cfg) ((cfg)->padding + ui_px(8))
#define PANEL_PAD_BOTTOM ui_px(8)

static lv_obj_t *veil;	// dims the page still showing while the sheet is part-way in
static lv_obj_t *panel; // the full-screen sheet, parked above the top edge
static lv_obj_t *brightness_slider;
static lv_obj_t *mseb_btn, *eq_btn;
static lv_obj_t *wifi_btn, *bt_btn;
static lv_obj_t *fade_btn, *gain_btn;
static lv_obj_t *airplay_btn;
static lv_obj_t *lineout_btn;
static lv_obj_t *sonixlink_btn;
static lv_obj_t *peq_btn;
static lv_obj_t *dlna_btn;
static lv_obj_t *sleep_music_btn;
static lv_obj_t *sleep_audiobook_btn;
static lv_obj_t *sleep_podcast_btn;
static lv_obj_t *wifi_transfer_btn;
static lv_obj_t *gapless_btn;
static lv_obj_t *volume_lock_btn;
static lv_obj_t *direct_btn;

// ---------------------------------------------------------------------------
// Which buttons the panel carries, and where
//
// Kept as names rather than as indices so that adding a button later joins a
// saved layout instead of shifting everything after it. The grid and the list
// of the ones left out together account for every button exactly once,
// whatever the config file says.
// ---------------------------------------------------------------------------

// Two tables and not one struct of pairs, because the two columns are read by
// different things and only one of them is text. The keys are what the config
// file carries and must never be retranslated or renamed; the tags are what the
// settings page puts on screen, and they reach tr() through
// quickpanel_button_tag(), a hop the string extractor cannot follow -- so
// button_tag is named in tools/extract_strings.py's TABLES, and the keys stay
// out of the language files where they never belonged.
static const char *const button_key[QP_BTN_COUNT] = {
	[QP_BTN_WIFI] = "wifi",
	[QP_BTN_BLUETOOTH] = "bluetooth",
	[QP_BTN_AIRPLAY] = "airplay",
	[QP_BTN_MSEB] = "mseb",
	[QP_BTN_EQ] = "eq",
	[QP_BTN_FADE] = "fade",
	[QP_BTN_GAIN] = "gain",
	[QP_BTN_LINEOUT] = "lineout",
	[QP_BTN_SONIXLINK] = "sonixlink",
	[QP_BTN_PEQ] = "peq",
	[QP_BTN_DLNA] = "dlna",
	[QP_BTN_SLEEP_MUSIC] = "sleep_music",
	[QP_BTN_SLEEP_AUDIOBOOK] = "sleep_audiobook",
	[QP_BTN_SLEEP_PODCAST] = "sleep_podcast",
	[QP_BTN_WIFI_TRANSFER] = "wifi_transfer",
	[QP_BTN_GAPLESS] = "gapless",
	[QP_BTN_VOLUME_LOCK] = "volume_lock",
	[QP_BTN_DIRECT] = "direct",
};

static const char *const button_tag[QP_BTN_COUNT] = {
	[QP_BTN_WIFI] = "wi_fi",
	[QP_BTN_BLUETOOTH] = "bluetooth",
	[QP_BTN_AIRPLAY] = "airplay",
	[QP_BTN_MSEB] = "mseb",
	[QP_BTN_EQ] = "equaliser",
	[QP_BTN_FADE] = "fade",
	[QP_BTN_GAIN] = "gain",
	[QP_BTN_LINEOUT] = "quickpanel_line_out",
	[QP_BTN_SONIXLINK] = "sonixlink",
	[QP_BTN_PEQ] = "peq",
	[QP_BTN_DLNA] = "dlna",
	[QP_BTN_SLEEP_MUSIC] = "quickpanel_sleep_music",
	[QP_BTN_SLEEP_AUDIOBOOK] = "quickpanel_sleep_audiobook",
	[QP_BTN_SLEEP_PODCAST] = "quickpanel_sleep_podcast",
	[QP_BTN_WIFI_TRANSFER] = "wifitransfer_title",
	[QP_BTN_GAPLESS] = "musicsettings_gapless_playback",
	[QP_BTN_VOLUME_LOCK] = "quickpanel_volume_lock",
	[QP_BTN_DIRECT] = "musicsettings_direct",
};

static uint8_t slots[QP_SLOT_COUNT];  // the grid, QP_BTN_NONE where it is empty
static uint8_t hidden[QP_BTN_COUNT]; // the buttons that are not in the grid
static uint8_t hidden_n;
static bool order_loaded;

// Set while a long press opens a page, so the click that ends the same press
// does not also toggle the effect.
static bool long_press_consumed;
static bool panel_open;
static int panel_h;

// Interactive drag state: where the sheet was when the finger went down.
static bool drag_active;
static int drag_from_y;
static uint32_t drag_begin_ms;

// A gesture this fast decides by its direction rather than by how far it got.
//
// A flick is short: the finger goes down, snaps a hundred pixels and is gone,
// and that never reaches the quarter of a screen a deliberate drag is measured
// against -- so the sheet followed the finger and then snapped back, which from
// the outside is a control centre that ignores a quick swipe. The floor keeps
// the wobble of a tap out of it.
#define FLICK_MIN_PX ui_px(40)
#define FLICK_SPEED_PX_S ui_px(500)

// How far a drag has to travel before the release opens or closes rather than
// snapping back. Fixed pixels and not a share of the panel: both players have
// the same 480-wide glass, so the same gesture decides the same way on the
// 720-tall R3 Pro II and the 800-tall R1. 180 is a quarter of 720.
#define DRAG_COMMIT_PX ui_px(180)

// ---------------------------------------------------------------------------
// The stretch of the controls card
//
// The card shows the first QP_SLOT_VISIBLE buttons. When the panel holds more,
// the card is a little taller, a handle sits under the brightness slider, and
// a drag down on the sheet stretches the card downwards, over the now-playing
// card: the slider moves down with the edge and the buttons past the eighth
// open up between the first two lines and the slider, as further lines of the
// same grid. A drag up folds it back before a second one carries the sheet
// away. The now-playing card fades as the controls card covers it, and is
// taken out altogether once it is covered, so a tap under the stretched card
// cannot reach its transport.
// ---------------------------------------------------------------------------

static lv_obj_t *controls_card;
static lv_obj_t *np_card;
static lv_obj_t *quick_row;	   // the first QP_SLOT_VISIBLE buttons
static lv_obj_t *extra_clip;	   // the window the stretch opens, under the first lines
static lv_obj_t *extra_row;	   // the rest, inside that window
static lv_obj_t *bright_row;	   // moved down by the stretch
static lv_obj_t *expand_handle; // the line under the slider, and a tap target
static lv_obj_t *expand_pill;
static int card_h_rest;		   // both cards' height with nothing past the eighth
static int card_top;		   // the controls card's top, inside the sheet
static int card_h_base;		   // the controls card's height at rest, handle room included
static int expand_full;		   // how far the card stretches; 0 when nothing is past the eighth
static int expand_h;		   // how far it is stretched now
static int expand_from;		   // where a drag found it
static uint32_t expand_begin_ms;

// Set once a press has moved the sheet or the card, so the click that ends the
// same press does not also fold the card from its handle.
static bool press_dragged;

// The now-playing card's widgets.
static lv_obj_t *np_title;
static lv_obj_t *np_artist;
static lv_obj_t *np_play_btn;
static lv_obj_t *np_play_icon;
static lv_obj_t *np_repeat_icon;

// Transport widgets. Prev, next and repeat are hidden on a live stream, as the
// player hides them: they would not do nothing, they would quietly swap the
// station for a track out of the queue. Prev and next stay for a station from
// radio.txt, where they move along that list.
static lv_obj_t *np_prev_btn;
static lv_obj_t *np_next_btn;
static lv_obj_t *np_prev_icon;
static lv_obj_t *np_next_icon;
static lv_obj_t *np_star_btn;
static lv_obj_t *np_repeat_btn;
static lv_obj_t *np_star_icon;
static lv_timer_t *poll_timer;

bool quickpanel_is_open(void) { return panel_open; }

// ---------------------------------------------------------------------------
// now-playing state
// ---------------------------------------------------------------------------

static void refresh_repeat_icon(void) {
	lv_color_t active = theme()->accent;
	lv_color_t inactive = lv_color_make(128, 128, 128);
	const lv_image_dsc_t *glyph;
	lv_color_t color;

	switch (playlist_get_mode()) {
	case PLAYBACK_MODE_REPEAT_ONE:
		glyph = &icon_repeat_one;
		color = active;
		break;
	case PLAYBACK_MODE_REPEAT_ALL:
		glyph = &icon_repeat_all;
		color = active;
		break;
	case PLAYBACK_MODE_SHUFFLE:
		glyph = &icon_shuffle;
		color = active;
		break;
	case PLAYBACK_MODE_SHUFFLE_REPEAT:
		glyph = &icon_shuffle_repeat;
		color = active;
		break;
	default:
		glyph = &icon_repeat_off;
		color = inactive;
		break;
	}
	lv_image_set_src(np_repeat_icon, glyph);
	lv_obj_set_style_image_recolor(np_repeat_icon, color, 0);
	lv_obj_set_style_image_recolor_opa(np_repeat_icon, LV_OPA_COVER, 0);
}

static void refresh_now_playing_card(void) {
	device_state_t state;
	device_state_get(&state);

	if (state.live) {
		// A radio: the station on the first line, what it reports on air on the
		// second. A live stream has no file, so without this branch the card
		// shows the no-track text while a station is playing.
		scrolltext_set(np_title, state.metadata.title);
		scrolltext_set(np_artist, state.metadata.artist);
	} else if (state.current_file[0]) {
		const char *slash = strrchr(state.current_file, '/');
		scrolltext_set(np_title, state.metadata.title[0] ? state.metadata.title
														 : (slash ? slash + 1 : state.current_file));
		// The same name the player line and the screensaver show.
		const char *artist = metadata_shown_artist(&state.metadata);
		scrolltext_set(np_artist, artist);
	} else {
		scrolltext_set(np_title, tr("quickpanel_no_track"));
		scrolltext_set(np_artist, "");
	}

	// A live stream cannot be paused, so on a radio the button becomes the stop
	// the player also shows.
	const lv_image_dsc_t *glyph = state.status == AUDIO_STATUS_PLAYING ? &icon_pause_large : &icon_play_large;
	if (state.live && state.status == AUDIO_STATUS_PLAYING) {
		glyph = &icon_stop;
	}
	lv_image_set_src(np_play_icon, glyph);

	bool station_steps = state.live && radio_can_step();

	// Greyed out and inert while a station connects, as on the player.
	bool connecting = state.live && radio_is_connecting();
	lv_obj_t *const transport[] = {np_prev_btn, np_play_btn, np_next_btn};
	for (size_t i = 0; i < sizeof(transport) / sizeof(transport[0]); i++) {
		if (!transport[i]) {
			continue;
		}
		if (connecting) {
			lv_obj_add_state(transport[i], LV_STATE_DISABLED);
		} else {
			lv_obj_remove_state(transport[i], LV_STATE_DISABLED);
		}
	}
	lv_obj_t *const only_for_tracks[] = {np_prev_btn, np_next_btn, np_repeat_btn};
	for (size_t i = 0; i < sizeof(only_for_tracks) / sizeof(only_for_tracks[0]); i++) {
		if (!only_for_tracks[i]) {
			continue;
		}
		bool steps = station_steps && only_for_tracks[i] != np_repeat_btn;
		if (state.live && !steps) {
			lv_obj_set_hidden(only_for_tracks[i], true);
		} else {
			lv_obj_set_hidden(only_for_tracks[i], false);
		}
	}

	// Must come after the loop above: that loop unhides the repeat button for
	// everything that is not a stream, books included, so hiding it earlier
	// would be undone in the same pass.
	//
	// On a book the transport is a pair of timed jumps, mirroring the player so
	// the two places do not disagree about what the same buttons do. prev_cb and
	// next_cb already route through the player, which knows about books, so only
	// the glyphs change here.
	bool book = !state.live && audiobook_is_playing();
	// A podcast is driven like an audiobook -- timed jumps instead of prev/next
	// track -- but with its own intervals (podcast_skip_*), no repeat mode, and a
	// star that follows the feed rather than the track. Same dressing as the
	// player.
	long long podcast_feed_id = 0;
	player_current_podcast_feed(&podcast_feed_id, NULL);
	bool podcast = !state.live && !book && podcastcache_is_episode(state.current_file);
	bool skips = book || podcast;
	int back = book ? audiobook_skip_back() : podcast ? podcast_skip_back() : 0;
	int forward = book ? audiobook_skip_forward() : podcast ? podcast_skip_forward() : 0;
	if (np_prev_icon) {
		lv_image_set_src(np_prev_icon, !skips                       ? &icon_skip_back_large
									   : back == AUDIOBOOK_SKIP_HUGE ? &icon_prev_60
									   : back == AUDIOBOOK_SKIP_LONG ? &icon_prev_30
																	 : &icon_prev_10);
	}
	if (np_next_icon) {
		lv_image_set_src(np_next_icon, !skips                          ? &icon_skip_forward_large
									   : forward == AUDIOBOOK_SKIP_HUGE ? &icon_next_60
									   : forward == AUDIOBOOK_SKIP_LONG ? &icon_next_30
																		: &icon_next_10);
	}

	// Repeat/shuffle goes away on both books and podcasts: a queue of episodes is
	// not an album. The star goes away only on books; on a podcast it stays and
	// follows the feed (see star_cb).
	if (np_repeat_btn) {
		if (book || podcast) {
			lv_obj_set_hidden(np_repeat_btn, true);
		} else {
			lv_obj_set_hidden(np_repeat_btn, false);
		}
	}
	if (np_star_btn) {
		if (book) {
			lv_obj_set_hidden(np_star_btn, true);
		} else {
			lv_obj_set_hidden(np_star_btn, false);
		}
	}

	// Accent glyph on a disc that is white under the dark theme and the quiet
	// dark circle under the light one, where white would vanish into the card.
	lv_obj_set_style_image_recolor(np_play_icon, theme()->accent, 0);
	lv_obj_set_style_image_recolor_opa(np_play_icon, LV_OPA_COVER, 0);
	if (np_play_btn) {
		lv_obj_set_style_bg_color(np_play_btn, theme()->dark ? lv_color_white() : theme()->surface_pressed, 0);
	}

	// On a radio the star is about the station and lives in the radio database:
	// there is no file to look up in the music library.
	radio_station_t station;
	bool fav;
	long qobuz_id = 0;
	long tidal_id = 0;
	if (radio_current_station(&station)) {
		fav = radio_fav_contains(station.uuid);
	} else if (podcast) {
		// On a podcast the star reports whether the feed is followed, exactly as
		// the player's star does.
		fav = podcastsubs_is_followed(podcast_feed_id);
	} else if (state.current_file[0] && (tidal_id = tidalcache_track_id(state.current_file)) > 0) {
		// Same reasoning as the Qobuz case below: the star must report what is on
		// the account, not what is in the local database.
		//
		// The local mirror is also refreshed, because it may not be populated
		// yet: the control centre can be opened on a Tidal track without the
		// player ever having been visited, and an empty star on a track that is
		// a favourite turns the next tap into an "add" for something already
		// added.
		fav = tidalsync_is_favorite(tidal_id);
		tidalsync_favorites_refresh(NULL);
	} else if (state.current_file[0] && (qobuz_id = qobuzcache_track_id(state.current_file)) > 0) {
		// A Qobuz track: the star reports the account state, like the player's.
		// Reading the local database here would draw a star that disagrees with
		// the one a tap fills.
		fav = qobuzsync_is_favorite(qobuz_id);
	} else {
		fav = state.current_file[0] && library_fav_contains(state.current_file);
	}
	lv_image_set_src(np_star_icon, fav ? &icon_star_filled : &icon_star);
	lv_obj_set_style_image_recolor(np_star_icon, fav ? lv_color_make(246, 211, 45) : lv_color_make(150, 150, 150), 0);
	lv_obj_set_style_image_recolor_opa(np_star_icon, LV_OPA_COVER, 0);

	refresh_repeat_icon();
}

static void refresh_audio_buttons(void);

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	refresh_now_playing_card();
	// The circles can be turned off by something other than a tap while the
	// panel is open -- line out ends when its jack is pulled or when Bluetooth
	// takes the sound -- so the row is repainted with the card.
	refresh_audio_buttons();
}

// ---------------------------------------------------------------------------
// open / close
// ---------------------------------------------------------------------------

static void anim_y_cb(void *obj, int32_t v);

// Defined further down with the circle buttons; panel_prepare() needs it here.
static void refresh_audio_buttons(void);

// The full-screen object behind the sheet has no background: it exists only to
// collect the tap that closes the sheet, and LVGL does not draw a background it
// cannot see.
//
// It must stay that way. A translucent veil over the whole 480x720 screen would
// have to be blended every frame of the slide, with everything under it redrawn
// first to blend against -- the whole page plus a full-screen alpha pass, at
// every step, on a CPU that is also feeding the DAC.
static void anim_y_cb(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }

// True when the sheet raised the status bar itself and so has to put it away
// again. That happens exactly when the control centre is opened from the
// player, the one page that hides the bar.
static bool raised_topbar;

// Restores whatever the page underneath had. Called when the sheet has finished
// sliding away, not when it starts: the bar has to stay up for the whole slide,
// or it flickers out from under a sheet still on screen.
//
// The bar goes back down only if the player is still the thing underneath. A
// long press on one of the icons closes the player and loads a page while this
// slide is still running, and a page keeps its status bar: without the check
// the bar would be hidden on that page a fifth of a second after it appeared,
// taking the pull-down handle with it.
static void restore_topbar(void) {
	if (raised_topbar) {
		raised_topbar = false;
		if (player_sheet_is_open()) {
			topbar_set_hidden(true);
		}
	}
	// And the colours of a page painted in its own (an album's page), which the
	// bar gave up while the sheet was over it.
	topbar_tint_hold(false);
}

static void hide_when_parked_cb(lv_anim_t *a) {
	(void)a;
	if (!panel_open) {
		lv_obj_set_hidden(veil, true);
		player_lyrics_covered(PLAYER_COVER_QUICKPANEL, false);
		restore_topbar();
	}
}

static void panel_slide_to(int y, bool animate) {
	lv_anim_delete(panel, anim_y_cb); // never race a previous slide
	if (!animate) {
		lv_obj_set_y(panel, y);
		if (!panel_open) {
			lv_obj_set_hidden(veil, true);
			player_lyrics_covered(PLAYER_COVER_QUICKPANEL, false);
			restore_topbar();
		}
		return;
	}
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, panel);
	lv_anim_set_exec_cb(&a, anim_y_cb);
	lv_anim_set_values(&a, lv_obj_get_y(panel), y);
	lv_anim_set_duration(&a, PANEL_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, hide_when_parked_cb);
	lv_anim_start(&a);
}

// ---------------------------------------------------------------------------
// Stretching the controls card (see the note over controls_card)
// ---------------------------------------------------------------------------

static void expand_set(int h) {
	if (!controls_card) {
		return;
	}
	if (h > expand_full) {
		h = expand_full;
	}
	if (h < 0) {
		h = 0;
	}
	expand_h = h;
	lv_obj_set_height(controls_card, card_h_base + h);
	lv_obj_set_height(extra_clip, h);
	lv_obj_set_style_translate_y(bright_row, h, 0);

	// In proportion to the stretch: the second block comes in as the card
	// uncovers it, and the card underneath goes out as it is covered.
	lv_opa_t shown = expand_full > 0 ? (lv_opa_t)(LV_OPA_COVER * h / expand_full) : LV_OPA_TRANSP;
	lv_obj_set_hidden(extra_clip, h == 0);
	lv_obj_set_style_opa(extra_row, shown, 0);
	lv_obj_set_hidden(np_card, expand_full > 0 && h >= expand_full);
	lv_obj_set_style_opa(np_card, (lv_opa_t)(LV_OPA_COVER - shown), 0);
}

static void expand_anim_cb(void *obj, int32_t v) {
	(void)obj;
	expand_set((int)v);
}

static void expand_to(int h, bool animate) {
	if (!controls_card) {
		return;
	}
	lv_anim_delete(controls_card, expand_anim_cb);
	if (!animate) {
		expand_set(h);
		return;
	}
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, controls_card);
	lv_anim_set_exec_cb(&a, expand_anim_cb);
	lv_anim_set_values(&a, expand_h, h);
	lv_anim_set_duration(&a, PANEL_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

static void expand_drag_begin(void) {
	lv_anim_delete(controls_card, expand_anim_cb);
	expand_from = expand_h;
	expand_begin_ms = lv_tick_get();
}

static void expand_drag_update(int dy) { expand_set(expand_from + dy); }

// Past halfway it stays stretched, short of it it folds back; a flick goes the
// way it was flicked, as the sheet's own drag does.
static void expand_drag_end(void) {
	int travel = expand_h - expand_from;
	bool open = expand_h * 2 > expand_full;
	uint32_t elapsed = lv_tick_elaps(expand_begin_ms);
	int distance = travel > 0 ? travel : -travel;
	if (elapsed > 0 && distance >= FLICK_MIN_PX && (distance * 1000) / (int)elapsed >= FLICK_SPEED_PX_S) {
		open = travel > 0;
	}
	expand_to(open ? expand_full : 0, true);
}

static void expand_handle_clicked_cb(lv_event_t *e) {
	(void)e;
	if (press_dragged) {
		return;
	}
	expand_to(expand_h > 0 ? 0 : expand_full, true);
}

// Everything the sheet has to freshen before it becomes visible, whether it is
// being animated in or dragged in a pixel at a time.
static void panel_prepare(void) {
	long max = power_get_max_brightness();
	if (max <= 0) {
		max = 100;
	}
	lv_slider_set_range(brightness_slider, BRIGHTNESS_FLOOR, (int32_t)max);
	long value = power_get_brightness();
	if (value < BRIGHTNESS_FLOOR) {
		value = max;
	}
	lv_slider_set_value(brightness_slider, (int32_t)value, LV_ANIM_OFF);
	lv_obj_set_style_bg_color(brightness_slider, theme()->accent, LV_PART_INDICATOR);
	// The sheet is built once at startup, so the knob outline has to be reapplied
	// for whichever theme is live now; otherwise it keeps the ring (or lack of
	// one) it was created with.
	theme_apply_slider_knob(brightness_slider);

	refresh_now_playing_card();
	refresh_audio_buttons();

	// The sheet always comes in folded, as Android's does.
	expand_to(0, false);
	if (expand_pill) {
		lv_obj_set_style_bg_color(expand_pill, theme()->text_primary, 0);
	}

	lv_obj_set_hidden(veil, false);
	player_lyrics_covered(PLAYER_COVER_QUICKPANEL, true);
	lv_obj_move_foreground(veil);

	// The player hides the status bar so artwork can run to the top edge. The
	// control centre needs it back: the clock, battery and volume belong with
	// these controls, and the sheet covers the artwork anyway while it is up.
	if (topbar_is_hidden()) {
		raised_topbar = true;
		topbar_set_hidden(false);
	}
	// In the theme's colours, like the sheet it now belongs to.
	topbar_tint_hold(true);

	// The bar is also restacked above the sheet, so clock, battery and volume
	// stay readable with the control centre open.
	topbar_bring_to_front();
}

// While the device is acting as a USB sound card, a Wi-Fi file server or a
// Bluetooth receiver it is not playing anything, and those pages are meant to
// stay in front, so the control centre would show transport buttons for a
// transport that is not running. It refuses to open instead.
bool quickpanel_blocked(void) {
	// The transfer answers with the switch rather than with the server: killing
	// thttpd takes a moment and the answer about the process is cached on top of
	// that, so asking the process left the control centre refusing to open for
	// seconds after the page said the transfer was off.
	return usbdac_is_active() || wifitransfer_get_enabled() || btreceiver_is_active();
}

void quickpanel_open(void) {
	if (!panel || panel_open || quickpanel_blocked()) {
		return;
	}
	panel_open = true;
	panel_prepare();
	lv_timer_resume(poll_timer);
	panel_slide_to(0, true);
}

void quickpanel_close(void) {
	if (!panel || !panel_open) {
		return;
	}
	panel_open = false;
	lv_timer_pause(poll_timer);
	panel_slide_to(-panel_h, true);
}

// ---------------------------------------------------------------------------
// Following the finger: the gesture moves the sheet directly, and only the last
// stretch after the release is animated.
// ---------------------------------------------------------------------------

void quickpanel_drag_begin(void) {
	if (!panel || drag_active) {
		return;
	}
	lv_anim_delete(panel, anim_y_cb);
	drag_active = true;
	drag_from_y = panel_open ? 0 : -panel_h;
	drag_begin_ms = lv_tick_get();

	if (!panel_open) {
		panel_prepare();
		lv_obj_set_y(panel, -panel_h);
		lv_timer_resume(poll_timer); // the card is visible from the first pixel of travel
	}
}

void quickpanel_drag_update(int dy) {
	if (!drag_active) {
		return;
	}
	int y = drag_from_y + dy;
	if (y > 0) {
		y = 0;
	}
	if (y < -panel_h) {
		y = -panel_h;
	}
	lv_obj_set_y(panel, y);
}

void quickpanel_drag_end(void) {
	if (!drag_active) {
		return;
	}
	drag_active = false;

	// A fixed stretch of travel decides the outcome, in whichever direction the
	// gesture started: the same point-of-no-return feel as the swipe-back.
	int y = lv_obj_get_y(panel);
	int travel = y - drag_from_y;
	bool open = drag_from_y == 0 ? travel > -DRAG_COMMIT_PX : travel > DRAG_COMMIT_PX;

	// Unless it was a flick, which is over long before it has covered that much.
	// Then only the direction counts.
	uint32_t elapsed = lv_tick_elaps(drag_begin_ms);
	int distance = travel > 0 ? travel : -travel;
	if (elapsed > 0 && distance >= FLICK_MIN_PX && (distance * 1000) / (int)elapsed >= FLICK_SPEED_PX_S) {
		open = travel > 0;
	}

	panel_open = open;
	if (open) {
		lv_timer_resume(poll_timer);
	} else {
		lv_timer_pause(poll_timer);
	}
	panel_slide_to(open ? 0 : -panel_h, true);
}

static void veil_clicked_cb(lv_event_t *e) {
	(void)e;
	quickpanel_close();
}

// The upward half of the gesture: a drag anywhere on the sheet carries it back
// out, following the finger all the way. With buttons past the eighth, a drag
// down stretches the controls card instead, and a drag up on a stretched card
// folds it before anything carries the sheet away.
static void panel_drag_cb(lv_event_t *e) {
	static lv_point_t start;
	static bool tracking;
	static enum { DRAG_NONE, DRAG_SHEET, DRAG_CARD } engaged;

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}
	lv_event_code_t code = lv_event_get_code(e);

	if (quickpanel_blocked()) {
		return;
	}

	if (code == LV_EVENT_PRESSED) {
		lv_indev_get_point(indev, &start);
		tracking = panel_open;
		engaged = DRAG_NONE;
		press_dragged = false;
		return;
	}
	if (!tracking) {
		return;
	}

	lv_point_t p;
	lv_indev_get_point(indev, &p);
	int dy = p.y - start.y;

	if (code == LV_EVENT_PRESSING) {
		if (engaged == DRAG_NONE) {
			if (dy <= -8) {
				engaged = expand_h > 0 ? DRAG_CARD : DRAG_SHEET;
			} else if (dy >= 8 && expand_full > 0 && expand_h < expand_full) {
				engaged = DRAG_CARD;
			} else {
				return; // not a drag yet, or a drag down with nothing to uncover
			}
			press_dragged = true;
			if (engaged == DRAG_CARD) {
				expand_drag_begin();
			} else {
				quickpanel_drag_begin();
			}
		}
		if (engaged == DRAG_CARD) {
			expand_drag_update(dy);
		} else {
			quickpanel_drag_update(dy);
		}
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		tracking = false;
		if (engaged == DRAG_CARD) {
			expand_drag_end();
		} else if (engaged == DRAG_SHEET) {
			quickpanel_drag_end();
		}
		engaged = DRAG_NONE;
	}
}

// ---------------------------------------------------------------------------
// controls
// ---------------------------------------------------------------------------

static void brightness_changed_cb(lv_event_t *e) {
	(void)e;
	power_set_brightness(lv_slider_get_value(brightness_slider));
}

static void brightness_released_cb(lv_event_t *e) {
	(void)e;
	config_set_int("screen", "brightness", (int)lv_slider_get_value(brightness_slider));
	config_save();
}

static void play_cb(lv_event_t *e) {
	(void)e;
	player_key_play_pause();
	refresh_now_playing_card();
}

// The control centre is an on-screen control, so its prev/next do what the
// player's on-screen buttons do: on a podcast they seek by seconds rather than
// changing episode, which stays with the physical side keys.
static void next_cb(lv_event_t *e) {
	(void)e;
	player_screen_next();
	refresh_now_playing_card();
}

static void prev_cb(lv_event_t *e) {
	(void)e;
	player_screen_prev();
	refresh_now_playing_card();
}

static void repeat_cb(lv_event_t *e) {
	(void)e;
	playlist_cycle_mode();
	refresh_repeat_icon();
}

// Result of a Qobuz star, delivered by the qobuzsync worker. Same contract as
// the player's star: success needs no message, since the filled star already
// says it, but failure does, because the star filled on touch and now empties
// again. Reported on the GUI thread via gui_post, never from the worker.
static char qp_fav_error[192];

static void qp_fav_error_async(void *user) {
	(void)user;
	if (qp_fav_error[0]) {
		gui_notify_popup(qp_fav_error);
	}
	refresh_now_playing_card();
	player_refresh_now_playing();
}

static void qp_fav_done(bool ok, const char *error, void *user) {
	(void)user;
	snprintf(qp_fav_error, sizeof(qp_fav_error), "%s",
			 ok ? "" : (error && error[0] ? error : tr("qobuz_favourites_failed")));
	gui_post(qp_fav_error_async, NULL);
}

static void qp_tidal_fav_done(bool ok, const char *error, void *user) {
	(void)user;
	snprintf(qp_fav_error, sizeof(qp_fav_error), "%s",
			 ok ? "" : (error && error[0] ? error : tr("tidal_favourites_failed")));
	gui_post(qp_fav_error_async, NULL);
}

static void star_cb(lv_event_t *e) {
	(void)e;

	radio_station_t station;
	if (radio_current_station(&station)) {
		if (radio_fav_contains(station.uuid)) {
			radio_fav_remove(station.uuid);
		} else {
			radio_fav_add(&station);
		}
		refresh_now_playing_card();
		player_refresh_now_playing();
		return;
	}

	// A podcast: the star follows or unfollows the feed, like the player's, and
	// reads the same source so the two stars cannot disagree.
	long long feed_id = 0;
	podcast_feed_t feed;
	if (player_current_podcast_feed(&feed_id, &feed)) {
		if (podcastsubs_is_followed(feed_id)) {
			podcastsubs_unfollow(feed_id);
		} else {
			podcastsubs_follow(&feed);
		}
		refresh_now_playing_card();
		player_refresh_now_playing();
		return;
	}

	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0]) {
		return;
	}

	// A Qobuz track: the star goes to the account favourites, like the player's,
	// and not to the local database, which would store the path of a cache file
	// that is gone twenty tracks later.
	long qobuz_id = qobuzcache_track_id(state.current_file);
	if (qobuz_id > 0) {
		bool want = !qobuzsync_is_favorite(qobuz_id);
		qobuzsync_favorite_toggle(qobuz_id, want, qp_fav_done, NULL);
		refresh_now_playing_card(); // the local mirror already flipped, so redraw at once
		player_refresh_now_playing();
		return;
	}

	long tidal_id = tidalcache_track_id(state.current_file);
	if (tidal_id > 0) {
		bool want = !tidalsync_is_favorite(tidal_id);
		tidalsync_favorite_toggle(tidal_id, want, qp_tidal_fav_done, NULL);
		refresh_now_playing_card();
		player_refresh_now_playing();
		return;
	}

	const char *slash = strrchr(state.current_file, '/');
	const char *title = state.metadata.title[0] ? state.metadata.title : (slash ? slash + 1 : state.current_file);
	library_fav_toggle(state.current_file, title, state.metadata.artist);
	refresh_now_playing_card();
	player_refresh_now_playing(); // the player's own star follows at once
}

// ---------------------------------------------------------------------------
// the order of the round buttons
// ---------------------------------------------------------------------------

// What an empty place in the grid is written as.
#define QP_SLOT_EMPTY_KEY "-"

// A list is written as two keys of at most eight names each -- `order` and
// `order_more`, `hidden` and `hidden_more` -- because a setting holds 127
// characters and sixteen names do not fit in them. Eight of the longest do,
// with room to spare.
#define QP_LIST_SPLIT 8

// Reads one comma-separated list of button names into `out`, keeping only names
// that are still buttons and dropping the ones already taken by an earlier
// list. A truncated or hand-mangled setting therefore costs the names it
// mangled and nothing else. "-" is kept as an empty place, which is how the
// grid's gaps survive a restart.
static uint8_t parse_list(const char *saved, uint8_t *out, uint8_t cap, bool *placed) {
	uint8_t n = 0;
	const char *p = saved ? saved : "";

	while (*p && n < cap) {
		while (*p == ',' || *p == ' ') {
			p++;
		}
		const char *start = p;
		while (*p && *p != ',') {
			p++;
		}
		size_t len = (size_t)(p - start);
		while (len > 0 && start[len - 1] == ' ') {
			len--;
		}
		if (len == strlen(QP_SLOT_EMPTY_KEY) && strncmp(QP_SLOT_EMPTY_KEY, start, len) == 0) {
			out[n++] = QP_BTN_NONE;
			continue;
		}
		for (int i = 0; i < QP_BTN_COUNT && len; i++) {
			if (placed[i] || strlen(button_key[i]) != len || strncmp(button_key[i], start, len) != 0) {
				continue;
			}
			placed[i] = true;
			out[n++] = (uint8_t)i;
			break;
		}
	}
	return n;
}

// The layout the config file is written in.
//
//   1  no grid and no list of the ones left out; every button was in the panel
//   2  a visible list and a hidden list, crossfade out and SonixLink in its place
//   3  a grid of eight places that can be empty
//   4  a grid of sixteen places, written without the empty ones at its end and
//      split over two keys each (see QP_LIST_SPLIT)
//
// A file older than the current version is brought forward on the way in: at 1
// crossfade leaves the panel and SonixLink takes the place it had, and below 4
// the list is poured into the first eight places, anything past the eighth
// going out with the rest. The other eight start empty: the panel a file of
// that age describes is the panel it keeps.
//
// Nothing is written here. The move is worked out again at every start until
// the page is used, and then saved with the rest -- a boot that changes nothing
// has no business writing to the card.
#define QP_LAYOUT_VERSION 4

static void order_load(void) {
	if (order_loaded) {
		return;
	}
	order_loaded = true;

	long saved_version = config_get_int("quickpanel", "layout", 1);
	int grid = saved_version < 4 ? QP_SLOT_VISIBLE : QP_SLOT_COUNT;

	bool placed[QP_BTN_COUNT] = {false};

	// Long enough for a saved grid of empty places plus every button the file
	// never mentioned, which is the worst a hand-edited file can produce.
	uint8_t line[QP_SLOT_COUNT + QP_BTN_COUNT];
	uint8_t line_n = parse_list(config_get("quickpanel", "order", ""), line, QP_LIST_SPLIT, placed);
	if (saved_version >= 4) {
		line_n += parse_list(config_get("quickpanel", "order_more", ""), line + line_n,
							 (uint8_t)(QP_SLOT_COUNT - line_n), placed);
	}
	hidden_n = parse_list(config_get("quickpanel", "hidden", ""), hidden, QP_BTN_COUNT, placed);
	hidden_n += parse_list(config_get("quickpanel", "hidden_more", ""), hidden + hidden_n,
						   (uint8_t)(QP_BTN_COUNT - hidden_n), placed);

	// Anything the file did not mention: a fresh install, or a button that did
	// not exist when it was written. It goes into the panel, because a control
	// nobody has decided about is better offered than hidden -- except the two
	// below, and the grid is already full without them.
	for (int i = 0; i < QP_BTN_COUNT; i++) {
		if (placed[i]) {
			continue;
		}
		// Crossfade, which the migration below is taking out on purpose, and the
		// buttons that arrive into a panel already holding eight: DLNA, the three
		// sleep timers, Wi-Fi transfer, gapless, the volume lock and Direct.
		// Appending them would change a panel nobody asked to change, so they
		// wait among the unused ones, where the settings page shows them.
		bool waits = i == QP_BTN_DLNA || i == QP_BTN_SLEEP_MUSIC || i == QP_BTN_SLEEP_AUDIOBOOK ||
					 i == QP_BTN_SLEEP_PODCAST || i == QP_BTN_WIFI_TRANSFER || i == QP_BTN_GAPLESS ||
					 i == QP_BTN_VOLUME_LOCK || i == QP_BTN_DIRECT;
		if ((saved_version < 2 && i == QP_BTN_FADE) || waits) {
			hidden[hidden_n++] = (uint8_t)i;
		} else {
			line[line_n++] = (uint8_t)i;
		}
	}

	if (saved_version < 2) {
		// Crossfade out, SonixLink into the place it held. By name and not by
		// index, because the saved order is the user's and not the built-in one.
		for (int i = 0; i < line_n; i++) {
			if (line[i] != QP_BTN_FADE) {
				continue;
			}
			line[i] = QP_BTN_SONIXLINK;
			hidden[hidden_n++] = QP_BTN_FADE;

			// SonixLink was appended a moment ago as an unmentioned button; now
			// that it has a place of its own, that copy has to go.
			for (int j = i + 1; j < line_n; j++) {
				if (line[j] == QP_BTN_SONIXLINK) {
					memmove(&line[j], &line[j + 1], (size_t)(line_n - j - 1));
					line_n--;
					break;
				}
			}
			break;
		}
	}

	// Into the grid. From version 3 on the list already carries the empty
	// places as QP_BTN_NONE, so this is a copy; below it, it is the pour.
	int at = 0;
	for (int i = 0; i < line_n && at < grid; i++) {
		slots[at++] = line[i];
	}
	while (at < QP_SLOT_COUNT) {
		slots[at++] = QP_BTN_NONE;
	}

	// Whatever did not fit, which only the buttons appended above can be.
	for (int i = grid; i < line_n; i++) {
		if (line[i] < QP_BTN_COUNT) {
			hidden[hidden_n++] = line[i];
		}
	}
}

// Writes `count` names from `items` under `key`, "-" for an empty place.
static void write_list(const char *key, const uint8_t *items, int count) {
	char text[QP_LIST_SPLIT * 20];
	size_t used = 0;

	text[0] = '\0';
	for (int i = 0; i < count; i++) {
		const char *name = items[i] < QP_BTN_COUNT ? button_key[items[i]] : QP_SLOT_EMPTY_KEY;
		int wrote = snprintf(text + used, sizeof(text) - used, "%s%s", i ? "," : "", name);
		if (wrote <= 0 || (size_t)wrote >= sizeof(text) - used) {
			break;
		}
		used += (size_t)wrote;
	}
	config_set("quickpanel", key, text);
}

static void order_save(void) {
	// Up to the last button: the empty places after it are what a short list
	// reads back as anyway.
	int last = QP_SLOT_COUNT;
	while (last > 0 && slots[last - 1] >= QP_BTN_COUNT) {
		last--;
	}
	int head = last < QP_LIST_SPLIT ? last : QP_LIST_SPLIT;
	write_list("order", slots, head);
	write_list("order_more", slots + head, last - head);

	head = hidden_n < QP_LIST_SPLIT ? hidden_n : QP_LIST_SPLIT;
	write_list("hidden", hidden, head);
	write_list("hidden_more", hidden + head, hidden_n - head);

	config_set_int("quickpanel", "layout", QP_LAYOUT_VERSION);
	config_save();
}

static lv_obj_t *button_widget(quickpanel_button_t which) {
	switch (which) {
	case QP_BTN_WIFI:
		return wifi_btn;
	case QP_BTN_BLUETOOTH:
		return bt_btn;
	case QP_BTN_AIRPLAY:
		return airplay_btn;
	case QP_BTN_MSEB:
		return mseb_btn;
	case QP_BTN_EQ:
		return eq_btn;
	case QP_BTN_FADE:
		return fade_btn;
	case QP_BTN_GAIN:
		return gain_btn;
	case QP_BTN_LINEOUT:
		return lineout_btn;
	case QP_BTN_SONIXLINK:
		return sonixlink_btn;
	case QP_BTN_PEQ:
		return peq_btn;
	case QP_BTN_DLNA:
		return dlna_btn;
	case QP_BTN_SLEEP_MUSIC:
		return sleep_music_btn;
	case QP_BTN_SLEEP_AUDIOBOOK:
		return sleep_audiobook_btn;
	case QP_BTN_SLEEP_PODCAST:
		return sleep_podcast_btn;
	case QP_BTN_WIFI_TRANSFER:
		return wifi_transfer_btn;
	case QP_BTN_GAPLESS:
		return gapless_btn;
	case QP_BTN_VOLUME_LOCK:
		return volume_lock_btn;
	case QP_BTN_DIRECT:
		return direct_btn;
	default:
		return NULL;
	}
}

// Puts the widgets in the grid's order: the first QP_SLOT_VISIBLE into the
// card's own rows, the rest into the block the stretch uncovers. Both are
// wrapping flexes, so a child's index is its position on screen and nothing
// has to be rebuilt; a hidden child is skipped by the flex layout, so an empty
// place in the grid is simply a button the panel does not draw, and the row
// closes up behind it.
static void order_apply(void) {
	int at = 0;
	for (int i = 0; i < QP_SLOT_COUNT; i++) {
		if (slots[i] >= QP_BTN_COUNT) {
			continue;
		}
		lv_obj_t *btn = button_widget((quickpanel_button_t)slots[i]);
		if (!btn) {
			continue;
		}
		bool first = at < QP_SLOT_VISIBLE;
		lv_obj_t *home = first ? quick_row : extra_row;
		if (lv_obj_get_parent(btn) != home) {
			lv_obj_set_parent(btn, home);
		}
		lv_obj_set_hidden(btn, false);
		lv_obj_move_to_index(btn, first ? at : at - QP_SLOT_VISIBLE);
		at++;
	}
	for (int i = 0; i < hidden_n; i++) {
		lv_obj_t *btn = button_widget((quickpanel_button_t)hidden[i]);
		if (btn) {
			lv_obj_set_hidden(btn, true);
		}
	}

	// How far the card stretches: a line and the gap above it for each line of
	// the second block, the same pitch as the lines above.
	int extra = at > QP_SLOT_VISIBLE ? at - QP_SLOT_VISIBLE : 0;
	int lines = (extra + QP_BUTTONS_PER_LINE - 1) / QP_BUTTONS_PER_LINE;
	expand_full = lines * (CIRCLE_BUTTON_SIZE + CIRCLE_BUTTON_GAP);

	// With a handle to show the card takes HANDLE_ROOM from the now-playing
	// card, which has height to spare under its transport; without one both
	// stay as they always were.
	if (controls_card) {
		int room = expand_full ? HANDLE_ROOM : 0;
		card_h_base = card_h_rest + room;
		lv_obj_set_y(np_card, card_top + card_h_base + CARD_GAP);
		lv_obj_set_height(np_card, card_h_rest - room);
		lv_obj_set_hidden(expand_handle, expand_full == 0);
	}
	expand_to(0, false);
}

int quickpanel_hidden_count(void) {
	order_load();
	return hidden_n;
}

quickpanel_button_t quickpanel_hidden_at(int position) {
	order_load();
	if (position < 0 || position >= hidden_n) {
		return QP_BTN_NONE;
	}
	return (quickpanel_button_t)hidden[position];
}

// Takes `button` out of the list of the ones left out. A handful of bytes, so a
// memmove and not a data structure.
static void hidden_remove(quickpanel_button_t button) {
	for (int i = 0; i < hidden_n; i++) {
		if (hidden[i] != button) {
			continue;
		}
		memmove(&hidden[i], &hidden[i + 1], (size_t)(hidden_n - i - 1));
		hidden_n--;
		return;
	}
}

static void layout_changed(void) {
	order_save();
	order_apply();
}

int quickpanel_in_use_count(void) {
	order_load();
	int n = 0;
	for (int i = 0; i < QP_SLOT_COUNT; i++) {
		n += slots[i] < QP_BTN_COUNT;
	}
	return n;
}

quickpanel_button_t quickpanel_in_use_at(int position) {
	order_load();
	for (int i = 0; i < QP_SLOT_COUNT; i++) {
		if (slots[i] < QP_BTN_COUNT && position-- == 0) {
			return (quickpanel_button_t)slots[i];
		}
	}
	return QP_BTN_NONE;
}

// The buttons in the panel without `button`, in order and with no gaps.
static int in_use_without(quickpanel_button_t button, uint8_t *out) {
	int n = 0;
	for (int i = 0; i < QP_SLOT_COUNT; i++) {
		if (slots[i] < QP_BTN_COUNT && slots[i] != button) {
			out[n++] = slots[i];
		}
	}
	return n;
}

static void hidden_insert(quickpanel_button_t button, int position) {
	if (position < 0) {
		position = 0;
	}
	if (position > hidden_n) {
		position = hidden_n;
	}
	memmove(&hidden[position + 1], &hidden[position], (size_t)(hidden_n - position));
	hidden[position] = (uint8_t)button;
	hidden_n++;
}

void quickpanel_move_in_use(quickpanel_button_t button, int position) {
	order_load();
	if (button < 0 || button >= QP_BTN_COUNT) {
		return;
	}
	uint8_t list[QP_SLOT_COUNT + 1];
	int n = in_use_without(button, list);
	hidden_remove(button);

	if (position < 0) {
		position = 0;
	}
	if (position > n) {
		position = n;
	}
	memmove(&list[position + 1], &list[position], (size_t)(n - position));
	list[position] = (uint8_t)button;
	n++;

	// One too many: the last goes to the top of the ones left out.
	if (n > QP_SLOT_COUNT) {
		hidden_insert((quickpanel_button_t)list[--n], 0);
	}
	for (int i = 0; i < QP_SLOT_COUNT; i++) {
		slots[i] = i < n ? list[i] : QP_BTN_NONE;
	}
	layout_changed();
}

void quickpanel_move_hidden(quickpanel_button_t button, int position) {
	order_load();
	if (button < 0 || button >= QP_BTN_COUNT) {
		return;
	}
	uint8_t list[QP_SLOT_COUNT];
	int n = in_use_without(button, list);
	for (int i = 0; i < QP_SLOT_COUNT; i++) {
		slots[i] = i < n ? list[i] : QP_BTN_NONE;
	}
	hidden_remove(button);
	hidden_insert(button, position);
	layout_changed();
}

const char *quickpanel_button_tag(quickpanel_button_t button) {
	if (button < 0 || button >= QP_BTN_COUNT) {
		return "";
	}
	return button_tag[button];
}

const lv_image_dsc_t *quickpanel_button_icon(quickpanel_button_t button) {
	switch (button) {
	case QP_BTN_WIFI:
		return &icon_wifi;
	case QP_BTN_BLUETOOTH:
		return &icon_bluetooth;
	case QP_BTN_AIRPLAY:
		return &icon_airplay_quick;
	case QP_BTN_MSEB:
		return &icon_mseb;
	case QP_BTN_EQ:
		return &icon_equalizer;
	case QP_BTN_FADE:
		return &icon_fade_track;
	case QP_BTN_GAIN:
		return &icon_low_gain;
	case QP_BTN_LINEOUT:
		return &icon_lineout;
	case QP_BTN_SONIXLINK:
		return &icon_sonixlink_quick;
	case QP_BTN_PEQ:
		return &icon_peq_quick;
	case QP_BTN_DLNA:
		return &icon_dlna_quick;
	case QP_BTN_SLEEP_MUSIC:
		return &icon_sleep_music_quick;
	case QP_BTN_SLEEP_AUDIOBOOK:
		return &icon_sleep_audiobook_quick;
	case QP_BTN_SLEEP_PODCAST:
		return &icon_sleep_podcast_quick;
	case QP_BTN_WIFI_TRANSFER:
		return &icon_wifi_transfer_quick;
	case QP_BTN_GAPLESS:
		return &icon_gapless_on_quick;
	case QP_BTN_VOLUME_LOCK:
		return &icon_volume_lock_quick;
	case QP_BTN_DIRECT:
		return &icon_direct_quick;
	default:
		return &icon_wifi;
	}
}

// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------

// First in line for a circle's tap and hold: a press that has become a drag of
// the sheet or the card ends there, and the circle's own handlers never hear of
// it. The finger stays on a circle while the sheet carries both, and a circle
// keeps its press while the finger leaves it, so either would otherwise end the
// drag by toggling or by opening a page.
static void circle_guard_cb(lv_event_t *e) {
	if (press_dragged) {
		lv_event_stop_processing(e);
	}
}

// One round quick control: a circle in the pressed-surface colour with the
// glyph centred on it. Callers attach their own handlers. Presses bubble on to
// the sheet, so a drag that starts on a circle moves the sheet or the card like
// a drag anywhere else.
static lv_obj_t *make_circle_button(lv_obj_t *parent, const lv_image_dsc_t *glyph) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_event_bubble(btn, true);
	lv_obj_add_event_cb(btn, circle_guard_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(btn, circle_guard_cb, LV_EVENT_LONG_PRESSED, NULL);
	lv_obj_set_size(btn, CIRCLE_BUTTON_SIZE, CIRCLE_BUTTON_SIZE);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_add_style(btn, &theme_style_switch, 0); // neutral circle that follows the theme
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);

	lv_obj_t *icon = lv_image_create(btn);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
	lv_obj_center(icon);

	return btn;
}

// Paints a quick button for its state: accent fill with a white glyph while on,
// the neutral circle while off.
static void circle_button_set_on(lv_obj_t *btn, bool on) {
	if (!btn) {
		return;
	}
	lv_obj_t *icon = lv_obj_get_child(btn, 0);

	if (on) {
		lv_obj_set_style_bg_color(btn, theme()->accent, 0);
		lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
		if (icon) {
			lv_obj_set_style_image_recolor(icon, lv_color_white(), 0);
		}
	} else {
		lv_obj_remove_local_style_prop(btn, LV_STYLE_BG_COLOR, 0);
		lv_obj_remove_local_style_prop(btn, LV_STYLE_BG_OPA, 0);
		if (icon) {
			lv_obj_remove_local_style_prop(icon, LV_STYLE_IMAGE_RECOLOR, 0);
		}
	}
}

// Whether that timer will actually stop the music: the switch on AND a length
// chosen. See the note over the handlers.
static bool sleep_timer_armed(sleeptimer_kind_t kind);

// Repaints every round control from the state it switches, so the panel shows
// the truth even when the setting was changed somewhere else.
static void refresh_audio_buttons(void) {
	circle_button_set_on(eq_btn, eq_get_enabled());
	circle_button_set_on(mseb_btn, mseb_get_enabled());
	circle_button_set_on(wifi_btn, wifi_get_enabled());
	circle_button_set_on(bt_btn, bluetooth_get_enabled());
	circle_button_set_on(fade_btn, musicsettings_fade_enabled());
	circle_button_set_on(airplay_btn, airplay_get_enabled());
	circle_button_set_on(sonixlink_btn, sonixlink_get_enabled());
	circle_button_set_on(dlna_btn, dlna_get_enabled());
	circle_button_set_on(peq_btn, peq_get_enabled());
	circle_button_set_on(sleep_music_btn, sleep_timer_armed(SLEEPTIMER_MUSIC));
	circle_button_set_on(sleep_audiobook_btn, sleep_timer_armed(SLEEPTIMER_AUDIOBOOK));
	circle_button_set_on(sleep_podcast_btn, sleep_timer_armed(SLEEPTIMER_PODCAST));
	circle_button_set_on(wifi_transfer_btn, wifitransfer_get_enabled());

	// Two or three gains: the icon names the level (L, M, H) and the circle is
	// lit for anything above the lowest.
	int level = musicsettings_gain_level();
	if (gain_btn) {
		lv_obj_t *icon = lv_obj_get_child(gain_btn, 0);
		if (icon) {
			const lv_image_dsc_t *src = &icon_low_gain;
			if (level == gain_level_count() - 1 && level > 0) {
				src = &icon_high_gain;
			} else if (level > 0) {
				src = &icon_mid_gain;
			}
			lv_image_set_src(icon, src);
		}
	}
	circle_button_set_on(gain_btn, level > 0);
	circle_button_set_on(lineout_btn, lineout_is_active());

	// The drawing changes with the state as well as the fill: the two halves
	// closed up while on, apart while off.
	bool gapless = musicsettings_gapless_enabled();
	if (gapless_btn) {
		lv_obj_t *icon = lv_obj_get_child(gapless_btn, 0);
		if (icon) {
			lv_image_set_src(icon, gapless ? &icon_gapless_on_quick : &icon_gapless_off_quick);
		}
	}
	circle_button_set_on(gapless_btn, gapless);
	circle_button_set_on(volume_lock_btn, volume_lock_enabled());
	circle_button_set_on(direct_btn, direct_get_enabled());
}

// After a button has switched something: the circles repaint, and the page
// under the sheet is told, since it may be the very page that setting lives on
// and must not go on showing the old state. Pages that carry one of these
// settings answer LV_EVENT_REFRESH by reading it again; the rest ignore it.
static void toggled(void) {
	refresh_audio_buttons();
	lv_obj_t *page = lv_screen_active();
	if (page) {
		lv_obj_send_event(page, LV_EVENT_REFRESH, NULL);
	}
}

// Line out: the jack driven for an amplifier, at the fixed level the stock
// player uses. Switching it on is worth a word first -- the output goes to full
// scale, and a pair of headphones still in the socket would say so loudly --
// while switching it off needs none, since it only ever lowers the level back
// to the user's own.
// The level moves the instant the mode is switched, so the number in the status
// bar has to move with it. Left to the bar's own poll, the figure would lag the
// sound by up to a second.
static void lineout_applied(void) {
	refresh_audio_buttons();
	topbar_refresh_volume(get_volume_percent());
}

static void lineout_confirmed(void *user) {
	(void)user;
	lineout_set(true);
	lineout_applied();
}

static void lineout_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}

	if (lineout_is_active()) {
		lineout_set(false);
		lineout_applied();
		return;
	}

	// Nothing to drive: an empty socket, or sound going out over the air, where
	// the fixed level would reach the headphones as an absolute volume.
	if (bluetooth_audio_active() || headphone_jack_state() == JACK_NONE) {
		gui_notify_popup("quickpanel_line_out_needs_the_jack");
		return;
	}

	quickpanel_close(); // the dialog belongs to the page, not under the sheet
	confirm_show("quickpanel_line_out", tr("quickpanel_the_volume_will_go_to_100"), "quickpanel_activate", lineout_confirmed, NULL);
}

static void wifi_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}
	if (!wifi_available()) {
		return; // no radio to switch; the settings page explains why
	}
	wifi_set_enabled(!wifi_get_enabled());
	toggled();
	topbar_refresh_radios();
}

static void bt_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}
	if (!bluetooth_available()) {
		return;
	}
	bluetooth_set_enabled(!bluetooth_get_enabled());
	toggled();
	topbar_refresh_radios();
}

// Whether there is a network to be reachable on. The same question the pages of
// these three services ask, so the circle and the page never disagree.
static bool wifi_is_connected(void) {
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
}

// Switching one of the network services on with no network. Says so and
// refuses, rather than lighting a circle for something nothing can reach.
//
// Switching OFF is never gated: if it is on it goes off, whatever has happened
// to the network meanwhile.
static bool network_needed(const char *why) {
	if (wifi_is_connected()) {
		return false;
	}
	gui_notify_popup(why);
	return true;
}

// The AirPlay circle switches the receiver on: from then on the player is
// merely discoverable by phones. It does not stop playback, prompt, or open
// anything. Local music stops only when a phone actually starts sending audio,
// and it is the receiving thread that stops it (play_worker in
// system/airplay.c), not this tap. Reachable is not the same as in use, and
// this matches the Wi-Fi and Bluetooth circles beside it, which also switch on
// without switching anything off.
static void airplay_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}

	bool want = !airplay_get_enabled();

	if (want) {
		if (!airplay_available()) {
			gui_notify_popup("airplay_unavailable");
			return;
		}
		if (network_needed("quickpanel_airplay_only_works_over_wi_fi")) {
			return;
		}
	}

	airplay_set_enabled(want);
	toggled();
}

static void mseb_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return; // the long press that opened the page must not also toggle
	}
	musicsettings_set_mseb_enabled(!mseb_get_enabled());
	toggled();
}

static void eq_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}
	musicsettings_set_eq_enabled(!eq_get_enabled());
	toggled();
}

static void fade_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}
	musicsettings_set_fade_enabled(!musicsettings_fade_enabled());
	toggled();
}

// SonixLink, same contract as the radios: a tap switches the server on or off,
// a hold opens its page, which is where the address and the port are. And the
// same gate as AirPlay beside it -- a server with no network to listen on is a
// lit circle that means nothing.
static void sonixlink_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}

	bool want = !sonixlink_get_enabled();
	// Bluetooth carries it too, for a paired phone: Wi-Fi is needed only when
	// Bluetooth is off.
	if (want && !bluetooth_get_enabled() && network_needed("sonixlink_needs_wifi_or_bluetooth")) {
		return;
	}

	sonixlink_set_enabled(want);
	sonixlink_pump_pace();
	toggled();
}

// ---------------------------------------------------------------------------
// The sleep timers
//
// Three switches for three timers, and what they are for is the switching OFF:
// "it is later than I thought, do not stop" is a thing somebody thinks with the
// player already in their hand, without wanting to walk into the settings page
// of whichever of the three is counting.
//
// Lit means it will actually stop the music, which is the switch AND a length:
// the switch on with the wheels at 00:00 arms nothing (see sleeptimer.h), and a
// button lit for that would be a button that lies. Pressing one with no length
// chosen says so rather than lighting up and doing nothing.
// ---------------------------------------------------------------------------

static bool sleep_timer_armed(sleeptimer_kind_t kind) {
	return sleeptimer_enabled(kind) && sleeptimer_minutes(kind) > 0;
}

static void sleep_timer_clicked(sleeptimer_kind_t kind) {
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}

	if (sleeptimer_enabled(kind)) {
		sleeptimer_set_enabled(kind, false);
	} else if (sleeptimer_minutes(kind) > 0) {
		sleeptimer_set_enabled(kind, true);
	} else {
		gui_notify_popup("quickpanel_sleep_no_length");
		return;
	}
	toggled();
}

static void sleep_music_clicked_cb(lv_event_t *e) {
	(void)e;
	sleep_timer_clicked(SLEEPTIMER_MUSIC);
}

static void sleep_audiobook_clicked_cb(lv_event_t *e) {
	(void)e;
	sleep_timer_clicked(SLEEPTIMER_AUDIOBOOK);
}

static void sleep_podcast_clicked_cb(lv_event_t *e) {
	(void)e;
	sleep_timer_clicked(SLEEPTIMER_PODCAST);
}

static void open_page(lv_obj_t *screen);

// A hold opens the page that sets the length: the playback options for music,
// the audiobook options, the podcast settings. Looked up at the press rather
// than when the buttons are made, since not every one of those pages exists
// by then.
static void sleep_page_cb(lv_event_t *e) {
	sleeptimer_kind_t kind = (sleeptimer_kind_t)(intptr_t)lv_event_get_user_data(e);
	lv_obj_t *screen = kind == SLEEPTIMER_MUSIC		 ? musicsettings_playback_screen()
					   : kind == SLEEPTIMER_AUDIOBOOK ? audiobooksettings_screen
													  : podcastpage_settings_screen();
	open_page(screen);
}

// DLNA, the third of the same family: a phone pushes music at the player over
// the network, so without one there is nothing to push it from.
static void dlna_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}

	bool want = !dlna_get_enabled();

	if (want) {
		if (!dlna_available()) {
			gui_notify_popup("dlna_unavailable");
			return;
		}
		if (network_needed("quickpanel_dlna_only_works_over_wi_fi")) {
			return;
		}
	}

	dlna_set_enabled(want);
	toggled();
}

// Wi-Fi transfer. The server lives only as long as its page is open (see
// gui/wireless/wifitransfer.h), so switching it on opens the page with the
// server starting; switching it off stops it where it is. Same network gate as
// the three above.
static void wifi_transfer_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}

	if (wifitransfer_get_enabled()) {
		wifitransfer_page_stop();
		toggled();
		return;
	}
	if (!wifitransfer_available()) {
		gui_notify_popup("wifitransfer_unavailable");
		return;
	}
	if (network_needed("quickpanel_wifitransfer_only_works_over_wi_fi")) {
		return;
	}

	quickpanel_close();
	bool from_player = player_sheet_is_open();
	if (from_player) {
		player_sheet_close(false);
	}
	wifitransfer_page_open_started();
	if (from_player) {
		switcher_set_player_return(wifitransfer_screen);
	}
}

// The parametric equaliser, same contract as the graphic one beside it: a tap
// switches it, a hold opens its page.
static void peq_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}
	peq_set_enabled(!peq_get_enabled());
	toggled();
}

// Gapless, the switch on the playback options page: a tap flips it, a hold
// opens that page. Looked up at the press, like the sleep timer's page.
static void gapless_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}
	musicsettings_set_gapless_enabled(!musicsettings_gapless_enabled());
	toggled();
}

static void gapless_page_cb(lv_event_t *e) {
	(void)e;
	open_page(musicsettings_playback_screen());
}

// The volume lock (alsa-controls.h): a tap flips it. The status bar's glyph is
// repainted here rather than left to its poll, because the padlock up there is
// the lock's only sign once this sheet is put away.
static void volume_lock_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}
	volume_lock_set_enabled(!volume_lock_enabled());
	topbar_refresh_volume(get_volume_percent());
	toggled();
}

// Direct (eq.h): a tap flips it, a hold opens the music page, where its switch
// sits above the effects it bypasses. The effects' own buttons stay as they
// are: they show what comes back.
static void direct_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}
	direct_set_enabled(!direct_get_enabled());
	toggled();
}

static void gain_clicked_cb(lv_event_t *e) {
	(void)e;
	if (long_press_consumed) {
		long_press_consumed = false;
		return;
	}
	musicsettings_gain_step();
	toggled();
}

static void open_page(lv_obj_t *screen) {
	long_press_consumed = true;
	quickpanel_close();
	if (!screen) {
		return;
	}

	// The player is a sheet on the top layer, not a page: with it up, loading a
	// screen leaves the page behind the sheet, and closing the sheet puts its
	// own opener back, taking the page away again. So the sheet goes down
	// first, and leaving the page comes back into it -- the same route the
	// player's own menu takes to the queue and the track details.
	bool from_player = player_sheet_is_open();
	if (from_player) {
		player_sheet_close(false);
	}
	if (screen == lv_screen_active()) {
		return; // already there: switching would record a step nobody took
	}
	switch_screen(screen);
	if (from_player) {
		switcher_set_player_return(screen);
	}
}

static void open_page_cb(lv_event_t *e) { open_page(lv_event_get_user_data(e)); }

// A bare glyph button for the transport row.
static lv_obj_t *make_flat_button(lv_obj_t *parent, int size, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, size, size);
	lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_pad_all(btn, 0, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
	return btn;
}

// The rounded surface each half of the panel sits on. Both cards are the same
// size and stack from the top of the sheet with one gap between them, so
// together they fill the screen instead of floating in the middle of it.
static lv_obj_t *make_card(lv_obj_t *parent, int width, int height, int top_y) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_ignore_layout(card, true);
	lv_obj_set_size(card, width, height);
	lv_obj_align(card, LV_ALIGN_TOP_MID, 0, top_y);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, ui_px(20), 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, CARD_PADDING, 0);
	lv_obj_set_scrollable(card, false);
	// Presses bubble to the sheet, whose drag handler carries the panel back out
	// from anywhere on it.
	lv_obj_set_event_bubble(card, true);
	return card;
}

void quickpanel_init(gui_config_t *cfg) {
	panel_h = cfg->screen_height;

	veil = lv_obj_create(lv_layer_top());
	lv_obj_set_size(veil, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(veil, 0, 0);
	lv_obj_set_style_bg_opa(veil, LV_OPA_TRANSP, 0); // see anim_y_cb: it is a tap target, not a dimmer
	lv_obj_set_style_border_width(veil, 0, 0);
	lv_obj_set_style_radius(veil, 0, 0);
	lv_obj_set_style_pad_all(veil, 0, 0);
	lv_obj_set_scrollable(veil, false);
	lv_obj_set_hidden(veil, true);
	lv_obj_add_event_cb(veil, veil_clicked_cb, LV_EVENT_CLICKED, NULL);

	// The sheet fills the whole screen, iOS-control-centre style.
	panel = lv_obj_create(veil);
	lv_obj_set_size(panel, cfg->screen_width, panel_h);
	lv_obj_set_pos(panel, 0, -panel_h);
	lv_obj_add_style(panel, &theme_style_screen, 0);
	lv_obj_set_style_radius(panel, 0, 0);
	lv_obj_set_style_border_width(panel, 0, 0);
	lv_obj_set_style_pad_hor(panel, cfg->padding, 0);
	lv_obj_set_style_pad_top(panel, PANEL_PAD_TOP(cfg), 0);
	lv_obj_set_style_pad_bottom(panel, PANEL_PAD_BOTTOM, 0);
	lv_obj_set_style_pad_gap(panel, ui_px(14), 0);
	lv_obj_set_scrollable(panel, false);
	lv_obj_set_event_bubble(panel, false);
	lv_obj_set_clickable(panel, true);
	lv_obj_add_event_cb(panel, panel_drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(panel, panel_drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(panel, panel_drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(panel, panel_drag_cb, LV_EVENT_PRESS_LOST, NULL);

	// Two equal cards stacked under the status bar: a small gap above the first,
	// one between them, a strip at the bottom for the close line, and the rest
	// split evenly between the cards. All measured inside the sheet's padding,
	// which is where LVGL aligns them.
	int content_h = panel_h - PANEL_PAD_TOP(cfg) - PANEL_PAD_BOTTOM;
	int top_inset = cfg->top_bar_height - PANEL_PAD_TOP(cfg) + CARD_TOP_GAP;
	if (top_inset < 0) {
		top_inset = 0; // a bar shorter than the padding leaves nothing to clear
	}
	int card_w = cfg->screen_width - 2 * cfg->padding;
	int card_h = (content_h - top_inset - HINT_STRIP_H - CARD_GAP) / 2;
	if (card_h < 160) {
		card_h = 160; // floor for panels shorter than any shipped screen
	}

	// --- First card: the quick controls. ---
	controls_card = make_card(panel, card_w, card_h, top_inset);
	card_h_rest = card_h;
	card_h_base = card_h;
	card_top = top_inset;
	lv_obj_set_flex_flow(controls_card, LV_FLEX_FLOW_COLUMN);
	// Brightness under the buttons; the buttons take the height above it and
	// their rows are centred in it, so a single row of four sits midway between
	// the top of the card and the slider. A fixed height and not a share of the
	// card, so the slider stays where it is while the card stretches.
	lv_obj_set_flex_align(controls_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(controls_card, CARD_ROW_GAP, 0);
	int row_h = card_h - 2 * CARD_PADDING - BRIGHT_ROW_H - CARD_ROW_GAP;

	lv_obj_t *row = lv_obj_create(controls_card);
	quick_row = row;
	// Wraps to a second line past four buttons; the third flex argument below
	// centres the lines inside the height.
	lv_obj_set_size(row, lv_pct(100), row_h);
	lv_obj_set_style_bg_opa(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_gap(row, CIRCLE_BUTTON_GAP, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_event_bubble(row, true);
	lv_obj_set_style_pad_row(row, CIRCLE_BUTTON_GAP, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// The radios: a tap switches them, a hold opens their page, the same contract
	// MSEB and the equalizer use.
	wifi_btn = make_circle_button(row, &icon_wifi);
	lv_obj_add_event_cb(wifi_btn, wifi_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(wifi_btn, open_page_cb, LV_EVENT_LONG_PRESSED, wifisettings_screen);

	bt_btn = make_circle_button(row, &icon_bluetooth);
	lv_obj_add_event_cb(bt_btn, bt_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(bt_btn, open_page_cb, LV_EVENT_LONG_PRESSED, btsettings_screen);

	// AirPlay sits beside the other two radios because it is a switch to reach
	// while doing something else, not a page to enter. Same contract as Wi-Fi and
	// Bluetooth: tap switches it, long press opens the page, which remains where
	// the incoming stream is shown.
	airplay_btn = make_circle_button(row, &icon_airplay_quick);
	lv_obj_add_event_cb(airplay_btn, airplay_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(airplay_btn, open_page_cb, LV_EVENT_LONG_PRESSED, airplay_screen);

	// MSEB and the equalizer: tap to switch on or off, hold to open the page.
	mseb_btn = make_circle_button(row, &icon_mseb);
	lv_obj_add_event_cb(mseb_btn, mseb_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(mseb_btn, open_page_cb, LV_EVENT_LONG_PRESSED, musicsettings_mseb_screen());

	eq_btn = make_circle_button(row, &icon_equalizer);
	lv_obj_add_event_cb(eq_btn, eq_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(eq_btn, open_page_cb, LV_EVENT_LONG_PRESSED, musicsettings_eq_screen());

	// Crossfade, same contract: tap switches it, hold opens its page.
	fade_btn = make_circle_button(row, &icon_fade_track);
	lv_obj_add_event_cb(fade_btn, fade_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(fade_btn, open_page_cb, LV_EVENT_LONG_PRESSED, musicsettings_fade_screen());

	// Gain is the odd control: two states rather than on and off, so the glyph
	// itself changes and the accent marks only the loud one. Low gain is not
	// "off", it is the normal setting.
	//
	// No long press here: gain is a plain switch on the music settings page
	// rather than a section of its own, so there is no page to shortcut to, and
	// opening the whole page from a hold surprised more than it helped.
	gain_btn = make_circle_button(row, &icon_low_gain);
	lv_obj_add_event_cb(gain_btn, gain_clicked_cb, LV_EVENT_CLICKED, NULL);

	// Line out: a tap switches the jack between headphones and an amplifier. No
	// long press either -- there is no page behind it.
	lineout_btn = make_circle_button(row, &icon_lineout);
	lv_obj_add_event_cb(lineout_btn, lineout_clicked_cb, LV_EVENT_CLICKED, NULL);

	sonixlink_btn = make_circle_button(row, &icon_sonixlink_quick);
	lv_obj_add_event_cb(sonixlink_btn, sonixlink_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(sonixlink_btn, open_page_cb, LV_EVENT_LONG_PRESSED, sonixlink_screen);

	peq_btn = make_circle_button(row, &icon_peq_quick);
	lv_obj_add_event_cb(peq_btn, peq_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(peq_btn, open_page_cb, LV_EVENT_LONG_PRESSED, peqpage_screen());

	dlna_btn = make_circle_button(row, &icon_dlna_quick);
	lv_obj_add_event_cb(dlna_btn, dlna_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(dlna_btn, open_page_cb, LV_EVENT_LONG_PRESSED, dlna_screen);

	// Each of the three opens, on a long press, the page of its own kind of
	// listening, where its length is chosen.
	sleep_music_btn = make_circle_button(row, &icon_sleep_music_quick);
	lv_obj_add_event_cb(sleep_music_btn, sleep_music_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(sleep_music_btn, sleep_page_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)SLEEPTIMER_MUSIC);

	sleep_audiobook_btn = make_circle_button(row, &icon_sleep_audiobook_quick);
	lv_obj_add_event_cb(sleep_audiobook_btn, sleep_audiobook_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(sleep_audiobook_btn, sleep_page_cb, LV_EVENT_LONG_PRESSED,
						(void *)(intptr_t)SLEEPTIMER_AUDIOBOOK);

	sleep_podcast_btn = make_circle_button(row, &icon_sleep_podcast_quick);
	lv_obj_add_event_cb(sleep_podcast_btn, sleep_podcast_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(sleep_podcast_btn, sleep_page_cb, LV_EVENT_LONG_PRESSED,
						(void *)(intptr_t)SLEEPTIMER_PODCAST);

	// A tap opens the page with the server starting; a hold opens it as it is.
	wifi_transfer_btn = make_circle_button(row, &icon_wifi_transfer_quick);
	lv_obj_add_event_cb(wifi_transfer_btn, wifi_transfer_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(wifi_transfer_btn, open_page_cb, LV_EVENT_LONG_PRESSED, wifitransfer_screen);

	gapless_btn = make_circle_button(row, &icon_gapless_off_quick);
	lv_obj_add_event_cb(gapless_btn, gapless_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(gapless_btn, gapless_page_cb, LV_EVENT_LONG_PRESSED, NULL);

	volume_lock_btn = make_circle_button(row, &icon_volume_lock_quick);
	lv_obj_add_event_cb(volume_lock_btn, volume_lock_clicked_cb, LV_EVENT_CLICKED, NULL);

	direct_btn = make_circle_button(row, &icon_direct_quick);
	lv_obj_add_event_cb(direct_btn, direct_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(direct_btn, open_page_cb, LV_EVENT_LONG_PRESSED, musicsettings_screen);

	bright_row = lv_obj_create(controls_card);
	lv_obj_set_size(bright_row, lv_pct(100), BRIGHT_ROW_H);
	lv_obj_set_style_bg_opa(bright_row, 0, 0);
	lv_obj_set_style_border_width(bright_row, 0, 0);
	lv_obj_set_style_pad_all(bright_row, 0, 0);
	// The knob is 26 px across and centred on the value, so at full scale half of
	// it falls past the end of the track; the row clips its children, which would
	// slice the knob down the middle. Keeping the track 13 px short of the row's
	// right edge gives it room.
	lv_obj_set_style_pad_right(bright_row, ui_px(13), 0);
	lv_obj_set_style_pad_gap(bright_row, ui_px(18), 0);
	lv_obj_set_scrollable(bright_row, false);
	lv_obj_set_event_bubble(bright_row, true);
	lv_obj_set_flex_flow(bright_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bright_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *sun = lv_image_create(bright_row);
	lv_image_set_src(sun, &icon_sun);
	lv_obj_add_style(sun, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(sun, LV_OPA_COVER, 0);

	brightness_slider = lv_slider_create(bright_row);
	lv_obj_set_height(brightness_slider, ui_px(10));
	lv_obj_set_flex_grow(brightness_slider, 1);
	lv_slider_set_range(brightness_slider, BRIGHTNESS_FLOOR, 100);
	lv_obj_set_style_radius(brightness_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_bg_color(brightness_slider, theme()->text_secondary, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(brightness_slider, LV_OPA_40, LV_PART_MAIN);
	lv_obj_set_style_radius(brightness_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(brightness_slider, theme()->accent, LV_PART_INDICATOR);
	lv_obj_set_style_radius(brightness_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
	lv_obj_set_style_bg_color(brightness_slider, lv_color_white(), LV_PART_KNOB);
	theme_apply_slider_knob(brightness_slider);
	lv_obj_set_style_pad_all(brightness_slider, ui_px(8), LV_PART_KNOB);
	lv_obj_set_style_shadow_width(brightness_slider, ui_px(8), LV_PART_KNOB);
	lv_obj_set_style_shadow_opa(brightness_slider, LV_OPA_30, LV_PART_KNOB);
	lv_obj_set_style_shadow_color(brightness_slider, lv_color_black(), LV_PART_KNOB);
	lv_obj_set_ext_click_area(brightness_slider, ui_px(18));
	lv_obj_add_event_cb(brightness_slider, brightness_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(brightness_slider, brightness_released_cb, LV_EVENT_RELEASED, NULL);

	// The buttons past the eighth. A window outside the flex, starting where the
	// first two lines end, opens as tall as the stretch while the slider moves
	// down by as much; the buttons sit in it one gap down, so they read as
	// further lines of the grid above. The window clips them, so the stretch is
	// what uncovers them.
	int first_lines = QP_SLOT_VISIBLE / QP_BUTTONS_PER_LINE;
	int first_h = first_lines * CIRCLE_BUTTON_SIZE + (first_lines - 1) * CIRCLE_BUTTON_GAP;
	extra_clip = lv_obj_create(controls_card);
	lv_obj_remove_style_all(extra_clip);
	lv_obj_set_ignore_layout(extra_clip, true);
	lv_obj_set_size(extra_clip, lv_pct(100), 0);
	lv_obj_set_pos(extra_clip, 0, (row_h + first_h) / 2); // the grid's lines are centred in the row
	lv_obj_set_scrollable(extra_clip, false);
	lv_obj_set_event_bubble(extra_clip, true);
	lv_obj_set_hidden(extra_clip, true);

	extra_row = lv_obj_create(extra_clip);
	lv_obj_set_size(extra_row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_pos(extra_row, 0, CIRCLE_BUTTON_GAP);
	lv_obj_set_style_bg_opa(extra_row, 0, 0);
	lv_obj_set_style_border_width(extra_row, 0, 0);
	lv_obj_set_style_pad_all(extra_row, 0, 0);
	lv_obj_set_style_pad_gap(extra_row, CIRCLE_BUTTON_GAP, 0);
	lv_obj_set_scrollable(extra_row, false);
	lv_obj_set_event_bubble(extra_row, true);
	lv_obj_set_flex_flow(extra_row, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(extra_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// The handle: a short line in the band under the slider -- the room the
	// card gains for it and the card's bottom padding -- which travels down
	// with the edge as the card stretches. The line is what is seen; the strip
	// round it, the whole band high, is what a tap finds, and a tap folds or
	// stretches the card. Presses bubble on to the sheet, so a drag starting on
	// it is the same drag as anywhere else.
	expand_handle = lv_obj_create(controls_card);
	lv_obj_set_ignore_layout(expand_handle, true);
	lv_obj_set_size(expand_handle, ui_px(160), HANDLE_ROOM + CARD_PADDING);
	lv_obj_align(expand_handle, LV_ALIGN_BOTTOM_MID, 0, CARD_PADDING);
	lv_obj_set_style_bg_opa(expand_handle, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(expand_handle, 0, 0);
	lv_obj_set_style_shadow_width(expand_handle, 0, 0);
	lv_obj_set_style_pad_all(expand_handle, 0, 0);
	lv_obj_set_scrollable(expand_handle, false);
	lv_obj_set_event_bubble(expand_handle, true);
	lv_obj_add_event_cb(expand_handle, expand_handle_clicked_cb, LV_EVENT_CLICKED, NULL);

	expand_pill = lv_obj_create(expand_handle);
	lv_obj_set_size(expand_pill, ui_px(36), ui_px(5));
	lv_obj_center(expand_pill);
	lv_obj_set_style_radius(expand_pill, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(expand_pill, theme()->text_primary, 0);
	lv_obj_set_style_bg_opa(expand_pill, LV_OPA_40, 0);
	lv_obj_set_style_border_width(expand_pill, 0, 0);
	lv_obj_set_style_shadow_width(expand_pill, 0, 0);
	lv_obj_set_clickable(expand_pill, false);

	// --- Second card: what is playing, and its transport. ---
	np_card = make_card(panel, card_w, card_h, top_inset + card_h + CARD_GAP);
	lv_obj_set_flex_flow(np_card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(np_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(np_card, ui_px(6), 0);

	// Title and artist are centred and scroll when too long, the same treatment
	// as the player's now-playing lines. Each is exactly one line of its own
	// font high: a scrolling label shorter than its text scrolls it upwards
	// instead of sideways, which is what a fixed height did to the artist as
	// soon as the text size was Large.
	np_title = lv_label_create(np_card);
	lv_obj_set_width(np_title, lv_pct(100));
	lv_obj_set_height(np_title, lv_font_get_line_height(&font_ui_26));
	lv_obj_add_style(np_title, &theme_style_text, 0);
	lv_obj_set_style_text_font(np_title, &font_ui_26, 0);
	lv_obj_set_style_text_align(np_title, LV_TEXT_ALIGN_CENTER, 0);
	scrolltext_apply(np_title);

	np_artist = lv_label_create(np_card);
	lv_obj_set_width(np_artist, lv_pct(100));
	lv_obj_set_height(np_artist, lv_font_get_line_height(&font_ui_22));
	lv_obj_add_style(np_artist, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(np_artist, &font_ui_22, 0);
	lv_obj_set_style_text_align(np_artist, LV_TEXT_ALIGN_CENTER, 0);
	scrolltext_apply(np_artist);

	lv_obj_t *transport = lv_obj_create(np_card);
	lv_obj_set_size(transport, lv_pct(100), ui_px(96));
	lv_obj_set_style_bg_opa(transport, 0, 0);
	lv_obj_set_style_border_width(transport, 0, 0);
	lv_obj_set_style_pad_all(transport, 0, 0);
	lv_obj_set_scrollable(transport, false);
	lv_obj_set_event_bubble(transport, true);
	lv_obj_set_flex_flow(transport, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(transport, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(transport, ui_px(4), 0);
	lv_obj_set_style_pad_top(transport, ui_px(10), 0);

	// Repeat/shuffle pinned left, the star pinned right, transport centred
	// between them: the player's arrangement.
	lv_obj_t *repeat_btn = make_flat_button(transport, ui_px(56), repeat_cb);
	np_repeat_btn = repeat_btn;
	lv_obj_set_ignore_layout(repeat_btn, true);
	lv_obj_align(repeat_btn, LV_ALIGN_LEFT_MID, 0, 0);
	np_repeat_icon = lv_image_create(repeat_btn);
	lv_obj_center(np_repeat_icon);

	lv_obj_t *prev_btn = make_flat_button(transport, ui_px(76), prev_cb);
	np_prev_btn = prev_btn;
	np_star_icon = NULL; // really created further down, once the star button exists
	lv_obj_t *prev_icon = lv_image_create(prev_btn);
	np_prev_icon = prev_icon;
	lv_image_set_src(prev_icon, &icon_skip_back_large);
	lv_obj_add_style(prev_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(prev_icon, LV_OPA_COVER, 0);
	lv_obj_center(prev_icon);

	// The play disc keeps the neutral circle colour the other round controls use;
	// the white disc belongs over artwork, not on a card that follows the theme.
	lv_obj_t *play_btn = make_flat_button(transport, ui_px(84), play_cb);
	lv_obj_set_style_radius(play_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_add_style(play_btn, &theme_style_switch, 0);
	lv_obj_set_style_bg_opa(play_btn, LV_OPA_COVER, 0);
	np_play_btn = play_btn;
	np_play_icon = lv_image_create(play_btn);
	lv_obj_add_style(np_play_icon, &theme_style_icon, 0);
	lv_obj_center(np_play_icon);

	lv_obj_t *next_btn = make_flat_button(transport, ui_px(76), next_cb);
	np_next_btn = next_btn;
	lv_obj_t *next_icon = lv_image_create(next_btn);
	// The look of the three while a station connects; see the refresh.
	lv_obj_set_style_opa(prev_btn, LV_OPA_40, LV_STATE_DISABLED);
	lv_obj_set_style_opa(play_btn, LV_OPA_40, LV_STATE_DISABLED);
	lv_obj_set_style_opa(next_btn, LV_OPA_40, LV_STATE_DISABLED);
	np_next_icon = next_icon;
	lv_image_set_src(next_icon, &icon_skip_forward_large);
	lv_obj_add_style(next_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(next_icon, LV_OPA_COVER, 0);
	lv_obj_center(next_icon);

	lv_obj_t *star_btn = make_flat_button(transport, ui_px(56), star_cb);
	np_star_btn = star_btn;
	lv_obj_set_ignore_layout(star_btn, true);
	lv_obj_align(star_btn, LV_ALIGN_RIGHT_MID, 0, 0);
	np_star_icon = lv_image_create(star_btn);
	lv_obj_center(np_star_icon);

	// --- The way out: a short bar at the bottom, a home-indicator rather than a
	// chevron, because it reads as "drag me" instead of "press me" and dragging
	// is what actually closes the panel. The line is not clickable: a finger
	// landing on it talks to the surface underneath, where the gesture lives.
	lv_obj_t *close_btn = lv_obj_create(panel);
	lv_obj_set_ignore_layout(close_btn, true);
	lv_obj_set_size(close_btn, ui_px(140), HINT_STRIP_H - ui_px(8));
	// Flush with the bottom of the sheet's content box, which centres the line in
	// the band between the last card and the screen edge: the band is
	// HINT_STRIP_H below the card plus PANEL_PAD_BOTTOM under the content box,
	// and the button is HINT_STRIP_H - 8 tall, so its middle lands halfway. Any
	// offset here takes it off centre.
	lv_obj_align(close_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_bg_opa(close_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(close_btn, 0, 0);
	lv_obj_set_style_border_width(close_btn, 0, 0);
	lv_obj_set_style_pad_all(close_btn, 0, 0);
	lv_obj_set_clickable(close_btn, false);
	lv_obj_set_scrollable(close_btn, false);

	lv_obj_t *hint = lv_image_create(close_btn);
	lv_image_set_src(hint, &icon_control_center_line);
	lv_obj_add_style(hint, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(hint, LV_OPA_60, 0);
	lv_obj_center(hint);

	// The controls card in front of the now-playing one, which it covers when
	// stretched.
	lv_obj_move_to_index(controls_card, lv_obj_get_index(np_card));

	// Built in the order above, then arranged into the user's -- see
	// Settings > More > Control centre. Last, once both rows and the handle
	// exist to arrange.
	order_load();
	order_apply();

	poll_timer = lv_timer_create(poll_cb, PANEL_POLL_MS, NULL);
	lv_timer_pause(poll_timer);
}
