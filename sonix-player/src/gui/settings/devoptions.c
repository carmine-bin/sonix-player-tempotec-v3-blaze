#include "devoptions.h"

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/settings/processespage.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/bluetooth/btlog.h"
#include "src/system/device/adb.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/core/logging.h"
#include "src/system/core/config.h"

lv_obj_t *devoptions_screen;

static lv_obj_t *adb_switch;
static lv_obj_t *log_switch;
static lv_obj_t *db_log_switch;
static lv_obj_t *log_name_label; // rises to make room for the path when logging
static lv_obj_t *log_path_label;
static lv_obj_t *bt_log_switch;
static lv_obj_t *bt_log_name_label;
static lv_obj_t *bt_log_path_label;
static lv_timer_t *adb_poll_timer;

// Turning ADB on or off happens on its own thread and takes a second or two:
// adbd has to appear, or be waited out and then insisted upon. The switch
// follows what is really running, so while that is in flight it has to be
// asked again rather than left showing the state from before the tap.
#define ADB_POLL_MS 700

// How long the switch keeps showing what was asked for before it gives up and
// shows what is there. Long enough for the slowest stop (two waits of two
// seconds, then the gadget taken apart), short enough that a request which
// never comes true cannot leave the switch lying.
#define ADB_PENDING_MAX_MS 8000

static bool adb_pending;	// a tap is still being carried out
static bool adb_wanted;		// what that tap asked for
static uint32_t adb_asked_at;

static void update_adb_row(void) {
	if (!adb_switch) {
		return;
	}

	bool running = adb_is_running();

	// Between the tap and the daemon agreeing with it there are one or two
	// seconds during which the truth is still the old state. Showing it pulls
	// the switch back under the user's finger, which reads as the toggle
	// refusing the tap -- so the request stands until it comes true.
	if (adb_pending) {
		if (running == adb_wanted || lv_tick_elaps(adb_asked_at) > ADB_PENDING_MAX_MS) {
			adb_pending = false;
		} else {
			running = adb_wanted;
		}
	}

	if (running) {
		lv_obj_add_state(adb_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(adb_switch, LV_STATE_CHECKED);
	}
}

// The path only means something while the log is actually being written.
// With logging on, the name moves up to the top of the row and the path
// slides in underneath as its subtitle; off, the row looks like any other.
static void show_path(lv_obj_t *toggle, lv_obj_t *name_label, lv_obj_t *path_label, bool on, const char *path) {
	if (toggle) {
		if (on) {
			lv_obj_add_state(toggle, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		}
	}

	if (path_label && name_label) {
		if (on) {
			lv_label_set_text(path_label, path);
			lv_obj_set_hidden(path_label, false);
			lv_obj_align(name_label, LV_ALIGN_TOP_LEFT, 0, 0);
		} else {
			lv_obj_set_hidden(path_label, true);
			lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 0, 0);
		}
	}
}

static void update_log_row(void) {
	bool on = logging_to_sd();
	show_path(log_switch, log_name_label, log_path_label, on, on ? logging_path() : "");
}

static void update_bt_log_row(void) {
	bool on = btlog_enabled();
	show_path(bt_log_switch, bt_log_name_label, bt_log_path_label, on, on ? btlog_path() : "");
}

// The path as the subtitle of a row: the first thing anybody needs to know
// when asked to send the log, and only shown while the log is being written.
static lv_obj_t *add_path_label(lv_obj_t *card) {
	lv_obj_t *label = lv_label_create(card);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(label, lv_pct(72));
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_18, 0);
	lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	return label;
}

// A note under a card rather than a subtitle inside it: a sentence about when
// to use the switch, too long to sit on the row without crowding the name.
static void add_note(lv_obj_t *container, const char *key) {
	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr(key));
}

static void adb_toggled_cb(lv_event_t *e) {
	(void)e;
	adb_wanted = lv_obj_has_state(adb_switch, LV_STATE_CHECKED);
	adb_pending = true;
	adb_asked_at = lv_tick_get();
	adb_set_enabled(adb_wanted);
}

static void log_toggled_cb(lv_event_t *e) {
	(void)e;
	logging_set_to_sd(lv_obj_has_state(log_switch, LV_STATE_CHECKED));
	update_log_row();
}

static void bt_log_toggled_cb(lv_event_t *e) {
	(void)e;
	btlog_set_enabled(lv_obj_has_state(bt_log_switch, LV_STATE_CHECKED));
	update_bt_log_row();
}

static void db_log_toggled_cb(lv_event_t *e) {
	(void)e;
	library_set_log_database(lv_obj_has_state(db_log_switch, LV_STATE_CHECKED));
}

static void adb_poll_cb(lv_timer_t *timer) {
	(void)timer;
	update_adb_row();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	update_adb_row();
	update_log_row();
	update_bt_log_row(); // the card may have come or gone since
	lv_timer_resume(adb_poll_timer);
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(adb_poll_timer);
}

void devoptions_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(devoptions_screen, cfg, "developer_options");

	settingsrow_toggle(container, "devoptions_adb", &adb_switch, adb_toggled_cb);

	lv_obj_t *log_card = settingsrow_toggle(container, "devoptions_log_to_microsd", &log_switch, log_toggled_cb);
	log_name_label = lv_obj_get_child(log_card, 0);
	log_path_label = add_path_label(log_card);

	// Under the log switch, because it only means anything with the log on: the
	// scan names every file it reads, which is how a crash during a scan can be
	// pinned to the file that caused it.
	settingsrow_toggle(container, "devoptions_database_log", &db_log_switch, db_log_toggled_cb);
	if (library_log_database()) {
		lv_obj_add_state(db_log_switch, LV_STATE_CHECKED);
	}
	add_note(container, "devoptions_dbtrace_note");

	// A file of its own, so it works with the log above off and is the one
	// thing to send about a device that will not connect.
	lv_obj_t *bt_card = settingsrow_toggle(container, "devoptions_bluetooth_log", &bt_log_switch, bt_log_toggled_cb);
	bt_log_name_label = lv_obj_get_child(bt_card, 0);
	bt_log_path_label = add_path_label(bt_card);
	add_note(container, "devoptions_bluetooth_log_note");

	// Opens the page showing RAM and running processes. A navigation row with a
	// chevron, like the developer options entry on the previous page.
	settingsrow_add(container, "processes", NULL, switch_screen_cb, processespage_screen);

	update_adb_row();
	update_log_row();
	update_bt_log_row();

	adb_poll_timer = lv_timer_create(adb_poll_cb, ADB_POLL_MS, NULL);
	lv_timer_pause(adb_poll_timer);

	lv_obj_add_event_cb(devoptions_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(devoptions_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
