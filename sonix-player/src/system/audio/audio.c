#include "audio.h"
#include "src/system/audio/levelmeter.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/decode/decode.h"
#include "src/system/decode/growfile.h"
#include "src/system/decode/sndfile.h"
#include "src/system/audio/replaygain.h"
#include "src/system/audio/speed.h"
#include "src/system/audio/swvolume.h"
#include "src/system/audio/usbaudio.h"
#include "src/system/audio/eq.h"
#include "src/system/audio/decimate.h"
#include "src/system/core/config.h"
#include "src/system/device/power.h"
#include "src/system/core/utils.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>

#include <sched.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <alloca.h>
#include <alsa/asoundlib.h>
#include <time.h>

// Millisecond timestamps for the play/start/end log lines, so the ordering and
// the gaps between them can be read back.
static long log_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}


// The host build has no sound card: ALSA falls back to a null device that
// swallows a whole track in a few milliseconds, so playback there is over
// before it starts and states like "paused" are unreachable. Pacing the writes
// to real time makes the simulator behave like a device for anything that
// depends on timing.
#ifdef HOST_BUILD
static void host_pace(snd_pcm_sframes_t frames, int rate) {
	if (frames > 0 && rate > 0) {
		usleep((useconds_t)((frames * 1000000.0) / rate));
	}
}
#else
#define host_pace(frames, rate) ((void)0)
#endif

// Stops a stream for a pause, and says how many bytes of already-decoded audio
// were thrown away with it.
//
// The stream is DROPPED, not paused. This driver does not survive
// snd_pcm_pause(): a track left paused and then skipped wedges the playback
// thread inside ALSA until the player is restarted. The stock player cannot
// pause the hardware either -- it runs on tinyalsa, which has no pause call at
// all, and simply stops feeding the device.
//
// The cost of dropping is that whatever the DAC had buffered (up to ~0.7 s with
// the deep buffer here) is discarded unheard, so the caller winds its source
// back by the returned amount.
// One per turn of a decode loop, for the watchdog.
//
// The playback thread runs SCHED_RR while its stream plays (see
// playback_follow), and this device has one core: a turn that
// never blocks starves everything else, the interface included, and the log
// stops because nothing else is scheduled to write to it. From outside that is
// a frozen player with nothing in the log -- and from the watchdog's side a
// thread in state R says only "running", not whether it is making progress.
// This number says which: racing away while the interface is stalled is a spin,
// standing still is a thread stuck in a call that never came back.
static volatile unsigned loop_turns;

unsigned audio_loop_turns(void) { return loop_turns; }

static void dac_ramp_before_cut(snd_pcm_t *pcm);

static int64_t pause_stop_stream(snd_pcm_t *pcm, int frame_bytes) {
	// No stream is a fair answer, not a fault: the device is handed back after
	// a few seconds of pause and this is called on the way into pause. Alsa
	// would answer a null handle with an assert, and an abort in here costs the
	// whole audio thread.
	if (!pcm) {
		return 0;
	}

	snd_pcm_sframes_t queued = 0;
	if (snd_pcm_delay(pcm, &queued) < 0 || queued < 0) {
		queued = 0; // buffered amount unknown: resume a touch late
	}

	// Counted before the fade, so the resume plays the faded part again.
	dac_ramp_before_cut(pcm);
	snd_pcm_drop(pcm);

	fprintf(stderr, "audio[%ld]: paused (stream dropped, %ld frames unplayed)\n", log_ms(), (long)queued);
	return (int64_t)queued * frame_bytes;
}

// Resuming from pause must cope with whatever happened to the PCM while it sat
// paused: a long screen-off can leave it SUSPENDED (or dropped back to SETUP),
// and a write to a device still stuck in PAUSED blocks forever with no error.
// Whatever the state, this leaves the stream RUNNING or freshly PREPARED so the
// next write starts it.
static void pcm_restart_after_pause(snd_pcm_t *pcm) {
	snd_pcm_state_t st = snd_pcm_state(pcm);

	// A system suspend can leave the device suspended underneath; that has to
	// be undone before anything else will work.
	if (st == SND_PCM_STATE_SUSPENDED) {
		int err = snd_pcm_resume(pcm);
		int tries = 0;
		while (err == -EAGAIN && tries++ < 10) {
			usleep(100 * 1000);
			err = snd_pcm_resume(pcm);
		}
	}

	// The stream was DROPPED when the pause began (see the pause branch), so
	// there is nothing to un-pause: it just needs preparing again before the
	// next write.
	int err = snd_pcm_prepare(pcm);
	fprintf(stderr, "audio[%ld]: restarting after pause (state %d, prepare %s)\n", log_ms(), (int)st,
			err < 0 ? snd_strerror(err) : "ok");
}


// The sample encodings the WAV path reads. The device is opened at S16_LE for
// the first two and at S32_LE, left-justified, for the rest -- the same format
// the decoded path uses for 24-bit sources.
typedef enum {
	WAV_PCM_U8,
	WAV_PCM_S16,
	WAV_PCM_S24, // packed, three bytes a sample
	WAV_PCM_S32,
	WAV_FLOAT32,
	WAV_FLOAT64,
} wav_sample_t;

typedef struct {
	int channels;
	int sample_rate;
	int bits_per_sample; // as stored in the file, for the format line
	wav_sample_t sample;
	int src_frame_bytes; // one frame as stored in the file
	int out_bits;		 // 16 or 32: what the device is opened with
	// 64-bit: long is 32 bits on the device, and a 24/96 stereo file passes
	// 2 GB after about an hour.
	int64_t data_offset;
	int64_t data_size;
} wav_info_t;

// parse_wav() results.
#define WAV_OK 0
#define WAV_BAD -1		   // not a WAV, or a broken one
#define WAV_UNSUPPORTED -2 // a sound WAV in an encoding this path does not read

// The ALSA device playback opens. "default" is the DAC behind the jacks;
// Bluetooth swaps in a bluealsa PCM name so the stream goes to the
// headphones instead. Read under the mutex, because it is set from the
// interface thread while the playback thread may be about to open a file.
// It gets a lock of its own rather than sharing audio_mutex: open_pcm_device()
// runs on the playback thread at points where that mutex is already held, and
// a second acquisition there would be a deadlock rather than a bug report.
#define AUDIO_DEFAULT_PCM "default"
static char output_pcm[160] = AUDIO_DEFAULT_PCM;

// ---------------------------------------------------------------------------
// 32-bit samples on the built-in card
//
// The card's DMA driver (soc_aic.ko) takes S16_LE and S24_LE and nothing
// else. Asked for S32_LE, the "default" plug converts to S24_LE itself, and
// a plug that converts reaches the card through mmap: it writes the converted
// samples straight into the DMA buffer, mapped into this process by the
// driver's own aic_dma_pcm_mmap() -- a remap_pfn_range() over the buffer's
// pages with the cache attribute bits cleared. The stock player never takes
// that path: it imports snd_pcm_writei and none of the mmap calls.
//
// Twice now the kernel has died on memory overwritten with values shaped
// exactly like that conversion's output -- 0x003693ef in an anon_vma pointer,
// then 0x007f20d0 over kernel code, 24-bit samples in 32-bit words -- both
// times on a player that had been playing 24-bit tracks through it. So the
// card is asked for S24_LE directly, the samples are shifted here, and the
// plug, with nothing to convert, passes plain writes through to the driver's
// copy path, the one the stock player has always used.
//
// The shift is the plug's own, checked against alsa-lib byte for byte: an
// arithmetic shift of the 32-bit word by eight, sign carried into the top
// byte, which leaves a DoP marker and its sixteen bits exactly where the DAC
// has always found them.
// ---------------------------------------------------------------------------

// Whether 32-bit frames for `pcm` go out as S24_LE. Decided from the handle's
// own name, so it is right for a handle that fell back to the card.
static bool pcm_is_card(snd_pcm_t *pcm) {
	const char *name = pcm ? snd_pcm_name(pcm) : NULL;
	return name && strcmp(name, AUDIO_DEFAULT_PCM) == 0;
}

static bool pcm_takes_s24(snd_pcm_t *pcm, int bits) { return bits == 32 && pcm_is_card(pcm); }

typedef struct {
	int32_t *data;
	size_t capacity; // in samples
} s24_scratch_t;

// The frames shifted into S24_LE, in `scratch`. The caller's buffer is left
// alone: a write that is retried after an underrun hands the same buffer in
// again. NULL when the scratch cannot be grown.
static const void *s24_from_s32(s24_scratch_t *scratch, const void *buf, size_t samples) {
	if (samples > scratch->capacity) {
		int32_t *grown = realloc(scratch->data, samples * sizeof(int32_t));
		if (!grown) {
			return NULL;
		}
		scratch->data = grown;
		scratch->capacity = samples;
	}
	const int32_t *in = buf;
	for (size_t i = 0; i < samples; i++) {
		scratch->data[i] = in[i] >> 8;
	}
	return scratch->data;
}

// One each for the decode loop and the external sources, which write from
// threads of their own.
static s24_scratch_t playback_s24;
static s24_scratch_t external_s24;

static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_t playback_thread;
static pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t audio_cond = PTHREAD_COND_INITIALIZER;

static audio_command_t audio_command = AUDIO_CMD_NONE;
static audio_status_t playback_status = AUDIO_STATUS_STOPPED;
static char current_filepath[512] = {0};
static bool play_request = false;
// When the pending play request was made. The playback thread clears it as it
// picks the request up, so a value that stops moving is a thread that has
// stopped coming back for work -- which is what a stall looks like from
// outside: the interface answers, the request is logged, and no track starts.
// Read by the watchdog in main.c.
static long play_request_ms;
static bool write_fail_retry_used = false; // one automatic track retry per play request

// What audio_quiet_ms() reads: whether a track is being started or a PCM is
// being opened right now, and when the last of those finished. Written by the
// playback thread, read without a lock from the waveform worker -- each is a
// word wide on this target, so the worst a racing read sees is the state from
// an instant ago.
static volatile bool track_starting;
static volatile bool pcm_opening;
static volatile long quiet_since_ms;

// The result of the last open_pcm_device(): 0, or the negative error that
// made it give up. play_file() reads it to tell a device that cannot do S32
// from a driver that has run out of memory.
static int last_open_err;

// Set by audio_notify_resume(): a system suspend leaves an open stream in
// SND_PCM_STATE_SUSPENDED, and a paused track keeps its stream open across
// the whole standby. The playback thread clears the flag by putting the
// device back on its feet, so nothing later meets a suspended handle.
static bool resume_recover_pending = false;

// Raised by the suspend path: as soon as the playback thread sees it, it
// closes the PCM the gapless path is holding instead of waiting for it to
// time out.
static volatile bool hold_flush_request = false;

// True while play_file() is running -- including while paused. This is the
// distinction "playback_status == STOPPED" cannot make: the status changes as
// soon as the command arrives, but the track context (open decoder, FILE* on
// the card, thread inside the track loop) only dies when play_file() returns.
// The suspend path waits on this, not on the status: the microSD loses power
// in mem, and a descriptor left open across it leads to the reboot on the
// first play afterwards.
static volatile bool playback_context_active = false;

// Fresh restart after mem: the first play is not an unpause. The stock binary
// has a dedicated flag (player+0xBB8, set_param(0x1e,1) just before mem) that
// the next play consumes to rebuild the track instead of resuming a
// pre-suspend state. Same idea here: path and position are saved before the
// freeze, and the next audio_resume() turns them into an audio_play_at() --
// fresh decoder, fresh output route, fresh PCM.
static bool restart_fresh_pending = false;
static char restart_fresh_path[512];
static double restart_fresh_pos;

// The analogue output brought down before the power goes for good (power-off,
// reboot). Called with the PCM closed and the DAC already muted.
//
// The Blaze's amplifier thumps when its supply goes, on the stock firmware as
// much as here, and nothing on the board stops it: the mute pin does not cover
// it. The least of it is heard when the machine driver takes the card down in
// its own order -- route 5: mute pin, HBC3000 off, then the amplifier's DC-DC --
// before the power does, rather than the power taking everything at once.
// Elsewhere this is the same park as before mem.
void audio_quiet_output_for_power_off(void) {
	if (!alsa_board_is_ak4493()) {
		audio_park_output_before_suspend();
		return;
	}
	alsa_suspend_mute();
	fprintf(stderr, "audio: card taken down on route 5 before the power goes\n");
#ifndef HOST_BUILD
	alsa_set_control("Output Port Switch", 5);
	usleep(120 * 1000);
	alsa_controls_note_output(5);
#endif
}

// ---------------------------------------------------------------------------
// Re-initialising the output route after waking from mem.
//
// mem powers the HBC3000 (the FPGA audio bridge) off, and the card's
// suspend_pre resets the driver's route to 0. The first route write after it
// is therefore a real change, which runs hbc3000_enable(): power, the FPGA
// configuration loaded over JTAG, the DAC powered up. A PCM opened before that
// write starts the HBC3000 dead, the AIC reset fails and the device reboots.
//
// The write takes a few hundred milliseconds in the driver, so on the R3 Pro II
// it runs on a low-priority thread while the screen comes back. The card is
// closed to everything else (alsa_wait_output_reinit()) from the parking of
// the route before mem until the write and the DAC level are done.
// ---------------------------------------------------------------------------

// On the thread that called alsa_output_reinit_enter_thread().
static void output_reinit_now(void) {
	long start = log_ms();
#ifndef HOST_BUILD
	// The Blaze's card module has no suspend_pre (stock x1600_tempotec_v3a
	// card: generic snd_soc_pm_ops only), so its driver still holds the route
	// from before mem and the same write would be its "no change" shortcut.
	// Through the other socket first, as before 1.3.0.
	if (alsa_board_is_ak4493()) {
		alsa_set_control("Output Port Switch", output_reinit_partner(detect_output()));
		usleep(120 * 1000); // settle: how long the driver takes over a route change
	}
#endif
	alsa_controls_rewrite_output();
	// The DAC back once the real route is live again.
	alsa_suspend_restore();
	fprintf(stderr, "audio: output re-init after resume done in %ld ms\n", log_ms() - start);
}

static void *output_reinit_thread(void *unused) {
	(void)unused;
	alsa_output_reinit_enter_thread();
	thread_be_low_priority("outreinit");
	output_reinit_now();
	alsa_output_reinit_leave_thread();
	alsa_output_reinit_end();
	return NULL;
}

void audio_start_output_reinit_after_resume(void) {
	// No HBC3000 on the CS43131 board: nothing went down, nothing to re-init.
	if (alsa_board_is_cs43131()) {
		alsa_suspend_restore();
		return;
	}
	// Parked on route 5 (the Blaze): the amplifier stays off until the next
	// play, whose auto_set_output() writes the real route from 5 -- a real
	// change that powers the HBC3000 up, the way the stock player resumes
	// after its own idle park. Nothing to switch, so nothing clicks at the wake.
	// The card is opened first: the park may have closed it (no analogue
	// socket), and alsa_suspend_restore() writes to it.
	if (alsa_board_is_ak4493() && alsa_controls_output() == 5) {
		alsa_output_reinit_end();
		alsa_suspend_restore();
		return;
	}
	fprintf(stderr, "audio: output re-init after resume, route %d, in the background\n", detect_output());

	// Closed already by the parking before mem; closed here otherwise.
	alsa_output_reinit_begin();
	pthread_attr_t attr;
	pthread_t thread;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	int err = pthread_create(&thread, &attr, output_reinit_thread, NULL);
	pthread_attr_destroy(&attr);
	if (err != 0) {
		fprintf(stderr, "audio: no thread for the output re-init (%s); done here\n", strerror(err));
		alsa_output_reinit_enter_thread();
		output_reinit_now();
		alsa_output_reinit_leave_thread();
		alsa_output_reinit_end();
	}
}

// The pop into the headphones as the R3 Pro II goes into mem.
//
// mem cuts the HBC3000's power with its output stage still switched on to the
// socket, and the amplifier losing its supply with the headphones connected is
// the thump. The mixer offers no mute for that stage -- the card's controls are
// the route, the balanced line-out flag and DOP_EN, the codec's are before the
// amplifier -- but a route change is the machine driver's own orderly sequence
// (mute the old port, reconfigure, unmute the new one), and it is silent: the
// route write at every wake runs the same sequence and nobody hears it.
//
// So before the suspend the route is moved to the other socket, the empty one.
// The port in use is muted by the driver, and what goes down in mem is an
// amplifier driving nothing. The re-init above puts the real route back.
//
// Called with the PCM closed: audio_suspend_freeze() has run. Closes the card
// to everything but the re-init that follows the suspend, or the attempt.
void audio_park_output_before_suspend(void) {
	if (alsa_board_is_cs43131()) {
		return; // no HBC3000, and no thump to take away
	}
	alsa_wait_output_reinit(); // the last wake's re-init, if still running
	int x = detect_output();
	if (x < 1 || x > 3) {
		alsa_output_reinit_begin();
		return; // not one of the analogue sockets
	}
	// The Blaze goes into mem on route 5, the driver's own take-down (mute pin,
	// HBC3000 off, amplifier DC-DC off). Usually the idle park below did it
	// long ago and there is nothing left to switch; otherwise its soft pop
	// comes here. Left on its route, the amplifier losing its supply in mem
	// clicks loudly, and the partner route clicked too (user, 2026-10-16).
	if (alsa_board_is_ak4493() && alsa_controls_output() == 5) {
		fprintf(stderr, "audio: amplifier already off (route 5) before mem\n");
		return;
	}
	int y = alsa_board_is_ak4493() ? 5 : output_reinit_partner(x);
	// The DAC first: the route change mutes the old port, but the DAC feeding
	// it would still be live when its supply goes. Silent until the wake has
	// put the real route back (audio_start_output_reinit_after_resume()).
	alsa_suspend_mute();
	// The balanced line-out flag with it: on the 4.4 mm socket headphone and
	// line out are the same route, and a report of a pop has to say which.
	fprintf(stderr, "audio: output parked on %d before mem (was %d, balanced line out %d)\n", y, x, alsa_output_key() & 1);
#ifndef HOST_BUILD
	bool parked = alsa_set_control("Output Port Switch", y) >= 0;
	usleep(120 * 1000); // the driver's mute and route change, before the power goes
	// Written by hand like the re-init: the cache follows, so a suspend that
	// fails after this puts the real route back at the next play. Only when
	// the write took: the wake trusts a cached 5 and skips its re-init.
	if (parked) {
		alsa_controls_note_output(y);
	}
#endif
	alsa_output_reinit_begin();
}

// True while the playback thread holds an open PCM handle. The suspend path
// waits on this: no ALSA object may be alive when the SoC stops.
static volatile bool pcm_device_open = false;

// How long a paused stream may keep the device -- and with it the CS43198 and
// the amplifier -- powered before it is handed back. The usual way to leave the
// player is a track paused, and holding the whole analogue chain up for hours
// is the bulk of the standby drain. The stock player closes its device whenever
// it is not feeding it, which is why its "pop" is a familiar sound on these
// players.
#define PAUSED_DEVICE_CLOSE_MS 5000

// How long the Blaze's amplifier stays on once the music has stopped (see
// idle_amp_off()).
//
// Going off pops: the driver's own off sequence (route 5), on the stock
// firmware too. Measured on the device (2026-11-19, IEMs, a microphone) it is
// the amplifier's mute pin switching, and no order of the pins was found that
// makes it quiet -- with the DAC playing, stopped, configured or not, the mute
// ramped or not. So the only choice left is when it comes, and it should come
// when nobody is listening.
//
// Headphones in and the music on the internal DAC: five minutes, the pause of
// someone who is coming back. It was the stock player's thirty seconds, and a
// pause under a minute with the headphones on got the pop in the ears.
//
// Nothing plugged in, or the music going to Bluetooth or a USB DAC: off as
// soon as the device is handed back. Nobody is on the socket to hear it, and
// the amplifier has nothing to do.
//
// [audio] amp_off_plugged_seconds and amp_off_unplugged_seconds.
#define AMP_OFF_PLUGGED_S 300
#define AMP_OFF_UNPLUGGED_S 0
static bool idle_amp_off(long paused_ms);
static long idle_amp_off_ms(void);

// When idle_amp_off() last found the amplifier on with its wait not over: a
// park is coming. See audio_amp_park_pending().
static volatile long amp_wait_seen_ms;

// True when the stream is going out over Bluetooth rather than to the DAC.
static bool output_is_bluetooth(void) {
	char device[sizeof(output_pcm)];
	audio_get_output_device(device, sizeof(device));
	return strncmp(device, "bluealsa", 8) == 0;
}

// ...and over Bluetooth the wait is zero.
//
// bluealsa's PCM is not a sound card. The A2DP transport is acquired when the
// stream starts and released the moment it is dropped, and snd_pcm_prepare()
// does not take it back: writes after an unpause are accepted by the plugin and
// go nowhere.
//
// So on Bluetooth the pause hands the device back immediately and the unpause
// opens a fresh stream, which is the same path a track change takes. Nothing is
// lost by it: there is no DAC to keep warm at that end of the link.
static long paused_device_close_ms(void) { return output_is_bluetooth() ? 0 : PAUSED_DEVICE_CLOSE_MS; }

// Set while the metronome waits for the output that a paused track still holds
// (audio_external_begin_yielding): the pause hands it back at its next check
// rather than when PAUSED_DEVICE_CLOSE_MS is up.
static volatile bool paused_release_wanted;

bool audio_output_is_bluetooth(void) { return output_is_bluetooth(); }

// ---------------------------------------------------------------------------
// "Dissolvenza": the volume ramp at each end of a track.
//
// One decoder, one stream: two tracks cannot overlap the way a mixing desk
// would cross-fade them, so this is the honest version of the effect -- the
// last seconds of a track ramp down to silence and the first seconds of the
// next ramp up from it. Back to back, that is what a listener hears as a
// cross-fade.
//
// The gain is Q10 fixed point (1024 = unity): plain integer multiplies on the
// sample, no floating point anywhere near the per-sample path.
// ---------------------------------------------------------------------------

static volatile bool fade_enabled;
static volatile int fade_secs = 3;

void audio_set_fade(bool enabled, int seconds) {
	if (seconds < 1) {
		seconds = 1;
	}
	if (seconds > 12) {
		seconds = 12;
	}
	fade_enabled = enabled;
	fade_secs = seconds;
	fprintf(stderr, "audio: fade %s (%d s)\n", enabled ? "on" : "off", seconds);
}

// 0..1024 for a position in the track. Unity everywhere but the two ends.
static int fade_gain_q10(double pos_secs, double total_secs) {
	if (!fade_enabled) {
		return 1024;
	}

	double span = (double)fade_secs;
	int gain = 1024;

	if (pos_secs < span) {
		int in_gain = (int)((pos_secs / span) * 1024.0);
		gain = in_gain < gain ? in_gain : gain;
	}
	if (total_secs > span * 2 && pos_secs > total_secs - span) {
		double left = total_secs - pos_secs;
		int out_gain = left > 0 ? (int)((left / span) * 1024.0) : 0;
		gain = out_gain < gain ? out_gain : gain;
	}

	if (gain < 0) {
		gain = 0;
	}
	if (gain > 1024) {
		gain = 1024;
	}
	return gain;
}

static void fade_apply_s16(short *samples, int count, int gain) {
	if (gain >= 1024) {
		return;
	}
	for (int i = 0; i < count; i++) {
		samples[i] = (short)(((int)samples[i] * gain) >> 10);
	}
}

static void fade_apply_s32(int32_t *samples, int count, int gain) {
	if (gain >= 1024) {
		return;
	}
	for (int i = 0; i < count; i++) {
		samples[i] = (int32_t)(((int64_t)samples[i] * gain) >> 10);
	}
}
static bool stop_thread = false;

static double seek_target_secs = -1.0;
static bool seek_request = false;

// Set true by the playback thread when a track plays through to its end (as
// opposed to being stopped/replaced by the user). Consumed via
// audio_take_completion() so the controller can auto-advance.
static bool track_completed = false;
static void (*completion_hook)(void);

void audio_set_completion_hook(void (*hook)(void)) { completion_hook = hook; }

// Marks the end of a track's playback routine. The catch is that tearing down
// the old track happens *after* audio_play() has already announced the new one
// (the thread only notices `play_request` once it drops out of its write loop,
// and snd_pcm_drain() can sit there for a while). Blindly writing STOPPED here
// would overwrite the new track's PLAYING and leave the UI showing a stopped
// player for a track that is in fact starting -- so when another file is
// already queued up, leave the status alone.
// Caller must hold audio_mutex.
static void mark_stopped_unless_replaced(void) {
	if (!play_request) {
		playback_status = AUDIO_STATUS_STOPPED;
	}
}

static double progress_current_secs = 0.0;

// Where the bar was when the track was paused.
//
// Pausing winds the decoder back over the frames that were queued but never
// heard, so resuming carries on from the sound and not from the file. That is
// right for the audio and wrong for the display: the position on screen jumps
// backwards by however deep the output buffer is, which on this player is most
// of a second.
//
// So the reported position has a floor while the rewind is in effect. The bar
// stays where the music stopped, resuming replays the buffered moment behind
// it, and the floor lifts by itself as soon as real progress passes it. -1
// means no floor.
static double progress_floor_secs = -1.0;

// The position to report for a file that is being opened: where a seek already
// waiting for it will put it, or 0. A reopen at the same point -- the output
// moving to or from the headphones -- is a play and a seek, and reporting 0
// until the seek is reached drew the bar at the start and back. Caller holds
// audio_mutex.
static double opening_position(void) {
	return seek_request && seek_target_secs > 0 ? seek_target_secs : 0.0;
}

// The position after a block has gone out, unless a seek has been asked for
// meanwhile: audio_seek() already reports its target, and the block that was
// being written when it arrived belongs to the old position. Caller holds
// audio_mutex.
static void report_position(double seconds) {
	if (!seek_request) {
		progress_current_secs = seconds;
	}
}

// Playback speed, for audiobooks. It lives here rather than being handed in
// with the track because it can be changed while one is playing, from the
// player's own pop-over. 1.0 is untouched and costs nothing: speed.c bypasses
// itself entirely at that factor.
static double playback_speed = 1.0;

// What speed.c pulls its input through.
typedef struct {
	decoder_t *dec;
} decoder_fill_t;

static int decoder_fill(void *user, short *dst, int frames) {
	decoder_fill_t *ctx = user;
	return (int)decoder_read_pcm_frames_s16(ctx->dec, (uint64_t)frames, dst);
}
static double progress_total_secs = 0.0;

static int stream_sample_rate = 0;
static int stream_channels = 0;
static int stream_bits = 0; // source bit depth (16/24/32), for the format line
// DSD, for the format line: 64/128/256/512, or 0 when the track is not DSD. A DSD
// track's real identity is its multiple, not "24 bit at 176.4 kHz" -- that is
// only what the carrier looks like.
static int stream_dsd_multiple = 0;
// Also for the format line and the details page: what is inside the file and
// at how many kbps. A lossy format has no bit depth of its own -- see
// decoder_is_lossy() -- and is described by bitrate instead.
static int stream_bitrate_kbps = 0;
static bool stream_lossy = false;
static char stream_codec[16] = "";

// Little-endian readers for the header fields.
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const unsigned char *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#define WAVE_FORMAT_PCM 0x0001
#define WAVE_FORMAT_IEEE_FLOAT 0x0003
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE

static int parse_wav(const char *filepath, wav_info_t *info, FILE **file_out) {
	FILE *f = fopen(filepath, "rb");
	if (!f) {
		fprintf(stderr, "Audio: Failed to open file: %s\n", filepath);
		return WAV_BAD;
	}

	memset(info, 0, sizeof(*info));

	unsigned char riff[12];
	if (fread(riff, 1, sizeof(riff), f) != sizeof(riff) || memcmp(riff, "RIFF", 4) != 0 ||
		memcmp(riff + 8, "WAVE", 4) != 0) {
		fclose(f);
		return WAV_BAD;
	}

	if (fseeko(f, 0, SEEK_END) != 0) {
		fclose(f);
		return WAV_BAD;
	}
	int64_t file_end = (int64_t)ftello(f);
	fseeko(f, (off_t)sizeof(riff), SEEK_SET);

	int tag = 0;
	int bits = 0;
	bool have_fmt = false;

	unsigned char head[8];
	while (fread(head, 1, sizeof(head), f) == sizeof(head)) {
		uint32_t size = le32(head + 4);
		int64_t body = (int64_t)ftello(f);
		if (body < 0) {
			break;
		}

		if (memcmp(head, "fmt ", 4) == 0) {
			// Up to the end of WAVEFORMATEXTENSIBLE: 16 bytes of WAVEFORMAT, the
			// extension size, valid bits, channel mask and the sub-format GUID,
			// whose first two bytes are the real format tag.
			unsigned char fmt[40];
			size_t want = size < sizeof(fmt) ? size : sizeof(fmt);
			if (size < 16 || fread(fmt, 1, want, f) != want) {
				fclose(f);
				return WAV_BAD;
			}
			tag = le16(fmt);
			info->channels = le16(fmt + 2);
			info->sample_rate = (int)le32(fmt + 4);
			bits = le16(fmt + 14);
			if (tag == WAVE_FORMAT_EXTENSIBLE) {
				tag = want >= 26 ? le16(fmt + 24) : 0;
			}
			have_fmt = true;
		} else if (memcmp(head, "data", 4) == 0) {
			info->data_offset = body;
			// 0 and 0xFFFFFFFF are what a writer leaves when it never went back
			// to fill the length in, and a length past the end of the file is
			// a truncated copy: in all three cases the audio runs to the end.
			int64_t declared = (int64_t)size;
			if (size == 0 || size == 0xFFFFFFFFu || body + declared > file_end) {
				declared = file_end - body;
			}
			info->data_size = declared;
			break;
		}

		// Chunks are padded to an even length.
		if (fseeko(f, (off_t)(body + (int64_t)size + (size & 1)), SEEK_SET) != 0) {
			break;
		}
	}

	if (!have_fmt || !info->data_offset || info->channels <= 0 || info->channels > 8 || info->sample_rate <= 0) {
		fclose(f);
		return WAV_BAD;
	}

	info->bits_per_sample = bits;
	if (tag == WAVE_FORMAT_PCM && bits == 8) {
		info->sample = WAV_PCM_U8;
	} else if (tag == WAVE_FORMAT_PCM && bits == 16) {
		info->sample = WAV_PCM_S16;
	} else if (tag == WAVE_FORMAT_PCM && bits == 24) {
		info->sample = WAV_PCM_S24;
	} else if (tag == WAVE_FORMAT_PCM && bits == 32) {
		info->sample = WAV_PCM_S32;
	} else if (tag == WAVE_FORMAT_IEEE_FLOAT && bits == 32) {
		info->sample = WAV_FLOAT32;
	} else if (tag == WAVE_FORMAT_IEEE_FLOAT && bits == 64) {
		info->sample = WAV_FLOAT64;
	} else {
		fprintf(stderr, "audio: '%s' is WAV format 0x%04x at %d bits, not read by the WAV path\n", filepath, tag,
				bits);
		fclose(f);
		return WAV_UNSUPPORTED;
	}

	info->src_frame_bytes = info->channels * (bits / 8);
	info->out_bits = (info->sample == WAV_PCM_U8 || info->sample == WAV_PCM_S16) ? 16 : 32;

	// Whole frames only.
	info->data_size -= info->data_size % info->src_frame_bytes;

	fseeko(f, (off_t)info->data_offset, SEEK_SET);
	*file_out = f;
	return WAV_OK;
}

// Turns `frames` frames read from the file into the samples the device takes.
// Only called for the encodings that need it: S16 and S32 are read straight
// into the output buffer.
static void wav_convert(const wav_info_t *info, const unsigned char *in, void *out, size_t frames) {
	size_t n = frames * (size_t)info->channels;
	switch (info->sample) {
	case WAV_PCM_U8: {
		int16_t *o = out;
		for (size_t i = 0; i < n; i++) {
			o[i] = (int16_t)(((int)in[i] - 128) * 256);
		}
		break;
	}
	case WAV_PCM_S24: {
		// Left-justified, as a 24-bit FLAC arrives: the low byte is zero.
		int32_t *o = out;
		for (size_t i = 0; i < n; i++, in += 3) {
			uint32_t v = (uint32_t)in[0] << 8 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 24;
			o[i] = (int32_t)v;
		}
		break;
	}
	case WAV_FLOAT32: {
		int32_t *o = out;
		for (size_t i = 0; i < n; i++) {
			float x;
			memcpy(&x, in + i * 4, sizeof(x));
			o[i] = x >= 1.0f ? INT32_MAX : x < -1.0f ? INT32_MIN : x == x ? (int32_t)(x * 2147483648.0f) : 0;
		}
		break;
	}
	case WAV_FLOAT64: {
		int32_t *o = out;
		for (size_t i = 0; i < n; i++) {
			double x;
			memcpy(&x, in + i * 8, sizeof(x));
			o[i] = x >= 1.0 ? INT32_MAX : x < -1.0 ? INT32_MIN : x == x ? (int32_t)(x * 2147483648.0) : 0;
		}
		break;
	}
	case WAV_PCM_S16:
	case WAV_PCM_S32:
		break;
	}
}

static bool wav_needs_conversion(const wav_info_t *info) {
	return info->sample != WAV_PCM_S16 && info->sample != WAV_PCM_S32;
}

// When non-zero, the next open_pcm_device() sizes the buffer to this many
// milliseconds instead of its own 750.
//
// It exists for the emulator, whose need is the reverse of the one the 750 ms
// serve: a game has to sound a button press now, not three quarters of a second
// later, and the short buffer doubles as its metronome -- writing 738 samples
// per frame to a blocking PCM lets ALSA keep the 59.73 fps pace instead of an
// estimating usleep. The short reserve is affordable because the emulator does
// not read the card while running: the ROM is already entirely in memory.
static int pcm_forced_buffer_ms;

// ---------------------------------------------------------------------------
// Gapless: the PCM that stays open between tracks
//
// Without it, the end of a track drains the PCM (snd_pcm_drain waits for the
// card to play everything out), closes it and parks the thread; the interface
// notices on the next tick of its timer, opens the next file, reopens the PCM
// and starts writing again. Drain, close, a timer tick and two opens sit
// between the last sample of one track and the first of the next -- a few
// tenths of a second on this device, which is a defect on a live album or a
// concept record where one track runs into the next.
//
// With it, the PCM is neither drained nor closed at the end of a track: it
// stays open with its queue still inside -- up to 750 ms of music already
// handed to the card, which keeps playing on its own. The next track opens
// while that queue drains and appends to the same queue, so the card is never
// left with nothing to play and there is no silence.
//
// The conditions for holding it are deliberately strict:
//
//   * the track must have ended on its own. After next, stop, or another
//     track, the previous queue is not wanted: snd_pcm_drop as usual.
//   * no DoP. A DSD stream has the DAC in a mode of its own, and appending PCM
//     to it is full-volume noise.
//   * the next track must have exactly the same format: channels, rate and
//     depth. An open PCM is configured on those three numbers and cannot be
//     reconfigured without closing it, which is the very thing being avoided.
//     A 44.1 kHz album plays gapless; the 96 kHz track after it reopens and
//     the gap returns, as it should.
//   * and the same output. Switching to Bluetooth midway is a different PCM.
//
// If the next track never comes, the thread waits a little over half a second
// (GAPLESS_HOLD_MS, less than the queue the card holds), then drains and
// closes as it would have done immediately. End of album therefore costs half
// a second of PCM held open, inaudibly.
// ---------------------------------------------------------------------------

// How long a PCM is held open while no next track arrives. 600 ms: less than
// the ~750 the card has queued -- otherwise it would run dry first -- and more
// than one tick of the timer that advances the queue.
#define GAPLESS_HOLD_MS 600

// How long a file that is still downloading is waited on before it is declared
// unreadable. Six seconds in half-second steps: long enough for a slow network
// to deliver the header, short enough not to look like a freeze -- and the
// wait gives up on the first command anyway. See play_decoded_file().
#define DECODER_GROW_RETRIES 12
#define DECODER_GROW_RETRY_MS 500

static bool gapless_enabled;

// The pointer sits under a small lock of its own, held only briefly: another
// thread can come to close it (the emulator claiming the PCM for itself, see
// audio_external_begin_latency), and closing the same handle twice is a crash.
// The actual drain happens outside the lock -- snd_pcm_drain waits for the
// card to finish, and nobody should queue up behind that.
static pthread_mutex_t held_lock = PTHREAD_MUTEX_INITIALIZER;
static snd_pcm_t *held_pcm;
static int held_channels;
static int held_rate;
static int held_bits;
static snd_pcm_uframes_t held_period;
static char held_device[sizeof(output_pcm)];

// A Bluetooth stream converted to the headphones' rate is a plug of this
// player's own, opened under this name; see open_bluetooth_at_rate(). It starts
// with "bluealsa" so that pcm_is_bluetooth() knows the handle for what it is.
#define BT_CONVERTED_PCM "bluealsa_sonix"

// The route such a handle stands for, since its own name is the plug's.
static snd_pcm_t *bt_converted_pcm;
static char bt_converted_device[sizeof(output_pcm)];

// The device name a handle was opened for: the route, for the converted plug.
static const char *pcm_device_name(snd_pcm_t *pcm) {
	if (pcm && pcm == bt_converted_pcm) {
		return bt_converted_device;
	}
	return pcm ? snd_pcm_name(pcm) : NULL;
}
static int held_route;
static bool held_empty; // dropped and re-prepared for a skip: nothing queued, must not be "started" to drain
// When the PCM was taken into hold, so the log says how long the next track
// took to arrive: the margin gapless works within is the queue held at the end.
static long held_since_ms;

void audio_set_gapless(bool enabled) {
	gapless_enabled = enabled;
	printf("audio: gapless %s\n", enabled ? "on" : "off");
}

bool audio_get_gapless(void) { return gapless_enabled; }

// How long a stream is given to play out what it still holds before it is
// dropped instead. Comfortably more than the deepest buffer this player opens,
// and short enough that a link which is never going to drain does not take the
// playback thread with it.
#define DRAIN_TIMEOUT_MS 3000

// ...and how long it may make no progress at all before it is taken for
// finished. The buffer going down slowly is a stream still playing out; the
// buffer not going down is one nobody is reading.
#define DRAIN_STALL_MS 200

// How long a period should last above 192 kHz, and how large one is ever
// allowed to get. The length is what 4096 frames last at 44.1 kHz, which is the
// sizing every rate below has been using all along; the cap is there because
// the frames are what the driver has to find DMA memory for, and a request it
// cannot meet is settled downwards rather than refused.
#define PERIOD_TARGET_MS 90
#define PERIOD_FRAMES_MAX 32768

// The smallest period the two limits below will go down to: 23 ms at 44.1 kHz,
// the size the stock player runs with.
#define PERIOD_FRAMES_MIN 1024

// The most a USB DAC's buffer is asked to take up to 192 kHz, and how many
// times a buffer the driver found no memory for is halved before giving up.
// See open_pcm_device_now().
#define USB_BUFFER_MAX_BYTES (512u * 1024u)
// ...and in time: on the port the volume is taken off the samples before they
// are queued, so a change is heard only once the queue ahead of it has played.
// 370 ms at 44.1 kHz instead of 743, still several stalls' worth.
#define USB_BUFFER_MAX_MS 400
#define PCM_NOMEM_HALVINGS 3

// How much audio one turn of the decode loop handles.
//
// Not the same question as how deep the card's buffer is, and it has to be
// asked separately: the driver settles the period wherever its DMA memory runs
// out, so a long one asked for at 705.6 kHz can still come back short. The loop
// would then turn a hundred and seventy times a second whatever was requested,
// and the thread doing the turning is real-time on the one core the interface
// also wants. It is not the work that hurts there, it is being interrupted that
// often to do it.
//
// So a turn reads and writes whole periods until it has about this much, and
// blocks once instead of eight times. snd_pcm_writei takes any number of
// frames; more than a period is queued and waited on, which is the same waiting
// done in one piece.
#define CHUNK_TARGET_MS 90

static snd_pcm_uframes_t chunk_for(snd_pcm_uframes_t period, int sample_rate) {
	snd_pcm_uframes_t want = (snd_pcm_uframes_t)(((long)sample_rate * CHUNK_TARGET_MS) / 1000);
	snd_pcm_uframes_t chunk = period;
	while (chunk < want && chunk + period > chunk) {
		chunk += period;
	}
	return chunk;
}

// How long the end of a track waits after closing a bluealsa PCM, before the
// next track opens one. See the call site.
#define BT_REOPEN_SETTLE_MS 200

// Whether this handle -- not the route installed right now -- writes to
// bluealsa. Asked of the handle, because the two can differ: the headphones
// dropping flips the route back to the jack while the track being finished is
// still on the old PCM, and every question below is about that PCM.
// snd_pcm_name() answers with the name the handle was opened under, so it is
// also right when an open fell back to the jack on its own.
static bool pcm_is_bluetooth(snd_pcm_t *pcm) {
	const char *name = pcm ? snd_pcm_name(pcm) : NULL;
	return name && strncmp(name, "bluealsa", 8) == 0;
}

// Plays out what the stream still holds, and comes back whatever happens.
//
// snd_pcm_drain() on a blocking handle waits for the buffer to empty, and over
// Bluetooth that can be for ever. A bluealsa PCM is not a sound card: there is
// no clock consuming the buffer, only the A2DP link, and headphones walking out
// of range are enough for it to stop consuming anything. A playback thread that
// goes into alsa-lib there never comes out: the track never ends, the next play
// request is never picked up, and the handle is never closed, which keeps
// bluealsa's client alive with its transport acquired so the headphones cannot
// re-establish it either.
//
// So over Bluetooth the waiting is done here instead: ask how much is still
// queued, sleep, ask again, and give up at the deadline. snd_pcm_delay() reads
// the stream's pointer and cannot block. On a real card the ordinary drain is
// used, bounded by a clock that always runs.
static void pcm_drain_bounded(snd_pcm_t *pcm) {
	if (!pcm) {
		return;
	}
	if (!pcm_is_bluetooth(pcm)) {
		snd_pcm_drain(pcm);
		return;
	}

	long waited_ms = 0;
	long stalled_ms = 0;
	snd_pcm_sframes_t previous = -1;
	for (;;) {
		snd_pcm_sframes_t left = 0;
		// A handle that cannot say (an xrun, a plugin that has given up) has
		// nothing left worth waiting for.
		if (snd_pcm_delay(pcm, &left) < 0 || left <= 0) {
			break;
		}

		// Gone as soon as it stops going down, not at the far end of the
		// deadline. At the natural end of a track over A2DP the queue empties
		// to within a period and then simply stops, because nothing is asking
		// bluealsa for more; sitting out the whole deadline for that would put
		// three seconds of silence between one track and the next. A fifth of a
		// second without progress is proof enough.
		if (previous >= 0 && left >= previous) {
			stalled_ms += 20;
		} else {
			stalled_ms = 0;
		}
		previous = left;

		if (stalled_ms >= DRAIN_STALL_MS || waited_ms >= DRAIN_TIMEOUT_MS) {
			fprintf(stderr, "audio[%ld]: the Bluetooth PCM will not drain (%ld frames left after %ld ms): letting it go\n",
					log_ms(), (long)left, waited_ms);
			break;
		}
		usleep(20 * 1000);
		waited_ms += 20;
	}
	// Either it played out or it never will; in both cases the stream stops
	// here rather than in a wait with no end.
	snd_pcm_drop(pcm);
}

// Closes the held PCM, letting it play out what it still has queued. Playback
// thread only: nothing else touches held_pcm.
static void gapless_release(void) {
	pthread_mutex_lock(&held_lock);
	snd_pcm_t *pcm = held_pcm;
	held_pcm = NULL;
	bool empty = held_empty;
	pthread_mutex_unlock(&held_lock);

	if (!pcm) {
		return;
	}
	fprintf(stderr, "audio[%ld]: gapless: closing the held PCM after %ld ms%s\n", log_ms(), log_ms() - held_since_ms,
			empty ? " (empty, kept over a skip)" : "");
	if (empty) {
		snd_pcm_close(pcm);
		pcm_device_open = false;
		return;
	}
	if (snd_pcm_state(pcm) == SND_PCM_STATE_PREPARED) {
		snd_pcm_start(pcm);
	}
	// Drain rather than drop: what is in there is the real tail of the track
	// that just ended, and it has to be heard. With a deadline, because a
	// handle that will never drain must not take the thread with it.
	pcm_drain_bounded(pcm);
	snd_pcm_close(pcm);
	pcm_device_open = false;
}

// True while a PCM is currently being held.
static bool gapless_holding(void) {
	pthread_mutex_lock(&held_lock);
	bool held = held_pcm != NULL;
	pthread_mutex_unlock(&held_lock);
	return held;
}

// The track ended on its own and the format can be reused, so keep the PCM.
static void gapless_hold(snd_pcm_t *pcm, int channels, int rate, int bits, snd_pcm_uframes_t period, bool empty) {
	pthread_mutex_lock(&held_lock);
	held_pcm = pcm;
	held_empty = empty;
	pthread_mutex_unlock(&held_lock);
	held_channels = channels;
	held_rate = rate;
	held_bits = bits;
	held_period = period;
	held_route = alsa_output_key();
	held_since_ms = log_ms();
	// The device this handle is really on. Taken from the handle and not from
	// the route, for the reason given at keep_open: the route can already have
	// moved on, and then the next track would be handed a PCM to somewhere it
	// did not ask for.
	const char *opened_as = pcm_device_name(pcm);
	snprintf(held_device, sizeof(held_device), "%s", opened_as ? opened_as : AUDIO_DEFAULT_PCM);
	fprintf(stderr, "audio[%ld]: gapless: holding the PCM (%d ch, %d Hz, %d bit)%s\n", log_ms(), channels, rate, bits,
			empty ? ", emptied for a skip" : "");
}

// Whether a bluealsa handle can be held for the next track, or taken back for
// it.
//
// A bluealsa PCM is not a sound card: its A2DP transport is released once the
// stream stops being fed, and writes after that are accepted by the plugin and
// go nowhere. So the handle must still be RUNNING with at least
// BT_HOLD_MIN_QUEUE_MS of audio queued: the transport has never gone idle, and
// the next track's first write lands before the queue runs out. And it must
// still be the output: headphones dropping mid-track flip the route back to
// the jack at once (bluetooth.c, apply_output_routing), and a track ending a
// moment later would otherwise hold a dead handle.
#define BT_HOLD_MIN_QUEUE_MS 120

static bool bt_hold_usable(snd_pcm_t *pcm) {
	if (snd_pcm_state(pcm) != SND_PCM_STATE_RUNNING) {
		return false;
	}
	char device[sizeof(output_pcm)];
	audio_get_output_device(device, sizeof(device));
	const char *opened_as = pcm_device_name(pcm);
	if (!opened_as || strcmp(device, opened_as) != 0) {
		return false;
	}
	unsigned int rate = 0;
	snd_pcm_hw_params_t *hw;
	snd_pcm_hw_params_alloca(&hw);
	if (snd_pcm_hw_params_current(pcm, hw) < 0 || snd_pcm_hw_params_get_rate(hw, &rate, NULL) < 0 || rate == 0) {
		return false;
	}
	snd_pcm_sframes_t queued = 0;
	if (snd_pcm_delay(pcm, &queued) < 0) {
		return false;
	}
	return queued >= (snd_pcm_sframes_t)(rate * BT_HOLD_MIN_QUEUE_MS / 1000);
}

// "Keep the PCM across a manual skip": the new track asked for is already
// waiting, so the old queue is dropped (what a skip always did) and the
// handle, put back in PREPARED, is held for it instead of being closed. Same
// format is still checked by gapless_take(), which closes it otherwise. It
// saves the close and open of the card at every skip: the new track is heard
// sooner (Blaze, skip to sound: median 84 ms -> 46 ms). [audio] keep_pcm_on_skip = 0 turns
// it off.
static bool skip_hold_prepare(snd_pcm_t *pcm, bool passthrough, bool is_paused) {
	if (!pcm || passthrough || is_paused || pcm_is_bluetooth(pcm) || !config_get_int("audio", "keep_pcm_on_skip", 1)) {
		return false;
	}
	pthread_mutex_lock(&audio_mutex);
	bool next_waiting = play_request;
	pthread_mutex_unlock(&audio_mutex);
	if (!next_waiting) {
		return false; // a stop, or the end of the queue: close as usual
	}
	dac_ramp_before_cut(pcm);
	snd_pcm_drop(pcm);
	if (snd_pcm_prepare(pcm) < 0) {
		return false;
	}
	return true;
}

// The held PCM, when it is the one needed. On a format mismatch it is closed
// and NULL returned, so the caller opens a device as usual.
static snd_pcm_t *gapless_take(int channels, int rate, int bits, snd_pcm_uframes_t *period_out) {
	if (!gapless_holding()) {
		return NULL;
	}

	char device[sizeof(output_pcm)];
	audio_get_output_device(device, sizeof(device));

	// The output route counts too: headphones can have been plugged in between
	// the two tracks, and reprogramming the codec under a live stream is
	// exactly what the stock binary never does.
	if (channels != held_channels || rate != held_rate || bits != held_bits || strcmp(device, held_device) != 0 ||
		alsa_output_key() != held_route) {
		fprintf(stderr, "audio[%ld]: gapless: different format (%d/%d/%d against %d/%d/%d), reopening\n", log_ms(), channels,
				rate, bits, held_channels, held_rate, held_bits);
		gapless_release();
		return NULL;
	}

	pthread_mutex_lock(&held_lock);
	snd_pcm_t *pcm = held_pcm;
	held_pcm = NULL;
	pthread_mutex_unlock(&held_lock);
	if (!pcm) {
		return NULL;
	}
	if (pcm_is_bluetooth(pcm) && !bt_hold_usable(pcm)) {
		fprintf(stderr, "audio[%ld]: gapless: the Bluetooth queue ran low or the stream stopped, reopening\n",
				log_ms());
		pthread_mutex_lock(&held_lock);
		held_pcm = pcm;
		pthread_mutex_unlock(&held_lock);
		gapless_release();
		return NULL;
	}
	if (period_out) {
		*period_out = held_period;
	}
	fprintf(stderr, "audio[%ld]: gapless: reusing the open PCM, the next track came %ld ms after the end\n", log_ms(),
			log_ms() - held_since_ms);
	return pcm;
}

// The output as an external source holds it, and who to ask for it back. Both
// are read in open_pcm_device() without external_lock: that pointer is what the
// retry loop is about to find busy anyway, so a stale read costs at worst one
// request that was not needed, or one that arrives a moment late.
static snd_pcm_t *external_pcm;
static bool (*external_release_cb)(void);
// Set when anything else asks for the output while an external source holds
// it, for a source with no callback to be told through (audio_external_wanted).
static volatile bool external_wanted;
// The holder gives way to anyone who asks (audio_external_begin_yielding).
static bool external_yielding;

void audio_set_external_release_cb(bool (*cb)(void)) { external_release_cb = cb; }

// Asks whoever holds the output for it back, if anyone does.
//
// The AirPlay receiver holds the PCM for as long as it is switched on -- it
// cannot tell that the phone has finished, because shairport keeps its socket
// open for its whole life. Without this, a local track or a streamed one would
// meet a busy device, retry for two seconds and give up.
//
// The request does not block: the hand-back happens on the AirPlay thread
// within its poll, and the retry loop in open_pcm_device() is already built to
// wait out a device that is busy for a moment.
//
// Never fires on the external path itself: audio_external_begin_latency() only
// reaches open_pcm_device() with external_pcm still NULL.
static void release_external_output(void) {
	if (external_pcm) {
		external_wanted = true;
	}
	if (external_pcm && external_release_cb) {
		external_release_cb();
	}
}

bool audio_external_wanted(void) { return external_wanted; }

static void bt_seek_probe_cancel(void);

// Which bluealsa plugin the ALSA loader actually mapped, said once.
//
// The plugin is a file in the firmware, not part of this build, and the two
// that can be at that path are not interchangeable: the stock 4.1.1 drains with
// poll(..., -1) and never reports SND_PCM_STATE_DRAINING, the 4.3.1 does both.
// The path alone does not say which is there, so the size goes with it -- it is
// what tells a replaced file from the original without opening a shell.
static void log_bluealsa_plugin(void) {
	static bool said;
	if (said) {
		return;
	}
	said = true;

	FILE *maps = fopen("/proc/self/maps", "r");
	if (!maps) {
		return;
	}
	char line[512];
	while (fgets(line, sizeof(line), maps)) {
		char *at = strstr(line, "/libasound_module_pcm_bluealsa.so");
		if (!at) {
			continue;
		}
		char *start = strrchr(line, ' ');
		if (!start) {
			break;
		}
		start++;
		start[strcspn(start, "\r\n")] = '\0';
		struct stat st;
		if (stat(start, &st) == 0) {
			fprintf(stderr, "audio: bluealsa plugin %s (%ld bytes)\n", start, (long)st.st_size);
		} else {
			fprintf(stderr, "audio: bluealsa plugin %s\n", start);
		}
		break;
	}
	fclose(maps);
}

// The plugin chain behind an opened PCM (plug, conversions, dmix, rate, the
// hardware) as snd_pcm_dump() describes it, logged whenever it differs from the
// last one logged.
static void log_pcm_chain(snd_pcm_t *pcm) {
	static char last[4096];
	snd_output_t *out = NULL;
	if (snd_output_buffer_open(&out) < 0) {
		return;
	}
	char *text = NULL;
	if (snd_pcm_dump(pcm, out) >= 0 && snd_output_buffer_string(out, &text) > 0 && text &&
		strncmp(text, last, sizeof(last) - 1) != 0) {
		snprintf(last, sizeof(last), "%s", text);
		char *save = NULL;
		for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			fprintf(stderr, "audio: pcm | %s\n", line);
		}
	}
	snd_output_close(out);
}

// ---------------------------------------------------------------------------
// Bluetooth at the headphones' own rate
//
// bluealsa's plugin answers a rate other than the one the A2DP stream runs at by
// asking the headphones for a new configuration, in the middle of the
// connection: the transport is torn down and built again. Some headphones
// refuse ("Couldn't set A2DP configuration: Resource temporarily unavailable"),
// the track does not start, and with a pair of ROSE Ceramics U bluetoothd went
// down after the refusal and took the connection with it. Where it works it is
// still a gap, and the remote's buttons dropping, at every change of rate.
//
// So the stream stays at the rate chosen when the link came up, and the track is
// converted to it by a plug of this player's own in front of bluealsa's PCM, its
// slave rate fixed. [bluetooth] follow_track_rate = 1 asks the headphones
// instead, as before.
// ---------------------------------------------------------------------------

// Opens `device` (a bluealsa name) behind a plug whose slave runs at `rate`,
// under BT_CONVERTED_PCM. The plug is added to a copy of the global
// configuration, where bluealsa's own definitions are, and the copy is dropped
// once the PCM is open -- which is what snd_pcm_open() does with the
// configuration it opens from, too.
static int open_bluetooth_at_rate(snd_pcm_t **pcm, const char *device, unsigned rate, int mode) {
	char text[sizeof(output_pcm) + 128];
	int len = snprintf(text, sizeof(text), "pcm." BT_CONVERTED_PCM " { type plug slave { pcm \"%s\" rate %u } }",
					   device, rate);
	if (len < 0 || (size_t)len >= sizeof(text)) {
		return -EINVAL;
	}

	int err = snd_config_update();
	if (err < 0) {
		return err;
	}
	snd_config_t *conf = NULL;
	err = snd_config_copy(&conf, snd_config);
	if (err < 0) {
		return err;
	}
	snd_input_t *in = NULL;
	err = snd_input_buffer_open(&in, text, len);
	if (err >= 0) {
		err = snd_config_load(conf, in);
		snd_input_close(in);
	}
	if (err >= 0) {
		err = snd_pcm_open_lconf(pcm, BT_CONVERTED_PCM, SND_PCM_STREAM_PLAYBACK, mode, conf);
	}
	snd_config_delete(conf);
	if (err >= 0) {
		bt_converted_pcm = *pcm;
		snprintf(bt_converted_device, sizeof(bt_converted_device), "%s", device);
	}
	return err;
}

#ifndef HOST_BUILD
// How the kernel's free memory is split up, one line per zone: the number of
// free blocks of 1, 2, 4, 8... pages. Written when a driver finds no memory,
// because MemAvailable cannot say whether the memory is short or only broken
// into pieces.
static void log_buddyinfo(const char *when) {
	FILE *f = fopen("/proc/buddyinfo", "r");
	if (!f) {
		return;
	}
	char line[200];
	while (fgets(line, sizeof(line), f)) {
		fprintf(stderr, "audio: free blocks %s | %s", when, line);
	}
	fclose(f);
}
#endif

// Lets go of the kernel's page cache: the clean copies of files it keeps in
// whatever memory is free. Nothing is lost -- the files are still on the card
// and on flash -- and what comes back is free memory in larger pieces, which is
// what a driver allocating its buffers needs and what hours of reading scatter.
// Dirty pages are not touched, and nothing waits for the card.
static void release_page_cache(void) {
#ifndef HOST_BUILD
	log_buddyinfo("before");
	FILE *f = fopen("/proc/sys/vm/drop_caches", "w");
	if (!f) {
		return;
	}
	fputs("1\n", f);
	fclose(f);
	fprintf(stderr, "audio: page cache released for the driver's buffer\n");
	log_buddyinfo("after");
#endif
}

// Set for the length of a DSD track sent raw to a USB DAC ([usb] dsd_mode
// Native): every open of that track, the reopens after an error included, then
// asks hw: rather than plughw: for this format, at exactly the rate given.
// SND_PCM_FORMAT_UNKNOWN otherwise.
static int usb_native_format = SND_PCM_FORMAT_UNKNOWN;

static snd_pcm_t *open_pcm_device_now(int channels, int sample_rate, int bits_per_sample, snd_pcm_uframes_t *period_size_out) {
	snd_pcm_t *pcm_handle = NULL;

	// Not on the card while the route re-init after mem is powering it up.
	alsa_wait_output_reinit();

	release_external_output();

	// A track change closes the old PCM and opens a new one back to back. On
	// real hardware the close is not always instantaneous -- the driver can
	// report EBUSY for a moment while the old stream is torn down -- and one
	// failed open here means the new track silently never starts. So the open
	// is retried briefly instead of being given up on the first attempt.
	char device[sizeof(output_pcm)];
	audio_get_output_device(device, sizeof(device));
	// The plug layer cannot carry SPECIAL (alsa-lib knows no width for it),
	// and raw DSD must reach the card untouched anyway.
	bool native = usb_native_format != SND_PCM_FORMAT_UNKNOWN;
	if (native && strncmp(device, "plughw:", 7) == 0) {
		memmove(device, device + 4, strlen(device + 4) + 1);
	}

	// bluealsa reports the same race differently. Where a card driver says EBUSY
	// while the old stream is torn down, the plugin has to re-acquire the A2DP
	// transport over D-Bus first and answers ENODEV, EIO or ETIMEDOUT until it
	// has -- so on Bluetooth those count as "not yet" too, since the link can
	// need a few hundred milliseconds.
	bool bluetooth = strncmp(device, "bluealsa", 8) == 0;

	// A bluealsa name is worth opening only while there is something on the
	// other end of it. When there is not, opening it is at best two seconds of
	// retries per track and at worst a wait inside the plugin, which talks to
	// the bluealsa daemon over D-Bus and has no deadline of its own -- and all
	// of that happens before the fallback below is ever reached, so a dropped
	// link would leave nothing playing at all, jack included.
	//
	// Asked of bluetooth.c and not guessed: it is the side that hears the
	// AudioSink notifications, and its answer is a read of two booleans.
	if (bluetooth && !bluetooth_audio_active()) {
		fprintf(stderr, "audio: '%s' has no sink any more; the jack takes it back\n", device);
		audio_set_output_device(NULL);
		snprintf(device, sizeof(device), "%s", AUDIO_DEFAULT_PCM);
		bluetooth = false;
	}

	// Non-blocking, over Bluetooth only. It is what lets pcm_write_bluetooth()
	// put a deadline on a write that would otherwise wait for room that may
	// never come again. A sound card keeps a blocking handle: there the clock
	// guarantees the room.
	int mode = bluetooth ? SND_PCM_NONBLOCK : 0;

	// How long an open is given before the fallback below takes over.
	//
	// Two seconds is right for a sound card: what holds it is a driver tearing
	// down the previous stream. A bluealsa open is a different thing -- the
	// plugin has to make bluealsa take the A2DP transport back from bluez over
	// D-Bus, and on one core, with an encoder thread of its own at real-time
	// priority, that round trip is not bounded by anything this player controls.
	//
	// And the cost of giving up early is paid in the wrong place: the fallback
	// plays the track out of the jack, which with the headphones still on the
	// listener's head is silence, one track at a time until an open happens to
	// finish in time.
	//
	// So while the radio still says there is a sink, wait for it.
	// See open_bluetooth_at_rate().
	unsigned convert_to = 0;
	if (bluetooth && !config_get_int("bluetooth", "follow_track_rate", 0)) {
		unsigned sink_rate = bluetooth_sink_rate();
		if (sink_rate > 0 && sink_rate != (unsigned)sample_rate) {
			convert_to = sink_rate;
			fprintf(stderr, "audio: the headphones stay at %u Hz; the %d Hz track is converted to it\n", sink_rate,
					sample_rate);
		}
	}

	int err = -EBUSY;
	int attempts = 0;
	int budget = (bluetooth && bluetooth_audio_active()) ? 160 : 40; // eight seconds, or two
	for (; attempts < budget; attempts++) {
		err = convert_to ? open_bluetooth_at_rate(&pcm_handle, device, convert_to, mode)
						 : snd_pcm_open(&pcm_handle, device, SND_PCM_STREAM_PLAYBACK, mode);
		if (err >= 0) {
			break;
		}
		bool transient = (err == -EBUSY || err == -EAGAIN) ||
						 (bluetooth && (err == -ENODEV || err == -EIO || err == -ETIMEDOUT || err == -ECONNREFUSED));
		if (!transient) {
			break; // a real error, not a teardown race: no point retrying
		}
		usleep(50 * 1000);
	}

	// Headphones that walked out of range take their PCM with them, and a
	// track that refuses to start is a worse outcome than one coming out of
	// the wrong hole. Fall back to the jack, and say so in the log.
	if (err < 0 && strcmp(device, AUDIO_DEFAULT_PCM) != 0) {
		fprintf(stderr, "audio: '%s' would not open (%s); falling back to " AUDIO_DEFAULT_PCM "\n", device,
				snd_strerror(err));
		err = snd_pcm_open(&pcm_handle, AUDIO_DEFAULT_PCM, SND_PCM_STREAM_PLAYBACK, 0);

		// And stop asking for it, so the dead name does not stay installed and
		// cost every following track the same wait while the interface, the
		// volume curve and the power rules all believe the sound is leaving
		// over Bluetooth. Only when the radio agrees the sink is gone: a link
		// that is merely busy for a moment must keep its route, or one failed
		// open would move the listener to the jack for good.
		if (err >= 0 && bluetooth && !bluetooth_audio_active()) {
			audio_set_output_device(NULL);
		}
	}

	if (err < 0) {
		fprintf(stderr, "Audio: Cannot open PCM device '%s': %s (after %d attempts)\n", device, snd_strerror(err),
				attempts);
		return NULL;
	}
	if (attempts > 0) {
		fprintf(stderr, "audio[%ld]: opening '%s' took %d attempts (%d ms)\n", log_ms(), device, attempts,
				attempts * 50);
	}

	snd_pcm_hw_params_t *hw_params;
	snd_pcm_hw_params_alloca(&hw_params);
	snd_pcm_hw_params_any(pcm_handle, hw_params);
	snd_pcm_hw_params_set_access(pcm_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);

	snd_pcm_format_t format;
	if (native) {
		format = (snd_pcm_format_t)usb_native_format;
	} else if (bits_per_sample == 8) {
		format = SND_PCM_FORMAT_U8;
	} else if (bits_per_sample == 16) {
		format = SND_PCM_FORMAT_S16_LE;
	} else if (bits_per_sample == 24) {
		format = SND_PCM_FORMAT_S24_3LE;
	} else if (bits_per_sample == 32) {
		// The built-in card in its own format, so that nothing converts on
		// the way; see pcm_takes_s24().
		format = pcm_takes_s24(pcm_handle, 32) ? SND_PCM_FORMAT_S24_LE : SND_PCM_FORMAT_S32_LE;
	} else {
		fprintf(stderr, "Audio: Unsupported bits per sample: %d\n", bits_per_sample);
		snd_pcm_close(pcm_handle);
		return NULL;
	}

	if (snd_pcm_hw_params_set_format(pcm_handle, hw_params, format) < 0) {
		fprintf(stderr, "audio: format %s refused by the device\n", snd_pcm_format_name(format));
		snd_pcm_close(pcm_handle);
		return NULL;
	}
	// Checked, because a refusal here is silent and sounds like something else
	// entirely. If the device will not do this channel count the parameters
	// keep whatever the hardware allows, and frames written in the layout the
	// caller believes in then come out at the wrong speed with the channels
	// interleaved wrongly -- which is heard as a completely different piece of
	// audio, not as an error.
	if (snd_pcm_hw_params_set_channels(pcm_handle, hw_params, channels) < 0) {
		unsigned int got = (unsigned int)channels;
		snd_pcm_hw_params_set_channels_near(pcm_handle, hw_params, &got);
		fprintf(stderr, "audio: %d channel(s) refused, device settled on %u\n", channels, got);
	}

	// The rate is set exactly, not "near": this DAC follows the track (the CS43198
	// is fed whatever the stream runs at), and silently resampling a 96 kHz file
	// down to 44.1 is precisely what "the DAC does not really change rate" would
	// look like. If the hardware truly cannot do this rate, fall back to the
	// nearest one rather than failing the track -- and say so in the log.
	unsigned int val = sample_rate;
	int dir = 0;
	if (snd_pcm_hw_params_set_rate(pcm_handle, hw_params, val, 0) < 0) {
		if (native) {
			// Raw DSD at any other rate is the wrong speed, not a near miss.
			fprintf(stderr, "usb: native DSD refused at %d Hz\n", sample_rate);
			snd_pcm_close(pcm_handle);
			return NULL;
		}
		snd_pcm_hw_params_set_rate_near(pcm_handle, hw_params, &val, &dir);
		fprintf(stderr, "audio: %d Hz unavailable, device settled on %u Hz\n", sample_rate, val);
	}

	// A deep buffer, on purpose: ~0.75 s at 44.1 kHz. The playback thread can
	// stall for seconds when a big album cover decodes at a track change
	// (memory pressure evicts code pages, the SD is saturated, the thread
	// page-faults), and the AIC driver answers a starved stream with EIO. Four
	// periods of 1024 frames (93 ms) cannot ride that out; this absorbs most of
	// it. Responsiveness does not suffer: stops and switches drop the buffer,
	// they never drain it.
	unsigned int periods = 8;
	snd_pcm_uframes_t period_size = 4096;

	// ...as long as 4096 frames are still a period. Past 96 kHz they stop being
	// one and become a fragment, and the reserve goes with them: 4096 frames are
	// 93 ms at 44.1 kHz, 23 ms at 176.4 kHz, and 5.8 ms at 705.6 kHz, where
	// DSD256 goes over DoP. The whole eight-period buffer at 705.6 kHz was
	// 46 ms, and the stream starts on a full buffer, so that was also all it
	// ever had in hand against an interface sharing the one core. The first
	// thing that takes the core for longer than that is an underrun, a prepare
	// and a rewrite.
	//
	// So above 96 kHz the period is chosen by how long it lasts instead, and the
	// reserve comes out where it is at 44.1 kHz. Everything at or below keeps
	// exactly the sizing it has always had.
	if (sample_rate > 96000) {
		// Doubled while the next step is still near the target rather than past
		// it: a period is a power of two here, and always taking the first one
		// longer than the target would ask for 170 ms at 192 kHz to hit 90.
		snd_pcm_uframes_t want = (snd_pcm_uframes_t)(((long)sample_rate * PERIOD_TARGET_MS) / 1000);
		while (period_size * 2 <= want + want / 2 && period_size * 2 <= PERIOD_FRAMES_MAX) {
			period_size *= 2;
		}
	}

	// A USB DAC is the exception, up to 192 kHz. Its buffer is not memory the
	// card was given at boot, as the built-in DAC's is: snd-usb-audio allocates
	// it at every open, together with the transfer buffers, and frees it at every
	// close -- so on this device, with 56 MB for everything, a track change is a
	// fresh request for all of it. The period sizing above asks 1 MB of it for a
	// 24-bit track at 176.4 or 192 kHz. Capped, those keep a period of 8192
	// frames and a buffer of about 350 ms, which is still several times what one
	// stall of the interface takes. Above 192 kHz (DoP, DXD) the reserve is
	// already as thin as it can be and the sizing stays; a refusal there is left
	// to the retry below.
	//
	// A card named by number is a USB one: the built-in DAC is "default" and
	// Bluetooth is "bluealsa...".
	bool usb_card = strncmp(device, "plughw:", 7) == 0 || strncmp(device, "hw:", 3) == 0;
	unsigned int frame_bytes = (unsigned int)channels * (unsigned int)((bits_per_sample + 7) / 8);
	if (usb_card && sample_rate <= 192000 && frame_bytes > 0) {
		while (period_size > PERIOD_FRAMES_MIN && (period_size * periods * frame_bytes > USB_BUFFER_MAX_BYTES ||
													  period_size * periods * 1000 > (snd_pcm_uframes_t)sample_rate * USB_BUFFER_MAX_MS)) {
			period_size /= 2;
		}
	}

	// The space as it stands before the buffer is sized, for the retry below:
	// once a size has been asked for, the parameters hold that one value and
	// nothing else.
	snd_pcm_hw_params_t *unsized;
	snd_pcm_hw_params_alloca(&unsized);
	snd_pcm_hw_params_copy(unsized, hw_params);

	// Two ways of asking for a buffer, selected by a config key. The default is
	// the frame-based sizing above.
	//
	// The stock player's way, when "audio"/"buffer_time_ms" is set: the size is
	// asked for in TIME and ALSA picks the frames. A value of 200 is roughly
	// what a normal player uses.
	//   [audio]
	//   buffer_time_ms = 200
	// The emulator asks for a much shorter buffer of its own and overrides both:
	// see pcm_forced_buffer_ms.
	int buffer_time_ms = pcm_forced_buffer_ms > 0 ? pcm_forced_buffer_ms : (int)config_get_int("audio", "buffer_time_ms", 0);
	if (buffer_time_ms > 0) {
		unsigned int buffer_time = (unsigned int)buffer_time_ms * 1000u;   // microseconds
		unsigned int period_time = buffer_time / 4;
		snd_pcm_hw_params_set_buffer_time_near(pcm_handle, hw_params, &buffer_time, &dir);
		snd_pcm_hw_params_set_period_time_near(pcm_handle, hw_params, &period_time, &dir);
		fprintf(stderr, "audio: stock-style sizing, buffer %u us / period %u us\n", buffer_time,
				period_time);
	} else {
		snd_pcm_hw_params_set_periods_near(pcm_handle, hw_params, &periods, &dir);
		snd_pcm_hw_params_set_period_size_near(pcm_handle, hw_params, &period_size, &dir);
	}

	err = snd_pcm_hw_params(pcm_handle, hw_params);

	// Out of memory is not a refusal of the format. The driver is asking the
	// kernel for the buffer this very moment, and after hours of reading the
	// card the free memory is in pieces too small for it even with megabytes
	// "available". Asking again at once for the same amount, or for another
	// large amount in another format, is the likeliest way to take the device
	// down with it. So the page cache is let go once -- clean copies of files,
	// which the card still holds -- and the same format is asked for again with
	// half the buffer each time, down to a floor.
	for (int halvings = 0; err == -ENOMEM && buffer_time_ms <= 0 && halvings < PCM_NOMEM_HALVINGS &&
						   period_size / 2 >= PERIOD_FRAMES_MIN;
		 halvings++) {
		if (halvings == 0) {
			release_page_cache();
		}
		period_size /= 2;
		fprintf(stderr, "audio: no memory in the driver for the buffer; asking again with %lu-frame periods\n",
				(unsigned long)period_size);
		snd_pcm_hw_params_copy(hw_params, unsized);
		periods = 8;
		snd_pcm_hw_params_set_periods_near(pcm_handle, hw_params, &periods, &dir);
		snd_pcm_hw_params_set_period_size_near(pcm_handle, hw_params, &period_size, &dir);
		err = snd_pcm_hw_params(pcm_handle, hw_params);
	}
	if (err < 0) {
		fprintf(stderr, "Audio: Cannot apply HW parameters: %s\n", snd_strerror(err));
		last_open_err = err;
		snd_pcm_close(pcm_handle);
		return NULL;
	}

	// Read back what the hardware actually settled on, not what was asked for:
	// this line is the answer to "is the DAC really following the track?" --
	// play a 24/96 FLAC after a 16/44.1 MP3 and the log has to change.
	{
		unsigned int actual_rate = 0;
		snd_pcm_format_t actual_format = SND_PCM_FORMAT_UNKNOWN;
		unsigned int actual_channels = 0;
		int rdir = 0;
		snd_pcm_hw_params_get_rate(hw_params, &actual_rate, &rdir);
		snd_pcm_hw_params_get_format(hw_params, &actual_format);
		snd_pcm_hw_params_get_channels(hw_params, &actual_channels);
		// The device name belongs on this line: without it the log cannot tell
		// a track that went to the headphones from one that went to the jack,
		// since the two otherwise print identically.
		fprintf(stderr, "audio[%ld]: PCM open on '%s': %u Hz %s %u ch (asked for %d Hz / %d bit)\n", log_ms(), device,
				actual_rate, snd_pcm_format_name(actual_format), actual_channels, sample_rate, bits_per_sample);
	}

	// A seek measurement belongs to the stream it was taken on. A track change
	// between the seek and the measurement gives a fresh, still-filling PCM and
	// a number that says nothing about the seek.
	bt_seek_probe_cancel();

	if (bluetooth) {
		log_bluealsa_plugin();
	}

	// What ALSA settled on, whichever way it was asked: with time-based sizing
	// the frames were its choice from the start, and with frame-based sizing a
	// long period can be more than the driver will allocate, so "near" is not
	// "what was asked for". The write loop is driven by this number and the
	// buffer it writes from is allocated to it.
	{
		snd_pcm_uframes_t actual_period = 0;
		if (snd_pcm_hw_params_get_period_size(hw_params, &actual_period, &dir) >= 0 && actual_period > 0) {
			period_size = actual_period;
		}
		snd_pcm_uframes_t actual_buffer = 0;
		if (snd_pcm_hw_params_get_buffer_size(hw_params, &actual_buffer) >= 0 && actual_buffer > 0) {
			unsigned int rate = val ? val : (unsigned int)sample_rate;
			fprintf(stderr, "audio: buffer %lu frames (%u ms), period %lu frames (%u ms)\n",
					(unsigned long)actual_buffer, (unsigned int)((actual_buffer * 1000u) / rate),
					(unsigned long)period_size, (unsigned int)((period_size * 1000u) / rate));
		}
	}

	// The software parameters are always set, and the important one is the
	// start threshold: the stream only starts on a full buffer.
	//
	// Otherwise the stream starts on the first period written -- 25 ms of
	// reserve at 176.4 kHz -- and any system hiccup (the SD card contended with
	// a download, a cover being decoded) turns straight into an underrun. After
	// a seek or an unpause that becomes a "Broken pipe" on every write: the
	// stream restarts with a single period queued and dies before the next one.
	// Starting on a full buffer gives every start and restart the whole buffer
	// as reserve.
	{
		snd_pcm_uframes_t buffer_frames = 0;
		snd_pcm_hw_params_get_buffer_size(hw_params, &buffer_frames);

		snd_pcm_sw_params_t *sw_params;
		snd_pcm_sw_params_alloca(&sw_params);
		if (snd_pcm_sw_params_current(pcm_handle, sw_params) >= 0) {
			snd_pcm_sw_params_set_avail_min(pcm_handle, sw_params, period_size);
			// Start threshold = the whole buffer (see above).
			if (buffer_frames > 0) {
				snd_pcm_sw_params_set_start_threshold(pcm_handle, sw_params, buffer_frames);
			}
			snd_pcm_sw_params(pcm_handle, sw_params);
		}
	}

	log_pcm_chain(pcm_handle);

	*period_size_out = period_size;
	pcm_device_open = true;
	return pcm_handle;
}

// Every PCM this player opens goes through here, so that audio_quiet_ms() knows
// when one is being opened and when the last one was.
static snd_pcm_t *open_pcm_device(int channels, int sample_rate, int bits_per_sample, snd_pcm_uframes_t *period_size_out) {
	pcm_opening = true;
	last_open_err = 0;
	snd_pcm_t *pcm = open_pcm_device_now(channels, sample_rate, bits_per_sample, period_size_out);
	if (!pcm && last_open_err == 0) {
		last_open_err = -EIO;
	}
	quiet_since_ms = log_ms();
	pcm_opening = false;
	return pcm;
}

// The end of a track start, as audio_quiet_ms() counts it: the PCM is open, or
// the attempt is over.
static void track_started(void) {
	if (track_starting) {
		quiet_since_ms = log_ms();
		track_starting = false;
	}
}

// A line every HEALTH_PERIOD_MS while a track plays: the share of the core the
// playback thread used, how many writes needed recovering, and MemAvailable.
// While a stream plays the thread runs SCHED_RR on a single core, so whatever
// it uses is taken from the interface first.
//
// A minute, not ten seconds. At ten the two lines were a seventh of the whole
// log on the card -- 360 lines an hour of listening, each one a read of
// /proc/stat, the interface's stat and /proc/meminfo on top of the card write
// -- and a figure averaged over a minute is the steadier of the two anyway.
// A window broken by a pause is still thrown away rather than reported.
#define HEALTH_PERIOD_MS 60000

static volatile unsigned write_recoveries; // counted in pcm_write_recover()

// Where the thread's time goes inside a turn: reading and decoding the file,
// the effects chain, and handing the block to ALSA (alsa-lib and the kernel).
typedef enum { STAGE_READ, STAGE_EFFECTS, STAGE_WRITE, STAGE_COUNT } stage_t;

typedef struct {
	long at_ms;
	int64_t cpu_ns;
	unsigned recoveries;
	int64_t stage_ns[STAGE_COUNT];
	int64_t lap_ns;
	// Clock ticks from /proc, -1 when unreadable: the whole system (total and
	// idle), the interface thread, and bluealsa while the output is Bluetooth.
	long long sys_total, sys_idle, ui_ticks, bluealsa_ticks;
} health_t;

static int64_t thread_cpu_ns(void) {
	struct timespec ts;
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) {
		return 0;
	}
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static long mem_available_kb(void) {
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) {
		return -1;
	}
	char line[96];
	long kb = -1;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) {
			break;
		}
	}
	fclose(f);
	return kb;
}

// "cpu" line of /proc/stat: all ticks, and idle plus iowait.
static bool system_ticks(long long *total, long long *idle) {
	FILE *f = fopen("/proc/stat", "r");
	if (!f) {
		return false;
	}
	unsigned long long v[8] = {0};
	int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
				   &v[7]);
	fclose(f);
	if (n < 4) {
		return false;
	}
	*total = 0;
	for (int i = 0; i < 8; i++) {
		*total += (long long)v[i];
	}
	*idle = (long long)(v[3] + v[4]);
	return true;
}

// utime + stime of a /proc/.../stat file, -1 when unreadable. The fields
// after the command name, which can hold spaces, are counted from the last ')'.
static long long stat_ticks(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	char line[512];
	char *ok = fgets(line, sizeof(line), f);
	fclose(f);
	char *p = ok ? strrchr(line, ')') : NULL;
	if (!p) {
		return -1;
	}
	unsigned long long utime = 0, stime = 0;
	// state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime
	if (sscanf(p + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime, &stime) != 2) {
		return -1;
	}
	return (long long)(utime + stime);
}

// bluealsa's pid, looked up in /proc and kept while its comm still matches.
static int bluealsa_pid;

static bool pid_is_bluealsa(int pid) {
	char path[48], comm[32] = {0};
	snprintf(path, sizeof(path), "/proc/%d/comm", pid);
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	bool ok = fgets(comm, sizeof(comm), f) != NULL;
	fclose(f);
	return ok && strncmp(comm, "bluealsa\n", 9) == 0;
}

static long long bluealsa_ticks(void) {
	if (bluealsa_pid <= 0 || !pid_is_bluealsa(bluealsa_pid)) {
		bluealsa_pid = 0;
		DIR *dir = opendir("/proc");
		if (!dir) {
			return -1;
		}
		struct dirent *de;
		while ((de = readdir(dir)) != NULL) {
			int pid = atoi(de->d_name);
			if (pid > 0 && pid_is_bluealsa(pid)) {
				bluealsa_pid = pid;
				break;
			}
		}
		closedir(dir);
		if (bluealsa_pid <= 0) {
			return -1;
		}
	}
	char path[48];
	snprintf(path, sizeof(path), "/proc/%d/stat", bluealsa_pid);
	return stat_ticks(path);
}

// The interface runs on the process's main thread, whose tid is the pid.
static long long interface_ticks(void) {
	char path[64];
	snprintf(path, sizeof(path), "/proc/self/task/%d/stat", (int)getpid());
	return stat_ticks(path);
}

static void health_sample_system(health_t *h) {
	if (!system_ticks(&h->sys_total, &h->sys_idle)) {
		h->sys_total = h->sys_idle = -1;
	}
	h->ui_ticks = interface_ticks();
	h->bluealsa_ticks = output_is_bluetooth() ? bluealsa_ticks() : -1;
}

// Percent of the whole machine between two readings, -1 when either is missing.
static int ticks_percent(long long before, long long after, long long total) {
	if (before < 0 || after < 0 || total <= 0) {
		return -1;
	}
	return (int)((after - before) * 100 / total);
}

static void health_start(health_t *h) {
	health_sample_system(h);
	h->at_ms = log_ms();
	h->cpu_ns = thread_cpu_ns();
	h->recoveries = write_recoveries;
	memset(h->stage_ns, 0, sizeof(h->stage_ns));
	h->lap_ns = h->cpu_ns;
}

// health_mark() at the top of a turn, then health_lap() after each stage.
static void health_mark(health_t *h) { h->lap_ns = thread_cpu_ns(); }

static void health_lap(health_t *h, stage_t stage) {
	int64_t now = thread_cpu_ns();
	h->stage_ns[stage] += now - h->lap_ns;
	h->lap_ns = now;
}

// Tenths of a percent of the core over `span_ms`.
static int health_permille(int64_t ns, long span_ms) { return (int)(ns / 1000 / span_ms); }

// Called after every write. A window stretched by a pause says nothing about
// playing, so it is restarted instead of reported.
static void health_tick(health_t *h, int rate, int out_rate, int bits) {
	long now = log_ms();
	long span = now - h->at_ms;
	if (span < HEALTH_PERIOD_MS) {
		return;
	}
	if (span < 2 * HEALTH_PERIOD_MS) {
		int64_t used = thread_cpu_ns() - h->cpu_ns;
		int all = health_permille(used, span);
		int rd = health_permille(h->stage_ns[STAGE_READ], span);
		int fx = health_permille(h->stage_ns[STAGE_EFFECTS], span);
		int wr = health_permille(h->stage_ns[STAGE_WRITE], span);
		char to[32] = "";
		if (out_rate != rate) {
			snprintf(to, sizeof(to), " (%d Hz out)", out_rate);
		}
		fprintf(stderr,
				"audio[%ld]: %ld s at %d Hz%s / %d bit: playback thread %d.%d%% of the core (read+decode %d.%d%%, "
				"effects %d.%d%%, ALSA write %d.%d%%), %u write recoveries, MemAvailable %ld kB\n",
				now, span / 1000, rate, to, bits, all / 10, all % 10, rd / 10, rd % 10, fx / 10, fx % 10, wr / 10,
				wr % 10, write_recoveries - h->recoveries, mem_available_kb());

		long long total = 0, idle = 0;
		if (h->sys_total >= 0 && system_ticks(&total, &idle) && total > h->sys_total) {
			long long dt = total - h->sys_total;
			int idle_pct = (int)((idle - h->sys_idle) * 100 / dt);
			int ui_pct = ticks_percent(h->ui_ticks, interface_ticks(), dt);
			int ba_pct = h->bluealsa_ticks >= 0 ? ticks_percent(h->bluealsa_ticks, bluealsa_ticks(), dt) : -1;
			char ba[32] = "";
			if (ba_pct >= 0) {
				snprintf(ba, sizeof(ba), ", bluealsa %d%%", ba_pct);
			}
			fprintf(stderr, "audio[%ld]: the core over the same span: idle %d%%, interface %d%%%s\n", now, idle_pct,
					ui_pct, ba);
		}
	}
	health_start(h);
}

// Writes one buffer of frames, recovering from the errors a real device
// throws instead of killing the track on the first one:
//
//   -EPIPE    underrun -> prepare and rewrite
//   -ESTRPIPE the device was suspended -> resume (then prepare) and rewrite
//   -EIO      the stream went bad underneath -- on this hardware an
//             output-route switch (headphones in/out, line-out toggle) can
//             invalidate the open stream -- so the device is closed and
//             reopened from scratch, then the write retried
//
// Returns the frames written, or negative once recovery has failed several
// times in a row; *pcm may be replaced (or NULL) on return.
static bool playback_reader_should_abort(void);

// How long a write to a Bluetooth PCM may make no progress before the link is
// taken for gone. The same two seconds the wait below already used for a card
// with a stopped clock: long enough for a hiccup on the air, short enough that
// the queue does not stand still.
#define BT_WRITE_STALL_MS 2000

// After a seek on a Bluetooth PCM: one line, a second and a half later, saying
// how much audio is queued.
//
// It separates the two possible explanations of stuttering after a seek. A full
// queue (around 700 ms) means the player is keeping up and the trouble is on
// the air or in the sink; a queue near zero means it is not keeping up, and the
// place to look is here. From outside the two look identical.
//
// Bluetooth only, after a seek only, once per seek.
#define BT_SEEK_PROBE_MS 1500
static long bt_seek_probe_at;

// How long a track takes to start COMING OUT, on the headphones.
//
// Two different causes look identical from outside, and this tells them apart:
//
//   * the stream starts and simply is not heard -- the link is the place to
//     look, and this line says RUNNING after a few hundred ms;
//   * the stream never starts, because the start threshold is the whole buffer
//     (some seven hundred milliseconds of audio) and the CPU does not fill it
//     fast enough -- and this line says PREPARED, and for how long.
//
// One line per track, bluealsa only, like the seek probe. RUNNING says only
// that the plugin started, not that anything is consuming: if the A2DP stream
// is not really streaming, the frames sit in bluealsa's queue. A second line
// shortly after carries the queue depth, and a queue that stays full means
// nothing on the other side is taking anything away.
#define BT_START_GIVEUP_MS 5000
#define BT_START_FLOW_MS 2000
static long bt_start_at;
static long bt_start_flow_at;
static bool bt_start_said;
static long long bt_start_frames;
static int bt_start_rate;

// The A2DP stream state, as bluez has it, appended to the lines below.
//
// Everything this file can see says the same thing whether the headphones are
// playing or not: the writes go through, the queue drains at the right rate,
// the encoder keeps up. Only bluez knows whether the stream those frames are
// encoded into is active, and a stream that is idle or pending renders nothing
// however perfectly it is fed. Asked for at the start and read two seconds
// later, off the worker, because the playback thread does not wait on D-Bus.
static void bt_streams_text(char *out, size_t size) {
	char streams[192];
	int age = -1;
	if (bluetooth_a2dp_streams(streams, (int)sizeof(streams), &age) && streams[0]) {
		snprintf(out, size, "; A2DP stream %s (read %d ms ago)", streams, age);
	} else {
		out[0] = '\0';
	}
}

static void bt_start_probe_arm(snd_pcm_t *pcm) {
	bt_start_at = (pcm && pcm_is_bluetooth(pcm)) ? log_ms() : 0;
	bt_start_flow_at = 0;
	bt_start_said = false;
	bt_start_frames = 0;
	bt_start_rate = 0;
	if (bt_start_at) {
		bluetooth_request_a2dp_streams();
	}
}

static void bt_start_probe_note(snd_pcm_t *pcm, snd_pcm_sframes_t written, int rate) {
	if (!bt_start_at || !pcm) {
		return;
	}
	if (written > 0) {
		bt_start_frames += written;
	}
	bt_start_rate = rate;
	long now = log_ms();

	if (bt_start_said) {
		if (bt_start_flow_at && now >= bt_start_flow_at) {
			long elapsed = now - bt_start_at;
			bt_start_flow_at = 0;
			snd_pcm_sframes_t queued = 0;
			if (snd_pcm_delay(pcm, &queued) < 0) {
				queued = -1;
			}
			// Written minus still queued is what the encoder has taken. Against
			// the frames the elapsed time is worth, it says in one number
			// whether anything is consuming the stream.
			long long taken = bt_start_frames - (queued > 0 ? queued : 0);
			long long realtime = rate > 0 ? (long long)((double)rate * elapsed / 1000.0) : 0;
			char streams[224];
			bt_streams_text(streams, sizeof(streams));
			fprintf(stderr,
					"audio[%ld]: %ld ms after the start: %lld frames written, %ld queued, %lld taken out of %lld of real "
					"time%s\n",
					now, elapsed, bt_start_frames, (long)queued, taken, realtime, streams);
		}
		return;
	}

	long elapsed = now - bt_start_at;
	snd_pcm_state_t state = snd_pcm_state(pcm);
	if (state == SND_PCM_STATE_RUNNING) {
		bt_start_said = true;
		bt_start_flow_at = now + BT_START_FLOW_MS;
		// Asked again here so the line below reads an answer from after the
		// stream started, not one from before the PCM was even open.
		bluetooth_request_a2dp_streams();
		fprintf(stderr, "audio[%ld]: the track started coming out after %ld ms (%lld frames written)\n", now, elapsed,
				bt_start_frames);
		return;
	}
	if (elapsed >= BT_START_GIVEUP_MS) {
		bt_start_said = true;
		char streams[224];
		bt_streams_text(streams, sizeof(streams));
		fprintf(stderr, "audio[%ld]: after %ld ms the track has still not come out (state %s, %lld frames written)%s\n",
				now, elapsed, snd_pcm_state_name(state), bt_start_frames, streams);
	}
}

static void bt_seek_probe_arm(snd_pcm_t *pcm) {
	bt_seek_probe_at = (pcm && pcm_is_bluetooth(pcm)) ? log_ms() + BT_SEEK_PROBE_MS : 0;
}

static void bt_seek_probe_cancel(void) { bt_seek_probe_at = 0; }

static void bt_seek_probe_report(snd_pcm_t *pcm) {
	if (!bt_seek_probe_at || log_ms() < bt_seek_probe_at) {
		return;
	}
	bt_seek_probe_at = 0;
	if (!pcm) {
		return;
	}
	snd_pcm_sframes_t queued = 0;
	if (snd_pcm_delay(pcm, &queued) < 0) {
		queued = -1;
	}
	fprintf(stderr, "audio[%ld]: %d ms after the seek the Bluetooth queue is %ld frames (state %s)\n", log_ms(),
			BT_SEEK_PROBE_MS, (long)queued, snd_pcm_state_name(snd_pcm_state(pcm)));
}

// Waits up to `timeout_ms` for room in a bluealsa PCM: 1 with room, 0 without,
// negative on an error.
//
// Not snd_pcm_wait(). Its loop in alsa-lib calls poll() again, with the full
// timeout, whenever poll() reports a descriptor ready but the plugin's revents
// hold neither POLLIN nor POLLOUT, and never sleeps in between. The bluealsa
// plugin polls an event fd and its D-Bus connection alongside the PCM, and
// either can be readable while the PCM has no room: the caller, at real-time
// priority on the one core, then spins inside alsa-lib and nothing below it
// runs again -- not the interface, not the card's I/O, not the kernel threads.
// Here one poll() is made per call, and a readiness that is not room is
// answered with a sleep.
#define BT_POLL_FDS_MAX 8
#define BT_SPURIOUS_SLEEP_US (5 * 1000)

static int pcm_wait_bluetooth(snd_pcm_t *pcm, int timeout_ms) {
	struct pollfd pfd[BT_POLL_FDS_MAX];
	int count = snd_pcm_poll_descriptors_count(pcm);
	if (count <= 0 || count > BT_POLL_FDS_MAX) {
		usleep(BT_SPURIOUS_SLEEP_US);
		return 0;
	}
	count = snd_pcm_poll_descriptors(pcm, pfd, (unsigned)count);
	if (count <= 0) {
		usleep(BT_SPURIOUS_SLEEP_US);
		return 0;
	}
	int ready = poll(pfd, (nfds_t)count, timeout_ms);
	if (ready < 0) {
		return errno == EINTR ? 0 : -errno;
	}
	if (ready == 0) {
		return 0;
	}
	unsigned short revents = 0;
	int err = snd_pcm_poll_descriptors_revents(pcm, pfd, (unsigned)count, &revents);
	if (err < 0) {
		return err;
	}
	if (revents & (POLLERR | POLLNVAL)) {
		return -EIO;
	}
	if (revents & POLLOUT) {
		return 1;
	}
	usleep(BT_SPURIOUS_SLEEP_US);
	return 0;
}

// Writes a whole period to a bluealsa PCM without ever blocking indefinitely.
//
// snd_pcm_writei() on a blocking handle waits for room, and over A2DP that room
// can stop coming for good -- there is no clock consuming the buffer, only a
// link that may have gone away, and the playback thread parks in poll() with a
// play request pending and no track ever starting. The wait above catches the
// case where the stream is RUNNING and the poll says nothing; it does not catch
// the write itself deciding to wait.
//
// So the Bluetooth PCM is opened non-blocking (open_pcm_device) and the waiting
// is done here, in slices, with a deadline. A partial write is not a loss here:
// the rest of the period is written on the next turn round the loop.
static snd_pcm_sframes_t pcm_write_bluetooth(snd_pcm_t *pcm, const void *buf, snd_pcm_uframes_t frames,
											 int frame_bytes) {
	const char *from = (const char *)buf;
	snd_pcm_uframes_t done = 0;
	long last_progress_ms = log_ms();

	while (done < frames) {
		snd_pcm_sframes_t n = snd_pcm_writei(pcm, from + done * (size_t)frame_bytes, frames - done);
		if (n > 0) {
			done += (snd_pcm_uframes_t)n;
			last_progress_ms = log_ms();
			continue;
		}
		if (n < 0 && n != -EAGAIN) {
			// A real error (-EPIPE, -ESTRPIPE, -ENODEV...): the caller knows
			// how to recover from each, and knows nothing about what has
			// already gone out, so anything written so far is reported first.
			return done > 0 ? (snd_pcm_sframes_t)done : n;
		}

		// No room -- either -EAGAIN, or a write that took nothing and said so
		// without complaining. A command waiting is a better reason to come
		// back than any deadline: whoever pressed stop or next is not waiting
		// for this.
		if (playback_reader_should_abort()) {
			return done > 0 ? (snd_pcm_sframes_t)done : -EINTR;
		}

		// The deadline is read off the clock, and not from how the wait below
		// came back. Counting only the polls that TIMED OUT would leave one way
		// through with no deadline at all: a poll that keeps saying "there is
		// room" while the write keeps saying there is not. bluealsa answers
		// exactly that way -- its poll follows the FIFO, the write follows the
		// transport, and the two disagree the moment the link stops draining --
		// so an indefinite block would simply become an indefinite spin.
		long since = log_ms() - last_progress_ms;
		if (since < 0) {
			since = 0; // the millisecond counter wrapped; start the window again
			last_progress_ms = log_ms();
		}
		if (since >= BT_WRITE_STALL_MS) {
			fprintf(stderr, "audio[%ld]: the Bluetooth PCM has taken nothing for %ld ms: calling it lost\n",
					log_ms(), since);
			return done > 0 ? (snd_pcm_sframes_t)done : -EIO;
		}

		int ready = pcm_wait_bluetooth(pcm, 100);
		if (ready < 0) {
			return done > 0 ? (snd_pcm_sframes_t)done : (snd_pcm_sframes_t)ready;
		}
		if (ready > 0) {
			// Told there is room by a poll the write has just contradicted.
			// Ten milliseconds, so a disagreement that lasts becomes a wait and
			// not a thread spinning on one core against a device that is not
			// listening.
			usleep(10 * 1000);
		}
	}
	return (snd_pcm_sframes_t)done;
}

// The playback thread is real-time only while a stream is playing.
//
// Real-time is what keeps a playing stream fed whatever the interface is
// doing. But a stream that is not running yet -- a track starting, a seek, a
// resume, a recovery -- is filled from empty in one go: the start threshold is
// the whole buffer, so nothing is written to the card until it is full, and
// no write blocks on the way. At 192 kHz that is most of a second of audio
// decoded, run through the effects and written without a pause, on the one
// core, ahead of the interface -- right as the now-playing screen slides in.
// Opening the device is worse: the codec's power-up and reset are busy waits
// in the driver (more than a hundred milliseconds of them), and a real-time
// caller holds the core through every one.
//
// Nothing is playing in either case, so there is nothing for the priority to
// protect: the thread steps down to the interface's level until the stream
// starts, and goes back up from the first write that finds it running.
#define PLAYBACK_RT_PRIORITY 10

static bool playback_rt;

static void playback_set_rt(bool on) {
	if (on == playback_rt) {
		return;
	}
	struct sched_param param = {.sched_priority = on ? PLAYBACK_RT_PRIORITY : 0};
	if (pthread_setschedparam(pthread_self(), on ? SCHED_RR : SCHED_OTHER, &param) == 0) {
		playback_rt = on;
	}
}

// The stream the thread's priority follows, its buffer, and how much has been
// written to it since it last stopped playing.
static snd_pcm_t *follow_pcm;
static snd_pcm_uframes_t follow_buffer;
static snd_pcm_uframes_t follow_filled;
static long follow_started_ms; // log_ms() when it was first seen started, 0 = not started

// How long after a start the thread stays at the interface's level. The card
// is started by the driver on a kernel worker at normal priority (soc_aic
// start_work): codecs unmuted, muted again one after the other over i2c, clock
// stopped and restarted, unmuted. A real-time thread decoding the next chunk
// right then held that worker for 30-43 ms in 9 of 32 starts at 192 kHz, with
// one AK4493 unmuted and its ramp (21 ms at 192 kHz) at full level when the
// clock was cut: a very loud pop. Uncontended the sequence takes about 7 ms,
// and the buffer is full here.
#define PLAYBACK_START_SETTLE_MS 100

static bool pcm_playing(snd_pcm_t *pcm) {
	snd_pcm_state_t st = snd_pcm_state(pcm);
	return st == SND_PCM_STATE_RUNNING || st == SND_PCM_STATE_DRAINING;
}

// ---------------------------------------------------------------------------
// The tick of a stream cut in the middle of the music
//
// The driver's stop (soc_aic stop_work) mutes the two AK4493s over i2c and
// stops their clock about 5 ms later, while the codecs' own ramp down takes up
// to 4080/fs (93 ms at 44.1 kHz): the DAC stops at almost the level it was
// playing, and a pause, a seek or a skip ticks. The stock player cuts the same
// way -- an XRUN ioctl, with its fade off by default -- and ticks the same.
//
// So before such a cut the DAC is taken down with its own attenuator and the
// cut lands on silence. The samples are never touched. It comes back up, with
// the same ramp, once the next stream on the card runs.
// ---------------------------------------------------------------------------
static volatile bool dac_ramped; // taken to mute here, not yet brought back

static void dac_ramp_before_cut(snd_pcm_t *pcm) {
	if (!pcm || !pcm_is_card(pcm) || !pcm_playing(pcm)) {
		return;
	}
	// DoP: the codec's ramp counts the DSD clock, not the carrier's rate --
	// timed as DSD64's 44.1 kHz, the slowest it can be.
	unsigned rate = get_dac_dop() == 1 ? 44100 : 0;
	snd_pcm_hw_params_t *hw;
	snd_pcm_hw_params_alloca(&hw);
	if (!rate && snd_pcm_hw_params_current(pcm, hw) == 0) {
		snd_pcm_hw_params_get_rate(hw, &rate, NULL);
	}
	if (alsa_mute_ramp((int)rate)) {
		dac_ramped = true;
	}
}

static void dac_ramp_release(snd_pcm_t *pcm) {
	if (dac_ramped && pcm_is_card(pcm) && pcm_playing(pcm)) {
		dac_ramped = false;
		alsa_suspend_restore();
	}
}

// Real-time while `pcm` plays, the interface's level otherwise. NULL is a
// device about to be opened. A whole buffer written also counts as playing,
// whatever the state says: the stream starts on a full buffer, so by then it
// is either running or about to, and a PCM that never reports it does not
// leave the thread below the interface for the length of a track.
static void playback_follow(snd_pcm_t *pcm) {
	if (pcm != follow_pcm) {
		follow_pcm = pcm;
		follow_filled = 0;
		follow_buffer = 0;
		follow_started_ms = 0;
		snd_pcm_uframes_t period = 0;
		if (pcm && snd_pcm_get_params(pcm, &follow_buffer, &period) < 0) {
			follow_buffer = 0;
		}
		levelmeter_set_buffer_frames((unsigned)follow_buffer);
	}
	if (!pcm) {
		playback_set_rt(false);
		return;
	}
	bool playing = pcm_playing(pcm);
	if (playing) {
		follow_filled = 0;
		dac_ramp_release(pcm);
	}
	if (!playing && !(follow_buffer > 0 && follow_filled >= follow_buffer)) {
		follow_started_ms = 0;
		playback_set_rt(false);
		return;
	}
	if (!follow_started_ms) {
		follow_started_ms = log_ms();
	}
	playback_set_rt(log_ms() - follow_started_ms >= PLAYBACK_START_SETTLE_MS);
}

// After a write of `frames` to `pcm`.
static void playback_wrote(snd_pcm_t *pcm, snd_pcm_uframes_t frames) {
	if (pcm == follow_pcm && !pcm_playing(pcm)) {
		follow_filled += frames;
	}
	playback_follow(pcm);
}

// Wakes the playback thread out of its wait for room in the card's buffer,
// so that a pause, a seek or a stop is acted on at once. Without it the wait
// ends only when a period frees, up to a period of music after the tap (some
// 90 ms at 44.1 kHz), and the codec's fade-out comes that much late.
static int playback_wake_fd = -1;

static void playback_wake(void) {
	uint64_t one = 1;
	if (playback_wake_fd >= 0 && write(playback_wake_fd, &one, sizeof(one)) < 0) {
		// Full counter: a wake-up is already pending, which is all this asks.
	}
}

// The wait between two looks at the commands while paused with the device
// handed back. Not a fixed nap: every command that can end a pause (resume,
// play, seek, stop, a request for the device) rings playback_wake() and ends it
// at once. The second it lasts otherwise is only idle_amp_off()'s clock, which
// parks the amp on the second rather than on the fifth of one. It was a 200 ms
// usleep: five wake-ups a second, real-time thread and all, for as long as the
// player sat paused.
#define PAUSED_RELEASED_WAIT_MS 1000

static void paused_wait_released(void) {
	if (playback_wake_fd < 0) {
		usleep(200 * 1000); // no doorbell: look as often as before
		return;
	}
	struct pollfd p = {.fd = playback_wake_fd, .events = POLLIN};
	if (poll(&p, 1, PAUSED_RELEASED_WAIT_MS) > 0) {
		uint64_t count;
		if (read(playback_wake_fd, &count, sizeof(count)) < 0) {
			// Already drained: nothing was pending.
		}
	}
}

// snd_pcm_wait() that also returns, with 0, on playback_wake(). 1: room,
// 0: timeout or wake-up, < 0: an error, for the writei to report.
static int pcm_wait_or_wake(snd_pcm_t *pcm, int timeout_ms) {
	struct pollfd fds[9];
	int n = snd_pcm_poll_descriptors_count(pcm);

	if (playback_wake_fd < 0 || n <= 0 || n >= (int)(sizeof(fds) / sizeof(fds[0]))) {
		return snd_pcm_wait(pcm, timeout_ms);
	}
	n = snd_pcm_poll_descriptors(pcm, fds, (unsigned int)n);
	if (n <= 0) {
		return snd_pcm_wait(pcm, timeout_ms);
	}
	fds[n].fd = playback_wake_fd;
	fds[n].events = POLLIN;
	fds[n].revents = 0;

	int r = poll(fds, (nfds_t)n + 1, timeout_ms);
	if (r < 0) {
		return errno == EINTR ? 0 : -errno;
	}
	if (fds[n].revents & POLLIN) {
		uint64_t count;
		if (read(playback_wake_fd, &count, sizeof(count)) < 0) {
			// Already drained: nothing was pending.
		}
	}
	unsigned short revents = 0;
	if (snd_pcm_poll_descriptors_revents(pcm, fds, (unsigned int)n, &revents) < 0) {
		return -EIO;
	}
	if (revents & (POLLERR | POLLNVAL)) {
		return -EIO;
	}
	return (revents & POLLOUT) ? 1 : 0;
}

static snd_pcm_sframes_t pcm_write_recover(snd_pcm_t **pcm, const void *buf, snd_pcm_uframes_t frames, int channels,
										   int rate, int bits, snd_pcm_uframes_t *period) {
	for (int attempt = 0; attempt < 8; attempt++) {
		if (!*pcm) {
			return -EIO; // a previous recovery closed it and could not get it back
		}
		playback_follow(*pcm);

		// Never write blind to a RUNNING stream. The blocking writei waits for
		// room in the card's buffer; after a storm of underruns this hardware
		// can sit with a full buffer and a stopped clock, and that room never
		// frees again, leaving the thread inside ALSA out of reach of any
		// wake-up.
		//
		// But waiting only makes sense while the card is consuming: consumption
		// is what frees room, and a stopped clock is what the wait is meant to
		// detect. On a freshly PREPARED stream -- track start, post-seek,
		// post-underrun -- this driver's poll signals nothing until the stream
		// has started, so waiting there would declare a healthy device stuck and
		// close it. A write to a prepared stream cannot block: the room is there
		// by definition, and the write is what starts it.
		bool room = true;
		snd_pcm_state_t pcm_state = snd_pcm_state(*pcm);
		if (pcm_state == SND_PCM_STATE_RUNNING || pcm_state == SND_PCM_STATE_DRAINING) {
			room = false;
			bool bluetooth = pcm_is_bluetooth(*pcm);
			long give_up_at = log_ms() + 2000;
			while (!room && log_ms() < give_up_at) {
				if (playback_reader_should_abort()) {
					return -EINTR; // a command is pending: the main loop handles it
				}
				int r = bluetooth ? pcm_wait_bluetooth(*pcm, 100) : pcm_wait_or_wake(*pcm, 100);
				if (r != 0) {
					room = true; // ready (1) or error (<0): the writei will say which
				}
			}
		}

		snd_pcm_sframes_t written;
		if (!room) {
			fprintf(stderr, "audio[%ld]: buffer never free in 2 s: device wedged, rebuilding it\n", log_ms());
			written = -EIO; // skip the writei and go straight to the rebuild
		} else if (pcm_is_bluetooth(*pcm)) {
			written = pcm_write_bluetooth(*pcm, buf, frames, channels * (bits / 8));
		} else if (pcm_takes_s24(*pcm, bits)) {
			const void *s24 = s24_from_s32(&playback_s24, buf, (size_t)frames * (size_t)channels);
			written = s24 ? snd_pcm_writei(*pcm, s24, frames) : -ENOMEM;
		} else {
			written = snd_pcm_writei(*pcm, buf, frames);
		}
		if (written >= 0) {
			playback_wrote(*pcm, (snd_pcm_uframes_t)written);
			return written;
		}

		write_recoveries++;
		fprintf(stderr, "audio[%ld]: write error: %s (recovery %d)\n", log_ms(), snd_strerror((int)written),
				attempt + 1);

		if (written == -EPIPE) {
			snd_pcm_prepare(*pcm);
			continue;
		}
		if (written == -ESTRPIPE) {
			int r = -EAGAIN;
			for (int t = 0; r == -EAGAIN && t < 20; t++) {
				r = snd_pcm_resume(*pcm);
				if (r == -EAGAIN) {
					usleep(100 * 1000);
				}
			}
			if (r < 0) {
				snd_pcm_prepare(*pcm);
			}
			continue;
		}

		// -EIO and anything else: rebuild the device from scratch. The stall
		// that produces the EIO (a busy DMA, a route settling) needs a real
		// moment to clear, so the backoff grows with each attempt; reopening
		// immediately only earns another EIO.
		snd_pcm_close(*pcm);
		*pcm = NULL;
		playback_follow(NULL);
		usleep((attempt + 1) * 150 * 1000);
		// The route, not just the PCM. Reopening the stream without redoing the
		// codec routing leaves the DAC in a state where high-rate streams never
		// start again -- 96 kHz tracks die while 48 kHz ones still play -- and
		// only a reboot clears it. Same move as pcm_reroute, for the same
		// reason.
		auto_set_output();
		*pcm = open_pcm_device(channels, rate, bits, period);
		if (!*pcm) {
			return written; // it really is gone
		}
		fprintf(stderr, "audio[%ld]: pcm reopened (recovery %d)\n", log_ms(), attempt + 1);
	}
	return -EIO;
}

// ---------------------------------------------------------------------------
// USB: re-scaling what is already queued when the volume moves
//
// On the port the volume is taken off the samples before they are queued, and
// up to USB_BUFFER_MAX_MS of them are queued ahead, so a change was heard only
// once that had played. A copy of the queued samples is kept from before the
// gain; at a change the stream is rewound -- all but one period, which may
// already be on its way to the device -- and those frames written again at
// the new gain.
// ---------------------------------------------------------------------------

typedef struct {
	uint8_t *data;	   // the frames, before the gain, as a ring
	size_t cap;		   // frames it holds
	size_t head;	   // where the next frame goes
	size_t fill;	   // frames in it
	size_t frame_bytes;
	uint8_t *scratch; // one chunk, re-scaled on its way back out
	size_t scratch_frames;
	unsigned gen; // the gain the queue was written at
} requeue_t;

static bool requeue_init(requeue_t *q, size_t cap, size_t frame_bytes, size_t scratch_frames) {
	memset(q, 0, sizeof(*q));
	q->data = malloc(cap * frame_bytes);
	q->scratch = malloc(scratch_frames * frame_bytes);
	if (!q->data || !q->scratch) {
		free(q->data);
		free(q->scratch);
		memset(q, 0, sizeof(*q));
		return false;
	}
	q->cap = cap;
	q->frame_bytes = frame_bytes;
	q->scratch_frames = scratch_frames;
	q->gen = swvolume_generation();
	return true;
}

static void requeue_free(requeue_t *q) {
	free(q->data);
	free(q->scratch);
	memset(q, 0, sizeof(*q));
}

// After a write that took all of `frames`; anything else and the copy no
// longer matches the queue, so it is emptied (requeue_keep(q, NULL, 0)).
static void requeue_keep(requeue_t *q, const void *buf, size_t frames) {
	if (!q->cap) {
		return;
	}
	if (!buf) {
		q->fill = 0;
		return;
	}
	const uint8_t *from = buf;
	if (frames > q->cap) {
		from += (frames - q->cap) * q->frame_bytes;
		frames = q->cap;
	}
	size_t first = q->cap - q->head < frames ? q->cap - q->head : frames;
	memcpy(q->data + q->head * q->frame_bytes, from, first * q->frame_bytes);
	memcpy(q->data, from + first * q->frame_bytes, (frames - first) * q->frame_bytes);
	q->head = (q->head + frames) % q->cap;
	q->fill = q->fill + frames < q->cap ? q->fill + frames : q->cap;
}

static void requeue_rescale(requeue_t *q, snd_pcm_t **pcm, int channels, int rate, int bits, snd_pcm_uframes_t *period) {
	unsigned gen = swvolume_generation();
	if (!q->cap || gen == q->gen) {
		return;
	}
	q->gen = gen;
	if (!q->fill || !*pcm || snd_pcm_state(*pcm) != SND_PCM_STATE_RUNNING) {
		return;
	}
	snd_pcm_sframes_t can = snd_pcm_rewindable(*pcm);
	if (can <= (snd_pcm_sframes_t)*period) {
		return;
	}
	snd_pcm_uframes_t want = (snd_pcm_uframes_t)can - *period;
	if (want > q->fill) {
		want = q->fill;
	}
	snd_pcm_sframes_t back = snd_pcm_rewind(*pcm, want);
	if (back <= 0) {
		return;
	}
	size_t at = (q->head + q->cap - (size_t)back) % q->cap;
	size_t done = 0;
	while (done < (size_t)back) {
		size_t take = (size_t)back - done < q->scratch_frames ? (size_t)back - done : q->scratch_frames;
		size_t first = q->cap - at < take ? q->cap - at : take;
		memcpy(q->scratch, q->data + at * q->frame_bytes, first * q->frame_bytes);
		memcpy(q->scratch + first * q->frame_bytes, q->data, (take - first) * q->frame_bytes);
		if (swvolume_active()) {
			if (bits == 32) {
				swvolume_apply_s32((int32_t *)q->scratch, (int)(take * (size_t)channels));
			} else {
				swvolume_apply_s16((short *)q->scratch, (int)(take * (size_t)channels));
			}
		}
		if (pcm_write_recover(pcm, q->scratch, take, channels, rate, bits, period) != (snd_pcm_sframes_t)take) {
			q->fill = 0; // the queue is short of what the copy says: start the copy again
			return;
		}
		at = (at + take) % q->cap;
		done += take;
	}
}



// Mid-track jack change (headphones in/out while playing): the stock engine
// sets a reinit flag and its device layer reopens on the new route; this is
// the same move between two writes -- the only moment no stream is live, so
// the codec switch cannot stall anything.
static bool pcm_reroute(snd_pcm_t **pcm, int channels, int rate, int bits, snd_pcm_uframes_t *period) {
	fprintf(stderr, "audio[%ld]: jack changed, rerouting\n", log_ms());
	if (*pcm) {
		dac_ramp_before_cut(*pcm);
		snd_pcm_drop(*pcm);
		snd_pcm_close(*pcm);
		*pcm = NULL;
	}
	playback_follow(NULL);
	usleep(150 * 1000); // let the switch settle before the codec is poked
	auto_set_output();
	*pcm = open_pcm_device(channels, rate, bits, period);
	return *pcm != NULL;
}

// A write failure that survived every in-place recovery still gets one clean
// retry of the whole track: close everything, pause, and play the same file
// again from the top, so a failed start costs a half-second hiccup rather than
// silence. One retry only, then a real stop.
static bool retry_track_once(const char *filepath, const char *reason) {
	pthread_mutex_lock(&audio_mutex);
	if (write_fail_retry_used) {
		pthread_mutex_unlock(&audio_mutex);
		return false;
	}
	write_fail_retry_used = true;
	strncpy(current_filepath, filepath, sizeof(current_filepath) - 1);
	current_filepath[sizeof(current_filepath) - 1] = '\0';
	play_request = true;
	play_request_ms = log_ms();
	audio_command = AUDIO_CMD_PLAY;
	playback_status = AUDIO_STATUS_PLAYING;
	pthread_cond_signal(&audio_cond);
	playback_wake(); // and out of a pause, if it sits in one
	pthread_mutex_unlock(&audio_mutex);
	fprintf(stderr, "audio[%ld]: retrying '%s' (%s)\n", log_ms(), filepath, reason);
	return true;
}

// ---------------------------------------------------------------------------
// Rebuffering
//
// Only tracks that are played while still downloading need this. The rule is
// the usual one for anything playing off a network: play while there is enough
// ahead, and when too little is left, stop and wait until there is plenty
// again. The two thresholds differ on purpose -- stop at two seconds, resume
// at eight -- otherwise the wait would be re-entered on every buffer, which is
// stuttering by another name.
// ---------------------------------------------------------------------------

#define REBUFFER_LOW_SECS 2.0	// below this much ahead, stop
#define REBUFFER_HIGH_SECS 8.0	// and do not resume before this much is back
#define REBUFFER_MAX_WAIT_MS 60000

// Seconds of music ahead of the play position, or -1 when the file is no
// longer growing (and there is then nothing to wait for).
static double growing_headroom_secs(decoder_t *dec, int sample_rate, uint64_t at_frame) {
	uint64_t reachable = decoder_seekable_limit(dec);
	if (reachable == 0 || sample_rate <= 0) {
		return -1.0;
	}
	if (reachable <= at_frame) {
		return 0.0;
	}
	return (double)(reachable - at_frame) / (double)sample_rate;
}

static void rebuffer_if_starved(decoder_t *dec, int sample_rate, uint64_t at_frame) {
	double head = growing_headroom_secs(dec, sample_rate, at_frame);
	if (head < 0 || head >= REBUFFER_LOW_SECS) {
		return;
	}

	fprintf(stderr, "audio[%ld]: buffer run out (%.1f s ahead): waiting\n", log_ms(), head);

	int waited_ms = 0;
	while (waited_ms < REBUFFER_MAX_WAIT_MS) {
		// A new command -- stop, pause, another track -- outranks the wait:
		// leave at once and let the main loop deal with it.
		pthread_mutex_lock(&audio_mutex);
		bool leave = audio_command != AUDIO_CMD_PLAY || play_request || stop_thread || seek_request;
		pthread_mutex_unlock(&audio_mutex);
		if (leave) {
			return;
		}

		// The download finished (or died): nothing more is coming, carry on
		// with what is there.
		if (!decoder_is_growing(dec)) {
			return;
		}

		head = growing_headroom_secs(dec, sample_rate, at_frame);
		if (head < 0 || head >= REBUFFER_HIGH_SECS) {
			fprintf(stderr, "audio[%ld]: resuming with %.1f s buffered\n", log_ms(), head);
			return;
		}

		usleep(100 * 1000);
		waited_ms += 100;
	}

	fprintf(stderr, "audio[%ld]: a minute of waiting and the buffer will not fill: carrying on anyway\n", log_ms());
}

// The safety net for when the estimate above is wrong.
//
// The check above works from a ratio (bytes downloaded over total bytes,
// carried onto frames) and is only an estimate: it can report headroom while
// the decoder is already reading at the edge of the download, where reads
// trickle and the card underruns continuously.
//
// A short read from a growing file is not an estimate: it is proof that the
// edge has been reached. So this waits on real bytes: it takes where the
// download has got to, computes the file's rate (total bytes over total
// duration, both known), and does not resume before REBUFFER_HIGH_SECS
// seconds of file are in hand beyond that point.
static void rebuffer_after_short_read(decoder_t *dec, int sample_rate, uint64_t at_frame) {
	long done = 0, total = 0;
	if (!decoder_grow_span(dec, &done, &total) || total <= 0) {
		return; // not growing (any more): nothing to wait for
	}

	// The flag can be stale: raised while playing at the edge, followed by a
	// backwards seek. When there is already enough ahead of the play position
	// there is no starvation to wait out, and waiting anyway would leave the
	// music stuck at the seek target with the data already on disk.
	double head = growing_headroom_secs(dec, sample_rate, at_frame);
	if (head >= REBUFFER_LOW_SECS) {
		return;
	}
	uint64_t total_frames = decoder_total_pcm_frames(dec);
	if (total_frames == 0 || sample_rate <= 0) {
		return;
	}
	double total_secs = (double)total_frames / (double)sample_rate;
	if (total_secs <= 0) {
		return;
	}
	double file_rate = (double)total / total_secs; // file bytes per second of music

	long target = done + (long)(file_rate * REBUFFER_HIGH_SECS);
	if (target > total) {
		target = total;
	}

	fprintf(stderr, "audio[%ld]: reading at the edge of the download (%ld KB): waiting for the buffer\n", log_ms(),
			done / 1024);

	int waited_ms = 0;
	while (waited_ms < REBUFFER_MAX_WAIT_MS) {
		pthread_mutex_lock(&audio_mutex);
		bool leave = audio_command != AUDIO_CMD_PLAY || play_request || stop_thread || seek_request;
		pthread_mutex_unlock(&audio_mutex);
		if (leave) {
			return; // a command outranks the wait
		}

		if (!decoder_grow_span(dec, &done, &total)) {
			fprintf(stderr, "audio[%ld]: the download is finished, resuming\n", log_ms());
			return;
		}
		if (done >= target) {
			fprintf(stderr, "audio[%ld]: resuming with the buffer full (%ld KB)\n", log_ms(), done / 1024);
			return;
		}

		usleep(100 * 1000);
		waited_ms += 100;
	}
	fprintf(stderr, "audio[%ld]: a minute at the edge and it is not growing: carrying on anyway\n", log_ms());
}

// Play routine for uncompressed WAV files: PCM data is streamed straight from
// the file, no decode step needed.
static void play_decoded_file(const char *filepath, decode_format_t format);

// What a seek does to the queued audio.
//
// A card: dropped and prepared, so the new position is heard at once.
//
// bluealsa: left alone. A drop makes bluealsa release the A2DP transport, and
// a prepare does not acquire it back -- the writes after it are accepted and
// the headphones hear nothing, or a transport torn down and set up again
// under a run of seeks. The queue (under 700 ms) plays out and the new
// position follows it. `already_dropped`: the stream was dropped anyway (a
// wait for a download), so it is prepared again as a card would be.
static void seek_flush(snd_pcm_t *pcm, bool already_dropped) {
	if (pcm_is_bluetooth(pcm) && !already_dropped) {
		return;
	}
	dac_ramp_before_cut(pcm);
	snd_pcm_drop(pcm);
	snd_pcm_prepare(pcm);
}

static void play_wav_file(const char *filepath) {
	FILE *f = NULL;
	wav_info_t info;
	int parsed = parse_wav(filepath, &info, &f);
	// ADPCM, a-law and the rest: libsndfile reads them, and the decoded path
	// plays what it hands over.
	if (parsed == WAV_UNSUPPORTED && sndfile_available()) {
		play_decoded_file(filepath, DECODE_FORMAT_SNDFILE);
		return;
	}
	if (parsed != WAV_OK) {
		fprintf(stderr, "Audio: Failed to parse WAV metadata: %s\n", filepath);
		pthread_mutex_lock(&audio_mutex);
		audio_command = AUDIO_CMD_STOP;
		playback_status = AUDIO_STATUS_STOPPED;
		pthread_mutex_unlock(&audio_mutex);
		return;
	}

	// Progress, seeks and pauses count bytes of the file, not of what reaches the device.
	double bytes_per_sec = (double)info.sample_rate * info.src_frame_bytes;
	int src_frame_bytes = info.src_frame_bytes;
	pthread_mutex_lock(&audio_mutex);
	progress_total_secs = (double)info.data_size / bytes_per_sec;
	progress_current_secs = opening_position();
	progress_floor_secs = -1.0;
	stream_sample_rate = info.sample_rate;
	stream_channels = info.channels;
	stream_bits = info.bits_per_sample;
	stream_dsd_multiple = 0;
	stream_lossy = false;
	stream_bitrate_kbps = (int)(bytes_per_sec * 8.0 / 1000.0 + 0.5);
	stream_codec[0] = '\0'; // no decoder name: the page names it by the extension
	// See the decoder path: a restore already has PAUSE queued, so it must
	// not flash through PLAYING on its way to paused.
	playback_status = (audio_command == AUDIO_CMD_PAUSE) ? AUDIO_STATUS_PAUSED : AUDIO_STATUS_PLAYING;
	pthread_mutex_unlock(&audio_mutex);

	// The previous track's PCM when it stayed open and suits this one, as on
	// the decoded path.
	snd_pcm_uframes_t period_size;
	snd_pcm_t *pcm_handle = gapless_take(info.channels, info.sample_rate, info.out_bits, &period_size);
	playback_follow(pcm_handle);
	if (pcm_handle) {
		pcm_device_open = true;
	} else {
		pcm_handle = open_pcm_device(info.channels, info.sample_rate, info.out_bits, &period_size);
	}
	if (!pcm_handle) {
		fclose(f);
		pthread_mutex_lock(&audio_mutex);
		audio_command = AUDIO_CMD_STOP;
		playback_status = AUDIO_STATUS_STOPPED;
		pthread_mutex_unlock(&audio_mutex);
		return;
	}
	track_started();

	// From here, how long this track takes to start coming out is timed.
	bt_start_probe_arm(pcm_handle);
	health_t health;
	health_start(&health);

	int frame_bytes = info.channels * (info.out_bits / 8);
	long paused_at_ms = 0; // when the current pause began (device power-down clock)
	char *buffer = malloc(period_size * frame_bytes);
	// What is read from the file, when it is not already what the device takes.
	unsigned char *raw = NULL;
	if (buffer && wav_needs_conversion(&info)) {
		raw = malloc(period_size * (size_t)src_frame_bytes);
		if (!raw) {
			free(buffer);
			buffer = NULL;
		}
	}
	if (!buffer) {
		fprintf(stderr, "Audio: Out of memory for period buffer\n");
		snd_pcm_close(pcm_handle);
		fclose(f);
		pthread_mutex_lock(&audio_mutex);
		audio_command = AUDIO_CMD_STOP;
		playback_status = AUDIO_STATUS_STOPPED;
		pthread_mutex_unlock(&audio_mutex);
		return;
	}

	// int64_t and not long: on the device (MIPS o32) long is 32 bit, and this
	// counts PCM bytes. At 44.1 kHz 16-bit stereo that is 176,400 bytes a
	// second, so a 32-bit sign flips after three and three quarter hours --
	// the middle of a long audiobook, not an edge case. A negative count makes
	// progress_current_secs negative and also decides whether a track finished
	// or was cut short (cut_short, below), so the queue would advance on its
	// own mid-book.
	int64_t bytes_played = 0;
	int64_t read_pos = 0; // bytes of the data chunk already read, ahead of bytes_played by what is queued
	bool is_paused = false;
	bool played_to_the_end = false;
	int route_check = 0;
	int track_route = alsa_output_key(); // the output auto_set_output just applied

	while (1) {
		loop_turns++;
		pthread_mutex_lock(&audio_mutex);
		if (audio_command == AUDIO_CMD_STOP || play_request) {
			pthread_mutex_unlock(&audio_mutex);
			break;
		}
		if (seek_request) {
			double target = seek_target_secs;
			seek_request = false;
			pthread_mutex_unlock(&audio_mutex);

			int64_t target_byte_pos = (int64_t)(target * bytes_per_sec);
			target_byte_pos = (target_byte_pos / src_frame_bytes) * src_frame_bytes;
			if (target_byte_pos < 0)
				target_byte_pos = 0;
			if (target_byte_pos > info.data_size)
				target_byte_pos = info.data_size;

			fseeko(f, (off_t)(info.data_offset + target_byte_pos), SEEK_SET);
			read_pos = target_byte_pos;

			// The device may have been handed back during a long pause (see the
			// PAUSE branch below), and then there is no stream to flush. It has
			// to be checked: alsa answers a null handle with an assert, which
			// aborts -- the crash handler parks the audio thread and the player
			// goes silent until it is restarted, with the interface still
			// running as if nothing had happened.
			//
			// Nothing else needs doing when the device is gone. The file is
			// already at the new position, the track stays paused, and the
			// unpause opens a fresh stream from there.
			if (pcm_handle) {
				seek_flush(pcm_handle, false);
				is_paused = false;
				bt_seek_probe_arm(pcm_handle);
			}

			bytes_played = target_byte_pos;
			pthread_mutex_lock(&audio_mutex);
			progress_current_secs = (double)bytes_played / bytes_per_sec;
			pthread_mutex_unlock(&audio_mutex);
			continue;
		}
		if (audio_command == AUDIO_CMD_PAUSE) {
			bool entering = !is_paused;
			if (entering) {
				is_paused = true;
				playback_status = AUDIO_STATUS_PAUSED;
			}
			pthread_mutex_unlock(&audio_mutex);

			if (entering) {
				int64_t rewind_bytes = pause_stop_stream(pcm_handle, src_frame_bytes);
				if (rewind_bytes > 0) {
					// What was still in the buffer was never heard: wind the
					// file back over it so resuming picks up where the sound
					// actually stopped.
					bytes_played -= rewind_bytes;
					if (bytes_played < 0) {
						bytes_played = 0;
					}
					fseeko(f, (off_t)(info.data_offset + bytes_played), SEEK_SET);
					read_pos = bytes_played;
					pthread_mutex_lock(&audio_mutex);
					progress_floor_secs = progress_current_secs; // the bar stays put
					progress_current_secs = (double)bytes_played / bytes_per_sec;
					pthread_mutex_unlock(&audio_mutex);
				}
			}

			// Straight out of a system suspend the device is left SUSPENDED
			// underneath a paused track: put it back on its feet now, rather
			// than letting the next thing that touches it find it that way.
			pthread_mutex_lock(&audio_mutex);
			bool recover = resume_recover_pending;
			resume_recover_pending = false;
			pthread_mutex_unlock(&audio_mutex);
			if (recover && pcm_handle) {
				pcm_restart_after_pause(pcm_handle);
			}

			// Paused long enough: give the device back so the DAC and the amp
			// can power down. The file stays open and the position stays
			// where it is; the unpause below opens a fresh stream.
			if (entering) {
				paused_at_ms = log_ms();
			}
			if (pcm_handle && (paused_release_wanted || (log_ms() - paused_at_ms) >= paused_device_close_ms())) {
				snd_pcm_close(pcm_handle);
				pcm_handle = NULL;
				pcm_device_open = false;
				playback_follow(NULL);
				fprintf(stderr, "audio[%ld]: device released while paused (DAC powered down)\n", log_ms());
			}
			if (!pcm_handle) {
				idle_amp_off(log_ms() - paused_at_ms);
			}

			// With the device handed back there is nothing to be quick about:
			// the wait ends on a command's doorbell or after a second
			// (paused_wait_released), instead of twenty looks a second, for
			// however many hours the player sits paused in a pocket. The
			// unpause still feels instant.
			if (pcm_handle) {
				usleep(50000);
			} else {
				paused_wait_released();
			}
			continue;
		} else {
			// RESUME is a one-shot command: consumed here, back to
			// AUDIO_CMD_PLAY. A command left at RESUME poisons every "is a
			// command pending?" test (audio_command != AUDIO_CMD_PLAY) -- the
			// growfile waits would give up immediately, the wait for room in
			// the card's buffer would return -EINTR and throw away a block of
			// samples per pass, and the "retry after rebuffering" path at the
			// end of a track would loop without ever declaring the end.
			if (audio_command == AUDIO_CMD_RESUME) {
				audio_command = AUDIO_CMD_PLAY;
			}
			if (is_paused) {
				// An unpause always reopens the device from scratch, even after
				// a one-second pause. Reusing the stopped stream (dropped at
				// the pause, prepared now) leaves this driver in a bad state:
				// the audio restarts stuttering, and the real-time audio thread
				// on a single core never yields the CPU, so the interface
				// stutters with it. The open-then-write path is the one every
				// track start, prev and next takes. The cost is a few
				// milliseconds of open, inaudible under a finger that has just
				// pressed play.
				if (pcm_handle) {
					snd_pcm_close(pcm_handle);
					pcm_handle = NULL;
					pcm_device_open = false;
					playback_follow(NULL);
					// Breathing room after the close: the same as the 120 ms
					// on a track's way out and the 150 ms in pcm_reroute. On
					// this driver an open glued to a close comes back busy or
					// half-broken, and a stream opened that way stutters.
					usleep(150 * 1000);
				}
				// Route before stream, as play_file does at every track start:
				// during the pause the codec may have gone to rest (amp down,
				// mute), and opening a PCM over a half-asleep route is the
				// other way to get stuttering music. No stream is live here, so
				// poking the codec cannot stall anything.
				auto_set_output();
				{
					// The device was handed back during the pause: take it
					// again. The file is already positioned where the sound
					// stopped, so nothing else has to move.
					snd_pcm_uframes_t new_period = period_size;
					pcm_handle = open_pcm_device(info.channels, info.sample_rate, info.out_bits, &new_period);
					if (!pcm_handle) {
						fprintf(stderr, "Audio: cannot reopen the device after a pause\n");
						audio_command = AUDIO_CMD_STOP;
						playback_status = AUDIO_STATUS_STOPPED;
						pthread_mutex_unlock(&audio_mutex);
						break;
					}
					if (new_period > period_size) {
						char *bigger = realloc(buffer, new_period * frame_bytes);
						if (bigger) {
							buffer = bigger;
						}
						if (bigger && raw) {
							unsigned char *more = realloc(raw, new_period * (size_t)src_frame_bytes);
							if (more) {
								raw = more;
							} else {
								bigger = NULL;
							}
						}
						if (!bigger) {
							snd_pcm_close(pcm_handle);
							pcm_handle = NULL;
							pcm_device_open = false;
							playback_follow(NULL);
							audio_command = AUDIO_CMD_STOP;
							playback_status = AUDIO_STATUS_STOPPED;
							pthread_mutex_unlock(&audio_mutex);
							break;
						}
						buffer = bigger;
					}
					period_size = new_period;
					fprintf(stderr, "audio[%ld]: device taken back for the unpause\n", log_ms());
					// A reopen is a fresh start for the sink as much as a new
					// track is, and the clock has to start again with it: left
					// armed from the track, the probe would charge the whole
					// pause to the restart.
					bt_start_probe_arm(pcm_handle);
				}
				is_paused = false;
				playback_status = AUDIO_STATUS_PLAYING;
			}
		}
		pthread_mutex_unlock(&audio_mutex);

		// Every ~32 buffers: has the jack changed?
		if (++route_check >= 32) {
			route_check = 0;
			int now_route = alsa_output_key();
			if (now_route != track_route) {
				track_route = now_route;
				if (!pcm_reroute(&pcm_handle, info.channels, info.sample_rate, info.out_bits,
								 &period_size)) {
					fprintf(stderr, "Audio: reroute failed\n");
					pthread_mutex_lock(&audio_mutex);
					audio_command = AUDIO_CMD_STOP;
					playback_status = AUDIO_STATUS_STOPPED;
					pthread_mutex_unlock(&audio_mutex);
					break;
				}
			}
		}

		playback_follow(pcm_handle);
		// Up to the end of the data chunk and no further: what follows it is
		// metadata, not audio.
		health_mark(&health);
		int64_t left = info.data_size - read_pos;
		size_t want = period_size * (size_t)src_frame_bytes;
		if ((int64_t)want > left) {
			want = left > 0 ? (size_t)left : 0;
		}
		size_t read_bytes = want ? fread(raw ? (void *)raw : (void *)buffer, 1, want, f) : 0;
		read_bytes -= read_bytes % (size_t)src_frame_bytes;
		read_pos += (int64_t)read_bytes;
		int frames_read = (int)(read_bytes / (size_t)src_frame_bytes);
		if (frames_read > 0) {
			if (raw) {
				wav_convert(&info, raw, buffer, (size_t)frames_read);
			}
			health_lap(&health, STAGE_READ);
			if (info.out_bits == 16) {
				replaygain_process((short *)buffer, frames_read, info.channels);
				eq_process((short *)buffer, frames_read, info.channels, info.sample_rate);
				// Soundfield last, on the finished stereo pair -- the stock
				// module sits at the end of the chain too, taking whatever the
				// tone shaping left it and widening that.
				soundfield_process((short *)buffer, frames_read, info.channels);
				crossfeed_process((short *)buffer, frames_read, info.channels, info.sample_rate);
				mono_process((short *)buffer, frames_read, info.channels);
				balance_process((short *)buffer, frames_read, info.channels);
				levelmeter_feed_s16((short *)buffer, frames_read, info.channels, info.sample_rate);
			} else {
				replaygain_process_s32((int32_t *)buffer, frames_read, info.channels);
				eq_process_s32((int32_t *)buffer, frames_read, info.channels, info.sample_rate);
				soundfield_process_s32((int32_t *)buffer, frames_read, info.channels);
				crossfeed_process_s32((int32_t *)buffer, frames_read, info.channels, info.sample_rate);
				mono_process_s32((int32_t *)buffer, frames_read, info.channels);
				balance_process_s32((int32_t *)buffer, frames_read, info.channels);
				levelmeter_feed_s32((int32_t *)buffer, frames_read, info.channels, info.sample_rate);
			}
		}
		if (frames_read <= 0) {
			// As in the decoded path: a file that gave nothing has not been
			// played, and must not be reported as finished.
			if (bytes_played == 0) {
				fprintf(stderr, "audio[%ld]: '%s' returned no audio at all\n", log_ms(), filepath);
				if (retry_track_once(filepath, "nothing read")) {
					break;
				}
				pthread_mutex_lock(&audio_mutex);
				audio_command = AUDIO_CMD_STOP;
				playback_status = AUDIO_STATUS_STOPPED;
				pthread_mutex_unlock(&audio_mutex);
				break;
			}

			// Track completed naturally -- unless a stop or another track
			// arrived while the last read was blocked. That is not an end but
			// an interrupted read, and calling it "finished" advances the queue
			// on top of what the user just asked for.
			//
			// It happens with tracks played while still downloading:
			// abandoning the download makes the read in flight return empty.
			pthread_mutex_lock(&audio_mutex);
			bool natural_end = audio_command != AUDIO_CMD_STOP && !play_request;
			audio_command = AUDIO_CMD_STOP;
			playback_status = AUDIO_STATUS_STOPPED;
			track_completed = natural_end;
			pthread_mutex_unlock(&audio_mutex);
			played_to_the_end = natural_end;
			if (natural_end && completion_hook) {
				completion_hook();
			}
			break;
		}

		snd_pcm_uframes_t frames_to_write = (snd_pcm_uframes_t)frames_read;
		if (fade_enabled) {
			int gain = fade_gain_q10((double)bytes_played / bytes_per_sec, (double)info.data_size / bytes_per_sec);
			int samples = (int)frames_to_write * info.channels;
			if (info.out_bits == 32) {
				fade_apply_s32((int32_t *)buffer, samples, gain);
			} else {
				fade_apply_s16((short *)buffer, samples, gain);
			}
		}

		// The volume, when it is this side of the cable and not in a register:
		// a device on the USB-C port with no control of its own. A no-op, one
		// comparison deep, on every other route.
		if (swvolume_active()) {
			int samples = (int)frames_to_write * info.channels;
			if (info.out_bits == 16) {
				swvolume_apply_s16((short *)buffer, samples);
			} else {
				swvolume_apply_s32((int32_t *)buffer, samples);
			}
		}

		health_lap(&health, STAGE_EFFECTS);
		snd_pcm_sframes_t written = pcm_write_recover(&pcm_handle, buffer, frames_to_write, info.channels,
													  info.sample_rate, info.out_bits, &period_size);
		health_lap(&health, STAGE_WRITE);

		if (written == -EINTR) {
			continue; // a command is pending: the top of the loop handles it
		}
		if (written < 0 || !pcm_handle) {
			fprintf(stderr, "Audio: Write failed beyond recovery: %s\n", snd_strerror((int)written));
			if (retry_track_once(filepath, "write failure")) {
				break; // the thread loop picks the queued replay up
			}
			pthread_mutex_lock(&audio_mutex);
			audio_command = AUDIO_CMD_STOP;
			playback_status = AUDIO_STATUS_STOPPED;
			pthread_mutex_unlock(&audio_mutex);
			break;
		}

		bt_seek_probe_report(pcm_handle);
		bt_start_probe_note(pcm_handle, written, info.sample_rate);

		bytes_played += (int64_t)written * src_frame_bytes;
		health_tick(&health, info.sample_rate, info.sample_rate, info.bits_per_sample);
		host_pace(written, info.sample_rate);
		pthread_mutex_lock(&audio_mutex);
		report_position((double)bytes_played / bytes_per_sec);
		pthread_mutex_unlock(&audio_mutex);
	}

	pthread_mutex_lock(&audio_mutex);
	mark_stopped_unless_replaced();
	pthread_mutex_unlock(&audio_mutex);

	// Gapless, on the same conditions as the decoded path.
	bool keep_open = pcm_handle && gapless_enabled && played_to_the_end && !is_paused &&
					 (!pcm_is_bluetooth(pcm_handle) || bt_hold_usable(pcm_handle));
	bool kept_empty = !keep_open && gapless_enabled && !played_to_the_end &&
					  skip_hold_prepare(pcm_handle, false, is_paused);
	keep_open = keep_open || kept_empty;
	if (keep_open) {
		gapless_hold(pcm_handle, info.channels, info.sample_rate, info.out_bits, period_size, kept_empty);
		pcm_handle = NULL;
	}

	// Leaving a track while the device is paused must not drain: snd_pcm_drain()
	// waits for a buffer that a paused device never empties, and the playback
	// thread would never come back for the next track.
	//
	// So the stream is drained only when the track really did reach its end;
	// every other exit is a switch to something else, and the tail of the old
	// track is not wanted.
	if (pcm_handle) {
		// Nothing to un-pause: a paused stream was dropped when the pause
		// began, so it is already stopped and safe to close.
		if (played_to_the_end) {
			// A track shorter than the buffer may never have reached the start
			// threshold: the stream is still PREPARED with the whole track
			// queued. Start it by hand, or the drain has nothing to flush and
			// those last moments are never heard.
			if (snd_pcm_state(pcm_handle) == SND_PCM_STATE_PREPARED) {
				snd_pcm_start(pcm_handle);
			}
			pcm_drain_bounded(pcm_handle);
		} else {
			dac_ramp_before_cut(pcm_handle);
			snd_pcm_drop(pcm_handle);
			}
		snd_pcm_close(pcm_handle);
	}
	pcm_device_open = keep_open;
	free(buffer);
	free(raw);
	fclose(f);
}

// Play routine for compressed formats (MP3/FLAC/OGG Vorbis) via the decoder
// abstraction in decode.h. Always decodes to interleaved signed 16-bit PCM.
// How much a track is decimated before the effects chain: over Bluetooth, when
// the sink runs at a fraction of the track's rate, the chain and alsa's plug
// then work at the sink's rate. S32 stereo or mono only; 1 means untouched.
//
//   [bluetooth]
//   decimate = 0    leave the whole conversion to the plug
//
// A turn of a decimated track, in the track's frames. The decoding and the
// filtering of four input frames per output frame happen at real-time priority
// on the one core, so the turn is held to DECIMATED_TURN_MS of audio: the
// interface waits for a whole turn, and at 90 ms a turn it misses frames. The
// writes are non-blocking and paced by the PCM's avail_min, so shorter turns
// only mean more of them.
#define DECIMATED_TURN_MS 24

static snd_pcm_uframes_t decimated_chunk(snd_pcm_uframes_t period, int out_rate, int decim) {
	snd_pcm_uframes_t chunk = chunk_for(period, out_rate);
	if (decim > 1) {
		snd_pcm_uframes_t cap = (snd_pcm_uframes_t)((long)out_rate * DECIMATED_TURN_MS / 1000);
		if (cap > 0 && chunk > cap) {
			chunk = cap;
		}
	}
	return chunk * (snd_pcm_uframes_t)decim;
}

static int bluetooth_decimation(int sample_rate, int channels, int out_bits, bool passthrough) {
	if (passthrough || out_bits != 32 || channels < 1 || channels > DECIMATE_CHANNELS_MAX || !output_is_bluetooth()) {
		return 1;
	}
	if (!config_get_int("bluetooth", "decimate", 1) || config_get_int("bluetooth", "follow_track_rate", 0)) {
		return 1;
	}
	unsigned sink_rate = bluetooth_sink_rate();
	return sink_rate ? decimate_factor_for(sample_rate, (int)sink_rate) : 1;
}

// How long the DAC's volume stays at mute at the start of a DSD track: the
// codec driver's own sequence (mute, amplifier cycle, unmute) ends 0.56 s after
// the stream opens, and the volume ramp back up is the codec's.
#define DSD_UNMUTE_AFTER_MS 800

// DoP silence after the last frame of a track: `ms` of it, the marker carrying
// on from `last_marker`. Left-justified 32-bit words, as the decoder makes them.
// Raw DSD (usb_native_format set) has no marker: its silence is 0x69 in every byte.
static void dop_silence_tail(snd_pcm_t **pcm, int channels, int rate, snd_pcm_uframes_t *period_size,
							 uint32_t last_marker, int ms) {
	if (ms <= 0 || !*pcm) {
		return;
	}
	long frames_left = (long)rate * ms / 1000;
	enum { CHUNK = 4096 };
	int32_t *chunk = malloc(sizeof(int32_t) * CHUNK * (size_t)channels);
	if (!chunk) {
		return;
	}
	uint32_t marker = last_marker;
	while (frames_left > 0) {
		long n = frames_left < CHUNK ? frames_left : CHUNK;
		for (long i = 0; i < n; i++) {
			marker = marker == 0x05 ? 0xFA : 0x05;
			int32_t word = usb_native_format != SND_PCM_FORMAT_UNKNOWN
							   ? (int32_t)0x69696969u
							   : (int32_t)(((marker << 16) | (0x69u << 8) | 0x69u) << 8);
			for (int c = 0; c < channels; c++) {
				chunk[i * channels + c] = word;
			}
		}
		snd_pcm_sframes_t written = pcm_write_recover(pcm, chunk, (snd_pcm_uframes_t)n, channels, rate, 32, period_size);
		if (written < 0 || !*pcm) {
			break;
		}
		frames_left -= n;
	}
	free(chunk);
	fprintf(stderr, "audio[%ld]: DSD silence tail done (%d ms)\n", log_ms(), ms);
}

static void play_decoded_file(const char *filepath, decode_format_t format) {
	decoder_t *dec = decoder_open(filepath, format);
	if (!dec) {
		// A busy card (a cover decoding, the library scanning) can make the
		// first open fail transiently; one more try costs nothing and saves
		// the track.
		usleep(200 * 1000);
		dec = decoder_open(filepath, format);
	}

	// A file still downloading is not a broken file: it is not all there yet.
	//
	// A streamed track starts playing while it arrives, as soon as the cache
	// reports enough. Between that report and this open there are a couple of
	// thread hops, and now and then the decoder arrives a moment too early: the
	// FLAC header is not complete and decoder_open() refuses.
	//
	// So it waits: while the file grows, bytes are arriving and retrying is
	// worthwhile. The wait gives up on any command (another track, a stop, a
	// pause), so it blocks nothing.
	for (int wait = 0; !dec && growfile_is_growing(filepath) && wait < DECODER_GROW_RETRIES; wait++) {
		pthread_mutex_lock(&audio_mutex);
		bool leave = audio_command != AUDIO_CMD_PLAY || play_request || stop_thread;
		pthread_mutex_unlock(&audio_mutex);
		if (leave) {
			break;
		}
		if (wait == 0) {
			fprintf(stderr, "audio[%ld]: '%s' is still downloading, waiting instead of giving up\n", log_ms(),
					filepath);
		}
		usleep(DECODER_GROW_RETRY_MS * 1000);
		dec = decoder_open(filepath, format);
	}

	if (!dec) {
		fprintf(stderr, "Audio: Failed to open decoder for: %s\n", filepath);
		pthread_mutex_lock(&audio_mutex);
		audio_command = AUDIO_CMD_STOP;
		playback_status = AUDIO_STATUS_STOPPED;
		pthread_mutex_unlock(&audio_mutex);
		return;
	}

	// Raw DSD to a USB DAC, when [usb] dsd_mode asks for it and the DAC has a
	// DSD format of its own: DSD_U32_BE if the kernel knows the DAC, SPECIAL
	// (a UAC2 raw-data setting) otherwise. Refused, the track goes as DoP if
	// the DAC takes the DoP rate, as PCM if not. Like DoP, the samples cannot
	// be scaled: without a volume control on the DAC it plays at full scale.
	usb_native_format = SND_PCM_FORMAT_UNKNOWN;
	bool usb_dsd_fallback_pcm = false;
	if (decoder_passthrough(dec) && !output_is_bluetooth() && usbaudio_active() && usbaudio_dsd_native()) {
		// The probe opens the card, which a held gapless PCM would keep busy.
		gapless_release();
		unsigned dsd = decoder_dsd_rate(dec);
		int native = usbaudio_native_dsd_format(dsd / 32);
		if (native != SND_PCM_FORMAT_UNKNOWN && decoder_dsd_to_native(dec)) {
			usb_native_format = native;
			fprintf(stderr, "usb: native DSD via %s at %u Hz\n", snd_pcm_format_name((snd_pcm_format_t)native),
					dsd / 32);
		} else if (usbaudio_takes_rate(dsd / 16)) {
			fprintf(stderr, "usb: native DSD not taken; DoP at %u Hz instead\n", dsd / 16);
		} else {
			fprintf(stderr, "usb: native DSD and DoP at %u Hz both refused; PCM instead\n", dsd / 16);
			usb_dsd_fallback_pcm = true;
		}
	}

	// Headphones have no DSD mode to switch into: DSD goes to them as PCM.
	// So does a USB DAC that cannot take the level itself: DoP cannot be scaled,
	// so it would play at full volume whatever the slider says (usbaudio_dsd_as_pcm).
	// And DSD512 on every output: its DoP rate, 1411.2 kHz, is past what the
	// card takes (768 kHz), so like the stock player it goes as PCM -- unless a
	// USB DAC took it natively (usb_native_format), at 705.6 kHz raw.
	bool dsd_as_pcm = decoder_passthrough(dec) &&
					  ((decoder_dsd_multiple(dec) == 512 && usb_native_format == SND_PCM_FORMAT_UNKNOWN) ||
					   output_is_bluetooth() || usb_dsd_fallback_pcm ||
					   (usbaudio_active() && usb_native_format == SND_PCM_FORMAT_UNKNOWN && usbaudio_dsd_as_pcm())) &&
					  decoder_dsd_to_pcm(dec);

	int channels = decoder_channels(dec);
	int sample_rate = decoder_sample_rate(dec);
	uint64_t total_frames = decoder_total_pcm_frames(dec);
	int source_bits = decoder_source_bits(dec);

	// New track, clean start: the crossfeed delay line still holds half a
	// millisecond of the previous one, which would be audible over a quiet
	// opening.
	crossfeed_reset();

	// DoP: the frames are DSD bits with a marker byte the DAC watches for, not
	// audio. Nothing in the chain may touch them -- see decoder_passthrough().
	bool passthrough = decoder_passthrough(dec);
	int dsd_multiple = decoder_dsd_multiple(dec);

	// Hi-res sources go to the DAC at their real depth: a 24-bit FLAC plays
	// as S32 (samples left-justified, bit-perfect), the way the stock player
	// flips its stream to "PCM_FORMAT_PCM 32bit". Everything else stays 16 --
	// except on the USB-C port, where the volume is taken off the samples
	// (swvolume.c): 50-60 dB off a 16-bit word leaves a handful of bits and
	// audible distortion at low volume, so the port gets S32 there too.
	// shortcut: not while stretched (the stretcher is 16-bit only); a speed
	// change in mid-track on the port applies from the next track.
	int out_bits = (source_bits > 16 || (usbaudio_active() && playback_speed == 1.0)) ? 32 : 16;
	if (usb_native_format != SND_PCM_FORMAT_UNKNOWN) {
		out_bits = 32; // four bytes of DSD a channel a frame, whatever the speed
	}

	// The rate the device is opened at. The position, the fade and every
	// byte count below stay in the track's own frames.
	decimator_t decim;
	int decim_factor = bluetooth_decimation(sample_rate, channels, out_bits, passthrough);
	// 176.4 kHz is more than any Bluetooth link carries, whatever the
	// decimation settings say.
	if (dsd_as_pcm && output_is_bluetooth() && decim_factor == 1) {
		unsigned sink = bluetooth_sink_rate();
		decim_factor = decimate_factor_for(sample_rate, sink ? (int)sink : 44100);
	}
	if (decim_factor > 1 && decimate_init(&decim, decim_factor, channels) != 0) {
		decim_factor = 1;
	}
	int out_rate = sample_rate / decim_factor;
	if (decim_factor > 1) {
		fprintf(stderr, "audio: the %d Hz track is decimated by %d to %d Hz before the effects, for the headphones\n",
				sample_rate, decim_factor, out_rate);
	}

	pthread_mutex_lock(&audio_mutex);
	progress_total_secs = (double)total_frames / sample_rate;
	progress_current_secs = opening_position();
	progress_floor_secs = -1.0;
	stream_sample_rate = sample_rate;
	stream_channels = channels;
	stream_bits = source_bits;
	stream_dsd_multiple = dsd_multiple;
	stream_bitrate_kbps = decoder_bitrate_kbps(dec);
	stream_lossy = decoder_is_lossy(dec);
	snprintf(stream_codec, sizeof(stream_codec), "%s", decoder_codec_name(dec));
	// A lossy format that does not declare its bitrate: the player can only
	// print the format name. Worth logging, because on a streaming track there
	// is no other way to work it out -- the cache file is still growing.
	if (stream_lossy && stream_bitrate_kbps <= 0) {
		fprintf(stderr, "audio[%ld]: %s does not declare a bitrate\n", log_ms(), stream_codec);
	}
	// A track that is being restored comes up with PAUSE already queued: say
	// PAUSED right away rather than flashing through PLAYING, which would make
	// the LED change colour for a moment at every boot and the status bar show
	// a play glyph for a track that never started.
	playback_status = (audio_command == AUDIO_CMD_PAUSE) ? AUDIO_STATUS_PAUSED : AUDIO_STATUS_PLAYING;
	pthread_mutex_unlock(&audio_mutex);

	// The DAC has to be told before the stream starts, or the first periods
	// reach it while it is still reading the words as PCM -- which at DoP
	// levels is a bang.
	bool dsd_muted = false;			 // the DAC's volume is held down for the start of a DSD track
	long dsd_unmute_at = 0;			 // log_ms() at which it comes back
	uint32_t dsd_last_marker = 0x05; // the DoP marker of the last frame written
	if (passthrough) {
		// Any held PCM goes first: a DAC about to switch to DSD must not still
		// have the previous track's PCM tail queued.
		gapless_release();
		// The codec driver unmutes, mutes and unmutes again around the switch
		// to DSD (and cycles the amplifier half a second in): measured as two
		// clicks at the start of every DSD track. The DAC's own volume is held
		// at mute across it, which the driver's mute bits do not touch, and
		// brought back with the codec's ramp once the sequence is done (see
		// DSD_UNMUTE_AFTER_MS in the loop).
		if (config_get_int("audio", "dsd_start_mute", 1)) {
			alsa_suspend_mute();
			dsd_muted = true;
			dac_ramped = false; // a mute left by a cut is this one's to lift now
		}
		set_dac_dop(1);
	}

	snd_pcm_uframes_t period_size;
	// The previous track's PCM, if it stayed open and suits this one too: that
	// is the whole of gapless. Never for DoP -- see the comment above
	// gapless_release().
	snd_pcm_t *pcm_handle = passthrough ? NULL : gapless_take(channels, out_rate, out_bits, &period_size);
	playback_follow(pcm_handle);
	if (pcm_handle) {
		pcm_device_open = true;
	} else {
		pcm_handle = open_pcm_device(channels, out_rate, out_bits, &period_size);
	}
	if (!pcm_handle && out_bits == 32 && !passthrough && last_open_err == -ENOMEM) {
		// Not the device refusing S32: the driver found no memory even for the
		// smallest buffer. Another open straight away, in any format, asks the
		// same exhausted kernel for more.
		fprintf(stderr, "audio: not retried at 16 bit -- the driver is out of memory, not the format refused\n");
	} else if (!pcm_handle && out_bits == 32 && !passthrough) {
		// If this output cannot do S32 for some reason, 16-bit playback still
		// beats silence. Not for DoP: sixteen bits cannot carry a marker byte
		// and a twenty-four bit payload, so half a DoP stream is not quieter
		// DSD, it is noise.
		fprintf(stderr, "Audio: S32 open failed, falling back to 16-bit\n");
		out_bits = 16;
		// The decimator is S32 only.
		decim_factor = 1;
		out_rate = sample_rate;
		pcm_handle = open_pcm_device(channels, sample_rate, out_bits, &period_size);
	}
	if (!pcm_handle && passthrough) {
		// DSD256 asks the card for 705.6 kHz, which an output that is not this
		// device's own DAC may simply refuse. The track does not play and the
		// log says why rather than leaving a silent stop to be guessed at; only
		// Bluetooth is sent PCM instead (decoder_dsd_to_pcm, above).
		fprintf(stderr, "audio: DoP at %d Hz refused by this output; the track cannot play\n", sample_rate);
	}
	if (!pcm_handle) {
		set_dac_dop(0);
		usb_native_format = SND_PCM_FORMAT_UNKNOWN;
		decoder_close(dec);
		pthread_mutex_lock(&audio_mutex);
		audio_command = AUDIO_CMD_STOP;
		playback_status = AUDIO_STATUS_STOPPED;
		pthread_mutex_unlock(&audio_mutex);
		return;
	}
	track_started();
	dsd_unmute_at = log_ms() + DSD_UNMUTE_AFTER_MS;

	// From here, how long this track takes to start coming out is timed.
	bt_start_probe_arm(pcm_handle);

	double bytes_per_sec = (double)sample_rate * channels * (out_bits / 8);
	int frame_bytes = channels * (out_bits / 8);
	long paused_at_ms = 0; // when the current pause began (device power-down clock)
	// In the track's frames: one chunk decimated is one chunk at the device.
	snd_pcm_uframes_t chunk_frames = decimated_chunk(period_size, out_rate, decim_factor);
	void *buffer = malloc(chunk_frames * frame_bytes);
	if (!buffer) {
		fprintf(stderr, "Audio: Out of memory for period buffer\n");
		snd_pcm_close(pcm_handle);
		set_dac_dop(0);
		usb_native_format = SND_PCM_FORMAT_UNKNOWN;
		decoder_close(dec);
		pthread_mutex_lock(&audio_mutex);
		audio_command = AUDIO_CMD_STOP;
		playback_status = AUDIO_STATUS_STOPPED;
		pthread_mutex_unlock(&audio_mutex);
		return;
	}

	// PCM to a USB DAC: what is queued is re-scaled when the volume moves (see
	// requeue_rescale). Without the memory for it, the change waits for the queue.
	requeue_t requeue;
	memset(&requeue, 0, sizeof(requeue));
	if (!passthrough && usbaudio_active() && !output_is_bluetooth()) {
		requeue_init(&requeue, (size_t)out_rate * USB_BUFFER_MAX_MS / 1000 + chunk_frames, (size_t)frame_bytes,
					 chunk_frames);
	}

	// int64_t and not long: on the device (MIPS o32) long is 32 bit, and this
	// counts PCM bytes. At 44.1 kHz 16-bit stereo that is 176,400 bytes a
	// second, so a 32-bit sign flips after three and three quarter hours --
	// the middle of a long audiobook, not an edge case. A negative count makes
	// progress_current_secs negative and also decides whether a track finished
	// or was cut short (cut_short, below), so the queue would advance on its
	// own mid-book.
	int64_t bytes_played = 0;
	bool is_paused = false;
	bool played_to_the_end = false;
	int route_check = 0;
	int track_route = alsa_output_key(); // the output auto_set_output just applied

	// The time stretcher. Only on the 16-bit path: what it is for is
	// audiobooks, which are AAC and so always 16-bit, and a hi-res FLAC has no
	// business being played at 1.5x. NULL simply means the feature is not
	// available and playback runs at 1.0.
	speed_t *stretch = (out_bits == 16) ? speed_open(channels, sample_rate) : NULL;
	decoder_fill_t fill_ctx = {dec};
	health_t health;
	health_start(&health);

	while (1) {
		loop_turns++;
		if (dsd_muted && log_ms() >= dsd_unmute_at) {
			dsd_muted = false;
			alsa_suspend_restore();
		}
		pthread_mutex_lock(&audio_mutex);
		if (audio_command == AUDIO_CMD_STOP || play_request) {
			pthread_mutex_unlock(&audio_mutex);
			break;
		}
		if (seek_request) {
			double target = seek_target_secs;
			seek_request = false;
			pthread_mutex_unlock(&audio_mutex);

			uint64_t target_frame = (uint64_t)(target * sample_rate);
			if (target_frame > total_frames) {
				target_frame = total_frames;
			}

			// The requested point may not have downloaded yet. Clamping the
			// request to where the download has reached would mean asking for
			// 1:17 and landing at 0:51; a streaming app does the other thing --
			// it stops on the requested point, downloads up to it, and resumes
			// from there.
			//
			// So when the requested point is past the edge, wait for the edge
			// to reach it -- progress bar parked on the requested point, card
			// silent, and the wait giving up on any command (including another
			// seek: the last touch always wins).
			uint64_t reachable = decoder_seekable_limit(dec);
			bool dropped_while_waiting = false;
			if (reachable > 0 && target_frame > reachable && decoder_is_growing(dec)) {
				fprintf(stderr, "audio[%ld]: the requested point has not downloaded yet: waiting for it\n", log_ms());
				// Silence while waiting, when there is a stream to silence: a
				// seek can arrive with the device handed back.
				if (pcm_handle) {
					dac_ramp_before_cut(pcm_handle);
					snd_pcm_drop(pcm_handle);
					dropped_while_waiting = true;
				}

				// The bar sits on the requested point: that is where this is going.
				pthread_mutex_lock(&audio_mutex);
				progress_current_secs = (double)target_frame / sample_rate;
				pthread_mutex_unlock(&audio_mutex);

				while (true) {
					pthread_mutex_lock(&audio_mutex);
					bool leave = audio_command != AUDIO_CMD_PLAY || play_request || seek_request;
					pthread_mutex_unlock(&audio_mutex);
					if (leave) {
						break; // the top of the loop handles it
					}
					if (!decoder_is_growing(dec)) {
						break; // finished (or died): go as far as possible
					}
					reachable = decoder_seekable_limit(dec);
					if (reachable >= target_frame) {
						fprintf(stderr, "audio[%ld]: the requested point has downloaded, resuming from there\n", log_ms());
						break;
					}
					usleep(100 * 1000);
				}
				reachable = decoder_seekable_limit(dec);
			}
			// As far as is reachable now: if the wait was interrupted (or the
			// download died) this is still the closest point to the requested
			// one that actually exists.
			if (reachable > 0 && target_frame > reachable) {
				target_frame = reachable;
			}

			// If the seek fails -- which happens on a file still downloading
			// when the requested point has not arrived -- go back to where the
			// decoder was rather than leaving it nowhere: the music carries on
			// from where it was, far better than playback dying for good.
			uint64_t was_at = (uint64_t)(bytes_played / frame_bytes);
			if (!decoder_seek_to_frame(dec, target_frame)) {
				if (decoder_seek_to_frame(dec, was_at)) {
					target_frame = was_at;
				}
			}
			// What it is holding belongs to the part of the book just left.
			speed_reset(stretch);
			// Same for the crossfeed delay line: it holds half a millisecond of
			// the point just left, which without this is heard crossing from
			// one ear to the other.
			crossfeed_reset();
			if (decim_factor > 1) {
				decimate_reset(&decim);
			}

			// And the starvation flag recorded before the seek: it belonged to
			// the old position (growfile_seek already clears it; this is the
			// second turn of the key for formats that seek by other routes).
			(void)decoder_take_starved(dec);

			// As in the WAV loop above: no device means no stream to flush, and
			// asking alsa to drop a null one aborts the audio thread.
			if (pcm_handle) {
				seek_flush(pcm_handle, dropped_while_waiting);
				is_paused = false;
				bt_seek_probe_arm(pcm_handle);
			}

			bytes_played = (int64_t)(target_frame * frame_bytes);
			pthread_mutex_lock(&audio_mutex);
			progress_current_secs = (double)bytes_played / bytes_per_sec;
			pthread_mutex_unlock(&audio_mutex);
			continue;
		}
		if (audio_command == AUDIO_CMD_PAUSE) {
			bool entering = !is_paused;
			if (entering) {
				is_paused = true;
				playback_status = AUDIO_STATUS_PAUSED;
			}
			pthread_mutex_unlock(&audio_mutex);

			if (entering) {
				int64_t rewind_bytes = pause_stop_stream(pcm_handle, frame_bytes) * decim_factor;
				if (rewind_bytes > 0) {
					// The frames still queued were never heard: put the decoder
					// back over them so resuming carries on from where the
					// sound actually stopped instead of skipping a beat.
					uint64_t played_frames = (uint64_t)(bytes_played / frame_bytes);
					uint64_t rewind_frames = (uint64_t)(rewind_bytes / frame_bytes);
					uint64_t resume_frame = played_frames > rewind_frames ? played_frames - rewind_frames : 0;
					if (decoder_seek_to_frame(dec, resume_frame)) {
						if (decim_factor > 1) {
							decimate_reset(&decim);
						}
						bytes_played = (int64_t)(resume_frame * frame_bytes);
						pthread_mutex_lock(&audio_mutex);
						progress_floor_secs = progress_current_secs; // the bar stays put
						progress_current_secs = (double)bytes_played / bytes_per_sec;
						pthread_mutex_unlock(&audio_mutex);
					}
				}
			}

			pthread_mutex_lock(&audio_mutex);
			bool recover = resume_recover_pending;
			resume_recover_pending = false;
			pthread_mutex_unlock(&audio_mutex);
			if (recover && pcm_handle) {
				pcm_restart_after_pause(pcm_handle);
			}

			// Paused long enough: hand the device back so the DAC and the amp
			// power down. The decoder keeps its place; the unpause opens a
			// fresh stream and carries on from the same frame.
			if (entering) {
				paused_at_ms = log_ms();
			}
			if (pcm_handle && (paused_release_wanted || (log_ms() - paused_at_ms) >= paused_device_close_ms())) {
				snd_pcm_close(pcm_handle);
				pcm_handle = NULL;
				pcm_device_open = false;
				playback_follow(NULL);
				fprintf(stderr, "audio[%ld]: device released while paused (DAC powered down)\n", log_ms());
			}
			if (!pcm_handle) {
				idle_amp_off(log_ms() - paused_at_ms);
			}

			// With the device handed back there is nothing to be quick about:
			// the wait ends on a command's doorbell or after a second
			// (paused_wait_released), instead of twenty looks a second, for
			// however many hours the player sits paused in a pocket. The
			// unpause still feels instant.
			if (pcm_handle) {
				usleep(50000);
			} else {
				paused_wait_released();
			}
			continue;
		} else {
			// RESUME is one-shot: consumed here, back to PLAY -- see the twin
			// comment in the WAV loop. Leaving it set poisons every "is a
			// command pending?" test and the music comes out stuttering.
			if (audio_command == AUDIO_CMD_RESUME) {
				audio_command = AUDIO_CMD_PLAY;
			}
			if (is_paused) {
				// An unpause always reopens the device from scratch -- see the
				// twin comment in the WAV loop. Drop-at-pause plus
				// prepare-at-resume leaves this driver's stream in a bad state:
				// stuttering audio and a stuttering interface, since the
				// real-time audio thread never yields the CPU. Open-then-write
				// is the path every track start takes.
				if (pcm_handle) {
					snd_pcm_close(pcm_handle);
					pcm_handle = NULL;
					pcm_device_open = false;
					playback_follow(NULL);
					// Breathing room after the close -- like the 120 ms on a
					// track's way out and the 150 ms in pcm_reroute: on this
					// driver an open glued to a close comes back busy or
					// half-broken, and the stream stutters.
					usleep(150 * 1000);
				}
				// Route before stream, as play_file does at every start: during
				// the pause the codec may have gone to rest, and a PCM opened
				// over a half-asleep route stutters. No stream is live here, so
				// poking the codec cannot stall anything.
				auto_set_output();
				{
					snd_pcm_uframes_t new_period = period_size;
					pcm_handle = open_pcm_device(channels, out_rate, out_bits, &new_period);
					if (!pcm_handle) {
						fprintf(stderr, "Audio: cannot reopen the device after a pause\n");
						audio_command = AUDIO_CMD_STOP;
						playback_status = AUDIO_STATUS_STOPPED;
						pthread_mutex_unlock(&audio_mutex);
						break;
					}
					snd_pcm_uframes_t new_chunk = decimated_chunk(new_period, out_rate, decim_factor);
					if (new_chunk > chunk_frames) {
						void *bigger = realloc(buffer, new_chunk * frame_bytes);
						if (!bigger) {
							snd_pcm_close(pcm_handle);
							pcm_handle = NULL;
							pcm_device_open = false;
							playback_follow(NULL);
							audio_command = AUDIO_CMD_STOP;
							playback_status = AUDIO_STATUS_STOPPED;
							pthread_mutex_unlock(&audio_mutex);
							break;
						}
						buffer = bigger;
					}
					chunk_frames = new_chunk;
					period_size = new_period;
					fprintf(stderr, "audio[%ld]: device taken back for the unpause\n", log_ms());
					// A reopen is a fresh start for the sink as much as a new
					// track is, and the clock has to start again with it: left
					// armed from the track, the probe would charge the whole
					// pause to the restart.
					bt_start_probe_arm(pcm_handle);
				}
				is_paused = false;
				playback_status = AUDIO_STATUS_PLAYING;
			}
		}
		pthread_mutex_unlock(&audio_mutex);

		// Every ~32 buffers: has the jack changed?
		if (++route_check >= 32) {
			route_check = 0;
			int now_route = alsa_output_key();
			if (now_route != track_route) {
				track_route = now_route;
				if (!pcm_reroute(&pcm_handle, channels, out_rate, out_bits, &period_size)) {
					fprintf(stderr, "Audio: reroute failed\n");
					pthread_mutex_lock(&audio_mutex);
					audio_command = AUDIO_CMD_STOP;
					playback_status = AUDIO_STATUS_STOPPED;
					pthread_mutex_unlock(&audio_mutex);
					break;
				}
			}
		}

		// --- the reserve of a track that is still downloading ----------------
		//
		// A streamed track plays while it downloads. Ahead of the play position
		// is a reserve of bytes already received; when the network slows down
		// the reserve is consumed and the decoder asks for data that is not
		// there. Trickling onwards means the read blocks, the card runs its
		// buffer dry and underruns, recovers and underruns again -- audible
		// stutter, and after eight consecutive attempts the write gives up and
		// the track dies.
		//
		// A streaming app instead stops, refills, and resumes. So does this.
		if (decoder_is_growing(dec)) {
			rebuffer_if_starved(dec, sample_rate, bytes_played / (uint64_t)frame_bytes);
		}

		// How many frames came out (what goes to the card) and how many went
		// in (what the position in the track advances by). The two are the
		// same at normal speed, and deliberately not at any other.
		uint64_t frames_read;
		uint64_t input_frames;
		// Before the decode, as a seek or a resume this turn has just emptied
		// the stream (see playback_follow).
		playback_follow(pcm_handle);
		health_mark(&health);
		if (out_bits == 32) {
			frames_read = decoder_read_pcm_frames_s32(dec, chunk_frames, (int32_t *)buffer);
			input_frames = frames_read;
			health_lap(&health, STAGE_READ);
			if (frames_read != 0 && decim_factor > 1) {
				frames_read = (uint64_t)decimate_s32(&decim, (int32_t *)buffer, (int)frames_read);
				if (frames_read == 0) {
					// Too few frames for one output frame; the delay lines
					// hold them.
					bytes_played += (int64_t)input_frames * frame_bytes;
					continue;
				}
			}
			if (frames_read != 0 && !passthrough) {
				// ReplayGain first: it is a correction to the source level, so
				// everything after it works on the level the listener will
				// actually hear.
				replaygain_process_s32((int32_t *)buffer, (int)frames_read, channels);
				eq_process_s32((int32_t *)buffer, (int)frames_read, channels, out_rate);
				soundfield_process_s32((int32_t *)buffer, (int)frames_read, channels);
				// Crossfeed after soundfield and before balance: one widens the
				// image, this pulls it back inside the head, and balance stays
				// the last word on what reaches each ear.
				crossfeed_process_s32((int32_t *)buffer, (int)frames_read, channels, out_rate);
				mono_process_s32((int32_t *)buffer, (int)frames_read, channels);
				balance_process_s32((int32_t *)buffer, (int)frames_read, channels);
				levelmeter_feed_s32((int32_t *)buffer, (int)frames_read, channels, out_rate);
			}
		} else {
			pthread_mutex_lock(&audio_mutex);
			double want_speed = playback_speed;
			pthread_mutex_unlock(&audio_mutex);

			if (stretch) {
				speed_set_factor(stretch, want_speed);
				frames_read = (uint64_t)speed_pull(stretch, (short *)buffer, (int)chunk_frames, decoder_fill, &fill_ctx,
												   &input_frames);
			} else {
				frames_read = decoder_read_pcm_frames_s16(dec, chunk_frames, (short *)buffer);
				input_frames = frames_read;
			}
			health_lap(&health, STAGE_READ);
			if (frames_read != 0) {
				replaygain_process((short *)buffer, (int)frames_read, channels);
				// The graphic EQ, in place on the decoded block. A no-op when off.
				eq_process((short *)buffer, (int)frames_read, channels, sample_rate);
				// ...then the stereo width, at the end of the chain.
				soundfield_process((short *)buffer, (int)frames_read, channels);
				// Crossfeed right after: what soundfield widened, this pulls
				// back inside the head.
				crossfeed_process((short *)buffer, (int)frames_read, channels, sample_rate);
				// Last in the chain, after everything that could have changed
				// the level: the balance is about what reaches each ear.
				mono_process((short *)buffer, (int)frames_read, channels);
				balance_process((short *)buffer, (int)frames_read, channels);
				levelmeter_feed_s16((short *)buffer, (int)frames_read, channels, sample_rate);
			}
		}

		// Has the decoder reached the edge of the download? Two signals:
		//
		//   * the read came back short (the simple case);
		//   * the read came back full but had to wait (decoder_take_starved).
		//     growfile fills the read by waiting for chunks, so from here it
		//     looks normal -- except every read costs a tenth of a second and
		//     every write is an underrun.
		//
		// Either way, do not trickle onwards: stop and refill, like any
		// streaming app. (What the read already produced is still played below:
		// the gap starts after the last good sample.)
		uint64_t read_count = decim_factor > 1 ? input_frames : frames_read;
		if (decoder_is_growing(dec) && (read_count < chunk_frames || decoder_take_starved(dec))) {
			rebuffer_after_short_read(dec, sample_rate, (uint64_t)(bytes_played / frame_bytes));
			if (frames_read == 0) {
				// Nothing at all came out: retry after refilling. Falling
				// through would declare "track finished" for a track merely
				// stuck at the edge. If the download did die, the next pass
				// sees a file that no longer grows and the cut-short branch
				// decides the ending.
				continue;
			}
		}

		if (frames_read == 0) {
			// First of all: the read can have come back empty not because the
			// file ended but because a pending command made it give up
			// (growfile's wake-up). Pause, stop, another track, a seek --
			// whichever it is, the top of the loop handles it: a pause must
			// pause, not end the track.
			pthread_mutex_lock(&audio_mutex);
			bool command_pending = audio_command != AUDIO_CMD_PLAY || play_request || seek_request;
			pthread_mutex_unlock(&audio_mutex);
			if (command_pending) {
				continue;
			}

			// Nothing at all came out of this file: that is a failed read, not
			// a track that finished. Reported as a completion, the UI would
			// auto-advance, the next file would read short too, and the queue
			// would walk on in silence. The card can be briefly unreadable
			// after a spell with the screen off (the same reason decoder_open
			// already retries), so the track gets one retry and then stops
			// WITHOUT claiming it played.
			if (bytes_played == 0) {
				fprintf(stderr, "audio[%ld]: '%s' returned no audio at all\n", log_ms(), filepath);
				if (retry_track_once(filepath, "nothing decoded")) {
					break;
				}
				pthread_mutex_lock(&audio_mutex);
				audio_command = AUDIO_CMD_STOP;
				playback_status = AUDIO_STATUS_STOPPED;
				pthread_mutex_unlock(&audio_mutex);
				break;
			}

			// End of track -- if it really is the end.
			//
			// The queue must never walk on by itself. It advances when a track
			// reaches its end and in no other case. Two lies that look like an
			// end are rejected here:
			//
			//   * a stop or another track arrived while blocked in the last
			//     read. That is an interrupted read, not an end: treating it as
			//     one sends the queue on top of what the user just asked for;
			//   * the file ended well short of its duration. This happens to
			//     tracks played while downloading: if the download dies, what is
			//     on disk stops halfway and the read comes back empty. It looks
			//     like the end of the track, but the track is twice as long.
			//
			// Without the second test the player walks itself through the whole
			// list: every truncated track advances the queue, and the next one --
			// not downloaded yet -- is truncated in turn.
			uint64_t declared = decoder_total_pcm_frames(dec);
			uint64_t reached = (uint64_t)(bytes_played / frame_bytes);
			bool cut_short = declared > 0 && sample_rate > 0 && reached + (uint64_t)sample_rate < declared;

			pthread_mutex_lock(&audio_mutex);
			bool natural_end = audio_command != AUDIO_CMD_STOP && !play_request && !cut_short;
			audio_command = AUDIO_CMD_STOP;
			playback_status = AUDIO_STATUS_STOPPED;
			track_completed = natural_end;
			pthread_mutex_unlock(&audio_mutex);
			played_to_the_end = natural_end;
			if (natural_end && completion_hook) {
				completion_hook();
			}

			if (cut_short) {
				fprintf(stderr, "audio[%ld]: '%s' stopped at %.1f s of %.1f: cut short, the queue stays put\n", log_ms(),
						filepath, (double)reached / sample_rate, (double)declared / sample_rate);
			}
			break;
		}

		// The fade is a gain, so it is one more thing a DoP stream cannot have.
		if (fade_enabled && !passthrough) {
			int gain = fade_gain_q10((double)bytes_played / bytes_per_sec, (double)total_frames / sample_rate);
			int samples = (int)frames_read * channels;
			if (out_bits == 32) {
				fade_apply_s32((int32_t *)buffer, samples, gain);
			} else {
				fade_apply_s16((short *)buffer, samples, gain);
			}
		}

		// What is already queued first, at the volume as it is now; then this
		// chunk's copy, from before the gain.
		requeue_rescale(&requeue, &pcm_handle, channels, out_rate, out_bits, &period_size);
		requeue_keep(&requeue, buffer, frames_read);

		// And the software volume, under the same rule and for the same reason:
		// scaling a DoP stream would stop it being DSD and start being noise.
		if (swvolume_active() && !passthrough) {
			int samples = (int)frames_read * channels;
			if (out_bits == 32) {
				swvolume_apply_s32((int32_t *)buffer, samples);
			} else {
				swvolume_apply_s16((short *)buffer, samples);
			}
		}

		health_lap(&health, STAGE_EFFECTS);
		snd_pcm_sframes_t written =
			pcm_write_recover(&pcm_handle, buffer, frames_read, channels, out_rate, out_bits, &period_size);
		health_lap(&health, STAGE_WRITE);
		if (written != (snd_pcm_sframes_t)frames_read) {
			requeue_keep(&requeue, NULL, 0); // the queue no longer ends where the copy does
		}

		if (written == -EINTR) {
			// Not an error: a command (pause, stop, another track, a seek)
			// arrived while waiting for room in the buffer. The top of the
			// loop handles it; this block of samples is lost, which is right --
			// whoever pressed the key did not want to hear it.
			continue;
		}
		if (written < 0 || !pcm_handle) {
			fprintf(stderr, "Audio: Write failed beyond recovery: %s\n", snd_strerror((int)written));
			if (retry_track_once(filepath, "write failure")) {
				break; // the thread loop picks the queued replay up
			}
			pthread_mutex_lock(&audio_mutex);
			audio_command = AUDIO_CMD_STOP;
			playback_status = AUDIO_STATUS_STOPPED;
			pthread_mutex_unlock(&audio_mutex);
			break;
		}

		bt_seek_probe_report(pcm_handle);
		bt_start_probe_note(pcm_handle, written, out_rate);
		if (passthrough && out_bits == 32 && frames_read > 0) {
			// The marker is the top byte of a left-justified DoP word.
			dsd_last_marker = ((uint32_t)((int32_t *)buffer)[(frames_read - 1) * channels] >> 24) & 0xFF;
		}

		// The position moves by INPUT frames: at 1.5x, one period written to
		// the card is a period and a half of the book. Scaled by how much of
		// the block the card actually took, for the rare partial write.
		if (frames_read > 0) {
			bytes_played += (int64_t)((double)written * frame_bytes * (double)input_frames / (double)frames_read);
		}
		health_tick(&health, sample_rate, out_rate, source_bits);
		host_pace(written, out_rate);
		pthread_mutex_lock(&audio_mutex);
		report_position((double)bytes_played / bytes_per_sec);
		pthread_mutex_unlock(&audio_mutex);
	}

	pthread_mutex_lock(&audio_mutex);
	mark_stopped_unless_replaced();
	pthread_mutex_unlock(&audio_mutex);

	// A track left inside the start window (a skip in the first second) must
	// not leave the DAC at mute.
	if (dsd_muted) {
		dsd_muted = false;
		alsa_suspend_restore();
	}

	// The track ended on its own, gapless is on and nothing special happened:
	// the PCM stays open with its queue inside and the next track appends to
	// it. See the comment above gapless_release() for the conditions and the
	// reason behind each.
	//
	// Over Bluetooth only while the handle is live: see bt_hold_usable().
	bool keep_open = pcm_handle && gapless_enabled && played_to_the_end && !is_paused && !passthrough &&
					 (!pcm_is_bluetooth(pcm_handle) || bt_hold_usable(pcm_handle));
	bool kept_empty = !keep_open && gapless_enabled && !played_to_the_end && skip_hold_prepare(pcm_handle, passthrough, is_paused);
	keep_open = keep_open || kept_empty;
	if (keep_open) {
		gapless_hold(pcm_handle, channels, out_rate, out_bits, period_size, kept_empty);
		pcm_handle = NULL;
	}

	if (pcm_handle) {
		bool was_paused = is_paused;
		// Asked before the close, because afterwards there is no handle to ask.
		bool was_bluetooth = pcm_is_bluetooth(pcm_handle);
		// Nothing to un-pause: a paused stream was dropped when the pause
		// began, so it is already stopped and safe to close. Only a track that
		// reached its end is drained -- every other exit is a switch to
		// something else, and the tail of the old track is not wanted.
		if (played_to_the_end) {
			// A track shorter than the buffer may never have reached the start
			// threshold: the stream is still PREPARED with the whole track
			// queued. Start it by hand, or the drain has nothing to flush and
			// those last moments are never heard.
			if (snd_pcm_state(pcm_handle) == SND_PCM_STATE_PREPARED) {
				snd_pcm_start(pcm_handle);
			}
			// A DSD stream that simply stops is a click. After the last frame the
			// ring still holds music from three quarters of a second ago, and a
			// drain lets the card play on into it until the next period
			// interrupt, before the driver mutes the DAC. So the stream ends in
			// DSD silence (0x69 under a marker that keeps alternating) and is
			// stopped by hand while that silence is still playing, the way a
			// skip stops it in the middle of the music.
			if (passthrough && out_bits == 32 && channels == 2) {
				int tail_ms = (int)config_get_int("audio", "dsd_tail_ms", 300);
				dop_silence_tail(&pcm_handle, channels, out_rate, &period_size, dsd_last_marker, tail_ms);
				if (tail_ms > 0) {
					// Half the tail is left when the card says so; the position
					// it reports can run a period behind, so that is still
					// silence ahead of it, not the end.
					snd_pcm_sframes_t stop_at = (snd_pcm_sframes_t)out_rate * (tail_ms / 2) / 1000;
					for (int waited = 0; waited < 3000; waited += 5) {
						snd_pcm_sframes_t left = 0;
						if (snd_pcm_delay(pcm_handle, &left) < 0 || left <= stop_at) {
							break;
						}
						usleep(5 * 1000);
					}
					snd_pcm_drop(pcm_handle);
				} else {
					pcm_drain_bounded(pcm_handle);
				}
			} else {
				pcm_drain_bounded(pcm_handle);
			}
		} else {
			dac_ramp_before_cut(pcm_handle);
			snd_pcm_drop(pcm_handle);
			}
		snd_pcm_close(pcm_handle);

		// A stream that sat paused (a track left paused while the screen was
		// off, then skipped) leaves the codec idle, and the driver wants a
		// moment before it will hand out the device again. Without this the
		// next track's open can come back busy, and that track never plays.
		if (was_paused) {
			usleep(120 * 1000);
		} else if (was_bluetooth) {
			// The same moment, for the same reason, at the other device. The
			// close above does not end at the plugin: bluealsa still has to
			// hand the A2DP transport back to bluez over D-Bus, and the next
			// track's open has to make it take it again. Reopening into the
			// middle of that is a race nobody wins, and on a card the identical
			// race is already waited out three lines up.
			//
			// A fifth of a second, against a link that already carries the best
			// part of a second of buffer. Only when the handle was not held for
			// the next track.
			usleep(BT_REOPEN_SETTLE_MS * 1000);
		}
	}
	pcm_device_open = keep_open;
	pthread_mutex_lock(&audio_mutex);
	stream_dsd_multiple = 0;
	pthread_mutex_unlock(&audio_mutex);
	// Off again with the stream. A DAC left in DSD mode reads the next
	// ordinary track's words as bits and plays noise at full level, so this
	// belongs on the way out of every exit from this function, not only the
	// tidy one.
	if (!keep_open) {
		set_dac_dop(0);
	}
	usb_native_format = SND_PCM_FORMAT_UNKNOWN;
	requeue_free(&requeue);
	free(buffer);
	speed_close(stretch);
	decoder_close(dec);
}

static void play_file(const char *filepath) {
	// One line in, one line out: with these, the log answers by itself
	// whether a track that "never started" was never picked up, failed to
	// open, or started and ended at once.
	fprintf(stderr, "audio[%ld]: starting '%s'\n", log_ms(), filepath);

	// And where the crash handler can reach it: a log ends at whatever was
	// flushed, and a process killed rather than faulting writes nothing at all,
	// so the name of the track has to be somewhere a signal handler can read it
	// without going through a stream.
	crumb_set_track(filepath);

	decode_format_t format = decode_detect_format(filepath);
	// The route, applied here: no PCM exists in this thread right now, so
	// the codec switch can never stall a live stream (decompile: the stock
	// remaps po/spdif via its engine's reinit flag, never on a live device).
	//
	// With gapless there can very much be one -- the PCM held open by the
	// previous track, its queue still playing. If the route changed (headphones
	// plugged in between the two tracks) that PCM must be closed before the
	// codec is reprogrammed, not after.
	if (gapless_holding() && alsa_output_key() != held_route) {
		gapless_release();
	}
	// Before the route is written, not only before the PCM is opened: writing
	// the codec route while another stream's PCM is open is the move that
	// wedges this driver (see pcm_reroute), and this is the one place on the
	// way into a track where nothing is holding a lock.
	// No stream of this player's is playing out: the route and the open that
	// follow can wait for the interface (see playback_follow).
	if (!gapless_holding()) {
		playback_follow(NULL);
	}
	release_external_output();
	auto_set_output();

	if (format == DECODE_FORMAT_UNKNOWN) {
		play_wav_file(filepath);
	} else {
		play_decoded_file(filepath, format);
	}
	// A held stream may still be playing out its last buffer.
	playback_set_rt(true);

	pthread_mutex_lock(&audio_mutex);
	bool replaced = play_request;
	pthread_mutex_unlock(&audio_mutex);
	fprintf(stderr, "audio[%ld]: '%s' ended (%s)\n", log_ms(), filepath,
			replaced ? "replaced by a new track" : "stop or end of track");
}

static void *playback_thread_func(void *arg) {
	// The name, and only the name -- not thread_be_background(), which also puts
	// the caller on SCHED_IDLE, and this is the one thread that must never wait
	// behind anything. /proc/self/task/*/comm is what the watchdog's dump
	// prints, and a stall report that says "sonix_player_ho" three times over
	// does not say which thread stopped.
	prctl(PR_SET_NAME, "playback", 0, 0, 0);

	// This thread runs the decoders, so it is the one a malformed file can
	// recurse to the bottom of its stack: give it somewhere for the crash
	// handler to run.
	thread_signal_stack();

	// Real-time round-robin, modestly: the UI decoding a 9 MB cover on the
	// one core must never starve the thread that feeds the DAC. Failing is
	// fine (the host build has no privilege for it).
	playback_set_rt(true);
	if (playback_rt) {
		fprintf(stderr, "audio: playback thread running at RT priority\n");
	}

	while (1) {
		pthread_mutex_lock(&audio_mutex);
		// Stopped from here (album over, or stopped): the Blaze's amplifier
		// goes off after idle_amp_off_ms(), as it does when paused.
		long idle_since = log_ms();
		bool idle_park_done = !alsa_board_is_ak4493();
		while (!play_request && !stop_thread) {
			// Suspend-to-RAM is about to start: any held PCM has to be closed
			// now, not in GAPLESS_HOLD_MS. Only this thread may close it, so
			// the request lands here.
			if (hold_flush_request) {
				hold_flush_request = false;
				pthread_mutex_unlock(&audio_mutex);
				gapless_release();
				pthread_mutex_lock(&audio_mutex);
				continue;
			}
			if (!gapless_holding()) {
				long left = idle_amp_off_ms() - (log_ms() - idle_since);
				if (!idle_park_done && left <= 0) {
					idle_park_done = true; // once: a refused park must not spin
					pthread_mutex_unlock(&audio_mutex);
					bool external_held = !idle_amp_off(log_ms() - idle_since);
					pthread_mutex_lock(&audio_mutex);
					// A receiver has the card: park idle_amp_off_ms() after it
					// lets go instead (audio_external_end rings the doorbell).
					while (external_held && external_pcm && !play_request && !stop_thread) {
						pthread_cond_wait(&audio_cond, &audio_mutex);
					}
					if (external_held) {
						idle_park_done = false;
						idle_since = log_ms();
					}
					continue;
				}
				if (idle_park_done) {
					pthread_cond_wait(&audio_cond, &audio_mutex);
					continue;
				}
				// Looked at again every second rather than once at the end:
				// the wait depends on the socket, and headphones pulled out
				// four minutes in should not keep the amplifier on for one
				// more. It is also the mark the standby reads.
				if (alsa_controls_output() != 5) {
					amp_wait_seen_ms = log_ms();
				}
				if (left > 1000) {
					left = 1000;
				}
				// In whole seconds plus the rest: `long` is 32 bits on the
				// device, and 30 000 ms in nanoseconds is not, which made this
				// wait return at once and spin at RT priority for two seconds
				// in every four, starving a receiver stream into underruns.
				struct timespec until;
				clock_gettime(CLOCK_REALTIME, &until);
				until.tv_sec += left / 1000;
				until.tv_nsec += (left % 1000) * 1000000L;
				until.tv_sec += until.tv_nsec / 1000000000L;
				until.tv_nsec %= 1000000000L;
				pthread_cond_timedwait(&audio_cond, &audio_mutex, &until);
				continue;
			}

			// With a PCM held open (gapless) the wait is bounded: if the next
			// track does not arrive soon, what the card has queued runs out and
			// the PCM must be closed instead of sitting there holding the DAC.
			struct timespec until;
			clock_gettime(CLOCK_REALTIME, &until);
			until.tv_nsec += (long)GAPLESS_HOLD_MS * 1000000L;
			until.tv_sec += until.tv_nsec / 1000000000L;
			until.tv_nsec %= 1000000000L;

			if (pthread_cond_timedwait(&audio_cond, &audio_mutex, &until) == ETIMEDOUT) {
				// Outside the lock: closing a PCM can mean waiting for the
				// card to finish, and the interface must not queue behind that.
				pthread_mutex_unlock(&audio_mutex);
				gapless_release();
				pthread_mutex_lock(&audio_mutex);
			}
		}
		if (stop_thread) {
			pthread_mutex_unlock(&audio_mutex);
			// A PCM held open by gapless does not go away on its own.
			gapless_release();
			break;
		}

		char filepath[512];
		strncpy(filepath, current_filepath, sizeof(filepath));
		play_request = false;
		play_request_ms = 0;
		track_starting = true;
		playback_context_active = true;
		pthread_mutex_unlock(&audio_mutex);

		play_file(filepath);
		track_started(); // a track that never reached its open

		pthread_mutex_lock(&audio_mutex);
		playback_context_active = false;
		pthread_mutex_unlock(&audio_mutex);
	}
	return NULL;
}

// What growfile asks every tenth of a second while the audio thread waits for
// a chunk that is not coming: is there a reason to give up? A stop, a pause,
// another track queued, or a seek. Without it the thread stays in that wait for
// up to twenty seconds after the key was pressed, and from outside the player
// looks dead: prev does nothing and local tracks will not start.
static bool playback_reader_should_abort(void) {
	pthread_mutex_lock(&audio_mutex);
	bool out = audio_command != AUDIO_CMD_PLAY || play_request || stop_thread || seek_request;
	pthread_mutex_unlock(&audio_mutex);
	return out;
}

int audio_init(void) {
	static bool initialized = false;
	if (initialized)
		return 0;

	growfile_set_reader_abort_cb(playback_reader_should_abort);
	playback_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

	set_volume_percent(40); // a sane, audible default on the new dB taper
	// Not on the Blaze: its stock player leaves the card unrouted -- amplifier
	// off, output muted -- until the first track, and so does this. A board
	// that is powered off before anything has played then has no amplifier to
	// thump, and a jack pushed in at the start meets a dead output. The first
	// play routes it (play_file, external_open_locked).
	if (!alsa_board_is_ak4493()) {
		auto_set_output();
	}

	pthread_mutex_lock(&audio_mutex);
	stop_thread = false;
	play_request = false;
	play_request_ms = 0;
	audio_command = AUDIO_CMD_STOP;
	playback_status = AUDIO_STATUS_STOPPED;
	pthread_mutex_unlock(&audio_mutex);

	int err = pthread_create(&playback_thread, NULL, playback_thread_func, NULL);
	if (err != 0) {
		fprintf(stderr, "Audio: Failed to create background thread\n");
		return -1;
	}

	initialized = true;
	return 0;
}

int audio_play(const char *filepath) {
	fprintf(stderr, "audio[%ld]: play requested '%s'\n", log_ms(), filepath);

	// NO route write here: this runs on the UI thread while the previous
	// track's PCM may still be open, and poking "Output Port Switch" with a
	// configured stream up stalls the DMA (the after-headphone-plug wedge).
	// The playback thread routes at track start, the stock engine's way.

	pthread_mutex_lock(&audio_mutex);
	restart_fresh_pending = false; // an explicit play beats the armed restart
	strncpy(current_filepath, filepath, sizeof(current_filepath) - 1);
	play_request = true;
	play_request_ms = log_ms();
	write_fail_retry_used = false;
	audio_command = AUDIO_CMD_PLAY;
	playback_status = AUDIO_STATUS_PLAYING;

	// Zero the progress now instead of leaving the outgoing track's numbers up
	// until the thread has opened the new file: the UI polls this at any
	// moment, and stale values make the bar jump back to where the previous
	// track was before snapping to the new one.
	progress_current_secs = 0.0;
	progress_floor_secs = -1.0;
	progress_total_secs = 0.0;
	stream_sample_rate = 0;
	stream_channels = 0;
	seek_request = false;
	seek_target_secs = -1.0;
	stream_bits = 0;
	stream_bitrate_kbps = 0;
	stream_lossy = false;
	stream_codec[0] = '\0';
	track_completed = false; // fresh playback; any prior completion is consumed
	pthread_cond_signal(&audio_cond);
	playback_wake(); // and out of a pause, if it sits in one
	pthread_mutex_unlock(&audio_mutex);
	return 0;
}

// Play a file starting somewhere other than its beginning: an audiobook picked
// up where it was left. Same shape as audio_play_paused() -- which comes up
// PAUSED at `start_secs` for the boot-time "remember track" restore, consuming
// the queued PAUSE and the queued seek before a frame is written, so nothing
// is heard until play is pressed. Both are functions of their own rather than
// a play followed by a seek because both halves are set under one hold of the
// mutex: otherwise the playback thread could open the file and push its first
// second out of the speakers before the seek reached it.
int audio_play_at(const char *filepath, double start_secs) {
	fprintf(stderr, "audio[%ld]: play requested '%s' from %.1f s\n", log_ms(), filepath, start_secs);

	pthread_mutex_lock(&audio_mutex);
	restart_fresh_pending = false; // an explicit play beats the armed restart
	strncpy(current_filepath, filepath, sizeof(current_filepath) - 1);
	current_filepath[sizeof(current_filepath) - 1] = '\0';
	play_request = true;
	play_request_ms = log_ms();
	write_fail_retry_used = false;
	audio_command = AUDIO_CMD_PLAY;
	playback_status = AUDIO_STATUS_PLAYING;
	if (start_secs > 0) {
		seek_request = true;
		seek_target_secs = start_secs;
	}
	progress_current_secs = start_secs > 0 ? start_secs : 0;
	progress_floor_secs = -1.0;
	progress_total_secs = 0;
	pthread_mutex_unlock(&audio_mutex);

	pthread_cond_signal(&audio_cond);
	playback_wake(); // and out of a pause, if it sits in one
	return 0;
}

int audio_play_paused(const char *filepath, double start_secs) {
	fprintf(stderr, "audio[%ld]: restore requested '%s' at %.1f s (paused)\n", log_ms(), filepath, start_secs);

	pthread_mutex_lock(&audio_mutex);
	restart_fresh_pending = false; // an explicit play beats the armed restart
	strncpy(current_filepath, filepath, sizeof(current_filepath) - 1);
	current_filepath[sizeof(current_filepath) - 1] = '\0';
	play_request = true;
	play_request_ms = log_ms();
	write_fail_retry_used = false;
	audio_command = AUDIO_CMD_PAUSE;
	playback_status = AUDIO_STATUS_PAUSED;
	if (start_secs > 0) {
		seek_request = true;
		seek_target_secs = start_secs;
	}
	progress_current_secs = start_secs > 0 ? start_secs : 0;
	progress_floor_secs = -1.0;
	progress_total_secs = 0;
	pthread_mutex_unlock(&audio_mutex);

	pthread_cond_signal(&audio_cond);
	playback_wake(); // and out of a pause, if it sits in one
	return 0;
}

// audio_notify_resume() below is told by the power manager the moment the SoC
// comes back from suspend: the stream a paused track left open is SUSPENDED by
// then. The playback thread does the recovery itself rather than having ALSA
// poked from the interface thread.
bool audio_device_is_open(void) { return pcm_device_open; }

// Lock-free for the same reason as the function below: the reader is a
// background thread that must never wait on the playback thread.
long audio_quiet_ms(void) {
	if (play_request || track_starting || pcm_opening) {
		return -1;
	}
	long since = log_ms() - quiet_since_ms;
	return since > 0 ? since : 0;
}

// How long the pending play request has been waiting, or 0 when there is none.
//
// Read without taking audio_mutex on purpose: the caller is the watchdog, and
// the case it exists to report is the playback thread not coming back -- which
// on the unpause path means it is inside snd_pcm_open() with audio_mutex held.
// A watchdog that blocks on the very lock it is investigating reports nothing.
// Both values are a word wide on this target, so the worst a lock-free read can
// see is the state from an instant ago.
long audio_play_request_age_ms(void) {
	if (!play_request || play_request_ms == 0) {
		return 0;
	}
	long age = log_ms() - play_request_ms;
	return age > 0 ? age : 0;
}

void audio_notify_resume(void) {
	pthread_mutex_lock(&audio_mutex);
	resume_recover_pending = true;
	pthread_mutex_unlock(&audio_mutex);
}

// Everything down before mem. Reverse engineering of the stock binary shows
// its standby routine issues STOP, waits for the STOPPED state and only then
// writes `mem`: no ALSA object may exist while the SoC stops, because the AIC
// inside the SoC loses its registers and a handle that outlives it is the
// shortest path to the reboot on the first play. This forces the gapless PCM
// closed and waits -- with a cap -- for the playback thread to release the
// device. Returns true once nothing is open. It does not stop the music: the
// caller must already have decided that (the prototype only suspends when
// stopped).
bool audio_suspend_quiesce(int timeout_ms) {
	pthread_mutex_lock(&audio_mutex);
	hold_flush_request = true;
	pthread_cond_broadcast(&audio_cond);
	pthread_mutex_unlock(&audio_mutex);

	for (int waited = 0; waited < timeout_ms; waited += 20) {
		if (!pcm_device_open && !gapless_holding()) {
			return true;
		}
		usleep(20 * 1000);
	}
	return !pcm_device_open && !gapless_holding();
}

bool audio_playback_context_active(void) { return playback_context_active; }

// The real freeze, for mem. ALSA objects are not enough: a paused track keeps
// play_file() alive with the decoder and a FILE* open on the card, the card
// loses power in mem, and the first play after wake -- an unpause of that
// context -- reboots the device. This does what the stock binary does: a real
// STOP, a wait for the context to be genuinely dead (play_file returned, not
// merely the status changed), and "next play = fresh restart" armed with the
// saved path and position.
//
// The logical state is left at PAUSED when there was a track, so the play
// button still reads as "resume", and that resume -- intercepted in
// audio_resume() -- becomes the fresh start from the right point.
bool audio_suspend_freeze(int timeout_ms) {
	pthread_mutex_lock(&audio_mutex);
	bool was_paused = playback_status == AUDIO_STATUS_PAUSED;
	char path[512];
	snprintf(path, sizeof(path), "%s", current_filepath);
	double pos = progress_current_secs;
	double total = progress_total_secs;

	// STOP, exactly as the user would send it, and a wake-up for any waiter.
	audio_command = AUDIO_CMD_STOP;
	playback_status = AUDIO_STATUS_STOPPED;
	seek_request = false;
	seek_target_secs = -1.0;
	track_completed = false;
	hold_flush_request = true;
	pthread_cond_broadcast(&audio_cond);
	pthread_mutex_unlock(&audio_mutex);
	// A track paused with the device handed back sits in paused_wait_released()
	// for up to a second; standby should not wait out that second.
	playback_wake();

	// Wait for the real teardown: context dead, PCM closed, gapless empty.
	bool clean = false;
	for (int waited = 0; waited <= timeout_ms && !clean; waited += 20) {
		clean = !playback_context_active && !pcm_device_open && !gapless_holding();
		if (!clean) {
			usleep(20 * 1000);
		}
	}
	if (!clean) {
		fprintf(stderr, "audio: freeze failed (context=%d pcm=%d gapless=%d)\n",
				(int)playback_context_active, (int)pcm_device_open, (int)gapless_holding());
		return false;
	}

	if (was_paused && path[0]) {
		pthread_mutex_lock(&audio_mutex);
		restart_fresh_pending = true;
		snprintf(restart_fresh_path, sizeof(restart_fresh_path), "%s", path);
		restart_fresh_pos = pos;
		// The facade stays "paused where you were": the context underneath is
		// dead, but the next play rebuilds it.
		playback_status = AUDIO_STATUS_PAUSED;
		snprintf(current_filepath, sizeof(current_filepath), "%s", path);
		progress_current_secs = pos;
		progress_total_secs = total;
		pthread_mutex_unlock(&audio_mutex);
		fprintf(stderr, "audio: frozen while paused: '%s' at %.1f s, fresh restart armed\n", path, pos);
	} else {
		fprintf(stderr, "audio: frozen while stopped\n");
	}
	return true;
}

void audio_pause(void) {
	printf("pausing\n");
	pthread_mutex_lock(&audio_mutex);
	audio_command = AUDIO_CMD_PAUSE;

	// Report the new state immediately instead of waiting for the playback
	// thread to notice the command. It only looks at it between PCM writes, so
	// for up to a period's worth of audio the status would still say PLAYING --
	// and a second tap in that window would ask for a pause again instead of a
	// resume, leaving the button stuck on the play glyph.
	playback_status = AUDIO_STATUS_PAUSED;
	pthread_mutex_unlock(&audio_mutex);
	playback_wake();
}

void audio_resume(void) {
	// The fresh restart, when the freeze armed one: the track context no longer
	// exists (killed before mem, deliberately), so "resume" here means
	// rebuilding everything from scratch at the right point -- fresh decoder,
	// fresh route, fresh PCM. That is the same path every track start takes.
	pthread_mutex_lock(&audio_mutex);
	if (restart_fresh_pending && restart_fresh_path[0]) {
		char path[512];
		snprintf(path, sizeof(path), "%s", restart_fresh_path);
		double pos = restart_fresh_pos;
		restart_fresh_pending = false;
		pthread_mutex_unlock(&audio_mutex);
		fprintf(stderr, "audio: resume after mem -> fresh start of '%s' at %.1f s\n", path, pos);
		audio_play_at(path, pos);
		return;
	}
	pthread_mutex_unlock(&audio_mutex);

	printf("resuming\n");

	pthread_mutex_lock(&audio_mutex);
	audio_command = AUDIO_CMD_RESUME;

	// Same reasoning as audio_pause(): the intent is the truth until the
	// playback thread catches up with it.
	if (playback_status == AUDIO_STATUS_PAUSED) {
		playback_status = AUDIO_STATUS_PLAYING;
	}
	pthread_mutex_unlock(&audio_mutex);
	playback_wake(); // the paused loop waits on it (paused_wait_released)
}

void audio_stop(void) {
	printf("stopping\n");
	pthread_mutex_lock(&audio_mutex);
	audio_command = AUDIO_CMD_STOP;
	playback_status = AUDIO_STATUS_STOPPED;
	seek_request = false;
	seek_target_secs = -1.0;
	track_completed = false; // deliberate stop is not a natural completion
	pthread_mutex_unlock(&audio_mutex);
	playback_wake();
}

audio_status_t audio_get_status(void) {
	pthread_mutex_lock(&audio_mutex);
	audio_status_t status = playback_status;
	pthread_mutex_unlock(&audio_mutex);
	return status;
}

bool audio_take_completion(void) {
	pthread_mutex_lock(&audio_mutex);
	bool completed = track_completed;
	track_completed = false;
	pthread_mutex_unlock(&audio_mutex);
	return completed;
}

void audio_get_current_file(char *out, size_t out_size) {
	if (!out || out_size == 0)
		return;
	pthread_mutex_lock(&audio_mutex);
	strncpy(out, current_filepath, out_size - 1);
	out[out_size - 1] = '\0';
	pthread_mutex_unlock(&audio_mutex);
}

void audio_get_progress(double *current_secs, double *total_secs) {
	pthread_mutex_lock(&audio_mutex);
	double current = progress_current_secs;
	if (progress_floor_secs >= 0.0) {
		if (current >= progress_floor_secs) {
			progress_floor_secs = -1.0; // playback has caught up
		} else {
			current = progress_floor_secs;
		}
	}
	if (current_secs)
		*current_secs = current;
	if (total_secs)
		*total_secs = progress_total_secs;
	pthread_mutex_unlock(&audio_mutex);
}

void audio_get_stream_info(int *sample_rate, int *channels) {
	pthread_mutex_lock(&audio_mutex);
	if (sample_rate)
		*sample_rate = stream_sample_rate;
	if (channels)
		*channels = stream_channels;
	pthread_mutex_unlock(&audio_mutex);
}

int audio_get_stream_bits(void) {
	pthread_mutex_lock(&audio_mutex);
	int bits = stream_bits;
	pthread_mutex_unlock(&audio_mutex);
	return bits;
}

int audio_get_stream_bitrate_kbps(void) {
	pthread_mutex_lock(&audio_mutex);
	int kbps = stream_bitrate_kbps;
	pthread_mutex_unlock(&audio_mutex);
	return kbps;
}

bool audio_stream_is_lossy(void) {
	pthread_mutex_lock(&audio_mutex);
	bool lossy = stream_lossy;
	pthread_mutex_unlock(&audio_mutex);
	return lossy;
}

void audio_get_stream_codec(char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	pthread_mutex_lock(&audio_mutex);
	snprintf(out, size, "%s", stream_codec);
	pthread_mutex_unlock(&audio_mutex);
}

int audio_get_dsd_multiple(void) {
	pthread_mutex_lock(&audio_mutex);
	int m = stream_dsd_multiple;
	pthread_mutex_unlock(&audio_mutex);
	return m;
}

// Playback speed, for audiobooks. Applied on the next block, so it can be
// changed while a book is playing without a gap.
void audio_set_speed(double factor) {
	pthread_mutex_lock(&audio_mutex);
	playback_speed = factor;
	pthread_mutex_unlock(&audio_mutex);
}

double audio_get_speed(void) {
	pthread_mutex_lock(&audio_mutex);
	double value = playback_speed;
	pthread_mutex_unlock(&audio_mutex);
	return value;
}

void audio_seek(double seconds) {
	printf("seeking to %.2f seconds\n", seconds);

	pthread_mutex_lock(&audio_mutex);
	progress_current_secs = seconds; // the bar follows the finger, not the decoder
	progress_floor_secs = -1.0;		// and a deliberate jump outranks the pause floor
	if (restart_fresh_pending) {
		// Paused through a suspend: there is no track context to take the
		// seek, and the play that follows rebuilds one at the saved point.
		// The point is moved instead, or that play would carry on from where
		// the pause was and not from where the bar was put.
		restart_fresh_pos = seconds;
	} else {
		seek_target_secs = seconds;
		seek_request = true;
	}
	pthread_mutex_unlock(&audio_mutex);
	playback_wake();
}

// ---------------------------------------------------------------------------
// where the sound goes
// ---------------------------------------------------------------------------

void audio_set_output_device(const char *pcm) {
	pthread_mutex_lock(&output_lock);
	char before[sizeof(output_pcm)];
	snprintf(before, sizeof(before), "%s", output_pcm);
	if (pcm && pcm[0]) {
		snprintf(output_pcm, sizeof(output_pcm), "%s", pcm);
	} else {
		snprintf(output_pcm, sizeof(output_pcm), "%s", AUDIO_DEFAULT_PCM);
	}
	// Only when it really moved, and on stderr. This line is how a log answers
	// "did the sound stop going to the headphones, or did it stop coming out of
	// them" -- so it must not be one of twenty identical lines, and it must not
	// be sitting in a block buffer when the interesting moment passes.
	char after[sizeof(output_pcm)];
	snprintf(after, sizeof(after), "%s", output_pcm);
	pthread_mutex_unlock(&output_lock);

	if (strcmp(before, after) != 0) {
		fprintf(stderr, "audio[%ld]: the output moves from '%s' to '%s'\n", log_ms(), before, after);
	}
}

void audio_get_output_device(char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return;
	}
	pthread_mutex_lock(&output_lock);
	snprintf(out, out_size, "%s", output_pcm);
	pthread_mutex_unlock(&output_lock);
}

// ---------------------------------------------------------------------------
// Somebody else's audio
//
// AirPlay is the case this exists for. shairport does not open a PCM -- it has
// no ALSA backend at all, its `-o ot` backend is a unix-socket client -- so it
// decodes ALAC and hands the player raw 44100/16/stereo frames to play. The
// stock firmware does exactly the same thing, feeding them into the same output
// chain local playback uses.
//
// This is deliberately NOT part of the playback thread: that thread's whole
// shape is "open a file, decode it, write it", and there is no file here. It is
// the device, opened by the same open_pcm_device() so the routing, the retries
// and the fall back to the jack all behave identically, and written to by
// whoever is holding the other end.
//
// The caller must stop local playback first. Two writers on one PCM is not a
// thing ALSA does.
// ---------------------------------------------------------------------------

static int external_frame_bytes = 4;
static pthread_mutex_t external_lock = PTHREAD_MUTEX_INITIALIZER;

// The wait before the amplifier goes off, for what is plugged in right now
// (see AMP_OFF_PLUGGED_S).
static long idle_amp_off_ms(void) {
	char device[sizeof(output_pcm)];
	audio_get_output_device(device, sizeof(device));
	bool listened = strcmp(device, AUDIO_DEFAULT_PCM) == 0 && alsa_jack_plugged();
	long seconds = listened ? config_get_int("audio", "amp_off_plugged_seconds", AMP_OFF_PLUGGED_S)
							: config_get_int("audio", "amp_off_unplugged_seconds", AMP_OFF_UNPLUGGED_S);
	return seconds > 0 ? seconds * 1000 : 0;
}

bool audio_amp_park_pending(void) {
	long seen = amp_wait_seen_ms;
	// Three seconds: the waits mark it every second. Older is a wait that
	// ended some other way (a play, a park), and must not hold the standby.
	return alsa_board_is_ak4493() && seen && log_ms() - seen < 3000 && alsa_controls_output() != 5;
}

// The stock player's output device drops to route 5 thirty seconds after its
// last write, so its soft pop comes then and mem later finds the amplifier
// already off. The same here, from the paused and stopped loops with the PCM
// closed, after idle_amp_off_ms(): the next play's auto_set_output() writes
// the real route from 5. Not while a receiver or DAC-mode stream holds the
// card: a route write under a live stream stalls its DMA.
// False only when an external stream held the card and the park was skipped.
static bool idle_amp_off(long paused_ms) {
	if (!alsa_board_is_ak4493() || alsa_controls_output() == 5) {
		return true;
	}
	if (paused_ms < idle_amp_off_ms()) {
		amp_wait_seen_ms = log_ms();
		return true;
	}
	// Held through the write: an external stream opening in between would
	// have the route pulled from under it.
	pthread_mutex_lock(&external_lock);
	bool card_free = !external_pcm;
	if (card_free) {
		fprintf(stderr, "audio[%ld]: idle %ld s: amplifier off (route 5)\n", log_ms(), paused_ms / 1000);
#ifndef HOST_BUILD
		bool ok = alsa_set_control("Output Port Switch", 5) >= 0;
#else
		bool ok = true;
#endif
		// The cache says 5 only if the route is 5: mem trusts it and skips its
		// own park, and an amplifier left on into mem clicks loudly.
		if (ok) {
			alsa_controls_note_output(5);
		}
	}
	pthread_mutex_unlock(&external_lock);
	return card_free;
}

// What the open was made on, so a change can be noticed.
//
// Local playback reads alsa_output_key() every chunk and re-routes when it
// moves. An external source has no decode loop to do that in, and the two
// things the output depends on are only read when a PCM is opened:
//
//   the route ("Output Port Switch"), which is what sends the sound to the
//   4.4 mm socket rather than the 3.5 mm one, and
//
//   the ALSA device name, which is what sends it to a USB-C DAC rather than
//   to the internal one.
//
// Without watching both, plugging anything in while a Bluetooth receiver,
// AirPlay or the emulator is running leaves the stream coming out of whatever
// was in use when it started, and the socket the headphones are actually in
// silent.
static int external_rate;
static int external_channels;
static int external_bits;
static int external_buffer_ms;
static int external_route = -1;			// alsa_output_key() at the open
static char external_device[160];		// the ALSA name at the open
static long external_checked_ms;		// when the two above were last compared

// Underruns since the open, and when the last one was reported. A receiver
// that stutters says so in the log, and how often -- which is the difference
// between a thread that is not scheduled in time and a sender that stalls now
// and then over the air.
static unsigned external_underruns;
static long external_underrun_logged_ms;

// How often the check below is worth making. The jack is a sysfs read and the
// device name a string compare under a mutex, so it is not expensive -- but at
// 44.1 kHz a receiver hands over a chunk every eleven milliseconds, and doing
// it ninety times a second on a single core buys nothing. A quarter of a
// second is faster than a plug goes in.
#define EXTERNAL_ROUTE_CHECK_MS 250

// Under external_lock. Applies the route, opens, and records both.
static bool external_open_locked(void) {
	// The route first, as play_file() does it. Skipping it would inherit
	// whatever route the last local track left behind -- or, on a player that
	// has not played anything since boot, none at all.
	auto_set_output();

	pcm_forced_buffer_ms = external_buffer_ms;
	snd_pcm_uframes_t period = 0;
	external_pcm = open_pcm_device(external_channels, external_rate, external_bits, &period);
	pcm_forced_buffer_ms = 0;

	if (!external_pcm) {
		return false;
	}
	pcm_device_open = true;
	external_route = alsa_output_key();
	audio_get_output_device(external_device, sizeof(external_device));
	return true;
}

bool audio_external_begin(int sample_rate, int channels, int bits) {
	return audio_external_begin_latency(sample_rate, channels, bits, 0);
}

// Under external_lock, which it lets go of while it waits. Gives the holder up
// to `ms` to close the PCM after being asked, and says whether it did.
static bool external_wait_closed(int ms) {
	for (int waited = 0; external_pcm && waited < ms; waited += 10) {
		pthread_mutex_unlock(&external_lock);
		usleep(10 * 1000);
		pthread_mutex_lock(&external_lock);
	}
	return !external_pcm;
}

// How long a holder that gives way is waited for: it notices between two
// writes, a few tens of milliseconds.
#define EXTERNAL_YIELD_WAIT_MS 1000

bool audio_external_begin_latency(int sample_rate, int channels, int bits, int buffer_ms) {
	// A PCM held by gapless occupies the DAC. Callers here want to open one of
	// their own on the same device, and without this they find it busy right
	// after a track has ended on its own.
	gapless_release();

	pthread_mutex_lock(&external_lock);

	// The metronome holds the output only until someone else wants it: asked,
	// it closes, and this opens the PCM afresh on this source's own terms.
	if (external_pcm && external_yielding) {
		external_wanted = true;
		external_wait_closed(EXTERNAL_YIELD_WAIT_MS);
	}

	if (external_pcm) {
		pthread_mutex_unlock(&external_lock);
		return true; // already open, and on the same terms
	}

	// One frame is one sample per channel. 24-bit is the odd one out -- ALSA's
	// S24_3LE packs it into three bytes, not four -- and this is what a partial
	// write advances by, so getting it wrong would swap the channels for the
	// rest of the stream rather than merely sounding wrong once.
	external_frame_bytes = channels * (bits == 24 ? 3 : bits / 8);

	// Remembered rather than merely used: a re-open after the headphones move
	// has to ask for exactly the same stream. The buffer among them, because
	// the emulator's short one is the whole of how it keeps time.
	external_wanted = false;
	external_rate = sample_rate;
	external_channels = channels;
	external_bits = bits;
	external_buffer_ms = buffer_ms;
	external_checked_ms = log_ms();
	external_underruns = 0;
	external_underrun_logged_ms = 0;

	bool ok = external_open_locked();

	printf("audio: external source %s (%d Hz, %d ch, %d bits)\n", ok ? "playing" : "COULD NOT OPEN", sample_rate,
		   channels, bits);

	pthread_mutex_unlock(&external_lock);
	return ok;
}

bool audio_external_begin_yielding(int sample_rate, int channels, int bits, int buffer_ms) {
	// Never shares: a PCM some other source holds is asked back first (the
	// AirPlay receiver keeps one while it is merely switched on), and if it
	// does not come back the answer is no.
	//
	// Asked without external_lock held, as local playback asks: the AirPlay
	// side takes its own locks to hand the PCM back.
	pthread_mutex_lock(&external_lock);
	bool held = external_pcm != NULL;
	pthread_mutex_unlock(&external_lock);
	if (held) {
		release_external_output();
		pthread_mutex_lock(&external_lock);
		bool closed = external_wait_closed(EXTERNAL_YIELD_WAIT_MS * 2);
		pthread_mutex_unlock(&external_lock);
		if (!closed) {
			fprintf(stderr, "audio: the output is held by another source; not shared\n");
			return false;
		}
	}

	// A track paused a moment ago keeps the device for a few seconds more;
	// paused, it is not playing, so it is asked to give it back now.
	pthread_mutex_lock(&audio_mutex);
	bool paused_holding = playback_status == AUDIO_STATUS_PAUSED && pcm_device_open;
	pthread_mutex_unlock(&audio_mutex);
	if (paused_holding) {
		paused_release_wanted = true;
		playback_wake();
		for (int waited = 0; pcm_device_open && waited < EXTERNAL_YIELD_WAIT_MS; waited += 10) {
			usleep(10 * 1000);
		}
		paused_release_wanted = false;
	}

	if (!audio_external_begin_latency(sample_rate, channels, bits, buffer_ms)) {
		return false;
	}
	pthread_mutex_lock(&external_lock);
	external_yielding = external_pcm != NULL;
	pthread_mutex_unlock(&external_lock);
	return true;
}

// Under external_lock. Re-opens when the headphones have moved, and says
// whether there is still a PCM to write to.
static bool external_follow_output(void) {
	if (log_ms() - external_checked_ms < EXTERNAL_ROUTE_CHECK_MS) {
		return true;
	}
	external_checked_ms = log_ms();

	char device[sizeof(external_device)];
	audio_get_output_device(device, sizeof(device));
	int route = alsa_output_key();

	if (route == external_route && strcmp(device, external_device) == 0) {
		return true;
	}

	fprintf(stderr, "audio: the output moved under the external source (route %d -> %d, '%s' -> '%s'); re-opening\n",
			external_route, route, external_device, device);

	// Dropped rather than drained: what is still in the buffer belongs to the
	// socket nobody is listening to any more, and draining it would play it
	// there before the change took effect.
	dac_ramp_before_cut(external_pcm);
	snd_pcm_drop(external_pcm);
	snd_pcm_close(external_pcm);
	external_pcm = NULL;
	pcm_device_open = false;

	if (!external_open_locked()) {
		// The new device would not open. Saying so once is enough: the check
		// runs again in a quarter of a second, and the next plug -- or the
		// same one seated properly -- is another go at it.
		fprintf(stderr, "audio: the new output would not open; the external source has nowhere to write\n");
		return false;
	}
	return true;
}

// Under external_lock. Waits for room in a PCM that has none, and says whether
// to go on writing. A Bluetooth PCM that takes nothing for BT_WRITE_STALL_MS
// is closed and opened again: the transport under it is gone, and without a
// fresh open every later write would be accepted and never heard.
static bool external_wait_room(long *last_progress_ms) {
	long since = log_ms() - *last_progress_ms;
	if (since < 0) {
		since = 0; // the millisecond counter wrapped; start the window again
		*last_progress_ms = log_ms();
	}
	if (since >= BT_WRITE_STALL_MS) {
		fprintf(stderr, "audio: the external source's PCM has taken nothing for %ld ms; re-opening\n", since);
		snd_pcm_drop(external_pcm);
		snd_pcm_close(external_pcm);
		external_pcm = NULL;
		pcm_device_open = false;
		if (!external_open_locked()) {
			fprintf(stderr, "audio: the output would not open again; the external source has nowhere to write\n");
			return false;
		}
		*last_progress_ms = log_ms();
		return true;
	}

	// The same guard pcm_write_bluetooth() has: bluealsa's poll follows its
	// FIFO and the write follows the transport, so a poll that says "room"
	// against a write that takes nothing becomes a short sleep, not a spin.
	if (snd_pcm_wait(external_pcm, 100) > 0) {
		usleep(10 * 1000);
	}
	return true;
}

// Under external_lock. The software volume on a copy of the frames, in the
// format the source opened with: the caller's buffer is const, and may be
// handed in again. NULL when the copy cannot be grown.
static s24_scratch_t external_vol;
static const void *external_scaled(const void *frames, int count) {
	size_t samples = (size_t)count * (size_t)external_channels;
	if (samples > external_vol.capacity) {
		// Four bytes a sample, which also holds S16 and S24_3LE.
		int32_t *grown = realloc(external_vol.data, samples * sizeof(int32_t));
		if (!grown) {
			return NULL;
		}
		external_vol.data = grown;
		external_vol.capacity = samples;
	}
	memcpy(external_vol.data, frames, (size_t)count * (size_t)external_frame_bytes);
	if (external_bits == 16) {
		swvolume_apply_s16((short *)external_vol.data, (int)samples);
	} else if (external_bits == 24) {
		swvolume_apply_s24_3le((unsigned char *)external_vol.data, (int)samples);
	} else {
		swvolume_apply_s32(external_vol.data, (int)samples);
	}
	return external_vol.data;
}

int audio_external_write(const void *frames, int count) {
	pthread_mutex_lock(&external_lock);

	if (!external_pcm || count <= 0) {
		pthread_mutex_unlock(&external_lock);
		return 0;
	}

	// The headphones may have moved since the last chunk. This is the only
	// place that runs often enough to notice: an external source has no decode
	// loop of its own, and nothing else re-opens a PCM it did not open.
	if (!external_follow_output()) {
		pthread_mutex_unlock(&external_lock);
		return 0;
	}

	// The volume, under the decode loop's rule: a device on the USB-C port
	// with no control of its own. Without it a receiver plays at full scale.
	// No DoP check: the USB DAC mode, the one source that sends DSD, has the
	// port as a gadget, where no USB-C device can be in circuit.
	if (swvolume_active()) {
		frames = external_scaled(frames, count);
		if (!frames) {
			pthread_mutex_unlock(&external_lock);
			return 0; // silence, not the stream at full scale
		}
	}

	int written = 0;
	long last_progress_ms = log_ms();
	while (written < count) {
		const void *chunk = (const char *)frames + (size_t)written * external_frame_bytes;
		if (pcm_takes_s24(external_pcm, external_bits)) {
			chunk = s24_from_s32(&external_s24, chunk, (size_t)(count - written) * (size_t)external_channels);
		}
		snd_pcm_sframes_t got = chunk ? snd_pcm_writei(external_pcm, chunk, (snd_pcm_uframes_t)(count - written)) : -ENOMEM;
		if (got > 0) {
			written += (int)got;
			last_progress_ms = log_ms();
			continue;
		}
		if (got == 0 || got == -EAGAIN) {
			// No room. A bluealsa PCM is opened non-blocking (open_pcm_device),
			// so the wait that paces the source to real time happens here; the
			// rest of the chunk is not dropped.
			if (!external_wait_room(&last_progress_ms)) {
				break;
			}
			continue;
		}
		if (got == -EPIPE) {
			// An underrun: the sender stalled, which over a wireless link is
			// ordinary. Prepare and carry on rather than tearing the stream
			// down over a gap. Reported at most every two seconds.
			external_underruns++;
			if (external_underrun_logged_ms == 0 || log_ms() - external_underrun_logged_ms >= 2000) {
				external_underrun_logged_ms = log_ms();
				fprintf(stderr, "audio: the external source ran dry (%u underruns since it opened)\n", external_underruns);
			}
			snd_pcm_prepare(external_pcm);
			continue;
		}
		if (got == -ESTRPIPE) {
			while (snd_pcm_resume(external_pcm) == -EAGAIN) {
				usleep(100 * 1000);
			}
			snd_pcm_prepare(external_pcm);
			continue;
		}
		fprintf(stderr, "audio: external write failed: %s\n", snd_strerror((int)got));
		break;
	}
	if (written > 0) {
		dac_ramp_release(external_pcm);
	}

	pthread_mutex_unlock(&external_lock);
	return written;
}

void audio_external_end(void) {
	pthread_mutex_lock(&external_lock);

	if (external_pcm) {
		dac_ramp_before_cut(external_pcm);
		snd_pcm_drop(external_pcm);
		snd_pcm_close(external_pcm);
		external_pcm = NULL;
		pcm_device_open = false;
		external_yielding = false;
		external_route = -1;
		external_device[0] = '\0';
		printf("audio: external source finished; device handed back (%u underruns)\n", external_underruns);
	}

	pthread_mutex_unlock(&external_lock);
	// An idle park waiting on the receiver starts its countdown now.
	pthread_mutex_lock(&audio_mutex);
	pthread_cond_broadcast(&audio_cond);
	pthread_mutex_unlock(&audio_mutex);
}
