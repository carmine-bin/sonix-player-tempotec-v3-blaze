#include "sysinfo.h"

#include "src/system/core/respath.h"

#include "src/system/device/system.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/statvfs.h>

#define SYSINFO_FILE SONIX_RESOURCE_DIR "/components/system-info.json"

static char os_version[64];
static char build_version[64];
static char device_name[64];
static char dac_info[96];
static char ota_repo[128];

// Every player this binary knows how to be. Both serial prefixes are the ones
// printed on the box.
static const sysinfo_model_t MODELS[] = {
	{
		.name = "HiBy R3 Pro II",
		.panel_width = 480,
		.panel_height = 720,
		.update_stem = "r3proii",
		.serial_prefix = "R3PII",
		.pmic_charger = false,
		.cs43131 = false,
		.ak4493 = false,
		.front_keys = false,
		.tap_wake = true,
		.one_flank = false,
		.media_keys_swapped = true,
		.one_boot_logo = false,
		.boot_animation = false,
	},
	{
		.name = "HiBy R1",
		.panel_width = 480,
		.panel_height = 800,
		.update_stem = "r1",
		.serial_prefix = "R1",
		.pmic_charger = true,
		.cs43131 = true,
		.ak4493 = false,
		.front_keys = false,
		.tap_wake = false,
		.one_flank = true,
		.media_keys_swapped = false,
		.one_boot_logo = false,
		.boot_animation = false,
	},
	{
		// Charger in the AXP2101 PMIC; AK4493SEQ DAC with 3.5 mm and 4.4 mm.
		// Laid out at the 320x480 glass's own size: sizes go through ui_px().
		.name = "TempoTec V3 Blaze",
		.panel_width = 320,
		.panel_height = 480,
		.update_stem = "v3_analog_2025",
		.serial_prefix = "",
		.pmic_charger = true,
		.cs43131 = false,
		.ak4493 = true,
		.front_keys = true,
		.tap_wake = true, // stock hiby_player drives the same gt9xx gesture_sw node
		.one_flank = false,
		// Like the R3 Pro II's: KEY_NEXTSONG is the key that goes back,
		// confirmed on the device.
		.media_keys_swapped = true,
		// TempoTec's own logo is /etc/logo.jpeg. logo1 and logo2 are HiBy's,
		// left over from the base firmware and sized for other panels.
		.one_boot_logo = true,
		.boot_animation = true,
		// The RAM-less ST77903 takes a new page at 45 Hz.
		.slow_pan = true,
		// Its stock hiby_player raises the PMIC's input limit for a wall
		// charger; the driver alone leaves it at 500 mA.
		.cable_charge_policy = true,
	},
};

// ---------------------------------------------------------------------------
// the version file
// ---------------------------------------------------------------------------

// Case-insensitive strstr. Needed because the key name is typed by hand by
// whoever assembles the firmware image, and "os_version" instead of
// "OS_version" would leave the page showing two dashes and no explanation.
// (strcasestr exists but is a GNU extension needing a feature-test macro; ten
// lines cost less.)
static const char *find_nocase(const char *haystack, const char *needle) {
	size_t n = strlen(needle);
	if (n == 0) {
		return haystack;
	}
	for (const char *p = haystack; *p; p++) {
		size_t i = 0;
		while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
			i++;
		}
		if (i == n) {
			return p;
		}
	}
	return NULL;
}

// A miniature JSON reader, deliberately so: it looks for "key", then the colon,
// then the string that follows. It handles no numbers, nesting or escape
// sequences, and does not need to -- the file is defined here and holds two
// strings. All it has to do is survive a badly written file: on anything
// unexpected the field stays empty and the page shows a dash.
static bool json_string_field(const char *text, const char *key, char *out, size_t out_size) {
	out[0] = '\0';

	// The key in quotes, so "Build_version" is not matched inside a longer key
	// that contains it.
	char quoted[64];
	if (snprintf(quoted, sizeof(quoted), "\"%s\"", key) >= (int)sizeof(quoted)) {
		return false;
	}

	const char *p = find_nocase(text, quoted);
	if (!p) {
		return false;
	}
	p += strlen(quoted);

	while (*p && isspace((unsigned char)*p)) {
		p++;
	}
	if (*p != ':') {
		return false;
	}
	p++;
	while (*p && isspace((unsigned char)*p)) {
		p++;
	}
	if (*p != '"') {
		return false;
	}
	p++;

	const char *end = strchr(p, '"');
	if (!end) {
		return false;
	}

	size_t len = (size_t)(end - p);
	if (len >= out_size) {
		len = out_size - 1;
	}
	memcpy(out, p, len);
	out[len] = '\0';
	return true;
}

void sysinfo_load(void) {
	os_version[0] = '\0';
	build_version[0] = '\0';
	device_name[0] = '\0';
	dac_info[0] = '\0';
	ota_repo[0] = '\0';

	FILE *f = fopen(SYSINFO_FILE, "rb");
	if (!f) {
		printf("sysinfo: %s missing, versions not available\n", SYSINFO_FILE);
		return;
	}

	// The file is a handful of lines; anything longer is not the expected file
	// and gets truncated without ceremony.
	char buf[1024];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';

	// The names the file uses, matched case-insensitively by find_nocase.
	json_string_field(buf, "OS_version", os_version, sizeof(os_version));
	json_string_field(buf, "Build_version", build_version, sizeof(build_version));
	json_string_field(buf, "device-name", device_name, sizeof(device_name));
	json_string_field(buf, "dac-info", dac_info, sizeof(dac_info));
	json_string_field(buf, "ota-repo", ota_repo, sizeof(ota_repo));

	const sysinfo_model_t *model = sysinfo_model();
	printf("sysinfo: device '%s' (%s), system '%s', build '%s'\n", device_name,
		   model ? "known" : "NOT a model this build knows", os_version, build_version);
}

const char *sysinfo_os_version(void) { return os_version; }
const char *sysinfo_build_version(void) { return build_version; }
const char *sysinfo_device_name(void) { return device_name; }
const char *sysinfo_dac_info(void) { return dac_info; }
const char *sysinfo_ota_repo(void) { return ota_repo; }

// Matched without regard to case or to the spelling of "HiBy": the name is
// typed by hand into a text file, and "Hiby R1" is the same player as
// "HiBy R1".
const sysinfo_model_t *sysinfo_model(void) {
	if (!device_name[0]) {
		return NULL;
	}
	for (size_t i = 0; i < sizeof(MODELS) / sizeof(MODELS[0]); i++) {
		if (strcasecmp(device_name, MODELS[i].name) == 0) {
			return &MODELS[i];
		}
	}
	return NULL;
}

const sysinfo_model_t *sysinfo_model_by_panel(int width, int height) {
	for (size_t i = 0; i < sizeof(MODELS) / sizeof(MODELS[0]); i++) {
		if (MODELS[i].panel_width == width && MODELS[i].panel_height == height) {
			return &MODELS[i];
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// space on the card
// ---------------------------------------------------------------------------

bool sysinfo_sd_usage(sysinfo_storage_t *out) {
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));

	const char *root = storage_sd_root();
	if (!root) {
		return false;
	}

	struct statvfs st;
	if (statvfs(root, &st) != 0) {
		return false;
	}

	// f_frsize is the block size f_blocks is counted in; f_bsize is the
	// preferred I/O size and not the same thing, even though they match on many
	// filesystems. Some leave f_frsize at zero.
	uint64_t unit = st.f_frsize ? (uint64_t)st.f_frsize : (uint64_t)st.f_bsize;
	if (unit == 0 || st.f_blocks == 0) {
		return false;
	}

	out->present = true;
	out->total = (uint64_t)st.f_blocks * unit;
	// f_bfree counts reserved blocks too, f_bavail only those an ordinary
	// process can really use. For "how much is left" f_bavail is the honest
	// answer; used space is measured against f_bfree, or the figures would not
	// add up to the total.
	out->free = (uint64_t)st.f_bavail * unit;
	out->used = out->total - (uint64_t)st.f_bfree * unit;

	if (out->used > out->total) {
		out->used = out->total;
	}
	out->used_percent = (int)((out->used * 100 + out->total / 2) / out->total);
	if (out->used_percent > 100) {
		out->used_percent = 100;
	}
	return true;
}

void sysinfo_format_size(uint64_t bytes, char *out, size_t out_size) {
	const uint64_t KB = 1024;
	const uint64_t MB = KB * 1024;
	const uint64_t GB = MB * 1024;

	if (bytes >= GB) {
		// One decimal: "58.9 GB" says what is needed, "58.94 GB" does not.
		unsigned long long whole = bytes / GB;
		unsigned long long tenths = ((bytes % GB) * 10) / GB;
		snprintf(out, out_size, "%llu.%llu GB", whole, tenths);
	} else if (bytes >= MB) {
		snprintf(out, out_size, "%llu MB", (unsigned long long)((bytes + MB / 2) / MB));
	} else {
		snprintf(out, out_size, "%llu KB", (unsigned long long)((bytes + KB / 2) / KB));
	}
}

// ---------------------------------------------------------------------------
// serial number
// ---------------------------------------------------------------------------

const char *sysinfo_serial_number(void) {
	// Read once and kept: the efuses are burned at the factory.
	static char serial[24];
	static bool tried;
	if (tried) {
		return serial;
	}
	tried = true;
	serial[0] = '\0';

	const sysinfo_model_t *model = sysinfo_model();
	if (!model) {
		return serial; // no prefix to build it from
	}

	FILE *f = fopen("/proc/jz/efuse/efuse_chip_id", "r");
	if (!f) {
		return serial; // build host, or firmware without the efuse module
	}
	char line[96] = "";
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return serial;
	}
	fclose(f);

	// "CHIP_ID: 90a70a428496c4012f146c0404000001" -- the serial printed on the
	// box is the model's prefix plus the first eight hex digits, uppercased
	// (verified on a real R3 Pro II).
	const char *hex = strchr(line, ':');
	hex = hex ? hex + 1 : line;
	while (*hex == ' ' || *hex == '\t') {
		hex++;
	}

	char id[9];
	int n = 0;
	while (n < 8 && isxdigit((unsigned char)hex[n])) {
		id[n] = (char)toupper((unsigned char)hex[n]);
		n++;
	}
	id[n] = '\0';
	if (n == 8) {
		snprintf(serial, sizeof(serial), "%s%s", model->serial_prefix, id);
	}
	return serial;
}
