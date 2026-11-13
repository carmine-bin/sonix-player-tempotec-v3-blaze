#include "wifi.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "src/system/device/clock.h"
#include "src/system/core/config.h"
#include "src/system/core/logging.h"
#include "src/system/device/power.h"
#include "src/system/device/sysserver.h"
#include "src/system/core/utils.h"

// The interface. Both the stock scripts and sys_server hard-code it, so there
// is nothing to discover.
#define WIFI_IFACE "wlan0"

// Where sys_server leaves what it read out of wpa_supplicant. These are the
// stock player's paths, taken from the daemon's own strings.
#define WIFI_RESULT_FILE "/data/wifi_result.txt"
#define WIFI_STATUS_FILE "/data/wifi_status.txt"
#define WIFI_NETWORK_FILE "/data/wifi_network.txt"

#define WPA_CLI "wpa_cli"
#define UDHCPC "udhcpc"
#define WIFI_ON_SCRIPT "/usr/bin/wifi_on.sh"
#define WIFI_OFF_SCRIPT "/usr/bin/wifi_off.sh"

// The name the device announces over DHCP, same as wifi_on.sh's default.
#define WIFI_HOSTNAME "HiBy_Music"

// How long a scan is given before its results are read back. wpa_supplicant
// needs a good two seconds to walk the channels; asking sooner returns the
// previous sweep.
#define SCAN_SETTLE_MS 3000

// How many times a scan's results are read before an empty table is believed:
// at SCAN_SETTLE_MS, then twice more half of it apart.
#define SCAN_READS 3

// The status poll while the radio is up. Two seconds is fast enough for the
// status bar's bars to follow the room and cheap enough to ignore: one wpa_cli
// fork, with the worker asleep in between.
#define STATUS_POLL_MS 2000

// The same poll with the screen off. poll_status() forks wpa_cli, plus a second
// wpa_cli signal_poll while associated, so a two-second period is up to 3600
// forks an hour to update an icon nobody is looking at. Ten seconds until
// standby switches the radio off entirely. Commands do not come through here:
// they arrive as jobs and wake the thread immediately.
#define STATUS_POLL_STANDBY_MS 10000

// With the radio off there is nothing to ask anybody.
#define IDLE_WAIT_MS 300000

// How often the worker asks wpa_cli how things are going. Normally two
// seconds; slowed right down while something is moving bulk data over the
// radio, because every one of those polls is a fork of this whole process --
// and on one core, a page-table copy of the player every two seconds is time
// the transfer is not getting. The status bar's signal bars lag by a few
// seconds while a transfer runs, and that is the whole cost.
static bool g_poll_slow;

void wifi_set_status_poll_slow(bool slow) { g_poll_slow = slow; }

// A DHCP attempt is not retried more often than this, so an access point that
// hands out no lease does not turn into a fork loop.
#define DHCP_RETRY_MS 20000

// How long a join is given before it is called off, and how often the state is
// read while it runs.
//
// select_network makes wpa_supplicant sweep the channels before it can
// associate at all, and on a crowded 2.4 GHz band that sweep alone eats several
// seconds; a first association that has to be retried eats several more. Thirty
// seconds covers the slow-but-fine case and is still short enough that a router
// which is never going to answer does not hold the page.
#define ASSOC_TIMEOUT_MS 30000
#define ASSOC_POLL_MS 500

// How long the supplicant has to sit still before the wait gives up early.
//
// DISCONNECTED is not a verdict: it is where wpa_supplicant parks between two
// scans and between two association attempts, so a single sample of it says
// nothing. Only an unbroken run of it this long means the join is not coming.
#define ASSOC_PARKED_MS 10000

// Signal thresholds, in dBm, for the four glyphs (zero / low / high / max).
#define RSSI_MAX_DBM (-55)
#define RSSI_HIGH_DBM (-67)
#define RSSI_LOW_DBM (-80)

// ---------------------------------------------------------------------------
// state, shared between the worker and the interface thread
// ---------------------------------------------------------------------------

typedef enum {
	JOB_NONE = 0,
	JOB_POWER,	   // arg_int: 1 on, 0 off
	JOB_SCAN,
	JOB_CONNECT,   // arg_a: ssid, arg_b: psk ("" = open)
	JOB_DISCONNECT,
	JOB_FORGET,	   // arg_a: ssid
} job_type_t;

typedef struct {
	job_type_t type;
	char arg_a[WIFI_SSID_MAX];
	char arg_b[128];
	int arg_int;
} job_t;

#define JOB_QUEUE_LEN 8

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static bool worker_running;

static job_t queue[JOB_QUEUE_LEN];
static int queue_head, queue_count;

static bool g_enabled;			 // the switch, as the user left it
static bool g_power_busy;		 // a power job is in flight
static bool g_scanning;
static wifi_status_t g_status;
static wifi_network_t g_networks[WIFI_MAX_NETWORKS];
static int g_network_count;
static uint32_t g_serial;

static wifi_op_t g_op = WIFI_OP_IDLE;
static char g_op_ssid[WIFI_SSID_MAX];
static wifi_fail_t g_fail = WIFI_FAIL_NONE;
static wifi_opkind_t g_opkind = WIFI_OPKIND_CONNECT;

// Set when the user leaves or forgets a network, cleared as soon as the radio
// reports a link again.
//
// Clearing the cached status is not enough on its own: refresh_networks() reads
// the CURRENT flag out of list_networks, which means "the network
// wpa_supplicant has selected" and stays set for a while after the association
// is gone. While this is up, nothing is current, whatever the saved list says.
static bool link_dropped;

// ---------------------------------------------------------------------------
// running things
// ---------------------------------------------------------------------------

// Runs a program and collects its stdout. fork+exec rather than popen, because
// the arguments are SSIDs and passphrases the user typed and a shell would
// execute a network name such as "; rm -rf /". Returns the exit status, or -1
// when the child could not be started.
static int run_argv(char *const argv[], char *out, size_t out_size) {
	if (out && out_size) {
		out[0] = '\0';
	}

	int pipe_fd[2];
	if (pipe(pipe_fd) != 0) {
		return -1;
	}

	// vfork, not fork: this runs every two seconds while Wi-Fi is on, and
	// fork() copies the page tables of the whole player first -- on this one
	// core that showed up in a profile. The child below only makes system
	// calls (dup2, open, close, exec), which is what vfork allows.
	pid_t pid = vfork();
	if (pid < 0) {
		close(pipe_fd[0]);
		close(pipe_fd[1]);
		return -1;
	}

	if (pid == 0) {
		// Child: stdout down the pipe, stderr to the log through the daemons'
		// pipe (a script may leave wpa_supplicant running), nothing on stdin.
		close(pipe_fd[0]);
		logging_child_stdio();
		dup2(pipe_fd[1], STDOUT_FILENO);
		close(pipe_fd[1]);
		int devnull = open("/dev/null", O_RDONLY);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			close(devnull);
		}
		execvp(argv[0], argv);
		_exit(127);
	}

	close(pipe_fd[1]);

	size_t used = 0;
	char scratch[256];
	for (;;) {
		ssize_t got = read(pipe_fd[0], scratch, sizeof(scratch));
		if (got <= 0) {
			break;
		}
		if (out && out_size && used + 1 < out_size) {
			size_t room = out_size - used - 1;
			size_t take = ((size_t)got < room) ? (size_t)got : room;
			memcpy(out + used, scratch, take);
			used += take;
			out[used] = '\0';
		}
	}
	close(pipe_fd[0]);

	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
		// interrupted; keep waiting
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Runs wpa_cli -i wlan0 with the given arguments, collecting its output.
static int wpa(char *const *args, int arg_count, char *out, size_t out_size) {
	char *argv[12];
	int n = 0;
	argv[n++] = (char *)WPA_CLI;
	argv[n++] = (char *)"-i";
	argv[n++] = (char *)WIFI_IFACE;
	for (int i = 0; i < arg_count && n < 11; i++) {
		argv[n++] = args[i];
	}
	argv[n] = NULL;
	return run_argv(argv, out, out_size);
}

static int wpa1(const char *a, char *out, size_t out_size) {
	char *args[1] = {(char *)a};
	return wpa(args, 1, out, out_size);
}

static int wpa2(const char *a, const char *b, char *out, size_t out_size) {
	char *args[2] = {(char *)a, (char *)b};
	return wpa(args, 2, out, out_size);
}

static int wpa4(const char *a, const char *b, const char *c, const char *d, char *out, size_t out_size) {
	char *args[4] = {(char *)a, (char *)b, (char *)c, (char *)d};
	return wpa(args, 4, out, out_size);
}

// Whether wpa_cli's answer means the command was taken.
//
// wpa_cli exits 0 whatever the supplicant thought of the request: a rejected
// set_network is a successful run of a program that printed "FAIL". The reply
// text is the only place the refusal appears, so it has to be read.
static bool wpa_ok(const char *out) { return out && out[0] && strncmp(out, "FAIL", 4) != 0; }

// Reads a whole small file into the buffer. True when something was read.
static bool slurp(const char *path, char *buf, size_t size) {
	buf[0] = '\0';
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	size_t got = fread(buf, 1, size - 1, f);
	fclose(f);
	buf[got] = '\0';
	return got > 0;
}

static void sleep_ms(int ms) {
	struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

// With bcm_wlbt_power (brcmfmac in place of cywdhd) wlan0 exists only while Wi-Fi
// is on: wifi_on.sh powers the chip and loads the driver.
#define WIFI_RADIO_POWER "/sys/devices/platform/bcm_wlbt_power/wifi_power"

bool wifi_available(void) {
	return access("/sys/class/net/" WIFI_IFACE, F_OK) == 0 || access(WIFI_RADIO_POWER, F_OK) == 0;
}

// ---------------------------------------------------------------------------
// parsing what wpa_supplicant says
// ---------------------------------------------------------------------------

static int bars_for(int dbm) {
	if (dbm == 0) {
		return 0;
	}
	if (dbm >= RSSI_MAX_DBM) {
		return 3;
	}
	if (dbm >= RSSI_HIGH_DBM) {
		return 2;
	}
	if (dbm >= RSSI_LOW_DBM) {
		return 1;
	}
	return 0;
}

// Pulls "key=value" out of a wpa_cli status/signal_poll block.
static bool status_field(const char *text, const char *key, char *out, size_t size) {
	size_t key_len = strlen(key);
	const char *p = text;

	while (p && *p) {
		if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
			const char *value = p + key_len + 1;
			const char *end = strchr(value, '\n');
			size_t len = end ? (size_t)(end - value) : strlen(value);
			if (len >= size) {
				len = size - 1;
			}
			memcpy(out, value, len);
			out[len] = '\0';
			// Strip trailing CR and spaces, which turn up over a serial console.
			while (len > 0 && (out[len - 1] == '\r' || out[len - 1] == ' ')) {
				out[--len] = '\0';
			}
			return true;
		}
		p = strchr(p, '\n');
		if (p) {
			p++;
		}
	}
	out[0] = '\0';
	return false;
}

// wpa_supplicant never prints an SSID raw: it runs the name through
// printf_encode first, so every byte outside plain ASCII comes back as a C
// escape. A hidden network, one that answers a probe without giving its name,
// therefore arrives as a run of "\x00".
//
// Decodes the escapes in place and returns the resulting length, so the caller
// can tell a real name from a string of nothing.
static size_t ssid_unescape(char *text) {
	char *read = text;
	char *write = text;

	while (*read) {
		if (*read != '\\' || read[1] == '\0') {
			*write++ = *read++;
			continue;
		}

		read++; // step over the backslash
		switch (*read) {
		case 'n':
			*write++ = '\n';
			read++;
			break;
		case 'r':
			*write++ = '\r';
			read++;
			break;
		case 't':
			*write++ = '\t';
			read++;
			break;
		case 'e':
			*write++ = 27;
			read++;
			break;
		case '\\':
		case '"':
			*write++ = *read++;
			break;
		case 'x': {
			int value = 0;
			int digits = 0;
			read++;
			while (digits < 2 && isxdigit((unsigned char)*read)) {
				char c = *read++;
				int nibble = (c <= '9') ? c - '0' : (c | 0x20) - 'a' + 10;
				value = value * 16 + nibble;
				digits++;
			}
			if (digits == 0) {
				*write++ = 'x'; // a lone "\x" is kept rather than swallowed
			} else {
				*write++ = (char)value;
			}
			break;
		}
		default:
			*write++ = *read++;
			break;
		}
	}

	*write = '\0';
	return (size_t)(write - text);
}

// True when a decoded SSID does not belong in a list: empty, or nothing but
// NULs and control bytes. Those are hidden networks, and there is no name to
// tap.
static bool ssid_is_hidden(const char *ssid, size_t length) {
	if (length == 0) {
		return true;
	}
	for (size_t i = 0; i < length; i++) {
		unsigned char c = (unsigned char)ssid[i];
		if (c >= 0x20 && c != 0x7f) {
			return false; // at least one readable character
		}
	}
	return true;
}

// One line of scan_results:
//   bssid <tab> frequency <tab> signal <tab> flags <tab> ssid
// The header line and anything with fewer fields is skipped.
static bool parse_scan_line(char *line, wifi_network_t *out) {
	char *fields[5];
	int n = 0;

	fields[n++] = line;
	for (char *p = line; *p && n < 5; p++) {
		if (*p == '\t') {
			*p = '\0';
			fields[n++] = p + 1;
		}
	}
	if (n < 5) {
		return false;
	}
	if (strncmp(fields[0], "bssid", 5) == 0) {
		return false; // the header line
	}
	if (ssid_is_hidden(fields[4], ssid_unescape(fields[4]))) {
		return false; // hidden network: nothing to show and nothing to tap
	}

	memset(out, 0, sizeof(*out));
	snprintf(out->ssid, sizeof(out->ssid), "%s", fields[4]);
	out->signal = atoi(fields[2]);
	out->bars = bars_for(out->signal);
	out->secured = strstr(fields[3], "WPA") != NULL || strstr(fields[3], "WEP") != NULL;
	out->id = -1;
	return true;
}

// One line of list_networks:
//   id <tab> ssid <tab> bssid <tab> flags
static bool parse_network_line(char *line, char *ssid, size_t ssid_size, int *id, bool *current) {
	char *fields[4];
	int n = 0;

	fields[n++] = line;
	for (char *p = line; *p && n < 4; p++) {
		if (*p == '\t') {
			*p = '\0';
			fields[n++] = p + 1;
		}
	}
	if (n < 2) {
		return false;
	}
	if (fields[0][0] < '0' || fields[0][0] > '9') {
		return false; // the header line
	}

	*id = atoi(fields[0]);
	// Saved names come back escaped too: the same printf_encode on the way out.
	ssid_unescape(fields[1]);
	snprintf(ssid, ssid_size, "%s", fields[1]);
	*current = (n >= 4) && strstr(fields[3], "CURRENT") != NULL;
	return true;
}

// ---------------------------------------------------------------------------
// The readers. The files sys_server writes are the stock path and still work
// where wpa_cli's control socket cannot be opened; each reader below says which
// of the two it trusts first.
// ---------------------------------------------------------------------------

// wpa_cli first, and its answer stands even when the table is empty. The file
// is a dump sys_server takes when it runs a queued WIFI:SCAN, so right after
// the radio comes up it holds an empty table, and after a power cycle it can
// hold the last room the radio was in. It is read only when wpa_cli did not
// run at all.
static void read_scan_results(char *buf, size_t size) {
	if (wpa1("scan_results", buf, size) == 0) {
		char *end = strrchr(buf, '\n');
		if (end && end[1]) {
			end[1] = '\0'; // a full buffer ends mid-line: drop the cut one
		}
		return;
	}
	slurp(WIFI_RESULT_FILE, buf, size);
}

static void read_saved_networks(char *buf, size_t size) {
	if (slurp(WIFI_NETWORK_FILE, buf, size) && strchr(buf, '\t')) {
		return;
	}
	wpa1("list_networks", buf, size);
}

static void read_status(char *buf, size_t size) {
	// sys_server rewrites the status file only when asked, so wpa_cli is the
	// fresher of the two here and goes first.
	if (wpa1("status", buf, size) == 0 && strstr(buf, "wpa_state=")) {
		return;
	}
	slurp(WIFI_STATUS_FILE, buf, size);
}

// ---------------------------------------------------------------------------
// worker: the jobs
// ---------------------------------------------------------------------------

// The reason the last join ended as it did. Written by the worker, read by the
// interface, so it takes the same lock everything else shared does.
static void set_fail(wifi_fail_t reason) {
	pthread_mutex_lock(&lock);
	g_fail = reason;
	pthread_mutex_unlock(&lock);
}

static void set_opkind(wifi_opkind_t kind) {
	pthread_mutex_lock(&lock);
	g_opkind = kind;
	pthread_mutex_unlock(&lock);
}

static void set_op(wifi_op_t op, const char *ssid) {
	pthread_mutex_lock(&lock);
	g_op = op;
	if (ssid) {
		snprintf(g_op_ssid, sizeof(g_op_ssid), "%s", ssid);
	}
	pthread_mutex_unlock(&lock);
}

// Every operation goes through sys_server first; the direct wpa_cli path is the
// fallback for a firmware, or a host build, without the daemon.
static bool have_sysserver(void) {
	static int cached = -1;
	if (cached < 0) {
		cached = sysserver_available() ? 1 : 0;
	}
	return cached == 1;
}

static void run_script(const char *path) {
	if (access(path, X_OK) != 0) {
		return;
	}
	char *argv[3] = {(char *)"sh", (char *)path, NULL};
	run_argv(argv, NULL, 0);
}

// Priority 1 for every saved network and 2 for the one being joined, which is
// what the stock sys_server does (0x4049e0 over its saved list, then 2 for the
// chosen one at 0x405ac0 and 0x40536c). With all of them enabled again after
// the join (enable_all_networks), wpa_supplicant prefers the network picked
// last while it is in range and takes any other saved one when it is not.
static void set_priorities(int chosen) {
	static char buf[4096];
	if (wpa1("list_networks", buf, sizeof(buf)) != 0) {
		return;
	}

	char *save_line = NULL;
	for (char *line = strtok_r(buf, "\n", &save_line); line; line = strtok_r(NULL, "\n", &save_line)) {
		char name[WIFI_SSID_MAX];
		char id_text[16];
		char out[64];
		int id = -1;
		bool current = false;
		if (!parse_network_line(line, name, sizeof(name), &id, &current)) {
			continue;
		}
		snprintf(id_text, sizeof(id_text), "%d", id);
		wpa4("set_network", id_text, "priority", id == chosen ? "2" : "1", out, sizeof(out));
	}
}

// select_network leaves every other saved network disabled, and save_config
// writes that down, so the player would only ever rejoin the network picked
// last, reboots included. The stock sys_server ends every join with
// enable_network all (0x405490, 0x4058c4), after its save_config, so on stock
// the others stay enabled until wpa_supplicant restarts; here `save` keeps
// them enabled in the file too. Only networks that are disabled are touched:
// one parked after a refused handshake (TEMP-DISABLED) stays parked.
static void enable_all_networks(bool save) {
	char out[64];
	wpa2("enable_network", "all", out, sizeof(out));
	if (save && wpa_ok(out)) {
		wpa1("save_config", out, sizeof(out));
	}
}

// How long the radio is given to come up, or to go down, after sys_server has
// taken the request. wifi_on.sh alone sleeps 1.3 s after starting the
// supplicant, and the daemon starts a request up to a second after answering it.
#define SUPPLICANT_UP_MS 8000
#define SUPPLICANT_DOWN_MS 4000
#define SUPPLICANT_POLL_MS 250

// sys_server answers "OK" the moment it queues WIFI:ON or WIFI:OFF and runs it
// up to a second later (its queue thread sleeps a second whenever the queue is
// empty), so the reply says nothing about the supplicant. Waits until wpa_cli
// gets an answer out of it (`up`) or stops getting one (`!up`). True when that
// happened in time.
//
// Down matters as much as up: a WIFI:OFF followed at once by a WIFI:ON (the
// switch flicked twice) would otherwise find the old supplicant still answering,
// call the radio up, and have it killed a moment later by the queued OFF.
static bool wait_for_supplicant(bool up) {
	char buf[512];
	int limit = up ? SUPPLICANT_UP_MS : SUPPLICANT_DOWN_MS;
	for (int waited = 0; waited < limit; waited += SUPPLICANT_POLL_MS) {
		sleep_ms(SUPPLICANT_POLL_MS);
		bool answers = wpa1("status", buf, sizeof(buf)) == 0 && strstr(buf, "wpa_state=");
		if (answers == up) {
			printf("wifi: wpa_supplicant %s after %d ms\n", up ? "answered" : "gone",
				   waited + SUPPLICANT_POLL_MS);
			return true;
		}
	}
	printf("wifi: wpa_supplicant still %s after %d ms\n", up ? "silent" : "answering", limit);
	return false;
}

static void do_power(bool on) {
	char command[64];
	char reply[128];

	if (have_sysserver()) {
		snprintf(command, sizeof(command), on ? "WIFI:ON:%s" : "WIFI:OFF:%s", WIFI_IFACE);
		if (sysserver_request(command, reply, sizeof(reply)) == 0) {
			// Builds before enable_all_networks() saved every network but the
			// last one joined as disabled. Enabled again in memory at each
			// power-on, so the radio finds any of them; the next join saves it.
			if (wait_for_supplicant(on) && on) {
				enable_all_networks(false);
			}
			return;
		}
	}

	run_script(on ? WIFI_ON_SCRIPT : WIFI_OFF_SCRIPT);
	sleep_ms(on ? 1500 : 300);
	if (on) {
		enable_all_networks(false);
	}
}

// Reads the scan and the saved list, merges them, and publishes the result.
// True when the scan held at least one line. With `keep_if_empty` an empty scan
// is not published, so a retry does not replace the list on screen with
// nothing in between.
static bool refresh_networks(bool keep_if_empty) {
	static char scan_buf[16384];
	static char saved_buf[4096];

	read_scan_results(scan_buf, sizeof(scan_buf));
	bool scanned = strchr(scan_buf, '\t') != NULL;
	if (!scanned && keep_if_empty) {
		return false;
	}
	read_saved_networks(saved_buf, sizeof(saved_buf));

	wifi_network_t found[WIFI_MAX_NETWORKS];
	int count = 0;

	// The scan first: it is the only source of signal strength.
	char *save_line = NULL;
	for (char *line = strtok_r(scan_buf, "\n", &save_line); line && count < WIFI_MAX_NETWORKS;
		 line = strtok_r(NULL, "\n", &save_line)) {
		wifi_network_t net;
		if (!parse_scan_line(line, &net)) {
			continue;
		}

		// On a mesh the same SSID comes back once per access point. Keep the
		// strongest and drop the rest, so the list shows networks, not radios.
		bool merged = false;
		for (int i = 0; i < count; i++) {
			if (strcmp(found[i].ssid, net.ssid) == 0) {
				if (net.signal > found[i].signal) {
					found[i].signal = net.signal;
					found[i].bars = net.bars;
				}
				found[i].secured = found[i].secured || net.secured;
				merged = true;
				break;
			}
		}
		if (!merged) {
			found[count++] = net;
		}
	}

	// Then the saved ones: they mark what is already known and add networks that
	// are configured but out of range right now.
	save_line = NULL;
	for (char *line = strtok_r(saved_buf, "\n", &save_line); line; line = strtok_r(NULL, "\n", &save_line)) {
		char ssid[WIFI_SSID_MAX];
		int id = -1;
		bool current = false;
		if (!parse_network_line(line, ssid, sizeof(ssid), &id, &current)) {
			continue;
		}

		bool matched = false;
		for (int i = 0; i < count; i++) {
			if (strcmp(found[i].ssid, ssid) == 0) {
				found[i].saved = true;
				found[i].id = id;
				found[i].current = found[i].current || current;
				matched = true;
				break;
			}
		}
		if (!matched && count < WIFI_MAX_NETWORKS && ssid[0]) {
			wifi_network_t net;
			memset(&net, 0, sizeof(net));
			snprintf(net.ssid, sizeof(net.ssid), "%s", ssid);
			net.saved = true;
			net.secured = true; // unknown until seen again; assume the safer of the two
			net.current = current;
			net.id = id;
			found[count++] = net;
		}
	}

	// Strongest first, with the connected network pinned to the top.
	for (int i = 1; i < count; i++) {
		wifi_network_t key = found[i];
		int j = i - 1;
		while (j >= 0 && ((key.current && !found[j].current) ||
						  (key.current == found[j].current && key.signal > found[j].signal))) {
			found[j + 1] = found[j];
			j--;
		}
		found[j + 1] = key;
	}

	pthread_mutex_lock(&lock);
	if (link_dropped) {
		for (int i = 0; i < count; i++) {
			found[i].current = false;
		}
	}
	memcpy(g_networks, found, sizeof(wifi_network_t) * (size_t)count);
	g_network_count = count;
	g_serial++;
	pthread_mutex_unlock(&lock);
	return scanned;
}

static void do_scan(void) {
	char out[64];

	// wpa_cli scan is answered by the supplicant itself. FAIL-BUSY means a scan
	// is already running: its results are the ones read below.
	if (wpa1("scan", out, sizeof(out)) != 0 && !out[0] && have_sysserver()) {
		char reply[128];
		char command[64];
		snprintf(command, sizeof(command), "WIFI:SCAN:%s", WIFI_IFACE);
		sysserver_request(command, reply, sizeof(reply));
	}

	// An empty table right after the radio came up is a sweep not finished yet,
	// not an empty room: look again twice before saying there is nothing.
	for (int tries = 0; tries < SCAN_READS; tries++) {
		sleep_ms(tries ? SCAN_SETTLE_MS / 2 : SCAN_SETTLE_MS);
		if (refresh_networks(tries + 1 < SCAN_READS)) {
			break;
		}
	}
}

// The id wpa_supplicant knows this SSID under, or -1.
//
// This one asks wpa_cli and only falls back to the file sys_server wrote, which
// is the opposite of what the list on screen does, and deliberately. The file
// is rewritten only when a scan is asked for, so between two scans it can name
// networks under numbers wpa_supplicant has since given to something else --
// and unlike a row drawn one id out of date, "WIFI:CONNECT:wlan0 3" with a
// stale 3 joins the wrong network or nothing at all.
static int saved_network_id(const char *ssid) {
	static char buf[4096];
	if (!(wpa1("list_networks", buf, sizeof(buf)) == 0 && strchr(buf, '\t'))) {
		read_saved_networks(buf, sizeof(buf));
	}

	char *save_line = NULL;
	for (char *line = strtok_r(buf, "\n", &save_line); line; line = strtok_r(NULL, "\n", &save_line)) {
		char name[WIFI_SSID_MAX];
		int id = -1;
		bool current = false;
		if (parse_network_line(line, name, sizeof(name), &id, &current) && strcmp(name, ssid) == 0) {
			return id;
		}
	}
	return -1;
}

// wpa_supplicant wants string values quoted, so the argument handed to wpa_cli
// literally includes the double quotes. This is another reason the call goes
// through execv and never through a shell.
//
// The quotes are closed here, which means a value carrying one of its own has
// to be escaped: `set_network 0 psk "ab"cd"` is not a passphrase with a
// quote in it, it is a syntax error, and wpa_supplicant answers FAIL. Same for
// a backslash. `out` needs room for twice the value plus the two quotes.
static void quoted(const char *value, char *out, size_t size) {
	size_t at = 0;
	if (size < 3) {
		if (size) {
			out[0] = '\0';
		}
		return;
	}
	out[at++] = '"';
	for (const char *p = value; *p && at + 2 < size; p++) {
		if (*p == '"' || *p == '\\') {
			out[at++] = '\\';
		}
		out[at++] = *p;
	}
	out[at++] = '"';
	out[at] = '\0';
}

// A 64-character hex string is not a passphrase but the key itself, and goes to
// wpa_supplicant unquoted. Anyone who pastes one in has already been through
// the trouble of finding it; refusing it as "too long" would be absurd.
static bool psk_is_raw_key(const char *psk) {
	if (strlen(psk) != 64) {
		return false;
	}
	for (const char *p = psk; *p; p++) {
		if (!isxdigit((unsigned char)*p)) {
			return false;
		}
	}
	return true;
}

// Whether wpa_supplicant will even store this passphrase. WPA-PSK is 8 to 63
// characters, and anything outside that is rejected at set_network time -- so
// catching it here turns thirty seconds of waiting for an association that was
// never configured into an immediate, and correct, "wrong password".
static bool psk_is_usable(const char *psk) {
	size_t len = strlen(psk);
	return (len >= 8 && len <= 63) || psk_is_raw_key(psk);
}

// Whether wpa_supplicant has parked this network after refusing to authenticate
// with it, which is what a wrong passphrase looks like from the outside: the
// supplicant tries, the access point rejects the handshake, and after a few
// goes the network is flagged TEMP-DISABLED in list_networks. There is no field
// in `status` that says it.
static bool network_rejected(const char *ssid) {
	static char buf[4096];
	if (wpa1("list_networks", buf, sizeof(buf)) != 0) {
		return false;
	}

	char *save_line = NULL;
	for (char *line = strtok_r(buf, "\n", &save_line); line; line = strtok_r(NULL, "\n", &save_line)) {
		// The flags are the fourth field; parse_network_line only keeps the
		// name and the id, so the line is read again here.
		char *copy = line;
		char *fields[4];
		int n = 0;
		fields[n++] = copy;
		for (char *p = copy; *p && n < 4; p++) {
			if (*p == '\t') {
				*p = '\0';
				fields[n++] = p + 1;
			}
		}
		if (n < 4) {
			continue;
		}
		ssid_unescape(fields[1]);
		if (strcmp(fields[1], ssid) == 0) {
			// TEMP-DISABLED only. A plain [DISABLED] is every network that is
			// not the selected one, which says nothing about passphrases.
			return strstr(fields[3], "TEMP-DISABLED") != NULL;
		}
	}
	return false;
}

// Writes the passphrase into network `id_text`, quoted or raw as it deserves.
// False when wpa_supplicant would not take it.
static bool set_psk(const char *id_text, const char *psk) {
	char out[256];
	char quoted_value[2 * 128 + 3];

	if (psk_is_raw_key(psk)) {
		wpa4("set_network", id_text, "psk", psk, out, sizeof(out));
	} else {
		quoted(psk, quoted_value, sizeof(quoted_value));
		wpa4("set_network", id_text, "psk", quoted_value, out, sizeof(out));
	}
	if (!wpa_ok(out)) {
		printf("wifi: wpa_supplicant refused the passphrase (%s)\n", out[0] ? out : "no reply");
		return false;
	}
	return true;
}

// What the direct route managed. The three cases are not interchangeable: a
// wpa_cli that is not there at all is a reason to try the daemon instead, while
// a wpa_supplicant that answered FAIL has already given its verdict and asking
// a second time through a worse road only repeats it.
typedef enum {
	DIRECT_OK,
	DIRECT_REFUSED,		// wpa_supplicant said no
	DIRECT_UNAVAILABLE, // wpa_cli did not run
} direct_result_t;

static direct_result_t connect_direct(const char *ssid, const char *psk) {
	char out[256];
	char id_text[16];
	char quoted_value[2 * WIFI_SSID_MAX + 3];

	int id = saved_network_id(ssid);
	if (id < 0) {
		int rc = wpa1("add_network", out, sizeof(out));
		if (rc != 0 && !out[0]) {
			printf("wifi: wpa_cli did not run\n");
			return DIRECT_UNAVAILABLE;
		}
		if (!wpa_ok(out)) {
			printf("wifi: add_network failed (%s)\n", out);
			return DIRECT_REFUSED;
		}
		id = atoi(out);
		if (id < 0) {
			return DIRECT_REFUSED;
		}
		snprintf(id_text, sizeof(id_text), "%d", id);

		quoted(ssid, quoted_value, sizeof(quoted_value));
		wpa4("set_network", id_text, "ssid", quoted_value, out, sizeof(out));
		if (!wpa_ok(out)) {
			printf("wifi: the network name was refused (%s)\n", out[0] ? out : "no reply");
			return DIRECT_REFUSED;
		}

		if (psk && psk[0]) {
			if (!set_psk(id_text, psk)) {
				set_fail(WIFI_FAIL_WRONG_KEY);
				return DIRECT_REFUSED;
			}
		} else {
			wpa4("set_network", id_text, "key_mgmt", "NONE", out, sizeof(out));
		}
	} else {
		snprintf(id_text, sizeof(id_text), "%d", id);
		// A saved network whose password is being re-entered. key_mgmt goes
		// with it: a network first joined as an open one carries key_mgmt=NONE,
		// and a passphrase set over the top of that is never used.
		if (psk && psk[0]) {
			if (!set_psk(id_text, psk)) {
				set_fail(WIFI_FAIL_WRONG_KEY);
				return DIRECT_REFUSED;
			}
			wpa4("set_network", id_text, "key_mgmt", "WPA-PSK", out, sizeof(out));
		}
	}

	// Probe for this name directly instead of waiting for it in beacons and
	// broadcast probe answers: that is what finds a hidden network, and the
	// stock sys_server sets it on every join by name (0x405a74). Saved networks
	// get it too, so the ones joined before this was set catch up.
	wpa4("set_network", id_text, "scan_ssid", "1", out, sizeof(out));
	if (!wpa_ok(out)) {
		printf("wifi: scan_ssid not taken for network %s (%s)\n", id_text, out[0] ? out : "no reply");
	}
	set_priorities(id);

	wpa2("enable_network", id_text, out, sizeof(out));
	wpa2("select_network", id_text, out, sizeof(out));
	if (!wpa_ok(out)) {
		printf("wifi: select_network %s failed (%s)\n", id_text, out[0] ? out : "no reply");
		return DIRECT_REFUSED;
	}
	wpa1("save_config", out, sizeof(out));
	printf("wifi: joining \"%s\" through wpa_cli, network %s\n", ssid, id_text);
	return DIRECT_OK;
}

// Waits for wpa_supplicant to settle. Returns true once the link is up,
// whether or not DHCP has answered yet.
//
// Every state other than COMPLETED and the two idle ones means work is going
// on -- SCANNING, AUTHENTICATING, ASSOCIATING, ASSOCIATED, 4WAY_HANDSHAKE,
// GROUP_HANDSHAKE -- and the wait simply lets it. Only a supplicant that has
// been sitting in DISCONNECTED or INACTIVE for ASSOC_PARKED_MS without moving
// counts as one that has stopped trying.
static bool wait_for_association(bool *saw_handshake) {
	static char buf[2048];
	char state[32];
	char last[32] = "";
	int parked_ms = 0;

	if (saw_handshake) {
		*saw_handshake = false;
	}

	for (int elapsed = 0; elapsed < ASSOC_TIMEOUT_MS; elapsed += ASSOC_POLL_MS) {
		sleep_ms(ASSOC_POLL_MS);
		read_status(buf, sizeof(buf));
		if (!status_field(buf, "wpa_state", state, sizeof(state))) {
			continue;
		}
		if (strcmp(state, last) != 0) {
			printf("wifi: wpa_state=%s after %d ms\n", state, elapsed + ASSOC_POLL_MS);
			snprintf(last, sizeof(last), "%s", state);
		}

		if (strcmp(state, "COMPLETED") == 0) {
			return true;
		}
		// Reaching the four-way handshake means the access point was there and
		// answered to this name: whatever goes wrong after it is the key.
		if (saw_handshake && strcmp(state, "4WAY_HANDSHAKE") == 0) {
			*saw_handshake = true;
		}
		if (strcmp(state, "DISCONNECTED") == 0 || strcmp(state, "INACTIVE") == 0) {
			parked_ms += ASSOC_POLL_MS;
			if (parked_ms >= ASSOC_PARKED_MS) {
				printf("wifi: parked in %s, giving up\n", state);
				return false;
			}
		} else {
			parked_ms = 0;
		}
	}
	printf("wifi: no association within %d ms\n", ASSOC_TIMEOUT_MS);
	return false;
}

static void request_dhcp(void) {
	char *argv[10];
	int n = 0;
	argv[n++] = (char *)UDHCPC;
	argv[n++] = (char *)"-i";
	argv[n++] = (char *)WIFI_IFACE;
	argv[n++] = (char *)"-q"; // quit once the lease is in
	argv[n++] = (char *)"-n"; // give up rather than keep a daemon around
	argv[n++] = (char *)"-t";
	argv[n++] = (char *)"6";
	argv[n++] = (char *)"-x";
	argv[n++] = (char *)"hostname:" WIFI_HOSTNAME;
	argv[n] = NULL;
	run_argv(argv, NULL, 0);
}

// Whether a string can be handed to sys_server without a shell eating part of
// it, or worse.
//
// The daemon builds its work as text and runs it through system(), so anything
// the user typed lands in a shell: a network name containing a '#' after a
// space is truncated at the comment, and the daemon's own parser splits on ':'.
//
// The characters below are the ones that would be swallowed or, in the case of
// the first few, obeyed: a passphrase containing a semicolon and a command
// would be a command, run as root. The direct route does not go near a shell
// (see run_argv), which is why it is tried first and this test only guards the
// fallback.
static bool text_is_shell_safe(const char *text) {
	return text[0] && strpbrk(text, ":;&|<>()$`\\\"'*?[]#~!\n\t") == NULL;
}

static void do_connect(const char *ssid, const char *psk) {
	bool sent = false;
	bool has_psk = psk && psk[0];

	set_fail(WIFI_FAIL_NONE);
	pthread_mutex_lock(&lock);
	link_dropped = false;
	pthread_mutex_unlock(&lock);

	// A passphrase wpa_supplicant will not store is a wrong passphrase, and
	// saying so now beats waiting half a minute for an association that was
	// never set up.
	if (has_psk && !psk_is_usable(psk)) {
		printf("wifi: the passphrase is %d characters; WPA takes 8 to 63\n", (int)strlen(psk));
		set_fail(WIFI_FAIL_WRONG_KEY);
		set_op(WIFI_OP_FAILED, ssid);
		return;
	}

	// The length is worth having in the log next to the name: a name that came
	// out short is the whole story, and quotes alone do not show it.
	printf("wifi: joining \"%s\" (%d characters, %s)\n", ssid, (int)strlen(ssid),
		   has_psk ? "with a passphrase" : "open");

	// wpa_cli first, sys_server second, which is the other way round from every
	// other request in this file -- and deliberately.
	//
	// A connect is the only command that carries text the user typed. Every
	// other one (WIFI:ON, WIFI:SCAN, WIFI:CONNECT by number) carries an
	// interface name and an integer, so the daemon's habit of pasting its
	// arguments into a shell cannot hurt them. Here it can, and the daemon
	// answers OK to a command it has already mangled. The direct route speaks
	// to wpa_cli through execv, with no shell anywhere and the quoting done
	// here, so a name with a #, a space or an accent survives it.
	direct_result_t direct = connect_direct(ssid, psk);
	sent = direct == DIRECT_OK;

	if (direct == DIRECT_UNAVAILABLE && have_sysserver()) {
		// No wpa_cli on this firmware. The daemon is what is left, and it gets
		// only the names it can carry intact.
		if (!text_is_shell_safe(ssid) || (has_psk && !text_is_shell_safe(psk))) {
			printf("wifi: no wpa_cli, and sys_server cannot carry this name or passphrase intact\n");
		} else {
			char command[64 + WIFI_SSID_MAX + 128];
			char reply[128];
			int id = saved_network_id(ssid);

			if (id >= 0 && !has_psk) {
				snprintf(command, sizeof(command), "WIFI:CONNECT:%s %d", WIFI_IFACE, id);
			} else {
				snprintf(command, sizeof(command), "WIFI:SSID_CONNECT:%s:%s:%s", WIFI_IFACE, ssid,
						 has_psk ? psk : "");
			}
			sent = sysserver_request(command, reply, sizeof(reply)) == 0;
			printf("wifi: joining \"%s\" through sys_server -> %s\n", ssid, sent ? "accepted" : "refused");

			// Accepted is not the same as understood. If wpa_supplicant now has
			// no network under the name that was asked for, the daemon wrote
			// down something else, and waiting for that to associate is waiting
			// for nothing.
			if (sent && saved_network_id(ssid) < 0) {
				printf("wifi: sys_server stored the network under another name; not waiting for it\n");
				sent = false;
			}
		}
	}

	if (!sent) {
		// Nothing reached wpa_supplicant, so there is nothing to wait for.
		// Which of the steps it was, the log above says.
		if (wifi_last_failure() == WIFI_FAIL_NONE) {
			set_fail(WIFI_FAIL_REFUSED);
		}
		set_op(WIFI_OP_FAILED, ssid);
		return;
	}

	bool handshake = false;
	bool associated = wait_for_association(&handshake);
	if (associated) {
		request_dhcp();
	} else {
		// The access point turned the handshake down. Two things say so, and
		// either is enough: wpa_supplicant parks a network it cannot
		// authenticate with (TEMP-DISABLED), and the join got as far as the
		// four-way handshake, which it only reaches once the name has been
		// matched and the radio answered. The second is what catches the case
		// where the supplicant is still retrying when the wait runs out and has
		// not given up on the network yet.
		set_fail(has_psk && (handshake || network_rejected(ssid)) ? WIFI_FAIL_WRONG_KEY : WIFI_FAIL_OTHER);
		printf("wifi: \"%s\" did not join (%s)\n", ssid,
			   wifi_last_failure() == WIFI_FAIL_WRONG_KEY ? "wrong passphrase" : "no answer");
	}
	// After the wait, not before: with the others enabled during the join,
	// wpa_supplicant could settle on one of them while the page waits for this
	// one. After a failed join it is what lets the radio go back to a network
	// that works. sys_server's own join already ends with this.
	if (direct == DIRECT_OK) {
		enable_all_networks(true);
	}
	set_op(associated ? WIFI_OP_OK : WIFI_OP_FAILED, ssid);
	refresh_networks(false);
}

// Forgets the live link at once, instead of waiting for the next poll to notice.
//
// Leaving a network or forgetting one is a request whose whole point is that
// the connection goes away, and the status poll takes two seconds at best to
// notice -- longer while the saved-network file sys_server wrote still carries
// the CURRENT flag. The cache is local, so it is cleared here and the next poll
// merely confirms it.
static void forget_link_state(void) {
	pthread_mutex_lock(&lock);
	link_dropped = true;
	if (g_status.state == WIFI_STATE_CONNECTED || g_status.state == WIFI_STATE_CONNECTING) {
		memset(&g_status, 0, sizeof(g_status));
		g_status.state = WIFI_STATE_ON;
	}
	for (int i = 0; i < g_network_count; i++) {
		g_networks[i].current = false;
	}
	g_serial++;
	pthread_mutex_unlock(&lock);
}

// Whether `ssid` is the network the cache says is joined.
static bool is_current_network(const char *ssid) {
	bool current = false;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < g_network_count; i++) {
		if (g_networks[i].current && strcmp(g_networks[i].ssid, ssid) == 0) {
			current = true;
			break;
		}
	}
	pthread_mutex_unlock(&lock);
	return current;
}

static void do_disconnect(void) {
	if (have_sysserver()) {
		char command[64];
		char reply[128];
		int id = -1;

		pthread_mutex_lock(&lock);
		for (int i = 0; i < g_network_count; i++) {
			if (g_networks[i].current) {
				id = g_networks[i].id;
				break;
			}
		}
		pthread_mutex_unlock(&lock);

		if (id >= 0) {
			snprintf(command, sizeof(command), "WIFI:DISCONNECT:%s %d", WIFI_IFACE, id);
			if (sysserver_request(command, reply, sizeof(reply)) == 0) {
				forget_link_state();
				set_op(WIFI_OP_OK, NULL);
				refresh_networks(false);
				return;
			}
		}
	}

	wpa1("disconnect", NULL, 0);
	forget_link_state();
	set_op(WIFI_OP_OK, NULL);
	refresh_networks(false);
}

static void do_forget(const char *ssid) {
	int id = saved_network_id(ssid);
	if (id < 0) {
		set_op(WIFI_OP_FAILED, ssid);
		return;
	}

	bool was_current = is_current_network(ssid);

	bool done = false;
	if (have_sysserver()) {
		char command[64];
		char reply[128];
		snprintf(command, sizeof(command), "WIFI:REMOVE:%s %d", WIFI_IFACE, id);
		done = sysserver_request(command, reply, sizeof(reply)) == 0;
	}

	if (!done) {
		char id_text[16];
		char out[128];
		snprintf(id_text, sizeof(id_text), "%d", id);
		done = wpa2("remove_network", id_text, out, sizeof(out)) == 0;
		wpa1("save_config", out, sizeof(out));
	}

	// Removing a network does not always bring the link down with it -- the
	// association can outlive the entry that describes it -- and a player still
	// joined to a network it no longer knows is neither one thing nor the other.
	if (done && was_current) {
		wpa1("disconnect", NULL, 0);
		forget_link_state();
	}

	set_op(done ? WIFI_OP_OK : WIFI_OP_FAILED, ssid);
	refresh_networks(false);
}

// The periodic read: link state, signal strength, and a DHCP request if
// association happened without an address.
static void poll_status(void) {
	static char buf[2048];
	static uint32_t last_dhcp;

	wifi_status_t status;
	memset(&status, 0, sizeof(status));

	read_status(buf, sizeof(buf));

	char state[32];
	if (!status_field(buf, "wpa_state", state, sizeof(state))) {
		status.state = WIFI_STATE_ON; // radio up, but the supplicant is not answering
	} else if (strcmp(state, "COMPLETED") == 0) {
		status.state = WIFI_STATE_CONNECTED;
	} else if (strcmp(state, "INACTIVE") == 0 || strcmp(state, "DISCONNECTED") == 0 ||
			   strcmp(state, "INTERFACE_DISABLED") == 0 || strcmp(state, "SCANNING") == 0) {
		status.state = WIFI_STATE_ON;
	} else {
		status.state = WIFI_STATE_CONNECTING;
	}

	status_field(buf, "ssid", status.ssid, sizeof(status.ssid));
	ssid_unescape(status.ssid); // wpa_supplicant escapes this one too
	status_field(buf, "ip_address", status.ip, sizeof(status.ip));
	status_field(buf, "address", status.mac, sizeof(status.mac));

	if (status.state == WIFI_STATE_CONNECTED) {
		char signal_buf[512];
		char rssi[16];
		if (wpa1("signal_poll", signal_buf, sizeof(signal_buf)) == 0 &&
			status_field(signal_buf, "RSSI", rssi, sizeof(rssi))) {
			status.signal = atoi(rssi);
		}
		status.bars = status.signal ? bars_for(status.signal) : 3;

		// Associated but with no address: the lease never came, or was lost when
		// the network changed. Ask again, but not on every poll.
		if (!status.ip[0] && (uint32_t)(now_ms() - last_dhcp) > DHCP_RETRY_MS) {
			last_dhcp = now_ms();
			request_dhcp();
		}
	}

	pthread_mutex_lock(&lock);
	if (status.state == WIFI_STATE_CONNECTED) {
		link_dropped = false; // the radio says otherwise, and it is the authority
	}
	// The switch has the last word: the poll must never report a link on a radio
	// that has just been turned off.
	bool was_online = g_status.state == WIFI_STATE_CONNECTED && g_status.ip[0] != '\0';
	if (!g_enabled) {
		memset(&g_status, 0, sizeof(g_status));
		g_status.state = WIFI_STATE_OFF;
	} else {
		g_status = status;
	}
	bool now_online = g_status.state == WIFI_STATE_CONNECTED && g_status.ip[0] != '\0';
	pthread_mutex_unlock(&lock);

	// A fresh association is the moment to ask the network for the time. The
	// stock player does the same, running ntpdate against 0.pool.ntp.org and
	// time.windows.com as soon as wpa_supplicant reports COMPLETED, and it is
	// the only clock correction the firmware makes anywhere, on a device whose
	// RTC cannot be trusted.
	if (now_online && !was_online) {
		clock_network_sync();
	}
}

// ---------------------------------------------------------------------------
// worker loop
// ---------------------------------------------------------------------------

static bool take_job(job_t *out) {
	if (queue_count == 0) {
		return false;
	}
	*out = queue[queue_head];
	queue_head = (queue_head + 1) % JOB_QUEUE_LEN;
	queue_count--;
	return true;
}

static void post_job(const job_t *job) {
	pthread_mutex_lock(&lock);
	if (queue_count < JOB_QUEUE_LEN) {
		queue[(queue_head + queue_count) % JOB_QUEUE_LEN] = *job;
		queue_count++;
		pthread_cond_signal(&wake);
	}
	pthread_mutex_unlock(&lock);
}

static void *wifi_worker(void *arg) {
	(void)arg;
	thread_be_background("wifi");

	uint32_t last_poll = 0;

	for (;;) {
		job_t job;
		bool have_job;

		pthread_mutex_lock(&lock);
		have_job = take_job(&job);
		if (!have_job) {
			// Wait for work, but wake often enough to keep the status fresh while
			// the radio is on. deadline_in_ms() carries the millisecond into
			// tv_sec: a two-second poll is 2,000,000,000 ns, and adding that
			// straight to tv_nsec overflows a 32-bit long.
			struct timespec deadline;
			deadline_in_ms(&deadline, !g_enabled ? IDLE_WAIT_MS
												 : (power_screen_is_on() ? STATUS_POLL_MS : STATUS_POLL_STANDBY_MS));
			pthread_cond_timedwait(&wake, &lock, &deadline);
			have_job = take_job(&job);
		}
		bool enabled = g_enabled;
		pthread_mutex_unlock(&lock);

		if (have_job) {
			switch (job.type) {
			case JOB_POWER:
				do_power(job.arg_int != 0);
				pthread_mutex_lock(&lock);
				g_power_busy = false;
				if (!job.arg_int) {
					memset(&g_status, 0, sizeof(g_status));
					g_status.state = WIFI_STATE_OFF;
					g_network_count = 0;
					g_serial++;
				}
				pthread_mutex_unlock(&lock);
				// No sweep here. The two pages with a list ask for their own
				// as soon as the radio is ready (wifisettings.c and
				// setupwifi.c, both in poll_cb), and with neither open a sweep
				// is airtime taken from the supplicant while it joins a saved
				// network: on every screen-on after the idle park, at the edge
				// of range, where the join is hardest. Stock does not scan on
				// resume either (WIFI:ON at 0x482b80, no WIFI:SCAN after it).
				if (job.arg_int) {
					poll_status();
				}
				break;

			case JOB_SCAN:
				do_scan();
				pthread_mutex_lock(&lock);
				g_scanning = false;
				pthread_mutex_unlock(&lock);
				break;

			case JOB_CONNECT:
				do_connect(job.arg_a, job.arg_b);
				poll_status();
				break;

			case JOB_DISCONNECT:
				do_disconnect();
				poll_status();
				break;

			case JOB_FORGET:
				do_forget(job.arg_a);
				poll_status();
				break;

			case JOB_NONE:
			default:
				break;
			}
			continue;
		}

		uint32_t poll_period =
			(power_screen_is_on() && !g_poll_slow) ? STATUS_POLL_MS : STATUS_POLL_STANDBY_MS;
		if (enabled && (uint32_t)(now_ms() - last_poll) >= poll_period) {
			last_poll = now_ms();
			poll_status();
		}
	}

	return NULL;
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

void wifi_init(void) {
	if (worker_running) {
		return;
	}

	g_enabled = config_get_int("wireless", "wifi", 0) != 0;
	g_status.state = WIFI_STATE_OFF;

	if (pthread_create(&worker, NULL, wifi_worker, NULL) != 0) {
		fprintf(stderr, "wifi: could not start the worker thread\n");
		g_enabled = false;
		return;
	}
	pthread_detach(worker);
	worker_running = true;

	if (g_enabled && wifi_available()) {
		job_t job = {.type = JOB_POWER, .arg_int = 1};
		g_power_busy = true;
		post_job(&job);
	} else if (g_enabled) {
		g_enabled = false; // no interface, so the switch must not read as on
	}
}

bool wifi_get_enabled(void) {
	pthread_mutex_lock(&lock);
	bool on = g_enabled;
	pthread_mutex_unlock(&lock);
	return on;
}

static void set_enabled(bool on, bool remember) {
	if (!worker_running) {
		return;
	}

	pthread_mutex_lock(&lock);
	if (g_enabled == on) {
		pthread_mutex_unlock(&lock);
		return;
	}
	g_enabled = on;
	g_power_busy = true;
	if (!on) {
		memset(&g_status, 0, sizeof(g_status));
		g_status.state = WIFI_STATE_OFF;
		g_network_count = 0;
		g_serial++;
	}
	pthread_mutex_unlock(&lock);

	if (remember) {
		config_set_int("wireless", "wifi", on ? 1 : 0);
		config_save();
	}

	job_t job = {.type = JOB_POWER, .arg_int = on ? 1 : 0};
	post_job(&job);
}

void wifi_set_enabled(bool on) { set_enabled(on, true); }

// The idle park. Same radio, but the setting is left alone: a park written to
// the config comes back after a reboot as "the user switched Wi-Fi off", with
// the toggle reading off and no way to tell it was the power saver.
void wifi_set_enabled_transient(bool on) { set_enabled(on, false); }

bool wifi_busy(void) {
	pthread_mutex_lock(&lock);
	bool busy = g_power_busy;
	pthread_mutex_unlock(&lock);
	return busy;
}

// The address the interface actually has, straight from the kernel.
//
// Not wifi_get_status(): that answers from the last poll of `wpa_cli status`,
// which is a fork away and can come back without a wpa_state at all (a busy
// supplicant, a read that lost the race) or with wpa_state=SCANNING while the
// link is perfectly up and carrying traffic. Anything that decides whether to
// keep a server running on one sample of that tears it down at random.
//
// An address on wlan0 is what a server needs and nothing else, and asking the
// kernel for it is an ioctl: no fork, no supplicant, no flap.
bool wifi_interface_address(char *out, size_t size) {
	if (out && size) {
		out[0] = '\0';
	}

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return false;
	}

	struct ifreq req;
	memset(&req, 0, sizeof(req));
	snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", WIFI_IFACE);

	bool have = false;
	if (ioctl(fd, SIOCGIFADDR, &req) == 0) {
		struct sockaddr_in *in = (struct sockaddr_in *)&req.ifr_addr;
		if (in->sin_addr.s_addr != 0) {
			have = true;
			if (out && size) {
				inet_ntop(AF_INET, &in->sin_addr, out, (socklen_t)size);
			}
		}
	}
	close(fd);
	return have;
}

void wifi_get_status(wifi_status_t *out) {
	if (!out) {
		return;
	}
	pthread_mutex_lock(&lock);
	*out = g_status;
	pthread_mutex_unlock(&lock);
}

void wifi_scan_start(void) {
	pthread_mutex_lock(&lock);
	bool skip = g_scanning || !g_enabled;
	if (!skip) {
		g_scanning = true;
	}
	pthread_mutex_unlock(&lock);

	if (skip) {
		return;
	}

	job_t job = {.type = JOB_SCAN};
	post_job(&job);
}

bool wifi_scan_running(void) {
	pthread_mutex_lock(&lock);
	bool running = g_scanning;
	pthread_mutex_unlock(&lock);
	return running;
}

uint32_t wifi_networks_serial(void) {
	pthread_mutex_lock(&lock);
	uint32_t serial = g_serial;
	pthread_mutex_unlock(&lock);
	return serial;
}

int wifi_get_networks(wifi_network_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	pthread_mutex_lock(&lock);
	int count = g_network_count < max ? g_network_count : max;
	memcpy(out, g_networks, sizeof(wifi_network_t) * (size_t)count);
	pthread_mutex_unlock(&lock);
	return count;
}

void wifi_connect(const char *ssid, const char *psk) {
	if (!ssid || !ssid[0]) {
		return;
	}
	set_opkind(WIFI_OPKIND_CONNECT);
	set_op(WIFI_OP_BUSY, ssid);

	job_t job = {.type = JOB_CONNECT};
	snprintf(job.arg_a, sizeof(job.arg_a), "%s", ssid);
	snprintf(job.arg_b, sizeof(job.arg_b), "%s", psk ? psk : "");
	post_job(&job);
}

void wifi_disconnect(void) {
	// Reported like the other two: leaving a network is something the user
	// asked for and wants told about.
	set_opkind(WIFI_OPKIND_DISCONNECT);
	set_op(WIFI_OP_BUSY, NULL);

	job_t job = {.type = JOB_DISCONNECT};
	post_job(&job);
}

void wifi_forget(const char *ssid) {
	if (!ssid || !ssid[0]) {
		return;
	}
	set_opkind(WIFI_OPKIND_FORGET);
	set_op(WIFI_OP_BUSY, ssid);

	job_t job = {.type = JOB_FORGET};
	snprintf(job.arg_a, sizeof(job.arg_a), "%s", ssid);
	post_job(&job);
}

wifi_op_t wifi_take_op_result(char *ssid_out, int ssid_size) {
	pthread_mutex_lock(&lock);
	wifi_op_t op = g_op;
	if (ssid_out && ssid_size > 0) {
		snprintf(ssid_out, (size_t)ssid_size, "%s", g_op_ssid);
	}
	if (op == WIFI_OP_OK || op == WIFI_OP_FAILED) {
		g_op = WIFI_OP_IDLE; // a terminal result is handed out only once
	}
	pthread_mutex_unlock(&lock);
	return op;
}

wifi_fail_t wifi_last_failure(void) {
	pthread_mutex_lock(&lock);
	wifi_fail_t reason = g_fail;
	pthread_mutex_unlock(&lock);
	return reason;
}

wifi_opkind_t wifi_last_op_kind(void) {
	pthread_mutex_lock(&lock);
	wifi_opkind_t kind = g_opkind;
	pthread_mutex_unlock(&lock);
	return kind;
}
