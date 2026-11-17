#ifndef METERVIEW_H
#define METERVIEW_H

#include "lvgl/lvgl.h"

#include <stdbool.h>

// The level meter on the now-playing page: a page of its own over the sleeve,
// the way the lyrics are, drawn from what is being heard (levelmeter.h).
//
// A swipe up on the sleeve brings it in from the bottom and a swipe down puts
// it away; the other directions are taken (right to left the lyrics, left to
// right the player itself, down from the top the control centre). A tap
// on one of the two names along the bottom shows that one, and a tap anywhere
// else moves to the other:
//
//   Spectrum  24 bands from 40 Hz to 20 kHz, a 2048-point FFT of the mono sum,
//             -66..0 dB, falling 20 dB a second, a cap over each band held a
//             moment. Above it a thin peak bar per channel, -48..0 dBFS, rising
//             at once and falling 24 dB in 2.8 s, with the highest of the last
//             1.5 s held as a line and written out in dB.
//   VU        two analogue dials, a needle each: a VU's 300 ms integration,
//             -20..+3 VU with 0 VU at -18 dBFS, on the voltage scale a real VU
//             is printed with. How loud it sounds rather than how high it
//             reaches.
//
// On both a red CLIP lights for two seconds when the output clips -- a sample
// at full scale after the effects, which the output stage clamps -- and two
// lines under the top row say what is playing and what it goes through.
//
// The page shown is saved in the config (Music > Now Playing picks it too);
// whether the page is up lasts while the player runs, as the lyrics' does.
// The playback thread measures while the player is open (or the page is up)
// and the screen on, so the page has the music ready the moment it comes in;
// it is drawn only while it is on screen. Otherwise the timer slows down and
// the measuring stops.

typedef enum {
	METER_SPECTRUM = 0,
	METER_VU = 1,
	METER_MODES
} meter_mode_t;

meter_mode_t meterview_mode(void);
void meterview_set_mode(meter_mode_t mode);

// Whether the page is up (or on its way), for the player: the lyrics' swipe
// does not start over it.
bool meterview_is_up(void);

// Built once by the player, on its artwork panel. `where` says whether the page
// may be brought in now and, if so, the sleeve's area relative to the panel.
// `look` is told where the page is (0..1000) whenever it moves, so the player
// can push the sleeve out ahead of it and put the blurred sleeve behind the
// screen as the lyrics do; it answers whether that blurred picture is up, and
// the page is drawn clear over it then, on a dark ground of its own otherwise.
void meterview_create(lv_obj_t *panel, bool (*where)(lv_area_t *rel), bool (*look)(int32_t progress, int32_t height));

#endif /* METERVIEW_H */
