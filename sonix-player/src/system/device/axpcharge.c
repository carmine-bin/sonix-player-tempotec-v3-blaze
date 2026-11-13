#include "axpcharge.h"

#include "src/system/core/config.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"
#include "src/system/audio/usbaudio.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define MP2731_NODE "/sys/class/power_supply/mp2731-charger"

bool axpcharge_applies(void) {
	static int answer = -1;
	if (answer < 0) {
		const sysinfo_model_t *model = sysinfo_model();
		answer = model ? model->pmic_charger : access(MP2731_NODE, F_OK) != 0;
	}
	return answer != 0;
}

bool axpcharge_cable_policy(void) {
	const sysinfo_model_t *model = sysinfo_model();
	return model && model->pmic_charger && model->cable_charge_policy;
}

#ifdef HOST_BUILD

// The simulator has no PMIC: the switches are shown and saved, nothing more.
void axpcharge_set(bool limit_voltage, bool limit_current) {
	(void)limit_voltage;
	(void)limit_current;
}
void axpcharge_tick(void) {}
bool axpcharge_holding(void) { return false; }
void axpcharge_block_charging(bool block) { (void)block; }
bool axpcharge_reg_read(uint8_t reg, uint8_t *value) {
	(void)reg;
	(void)value;
	return false;
}
bool axpcharge_reg_write(uint8_t reg, uint8_t value) {
	(void)reg;
	(void)value;
	return false;
}

#else

#include <dirent.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <string.h>
#include <sys/ioctl.h>

#define AXP_BUS "/dev/i2c-0"
#define AXP_ADDR 0x34

// REG01h[2:0], charging status: 4 is "charge done".
#define AXP_REG_STATUS 0x01
#define AXP_STATUS_DONE 4

// REG62h[4:0], constant charge current: N*25 mA up to 8 (200 mA), then
// 200 + (N-8)*100 mA. 11 is 500 mA.
#define AXP_REG_CURRENT 0x62
#define AXP_CURRENT_MASK 0x1Fu
#define AXP_CURRENT_CAP 11u

// REG16h[2:0], VBUS input current limit: 100, 500, 900, 1000, 1500, 2000 mA
// (the table axp2101.ko's axp210x_set_input_current_limit indexes, .rodata
// 0xdd8). What the cable may give the player in all, system and battery
// together; the driver's probe leaves it at 1, 500 mA.
#define AXP_REG_INPUT 0x16
#define AXP_INPUT_MASK 0x07u
static const int INPUT_STEP_MA[8] = {100, 500, 900, 1000, 1500, 2000, 0, 0}; // 0: reserved

// REG64h[2:0], charge target voltage: 1 4.00 V, 2 4.10 V, 3 4.20 V, 4 4.35 V,
// 5 4.40 V. The R1's axp2101.sh loads the driver with
// charge_voltage_limit=4400, which the driver writes here at every boot.
#define AXP_REG_VOLTAGE 0x64
#define AXP_VOLTAGE_MASK 0x07u
#define AXP_VOLTAGE_CAP 3u
#define AXP_VOLTAGE_STOCK 5u

// The cap goes on only once the battery is down to about it. Lowered under a
// fuller one (4.20 V against 4.35 V) the PMIC flags battery over-voltage when
// the cable goes in, and pulling the cable then cuts the power (REG21h=0x20,
// DCDC under-voltage). 90 mV over was seen to be harmless; this leaves room
// for a reading taken under load. Until then the stock driver's own switch
// stops the charge, so the battery stays where it is; its probe puts the
// charge current back at every boot, so nothing is left stuck at zero.
#define BATTERY_UV_NODE "/sys/class/power_supply/battery/voltage_now"
#define CHARGER_SWITCH_NODE "/sys/class/power_supply/axp_battery/charger"
#define CAP_ARM_UV 4230000L

// How often the tick re-asserts the switches, and retries after a failure.
#define REASSERT_MS 5000

// The charge current found before the cap first lowered it, kept in the
// config: after a restart with the cap on, the register holds the cap.
#define CURRENT_KEY "axp_charge_current"

static bool configured;
static bool want_voltage, want_current;
static bool holding;
static bool charge_stopped; // by us, through CHARGER_SWITCH_NODE
static bool cap_stop;		// the cap wants the charge stopped, as of the last apply()
static bool charge_held;	// the Blaze: REG62h read back at 0 with a stop wanted
static long long next_ms;

// DAC mode with charging switched off (power_set_charging_allowed). Written by
// whichever thread starts DAC mode, read here on the power tick.
static volatile bool want_block;
static bool block_applied; // want_block as of the last apply()

// The voltage to go back to: the register as first found, unless it already
// held the cap, left there by an earlier run.
static uint8_t voltage_stock = AXP_VOLTAGE_STOCK;
static bool voltage_seen;

static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// One SMBus byte transfer. I2C_SLAVE_FORCE because the PMIC's own driver holds
// the address.
static bool xfer(uint8_t reg, uint8_t *value, bool write) {
	int fd = open(AXP_BUS, O_RDWR);
	if (fd < 0) {
		return false;
	}
	bool ok = false;
	if (ioctl(fd, I2C_SLAVE_FORCE, AXP_ADDR) >= 0) {
		union i2c_smbus_data data;
		data.byte = write ? *value : 0;
		struct i2c_smbus_ioctl_data args = {
			.read_write = write ? I2C_SMBUS_WRITE : I2C_SMBUS_READ,
			.command = reg,
			.size = I2C_SMBUS_BYTE_DATA,
			.data = &data,
		};
		if (ioctl(fd, I2C_SMBUS, &args) >= 0) {
			if (!write) {
				*value = data.byte;
			}
			ok = true;
		}
	}
	close(fd);
	return ok;
}

static bool axp_read(uint8_t reg, uint8_t *value) { return xfer(reg, value, false); }
static bool axp_write(uint8_t reg, uint8_t value) { return xfer(reg, &value, true); }

// Writes `target` into the bits of `mask`, keeping the others, and reads it
// back.
static bool set_field(uint8_t reg, uint8_t mask, uint8_t target) {
	uint8_t v;
	if (!axp_read(reg, &v)) {
		return false;
	}
	if ((v & mask) == target) {
		return true;
	}
	if (!axp_write(reg, (uint8_t)((v & ~mask) | target)) || !axp_read(reg, &v)) {
		return false;
	}
	return (v & mask) == target;
}

// ---------------------------------------------------------------------------
// The cable policy (the Blaze: axpcharge_cable_policy())
//
// The driver alone leaves the input limit at its probe's 500 mA for every
// cable, and the AXP2101 has no charger detection of its own: REG16h changes
// only when it is written, and keeps its value across an unplug. The stock
// hiby_player is what raises it, through the driver's own supply "usb", on a
// USB event (0x485000, writing only what changed through 0x484d00): with VBUS
// up and the controller not "configured", it writes charge_control_limit_max=0
// (2000 mA in) and constant_charge_current_max=1000; with a host, 500 and
// 500. Here the same numbers, by the same two nodes, decided again at every
// look at the cable. Where each comes from: informe-carga.md (sections 4 and
// 5, ~/Work/sonix-medidas/opt3-carga) and a second reading of stock 1.3
// (axp2101.ko md5 fad2d7fd, hiby_player md5 8185e110).
//
// What the driver does with them, which shapes the rest:
//
//   * the input limit is written to REG16h at once (0 meaning 2000 mA), with
//     bit 1 of REG18h set, which only a limit of 100 clears;
//   * the charge current is kept as a request. 500 mA or less goes to REG62h
//     at once; more only from the driver's thread, after five polls with VBUS
//     good, which it also does after every plug: whatever REG62h was made to
//     hold, the last request is back there some 5-6 s after a cable goes in;
//   * its axp_battery/charger switch zeroes REG62h and nothing else, so the
//     thread undoes it at the next plug. The stop here is therefore a charge
//     current of 0, stock's own way, which the thread then keeps.
//
// Where this differs from stock, on purpose:
//
//   * no cable: stock writes (100, 100) with the REG18h bit cleared, kept
//     until the next USB event. A cable that turns up while nothing is
//     looking -- the player suspended, or between two looks -- would then meet
//     100 mA. Here the driver's own defaults go back instead.
//   * stock decides once, half a second after the plug, so a PC still
//     enumerating can be taken for a wall charger. Here nothing is raised
//     until the line has stayed without a host for a while with a gadget bound
//     (with none, a PC cannot enumerate the player and would look the same:
//     the charge screen, before usb.c has bound one), and not while the player
//     is itself the host. A host seen once stays a host until the cable goes.
//   * a second writer in stock asks 1500 mA of charge current some seconds
//     after the charge starts, past the AXP2101's 1 A maximum (REG62h code 21,
//     reserved). 1000 here, never more.
// ---------------------------------------------------------------------------

#define USB_SUPPLY "/sys/class/power_supply/usb"
#define INPUT_NODE USB_SUPPLY "/charge_control_limit_max"	 // 0 = 2000 mA
#define CURRENT_NODE USB_SUPPLY "/constant_charge_current_max" // mA
#define UDC_DIR "/sys/class/udc"
#define GADGET_DIR "/sys/kernel/config/usb_gadget"

// How often the cable is looked at, and how long the line must stay quiet,
// with VBUS up and a gadget bound, before it counts as a wall charger. A host
// enumerates within a second or two of the gadget binding, and usb.c rebinds a
// parked gadget at its next poll (four seconds at most, in standby).
#define CABLE_LOOK_MS 1000
#define WALL_SETTLE_MS 6000

typedef enum { CABLE_NONE, CABLE_UNKNOWN, CABLE_HOST, CABLE_WALL } cable_t;

static const char *const CABLE_NAMES[] = {"no cable", "a cable not yet told apart", "a USB host", "a wall charger"};

// The numbers written for each kind of cable, and nothing above them.
//
//   input: what goes into charge_control_limit_max. 500 is the probe's own
//          (axp2101_charger_dt_parse, info+236) and stock's for a host; 0 is
//          stock's for a wall charger, which the driver turns into 2000 mA.
//   charge: what goes into constant_charge_current_max. 780 is the driver's
//          own default (pdata .data+0x64; REG62h 13, 700 mA once the driver's
//          thread applies it); 500 and 1000 are stock's.
typedef struct {
	int input;
	int charge;
} policy_t;

static const policy_t POLICY[] = {
	[CABLE_NONE] = {500, 780},
	[CABLE_UNKNOWN] = {500, 780},
	[CABLE_HOST] = {500, 500},
	[CABLE_WALL] = {0, 1000},
};

// The bounds every write is checked against, whatever the table says: the
// most stock ever asks for with a wall charger.
#define CHARGE_MA_MAX 1000
#define CHARGE_MA_SWITCH 500 // the 500 mA switch

static int policy_nodes = -1; // -1 not looked for yet, 0 missing, 1 there
static cable_t cable = CABLE_NONE;
static long long next_look_ms;
static long long quiet_since; // since when the line has looked like a wall charger, 0 = it has not
static bool host_seen;		  // a host has talked on this cable: it stays a host until it is pulled
static char udc_state[32];	  // as last read, for the log
static policy_t written = {-1, -1};
static uint8_t logged_input = 0xff, logged_current = 0xff;

static long battery_uv(void);

static bool dac_block_applies(void) { return axpcharge_cable_policy() && want_block && cable != CABLE_NONE; }

static int input_ma(int input) { return input == 0 ? 2000 : input; }

// The REG16h field the driver writes for a limit: the largest step not above it.
static uint8_t input_field(int input) {
	int ma = input_ma(input);
	uint8_t field = 0;
	for (uint8_t i = 0; i < 6; i++) {
		if (INPUT_STEP_MA[i] <= ma) {
			field = i;
		}
	}
	return field;
}

// The REG62h field for a charge current, the driver's encoding (0x1018):
// 25 mA steps up to 200, 100 mA steps above, rounded down.
static uint8_t current_field(int ma) { return (uint8_t)(ma <= 200 ? ma / 25 : 8 + (ma - 200) / 100); }

static int current_ma(unsigned n) { return n <= 8 ? (int)n * 25 : 200 + ((int)n - 8) * 100; }

// The first line of a small sysfs file, newline cut. False when it is not there.
static bool read_line(const char *path, char *out, size_t size) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	bool ok = fgets(out, (int)size, f) != NULL;
	fclose(f);
	if (ok) {
		out[strcspn(out, "\r\n")] = '\0';
	}
	return ok;
}

// The state of the first (the only) device controller, "not attached",
// "configured" and so on.
static bool read_udc_state(char *out, size_t size) {
	DIR *dir = opendir(UDC_DIR);
	if (!dir) {
		return false;
	}
	bool ok = false;
	struct dirent *de;
	while (!ok && (de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}
		char path[300];
		snprintf(path, sizeof(path), UDC_DIR "/%s/state", de->d_name);
		ok = read_line(path, out, size);
	}
	closedir(dir);
	return ok;
}

// Whether any gadget is bound to the controller: storage, ADB or the DAC. With
// none, a host has nothing to enumerate and the line stays quiet for a PC too.
static bool gadget_bound(void) {
	DIR *dir = opendir(GADGET_DIR);
	if (!dir) {
		return false;
	}
	bool bound = false;
	struct dirent *de;
	while (!bound && (de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}
		char path[300], udc[64];
		snprintf(path, sizeof(path), GADGET_DIR "/%s/UDC", de->d_name);
		bound = read_line(path, udc, sizeof(udc)) && udc[0] != '\0';
	}
	closedir(dir);
	return bound;
}

// The states a controller can reach with no host on the line. "default" is a
// bus reset, which the plug of a charger with D+ and D- shorted may look like,
// and "suspended" an idle bus after it; only a host gives the device an
// address. Anything else ("addressed", "configured", strings this list does
// not know) counts as a host. Stock calls everything but "configured" a wall
// charger, at once.
static bool line_quiet(const char *state) {
	static const char *const QUIET[] = {"not attached", "attached", "powered", "default", "suspended"};
	for (size_t i = 0; i < sizeof(QUIET) / sizeof(QUIET[0]); i++) {
		if (strcmp(state, QUIET[i]) == 0) {
			return true;
		}
	}
	return false;
}

static cable_t look_at_cable(long long now) {
	if (!usb_vbus_present()) {
		quiet_since = 0;
		host_seen = false;
		snprintf(udc_state, sizeof(udc_state), "-");
		return CABLE_NONE;
	}
	if (!read_udc_state(udc_state, sizeof(udc_state))) {
		snprintf(udc_state, sizeof(udc_state), "?");
	} else if (!line_quiet(udc_state)) {
		host_seen = true;
	}
	// Once a host, a host for as long as the cable stays: the line goes quiet
	// for a PC too while usb.c rebuilds a gadget or hands the controller to DAC
	// mode or ADB, and that is no reason to raise anything.
	if (host_seen) {
		quiet_since = 0;
		return CABLE_HOST;
	}
	if (udc_state[0] == '?') {
		quiet_since = 0;
		return CABLE_UNKNOWN;
	}
	// The player is the host itself (a DAC on the port), or there is nothing a
	// host could have enumerated: no telling.
	if (usbaudio_active() || !gadget_bound()) {
		quiet_since = 0;
		return CABLE_UNKNOWN;
	}
	if (!quiet_since) {
		quiet_since = now;
	}
	return now - quiet_since >= WALL_SETTLE_MS ? CABLE_WALL : CABLE_UNKNOWN;
}

// Looks at the cable; true when it is another kind of cable than before.
static bool look(long long now) {
	next_look_ms = now + CABLE_LOOK_MS;
	cable_t seen = look_at_cable(now);
	if (seen == cable) {
		return false;
	}
	printf("axpcharge: cable: %s (UDC '%s')\n", CABLE_NAMES[seen], udc_state);
	if (cable == CABLE_NONE) {
		// A new plug: everything is written again, whatever the driver kept.
		written.input = written.charge = -1;
	}
	cable = seen;
	return true;
}

static bool write_node(const char *path, int value) {
	FILE *f = fopen(path, "w");
	if (!f) {
		return false;
	}
	bool ok = fprintf(f, "%d", value) > 0;
	return fclose(f) == 0 && ok;
}

// Why the charge is stopped, or NULL. Never with no cable: the next plug then
// meets the driver's own current, and the first look after it (a second at
// most, before the driver's thread applies anything) stops it again.
static const char *stop_reason(void) {
	if (cable == CABLE_NONE) {
		return NULL;
	}
	if (dac_block_applies()) {
		return "no charging in DAC mode";
	}
	return cap_stop ? "the 80 % cap" : NULL;
}

static policy_t policy_now(void) {
	policy_t want = POLICY[cable];
	if (want_current && want.charge > CHARGE_MA_SWITCH) {
		want.charge = CHARGE_MA_SWITCH;
	}
	// The stop, the way stock's (10, 100, 0) stops it: a charge current of 0,
	// which the driver writes to REG62h at once and its thread keeps there.
	if (stop_reason()) {
		want.charge = 0;
	}
	// Never above stock, whatever the table above says.
	if (want.charge > CHARGE_MA_MAX) {
		want.charge = CHARGE_MA_MAX;
	}
	if (want.input != 0 && want.input != 500) {
		want.input = 500;
	}
	return want;
}

// The one place that decides the currents. The 80 % cap and the stop for DAC
// mode follow in apply_voltage(), through the charger switch.
static bool apply_policy(void) {
	if (policy_nodes < 0) {
		policy_nodes = access(INPUT_NODE, W_OK) == 0 && access(CURRENT_NODE, W_OK) == 0;
		if (!policy_nodes) {
			printf("axpcharge: %s or %s is not writable; the charge current stays the driver's\n", INPUT_NODE,
				   CURRENT_NODE);
		}
	}
	if (!policy_nodes) {
		return true;
	}

	policy_t want = policy_now();
	bool ok = true;
	bool changed = false;
	if (want.input != written.input) {
		if (write_node(INPUT_NODE, want.input)) {
			written.input = want.input;
			changed = true;
		} else {
			ok = false;
		}
	}
	if (want.charge != written.charge) {
		if (write_node(CURRENT_NODE, want.charge)) {
			written.charge = want.charge;
			changed = true;
		} else {
			ok = false;
		}
	}
	if (changed) {
		const char *why = stop_reason();
		printf("axpcharge: %s (UDC '%s'): input limit %d mA, charge current %d mA%s%s%s%s (battery %ld uV)\n",
			   CABLE_NAMES[cable], udc_state, input_ma(want.input), want.charge,
			   want_current ? " (500 mA switch on)" : "", why ? ", stopped for " : "", why ? why : "",
			   ok ? "" : " (a node did not take it, retrying)", battery_uv());
	}

	charge_held = false;
	if (cable == CABLE_NONE) {
		logged_input = logged_current = 0xff; // said again at the next cable
		return ok;
	}

	// What the PMIC holds, checked against what was asked. The driver writes
	// REG16h at once and REG62h above 500 mA only from its thread, a few
	// seconds after a plug; a field still off is written directly, the way
	// the 500 mA switch always has been. REG16h only for a cable told apart:
	// for one that is not, the input stays whatever the driver and the PMIC
	// make of it, as it always has.
	uint8_t in, cur;
	if (!axp_read(AXP_REG_INPUT, &in) || !axp_read(AXP_REG_CURRENT, &cur)) {
		return false;
	}
	if (cable == CABLE_HOST || cable == CABLE_WALL) {
		uint8_t field = input_field(want.input);
		if ((in & AXP_INPUT_MASK) != field) {
			printf("axpcharge: REG16h=0x%02x, not the %d mA asked for; setting it\n", in, input_ma(want.input));
			ok = set_field(AXP_REG_INPUT, AXP_INPUT_MASK, field) && ok;
			axp_read(AXP_REG_INPUT, &in);
		}
	}
	uint8_t field = current_field(want.charge);
	if ((cur & AXP_CURRENT_MASK) != field) {
		ok = set_field(AXP_REG_CURRENT, AXP_CURRENT_MASK, field) && ok;
		axp_read(AXP_REG_CURRENT, &cur);
	}
	charge_held = want.charge == 0 && (cur & AXP_CURRENT_MASK) == 0;
	if (in != logged_input || cur != logged_current) {
		logged_input = in;
		logged_current = cur;
		printf("axpcharge: PMIC now REG16h=0x%02x (%d mA in), REG62h=0x%02x (%d mA)\n", in,
			   INPUT_STEP_MA[in & AXP_INPUT_MASK], cur, current_ma(cur & AXP_CURRENT_MASK));
	}
	return ok;
}

// shortcut: no voltage reading counts as low enough, the cap's old behaviour, so
// a model without the node (the R1?) is unchanged; give it its own node if it shuts off too.
static long battery_uv(void) {
	long uv = 0;
	FILE *f = fopen(BATTERY_UV_NODE, "r");
	if (f) {
		if (fscanf(f, "%ld", &uv) != 1) {
			uv = 0;
		}
		fclose(f);
	}
	return uv;
}

static void stop_charge(bool stop) {
	if (stop == charge_stopped) {
		return;
	}
	FILE *f = fopen(CHARGER_SWITCH_NODE, "w");
	if (!f) {
		return;
	}
	bool ok = fputs(stop ? "0" : "1", f) >= 0;
	ok = fclose(f) == 0 && ok;
	if (ok) {
		charge_stopped = stop;
		printf("axpcharge: charge %s (battery %ld uV)\n",
			   stop ? "stopped until the battery is down to the cap" : "resumed", battery_uv());
	}
}

static bool apply_voltage(void) {
	uint8_t v;
	if (!axp_read(AXP_REG_VOLTAGE, &v)) {
		return false;
	}
	if (!voltage_seen) {
		voltage_seen = true;
		if ((v & AXP_VOLTAGE_MASK) != AXP_VOLTAGE_CAP) {
			voltage_stock = v & AXP_VOLTAGE_MASK;
		}
	}
	bool cap = want_voltage;
	if (cap && (v & AXP_VOLTAGE_MASK) != AXP_VOLTAGE_CAP && battery_uv() > CAP_ARM_UV) {
		cap = false;
	}
	cap_stop = want_voltage && !cap;
	// On the Blaze the stop is a charge current of 0, decided with the rest
	// of the currents in apply_policy(): the driver's switch only zeroes
	// REG62h, and the driver puts its own current back over it at every plug
	// and at every write of the current.
	if (!axpcharge_cable_policy()) {
		stop_charge(cap_stop);
	}
	return set_field(AXP_REG_VOLTAGE, AXP_VOLTAGE_MASK, cap ? AXP_VOLTAGE_CAP : voltage_stock);
}

// On: a current above the cap is saved, then lowered; one already at or below
// it is left. Off: only a register holding the cap is touched, and only put
// back to a saved value, so a player that never used the switch keeps its own.
static bool apply_current(void) {
	uint8_t v;
	if (!axp_read(AXP_REG_CURRENT, &v)) {
		return false;
	}
	unsigned field = v & AXP_CURRENT_MASK;
	long saved = config_get_int("power", CURRENT_KEY, -1);

	if (want_current) {
		if (field <= AXP_CURRENT_CAP) {
			return true;
		}
		if (saved != (long)field) {
			config_set_int("power", CURRENT_KEY, (long)field);
			config_save();
		}
		return set_field(AXP_REG_CURRENT, AXP_CURRENT_MASK, AXP_CURRENT_CAP);
	}

	if (field != AXP_CURRENT_CAP || saved <= (long)AXP_CURRENT_CAP || saved > (long)AXP_CURRENT_MASK) {
		return true;
	}
	return set_field(AXP_REG_CURRENT, AXP_CURRENT_MASK, (uint8_t)saved);
}

static bool apply(void) {
	bool ok = apply_voltage();
	bool cap_held;
	if (axpcharge_cable_policy()) {
		block_applied = want_block;
		ok = apply_policy() && ok;
		// The indicator follows the charger, not the wish: hidden only while
		// REG62h reads back 0 for DAC mode.
		system_suppress_charging(dac_block_applies() && charge_held);
		cap_held = cap_stop && charge_held;
	} else {
		ok = apply_current() && ok;
		cap_held = charge_stopped;
	}

	uint8_t status;
	holding = want_voltage && (cap_held || (axp_read(AXP_REG_STATUS, &status) && (status & 7u) == AXP_STATUS_DONE));
	return ok;
}

// What the PMIC holds, once, so the log says what the switches started from.
static void log_first_contact(void) {
	static bool done;
	if (done) {
		return;
	}
	done = true;
	uint8_t volt, cur, in;
	if (!axp_read(AXP_REG_VOLTAGE, &volt) || !axp_read(AXP_REG_CURRENT, &cur) || !axp_read(AXP_REG_INPUT, &in)) {
		printf("axpcharge: the PMIC does not answer on %s at 0x%02x\n", AXP_BUS, AXP_ADDR);
		return;
	}
	printf("axpcharge: found REG64h=0x%02x, REG62h=0x%02x (%d mA), REG16h=0x%02x (%d mA in)\n", volt, cur,
		   current_ma(cur & AXP_CURRENT_MASK), in, INPUT_STEP_MA[in & AXP_INPUT_MASK]);
	// Why this boot started and why the last one ended, for reports of the
	// player switching itself off (REG21h seen: 0x01 long press, 0x02 software).
	uint8_t on_src, off_src;
	if (axp_read(0x20, &on_src) && axp_read(0x21, &off_src)) {
		printf("axpcharge: power-on source REG20h=0x%02x, last power-off REG21h=0x%02x\n", on_src, off_src);
	}
	// And what decides a power-off of that kind, for REG21h=0x20 (a DCDC under
	// voltage) seen after hangs: REG22h-25h are the protection enables and
	// thresholds (REG23h, which DCDCs may cut the power, is left at its OTP
	// default by the stock driver), REG48h-4Ah the interrupt flags latched since
	// the PMIC came up. Read only.
	uint8_t prot[4], irq[3];
	bool ok = true;
	for (int i = 0; i < 4; i++) {
		ok = axp_read((uint8_t)(0x22 + i), &prot[i]) && ok;
	}
	for (int i = 0; i < 3; i++) {
		ok = axp_read((uint8_t)(0x48 + i), &irq[i]) && ok;
	}
	if (ok) {
		printf("axpcharge: protection REG22h-25h=%02x %02x %02x %02x, latched IRQ REG48h-4Ah=%02x %02x %02x\n",
			   prot[0], prot[1], prot[2], prot[3], irq[0], irq[1], irq[2]);
	}
}

void axpcharge_set(bool limit_voltage, bool limit_current) {
	if (!axpcharge_applies()) {
		return;
	}
	log_first_contact();
	configured = true;
	want_voltage = limit_voltage;
	want_current = limit_current;
	if (axpcharge_cable_policy()) {
		look(now_ms());
	}
	bool ok = apply();
	next_ms = now_ms() + REASSERT_MS;
	printf("axpcharge: charge limit %s, 500 mA limit %s%s\n", want_voltage ? "on" : "off",
		   want_current ? "on" : "off", ok ? "" : " (the PMIC did not take it, retrying)");
}

void axpcharge_tick(void) {
	if (!configured) {
		return;
	}
	long long now = now_ms();
	bool due = now >= next_ms;
	if (axpcharge_cable_policy()) {
		// A new kind of cable, or a change of mind about DAC mode, is acted on
		// at this tick rather than at the next re-assertion.
		if (now >= next_look_ms && look(now)) {
			due = true;
		}
		if (block_applied != want_block) {
			due = true;
		}
	}
	if (!due) {
		return;
	}
	next_ms = now + REASSERT_MS;
	apply();
}

void axpcharge_block_charging(bool block) {
	want_block = block;
	printf("axpcharge: charging in DAC mode %s\n", block ? "off: stopping the charger" : "allowed");
}

bool axpcharge_holding(void) { return holding; }

bool axpcharge_reg_read(uint8_t reg, uint8_t *value) { return axp_read(reg, value); }
bool axpcharge_reg_write(uint8_t reg, uint8_t value) { return axp_write(reg, value); }

#endif /* HOST_BUILD */
