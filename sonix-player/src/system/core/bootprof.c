#include "src/system/core/bootprof.h"

#include "src/system/core/utils.h"
#include "src/system/device/sysinfo.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// A start leaves about a hundred and ten marks; one past the end is dropped
// and the report says how many were.
#define MARKS 192
#define SLOWEST_SHOWN 15

typedef struct {
	const char *group;
	const char *name;
	uint32_t wall_us;
	uint32_t cpu_us;
	int32_t heap_bytes;
} mark_t;

static mark_t marks[MARKS];
static int mark_count;
static int marks_dropped;
static const char *group = "main";

// The start of the open stretch. Read again at the end of every mark, after
// the heap has been measured, so that measuring is in nobody's stretch.
static uint64_t open_wall_us;
static uint64_t open_cpu_us;
static size_t open_heap;

// Since the kernel started, which is what the monotonic clock counts on Linux.
static uint64_t start_wall_us;
static uint64_t start_cpu_us;
static uint64_t process_cpu_at_start_us; // other threads only: this one's share is taken off
static bool started;
static pthread_t start_thread;

static bool armed;
static char out_path[600];

bool bootprof_reported;

static uint64_t clock_us(clockid_t id) {
	struct timespec ts;
	if (clock_gettime(id, &ts) != 0) {
		return 0;
	}
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void open_stretch(void) {
	heap_usage(&open_heap, NULL);
	open_cpu_us = clock_us(CLOCK_THREAD_CPUTIME_ID);
	open_wall_us = clock_us(CLOCK_MONOTONIC);
}

void bootprof_start(void) {
	start_wall_us = clock_us(CLOCK_MONOTONIC);
	start_cpu_us = clock_us(CLOCK_THREAD_CPUTIME_ID);
	process_cpu_at_start_us = clock_us(CLOCK_PROCESS_CPUTIME_ID) - start_cpu_us;
	start_thread = pthread_self();
	started = true;
	open_stretch();
}

void bootprof_group(const char *name) { group = name; }

void bootprof_mark(const char *name) {
	// The stretches are one thread's: a mark from another would cut one in two.
	if (!started || bootprof_reported || !pthread_equal(pthread_self(), start_thread)) {
		return;
	}
	uint64_t wall = clock_us(CLOCK_MONOTONIC);
	uint64_t cpu = clock_us(CLOCK_THREAD_CPUTIME_ID);
	if (mark_count >= MARKS) {
		marks_dropped++;
	} else {
		size_t heap = 0;
		heap_usage(&heap, NULL);
		marks[mark_count++] = (mark_t){
			.group = group,
			.name = name,
			.wall_us = (uint32_t)(wall - open_wall_us),
			.cpu_us = (uint32_t)(cpu - open_cpu_us),
			.heap_bytes = (int32_t)((long long)heap - (long long)open_heap),
		};
	}
	open_stretch();
}

void bootprof_arm(const char *sd_root) {
	const char *env = getenv("SONIX_BOOTPROF");
	if (env && env[0]) {
		snprintf(out_path, sizeof(out_path), "%s", env);
		armed = true;
		return;
	}
	if (!sd_root || !sd_root[0]) {
		return;
	}
	snprintf(out_path, sizeof(out_path), "%s/.local/bootprof.txt", sd_root);
	armed = access(out_path, W_OK) == 0;
}

// To the log and to the file, the same line. `file` may be NULL: a card that
// went read-only since the check still leaves the report in the log.
__attribute__((format(printf, 2, 3))) static void say(FILE *file, const char *fmt, ...) {
	char line[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	printf("bootprof: %s\n", line);
	if (file) {
		fprintf(file, "%s\n", line);
	}
}

static int by_wall(const void *a, const void *b) {
	const mark_t *x = *(const mark_t *const *)a, *y = *(const mark_t *const *)b;
	return x->wall_us < y->wall_us ? 1 : x->wall_us > y->wall_us ? -1 : 0;
}

static void report(void) {
	uint64_t end_wall_us = clock_us(CLOCK_MONOTONIC);
	uint64_t end_cpu_us = clock_us(CLOCK_THREAD_CPUTIME_ID);
	FILE *file = fopen(out_path, "a");

	char when[32] = "";
	time_t now = time(NULL);
	struct tm tm;
	if (localtime_r(&now, &tm)) {
		strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
	}
	const char *build = sysinfo_build_version();
	say(file, "==== start of %s, build %s ====", when, build && build[0] ? build : "?");
	say(file, "the player started %.2f s after the kernel; first pass of the main loop done %.2f s after that",
		(double)start_wall_us / 1e6, (double)(end_wall_us - start_wall_us) / 1e6);
	say(file, "interface thread: %.2f s on the CPU of those %.2f s", (double)(end_cpu_us - start_cpu_us) / 1e6,
		(double)(end_wall_us - start_wall_us) / 1e6);
	// The rest of the player in the same stretch -- the boot animation's
	// decoder, the workers the services start -- which on one core is time the
	// interface thread waited for.
	say(file, "the player's other threads: %.2f s on the CPU so far",
		(double)(clock_us(CLOCK_PROCESS_CPUTIME_ID) - (end_cpu_us - start_cpu_us) - process_cpu_at_start_us) / 1e6);
	say(file, "%-5s %-36s %9s %9s %9s %9s", "group", "what", "at ms", "wall ms", "cpu ms", "heap KB");

	uint64_t at_us = 0;
	const char *open_group = NULL;
	uint64_t group_wall = 0, group_cpu = 0;
	long long group_heap = 0;
	for (int i = 0; i <= mark_count; i++) {
		const mark_t *m = i < mark_count ? &marks[i] : NULL;
		if (open_group && (!m || strcmp(m->group, open_group) != 0)) {
			// "gui" comes once, in one piece; "main" is everything around it
			// and gets one line for each side.
			say(file, "%-5s %-36s %9s %9.1f %9.1f %9.1f", open_group, "-- total", "", (double)group_wall / 1e3,
				(double)group_cpu / 1e3, (double)group_heap / 1024.0);
			group_wall = group_cpu = 0;
			group_heap = 0;
		}
		if (!m) {
			break;
		}
		open_group = m->group;
		at_us += m->wall_us;
		group_wall += m->wall_us;
		group_cpu += m->cpu_us;
		group_heap += m->heap_bytes;
		say(file, "%-5s %-36s %9.1f %9.1f %9.1f %9.1f", m->group, m->name, (double)at_us / 1e3,
			(double)m->wall_us / 1e3, (double)m->cpu_us / 1e3, (double)m->heap_bytes / 1024.0);
	}
	if (marks_dropped) {
		say(file, "%d marks past the table's %d were dropped: raise MARKS", marks_dropped, MARKS);
	}

	static const mark_t *sorted[MARKS];
	for (int i = 0; i < mark_count; i++) {
		sorted[i] = &marks[i];
	}
	qsort(sorted, (size_t)mark_count, sizeof(sorted[0]), by_wall);
	say(file, "-- the %d slowest", mark_count < SLOWEST_SHOWN ? mark_count : SLOWEST_SHOWN);
	for (int i = 0; i < mark_count && i < SLOWEST_SHOWN; i++) {
		say(file, "%-5s %-36s %9s %9.1f %9.1f %9.1f", sorted[i]->group, sorted[i]->name, "",
			(double)sorted[i]->wall_us / 1e3, (double)sorted[i]->cpu_us / 1e3, (double)sorted[i]->heap_bytes / 1024.0);
	}
	if (file) {
		fclose(file);
	}
}

void bootprof_first_pass(void) {
	if (bootprof_reported) {
		return;
	}
	bootprof_mark("first pass of the main loop");
	bootprof_reported = true;
	if (armed) {
		report();
	}
}

unsigned long long bootprof_call_begin(void) { return armed ? clock_us(CLOCK_MONOTONIC) : 0; }

void bootprof_call_end(const char *what, unsigned long long begun) {
	if (!begun) {
		return;
	}
	double ms = (double)(clock_us(CLOCK_MONOTONIC) - begun) / 1e3;
	// Which thread waited is the whole question: on the interface's it is a
	// frame that did not get drawn.
	const char *where = pthread_equal(pthread_self(), start_thread) ? "interface thread" : "another thread";
	printf("bootprof: call %s %.1f ms on %s\n", what, ms, where);
	FILE *file = fopen(out_path, "a");
	if (file) {
		fprintf(file, "call  %-36s %9.1f ms on %s\n", what, ms, where);
		fclose(file);
	}
}
