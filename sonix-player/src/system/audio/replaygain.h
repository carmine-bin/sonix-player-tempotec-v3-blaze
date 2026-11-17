#ifndef REPLAYGAIN_H
#define REPLAYGAIN_H

#include <stdbool.h>
#include <stdint.h>

#include "src/system/library/metadata.h"

// ReplayGain: playing a whole card at one loudness.
//
// Whoever tagged the file worked out its loudness once and wrote the
// correction into the tags; the player applies it. Nothing is analysed here --
// an analysis pass over a card of music is minutes of work this device should
// not be doing.
//
// Four states, the same three the stock player offers and an extra option
// (`settings_replaygain_vg_off` / `_track` / `_album` in its binary): off, per
// track, per album, track when shuffled. Track and album are not degrees of 
// strictness: correcting each track of a record separately flattens a movement 
// the producer meant to be quiet, so an album is corrected by one figure and a 
// shuffle by each track's own.
// 'Track when shuffled' uses track gain if shuffle mode is on, album gain 
// otherwise.

typedef enum {
	REPLAYGAIN_OFF = 0,
	REPLAYGAIN_TRACK = 1,
	REPLAYGAIN_ALBUM = 2,
	REPLAYGAIN_TRACK_WHEN_SHUFFLED = 3,
} replaygain_mode_t;

replaygain_mode_t replaygain_mode(void);
void replaygain_set_mode(replaygain_mode_t mode);

// Called at each track change with what the tags said. Works out the gain once
// and hands it to the audio thread; a file with no ReplayGain tags leaves
// playback untouched, whatever the mode.
void replaygain_load(const song_metadata_t *meta);

// What is being applied right now, in dB, and whether anything is. For the
// interface to be able to say so.
bool replaygain_active(void);

// Direct (eq.h): the correction stays loaded for each track and is not applied
// while this is on, and replaygain_active() answers no.
void replaygain_set_bypass(bool on);
double replaygain_current_db(void);

// In place on interleaved PCM, audio thread only. A no-op when off or when the
// track carries no usable tag.
void replaygain_process(short *frames, int frame_count, int channels);
void replaygain_process_s32(int32_t *frames, int frame_count, int channels);

#endif /* REPLAYGAIN_H */
