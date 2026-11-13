#ifndef SYSINFO_H
#define SYSINFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What the information page has to show: the two version numbers, and how much
// space is left on the card.

// ---------------------------------------------------------------------------
// versions
// ---------------------------------------------------------------------------
//
// Not compiled into the binary but read from /usr/resource/sonix/components/system-info.json, a
// text file next to the language directory:
//
//     {
//         "device-name": "HiBy R3 Pro II",
//         "dac-info": "Dual Cirrus Logic CS43198",
//         "OS_version": "1.0",
//         "Build_version": "182",
//         "ota-repo": "Jepl4r/sonix-player"
//     }
//
// This way updating the build number does not mean rebuilding, and whoever
// assembles a firmware image can write it from the build script. A missing or
// unreadable file is not an error: the entries show a dash.
//
// "device-name" is what tells one player from another. The firmware packer
// writes a different one into each image, and it is the whole of what this
// binary knows about which machine it woke up on -- see sysinfo_model().

// Rereads the file. Called once at startup; the functions below do not touch
// the disk.
void sysinfo_load(void);

// "" when the file is missing or does not carry the key.
const char *sysinfo_os_version(void);
const char *sysinfo_build_version(void);

// The name of the player, verbatim from the file: "HiBy R3 Pro II", "HiBy R1".
// "" when the file does not say.
const char *sysinfo_device_name(void);

// The converter, verbatim from the file, for the information page to print.
// The player never decides anything by it.
const char *sysinfo_dac_info(void);

// The GitHub repository the update from the internet looks in, "owner/name",
// verbatim from the file (ota.h). "" when the file does not say.
const char *sysinfo_ota_repo(void);

// ---------------------------------------------------------------------------
// the models, and what differs between them
// ---------------------------------------------------------------------------
//
// One table, keyed on device-name.
//
// The panel is written here rather than read from the framebuffer: the pages
// subtract from screen_height in unsigned arithmetic, and LVGL's fbdev driver
// reports 800x480 until its file is set. The framebuffer size only picks a row
// of this table, by exact match, when the name is missing (panel_size() in
// main.c).
typedef struct {
	const char *name;		 // as written in system-info.json
	int panel_width;		 // the panel, on the device and on the simulator alike
	int panel_height;
	const char *update_stem; // "<stem>.upt" is the update file this player accepts
	const char *serial_prefix; // what the number on the box starts with
	bool pmic_charger;		   // charged by the AXP2101 alone, no MP2731 (axpcharge.h)
	bool cs43131;			   // one CS43131, 3.5 mm only, no HBC3000 (alsa-controls.h)
	bool ak4493;			   // dual AK4493 behind the HBC3000: six filters, no DRE/NOS (alsa-controls.h)
	bool front_keys;		   // previous, play/pause and next under the screen, the volume on the right flank (remap.c)
	bool tap_wake;			   // the touch controller can wake the screen (power.h)
	bool one_flank;			   // every button on the right flank (remap.c)
	bool media_keys_swapped;   // KEY_NEXTSONG is the upper skip key, not next (system.c)
	bool one_boot_logo;		   // /etc/logo.jpeg whatever the theme; logo1/logo2 are not its own (bootlogo.h)
	bool boot_animation;	   // plays TempoTec's frames over that logo at startup (bootanim.h)
	bool slow_pan;			   // FBIOPAN_DISPLAY holds the caller 10-16 ms: drawn into RAM, flipped on a thread (main.c)
	bool cable_charge_policy;  // input and charge current set by the kind of cable, as its stock player does (axpcharge.h)
} sysinfo_model_t;

// The entry for the name in the file, or NULL when the file says nothing or
// names a player this binary does not know. NULL is not a failure to paper
// over: see firmware_update_file_find().
const sysinfo_model_t *sysinfo_model(void);

// The entry whose panel is exactly width x height, or NULL. For sizing the
// interface when system-info.json names no model; not for choosing an update
// file or a serial prefix, which need the name itself.
const sysinfo_model_t *sysinfo_model_by_panel(int width, int height);

// The device serial number: the model's prefix plus the first eight hex digits
// (upper case) of the SoC efuse chip id (/proc/jz/efuse/efuse_chip_id, line
// "CHIP_ID: <32 hex>"). It is exactly the number printed on the box of either
// model (chip id 90a70a42... -> box R3PII90A70A42). "" when the node
// is absent (the host build, or a firmware without the efuse module) or the
// model is unknown. Read once and kept: efuses do not change.
const char *sysinfo_serial_number(void);

// ---------------------------------------------------------------------------
// microSD card
// ---------------------------------------------------------------------------

typedef struct {
	bool present;	 // false when no card is mounted
	uint64_t total;	 // bytes
	uint64_t used;	 // bytes
	uint64_t free;	 // bytes available to this process, not counting reserved
	int used_percent; // 0..100, rounded
} sysinfo_storage_t;

// false when there is no card or it cannot be queried.
bool sysinfo_sd_usage(sysinfo_storage_t *out);

// "12.3 GB", "512 MB", "48 KB". Decimal point rather than comma: it is the same
// string in all five languages and does not pass through tr().
void sysinfo_format_size(uint64_t bytes, char *out, size_t out_size);

#endif /* SYSINFO_H */
