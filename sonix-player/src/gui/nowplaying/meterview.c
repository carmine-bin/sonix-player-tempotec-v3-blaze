#include "meterview.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/uiscale.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/eq.h"
#include "src/system/audio/levelmeter.h"
#include "src/system/audio/replaygain.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define FRAME_MS 33 // 30 a second: the panel runs at 45, and the meter now has new data every 20 ms
#define IDLE_MS 500 // the check while the page is not on screen
#define CLIP_HOLD_MS 2000
#define PEAK_HOLD_MS 1500

#define PEAK_FLOOR_DB (-48.0f)
#define PEAK_FALL_DB_S (24.0f / 2.8f) // the IEC/BBC-style return

#define VU_REF_DBFS (-18.0f) // 0 VU, the usual digital alignment
#define VU_MIN (-20.0f)
#define VU_MAX (3.0f)
#define VU_TAU_S 0.065f // 99% of a step in 300 ms

#define BANDS 20
#define SPEC_FLOOR_DB (-66.0f)
#define SPEC_FALL_DB_S 20.0f
#define SPEC_LOW_HZ 60.0f
#define SPEC_HIGH_HZ 16000.0f
#define FFT_N LEVELMETER_SPECTRUM_SAMPLES

// The page's slide, as the lyrics': 0 the sleeve, FULL the meter.
#define FULL 1000
#define SLIDE_MS 220
#define DRAG_PX ui_px(14)
#define COMMIT_FRACTION 4 // a quarter of the sleeve's height commits the swipe

static lv_obj_t *page;
static lv_timer_t *timer;
static bool (*where_cb)(lv_area_t *rel);
static bool (*look_cb)(int32_t progress, int32_t height);
static bool clear_ground; // drawn over the blurred sleeve, not on a ground of its own

// Where the parts that move were drawn, for invalidating only those each frame:
// the names along the bottom and the scale's figures do not change, and over the
// blurred sleeve every pixel invalidated is a pixel of picture redrawn.
static lv_area_t dyn_area, clip_area, names_area;
static bool dyn_valid;
static uint32_t last_sig; // what the frame showed, to skip one that would look the same
static meter_mode_t mode;
static bool mode_loaded;

static int32_t progress; // where the page is
static bool up_wanted;	 // brought in, and not put away since
static bool swiped;		 // the release that ends a swipe is not a tap
static lv_area_t placed; // the sleeve, relative to the panel
static bool placed_valid;

// What is drawn, after the ballistics.
static float shown_db[2] = {PEAK_FLOOR_DB, PEAK_FLOOR_DB};
static float hold_db[2] = {PEAK_FLOOR_DB, PEAK_FLOOR_DB};
static uint32_t hold_at[2];
static float vu_level[2]; // linear rms, integrated
// What the VU integrates towards: the last window's rms, kept between reads.
// A block is a period of the output (90 ms at 44.1 kHz) and this runs every
// 40 ms, so most frames find no new block; integrating towards silence on
// those read a steady tone ten decibels low.
static float vu_target[2];
static uint32_t got_at;
#define SIGNAL_GONE_MS 250
static float spec_db[BANDS];
// The caps over the bands: the highest each has been lately, held a moment
// and then let down more slowly than the band itself.
static float cap_db[BANDS];
static uint32_t cap_at[BANDS];
#define CAP_HOLD_MS 400
#define CAP_FALL_DB_S 12.0f

// Two lines under the top row: what is playing, and what it goes through.
static char info_format[96];
static char info_chain[128];
static uint32_t info_at;
#define INFO_EVERY_MS 1000
static void info_refresh(uint32_t now, bool force);
static uint32_t clip_at;
static bool clip_seen;
static uint32_t last_tick;

meter_mode_t meterview_mode(void) {
	if (!mode_loaded) {
		// A key of its own: "meter_mode" counted peak, VU and spectrum.
		long v = config_get_int("screen", "meter_page", METER_SPECTRUM);
		mode = (v >= 0 && v < METER_MODES) ? (meter_mode_t)v : METER_SPECTRUM;
		mode_loaded = true;
	}
	return mode;
}

static void reset_ballistics(void) {
	for (int c = 0; c < 2; c++) {
		shown_db[c] = hold_db[c] = PEAK_FLOOR_DB;
		vu_level[c] = vu_target[c] = 0.0f;
	}
	for (int i = 0; i < BANDS; i++) {
		spec_db[i] = cap_db[i] = SPEC_FLOOR_DB;
	}
	clip_seen = false;
}

void meterview_set_mode(meter_mode_t m) {
	meterview_mode();
	last_sig = 0;
	mode = m;
	config_set_int("screen", "meter_page", (long)m);
	config_save();
	reset_ballistics();
	if (page) {
		lv_obj_invalidate(page);
	}
}

bool meterview_is_up(void) { return progress > 0; }

static float to_db(float lin) { return lin > 1e-6f ? 20.0f * log10f(lin) : -120.0f; }

// ---------------------------------------------------------------------------
// The spectrum
// ---------------------------------------------------------------------------

static float fft_re[FFT_N], fft_im[FFT_N], window[FFT_N];
static bool window_ready;

static void fft(float *re, float *im, int n) {
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			float t = re[i];
			re[i] = re[j];
			re[j] = t;
			t = im[i];
			im[i] = im[j];
			im[j] = t;
		}
	}
	for (int len = 2; len <= n; len <<= 1) {
		float ang = -2.0f * (float)M_PI / (float)len;
		float wr = cosf(ang), wi = sinf(ang);
		for (int i = 0; i < n; i += len) {
			float cr = 1.0f, ci = 0.0f;
			for (int k = 0; k < len / 2; k++) {
				int a = i + k, b = i + k + len / 2;
				float tr = re[b] * cr - im[b] * ci;
				float ti = re[b] * ci + im[b] * cr;
				re[b] = re[a] - tr;
				im[b] = im[a] - ti;
				re[a] += tr;
				im[a] += ti;
				float nr = cr * wr - ci * wi;
				ci = cr * wi + ci * wr;
				cr = nr;
			}
		}
	}
}

// Each band's level now, in dB of full scale; false when nothing is playing.
static bool spectrum_now(float *db) {
	int rate = 0;
	if (!levelmeter_read_samples(fft_re, FFT_N, &rate)) {
		return false;
	}
	if (!window_ready) {
		for (int i = 0; i < FFT_N; i++) {
			window[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / (float)(FFT_N - 1));
		}
		window_ready = true;
	}
	for (int i = 0; i < FFT_N; i++) {
		fft_re[i] *= window[i];
		fft_im[i] = 0.0f;
	}
	fft(fft_re, fft_im, FFT_N);
	float top = fminf(SPEC_HIGH_HZ, (float)rate * 0.45f);
	float bin_hz = (float)rate / (float)FFT_N;
	float ratio = powf(top / SPEC_LOW_HZ, 1.0f / BANDS);
	float lo = SPEC_LOW_HZ;
	for (int b = 0; b < BANDS; b++) {
		float hi = lo * ratio;
		int k0 = (int)(lo / bin_hz + 0.5f), k1 = (int)(hi / bin_hz + 0.5f);
		if (k0 < 1) {
			k0 = 1;
		}
		if (k1 < k0) {
			k1 = k0;
		}
		if (k1 >= FFT_N / 2) {
			k1 = FFT_N / 2 - 1;
		}
		float m = 0.0f;
		for (int k = k0; k <= k1; k++) {
			float v = fft_re[k] * fft_re[k] + fft_im[k] * fft_im[k];
			if (v > m) {
				m = v;
			}
		}
		// A full-scale sine through a Hann window peaks at N/4 in its bin.
		db[b] = to_db(sqrtf(m) * 4.0f / (float)FFT_N);
		lo = hi;
	}
	return true;
}

// ---------------------------------------------------------------------------
// The ballistics, once per frame
// ---------------------------------------------------------------------------

static void step(uint32_t now) {
	float dt = last_tick ? (float)(now - last_tick) / 1000.0f : FRAME_MS / 1000.0f;
	if (dt > 0.5f) {
		dt = 0.5f;
	}
	last_tick = now;

	levelmeter_reading_t r;
	bool got = levelmeter_read(&r);
	if (got && r.clipped) {
		clip_at = now;
		clip_seen = true;
	}
	if (got) {
		vu_target[0] = r.rms[0];
		vu_target[1] = r.rms[1];
		got_at = now;
	} else if (now - got_at > SIGNAL_GONE_MS) {
		vu_target[0] = vu_target[1] = 0.0f;
	}

	if (mode == METER_SPECTRUM) {
		float db[BANDS];
		bool live = spectrum_now(db);
		for (int b = 0; b < BANDS; b++) {
			float fallen = spec_db[b] - SPEC_FALL_DB_S * dt;
			float target = live ? db[b] : SPEC_FLOOR_DB;
			spec_db[b] = fmaxf(fmaxf(target, fallen), SPEC_FLOOR_DB);
			if (spec_db[b] >= cap_db[b]) {
				cap_db[b] = spec_db[b];
				cap_at[b] = now;
			} else if (now - cap_at[b] > CAP_HOLD_MS) {
				cap_db[b] = fmaxf(cap_db[b] - CAP_FALL_DB_S * dt, spec_db[b]);
			}
		}
	}

	for (int c = 0; c < 2; c++) {
		if (mode == METER_SPECTRUM) { // the strip over the spectrum
			float fallen = shown_db[c] - PEAK_FALL_DB_S * dt;
			float db = got ? to_db(r.peak[c]) : PEAK_FLOOR_DB;
			shown_db[c] = fmaxf(fmaxf(db, fallen), PEAK_FLOOR_DB);
			if (shown_db[c] >= hold_db[c] || now - hold_at[c] > PEAK_HOLD_MS) {
				hold_db[c] = shown_db[c];
				hold_at[c] = now;
			}
		} else {
			vu_level[c] += (vu_target[c] - vu_level[c]) * (1.0f - expf(-dt / VU_TAU_S));
			shown_db[c] = to_db(vu_level[c]) - VU_REF_DBFS; // in VU
		}
	}
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void rect(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t color, lv_opa_t opa,
				 int32_t radius) {
	if (x2 < x1 || y2 < y1) {
		return;
	}
	lv_draw_rect_dsc_t dsc;
	lv_draw_rect_dsc_init(&dsc);
	dsc.bg_color = color;
	dsc.bg_opa = opa;
	dsc.radius = radius;
	lv_area_t a = {x1, y1, x2, y2};
	lv_draw_rect(layer, &dsc, &a);
}

static void text(lv_layer_t *layer, const char *s, const lv_font_t *font, int32_t x1, int32_t y1, int32_t x2,
				 lv_text_align_t align, lv_color_t color, lv_opa_t opa) {
	lv_draw_label_dsc_t dsc;
	lv_draw_label_dsc_init(&dsc);
	dsc.text = s;
	dsc.text_local = 1; // some are built on the stack
	dsc.font = font;
	dsc.color = color;
	dsc.opa = opa;
	dsc.align = align;
	lv_area_t a = {x1, y1, x2, y1 + font->line_height};
	lv_draw_label(layer, &dsc, &a);
}

static const lv_color_t CLIP_RED = LV_COLOR_MAKE(224, 27, 36);
static const lv_color_t HOT_AMBER = LV_COLOR_MAKE(245, 180, 40);

// One horizontal bar from `lo` to `hi` (dB or VU), filled to `value`, with the
// part past `hot` in amber and a line at `hold`.
static void hbar(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, float lo, float hi, float hot,
				 float value, float hold) {
	int32_t w = x2 - x1 + 1;
	rect(layer, x1, y1, x2, y2, lv_color_white(), LV_OPA_20, ui_px(4));
	float f = fminf(fmaxf((value - lo) / (hi - lo), 0.0f), 1.0f);
	float fh = fminf(fmaxf((hot - lo) / (hi - lo), 0.0f), 1.0f);
	int32_t end = x1 + (int32_t)(f * (float)w) - 1;
	int32_t hot_x = x1 + (int32_t)(fh * (float)w);
	rect(layer, x1, y1, end < hot_x - 1 ? end : hot_x - 1, y2, theme()->accent, LV_OPA_COVER, ui_px(4));
	if (end >= hot_x) {
		rect(layer, hot_x, y1, end, y2, HOT_AMBER, LV_OPA_COVER, ui_px(4));
	}
	if (hold > lo) {
		int32_t hx = x1 + (int32_t)(fminf((hold - lo) / (hi - lo), 1.0f) * (float)w) - ui_px(3);
		rect(layer, hx, y1, hx + ui_px(3), y2, lv_color_white(), LV_OPA_90, 0);
	}
}

static void line(lv_layer_t *layer, float x1, float y1, float x2, float y2, int32_t width, lv_color_t color) {
	lv_draw_line_dsc_t dsc;
	lv_draw_line_dsc_init(&dsc);
	dsc.p1 = (lv_point_precise_t){(lv_value_precise_t)x1, (lv_value_precise_t)y1};
	dsc.p2 = (lv_point_precise_t){(lv_value_precise_t)x2, (lv_value_precise_t)y2};
	dsc.width = width;
	dsc.color = color;
	dsc.opa = LV_OPA_COVER;
	dsc.round_start = 1;
	dsc.round_end = 1;
	lv_draw_line(layer, &dsc);
}

// The VU as two arc gauges, in the look of the rest of the interface: a track
// of three quarters of a circle open at the bottom, filled in the accent up to
// 0 VU and in amber past it, a tick at 0, and the reading written large in the
// middle. Linear in dB, the way every other figure on this page is.
#define GAUGE_START_DEG 135.0f // LVGL's angles: 0 at three o'clock, clockwise
#define GAUGE_SWEEP_DEG 270.0f

static void arc(lv_layer_t *layer, int32_t cx, int32_t cy, int32_t r, float from_deg, float to_deg, int32_t width,
				lv_color_t color, lv_opa_t opa) {
	if (to_deg - from_deg < 0.5f) {
		return;
	}
	lv_draw_arc_dsc_t dsc;
	lv_draw_arc_dsc_init(&dsc);
	dsc.center.x = cx;
	dsc.center.y = cy;
	dsc.radius = (uint16_t)r;
	dsc.start_angle = (lv_value_precise_t)from_deg;
	dsc.end_angle = (lv_value_precise_t)to_deg;
	dsc.width = width;
	dsc.rounded = 1;
	dsc.color = color;
	dsc.opa = opa;
	lv_draw_arc(layer, &dsc);
}

static float gauge_frac(float vu) { return fminf(fmaxf((vu - VU_MIN) / (VU_MAX - VU_MIN), 0.0f), 1.0f); }

static void gauge(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, float vu, const char *name) {
	lv_color_t ink = lv_color_white();
	int32_t w = x2 - x1 + 1, h = y2 - y1 + 1;
	int32_t width = ui_px(10);
	// The open bottom of the circle is where the name goes, so the gauge may
	// be a little taller than the space would otherwise allow.
	int32_t r = LV_MIN(w, h * 6 / 5) / 2 - width / 2 - ui_px(2);
	int32_t cx = x1 + w / 2, cy = y1 + width / 2 + ui_px(2) + r;
	float zero = gauge_frac(0.0f), f = gauge_frac(vu);
	float a0 = GAUGE_START_DEG, a_zero = a0 + GAUGE_SWEEP_DEG * zero, a_val = a0 + GAUGE_SWEEP_DEG * f;

	arc(layer, cx, cy, r, a0, a0 + GAUGE_SWEEP_DEG, width, ink, LV_OPA_20);
	arc(layer, cx, cy, r, a0, fminf(a_val, a_zero), width, theme()->accent, LV_OPA_COVER);
	if (a_val > a_zero) {
		arc(layer, cx, cy, r, a_zero, a_val, width, HOT_AMBER, LV_OPA_COVER);
	}
	// The tick at 0 VU, just outside the track.
	float t = a_zero * (float)M_PI / 180.0f;
	float r1 = (float)r + (float)width / 2.0f + (float)ui_px(3), r2 = r1 + (float)ui_px(7);
	line(layer, (float)cx + cosf(t) * r1, (float)cy + sinf(t) * r1, (float)cx + cosf(t) * r2, (float)cy + sinf(t) * r2,
		 ui_px(2), ink);

	// The reading, and what it is.
	const lv_font_t *big = &font_ui_28;
	const lv_font_t *small = &font_ui_16;
	char buf[16];
	if (vu <= VU_MIN + 0.05f) {
		snprintf(buf, sizeof(buf), "--");
	} else {
		snprintf(buf, sizeof(buf), "%+.1f", (double)fminf(vu, VU_MAX + 0.9f));
	}
	int32_t ty = cy - big->line_height / 2 - ui_px(4);
	text(layer, buf, big, cx - r, ty, cx + r, LV_TEXT_ALIGN_CENTER, ink, LV_OPA_COVER);
	text(layer, "VU", small, cx - r, ty + big->line_height, cx + r, LV_TEXT_ALIGN_CENTER, ink, LV_OPA_60);
	// The channel, in the gap at the bottom.
	int32_t ny = cy + (int32_t)((float)r * 0.70f) - small->line_height / 2;
	text(layer, name, &font_ui_18, cx - ui_px(20), ny, cx + ui_px(20), LV_TEXT_ALIGN_CENTER, ink, LV_OPA_80);
}

static void draw_dials(lv_layer_t *layer, int32_t x1, int32_t top, int32_t x2, int32_t bottom) {
	int32_t gap = ui_px(16);
	int32_t w = (x2 - x1 + 1 - gap) / 2;
	int32_t h = bottom - top + 1;
	if (h > w) {
		h = w;
	}
	int32_t y1 = top + (bottom - top + 1 - h) / 2;
	gauge(layer, x1, y1, x1 + w - 1, y1 + h - 1, shown_db[0], "L");
	gauge(layer, x1 + w + gap, y1, x1 + 2 * w + gap - 1, y1 + h - 1, shown_db[1], "R");
}

static void draw_spectrum(lv_layer_t *layer, int32_t x1, int32_t top, int32_t x2, int32_t bottom) {
	const lv_font_t *small = &font_ui_16;
	// The peaks over it, as a strip: one thin bar a channel, the held peak
	// written beside it.
	lv_color_t ink = lv_color_white();
	int32_t row = small->line_height;
	int32_t strip = ui_px(8);
	int32_t lab = ui_px(16), val = ui_px(52);
	static const char *const CH[2] = {"L", "R"};
	for (int c = 0; c < 2; c++) {
		int32_t y = top + c * (row + ui_px(2));
		int32_t by = y + (row - strip) / 2;
		text(layer, CH[c], small, x1, y, x1 + lab, LV_TEXT_ALIGN_LEFT, ink, LV_OPA_70);
		hbar(layer, x1 + lab, by, x2 - val, by + strip - 1, PEAK_FLOOR_DB, 0.0f, -6.0f, shown_db[c], hold_db[c]);
		char buf[16];
		if (hold_db[c] <= PEAK_FLOOR_DB + 0.05f) {
			snprintf(buf, sizeof(buf), "--");
		} else {
			snprintf(buf, sizeof(buf), "%+.1f", (double)hold_db[c]);
		}
		text(layer, buf, small, x2 - val, y, x2, LV_TEXT_ALIGN_RIGHT, ink, LV_OPA_COVER);
	}
	top += 2 * row + ui_px(2) + ui_px(12);
	int32_t base = bottom - small->line_height - ui_px(6);
	int32_t span = base - top;
	int32_t gap = ui_px(4);
	int32_t bw = ((x2 - x1 + 1) - gap * (BANDS - 1)) / BANDS;
	int32_t used = bw * BANDS + gap * (BANDS - 1);
	x1 += ((x2 - x1 + 1) - used) / 2;
	for (int b = 0; b < BANDS; b++) {
		int32_t bx = x1 + b * (bw + gap);
		float f = (spec_db[b] - SPEC_FLOOR_DB) / -SPEC_FLOOR_DB;
		int32_t h = (int32_t)(fminf(fmaxf(f, 0.0f), 1.0f) * (float)span);
		rect(layer, bx, top, bx + bw - 1, base, lv_color_white(), LV_OPA_10, ui_px(2));
		rect(layer, bx, base - h, bx + bw - 1, base, theme()->accent, LV_OPA_COVER, ui_px(2));
		if (cap_db[b] > SPEC_FLOOR_DB + 1.0f) {
			float fc = (cap_db[b] - SPEC_FLOOR_DB) / -SPEC_FLOOR_DB;
			int32_t cy = base - (int32_t)(fminf(fc, 1.0f) * (float)span);
			int32_t gap_px = ui_px(3);
			if (cy > base - h - gap_px) {
				cy = base - h - gap_px; // never on the bar: a cap sits clear of it
			}
			if (cy - ui_px(3) >= top) {
				rect(layer, bx, cy - ui_px(3), bx + bw - 1, cy, lv_color_white(), LV_OPA_COVER, ui_px(1));
			}
		}
	}
	int32_t ly = bottom - small->line_height;
	text(layer, "60", small, x1, ly, x1 + ui_px(60), LV_TEXT_ALIGN_LEFT, ink, LV_OPA_70);
	text(layer, "1k", small, (x1 + x2) / 2 - ui_px(30), ly, (x1 + x2) / 2 + ui_px(30), LV_TEXT_ALIGN_CENTER, ink,
		 LV_OPA_70);
	text(layer, "16k", small, x2 - ui_px(60), ly, x2, LV_TEXT_ALIGN_RIGHT, ink, LV_OPA_70);
}

static void draw_cb(lv_event_t *e) {
	lv_layer_t *layer = lv_event_get_layer(e);
	if (!layer) {
		return;
	}
	lv_area_t a;
	lv_obj_get_coords(page, &a);
	int32_t pad = ui_px(18);
	int32_t x1 = a.x1 + pad, x2 = a.x2 - pad;
	lv_color_t ink = lv_color_white();

	// Top right, on the back chevron's line in the other corner -- which is
	// also below the status bar when that stays up over the player: CLIP, lit
	// for two seconds after the output clips.
	bool clip_lit = clip_seen && lv_tick_elaps(clip_at) < CLIP_HOLD_MS;
	const lv_font_t *tag = &font_ui_16;
	int32_t cw = ui_px(58), ch = tag->line_height + ui_px(8);
	int32_t cy = a.y1 + back_btn_centre_y() - ch / 2;
	rect(layer, x2 - cw + 1, cy, x2, cy + ch, clip_lit ? CLIP_RED : ink, clip_lit ? LV_OPA_COVER : LV_OPA_20,
		 ui_px(6));
	clip_area = (lv_area_t){x2 - cw + 1, cy, x2, cy + ch};
	text(layer, "CLIP", tag, x2 - cw + 1, cy + ui_px(4), x2, LV_TEXT_ALIGN_CENTER, ink,
		 clip_lit ? LV_OPA_COVER : LV_OPA_50);

	// Bottom: the three kinds, the one shown lit. A tap anywhere moves on.
	static const char *const NAMES[METER_MODES] = {"musicsettings_meter_spectrum", "musicsettings_meter_vu"};
	const lv_font_t *nf = &font_ui_18;
	int32_t ny = a.y2 - pad - nf->line_height;
	int32_t third = (x2 - x1 + 1) / METER_MODES;
	names_area = (lv_area_t){x1, ny - ui_px(14), x2, a.y2};
	for (int i = 0; i < METER_MODES; i++) {
		bool on = (int)mode == i;
		int32_t tx1 = x1 + i * third, tx2 = tx1 + third - 1;
		text(layer, tr(NAMES[i]), nf, tx1, ny, tx2, LV_TEXT_ALIGN_CENTER, on ? theme()->accent : ink,
			 on ? LV_OPA_COVER : LV_OPA_50);
		if (on) {
			rect(layer, (tx1 + tx2) / 2 - ui_px(14), ny + nf->line_height + ui_px(3), (tx1 + tx2) / 2 + ui_px(14),
				 ny + nf->line_height + ui_px(6), theme()->accent, LV_OPA_COVER, ui_px(2));
		}
	}

	// What is playing, and what it goes through, under the top row.
	const lv_font_t *inf = &font_ui_16;
	int32_t iy = cy + ch + ui_px(10);
	text(layer, info_format, inf, x1, iy, x2, LV_TEXT_ALIGN_CENTER, ink, LV_OPA_COVER);
	text(layer, info_chain, inf, x1, iy + inf->line_height + ui_px(2), x2, LV_TEXT_ALIGN_CENTER, ink, LV_OPA_70);

	int32_t top = iy + 2 * inf->line_height + ui_px(16);
	int32_t bottom = ny - ui_px(18);
	dyn_area = (lv_area_t){a.x1, top, a.x2, bottom};
	dyn_valid = true;
	if (mode == METER_SPECTRUM) {
		draw_spectrum(layer, x1, top, x2, bottom);
	} else {
		draw_dials(layer, x1, top, x2, bottom);
	}
}

// ---------------------------------------------------------------------------
// The slide
// ---------------------------------------------------------------------------

static void set_ground(bool clear) {
	if (clear == clear_ground) {
		return;
	}
	clear_ground = clear;
	lv_obj_set_style_bg_opa(page, clear ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
	lv_obj_invalidate(page);
}

static void progress_apply(int32_t p) {
	if (p < 0) {
		p = 0;
	}
	if (p > FULL) {
		p = FULL;
	}
	progress = p;
	if (!placed_valid) {
		return;
	}
	if (look_cb) {
		set_ground(look_cb(p, lv_area_get_height(&placed)));
	}
	if (p == 0) {
		lv_obj_set_hidden(page, true);
		return;
	}
	int32_t h = lv_area_get_height(&placed);
	lv_obj_set_pos(page, placed.x1, placed.y1 + h * (FULL - p) / FULL);
	if (lv_obj_is_hidden(page)) {
		lv_obj_set_hidden(page, false);
		lv_obj_move_foreground(page);
		reset_ballistics();
		info_refresh(lv_tick_get(), true);
		last_tick = 0;
		levelmeter_set_wanted(true);
		lv_timer_set_period(timer, FRAME_MS);
		lv_timer_ready(timer);
	}
}

static void progress_exec(void *var, int32_t v) {
	(void)var;
	progress_apply(v);
}

static void slide_to(int32_t p) {
	lv_anim_delete(page, progress_exec);
	if (progress == p) {
		progress_apply(p);
		return;
	}
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, page);
	lv_anim_set_exec_cb(&a, progress_exec);
	lv_anim_set_values(&a, progress, p);
	lv_anim_set_duration(&a, SLIDE_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

// Where the sleeve is now; false when the page may not be up.
static bool place(void) {
	lv_area_t rel;
	if (!where_cb || !where_cb(&rel)) {
		return false;
	}
	if (!placed_valid || memcmp(&rel, &placed, sizeof(rel)) != 0) {
		placed = rel;
		placed_valid = true;
		lv_obj_set_size(page, lv_area_get_width(&rel), lv_area_get_height(&rel));
		dyn_valid = false; // drawn again whole at the new size
		progress_apply(progress);
	}
	return true;
}

// ---------------------------------------------------------------------------
// The swipes: up on the sleeve brings the page, down on the page takes it away
// ---------------------------------------------------------------------------

static void swiped_clear_cb(void *unused) {
	(void)unused;
	swiped = false;
}

static void drag_cb(lv_event_t *e) {
	static lv_point_t start;
	static bool tracking, engaged;

	bool opening = (bool)(uintptr_t)lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}
	if (code == LV_EVENT_PRESSED) {
		tracking = opening ? progress == 0 && !player_sheet_drag_active() && place() : progress == FULL;
		engaged = false;
		lv_indev_get_point(indev, &start);
		return;
	}
	if (!tracking) {
		return;
	}
	lv_point_t p;
	lv_indev_get_point(indev, &p);
	int dx = p.x - start.x, dy = p.y - start.y;
	int32_t h = placed_valid ? lv_area_get_height(&placed) : 1;

	if (code == LV_EVENT_PRESSING) {
		if (!engaged) {
			if (player_sheet_drag_active() || switcher_back_drag_active()) {
				tracking = false;
				return;
			}
			if (LV_ABS(dx) >= DRAG_PX && LV_ABS(dx) >= LV_ABS(dy)) {
				tracking = false; // sideways: the lyrics, or the player going
				return;
			}
			if (LV_ABS(dy) < DRAG_PX || LV_ABS(dy) < 2 * LV_ABS(dx)) {
				return;
			}
			if ((opening && dy > 0) || (!opening && dy < 0)) {
				tracking = false; // the other way: not this gesture
				return;
			}
			engaged = true;
			swiped = true;
			lv_anim_delete(page, progress_exec);
		}
		int32_t moved = (int32_t)(-dy) * FULL / h;
		progress_apply(opening ? moved : FULL + moved);
		return;
	}
	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		tracking = false;
		if (!engaged) {
			return;
		}
		engaged = false;
		bool far = LV_ABS(dy) > h / COMMIT_FRACTION;
		up_wanted = opening ? far : !far;
		slide_to(up_wanted ? FULL : 0);
		lv_async_call(swiped_clear_cb, NULL);
	}
}

// On one of the names along the bottom, that one; anywhere else, the next.
static void tap_cb(lv_event_t *e) {
	(void)e;
	if (swiped || player_sheet_drag_active() || progress != FULL) {
		return;
	}
	lv_indev_t *indev = lv_indev_active();
	lv_point_t p = {0, 0};
	if (indev) {
		lv_indev_get_point(indev, &p);
	}
	meter_mode_t next = (meter_mode_t)((meterview_mode() + 1) % METER_MODES);
	if (indev && dyn_valid && p.y >= names_area.y1 && p.y <= names_area.y2) {
		int32_t third = (names_area.x2 - names_area.x1 + 1) / METER_MODES;
		int32_t i = (p.x - names_area.x1) / (third > 0 ? third : 1);
		next = (meter_mode_t)(i < 0 ? 0 : i >= METER_MODES ? METER_MODES - 1 : i);
		if (next == meterview_mode()) {
			return;
		}
	}
	meterview_set_mode(next);
}

// "FLAC · 44.1 kHz · 16 bit · 890 kbps": for a lossless file the average
// bitrate of the file itself, as Lotoo shows it; for a lossy one the figure
// the file declares. And under it, what the sound goes through.
static void info_build(char *fmt, size_t fmt_size, char *chain, size_t chain_size) {
	device_state_t st;
	device_state_get(&st);
	fmt[0] = '\0';
	int dsd = audio_get_dsd_multiple();
	if (dsd > 0) {
		snprintf(fmt, fmt_size, "DSD%d \xc2\xb7 %.1f MHz", dsd, 2.8224 * dsd / 64.0);
	} else if (st.stream_sample_rate > 0 && st.current_file[0]) {
		char codec[16] = "";
		audio_get_stream_codec(codec, sizeof(codec));
		if (!codec[0]) {
			const char *dot = strrchr(st.current_file, '.');
			size_t k = 0;
			for (const char *c = dot ? dot + 1 : ""; *c && k < sizeof(codec) - 1; c++) {
				codec[k++] = (char)((*c >= 'a' && *c <= 'z') ? *c - 32 : *c);
			}
			codec[k] = '\0';
		}
		double khz = st.stream_sample_rate / 1000.0;
		int kbps = 0;
		if (audio_stream_is_lossy()) {
			kbps = audio_get_stream_bitrate_kbps();
		}
		struct stat sb;
		if (kbps <= 0 && !st.live && st.progress_total_secs > 0.5 && stat(st.current_file, &sb) == 0 && sb.st_size > 0) {
			kbps = (int)((double)sb.st_size * 8.0 / st.progress_total_secs / 1000.0 + 0.5);
		}
		int bits = audio_get_stream_bits();
		char rate[24];
		snprintf(rate, sizeof(rate), khz == (int)khz ? "%.0f kHz" : "%.1f kHz", khz);
		if (audio_stream_is_lossy()) {
			snprintf(fmt, fmt_size, kbps > 0 ? "%s \xc2\xb7 %s \xc2\xb7 %d kbps" : "%s \xc2\xb7 %s", codec, rate, kbps);
		} else if (kbps > 0) {
			snprintf(fmt, fmt_size, "%s \xc2\xb7 %s \xc2\xb7 %d bit \xc2\xb7 %d kbps", codec, rate, bits > 0 ? bits : 16,
					 kbps);
		} else {
			snprintf(fmt, fmt_size, "%s \xc2\xb7 %s \xc2\xb7 %d bit", codec, rate, bits > 0 ? bits : 16);
		}
	}

	if (direct_get_enabled()) {
		snprintf(chain, chain_size, "%s", tr("musicsettings_direct"));
		return;
	}
	struct {
		bool on;
		const char *name;
	} fx[] = {
		{replaygain_active(), "ReplayGain"}, {mseb_get_enabled(), "MSEB"},		  {eq_get_enabled(), "EQ"},
		{peq_get_enabled(), "PEQ"},			 {soundfield_get_enabled(), "Soundfield"}, {crossfeed_get_enabled(), "Crossfeed"},
		{mono_get_enabled(), "Mono"},		 {balance_get_enabled(), "Balance"},
	};
	size_t used = 0;
	chain[0] = '\0';
	for (size_t i = 0; i < sizeof(fx) / sizeof(fx[0]); i++) {
		if (!fx[i].on) {
			continue;
		}
		int w = snprintf(chain + used, chain_size - used, "%s%s", used ? " \xc2\xb7 " : "", fx[i].name);
		if (w < 0 || (size_t)w >= chain_size - used) {
			break;
		}
		used += (size_t)w;
	}
	if (!used) {
		snprintf(chain, chain_size, "%s", tr("meter_no_effects"));
	}
}

// Rebuilt once a second, and the page drawn whole when it changes.
static void info_refresh(uint32_t now, bool force) {
	if (!force && now - info_at < INFO_EVERY_MS) {
		return;
	}
	info_at = now;
	char fmt[sizeof(info_format)], chain[sizeof(info_chain)];
	info_build(fmt, sizeof(fmt), chain, sizeof(chain));
	if (strcmp(fmt, info_format) != 0 || strcmp(chain, info_chain) != 0) {
		snprintf(info_format, sizeof(info_format), "%s", fmt);
		snprintf(info_chain, sizeof(info_chain), "%s", chain);
		dyn_valid = false; // the next frame redraws the page whole
		last_sig = 0;
	}
}

// A digest of what the next frame would show, so a frame identical to the
// last -- paused, silent and settled -- is not drawn at all.
static uint32_t frame_signature(void) {
	uint32_t h = 2166136261u;
	const float *vals[] = {shown_db, hold_db, spec_db, cap_db};
	const int counts[] = {2, 2, BANDS, BANDS};
	for (int v = 0; v < 4; v++) {
		for (int i = 0; i < counts[v]; i++) {
			int32_t q = (int32_t)(vals[v][i] * 10.0f); // a tenth of a dB is a pixel's worth at most
			h = (h ^ (uint32_t)q) * 16777619u;
		}
	}
	bool clip_lit = clip_seen && lv_tick_elaps(clip_at) < CLIP_HOLD_MS;
	return (h ^ (uint32_t)clip_lit ^ ((uint32_t)mode << 8)) * 16777619u;
}

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

static void tick_cb(lv_timer_t *t) {
	bool allowed = place();
	if (!allowed) {
		// Away with the player, the page goes too, and comes back with it.
		if (progress > 0) {
			lv_anim_delete(page, progress_exec);
			progress_apply(0);
		}
	} else if (up_wanted && progress == 0 && !lv_anim_get(page, progress_exec)) {
		progress_apply(FULL);
	}
	if (progress == 0 || !power_screen_is_on()) {
		levelmeter_set_wanted(false);
		lv_timer_set_period(t, IDLE_MS);
		last_tick = 0;
		return;
	}
	lv_timer_set_period(t, FRAME_MS);
	levelmeter_set_wanted(true);
	if (look_cb && progress == FULL) {
		set_ground(look_cb(FULL, lv_area_get_height(&placed))); // the blur may have arrived since
	}
	step(lv_tick_get());
	info_refresh(lv_tick_get(), false);
	uint32_t sig = frame_signature();
	if (sig == last_sig && dyn_valid) {
		return;
	}
	last_sig = sig;
	if (dyn_valid && progress == FULL) {
		lv_obj_invalidate_area(page, &dyn_area);
		lv_obj_invalidate_area(page, &clip_area);
	} else {
		lv_obj_invalidate(page);
	}
}

void meterview_create(lv_obj_t *panel, bool (*where)(lv_area_t *rel), bool (*look)(int32_t progress, int32_t height)) {
	where_cb = where;
	look_cb = look;
	meterview_mode();
	reset_ballistics();

	page = lv_obj_create(panel);
	lv_obj_remove_style_all(page);
	lv_obj_set_style_bg_color(page, lv_color_make(14, 14, 16), 0);
	lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(page, false);
	lv_obj_set_clickable(page, true);
	lv_obj_set_event_bubble(page, false);
	lv_obj_add_event_cb(page, draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
	lv_obj_add_event_cb(page, tap_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(page, drag_cb, LV_EVENT_PRESSED, (void *)(uintptr_t) false);
	lv_obj_add_event_cb(page, drag_cb, LV_EVENT_PRESSING, (void *)(uintptr_t) false);
	lv_obj_add_event_cb(page, drag_cb, LV_EVENT_RELEASED, (void *)(uintptr_t) false);
	lv_obj_add_event_cb(page, drag_cb, LV_EVENT_PRESS_LOST, (void *)(uintptr_t) false);
	// Sideways on the page is the player going, as on the sleeve.
	player_sheet_attach_drag(page, false);
	lv_obj_set_hidden(page, true);

	lv_obj_add_event_cb(panel, drag_cb, LV_EVENT_PRESSED, (void *)(uintptr_t) true);
	lv_obj_add_event_cb(panel, drag_cb, LV_EVENT_PRESSING, (void *)(uintptr_t) true);
	lv_obj_add_event_cb(panel, drag_cb, LV_EVENT_RELEASED, (void *)(uintptr_t) true);
	lv_obj_add_event_cb(panel, drag_cb, LV_EVENT_PRESS_LOST, (void *)(uintptr_t) true);

	timer = lv_timer_create(tick_cb, IDLE_MS, NULL);
	power_pause_in_standby(timer);
}
