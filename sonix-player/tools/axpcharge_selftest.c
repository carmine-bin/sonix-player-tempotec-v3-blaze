// Host self-check for the Blaze's cable charge policy (src/system/device/axpcharge.c).
//
// The device code itself, not HOST_BUILD's stubs: sysfs is a directory tree
// under a temporary root, the PMIC an array of registers behind a fake I2C
// ioctl, the clock a counter. Each kind of cable is plugged in and the nodes
// and registers are checked against stock's numbers; then the 500 mA switch,
// the stop for DAC mode with its indicator, the 80 % cap's stop surviving a
// replug and the driver's thread, and the R1, which must not be touched at all.
//
//   gcc -O2 -Wall -I. tools/axpcharge_selftest.c -o /tmp/axpcharge_selftest
//   /tmp/axpcharge_selftest /tmp

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/device/sysinfo.h"

// --- the fakes, before the macros that route the device code to them ---

static char root[256];
static uint8_t regs[256];
static uint64_t fake_ms = 1000;
static bool i2c_broken;

static void rooted(char *out, size_t size, const char *path) { snprintf(out, size, "%s%s", root, path); }

static FILE *fake_fopen(const char *path, const char *mode) {
	char p[512];
	rooted(p, sizeof(p), path);
	// sysfs: a node that is not there cannot be created by writing it.
	if (mode[0] == 'w' && access(p, F_OK) != 0) {
		errno = ENOENT;
		return NULL;
	}
	return fopen(p, mode);
}

static DIR *fake_opendir(const char *path) {
	char p[512];
	rooted(p, sizeof(p), path);
	return opendir(p);
}

static int fake_access(const char *path, int mode) {
	char p[512];
	rooted(p, sizeof(p), path);
	return access(p, mode);
}

static int fake_open(const char *path, int flags, ...) {
	(void)flags;
	return strcmp(path, "/dev/i2c-0") == 0 ? open("/dev/null", O_RDWR) : -1;
}

static int fake_ioctl(int fd, unsigned long request, ...) {
	(void)fd;
	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	if (request == I2C_SLAVE_FORCE) {
		return (long)arg == 0x34 ? 0 : -1;
	}
	if (request != I2C_SMBUS) {
		return -1;
	}
	if (i2c_broken) {
		return -1;
	}
	struct i2c_smbus_ioctl_data *args = arg;
	if (args->read_write == I2C_SMBUS_WRITE) {
		regs[args->command] = args->data->byte;
	} else {
		args->data->byte = regs[args->command];
	}
	return 0;
}

static int fake_clock_gettime(clockid_t id, struct timespec *ts) {
	(void)id;
	ts->tv_sec = (time_t)(fake_ms / 1000);
	ts->tv_nsec = (long)(fake_ms % 1000) * 1000000;
	return 0;
}

#define fopen fake_fopen
#define opendir fake_opendir
#define access fake_access
#define open fake_open
#define ioctl fake_ioctl
#define clock_gettime fake_clock_gettime
#include "src/system/device/axpcharge.c"
#undef fopen
#undef opendir
#undef access
#undef open
#undef ioctl
#undef clock_gettime

// --- what axpcharge.c calls outside itself ---

static const sysinfo_model_t R1 = {.name = "HiBy R1", .pmic_charger = true};
static const sysinfo_model_t BLAZE = {.name = "TempoTec V3 Blaze", .pmic_charger = true, .cable_charge_policy = true};
static const sysinfo_model_t *model = &R1;
static bool vbus, otg_host, suppressed;

const sysinfo_model_t *sysinfo_model(void) { return model; }
bool usb_vbus_present(void) { return vbus; }
bool usbaudio_active(void) { return otg_host; }
void system_suppress_charging(bool s) { suppressed = s; }
long config_get_int(const char *section, const char *key, long fallback) {
	(void)section, (void)key;
	return fallback;
}
void config_set_int(const char *section, const char *key, long value) { (void)section, (void)key, (void)value; }
bool config_save(void) { return true; }

// --- the fake sysfs ---

static void put(const char *path, const char *text) {
	char p[512];
	rooted(p, sizeof(p), path);
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", p);
	for (char *s = dir + strlen(root) + 1; *s; s++) {
		if (*s == '/') {
			*s = '\0';
			mkdir(dir, 0755);
			*s = '/';
		}
	}
	FILE *f = fopen(p, "w");
	assert(f);
	fputs(text, f);
	fclose(f);
}

static void get(const char *path, char *out, size_t size) {
	char p[512];
	rooted(p, sizeof(p), path);
	FILE *f = fopen(p, "r");
	assert(f);
	out[0] = '\0';
	if (!fgets(out, (int)size, f)) {
		out[0] = '\0';
	}
	fclose(f);
}

static int node(const char *path) {
	char text[32];
	get(path, text, sizeof(text));
	return atoi(text);
}

#define LIMIT "/sys/class/power_supply/usb/charge_control_limit_max"
#define CURRENT "/sys/class/power_supply/usb/constant_charge_current_max"
#define SWITCH "/sys/class/power_supply/axp_battery/charger"
#define UV "/sys/class/power_supply/battery/voltage_now"
#define UDC "/sys/class/udc/13500000.otg_new/state"
#define GADGET "/sys/kernel/config/usb_gadget/android0/UDC"

static void udc(const char *state) { put(UDC, state); }
static void gadget(bool bound) { put(GADGET, bound ? "13500000.otg_new\n" : "\n"); }

// What axp2101.ko does with the nodes (a stock 1.3 reading, md5 fad2d7fd):
// the input limit straight to REG16h (0 = 2000 mA) and bit 1 of REG18h; a
// charge current request kept, written to REG62h at once if 500 mA or less;
// the charger switch zeroing REG62h (or putting the request back); and its
// thread, once a second, writing the request to REG62h after five polls in a
// row with VBUS good, which is what happens 5-6 s after every plug.
static int drv_request = 780;
static char seen_limit[32], seen_current[32], seen_switch[32];
static int drv_good_polls;
static uint64_t drv_next_poll;

static uint8_t drv_field(int ma) { return (uint8_t)(ma <= 200 ? ma / 25 : 8 + (ma - 200) / 100); }

static bool changed(const char *path, char *seen, char *now, size_t size) {
	char p[512];
	rooted(p, sizeof(p), path);
	FILE *f = fopen(p, "r");
	now[0] = '\0';
	if (f) {
		if (!fgets(now, (int)size, f)) {
			now[0] = '\0';
		}
		fclose(f);
	}
	if (strcmp(now, seen) == 0) {
		return false;
	}
	snprintf(seen, size, "%s", now);
	return now[0] >= '0' && now[0] <= '9';
}

static void driver(void) {
	char now[32];
	if (changed(LIMIT, seen_limit, now, sizeof(now))) {
		int ma = atoi(now) == 0 ? 2000 : atoi(now);
		static const int STEPS[] = {100, 500, 900, 1000, 1500, 2000};
		uint8_t field = 0;
		for (uint8_t i = 0; i < 6; i++) {
			if (STEPS[i] <= ma) {
				field = i;
			}
		}
		regs[0x16] = (uint8_t)((regs[0x16] & ~7u) | field);
		regs[0x18] = (uint8_t)(atoi(now) == 100 ? regs[0x18] & ~2u : regs[0x18] | 2u);
	}
	if (changed(CURRENT, seen_current, now, sizeof(now))) {
		drv_request = atoi(now);
		if (drv_request <= 500) {
			regs[0x62] = drv_field(drv_request);
		}
	}
	if (changed(SWITCH, seen_switch, now, sizeof(now))) {
		regs[0x62] = atoi(now) == 0 ? 0 : drv_field(drv_request);
	}
	if (fake_ms >= drv_next_poll) {
		drv_next_poll = fake_ms + 1000;
		drv_good_polls = vbus ? drv_good_polls + 1 : 0;
		if (drv_good_polls == 5) {
			regs[0x62] = drv_field(drv_request);
		}
	}
}

// `ms` of player time, the power tick every 250 ms, the driver after each.
// Every request the code makes is checked against the bounds: never above
// stock's numbers.
static void run(int ms) {
	for (int t = 0; t < ms; t += 250) {
		fake_ms += 250;
		axpcharge_tick();
		driver();
		if (model->cable_charge_policy) {
			int limit = node(LIMIT), current = node(CURRENT);
			assert(limit == 0 || limit == 500);
			assert(current >= 0 && current <= 1000);
			assert((regs[0x16] & 7) <= 5 && (regs[0x62] & 0x1f) <= 16);
		}
	}
}

static void expect(int limit, int current, uint8_t reg16, uint8_t reg62, const char *what) {
	if (node(LIMIT) != limit || node(CURRENT) != current || (regs[0x16] & 7) != reg16 ||
		(regs[0x62] & 0x1f) != reg62) {
		fprintf(stderr, "FAIL %s: limit %d current %d REG16h %u REG62h %u, wanted %d %d %u %u\n", what, node(LIMIT),
				node(CURRENT), regs[0x16] & 7, regs[0x62] & 0x1f, limit, current, reg16, reg62);
		exit(1);
	}
	printf("ok   %s\n", what);
}

static void expect_suppressed(bool want, const char *what) {
	if (suppressed != want) {
		fprintf(stderr, "FAIL %s: indicator %s\n", what, suppressed ? "hidden" : "shown");
		exit(1);
	}
	printf("ok   %s\n", what);
}

int main(int argc, char **argv) {
	snprintf(root, sizeof(root), "%s/axpcharge-XXXXXX", argc > 1 ? argv[1] : "/tmp");
	assert(mkdtemp(root));
	setvbuf(stdout, NULL, _IONBF, 0);

	// The PMIC as the driver's probe leaves it: 4.40 V, 700 mA, 500 mA in.
	regs[0x64] = 0x05;
	regs[0x62] = 0x0d;
	regs[0x16] = 0x01;
	put(LIMIT, "untouched");
	put(CURRENT, "untouched");
	put(SWITCH, "1");
	put(UV, "4100000");
	udc("not attached");
	gadget(true);

	// The R1: the same PMIC, and nothing here may change for it.
	model = &R1;
	axpcharge_set(false, false);
	vbus = true;
	run(20000);
	char text[32];
	get(LIMIT, text, sizeof(text));
	assert(strcmp(text, "untouched") == 0);
	get(CURRENT, text, sizeof(text));
	assert(strcmp(text, "untouched") == 0);
	assert(regs[0x16] == 0x01 && regs[0x62] == 0x0d);
	printf("ok   R1: no node written, REG16h and REG62h as the driver left them\n");
	vbus = false;
	run(2000);

	// The Blaze from here on, booted with no cable.
	model = &BLAZE;
	axpcharge_set(false, false);
	expect(500, 780, 1, 13, "no cable: the driver's own defaults");

	// A wall charger: nothing raised until the line has been quiet a while.
	vbus = true;
	run(4000);
	expect(500, 780, 1, 13, "wall charger, first seconds: not told apart yet");
	run(4000);
	expect(0, 1000, 5, 16, "wall charger: 2000 mA in, 1000 mA charge (stock)");

	// The PMIC loses the input limit (a plug detection of its own, say): set again.
	regs[0x16] = 0x01;
	run(6000);
	expect(0, 1000, 5, 16, "wall charger: REG16h found back at 500 mA, set again");

	// Pulled: the defaults within a second, nothing written to the PMIC.
	vbus = false;
	regs[0x62] = 0x10;
	run(1000);
	expect(500, 780, 1, 16, "unplugged: the driver's defaults, REG62h left alone");
	regs[0x16] = 0x01;
	regs[0x62] = 0x0d;

	// A PC: a host at once, and a host still while the line goes quiet.
	vbus = true;
	udc("configured");
	run(1000);
	expect(500, 500, 1, 11, "USB host: 500 mA in, 500 mA charge (stock)");
	udc("not attached");
	run(20000);
	expect(500, 500, 1, 11, "USB host gone quiet (gadget rebuilt): still a host");
	vbus = false;
	run(1000);
	udc("not attached");

	// A cable with no gadget to enumerate (the charge screen): never a wall.
	gadget(false);
	vbus = true;
	run(20000);
	expect(500, 780, 1, 13, "no gadget bound: not told apart, driver's defaults");
	vbus = false;
	run(1000);
	gadget(true);

	// The player as the host itself (a DAC on the port): never a wall.
	otg_host = true;
	vbus = true;
	run(20000);
	expect(500, 780, 1, 13, "player is the USB host: not told apart");
	otg_host = false;
	vbus = false;
	run(1000);

	// An unknown controller state counts as a host.
	udc("reconnecting");
	vbus = true;
	run(10000);
	expect(500, 500, 1, 11, "unknown UDC state: a host");
	vbus = false;
	run(1000);
	udc("not attached");
	regs[0x62] = 0x0d;

	// A wall charger, then straight into a PC without VBUS dropping between
	// two looks: a host at once.
	vbus = true;
	run(8000);
	expect(0, 1000, 5, 16, "wall charger again");
	udc("configured");
	run(1000);
	expect(500, 500, 1, 11, "wall charger swapped for a PC between two looks: a host");
	vbus = false;
	run(1000);
	udc("not attached");

	// A PC asleep before the plug looks like a wall charger (stock calls it
	// one too); its port sags at 2000 mA: back to 500 for this cable.
	udc("suspended");
	vbus = true;
	run(8000);
	expect(0, 1000, 5, 16, "PC asleep before the plug: taken for a wall charger");
	regs[0x01] |= 0x08; // VINDPM
	run(2000);
	expect(0, 1000, 5, 16, "VINDPM at two looks: still 2000");
	run(1000);
	expect(500, 780, 1, 13, "VINDPM at three looks: 500 mA in for this cable");
	regs[0x01] &= (uint8_t)~0x08;
	run(20000);
	expect(500, 780, 1, 13, "VINDPM gone: still 500 until the cable is pulled");
	vbus = false;
	run(1000);
	udc("not attached");
	vbus = true;
	run(8000);
	expect(0, 1000, 5, 16, "a new cable: a wall charger again");

	// The port's protection cuts VBUS soon after the raise: the cable that
	// comes back stays at 500 mA, and does not trip it again.
	vbus = false;
	run(16000);
	vbus = true;
	run(8000);
	expect(0, 1000, 5, 16, "replugged after 16 s: a wall charger");
	run(500);
	vbus = false; // cut within 3 s of the raise
	run(3000);
	vbus = true;
	run(20000);
	expect(500, 780, 1, 13, "back 3 s after a cut at 2000 mA: stays at 500");
	vbus = false;
	run(20000);
	vbus = true;
	run(8000);
	expect(0, 1000, 5, 16, "back 20 s later: a wall charger again");
	vbus = false; // 2 s after the raise again: let that cut age out
	run(16000);
	regs[0x62] = 0x0d;

	// The 500 mA switch on a wall charger: 2000 in, 500 into the battery.
	axpcharge_set(false, true);
	vbus = true;
	run(8000);
	expect(0, 500, 5, 11, "wall charger, 500 mA switch on: charge 500 mA");
	axpcharge_set(false, false);
	run(1000);
	expect(0, 1000, 5, 16, "500 mA switch off again: 1000 mA");

	// DAC mode with charging off, on a host: a charge current of 0, kept
	// through the driver's thread, and the indicator hidden.
	vbus = false;
	run(1000);
	udc("configured");
	vbus = true;
	run(1000);
	expect(500, 500, 1, 11, "DAC mode, charging allowed: host currents");
	if (suppressed) {
		fprintf(stderr, "FAIL DAC mode, charging allowed: indicator hidden\n");
		return 1;
	}
	axpcharge_block_charging(true);
	run(250);
	expect(500, 0, 1, 0, "DAC mode, charging off: charge current 0");
	expect_suppressed(true, "DAC mode, charging off: indicator hidden");
	assert(!axpcharge_holding());
	run(20000);
	expect(500, 0, 1, 0, "DAC mode, charging off: still 0 past the driver's thread");
	axpcharge_block_charging(false);
	run(250);
	expect(500, 500, 1, 11, "DAC mode, charging allowed again: 500 mA");
	expect_suppressed(false, "DAC mode, charging allowed again: indicator shown");

	// Charging off with no VBUS: nothing to stop, and nothing hidden; back
	// in, stopped before the driver's thread gets to it.
	axpcharge_block_charging(true);
	vbus = false;
	run(1000);
	expect(500, 780, 1, 0, "DAC mode, charging off, no VBUS: the driver's defaults asked (its thread applies them at a plug)");
	expect_suppressed(false, "DAC mode, charging off, no VBUS: indicator shown");
	vbus = true;
	for (int i = 0; i < 40; i++) {
		run(250);
		assert(i < 4 || (regs[0x62] & 0x1f) == 0); // stopped within a second, and kept
	}
	expect(500, 0, 1, 0, "DAC mode, charging off, VBUS back: 0 again, through the plug");
	expect_suppressed(true, "DAC mode, charging off, VBUS back: indicator hidden");

	// The PMIC does not answer: REG62h cannot be read back at 0, so the
	// indicator is not hidden.
	i2c_broken = true;
	run(6000);
	expect_suppressed(false, "PMIC not answering: the indicator is not hidden");
	i2c_broken = false;
	axpcharge_block_charging(false);
	run(6000);
	expect(500, 500, 1, 11, "DAC mode left: 500 mA");

	// The 80 % cap above its arming voltage on a wall charger: 0, and still 0
	// after a replug and the driver's thread (its own switch would be undone
	// there).
	vbus = false;
	run(1000);
	udc("not attached");
	put(UV, "4300000");
	axpcharge_set(true, false);
	vbus = true;
	run(10000);
	expect(0, 0, 5, 0, "80 % cap above 4.23 V on a wall charger: 2000 mA in, charge 0");
	assert(axpcharge_holding());
	expect_suppressed(false, "80 % cap: the indicator is not hidden (the cap shows itself)");
	vbus = false;
	run(1000);
	vbus = true;
	run(20000);
	expect(0, 0, 5, 0, "80 % cap: still 0 after a replug and the driver's thread");
	put(UV, "4150000");
	run(6000);
	expect(0, 1000, 5, 16, "80 % cap below 4.23 V: REG64h capped, 1000 mA again");
	assert(regs[0x64] == 0x03);

	// The Blaze never uses the driver's switch.
	char text2[8];
	get(SWITCH, text2, sizeof(text2));
	assert(strcmp(text2, "1") == 0);
	printf("ok   the driver's charger switch never written on the Blaze\n");

	char cmd[300];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	if (system(cmd) != 0) {
		fprintf(stderr, "axpcharge_selftest: could not remove %s\n", root);
	}
	printf("axpcharge_selftest: all checks passed\n");
	return 0;
}
