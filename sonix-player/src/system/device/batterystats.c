#include "batterystats.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/system/audio/audio.h"
#include "src/system/audio/usbdac.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"
#include "src/system/remote/airplay.h"
#include "src/system/remote/dlna.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/radio.h"
#include "src/system/streaming/tidalcache.h"

// What each part draws, in % of the battery per hour. They only decide how a
// measured drop is shared out, never how big it is.
// shortcut: only screen-off local playback is measured (8.35-9.15 %/h, the
// panel-wake A/B of 2026-10-19); the rest are seeds. Refit them all from the
// "batterystats: -N%" log lines once a few days of normal use are in.
#define RATE_AWAKE 2.5f	 // the SoC up and nothing playing, screen dark
#define RATE_SCREEN 5.0f // the panel and the backlight, on top of that
#define RATE_SLEEP 0.5f	 // suspended to RAM
#define RATE_BT_OUT 2.0f // the radio sending to headphones, on top of the source
static const float RATE_PLAY[BS_SOURCE_COUNT] = {
	// On top of RATE_AWAKE: decoding, the DAC and the amplifier, and the
	// radio the music comes in on.
	[BS_LOCAL] = 6.2f, // the measured 8.7 %/h less RATE_AWAKE
	[BS_BT_RECEIVER] = 8.0f, [BS_USB_DAC] = 6.0f, [BS_RADIO] = 8.5f, [BS_QOBUZ] = 8.5f,
	[BS_TIDAL] = 8.5f,		 [BS_PODCAST] = 8.5f, [BS_AIRPLAY] = 8.5f, [BS_DLNA] = 8.5f,
};

// The short names of the log line, in bs_item_t order.
static const char *const ITEM_NAMES[BS_ITEM_COUNT] = {
	"local", "btrx", "usbdac", "radio", "qobuz", "tidal", "podcast", "airplay", "dlna", "screen", "standby", "sleep", "btout",
};

#define STATS_MAGIC 0x53425431u // "SBT1"
#define STATS_VERSION 1
#define SAVE_EVERY_MS (10 * 60 * 1000)
// Before this the clock has not been set (an RTC back at its epoch), and an
// hour would be filed under a day long gone. 2020-09-13.
#define CLOCK_SANE_MS 1600000000000LL
// A rise of this much with no cable seen is a charge all the same (one the
// charger state did not show); less is the gauge settling after a load.
#define LEVEL_RISE_IS_CHARGE 3

typedef struct {
	uint32_t magic;
	uint32_t version;
	int64_t since_ms;
	batterystats_hour_t hours[BS_HOURS];
} stats_file_t;

static stats_file_t s_stats;
static const char *s_path;
static uint64_t s_last_mono, s_last_boot, s_saved_mono;
static int s_ref_level = -1;				 // the level the next drop is measured from
static bool s_ref_fresh;					 // no drop yet since s_ref_level was taken
static uint64_t s_interval_ms[BS_ITEM_COUNT]; // per item, since the last drop: for the log line

static uint64_t clock_ms(clockid_t id) {
	struct timespec ts;
	clock_gettime(id, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static batterystats_hour_t *hour_slot(int32_t hour) {
	batterystats_hour_t *h = &s_stats.hours[hour % BS_HOURS];
	if (h->hour != hour) {
		memset(h, 0, sizeof(*h));
		h->hour = hour;
		h->level = -1;
	}
	return h;
}

// Adds [from, to) of wall-clock time to `item`, split at the hour boundaries,
// with the share of the battery the model gives it at `rate` %/h.
static void add_span(int item, int64_t from, int64_t to, float rate) {
	while (from < to) {
		int64_t end = (from / BS_HOUR_MS + 1) * BS_HOUR_MS;
		if (end > to) {
			end = to;
		}
		batterystats_hour_t *h = hour_slot((int32_t)(from / BS_HOUR_MS));
		h->ms[item] += (uint32_t)(end - from);
		h->pending[item] += rate * (float)(end - from) / (float)BS_HOUR_MS;
		s_interval_ms[item] += (uint64_t)(end - from);
		from = end;
	}
}

// Where the music is coming from, or -1 if none is playing. The radios and
// receivers first: they play through audio_external_*, which audio.c's own
// status says nothing about.
static int playing_source(void) {
	if (btreceiver_is_streaming()) {
		return BS_BT_RECEIVER;
	}
	usbdac_state_t dac;
	usbdac_get_state(&dac);
	if (dac.streaming) {
		return BS_USB_DAC;
	}
	if (radio_is_playing()) {
		return BS_RADIO;
	}
	airplay_state_t air;
	airplay_get_state(&air);
	if (air.playing) {
		return BS_AIRPLAY;
	}
	if (audio_get_status() != AUDIO_STATUS_PLAYING) {
		return -1;
	}
	if (dlna_owns_playback()) {
		return BS_DLNA;
	}
	char path[512];
	audio_get_current_file(path, sizeof(path));
	if (qobuzcache_owns(path)) {
		return BS_QOBUZ;
	}
	if (tidalcache_owns(path)) {
		return BS_TIDAL;
	}
	if (podcastcache_owns(path)) {
		return BS_PODCAST;
	}
	return BS_LOCAL;
}

// The model's figures since the last drop stand as they are: there is no drop
// to measure them against (the cable went in, or the counting starts over).
static void keep_pending(void) {
	for (int i = 0; i < BS_HOURS; i++) {
		batterystats_hour_t *h = &s_stats.hours[i];
		for (int k = 0; k < BS_ITEM_COUNT; k++) {
			h->used[k] += h->pending[k];
			h->pending[k] = 0;
		}
	}
	memset(s_interval_ms, 0, sizeof(s_interval_ms));
}

// The level fell by `drop` %: that is what everything since the last drop
// took, shared out in the proportions the model gave each.
static void settle(int drop) {
	float total = 0;
	for (int i = 0; i < BS_HOURS; i++) {
		for (int k = 0; k < BS_ITEM_COUNT; k++) {
			total += s_stats.hours[i].pending[k];
		}
	}
	for (int i = 0; i < BS_HOURS && total > 0; i++) {
		batterystats_hour_t *h = &s_stats.hours[i];
		for (int k = 0; k < BS_ITEM_COUNT; k++) {
			h->used[k] += h->pending[k] * (float)drop / total;
			h->pending[k] = 0;
		}
	}

	// One line per drop, the data a refit of RATE_* is made from. The first
	// after a cable or a restart started somewhere inside a percent: a fit
	// leaves it out.
	char line[384];
	uint64_t span = s_interval_ms[BS_SCREEN] + s_interval_ms[BS_STANDBY] + s_interval_ms[BS_SLEEP];
	int n = snprintf(line, sizeof(line), "batterystats: -%d%% in %llu s%s:", drop, (unsigned long long)(span / 1000),
					 s_ref_fresh ? " (first)" : "");
	for (int k = 0; k < BS_ITEM_COUNT && n > 0 && (size_t)n < sizeof(line); k++) {
		if (s_interval_ms[k] >= 1000) {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %s=%llu", ITEM_NAMES[k],
						  (unsigned long long)(s_interval_ms[k] / 1000));
		}
	}
	printf("%s\n", line); // not puts(): glibc 2.22 puts() skips the stamped stdout (see Makefile)
	memset(s_interval_ms, 0, sizeof(s_interval_ms));
	s_ref_fresh = false;
}

static void track_level(int level, bool powered) {
	if (powered) {
		s_ref_level = -1;
		keep_pending();
		return;
	}
	if (level < 0) {
		return;
	}
	if (s_ref_level < 0 || level >= s_ref_level + LEVEL_RISE_IS_CHARGE) {
		keep_pending();
		s_ref_level = level;
		s_ref_fresh = true;
	} else if (level < s_ref_level) {
		settle(s_ref_level - level);
		s_ref_level = level;
	}
	// A rise of a percent or two is the gauge recovering after a load: the
	// reference stays where it was, or the same percent would be counted twice.
}

void batterystats_init(void) {
#ifdef HOST_BUILD
	s_path = getenv("SONIX_BATTERY_STATS"); // unset: kept in memory for the session
#else
	s_path = "/usr/data/battery_stats.bin";
#endif
	FILE *f = s_path ? fopen(s_path, "rb") : NULL;
	if (f) {
		static stats_file_t in;
		if (fread(&in, 1, sizeof(in), f) == sizeof(in) && in.magic == STATS_MAGIC && in.version == STATS_VERSION) {
			s_stats = in;
		}
		fclose(f);
	}
	s_stats.magic = STATS_MAGIC;
	s_stats.version = STATS_VERSION;
	// Unsettled shares from before a restart stay as the model put them: the
	// level the next drop would be measured from is gone with the restart.
	keep_pending();
}

void batterystats_tick(bool screen_on) {
	uint64_t mono = clock_ms(CLOCK_MONOTONIC);
	uint64_t boot = clock_ms(CLOCK_BOOTTIME);
	int64_t wall = (int64_t)clock_ms(CLOCK_REALTIME);
	if (s_last_mono == 0 || wall < CLOCK_SANE_MS) {
		s_last_mono = mono;
		s_last_boot = boot;
		s_saved_mono = mono;
		return;
	}
	// CLOCK_BOOTTIME goes on counting through a suspend and CLOCK_MONOTONIC
	// does not: the difference is the time asleep, which no tick ever sees.
	uint64_t total = boot - s_last_boot;
	if (total < 1000) {
		return; // a second at a time is plenty
	}
	uint64_t awake = mono - s_last_mono;
	if (awake > total) {
		awake = total;
	}
	s_last_mono = mono;
	s_last_boot = boot;
	if (s_stats.since_ms == 0) {
		s_stats.since_ms = wall - (int64_t)total;
	}

	// External power pays for everything while it is in, except in DAC mode
	// with the charging switched off, where the battery carries the device.
	bool powered = usb_vbus_present() && (!usbdac_is_active() || usbdac_charging_enabled());
	float scale = powered ? 0.0f : 1.0f;

	// The sleep came first, then the stretch awake up to now.
	int64_t woke = wall - (int64_t)awake;
	add_span(BS_SLEEP, wall - (int64_t)total, woke, RATE_SLEEP * scale);
	// The SoC staying up is put down to whatever kept it up: the music if any
	// is playing, otherwise the screen or standby.
	int source = playing_source();
	float state_rate = (screen_on ? RATE_SCREEN : 0.0f) + (source < 0 ? RATE_AWAKE : 0.0f);
	add_span(screen_on ? BS_SCREEN : BS_STANDBY, woke, wall, state_rate * scale);
	if (source >= 0) {
		add_span(source, woke, wall, (RATE_AWAKE + RATE_PLAY[source]) * scale);
		// The receiver and DAC mode play through the DAC whatever the route says.
		if (source != BS_BT_RECEIVER && source != BS_USB_DAC && audio_output_is_bluetooth()) {
			add_span(BS_BT_OUT, woke, wall, RATE_BT_OUT * scale);
		}
	}

	const char *text = read_battery_percent();
	int level = text && text[0] >= '0' && text[0] <= '9' ? atoi(text) : -1;
	batterystats_hour_t *h = hour_slot((int32_t)(wall / BS_HOUR_MS));
	if (level >= 0) {
		h->level = (int8_t)level;
	}
	h->cable = h->cable || powered;
	track_level(level, powered);

	if (mono - s_saved_mono >= SAVE_EVERY_MS) {
		batterystats_save();
	}
}

void batterystats_forget(void) {
	if (s_path && unlink(s_path) == 0) {
		printf("batterystats: %s deleted\n", s_path);
	}
	s_path = NULL;
}

void batterystats_save(void) {
	s_saved_mono = clock_ms(CLOCK_MONOTONIC);
	if (!s_path) {
		return;
	}
	// Written beside and renamed over, so a power cut mid-write leaves the
	// previous copy rather than half of this one.
	char tmp[256];
	snprintf(tmp, sizeof(tmp), "%s.tmp", s_path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		printf("batterystats: %s will not open (%s)\n", tmp, strerror(errno));
		return;
	}
	bool ok = write(fd, &s_stats, sizeof(s_stats)) == (ssize_t)sizeof(s_stats) && fsync(fd) == 0;
	close(fd);
	if (!ok || rename(tmp, s_path) != 0) {
		printf("batterystats: saving %s failed (%s)\n", s_path, strerror(errno));
		unlink(tmp);
	}
}

const batterystats_hour_t *batterystats_hour(int ago) {
	if (ago < 0 || ago >= BS_HOURS) {
		return NULL;
	}
	int32_t hour = (int32_t)((int64_t)clock_ms(CLOCK_REALTIME) / BS_HOUR_MS) - ago;
	const batterystats_hour_t *h = &s_stats.hours[hour % BS_HOURS];
	return h->hour == hour ? h : NULL;
}

int64_t batterystats_since_ms(void) { return s_stats.since_ms; }
