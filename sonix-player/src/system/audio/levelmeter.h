#ifndef LEVELMETER_H
#define LEVELMETER_H

#include <stdbool.h>
#include <stdint.h>

// What the level meters on the now-playing page are drawn from.
//
// The playback thread hands over every block at the very end of the effects
// chain -- after ReplayGain, the equalisers, soundfield, crossfeed, mono and
// balance -- which is the signal the DAC is given. From it this keeps, per
// block, the peak and the mean square of each channel and whether any sample
// sat at full scale (a clipped sample: the chain's output stage clamps), and a
// mono copy of the samples for the spectrum.
//
// A block is written into the output's buffer, which on the Blaze holds about a
// third of a second, so it is heard that much later. Each block is stamped with
// when it will be heard, and the interface reads what is being heard now rather
// than what was just decoded: a meter a third of a second ahead of the music
// looks broken.
//
// Nothing is done unless the interface says a meter is on screen
// (levelmeter_set_wanted): the feed then returns at its first line.
//
// Not fed by DSD sent as DoP or native, which has no PCM samples to measure,
// nor by the radio, whose chain is its own.

typedef struct {
	float peak[2];	 // 0..1 of full scale, the highest in the window
	float rms[2];	 // 0..1, root mean square over the window
	bool clipped;	 // a sample at full scale in the window
	int channels;	 // 1 or 2
	int sample_rate; // of the blocks read
} levelmeter_reading_t;

// The interface, while a meter is shown. Off by default.
void levelmeter_set_wanted(bool wanted);
bool levelmeter_wanted(void);

// The playback thread, from playback_follow(): how many frames the output
// buffer holds, so a block can be stamped with when it will be heard.
void levelmeter_set_buffer_frames(unsigned frames);

// The playback thread, on each finished block.
void levelmeter_feed_s16(const int16_t *frames, int frame_count, int channels, int sample_rate);
void levelmeter_feed_s32(const int32_t *frames, int frame_count, int channels, int sample_rate);

// The interface: everything heard since the previous call. False when nothing
// was (paused, stopped, between tracks, or a source that does not feed this).
bool levelmeter_read(levelmeter_reading_t *out);

// The interface: the `count` mono samples being heard now, -1..1, oldest first,
// and the rate they were played at. False when there is not that much yet.
#define LEVELMETER_SPECTRUM_SAMPLES 1024
bool levelmeter_read_samples(float *out, int count, int *sample_rate);

#endif /* LEVELMETER_H */
