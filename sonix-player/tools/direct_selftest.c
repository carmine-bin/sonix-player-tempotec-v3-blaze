// Host self-check for Direct (src/system/audio/eq.h).
//
// Switches every effect on with settings that change the samples, checks that
// they do, then switches Direct on and checks that every process function
// leaves a block of noise exactly as it found it, in 16 and in 32 bits, and
// that each effect still reports its own switch as on. Direct off again, and
// the block must be changed once more.
//
//   gcc -O2 -Wall -I. tools/direct_selftest.c src/system/audio/eq.c src/system/audio/replaygain.c
//     src/system/core/config.c -lpthread -lm -o /tmp/direct_selftest   (one line)
//   /tmp/direct_selftest [dir]      (dir defaults to $TMPDIR or /tmp)

#include "src/system/audio/eq.h"
#include "src/system/audio/replaygain.h"
#include "src/system/core/config.h"
#include "src/system/library/metadata.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FRAMES 4096
#define RATE 44100

static int16_t src16[FRAMES * 2], buf16[FRAMES * 2];
static int32_t src32[FRAMES * 2], buf32[FRAMES * 2];

// The whole chain in the order audio.c runs it.
static void chain16(void) {
	memcpy(buf16, src16, sizeof(buf16));
	replaygain_process(buf16, FRAMES, 2);
	eq_process(buf16, FRAMES, 2, RATE);
	soundfield_process(buf16, FRAMES, 2);
	crossfeed_process(buf16, FRAMES, 2, RATE);
	mono_process(buf16, FRAMES, 2);
	balance_process(buf16, FRAMES, 2);
}

static void chain32(void) {
	memcpy(buf32, src32, sizeof(buf32));
	replaygain_process_s32(buf32, FRAMES, 2);
	eq_process_s32(buf32, FRAMES, 2, RATE);
	soundfield_process_s32(buf32, FRAMES, 2);
	crossfeed_process_s32(buf32, FRAMES, 2, RATE);
	mono_process_s32(buf32, FRAMES, 2);
	balance_process_s32(buf32, FRAMES, 2);
}

static bool same16(void) { return memcmp(buf16, src16, sizeof(buf16)) == 0; }
static bool same32(void) { return memcmp(buf32, src32, sizeof(buf32)) == 0; }

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : (getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	char ini[600];
	snprintf(ini, sizeof(ini), "%s/direct_selftest.ini", dir);
	unlink(ini);
	config_init(ini);
	eq_init();

	// Noise at about a quarter of full scale, so nothing below clips its way
	// back to the input by accident.
	uint32_t seed = 12345;
	for (int i = 0; i < FRAMES * 2; i++) {
		seed = seed * 1664525u + 1013904223u;
		src16[i] = (int16_t)((int32_t)(seed >> 16) - 32768) / 4;
		src32[i] = (int32_t)src16[i] * 65536;
	}

	assert(!direct_get_enabled());
	chain16();
	chain32();
	assert(same16() && same32()); // a fresh config has every effect off

	// Each effect on, one at a time, with a setting that must change the block.
	eq_set_band(5, 6);
	eq_set_enabled(true);
	chain16();
	chain32();
	assert(!same16() && !same32());
	eq_set_enabled(false);

	mseb_set_value(0, 5);
	mseb_set_enabled(true);
	chain16();
	chain32();
	assert(!same16() && !same32());
	mseb_set_enabled(false);

	peq_band_t band;
	peq_get_band(0, &band);
	band.on = true;
	band.freq = 1000;
	band.gain_tenths = 60;
	peq_set_band(0, &band);
	peq_set_enabled(true);
	chain16();
	chain32();
	assert(!same16() && !same32());
	peq_set_enabled(false);

	soundfield_set_width(150);
	soundfield_set_enabled(true);
	chain16();
	chain32();
	assert(!same16() && !same32());
	soundfield_set_enabled(false);

	crossfeed_set_enabled(true);
	chain16();
	chain32();
	assert(!same16() && !same32());
	crossfeed_set_enabled(false);

	mono_set_enabled(true);
	chain16();
	chain32();
	assert(!same16() && !same32());
	mono_set_enabled(false);

	balance_set_value(-60);
	balance_set_enabled(true);
	chain16();
	chain32();
	assert(!same16() && !same32());

	balance_set_enabled(false);

	// ReplayGain, with a track that asks for six decibels less.
	static song_metadata_t meta;
	meta.has_track_gain = true;
	meta.track_gain_db = -6.0;
	meta.track_peak = 0.5;
	replaygain_set_mode(REPLAYGAIN_TRACK);
	replaygain_load(&meta);
	assert(replaygain_active());
	chain16();
	chain32();
	assert(!same16() && !same32());
	balance_set_enabled(true);

	// All of them at once, then Direct over them.
	eq_set_enabled(true);
	mseb_set_enabled(true);
	peq_set_enabled(true);
	soundfield_set_enabled(true);
	crossfeed_set_enabled(true);
	mono_set_enabled(true);
	chain16();
	chain32();
	assert(!same16() && !same32());

	direct_set_enabled(true);
	assert(direct_get_enabled());
	// Twice: the first block after a switch is where a ramp or a filter's
	// tail would show.
	for (int pass = 0; pass < 2; pass++) {
		chain16();
		chain32();
		assert(same16() && same32());
	}
	// The switches say what comes back, not what is acting.
	assert(eq_get_enabled() && mseb_get_enabled() && peq_get_enabled() && soundfield_get_enabled() &&
		   crossfeed_get_enabled() && mono_get_enabled() && balance_get_enabled());
	assert(config_get_int("eq", "enabled", 0) == 1 && config_get_int("audio", "direct", 0) == 1);
	// ReplayGain keeps its mode and the track's figure, and applies nothing.
	assert(!replaygain_active() && replaygain_mode() == REPLAYGAIN_TRACK);

	// A switch moved while Direct is on is remembered and still not applied.
	eq_set_enabled(false);
	eq_set_enabled(true);
	crossfeed_set_enabled(false);
	crossfeed_set_enabled(true);
	chain16();
	chain32();
	assert(same16() && same32());

	// What a restart finds: Direct on, the effects waiting behind it.
	eq_init();
	assert(direct_get_enabled() && eq_get_enabled() && balance_get_enabled());
	chain16();
	chain32();
	assert(same16() && same32());

	direct_set_enabled(false);
	assert(replaygain_active());
	chain16();
	chain32();
	assert(!same16() && !same32());

	unlink(ini);
	printf("selftest: Direct leaves 16- and 32-bit blocks untouched with all seven effects and ReplayGain on, and gives them back\n");
	return 0;
}
