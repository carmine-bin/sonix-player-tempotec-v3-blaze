#include "alsa-controls.h"

#include "src/system/audio/audio.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/core/config.h"
#include "src/system/audio/swvolume.h"
#include "src/system/audio/usbaudio.h"
#include "src/system/core/utils.h"
#include "src/system/device/sysinfo.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static long current_volume;
static int current_percent = 50;
static long current_output = -1; // last value written to "Output Port Switch"

// ---------------------------------------------------------------------------
// Line out
//
// The same jacks, driven for an amplifier instead of a pair of headphones. It
// is a software mode, not a jack the kernel reports: the 3.5 mm hole is always
// "headset" and the 4.4 mm one always "balance", and what changes is the pair
// of mixer controls written for them.
//
//   3.5 mm  Output Port Switch = 1   (headphone is 2; the machine driver treats
//                                     1 and 2 as the same physical path, so what
//                                     really changes is the fixed level)
//   4.4 mm  Balance Lineout En = 1 THEN Output Port Switch = 3
//
// The flag on its own does nothing: its handler in the machine driver stores it
// and returns. The write of the route is what reads it back and reconfigures
// the HBC3000 GPOs for the balanced line out, which is a genuinely different
// analogue configuration -- so the route must be written again even when it is
// already 3 and only the flag moved.
//
// The level is the stock LO_VOLUME_INDEX: index 100 of the gain table in force,
// which is the same path the volume keys take, not the number 100 written into
// the DAC. It is temporary -- the user's own level is put back on the way out
// and is never overwritten in the config.
// ---------------------------------------------------------------------------
// The V3 Blaze's stock player holds 95 (LO_VOLUME_INDEX in its
// ot_devices_2025.json); here it is 100 there too, as the confirmation promises.
#define LINEOUT_VOLUME_INDEX 100

// ---------------------------------------------------------------------------
// The USB-C digital output
//
// Whether something peripheral is on the port. Detected, never chosen: there is
// no setting for it, and nothing here acts on the answer -- it is read for the
// diagnostic page. A USB DAC or USB-C headphones arrive as a second sound card
// (see usbaudio.c), not as a route on this one.
//
// The Type-C port is what carries the answer on this firmware:
//
//     port_type  = dual source [sink]
//     data_role  = host [device]
//     power_role = source [sink]
//
// That is the standard Linux typec sysfs shape: the alternatives listed, the
// current one in brackets. With nothing but a charger or a PC on the port the
// role is device; when a peripheral is attached the role flips and data_role
// reads "[host]".
//
// The other two paths are fallbacks for firmware that publishes the switch
// node instead. Neither exists here: /sys/class/switch holds only "headset"
// and "balance", and jz-dwc2/otg_id is absent.
#define USBC_DATA_ROLE "/sys/class/typec/port0/data_role"
#define USBC_SWITCH "/sys/class/switch/otg0/state"
#define USBC_OTG_ID "/sys/devices/platform/jz-dwc2/dwc2/otg_id"

// Whether a file's first line contains `needle`. The typec attributes list all
// the roles and bracket the current one, so the question is never equality.
static bool file_contains(const char *path, const char *needle) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	char line[128];
	bool found = fgets(line, sizeof(line), f) && strstr(line, needle) != NULL;
	fclose(f);
	return found;
}

static bool usbc_attached(void) {
	static int said;

	if (file_contains(USBC_DATA_ROLE, "[host]")) {
		if (said != 3) {
			said = 3;
			fprintf(stderr, "alsa: usb-c port occupied (%s)\n", USBC_DATA_ROLE);
		}
		return true;
	}
	if (file_matches(USBC_SWITCH, "1")) {
		if (said != 1) {
			said = 1;
			fprintf(stderr, "alsa: usb-c port occupied (%s)\n", USBC_SWITCH);
		}
		return true;
	}
	// The id pin reads the other way round: grounded (0) is a device on the
	// port, floating (1) is nothing.
	if (file_matches(USBC_OTG_ID, "0")) {
		if (said != 2) {
			said = 2;
			fprintf(stderr, "alsa: usb-c port occupied (%s)\n", USBC_OTG_ID);
		}
		return true;
	}
	said = 0;
	return false;
}

bool usbc_port_attached(void) { return usbc_attached(); }

// ---------------------------------------------------------------------------
// The V3 Blaze's audio board
//
// Two AK4493s (one per channel) behind the same HBC3000 bridge as the R3 Pro
// II, with a 3.5 mm and a 4.4 mm socket. Its machine driver
// (x1600_tempotec_v3a_sound_card.ko, codec_board_ot_put) takes the same routes
// -- 1 and 2 are the 3.5 mm path, 3 and 7 the 4.4 mm one, 4 the S/PDIF -- and
// has the same "X == current route -> return" shortcut, so the re-init dance
// below applies unchanged. It has no "Balance Lineout En": 3 and 7 run the very
// same GPO sequence, so line out on either socket is the fixed level alone.
//
// The codec (codec_ak4493_dual.ko) publishes "Left/Right Playback Volume" with
// the CS43198's sense -- its put writes 255 - value into ATT, so 0 is loudest
// and 255 mute, 0.5 dB a step -- and keeps a shadow of every register it writes,
// which ak4493_initial() replays after each route change: volume and filter
// survive the reconfiguration. "Digital Filter" takes 0..5 (the six AK4493
// filters, see set_dac_filter()). No DRE_EN and no NOS_EN.
// ---------------------------------------------------------------------------
bool alsa_board_is_ak4493(void) {
	static int answer = -1;
	if (answer < 0) {
		const sysinfo_model_t *model = sysinfo_model();
		answer = model && model->ak4493;
	}
	return answer != 0;
}

// DRE_EN exists on the CS43198 board only; NOS_EN on the CS43198 and, through
// its open codec driver, the R1's CS43131 -- not on the AK4493.
static bool board_has_dre(void) { return !alsa_board_is_cs43131() && !alsa_board_is_ak4493(); }
static bool board_has_nos(void) { return !alsa_board_is_ak4493(); }

// ---------------------------------------------------------------------------
// The R1's audio board
//
// One CS43131 on the SoC's I2S, a 3.5 mm socket and the USB-C port: no HBC3000
// and no 4.4 mm socket. Its machine driver (x1600_hiby_r1_sound_card.ko) does
// one thing with "Output Port Switch": 2 and 3 power the CS43131 up, any other
// value powers it down. So on this board the route is always 2, written once,
// with none of the HBC3000's re-init around it.
//
// Its codec driver (codec_cs43131.ko, the open one from hiby-custom-kernel)
// has "Digital Filter" and "NOS_EN" as on the CS43198, and no DRE_EN.
// ---------------------------------------------------------------------------

// The codec's register node: its presence names the board when sysinfo does
// not.
#define CS43131_REG_NODE "/sys/bus/i2c/devices/3-0030/write_reg_val"

bool alsa_board_is_cs43131(void) {
	static int answer = -1;
	if (answer < 0) {
		const sysinfo_model_t *model = sysinfo_model();
		answer = model ? model->cs43131 : access(CS43131_REG_NODE, F_OK) == 0;
	}
	return answer != 0;
}

static bool lineout_on;
static int lineout_saved_percent = -1;
static int lineout_jack; // the jack the mode was switched on for
static int current_balance_lineout = -1; // last value written to "Balance Lineout En"

static void apply_volume_hw(int percent);

// Which of the board's gain tables the index is read from: 0 is the lowest.
// Two on the R3 Pro II and the R1 (MDB, HDB), three on the V3 Blaze (LDB, MDB,
// HDB), as the stock players offer them. Low by default, like the stock player.
static int gain_level;

// The stock firmware's own volume curves, not a computed formula.
//
// The stock binary does not compute raw = 255 - volume*2.55: it reads a
// 101-entry lookup table indexed by the UI volume 0..100. The value is CS43198
// attenuation (0 = loudest, 255 = mute). The gain toggle picks which table, and
// is binary:
//
//   Low Gain  (High Gain OFF) -> MDB   (index 100 -> raw 12)
//   High Gain (High Gain ON)  -> HDB   (index 100 -> raw 0)
//
// Confirmed from the disassembly: the gain wrapper passes "MDB" when the
// boolean is 0 and "HDB" when it is 1. The HiBy engine's third curve, LDB,
// exists but is not the R3 Pro II's low gain and must not be used here.
//
// These two tables are the whole volume law on the jacks -- nothing is done to
// the samples on this route -- and they are drawn as one. Each steps 0.5 dB per
// index from 100 down to 45, 1 dB down to 10, then 2 and 5: fine adjustment at
// the top of the scale and coarse at the bottom. And MDB sits exactly 6.0 dB
// under HDB at every index, which is the gain switch and all of it.
//
// index 0 = 255 in both: zero volume is an explicit mute, not interpolated.
static const int HIBY_HW_MDB[101] = {
	255, 188, 178, 168, 158, 154, 150, 146, 142, 138, //
	136, 134, 132, 130, 128, 126, 124, 122, 120, 118, //
	116, 114, 112, 110, 108, 106, 104, 102, 100, 98,  //
	96,  94,  92,  90,  88,  86,  84,  82,  80,  78,   //
	76,  74,  72,  70,  68,  67,  66,  65,  64,  63,   //
	62,  61,  60,  59,  58,  57,  56,  55,  54,  53,   //
	52,  51,  50,  49,  48,  47,  46,  45,  44,  43,   //
	42,  41,  40,  39,  38,  37,  36,  35,  34,  33,   //
	32,  31,  30,  29,  28,  27,  26,  25,  24,  23,   //
	22,  21,  20,  19,  18,  17,  16,  15,  14,  13,   //
	12};

static const int HIBY_HW_HDB[101] = {
	255, 176, 166, 156, 146, 142, 138, 134, 130, 126, //
	124, 122, 120, 118, 116, 114, 112, 110, 108, 106, //
	104, 102, 100, 98,  96,  94,  92,  90,  88,  86,   //
	84,  82,  80,  78,  76,  74,  72,  70,  68,  66,   //
	64,  62,  60,  58,  56,  55,  54,  53,  52,  51,   //
	50,  49,  48,  47,  46,  45,  44,  43,  42,  41,   //
	40,  39,  38,  37,  36,  35,  34,  33,  32,  31,   //
	30,  29,  28,  27,  26,  25,  24,  23,  22,  21,   //
	20,  19,  18,  17,  16,  15,  14,  13,  12,  11,   //
	10,  9,   8,   7,   6,   5,   4,   3,   2,   1,     //
	0};

// The V3 Blaze's three gains, from its ot_devices_2025.json: HDB is the R3 Pro
// II's table, MDB sits 3 dB (6 raw steps) under it and LDB 9 dB (18 steps).
static const int BLAZE_HW_LDB[101] = {
	255, 194, 184, 174, 164, 160, 156, 152, 148, 144, //
	142, 140, 138, 136, 134, 132, 130, 128, 126, 124, //
	122, 120, 118, 116, 114, 112, 110, 108, 106, 104, //
	102, 100, 98,  96,  94,  92,  90,  88,  86,  84,   //
	82,  80,  78,  76,  74,  73,  72,  71,  70,  69,   //
	68,  67,  66,  65,  64,  63,  62,  61,  60,  59,   //
	58,  57,  56,  55,  54,  53,  52,  51,  50,  49,   //
	48,  47,  46,  45,  44,  43,  42,  41,  40,  39,   //
	38,  37,  36,  35,  34,  33,  32,  31,  30,  29,   //
	28,  27,  26,  25,  24,  23,  22,  21,  20,  19,   //
	18};

static const int BLAZE_HW_MDB[101] = {
	255, 182, 172, 162, 152, 148, 144, 140, 136, 132, //
	130, 128, 126, 124, 122, 120, 118, 116, 114, 112, //
	110, 108, 106, 104, 102, 100, 98,  96,  94,  92,  //
	90,  88,  86,  84,  82,  80,  78,  76,  74,  72,   //
	70,  68,  66,  64,  62,  61,  60,  59,  58,  57,   //
	56,  55,  54,  53,  52,  51,  50,  49,  48,  47,   //
	46,  45,  44,  43,  42,  41,  40,  39,  38,  37,   //
	36,  35,  34,  33,  32,  31,  30,  29,  28,  27,   //
	26,  25,  24,  23,  22,  21,  20,  19,  18,  17,   //
	16,  15,  14,  13,  12,  11,  10,  9,   8,   7,    //
	6};

static long percent_to_raw(int percent) {
	if (percent < 0) {
		percent = 0;
	}
	if (percent > 100) {
		percent = 100;
	}
	if (alsa_board_is_ak4493()) {
		static const int *const BLAZE[3] = {BLAZE_HW_LDB, BLAZE_HW_MDB, HIBY_HW_HDB};
		return BLAZE[gain_level][percent];
	}
	return gain_level ? HIBY_HW_HDB[percent] : HIBY_HW_MDB[percent];
}

long alsa_volume_gain_db100(int percent) {
	if (percent <= 0) {
		return ALSA_VOLUME_MUTE_DB100;
	}
	// One raw step of the CS43198 is 0.5 dB.
	return -percent_to_raw(percent) * 50;
}


// Every mixer control the card publishes, with its range and its value now.
//
// Two things can only be answered from the card itself: whether "Output Port
// Switch" accepts a 4 at all (a maximum of 3 means there is no digital route on
// this hardware), and whether the "SPDIF Output" control the stock binary
// writes is registered.
void alsa_list_controls(char *out, size_t out_size) {
	snd_ctl_t *ctl;
	snd_ctl_elem_list_t *list;

	if (!out || out_size == 0) {
		return;
	}
	out[0] = '\0';

	if (snd_ctl_open(&ctl, "hw:0", 0) < 0) {
		snprintf(out, out_size, "  (no hw:0 card)\n");
		return;
	}

	snd_ctl_elem_list_alloca(&list);
	if (snd_ctl_elem_list(ctl, list) < 0) {
		snprintf(out, out_size, "  (list not readable)\n");
		snd_ctl_close(ctl);
		return;
	}

	unsigned int count = snd_ctl_elem_list_get_count(list);
	if (snd_ctl_elem_list_alloc_space(list, count) < 0 || snd_ctl_elem_list(ctl, list) < 0) {
		snprintf(out, out_size, "  (%u controls, space could not be allocated)\n", count);
		snd_ctl_close(ctl);
		return;
	}

	size_t at = 0;
	for (unsigned int i = 0; i < count; i++) {
		snd_ctl_elem_id_t *id;
		snd_ctl_elem_info_t *info;
		snd_ctl_elem_value_t *value;

		snd_ctl_elem_id_alloca(&id);
		snd_ctl_elem_info_alloca(&info);
		snd_ctl_elem_value_alloca(&value);

		snd_ctl_elem_list_get_id(list, i, id);
		snd_ctl_elem_info_set_id(info, id);
		if (snd_ctl_elem_info(ctl, info) < 0) {
			continue;
		}
		snd_ctl_elem_value_set_id(value, id);

		const char *name = snd_ctl_elem_info_get_name(info);
		char line[160];
		if (snd_ctl_elem_info_get_type(info) == SND_CTL_ELEM_TYPE_INTEGER) {
			long now = snd_ctl_elem_read(ctl, value) < 0 ? -1 : snd_ctl_elem_value_get_integer(value, 0);
			snprintf(line, sizeof(line), "  %s = %ld (%ld..%ld)\n", name, now,
					 snd_ctl_elem_info_get_min(info), snd_ctl_elem_info_get_max(info));
		} else {
			snprintf(line, sizeof(line), "  %s\n", name);
		}

		size_t len = strlen(line);
		if (at + len + 1 >= out_size) {
			break;
		}
		memcpy(out + at, line, len);
		at += len;
		out[at] = '\0';
	}

	if (!at) {
		snprintf(out, out_size, "  (no controls)\n");
	}

	snd_ctl_elem_list_free_space(list);
	snd_ctl_close(ctl);
}

// ---------------------------------------------------------------------------
// The route re-init after mem (see alsa-controls.h)
//
// Its route write powers the HBC3000 and reloads it, a few hundred
// milliseconds inside the driver, and until it is done the card must not be
// opened. The gate is closed from the parking of the route before mem to the
// end of that write.
// ---------------------------------------------------------------------------
static pthread_mutex_t reinit_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reinit_cond = PTHREAD_COND_INITIALIZER;
static bool reinit_running;
static __thread bool reinit_thread; // true on the re-init thread itself

void alsa_output_reinit_begin(void) {
	pthread_mutex_lock(&reinit_lock);
	reinit_running = true;
	pthread_mutex_unlock(&reinit_lock);
}

void alsa_output_reinit_enter_thread(void) { reinit_thread = true; }

void alsa_output_reinit_leave_thread(void) { reinit_thread = false; }

void alsa_output_reinit_end(void) {
	pthread_mutex_lock(&reinit_lock);
	reinit_running = false;
	pthread_cond_broadcast(&reinit_cond);
	pthread_mutex_unlock(&reinit_lock);
}

void alsa_wait_output_reinit(void) {
	if (reinit_thread) {
		return;
	}
	pthread_mutex_lock(&reinit_lock);
	while (reinit_running) {
		pthread_cond_wait(&reinit_cond, &reinit_lock);
	}
	pthread_mutex_unlock(&reinit_lock);
}

int alsa_set_control(const char *name, long value) {
	snd_ctl_t *ctl;
	snd_ctl_elem_id_t *id;
	snd_ctl_elem_value_t *elem;
	int err;

	alsa_wait_output_reinit();

	err = snd_ctl_open(&ctl, "hw:0", 0);
	if (err < 0) {
		return err;
	}

	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_value_alloca(&elem);

	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, name);

	snd_ctl_elem_value_set_id(elem, id);
	snd_ctl_elem_value_set_integer(elem, 0, value);

	err = snd_ctl_elem_write(ctl, elem);

	snd_ctl_close(ctl);
	return err;
}

// Returns a valid value for ALSA "Output Port Switch" based on what is plugged
// in.
int detect_output(void) {
	const char *const sysfs_hs_switch = "/sys/class/switch/headset/state";
	const char *const sysfs_bal_switch = "/sys/class/switch/balance/state";

	// One socket, and route 1 would power the DAC down. Line out on it is the
	// fixed level alone.
	if (alsa_board_is_cs43131()) {
		return 2;
	}

	bool balanced = file_matches(sysfs_bal_switch, "1");

	// Line out rides the jack that is plugged in: the balanced hole keeps route
	// 3 and is told apart by the flag, the 3.5 mm one moves from 2 to 1. With
	// nothing plugged in the 3.5 mm route is the default here as well.
	if (lineout_on) {
		return balanced ? 3 : 1;
	}

	if (balanced) {
		return 3; // 4.4mm balanced output
	}

	if (file_matches(sysfs_hs_switch, "1")) {
		return 2; // 3.5mm headset output
	}

	return 2; // Default to 3.5mm device
}

bool alsa_jack_plugged(void) {
	return file_matches("/sys/class/switch/headset/state", "1") || file_matches("/sys/class/switch/balance/state", "1");
}

// See alsa-controls.h. Two sockets: {1, 2} single-ended and {3} balanced.
int output_reinit_partner(int route) { return route == 3 ? 2 : 3; }

// True when the balanced line out is what the route should be carrying: the
// flag the machine driver reads while applying route 3.
static bool want_balance_lineout(void) { return lineout_on && detect_output() == 3 && !alsa_board_is_ak4493(); }

// What the output configuration is, as one number: the route and the balanced
// line-out flag together. The playback loop watches this rather than the route
// alone, because on the 4.4 mm jack headphone and line out are both route 3 and
// only the flag tells them apart -- without it, switching to line out mid-track
// would leave the stream on the old configuration until the next track.
int alsa_output_key(void) { return detect_output() * 2 + (want_balance_lineout() ? 1 : 0); }

// The DAC's four digital interpolation filters, the ALSA "Digital Filter"
// control the stock player writes (0..3), on both boards:
//   0 fast roll-off, low latency      1 fast roll-off, phase compensated
//   2 slow roll-off, low latency      3 slow roll-off, phase compensated
static int current_filter = -1;

// The AK4493's six, as its codec driver numbers them (ak4493_set_digital_filter,
// SD / SLOW / SSLOW bits):
//   0 sharp roll-off              1 slow roll-off
//   2 short delay sharp roll-off  3 short delay slow roll-off
//   4 super slow roll-off         5 low dispersion short delay
int dac_filter_count(void) { return alsa_board_is_ak4493() ? 6 : 4; }

void set_dac_filter(int filter) {
	if (filter < 0 || filter >= dac_filter_count()) {
		return;
	}
	if (filter == current_filter) {
		return; // same reasoning as the output switch: never poke it idly
	}
	current_filter = filter;
	alsa_set_control("Digital Filter", filter);
	printf("set dac filter to %d\n", filter);
}

int get_dac_filter(void) { return current_filter < 0 ? 0 : current_filter; }

// The DAC's Dynamic Range Enhancement, the ALSA "DRE_EN" switch the stock
// player writes (decompile FUN_00482120). Cached for the same reason as the
// filter and the output route: never poke the codec idly mid-stream.
static int current_dre = -1;

// DRE does not exist on the CS43131 or the AK4493 board: it stays off there
// and nothing is written.
void set_dac_dre(int enabled) {
	enabled = enabled && board_has_dre() ? 1 : 0;
	if (enabled == current_dre) {
		return;
	}
	current_dre = enabled;
	if (!board_has_dre()) {
		return;
	}
	alsa_set_control("DRE_EN", enabled);
	printf("set dac DRE to %d\n", enabled);
}

int get_dac_dre(void) {
	if (!board_has_dre()) {
		return 0;
	}
	return current_dre < 0 ? 1 : current_dre;
}

// The DAC's non-oversampling mode, the ALSA "NOS_EN" switch the stock player
// writes (decompile FUN_004820e0, settings case 0x7e). Same cached contract.
static int current_nos = -1;

void set_dac_nos(int enabled) {
	enabled = enabled && board_has_nos() ? 1 : 0;
	if (enabled == current_nos) {
		return;
	}
	current_nos = enabled;
	if (!board_has_nos()) {
		return;
	}
	alsa_set_control("NOS_EN", enabled);
	printf("set dac NOS to %d\n", enabled);
}

int get_dac_nos(void) { return current_nos < 0 ? 0 : current_nos; }

// ---------------------------------------------------------------------------
// DoP
//
// "DOP_EN" belongs to the sound card rather than the codec: its handler in
// x1600_hiby_r3proii_sound_card.ko (dop_en_put) calls cs43198_set_dsd_en()
// straight through. With it on the DAC stops reading the stream as PCM and
// starts looking for the DoP marker in the top byte of every word; with it
// off, a DoP stream is white noise at full level.
//
// So it goes on before the stream is opened and off again the moment the track
// ends. Never left on: the next track is ordinary PCM and the DAC would sit
// waiting for markers that never come.
// ---------------------------------------------------------------------------
static int current_dop = -1;

// ---------------------------------------------------------------------------
// DSD gain compensation
//
// The stock player's "DSDGain", read out of its ot_devices.json as
// {0, -2, -4, -6, -8, -10, -12} and found again in the disassembly at the DSD
// branch of its volume routine: the number is ADDED to the raw attenuation and
// the sum is clamped at zero, which is what its own "dsdl gain %d, set to 0"
// and "dsdr gain %d, set to 0" lines are about.
//
// So a negative entry does not attenuate. The CS43198 control is attenuation --
// a lower raw value is a louder output -- so subtracting from it takes
// attenuation away. That is the whole point: at the same volume index a DSD
// track comes out LOUDER than a PCM one, which is why the stock player sounds
// louder on DSD than a player that simply follows the PCM curve.
//
// One raw step is about 0.5 dB on this path: the two gain tables differ by 12
// steps at index 100 and that difference is the documented 6 dB. So the seven
// entries are 0 to +6 dB of compensation, and that is how they are put to the
// user -- the internal numbers are HiBy's and stay here.
//
// None of this touches a sample. DoP carries the DSD bitstream with a marker in
// the top byte of every word, and multiplying any of it by a gain destroys the
// stream; the compensation is a DAC register and nothing else.
#define DSD_GAIN_STEPS 7
static const int HIBY_DSD_GAIN_RAW[DSD_GAIN_STEPS] = {0, -2, -4, -6, -8, -10, -12};

static int dsd_gain_index; // 0..6; 0 is no compensation at all

// The attenuation actually written to the DAC: the curve's value, plus the DSD
// offset while a DoP stream is open. Off the DoP path this returns `raw`
// untouched, so nothing changes for PCM.
static long with_dsd_gain(long raw) {
	if (current_dop != 1 || dsd_gain_index <= 0) {
		return raw;
	}

	// Index 0 of both curves is 255, and the note over them says what that is:
	// an explicit mute rather than a point on the scale. Taking twelve levels
	// off it would leave 243 -- inaudible, but no longer the mute the user
	// asked for, and a mute that is only nearly a mute is the kind of thing
	// nobody finds again.
	if (raw >= 255) {
		return raw;
	}

	int index = dsd_gain_index;
	if (index >= DSD_GAIN_STEPS) {
		index = DSD_GAIN_STEPS - 1;
	}

	raw += HIBY_DSD_GAIN_RAW[index];
	if (raw < 0) {
		raw = 0; // no headroom left: the DAC is already at its loudest
	}
	if (raw > 255) {
		raw = 255;
	}
	return raw;
}

// The two channel controls, from the index in force. Its own function because
// three things reach it: a volume change, a gain change, and entering or
// leaving DoP -- and that last one runs on the playback thread, which has no
// business touching the USB device, the software volume or Bluetooth.
// The DAC silent through a suspend: see alsa_suspend_mute() in the header.
static bool suspend_muted;

static void write_dac_attenuation(void) {
	current_volume = with_dsd_gain(percent_to_raw(current_percent));
	// Held at mute until alsa_suspend_restore(), which writes this level: the
	// track being stopped on the way to a power-off (a DSD one leaving DoP, a
	// volume key) must not bring the music back up under the parked route.
	if (suspend_muted) {
		return;
	}
	alsa_set_control("Right Playback Volume", current_volume);
	alsa_set_control("Left Playback Volume", current_volume);
}

void alsa_suspend_mute(void) { alsa_mute_ramp(0); }

bool alsa_mute_ramp(int rate) {
	if (alsa_board_is_cs43131() || suspend_muted) {
		return false;
	}
	// The codec's own volume ramp. The AK4493's runs at the sample rate: from
	// 0 dB to mute is 4080 steps of 1/fs (93 ms at 44.1 kHz), 16 for each
	// 0.5 dB level. A stream is waited on for 120 levels at most: 60 dB under
	// what was playing, a cut there is below hearing and the rest of the ramp
	// only delays the next sound.
	long steps = 255 - current_volume < 120 ? 255 - current_volume : 120;
	long ms = !alsa_board_is_ak4493() ? 30 : rate <= 0 ? 120 : steps * 16000L / rate + 5;
#ifndef HOST_BUILD
	alsa_set_control("Right Playback Volume", 255);
	alsa_set_control("Left Playback Volume", 255);
	usleep(ms * 1000);
#endif
	suspend_muted = true;
	fprintf(stderr, "alsa: DAC ramped to mute (%ld ms)\n", ms);
	return true;
}

void alsa_suspend_restore(void) {
	if (!suspend_muted) {
		return;
	}
	suspend_muted = false;
#ifndef HOST_BUILD
	write_dac_attenuation();
	usleep(30 * 1000);
#endif
	fprintf(stderr, "alsa: DAC level restored\n");
}

int alsa_dsd_gain_index(void) { return dsd_gain_index; }

void alsa_set_dsd_gain_index(int index) {
	if (index < 0) {
		index = 0;
	}
	if (index >= DSD_GAIN_STEPS) {
		index = DSD_GAIN_STEPS - 1;
	}
	if (index == dsd_gain_index) {
		return;
	}
	dsd_gain_index = index;
	// stderr and not printf, unlike its neighbours in this file: stdout is fully
	// buffered when the player's output is a file, so those lines reach a log
	// late or not at all.
	if (index > 0) {
		fprintf(stderr, "dsd gain: +%d dB (raw offset %d)\n", index, HIBY_DSD_GAIN_RAW[index]);
	} else {
		fprintf(stderr, "dsd gain: off\n");
	}

	// Only matters while a DSD track is open; otherwise the next set_dac_dop(1)
	// picks it up.
	if (current_dop == 1) {
		write_dac_attenuation();
	}
}

void set_dac_dop(int enabled) {
	enabled = enabled ? 1 : 0;
	if (enabled == current_dop) {
		return;
	}
	current_dop = enabled;
	alsa_set_control("DOP_EN", enabled);
	usbaudio_set_dop(enabled);
	printf("set dac DoP to %d\n", enabled);

	// The attenuation the DAC should be at is not the same on the two sides of
	// this switch, so it is written again on both edges. Leaving it out would
	// give the first seconds of a DSD track the PCM level, and -- worse -- leave
	// the DSD boost on the PCM track that follows it.
	//
	// Only when there is a boost to apply: with the compensation off this is a
	// setting nobody chose and a mixer write nobody needs.
	if (dsd_gain_index > 0) {
		write_dac_attenuation();
	}
}

int get_dac_dop(void) { return current_dop < 0 ? 0 : current_dop; }

// ---------------------------------------------------------------------------
// After a system suspend -- and why this has no caller
//
// The CS43198 keeps its registers across "mem": every AXP2101 rail carries
// suspend_volage equal to work_voltage, so nothing is lowered, and the cs43198
// driver's two PM callbacks are empty returns. Volume, digital filter, DRE and
// NOS all survive, so there is nothing to write again.
//
// What does go down is the HBC3000 audio bridge. Its suspend callback powers
// it off and the card's suspend_pre resets the driver's route to 0, so the
// first route write after "mem" is a real change and runs hbc3000_enable()
// again: audio_start_output_reinit_after_resume() makes that write.
//
// Calling this would mean i2c traffic to the codec on a bit-banged bus whose
// pins nothing reprograms at resume, for no gain.
// ---------------------------------------------------------------------------
void alsa_controls_reapply(void) {
	int filter = get_dac_filter();
	int dre = get_dac_dre();
	int nos = get_dac_nos();
	int percent = current_percent;

	current_output = -1; // force the route to be written again
	current_balance_lineout = -1; // and the line-out flag with it
	current_filter = -1;
	current_dre = -1;
	current_nos = -1;
	// DoP is not re-applied here -- a resume restarts the track, and the play
	// path sets it. Dropping the cache is what matters, so that the next write
	// is not skipped.
	current_dop = -1;

	auto_set_output();
	set_dac_filter(filter);
	set_dac_dre(dre);
	set_dac_nos(nos);
	// Volume and gain live in the same register. In line out the level is the
	// fixed index and set_volume_percent refuses to move it, so the hardware
	// write goes through the same path the mode itself uses.
	if (lineout_on) {
		apply_volume_hw(LINEOUT_VOLUME_INDEX);
	} else {
		set_volume_percent(percent);
	}

	// stderr because printf is block-buffered into the log file, and this line
	// has to reach the log when it happens, not when the buffer fills.
	fprintf(stderr, "alsa: controls re-applied after resume (route, %d%%, filter %d, DRE %d, NOS %d)\n", percent,
			filter, dre, nos);
}

// Writes the route to the mixer forcing a real transition, by writing a
// different valid route an instant before the wanted one.
//
// The machine driver (codec_board_ot_put) skips the entire port
// reconfiguration -- mute -> HBC3000/GPIO -> unmute -- when the requested route
// equals the one it already holds in RAM ("X == current_route -> return"). On
// the 4.4 balanced output that leaves PO_BAL_MUTE asserted: the jack is
// detected and the software route says 3, but the balanced amplifier is never
// unmuted, so there is no audio. Writing a different route (Y) first and then
// the wanted one (X) makes the driver run the full sequence.
//
// Safe only with the PCM closed, which every caller of auto_set_output() is
// (routing happens before opening, and a reroute closes first). Writing the
// route on a live stream stalls the DMA.
static int write_output_route(int route) {
	// A different valid route, to break the "X == X" shortcut -- but one from
	// the same socket as the target wherever there is a choice. Routes 1 and 2
	// are the two faces of the 3.5 mm jack and the driver runs nothing when it
	// moves between them, so they can stand in for each other for free; route 3
	// cannot: passing through it on the way to a single-ended route would hold
	// a balanced reconfiguration, for the delay below, with a single-ended plug
	// in the socket.
	int other = (route == 3) ? 2 : (route == 2 ? 1 : 2);
	// Said before the writes: the boot freeze after an update is suspected inside them.
	printf("set output: writing route %d, then %d\n", other, route);
	alsa_set_control("Output Port Switch", other);
	usleep(30 * 1000); // the driver mutes and needs a short delay before the change
	return alsa_set_control("Output Port Switch", route);
}

// Sets ALSA "Output Port Switch" from what is plugged in, and only when it has
// actually changed: poking the output switch while the previous track is still
// streaming stalls the audio DMA on this hardware, freezing the old stream for
// seconds and ending it in "write error: Input/output error". The
// current_output cache is what keeps the route from being touched mid-track.
void auto_set_output(void) {
	alsa_wait_output_reinit();

	// The CS43131 board: one route, no line-out flag, no transition to force.
	if (alsa_board_is_cs43131()) {
		int route = detect_output();
		if (route != current_output && alsa_set_control("Output Port Switch", route) >= 0) {
			current_output = route;
			printf("set output to %d\n", route);
		}
		return;
	}

	// Read once: the jack can be pulled between two reads, and the route and
	// the flag have to be decided from the same picture of the world.
	int output = detect_output();
	int flag = (lineout_on && output == 3) ? 1 : 0;
	bool flag_changed = flag != current_balance_lineout;

	// The Blaze's driver has no such flag (see alsa_board_is_ak4493()): writing
	// it would fail on every call and leave the cache never settling.
	if (alsa_board_is_ak4493()) {
		flag = 0;
		flag_changed = false;
	}

	// The flag first and always, because the driver reads it while applying the
	// route: on the balanced jack the pair is what decides headphone or line
	// out. Written even for the 3.5 mm route, so that coming back to the
	// balanced hole cannot find a stale 1 still set.
	if (flag_changed) {
		if (alsa_set_control("Balance Lineout En", flag) < 0) {
			// The cache must never claim a write that did not happen: the next
			// call would skip it and the driver would read the old flag while
			// applying the route.
			flag_changed = false;
		} else {
			current_balance_lineout = flag;
		}
	}

	if (output == current_output && !flag_changed) {
		return;
	}

	if (output == current_output) {
		// Same route, different flag: this write looks redundant and is not.
		// It is what makes the driver run its HBC3000 reconfiguration for the
		// new flag -- that step happens before the "same route, nothing to do"
		// shortcut -- so it must not be optimised away.
		alsa_set_control("Output Port Switch", output);
		printf("set output to %d (balanced line out %d)\n", output, flag);
		return;
	}

	// From route 5 (the amplifier parked off) any route is a real change, as
	// the stock player writes it after its idle park: the detour through
	// "other" would hold a single-ended route with a balanced plug in.
	int ok = current_output == 5 ? alsa_set_control("Output Port Switch", output) : write_output_route(output);
	if (ok >= 0) {
		current_output = output;
	}

	printf("set output to %d\n", output);
}

// Realigns the route cache after something else wrote "Output Port Switch"
// directly (the route parked before mem), so the cache never claims a route
// the driver does not hold.
void alsa_controls_note_output(int value) { current_output = value; }

int alsa_controls_output(void) { return (int)current_output; }

void alsa_controls_rewrite_output(void) {
	alsa_wait_output_reinit();

	int output = detect_output();
	int flag = (lineout_on && output == 3) ? 1 : 0;

	// The flag first: the driver reads it while applying route 3. The Blaze's
	// driver has no such control (see auto_set_output()).
	current_balance_lineout = -1;
	if (alsa_board_is_ak4493()) {
		current_balance_lineout = 0;
	} else if (alsa_set_control("Balance Lineout En", flag) >= 0) {
		current_balance_lineout = flag;
	}
	current_output = -1;
	if (alsa_set_control("Output Port Switch", output) >= 0) {
		current_output = output;
	}
	printf("set output to %d after suspend (balanced line out %d)\n", output, flag);
}

// Raw CS43198 attenuation, 0-255, applied to both channels.
void set_volume(long volume) {
	current_volume = volume;

	alsa_set_control("Right Playback Volume", current_volume);
	alsa_set_control("Left Playback Volume", current_volume);

	printf("set volume to raw %ld\n", current_volume);
}

// returns the last volume value set via set_volume()/change_volume()
long get_volume(void) { return current_volume; }

// ---------------------------------------------------------------------------
// Two volumes, one knob
//
// 55 in a pair of earbuds and 50 on a desk amp are not the same choice, so a
// level belongs to a profile and the profile follows wherever the sound is
// going. All four levels are held here, beside the one the hardware is actually
// at; every change lands in whichever profile is current, and the swap is
// driven by the audio routing rather than by a config key.
//
// Four profiles, one per place the sound can come out of. The jack is two of
// them and not one: 4.4 mm and 3.5 mm are different amplifier configurations
// into different headphones, so a level that is right for one is a level
// nobody chose for the other. USB-C is the fourth, and its level goes to the
// device out there rather than to the CS43198, so it is not even the same
// attenuation.
static volume_output_t profile_out = VOLUME_OUTPUT_PHONES;
static int level[VOLUME_OUTPUT_COUNT] = {-1, -1, -1, -1}; // -1 = never set, so the first reading adopts it
static bool profile_dirty;

// Where each level is written down. The first and the last are the historical
// key names, so a player updating from a build with only two profiles finds its
// jack level and its Bluetooth level exactly where it left them; the other two
// outputs simply start without one.
static const struct {
	const char *section;
	const char *key;
} PROFILE_KEYS[VOLUME_OUTPUT_COUNT] = {
	[VOLUME_OUTPUT_PHONES] = {"player", "volume"},
	[VOLUME_OUTPUT_BALANCED] = {"player", "volume_balanced"},
	[VOLUME_OUTPUT_USB] = {"player", "volume_usb"},
	[VOLUME_OUTPUT_BLUETOOTH] = {"wireless", "bt_volume"},
};

static const char *PROFILE_NAMES[VOLUME_OUTPUT_COUNT] = {
	[VOLUME_OUTPUT_PHONES] = "3.5 mm",
	[VOLUME_OUTPUT_BALANCED] = "4.4 mm",
	[VOLUME_OUTPUT_USB] = "usb-c",
	[VOLUME_OUTPUT_BLUETOOTH] = "bluetooth",
};

static bool output_in_range(volume_output_t out) { return out >= 0 && out < VOLUME_OUTPUT_COUNT; }

// ---------------------------------------------------------------------------
// The volume limit (see alsa-controls.h)
// ---------------------------------------------------------------------------

static bool limit_loaded;
static bool limit_on;
static int limit_level[VOLUME_OUTPUT_COUNT] = {100, 100, 100, 100};

static const char *const LIMIT_KEYS[VOLUME_OUTPUT_COUNT] = {
	[VOLUME_OUTPUT_PHONES] = "volume_limit_phones",
	[VOLUME_OUTPUT_BALANCED] = "volume_limit_balanced",
	[VOLUME_OUTPUT_USB] = "volume_limit_usb",
};

static int limit_clamp_value(int percent) {
	return percent < VOLUME_LIMIT_MIN ? VOLUME_LIMIT_MIN : (percent > 100 ? 100 : percent);
}

// Read on first use rather than at init: the first level is applied by
// audio_init(), after the config is loaded but before anything here is set up.
static void limit_load(void) {
	if (limit_loaded) {
		return;
	}
	limit_loaded = true;
	limit_on = config_get_int("audio", "volume_limit", 0) != 0;
	for (int i = 0; i < VOLUME_OUTPUT_COUNT; i++) {
		if (LIMIT_KEYS[i]) {
			limit_level[i] = limit_clamp_value((int)config_get_int("audio", LIMIT_KEYS[i], 100));
		}
	}
}

// The ceiling in force for `out` right now: 100 with the limit off and for the
// outputs it does not cover.
static int limit_ceiling(volume_output_t out) {
	limit_load();
	if (!limit_on || !output_in_range(out) || !LIMIT_KEYS[out]) {
		return 100;
	}
	return limit_level[out];
}

bool volume_limit_enabled(void) {
	limit_load();
	return limit_on;
}

int volume_limit(volume_output_t out) {
	limit_load();
	if (!output_in_range(out) || !LIMIT_KEYS[out]) {
		return 100;
	}
	return limit_level[out];
}

static void apply_volume_hw(int percent);

// Brings the remembered level of `out` under its ceiling, and the hardware with
// it when that is the output playing. Line out holds its own level and keeps
// it; the user's level underneath it is what comes down.
static void limit_enforce(volume_output_t out) {
	int ceiling = limit_ceiling(out);
	if (level[out] > ceiling) {
		level[out] = ceiling;
		profile_dirty = true;
	}
	if (out != profile_out) {
		return;
	}
	if (lineout_on) {
		if (lineout_saved_percent > ceiling) {
			lineout_saved_percent = ceiling;
		}
	} else if (current_percent > ceiling) {
		profile_dirty = true;
		apply_volume_hw(ceiling);
	}
}

void volume_limit_set_enabled(bool on) {
	limit_load();
	limit_on = on;
	config_set_int("audio", "volume_limit", on ? 1 : 0);
	config_save();
	if (on) {
		limit_enforce(VOLUME_OUTPUT_PHONES);
		limit_enforce(VOLUME_OUTPUT_BALANCED);
		limit_enforce(VOLUME_OUTPUT_USB);
		volume_profile_persist();
	}
}

// ---------------------------------------------------------------------------
// The volume lock (see alsa-controls.h)
// ---------------------------------------------------------------------------

// Read by the key threads and written by the interface. -1 until the config
// has been asked, which happens on the first question rather than at init for
// the same reason as the limit's.
static volatile int lock_on = -1;

bool volume_lock_enabled(void) {
	int on = lock_on;
	if (on < 0) {
		on = config_get_int("audio", "volume_lock", 0) != 0;
		lock_on = on;
	}
	return on != 0;
}

void volume_lock_set_enabled(bool on) {
	lock_on = on ? 1 : 0;
	config_set_int("audio", "volume_lock", on ? 1 : 0);
	config_save();
	printf("volume: %s\n", on ? "locked" : "unlocked");
}

void volume_limit_set(volume_output_t out, int percent) {
	limit_load();
	if (!output_in_range(out) || !LIMIT_KEYS[out]) {
		return;
	}
	limit_level[out] = limit_clamp_value(percent);
	config_set_int("audio", LIMIT_KEYS[out], limit_level[out]);
	limit_enforce(out);
}

void volume_profile_set_level(volume_output_t out, int percent) {
	if (!output_in_range(out)) {
		return;
	}
	level[out] = (percent >= 0 && percent <= 100) ? percent : -1;
	if (level[out] > limit_ceiling(out)) {
		level[out] = limit_ceiling(out);
	}
}

void volume_profile_init(int wired_percent, int bt_percent) {
	for (int i = 0; i < VOLUME_OUTPUT_COUNT; i++) {
		level[i] = -1;
	}
	volume_profile_set_level(VOLUME_OUTPUT_PHONES, wired_percent);
	volume_profile_set_level(VOLUME_OUTPUT_BLUETOOTH, bt_percent);
	profile_dirty = false;
}

volume_output_t volume_profile_output(void) { return profile_out; }

bool volume_profile_is_bluetooth(void) { return profile_out == VOLUME_OUTPUT_BLUETOOTH; }

int volume_profile_level(bool bluetooth) {
	return bluetooth ? level[VOLUME_OUTPUT_BLUETOOTH] : level[VOLUME_OUTPUT_PHONES];
}

static void write_profiles(void) {
	for (int i = 0; i < VOLUME_OUTPUT_COUNT; i++) {
		if (level[i] >= 0) {
			config_set_int(PROFILE_KEYS[i].section, PROFILE_KEYS[i].key, level[i]);
		}
	}
	config_save();
}

// Writes the levels down, but only when one of them has moved. Called from a
// poll rather than from every step of a held volume key, so a long press costs
// one config write instead of fifty.
void volume_profile_persist(void) {
	if (!profile_dirty) {
		return;
	}
	profile_dirty = false;
	write_profiles();
}

// The same, whether or not anything has moved: for the moment the user switches
// "Remember volume" on and every level in hand becomes the one to come back to.
void volume_profile_persist_now(void) {
	profile_dirty = false;
	write_profiles();
}

void volume_profile_set_output(volume_output_t out) {
	if (!output_in_range(out) || out == profile_out) {
		return;
	}

	// Sound going out over the air, or out of a socket the mode was not
	// switched on for, is not a line out. Leaving the mode here puts the old
	// output back on the user's level before the profiles are swapped, so the
	// fixed index cannot be mistaken for the level it was left at.
	if (lineout_on) {
		lineout_set(false);
	}

	level[profile_out] = current_percent; // the level the outgoing profile is at
	volume_output_t leaving = profile_out;
	profile_out = out;
	profile_dirty = true;

	int wanted = level[out];
	if (wanted < 0) {
		wanted = current_percent; // no level for this output yet: keep the one in hand
	}
	printf("volume: %s -> %s, %d%%\n", PROFILE_NAMES[leaving], PROFILE_NAMES[out], wanted);
	set_volume_percent(wanted);
	volume_profile_persist();
}

void volume_profile_set_bluetooth(bool bluetooth) {
	if (bluetooth) {
		volume_profile_set_output(VOLUME_OUTPUT_BLUETOOTH);
	} else if (profile_out == VOLUME_OUTPUT_BLUETOOTH) {
		volume_profile_set_output(VOLUME_OUTPUT_PHONES);
	}
}

int get_volume_percent(void) { return current_percent; }

// The level, on the hardware, without touching the two saved profile levels.
// This is the "apply the volume index" of the stock player: it walks the gain
// table in force, so the index is a position on the user's own curve and not a
// number written into the DAC. Line out uses it to hold index 100 without
// making 100 the level the jack is remembered at.
static void apply_volume_hw(int percent) {
	current_percent = percent;
	write_dac_attenuation();

	// And, if the sound is leaving over the USB-C port, on the device out
	// there: the CS43198's attenuation is not in that path at all. Free when
	// nothing is plugged in -- the call looks at one integer and returns.
	usbaudio_apply_volume(percent);

	// ...and, when that device turns out to have no volume control of its own,
	// on the samples themselves before they are handed to it. Which of the two
	// answers is in force is decided in swvolume.c; this call only says what
	// the index is now.
	swvolume_set_index(percent);

	// The DAC's attenuation does nothing to a stream that is being encoded
	// and sent over the air, so connected headphones are told separately over
	// AVRCP. Free when Bluetooth is off or the setting is: the call looks at
	// two booleans and returns.
	bluetooth_notify_volume(percent);

	// The DSD offset is named when it is in force, because otherwise the raw
	// value in this line does not follow from the index and the gain table and
	// the next person reading a log has no way to tell why. On stderr for that
	// same reason -- see the note in alsa_set_dsd_gain_index().
	if (current_dop == 1 && dsd_gain_index > 0) {
		fprintf(stderr, "set volume to %d%% (raw %ld, -%.1f dB, dsd +%d dB)\n", percent, current_volume,
				current_volume * 0.5, dsd_gain_index);
	} else {
		printf("set volume to %d%% (raw %ld, -%.1f dB)\n", percent, current_volume, current_volume * 0.5);
	}
}

void set_volume_percent(int percent) {
	if (percent < 0) {
		percent = 0;
	}
	if (percent > 100) {
		percent = 100;
	}

	// Line out is a fixed level. Refusing here rather than in every caller is
	// what keeps the volume keys, the slider and the Bluetooth notifications
	// from pulling the output off the index the mode exists to hold.
	if (lineout_on) {
		return;
	}

	// And no higher than the ceiling of the socket in use.
	int ceiling = limit_ceiling(profile_out);
	if (percent > ceiling) {
		percent = ceiling;
	}

	if (current_percent != percent || level[profile_out] != percent) {
		profile_dirty = true;
	}
	level[profile_out] = percent;

	apply_volume_hw(percent);
}

void change_volume_percent(int delta) { set_volume_percent(get_volume_percent() + delta); }

bool lineout_is_active(void) { return lineout_on; }

// Writes the route from here only when nothing is streaming. Reprogramming the
// codec under a live PCM stalls the audio DMA on this hardware -- it is the
// whole reason auto_set_output() has a cache -- and this function is called
// from the GUI thread, which knows nothing about where the playback thread is.
// While a track is running the mode change is left to that thread: it compares
// alsa_output_key() every few buffers and reroutes between two writes, which is
// the one moment no stream is live.
static void lineout_apply_route(void) {
	if (audio_get_status() != AUDIO_STATUS_STOPPED) {
		return;
	}
	auto_set_output();
}

void lineout_set(bool on) {
	if (on == lineout_on) {
		return;
	}

	if (on) {
		// Sound going out over the air has no jack to drive, and the level the
		// mode forces would be sent to the headphones as an absolute volume.
		if (volume_profile_is_bluetooth()) {
			fprintf(stderr, "alsa: line out refused, the sound is going over bluetooth\n");
			return;
		}

		// And an empty socket is nothing to drive either. Refusing here is what
		// lets the rule below be simple: the mode belongs to one jack, and any
		// change of jack ends it. Plug the cable, then switch it on.
		int jack = headphone_jack_state();
		if (jack == JACK_NONE) {
			fprintf(stderr, "alsa: line out refused, nothing is plugged in\n");
			return;
		}

		// The level to come back to. Taken before the mode is on, so it is the
		// user's own and not the fixed one.
		lineout_saved_percent = current_percent;
		lineout_jack = jack;
		lineout_on = true;
		lineout_apply_route(); // the flag and the route, in that order
		apply_volume_hw(LINEOUT_VOLUME_INDEX);
		fprintf(stderr, "alsa: line out on (jack %d, index %d)\n", lineout_jack, LINEOUT_VOLUME_INDEX);
		return;
	}

	// Out: the level first, then the route, and not the other way round. The
	// route write ends with the driver unmuting the headphone path, and the two
	// mixer round trips between that and a later volume write would be spent
	// with headphones on the socket at the line-out level.
	int back = lineout_saved_percent >= 0 ? lineout_saved_percent : current_percent;
	lineout_saved_percent = -1;
	lineout_on = false;
	apply_volume_hw(back);
	lineout_apply_route();
	fprintf(stderr, "alsa: line out off (back to %d%%)\n", back);
}

// The mode belongs to the jack it was switched on for. Pulling that jack out --
// or putting a different one in -- ends it, because the fixed level is not
// something to hand to whatever is plugged in next, and while the mode is on
// the volume keys cannot bring it down. Called from the status bar's poll,
// which is already reading the jack once a second.
void lineout_check_jack(void) {
	if (!lineout_on) {
		return;
	}
	if (headphone_jack_state() == lineout_jack) {
		return;
	}
	fprintf(stderr, "alsa: the jack changed, line out off\n");
	lineout_set(false);
}

int gain_level_count(void) { return alsa_board_is_ak4493() ? 3 : 2; }

// Switching gain re-applies the current volume so the shift takes effect
// immediately, exactly like changing the stock player's gain option.
void set_gain_level(int level) {
	if (level < 0) {
		level = 0;
	}
	if (level >= gain_level_count()) {
		level = gain_level_count() - 1;
	}
	if (level == gain_level) {
		return;
	}
	gain_level = level;
	printf("set gain to %d of %d\n", level, gain_level_count() - 1);
	// The gain is which of the tables the index is read from, so the level
	// has to be applied again either way -- including in line out, where
	// set_volume_percent refuses and the fixed index is the one to rewrite.
	if (lineout_on) {
		apply_volume_hw(LINEOUT_VOLUME_INDEX);
	} else {
		set_volume_percent(current_percent);
	}
}

int get_gain_level(void) { return gain_level; }

void set_high_gain(int enabled) { set_gain_level(enabled ? gain_level_count() - 1 : 0); }

int get_high_gain(void) { return gain_level == gain_level_count() - 1; }


// Which jack is occupied right now: reads the same switch nodes the output
// route detection uses. Balanced wins when both report plugged.
int headphone_jack_state(void) {
	if (file_matches("/sys/class/switch/balance/state", "1")) {
		return JACK_BALANCED;
	}
	if (file_matches("/sys/class/switch/headset/state", "1")) {
		return JACK_HEADSET;
	}
	return JACK_NONE;
}

// Raw attenuation delta, clamped to the 0-255 register range.
void change_volume(long amount) {
	current_volume += amount;

	if (current_volume > 255) {
		current_volume = 255;
	}

	if (current_volume < 0) {
		current_volume = 0;
	}

	set_volume(current_volume);
}
