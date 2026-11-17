#ifndef METERVIEW_H
#define METERVIEW_H

#include "lvgl/lvgl.h"

#include <stdbool.h>

// The level meter on the now-playing page: a band across the bottom of the
// sleeve, drawn from what is being heard (levelmeter.h).
//
//   Peak      one bar per channel, -48..0 dBFS. Rises at once, falls 24 dB in
//             2.8 s, with the highest of the last 1.5 s held as a line.
//   VU        the same two bars on a VU's 300 ms integration, -20..+3 VU with
//             0 VU at -18 dBFS: how loud it sounds rather than how high it
//             reaches.
//   Spectrum  twenty bands from 60 Hz up, a 1024-point FFT of the mono sum,
//             -66..0 dB, falling 20 dB a second.
//
// In every mode a red mark at the right lights for two seconds when the output
// clips -- a sample at full scale after the effects, which the output stage
// clamps -- and stays dark otherwise.
//
// The setting is in Music > Now Playing and saved in the config. The meter is
// worked only while it is on screen: the player open, the screen on, no lyrics
// over the sleeve. Otherwise its timer slows to a check every half second and
// the playback thread is told to stop measuring.

typedef enum {
	METER_OFF = 0,
	METER_PEAK = 1,
	METER_VU = 2,
	METER_SPECTRUM = 3,
} meter_mode_t;

meter_mode_t meterview_mode(void);
void meterview_set_mode(meter_mode_t mode);

// Built once by the player, on its artwork panel. `where` says whether the band
// may show now and, if so, where: an area relative to the panel.
void meterview_create(lv_obj_t *panel, bool (*where)(lv_area_t *rel));

#endif /* METERVIEW_H */
