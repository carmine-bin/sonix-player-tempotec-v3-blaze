#include "scheduledoff.h"

#include <stdio.h>

#include "src/gui/shell/confirm.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"

#define DEFAULT_MINUTES 30
#define MAX_MINUTES (24 * 60)
#define COUNTDOWN_S 10

static lv_timer_t *countdown_timer;
static int countdown_left;

int scheduledoff_minutes(void) {
	long minutes = config_get_int("power", "scheduled_off_minutes", DEFAULT_MINUTES);
	if (minutes < 1) {
		return DEFAULT_MINUTES;
	}
	return minutes > MAX_MINUTES ? MAX_MINUTES : (int)minutes;
}

void scheduledoff_set_minutes(int minutes) {
	if (minutes < 1) {
		return; // the wheels at 0:00: nothing to count, keep the last length
	}
	config_set_int("power", "scheduled_off_minutes", minutes > MAX_MINUTES ? MAX_MINUTES : minutes);
	config_save();
	if (power_scheduled_off_armed()) {
		power_scheduled_off_start((uint32_t)scheduledoff_minutes());
	}
}

bool scheduledoff_armed(void) { return power_scheduled_off_armed(); }

void scheduledoff_set(bool on) {
	char text[160];
	if (on) {
		int minutes = scheduledoff_minutes();
		power_scheduled_off_start((uint32_t)minutes);
		snprintf(text, sizeof(text), tr("scheduled_off_started"), minutes);
		gui_notify_popup(text);
	} else if (power_scheduled_off_armed()) {
		power_scheduled_off_cancel();
		gui_notify_popup("scheduled_off_cancelled");
	}
}

void scheduledoff_left_text(char *out, size_t size) {
	uint32_t left_s = (power_scheduled_off_left_ms() + 999u) / 1000u;
	if (left_s < 60) {
		snprintf(out, size, "%u s", (unsigned)left_s);
	} else {
		snprintf(out, size, "%u min", (unsigned)((left_s + 59u) / 60u));
	}
}

// --- the countdown ---

static void countdown_stop(void) {
	if (countdown_timer) {
		lv_timer_delete(countdown_timer);
		countdown_timer = NULL;
	}
}

static void shutdown_now(void *user) {
	(void)user;
	countdown_stop();
	printf("power: scheduled shutdown: switching off\n");
	fflush(stdout);
	power_shutdown();
}

static void countdown_show(void) {
	char text[160];
	snprintf(text, sizeof(text), tr("scheduled_off_countdown"), countdown_left);
	confirm_show("power_scheduled_off", text, "scheduled_off_now", shutdown_now, NULL);
}

static void countdown_cb(lv_timer_t *timer) {
	(void)timer;
	// The card gone means "Cancelar" (or another question took its place):
	// the player stays on. "Apagar ahora" also closes it, but its action has
	// already been queued and switches off regardless.
	if (!confirm_is_open()) {
		printf("power: scheduled shutdown cancelled from the countdown\n");
		countdown_stop();
		return;
	}
	countdown_left--;
	if (countdown_left > 0) {
		countdown_show();
		return;
	}
	confirm_close();
	shutdown_now(NULL);
}

// power.c's hook: the time is up. The screen is lit if it was dark, and kept
// lit for the countdown: the idle timer counts from now.
static void time_up(void) {
	if (!power_screen_is_on()) {
		power_screen_on();
	}
	power_notify_activity();
	lv_display_trigger_activity(lv_display_get_default());
	countdown_stop();
	countdown_left = COUNTDOWN_S;
	countdown_show();
	countdown_timer = lv_timer_create(countdown_cb, 1000, NULL);
}

void scheduledoff_init(gui_config_t *cfg) {
	(void)cfg;
	power_set_scheduled_off_hook(time_up);
}
