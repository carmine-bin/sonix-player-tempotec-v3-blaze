#include "gui.h"

#include <pthread.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/library/browser.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/settings/ccsettings.h"
#include "src/gui/nowplaying/chapters.h"
#include "src/gui/nowplaying/lyricsearch.h"
#include "src/gui/wireless/airplay.h"
#include "src/gui/bluetooth/airpodspage.h"
#include "src/gui/settings/appearance.h"
#include "src/gui/bluetooth/btaudio.h"
#include "src/gui/bluetooth/btsettings.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/scheduledoff.h"
#include "src/system/core/bootprof.h"
#include "src/system/core/wakeprof.h"
#include "src/gui/shell/corners.h"
#include "src/gui/settings/bootlogopage.h"
#include "src/gui/settings/kblayoutpage.h"
#include "src/gui/settings/screensaver.h"
#include "src/gui/library/audiobookextras.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/settings/devoptions.h"
#include "src/gui/ebook/ebookmarkspage.h"
#include "src/gui/ebook/ebookpage.h"
#include "src/gui/ebook/ebookreader.h"
#include "src/gui/ebook/ebooksettings.h"
#include "src/gui/settings/processespage.h"
#include "src/gui/settings/language.h"
#include "src/gui/library/libraryscan.h"
#include "src/gui/wireless/dlna.h"
#include "src/gui/wireless/sonixlink.h"
#include "src/gui/gearboy/gearboypage.h"
#include "src/gui/gearboy/gearboysettings.h"
#include "src/gui/flappybird/flappybird.h"
#include "src/gui/gearboy/gearboyplay.h"
#include "src/gui/shell/lowbattery.h"
#include "src/gui/shell/main_menu.h"
#include "src/gui/library/medialist.h"
#include "src/gui/library/filespage.h"
#include "src/gui/library/imageview.h"
#include "src/gui/library/textview.h"
#include "src/gui/shell/morepage.h"
#include "src/gui/shell/popover.h"
#include "src/gui/nowplaying/trackmenu.h"
#include "src/gui/library/music.h"
#include "src/gui/audio/eqsettings.h"
#include "src/gui/audio/peqsettings.h"
#include "src/gui/audio/peqautoeq.h"
#include "src/gui/settings/remap.h"
#include "src/gui/audio/msebsettings.h"
#include "src/gui/settings/musicsettings.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/library/playlistpage.h"
#include "src/gui/settings/powersettings.h"
#include "src/gui/shell/powermenu.h"
#include "src/gui/shell/quickpanel.h"
#include "src/gui/bluetooth/btreceiverpage.h"
#include "src/gui/audio/dacpage.h"
#include "src/gui/audio/metronomepage.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/audio/usbdac.h"
#include "src/system/net/wifitransfer.h"
#include "src/system/streaming/radio.h"
#include "src/gui/streaming/radiopage.h"
#include "src/gui/library/search.h"
#include "src/gui/settings/settings.h"
#include "src/gui/settings/systempage.h"
#include "src/gui/shell/setupwifi.h"
#include "src/gui/shell/welcome.h"
#include "src/gui/streaming/streaming.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/settings/timeset.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/volume_overlay.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/gui/wireless/wifitransfer.h"
#include "src/gui/wireless/wireless.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"
#include "src/system/device/power.h"

#include "lvgl/lvgl.h"
#include "lvgl/src/display/lv_display_private.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/core/lang.h"
#include "src/gui/shell/uiscale.h"

int32_t ui_panel_width = UI_REFERENCE_WIDTH;

// Long enough for a whole sentence: a notice truncated mid-word is a puzzle
// rather than a warning.
typedef struct {
	char text[256];
	const lv_image_dsc_t *icon; // NULL for a plain notice
	lv_color_t icon_color;
} popup_event_t;

static lv_obj_t *popup_veil; // the dimmed screen behind the card
static lv_obj_t *popup;		 // the card itself, inside the veil
static lv_obj_t *popup_icon;
static lv_obj_t *popup_label;
static lv_timer_t *popup_timer;

// ---------------------------------------------------------------------------
// gui_post: the bridge from any thread onto the GUI thread.
//
// lv_async_call() is not that bridge: without LV_USE_OS (not enabled here) LVGL
// takes no lock, and lv_async_call appends a timer to the timer list -- the same
// list lv_timer_handler() is walking on the GUI thread at that moment. Two
// threads on one unlocked linked list dereference garbage sooner or later.
//
// This bridge is built to be crossed from outside: a queue behind its own
// POSIX mutex (not an LVGL one) and a single timer, created once by the GUI
// thread, that drains it. Posters never touch LVGL.
// ---------------------------------------------------------------------------

#define GUI_POST_MAX 32


typedef struct {
	void (*cb)(void *);
	void *user;
	char thread[16]; // the poster's name, kept only for wakeprof.c
} gui_post_t;

static pthread_mutex_t post_lock = PTHREAD_MUTEX_INITIALIZER;
static gui_post_t post_ring[GUI_POST_MAX];
static int post_head;
static int post_count;
static lv_timer_t *post_timer;

// ---------------------------------------------------------------------------
// The doorbell
// ---------------------------------------------------------------------------
//
// The poster rings rather than the drainer polling. gui_post() writes a byte to
// an eventfd after enqueuing, and the main loop waits on that descriptor with
// poll() instead of sleeping with usleep(). An idle queue wakes nobody; a filled
// queue wakes the loop at once, so posted work runs as soon as poll() returns.
//
// A polling drain timer would also cap how long the main loop can sleep, because
// lv_timer_handler() returns the time until the next timer, keeping main.c's
// 250 ms cap out of reach.
//
// The timer stays, but slow (POST_TIMER_MS), as the safety net for an eventfd
// that could not be opened -- a kernel without it, or exhausted descriptors --
// and for a lost ring, which should not happen but must not be able to wedge
// the queue forever.
#define POST_TIMER_MS 1000

// Timer period when there is no doorbell: back to polling.
#define POST_TIMER_FALLBACK_MS 30

static int post_wake_fd = -1;

int gui_post_wake_fd(void) { return post_wake_fd; }

void gui_post_service(void) {
	// The doorbell rang, but the drain timer is a second from expiring, so
	// without this the posted work would sit there. Marking it ready runs it on
	// the next pass of lv_timer_handler(), which is immediate, and this is the
	// only way to touch the timer from the GUI thread, where this runs.
	if (post_timer) {
		lv_timer_ready(post_timer);
	}
}

bool gui_post(void (*cb)(void *), void *user) {
	if (!cb) {
		return false;
	}
	bool queued = false;
	gui_post_t entry = {cb, user, ""};
	if (wakeprof_on) {
		prctl(PR_GET_NAME, entry.thread, 0, 0, 0); // 16 bytes, as the field is
	}
	pthread_mutex_lock(&post_lock);
	if (post_count < GUI_POST_MAX) {
		post_ring[(post_head + post_count) % GUI_POST_MAX] = entry;
		post_count++;
		queued = true;
	} else {
		// Full: drop it and say so. A lost notice beats an unbounded queue
		// while the GUI is stalled.
		fprintf(stderr, "gui: message queue full, dropping one\n");
	}
	pthread_mutex_unlock(&post_lock);

	// Outside the lock: the write is one byte to a non-blocking eventfd, but a
	// poster must not hold the lock a moment longer than needed -- it is almost
	// always a thread that has just finished a long job the GUI may want at once.
	if (queued && post_wake_fd >= 0) {
		uint64_t one = 1;
		ssize_t ignored = write(post_wake_fd, &one, sizeof(one));
		(void)ignored;
	}
	return queued;
}

static void post_drain_cb(lv_timer_t *timer) {
	(void)timer;

	// Reading the eventfd clears its counter. It happens here rather than in the
	// main loop so it also applies when the safety-net timer did the waking, and
	// the queue is drained exactly once either way.
	if (post_wake_fd >= 0) {
		uint64_t drained;
		ssize_t ignored = read(post_wake_fd, &drained, sizeof(drained));
		(void)ignored;
	}

	for (;;) {
		pthread_mutex_lock(&post_lock);
		if (post_count == 0) {
			pthread_mutex_unlock(&post_lock);
			return;
		}
		gui_post_t entry = post_ring[post_head];
		post_head = (post_head + 1) % GUI_POST_MAX;
		post_count--;
		pthread_mutex_unlock(&post_lock);

		// Outside the lock: the callback may post in turn.
		if (wakeprof_on) {
			wakeprof_note_post((void *)entry.cb, entry.thread);
		}
		entry.cb(entry.user);
	}
}

// The dimmed screen every notice card sits on: black at 60%, no drop shadow
// anywhere. Dimming the page separates the card from it without drawing a dark
// smear around the corners that reads as a border.
//
// `dismissable` decides whether it eats taps and stops there (the pairing
// notice, which has to stay until the headphones are up) or whether the caller
// hangs a click handler on it.
static lv_obj_t *veil_create(bool dismissable) {
	(void)dismissable; // both kinds swallow taps; only the handler differs

	lv_obj_t *veil = lv_obj_create(lv_layer_top());
	lv_obj_set_hidden(veil, true);
	lv_obj_set_size(veil, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(veil, 0, 0);
	lv_obj_set_style_bg_color(veil, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(veil, LV_OPA_60, 0);
	lv_obj_set_style_border_width(veil, 0, 0);
	lv_obj_set_style_radius(veil, 0, 0);
	lv_obj_set_style_shadow_width(veil, 0, 0);
	lv_obj_set_style_pad_all(veil, 0, 0);
	lv_obj_set_scrollable(veil, false);
	lv_obj_set_clickable(veil, true);
	return veil;
}

static void popup_dismiss(void) {
	if (popup_veil) {
		lv_obj_set_hidden(popup_veil, true);
	}

	lv_timer_reset(popup_timer);
	lv_timer_pause(popup_timer);
}

static void popup_hide_cb(lv_timer_t *timer) {
	(void)timer;
	popup_dismiss();
}

// A tap anywhere off the card takes the notice away without waiting out the
// three seconds. Taps on the card itself do not get here: it is clickable, so
// it swallows them.
static void popup_veil_cb(lv_event_t *e) {
	(void)e;
	popup_dismiss();
}

static void popup_show_icon(const char *text, const lv_image_dsc_t *icon, lv_color_t color);

void popup_show(const char *text) { popup_show_icon(text, NULL, lv_color_black()); }

static void popup_show_icon(const char *text, const lv_image_dsc_t *icon, lv_color_t color) {
	if (!popup) {
		fprintf(stderr, "popup_show() was called, but popup is not initialized. this is unexpected...");
		return;
	}

	// A glyph over the sentence, for the notices that are about a thing rather
	// than about a failure: the headphones for "the volume lives over there".
	if (popup_icon) {
		if (icon) {
			lv_image_set_src(popup_icon, icon);
			lv_obj_set_style_image_recolor(popup_icon, color, 0);
			lv_obj_set_style_image_recolor_opa(popup_icon, LV_OPA_COVER, 0);
			lv_obj_set_hidden(popup_icon, false);
		} else {
			lv_obj_set_hidden(popup_icon, true);
		}
	}

	lv_label_set_text(popup_label, tr(text));
	// The card grows to whatever it has to say, rather than being a fixed box
	// a longer message spills out of.
	lv_obj_update_layout(popup);
	lv_obj_center(popup);
	lv_obj_move_foreground(popup_veil);
	lv_obj_set_hidden(popup_veil, false);
	lv_timer_reset(popup_timer);
	lv_timer_resume(popup_timer);
}

static void popup_async_cb(void *user_data) {
	popup_event_t *ev = user_data;

	popup_show_icon(ev->text, ev->icon, ev->icon_color);

	free(ev);
}

static void popup_notify(const char *text, const lv_image_dsc_t *icon, lv_color_t color) {
	popup_event_t *ev = malloc(sizeof(*ev));
	if (!ev) {
		return;
	}

	strncpy(ev->text, text, sizeof(ev->text) - 1);
	ev->text[sizeof(ev->text) - 1] = '\0';
	ev->icon = icon;
	ev->icon_color = color;
	// gui_post rather than lv_async_call: popups arrive from any thread
	// (bluetooth, usb, Qobuz downloads), and lv_async_call from off the GUI
	// thread corrupts the timer list. See gui_post.
	gui_post(popup_async_cb, ev);
}

void gui_notify_popup(const char *text) { popup_notify(text, NULL, lv_color_black()); }

bool gui_card_available(void) {
	if (usb_storage_active()) {
		return false;
	}
	// Not the block device name: that is remembered from the last card and
	// survives the card being pulled out.
	return storage_card_attached();
}

void gui_notify_no_card(void) { gui_notify_popup(usb_storage_active() ? "usb_storage_shared" : "sd_card_missing"); }

void gui_notify_popup_icon(const char *text, const lv_image_dsc_t *icon, lv_color_t color) {
	popup_notify(text, icon, color);
}

// ---------------------------------------------------------------------------
// The modal: a notice that stays until whatever it is about is over
//
// The popup above is a three-second toast. Some things take longer -- bringing
// headphones up takes the better part of half a minute -- and a toast that says
// "pairing" and then vanishes says nothing about what came of it.
//
// So this one has no timer. It goes up, and it comes down when the caller says
// so. The backdrop swallows taps deliberately: while a pairing is in flight
// there is nothing useful to tap.
// ---------------------------------------------------------------------------

static lv_obj_t *modal_backdrop;
static lv_obj_t *modal_icon;
static lv_obj_t *modal_title;
static lv_obj_t *modal_subtitle;
static lv_obj_t *modal_bar;

void gui_modal_show(const lv_image_dsc_t *icon, lv_color_t color, const char *title, const char *subtitle) {
	if (!modal_backdrop) {
		return;
	}

	if (icon) {
		lv_image_set_src(modal_icon, icon);
		lv_obj_set_style_image_recolor(modal_icon, color, 0);
		lv_obj_set_style_image_recolor_opa(modal_icon, LV_OPA_COVER, 0);
		lv_obj_set_hidden(modal_icon, false);
	} else {
		lv_obj_set_hidden(modal_icon, true);
	}

	lv_label_set_text(modal_title, title ? tr(title) : "");

	if (subtitle && subtitle[0]) {
		lv_label_set_text(modal_subtitle, tr(subtitle));
		lv_obj_set_hidden(modal_subtitle, false);
	} else {
		lv_obj_set_hidden(modal_subtitle, true);
	}

	// Every modal starts without one: a bar left over from the last job would
	// sit at whatever share that one reached.
	gui_modal_progress(-1);

	lv_obj_move_foreground(modal_backdrop);
	lv_obj_set_hidden(modal_backdrop, false);
}

void gui_modal_progress(int percent) {
	if (!modal_bar) {
		return;
	}
	if (percent < 0) {
		lv_obj_set_hidden(modal_bar, true);
		return;
	}
	lv_obj_set_hidden(modal_bar, false);
	// No animation: the value arrives several times a second already, and an
	// animation between two of them only makes the bar lag behind the work.
	lv_bar_set_value(modal_bar, percent > 100 ? 100 : percent, LV_ANIM_OFF);
}

void gui_modal_hide(void) {
	if (modal_backdrop) {
		lv_obj_set_hidden(modal_backdrop, true);
	}
}

bool gui_modal_visible(void) {
	return modal_backdrop && !lv_obj_is_hidden(modal_backdrop);
}

static bool tree_holds(const lv_obj_t *parent, const lv_obj_t *obj) {
	uint32_t count = lv_obj_get_child_count(parent);
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_t *child = lv_obj_get_child(parent, (int32_t)i);
		if (child == obj || tree_holds(child, obj)) {
			return true;
		}
	}
	return false;
}

// The screens, which include the top, system and bottom layers, are reached
// through the private display struct: LVGL has no public way to list them.
bool gui_obj_alive(const lv_obj_t *obj) {
	if (!obj) {
		return false;
	}
	for (lv_display_t *disp = lv_display_get_next(NULL); disp; disp = lv_display_get_next(disp)) {
		for (uint32_t i = 0; i < disp->screen_cnt; i++) {
			if (disp->screens[i] == obj || tree_holds(disp->screens[i], obj)) {
				return true;
			}
		}
	}
	return false;
}

static void modal_init(gui_config_t *cfg) {
	// No click handler on this one: it is the notice that has to stay until
	// what it is about is finished, so a stray tap must not take it away.
	modal_backdrop = veil_create(false);

	lv_obj_t *card = lv_obj_create(modal_backdrop);
	lv_obj_set_width(card, cfg->screen_width - 4 * cfg->padding);
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, ui_px(16), 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, ui_px(26), 0);
	lv_obj_set_style_pad_row(card, ui_px(14), 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_center(card);

	modal_icon = lv_image_create(card);
	lv_image_set_src(modal_icon, &icon_bluetooth_connecting);
	lv_obj_add_style(modal_icon, &theme_style_icon, 0);

	modal_title = lv_label_create(card);
	lv_obj_set_width(modal_title, lv_pct(100));
	lv_label_set_long_mode(modal_title, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(modal_title, &theme_style_text, 0);
	lv_obj_set_style_text_font(modal_title, &font_ui_24, 0);
	lv_obj_set_style_text_align(modal_title, LV_TEXT_ALIGN_CENTER, 0);

	modal_subtitle = lv_label_create(card);
	lv_obj_set_width(modal_subtitle, lv_pct(100));
	lv_label_set_long_mode(modal_subtitle, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(modal_subtitle, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(modal_subtitle, &font_ui_20, 0);
	lv_obj_set_style_text_align(modal_subtitle, LV_TEXT_ALIGN_CENTER, 0);

	// Hidden until something asks for it, and hidden rather than absent so a
	// job that has a length does not have to build one mid-flight.
	modal_bar = lv_bar_create(card);
	lv_obj_set_width(modal_bar, lv_pct(100));
	lv_obj_set_height(modal_bar, ui_px(8));
	lv_bar_set_range(modal_bar, 0, 100);
	lv_bar_set_value(modal_bar, 0, LV_ANIM_OFF);
	lv_obj_set_style_radius(modal_bar, ui_px(4), 0);
	lv_obj_set_style_radius(modal_bar, ui_px(4), LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(modal_bar, theme()->accent, LV_PART_INDICATOR);
	lv_obj_set_hidden(modal_bar, true);
}

// --- notifications from the button threads ---

static void key_async_cb(void *user_data) {
	gui_key_t key = (gui_key_t)(uintptr_t)user_data;

	// Ahead of the transport checks below: the gain is the DAC's, and DAC mode
	// is where it is wanted most.
	if (key == GUI_KEY_GAIN) {
		static const char *const NAMES[3] = {"musicsettings_gain_low", "musicsettings_gain_mid",
											 "musicsettings_gain_high"};
		musicsettings_gain_step();
		// Two-level players have low and high, no mid.
		int count = gain_level_count();
		int name = count > 1 ? musicsettings_gain_level() * 2 / (count - 1) : 0;
		char text[96];
		snprintf(text, sizeof(text), "%s: %s", tr("musicsettings_gain"), tr(NAMES[name < 0 ? 0 : (name > 2 ? 2 : name)]));
		popup_show_icon(text, NULL, lv_color_black());
		return;
	}

	// The side keys act on the local transport, and there are states in which
	// that transport is not what the device is doing. Pressing them then does
	// not do nothing -- it starts a local track underneath whatever is actually
	// going on, which is worse than nothing.
	if (usbdac_is_active() || wifitransfer_running()) {
		return;
	}
	// The same while the metronome holds the output.
	if (metronomepage_key(key)) {
		return;
	}

	// Receiver mode is the exception, and the only one: the press still means
	// what it says, it just has somewhere else to go. The music is on the phone
	// at the other end, so the key is sent there over AVRCP -- which is exactly
	// what a pair of headphones does with its own buttons, and this device is
	// the headphones now.
	switch (key) {
	case GUI_KEY_PLAY_PAUSE:
		if (btreceiver_key(BTRECEIVER_PLAY_PAUSE)) {
			return;
		}
		break;
	case GUI_KEY_NEXT:
		if (btreceiver_key(BTRECEIVER_NEXT)) {
			return;
		}
		break;
	case GUI_KEY_PREV:
		if (btreceiver_key(BTRECEIVER_PREV)) {
			return;
		}
		break;
	case GUI_KEY_GAIN: // handled above
		break;
	}

	switch (key) {
	case GUI_KEY_PLAY_PAUSE:
		player_key_play_pause();
		break;
	case GUI_KEY_NEXT:
		player_key_next();
		break;
	case GUI_KEY_PREV:
		player_key_prev();
		break;
	case GUI_KEY_GAIN:
		break;
	}
}

// gui_post, not lv_async_call: this arrives from the AVRCP reader thread that
// system.c starts for a connected Bluetooth remote. lv_async_call appends to
// the timer list the GUI thread is walking, unlocked -- see the bridge at the
// top of this file. The same list carries the player's progress timer, the one
// that advances the queue when a track ends.
void gui_notify_key(gui_key_t key) { gui_post(key_async_cb, (void *)(uintptr_t)key); }

// From the power tick, already on this thread. Each closed the way its own page
// closes it, so the page left on screen agrees with what happened.
void gui_close_idle_links(void) {
	if (usbdac_is_active()) {
		usbdac_stop(); // the DAC page's switch follows the state
	}
	if (wifitransfer_get_enabled() || wifitransfer_running()) {
		wifitransfer_page_stop();
	}
	if (btreceiver_is_active()) {
		btreceiverpage_close();
	}
	gui_notify_popup("power_idle_link_closed");
}

static void volume_async_cb(void *user_data) {
	int percent = (int)(intptr_t)user_data;
	volume_overlay_show(percent);
	topbar_refresh_volume(percent);
}

// From the AVRCP reader thread and from btvolume when the headphones move the
// level themselves, so across the bridge like the key above.
void gui_notify_volume(int percent) { gui_post(volume_async_cb, (void *)(intptr_t)percent); }

static void power_menu_async_cb(void *unused) {
	(void)unused;
	powermenu_show();
}

void gui_notify_power_menu(void) { gui_post(power_menu_async_cb, NULL); }


// One page built, and a mark for where the start went (bootprof.h).
#define BUILD(init)                                                                                                    \
	do {                                                                                                               \
		init(cfg);                                                                                                     \
		bootprof_mark(#init);                                                                                          \
	} while (0)

void gui_init(gui_config_t *cfg) {
	bootprof_group("gui");
	// Colours first: every screen below styles itself from the active palette.
	theme_init();

	// The player sheet is parked off the right edge of the top layer, which
	// makes that layer scrollable unless told otherwise -- and then a sideways
	// drag scrolls the whole layer, taking the status bar and the floating back
	// button along with it.
	lv_obj_set_scrollable(lv_layer_top(), false);
	lv_obj_set_scrollbar_mode(lv_layer_top(), LV_SCROLLBAR_MODE_OFF);

	main_menu_screen = lv_obj_create(NULL);
	music_screen = lv_obj_create(NULL);
	player_screen = lv_obj_create(lv_layer_top()); // a sheet, not a page: see player.h
	browser_screen = lv_obj_create(NULL);
	settings_screen = lv_obj_create(NULL);
	devoptions_screen = lv_obj_create(NULL);
	processespage_screen = lv_obj_create(NULL);
	systempage_screen = lv_obj_create(NULL);
	sysinfo_screen = lv_obj_create(NULL);
	libraryscan_screen = lv_obj_create(NULL);
	musicsettings_screen = lv_obj_create(NULL);
	powersettings_screen = lv_obj_create(NULL);
	appearance_screen = lv_obj_create(NULL);
	language_screen = lv_obj_create(NULL);
	medialist_screen = lv_obj_create(NULL);
	medialist_tracks_screen = lv_obj_create(NULL);
	medialist_albums_screen = lv_obj_create(NULL);
	audiobooks_screen = lv_obj_create(NULL);
	audiobooksettings_screen = lv_obj_create(NULL);
	audiobookcontrols_screen = lv_obj_create(NULL);
	audiobookscan_screen = lv_obj_create(NULL);
	chapters_screen = lv_obj_create(NULL);
	lyricsearch_screen = lv_obj_create(NULL);
	queue_screen = lv_obj_create(NULL);
	details_screen = lv_obj_create(NULL);
	wireless_screen = lv_obj_create(NULL);
	streaming_screen = lv_obj_create(NULL);
	radiopage_screen = lv_obj_create(NULL);
	dacpage_screen = lv_obj_create(NULL);
	radiolist_screen = lv_obj_create(NULL);
	radiosearch_screen = lv_obj_create(NULL);
	wifisettings_screen = lv_obj_create(NULL);
	btsettings_screen = lv_obj_create(NULL);
	btaudio_screen = lv_obj_create(NULL);
	btreceiverpage_screen = lv_obj_create(NULL);
	airpodspage_screen = lv_obj_create(NULL);
	wifitransfer_screen = lv_obj_create(NULL);
	airplay_screen = lv_obj_create(NULL);
	dlna_screen = lv_obj_create(NULL);
	sonixlink_screen = lv_obj_create(NULL);
	morepage_screen = lv_obj_create(NULL);
	filespage_screen = lv_obj_create(NULL);
	textview_screen = lv_obj_create(NULL);
	imageview_screen = lv_obj_create(NULL);
	gearboypage_screen = lv_obj_create(NULL);
	gearboyplay_screen = lv_obj_create(NULL);
	flappybird_screen = lv_obj_create(NULL);
	metronomepage_screen = lv_obj_create(NULL);

	bootprof_mark("theme, the empty screens");

	// Persistent topbar that stays above every screen.
	BUILD(topbar_init);

	// Persistent back button that hides when on the main page
	BUILD(back_btn_init);

	// Before the settings pages: the display page draws its screensaver switch
	// from screensaver_get_enabled(), which is only true once this has read the
	// config. Built the other way round, a screensaver saved as on comes up
	// showing an off switch.
	BUILD(screensaver_init);

	// The rounded screen corners, if they are on: above everything, including
	// the screensaver, which is why they live on the system layer.
	BUILD(corners_init);

	BUILD(main_menu_init);

	BUILD(music_init);
	BUILD(coverflow_init);

	// wireless section: the two radio pages first, so the section's tiles
	// have somewhere to point and the control centre's round buttons have a
	// page to open on a long press.
	BUILD(wifisettings_init);
	BUILD(wifitransfer_page_init); // after wifisettings: its "no Wi-Fi" row points there
	BUILD(airplay_page_init);		 // likewise
	BUILD(dlna_page_init);		 // likewise: its "no Wi-Fi" row points there too
	BUILD(sonixlink_page_init);	 // likewise
	BUILD(airpodspage_init);		 // before btaudio: its AirPods row points at this page
	BUILD(btaudio_init);			 // before btsettings: its gear points at this page
	BUILD(btreceiverpage_init);	 // likewise, for the button beside the gear
	BUILD(btsettings_init);
	BUILD(wireless_init);

	// streaming section: three services that are not implemented yet, so the
	// page exists and is walkable but its tiles say so when tapped.
	// radio before the section, so its tile has somewhere to point.
	BUILD(radiopage_init);
	BUILD(dacpage_init);
	// After dacpage: the More section points at its pages.
	BUILD(gearboyplay_init);  // before the list: one of its rows leads here
	BUILD(gearboysettings_init); // before the page: its gear opens this
	BUILD(gearboypage_init);
	BUILD(flappybird_init);
	BUILD(metronomepage_init);
	// before morepage: the books tile opens the shelf, and the shelf opens the
	// reader.
	BUILD(ebookreader_init);
	// before ebookpage: its corner button opens the bookmarks
	BUILD(ebookmarkspage_init);
	BUILD(ebooksettings_init); // likewise: the other corner button opens this
	BUILD(ebookpage_init);
	BUILD(morepage_init);
	BUILD(textview_init);	 // before filespage: a text file tapped there opens here
	BUILD(imageview_init); // likewise, for a picture
	BUILD(filespage_init);
	BUILD(streaming_init);

	BUILD(player_init);

	BUILD(browser_init);

	// remap_init before settings_init: the key-remap row carries the pointer to
	// its screen, which has to exist already. The control centre's order page
	// is reached from the same page and has to exist for the same reason.
	BUILD(remap_init);
	BUILD(ccsettings_init);
	BUILD(kblayoutpage_init); // the keyboard-language row on the same page
	BUILD(settings_init);
	BUILD(devoptions_init);
	BUILD(processespage_init); // the processes page the devoptions entry opens
	// after devoptions: the system entry opens the page that can unlock it
	BUILD(systempage_init);
	BUILD(bootlogopage_init); // before appearance: its row carries the screen
	BUILD(appearance_init);
	BUILD(language_init);
	BUILD(libraryscan_init);
	BUILD(msebsettings_init); // before musicsettings: the MSEB page hangs its gear off it
	BUILD(eqsettings_init);	// and the equalizer page hangs its own off this
	BUILD(peqsettings_init); // the parametric page hangs its preset screen off the gear
	BUILD(peqautoeq_init);  // the waveform entry on that page searches AutoEq profiles
	BUILD(musicsettings_init);
	BUILD(search_init);
	BUILD(medialist_init);
	BUILD(playlistpage_init);
	BUILD(audiobookextras_init); // the bookmarks and summary pages
	BUILD(audiobooks_init);
	BUILD(chapters_init);
	BUILD(lyricsearch_init);
	BUILD(trackmenu_init);

	// The leftward swipe that pulls the player in works on the browsing pages
	// -- menu, library lists, file browser -- but NOT on the settings pages
	// nor on the queue: there a stray horizontal drag would yank the player
	// over what is being adjusted or read.
	lv_obj_t *const player_swipe_screens[] = {
		main_menu_screen,		 music_screen,		browser_screen,		medialist_screen,
		// details_screen is deliberately absent: its page is reached FROM the
		// player, so dragging it sideways to go back into the player would be
		// one gesture doing two contradictory things. The back swipe is the
		// only way out of it.
		medialist_tracks_screen, medialist_albums_screen, audiobooks_screen, wireless_screen,
		streaming_screen,		 radiopage_screen,	radiolist_screen,	radiosearch_screen,
		morepage_screen,
	};
	for (size_t i = 0; i < sizeof(player_swipe_screens) / sizeof(player_swipe_screens[0]); i++) {
		player_sheet_attach_drag(player_swipe_screens[i], true);
	}

	// The rightward swipe-back works everywhere a page can go back from,
	// settings and queue included. The scan pages stay out -- their only
	// exits are their own buttons -- and the handler itself skips the main
	// menu.
	lv_obj_t *const back_swipe_screens[] = {
		main_menu_screen, music_screen,			browser_screen,	  settings_screen,
		devoptions_screen, processespage_screen, musicsettings_screen, powersettings_screen, appearance_screen,
		language_screen,
		medialist_screen,  medialist_tracks_screen, medialist_albums_screen, audiobooks_screen, audiobooksettings_screen,
		queue_screen,	  chapters_screen,		 details_screen,   audiobookcontrols_screen,
		lyricsearch_screen,
		wireless_screen,
		wifisettings_screen,
		btsettings_screen, btaudio_screen,	 airpodspage_screen, airplay_screen,   streaming_screen,
		radiopage_screen,  radiolist_screen, radiosearch_screen,
		dlna_screen,	   sonixlink_screen,	 morepage_screen,	  gearboypage_screen, metronomepage_screen,
	};
	for (size_t i = 0; i < sizeof(back_swipe_screens) / sizeof(back_swipe_screens[0]); i++) {
		switcher_attach_back_gesture(back_swipe_screens[i]);
	}

	bootprof_mark("swipe gestures");
	BUILD(quickpanel_init); // above the pages, under the popover/power menu
	popover_init(); // last: its veil must sit above the other top-layer chrome
	bootprof_mark("popover_init");
	BUILD(powersettings_init);
	BUILD(confirm_init);
	BUILD(toast_init);
	BUILD(lowbattery_init);
	BUILD(scheduledoff_init); // after confirm_init: its countdown is a confirm card

	// The screensaver is drawn both when the panel goes dark (so the wake opens
	// on it rather than on the page) and when it lights up again; and the
	// player tells it to let go of the artwork before freeing it, because a
	// track can change while the screensaver sits behind a blanked panel.
	power_set_wake_hook(screensaver_prepare);
	player_set_cover_release_cb(screensaver_release_artwork);

	// The notice card: the interface's card style, wrapping, and only as tall
	// as what it has to say.
	//
	// It sits on a veil rather than floating over the page on a drop shadow:
	// black at 60%, the same one the confirmation dialog uses, so a notice reads
	// as a notice across the whole interface. The veil is also what makes "tap
	// anywhere else to dismiss" possible.
	popup_veil = veil_create(true);
	lv_obj_add_event_cb(popup_veil, popup_veil_cb, LV_EVENT_CLICKED, NULL);

	popup = lv_obj_create(popup_veil);
	lv_obj_set_width(popup, cfg->screen_width - 4 * cfg->padding);
	lv_obj_set_height(popup, LV_SIZE_CONTENT);
	lv_obj_add_style(popup, &theme_style_card, 0);
	lv_obj_set_style_radius(popup, ui_px(16), 0);
	lv_obj_set_style_border_width(popup, 0, 0);
	lv_obj_set_style_shadow_width(popup, 0, 0);
	lv_obj_set_style_pad_all(popup, ui_px(22), 0);
	lv_obj_set_scrollable(popup, false);
	lv_obj_set_clickable(popup, true); // so a tap on the card is not a tap outside
	lv_obj_set_flex_flow(popup, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(popup, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(popup, ui_px(12), 0);
	lv_obj_center(popup);

	popup_icon = lv_image_create(popup);
	lv_image_set_src(popup_icon, &icon_headphones_big);
	lv_obj_add_style(popup_icon, &theme_style_icon, 0);
	lv_obj_set_hidden(popup_icon, true);

	popup_label = lv_label_create(popup);
	lv_obj_set_width(popup_label, lv_pct(100));
	lv_label_set_long_mode(popup_label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(popup_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(popup_label, &font_ui_22, 0);
	lv_obj_set_style_text_align(popup_label, LV_TEXT_ALIGN_CENTER, 0);

	// Three seconds: long enough to read a whole sentence, not just a two-word
	// notice.
	popup_timer = lv_timer_create(popup_hide_cb, 3000, NULL);
	lv_timer_pause(popup_timer); // nothing to hide until a notice is shown

	// The gui_post doorbell and the timer that drains it, created here, once, on
	// the GUI thread, because the timer is the only piece of LVGL in the whole
	// bridge. See POST_TIMER_MS: with the doorbell the timer is only a safety
	// net and can run slow; without it, it has to go back to polling
	// thirty-three times a second.
	//
	// EFD_NONBLOCK because neither poster nor drainer can afford to block here.
	post_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (post_wake_fd < 0) {
		perror("gui: eventfd");
	}
	post_timer = lv_timer_create(post_drain_cb, post_wake_fd >= 0 ? POST_TIMER_MS : POST_TIMER_FALLBACK_MS, NULL);
	// With the wake descriptor it is only the safety net (a post readies it and
	// the main loop drains the queue itself): no need for it every second in
	// standby. Without the descriptor it is the only way posts get served.
	if (post_wake_fd >= 0) {
		power_slow_in_standby(post_timer, 5000);
	}

	// The other kind of notice: the one that stays until it is over.
	BUILD(modal_init);

	// Volume HUD and the power menu, both above every page.
	BUILD(volume_overlay_init);
	BUILD(powermenu_init);

	// On a first boot the clock panel comes straight up over everything else:
	// with no network and an RTC that has never been written, the player has no
	// way to know the time, and nothing it timestamps can be trusted until it
	// does.
	BUILD(timeset_init);

	// A player that has never run asks two questions, in this order: which
	// language, then what time it is. That way the clock panel is already in
	// the language just chosen -- and being asked to dial in a date in a
	// language you cannot read is a poor way to meet a device. Confirming the
	// language brings the clock up itself, so the two only chain here on the
	// path where the language has already been answered before.
	BUILD(welcome_init);
	BUILD(setupwifi_init); // the step between the language and the clock

	// Ahead of everything else, and only on a device that has never been booted:
	// the greeting. Its button opens the language picker itself, which in turn
	// opens the clock, so only the first door is entered from here.
	if (language_needed()) {
		welcome_show_first_boot();
	} else if (timeset_needed()) {
		timeset_show();
	}

	switch_screen(main_menu_screen);
	bootprof_mark("popup, the rest of gui_init");
	bootprof_group("main");
}
