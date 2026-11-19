#include "powersettings.h"

#include <stdio.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/settings/batteryusage.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/scheduledoff.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/axpcharge.h"
#include "src/system/device/led.h"
#include "src/system/device/power.h"
#include "src/system/device/system.h"
#include "src/system/device/sysinfo.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/uiscale.h"

lv_obj_t *powersettings_screen;

// 80 to 100 in fives. Stopping short of full is what keeps a lithium cell
// healthy if the device mostly lives on the charger.
#define CHARGE_LIMIT_MIN 80
#define CHARGE_LIMIT_STEP 5
#define CHARGE_LIMIT_COUNT 5 // 80, 85, 90, 95, 100

static const struct {
	int minutes;
	const char *label;
} AUTO_OFF[] = {
	// 15 minutes at the shortest: less shuts the player off under a paused album.
	{15, "power_15_minutes"}, {30, "power_30_minutes"}, {60, "power_1_hour"}, {120, "power_2_hours"}, {180, "power_3_hours"}, {360, "power_6_hours"},
};
#define AUTO_OFF_COUNT ((int)(sizeof(AUTO_OFF) / sizeof(AUTO_OFF[0])))

// How long DAC mode, the Bluetooth receiver or Wi-Fi Transfer may sit with
// nothing going through it before power.c closes it. 0 is never.
static const struct {
	int minutes;
	const char *label;
} IDLE_LINK[] = {
	{5, "power_5_minutes"}, {10, "power_10_minutes"}, {15, "power_15_minutes"}, {30, "power_30_minutes"}, {0, "power_never"},
};
#define IDLE_LINK_COUNT ((int)(sizeof(IDLE_LINK) / sizeof(IDLE_LINK[0])))

// How long the screen stays dark before standby. Saved in seconds under
// [power] standby_mem_seconds, which power.c reads at startup.
static const struct {
	int seconds;
	const char *label;
} STANDBY_AFTER[] = {
	{30, "power_30_seconds"}, {60, "power_1_minute"},	{120, "power_2_minutes"},
	{180, "power_3_minutes"},  {300, "power_5_minutes"}, {600, "power_10_minutes"},
};
#define STANDBY_AFTER_COUNT ((int)(sizeof(STANDBY_AFTER) / sizeof(STANDBY_AFTER[0])))

// The standby card while standby is off: as low as a toggle with pills under
// it when they are folded away ("Split artists" in the music settings), the
// switch and the card's padding and nothing else.
#define STANDBY_CLOSED_H ui_px(76)

static lv_obj_t *charge_value, *charge_slider;
static lv_obj_t *idle_link_value, *idle_link_slider;
static lv_obj_t *auto_off_switch, *auto_off_value, *auto_off_slider, *auto_off_card;
static lv_obj_t *led_on_switch, *led_off_switch, *led_off_card;
static lv_obj_t *key_lock_switch;
static lv_obj_t *standby_switch, *standby_value, *standby_slider, *standby_card;
static lv_obj_t *charge_note;
static lv_obj_t *axp_limit_switch, *axp_current_switch; // the R1's two, instead of the slider

// --- LED colours ---
//
// One row per state; a tap moves it to the next colour and shows it on the LED
// for a moment. The patterns are the ones leds_sgm31324_add.sh registers on
// the Blaze (12-21 added there by hand); 9 and 10 are left out, they look the
// same as 14 and 13. 7, the blinking red, is left out too: breathing and solid are enough.
static const struct {
	int pattern;
	const char *label;
} LED_COLOURS[] = {
	{5, "led_colour_off"},
	{1, "led_colour_blue"},			 {17, "led_colour_blue_breathing"},
	{3, "led_colour_cyan"},			 {18, "led_colour_cyan_breathing"},
	{14, "led_colour_dark_blue"},	 {16, "led_colour_blue_blink"},
	{8, "led_colour_green"},		 {12, "led_colour_green_blink"},
	{2, "led_colour_yellow"},		 {19, "led_colour_yellow_breathing"},
	{4, "led_colour_orange"},		 {20, "led_colour_orange_breathing"},
	{15, "led_colour_red"},			 {6, "led_colour_red_breathing"},
	{13, "led_colour_purple"},		 {21, "led_colour_purple_breathing"},
	{11, "led_colour_white"},
};
#define LED_COLOUR_COUNT ((int)(sizeof(LED_COLOURS) / sizeof(LED_COLOURS[0])))
#define LED_PREVIEW_MS 3000

static const char *const LED_STATE_LABELS[LED_STATE_COUNT] = {
	[LED_STATE_IDLE] = "led_state_idle",		   [LED_STATE_PCM_48K] = "led_state_pcm_48k",
	[LED_STATE_PCM_192K] = "led_state_pcm_192k",   [LED_STATE_PCM_384K] = "led_state_pcm_384k",
	[LED_STATE_DSD] = "led_state_dsd",			   [LED_STATE_PODCAST] = "led_state_podcast",
	[LED_STATE_CHARGING] = "led_state_charging",   [LED_STATE_WIFI] = "led_state_wifi",
	[LED_STATE_BT_RX] = "led_state_bt_receiver",	[LED_STATE_PCM_96K] = "led_state_pcm_96k",
	[LED_STATE_PCM_768K] = "led_state_pcm_768k",
};

// The rows in page order: the enum is in saved-index order, with the PCM ranges
// added later at its end.
static const led_state_t LED_STATE_ORDER[LED_STATE_COUNT] = {
	LED_STATE_IDLE,		  LED_STATE_PCM_48K, LED_STATE_PCM_96K,	 LED_STATE_PCM_192K,
	LED_STATE_PCM_384K,	  LED_STATE_PCM_768K, LED_STATE_DSD,	 LED_STATE_PODCAST,
	LED_STATE_CHARGING,	  LED_STATE_WIFI,	 LED_STATE_BT_RX,
};

static lv_obj_t *led_colours_screen;
static lv_obj_t *led_colour_values[LED_STATE_COUNT];
static lv_timer_t *led_preview_timer;

static int led_colour_index(int pattern) {
	for (int i = 0; i < LED_COLOUR_COUNT; i++) {
		if (LED_COLOURS[i].pattern == pattern) {
			return i;
		}
	}
	return -1;
}

static void led_colour_label(led_state_t state) {
	int i = led_colour_index(led_state_pattern(state));
	lv_label_set_text(led_colour_values[state], i >= 0 ? tr(LED_COLOURS[i].label) : "?");
}

static void led_preview_done_cb(lv_timer_t *t) {
	(void)t;
	led_preview_timer = NULL;
	led_preview_end();
}

static void led_colour_row_cb(lv_event_t *e) {
	led_state_t state = (led_state_t)(intptr_t)lv_event_get_user_data(e);
	int next = LED_COLOURS[(led_colour_index(led_state_pattern(state)) + 1) % LED_COLOUR_COUNT].pattern;
	led_set_state_pattern(state, next);
	char key[24];
	snprintf(key, sizeof(key), "colour_%d", (int)state);
	config_set_int("led", key, next);
	led_colour_label(state);

	led_preview(next);
	if (led_preview_timer) {
		lv_timer_reset(led_preview_timer);
	} else {
		led_preview_timer = lv_timer_create(led_preview_done_cb, LED_PREVIEW_MS, NULL);
		lv_timer_set_repeat_count(led_preview_timer, 1);
	}
}

static void build_led_colours_page(gui_config_t *cfg) {
	led_colours_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(led_colours_screen, cfg, "power_led_colours");
	for (int i = 0; i < LED_STATE_COUNT; i++) {
		led_state_t s = LED_STATE_ORDER[i];
		settingsrow_add(container, LED_STATE_LABELS[s], &led_colour_values[s], led_colour_row_cb, (void *)(intptr_t)s);
		led_colour_label(s);
	}
	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr("power_led_colours_note"));
	switcher_attach_back_gesture(led_colours_screen);
}

// --- reading the saved values ---

static int charge_index(void) {
	int percent = (int)config_get_int("power", "charge_limit", 100);
	int index = (percent - CHARGE_LIMIT_MIN) / CHARGE_LIMIT_STEP;
	if (index < 0) {
		index = 0;
	}
	if (index >= CHARGE_LIMIT_COUNT) {
		index = CHARGE_LIMIT_COUNT - 1;
	}
	return index;
}

static int auto_off_index(void) {
	int minutes = (int)config_get_int("power", "auto_off_minutes", 30);
	if (minutes < AUTO_OFF[0].minutes) {
		return 0; // the 5 and 10 minutes earlier versions offered
	}
	for (int i = 0; i < AUTO_OFF_COUNT; i++) {
		if (AUTO_OFF[i].minutes == minutes) {
			return i;
		}
	}
	return 1; // the 30 minute default
}

static int idle_link_index(void) {
	int minutes = (int)config_get_int("power", "idle_link_minutes", 10);
	for (int i = 0; i < IDLE_LINK_COUNT; i++) {
		if (IDLE_LINK[i].minutes == minutes) {
			return i;
		}
	}
	return 1; // the 10 minute default
}

// A wait saved by hand that is not one of these lands on the next one up,
// or on the longest.
static int standby_after_index(void) {
	int seconds = (int)config_get_int("power", "standby_mem_seconds", 60); // power.c's default
	for (int i = 0; i < STANDBY_AFTER_COUNT; i++) {
		if (seconds <= STANDBY_AFTER[i].seconds) {
			return i;
		}
	}
	return STANDBY_AFTER_COUNT - 1;
}

// --- applying them ---

static void set_checked(lv_obj_t *sw, bool on) {
	if (on) {
		lv_obj_add_state(sw, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(sw, LV_STATE_CHECKED);
	}
}

void powersettings_apply(void) {
	if (axpcharge_applies()) {
		// The R1 has no percentage slider: a percentage limit saved on it
		// becomes the 80 % switch.
		if (config_get_int("power", "charge_limit", 100) < 100) {
			config_set_int("power", "charge_limit", 100);
			config_set_bool("power", "charge_limit_80", true);
			config_save();
		}
		power_set_charge_limit(100);
		axpcharge_set(config_get_bool("power", "charge_limit_80", false),
					  config_get_bool("power", "charge_500ma", false));
	} else {
		power_set_charge_limit(CHARGE_LIMIT_MIN + (charge_index() * CHARGE_LIMIT_STEP));
	}

	power_set_auto_off(config_get_bool("power", "auto_off", false), (uint32_t)AUTO_OFF[auto_off_index()].minutes);
	power_set_idle_link((uint32_t)IDLE_LINK[idle_link_index()].minutes);

	// Suspend-to-RAM standby, on by default: saves battery by suspending the
	// player shortly after the screen goes off.
	power_set_standby_enabled(config_get_bool("power", "standby_mem", true));
	power_set_standby_after((uint32_t)STANDBY_AFTER[standby_after_index()].seconds);

	// The LED master switch comes first: with it off there is nothing for the
	// standby option to turn off, and led.c keeps the charge indicator alive
	// on its own regardless.
	led_set_enabled(config_get_bool("power", "led_on", true));
	led_set_idle_off(config_get_bool("power", "led_off_standby", true));
	for (int s = 0; s < LED_STATE_COUNT; s++) {
		char key[24];
		snprintf(key, sizeof(key), "colour_%d", s);
		// A range split off later, never set, follows the colour saved for the
		// one it used to be part of (88.2-96 was "up to 192", 768 "above 192").
		int fallback = s == LED_STATE_PCM_96K	 ? led_state_pattern(LED_STATE_PCM_192K)
					   : s == LED_STATE_PCM_768K ? led_state_pattern(LED_STATE_PCM_384K)
												 : 0;
		int pattern = (int)config_get_int("led", key, fallback);
		if (led_colour_index(pattern) >= 0) {
			led_set_state_pattern((led_state_t)s, pattern);
		}
	}

	input_set_key_lock(config_get_bool("power", "lock_keys_screen_off", false));
}

static void refresh_labels(void) {
	if (led_colours_screen) {
		for (int s = 0; s < LED_STATE_COUNT; s++) {
			led_colour_label((led_state_t)s);
		}
	}
	if (charge_value) {
		lv_label_set_text_fmt(charge_value, "%d%%", CHARGE_LIMIT_MIN + (charge_index() * CHARGE_LIMIT_STEP));
	}
	if (axp_limit_switch) {
		set_checked(axp_limit_switch, config_get_bool("power", "charge_limit_80", false));
		set_checked(axp_current_switch, config_get_bool("power", "charge_500ma", false));
	}
	lv_label_set_text(auto_off_value, tr(AUTO_OFF[auto_off_index()].label));
	lv_label_set_text(idle_link_value, tr(IDLE_LINK[idle_link_index()].label));
	lv_label_set_text(standby_value, tr(STANDBY_AFTER[standby_after_index()].label));

	// The LED master switch, and the standby option that hangs off it: with the
	// LED switched off entirely there is nothing left for the standby option to
	// do, so it greys out.
	bool led_on = config_get_bool("power", "led_on", true);
	if (led_on) {
		lv_obj_add_state(led_on_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(led_on_switch, LV_STATE_CHECKED);
	}

	if (config_get_bool("power", "led_off_standby", true)) {
		lv_obj_add_state(led_off_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(led_off_switch, LV_STATE_CHECKED);
	}

	if (led_on) {
		lv_obj_remove_state(led_off_switch, LV_STATE_DISABLED);
		lv_obj_set_style_opa(led_off_card, LV_OPA_COVER, 0);
	} else {
		lv_obj_add_state(led_off_switch, LV_STATE_DISABLED);
		lv_obj_set_style_opa(led_off_card, LV_OPA_40, 0);
	}

	set_checked(key_lock_switch, config_get_bool("power", "lock_keys_screen_off", false));

	// Standby and its wait, on one card like the automatic power-off below.
	bool standby = config_get_bool("power", "standby_mem", true);
	if (standby) {
		lv_obj_add_state(standby_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(standby_switch, LV_STATE_CHECKED);
	}
	lv_label_set_text(standby_value, tr(STANDBY_AFTER[standby_after_index()].label));
	settingsrow_toggle_slider_expanded(standby_card, standby);
	if (!standby) {
		lv_obj_set_height(standby_card, STANDBY_CLOSED_H);
	}

	// One card: the slider is only on it while the toggle is on.
	bool auto_off = config_get_bool("power", "auto_off", false);
	if (auto_off) {
		lv_obj_add_state(auto_off_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(auto_off_switch, LV_STATE_CHECKED);
	}
	settingsrow_toggle_slider_expanded(auto_off_card, auto_off);
}

static void charge_changed_cb(lv_event_t *e) {
	(void)e;
	int index = (int)lv_slider_get_value(charge_slider);
	config_set_int("power", "charge_limit", CHARGE_LIMIT_MIN + (index * CHARGE_LIMIT_STEP));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void axp_limit_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "charge_limit_80", lv_obj_has_state(axp_limit_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void axp_current_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "charge_500ma", lv_obj_has_state(axp_current_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void auto_off_changed_cb(lv_event_t *e) {
	(void)e;
	int index = (int)lv_slider_get_value(auto_off_slider);
	config_set_int("power", "auto_off_minutes", AUTO_OFF[index].minutes);
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void idle_link_changed_cb(lv_event_t *e) {
	(void)e;
	config_set_int("power", "idle_link_minutes", IDLE_LINK[lv_slider_get_value(idle_link_slider)].minutes);
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void led_off_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "led_off_standby", lv_obj_has_state(led_off_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

// The master switch. With the LED off the standby option has nothing left to
// do, so it greys out -- and the charger still lights the LED regardless, so
// a charge in progress is never invisible.
static void led_on_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "led_on", lv_obj_has_state(led_on_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void key_lock_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "lock_keys_screen_off", lv_obj_has_state(key_lock_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void auto_off_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "auto_off", lv_obj_has_state(auto_off_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void standby_after_changed_cb(lv_event_t *e) {
	(void)e;
	int index = (int)lv_slider_get_value(standby_slider);
	config_set_int("power", "standby_mem_seconds", STANDBY_AFTER[index].seconds);
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void standby_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "standby_mem", lv_obj_has_state(standby_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

// --- the scheduled shutdown (scheduledoff.h) ---
//
// The switch arms it from now and the wheels are its length, the same pair the
// music's sleep timer uses. The control centre arms and cancels it from
// outside, and it disarms itself when the time comes, so the switch is read
// again every time the page comes up.
static settingsrow_duration_t sched_row;

static void sched_refresh(void) {
	settingsrow_duration_expanded(&sched_row, scheduledoff_armed());
	settingsrow_duration_repaint(&sched_row);
}

static void sched_toggle_cb(lv_event_t *e) {
	(void)e;
	scheduledoff_set(lv_obj_has_state(sched_row.toggle, LV_STATE_CHECKED));
	sched_refresh();
}

static void sched_wheel_cb(lv_event_t *e) {
	(void)e;
	scheduledoff_set_minutes(settingsrow_duration_minutes(&sched_row));
}

static void power_loaded_cb(lv_event_t *e) {
	(void)e;
	settingsrow_duration_set_minutes(&sched_row, scheduledoff_minutes());
	sched_refresh();
}

void powersettings_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(powersettings_screen, cfg, "power");

	// What the battery went on, first: the row people come to this page for.
	batteryusage_init(cfg);
	settingsrow_add(container, "battery_usage", NULL, switch_screen_cb, batteryusage_screen);

	// Suspend-to-RAM standby: suspends the player a while after the screen goes
	// off to save battery, and playback resumes on wake. On by default. What it
	// waits for is the screen going dark, and that timeout lives on the Screen
	// page, beside the brightness it belongs with; how long it then waits is
	// the slider on this card, there only while standby is on.
	standby_card = settingsrow_toggle_slider(container, "power_standby_mem", STANDBY_AFTER_COUNT, &standby_switch,
											 &standby_value, &standby_slider, standby_toggled_cb,
											 standby_after_changed_cb);
	lv_slider_set_value(standby_slider, standby_after_index(), LV_ANIM_OFF);
	lv_obj_t *standby_note = lv_label_create(container);
	lv_label_set_long_mode(standby_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(standby_note, lv_pct(100));
	lv_obj_add_style(standby_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(standby_note, &font_ui_22, 0);
	lv_label_set_text(standby_note, tr("power_standby_mem_note"));

	// The R1 charges through its PMIC, which takes a lower target voltage and a
	// lower current rather than a percentage: two switches there, the slider
	// everywhere else.
	bool pmic = axpcharge_applies();
	if (pmic) {
		settingsrow_toggle(container, "power_charge_limit_80", &axp_limit_switch, axp_limit_toggled_cb);
		lv_obj_t *limit_note = lv_label_create(container);
		lv_label_set_long_mode(limit_note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(limit_note, lv_pct(100));
		lv_obj_add_style(limit_note, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(limit_note, &font_ui_22, 0);
		lv_label_set_text(limit_note, tr("power_charge_limit_80_note"));
		settingsrow_toggle(container, "power_charge_500ma", &axp_current_switch, axp_current_toggled_cb);
	} else {
		settingsrow_slider(container, "power_charge_limit", CHARGE_LIMIT_COUNT, &charge_value, &charge_slider,
						   charge_changed_cb);
		lv_slider_set_value(charge_slider, charge_index(), LV_ANIM_OFF);
	}

	// The LED master switch, and the standby option that depends on it.
	settingsrow_toggle(container, "power_led_on", &led_on_switch, led_on_toggled_cb);
	led_off_card = settingsrow_toggle(container, "power_led_off_in_standby", &led_off_switch, led_off_toggled_cb);
	// Only where the palette above is what the LED module registers.
	const sysinfo_model_t *model = sysinfo_model(); // NULL on an unknown device or the simulator
	if (model && model->ak4493) {
		build_led_colours_page(cfg);
		settingsrow_add(container, "power_led_colours", NULL, switch_screen_cb, led_colours_screen);
	}
	// No "turn Wi-Fi off in standby" entry: parking the radio is automatic, and
	// wifi_in_use() in power.c knows on its own when not to turn it off --
	// Qobuz, Tidal, podcasts, radio, AirPlay, DLNA, transfers.

	// The lock gesture for the buttons on the case (system.c).
	settingsrow_toggle(container, "power_lock_keys_screen_off", &key_lock_switch, key_lock_toggled_cb);
	lv_obj_t *key_lock_note = lv_label_create(container);
	lv_label_set_long_mode(key_lock_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(key_lock_note, lv_pct(100));
	lv_obj_add_style(key_lock_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(key_lock_note, &font_ui_22, 0);
	lv_label_set_text(key_lock_note, tr("power_lock_keys_screen_off_note"));

	// Toggle and "after how long" on one card: the slider is only there while
	// the option is on.
	auto_off_card = settingsrow_toggle_slider(container, "power_auto_power_off", AUTO_OFF_COUNT,
											  &auto_off_switch, &auto_off_value, &auto_off_slider,
											  auto_off_toggled_cb, auto_off_changed_cb);
	lv_slider_set_value(auto_off_slider, auto_off_index(), LV_ANIM_OFF);

	// The scheduled shutdown, beside the automatic one it is easily taken for:
	// that one waits for the player to be idle, this one does not.
	settingsrow_toggle_duration(container, "power_scheduled_off", sched_toggle_cb, sched_wheel_cb, &sched_row);
	settingsrow_duration_set_minutes(&sched_row, scheduledoff_minutes());
	sched_refresh();
	lv_obj_t *sched_note = lv_label_create(container);
	lv_label_set_long_mode(sched_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(sched_note, lv_pct(100));
	lv_obj_add_style(sched_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(sched_note, &font_ui_22, 0);
	lv_label_set_text(sched_note, tr("power_scheduled_off_note"));
	lv_obj_add_event_cb(powersettings_screen, power_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);

	settingsrow_slider(container, "power_idle_link", IDLE_LINK_COUNT, &idle_link_value, &idle_link_slider,
					   idle_link_changed_cb);
	lv_slider_set_value(idle_link_slider, idle_link_index(), LV_ANIM_OFF);
	lv_obj_t *idle_link_note = lv_label_create(container);
	lv_label_set_long_mode(idle_link_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(idle_link_note, lv_pct(100));
	lv_obj_add_style(idle_link_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(idle_link_note, &font_ui_22, 0);
	lv_label_set_text(idle_link_note, tr("power_idle_link_note"));

	// If the charger driver has no control node there is nothing to enforce
	// the limit with, and saying so is better than a setting that silently
	// does nothing.
	charge_note = lv_label_create(container);
	lv_label_set_long_mode(charge_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(charge_note, lv_pct(100));
	lv_obj_add_style(charge_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(charge_note, &font_ui_22, 0);
	lv_label_set_text(charge_note, pmic || power_charge_limit_supported() ? tr("power_auto_off_note")
																	   : tr("power_charge_limit_unavailable"));

	// Everything saved becomes effective now, at startup -- including the LED
	// standby option, which lives in led.c rather than power.c.
	powersettings_apply();
	refresh_labels();
}
