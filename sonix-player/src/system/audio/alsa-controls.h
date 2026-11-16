#ifndef ALSA_CONTROLS_H
#define ALSA_CONTROLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int alsa_set_control(const char *name, long value);
// Every mixer control the card has, one per line, with the range and the
// current value for the integer ones. For the diagnostic page: the maximum of
// "Output Port Switch" says which routes exist at all.
void alsa_list_controls(char *out, size_t out_size);

int detect_output(void); // the "Output Port Switch" value for what is plugged in now

// True on the R1's audio board: one CS43131, a 3.5 mm socket and the USB-C
// port. No HBC3000, no 4.4 mm socket, no DRE or NOS, and the digital filter
// goes to the chip by another way. See alsa-controls.c.
bool alsa_board_is_cs43131(void);

// True on the V3 Blaze's audio board: two AK4493s behind the HBC3000, 3.5 mm and
// 4.4 mm. Six digital filters, no DRE, no NOS, no balanced line-out flag. See
// alsa-controls.c.
bool alsa_board_is_ak4493(void);

// The route on the OTHER physical socket, where the route is parked before a
// suspend (and, on the Blaze, passed through at the wake). Routes 1 and 2 are
// the two faces of the 3.5 mm one and the driver runs nothing between them, so
// a partner picked by "any different number" would mute nothing.
int output_reinit_partner(int route);


// The whole output configuration as one number: the route above plus the
// balanced line-out flag. Two configurations that differ only in the flag share
// route 3, so anything watching for a change of output has to compare this and
// not the route.
int alsa_output_key(void);
void auto_set_output(void);
// Realigns this file's cached route after the "Output Port Switch" mixer has
// been written by hand (the route parked before suspend to memory).
void alsa_controls_note_output(int value);
// The last route written to "Output Port Switch", or -1 before the first.
int alsa_controls_output(void);

// After a suspend: the line-out flag and the route written once each, whatever
// the cache holds. Wherever the suspend powered the HBC3000 off, the card's
// suspend_pre ran first (the card module links against the HBC3000's, so it
// registers later and suspends earlier) and the driver holds route 0: the
// write is a real change and runs the whole sequence, the HBC3000 powered up
// and loaded. After a refused suspend it holds the parked route instead.
void alsa_controls_rewrite_output(void);

// The card closed to everything but the route re-init after suspend to memory
// (audio_start_output_reinit_after_resume()). Between begin and end every
// other thread waits in alsa_set_control(), auto_set_output() and before a PCM
// is opened. begin is idempotent. The thread doing the re-init calls
// enter_thread before its first write and leave_thread after its last.
void alsa_output_reinit_begin(void);
void alsa_output_reinit_end(void);
void alsa_output_reinit_enter_thread(void);
void alsa_output_reinit_leave_thread(void);
void alsa_wait_output_reinit(void);
void set_volume(long volume);

// The DAC's digital filter (0..dac_filter_count()-1, see alsa-controls.c).
// Written only when it changes.
int dac_filter_count(void);
void set_dac_filter(int filter);
int get_dac_filter(void);

// The DAC's Dynamic Range Enhancement ("DRE_EN", 0/1). Written only when it
// changes; the stock player has it on.
void set_dac_dre(int enabled);
int get_dac_dre(void);

// The DAC's non-oversampling mode ("NOS_EN", 0/1), off by default.
void set_dac_nos(int enabled);
int get_dac_nos(void);

// DoP mode ("DOP_EN", 0/1). On only while a DSD track is streaming: it tells
// the DAC to look for the DoP marker instead of reading the words as PCM, so
// leaving it on would turn the next ordinary track into noise.
void set_dac_dop(int enabled);
int get_dac_dop(void);

// DSD gain compensation, 0..6, where the number is the compensation in dB and
// 0 is none.
//
// The stock player's DSD path does not follow the PCM volume curve: it takes
// the raw attenuation the curve gives and subtracts from it, so a DSD track
// comes out louder than a PCM one at the same volume index. Without that, DSD
// on this player is quieter than DSD on the stock firmware, and a listener who
// turns the knob up for it is left with the next PCM track far too loud.
//
// It is a DAC register and never a multiplication: DoP carries the bitstream
// with a marker in every word, and touching a sample of it would destroy the
// stream. See the note in alsa-controls.c for where the table comes from.
#define DSD_GAIN_MAX_DB 6
void alsa_set_dsd_gain_index(int index);
int alsa_dsd_gain_index(void);

// The gain: a shift of the whole volume scale, the way the stock player's
// per-gain volume tables do it. gain_level_count() levels, 0 the lowest and the
// default: two (6 dB apart) on the R3 Pro II and the R1, three on the V3 Blaze
// (low, mid 6 dB louder, high 3 dB louder still).
int gain_level_count(void);
void set_gain_level(int level);
int get_gain_level(void);
// The two-level form: on is the highest level, off the lowest.
void set_high_gain(int enabled);
int get_high_gain(void);

// What is plugged into the jacks right now.
#define JACK_NONE 0
#define JACK_HEADSET 1	// 3.5 mm
#define JACK_BALANCED 2 // 4.4 mm
int headphone_jack_state(void);
void change_volume(long amount);
long get_volume(void);

// The level the jacks play at for a volume index, in hundredths of a dB (0 at
// the top of the high gain table, negative below), with the gain setting in
// force. ALSA_VOLUME_MUTE_DB100 at index 0. Other outputs follow it so the same
// index sounds alike everywhere.
#define ALSA_VOLUME_MUTE_DB100 (-9999999L)
long alsa_volume_gain_db100(int percent);

// Writes every control again from the values this file last chose, dropping
// the "only write it when it changes" caches first.
//
// Deliberately NOT called from the resume path: on this board the PMIC holds
// every rail at its working voltage through "mem", so the codec keeps its
// registers and there is nothing to restore -- what really goes down is the
// HBC3000 bridge, and the route re-init at resume is what brings that back.
// It has no caller; see the note in alsa-controls.c.
void alsa_controls_reapply(void);

// The DAC silenced before a suspend and brought back after it, on the R3 Pro
// II. Parking the route mutes the analogue port, but the DAC feeding it was
// still live as its supply went, and that is a click of its own. Mute writes
// the channel controls to silent; restore writes back the level in force.
// Nothing on the R1, whose CS43131 is not on that path.
void alsa_suspend_mute(void);
void alsa_suspend_restore(void);

// The same mute for a stream that is still playing, waited on only for as long
// as the codec's own ramp takes from the level in force at `rate` (0: the
// slowest case). False when the DAC was already held, or has no such control.
bool alsa_mute_ramp(int rate);


// The hardware control is an attenuation: 0 is loudest, 255 is silent. Nothing
// outside this file should have to know that, so the UI and the key handlers
// work in plain percent, 0 = silent and 100 = loudest.
int get_volume_percent(void);
void set_volume_percent(int percent);

// Line out: the jack that is plugged in, driven for an amplifier instead of a
// pair of headphones. On the 3.5 mm hole that is route 1 in place of 2; on the
// 4.4 mm one it is "Balance Lineout En" plus a rewrite of route 3, which is a
// different analogue configuration in the machine driver.
//
// Either way the level goes to the stock line-out index (100) through the
// ordinary volume path, so it follows the gain table in force. That level is
// temporary: volume changes are refused while the mode is on, the user's own
// level is put back on the way out, and nothing is written to the config. The
// mode is not remembered across a reboot -- coming up at a fixed full level
// into a pair of headphones is not something to do by surprise.
// Whether the Type-C port has gone to host, i.e. something peripheral is on it
// rather than a charger or a PC. Read by the diagnostic page; nothing acts on
// it, since USB audio arrives as its own sound card (see usbaudio.c) and not as
// a route on this one.
bool usbc_port_attached(void);

bool lineout_is_active(void);
void lineout_set(bool on);

// Ends the mode when the jack it was switched on for is no longer the one
// plugged in. Called from the status bar's once-a-second poll, on the GUI
// thread like every other caller here.
void lineout_check_jack(void);

// Every output keeps its own level, and every change lands in whichever one is
// current. volume_profile_set_output() saves the level the outgoing output was
// at and applies the one the incoming output was left at;
// volume_profile_persist() writes them down and does nothing when none has
// moved. Call init once at startup with what the config holds.
//
// Four and not two because 4.4 mm, 3.5 mm and USB-C are as different from each
// other as any of them is from Bluetooth: different amplifier configurations
// into different headphones, and in the USB-C case an attenuation applied by
// another device entirely. Carrying one number across them is how a change of
// socket becomes a change of loudness nobody asked for.
typedef enum {
	VOLUME_OUTPUT_PHONES = 0, // 3.5 mm, and what an unplugged player counts as
	VOLUME_OUTPUT_BALANCED,	  // 4.4 mm
	VOLUME_OUTPUT_USB,		  // a DAC or a headset on the Type-C port
	VOLUME_OUTPUT_BLUETOOTH,
	VOLUME_OUTPUT_COUNT,
} volume_output_t;

void volume_profile_init(int wired_percent, int bt_percent);
// Seeds one output's remembered level, for the two the call above does not
// carry. Silent about a percent outside 0..100: that is "no level yet".
void volume_profile_set_level(volume_output_t out, int percent);
void volume_profile_set_output(volume_output_t out);
volume_output_t volume_profile_output(void);
// Bluetooth on or off, resolving the wired side to the 3.5 mm profile. Kept for
// callers that only know whether the sound is going over the air.
void volume_profile_set_bluetooth(bool bluetooth);
bool volume_profile_is_bluetooth(void);
int volume_profile_level(bool bluetooth);
void volume_profile_persist(void);
// Writes every level down whether or not one has moved.
void volume_profile_persist_now(void);
void change_volume_percent(int delta);

// The volume limit: a ceiling for each of the two sockets and for USB-C, which
// the level cannot be raised past while the limit is on, by any route -- the
// keys, the slider, the app, a phone over DLNA. Lowering a ceiling under the
// level an output was left at brings that level down to it, at once if it is
// the output in use. Bluetooth is not limited: the headphones out there keep a
// volume of their own. Line out is a fixed level switched on on purpose, and
// is not either. Off, with every ceiling at 100, until it is set.
//
// The ceilings are written to the config as they move, without saving: the
// page saves on release, so a drag is one write and not ninety.
#define VOLUME_LIMIT_MIN 10
bool volume_limit_enabled(void);
void volume_limit_set_enabled(bool on);
int volume_limit(volume_output_t out); // 100 for an output that has none
void volume_limit_set(volume_output_t out, int percent);

// The volume lock (the control centre's padlock): while it is on, the level
// stays where it is against everything that asks on the user's behalf -- the
// keys on the case, the cable's remote, the slider, the app, a phone over
// DLNA. Those routes ask here first and do nothing when the answer is yes;
// set_volume_percent() itself does not look, because what is not a request has
// to keep working: each output still comes up at its own level, the limit still
// brings a level down, line out still takes the jack to its own.
//
// Bluetooth headphones' own buttons are not locked. Their volume lives in
// them and moves whatever this side does; a player that refused to follow
// would only be showing the wrong number.
//
// Kept across restarts: it is switched on for a pocket, and a pocket is where
// a player gets restarted by accident.
bool volume_lock_enabled(void);
void volume_lock_set_enabled(bool on);

#endif
