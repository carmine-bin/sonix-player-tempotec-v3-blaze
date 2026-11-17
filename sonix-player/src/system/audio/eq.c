#include "eq.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "src/system/audio/replaygain.h"
#include "src/system/core/config.h"

// The classic ISO octave centres every 10-band graphic EQ uses (and the ten
// bands the stock player's geq carries).
const int eq_band_freq[EQ_BANDS] = {31, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};

// Peaking filters one octave wide read as the natural "graphic EQ" shape.
#define EQ_BAND_Q 1.1f

// ---------------------------------------------------------------------------
// Shared settings. Written by the UI thread, read by the audio thread; every
// write bumps `settings_version`, and eq_process() rebuilds its coefficients
// when the number it last saw is stale. Torn reads are harmless: the very
// next buffer rebuilds again from the settled values.
// ---------------------------------------------------------------------------

static volatile int band_gain_db[EQ_BANDS];
static volatile int eq_enabled;
static volatile int mseb_value[MSEB_BANDS];

// PEQ. The bands are not `volatile`: they are structs, and a volatile struct is
// not an atomic access in C, only slower code. The module's usual rule applies:
// the audio thread may read one mid-update, and the next buffer reads it whole,
// because settings_version is the last thing the UI thread bumps.
static peq_band_t peq_bands[PEQ_BANDS];
static volatile int peq_enabled;
static volatile int peq_preamp_tenths;

// How far the sliders may travel. The stock player's own sliders run to 20;
// the wider settings are for anyone who wants to push a characteristic past
// what HiBy offers. The per-step dB scales below do not change with it, so 20
// means exactly what it means on the original and a wider range simply reaches
// further.
static volatile int mseb_range = MSEB_RANGE_DEFAULT;

// Where the named presets live, once the card's root is known.
static char mseb_preset_dir[512];
static char eq_preset_dir[512];
static char peq_preset_dir[512];
static volatile int mseb_enabled;
static volatile unsigned settings_version = 1;

// Soundfield: on/off and the stereo width in hundredths (100 = untouched).
// It sits outside `settings_version` on purpose -- it has no coefficients to
// rebuild, the audio thread just reads the two numbers per buffer.
static volatile int sf_enabled;
static volatile int sf_width = SOUNDFIELD_WIDTH_DEFAULT;

// ---------------------------------------------------------------------------
// Channel balance -- see eq.h for where its range comes from.
// ---------------------------------------------------------------------------

static volatile int bal_enabled;
static volatile int mono_enabled;

// ---------------------------------------------------------------------------
// Direct -- see eq.h.
//
// Every `*_enabled` above and below is what the audio thread goes by. With
// Direct on they are all zero, whatever the switches say; the switches
// themselves are kept here, are what the config holds and what the getters
// answer with, so the pages go on showing what will come back.
// ---------------------------------------------------------------------------

static volatile int direct_on;
static volatile int eq_switch, mseb_switch, peq_switch, sf_switch, bal_switch, mono_switch, cf_switch;
static void direct_apply(void);
static volatile int bal_tenths;	  // -200..200, the slider's own value
static volatile int bal_left_q15; // the two attenuations, 32768 = untouched
static volatile int bal_right_q15;

static int clamp_balance(int tenths) {
	if (tenths < BALANCE_MIN) {
		tenths = BALANCE_MIN;
	}
	if (tenths > BALANCE_MAX) {
		tenths = BALANCE_MAX;
	}
	// Snapped to the half-decibel the original's slider steps by, rounded to
	// the nearest rather than towards zero.
	int step = BALANCE_STEP;
	return ((tenths < 0 ? tenths - step / 2 : tenths + step / 2) / step) * step;
}

// 10^(-dB/20) in Q15, without a pow() call anywhere near the audio path: every
// half decibel of attenuation from 0 to 20 dB, which is the whole range the
// slider can ask for.
static int attenuation_q15(int tenths_down) {
	static const int TABLE[41] = {
		32768, 30935, 29205, 27571, 26029, 24573, 23198, 21900, 20675, 19519, 18427,
		17396, 16423, 15504, 14637, 13818, 13045, 12315, 11627, 10976, 10362, 9783,
		9235,  8719,  8231,  7771,  7336,  6925,  6538,  6172,  5827,  5501,  5193,
		4903,  4629,  4370,  4125,  3894,  3677,  3471,  3277,
	};
	int index = tenths_down / BALANCE_STEP;
	if (index < 0) {
		index = 0;
	}
	if (index > 40) {
		index = 40;
	}
	return TABLE[index];
}

static void balance_set_gains(int tenths) {
	bal_tenths = tenths;
	// Only ever downwards: the channel being favoured is left alone and the
	// other one turned down, so a track already at full scale cannot be pushed
	// into clipping by a balance setting.
	bal_left_q15 = tenths > 0 ? attenuation_q15(tenths) : 32768;
	bal_right_q15 = tenths < 0 ? attenuation_q15(-tenths) : 32768;
}

static volatile int cf_enabled;
static volatile int cf_level = CROSSFEED_LEVEL_DEFAULT; // per cent
static volatile int cf_cutoff = CROSSFEED_CUTOFF_DEFAULT;
static volatile int cf_delay_us = CROSSFEED_DELAY_DEFAULT;

// The delay line. The 600 us maximum delay is 231 samples at 384 kHz, the
// highest rate this DAC accepts, so 256 covers it with margin at 2 KB total.
#define CF_DELAY_MAX_SAMPLES 256

typedef struct {
	int32_t line[CF_DELAY_MAX_SAMPLES];
	int pos;
	int32_t lp; // one-pole low-pass state, Q8 above the sample
} cf_channel_t;

static cf_channel_t cf_left, cf_right;

// What was prepared last time: if any of the three settings changes, or the
// sample rate does (it enters both computations), the values are recomputed.
static int cf_ready_rate;
static int cf_ready_cutoff;
static int cf_ready_delay_us;
static int cf_delay_samples;
static int cf_alpha_q15;   // low-pass coefficient
static int cf_feed_q15;    // how much of the opposite channel
static int cf_direct_q15;  // and how much of its own, so the level does not rise

static int clamp_step(int value, int min, int max, int step) {
	if (value < min) {
		value = min;
	}
	if (value > max) {
		value = max;
	}
	return min + ((value - min + step / 2) / step) * step;
}

static int clamp_width(int hundredths) {
	if (hundredths < SOUNDFIELD_WIDTH_MIN) {
		hundredths = SOUNDFIELD_WIDTH_MIN;
	}
	if (hundredths > SOUNDFIELD_WIDTH_MAX) {
		hundredths = SOUNDFIELD_WIDTH_MAX;
	}
	// Snapped to the plugin's own 0.05 step, so a stale config value cannot
	// leave the slider between two positions.
	return (hundredths / SOUNDFIELD_WIDTH_STEP) * SOUNDFIELD_WIDTH_STEP;
}

// ---------------------------------------------------------------------------
// PEQ: values, limits and configuration
// ---------------------------------------------------------------------------

// Starting frequencies. Nothing special about them -- any band can be moved
// anywhere -- but spreading them over octaves makes the page readable at a
// glance, where ten bands all at 1 kHz look like a bug.
static const int PEQ_DEFAULT_FREQ[PEQ_BANDS] = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};

static int clamp_int(int value, int low, int high) {
	if (value < low) {
		return low;
	}
	if (value > high) {
		return high;
	}
	return value;
}

static void peq_clamp_band(peq_band_t *b) {
	b->type = clamp_int(b->type, 0, PEQ_TYPE_COUNT - 1);
	b->freq = clamp_int(b->freq, PEQ_FREQ_MIN, PEQ_FREQ_MAX);
	b->gain_tenths = clamp_int(b->gain_tenths, PEQ_GAIN_MIN_TENTHS, PEQ_GAIN_MAX_TENTHS);
	b->q_cent = clamp_int(b->q_cent, PEQ_Q_MIN, PEQ_Q_MAX);
}

// "b3_freq" and friends: one config key per parameter, so the file stays
// readable and hand-editable.
static void peq_key(char *out, size_t size, int index, const char *what) {
	snprintf(out, size, "b%d_%s", index, what);
}

// Into the config, not yet onto the card.
static void peq_store_band(int index) {
	const peq_band_t *b = &peq_bands[index];
	char key[24];

	peq_key(key, sizeof(key), index, "on");
	config_set_int("peq", key, b->on ? 1 : 0);
	peq_key(key, sizeof(key), index, "type");
	config_set_int("peq", key, b->type);
	peq_key(key, sizeof(key), index, "freq");
	config_set_int("peq", key, b->freq);
	peq_key(key, sizeof(key), index, "gain");
	config_set_int("peq", key, b->gain_tenths);
	peq_key(key, sizeof(key), index, "q");
	config_set_int("peq", key, b->q_cent);
}

static void peq_save_band(int index) {
	peq_store_band(index);
	config_save();
}

static void peq_load(void) {
	peq_enabled = (int)config_get_int("peq", "enabled", 0);
	peq_preamp_tenths =
		clamp_int((int)config_get_int("peq", "preamp", 0), PEQ_PREAMP_MIN_TENTHS, PEQ_PREAMP_MAX_TENTHS);

	for (int i = 0; i < PEQ_BANDS; i++) {
		char key[24];
		peq_band_t b;

		peq_key(key, sizeof(key), i, "on");
		b.on = config_get_int("peq", key, 0) != 0;
		peq_key(key, sizeof(key), i, "type");
		b.type = (int)config_get_int("peq", key, PEQ_TYPE_PEAK);
		peq_key(key, sizeof(key), i, "freq");
		b.freq = (int)config_get_int("peq", key, PEQ_DEFAULT_FREQ[i]);
		peq_key(key, sizeof(key), i, "gain");
		b.gain_tenths = (int)config_get_int("peq", key, 0);
		peq_key(key, sizeof(key), i, "q");
		b.q_cent = (int)config_get_int("peq", key, PEQ_Q_DEFAULT);

		peq_clamp_band(&b);
		peq_bands[i] = b;
	}
}

void eq_init(void) {
	eq_enabled = (int)config_get_int("eq", "enabled", 0);
	for (int i = 0; i < EQ_BANDS; i++) {
		char key[16];
		snprintf(key, sizeof(key), "band%d", i);
		long g = config_get_int("eq", key, 0);
		if (g < EQ_GAIN_MIN_DB) {
			g = EQ_GAIN_MIN_DB;
		}
		if (g > EQ_GAIN_MAX_DB) {
			g = EQ_GAIN_MAX_DB;
		}
		band_gain_db[i] = (int)g;
	}
	sf_enabled = (int)config_get_int("soundfield", "enabled", 0);
	sf_width = clamp_width((int)config_get_int("soundfield", "width", SOUNDFIELD_WIDTH_DEFAULT));

	bal_enabled = (int)config_get_int("balance", "enabled", 0);
	mono_enabled = config_get_int("audio", "mono", 0) ? 1 : 0;
	balance_set_gains(clamp_balance((int)config_get_int("balance", "value", BALANCE_CENTRE)));

	cf_enabled = (int)config_get_int("crossfeed", "enabled", 0);
	cf_level = clamp_step((int)config_get_int("crossfeed", "level", CROSSFEED_LEVEL_DEFAULT), CROSSFEED_LEVEL_MIN,
						  CROSSFEED_LEVEL_MAX, CROSSFEED_LEVEL_STEP);
	cf_cutoff = clamp_step((int)config_get_int("crossfeed", "cutoff", CROSSFEED_CUTOFF_DEFAULT), CROSSFEED_CUTOFF_MIN,
						   CROSSFEED_CUTOFF_MAX, CROSSFEED_CUTOFF_STEP);
	cf_delay_us = clamp_step((int)config_get_int("crossfeed", "delay_us", CROSSFEED_DELAY_DEFAULT),
							 CROSSFEED_DELAY_MIN, CROSSFEED_DELAY_MAX, CROSSFEED_DELAY_STEP);
	cf_ready_rate = 0;
	crossfeed_reset();

	mseb_enabled = (int)config_get_int("mseb", "enabled", 0);
	mseb_range = (int)config_get_int("mseb", "range", MSEB_RANGE_DEFAULT);
	if (mseb_range != 20 && mseb_range != 40 && mseb_range != 100) {
		mseb_range = MSEB_RANGE_DEFAULT;
	}
	for (int i = 0; i < MSEB_BANDS; i++) {
		char key[16];
		snprintf(key, sizeof(key), "v%d", i);
		long v = config_get_int("mseb", key, 0);
		if (v < MSEB_VALUE_MIN) {
			v = MSEB_VALUE_MIN;
		}
		if (v > MSEB_VALUE_MAX) {
			v = MSEB_VALUE_MAX;
		}
		mseb_value[i] = (int)v;
	}

	peq_load();

	eq_switch = eq_enabled;
	mseb_switch = mseb_enabled;
	peq_switch = peq_enabled;
	sf_switch = sf_enabled;
	bal_switch = bal_enabled;
	mono_switch = mono_enabled;
	cf_switch = cf_enabled;
	direct_on = config_get_int("audio", "direct", 0) ? 1 : 0;
	direct_apply();

	settings_version++;
}

// What the audio thread goes by, from the switches and Direct. The version is
// bumped so the equalisers' chain is rebuilt the way it is when one of them is
// switched by hand: same coefficients swap, same ramp of the level.
static void direct_apply(void) {
	int through = !direct_on;
	bool crossfeed_back = through && cf_switch && !cf_enabled;
	eq_enabled = through && eq_switch;
	mseb_enabled = through && mseb_switch;
	peq_enabled = through && peq_switch;
	sf_enabled = through && sf_switch;
	bal_enabled = through && bal_switch;
	mono_enabled = through && mono_switch;
	if (crossfeed_back) {
		crossfeed_reset(); // as when its own switch brings it back
	}
	cf_enabled = through && cf_switch;
	replaygain_set_bypass(direct_on != 0);
	settings_version++;
}

void direct_set_enabled(bool enabled) {
	direct_on = enabled ? 1 : 0;
	direct_apply();
	config_set_int("audio", "direct", direct_on);
	config_save();
	printf("audio: direct %s\n", direct_on ? "on -- every effect and ReplayGain bypassed" : "off");
}

bool direct_get_enabled(void) { return direct_on != 0; }

void eq_set_enabled(bool enabled) {
	eq_switch = enabled ? 1 : 0;
	eq_enabled = eq_switch && !direct_on;
	settings_version++;
	config_set_int("eq", "enabled", eq_switch);
	config_save();
}

bool eq_get_enabled(void) { return eq_switch != 0; }

void eq_set_band(int band, int gain_db) {
	if (band < 0 || band >= EQ_BANDS) {
		return;
	}
	if (gain_db < EQ_GAIN_MIN_DB) {
		gain_db = EQ_GAIN_MIN_DB;
	}
	if (gain_db > EQ_GAIN_MAX_DB) {
		gain_db = EQ_GAIN_MAX_DB;
	}
	band_gain_db[band] = gain_db;
	settings_version++;

	char key[16];
	snprintf(key, sizeof(key), "band%d", band);
	config_set_int("eq", key, gain_db);
	config_save();
}

int eq_get_band(int band) { return (band >= 0 && band < EQ_BANDS) ? band_gain_db[band] : 0; }

// ---------------------------------------------------------------------------
// MSEB settings. Same contract as the graphic EQ's: UI writes, audio thread
// rebuilds on the next buffer.
// ---------------------------------------------------------------------------

// The stock engine's real parameter set, extracted from its data section:
// master_temp (scale -0.06 across four filters) plus bass0 0.25, bass1 0.2,
// thickness 0.12, vocal 0.125, female 0.1, male_vocal 0.1, female_vocal 0.1,
// instruments 0.1, air 0.25 -- sliders -10..+10, like its presets
// (e.g. {1,9,7,5,5,7,-6,10,7,-1}).
const char *const mseb_band_name[MSEB_BANDS] = {
	"eq_temperature", "eq_extended_bass", "eq_bass_texture", "eq_note_thickness", "eq_vocals",
	"eq_female_overtones", "eq_male_vocal", "eq_female_vocal", "eq_instruments", "eq_air",
};

void mseb_set_enabled(bool enabled) {
	mseb_switch = enabled ? 1 : 0;
	mseb_enabled = mseb_switch && !direct_on;
	settings_version++;
	config_set_int("mseb", "enabled", mseb_switch);
	config_save();
}

bool mseb_get_enabled(void) { return mseb_switch != 0; }

void mseb_set_value(int band, int value) {
	if (band < 0 || band >= MSEB_BANDS) {
		return;
	}
	if (value < -mseb_range) {
		value = -mseb_range;
	}
	if (value > mseb_range) {
		value = mseb_range;
	}
	mseb_value[band] = value;
	settings_version++;
}

int mseb_get_value(int band) { return (band >= 0 && band < MSEB_BANDS) ? mseb_value[band] : 0; }

// ---------------------------------------------------------------------------
// The equalizer's own presets: the ready-made curves everyone expects, and
// the user's own in <card>/.local/eq.
// ---------------------------------------------------------------------------

// Ten ISO bands: 31 63 125 250 500 1k 2k 4k 8k 16k, in whole dB.
const eq_preset_t EQ_DEFAULT_PRESETS[] = {
	{"eq_flat", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
	{"eq_rock", {5, 4, 2, -1, -2, 0, 3, 4, 5, 5}},
	{"eq_pop", {-1, 0, 2, 4, 4, 2, 0, -1, -1, -1}},
	{"eq_jazz", {3, 2, 1, 2, -1, -1, 0, 1, 2, 3}},
	{"eq_classical", {4, 3, 2, 0, -1, -1, 0, 2, 3, 4}},
	{"eq_dance", {6, 5, 3, 0, -1, -2, 1, 3, 4, 4}},
	{"eq_bass", {7, 6, 4, 2, 0, 0, 0, 0, 0, 0}},
	{"eq_treble", {0, 0, 0, 0, 0, 1, 3, 5, 6, 7}},
	{"eq_vocal", {-2, -2, -1, 2, 4, 4, 3, 1, 0, -1}},
	{"eq_acoustic", {4, 3, 1, 0, 1, 1, 2, 3, 3, 2}},
};
const int EQ_DEFAULT_PRESET_COUNT = (int)(sizeof(EQ_DEFAULT_PRESETS) / sizeof(EQ_DEFAULT_PRESETS[0]));

void eq_apply_preset(const eq_preset_t *preset) {
	if (!preset) {
		return;
	}
	for (int i = 0; i < EQ_BANDS; i++) {
		eq_set_band(i, preset->gain_db[i]);
	}
}

void eq_presets_set_dir(const char *sd_root) {
	if (!sd_root || !sd_root[0]) {
		eq_preset_dir[0] = '\0';
		return;
	}
	snprintf(eq_preset_dir, sizeof(eq_preset_dir), "%s/.local/eq", sd_root);
}

static bool name_is_usable(const char *name) {
	if (!name || !name[0] || strlen(name) > 100) {
		return false;
	}
	for (const char *c = name; *c; c++) {
		if (strchr("/\\:*?\"<>|", *c) != NULL) {
			return false;
		}
	}
	return true;
}

// Creates a directory and the .local above it. The presets live in
// <card>/.local/<kind>, and a single-level mkdir only worked because something
// else -- the log, or the thumbnail cache -- happened to create .local first at
// startup. With logging off and the cache fallen back to /tmp, saving a preset
// failed with ENOENT on a perfectly writable card.
static void mkdir_with_parent(const char *dir) {
	char parent[700];
	snprintf(parent, sizeof(parent), "%s", dir);
	char *slash = strrchr(parent, '/');
	if (slash && slash != parent) {
		*slash = '\0';
		mkdir(parent, 0777);
	}
	mkdir(dir, 0777);
}

bool eq_preset_save(const char *name) {
	if (!eq_preset_dir[0] || !name_is_usable(name)) {
		return false;
	}
	mkdir_with_parent(eq_preset_dir);

	char path[700];
	snprintf(path, sizeof(path), "%s/%s.ini", eq_preset_dir, name);

	FILE *f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "eq: cannot write '%s': %s\n", path, strerror(errno));
		return false;
	}
	fputs("# equalizer preset\n", f);
	for (int i = 0; i < EQ_BANDS; i++) {
		fprintf(f, "b%d=%d\n", i, band_gain_db[i]);
	}
	fclose(f);
	return true;
}

bool eq_preset_load(const char *name) {
	if (!eq_preset_dir[0] || !name_is_usable(name)) {
		return false;
	}

	char path[700];
	snprintf(path, sizeof(path), "%s/%s.ini", eq_preset_dir, name);

	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}

	char line[128];
	while (fgets(line, sizeof(line), f)) {
		int index = 0, value = 0;
		if (sscanf(line, "b%d=%d", &index, &value) == 2 && index >= 0 && index < EQ_BANDS) {
			eq_set_band(index, value);
		}
	}
	fclose(f);
	return true;
}

bool eq_preset_delete(const char *name) {
	if (!eq_preset_dir[0] || !name_is_usable(name)) {
		return false;
	}
	char path[700];
	snprintf(path, sizeof(path), "%s/%s.ini", eq_preset_dir, name);
	return remove(path) == 0;
}

int eq_preset_for_each(eq_preset_name_cb_t cb, void *user) {
	if (!cb || !eq_preset_dir[0]) {
		return 0;
	}

	DIR *dir = opendir(eq_preset_dir);
	if (!dir) {
		return 0;
	}

	int count = 0;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		size_t len = strlen(de->d_name);
		if (len < 5 || strcasecmp(de->d_name + len - 4, ".ini") != 0) {
			continue;
		}
		char name[128];
		snprintf(name, sizeof(name), "%.*s", (int)(len - 4), de->d_name);
		count++;
		if (!cb(name, user)) {
			break;
		}
	}
	closedir(dir);
	return count;
}

void eq_reset(void) {
	for (int i = 0; i < EQ_BANDS; i++) {
		eq_set_band(i, 0);
	}
}

// ---------------------------------------------------------------------------
// PEQ: what the UI touches
// ---------------------------------------------------------------------------

void peq_set_enabled(bool enabled) {
	peq_switch = enabled ? 1 : 0;
	peq_enabled = peq_switch && !direct_on;
	settings_version++;
	config_set_int("peq", "enabled", peq_switch);
	config_save();
}

bool peq_get_enabled(void) { return peq_switch != 0; }

void peq_get_band(int index, peq_band_t *out) {
	if (!out) {
		return;
	}
	if (index < 0 || index >= PEQ_BANDS) {
		memset(out, 0, sizeof(*out));
		return;
	}
	*out = peq_bands[index];
}

void peq_set_band(int index, const peq_band_t *band) {
	if (!band || index < 0 || index >= PEQ_BANDS) {
		return;
	}

	peq_band_t next = *band;
	peq_clamp_band(&next);
	peq_bands[index] = next;

	// Version bumped after the value: it is what the audio thread watches to
	// know there is something new to read.
	settings_version++;
	peq_save_band(index);
}

int peq_get_preamp(void) { return peq_preamp_tenths; }

void peq_set_preamp(int tenths) {
	peq_preamp_tenths = clamp_int(tenths, PEQ_PREAMP_MIN_TENTHS, PEQ_PREAMP_MAX_TENTHS);
	settings_version++;
	config_set_int("peq", "preamp", peq_preamp_tenths);
	config_save();
}

// ---------------------------------------------------------------------------
// Parametric presets
//
// A preset is a text file in the Equalizer APO format that AutoEq publishes
// for every headphone ("ParametricEQ.txt"), so a file downloaded from AutoEq,
// written by Equalizer APO or exported by REW loads as it is, and a preset
// saved here opens in any of them:
//
//     Preamp: -6.2 dB
//     Filter 1: ON LSC Fc 105 Hz Gain 6.5 dB Q 0.70
//     Filter 2: ON PK Fc 162 Hz Gain -2.8 dB Q 0.60
//     ...
//
// They live in <card>/PEQ, a folder the user can see and copy files into.
// Presets from before live in <card>/.local/peq as .ini files of this
// program's own; they are rewritten in the new form the first time the card
// is seen.
//
// The filters are the same arithmetic as APO's: RBJ biquads, and shelves that
// take their width from Q (APO's LSC and HSC). What APO has and this chain
// does not -- band-pass, notch, all-pass, more than PEQ_BANDS filters, a
// different curve per channel -- is left out and counted, so the page can
// say so.
// ---------------------------------------------------------------------------

static char peq_legacy_dir[512];

// A band reset to its defaults, without touching the live chain.
static peq_band_t peq_default_band(int index) {
	peq_band_t b = {
		.on = false,
		.type = PEQ_TYPE_PEAK,
		.freq = PEQ_DEFAULT_FREQ[index],
		.gain_tenths = 0,
		.q_cent = PEQ_Q_DEFAULT,
	};
	return b;
}

// "-6.2", "0.0", "3.5" from tenths, without printf's %f: the decimal point
// is always a point, whatever the locale, because that is what APO reads.
static void tenths_text(int tenths, char *out, size_t size) {
	int abs_tenths = tenths < 0 ? -tenths : tenths;
	snprintf(out, size, "%s%d.%d", tenths < 0 ? "-" : "", abs_tenths / 10, abs_tenths % 10);
}

static const char *apo_type_name(int type) {
	switch (type) {
	case PEQ_TYPE_LOWSHELF:
		return "LSC";
	case PEQ_TYPE_HIGHSHELF:
		return "HSC";
	case PEQ_TYPE_LOWPASS:
		return "LPQ";
	case PEQ_TYPE_HIGHPASS:
		return "HPQ";
	case PEQ_TYPE_PEAK:
	default:
		return "PK";
	}
}

static bool peq_write_apo(const char *path, const peq_band_t *bands, int preamp_tenths) {
	FILE *f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "peq: cannot write '%s': %s\n", path, strerror(errno));
		return false;
	}
	char number[16];
	tenths_text(preamp_tenths, number, sizeof(number));
	fprintf(f, "Preamp: %s dB\n", number);
	for (int i = 0; i < PEQ_BANDS; i++) {
		const peq_band_t *b = &bands[i];
		fprintf(f, "Filter %d: %s %s Fc %d Hz", i + 1, b->on ? "ON" : "OFF", apo_type_name(b->type), b->freq);
		// APO's LPQ and HPQ have no gain; writing one would not load back.
		if (b->type != PEQ_TYPE_LOWPASS && b->type != PEQ_TYPE_HIGHPASS) {
			tenths_text(b->gain_tenths, number, sizeof(number));
			fprintf(f, " Gain %s dB", number);
		}
		fprintf(f, " Q %d.%02d\n", b->q_cent / 100, b->q_cent % 100);
	}
	bool ok = ferror(f) == 0;
	if (fclose(f) != 0) {
		ok = false;
	}
	return ok;
}

// APO's filter names, and what each becomes here. The ones without a Q of
// their own (LP, HP, and the shelves written without one) get 0.71, the
// Butterworth value APO uses for them.
static bool apo_type(const char *name, int *type) {
	static const struct {
		const char *name;
		int type;
	} TYPES[] = {
		{"PK", PEQ_TYPE_PEAK},		   {"PEQ", PEQ_TYPE_PEAK},	  {"MODAL", PEQ_TYPE_PEAK},
		{"LSC", PEQ_TYPE_LOWSHELF},	   {"LS", PEQ_TYPE_LOWSHELF}, {"HSC", PEQ_TYPE_HIGHSHELF},
		{"HS", PEQ_TYPE_HIGHSHELF},	   {"LP", PEQ_TYPE_LOWPASS},  {"LPQ", PEQ_TYPE_LOWPASS},
		{"HP", PEQ_TYPE_HIGHPASS},	   {"HPQ", PEQ_TYPE_HIGHPASS},
	};
	for (size_t i = 0; i < sizeof(TYPES) / sizeof(TYPES[0]); i++) {
		if (strcasecmp(name, TYPES[i].name) == 0) {
			*type = TYPES[i].type;
			return true;
		}
	}
	return false;
}

#define APO_Q_DEFAULT 0.71

static int round_scaled(double value, double scale) {
	double v = value * scale;
	return (int)(v >= 0 ? v + 0.5 : v - 0.5);
}

// The next word of `*cursor`, split on blanks, NULL at the end of the line.
static char *next_word(char **cursor) {
	char *p = *cursor;
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	if (!*p || *p == '\r' || *p == '\n') {
		*cursor = p;
		return NULL;
	}
	char *word = p;
	while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
		p++;
	}
	if (*p) {
		*p++ = '\0';
	}
	*cursor = p;
	return word;
}

static bool word_number(const char *word, double *out) {
	if (!word) {
		return false;
	}
	char *end = NULL;
	*out = strtod(word, &end);
	return end != word;
}

typedef enum {
	APO_FILTER_OK,
	APO_FILTER_UNUSABLE, // a shape this chain does not have, or no frequency
	APO_FILTER_EMPTY,	 // REW's "None": a slot with no filter in it
} apo_filter_t;

// One "Filter N: ON PK Fc 105 Hz Gain -2.8 dB Q 0.70" line, everything after
// the colon. An OFF filter comes back as a band that is off.
static apo_filter_t apo_parse_filter(char *rest, peq_band_t *out) {
	char *cursor = rest;
	char *word = next_word(&cursor);
	if (!word) {
		return APO_FILTER_EMPTY;
	}
	bool on = strcasecmp(word, "OFF") != 0;
	if (strcasecmp(word, "ON") == 0 || strcasecmp(word, "OFF") == 0) {
		word = next_word(&cursor);
	}
	if (!word || strcasecmp(word, "None") == 0) {
		return APO_FILTER_EMPTY;
	}
	int type;
	if (!apo_type(word, &type)) {
		return APO_FILTER_UNUSABLE;
	}

	double fc = 0, gain = 0, q = APO_Q_DEFAULT;
	bool have_fc = false;
	while ((word = next_word(&cursor)) != NULL) {
		double value;
		if (strcasecmp(word, "Fc") == 0 && word_number(next_word(&cursor), &value)) {
			fc = value;
			have_fc = true;
		} else if (strcasecmp(word, "Gain") == 0 && word_number(next_word(&cursor), &value)) {
			gain = value;
		} else if (strcasecmp(word, "Q") == 0 && word_number(next_word(&cursor), &value)) {
			q = value;
		} else if (strcasecmp(word, "BW") == 0) {
			// "BW Oct 1.5": a width in octaves, the same thing as Q said
			// another way.
			char *unit = next_word(&cursor);
			if (unit && strcasecmp(unit, "Oct") == 0 && word_number(next_word(&cursor), &value) && value > 0) {
				double n = pow(2.0, value);
				q = sqrt(n) / (n - 1.0);
			}
		}
	}
	if (!have_fc) {
		return APO_FILTER_UNUSABLE;
	}

	out->on = on;
	out->type = type;
	out->freq = round_scaled(fc, 1.0);
	out->gain_tenths = round_scaled(gain, 10.0);
	out->q_cent = round_scaled(q, 100.0);
	peq_clamp_band(out);
	return APO_FILTER_OK;
}

// Reads an APO file into `bands` and `*preamp_tenths`. The bands not named in
// the file are left at their defaults. `*ignored` counts the filters that were
// in the file and could not come along. False when the file cannot be opened
// or holds no filter this chain can play.
static bool peq_read_apo(const char *path, peq_band_t *bands, int *preamp_tenths, int *ignored) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	for (int i = 0; i < PEQ_BANDS; i++) {
		bands[i] = peq_default_band(i);
	}
	*preamp_tenths = 0;
	*ignored = 0;

	int count = 0;
	char line[256];
	bool first = true;
	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		// A file saved by Notepad starts with a UTF-8 byte order mark, and
		// "\xEF\xBB\xBFPreamp" is not "Preamp": the preamp line was skipped.
		if (first && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
			p += 3;
		}
		first = false;
		while (*p == ' ' || *p == '\t') {
			p++;
		}
		if (*p == '#' || *p == '\0') {
			continue;
		}
		char *colon = strchr(p, ':');
		if (!colon) {
			continue;
		}
		*colon = '\0';
		char *rest = colon + 1;

		if (strcasecmp(p, "Preamp") == 0) {
			double db;
			char *cursor = rest;
			if (word_number(next_word(&cursor), &db)) {
				*preamp_tenths = clamp_int(round_scaled(db, 10.0), PEQ_PREAMP_MIN_TENTHS, PEQ_PREAMP_MAX_TENTHS);
			}
		} else if (strncasecmp(p, "Channel", 7) == 0) {
			// A second curve for one of the channels. This chain has one curve
			// for both, so the first is the one that is used.
			if (count > 0) {
				break;
			}
		} else if (strncasecmp(p, "Filter", 6) == 0) {
			peq_band_t b;
			apo_filter_t parsed = apo_parse_filter(rest, &b);
			if (parsed == APO_FILTER_EMPTY) {
				continue;
			}
			if (parsed == APO_FILTER_UNUSABLE) {
				(*ignored)++;
			} else if (count < PEQ_BANDS) {
				bands[count++] = b;
			} else {
				(*ignored)++;
			}
		}
	}
	fclose(f);
	return count > 0;
}

// The .ini of the presets saved before the move to the APO format.
static bool peq_read_legacy(const char *path, peq_band_t *bands, int *preamp_tenths) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	for (int i = 0; i < PEQ_BANDS; i++) {
		bands[i] = peq_default_band(i);
	}
	*preamp_tenths = 0;

	char line[160];
	while (fgets(line, sizeof(line), f)) {
		int preamp = 0;
		if (sscanf(line, "preamp=%d", &preamp) == 1) {
			*preamp_tenths = clamp_int(preamp, PEQ_PREAMP_MIN_TENTHS, PEQ_PREAMP_MAX_TENTHS);
			continue;
		}
		int index = 0, on = 0, type = 0, freq = 0, gain = 0, q = 0;
		if (sscanf(line, "b%d=%d,%d,%d,%d,%d", &index, &on, &type, &freq, &gain, &q) == 6 && index >= 0 &&
			index < PEQ_BANDS) {
			peq_band_t b = {.on = on != 0, .type = type, .freq = freq, .gain_tenths = gain, .q_cent = q};
			peq_clamp_band(&b);
			bands[index] = b;
		}
	}
	fclose(f);
	return true;
}

// Rewrites every preset left in <card>/.local/peq into <card>/PEQ. The old
// file is renamed rather than deleted, and a preset already in the new folder
// under the same name is left as it is.
static void peq_migrate_legacy(void) {
	DIR *dir = opendir(peq_legacy_dir);
	if (!dir) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		size_t len = strlen(de->d_name);
		if (len < 5 || strcasecmp(de->d_name + len - 4, ".ini") != 0) {
			continue;
		}
		// A name too long for the buffers is left where it is: a cut path
		// would read, write or rename the wrong file.
		char from[700], to[700], done[720];
		int n_from = snprintf(from, sizeof(from), "%s/%s", peq_legacy_dir, de->d_name);
		int n_to = snprintf(to, sizeof(to), "%s/%.*s.txt", peq_preset_dir, (int)(len - 4), de->d_name);
		if (n_from < 0 || (size_t)n_from >= sizeof(from) || n_to < 0 || (size_t)n_to >= sizeof(to)) {
			continue;
		}
		snprintf(done, sizeof(done), "%s.migrated", from);

		peq_band_t bands[PEQ_BANDS];
		int preamp = 0;
		if (!peq_read_legacy(from, bands, &preamp)) {
			continue;
		}
		mkdir(peq_preset_dir, 0777);
		if (access(to, F_OK) != 0 && !peq_write_apo(to, bands, preamp)) {
			continue;
		}
		rename(from, done);
		fprintf(stderr, "peq: preset '%s' moved to %s\n", de->d_name, to);
	}
	closedir(dir);
}

void peq_presets_set_dir(const char *sd_root) {
	if (!sd_root || !sd_root[0]) {
		peq_preset_dir[0] = '\0';
		peq_legacy_dir[0] = '\0';
		return;
	}
	snprintf(peq_preset_dir, sizeof(peq_preset_dir), "%s/PEQ", sd_root);
	snprintf(peq_legacy_dir, sizeof(peq_legacy_dir), "%s/.local/peq", sd_root);
	peq_migrate_legacy();
}

bool peq_preset_save(const char *name) {
	if (!peq_preset_dir[0] || !name_is_usable(name)) {
		return false;
	}
	mkdir(peq_preset_dir, 0777);

	char path[700];
	snprintf(path, sizeof(path), "%s/%s.txt", peq_preset_dir, name);
	return peq_write_apo(path, peq_bands, peq_preamp_tenths);
}

bool peq_preset_load(const char *name, int *ignored) {
	int dropped = 0;
	if (ignored) {
		*ignored = 0;
	}
	if (!peq_preset_dir[0] || !name_is_usable(name)) {
		return false;
	}

	char path[700];
	snprintf(path, sizeof(path), "%s/%s.txt", peq_preset_dir, name);

	peq_band_t bands[PEQ_BANDS];
	int preamp = 0;
	if (!peq_read_apo(path, bands, &preamp, &dropped)) {
		return false;
	}

	// Into the chain and the config together, with one write to the card at
	// the end rather than one per band.
	for (int i = 0; i < PEQ_BANDS; i++) {
		peq_bands[i] = bands[i];
		peq_store_band(i);
	}
	peq_preamp_tenths = preamp;
	config_set_int("peq", "preamp", peq_preamp_tenths);
	config_save();
	settings_version++;

	if (ignored) {
		*ignored = dropped;
	}
	return true;
}

bool peq_preset_delete(const char *name) {
	if (!peq_preset_dir[0] || !name_is_usable(name)) {
		return false;
	}
	char path[700];
	snprintf(path, sizeof(path), "%s/%s.txt", peq_preset_dir, name);
	return remove(path) == 0;
}

static int compare_names(const void *a, const void *b) {
	return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

// In alphabetical order: a folder of AutoEq downloads is a list of headphone
// names, and in the order the card happens to store them it cannot be searched
// by eye.
#define PEQ_LIST_MAX 512

int peq_preset_for_each(peq_preset_name_cb_t cb, void *user) {
	if (!cb || !peq_preset_dir[0]) {
		return 0;
	}

	DIR *dir = opendir(peq_preset_dir);
	if (!dir) {
		return 0;
	}

	char *names[PEQ_LIST_MAX];
	int count = 0;
	struct dirent *de;
	while (count < PEQ_LIST_MAX && (de = readdir(dir)) != NULL) {
		size_t len = strlen(de->d_name);
		if (len < 5 || de->d_name[0] == '.' || strcasecmp(de->d_name + len - 4, ".txt") != 0) {
			continue;
		}
		char *name = strndup(de->d_name, len - 4);
		if (name) {
			names[count++] = name;
		}
	}
	closedir(dir);

	qsort(names, (size_t)count, sizeof(names[0]), compare_names);
	bool going = true;
	for (int i = 0; i < count; i++) {
		if (going) {
			going = cb(names[i], user);
		}
		free(names[i]);
	}
	return count;
}

void peq_reset_band(int index) {
	if (index < 0 || index >= PEQ_BANDS) {
		return;
	}
	peq_bands[index] = peq_default_band(index);
	peq_save_band(index);
	settings_version++;
}

void peq_reset(void) {
	for (int i = 0; i < PEQ_BANDS; i++) {
		peq_reset_band(i);
	}
	peq_set_preamp(0);
	settings_version++;
}


int mseb_get_range(void) { return mseb_range; }

void mseb_set_range(int range) {
	if (range != 20 && range != 40 && range != 100) {
		range = MSEB_RANGE_DEFAULT;
	}
	mseb_range = range;

	// A narrower range has to bring the values that no longer fit back inside
	// it, or the sliders would show something the engine is not doing.
	for (int i = 0; i < MSEB_BANDS; i++) {
		if (mseb_value[i] > range) {
			mseb_value[i] = range;
		}
		if (mseb_value[i] < -range) {
			mseb_value[i] = -range;
		}
	}
	settings_version++;

	config_set_int("mseb", "range", range);
	mseb_save();
}

void mseb_reset(void) {
	for (int i = 0; i < MSEB_BANDS; i++) {
		mseb_value[i] = 0;
	}
	settings_version++;
	mseb_save();
}

// ---------------------------------------------------------------------------
// Named presets, one small file each in <card>/.local/MSEB.
// ---------------------------------------------------------------------------

void mseb_presets_set_dir(const char *sd_root) {
	if (!sd_root || !sd_root[0]) {
		mseb_preset_dir[0] = '\0';
		return;
	}
	snprintf(mseb_preset_dir, sizeof(mseb_preset_dir), "%s/.local/MSEB", sd_root);
}

static bool preset_path(const char *name, char *out, size_t out_size) {
	if (!mseb_preset_dir[0] || !name || !name[0] || strlen(name) > 100) {
		return false;
	}
	for (const char *c = name; *c; c++) {
		if (strchr("/\\:*?\"<>|", *c) != NULL) {
			return false;
		}
	}
	snprintf(out, out_size, "%s/%s.ini", mseb_preset_dir, name);
	return true;
}

bool mseb_preset_save(const char *name) {
	char path[700];
	if (!preset_path(name, path, sizeof(path))) {
		return false;
	}

	mkdir_with_parent(mseb_preset_dir); // on demand, like every other folder here

	FILE *f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "mseb: cannot write '%s': %s\n", path, strerror(errno));
		return false;
	}
	fprintf(f, "# MSEB preset\nrange=%d\n", mseb_range);
	for (int i = 0; i < MSEB_BANDS; i++) {
		fprintf(f, "v%d=%d\n", i, mseb_value[i]);
	}
	fclose(f);
	return true;
}

bool mseb_preset_load(const char *name) {
	char path[700];
	if (!preset_path(name, path, sizeof(path))) {
		return false;
	}

	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}

	int values[MSEB_BANDS] = {0};
	int range = mseb_range;
	char line[128];
	while (fgets(line, sizeof(line), f)) {
		int index = 0, value = 0;
		if (sscanf(line, "range=%d", &value) == 1) {
			range = value;
		} else if (sscanf(line, "v%d=%d", &index, &value) == 2 && index >= 0 && index < MSEB_BANDS) {
			values[index] = value;
		}
	}
	fclose(f);

	mseb_set_range(range); // the preset carries the range it was made at
	for (int i = 0; i < MSEB_BANDS; i++) {
		mseb_set_value(i, values[i]);
	}
	mseb_save();
	return true;
}

int mseb_preset_for_each(mseb_preset_cb_t cb, void *user) {
	if (!cb || !mseb_preset_dir[0]) {
		return 0;
	}

	DIR *dir = opendir(mseb_preset_dir);
	if (!dir) {
		return 0;
	}

	int count = 0;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		size_t len = strlen(de->d_name);
		if (len < 5 || strcasecmp(de->d_name + len - 4, ".ini") != 0) {
			continue;
		}
		char name[128];
		snprintf(name, sizeof(name), "%.*s", (int)(len - 4), de->d_name);
		count++;
		if (!cb(name, user)) {
			break;
		}
	}
	closedir(dir);
	return count;
}

bool mseb_preset_delete(const char *name) {
	char path[700];
	if (!preset_path(name, path, sizeof(path))) {
		return false;
	}
	return remove(path) == 0;
}

void mseb_save(void) {
	for (int i = 0; i < MSEB_BANDS; i++) {
		char key[16];
		snprintf(key, sizeof(key), "v%d", i);
		config_set_int("mseb", key, mseb_value[i]);
	}
	config_save();
}

// ---------------------------------------------------------------------------
// The filters. All state below belongs to the audio thread.
// ---------------------------------------------------------------------------

#define EQ_MAX_CHANNELS 2

typedef struct {
	float b0, b1, b2, a1, a2; // normalized coefficients
	float z1[EQ_MAX_CHANNELS], z2[EQ_MAX_CHANNELS]; // direct form II transposed state
} biquad_t;

// The shapes a biquad can take. The first three serve MSEB and the graphic EQ;
// the last two come with the parametric and have no gain -- a low-pass does not
// boost anything, it only cuts.
enum { F_PEAK, F_LOWSHELF, F_HIGHSHELF, F_LOWPASS, F_HIGHPASS };

// From the UI's filter shape to the engine's. Two separate enums on purpose:
// peq_type_t is the order the shapes appear in on the page, and reordering the
// page must not mean touching the engine.
static int peq_filter_type(int type) {
	switch (type) {
	case PEQ_TYPE_LOWSHELF:
		return F_LOWSHELF;
	case PEQ_TYPE_HIGHSHELF:
		return F_HIGHSHELF;
	case PEQ_TYPE_LOWPASS:
		return F_LOWPASS;
	case PEQ_TYPE_HIGHPASS:
		return F_HIGHPASS;
	case PEQ_TYPE_PEAK:
	default:
		return F_PEAK;
	}
}

// Whether the shape uses gain at all: the pass filters ignore it, so for them
// "zero gain" does not mean "does nothing".
static bool filter_uses_gain(int type) { return type == F_PEAK || type == F_LOWSHELF || type == F_HIGHSHELF; }

typedef struct {
	int slider;
	int type;
	int freq;
	float db_per_step;
	float q;
} mseb_def_t;

// What MSEB's sliders drive: the stock module's own bank, frequency by
// frequency, with its per-step dB scales and its Q.
//
// Temperature is four filters and not one. The stock master_temp is a tilt made
// of four overlapping high shelves, each cut by value * 0.06; folding them into
// a single shelf carrying the combined 0.24 dB/step gives a curve of a
// different shape, and -- because the automatic pre-gain below measures the
// cascade rather than the declared gains -- a different level as well.
static const mseb_def_t MSEB_DEFS[] = {
	{0, F_HIGHSHELF, 33, -0.06f, 0.4425f},	  {0, F_HIGHSHELF, 231, -0.06f, 0.4425f},	// master_temp
	{0, F_HIGHSHELF, 1617, -0.06f, 0.4425f},  {0, F_HIGHSHELF, 11319, -0.06f, 0.4425f}, // ...four of them
	{1, F_LOWSHELF, 70, 0.25f, 0.7289f},	  {2, F_PEAK, 100, 0.20f, 0.8571f},			// bass0 / bass1
	{3, F_PEAK, 200, 0.12f, 0.6667f},		  {4, F_PEAK, 650, 0.125f, 0.4041f},		// thickness / vocal
	{5, F_PEAK, 3000, 0.10f, 1.4140f},		  {6, F_PEAK, 5800, 0.10f, 0.9991f},		// female / male_vocal
	{7, F_PEAK, 9200, 0.10f, 0.9991f},		  {8, F_PEAK, 7500, 0.10f, 0.4041f}, // female_vocal / instruments
	{9, F_HIGHSHELF, 10000, 0.25f, 0.7289f},											 // air
};
#define MSEB_FILTERS ((int)(sizeof(MSEB_DEFS) / sizeof(MSEB_DEFS[0])))

// Fixed slots, one per possible filter, so a settings change updates
// coefficients in place and the delay-line state carries over. Resetting the
// state on every slider tick was an audible click, and the chain re-shuffling
// positions made it a crackle while adjusting.
#define CHAIN_SLOTS (MSEB_FILTERS + EQ_BANDS + PEQ_BANDS)
#define PEQ_SLOT0 (MSEB_FILTERS + EQ_BANDS)
static biquad_t chain[CHAIN_SLOTS];
static bool chain_active[CHAIN_SLOTS];
// The graphic equaliser and the parametric's headroom: what their samples are
// scaled by right now, and where their boosts say it should sit.
static float pregain = 1.0f;
static float pregain_target = 1.0f;

// MSEB's, which is a different thing computed a different way -- see the note
// over mseb_auto_preamp_db(). Kept in dB as well because the settings page
// reports it and because it can be positive.
static float mseb_pregain = 1.0f;
static float mseb_pregain_target = 1.0f;
static float mseb_pregain_db;
static unsigned built_version;
static int built_rate;

// RBJ audio-EQ-cookbook, double math on purpose: powf/sinf/cosf are GLIBC_2.27
// symbols and the device runs glibc 2.22, which caused a bootloop. This only
// runs on a settings change. Computes coefficients only; the caller decides
// what happens to the filter state.
static bool biquad_set_coeffs(biquad_t *f, int type, int freq, double gain_db, double q, int rate) {
	if (freq <= 0 || freq * 2 >= rate) {
		return false;
	}
	// Zero gain on a peak or a shelf means the filter is not there, and
	// skipping it is one filter less per sample. On a pass filter the gain does
	// not enter the computation at all.
	if (filter_uses_gain(type) && gain_db == 0.0) {
		return false;
	}
	if (q <= 0.0) {
		return false;
	}

	double A = pow(10.0, gain_db / 40.0);
	double w0 = 2.0 * M_PI * (double)freq / (double)rate;
	double cosw = cos(w0);
	double sinw = sin(w0);
	double b0, b1, b2, a0, a1, a2;

	if (type == F_LOWPASS || type == F_HIGHPASS) {
		// Still RBJ: Q here is the resonance at the knee, and at 0.707 the
		// curve is Butterworth (flat up to the cutoff).
		double alpha = sinw / (2.0 * q);
		if (type == F_LOWPASS) {
			b0 = (1.0 - cosw) / 2.0;
			b1 = 1.0 - cosw;
			b2 = (1.0 - cosw) / 2.0;
		} else {
			b0 = (1.0 + cosw) / 2.0;
			b1 = -(1.0 + cosw);
			b2 = (1.0 + cosw) / 2.0;
		}
		a0 = 1.0 + alpha;
		a1 = -2.0 * cosw;
		a2 = 1.0 - alpha;
	} else if (type == F_PEAK) {
		double alpha = sinw / (2.0 * q);
		b0 = 1.0 + alpha * A;
		b1 = -2.0 * cosw;
		b2 = 1.0 - alpha * A;
		a0 = 1.0 + alpha / A;
		a1 = -2.0 * cosw;
		a2 = 1.0 - alpha / A;
	} else {
		double sqA = sqrt(A);
		// The shelf takes its width from Q, the same field and the same
		// arithmetic the peak uses, rather than from RBJ's shelf-slope form
		// with S fixed at 1.
		//
		// The two meet at Q = 1/sqrt(2): S = 1 gives alpha = sin(w0)/sqrt(2),
		// which is what sin(w0)/(2Q) gives at 0.7071. So this is not a new
		// shape so much as the one knob the stock table actually turns -- its
		// shelves sit at 0.7289 and 0.4425, near that point and a long way from
		// it respectively, and with S pinned neither number reached the filter
		// at all.
		double alpha = sinw / (2.0 * q);
		if (type == F_LOWSHELF) {
			b0 = A * ((A + 1.0) - (A - 1.0) * cosw + 2.0 * sqA * alpha);
			b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosw);
			b2 = A * ((A + 1.0) - (A - 1.0) * cosw - 2.0 * sqA * alpha);
			a0 = (A + 1.0) + (A - 1.0) * cosw + 2.0 * sqA * alpha;
			a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosw);
			a2 = (A + 1.0) + (A - 1.0) * cosw - 2.0 * sqA * alpha;
		} else {
			b0 = A * ((A + 1.0) + (A - 1.0) * cosw + 2.0 * sqA * alpha);
			b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw);
			b2 = A * ((A + 1.0) + (A - 1.0) * cosw - 2.0 * sqA * alpha);
			a0 = (A + 1.0) - (A - 1.0) * cosw + 2.0 * sqA * alpha;
			a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosw);
			a2 = (A + 1.0) - (A - 1.0) * cosw - 2.0 * sqA * alpha;
		}
	}

	f->b0 = (float)(b0 / a0);
	f->b1 = (float)(b1 / a0);
	f->b2 = (float)(b2 / a0);
	f->a1 = (float)(a1 / a0);
	f->a2 = (float)(a2 / a0);
	return true;
}

// The level the graphic equaliser and the parametric play at, in dB: the
// manual preamp plus whatever of the biggest boost it does not already cover.
// Never above the preamp, and never less attenuation than the boost.
static double headroom_db(double max_boost, double manual) {
	double covered = manual < 0.0 ? -manual : 0.0;
	double more = max_boost - covered;
	return manual - (more > 0.0 ? more : 0.0);
}

static void slot_update(int slot, int type, int freq, double gain_db, double q, int rate, bool rate_changed) {
	bool was_active = chain_active[slot];
	bool now_active = biquad_set_coeffs(&chain[slot], type, freq, gain_db, q, rate);
	chain_active[slot] = now_active;

	// The delay line survives a coefficient change (that is the anti-click);
	// it only resets when the filter comes to life or the rate changed.
	if (now_active && (!was_active || rate_changed)) {
		memset(chain[slot].z1, 0, sizeof(chain[slot].z1));
		memset(chain[slot].z2, 0, sizeof(chain[slot].z2));
	}
}

// |H|^2 at one point of the unit circle, from the four trigonometric terms
// rather than from w.
//
// Split out because the pre-gain below walks thirteen filters across a hundred
// and twenty-eight frequencies: the four values depend on the frequency and not
// on the filter, so computing them here would be four sines and cosines per
// filter per point -- some six thousand of them, in software on a core with no
// FPU, on the thread that is about to fill an audio buffer.
//
// A ratio and not decibels for the same reason. Filters in series multiply, so
// the caller multiplies these and takes one logarithm per point instead of one
// per filter.
static double biquad_power_ratio_at(const biquad_t *f, double cos1, double sin1, double cos2, double sin2) {
	// z^-1 = cos(w) - j sin(w), hence the minus signs on the imaginary parts.
	double num_re = (double)f->b0 + (double)f->b1 * cos1 + (double)f->b2 * cos2;
	double num_im = -((double)f->b1 * sin1 + (double)f->b2 * sin2);
	double den_re = 1.0 + (double)f->a1 * cos1 + (double)f->a2 * cos2;
	double den_im = -((double)f->a1 * sin1 + (double)f->a2 * sin2);

	double num2 = num_re * num_re + num_im * num_im;
	double den2 = den_re * den_re + den_im * den_im;
	if (den2 <= 0.0 || num2 <= 0.0) {
		return 1.0; // nothing sensible to report: read as 0 dB
	}
	return num2 / den2;
}

static double biquad_gain_db_at(const biquad_t *f, double w) {
	double ratio = biquad_power_ratio_at(f, cos(w), sin(w), cos(2.0 * w), sin(2.0 * w));
	// 10*log10 of the power ratio: it is already a squared magnitude, which
	// saves a square root.
	return 10.0 * log10(ratio);
}

// ---------------------------------------------------------------------------
// MSEB's own pre-gain
//
// The stock module does not take headroom off the peak of its curve. It
// measures the combined response of its bank at 128 logarithmic frequencies,
// weights those points on a curve of its own, and then searches for the offset
// in dB that balances what is left above the line against what is left below
// it. The answer can be negative, zero OR POSITIVE: a setting that cuts more
// than it boosts gets some level handed back.
//
// So it is a level normalisation and not a clip guard, and the two disagree
// almost everywhere. Four overlapping filters at +3 dB make a curve that peaks
// well above +3; a filter that boosts and one that cuts partly cancel; a curve
// that only cuts leaves the stock louder than a player that can only attenuate.
//
// It belongs to MSEB alone. The graphic equaliser and the parametric keep the
// separate headroom further down, which is a Sonix stage and is not this: the
// stock MSEB module has no idea they exist, and feeding their boosts into this
// search would bend an algorithm that was reverse-engineered, not designed
// here.
//
// The numbers below -- 128 points over three decades from 20 Hz, the two
// weight ramps, ten halvings between -100 and +100 dB, the 0.2 ratio and the
// 1e-6 floor -- are the stock module's, read out of it rather than chosen.
// ---------------------------------------------------------------------------

#define MSEB_PROBE_POINTS 128
#define MSEB_PROBE_DECADES 3.0
#define MSEB_PROBE_START_HZ 20.0
#define MSEB_PROBE_RISING 30 // the first points, weighted 1.0 up to 2.0
#define MSEB_SEARCH_ROUNDS 10
#define MSEB_SEARCH_RATIO 0.2f
#define MSEB_SEARCH_FLOOR 1.0e-6f

// The weights, which depend on nothing and are built once. The heaviest point
// is around 100 Hz and the weight falls away towards the top of the band, so
// what happens under the voice counts for more than what happens above it.
static float mseb_weight[MSEB_PROBE_POINTS];
static bool mseb_weight_ready;

static void mseb_weights_build(void) {
	if (mseb_weight_ready) {
		return;
	}
	for (int i = 0; i < MSEB_PROBE_RISING; i++) {
		mseb_weight[i] = 1.0f + (float)i / (float)(MSEB_PROBE_RISING - 1);
	}
	int falling = MSEB_PROBE_POINTS - MSEB_PROBE_RISING;
	for (int j = 0; j < falling; j++) {
		mseb_weight[MSEB_PROBE_RISING + j] = 0.5f + (((float)falling - (float)j) / (float)(falling + 1)) * 1.5f;
	}
	mseb_weight_ready = true;
}

// The MSEB bank as the settings describe it, on the caller's own biquads.
//
// Built here rather than read out of `chain` because two callers want it: the
// rebuild, which runs on the audio thread and has the chain in front of it, and
// the settings page, which runs on the interface thread while the audio thread
// is using those same filters -- and which is asked with nothing playing at
// all, when the chain holds whatever the last track left in it.
static int mseb_bank_build(biquad_t *out, int rate) {
	int count = 0;
	if (!mseb_enabled) {
		return 0;
	}
	for (int i = 0; i < MSEB_FILTERS; i++) {
		double gain = (double)mseb_value[MSEB_DEFS[i].slider] * (double)MSEB_DEFS[i].db_per_step;
		memset(&out[count], 0, sizeof(out[count]));
		if (biquad_set_coeffs(&out[count], MSEB_DEFS[i].type, MSEB_DEFS[i].freq, gain, (double)MSEB_DEFS[i].q, rate)) {
			count++;
		}
	}
	return count;
}

static float mseb_auto_preamp_db(int rate) {
	biquad_t bank[MSEB_FILTERS];
	int count = mseb_bank_build(bank, rate);
	if (count == 0) {
		return 0.0f; // nothing to normalise: unity, the way the stock leaves it
	}

	mseb_weights_build();

	float response[MSEB_PROBE_POINTS];
	for (int i = 0; i < MSEB_PROBE_POINTS; i++) {
		double freq = pow(10.0, log10(MSEB_PROBE_START_HZ) + MSEB_PROBE_DECADES * (double)i / MSEB_PROBE_POINTS);
		double w = 2.0 * M_PI * freq / (double)rate;
		// Above Nyquist there is nothing for the bank to act on, and the
		// transfer function evaluated past pi is the mirrored image rather than
		// an answer. The grid keeps the stock's shape -- its top point is just
		// under 19 kHz, which is inside Nyquist at every rate from 44.1 kHz --
		// and only a 32 kHz or slower file reaches this.
		if (w >= M_PI) {
			response[i] = 0.0f;
			continue;
		}
		double cos1 = cos(w), sin1 = sin(w);
		double cos2 = cos(2.0 * w), sin2 = sin(2.0 * w);
		double power = 1.0;
		for (int f = 0; f < count; f++) {
			power *= biquad_power_ratio_at(&bank[f], cos1, sin1, cos2, sin2);
		}
		response[i] = power > 0.0 ? (float)(10.0 * log10(power)) : 0.0f;
	}

	float low = -100.0f;
	float high = 100.0f;
	float p = 0.0f;

	for (int round = 0; round < MSEB_SEARCH_ROUNDS; round++) {
		p = (low + high) * 0.5f;

		float sum = 0.0f;	   // the weighted curve, offset by p, added up signed
		float negative = 0.0f; // and how much of it sits below the line

		for (int i = 0; i < MSEB_PROBE_POINTS; i++) {
			float x = (p + response[i]) * mseb_weight[i];
			sum += x;
			if (x < 0.0f) {
				negative -= x;
			}
		}

		if (negative < MSEB_SEARCH_FLOOR) {
			high = p; // nothing below the line: come down
			continue;
		}
		// sum is (above - below), so the ratio is (above - below) / below and
		// the target 0.2 puts the curve at about six parts above for five
		// below.
		if (sum / negative > MSEB_SEARCH_RATIO) {
			high = p;
		} else {
			low = p;
		}
	}

	return p;
}

// ---------------------------------------------------------------------------
// Clip guard
//
// Neither level stage above is a clip guard for the whole chain. MSEB's is a
// normalisation that balances its curve around the line, so at the top of its
// range -- bass0 and bass1 together are some +45 dB at 100 -- it leaves the bass
// tens of decibels over full scale. The graphic equaliser's and the
// parametric's headroom looks at the single biggest boost, and overlapping
// bands (a shelf and a peak on the same octave) add up past it. Either way the
// output stage clamps, and a clamped waveform is the harsh, broken sound of an
// equaliser pushed hard.
//
// So after both, the cascade of every live filter is measured with both levels
// applied -- on MSEB's 128-point grid widened to 20 kHz, plus each filter's
// own frequency, where a narrow peak would slip between grid points -- and if
// its highest point is above CLIP_GUARD_CEILING_DB the difference comes off the
// level as well. A curve that stays under it is left exactly as before.
// ---------------------------------------------------------------------------

#define CLIP_GUARD_POINTS 128
#define CLIP_GUARD_START_HZ 20.0
#define CLIP_GUARD_DECADES 3.0
// A little under full scale: a filter rings past its steady-state response on
// transients, and a master already sitting at 0 dBFS has nowhere to go.
#define CLIP_GUARD_CEILING_DB (-0.5)

// Every filter the chain would run, MSEB first, on the caller's own biquads.
// Like mseb_bank_build, usable from the interface thread.
static int full_bank_build(biquad_t *out, int *freqs, int rate) {
	int count = 0;
	if (mseb_enabled) {
		for (int i = 0; i < MSEB_FILTERS; i++) {
			double gain = (double)mseb_value[MSEB_DEFS[i].slider] * (double)MSEB_DEFS[i].db_per_step;
			memset(&out[count], 0, sizeof(out[count]));
			if (biquad_set_coeffs(&out[count], MSEB_DEFS[i].type, MSEB_DEFS[i].freq, gain, (double)MSEB_DEFS[i].q,
								  rate)) {
				freqs[count++] = MSEB_DEFS[i].freq;
			}
		}
	}
	if (eq_enabled) {
		for (int i = 0; i < EQ_BANDS; i++) {
			memset(&out[count], 0, sizeof(out[count]));
			if (biquad_set_coeffs(&out[count], F_PEAK, eq_band_freq[i], (double)band_gain_db[i], EQ_BAND_Q, rate)) {
				freqs[count++] = eq_band_freq[i];
			}
		}
	}
	if (peq_enabled) {
		for (int i = 0; i < PEQ_BANDS; i++) {
			const peq_band_t *b = &peq_bands[i];
			if (!b->on) {
				continue;
			}
			memset(&out[count], 0, sizeof(out[count]));
			if (biquad_set_coeffs(&out[count], peq_filter_type(b->type), b->freq, (double)b->gain_tenths / 10.0,
								  (double)b->q_cent / 100.0, rate)) {
				freqs[count++] = b->freq;
			}
		}
	}
	return count;
}

static double cascade_db_at(const biquad_t *bank, int count, double freq, int rate) {
	double w = 2.0 * M_PI * freq / (double)rate;
	if (w <= 0.0 || w >= M_PI) {
		return -INFINITY;
	}
	double cos1 = cos(w), sin1 = sin(w);
	double cos2 = cos(2.0 * w), sin2 = sin(2.0 * w);
	double power = 1.0;
	for (int f = 0; f < count; f++) {
		power *= biquad_power_ratio_at(&bank[f], cos1, sin1, cos2, sin2);
	}
	return power > 0.0 ? 10.0 * log10(power) : -INFINITY;
}

// The extra cut, in dB (zero or negative), that keeps the whole chain under
// the ceiling with `level_db` -- both pre-gains together -- already applied.
static double clip_guard_db(int rate, double level_db) {
	biquad_t bank[CHAIN_SLOTS];
	int freqs[CHAIN_SLOTS];
	int count = full_bank_build(bank, freqs, rate);
	if (count == 0) {
		return 0.0;
	}

	double peak = -INFINITY;
	for (int i = 0; i <= CLIP_GUARD_POINTS; i++) {
		double freq =
			pow(10.0, log10(CLIP_GUARD_START_HZ) + CLIP_GUARD_DECADES * (double)i / CLIP_GUARD_POINTS);
		double db = cascade_db_at(bank, count, freq, rate);
		if (db > peak) {
			peak = db;
		}
	}
	for (int f = 0; f < count; f++) {
		double db = cascade_db_at(bank, count, (double)freqs[f], rate);
		if (db > peak) {
			peak = db;
		}
	}

	double over = peak + level_db - CLIP_GUARD_CEILING_DB;
	return over > 0.0 ? -over : 0.0;
}

static void filters_rebuild(int rate) {
	bool rate_changed = rate != built_rate;
	double max_boost = 0;
	int active = 0;

	// MSEB's gains stay out of max_boost: its level is settled by its own
	// pre-gain, further down, on the algorithm the stock module uses.
	for (int i = 0; i < MSEB_FILTERS; i++) {
		double gain = mseb_enabled ? (double)mseb_value[MSEB_DEFS[i].slider] * MSEB_DEFS[i].db_per_step : 0.0;
		slot_update(i, MSEB_DEFS[i].type, MSEB_DEFS[i].freq, gain, MSEB_DEFS[i].q, rate, rate_changed);
		if (chain_active[i]) {
			active++;
		}
	}
	for (int i = 0; i < EQ_BANDS; i++) {
		double gain = eq_enabled ? (double)band_gain_db[i] : 0.0;
		slot_update(MSEB_FILTERS + i, F_PEAK, eq_band_freq[i], gain, EQ_BAND_Q, rate, rate_changed);
		if (chain_active[MSEB_FILTERS + i]) {
			active++;
			if (gain > max_boost) {
				max_boost = gain;
			}
		}
	}

	// The parametric. A band that is off still goes through slot_update with
	// zero gain, so it keeps its place in the chain: turning it back on does
	// not shift every other filter, which is what made the chain crackle when
	// it changed shape under the fingers.
	for (int i = 0; i < PEQ_BANDS; i++) {
		const peq_band_t *b = &peq_bands[i];
		int slot = PEQ_SLOT0 + i;
		bool live = peq_enabled && b->on;
		int type = peq_filter_type(b->type);

		// Low-pass and high-pass have no gain to zero, so they are switched off
		// by clearing the frequency, which biquad_set_coeffs rejects.
		double gain = live ? (double)b->gain_tenths / 10.0 : 0.0;
		int freq = (live || filter_uses_gain(type)) ? b->freq : 0;

		slot_update(slot, type, freq, gain, (double)b->q_cent / 100.0, rate, rate_changed);
		if (chain_active[slot]) {
			active++;
			if (gain > max_boost) {
				max_boost = gain;
			}
		}
	}

	// MSEB's own level, from its own bank. Its filters are the first thing the
	// sample meets, and this is applied immediately in front of them.
	mseb_pregain_db = mseb_auto_preamp_db(rate);
	mseb_pregain_target = (float)pow(10.0, (double)mseb_pregain_db / 20.0);

	// The anti-clip headroom (the stock's geq_pre_gain), for the graphic
	// equaliser and the parametric. Not applied as a step: eq_process ramps the
	// running gain toward this over ~20 ms, which is what kills the zipper
	// crackle while a slider is moving.
	//
	// The parametric's preamp and the automatic headroom are one budget. A
	// preset from AutoEq or Equalizer APO carries its own clip guard in its
	// preamp ("Preamp: -6.2 dB" for a curve whose peak is +6.2), and taking the
	// boost off again on top of it played the curve 6 dB quieter than it plays
	// anywhere else. So the headroom only takes what a negative preamp has not
	// already taken; a positive preamp, which guards nothing, is applied on top
	// of the full headroom.
	double manual = peq_enabled ? (double)peq_preamp_tenths / 10.0 : 0.0;
	double level_db = headroom_db(max_boost, manual);
	// Whatever the two stages still leave over full scale comes off here, in
	// front of the graphic equaliser and the parametric. The chain is float,
	// so where in it the cut is made does not matter, only that it is made
	// before the output clamps.
	double guard_db = clip_guard_db(rate, (double)mseb_pregain_db + level_db);
	pregain_target = (float)pow(10.0, (level_db + guard_db) / 20.0);
	if (rate_changed) {
		pregain = pregain_target;
		mseb_pregain = mseb_pregain_target;
	}

	built_version = settings_version;
	built_rate = rate;
	fprintf(stderr,
			"eq: rebuilt (%d filters at %d Hz, mseb pregain %+.2f dB -> %.4f, geq/peq pregain -> %.3f, clip guard "
			"%+.2f dB)\n",
			active, rate, (double)mseb_pregain_db, (double)mseb_pregain_target, (double)pregain_target, guard_db);
}

// ---------------------------------------------------------------------------
// The parametric's response, for the curve drawn in the UI
//
// Not a separately drawn curve: the same coefficients that run on the audio,
// evaluated on the unit circle. The magnitude of
//
//     H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2),  z = e^(jw)
//
// and in decibels the total is the sum, because the filters are in series.
//
// Runs on the UI thread and on its own biquads, not on the chain's: the audio
// thread is using those.
// ---------------------------------------------------------------------------

int peq_response_tenths(int freq, int sample_rate) {
	if (sample_rate <= 0 || freq <= 0 || freq * 2 >= sample_rate) {
		return 0;
	}

	double total_db = peq_enabled ? (double)peq_preamp_tenths / 10.0 : 0.0;
	double w = 2.0 * M_PI * (double)freq / (double)sample_rate;

	for (int i = 0; i < PEQ_BANDS; i++) {
		const peq_band_t *b = &peq_bands[i];
		if (!b->on) {
			continue;
		}
		int type = peq_filter_type(b->type);

		biquad_t f;
		memset(&f, 0, sizeof(f));
		if (!biquad_set_coeffs(&f, type, b->freq, (double)b->gain_tenths / 10.0, (double)b->q_cent / 100.0,
							   sample_rate)) {
			continue;
		}
		total_db += biquad_gain_db_at(&f, w);
	}

	// Rounded to the nearest tenth rather than truncated towards zero: half a
	// tenth lost at every point drags the whole curve below the real one.
	return (int)(total_db >= 0 ? total_db * 10.0 + 0.5 : total_db * 10.0 - 0.5);
}

// What the engine does to the level on its own, recomputed here for the UI
// rather than read from the running gains: those belong to the audio thread,
// only live while something is playing, and would report unity with the player
// stopped whatever the settings say.
//
// Two stages added together, because that is what the ear gets: MSEB's own
// normalisation, which can go either way, and the headroom taken off the
// graphic equaliser and the parametric, which only ever comes down.
//
// The MSEB half is asked at 44.1 kHz. It depends on the rate -- the response is
// measured on the unit circle -- and the page has no track in front of it; the
// caveat below about slower files covers the difference.
#define EQ_HEADROOM_REFERENCE_RATE 44100

int eq_auto_headroom_tenths(void) {
	double max_boost = 0;
	double total = (double)mseb_auto_preamp_db(EQ_HEADROOM_REFERENCE_RATE);

	if (eq_enabled) {
		for (int i = 0; i < EQ_BANDS; i++) {
			double gain = (double)band_gain_db[i];
			if (gain > max_boost) {
				max_boost = gain;
			}
		}
	}
	if (peq_enabled) {
		for (int i = 0; i < PEQ_BANDS; i++) {
			const peq_band_t *b = &peq_bands[i];
			if (!b->on || !filter_uses_gain(peq_filter_type(b->type))) {
				continue;
			}
			double gain = (double)b->gain_tenths / 10.0;
			if (gain > max_boost) {
				max_boost = gain;
			}
		}
	}

	// Only the automatic part: the preamp has its own slider, and the page
	// shows this beside it.
	double manual = peq_enabled ? (double)peq_preamp_tenths / 10.0 : 0.0;
	double level_db = headroom_db(max_boost, manual);
	total += level_db - manual;
	total += clip_guard_db(EQ_HEADROOM_REFERENCE_RATE, total + manual);

	return (int)(total >= 0 ? total * 10.0 + 0.5 : total * 10.0 - 0.5);
}

// The chain runs over a block in passes of up to EQ_PASS_FRAMES frames. A pass
// first works out the two pre-gains frame by frame, then runs the active
// filters over every frame of the pass, two at a time, with their coefficients
// and state held in locals and both channels together. Every sample goes through the same float
// operations, in the same order, as it would one sample at a time through the
// whole chain:
//
//     x -> * mseb_pregain -> MSEB bank -> * pregain -> geq + peq
//
// The stock MSEB module puts its coefficient on the samples immediately before
// its own filters and knows nothing of what follows; the headroom for the other
// two is Sonix's and sits in front of the filters it is protecting.
#define EQ_PASS_FRAMES 256

static float pass_buf[EQ_PASS_FRAMES * EQ_MAX_CHANNELS]; // interleaved, as the block
static float pass_mseb_gain[EQ_PASS_FRAMES];
static float pass_gain[EQ_PASS_FRAMES];

// Direct form II transposed, one channel.
static void biquad_run_mono(biquad_t *f, float *buf, int n) {
	const float b0 = f->b0, b1 = f->b1, b2 = f->b2, a1 = f->a1, a2 = f->a2;
	float z1 = f->z1[0], z2 = f->z2[0];
	for (int i = 0; i < n; i++) {
		float x = buf[i];
		float y = b0 * x + z1;
		z1 = b1 * x - a1 * y + z2;
		z2 = b2 * x - a2 * y;
		buf[i] = y;
	}
	f->z1[0] = z1;
	f->z2[0] = z2;
}

// The same, on an interleaved stereo buffer. The two channels are independent
// recursions, so the core works on one while the other waits for a result.
static void biquad_run_stereo(biquad_t *f, float *buf, int n) {
	const float b0 = f->b0, b1 = f->b1, b2 = f->b2, a1 = f->a1, a2 = f->a2;
	float z1l = f->z1[0], z2l = f->z2[0];
	float z1r = f->z1[1], z2r = f->z2[1];
	for (int i = 0; i < n; i++) {
		float xl = buf[2 * i];
		float xr = buf[2 * i + 1];
		float yl = b0 * xl + z1l;
		float yr = b0 * xr + z1r;
		z1l = b1 * xl - a1 * yl + z2l;
		z1r = b1 * xr - a1 * yr + z2r;
		z2l = b2 * xl - a2 * yl;
		z2r = b2 * xr - a2 * yr;
		buf[2 * i] = yl;
		buf[2 * i + 1] = yr;
	}
	f->z1[0] = z1l;
	f->z2[0] = z2l;
	f->z1[1] = z1r;
	f->z2[1] = z2r;
}

// Two filters in a row, `f` then `g`, in one walk of the buffer: each sample
// leaves `f` and goes straight into `g`. The operations on it are the ones the
// two single walks make, in the same order -- only the loads and stores of the
// buffer between them are gone.
static void biquad_run_mono2(biquad_t *f, biquad_t *g, float *buf, int n) {
	const float fb0 = f->b0, fb1 = f->b1, fb2 = f->b2, fa1 = f->a1, fa2 = f->a2;
	const float gb0 = g->b0, gb1 = g->b1, gb2 = g->b2, ga1 = g->a1, ga2 = g->a2;
	float fz1 = f->z1[0], fz2 = f->z2[0];
	float gz1 = g->z1[0], gz2 = g->z2[0];
	for (int i = 0; i < n; i++) {
		float x = buf[i];
		float y = fb0 * x + fz1;
		fz1 = fb1 * x - fa1 * y + fz2;
		fz2 = fb2 * x - fa2 * y;
		float w = gb0 * y + gz1;
		gz1 = gb1 * y - ga1 * w + gz2;
		gz2 = gb2 * y - ga2 * w;
		buf[i] = w;
	}
	f->z1[0] = fz1;
	f->z2[0] = fz2;
	g->z1[0] = gz1;
	g->z2[0] = gz2;
}

static void biquad_run_stereo2(biquad_t *f, biquad_t *g, float *buf, int n) {
	const float fb0 = f->b0, fb1 = f->b1, fb2 = f->b2, fa1 = f->a1, fa2 = f->a2;
	const float gb0 = g->b0, gb1 = g->b1, gb2 = g->b2, ga1 = g->a1, ga2 = g->a2;
	float fz1l = f->z1[0], fz2l = f->z2[0], fz1r = f->z1[1], fz2r = f->z2[1];
	float gz1l = g->z1[0], gz2l = g->z2[0], gz1r = g->z1[1], gz2r = g->z2[1];
	for (int i = 0; i < n; i++) {
		float xl = buf[2 * i];
		float xr = buf[2 * i + 1];
		float yl = fb0 * xl + fz1l;
		float yr = fb0 * xr + fz1r;
		fz1l = fb1 * xl - fa1 * yl + fz2l;
		fz1r = fb1 * xr - fa1 * yr + fz2r;
		fz2l = fb2 * xl - fa2 * yl;
		fz2r = fb2 * xr - fa2 * yr;
		float wl = gb0 * yl + gz1l;
		float wr = gb0 * yr + gz1r;
		gz1l = gb1 * yl - ga1 * wl + gz2l;
		gz1r = gb1 * yr - ga1 * wr + gz2r;
		gz2l = gb2 * yl - ga2 * wl;
		gz2r = gb2 * yr - ga2 * wr;
		buf[2 * i] = wl;
		buf[2 * i + 1] = wr;
	}
	f->z1[0] = fz1l;
	f->z2[0] = fz2l;
	f->z1[1] = fz1r;
	f->z2[1] = fz2r;
	g->z1[0] = gz1l;
	g->z2[0] = gz2l;
	g->z1[1] = gz1r;
	g->z2[1] = gz2r;
}

// The active filters of slots [from, to), in slot order, two at a time.
static void slots_run_pass(float *buf, int n, int channels, int from, int to) {
	biquad_t *held = NULL; // an active filter waiting for a second one
	for (int b = from; b < to; b++) {
		if (!chain_active[b]) {
			continue;
		}
		if (!held) {
			held = &chain[b];
			continue;
		}
		if (channels == 2) {
			biquad_run_stereo2(held, &chain[b], buf, n);
		} else {
			biquad_run_mono2(held, &chain[b], buf, n);
		}
		held = NULL;
	}
	if (held) {
		if (channels == 2) {
			biquad_run_stereo(held, buf, n);
		} else {
			biquad_run_mono(held, buf, n);
		}
	}
}

static void gain_run_pass(float *buf, int n, int channels, const float *gain) {
	for (int i = 0; i < n; i++) {
		for (int c = 0; c < channels; c++) {
			buf[i * channels + c] = buf[i * channels + c] * gain[i];
		}
	}
}

// The same with one gain for the whole pass. With `skip_unity` a gain of
// exactly 1 is not applied: the product would be the sample itself. Only for
// samples known not to be denormal -- converted from the decoder, or scaled
// from those -- since an FPU that flushes denormals turns x * 1 into zero.
static void gain_run_const(float *buf, int samples, float gain, bool skip_unity) {
	if (skip_unity && gain == 1.0f) {
		return;
	}
	for (int i = 0; i < samples; i++) {
		buf[i] = buf[i] * gain;
	}
}

static bool slots_any_active(int from, int to) {
	for (int b = from; b < to; b++) {
		if (chain_active[b]) {
			return true;
		}
	}
	return false;
}

// Both gains move toward their targets a step at a time rather than jumping.
// The stock MSEB module has no such ramp -- its coefficient is recomputed and
// used -- so this is a Sonix choice and a deliberate difference: it changes
// nothing about where the gain settles, only how it gets there, and without it
// a moving slider crackles.
static inline void pregain_glide(float step) {
	if (pregain < pregain_target) {
		pregain += step;
		if (pregain > pregain_target) {
			pregain = pregain_target;
		}
	} else if (pregain > pregain_target) {
		pregain -= step;
		if (pregain < pregain_target) {
			pregain = pregain_target;
		}
	}

	if (mseb_pregain < mseb_pregain_target) {
		mseb_pregain += step;
		if (mseb_pregain > mseb_pregain_target) {
			mseb_pregain = mseb_pregain_target;
		}
	} else if (mseb_pregain > mseb_pregain_target) {
		mseb_pregain -= step;
		if (mseb_pregain < mseb_pregain_target) {
			mseb_pregain = mseb_pregain_target;
		}
	}
}

// One pass through the chain: pass_buf holds `n` frames on the way in and on
// the way out. The pre-gains advance one step per frame, before that frame.
//
// Both pre-gains at their targets is the usual case -- they move only for the
// few milliseconds after a setting changes -- and then every frame of the pass
// would get the same two values: the pass takes them as constants.
static void chain_run_pass(int n, int channels, float step) {
	if (pregain == pregain_target && mseb_pregain == mseb_pregain_target) {
		bool mseb = slots_any_active(0, MSEB_FILTERS);
		gain_run_const(pass_buf, n * channels, mseb_pregain, true);
		slots_run_pass(pass_buf, n, channels, 0, MSEB_FILTERS);
		gain_run_const(pass_buf, n * channels, pregain, !mseb);
		slots_run_pass(pass_buf, n, channels, MSEB_FILTERS, CHAIN_SLOTS);
		return;
	}
	for (int i = 0; i < n; i++) {
		pregain_glide(step);
		pass_mseb_gain[i] = mseb_pregain;
		pass_gain[i] = pregain;
	}
	gain_run_pass(pass_buf, n, channels, pass_mseb_gain);
	slots_run_pass(pass_buf, n, channels, 0, MSEB_FILTERS);
	gain_run_pass(pass_buf, n, channels, pass_gain);
	slots_run_pass(pass_buf, n, channels, MSEB_FILTERS, CHAIN_SLOTS);
}

static bool process_prepare(int frame_count, int channels, int sample_rate) {
	if ((!eq_enabled && !mseb_enabled && !peq_enabled) || frame_count <= 0 || sample_rate <= 0) {
		return false;
	}
	if (channels > EQ_MAX_CHANNELS) {
		return false; // only mono/stereo hardware here
	}
	if (built_version != settings_version || built_rate != sample_rate) {
		filters_rebuild(sample_rate);
	}
	// Switched on but with nothing to do: every band at 0 dB (a flat preset,
	// or the PEQ on with its filters zeroed) and both pre-gains settled at
	// unity. The pass would still turn every sample into a float and back --
	// two conversions, a clamp and a rounding per sample for an identical
	// result, or worse than identical on the 32-bit path, where a float's 24
	// bits of mantissa round off the bottom of a sample that uses more. Only
	// once the gains have finished gliding, so a slider brought back to flat
	// still lands smoothly.
	if (!slots_any_active(0, CHAIN_SLOTS) && pregain == 1.0f && pregain_target == 1.0f && mseb_pregain == 1.0f &&
		mseb_pregain_target == 1.0f) {
		return false;
	}
	return true;
}

void eq_process(short *frames, int frame_count, int channels, int sample_rate) {
	if (!process_prepare(frame_count, channels, sample_rate)) {
		return;
	}

	// Per-sample pregain step sized for a ~20 ms glide at this rate.
	float step = 1.0f / ((float)sample_rate * 0.02f);

	for (int done = 0; done < frame_count; done += EQ_PASS_FRAMES) {
		int n = frame_count - done < EQ_PASS_FRAMES ? frame_count - done : EQ_PASS_FRAMES;
		short *in = frames + (size_t)done * channels;
		int samples = n * channels;

		for (int i = 0; i < samples; i++) {
			pass_buf[i] = (float)in[i];
		}
		chain_run_pass(n, channels, step);
		for (int i = 0; i < samples; i++) {
			float x = pass_buf[i];
			if (x > 32767.0f) {
				x = 32767.0f;
			} else if (x < -32768.0f) {
				x = -32768.0f;
			}
			in[i] = (short)(x >= 0 ? x + 0.5f : x - 0.5f);
		}
	}
}

void eq_process_s32(int32_t *frames, int frame_count, int channels, int sample_rate) {
	if (!process_prepare(frame_count, channels, sample_rate)) {
		return;
	}

	float step = 1.0f / ((float)sample_rate * 0.02f);

	// The filters run in the same float chain either width uses; the samples
	// are scaled down to the 16-bit range on the way in and back up on the way
	// out, so the coefficients' numeric range (and the shared z-state, when a
	// 16-bit track follows a 24-bit one) behaves identically in both paths.
	const float down = 1.0f / 65536.0f;
	for (int done = 0; done < frame_count; done += EQ_PASS_FRAMES) {
		int n = frame_count - done < EQ_PASS_FRAMES ? frame_count - done : EQ_PASS_FRAMES;
		int32_t *in = frames + (size_t)done * channels;
		int samples = n * channels;

		for (int i = 0; i < samples; i++) {
			pass_buf[i] = (float)in[i] * down;
		}
		chain_run_pass(n, channels, step);
		for (int i = 0; i < samples; i++) {
			float x = pass_buf[i];
			if (x > 32767.0f) {
				x = 32767.0f;
			} else if (x < -32768.0f) {
				x = -32768.0f;
			}
			in[i] = (int32_t)(x * 65536.0f);
		}
	}
}

// ---------------------------------------------------------------------------
// Soundfield. The stock module's arithmetic, kept in integers: the original
// normalises to float, does mid/side, and scales back; the same result comes
// out of
//
//     sum = L + R          d = (R - L) * width
//     L'  = (sum - d) / 2  R' = (sum + d) / 2
//
// with `width` in hundredths, which is exact at width = 1.00 (L' = L, R' = R)
// rather than merely very close -- a "no effect" setting that quietly dithers
// the bottom bit is worse than no module at all.
// ---------------------------------------------------------------------------

void soundfield_set_enabled(bool enabled) {
	sf_switch = enabled ? 1 : 0;
	sf_enabled = sf_switch && !direct_on;
	config_set_int("soundfield", "enabled", sf_switch);
	config_save();
}

bool soundfield_get_enabled(void) { return sf_switch != 0; }

void soundfield_set_width(int hundredths) {
	sf_width = clamp_width(hundredths);
	config_set_int("soundfield", "width", sf_width);
	config_save();
}

int soundfield_get_width(void) { return sf_width; }

void balance_set_enabled(bool enabled) {
	bal_switch = enabled ? 1 : 0;
	bal_enabled = bal_switch && !direct_on;
	config_set_int("balance", "enabled", bal_switch);
	config_save();
}

bool balance_get_enabled(void) { return bal_switch != 0; }

void balance_set_value(int tenths_db) {
	balance_set_gains(clamp_balance(tenths_db));
	config_set_int("balance", "value", bal_tenths);
	config_save();
}

int balance_get_value(void) { return bal_tenths; }

void balance_process(short *frames, int frame_count, int channels) {
	if (!bal_enabled || bal_tenths == BALANCE_CENTRE || channels != 2 || frame_count <= 0 || !frames) {
		return;
	}

	int gl = bal_left_q15;
	int gr = bal_right_q15;
	for (int n = 0; n < frame_count; n++) {
		// Attenuation only, so the product cannot leave the 16-bit range and
		// there is nothing to clamp.
		frames[n * 2] = (short)((frames[n * 2] * gl) >> 15);
		frames[n * 2 + 1] = (short)((frames[n * 2 + 1] * gr) >> 15);
	}
}

void balance_process_s32(int32_t *frames, int frame_count, int channels) {
	if (!bal_enabled || bal_tenths == BALANCE_CENTRE || channels != 2 || frame_count <= 0 || !frames) {
		return;
	}

	int64_t gl = bal_left_q15;
	int64_t gr = bal_right_q15;
	for (int n = 0; n < frame_count; n++) {
		frames[n * 2] = (int32_t)((frames[n * 2] * gl) >> 15);
		frames[n * 2 + 1] = (int32_t)((frames[n * 2 + 1] * gr) >> 15);
	}
}

void mono_set_enabled(bool enabled) {
	mono_switch = enabled ? 1 : 0;
	mono_enabled = mono_switch && !direct_on;
	config_set_int("audio", "mono", mono_switch);
	config_save();
}

bool mono_get_enabled(void) { return mono_switch != 0; }

// The mean of the two channels, rounded towards zero: it cannot leave the
// range of either.
void mono_process(short *frames, int frame_count, int channels) {
	if (!mono_enabled || channels != 2 || frame_count <= 0 || !frames) {
		return;
	}
	for (int n = 0; n < frame_count; n++) {
		short m = (short)(((int)frames[n * 2] + (int)frames[n * 2 + 1]) / 2);
		frames[n * 2] = m;
		frames[n * 2 + 1] = m;
	}
}

void mono_process_s32(int32_t *frames, int frame_count, int channels) {
	if (!mono_enabled || channels != 2 || frame_count <= 0 || !frames) {
		return;
	}
	for (int n = 0; n < frame_count; n++) {
		int32_t m = (int32_t)(((int64_t)frames[n * 2] + (int64_t)frames[n * 2 + 1]) / 2);
		frames[n * 2] = m;
		frames[n * 2 + 1] = m;
	}
}

void soundfield_process(short *frames, int frame_count, int channels) {
	int w = sf_width;
	// Stereo only, exactly as the original: there is no width to speak of in a
	// mono stream, and the module has nothing to say about anything else.
	if (!sf_enabled || w == 100 || channels != 2 || frame_count <= 0 || !frames) {
		return;
	}

	for (int n = 0; n < frame_count; n++) {
		int l = frames[n * 2];
		int r = frames[n * 2 + 1];
		int sum = l + r;
		int d = ((r - l) * w) / 100;

		int out_l = (sum - d) / 2;
		int out_r = (sum + d) / 2;

		if (out_l > 32767) {
			out_l = 32767;
		} else if (out_l < -32768) {
			out_l = -32768;
		}
		if (out_r > 32767) {
			out_r = 32767;
		} else if (out_r < -32768) {
			out_r = -32768;
		}

		frames[n * 2] = (short)out_l;
		frames[n * 2 + 1] = (short)out_r;
	}
}

void soundfield_process_s32(int32_t *frames, int frame_count, int channels) {
	int w = sf_width;
	if (!sf_enabled || w == 100 || channels != 2 || frame_count <= 0 || !frames) {
		return;
	}

	for (int n = 0; n < frame_count; n++) {
		// 64-bit throughout: L + R already overflows 32 bits, and the
		// difference times a width of 2.00 overflows it twice over.
		int64_t l = frames[n * 2];
		int64_t r = frames[n * 2 + 1];
		int64_t sum = l + r;
		int64_t d = ((r - l) * w) / 100;

		int64_t out_l = (sum - d) / 2;
		int64_t out_r = (sum + d) / 2;

		if (out_l > INT32_MAX) {
			out_l = INT32_MAX;
		} else if (out_l < INT32_MIN) {
			out_l = INT32_MIN;
		}
		if (out_r > INT32_MAX) {
			out_r = INT32_MAX;
		} else if (out_r < INT32_MIN) {
			out_r = INT32_MIN;
		}

		frames[n * 2] = (int32_t)out_l;
		frames[n * 2 + 1] = (int32_t)out_r;
	}
}

// ---------------------------------------------------------------------------
// Crossfeed -- see eq.h for what it does and why.
// ---------------------------------------------------------------------------

// The expensive part (one exp and two divisions) runs once, never from the
// middle of a buffer: only when something actually changed.
static void crossfeed_prepare(int sample_rate) {
	if (sample_rate <= 0) {
		return;
	}
	if (cf_ready_rate == sample_rate && cf_ready_cutoff == cf_cutoff && cf_ready_delay_us == cf_delay_us) {
		return;
	}

	cf_ready_rate = sample_rate;
	cf_ready_cutoff = cf_cutoff;
	cf_ready_delay_us = cf_delay_us;

	// One-pole low-pass: y += alpha * (x - y), with
	// alpha = 1 - exp(-2*pi*fc/fs). The simplest filter there is, and enough
	// here -- the head is not a steep filter either.
	double alpha = 1.0 - exp(-2.0 * 3.14159265358979 * (double)cf_cutoff / (double)sample_rate);
	if (alpha > 1.0) {
		alpha = 1.0;
	}
	if (alpha < 0.0) {
		alpha = 0.0;
	}
	cf_alpha_q15 = (int)(alpha * 32768.0 + 0.5);

	int samples = (int)(((long long)cf_delay_us * sample_rate + 500000) / 1000000);
	if (samples < 0) {
		samples = 0;
	}
	if (samples > CF_DELAY_MAX_SAMPLES - 1) {
		samples = CF_DELAY_MAX_SAMPLES - 1;
	}
	cf_delay_samples = samples;

	// Strength and normalisation. A mono signal (L == R) would come out at
	// 1 + feed, so everything is divided by that: switching crossfeed on does
	// not raise the level or push a track already at full scale into clipping.
	double feed = (double)cf_level / 100.0;
	double norm = 1.0 / (1.0 + feed);
	cf_direct_q15 = (int)(norm * 32768.0 + 0.5);
	cf_feed_q15 = (int)(feed * norm * 32768.0 + 0.5);
}

void crossfeed_reset(void) {
	memset(&cf_left, 0, sizeof(cf_left));
	memset(&cf_right, 0, sizeof(cf_right));
}

void crossfeed_set_enabled(bool enabled) {
	cf_switch = enabled ? 1 : 0;
	cf_enabled = cf_switch && !direct_on;
	if (cf_enabled) {
		crossfeed_reset(); // no tail of the previous track when it comes back on
	}
	config_set_int("crossfeed", "enabled", cf_switch);
	config_save();
}

bool crossfeed_get_enabled(void) { return cf_switch != 0; }

void crossfeed_set_level(int percent) {
	cf_level = clamp_step(percent, CROSSFEED_LEVEL_MIN, CROSSFEED_LEVEL_MAX, CROSSFEED_LEVEL_STEP);
	cf_ready_rate = 0; // force the recompute
	config_set_int("crossfeed", "level", cf_level);
	config_save();
}

int crossfeed_get_level(void) { return cf_level; }

void crossfeed_set_cutoff(int hz) {
	cf_cutoff = clamp_step(hz, CROSSFEED_CUTOFF_MIN, CROSSFEED_CUTOFF_MAX, CROSSFEED_CUTOFF_STEP);
	cf_ready_rate = 0;
	config_set_int("crossfeed", "cutoff", cf_cutoff);
	config_save();
}

int crossfeed_get_cutoff(void) { return cf_cutoff; }

void crossfeed_set_delay(int microseconds) {
	cf_delay_us = clamp_step(microseconds, CROSSFEED_DELAY_MIN, CROSSFEED_DELAY_MAX, CROSSFEED_DELAY_STEP);
	cf_ready_rate = 0;
	config_set_int("crossfeed", "delay_us", cf_delay_us);
	config_save();
}

int crossfeed_get_delay(void) { return cf_delay_us; }

// One sample into the line, one out of the other end, low-passed. A delay of
// zero samples means no trip around the head: the sample itself is read back,
// not the previous one.
static inline int32_t cf_step(cf_channel_t *c, int32_t sample) {
	c->line[c->pos] = sample;
	int read = c->pos - cf_delay_samples;
	if (read < 0) {
		read += CF_DELAY_MAX_SAMPLES;
	}
	int32_t delayed = c->line[read];
	c->pos++;
	if (c->pos >= CF_DELAY_MAX_SAMPLES) {
		c->pos = 0;
	}
	c->lp += (int32_t)(((int64_t)(delayed - c->lp) * cf_alpha_q15) >> 15);
	return c->lp;
}

void crossfeed_process(short *frames, int frame_count, int channels, int sample_rate) {
	if (!cf_enabled || channels != 2 || frame_count <= 0 || !frames) {
		return;
	}
	crossfeed_prepare(sample_rate);

	for (int n = 0; n < frame_count; n++) {
		int32_t l = frames[n * 2];
		int32_t r = frames[n * 2 + 1];

		// Both channels are filtered before the mix: what reaches an ear is the
		// opposite channel as it was, not an already mixed signal.
		int32_t lp_l = cf_step(&cf_left, l);
		int32_t lp_r = cf_step(&cf_right, r);

		int32_t out_l = (int32_t)(((int64_t)l * cf_direct_q15 + (int64_t)lp_r * cf_feed_q15) >> 15);
		int32_t out_r = (int32_t)(((int64_t)r * cf_direct_q15 + (int64_t)lp_l * cf_feed_q15) >> 15);

		if (out_l > 32767) {
			out_l = 32767;
		}
		if (out_l < -32768) {
			out_l = -32768;
		}
		if (out_r > 32767) {
			out_r = 32767;
		}
		if (out_r < -32768) {
			out_r = -32768;
		}

		frames[n * 2] = (short)out_l;
		frames[n * 2 + 1] = (short)out_r;
	}
}

void crossfeed_process_s32(int32_t *frames, int frame_count, int channels, int sample_rate) {
	if (!cf_enabled || channels != 2 || frame_count <= 0 || !frames) {
		return;
	}
	crossfeed_prepare(sample_rate);

	// At 32 bits every intermediate stays in 64: the sample already fills most
	// of the word, and multiplying it by a Q15 factor would overflow it.
	for (int n = 0; n < frame_count; n++) {
		int64_t l = frames[n * 2];
		int64_t r = frames[n * 2 + 1];

		// The delay line holds the shifted-down version: the low-pass works on
		// smaller numbers and full resolution buys nothing here, since this
		// signal is attenuated and summed under the direct one anyway.
		int32_t lp_l = cf_step(&cf_left, (int32_t)(l >> 8));
		int32_t lp_r = cf_step(&cf_right, (int32_t)(r >> 8));

		// Multiplied by 256 rather than shifted left by eight: left-shifting a
		// negative value is undefined behaviour in C, and samples are negative
		// half the time. The compiler emits a shift either way.
		int64_t out_l = (l * cf_direct_q15 + (int64_t)lp_r * 256 * cf_feed_q15) >> 15;
		int64_t out_r = (r * cf_direct_q15 + (int64_t)lp_l * 256 * cf_feed_q15) >> 15;

		if (out_l > INT32_MAX) {
			out_l = INT32_MAX;
		}
		if (out_l < INT32_MIN) {
			out_l = INT32_MIN;
		}
		if (out_r > INT32_MAX) {
			out_r = INT32_MAX;
		}
		if (out_r < INT32_MIN) {
			out_r = INT32_MIN;
		}

		frames[n * 2] = (int32_t)out_l;
		frames[n * 2 + 1] = (int32_t)out_r;
	}
}
