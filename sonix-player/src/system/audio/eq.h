#ifndef EQ_H
#define EQ_H

#include <stdbool.h>
#include <stdint.h>

// A 10-band graphic equalizer applied in software to the decoded PCM, the
// same shape as the stock player's "geq_band_gain" (10 bands): peaking
// biquads at the standard octave centres, +/-12 dB per band.
//
// The UI thread sets gains; the audio thread calls eq_process() on every
// buffer and quietly rebuilds its filter state when something changed.

#define EQ_BANDS 10
#define EQ_GAIN_MIN_DB (-12)
#define EQ_GAIN_MAX_DB 12

// Loads enabled + band gains from the config. Call once at boot, before
// playback can start.
void eq_init(void);

void eq_set_enabled(bool enabled); // saved to config
bool eq_get_enabled(void);

void eq_set_band(int band, int gain_db); // clamped, saved to config
int eq_get_band(int band);

// Every band back to 0 dB.
void eq_reset(void);

// The ready-made curves. Gains are whole dB, one per band, in the order of
// eq_band_freq.
typedef struct {
	const char *name;
	int gain_db[EQ_BANDS];
} eq_preset_t;

extern const eq_preset_t EQ_DEFAULT_PRESETS[];
extern const int EQ_DEFAULT_PRESET_COUNT;
void eq_apply_preset(const eq_preset_t *preset);

// The user's own, one file each in <card>/.local/eq.
void eq_presets_set_dir(const char *sd_root);
bool eq_preset_save(const char *name);
bool eq_preset_load(const char *name);
bool eq_preset_delete(const char *name);
typedef bool (*eq_preset_name_cb_t)(const char *name, void *user);
int eq_preset_for_each(eq_preset_name_cb_t cb, void *user);

// The band centre frequencies, for the UI's labels.
extern const int eq_band_freq[EQ_BANDS];

// ---------------------------------------------------------------------------
// PEQ -- the parametric equalizer
//
// The graphic equalizer above has ten bands with one control each: how much to
// boost. Frequency, width and shape are fixed by the program. The parametric
// one hands all three to the listener: every band has its own frequency
// (20 Hz to 20 kHz, continuous), gain, Q and shape -- peak, low or high shelf,
// low-pass or high-pass.
//
// That is what headphone correction needs: published compensation curves are
// written in exactly this form -- "Filter 3: ON PK Fc 105 Hz Gain -4.2 dB
// Q 0.70" -- and cannot be expressed on ten fixed-frequency sliders at all.
//
// It runs on the same biquads as the graphic EQ and MSEB, in the same chain and
// with the same automatic anti-clipping pre-gain: they are filters, not three
// separate engines.
// ---------------------------------------------------------------------------

// Ten is what a serious compensation curve needs (published AutoEq curves are
// almost all 10-band) without the page turning into a list half a minute long
// to scroll.
#define PEQ_BANDS 10

#define PEQ_FREQ_MIN 20
#define PEQ_FREQ_MAX 20000

// Gains are in tenths of a dB: compensation curves go down to half a decibel,
// and rounding them to whole decibels is audible.
#define PEQ_GAIN_MIN_TENTHS (-150)
#define PEQ_GAIN_MAX_TENTHS 150

// Q in hundredths, from 0.10 to 10.00. Below 0.1 the peak is so wide it is a
// tone control; above 10 it is so narrow it barely touches any note.
#define PEQ_Q_MIN 10
#define PEQ_Q_MAX 1000
#define PEQ_Q_DEFAULT 100 // 1.00

// Pre-gain, also in tenths of a dB. Mostly needed downwards: a curve that
// boosts anything needs matching attenuation.
#define PEQ_PREAMP_MIN_TENTHS (-150)
#define PEQ_PREAMP_MAX_TENTHS 60

typedef enum {
	PEQ_TYPE_PEAK = 0,	// peaking
	PEQ_TYPE_LOWSHELF,	// low shelf
	PEQ_TYPE_HIGHSHELF, // high shelf
	PEQ_TYPE_LOWPASS,	// low-pass
	PEQ_TYPE_HIGHPASS,	// high-pass
	PEQ_TYPE_COUNT
} peq_type_t;

typedef struct {
	bool on;
	int type;		 // peq_type_t
	int freq;		 // Hz
	int gain_tenths; // tenths of a dB (ignored by low-pass and high-pass)
	int q_cent;		 // hundredths
} peq_band_t;

void peq_set_enabled(bool enabled); // saved to config
bool peq_get_enabled(void);

void peq_get_band(int index, peq_band_t *out);
void peq_set_band(int index, const peq_band_t *band); // clamped, saved to config

int peq_get_preamp(void); // tenths of a dB
void peq_set_preamp(int tenths);

// Every band off and back to its starting values, pre-gain included.
void peq_reset(void);

// One band: off, peaking, its default frequency, zero gain and Q 1.00. The
// pre-gain is left alone -- it belongs to the chain, not to the band.
void peq_reset_band(int index);

// Parametric presets: Equalizer APO text files, the ParametricEQ.txt AutoEq
// publishes for every headphone, one per name in <card>/PEQ. A preset saved
// here is written in the same form, so it opens in APO too. Setting the
// directory also moves the presets of the older format out of
// <card>/.local/peq.
//
// Loading replaces the whole chain: the file's filters in order from the first
// band, the rest off, and its preamp. `ignored` (may be NULL) gets how many
// filters of the file could not come along -- past the tenth, or of a kind the
// chain does not have. False, changing nothing, when the file cannot be read
// or holds no usable filter at all.
void peq_presets_set_dir(const char *sd_root);
bool peq_preset_save(const char *name);
bool peq_preset_load(const char *name, int *ignored);
bool peq_preset_delete(const char *name);
typedef bool (*peq_preset_name_cb_t)(const char *name, void *user);
int peq_preset_for_each(peq_preset_name_cb_t cb, void *user);

// Magnitude response of the whole parametric chain at one frequency, in tenths
// of a dB, pre-gain included. For the page's graph: the same coefficient
// arithmetic that later runs on the audio, not a separately drawn curve, so
// what is shown is what is heard.
int peq_response_tenths(int freq, int sample_rate);

// What the engine does to the level on its own, in tenths of a dB. Two stages
// added together, because that is what the ear gets:
//
//   - the headroom taken off the graphic equaliser and the parametric, an
//     attenuation equal to their most boosted band, less whatever a negative
//     parametric preamp already takes off, so never above zero;
//   - MSEB's own normalisation, which is not a clip guard and can go either
//     way (see the note over mseb_auto_preamp_db in eq.c).
//
// So the total may be positive, and a positive number is not a bug: a mostly
// cutting MSEB setting hands some level back, which is what the stock player
// does too.
//
// Worth showing because it is otherwise invisible: boosting a band by 8 dB
// works, but the overall level drops by the same amount, and without a number
// saying so the equalizer looks like it is doing nothing.
//
// The figure holds for a normal sample rate (44.1 kHz and above), which is the
// rate the MSEB half is asked at. On a 32 kHz or slower file the engine drops
// the highest bands and measures a shorter band, so the real figure differs a
// little from the one reported here.
int eq_auto_headroom_tenths(void);

// ---------------------------------------------------------------------------
// MSEB: the stock player's MageSound tuning, rebuilt on the same biquad
// engine (the original drives a bank of fixed filters from named sliders --
// master_temp, bass0... -- via mseb.ini; see decompile FUN_00484b00). Ten
// characteristics, each running symmetrically around flat over the configured
// range -- the parameter set and per-step dB scales read out of the stock
// binary's data section.
//
// Ten sliders, thirteen filters: master_temp is a tilt made of four
// overlapping high shelves. And the module carries a level of its own, which
// the stock computes from the measured response of its bank rather than from
// the boosts its sliders declare; eq.c has the whole of it.
// ---------------------------------------------------------------------------

#define MSEB_BANDS 10

// How far the sliders travel: 20 (what the stock player offers), 40 or 100,
// with 100 the default. The per-step dB scales are the stock player's, so a
// setting of 20 here does exactly what 20 does on the original; 40 and 100
// push a characteristic further than HiBy allows.
#define MSEB_RANGE_DEFAULT 100
#define MSEB_VALUE_MIN (-100)
#define MSEB_VALUE_MAX 100

extern const char *const mseb_band_name[MSEB_BANDS];

void mseb_set_enabled(bool enabled); // saved to config
bool mseb_get_enabled(void);
void mseb_set_value(int band, int value); // clamped; NOT saved (see mseb_save)
int mseb_get_value(int band);
void mseb_save(void); // persists every value; call when a finger lifts

// The slider travel: 20 (the stock player's own), 40 or 100. Saved, and it
// clamps whatever no longer fits when it narrows.
int mseb_get_range(void);
void mseb_set_range(int range);

// Every characteristic back to flat.
void mseb_reset(void);

// Named presets, one file each in <card>/.local/MSEB. The directory is told
// to this module once the card's root is known.
void mseb_presets_set_dir(const char *sd_root);
bool mseb_preset_save(const char *name);
bool mseb_preset_load(const char *name);
bool mseb_preset_delete(const char *name);

// Lists them; return false from the callback to stop early.
typedef bool (*mseb_preset_cb_t)(const char *name, void *user);
int mseb_preset_for_each(mseb_preset_cb_t cb, void *user);

// ---------------------------------------------------------------------------
// Soundfield -- the stock player's "Sound Field" module (Campo sonoro in its
// Italian strings), rebuilt from the original's own DSP loop (decompile
// FUN_00631a4c). It is a mid/side width control:
//
//     mid  = (L + R) / 2          side = (R - L) / 2
//     L'   = mid - width * side   R'   = mid + width * side
//
// so width 1.00 is the signal untouched, 0.00 collapses to mono and 2.00
// doubles the stereo spread. The plugin declares its own parameter as
// min 0, max 2, step 0.05 ("param_list" in FUN_00631e8c), which is exactly
// the range offered here, and it runs on stereo PCM only -- the original
// bypasses itself for DSD/DoP too.
// ---------------------------------------------------------------------------

// The width is carried in hundredths, so it stays integer arithmetic all the
// way to the samples.
#define SOUNDFIELD_WIDTH_MIN 0
#define SOUNDFIELD_WIDTH_MAX 200
#define SOUNDFIELD_WIDTH_STEP 5
#define SOUNDFIELD_WIDTH_DEFAULT 100

void soundfield_set_enabled(bool enabled); // saved to config
bool soundfield_get_enabled(void);
void soundfield_set_width(int hundredths); // clamped and snapped to the step
int soundfield_get_width(void);

// In place on interleaved PCM, audio thread only. A no-op when the module is
// off, when the width is 1.00, or when the stream is not stereo.
void soundfield_process(short *frames, int frame_count, int channels);
void soundfield_process_s32(int32_t *frames, int frame_count, int channels);

// ---------------------------------------------------------------------------
// Channel balance
// ---------------------------------------------------------------------------
//
// The stock player's own module, and its own numbers: its parameter panel is
// declared in the binary as
//
//     "name": "balance", "type": "HScroll", "min": "-20", "max": "20",
//     "step": "0.5", "data_type": "float", "link": "_dB"
//
// -- a single slider from -20 to +20 dB in half-decibel steps, with an Enable
// checkbox beside it. So that is what this is: one number, in TENTHS of a dB
// to stay in integers, negative towards the left channel and positive towards
// the right.
//
// It only ever attenuates. Leaning right by 6 dB turns the LEFT channel down
// by 6 dB rather than turning the right one up, because the right one may
// already be at full scale and the correction would be a clipped one.
#define BALANCE_MIN (-200) // -20.0 dB
#define BALANCE_MAX 200	   // +20.0 dB
#define BALANCE_STEP 5	   // 0.5 dB
#define BALANCE_CENTRE 0

void balance_set_enabled(bool enabled); // saved to config
bool balance_get_enabled(void);
void balance_set_value(int tenths_db); // clamped and snapped to the step
int balance_get_value(void);

// In place on interleaved PCM, audio thread only. A no-op when off, when
// centred, or on anything that is not stereo.
void balance_process(short *frames, int frame_count, int channels);
void balance_process_s32(int32_t *frames, int frame_count, int channels);

// ---------------------------------------------------------------------------
// Mono
// ---------------------------------------------------------------------------
//
// Both channels replaced by their mean, for a listener with one ear or one
// earbud. Runs after crossfeed and before balance, so the balance still sets
// what reaches each side. Saved as [audio] mono.

void mono_set_enabled(bool enabled);
bool mono_get_enabled(void);

// In place on interleaved PCM, audio thread only. A no-op when off or on
// anything that is not stereo.
void mono_process(short *frames, int frame_count, int channels);
void mono_process_s32(int32_t *frames, int frame_count, int channels);

// ---------------------------------------------------------------------------
// Crossfeed
//
// On headphones the left channel reaches only the left ear. With speakers, or
// live, some of the left sound travels around the head to the right ear as
// well, slightly later and darker, because the head shadows the highs. The
// brain locates sound from that delay and that shadowing.
//
// Without it, a recording with instruments panned hard sounds "inside the head"
// and becomes fatiguing. Crossfeed puts back by hand what headphones remove: a
// delayed, low-passed copy of the opposite channel is added to each ear.
//
//     delayed  = the opposite channel, shifted back a few tens of
//                microseconds (the path around the head)
//     darkened = one-pole low-pass (the head absorbing highs)
//     output   = direct + level * darkened, normalised
//
// The normalisation matters: without it a track already at full scale distorts
// the moment crossfeed is switched on. The division is by (1 + level), exactly
// the gain a mono signal would pick up.
//
// Three integer parameters:
//   * level, in hundredths: how much of the other channel arrives;
//   * cutoff, in hertz: where the darkening starts;
//   * delay, in microseconds: the path around the head.
//
// The defaults (60%, 700 Hz, 300 us) are where the literature converges and
// match the classic tuning of the most widely used crossfeed.
// ---------------------------------------------------------------------------

#define CROSSFEED_LEVEL_MIN 10 // 10%: barely perceptible
#define CROSSFEED_LEVEL_MAX 100
#define CROSSFEED_LEVEL_STEP 5
#define CROSSFEED_LEVEL_DEFAULT 60

#define CROSSFEED_CUTOFF_MIN 300
#define CROSSFEED_CUTOFF_MAX 2000
#define CROSSFEED_CUTOFF_STEP 50
#define CROSSFEED_CUTOFF_DEFAULT 700

#define CROSSFEED_DELAY_MIN 0 // microseconds
#define CROSSFEED_DELAY_MAX 600
#define CROSSFEED_DELAY_STEP 20
#define CROSSFEED_DELAY_DEFAULT 300

void crossfeed_set_enabled(bool enabled); // saved to config
bool crossfeed_get_enabled(void);
void crossfeed_set_level(int percent); // clamped and snapped to the step
int crossfeed_get_level(void);
void crossfeed_set_cutoff(int hz);
int crossfeed_get_cutoff(void);
void crossfeed_set_delay(int microseconds);
int crossfeed_get_delay(void);

// Clears the delay line and the filter. Call on every new track and every seek:
// what is in there belongs to the previous position, and without this the tail
// of the previous track is audible through the opposite channel.
void crossfeed_reset(void);

// In place on interleaved PCM, audio thread only. A no-op when off or on
// anything that is not stereo. The sample rate is genuinely needed: both the
// cutoff and the delay are in physical units, and at 192 kHz they span four
// times as many samples as at 48.
void crossfeed_process(short *frames, int frame_count, int channels, int sample_rate);
void crossfeed_process_s32(int32_t *frames, int frame_count, int channels, int sample_rate);

// ---------------------------------------------------------------------------
// Direct: the decoded samples go to the output as they are.
//
// One switch over every other one in this file and ReplayGain's: with it on,
// the graphic equaliser, the parametric, MSEB, soundfield, crossfeed, mono
// and balance all pass the audio through untouched and no ReplayGain
// correction is applied. Nothing else changes: each of them keeps its own
// switch and its values, reports them as they were left, and is back the
// moment Direct goes off. The DAC's own settings (filter, gain) are hardware
// and are not part of it. Saved to the config.
// ---------------------------------------------------------------------------
void direct_set_enabled(bool enabled);
bool direct_get_enabled(void);

// Applies MSEB + the graphic EQ in place to interleaved signed 16-bit PCM,
// with automatic pre-gain so boosted bands cannot clip. A no-op when both are
// disabled/flat. Audio thread only.
void eq_process(short *frames, int frame_count, int channels, int sample_rate);

// The same chain for the hi-res path: interleaved signed 32-bit samples
// (24-bit sources left-justified), sharing every filter's state with the
// 16-bit entry -- the two are never used at once.
void eq_process_s32(int32_t *frames, int frame_count, int channels, int sample_rate);

#endif // EQ_H
