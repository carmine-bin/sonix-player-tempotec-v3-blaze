#ifndef AUDIO_H
#define AUDIO_H

#include <alsa/asoundlib.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
	AUDIO_CMD_NONE,
	AUDIO_CMD_PLAY,
	AUDIO_CMD_STOP,
	AUDIO_CMD_PAUSE,
	AUDIO_CMD_RESUME,
	AUDIO_CMD_SEEK,
} audio_command_t;

// Actual playback status, maintained by the playback thread. This is the
// source of truth for "is it playing" -- distinct from audio_command_t,
// which is just the transient request channel into the playback thread.
typedef enum {
	AUDIO_STATUS_STOPPED,
	AUDIO_STATUS_PLAYING,
	AUDIO_STATUS_PAUSED,
} audio_status_t;

int audio_init();

// Closes any currently playing file first.
int audio_play(const char *filepath);

// Load a file PAUSED at start_secs (the boot-time track restore): nothing is
// heard until the user presses play.
int audio_play_paused(const char *filepath, double start_secs);

// Play a file from start_secs rather than from its beginning: an audiobook
// resumed where it was left.
int audio_play_at(const char *filepath, double start_secs);

void audio_pause();

// Call when the system has just resumed from suspend: an open stream is left
// SND_PCM_STATE_SUSPENDED behind a paused track, and the playback thread has
// to resume and re-prepare it before anything else touches the device.
void audio_notify_resume(void);

// Call before writing `mem`: closes the PCM the gapless path holds open and
// waits (up to timeout_ms) for the playback thread to release the device. True
// once no ALSA object is alive any more. Does not stop playback.
bool audio_suspend_quiesce(int timeout_ms);

// The freeze for suspend-to-RAM: a real STOP, a wait until play_file() has
// actually returned (decoder and FILE* closed -- the microSD loses power in
// mem), gapless drained, PCM closed. If a track was paused, its path and
// position are saved and a fresh restart is armed: the next audio_resume()
// rebuilds everything from scratch at the right point instead of resuming a
// context that no longer exists. This matches the original binary (flag
// player+0xBB8 set before mem, consumed by the play afterwards).
bool audio_suspend_freeze(int timeout_ms);

// True while play_file() is running, paused included; used by the test bench
// and the logs.
bool audio_playback_context_active(void);

// After a suspend to memory, or an attempt at one: writes the route again,
// which on the R3 Pro II powers the HBC3000 back up and reloads it, then puts
// the DAC level back. On the R3 Pro II this runs on a thread of its own and
// returns at once; the card's other users wait for it (see alsa-controls.h).
// Without it the first play after standby reboots the device.
void audio_start_output_reinit_after_resume(void);
// Moves the route to the unused socket so the driver mutes the one in use
// before mem cuts the amplifier's power (R3 Pro II; see audio.c).
void audio_park_output_before_suspend(void);

// True while the Blaze's amplifier is still on after the music stopped and
// its idle wait (headphones in: five minutes) is not over yet. The standby
// waits for it: mem would take the amplifier down early, and with it the pop
// the wait is there to keep out of the listener's ears.
bool audio_amp_park_pending(void);

// The analogue output brought down before a power-off or reboot: on the Blaze
// the machine driver's own off sequence (route 5), elsewhere the mem park.
void audio_quiet_output_for_power_off(void);

// True while the playback thread holds an open PCM handle. The suspend path
// waits for this to go false: an ALSA stream must never live across a system
// suspend -- the device comes back powered down and reconfigured, and what
// was opened before it is no longer a stream anybody can write to.
bool audio_device_is_open(void);

// How long a play request has been waiting to be picked up, in milliseconds, or
// 0 when nothing is pending. The playback thread clears it the moment it takes
// the request, so a number that keeps growing means the thread is inside a call
// that has not returned -- from outside that looks like "the interface answers,
// the track is logged, and nothing plays". The watchdog reports it.
long audio_play_request_age_ms(void);

// How long the audio side has been left alone: milliseconds since a track last
// finished starting or a PCM last finished opening, or -1 while one of those is
// going on (a play request not yet taken counts). For background work that
// reads a lot of the card at once -- the waveform -- and must stay out of the
// way of an open, when a USB DAC's driver is asking the kernel for its buffers.
long audio_quiet_ms(void);

// A counter the decode loop steps once per turn. It wraps, and its value on its
// own means nothing: what it is for is the difference between two readings a
// second apart. The playback thread is real-time and the device has one core,
// so a turn that never blocks takes the whole machine with it -- the interface
// freezes and the log goes quiet, because nothing else is scheduled to write to
// it. A thread in state R does not say whether it is getting anywhere; this
// does. Tens of thousands a second beside a stalled interface is a spin;
// standing still is a call that never came back.
unsigned audio_loop_turns(void);

// Gapless: the PCM stays open between one track and the next, so there is no
// instant in which the card has nothing to play.
//
// Only for local music, and only between two tracks with the same format --
// channels, rate and depth: an open PCM is configured for those three numbers,
// and reconfiguring means closing it, which is exactly what is being avoided.
// A 44.1 kHz album plays through; a 96 kHz track after it reopens as usual.
//
// Not for DoP (the DAC is in a different mode), nor after a manual stop, where
// the tail of the previous track is not wanted. Each condition is explained in
// full in audio.c.
void audio_set_gapless(bool enabled);
bool audio_get_gapless(void);

// Fade: ramp the last seconds of a track down to silence and the first seconds
// of the next up from it. One decoder means the two cannot truly overlap, so
// this is the ramp-out/ramp-in version of the effect. `seconds` is clamped to
// 1..12.
void audio_set_fade(bool enabled, int seconds);

void audio_resume();

void audio_stop();

audio_status_t audio_get_status(void);

// Returns true exactly once if the current track reached its end on its own
// since the last call, clearing the internal flag. User-initiated stops
// (audio_stop/audio_play) do NOT set it. Lets the controller distinguish a
// natural track completion (auto-advance) from a deliberate stop.
bool audio_take_completion(void);

// Called on the playback thread the moment a track reaches its end on its
// own, so the controller can start the next one while the held PCM still has
// its queue to play (see gapless). Must not block; NULL clears it.
void audio_set_completion_hook(void (*hook)(void));

// Empty string when nothing is loaded.
void audio_get_current_file(char *out, size_t out_size);

void audio_get_progress(double *current_secs, double *total_secs);

// Get the sample rate and channel count of the currently playing stream.
// Both are 0 if nothing has started playing yet.
void audio_get_stream_info(int *sample_rate, int *channels);
int audio_get_stream_bits(void); // source bit depth (16/24/32); 0 while unknown

// What is inside the file currently playing, for the format line in the player
// and for the details page. See decode.h: a lossy format has no bit depth of
// its own and is described by bitrate instead.
//
// audio_get_stream_bitrate_kbps() is the nominal bitrate read from the file,
// not bytes divided by seconds: on a Qobuz or Tidal track the cache file is
// still growing while it plays, so the average would visibly climb. 0 when
// unknown.
int audio_get_stream_bitrate_kbps(void);
bool audio_stream_is_lossy(void);
// "MP3", "AAC", "FLAC", "ALAC", ... Empty when the right name is the
// container's, that is, the extension.
void audio_get_stream_codec(char *out, size_t size);

// 64/128/256/512 while a DSD track is playing, 0 otherwise, whether it goes to
// the DAC untouched over DoP or filtered to PCM (DSD512, Bluetooth); see dsd.h.
int audio_get_dsd_multiple(void);

void audio_seek(double seconds);

// Playback speed. 1.0 is normal; anything else goes through a time stretcher,
// so the pitch does not move -- see speed.h. Only the 16-bit path honours it,
// which is every audiobook and every lossy track; a hi-res FLAC ignores it.
void audio_set_speed(double factor);
double audio_get_speed(void);

// Which ALSA device playback opens. "default" is the DAC and the jacks;
// Bluetooth hands in a bluealsa PCM name ("bluealsa:DEV=..,PROFILE=a2dp"), so
// the stream is encoded and sent to the headphones instead. NULL or an empty
// string puts it back to "default".
//
// The name is picked up at the next PCM open -- the next track, or the next
// play after a stop. A caller that wants the change to be audible right now
// restarts the track itself; pulling the device out from under a running
// playback thread is not something worth doing to save two seconds.
void audio_set_output_device(const char *pcm);
void audio_get_output_device(char *out, size_t out_size);

// Whether the stream is going out over Bluetooth. Asked outside this file by
// the software volume, which stands down there: on A2DP the level belongs to
// the headphones (AVRCP absolute volume), and ot_devices.json says the same
// with A2DP_FIXED_GAIN.
bool audio_output_is_bluetooth(void);

// ---------------------------------------------------------------------------
// Playing audio the player did not decode
//
// AirPlay: shairport has no ALSA backend at all -- its `-o ot` output is a unix
// socket client -- so it decodes ALAC and hands over raw frames for the player
// to put out, which is exactly what the stock firmware does with them.
//
// Stop local playback before calling begin(): one PCM takes one writer.
// Frames are interleaved in the format `bits` asked for, and `count` is frames,
// not bytes -- AirPlay hands over 32-bit stereo, so a frame is eight bytes.
// ---------------------------------------------------------------------------

// Told when local playback wants the output an external source is holding, so
// that source can hand it back. Returns true when the device is free or about
// to be; false means the external source is still using it and the open will
// fail.
//
// It exists because "holding the device" and "playing" are not the same thing
// for the AirPlay receiver: shairport keeps its socket for as long as the
// receiver is switched on, so nothing on that side notices that the phone has
// finished. Registered by airplay.c; nothing else has the problem (the
// emulator holds the output only while its own screen is up).
void audio_set_external_release_cb(bool (*cb)(void));

// The same request, for a source that polls rather than registers: true once
// local playback, or another external source, has asked for the output since
// this one began. The metronome checks it between writes and gives the output
// back.
bool audio_external_wanted(void);

bool audio_external_begin(int sample_rate, int channels, int bits);

// The same, but stating how deep the buffer must be. `buffer_ms` of 0 means the
// usual depth (~750 ms), which is exactly what begin() above does.
//
// Needed by the emulator, which has the opposite problem to the one the 750 ms
// solve: a game must sound the button press now, and the short buffer doubles
// as its metronome -- writing one frame's samples to a blocking PCM lets ALSA
// keep the pace of 59.73 frames per second.
bool audio_external_begin_latency(int sample_rate, int channels, int bits, int buffer_ms);

// begin_latency() for a source that gives the output up to anyone: the
// metronome. It does not share a PCM another source holds (that one is asked
// back first, and false comes back if it is not), and a later begin() from any
// other source sets audio_external_wanted() and waits for it to end before
// opening its own.
bool audio_external_begin_yielding(int sample_rate, int channels, int bits, int buffer_ms);
int audio_external_write(const void *frames, int count);
void audio_external_end(void);

#endif // AUDIO_H
