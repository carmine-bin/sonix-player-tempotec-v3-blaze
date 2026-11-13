#define _GNU_SOURCE 1

#include "usbdac.h"

#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/axpcharge.h"
#include "src/system/device/power.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// The gadget lives at the same path as the mass-storage one, and that is not a
// coincidence to be worked around: the stock firmware calls its UAC gadget
// `android0` too, because a USB controller takes exactly one gadget and the
// two modes are alternatives. So entering DAC mode empties that gadget's
// configuration and puts the audio function in it; leaving puts the disk back.
// Overridable only so the build/teardown can be exercised against a fake
// configfs tree in a test -- see /tmp/dactest. Nothing in the player sets it.
#ifndef GADGET
#define GADGET "/sys/kernel/config/usb_gadget/android0"
#endif
#define UAC_FUNC GADGET "/functions/uac_sa.a"
#define UAC_NODE "/dev/uac_sa"

// The ioctls the driver answers, with the numbers the stock player passes.
#define UAC_IOCTL_STATE 0	// int: 0 idle, >0 the host is streaming
#define UAC_IOCTL_FORMAT 1	// struct uac_format
#define UAC_IOCTL_CAPTURE 2 // int: 1 if this build can also record

// Filled by the kernel's uac_ioctl: `format` is 2 while the host has the
// native DSD alternate setting (alt 4) selected and 1 otherwise; `flags` is
// always 32, the subslot width, whatever the host sends.
struct uac_format {
	int format; // 2 = native DSD, 1 = PCM
	int rate;	// what the host asked for
	int flags;	// always 32
};

// The node appears only once the function is bound and the host has looked at
// it. The stock player waits 499 times 10 ms; the same number here, since
// whatever it is waiting for takes just as long either way.
#define NODE_WAIT_TRIES 499
#define NODE_WAIT_US 10000

// How long to wait before asking again whether the host is streaming. Two
// different waits: 100 ms is right while nothing is playing, and far too long
// in the middle of a stream, where a buffer lasts 10 ms.
#define IDLE_POLL_US 100000	  // nothing playing: look again in a moment
#define GAP_POLL_US 2000	  // mid-stream: a gap between buffers, not an end

// A stream is not over because ioctl 0 said 0 once: it says 0 in the gaps
// between buffers, which on a 10 ms buffer is most of the time. The output is
// only let go after the host has been quiet for this long: far longer than any
// gap, far shorter than a person notices.
#define IDLE_CLOSE_MS 1500

// How deep the output buffer is, and therefore how far behind the computer the
// socket runs.
//
// Left to itself audio.c opens eight periods of 4096 frames -- 32768 frames,
// which is two thirds of a second at 48 kHz -- and the stream only starts once
// the whole buffer is full, so that depth IS the delay, start to finish. That
// sizing is for local playback, where the decoding thread can stall for most of
// a second while a big cover is read off the card. Nothing here can: the samples
// arrive from the host at the rate the host clocks them, ten milliseconds at a
// time, and the only thing between the cable and the DAC is one read and one
// write.
//
// Lower is tighter and closer to an underrun, which on this path is a click
// rather than a stall. [usb] dac_output_ms moves it.
#define OUTPUT_MS_DEFAULT 80
#define OUTPUT_MS_MIN 20
#define OUTPUT_MS_MAX 500

static int output_ms(void) {
	int ms = (int)config_get_int("usb", "dac_output_ms", OUTPUT_MS_DEFAULT);
	return ms < OUTPUT_MS_MIN ? OUTPUT_MS_MIN : (ms > OUTPUT_MS_MAX ? OUTPUT_MS_MAX : ms);
}

// Ten milliseconds of audio, sized the way the stock player sizes it:
// rate * 8 / 100 bytes, eight bytes per frame -- stereo, 32 bits a sample --
// rounded up to a multiple of eight so a frame is never split.
static int frame_buffer_bytes(int rate) {
	int bytes = (rate * 8) / 100;
	return (bytes + 7) & ~7;
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static usbdac_state_t state;
static uint32_t last_audible_ms; // under `lock`: the last read that was not digital silence

// How long after the last read with sound in it the host still counts as
// playing, for usbdac_is_audible(): the quiet between two tracks included.
#define AUDIBLE_MS 5000

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
}
static unsigned serial;

static pthread_t audio_thread;
static bool thread_valid;
static pthread_t start_thread;
static bool start_thread_valid;
static volatile bool running;
static int node_fd = -1;

static void touch(void) {
	serial++;
}

static void set_error(const char *fmt, ...) {
	va_list ap;
	pthread_mutex_lock(&lock);
	va_start(ap, fmt);
	vsnprintf(state.error, sizeof(state.error), fmt, ap);
	va_end(ap);
	touch();
	pthread_mutex_unlock(&lock);
	fprintf(stderr, "usbdac: %s\n", state.error);
}

// ---------------------------------------------------------------------------
// The gadget
// ---------------------------------------------------------------------------

static bool write_file(const char *path, const char *value) {
	int fd = open(path, O_WRONLY);
	if (fd < 0) {
		return false;
	}
	ssize_t n = write(fd, value, strlen(value));
	close(fd);
	return n >= 0;
}

// Like write_file(), but says when it could not. Used for the attributes whose
// absence is a symptom rather than a shrug -- a missing product string is why
// a host calls the device "Unknown", and a missing feedback endpoint is why a
// stream dies on the first second.
static void write_noted(const char *path, const char *value) {
	if (!write_file(path, value)) {
		fprintf(stderr, "usbdac: could not write %s (%s)\n", path, strerror(errno));
	}
}

static bool path_exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0;
}

// Everything the stock uac_device_config.sh does, minus its `mount -t configfs`
// (configfs is already mounted here, which is why that script refuses to run on
// this firmware) and plus the one step that is easy to miss: soft_disconnect.
//
// A USB host does not re-read the descriptors of a device that never went
// away. Swapping the gadget behind a cable that stays plugged in leaves the
// computer believing it still has the disk it enumerated a minute ago, and no
// audio device appears. Dropping and raising the data-line pull-up is the
// electrical equivalent of unplugging and replugging, and without it this mode
// looks completely dead from the other end.
static void force_reenumeration(void) {
	static const char *const PATHS[] = {
		"/sys/class/usb_gadget/android0/soft_disconnect",
		GADGET "/soft_disconnect",
		NULL,
	};
	for (int i = 0; PATHS[i]; i++) {
		if (!path_exists(PATHS[i])) {
			continue;
		}
		write_file(PATHS[i], "1");
		usleep(300 * 1000);
		write_file(PATHS[i], "0");
		printf("usbdac: re-enumeration forced via %s\n", PATHS[i]);
		return;
	}
	// No soft_disconnect: unbinding and rebinding the UDC is the next best
	// thing, and gadget_build() has already done exactly that.
	printf("usbdac: no soft_disconnect; relying on the UDC rebind\n");
}

// Removes the function symlinks from the configuration, leaving the plain
// attributes (MaxPower, bmAttributes, strings) alone.
//
// Only the symlinks go: `configs/c.1` also holds `strings`, which is a real
// directory and has to survive.
static void unlink_functions(void) {
	DIR *dir = opendir(GADGET "/configs/c.1");
	if (!dir) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		char path[512];
		snprintf(path, sizeof(path), GADGET "/configs/c.1/%s", de->d_name);
		struct stat st;
		if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) {
			unlink(path);
		}
	}
	closedir(dir);
}

static bool read_first_line(const char *path, char *out, size_t out_size) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	bool ok = fgets(out, (int)out_size, f) != NULL;
	fclose(f);
	if (ok) {
		out[strcspn(out, "\r\n")] = '\0';
	}
	return ok;
}

// The last six digits of the chip id, the way the stock script does it, so two
// players on one desk are told apart by the host.
static const char *chip_serial(void) {
	static char serial[32];
	char line[128];
	if (read_first_line("/proc/jz/efuse/efuse_chip_id", line, sizeof(line))) {
		char *second = strchr(line, ' ');
		if (second) {
			snprintf(serial, sizeof(serial), "%.6s", second + 1);
			if (serial[0]) {
				return serial;
			}
		}
	}
	return "123456";
}

static bool first_udc(char *out, size_t out_size) {
	DIR *dir = opendir("/sys/class/udc");
	if (!dir) {
		return false;
	}
	bool found = false;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}
		snprintf(out, out_size, "%s", de->d_name);
		found = true;
		break;
	}
	closedir(dir);
	return found;
}

// The native DSD alternate setting (alt 4: UAC2 raw data, 32-bit subslots).
// The kernel's uac_sa reads its dsd_native_enable flag in afunc_bind and only
// creates the attribute later in that same bind, so the first bind after boot
// always goes out without it: the flag is written once the attribute exists,
// and the function bound again to pick it up. The flag is a kernel global that
// outlives the unbind, so later entries into DAC mode find it already set and
// skip the second bind. The stock player always sets it ("DSDMode": 1), and so
// does this one: a [usb] dac_dsd_native left by an older test build is ignored.
#define DSD_NATIVE_ATTR "/sys/class/android_usb/f_uac_sa/dsd_native_enable"

static bool apply_dsd_native(const char *udc) {
	const bool want = true;
	char value[16];
	if (!read_first_line(DSD_NATIVE_ATTR, value, sizeof(value))) {
		fprintf(stderr, "usbdac: %s missing; no native DSD\n", DSD_NATIVE_ATTR);
		return true;
	}
	bool on = strncmp(value, "on", 2) == 0 || value[0] == '1';
	if (on == want) {
		return true;
	}
	if (!write_file(DSD_NATIVE_ATTR, want ? "on" : "off")) {
		fprintf(stderr, "usbdac: could not write %s (%s)\n", DSD_NATIVE_ATTR, strerror(errno));
		return true;
	}
	write_file(GADGET "/UDC", "\n");
	if (!write_file(GADGET "/UDC", udc)) {
		set_error(tr("usbdac_bind_failed"), udc, strerror(errno));
		return false;
	}
	printf("usbdac: native DSD %s; gadget bound again\n", want ? "on" : "off");
	return true;
}

// Each step separately, with each failure named: a build that fails has to say
// which write failed and with what errno, because every one has a different
// cause.
static bool gadget_build(void) {
	char path[512];

	// Off the controller first: one controller takes one gadget, and a bound
	// configuration cannot be changed. Neither write is required to succeed --
	// the gadget may not exist, or may already be idle.
	write_file("/sys/kernel/config/usb_gadget/adb_demo/UDC", "\n");
	write_file(GADGET "/UDC", "\n");

	if (!path_exists(GADGET)) {
		if (mkdir(GADGET, 0755) != 0 && errno != EEXIST) {
			set_error(tr("usbdac_gadget_failed"), strerror(errno));
			return false;
		}
	}

	unlink_functions();

	// The test that matters: in configfs a function the kernel does not have
	// cannot be created, so this mkdir -- and only this mkdir -- says whether
	// this firmware can do USB audio at all.
	if (mkdir(UAC_FUNC, 0755) != 0 && errno != EEXIST) {
		set_error(tr("usbdac_no_uac"), strerror(errno));
		return false;
	}

	// The stream format. c_* is the host-to-device direction, the only one this
	// gadget declares.
	write_file(UAC_FUNC "/c_chmask", "0x03");
	write_file(UAC_FUNC "/c_ssize", "2");

	// The whole range the function advertises, not the single 48000 the stock
	// script writes -- the kernel's own uac_sa carries the string "32k~384khz"
	// and lets the host choose. If this build wants one value, fall back.
	if (!write_file(UAC_FUNC "/c_srate",
					"32000,44100,48000,88200,96000,176400,192000,352800,384000")) {
		write_file(UAC_FUNC "/c_srate", "48000");
	}

	// 239/2/1: misc, common class, interface association -- how a composite
	// audio device announces itself.
	write_file(GADGET "/bDeviceClass", "239");
	write_file(GADGET "/bDeviceSubClass", "2");
	write_file(GADGET "/bDeviceProtocol", "1");
	write_file(GADGET "/bcdUSB", "0x200");
	write_file(GADGET "/bcdDevice", "0x100");

	// No IN stream. The kernel defaults p_chmask to 3, which would declare a
	// capture interface -- the device as a microphone -- that nothing here ever
	// feeds, and a host that opens a streaming interface which never delivers a
	// packet is entitled to give up on the whole device. The stock player can
	// afford the default because it pumps snd_pcm_readi() into that endpoint
	// from a second thread.
	write_file(UAC_FUNC "/p_chmask", "0");

	mkdir(GADGET "/strings/0x409", 0755);
	mkdir(GADGET "/configs/c.1", 0755);
	mkdir(GADGET "/configs/c.1/strings/0x409", 0755);
	write_file(GADGET "/configs/c.1/MaxPower", "10");
	write_file(GADGET "/configs/c.1/bmAttributes", "0xC0");
	write_file(GADGET "/configs/c.1/strings/0x409/configuration", "uac_sa");

	// The device's own name. Without these string descriptors a host has
	// nothing to call the device but "Unknown USB Audio Device".
	write_noted(GADGET "/strings/0x409/manufacturer", "HiBy");
	write_noted(GADGET "/strings/0x409/product", usb_product());
	write_noted(GADGET "/strings/0x409/serialnumber", chip_serial());

	// What the kernel actually kept, so the log answers the next question
	// instead of raising it.
	static const char *const ATTRS[] = {"c_chmask", "c_ssize",		   "c_srate",		  "p_chmask",
										"p_ssize",  "p_srate",		   "dynamic_feedback", "dsd_native_enable",
										"string_assoc", NULL};
	for (int i = 0; ATTRS[i]; i++) {
		char value[128];
		snprintf(path, sizeof(path), UAC_FUNC "/%s", ATTRS[i]);
		if (read_first_line(path, value, sizeof(value))) {
			printf("usbdac:   %s = %s\n", ATTRS[i], value);
		}
	}

	snprintf(path, sizeof(path), GADGET "/configs/c.1/uac_sa.a");
	if (symlink(UAC_FUNC, path) != 0 && errno != EEXIST) {
		set_error(tr("usbdac_link_failed"), strerror(errno));
		return false;
	}

	char udc[256];
	if (!first_udc(udc, sizeof(udc))) {
		set_error(tr("usbdac_no_controller"));
		return false;
	}
	if (!write_file(GADGET "/UDC", udc)) {
		// This is where a malformed descriptor set shows up, and the errno is
		// the only clue: EBUSY means something else still holds the
		// controller, EINVAL means the gadget itself was refused.
		set_error(tr("usbdac_bind_failed"), udc, strerror(errno));
		return false;
	}
	printf("usbdac: gadget bound to %s\n", udc);
	if (!apply_dsd_native(udc)) {
		return false;
	}

	// Only after the bind. /sys/class/android_usb/f_uac_sa/ is created by the
	// function when it is instantiated, so writing to it any earlier fails with
	// ENOENT.
	//
	// dynamic_feedback is the one that matters. Asynchronous USB audio needs
	// the device to keep telling the host how fast its own clock is really
	// consuming, so the host can trim what it sends; without it the two clocks
	// drift and the host aborts within a second of pressing play. The stock
	// settings carry it as `dac_feedback` and default it on.
	static const char *const CLASS_DIR = "/sys/class/android_usb/f_uac_sa";
	if (path_exists(CLASS_DIR)) {
		char attr[512];
		snprintf(attr, sizeof(attr), "%s/dynamic_feedback", CLASS_DIR);
		write_noted(attr, "1");

		// Log the rest of the directory too, so the values are on record.
		DIR *dir = opendir(CLASS_DIR);
		if (dir) {
			struct dirent *de;
			while ((de = readdir(dir)) != NULL) {
				if (de->d_name[0] == '.') {
					continue;
				}
				char value[128];
				snprintf(attr, sizeof(attr), "%s/%s", CLASS_DIR, de->d_name);
				if (read_first_line(attr, value, sizeof(value))) {
					printf("usbdac:   f_uac_sa/%s = %s\n", de->d_name, value);
				}
			}
			closedir(dir);
		}
	} else {
		fprintf(stderr, "usbdac: %s does not exist; no feedback endpoint to enable\n", CLASS_DIR);
	}

	force_reenumeration();
	return true;
}

static void gadget_teardown(void) {
	write_file(GADGET "/UDC", "\n");
	unlink_functions();
	rmdir(UAC_FUNC);
}

// ---------------------------------------------------------------------------
// Charging
// ---------------------------------------------------------------------------

static void apply_charging(void) {
	bool on = usbdac_charging_enabled();

	// power.c owns the charger. It stops charging through the driver's
	// step-charging bit, which leaves the input path feeding the system -- so
	// the player carries on running from the cable with the battery untouched,
	// which is the whole point here.
	power_set_charging_allowed(on);

	// Say so where the user looks: with the charger off the battery icon and
	// the LED must stop claiming a charge is happening. On the Blaze
	// axpcharge.c says it itself, and only once the charger has really stopped.
	if (!axpcharge_cable_policy()) {
		system_suppress_charging(!on);
	}
	printf("usbdac: charging %s while in DAC mode\n", on ? "allowed" : "held off");
}

// The live answer, cached here rather than read back out of the config every
// time: config_set_bool() can fail silently when the table is full, and the
// setting must not depend on the write having succeeded. The config is where
// the choice is remembered across boots, not where it lives.
static int charging_cache = -1; // -1 = not read from config yet

void usbdac_set_charging(bool enabled) {
	charging_cache = enabled ? 1 : 0;
	config_set_bool("usb", "dac_charge_disable", !enabled);
	config_save();
	if (usbdac_is_active()) {
		apply_charging();
	}
}

bool usbdac_charging_enabled(void) {
	if (charging_cache < 0) {
		charging_cache = config_get_bool("usb", "dac_charge_disable", false) ? 0 : 1;
	}
	return charging_cache == 1;
}

// ---------------------------------------------------------------------------
// The audio thread
// ---------------------------------------------------------------------------

static bool wait_for_node(void) {
	for (int i = 0; i < NODE_WAIT_TRIES && running; i++) {
		if (access(UAC_NODE, F_OK) == 0) {
			return true;
		}
		usleep(NODE_WAIT_US);
	}
	return access(UAC_NODE, F_OK) == 0;
}

// DoP: DSD packed into PCM, which is what a computer without the
// manufacturer's ASIO driver sends -- MusicBee over a generic ASIO driver plays
// a DSD64 file as 176.4 kHz PCM. Every 32-bit word carries the marker in its
// top byte, 0x05 and 0xFA by turns, the same in both channels. Thirty-two
// frames of that never happen by chance in music, nor in PCM silence (0x00).
#define DOP_CHECK_FRAMES 32

static bool looks_like_dop(const unsigned char *bytes, ssize_t count) {
	if (count < DOP_CHECK_FRAMES * 8) {
		return false;
	}
	unsigned char previous = 0;
	for (int i = 0; i < DOP_CHECK_FRAMES; i++) {
		unsigned char left = bytes[i * 8 + 3], right = bytes[i * 8 + 7];
		if (left != right || (left != 0x05 && left != 0xFA) || left == previous) {
			return false;
		}
		previous = left;
	}
	return true;
}

// Native DSD from the host to DoP, the only way DSD reaches this DAC. A host
// frame is four bytes of the left channel and then four of the right, the
// earliest byte first and the earliest bit at the top of each byte: the
// DSD_U32_BE layout of a UAC2 raw-data setting, and the order the stock
// player's own repacking of this stream assumes (it interleaves the bytes as
// they come, with no swap and no bit reversal). Each host frame is 32 DSD bits
// a channel, so two DoP frames of 16, packed as decode/dsd.c packs a file.
static void native_to_dop(const unsigned char *in, int frames, int32_t *out, int *phase) {
	for (int f = 0; f < frames; f++, in += 8) {
		for (int half = 0; half < 2; half++) {
			uint32_t marker = *phase ? 0xFA : 0x05;
			*phase ^= 1;
			for (int c = 0; c < 2; c++) {
				const unsigned char *b = in + c * 4 + half * 2;
				*out++ = (int32_t)((marker << 24) | ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8));
			}
		}
	}
}

static void *audio_main(void *arg) {
	(void)arg;

	char *buffer = NULL;
	int buffer_size = 0;
	int32_t *dop_out = NULL; // twice the buffer: two DoP frames a host frame
	int dop_phase = 0;
	int open_rate = 0, open_bits = 0;
	bool output_open = false;
	bool dop = false, open_dop = false;

	struct uac_format current = {0};

	int idle_us = 0;

	while (running) {
		int host_state = 0;
		if (ioctl(node_fd, UAC_IOCTL_STATE, &host_state) != 0) {
			// The driver went away, or the host pulled the cable. Not fatal on
			// its own: look again in a moment.
			usleep(IDLE_POLL_US);
			continue;
		}

		if (host_state <= 0) {
			// Quiet. While the output is open this is almost always just the
			// gap between two buffers, so wait a short time and ask again --
			// and only give the DAC back once the quiet has lasted long enough
			// to mean the computer really has stopped.
			if (output_open) {
				idle_us += GAP_POLL_US;
				if (idle_us < IDLE_CLOSE_MS * 1000) {
					usleep(GAP_POLL_US);
					continue;
				}
				audio_external_end();
				set_dac_dop(0);
				output_open = false;
				open_rate = open_bits = 0;
				dop = open_dop = false;
				pthread_mutex_lock(&lock);
				state.streaming = false;
				touch();
				pthread_mutex_unlock(&lock);
				printf("usbdac: host stopped; DAC released\n");
			}
			idle_us = 0;
			usleep(IDLE_POLL_US);
			continue;
		}

		idle_us = 0;

		struct uac_format fmt;
		memset(&fmt, 0, sizeof(fmt));
		if (ioctl(node_fd, UAC_IOCTL_FORMAT, &fmt) != 0) {
			usleep(IDLE_POLL_US);
			continue;
		}

		// The host decides. format == 2 means native DSD; everything else
		// arrives as 32-bit stereo.
		int bits = (fmt.format == 2) ? 1 : 32;
		bool dsd = (bits == 1);

		if (fmt.rate <= 0) {
			usleep(IDLE_POLL_US);
			continue;
		}

		int want = frame_buffer_bytes(fmt.rate);
		if (want != buffer_size) {
			free(buffer);
			buffer = malloc((size_t)want);
			free(dop_out);
			dop_out = malloc((size_t)want * 2);
			if (!buffer || !dop_out) {
				buffer_size = 0;
				set_error(tr("usbdac_no_memory"));
				break;
			}
			buffer_size = want;
		}

		if (!output_open || fmt.rate != open_rate || bits != open_bits || dop != open_dop) {
			if (output_open) {
				audio_external_end();
				output_open = false;
			}
			// Before the stream opens, as for a DSD track: with DOP_EN off the
			// DAC plays a DoP stream as white noise at full level.
			set_dac_dop(dop || dsd);
			open_dop = dop;
			// DoP from the host is handed to the DAC as it comes; native DSD
			// goes out as DoP at twice the host's frame rate.
			int out_rate = dsd ? fmt.rate * 2 : fmt.rate;
			dop_phase = 0;
			output_open = audio_external_begin_latency(out_rate, 2, 32, output_ms());
			if (!output_open) {
				// Say so, but keep reading: the gadget's OUT endpoint has to
				// go on being drained or the host sees a device that stopped
				// consuming and aborts the stream. Throwing the samples away
				// keeps the link alive.
				set_error(tr("usbdac_rate_refused"), out_rate);
			}
			open_rate = fmt.rate;
			open_bits = bits;

			// fmt.format is logged because it is the only field left that could
			// carry the host's own bit depth. The driver always hands over
			// 32-bit words -- that is why the page says 32 whatever the Mac is
			// set to -- but if this number moves when the host setting changes,
			// it is the one to map.
			printf("usbdac: host is sending %d Hz, %s (format=%d flags=%d), output buffer %d ms\n", fmt.rate,
				   dsd ? "DSD" : (dop ? "DoP" : "32 bit PCM"), fmt.format, fmt.flags, output_ms());
			pthread_mutex_lock(&lock);
			state.streaming = true;
			// Native: 32 DSD bits a host frame; DoP: 16.
			state.sample_rate = dsd ? fmt.rate * 32 : (dop ? fmt.rate * 16 : fmt.rate);
			state.bits = bits;
			state.channels = 2;
			state.dsd = dsd || dop;
			state.error[0] = '\0';
			touch();
			pthread_mutex_unlock(&lock);
			current = fmt;
		}
		(void)current;

		ssize_t got = read(node_fd, buffer, (size_t)buffer_size);
		if (got <= 0) {
			// End of a stream, or the host paused between tracks: back to the
			// state poll rather than treating it as a failure.
			usleep(2000);
			continue;
		}

		// A host with nothing to play may keep the stream open and send digital
		// silence -- every byte the same, zero or the DSD idle pattern. Only
		// reads that are not count as sound.
		if (got > 1 && memcmp(buffer, buffer + 1, (size_t)got - 1) != 0) {
			pthread_mutex_lock(&lock);
			last_audible_ms = now_ms();
			pthread_mutex_unlock(&lock);
		}

		// A switch between PCM and DoP reopens the output with DOP_EN set to
		// match; this buffer was read for the old mode and is dropped.
		bool now_dop = !dsd && looks_like_dop((const unsigned char *)buffer, got);
		if (now_dop != dop) {
			dop = now_dop;
			continue;
		}

		// Eight bytes to a frame, and audio_external_write() counts frames.
		if (output_open && dsd) {
			native_to_dop((const unsigned char *)buffer, (int)(got / 8), dop_out, &dop_phase);
			audio_external_write(dop_out, (int)(got / 8) * 2);
		} else if (output_open) {
			audio_external_write(buffer, (int)(got / 8));
		}
	}

	if (output_open) {
		audio_external_end();
	}
	set_dac_dop(0);
	free(buffer);
	free(dop_out);

	pthread_mutex_lock(&lock);
	state.streaming = false;
	touch();
	pthread_mutex_unlock(&lock);
	printf("usbdac: audio thread finished\n");
	return NULL;
}

// The slow half of usbdac_start(), off the UI thread: storage_restore() shells
// out to umount and mount, and wait_for_node() waits up to five seconds for
// /dev/uac_sa to appear. Inline, that would freeze the interface for the whole
// of it.
static void *start_main(void *arg) {
	(void)arg;

	// One PCM takes one writer, and in DAC mode the player is not playing
	// anything of its own.
	audio_stop();

	// Tell usb.c to keep off the controller first: its watcher thread reacts to
	// the cable being in by rebuilding the mass-storage gadget, which would
	// land straight on top of this one.
	usb_set_dac_mode(true);

	if (!gadget_build()) {
		usb_set_dac_mode(false);
		goto failed;
	}

	apply_charging();

	running = true;
	if (!wait_for_node()) {
		set_error(tr("usbdac_device_missing"));
		running = false;
		gadget_teardown();
		usb_set_dac_mode(false);
		goto failed;
	}

	node_fd = open(UAC_NODE, O_RDWR);
	if (node_fd < 0) {
		set_error(tr("usbdac_open_failed"), UAC_NODE, strerror(errno));
		running = false;
		gadget_teardown();
		usb_set_dac_mode(false);
		goto failed;
	}

	if (pthread_create(&audio_thread, NULL, audio_main, NULL) != 0) {
		set_error(tr("usbdac_thread_failed"));
		running = false;
		close(node_fd);
		node_fd = -1;
		gadget_teardown();
		usb_set_dac_mode(false);
		goto failed;
	}
	thread_valid = true;

	pthread_mutex_lock(&lock);
	state.starting = false;
	state.active = true;
	touch();
	pthread_mutex_unlock(&lock);
	printf("usbdac: DAC mode on\n");
	return NULL;

failed:
	pthread_mutex_lock(&lock);
	state.starting = false;
	state.active = false;
	touch();
	pthread_mutex_unlock(&lock);
	return NULL;
}

bool usbdac_start(void) {
	if (usbdac_is_active() || start_thread_valid) {
		return true;
	}

	pthread_mutex_lock(&lock);
	memset(&state, 0, sizeof(state));
	state.starting = true;
	touch();
	pthread_mutex_unlock(&lock);

	if (pthread_create(&start_thread, NULL, start_main, NULL) != 0) {
		set_error(tr("usbdac_cannot_start_dac_mode"));
		pthread_mutex_lock(&lock);
		state.starting = false;
		touch();
		pthread_mutex_unlock(&lock);
		return false;
	}
	start_thread_valid = true;
	return true; // "accepted", not "finished" -- watch the state
}

void usbdac_stop(void) {
	// A start still in flight has to be waited for, or its thread would come
	// back and switch the mode on again behind this.
	if (start_thread_valid) {
		pthread_join(start_thread, NULL);
		start_thread_valid = false;
	}
	if (!usbdac_is_active() && !thread_valid) {
		return;
	}

	running = false;

	// Closing the node under the thread is what brings it back if it is
	// sitting in read(). A char device gives no shutdown() to lean on the way
	// a socket does, so this relies on the driver returning from the pending
	// read when its file is closed -- which it does, and in the worst case the
	// loop is never blocked for longer than one 10 ms buffer anyway.
	if (node_fd >= 0) {
		close(node_fd);
		node_fd = -1;
	}
	if (thread_valid) {
		pthread_join(audio_thread, NULL);
		thread_valid = false;
	}

	// Leaving DAC mode gives the charger back, and the indication with it.
	power_set_charging_allowed(true);
	if (!axpcharge_cable_policy()) {
		system_suppress_charging(false);
	}

	gadget_teardown();

	// Give the controller back. usb.c rebuilds and rebinds the mass-storage
	// gadget itself, so the computer sees the card again instead of nothing.
	usb_set_dac_mode(false);

	pthread_mutex_lock(&lock);
	memset(&state, 0, sizeof(state));
	touch();
	pthread_mutex_unlock(&lock);
	printf("usbdac: DAC mode off\n");
}

bool usbdac_is_active(void) {
	pthread_mutex_lock(&lock);
	bool on = state.active;
	pthread_mutex_unlock(&lock);
	return on;
}

bool usbdac_is_audible(void) {
	pthread_mutex_lock(&lock);
	bool audible = state.active && last_audible_ms != 0 && (now_ms() - last_audible_ms) < AUDIBLE_MS;
	pthread_mutex_unlock(&lock);
	return audible;
}

void usbdac_get_state(usbdac_state_t *out) {
	if (!out) {
		return;
	}
	pthread_mutex_lock(&lock);
	*out = state;
	pthread_mutex_unlock(&lock);
}

unsigned usbdac_serial(void) {
	return serial;
}
