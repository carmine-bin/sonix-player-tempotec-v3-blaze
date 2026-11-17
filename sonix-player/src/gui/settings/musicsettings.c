#include "musicsettings.h"
#include "musictiles.h"

#include <stdint.h>
#include <stdio.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/libraryscan.h"
#include "src/gui/nowplaying/meterview.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/audio/eqsettings.h"
#include "src/gui/audio/msebsettings.h"
#include "src/gui/audio/peqpage.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/settings/artistexceptions.h"
#include "src/gui/settings/lastfmsettings.h"
#include "src/gui/library/medialist.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/audio/alsa-controls.h"
#include "src/gui/shell/topbar.h"
#include "src/system/device/sysinfo.h"
#include "src/system/playback/playlist.h"
#include "src/system/playback/sleeptimer.h"
#include "src/system/audio/audio.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/audio/eq.h"
#include "src/system/playback/device_state.h"
#include "src/system/decode/decode.h"
#include "src/system/audio/replaygain.h"
#include "src/system/core/config.h"
#include "src/system/audio/usbaudio.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/library/metadata.h"
#include "src/gui/shell/uiscale.h"

lv_obj_t *musicsettings_screen;

// ---------------------------------------------------------------------------
// Filters: the CS43198's four digital filters, the same set the stock player
// exposes (ALSA "Digital Filter" 0..3, decompile FUN_004803e0), or the
// AK4493's six on the V3 Blaze (0..5, the order of its codec driver).
// ---------------------------------------------------------------------------

#define FILTER_MAX 6

static lv_obj_t *dacfilter_screen;
static lv_obj_t *filter_checks[FILTER_MAX];

static const char *const FILTER_NAMES[4] = {
	"musicsettings_fast_roll_off_low_latency",
	"musicsettings_fast_roll_off_phase_compensated",
	"musicsettings_slow_roll_off_low_latency",
	"musicsettings_slow_roll_off_phase_compensated",
};

static const char *const AK4493_FILTER_NAMES[6] = {
	"musicsettings_ak_sharp_roll_off",
	"musicsettings_ak_slow_roll_off",
	"musicsettings_ak_short_delay_sharp",
	"musicsettings_ak_short_delay_slow",
	"musicsettings_ak_super_slow",
	"musicsettings_ak_low_dispersion",
};

// Until one is chosen, the filter the stock firmware plays through. On the
// Blaze that is the AK4493's own reset value, short delay sharp roll-off (SD
// set in its codec driver's register 1 default, 0x22): the stock player never
// writes the control.
static int stored_dac_filter(void) {
	return (int)config_get_int("audio", "dac_filter", alsa_board_is_ak4493() ? 2 : 0);
}

static void refresh_filter_checks(void) {
	int active = stored_dac_filter();
	for (int i = 0; i < FILTER_MAX; i++) {
		if (filter_checks[i]) {
			lv_label_set_text(filter_checks[i], i == active ? "\xE2\x9C\x93" : "");
			lv_obj_set_style_text_color(filter_checks[i], theme()->accent, 0);
		}
	}
}

static void filter_row_cb(lv_event_t *e) {
	int filter = (int)(intptr_t)lv_event_get_user_data(e);

	config_set_int("audio", "dac_filter", filter);
	config_save();
	set_dac_filter(filter);
	refresh_filter_checks();
}

static void build_filter_page(gui_config_t *cfg) {
	dacfilter_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(dacfilter_screen, cfg, "musicsettings_filters");

	bool ak4493 = alsa_board_is_ak4493();
	for (int i = 0; i < dac_filter_count(); i++) {
		const char *name = ak4493 ? AK4493_FILTER_NAMES[i] : FILTER_NAMES[i];
		settingsrow_add(container, name, &filter_checks[i], filter_row_cb, (void *)(intptr_t)i);
		lv_obj_set_style_text_font(filter_checks[i], &font_ui_26, 0);
	}

	// An explanatory note: DAC filters mean nothing to most listeners.
	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr(alsa_board_is_cs43131()  ? "musicsettings_filter_note_cs43131"
							  : ak4493				  ? "musicsettings_filter_note_ak4493"
													  : "musicsettings_filter_note"));

	refresh_filter_checks();
	switcher_attach_back_gesture(dacfilter_screen);
}

// ---------------------------------------------------------------------------
// DRE: the DAC's Dynamic Range Enhancement, the ALSA "DRE_EN" switch the
// stock player writes (decompile FUN_00482120), applied at boot and on the
// toggle.
// ---------------------------------------------------------------------------

static lv_obj_t *dre_switch;

// Gapless: local music only. A stream (radio, Qobuz while downloading) has no
// "next track" ready to hand, and the emulator has a PCM of its own. The
// switch tells the audio engine how to behave at track boundaries; anything
// that does not go through them is unaffected.
static lv_obj_t *gapless_switch;

bool musicsettings_gapless_enabled(void) { return audio_get_gapless(); }

void musicsettings_set_gapless_enabled(bool enabled) {
	config_set_int("audio", "gapless", enabled ? 1 : 0);
	config_save();
	audio_set_gapless(enabled);
	if (gapless_switch) {
		if (enabled) {
			lv_obj_add_state(gapless_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(gapless_switch, LV_STATE_CHECKED);
		}
	}
}

static void gapless_toggle_cb(lv_event_t *e) {
	(void)e;
	musicsettings_set_gapless_enabled(lv_obj_has_state(gapless_switch, LV_STATE_CHECKED));
}

static void dre_toggle_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(dre_switch, LV_STATE_CHECKED);
	config_set_int("audio", "dac_dre", on ? 1 : 0);
	config_save();
	set_dac_dre(on ? 1 : 0);
}

// NOS: the DAC's non-oversampling mode, ALSA "NOS_EN" like the stock player
// (decompile FUN_004820e0).
static lv_obj_t *nos_switch;

static void nos_toggle_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(nos_switch, LV_STATE_CHECKED);
	config_set_int("audio", "dac_nos", on ? 1 : 0);
	config_save();
	set_dac_nos(on ? 1 : 0);
}

// High gain: the 6 dB gain step, done as the stock player does it -- a shift
// of the whole volume scale on the DAC's attenuation register.
static lv_obj_t *gain_switch;
static lv_obj_t *album_chain_switch;

// Three gains on the V3 Blaze, as pills instead of the switch; stored as
// "gain_level" there, so a config carried over from a two-gain player is not
// read as one of them.
static lv_obj_t *gain_pill[3];

static int stored_gain_level(void) {
	if (gain_level_count() == 3) {
		int level = (int)config_get_int("audio", "gain_level", 0);
		return level < 0 ? 0 : (level > 2 ? 2 : level);
	}
	return config_get_int("audio", "high_gain", 0) ? 1 : 0;
}

static void gain_refresh(void) {
	int level = stored_gain_level();
	if (gain_switch) {
		if (level > 0) {
			lv_obj_add_state(gain_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(gain_switch, LV_STATE_CHECKED);
		}
	}
	for (int i = 0; i < 3; i++) {
		settingsrow_pill_active(gain_pill[i], i == level);
	}
}

static void gain_level_store(int level) {
	if (gain_level_count() == 3) {
		config_set_int("audio", "gain_level", level);
	} else {
		config_set_int("audio", "high_gain", level ? 1 : 0);
	}
	config_save();
	set_gain_level(level);
	gain_refresh();
}

static void gain_toggle_cb(lv_event_t *e) {
	(void)e;
	gain_level_store(lv_obj_has_state(gain_switch, LV_STATE_CHECKED) ? 1 : 0);
}

static void gain_pick_cb(lv_event_t *e) { gain_level_store((int)(intptr_t)lv_event_get_user_data(e)); }

// Line out, the control centre's button as a row: the same switch, the same
// conditions, the same warning. The row shows the mode, not the tap -- it stays
// off until the warning is accepted, and follows the mode ending on its own
// (the jack pulled, Bluetooth taking over) when the page is shown again.
static lv_obj_t *lineout_switch;

static void lineout_refresh(void) {
	if (!lineout_switch) {
		return;
	}
	lv_obj_set_state(lineout_switch, LV_STATE_CHECKED, lineout_is_active());
}

static void lineout_applied(void) {
	lineout_refresh();
	topbar_refresh_volume(get_volume_percent());
}

static void lineout_confirmed(void *user) {
	(void)user;
	lineout_set(true);
	lineout_applied();
}

static void lineout_toggle_cb(lv_event_t *e) {
	(void)e;
	if (!lv_obj_has_state(lineout_switch, LV_STATE_CHECKED)) {
		lineout_set(false);
		lineout_applied();
		return;
	}
	lineout_refresh(); // off again until the warning is accepted
	if (bluetooth_audio_active() || headphone_jack_state() == JACK_NONE) {
		gui_notify_popup("quickpanel_line_out_needs_the_jack");
		return;
	}
	confirm_show("quickpanel_line_out", tr("quickpanel_the_volume_will_go_to_100"), "quickpanel_activate", lineout_confirmed, NULL);
}

// ---------------------------------------------------------------------------
// MSEB: the stock player's MageSound tuning (see eq.c for the engine and the
// mapping); nine character sliders, -10..+10 around flat.
// ---------------------------------------------------------------------------

static lv_obj_t *mseb_screen;
static lv_obj_t *mseb_enable_switch;
static lv_obj_t *mseb_sliders[MSEB_BANDS];
static lv_obj_t *mseb_values[MSEB_BANDS];

static lv_obj_t *mseb_reset_btn;
static lv_obj_t *eq_reset_btn;

// A reset that would reset nothing is not offered: the button greys out with
// the page's own switch.
static void reset_button_enabled(lv_obj_t *btn, bool enabled) {
	if (!btn) {
		return;
	}
	if (enabled) {
		lv_obj_remove_state(btn, LV_STATE_DISABLED);
		lv_obj_set_clickable(btn, true);
		lv_obj_set_style_opa(btn, LV_OPA_COVER, 0);
	} else {
		lv_obj_add_state(btn, LV_STATE_DISABLED);
		lv_obj_set_clickable(btn, false);
		lv_obj_set_style_opa(btn, LV_OPA_40, 0);
	}
}

static void mseb_apply_sliders_enabled(bool enabled) {
	for (int i = 0; i < MSEB_BANDS; i++) {
		if (!mseb_sliders[i]) {
			continue;
		}
		if (enabled) {
			lv_obj_remove_state(mseb_sliders[i], LV_STATE_DISABLED);
		} else {
			lv_obj_add_state(mseb_sliders[i], LV_STATE_DISABLED);
		}
	}
}

static void mseb_enable_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(mseb_enable_switch, LV_STATE_CHECKED);
	mseb_set_enabled(on);
	mseb_apply_sliders_enabled(on);
	reset_button_enabled(mseb_reset_btn, on);
}

static void mseb_slider_cb(lv_event_t *e) {
	int band = (int)(intptr_t)lv_event_get_user_data(e);
	if (band < 0 || band >= MSEB_BANDS || !mseb_sliders[band]) {
		return;
	}
	int value = (int)lv_slider_get_value(mseb_sliders[band]);
	mseb_set_value(band, value);
	lv_label_set_text_fmt(mseb_values[band], "%+d", value);
}

static void mseb_released_cb(lv_event_t *e) {
	(void)e;
	mseb_save(); // one config write when the finger lifts, not per pixel
}

static void refresh_mseb_sliders(void) {
	for (int i = 0; i < MSEB_BANDS; i++) {
		theme_apply_slider_knob(mseb_sliders[i]);
		if (mseb_sliders[i]) {
			lv_obj_set_style_bg_color(mseb_sliders[i], theme()->accent, LV_PART_INDICATOR);
			lv_obj_set_style_bg_color(mseb_sliders[i], theme()->text_secondary, LV_PART_MAIN);
		}
	}
}

// ---------------------------------------------------------------------------
// Fade: the volume ramp at each end of a track (see audio.c).
// ---------------------------------------------------------------------------

static lv_obj_t *fade_screen;
static lv_obj_t *fade_switch;
static lv_obj_t *fade_card, *fade_value, *fade_slider;

// One second per step, one to ten.
#define FADE_MIN_SECONDS 1
#define FADE_MAX_SECONDS 10
#define FADE_STEPS (FADE_MAX_SECONDS - FADE_MIN_SECONDS + 1)

static int fade_seconds_setting(void) {
	int v = (int)config_get_int("audio", "fade_seconds", 3);
	if (v < FADE_MIN_SECONDS) {
		v = FADE_MIN_SECONDS;
	}
	if (v > FADE_MAX_SECONDS) {
		v = FADE_MAX_SECONDS;
	}
	return v;
}

static void fade_apply_setting(void) {
	audio_set_fade(config_get_bool("audio", "fade", false), fade_seconds_setting());
}

static void fade_refresh_labels(void) {
	lv_label_set_text_fmt(fade_value, "%d s", fade_seconds_setting());
	bool on = config_get_bool("audio", "fade", false);
	if (on) {
		lv_obj_add_state(fade_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(fade_switch, LV_STATE_CHECKED);
	}
	settingsrow_toggle_slider_expanded(fade_card, on);
}

static void fade_toggle_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("audio", "fade", lv_obj_has_state(fade_switch, LV_STATE_CHECKED));
	config_save();
	fade_apply_setting();
	fade_refresh_labels();
}

static void fade_slider_cb(lv_event_t *e) {
	(void)e;
	int seconds = FADE_MIN_SECONDS + (int)lv_slider_get_value(fade_slider);
	config_set_int("audio", "fade_seconds", seconds);
	config_save();
	fade_apply_setting();
	lv_label_set_text_fmt(fade_value, "%d s", seconds);
}

static void build_fade_page(gui_config_t *cfg) {
	fade_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(fade_screen, cfg, "fade");

	fade_card = settingsrow_toggle_slider(container, "fade", FADE_STEPS, &fade_switch, &fade_value,
										  &fade_slider, fade_toggle_cb, fade_slider_cb);
	lv_slider_set_value(fade_slider, fade_seconds_setting() - FADE_MIN_SECONDS, LV_ANIM_OFF);

	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr("musicsettings_fade_note"));

	fade_refresh_labels();
	switcher_attach_back_gesture(fade_screen);
}

// ---------------------------------------------------------------------------
// Soundfield: the stock player's Sound Field module (labelled "Campo sonoro"
// in its Italian strings), a stereo width control. The engine lives in eq.c;
// this is its page.
// ---------------------------------------------------------------------------

static lv_obj_t *soundfield_screen;
static lv_obj_t *soundfield_switch;
static lv_obj_t *soundfield_card, *soundfield_value, *soundfield_slider;

// 0.00 to 2.00 in steps of 0.05: exactly the range the original's own plugin
// declares for its "width" parameter.
#define SOUNDFIELD_STEPS ((SOUNDFIELD_WIDTH_MAX - SOUNDFIELD_WIDTH_MIN) / SOUNDFIELD_WIDTH_STEP + 1)

static void soundfield_set_value_label(int hundredths) {
	// Printed by hand rather than with %.2f: the whole module is integer
	// arithmetic and there is no reason to drag a float formatter in for two
	// decimal places.
	lv_label_set_text_fmt(soundfield_value, "%d.%02d", hundredths / 100, hundredths % 100);
}

static void soundfield_refresh(void) {
	bool on = soundfield_get_enabled();
	soundfield_set_value_label(soundfield_get_width());
	if (on) {
		lv_obj_add_state(soundfield_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(soundfield_switch, LV_STATE_CHECKED);
	}
	settingsrow_toggle_slider_expanded(soundfield_card, on);
}

static void soundfield_toggle_cb(lv_event_t *e) {
	(void)e;
	soundfield_set_enabled(lv_obj_has_state(soundfield_switch, LV_STATE_CHECKED));
	soundfield_refresh();
}

static void soundfield_slider_cb(lv_event_t *e) {
	(void)e;
	int hundredths = SOUNDFIELD_WIDTH_MIN + (int)lv_slider_get_value(soundfield_slider) * SOUNDFIELD_WIDTH_STEP;
	soundfield_set_width(hundredths);
	soundfield_set_value_label(soundfield_get_width());
}

static void build_soundfield_page(gui_config_t *cfg) {
	soundfield_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(soundfield_screen, cfg, "musicsettings_soundfield");

	soundfield_card = settingsrow_toggle_slider(container, "musicsettings_soundfield", SOUNDFIELD_STEPS, &soundfield_switch,
											   &soundfield_value, &soundfield_slider, soundfield_toggle_cb,
											   soundfield_slider_cb);
	lv_slider_set_value(soundfield_slider,
						(soundfield_get_width() - SOUNDFIELD_WIDTH_MIN) / SOUNDFIELD_WIDTH_STEP, LV_ANIM_OFF);

	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr("musicsettings_soundstage_note"));

	soundfield_refresh();
	switcher_attach_back_gesture(soundfield_screen);
}

// ---------------------------------------------------------------------------
// Crossfeed -- the engine is in eq.c, this is its page.
//
// Three numbers and a switch. The switch and the amount share the expanding
// card (like soundfield), the other two sit in two cards below: turning
// crossfeed on without touching anything else already gives a sensible tuning,
// and the detail is further down for whoever wants it.
// ---------------------------------------------------------------------------

static lv_obj_t *corner_button(lv_obj_t *screen, gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph,
							   lv_event_cb_t cb, void *user);

static lv_obj_t *crossfeed_screen;
static lv_obj_t *crossfeed_switch;
static lv_obj_t *crossfeed_card, *crossfeed_value, *crossfeed_slider;
static lv_obj_t *crossfeed_cut_value, *crossfeed_cut_slider;
static lv_obj_t *crossfeed_delay_value, *crossfeed_delay_slider;
static lv_obj_t *crossfeed_detail_card, *crossfeed_delay_card;

#define CROSSFEED_LEVEL_STEPS ((CROSSFEED_LEVEL_MAX - CROSSFEED_LEVEL_MIN) / CROSSFEED_LEVEL_STEP + 1)
#define CROSSFEED_CUTOFF_STEPS ((CROSSFEED_CUTOFF_MAX - CROSSFEED_CUTOFF_MIN) / CROSSFEED_CUTOFF_STEP + 1)
#define CROSSFEED_DELAY_STEPS ((CROSSFEED_DELAY_MAX - CROSSFEED_DELAY_MIN) / CROSSFEED_DELAY_STEP + 1)

static void crossfeed_refresh(void) {
	bool on = crossfeed_get_enabled();
	lv_label_set_text_fmt(crossfeed_value, "%d%%", crossfeed_get_level());
	lv_label_set_text_fmt(crossfeed_cut_value, "%d Hz", crossfeed_get_cutoff());
	lv_label_set_text_fmt(crossfeed_delay_value, "%d \xC2\xB5s", crossfeed_get_delay());
	if (on) {
		lv_obj_add_state(crossfeed_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(crossfeed_switch, LV_STATE_CHECKED);
	}
	settingsrow_toggle_slider_expanded(crossfeed_card, on);

	// The two detail cards disappear while crossfeed is off: they are numbers
	// that affect nothing, and leaving them there to be dragged is worse than
	// hiding them.
	if (on) {
		lv_obj_set_hidden(crossfeed_detail_card, false);
		lv_obj_set_hidden(crossfeed_delay_card, false);
	} else {
		lv_obj_set_hidden(crossfeed_detail_card, true);
		lv_obj_set_hidden(crossfeed_delay_card, true);
	}
}

static void crossfeed_toggle_cb(lv_event_t *e) {
	(void)e;
	crossfeed_set_enabled(lv_obj_has_state(crossfeed_switch, LV_STATE_CHECKED));
	crossfeed_refresh();
}

static void crossfeed_level_cb(lv_event_t *e) {
	(void)e;
	crossfeed_set_level(CROSSFEED_LEVEL_MIN + (int)lv_slider_get_value(crossfeed_slider) * CROSSFEED_LEVEL_STEP);
	lv_label_set_text_fmt(crossfeed_value, "%d%%", crossfeed_get_level());
}

static void crossfeed_cutoff_cb(lv_event_t *e) {
	(void)e;
	crossfeed_set_cutoff(CROSSFEED_CUTOFF_MIN + (int)lv_slider_get_value(crossfeed_cut_slider) * CROSSFEED_CUTOFF_STEP);
	lv_label_set_text_fmt(crossfeed_cut_value, "%d Hz", crossfeed_get_cutoff());
}

static void crossfeed_delay_cb(lv_event_t *e) {
	(void)e;
	crossfeed_set_delay(CROSSFEED_DELAY_MIN + (int)lv_slider_get_value(crossfeed_delay_slider) * CROSSFEED_DELAY_STEP);
	lv_label_set_text_fmt(crossfeed_delay_value, "%d \xC2\xB5s", crossfeed_get_delay());
}

static void crossfeed_reset_cb(lv_event_t *e) {
	(void)e;
	crossfeed_set_level(CROSSFEED_LEVEL_DEFAULT);
	crossfeed_set_cutoff(CROSSFEED_CUTOFF_DEFAULT);
	crossfeed_set_delay(CROSSFEED_DELAY_DEFAULT);
	lv_slider_set_value(crossfeed_slider, (CROSSFEED_LEVEL_DEFAULT - CROSSFEED_LEVEL_MIN) / CROSSFEED_LEVEL_STEP,
						LV_ANIM_ON);
	lv_slider_set_value(crossfeed_cut_slider, (CROSSFEED_CUTOFF_DEFAULT - CROSSFEED_CUTOFF_MIN) / CROSSFEED_CUTOFF_STEP,
						LV_ANIM_ON);
	lv_slider_set_value(crossfeed_delay_slider, (CROSSFEED_DELAY_DEFAULT - CROSSFEED_DELAY_MIN) / CROSSFEED_DELAY_STEP,
						LV_ANIM_ON);
	crossfeed_refresh();
}

static void build_crossfeed_page(gui_config_t *cfg) {
	crossfeed_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(crossfeed_screen, cfg, "musicsettings_crossfeed");

	settingsrow_title_corner_slots(settingsrow_page_title(crossfeed_screen), cfg, 1);
	corner_button(crossfeed_screen, cfg, 0, &icon_reset, crossfeed_reset_cb, NULL);

	crossfeed_card =
		settingsrow_toggle_slider(container, "musicsettings_amount", CROSSFEED_LEVEL_STEPS, &crossfeed_switch,
								  &crossfeed_value, &crossfeed_slider, crossfeed_toggle_cb, crossfeed_level_cb);
	lv_slider_set_value(crossfeed_slider, (crossfeed_get_level() - CROSSFEED_LEVEL_MIN) / CROSSFEED_LEVEL_STEP,
						LV_ANIM_OFF);

	crossfeed_detail_card = settingsrow_slider(container, "musicsettings_cutoff_frequency", CROSSFEED_CUTOFF_STEPS,
											   &crossfeed_cut_value, &crossfeed_cut_slider, crossfeed_cutoff_cb);
	lv_slider_set_value(crossfeed_cut_slider, (crossfeed_get_cutoff() - CROSSFEED_CUTOFF_MIN) / CROSSFEED_CUTOFF_STEP,
						LV_ANIM_OFF);

	crossfeed_delay_card = settingsrow_slider(container, "musicsettings_delay", CROSSFEED_DELAY_STEPS, &crossfeed_delay_value,
											  &crossfeed_delay_slider, crossfeed_delay_cb);
	lv_slider_set_value(crossfeed_delay_slider, (crossfeed_get_delay() - CROSSFEED_DELAY_MIN) / CROSSFEED_DELAY_STEP,
						LV_ANIM_OFF);

	crossfeed_refresh();
	switcher_attach_back_gesture(crossfeed_screen);
}

// ---------------------------------------------------------------------------
// Channel balance
//
// The stock player's own module and its own numbers -- its parameter panel is
// declared in the binary as a slider from -20 to +20 dB in half-decibel steps
// with an Enable checkbox beside it, which is exactly this page. See eq.h for
// why it only ever attenuates.
// ---------------------------------------------------------------------------

static lv_obj_t *balance_screen;
static lv_obj_t *balance_switch;
static lv_obj_t *balance_card, *balance_value, *balance_slider;

#define BALANCE_STEPS ((BALANCE_MAX - BALANCE_MIN) / BALANCE_STEP + 1)

static void balance_set_value_label(int tenths) {
	// Printed by hand for the same reason the soundfield's is: the module is
	// integers all the way down and there is no float formatter to drag in.
	int magnitude = tenths < 0 ? -tenths : tenths;
	const char *side = tenths == 0 ? tr("centre") : (tenths < 0 ? tr("left") : tr("right"));
	if (tenths == 0) {
		lv_label_set_text(balance_value, side);
	} else {
		lv_label_set_text_fmt(balance_value, "%s %d.%d dB", side, magnitude / 10, magnitude % 10);
	}
}

static void balance_refresh(void) {
	bool on = balance_get_enabled();
	balance_set_value_label(balance_get_value());
	if (on) {
		lv_obj_add_state(balance_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(balance_switch, LV_STATE_CHECKED);
	}
	settingsrow_toggle_slider_expanded(balance_card, on);
}

static void balance_toggle_cb(lv_event_t *e) {
	(void)e;
	balance_set_enabled(lv_obj_has_state(balance_switch, LV_STATE_CHECKED));
	balance_refresh();
}

static void balance_slider_cb(lv_event_t *e) {
	(void)e;
	int tenths = BALANCE_MIN + (int)lv_slider_get_value(balance_slider) * BALANCE_STEP;
	balance_set_value(tenths);
	balance_set_value_label(balance_get_value());
}

static void build_balance_page(gui_config_t *cfg) {
	balance_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(balance_screen, cfg, "musicsettings_channel_balance");

	balance_card = settingsrow_toggle_slider(container, "musicsettings_balance", BALANCE_STEPS, &balance_switch,
											 &balance_value, &balance_slider, balance_toggle_cb, balance_slider_cb);
	lv_slider_set_value(balance_slider, (balance_get_value() - BALANCE_MIN) / BALANCE_STEP, LV_ANIM_OFF);

	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr("musicsettings_balance_note"));

	balance_refresh();
	switcher_attach_back_gesture(balance_screen);
}

// ---------------------------------------------------------------------------
// Volume limit
//
// A switch, and while it is on one ceiling per output: the level cannot be
// raised past it from anywhere (see alsa-controls.h). One each because the two
// sockets and USB-C are three different things to listen through, and keep
// three levels for the same reason. The R1 has no 4.4 mm socket, and so no
// slider for it.
// ---------------------------------------------------------------------------

static void refresh_active_chevrons(void);

static lv_obj_t *vlimit_screen;
static lv_obj_t *vlimit_switch;
#define VLIMIT_OUTPUT_COUNT 3
static lv_obj_t *vlimit_cards[VLIMIT_OUTPUT_COUNT], *vlimit_values[VLIMIT_OUTPUT_COUNT],
	*vlimit_sliders[VLIMIT_OUTPUT_COUNT];
static const volume_output_t VLIMIT_OUTPUTS[VLIMIT_OUTPUT_COUNT] = {VOLUME_OUTPUT_PHONES, VOLUME_OUTPUT_BALANCED,
																	 VOLUME_OUTPUT_USB};

#define VLIMIT_STEPS (100 - VOLUME_LIMIT_MIN + 1)

static bool vlimit_has_balanced(void) {
	const sysinfo_model_t *model = sysinfo_model();
	return !model || !model->cs43131;
}

static void vlimit_refresh(void) {
	bool on = volume_limit_enabled();
	if (on) {
		lv_obj_add_state(vlimit_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(vlimit_switch, LV_STATE_CHECKED);
	}
	for (int i = 0; i < VLIMIT_OUTPUT_COUNT; i++) {
		int limit = volume_limit(VLIMIT_OUTPUTS[i]);
		lv_label_set_text_fmt(vlimit_values[i], "%d", limit);
		lv_slider_set_value(vlimit_sliders[i], limit - VOLUME_LIMIT_MIN, LV_ANIM_OFF);
		bool absent = VLIMIT_OUTPUTS[i] == VOLUME_OUTPUT_BALANCED && !vlimit_has_balanced();
		lv_obj_set_hidden(vlimit_cards[i], !on || absent);
	}
}

// The level may just have come down under a new ceiling, and the number in
// the status bar is the one place on screen that says what it is.
static void vlimit_level_moved(void) { topbar_refresh_volume(get_volume_percent()); }

static void vlimit_toggle_cb(lv_event_t *e) {
	(void)e;
	volume_limit_set_enabled(lv_obj_has_state(vlimit_switch, LV_STATE_CHECKED));
	vlimit_refresh();
	vlimit_level_moved();
	refresh_active_chevrons();
}

static void vlimit_slider_cb(lv_event_t *e) {
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	int limit = VOLUME_LIMIT_MIN + (int)lv_slider_get_value(vlimit_sliders[i]);
	volume_limit_set(VLIMIT_OUTPUTS[i], limit);
	lv_label_set_text_fmt(vlimit_values[i], "%d", volume_limit(VLIMIT_OUTPUTS[i]));
	vlimit_level_moved();
}

static void vlimit_released_cb(lv_event_t *e) {
	(void)e;
	config_save();
}

static void vlimit_loaded_cb(lv_event_t *e) {
	(void)e;
	vlimit_refresh();
}

static void build_volume_limit_page(gui_config_t *cfg) {
	vlimit_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(vlimit_screen, cfg, "musicsettings_volume_limit");

	settingsrow_toggle(container, "on", &vlimit_switch, vlimit_toggle_cb);

	static const char *const NAMES[VLIMIT_OUTPUT_COUNT] = {"musicsettings_volume_limit_phones",
														   "musicsettings_volume_limit_balanced",
														   "musicsettings_volume_limit_usb"};
	for (int i = 0; i < VLIMIT_OUTPUT_COUNT; i++) {
		vlimit_cards[i] = settingsrow_slider(container, NAMES[i], VLIMIT_STEPS, &vlimit_values[i], &vlimit_sliders[i],
											 NULL);
		lv_obj_add_event_cb(vlimit_sliders[i], vlimit_slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
		lv_obj_add_event_cb(vlimit_sliders[i], vlimit_released_cb, LV_EVENT_RELEASED, NULL);
	}

	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr("musicsettings_volume_limit_note"));

	vlimit_refresh();
	lv_obj_add_event_cb(vlimit_screen, vlimit_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(vlimit_screen);
}

// ---------------------------------------------------------------------------
// Replay gain
// ---------------------------------------------------------------------------

static lv_obj_t *rg_switch;
static lv_obj_t *rg_card, *rg_pills;
static lv_obj_t *rg_track_pill, *rg_album_pill, *rg_track_when_shuffled_pill;

static void rg_refresh(void) {
	if (!rg_switch) {
		return;
	}
	replaygain_mode_t mode = replaygain_mode();
	bool on = mode != REPLAYGAIN_OFF;

	if (on) {
		lv_obj_add_state(rg_switch, LV_STATE_CHECKED);
		lv_obj_set_hidden(rg_pills, false);
	} else {
		lv_obj_remove_state(rg_switch, LV_STATE_CHECKED);
		lv_obj_set_hidden(rg_pills, true);
	}
	settingsrow_pill_active(rg_track_pill, mode == REPLAYGAIN_TRACK);
	settingsrow_pill_active(rg_album_pill, mode == REPLAYGAIN_ALBUM);
	settingsrow_pill_active(rg_track_when_shuffled_pill, mode == REPLAYGAIN_TRACK_WHEN_SHUFFLED);
}

// Whatever the setting becomes, the track already loaded has to be re-measured
// against it -- otherwise switching this on does nothing until the next song.
static void rg_apply_to_loaded_track(void) {
	device_state_t state;
	device_state_get(&state);
	replaygain_load(&state.metadata);
}

static void rg_toggle_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(rg_switch, LV_STATE_CHECKED);
	// Switching it back on returns to whichever of the two modes was last
	// chosen, rather than always to track gain.
	replaygain_set_mode(on ? (replaygain_mode_t)config_get_int("audio", "replaygain_last", REPLAYGAIN_TRACK)
						   : REPLAYGAIN_OFF);
	rg_apply_to_loaded_track();
	rg_refresh();
}

static void rg_pick_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	replaygain_mode_t mode = (replaygain_mode_t)(intptr_t)lv_event_get_user_data(e);
	replaygain_set_mode(mode);
	config_set_int("audio", "replaygain_last", (long)mode);
	config_save();
	rg_apply_to_loaded_track();
	rg_refresh();
}

// ---------------------------------------------------------------------------
// DSD gain compensation
//
// What the number means is in alsa-controls.h. Here it is one switch and the
// six compensations: off is 0 dB, so there is no 0 dB pill -- it would be a
// second way to say what the switch already says, and a state where the switch
// reads on and nothing is being compensated.
// ---------------------------------------------------------------------------

// The pills are +1 dB upwards; pill i carries index i + 1.
static lv_obj_t *dsd_gain_switch;
static lv_obj_t *dsd_gain_pills;
static lv_obj_t *dsd_gain_pill[DSD_GAIN_MAX_DB];

static void dsd_gain_refresh(void) {
	if (!dsd_gain_switch) {
		return;
	}
	int index = alsa_dsd_gain_index();
	bool on = index > 0;

	if (on) {
		lv_obj_add_state(dsd_gain_switch, LV_STATE_CHECKED);
		lv_obj_set_hidden(dsd_gain_pills, false);
	} else {
		lv_obj_remove_state(dsd_gain_switch, LV_STATE_CHECKED);
		lv_obj_set_hidden(dsd_gain_pills, true);
	}
	for (int i = 0; i < DSD_GAIN_MAX_DB; i++) {
		settingsrow_pill_active(dsd_gain_pill[i], i + 1 == index);
	}
}

static void dsd_gain_store(int index) {
	alsa_set_dsd_gain_index(index);
	config_set_int("audio", "dsd_gain", index);
	if (index > 0) {
		config_set_int("audio", "dsd_gain_last", index);
	}
	config_save();
	dsd_gain_refresh();
}

static void dsd_gain_toggle_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(dsd_gain_switch, LV_STATE_CHECKED);
	// Switching it on goes back to the last compensation chosen. The first time,
	// that is the whole 6 dB: this exists because DSD comes out quieter than the
	// stock player, and the largest step is the one that answers it.
	dsd_gain_store(on ? (int)config_get_int("audio", "dsd_gain_last", DSD_GAIN_MAX_DB) : 0);
}

static void dsd_gain_pick_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	dsd_gain_store((int)(intptr_t)lv_event_get_user_data(e));
}

// DSD over a USB DAC ([usb] dsd_mode, read by usbaudio_dsd_as_pcm when a track starts).
static lv_obj_t *usb_dsd_pill[4];

static void usb_dsd_refresh(void) {
	long mode = config_get_int("usb", "dsd_mode", USB_DSD_AUTO);
	for (int i = 0; i < 4; i++) {
		settingsrow_pill_active(usb_dsd_pill[i], i == mode);
	}
}

static void usb_dsd_pick_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	config_set_int("usb", "dsd_mode", (int)(intptr_t)lv_event_get_user_data(e));
	usb_dsd_refresh();
}

// The round corner buttons on the MSEB and equalizer pages, the same shape the
// music page uses for its gear and its search.
static lv_obj_t *corner_button(lv_obj_t *screen, gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph,
							   lv_event_cb_t cb, void *user) {
	lv_obj_t *button = lv_btn_create(screen);
	lv_obj_set_size(button, ui_px(56), ui_px(56));
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (ui_px(56) + ui_px(6)), cfg->padding + cfg->top_bar_height);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);

	return button;
}

// --- the two "back to flat" buttons ---------------------------------------

static void refresh_mseb_sliders_values(void);

static void mseb_reset_action(void *user) {
	(void)user;
	mseb_reset();
	refresh_mseb_sliders_values();
}

static void mseb_reset_cb(lv_event_t *e) {
	(void)e;
	confirm_show("musicsettings_reset_mseb", "musicsettings_mseb_reset_confirm_note", "reset", mseb_reset_action, NULL);
}

static void refresh_eq_sliders_values(void);

static void eq_reset_action(void *user) {
	(void)user;
	eq_reset();
	refresh_eq_sliders_values();
}

static void eq_reset_cb(lv_event_t *e) {
	(void)e;
	confirm_show("musicsettings_reset_the_equaliser", "musicsettings_eq_reset_confirm_note", "reset", eq_reset_action, NULL);
}

static void mseb_open_settings_cb(lv_event_t *e) {
	(void)e;
	switch_screen(msebsettings_screen());
}

static void eq_open_settings_cb(lv_event_t *e) {
	(void)e;
	switch_screen(eqsettings_screen());
}

// Puts every MSEB slider where the engine's values are -- after a reset, a
// preset, or a change of range (whose ends have just moved).
static void refresh_mseb_sliders_values(void) {
	int range = mseb_get_range();
	for (int i = 0; i < MSEB_BANDS; i++) {
		if (!mseb_sliders[i]) {
			continue;
		}
		lv_slider_set_range(mseb_sliders[i], -range, range);
		lv_slider_set_value(mseb_sliders[i], mseb_get_value(i), LV_ANIM_OFF);
		lv_label_set_text_fmt(mseb_values[i], "%+d", mseb_get_value(i));
	}
}

static void build_mseb_page(gui_config_t *cfg) {
	mseb_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(mseb_screen, cfg, "mseb");

	settingsrow_toggle(container, "on", &mseb_enable_switch, mseb_enable_cb);
	if (mseb_get_enabled()) {
		lv_obj_add_state(mseb_enable_switch, LV_STATE_CHECKED);
	}

	for (int i = 0; i < MSEB_BANDS; i++) {
		lv_obj_t *card = lv_obj_create(container);
		lv_obj_set_width(card, lv_pct(100));
		lv_obj_set_height(card, ui_px(104));
		lv_obj_add_style(card, &theme_style_card, 0);
		lv_obj_set_style_radius(card, ui_px(12), 0);
		lv_obj_set_style_border_width(card, 0, 0);
		lv_obj_set_style_shadow_width(card, 0, 0);
		lv_obj_set_style_pad_hor(card, ui_px(20), 0);
		lv_obj_set_style_pad_ver(card, ui_px(12), 0);
		lv_obj_set_scrollable(card, false);
		lv_obj_set_event_bubble(card, true);

		lv_obj_t *name = lv_label_create(card);
		lv_label_set_text(name, tr(mseb_band_name[i]));
		lv_obj_add_style(name, &theme_style_text, 0);
		lv_obj_set_style_text_font(name, &font_ui_22, 0);
		lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);

		mseb_values[i] = lv_label_create(card);
		lv_label_set_text_fmt(mseb_values[i], "%+d", mseb_get_value(i));
		lv_obj_add_style(mseb_values[i], &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(mseb_values[i], &font_ui_22, 0);
		lv_obj_align(mseb_values[i], LV_ALIGN_TOP_RIGHT, 0, 0);

		lv_obj_t *slider = lv_slider_create(card);
		lv_obj_set_width(slider, lv_pct(100));
		lv_obj_set_height(slider, ui_px(10));
		lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -ui_px(10));
		lv_slider_set_range(slider, -mseb_get_range(), mseb_get_range());
		// MSEB sliders are bipolar: zero sits in the middle and the coloured
		// bar grows from there, forwards for positives and backwards for
		// negatives. Without symmetrical mode LVGL fills from the left, so "-3"
		// would read as "almost nothing" rather than "a step below".
		lv_slider_set_mode(slider, LV_SLIDER_MODE_SYMMETRICAL);
		lv_slider_set_value(slider, mseb_get_value(i), LV_ANIM_OFF);

		lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
		lv_obj_set_style_bg_color(slider, theme()->text_secondary, LV_PART_MAIN);
		lv_obj_set_style_bg_opa(slider, LV_OPA_40, LV_PART_MAIN);
		lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(slider, theme()->accent, LV_PART_INDICATOR);
		lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
		lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
		theme_apply_slider_knob(slider);
		lv_obj_set_style_pad_all(slider, ui_px(7), LV_PART_KNOB);
		lv_obj_set_style_shadow_width(slider, ui_px(6), LV_PART_KNOB);
		lv_obj_set_style_shadow_opa(slider, LV_OPA_30, LV_PART_KNOB);
		lv_obj_set_style_shadow_color(slider, lv_color_black(), LV_PART_KNOB);
		// Drag the knob, do not tap the track. A plain LVGL slider jumps its
		// value to wherever the finger lands, so brushing a row on the way past
		// would move a band. With ADV_HITTEST the slider only answers a press
		// that begins on the knob; anywhere else on the track the press goes to
		// the card behind it and scrolls the page.
		lv_obj_set_adv_hittest(slider, true);
		// The knob is 24 px across, and with ADV_HITTEST the extended click area
		// belongs to the knob alone rather than padding the whole track.
		lv_obj_set_ext_click_area(slider, ui_px(18));
		lv_obj_add_event_cb(slider, mseb_slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
		lv_obj_add_event_cb(slider, mseb_released_cb, LV_EVENT_RELEASED, NULL);

		mseb_sliders[i] = slider;
	}

	for (int i = 0; i < MSEB_BANDS; i++) {
		lv_obj_set_style_opa(mseb_sliders[i], LV_OPA_40, LV_STATE_DISABLED);
	}
	mseb_apply_sliders_enabled(mseb_get_enabled());

	// The gear and the reset, in the corner: the same pair of buttons the music
	// page wears.
	corner_button(mseb_screen, cfg, 0, &icon_music_settings, mseb_open_settings_cb, NULL);
	mseb_reset_btn = corner_button(mseb_screen, cfg, 1, &icon_reset, mseb_reset_cb, NULL);
	reset_button_enabled(mseb_reset_btn, mseb_get_enabled());
	msebsettings_set_reload_cb(refresh_mseb_sliders_values);

	switcher_attach_back_gesture(mseb_screen);
	theme_register_refresh(refresh_mseb_sliders);
}

// Remember track / remember volume: what survives a power cycle.
static lv_obj_t *remember_track_switch;
static lv_obj_t *remember_volume_switch;

static void remember_track_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(remember_track_switch, LV_STATE_CHECKED);
	config_set_int("player", "remember_track", on ? 1 : 0);
	config_save();
}

static void remember_volume_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(remember_volume_switch, LV_STATE_CHECKED);
	config_set_int("player", "remember_volume", on ? 1 : 0);
	if (on) {
		// Every output's own level, each into its own slot, as in the status bar
		// poll: what is remembered is where each socket was left, not the one
		// number the player happens to be at.
		//
		// The profiles' levels and not the one the hardware is at: in line out
		// the output sits at the fixed index, which is a mode and not a level
		// the user chose to come back to.
		volume_profile_persist_now();
	}
	config_save();
}

// ---------------------------------------------------------------------------
// Equaliser: the 10-band graphic EQ (src/system/eq.c), one vertical slider per
// octave band, +/-12 dB.
// ---------------------------------------------------------------------------

static lv_obj_t *eq_screen;
static lv_obj_t *eq_enable_switch;
static lv_obj_t *eq_sliders[EQ_BANDS];

// Puts every equalizer slider where the engine's gains are, after a reset.
static void refresh_eq_sliders_values(void) {
	for (int i = 0; i < EQ_BANDS; i++) {
		if (eq_sliders[i]) {
			lv_slider_set_value(eq_sliders[i], eq_get_band(i), LV_ANIM_OFF);
		}
	}
}

static void eq_apply_sliders_enabled(bool enabled) {
	for (int i = 0; i < EQ_BANDS; i++) {
		if (!eq_sliders[i]) {
			continue;
		}
		if (enabled) {
			lv_obj_remove_state(eq_sliders[i], LV_STATE_DISABLED);
		} else {
			lv_obj_add_state(eq_sliders[i], LV_STATE_DISABLED);
		}
	}
}

static void eq_enable_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(eq_enable_switch, LV_STATE_CHECKED);
	eq_set_enabled(on);
	eq_apply_sliders_enabled(on);
	reset_button_enabled(eq_reset_btn, on);
}

static void eq_slider_cb(lv_event_t *e) {
	int band = (int)(intptr_t)lv_event_get_user_data(e);
	if (band >= 0 && band < EQ_BANDS && eq_sliders[band]) {
		eq_set_band(band, (int)lv_slider_get_value(eq_sliders[band]));
	}
}

// "31" ... "16k": the label under each slider.
static void eq_freq_text(int freq, char *out, size_t size) {
	freq &= 0xffff; // the table tops out at 16000; quiets -Wformat-truncation
	if (freq >= 1000) {
		snprintf(out, size, "%dk", freq / 1000);
	} else {
		snprintf(out, size, "%d", freq);
	}
}

// Re-paint the sliders' fills after a theme/accent change.
static void refresh_eq_sliders(void) {
	for (int i = 0; i < EQ_BANDS; i++) {
		theme_apply_slider_knob(eq_sliders[i]);
		if (eq_sliders[i]) {
			lv_obj_set_style_bg_color(eq_sliders[i], theme()->accent, LV_PART_INDICATOR);
			lv_obj_set_style_bg_color(eq_sliders[i], theme()->text_secondary, LV_PART_MAIN);
		}
	}
}

static void build_eq_page(gui_config_t *cfg) {
	eq_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(eq_screen, cfg, "equaliser");

	settingsrow_toggle(container, "on", &eq_enable_switch, eq_enable_cb);
	if (eq_get_enabled()) {
		lv_obj_add_state(eq_enable_switch, LV_STATE_CHECKED);
	}

	// The band card: ten vertical sliders side by side, frequency underneath.
	lv_obj_t *card = lv_obj_create(container);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, ui_px(360));
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, ui_px(12), 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, ui_px(14), 0);
	lv_obj_set_style_pad_gap(card, 0, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	for (int i = 0; i < EQ_BANDS; i++) {
		lv_obj_t *column = lv_obj_create(card);
		lv_obj_set_size(column, ui_px(38), lv_pct(100));
		lv_obj_set_style_bg_opa(column, 0, 0);
		lv_obj_set_style_border_width(column, 0, 0);
		lv_obj_set_style_pad_all(column, 0, 0);
		lv_obj_set_style_pad_gap(column, ui_px(8), 0);
		lv_obj_set_scrollable(column, false);
		lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		lv_obj_t *slider = lv_slider_create(column);
		lv_obj_set_size(slider, ui_px(6), lv_pct(80));
		lv_slider_set_range(slider, EQ_GAIN_MIN_DB, EQ_GAIN_MAX_DB);
		lv_slider_set_value(slider, eq_get_band(i), LV_ANIM_OFF);

		lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
		lv_obj_set_style_bg_color(slider, theme()->text_secondary, LV_PART_MAIN);
		lv_obj_set_style_bg_opa(slider, LV_OPA_40, LV_PART_MAIN);
		lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(slider, theme()->accent, LV_PART_INDICATOR);
		lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
		lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
		theme_apply_slider_knob(slider);
		lv_obj_set_style_pad_all(slider, ui_px(7), LV_PART_KNOB);
		lv_obj_set_style_shadow_width(slider, ui_px(6), LV_PART_KNOB);
		lv_obj_set_style_shadow_opa(slider, LV_OPA_30, LV_PART_KNOB);
		lv_obj_set_style_shadow_color(slider, lv_color_black(), LV_PART_KNOB);
		// A 6 px trough is no touch target; each column's full width is.
		lv_obj_set_ext_click_area(slider, ui_px(16));
		lv_obj_add_event_cb(slider, eq_slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);

		eq_sliders[i] = slider;

		char text[8];
		eq_freq_text(eq_band_freq[i], text, sizeof(text));
		lv_obj_t *label = lv_label_create(column);
		lv_label_set_text(label, text);
		lv_obj_add_style(label, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(label, &font_ui_18, 0);
	}

	// A dead slider looks dead: half opacity while the EQ is off.
	for (int i = 0; i < EQ_BANDS; i++) {
		lv_obj_set_style_opa(eq_sliders[i], LV_OPA_40, LV_STATE_DISABLED);
	}
	eq_apply_sliders_enabled(eq_get_enabled());

	// The gear and the reset, in the corner, like the MSEB page.
	corner_button(eq_screen, cfg, 0, &icon_music_settings, eq_open_settings_cb, NULL);
	eq_reset_btn = corner_button(eq_screen, cfg, 1, &icon_reset, eq_reset_cb, NULL);
	reset_button_enabled(eq_reset_btn, eq_get_enabled());
	eqsettings_set_reload_cb(refresh_eq_sliders_values);

	// What the scale means, in one dim line under the sliders.
	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_label_set_text(note, tr("musicsettings_eq_note"));

	switcher_attach_back_gesture(eq_screen);
	theme_register_refresh(refresh_eq_sliders);
}

// ---------------------------------------------------------------------------
// What the control centre's quick buttons reach in here: the two pages, and
// the two switches, so a toggle from outside leaves this page consistent.
// ---------------------------------------------------------------------------

lv_obj_t *musicsettings_eq_screen(void) { return eq_screen; }
lv_obj_t *musicsettings_mseb_screen(void) { return mseb_screen; }

lv_obj_t *musicsettings_fade_screen(void) { return fade_screen; }

static lv_obj_t *playback_screen;
lv_obj_t *musicsettings_playback_screen(void) { return playback_screen; }

bool musicsettings_fade_enabled(void) { return config_get_bool("audio", "fade", false); }

bool musicsettings_endless_shuffle(void) { return config_get_bool("player", "endless_shuffle", false); }

bool musicsettings_open_player_on_tap(void) { return config_get_bool("player", "open_player_on_tap", true); }

// The setters below are also called by the control centre, which can sit open
// on top of this page, so the chevrons have to be repainted from here.
static void refresh_active_chevrons(void);

void musicsettings_set_fade_enabled(bool enabled) {
	config_set_bool("audio", "fade", enabled);
	config_save();
	fade_apply_setting();
	if (fade_switch) {
		fade_refresh_labels(); // the page's own switch and its slider follow
	}
	refresh_active_chevrons();
}

bool musicsettings_high_gain(void) { return stored_gain_level() > 0; }

void musicsettings_set_high_gain(bool enabled) { gain_level_store(enabled ? gain_level_count() - 1 : 0); }

void musicsettings_gain_step(void) { gain_level_store((stored_gain_level() + 1) % gain_level_count()); }

int musicsettings_gain_level(void) { return stored_gain_level(); }

void musicsettings_set_eq_enabled(bool enabled) {
	eq_set_enabled(enabled);
	if (eq_enable_switch) {
		if (enabled) {
			lv_obj_add_state(eq_enable_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(eq_enable_switch, LV_STATE_CHECKED);
		}
	}
	eq_apply_sliders_enabled(enabled);
	reset_button_enabled(eq_reset_btn, enabled);
	refresh_active_chevrons();
}

void musicsettings_set_mseb_enabled(bool enabled) {
	mseb_set_enabled(enabled);
	if (mseb_enable_switch) {
		if (enabled) {
			lv_obj_add_state(mseb_enable_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(mseb_enable_switch, LV_STATE_CHECKED);
		}
	}
	mseb_apply_sliders_enabled(enabled);
	reset_button_enabled(mseb_reset_btn, enabled);
	refresh_active_chevrons();
}

// The rows whose chevron says whether what lies behind it is on. Every row
// that can be switched off, not a selection of them: a green chevron on MSEB
// and a grey one on an enabled fade is not a nuance, it is a lie.
static lv_obj_t *eq_row, *peq_row, *mseb_row, *soundfield_row, *crossfeed_row, *fade_row, *balance_row, *vlimit_row;

static void refresh_active_chevrons(void) {
	// With Direct on none of the effects is acting, whatever its switch says:
	// the chevrons say what the sound is going through.
	bool through = !direct_get_enabled();
	settingsrow_chevron_active(eq_row, through && eq_get_enabled());
	settingsrow_chevron_active(peq_row, through && peq_get_enabled());
	settingsrow_chevron_active(mseb_row, through && mseb_get_enabled());
	settingsrow_chevron_active(soundfield_row, through && soundfield_get_enabled());
	settingsrow_chevron_active(crossfeed_row, through && crossfeed_get_enabled());
	settingsrow_chevron_active(fade_row, musicsettings_fade_enabled());
	settingsrow_chevron_active(balance_row, through && balance_get_enabled());
	settingsrow_chevron_active(vlimit_row, volume_limit_enabled());
}

// Direct (eq.h), above the effects it bypasses.
static lv_obj_t *direct_switch;

static void direct_refresh(void) {
	if (direct_switch) {
		lv_obj_set_state(direct_switch, LV_STATE_CHECKED, direct_get_enabled());
	}
}

static void direct_toggle_cb(lv_event_t *e) {
	(void)e;
	direct_set_enabled(lv_obj_has_state(direct_switch, LV_STATE_CHECKED));
	refresh_active_chevrons();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_filter_checks();
	// Back from either page: the chevron follows what was left switched on.
	refresh_active_chevrons();
	lineout_refresh();
	direct_refresh();
}

// The row asks which folders first; the scan page opens only from there,
// which is why it carries no chevron.
static void scan_row_cb(lv_event_t *e) {
	(void)e;
	libraryscan_choose_folders();
}

static void album_chain_cb(lv_event_t *e) {
	device_state_set_album_chaining(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

// ---------------------------------------------------------------------------
// Playback options
//
// What playback does around a track rather than to it: which track comes next
// when the queue runs out, what survives the power switch, and whether the
// silence between two tracks is real. A page of its own rather than toggles on
// the music page among the DAC's own settings: nothing here touches the sound.
// ---------------------------------------------------------------------------

static lv_obj_t *folder_chain_switch;
static lv_obj_t *scan_screen;
static lv_obj_t *keep_articles_switch;
static lv_obj_t *detect_changes_switch;
static lv_obj_t *nowplaying_screen;
static lv_obj_t *allow_delete_switch;
static lv_obj_t *library_screen;
static lv_obj_t *album_view_switch;
static lv_obj_t *album_head_switch;
static lv_obj_t *quality_badges_switch;
static lv_obj_t *go_to_current_switch;

static void folder_chain_cb(lv_event_t *e) {
	device_state_set_folder_chaining(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

// The dim paragraph under a toggle that needs one.
static lv_obj_t *option_note(lv_obj_t *parent, const char *text) {
	lv_obj_t *note = lv_label_create(parent);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr(text));
	return note;
}

// ---------------------------------------------------------------------------
// Scan options
//
// The scan itself, and the one setting that only means anything to a scan: how
// a name is filed. It is a page of its own because the answer to "why did
// nothing change?" lives here -- the article switch is written into the index
// while it is being built, so it is read at the same moment the scan is
// started, in the same place.
// ---------------------------------------------------------------------------

static void keep_articles_cb(lv_event_t *e) {
	bool keep = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	config_set_int("player", "skip_articles", keep ? 0 : 1);
	config_save();
	// Straight away, so a scan started from the row above it uses the answer
	// the switch is showing.
	library_set_skip_articles(!keep);
}

// The retag check is part of Detect changes, and shown only while that is on.
static lv_obj_t *retagged_row, *retagged_note;

static void retagged_show(void) {
	bool on = library_detect_changes();
	lv_obj_set_hidden(retagged_row, !on);
	lv_obj_set_hidden(retagged_note, !on);
}

// Detect changes can be turned on behind the page's back, by the first scan
// (libraryscan.c), so the switch is read again each time the page comes up.
static void scan_page_loading_cb(lv_event_t *e) {
	(void)e;
	lv_obj_set_state(detect_changes_switch, LV_STATE_CHECKED, library_detect_changes());
	retagged_show();
}

static void detect_changes_cb(lv_event_t *e) {
	library_set_detect_changes(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
	retagged_show();
}

static void detect_retagged_cb(lv_event_t *e) {
	library_set_detect_retagged(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

// How tracks are filed: artists and genres split, albums joined. The index is
// filed again once, when the page is left, however many of these were touched
// on the way (library_reorganize).
static bool organize_changed;

static void organize_touched(void) { organize_changed = true; }

static lv_obj_t *split_artists_switch, *split_artists_pills;
static lv_obj_t *split_genres_switch, *split_genres_pills;
static lv_obj_t *unsplit_row;
static lv_obj_t *split_album_artists_row, *split_album_artists_switch;
static lv_obj_t *join_albums_switch;

typedef struct {
	const char *text;
	unsigned bit;
} split_pill_t;

static const split_pill_t SPLIT_ARTIST_PILLS[] = {
	{";", LIBRARY_SPLIT_SEMICOLON},
	{"/", LIBRARY_SPLIT_SLASH},
	{"&", LIBRARY_SPLIT_AMPERSAND},
	{",", LIBRARY_SPLIT_COMMA},
	{"feat.", LIBRARY_SPLIT_FEAT},
	{"vs.", LIBRARY_SPLIT_VS},
	{"x", LIBRARY_SPLIT_X},
	{"+", LIBRARY_SPLIT_PLUS},
	{"with", LIBRARY_SPLIT_WITH},
	{"and", LIBRARY_SPLIT_AND},
};
static const split_pill_t SPLIT_GENRE_PILLS[] = {
	{";", LIBRARY_SPLIT_SEMICOLON},
	{"/", LIBRARY_SPLIT_SLASH},
	{",", LIBRARY_SPLIT_COMMA},
};
#define SPLIT_ARTIST_COUNT (int)(sizeof(SPLIT_ARTIST_PILLS) / sizeof(SPLIT_ARTIST_PILLS[0]))
#define SPLIT_GENRE_COUNT (int)(sizeof(SPLIT_GENRE_PILLS) / sizeof(SPLIT_GENRE_PILLS[0]))
static lv_obj_t *split_artist_pill[SPLIT_ARTIST_COUNT];
static lv_obj_t *split_genre_pill[SPLIT_GENRE_COUNT];

static void organize_refresh(void) {
	bool artists = library_split_artists();
	bool genres = library_split_genres();
	unsigned a = library_artist_separators();
	unsigned g = library_genre_separators();

	lv_obj_set_state(split_artists_switch, LV_STATE_CHECKED, artists);
	lv_obj_set_hidden(split_artists_pills, !artists);
	lv_obj_set_hidden(unsplit_row, !artists);
	lv_obj_set_hidden(split_album_artists_row, !artists);
	lv_obj_set_state(split_album_artists_switch, LV_STATE_CHECKED, library_split_album_artists());
	for (int i = 0; i < SPLIT_ARTIST_COUNT; i++) {
		settingsrow_pill_active(split_artist_pill[i], (a & SPLIT_ARTIST_PILLS[i].bit) != 0);
	}
	lv_obj_set_state(split_genres_switch, LV_STATE_CHECKED, genres);
	lv_obj_set_hidden(split_genres_pills, !genres);
	for (int i = 0; i < SPLIT_GENRE_COUNT; i++) {
		settingsrow_pill_active(split_genre_pill[i], (g & SPLIT_GENRE_PILLS[i].bit) != 0);
	}
	lv_obj_set_state(join_albums_switch, LV_STATE_CHECKED, library_join_albums());
}

static void split_artists_cb(lv_event_t *e) {
	library_set_split_artists(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
	organize_touched();
	organize_refresh();
}

static void split_album_artists_cb(lv_event_t *e) {
	library_set_split_album_artists(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
	organize_touched();
}

static void split_genres_cb(lv_event_t *e) {
	library_set_split_genres(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
	organize_touched();
	organize_refresh();
}

// The pills are switches of their own: any number of separators at once.
static void split_artist_pill_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	unsigned bit = (unsigned)(uintptr_t)lv_event_get_user_data(e);
	library_set_artist_separators(library_artist_separators() ^ bit);
	organize_touched();
	organize_refresh();
}

static void genre_pill_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	unsigned bit = (unsigned)(uintptr_t)lv_event_get_user_data(e);
	library_set_genre_separators(library_genre_separators() ^ bit);
	organize_touched();
	organize_refresh();
}

static void join_albums_cb(lv_event_t *e) {
	library_set_join_albums(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
	organize_touched();
}

// Leaving for anywhere but the exceptions page, which is part of this one.
static void organize_leave_cb(lv_event_t *e) {
	(void)e;
	lv_obj_t *next = lv_screen_active();
	if (!organize_changed || next == scan_screen || next == artistexceptions_screen) {
		return;
	}
	organize_changed = false;
	library_reorganize();
}

static void build_scan_page(gui_config_t *cfg) {
	scan_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(scan_screen, cfg, "musicsettings_scan_options");
	settingsrow_title_corner_slots(settingsrow_page_title(scan_screen), cfg, 0);

	settingsrow_action(container, "musicsettings_scan_music_library", scan_row_cb, NULL);

	settingsrow_toggle(container, "musicsettings_do_not_ignore_articles", &keep_articles_switch, keep_articles_cb);
	if (!library_skip_articles()) {
		lv_obj_add_state(keep_articles_switch, LV_STATE_CHECKED);
	}

	// What happens when the card comes back from somewhere else: its new
	// files go into the index without a scan.
	settingsrow_toggle(container, "musicsettings_detect_changes", &detect_changes_switch, detect_changes_cb);
	option_note(container, "musicsettings_detect_changes_note");
	if (library_detect_changes()) {
		lv_obj_add_state(detect_changes_switch, LV_STATE_CHECKED);
	}

	lv_obj_t *retagged_switch = NULL;
	retagged_row = settingsrow_toggle(container, "musicsettings_detect_retagged", &retagged_switch, detect_retagged_cb);
	retagged_note = option_note(container, "musicsettings_detect_retagged_note");
	lv_obj_set_state(retagged_switch, LV_STATE_CHECKED, library_detect_retagged());
	retagged_show();

	settingsrow_toggle_pills(container, "musicsettings_split_artists", split_artists_cb, &split_artists_switch,
							 &split_artists_pills);
	for (int i = 0; i < SPLIT_ARTIST_COUNT; i++) {
		split_artist_pill[i] = settingsrow_pill_text(split_artists_pills, SPLIT_ARTIST_PILLS[i].text, (int)SPLIT_ARTIST_PILLS[i].bit,
											   split_artist_pill_cb);
	}
	split_album_artists_row = settingsrow_toggle(container, "musicsettings_split_album_artists",
												 &split_album_artists_switch, split_album_artists_cb);
	unsplit_row = settingsrow_add(container, "musicsettings_unsplit_artists", NULL, switch_screen_cb,
								  artistexceptions_screen);

	settingsrow_toggle_pills(container, "musicsettings_split_genres", split_genres_cb, &split_genres_switch,
							 &split_genres_pills);
	for (int i = 0; i < SPLIT_GENRE_COUNT; i++) {
		split_genre_pill[i] =
			settingsrow_pill_text(split_genres_pills, SPLIT_GENRE_PILLS[i].text, (int)SPLIT_GENRE_PILLS[i].bit, genre_pill_cb);
	}
	option_note(container, "musicsettings_split_note");

	settingsrow_toggle(container, "musicsettings_join_albums", &join_albums_switch, join_albums_cb);
	option_note(container, "musicsettings_join_albums_note");

	organize_refresh();
	lv_obj_add_event_cb(scan_screen, scan_page_loading_cb, LV_EVENT_SCREEN_LOAD_START, NULL);
	lv_obj_add_event_cb(scan_screen, organize_leave_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	lv_obj_add_event_cb(artistexceptions_screen, organize_leave_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	switcher_attach_back_gesture(scan_screen);
}

// ---------------------------------------------------------------------------
// Library and Now Playing
//
// What the lists and the now-playing page show, as opposed to what playback
// does with them: two pages, one for the library's lists and one for the page
// of the track that is playing. Their switches first, the pages further down.
// ---------------------------------------------------------------------------

static void album_view_cb(lv_event_t *e) {
	medialist_set_album_view(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void album_head_cb(lv_event_t *e) {
	medialist_set_album_head(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static lv_obj_t *nowplaying_at_boot_switch;

static void nowplaying_at_boot_cb(lv_event_t *e) {
	config_set_bool("music", "nowplaying_at_boot", lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
	config_save();
}

static void go_to_current_cb(lv_event_t *e) {
	medialist_set_go_to_current(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void quality_badges_cb(lv_event_t *e) {
	medialist_set_quality_badges(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

// "Show artist": a switch, and under it one pill per list it can be shown on.
// The pills are each on or off rather than one of three -- the artist can be
// wanted on the albums and on all the tracks alike -- and at least one stays
// on: a switch that is on and shows nothing anywhere is one nobody can read.
static lv_obj_t *artist_switch;
static lv_obj_t *artist_pills;
#define ARTIST_PILLS 5
static lv_obj_t *artist_pill[ARTIST_PILLS];
static const int ARTIST_PILL_BITS[ARTIST_PILLS] = {MEDIALIST_ARTIST_TRACKS, MEDIALIST_ARTIST_ALBUMS,
												   MEDIALIST_ARTIST_GENRES, MEDIALIST_ARTIST_FAVOURITES,
												   MEDIALIST_ARTIST_PLAYLISTS};

static void artist_refresh(void) {
	if (!artist_switch) {
		return;
	}
	bool on = medialist_show_artist();
	int lists = medialist_artist_lists();
	if (on) {
		lv_obj_add_state(artist_switch, LV_STATE_CHECKED);
		lv_obj_set_hidden(artist_pills, false);
	} else {
		lv_obj_remove_state(artist_switch, LV_STATE_CHECKED);
		lv_obj_set_hidden(artist_pills, true);
	}
	for (int i = 0; i < ARTIST_PILLS; i++) {
		settingsrow_pill_active(artist_pill[i], (lists & ARTIST_PILL_BITS[i]) != 0);
	}
}

static void artist_toggle_cb(lv_event_t *e) {
	(void)e;
	medialist_set_show_artist(lv_obj_has_state(artist_switch, LV_STATE_CHECKED), medialist_artist_lists());
	artist_refresh();
}

static void artist_pick_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	int lists = medialist_artist_lists() ^ (int)(intptr_t)lv_event_get_user_data(e);
	if ((lists & MEDIALIST_ARTIST_ALL) == 0) {
		return; // the last one stays on
	}
	medialist_set_show_artist(medialist_show_artist(), lists);
	artist_refresh();
}

static lv_obj_t *track_artist_switch;

static void track_artist_cb(lv_event_t *e) {
	metadata_set_track_artist(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
	player_refresh_artist();
}

static lv_obj_t *coverflow_switch;

static void coverflow_cb(lv_event_t *e) {
	coverflow_set_enabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static lv_obj_t *cover_accent_switch;

static void cover_accent_cb(lv_event_t *e) {
	player_cover_accent_set(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

// ---------------------------------------------------------------------------
// Which of the two arrangements the now-playing page uses
//
// Here and not under Appearance, where everything applies to the whole
// interface: this applies to the music on the card and to nothing else -- not
// to a radio station, not to a stream from a phone, not to an audiobook.
//
// The page owns the setting; this stores it and asks the page to decide again.
// ---------------------------------------------------------------------------

static lv_obj_t *layout_standard_pill;
static lv_obj_t *layout_alternative_pill;
static lv_obj_t *layout_studio_pill;

static void layout_refresh(void) {
	if (!layout_standard_pill) {
		return;
	}
	player_layout_t chosen = player_layout_get();
	settingsrow_pill_active(layout_standard_pill, chosen == PLAYER_LAYOUT_STANDARD);
	settingsrow_pill_active(layout_alternative_pill, chosen == PLAYER_LAYOUT_ALTERNATIVE);
	settingsrow_pill_active(layout_studio_pill, chosen == PLAYER_LAYOUT_STUDIO);
}

static void layout_pick_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	player_layout_set((player_layout_t)(intptr_t)lv_event_get_user_data(e));
	layout_refresh();
}

// ---------------------------------------------------------------------------
// Library
//
// Everything about finding the music: the Music page's tiles and Cover Flow,
// what an artist opens on, how an album's page and a list's rows are drawn,
// where a list opens, and whether tracks can be deleted from it. Now Playing,
// beside it, keeps what concerns the page of the track that is playing.
// ---------------------------------------------------------------------------

static void allow_delete_cb(lv_event_t *e) {
	medialist_set_allow_delete(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void build_library_page(gui_config_t *cfg) {
	library_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(library_screen, cfg, "musicsettings_library");
	settingsrow_title_corner_slots(settingsrow_page_title(library_screen), cfg, 0);

	// Which tiles the Music page shows, and in what order.
	musictiles_init(cfg);
	settingsrow_add(container, "musicsettings_rearrange_main_page", NULL, switch_screen_cb, musictiles_screen);

	// The album carousel. Off by default: it is a second way into the records,
	// not a replacement for the list, and the covers it draws are decoded at a
	// size the lists never ask for -- so switching it on fills the thumbnail
	// database with rather bigger pictures the first time it is flicked through.
	settingsrow_toggle(container, "cover_flow", &coverflow_switch, coverflow_cb);
	option_note(container, "cover_flow_note");
	if (coverflow_enabled()) {
		lv_obj_add_state(coverflow_switch, LV_STATE_CHECKED);
	}

	// On: an artist, a genre or a year is a list of its records. Off: a flat
	// list of its tracks, with the grouping button in the corner on an
	// artist's.
	settingsrow_toggle(container, "musicsettings_album_view", &album_view_switch, album_view_cb);
	option_note(container, "musicsettings_artist_opens_albums_note");
	if (medialist_album_view()) {
		lv_obj_add_state(album_view_switch, LV_STATE_CHECKED);
	}

	// How an album's own page is laid out: the plain list, or the cover and
	// what the record is at the top with the tracks under it.
	settingsrow_toggle(container, "musicsettings_album_head", &album_head_switch, album_head_cb);
	option_note(container, "musicsettings_album_head_note");
	if (medialist_album_head()) {
		lv_obj_add_state(album_head_switch, LV_STATE_CHECKED);
	}

	// Who a row is by, under its title, on the lists picked here.
	settingsrow_toggle_pills(container, "musicsettings_show_artist", artist_toggle_cb, &artist_switch, &artist_pills);
	artist_pill[0] = settingsrow_pill(artist_pills, "music_all_tracks", MEDIALIST_ARTIST_TRACKS, artist_pick_cb);
	artist_pill[1] = settingsrow_pill(artist_pills, "albums", MEDIALIST_ARTIST_ALBUMS, artist_pick_cb);
	artist_pill[2] = settingsrow_pill(artist_pills, "music_genres", MEDIALIST_ARTIST_GENRES, artist_pick_cb);
	artist_pill[3] = settingsrow_pill(artist_pills, "favourites", MEDIALIST_ARTIST_FAVOURITES, artist_pick_cb);
	artist_pill[4] = settingsrow_pill(artist_pills, "playlists", MEDIALIST_ARTIST_PLAYLISTS, artist_pick_cb);
	option_note(container, "musicsettings_show_artist_note");
	artist_refresh();
	theme_register_refresh(artist_refresh);

	// A small mark under a track's title saying what the file is. It comes from
	// what the scan wrote down, so a library indexed by an older build wears no
	// badges until it is scanned again.
	settingsrow_toggle(container, "audio_quality", &quality_badges_switch, quality_badges_cb);
	option_note(container, "musicsettings_shows_the_tracks_audio_quality_wi");
	if (medialist_quality_badges()) {
		lv_obj_add_state(quality_badges_switch, LV_STATE_CHECKED);
	}

	// The library lists open on what is playing: the track in All tracks, what
	// it belongs to in the others.
	settingsrow_toggle(container, "musicsettings_go_to_current", &go_to_current_switch, go_to_current_cb);
	option_note(container, "musicsettings_go_to_current_note");
	if (medialist_go_to_current()) {
		lv_obj_add_state(go_to_current_switch, LV_STATE_CHECKED);
	}

	// The bin among the selection's buttons, which deletes the chosen tracks
	// from the card and from the library. Off by default: it is the one
	// button on these lists that cannot be undone.
	settingsrow_toggle(container, "musicsettings_allow_delete", &allow_delete_switch, allow_delete_cb);
	option_note(container, "musicsettings_allow_delete_note");
	if (medialist_allow_delete()) {
		lv_obj_add_state(allow_delete_switch, LV_STATE_CHECKED);
	}

	switcher_attach_back_gesture(library_screen);
}

// ---------------------------------------------------------------------------
// Now Playing
//
// The page of the track that is playing: how it is arranged, the colour it
// wears, whose name it shows, and whether the player starts on it.
// ---------------------------------------------------------------------------

// The level meter on the sleeve (meterview.h).
static lv_obj_t *meter_pill[METER_MODES];

static void meter_refresh(void) {
	for (int i = 0; i < METER_MODES; i++) {
		if (meter_pill[i]) {
			settingsrow_pill_active(meter_pill[i], (int)meterview_mode() == i);
		}
	}
}

static void meter_pick_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	meterview_set_mode((meter_mode_t)(intptr_t)lv_event_get_user_data(e));
	meter_refresh();
}

static void build_nowplaying_page(gui_config_t *cfg) {
	nowplaying_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(nowplaying_screen, cfg, "musicsettings_now_playing");
	settingsrow_title_corner_slots(settingsrow_page_title(nowplaying_screen), cfg, 0);

	// How the now-playing page is arranged.
	lv_obj_t *layout_pills = NULL;
	settingsrow_pills(container, "musicsettings_player_layout", &layout_pills);
	layout_standard_pill =
		settingsrow_pill(layout_pills, "musicsettings_layout_standard", PLAYER_LAYOUT_STANDARD, layout_pick_cb);
	layout_alternative_pill =
		settingsrow_pill(layout_pills, "musicsettings_layout_alternative", PLAYER_LAYOUT_ALTERNATIVE, layout_pick_cb);
	layout_studio_pill =
		settingsrow_pill(layout_pills, "musicsettings_layout_studio", PLAYER_LAYOUT_STUDIO, layout_pick_cb);

	layout_refresh();
	theme_register_refresh(layout_refresh);

	// Said here rather than left to be discovered: the Waveform arrangement is
	// built out of a file on the card -- the shape of the track, the sleeve it
	// takes its colour from, the tags in the pill -- so a radio station, a
	// stream from a phone and an audiobook all get the standard one whatever
	// this says. Studio asks for less and takes all of them.
	option_note(container, "musicsettings_local_only_note");

	// The sleeve's colour as the accent of the other two arrangements too.
	lv_obj_t *meter_pills = NULL;
	settingsrow_pills(container, "musicsettings_meter", &meter_pills);
	static const char *const METER_NAMES[METER_MODES] = {"musicsettings_meter_spectrum", "musicsettings_meter_vu"};
	for (int i = 0; i < METER_MODES; i++) {
		meter_pill[i] = settingsrow_pill(meter_pills, METER_NAMES[i], i, meter_pick_cb);
	}
	meter_refresh();
	theme_register_refresh(meter_refresh);
	option_note(container, "musicsettings_meter_note");

	settingsrow_toggle(container, "musicsettings_cover_accent", &cover_accent_switch, cover_accent_cb);
	option_note(container, "musicsettings_cover_accent_note");
	if (player_cover_accent()) {
		lv_obj_add_state(cover_accent_switch, LV_STATE_CHECKED);
	}

	// Whose name goes under the title on the now-playing page, in the control
	// centre and on the screensaver: the album's artist, as the record is
	// filed, or the track's own. Off by default -- see metadata_shown_artist().
	settingsrow_toggle(container, "musicsettings_track_artist", &track_artist_switch, track_artist_cb);
	option_note(container, "musicsettings_track_artist_note");
	if (metadata_track_artist()) {
		lv_obj_add_state(track_artist_switch, LV_STATE_CHECKED);
	}

	// Whether a start with a remembered track lands on the now-playing page
	// rather than on the home screen. Read once, at startup (main.c).
	settingsrow_toggle(container, "musicsettings_nowplaying_at_boot", &nowplaying_at_boot_switch,
					   nowplaying_at_boot_cb);
	option_note(container, "musicsettings_nowplaying_at_boot_note");
	if (config_get_bool("music", "nowplaying_at_boot", false)) {
		lv_obj_add_state(nowplaying_at_boot_switch, LV_STATE_CHECKED);
	}

	switcher_attach_back_gesture(nowplaying_screen);
}

static lv_obj_t *endless_shuffle_switch;
static lv_obj_t *open_player_switch;
static lv_obj_t *mono_switch;

static void mono_cb(lv_event_t *e) {
	(void)e;
	mono_set_enabled(lv_obj_has_state(mono_switch, LV_STATE_CHECKED));
}

static void open_player_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("player", "open_player_on_tap", lv_obj_has_state(open_player_switch, LV_STATE_CHECKED));
	config_save();
}

static void endless_shuffle_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("player", "endless_shuffle", lv_obj_has_state(endless_shuffle_switch, LV_STATE_CHECKED));
	config_save();
}

// The sleep timer for music, which is the card, Tidal and Qobuz: one transport,
// one timer. Books and podcasts have their own, on their own pages, because the
// stretch that suits one is not the one that suits another.
static settingsrow_duration_t sleep_row;

static void sleep_refresh(void) {
	settingsrow_duration_expanded(&sleep_row, sleeptimer_enabled(SLEEPTIMER_MUSIC));
	settingsrow_duration_repaint(&sleep_row);
}

static void sleep_toggle_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_set_enabled(SLEEPTIMER_MUSIC, lv_obj_has_state(sleep_row.toggle, LV_STATE_CHECKED));
	sleep_refresh();
}

static void sleep_wheel_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_set_minutes(SLEEPTIMER_MUSIC, settingsrow_duration_minutes(&sleep_row));
}

// The control centre switches the same timer from outside this page, so the
// switch and the wheels are read again every time the page comes up.
static void playback_loaded_cb(lv_event_t *e) {
	(void)e;
	settingsrow_duration_set_minutes(&sleep_row, sleeptimer_minutes(SLEEPTIMER_MUSIC));
	sleep_refresh();
}

// Where "add to queue" puts a track: right after the one playing, or at the end
// of the queue. playlist_enqueue() keeps the setting; this page only shows it.
static lv_obj_t *queue_next_pill;
static lv_obj_t *queue_end_pill;

static void queue_add_refresh(void) {
	if (!queue_next_pill) {
		return;
	}
	bool at_end = playlist_enqueue_at_end();
	settingsrow_pill_active(queue_next_pill, !at_end);
	settingsrow_pill_active(queue_end_pill, at_end);
}

static void queue_add_pick_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	playlist_set_enqueue_at_end(lv_event_get_user_data(e) != NULL);
	queue_add_refresh();
}

static void build_playback_page(gui_config_t *cfg) {
	playback_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(playback_screen, cfg, "musicsettings_playback_options");
	// No corner buttons on this page, so the heading can have the width the
	// default reserves for one -- "Opzioni di riproduzione" does not fit without
	// it.
	settingsrow_title_corner_slots(settingsrow_page_title(playback_screen), cfg, 0);

	// No gaps between tracks. Underneath it is a PCM that stays open (see
	// audio.h): it works only for music on the card, and only between tracks of
	// the same format.
	// The engine's state and not the file's: with nothing saved the engine
	// starts with gapless off (see main.c), and the switch has to say so.
	settingsrow_toggle(container, "musicsettings_gapless_playback", &gapless_switch, gapless_toggle_cb);
	if (musicsettings_gapless_enabled()) {
		lv_obj_add_state(gapless_switch, LV_STATE_CHECKED);
	}

	// ReplayGain: off, corrected per track, corrected per album or corrected per track when shuffled.
	rg_card = settingsrow_toggle_pills(container, "musicsettings_replay_gain", rg_toggle_cb, &rg_switch, &rg_pills);
	rg_track_pill = settingsrow_pill(rg_pills, "track", REPLAYGAIN_TRACK, rg_pick_cb);
	rg_album_pill = settingsrow_pill(rg_pills, "albums", REPLAYGAIN_ALBUM, rg_pick_cb);
	rg_track_when_shuffled_pill = settingsrow_pill(rg_pills, "track_when_shuffled", REPLAYGAIN_TRACK_WHEN_SHUFFLED, rg_pick_cb);
	rg_refresh();

	// The fade at the two ends of a track: a page of its own because it carries
	// a length as well as a switch.
	build_fade_page(cfg);
	fade_row = settingsrow_add(container, "fade", NULL, switch_screen_cb, fade_screen);

	// Both channels as one, for a single earbud or one ear.
	settingsrow_toggle(container, "musicsettings_mono_audio", &mono_switch, mono_cb);
	if (mono_get_enabled()) {
		lv_obj_add_state(mono_switch, LV_STATE_CHECKED);
	}

	// Off: a tapped track starts playing and the list stays on screen.
	settingsrow_toggle(container, "musicsettings_open_player_on_tap", &open_player_switch, open_player_cb);
	if (musicsettings_open_player_on_tap()) {
		lv_obj_add_state(open_player_switch, LV_STATE_CHECKED);
	}

	// "Add to queue" in the library, the search and the browser: next, as it
	// always was, or at the end.
	lv_obj_t *queue_pills = NULL;
	settingsrow_pills(container, "add_to_queue", &queue_pills);
	queue_next_pill = settingsrow_pill(queue_pills, "queue_add_next", 0, queue_add_pick_cb);
	queue_end_pill = settingsrow_pill(queue_pills, "queue_add_end", 1, queue_add_pick_cb);
	queue_add_refresh();
	theme_register_refresh(queue_add_refresh);

	// Which of the two shuffles "Play in random order" starts. The mode button
	// on the player still reaches both by hand; this only decides what that one
	// action means. Off by default, so the action stays the plain shuffle: once
	// through the list and stop.
	settingsrow_toggle(container, "musicsettings_continuous_shuffle", &endless_shuffle_switch, endless_shuffle_cb);
	option_note(container, "musicsettings_shuffle_note");
	if (musicsettings_endless_shuffle()) {
		lv_obj_add_state(endless_shuffle_switch, LV_STATE_CHECKED);
	}

	// The sleep timer, directly under it, with its two wheels revealed by the
	// switch the way the screensaver's two pills are.
	settingsrow_toggle_duration(container, "sleep_timer", sleep_toggle_cb, sleep_wheel_cb, &sleep_row);
	settingsrow_duration_set_minutes(&sleep_row, sleeptimer_minutes(SLEEPTIMER_MUSIC));
	sleep_refresh();
	theme_register_refresh(sleep_refresh);
	lv_obj_add_event_cb(playback_screen, playback_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	// And again whenever the control centre switches something while the page
	// is the one under it.
	lv_obj_add_event_cb(playback_screen, playback_loaded_cb, LV_EVENT_REFRESH, NULL);

	// One record into the next, instead of the queue simply running out.
	settingsrow_toggle(container, "musicsettings_play_albums_back_to_back", &album_chain_switch, album_chain_cb);
	// What "back to back" means here, since the answer is not obvious: the next
	// record is the next one in the album list, and the repeat setting does not
	// get to send the same one round again.
	option_note(container, "musicsettings_album_chaining_note");
	if (device_state_album_chaining()) {
		lv_obj_add_state(album_chain_switch, LV_STATE_CHECKED);
	}

	// The same for a folder started from the browser. The order it walks in is
	// worth spelling out, because it is chosen and not obvious.
	settingsrow_toggle(container, "musicsettings_play_by_folder", &folder_chain_switch, folder_chain_cb);
	option_note(container, "musicsettings_folder_chaining_note");
	if (device_state_folder_chaining()) {
		lv_obj_add_state(folder_chain_switch, LV_STATE_CHECKED);
	}

	// What survives the power switch: the loaded track and the volume level.
	settingsrow_toggle(container, "musicsettings_remember_track", &remember_track_switch, remember_track_cb);
	if (config_get_int("player", "remember_track", 0)) {
		lv_obj_add_state(remember_track_switch, LV_STATE_CHECKED);
	}
	settingsrow_toggle(container, "musicsettings_remember_volume", &remember_volume_switch, remember_volume_cb);
	if (config_get_int("player", "remember_volume", 0)) {
		lv_obj_add_state(remember_volume_switch, LV_STATE_CHECKED);
	}

	switcher_attach_back_gesture(playback_screen);
}

static void musicsettings_loaded_cb(lv_event_t *e) {
	(void)e;
	rg_refresh();
}

void musicsettings_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(musicsettings_screen, cfg, "music");

	// The pages of behaviour first: how the index is built, what playback does
	// around a track, how the library's lists and the now-playing page look,
	// and Last.fm, which only reports what was played. Then the sound, from the
	// widest tool to the narrowest, and the DAC's own switches at the end.
	artistexceptions_init(cfg, organize_touched);
	build_scan_page(cfg);
	settingsrow_add(container, "musicsettings_scan_options", NULL, switch_screen_cb, scan_screen);

	build_playback_page(cfg);
	settingsrow_add(container, "musicsettings_playback_options", NULL, switch_screen_cb, playback_screen);

	build_library_page(cfg);
	settingsrow_add(container, "musicsettings_library", NULL, switch_screen_cb, library_screen);

	build_nowplaying_page(cfg);
	settingsrow_add(container, "musicsettings_now_playing", NULL, switch_screen_cb, nowplaying_screen);

	lastfmsettings_init(cfg);
	settingsrow_add(container, "lastfm", NULL, switch_screen_cb, lastfmsettings_screen());

	settingsrow_toggle(container, "musicsettings_direct", &direct_switch, direct_toggle_cb);
	direct_refresh();
	option_note(container, "musicsettings_direct_note");

	build_eq_page(cfg);
	eq_row = settingsrow_add(container, "equaliser", NULL, switch_screen_cb, eq_screen);

	// PEQ right below: the same thing said more precisely, so whoever comes
	// looking for the equaliser finds the slider one first and the parametric
	// one next to it.
	peqpage_init(cfg);
	peq_row = settingsrow_add(container, "peq", NULL, switch_screen_cb, peqpage_screen());

	build_mseb_page(cfg);
	mseb_row = settingsrow_add(container, "mseb", NULL, switch_screen_cb, mseb_screen);

	build_soundfield_page(cfg);
	soundfield_row = settingsrow_add(container, "musicsettings_soundfield", NULL, switch_screen_cb, soundfield_screen);

	// Crossfeed sits next to soundfield because they are relatives: one widens
	// the stereo image, the other pulls it back inside the head. Anyone looking
	// for one wants to see the other.
	build_crossfeed_page(cfg);
	crossfeed_row = settingsrow_add(container, "musicsettings_crossfeed", NULL, switch_screen_cb, crossfeed_screen);

	build_balance_page(cfg);
	balance_row = settingsrow_add(container, "musicsettings_channel_balance", NULL, switch_screen_cb, balance_screen);

	build_volume_limit_page(cfg);
	vlimit_row = settingsrow_add(container, "musicsettings_volume_limit", NULL, switch_screen_cb, vlimit_screen);

	// The gain step: off = low gain (the stock default), on = +6 dB. Three
	// pills where the board has three gains.
	if (gain_level_count() == 3) {
		lv_obj_t *pills;
		static const char *const GAIN_NAMES[3] = {"musicsettings_gain_low", "musicsettings_gain_mid",
												  "musicsettings_gain_high"};
		settingsrow_pills(container, "musicsettings_gain", &pills);
		for (int i = 0; i < 3; i++) {
			gain_pill[i] = settingsrow_pill(pills, GAIN_NAMES[i], i, gain_pick_cb);
		}
	} else {
		settingsrow_toggle(container, "musicsettings_high_gain", &gain_switch, gain_toggle_cb);
	}
	gain_refresh();
	theme_register_refresh(gain_refresh);

	settingsrow_toggle(container, "quickpanel_line_out", &lineout_switch, lineout_toggle_cb);
	lineout_refresh();

	// How loud DSD comes out. The DAC is the only thing that plays it here, so
	// this is the only place its level can be touched at all.
	//
	// What was chosen last time is loaded before the row rather than with the
	// other settings at the end of this function: the row paints itself from
	// what the audio side holds, so a load after it would leave the switch off
	// with the compensation on.
	alsa_set_dsd_gain_index((int)config_get_int("audio", "dsd_gain", 0));
	settingsrow_toggle_pills(container, "musicsettings_dsd_gain", dsd_gain_toggle_cb, &dsd_gain_switch,
							 &dsd_gain_pills);
	for (int i = 0; i < DSD_GAIN_MAX_DB; i++) {
		char text[16];
		snprintf(text, sizeof(text), "+%d dB", i + 1);
		dsd_gain_pill[i] = settingsrow_pill_text(dsd_gain_pills, text, i + 1, dsd_gain_pick_cb);
	}
	dsd_gain_refresh();

	lv_obj_t *usb_dsd_pills;
	settingsrow_pills(container, "musicsettings_usb_dsd", &usb_dsd_pills);
	usb_dsd_pill[USB_DSD_AUTO] = settingsrow_pill_text(usb_dsd_pills, "Auto", USB_DSD_AUTO, usb_dsd_pick_cb);
	usb_dsd_pill[USB_DSD_DOP] = settingsrow_pill_text(usb_dsd_pills, "DoP", USB_DSD_DOP, usb_dsd_pick_cb);
	usb_dsd_pill[USB_DSD_PCM] = settingsrow_pill_text(usb_dsd_pills, "PCM", USB_DSD_PCM, usb_dsd_pick_cb);
	usb_dsd_pill[USB_DSD_NATIVE] =
		settingsrow_pill_text(usb_dsd_pills, tr("musicsettings_usb_dsd_native"), USB_DSD_NATIVE, usb_dsd_pick_cb);
	option_note(container, "musicsettings_usb_dsd_note");
	option_note(container, "musicsettings_usb_dsd_native_note");
	usb_dsd_refresh();
	theme_register_refresh(usb_dsd_refresh);

	build_filter_page(cfg);
	settingsrow_add(container, "musicsettings_filters", NULL, switch_screen_cb, dacfilter_screen);

	// Non-oversampling off and dynamic-range enhancement on by default, like the
	// stock player. The R1's CS43131 has no DRE and the Blaze's AK4493 neither
	// switch: no rows there, and alsa-controls.c keeps them off.
	if (!alsa_board_is_ak4493()) {
		settingsrow_toggle(container, "musicsettings_nos", &nos_switch, nos_toggle_cb);
		if (config_get_int("audio", "dac_nos", 0)) {
			lv_obj_add_state(nos_switch, LV_STATE_CHECKED);
		}
	}

	if (!alsa_board_is_cs43131() && !alsa_board_is_ak4493()) {
		settingsrow_toggle(container, "musicsettings_dac_dre", &dre_switch, dre_toggle_cb);
		if (config_get_int("audio", "dac_dre", 1)) {
			lv_obj_add_state(dre_switch, LV_STATE_CHECKED);
		}
	}

	theme_register_refresh(rg_refresh);
	theme_register_refresh(dsd_gain_refresh);
	lv_obj_add_event_cb(musicsettings_screen, musicsettings_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);

	// What was chosen last time becomes effective at boot. audio_init has
	// already written a volume by this point, so the gain is re-applied here to
	// keep the two consistent.
	fade_apply_setting();

	set_dac_filter(stored_dac_filter());
	set_dac_dre((int)config_get_int("audio", "dac_dre", 1));
	set_dac_nos((int)config_get_int("audio", "dac_nos", 0));
	set_gain_level(stored_gain_level());


	lv_obj_add_event_cb(musicsettings_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	// The parametric equaliser's chevron, which no setter on this page repaints,
	// follows the control centre's button while the page is under it.
	lv_obj_add_event_cb(musicsettings_screen, screen_loaded_cb, LV_EVENT_REFRESH, NULL);
}
