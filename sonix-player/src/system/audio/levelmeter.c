#include "levelmeter.h"

#include <math.h>
#include <string.h>
#include <time.h>

// One entry per block fed, a ring the interface reads behind. Blocks are a
// period long (about 40 ms on the Blaze), so 128 of them are five seconds: far
// more than the output buffer holds, which is how far ahead of the reader the
// writer ever gets.
#define BLOCKS 128

typedef struct {
	uint32_t heard_at_ms;
	float peak[2];
	float sumsq[2];
	int frames;
	int channels;
	int rate;
	bool clipped;
} block_t;

static block_t blocks[BLOCKS];
static volatile uint32_t blocks_written; // entries, ever; the newest is (written - 1) % BLOCKS
static uint32_t blocks_read;			 // the interface's own

// The mono copy for the spectrum: enough for the output buffer at 192 kHz plus
// the window, so what is being heard is still in it.
#define SAMPLES 65536
static int16_t samples[SAMPLES];
static volatile uint32_t samples_written; // frames, ever
static volatile uint32_t samples_written_at_ms;
static volatile int samples_rate;

static volatile int wanted;
// When the interface last read. Its timer stops with the panel dark and cannot
// say so, so a reader gone for a second is taken as no reader at all.
static volatile uint32_t last_read_ms;
#define READER_GONE_MS 1000
static volatile unsigned buffer_frames;

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000);
}

void levelmeter_set_wanted(bool on) {
	if (on && !wanted) {
		blocks_read = blocks_written; // nothing from before it was shown
		last_read_ms = now_ms();
	}
	wanted = on ? 1 : 0;
}

bool levelmeter_wanted(void) { return wanted != 0; }

void levelmeter_set_buffer_frames(unsigned frames) { buffer_frames = frames; }

// When a block written now will be heard: once everything already queued ahead
// of it has played. The buffer is full while music plays, which is the only
// time this is called.
static bool reader_here(void) { return wanted && now_ms() - last_read_ms < READER_GONE_MS; }

static uint32_t heard_at(int rate) {
	unsigned ahead = buffer_frames;
	return now_ms() + (rate > 0 ? (uint32_t)((uint64_t)ahead * 1000u / (unsigned)rate) : 0u);
}

static void publish(block_t *b) {
	blocks[blocks_written % BLOCKS] = *b;
	__sync_synchronize(); // the entry before the count that makes it readable
	blocks_written++;
}

static void samples_done(int frame_count, int rate) {
	__sync_synchronize();
	samples_written += (uint32_t)frame_count;
	samples_written_at_ms = now_ms();
	samples_rate = rate;
}

void levelmeter_feed_s16(const int16_t *frames, int frame_count, int channels, int rate) {
	if (!frames || frame_count <= 0 || channels < 1 || channels > 2 || !reader_here()) {
		return;
	}
	block_t b = {.heard_at_ms = heard_at(rate), .frames = frame_count, .channels = channels, .rate = rate};
	int peak[2] = {0, 0};
	double sq[2] = {0, 0};
	uint32_t at = samples_written;
	for (int i = 0; i < frame_count; i++) {
		int mono = 0;
		for (int c = 0; c < channels; c++) {
			int v = frames[i * channels + c];
			int a = v < 0 ? -v : v;
			if (a > peak[c]) {
				peak[c] = a;
			}
			sq[c] += (double)v * v;
			mono += v;
		}
		samples[(at + (uint32_t)i) % SAMPLES] = (int16_t)(mono / channels);
	}
	for (int c = 0; c < channels; c++) {
		b.peak[c] = (float)peak[c] / 32768.0f;
		b.sumsq[c] = (float)(sq[c] / (32768.0 * 32768.0));
		b.clipped |= peak[c] >= 32767;
	}
	publish(&b);
	samples_done(frame_count, rate);
}

void levelmeter_feed_s32(const int32_t *frames, int frame_count, int channels, int rate) {
	if (!frames || frame_count <= 0 || channels < 1 || channels > 2 || !reader_here()) {
		return;
	}
	block_t b = {.heard_at_ms = heard_at(rate), .frames = frame_count, .channels = channels, .rate = rate};
	int64_t peak[2] = {0, 0};
	double sq[2] = {0, 0};
	uint32_t at = samples_written;
	for (int i = 0; i < frame_count; i++) {
		int64_t mono = 0;
		for (int c = 0; c < channels; c++) {
			int64_t v = frames[i * channels + c];
			int64_t a = v < 0 ? -v : v;
			if (a > peak[c]) {
				peak[c] = a;
			}
			double d = (double)v;
			sq[c] += d * d;
			mono += v;
		}
		samples[(at + (uint32_t)i) % SAMPLES] = (int16_t)((mono / channels) >> 16);
	}
	for (int c = 0; c < channels; c++) {
		b.peak[c] = (float)((double)peak[c] / 2147483648.0);
		b.sumsq[c] = (float)(sq[c] / (2147483648.0 * 2147483648.0));
		// The output stage clamps to the top of the range; a 24-bit DAC is
		// fed the top 24 bits, so anything within the last 8 is that clamp.
		b.clipped |= peak[c] >= (int64_t)INT32_MAX - 255;
	}
	publish(&b);
	samples_done(frame_count, rate);
}

bool levelmeter_read(levelmeter_reading_t *out) {
	memset(out, 0, sizeof(*out));
	uint32_t written = blocks_written;
	__sync_synchronize();
	if (written - blocks_read > BLOCKS) {
		blocks_read = written - BLOCKS; // fell behind: the oldest are gone
	}
	uint32_t now = now_ms();
	last_read_ms = now;
	double sq[2] = {0, 0};
	long frames = 0;
	bool any = false;
	while (blocks_read != written) {
		const block_t *b = &blocks[blocks_read % BLOCKS];
		if ((int32_t)(now - b->heard_at_ms) < 0) {
			break; // still in the output buffer
		}
		any = true;
		out->channels = b->channels;
		out->sample_rate = b->rate;
		out->clipped |= b->clipped;
		for (int c = 0; c < b->channels; c++) {
			if (b->peak[c] > out->peak[c]) {
				out->peak[c] = b->peak[c];
			}
			sq[c] += b->sumsq[c];
		}
		frames += b->frames;
		blocks_read++;
	}
	if (!any) {
		return false;
	}
	for (int c = 0; c < out->channels; c++) {
		out->rms[c] = frames > 0 ? (float)sqrt(sq[c] / (double)frames) : 0.0f;
	}
	if (out->channels == 1) {
		out->peak[1] = out->peak[0];
		out->rms[1] = out->rms[0];
	}
	return true;
}

bool levelmeter_read_samples(float *out, int count, int *sample_rate) {
	uint32_t written = samples_written;
	uint32_t written_at = samples_written_at_ms;
	int rate = samples_rate;
	__sync_synchronize();
	if (rate <= 0 || count <= 0 || count > SAMPLES / 4) {
		return false;
	}
	// The frame being heard now: as far back as the buffer, moved on by the
	// time since that write -- never past what has been written.
	uint32_t since = now_ms() - written_at;
	uint64_t moved = (uint64_t)since * (unsigned)rate / 1000u;
	uint64_t back = buffer_frames > moved ? buffer_frames - moved : 0;
	if ((uint64_t)written < back + (unsigned)count) {
		return false;
	}
	if (since > 1000) {
		return false; // nothing written for a second: not playing
	}
	uint32_t end = written - (uint32_t)back;
	for (int i = 0; i < count; i++) {
		out[i] = samples[(end - (uint32_t)count + (uint32_t)i) % SAMPLES] / 32768.0f;
	}
	if (sample_rate) {
		*sample_rate = rate;
	}
	return true;
}
