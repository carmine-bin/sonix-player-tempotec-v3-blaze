#ifndef METERVIEW_H
#define METERVIEW_H

#include "lvgl/lvgl.h"

#include <stdbool.h>

// The level meter on the now-playing page: a page of its own over the sleeve,
// the way the lyrics are, drawn from what is being heard (levelmeter.h).
//
// A swipe up on the sleeve brings it in from the bottom and a swipe down puts
// it away; the other directions are taken (right to left the lyrics, left to
// right the player itself, down from the top the control centre). A tap on it
// moves to the next of the three:
//
//   Peak      one bar per channel, -48..0 dBFS. Rises at once, falls 24 dB in
//             2.8 s, with the highest of the last 1.5 s held as a line and
//             written out in dB.
//   VU        the same two bars on a VU's 300 ms integration, -20..+3 VU with
//             0 VU at -18 dBFS: how loud it sounds rather than how high it
//             reaches.
//   Spectrum  twenty bands from 60 Hz up, a 1024-point FFT of the mono sum,
//             -66..0 dB, falling 20 dB a second.
//
// In all three a red CLIP lights for two seconds when the output clips -- a
// sample at full scale after the effects, which the output stage clamps.
//
// The kind shown is saved in the config (Music > Now Playing picks it too);
// whether the page is up lasts while the player runs, as the lyrics' does.
// It is measured only while the page is on screen: the player open, the screen
// on. Otherwise its timer slows to a check every half second and the playback
// thread stops measuring.

typedef enum {
	METER_PEAK = 0,
	METER_VU = 1,
	METER_SPECTRUM = 2,
	METER_MODES
} meter_mode_t;

meter_mode_t meterview_mode(void);
void meterview_set_mode(meter_mode_t mode);

// Whether the page is up (or on its way), for the player: the lyrics' swipe
// does not start over it.
bool meterview_is_up(void);

// Built once by the player, on its artwork panel. `where` says whether the page
// may be brought in now and, if so, the sleeve's area relative to the panel.
void meterview_create(lv_obj_t *panel, bool (*where)(lv_area_t *rel));

#endif /* METERVIEW_H */
