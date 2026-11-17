#include "replaygain.h"

#include "src/system/core/config.h"
#include "src/system/playback/playlist.h"

#include <math.h>
#include <stdio.h>

// See replaygain.h for what this is and why nothing is analysed here.

// Bounds on the correction. A tag asking for more than +12 dB is either wrong
// or a very quiet recording; either way that much boost is dangerous once the
// next, untagged track starts.
#define GAIN_MAX_DB 12.0
#define GAIN_MIN_DB (-30.0)

// The gain, in Q15: 32768 is untouched. Read on the audio thread, written on
// the interface thread at each track change -- a single aligned int, and the
// worst a torn read could do is apply the previous track's gain to one block.
static volatile int gain_q15 = 32768;
static volatile int active;
static volatile int bypass; // Direct (eq.h): the correction is known and not applied
static volatile double current_db;

replaygain_mode_t replaygain_mode(void) {
	long value = config_get_int("audio", "replaygain", REPLAYGAIN_OFF);
	if (value != REPLAYGAIN_TRACK && value != REPLAYGAIN_ALBUM && value != REPLAYGAIN_TRACK_WHEN_SHUFFLED) {
		return REPLAYGAIN_OFF;
	}
	return (replaygain_mode_t)value;
}

void replaygain_set_mode(replaygain_mode_t mode) {
	config_set_int("audio", "replaygain", (long)mode);
	config_save();
}

void replaygain_load(const song_metadata_t *meta) {
	replaygain_mode_t mode = replaygain_mode();

	gain_q15 = 32768;
	active = 0;
	current_db = 0;

	if (!meta || mode == REPLAYGAIN_OFF) {
		return;
	}

	if (mode == REPLAYGAIN_TRACK_WHEN_SHUFFLED) {
		playback_mode_t playback_mode = (playback_mode_t)config_get_int("player", "playback_mode", 0);
		if ( playback_mode == PLAYBACK_MODE_SHUFFLE || playback_mode == PLAYBACK_MODE_SHUFFLE_REPEAT ) {
			mode = REPLAYGAIN_TRACK;
		} else {
			mode = REPLAYGAIN_ALBUM;
		}
	}

	double db;
	double peak;
	if (mode == REPLAYGAIN_ALBUM && meta->has_album_gain) {
		db = meta->album_gain_db;
		peak = meta->album_peak;
	} else if (meta->has_track_gain) {
		// Album mode falls back to the track figure rather than to nothing: a
		// file tagged by a track-only tagger is still better corrected than
		// left alone, and it is what every other player does.
		db = meta->track_gain_db;
		peak = meta->track_peak;
	} else if (meta->has_album_gain) {
		db = meta->album_gain_db;
		peak = meta->album_peak;
	} else {
		return; // nothing to go on
	}

	if (db > GAIN_MAX_DB) {
		db = GAIN_MAX_DB;
	}
	if (db < GAIN_MIN_DB) {
		db = GAIN_MIN_DB;
	}

	double factor = pow(10.0, db / 20.0);

	// The peak keeps the correction from clipping: a track measured at 0.95
	// full scale takes about half a decibel before the loudest sample folds
	// over, and the fold-over is more audible than the loudness difference.
	if (peak > 0.0 && factor * peak > 1.0) {
		factor = 1.0 / peak;
		db = 20.0 * log10(factor);
	}

	int q = (int)(factor * 32768.0 + 0.5);
	if (q < 1) {
		q = 1;
	}
	if (q > 32768 * 4) {
		q = 32768 * 4; // +12 dB, the ceiling above, expressed in the same units
	}

	gain_q15 = q;
	current_db = db;
	active = (q != 32768);

	printf("replaygain: %s %+.2f dB (peak %.4f)\n", mode == REPLAYGAIN_ALBUM ? "album" : "track", db, peak);
}

bool replaygain_active(void) { return active != 0 && !bypass; }

void replaygain_set_bypass(bool on) { bypass = on ? 1 : 0; }
double replaygain_current_db(void) { return current_db; }

void replaygain_process(short *frames, int frame_count, int channels) {
	int g = gain_q15;
	if (!active || bypass || g == 32768 || frame_count <= 0 || channels <= 0 || !frames) {
		return;
	}

	int samples = frame_count * channels;
	for (int i = 0; i < samples; i++) {
		// The product can exceed 16 bits when the gain is above unity. The peak
		// check above makes that rare, not impossible: a tag can lie about its
		// own peak.
		int v = (frames[i] * g) >> 15;
		if (v > 32767) {
			v = 32767;
		} else if (v < -32768) {
			v = -32768;
		}
		frames[i] = (short)v;
	}
}

void replaygain_process_s32(int32_t *frames, int frame_count, int channels) {
	int64_t g = gain_q15;
	if (!active || bypass || g == 32768 || frame_count <= 0 || channels <= 0 || !frames) {
		return;
	}

	int samples = frame_count * channels;
	for (int i = 0; i < samples; i++) {
		int64_t v = ((int64_t)frames[i] * g) >> 15;
		if (v > INT32_MAX) {
			v = INT32_MAX;
		} else if (v < INT32_MIN) {
			v = INT32_MIN;
		}
		frames[i] = (int32_t)v;
	}
}
