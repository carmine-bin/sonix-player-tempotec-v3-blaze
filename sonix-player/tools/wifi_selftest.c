// Host self-check for the Wi-Fi worker (src/system/net/wifi.c).
//
// wpa_cli and udhcpc are shell scripts written into the work directory and put
// first on PATH, so the worker runs its real fork+exec path against them. The
// fake supplicant answers only while <dir>/up exists, has an empty table until
// a scan is asked for, and gives the sweep's results from the second read on.
// sys_server is modelled the way the stock daemon behaves: it answers "OK" when
// it queues a request and runs it later on its own thread, which idles between
// polls of an empty queue. The worker's sleeps run ten times faster.
//
//   gcc -O2 -Wall -DHOST_BUILD -I. -Ilvgl tools/wifi_selftest.c -lpthread -o /tmp/wifi_selftest
//   /tmp/wifi_selftest /tmp/wifi_selftest_dir

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fake_nanosleep(const struct timespec *req, struct timespec *rem) {
	long long ns = ((long long)req->tv_sec * 1000000000LL + req->tv_nsec) / 10;
	struct timespec scaled = {.tv_sec = (time_t)(ns / 1000000000LL), .tv_nsec = (long)(ns % 1000000000LL)};
	return nanosleep(&scaled, rem);
}
static int fake_access(const char *path, int mode);
#define nanosleep fake_nanosleep
#define access fake_access
#include "src/system/net/wifi.c"
#undef access
#undef nanosleep

// The interface exists, whatever machine this runs on.
static int fake_access(const char *path, int mode) {
	return strcmp(path, "/sys/class/net/" WIFI_IFACE) == 0 ? 0 : access(path, mode);
}

static char dir[512];

static void path_of(char *out, size_t size, const char *name) { snprintf(out, size, "%s/%s", dir, name); }

static bool exists(const char *name) {
	char p[600];
	path_of(p, sizeof(p), name);
	return access(p, F_OK) == 0;
}

static void put(const char *name, const char *text) {
	char p[600];
	path_of(p, sizeof(p), name);
	FILE *f = fopen(p, "w");
	assert(f);
	fputs(text, f);
	fclose(f);
}

static void drop(const char *name) {
	char p[600];
	path_of(p, sizeof(p), name);
	unlink(p);
}

// The fake wpa_cli's log, one command per line.
static char *read_log(void) {
	static char text[65536];
	char p[600];
	path_of(p, sizeof(p), "log");
	FILE *f = fopen(p, "r");
	text[0] = '\0';
	if (f) {
		size_t got = fread(text, 1, sizeof(text) - 1, f);
		text[got] = '\0';
		fclose(f);
	}
	return text;
}

static void mark_log(const char *mark) {
	char p[600];
	path_of(p, sizeof(p), "log");
	FILE *f = fopen(p, "a");
	assert(f);
	fprintf(f, "== %s\n", mark);
	fclose(f);
}

// The part of the log after the last "== mark" line.
static const char *log_since(const char *mark) {
	char needle[128];
	snprintf(needle, sizeof(needle), "== %s\n", mark);
	const char *text = read_log();
	const char *at = NULL;
	for (const char *p = strstr(text, needle); p; p = strstr(p + 1, needle)) {
		at = p;
	}
	assert(at);
	return at + strlen(needle);
}

static const char FAKE_WPA_CLI[] =
	"#!/bin/sh\n"
	"D=\"$WIFI_FAKE\"\n"
	"shift 2\n"
	"if [ ! -e \"$D/up\" ]; then echo \"$* (no supplicant)\" >> \"$D/log\"; exit 255; fi\n"
	"echo \"$*\" >> \"$D/log\"\n"
	"case \"$1\" in\n"
	"status)\n"
	"  if [ -e \"$D/joined\" ]; then\n"
	"    printf 'ssid=%s\\nwpa_state=COMPLETED\\nip_address=192.168.1.9\\naddress=00:11:22:33:44:55\\n' \"$(cat \"$D/joined\")\"\n"
	"  else printf 'wpa_state=DISCONNECTED\\naddress=00:11:22:33:44:55\\n'; fi ;;\n"
	"signal_poll) printf 'RSSI=%s\\nLINKSPEED=65\\n' \"$(cat \"$D/rssi\" 2>/dev/null || echo -50)\" ;;\n"
	"scan) touch \"$D/scanned\"; echo 1 > \"$D/pending\"; echo OK ;;\n"
	"scan_results)\n"
	"  echo 'bssid / frequency / signal level / flags / ssid'\n"
	"  if [ -e \"$D/scanned\" ]; then\n"
	"    n=$(cat \"$D/pending\")\n"
	"    if [ \"$n\" -gt 0 ]; then echo $((n - 1)) > \"$D/pending\"; else cat \"$D/air\"; fi\n"
	"  fi ;;\n"
	"list_networks) echo 'network id / ssid / bssid / flags'; cat \"$D/nets\" 2>/dev/null ;;\n"
	"add_network) n=$(wc -l < \"$D/nets\"); printf '%s\\t\\tany\\t[DISABLED]\\n' $n >> \"$D/nets\"; echo $n ;;\n"
	"set_network)\n"
	"  if [ \"$3\" = ssid ]; then\n"
	"    name=$(echo \"$4\" | sed 's/^\"//; s/\"$//')\n"
	"    awk -F '\\t' -v OFS='\\t' -v id=\"$2\" -v name=\"$name\" '$1 == id { $2 = name } { print }' \"$D/nets\" > \"$D/nets.new\"\n"
	"    mv \"$D/nets.new\" \"$D/nets\"\n"
	"  fi\n"
	"  echo OK ;;\n"
	"select_network)\n"
	"  name=$(awk -F '\\t' -v id=\"$2\" '$1 == id { printf \"%s\", $2 }' \"$D/nets\")\n"
	"  if awk -F '\\t' -v n=\"$name\" '$5 == n { f = 1 } END { exit !f }' \"$D/air\"; then\n"
	"    printf '%s' \"$name\" > \"$D/joined\"\n"
	"  else rm -f \"$D/joined\"; fi\n"
	"  echo OK ;;\n"
	"*) echo OK ;;\n"
	"esac\n";

static const char FAKE_UDHCPC[] = "#!/bin/sh\nexit 0\n";

static void install(const char *name, const char *text) {
	char p[600];
	put(name, text);
	path_of(p, sizeof(p), name);
	chmod(p, 0755);
}

// ---------------------------------------------------------------------------
// sys_server: answers at once, runs the work later, in order, on its own thread
// ---------------------------------------------------------------------------

static pthread_mutex_t ss_lock = PTHREAD_MUTEX_INITIALIZER;
static char ss_queue[16][96];
static int ss_head, ss_count;
static bool ss_running;
static int ss_scans; // WIFI:SCAN requests seen

static void ss_execute(const char *command) {
	if (strncmp(command, "WIFI:ON:", 8) == 0) {
		usleep(150000); // wifi_on.sh: killall, ifconfig, wpa_supplicant -B ...
		drop("scanned");
		put("up", "");
	} else if (strncmp(command, "WIFI:OFF:", 9) == 0) {
		drop("up");
		drop("joined");
	}
}

static void *ss_thread(void *arg) {
	(void)arg;
	for (;;) {
		char command[96];
		pthread_mutex_lock(&ss_lock);
		bool have = ss_count > 0;
		if (have) {
			snprintf(command, sizeof(command), "%s", ss_queue[ss_head]);
			ss_head = (ss_head + 1) % 16;
			ss_count--;
			ss_running = true;
		}
		pthread_mutex_unlock(&ss_lock);
		if (!have) {
			usleep(300000); // the stock daemon sleeps a whole second here
			continue;
		}
		ss_execute(command);
		pthread_mutex_lock(&ss_lock);
		ss_running = false;
		pthread_mutex_unlock(&ss_lock);
	}
	return NULL;
}

bool sysserver_available(void) { return true; }

int sysserver_request(const char *command, char *reply, size_t reply_size) {
	pthread_mutex_lock(&ss_lock);
	assert(ss_count < 16);
	snprintf(ss_queue[(ss_head + ss_count) % 16], sizeof(ss_queue[0]), "%s", command);
	ss_count++;
	if (strncmp(command, "WIFI:SCAN:", 10) == 0) {
		ss_scans++;
	}
	pthread_mutex_unlock(&ss_lock);
	if (reply && reply_size) {
		snprintf(reply, reply_size, "OK");
	}
	return 0;
}

// ---------------------------------------------------------------------------
// the rest of the player
// ---------------------------------------------------------------------------

long config_get_int(const char *section, const char *key, long fallback) {
	(void)section;
	(void)fallback;
	return strcmp(key, "wifi") == 0 ? 1 : 0;
}
void config_set_int(const char *section, const char *key, long value) { (void)section, (void)key, (void)value; }
bool config_save(void) { return true; }
bool power_screen_is_on(void) { return true; }
void clock_network_sync(void) {}
void logging_child_stdio(void) {}
void thread_be_background(const char *name) { (void)name; }
void deadline_in_ms(struct timespec *out, unsigned int ms) {
	clock_gettime(CLOCK_REALTIME, out);
	long long ns = (long long)out->tv_nsec + (long long)ms * 100000LL; // a tenth, like the sleeps
	out->tv_sec += (time_t)(ns / 1000000000LL);
	out->tv_nsec = (long)(ns % 1000000000LL);
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static bool wait_until(bool (*done)(void), int timeout_ms) {
	for (int waited = 0; waited < timeout_ms; waited += 10) {
		if (done()) {
			return true;
		}
		usleep(10000);
	}
	return done();
}

static bool not_scanning(void) { return !wifi_scan_running(); }
static bool not_busy(void) { return !wifi_busy(); }

// Waits until every job queued so far is done: a scan goes in behind them, and
// the worker takes jobs in order.
static void settle(void) {
	assert(wait_until(not_busy, 8000));
	wifi_scan_start();
	assert(wait_until(not_scanning, 8000));
}

static int count_networks(const char *ssid, int *bars) {
	wifi_network_t nets[WIFI_MAX_NETWORKS];
	int n = wifi_get_networks(nets, WIFI_MAX_NETWORKS);
	int on_air = 0;
	for (int i = 0; i < n; i++) {
		if (nets[i].signal) {
			on_air++;
		}
		if (ssid && strcmp(nets[i].ssid, ssid) == 0 && bars) {
			*bars = nets[i].bars;
		}
	}
	return on_air;
}

static int count_lines(const char *text, const char *line) {
	int n = 0;
	size_t len = strlen(line);
	for (const char *p = text; (p = strstr(p, line)); p += len) {
		if ((p == text || p[-1] == '\n') && p[len] == '\n') {
			n++;
		}
	}
	return n;
}

// Where `line` first appears in `log` as a whole line, or -1.
static int line_at(const char *log, const char *line) {
	size_t len = strlen(line);
	for (const char *p = log; (p = strstr(p, line)); p += len) {
		if ((p == log || p[-1] == '\n') && p[len] == '\n') {
			return (int)(p - log);
		}
	}
	return -1;
}

// ---------------------------------------------------------------------------
// the checks
// ---------------------------------------------------------------------------

static const char AIR[] = "aa:aa:aa:aa:aa:01\t2412\t-58\t[WPA2-PSK-CCMP][ESS]\tCasa\n"
						  "aa:aa:aa:aa:aa:02\t2437\t-85\t[WPA2-PSK-CCMP][ESS]\tVecino\n"
						  "aa:aa:aa:aa:aa:03\t2462\t-70\t[ESS]\t\n";

// The radio comes up through sys_server, whose OK arrives before the work is
// done: the worker must not call the radio up until the supplicant answers.
static void check_power_on(void) {
	wifi_init();
	assert(wait_until(not_busy, 5000));
	assert(exists("up"));
	// Networks an older build saved as disabled come back at power-on.
	assert(line_at(read_log(), "enable_network all") >= 0);
	settle();
	printf("selftest: power on waits for the supplicant\n");
}

// A user with nothing saved: the table is empty until the sweep finishes.
static void check_scan_waits_for_results(void) {
	settle();
	int scans_before = ss_scans;
	uint32_t serial_before = wifi_networks_serial();
	mark_log("scan");
	wifi_scan_start();
	assert(wait_until(not_scanning, 5000));

	int bars = -1;
	assert(count_networks("Casa", &bars) == 2); // the hidden one is left out
	assert(ss_scans == scans_before);			  // wpa_cli scan, not the queued WIFI:SCAN
	assert(wifi_networks_serial() == serial_before + 1); // the empty read was not published
	assert(count_lines(log_since("scan"), "scan_results") == 2);
	printf("selftest: an empty table right after a scan is read again\n");
}

// The switch flicked off and on at once: the queued OFF must not kill the
// supplicant the ON was waiting for, and a scan asked for straight after has
// to find the networks.
static void check_off_on(void) {
	settle();
	wifi_set_enabled(false);
	wifi_set_enabled(true);
	wifi_scan_start();
	assert(wait_until(not_scanning, 8000));
	assert(exists("up"));
	assert(count_networks(NULL, NULL) == 2);
	printf("selftest: off then on finds the networks\n");
}

static wifi_op_t wait_for_op(void) {
	for (int waited = 0; waited < 8000; waited += 10) {
		wifi_op_t op = wifi_take_op_result(NULL, 0);
		if (op == WIFI_OP_OK || op == WIFI_OP_FAILED) {
			return op;
		}
		usleep(10000);
	}
	assert(!"no result from the join");
	return WIFI_OP_FAILED;
}

// Joining by name, new or saved, asks the supplicant to probe for it, ranks the
// saved networks as the stock daemon does, and enables them all again once the
// join is over, whether it worked or not.
static void check_connect(void) {
	settle();
	mark_log("join new");
	wifi_connect("Casa", "password123");
	assert(wait_for_op() == WIFI_OP_OK);
	const char *log = log_since("join new");
	assert(count_lines(log, "add_network") == 1);
	assert(count_lines(log, "set_network 1 scan_ssid 1") == 1);
	int low = line_at(log, "set_network 0 priority 1");
	int high = line_at(log, "set_network 1 priority 2");
	int select = line_at(log, "select_network 1");
	int all = line_at(log, "enable_network all");
	assert(low >= 0 && high >= 0 && low < select && high < select);
	assert(all > select && line_at(log + all, "save_config") > 0);

	// Out of range: the join fails, and the others come back all the same.
	mark_log("join saved");
	wifi_connect("Otra", NULL);
	assert(wait_for_op() == WIFI_OP_FAILED);
	log = log_since("join saved");
	assert(count_lines(log, "add_network") == 0);
	assert(count_lines(log, "set_network 0 scan_ssid 1") == 1);
	assert(line_at(log, "set_network 0 priority 2") >= 0 && line_at(log, "set_network 1 priority 1") >= 0);
	assert(line_at(log, "enable_network all") > line_at(log, "select_network 0"));
	printf("selftest: every join sets scan_ssid and priorities, and enables the rest after\n");
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s WORKDIR\n", argv[0]);
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	snprintf(dir, sizeof(dir), "%s/wifi", argv[1]);
	mkdir(argv[1], 0755);
	mkdir(dir, 0755);
	drop("up");
	drop("joined");
	drop("scanned");
	drop("log");
	put("nets", "0\tOtra\tany\t[DISABLED]\n");
	put("air", AIR);
	install("wpa_cli", FAKE_WPA_CLI);
	install("udhcpc", FAKE_UDHCPC);

	char path[2048];
	snprintf(path, sizeof(path), "%s:%s", dir, getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin");
	setenv("PATH", path, 1);
	setenv("WIFI_FAKE", dir, 1);

	pthread_t ss;
	assert(pthread_create(&ss, NULL, ss_thread, NULL) == 0);
	pthread_detach(ss);

	check_power_on();
	check_scan_waits_for_results();
	check_off_on();
	check_connect();

	printf("selftest: all Wi-Fi checks passed\n");
	return 0;
}
