// fopencookie
#define _GNU_SOURCE

#include "logging.h"

#include <sys/klog.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/utils.h"
#include "src/system/core/lang.h"

// Where the log goes when it is on: beside everything else the player writes.
#define LOG_SUBDIR ".local"
#define LOG_FILE_NAME "sonix_player.log"

// Nothing ever shortened the log: left on, it reached 8 MB and more, which is
// a file nobody can read or send, and every boot appended to the end of a
// cluster chain that only grew. When it is past this size on opening, it is
// renamed to sonix_player.log.old (replacing the one before) and a fresh file
// is started, so the card holds at most twice this and the run being
// reported always starts near the top of a file. On opening only, not
// mid-run: a rename under the writer thread would mean moving the log, with
// the drain that waits on that very thread.
#define LOG_ROTATE_BYTES (4L * 1024 * 1024)

// The player prints a good deal before the card is mounted: which block device
// it found, what the battery reported, whether the clock survived. That cannot
// go to the card yet and must not go to the device's own storage, so it is held
// until the card turns up. A startup is a couple of kilobytes; anything past
// this is dropped.
#define HELD_MAX (32 * 1024)

// Where the held output waits. It is on tmpfs, so it never touches the card or
// the device's own flash, and it is unlinked the moment it is opened: nothing
// is left behind by a boot that does not finish, and the space goes back to
// the kernel when the descriptor closes.
//
// A real file and not an fmemopen() over a static array, for one reason: an
// fmemopen stream HAS NO DESCRIPTOR. fileno() returns -1, so the dup2 in
// redirect_to() fails with EBADF, stdout is never pointed at it and nothing
// reaches the buffer at all. In the USB path the same failure also leaves
// descriptors 1 and 2 open on the card, which keeps it busy and defeats the
// unmount that logging_suspend_for_usb() exists to allow.
#define EARLY_HELD_PATH "/tmp/sonix-log-early"
#define USB_HELD_PATH "/tmp/sonix-log-usb"

static FILE *early_stream;

static char sd_log_path[512];
static bool on_sd;

// The stream stdout/stderr currently feed. Kept so a switch can close the
// previous one: dup2 copies the descriptor into 1 and 2, but the original stays
// open too, and an fd left open on a file on the card keeps the card busy, so
// the USB export can never unmount it. Every log file is opened close-on-exec,
// so no child process inherits that original.
static FILE *active_stream;

// The card can go away under the log: the USB export unmounts it, and so does
// pulling it out. While it is gone everything printed is held the same way and
// appended to the file when the card comes back, so the export and hotplug
// diagnostics are not the lines that get lost.
static FILE *usb_stream;
static bool usb_was_on_sd;

// Held by everything that moves the log to another file, and by
// logging_sync(): a sync still running on the card's file would keep the card
// busy through the unmount that follows a release. Recursive because
// logging_attach_sd() goes through logging_resume_after_usb().
static pthread_mutex_t log_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

// The size of the card's log at the last sync; -1 before the first.
static off_t synced_size = -1;

const char *logging_path(void) {
	if (on_sd && sd_log_path[0]) {
		return sd_log_path;
	}
	// The setting is on but the file is not open: the card is out, or it came
	// back read-only. Reporting that is not the same as reporting "disabled",
	// which made a card change look like the option had turned itself off.
	if (config_get_bool("system", "log_to_sd", false)) {
		return tr("log_card_missing");
	}
	return tr("log_disabled");
}

// The switch shows the setting, not whether the file happens to be open right
// now. Pulling the card closes the file, and reading that back as the state of
// the option made "Log to microSD" flip itself off and stay off, even though
// nothing had ever written false to the config.
bool logging_to_sd(void) { return config_get_bool("system", "log_to_sd", false); }

// A place to hold output that cannot go where it belongs yet: a real file on
// tmpfs, unlinked at once so it has a name for nobody. NULL when tmpfs will
// not take it, and the caller then falls back to /dev/null.
static FILE *open_held(const char *path) {
	FILE *file = fopen(path, "w+e");
	if (!file) {
		return NULL;
	}
	unlink(path);
	return file;
}

// Copies what a held stream collected into `destination` and closes it. The
// copy goes through a small stack buffer rather than one big read: the whole
// point of moving this out of a static array was to stop reserving 32 kB for
// a startup that prints two.
static void drain_held(FILE *held, FILE *destination) {
	if (!held) {
		return;
	}
	// A line still in stdout's buffer is headed for the held file, through
	// the descriptors that point at it: it goes in now or it is lost.
	logging_flush();
	fflush(held);
	long size = ftell(held);
	rewind(held);

	if (destination && size > 0) {
		if (size > HELD_MAX) {
			size = HELD_MAX;
		}
		char chunk[512];
		long left = size;
		while (left > 0) {
			size_t want = (size_t)(left < (long)sizeof(chunk) ? left : (long)sizeof(chunk));
			size_t got = fread(chunk, 1, want, held);
			if (got == 0) {
				break;
			}
			fwrite(chunk, 1, got, destination);
			left -= (long)got;
		}
		fflush(destination);
	}
	fclose(held);
}

// ---------------------------------------------------------------------------
// The time on every line
//
// stdout and stderr are replaced with two streams of the player's own, built
// with fopencookie(), that put the time in front of each line and hand the
// result to descriptors 1 and 2. glibc documents the three standard streams as
// plain variables that a program may reassign, and every printf, puts, perror
// and fprintf(stderr) in the process -- LVGL's log and the libraries included
// -- reads the variable, so the whole log is stamped without a call site
// changing.
//
// The descriptors stay where they were: redirect_to() still moves 1 and 2 with
// dup2, and whatever writes to them directly goes out unstamped but in place.
// That is the crash handler in main.c (write(2) only, which is all a signal
// handler may use) and the child processes, which inherit the descriptors and
// not the streams.
//
// The time is the same local time the status bar shows, to the millisecond. A
// line with the date goes out before the first stamped line and again whenever
// the day changes: at midnight, and when the clock is set, which on a device
// that booted with no time is the difference between 1970 and today.
// ---------------------------------------------------------------------------

typedef struct {
	int fd;
	bool line_start; // the next byte begins a line
} stamp_stream_t;

static stamp_stream_t stamp_out = {STDOUT_FILENO, true};
static stamp_stream_t stamp_err = {STDERR_FILENO, true};

// Year and day of the year of the last date line; -1 before the first.
static int stamp_day = -1;

// Sees every whole line printed, before it is stamped. Set once, at startup.
static logging_line_tap_t line_tap;

void logging_set_line_tap(logging_line_tap_t tap) { line_tap = tap; }

static void write_all(int fd, const char *data, size_t size);

// ---------------------------------------------------------------------------
// The queue in front of the card
//
// A write() to the log's file takes the file's inode lock, and so does the
// fdatasync that follows it once a second; with the card busy (a hi-res track
// streaming off it, a shape being worked out) a sync can hold that lock for as
// long as the card takes to answer. Every thread that prints waits behind it:
// the interface, the playback thread, the buttons.
//
// So a printed line goes into this queue in memory and one thread of its own
// writes it out and syncs. A thread that prints never waits on the card. When
// the queue is full the line is dropped and counted, and the count goes into
// the log in front of the next line that fits.
//
// Each record is a one-byte descriptor, a two-byte length, then the bytes.
// ---------------------------------------------------------------------------

#define QUEUE_BYTES (64 * 1024)
#define QUEUE_RECORD_MAX 2048
#define QUEUE_HEADER 3
#define QUEUE_WRITE_MAX 8192
#define QUEUE_SYNC_MS 1000
#define QUEUE_DRAIN_MS 1500

static pthread_mutex_t queue_lock;
static pthread_cond_t queue_filled;
static pthread_cond_t queue_emptied;
static char queue_ring[QUEUE_BYTES];
static size_t queue_head; // oldest byte
static size_t queue_len;
static bool queue_writing; // the writer holds bytes it has taken out
static bool queue_running; // the writer thread exists (never in a forked child)
static unsigned long queue_dropped;

// Set while stdout and stderr lead to /dev/null, which with "Log to microSD"
// off -- the default -- is all the time. Every printed line used to go through
// the queue regardless: the lock, a signal that woke the writer thread (a
// context switch per line on one core) and a write() into /dev/null. Now emit()
// drops it on the spot. The stamp is still worked out and the line tap still
// sees every line, so the Bluetooth log is unchanged.
static bool log_discarding;

static void ring_put(const char *data, size_t size) {
	size_t tail = (queue_head + queue_len) % QUEUE_BYTES;
	size_t first = QUEUE_BYTES - tail < size ? QUEUE_BYTES - tail : size;
	memcpy(queue_ring + tail, data, first);
	memcpy(queue_ring, data + first, size - first);
	queue_len += size;
}

static void ring_get(char *out, size_t size) {
	size_t first = QUEUE_BYTES - queue_head < size ? QUEUE_BYTES - queue_head : size;
	memcpy(out, queue_ring + queue_head, first);
	memcpy(out + first, queue_ring, size - first);
	queue_head = (queue_head + size) % QUEUE_BYTES;
	queue_len -= size;
}

static void ring_peek(char *out, size_t size) {
	size_t first = QUEUE_BYTES - queue_head < size ? QUEUE_BYTES - queue_head : size;
	memcpy(out, queue_ring + queue_head, first);
	memcpy(out + first, queue_ring, size - first);
}

// One record, or nothing if it does not fit. Caller holds queue_lock.
static bool queue_record(int fd, const char *data, size_t size) {
	if (QUEUE_BYTES - queue_len < size + QUEUE_HEADER) {
		return false;
	}
	char header[QUEUE_HEADER] = {(char)fd, (char)(size & 0xff), (char)(size >> 8)};
	ring_put(header, QUEUE_HEADER);
	ring_put(data, size);
	return true;
}

// Bytes for descriptor `fd`: into the queue while the writer runs, straight
// to the descriptor before it starts and in a forked child.
static void emit(int fd, const char *data, size_t size) {
	if (log_discarding) {
		return;
	}
	if (!queue_running) {
		write_all(fd, data, size);
		return;
	}
	pthread_mutex_lock(&queue_lock);
	if (queue_dropped > 0) {
		char note[80];
		int len = snprintf(note, sizeof(note), "---- %lu bytes of log dropped: the card fell behind ----\n",
						   queue_dropped);
		if (len > 0 && queue_record(fd, note, (size_t)len)) {
			queue_dropped = 0;
		}
	}
	while (size > 0) {
		size_t part = size < QUEUE_RECORD_MAX ? size : QUEUE_RECORD_MAX;
		if (queue_dropped > 0 || !queue_record(fd, data, part)) {
			queue_dropped += size;
			break;
		}
		data += part;
		size -= part;
	}
	pthread_cond_signal(&queue_filled);
	pthread_mutex_unlock(&queue_lock);
}

static long queue_now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void sync_card(void);

// Takes out the records at the front that go to one descriptor, up to
// QUEUE_WRITE_MAX bytes. Caller holds queue_lock and has checked queue_len.
static size_t queue_take(char *out, int *fd_out) {
	size_t used = 0;
	int fd = -1;
	while (queue_len >= QUEUE_HEADER) {
		char header[QUEUE_HEADER];
		ring_peek(header, QUEUE_HEADER);
		int rec_fd = (unsigned char)header[0];
		size_t size = (unsigned char)header[1] | ((size_t)(unsigned char)header[2] << 8);
		if ((fd >= 0 && rec_fd != fd) || used + size > QUEUE_WRITE_MAX) {
			break;
		}
		ring_get(header, QUEUE_HEADER);
		ring_get(out + used, size);
		used += size;
		fd = rec_fd;
	}
	*fd_out = fd;
	return used;
}

static void *queue_writer(void *arg) {
	(void)arg;
	prctl(PR_SET_NAME, "logwriter", 0, 0, 0);
	static char out[QUEUE_WRITE_MAX];
	bool unsynced = false;
	long synced_at = queue_now_ms();

	for (;;) {
		pthread_mutex_lock(&queue_lock);
		while (queue_len == 0) {
			queue_writing = false;
			pthread_cond_broadcast(&queue_emptied);
			if (!unsynced) {
				pthread_cond_wait(&queue_filled, &queue_lock);
				continue;
			}
			long wait_ms = synced_at + QUEUE_SYNC_MS - queue_now_ms();
			if (wait_ms <= 0) {
				break;
			}
			struct timespec due;
			clock_gettime(CLOCK_MONOTONIC, &due);
			long ns = due.tv_nsec + (wait_ms % 1000) * 1000000L;
			due.tv_sec += wait_ms / 1000 + ns / 1000000000L;
			due.tv_nsec = ns % 1000000000L;
			if (pthread_cond_timedwait(&queue_filled, &queue_lock, &due) == ETIMEDOUT) {
				break;
			}
		}
		int fd = -1;
		size_t size = 0;
		if (queue_len > 0) {
			size = queue_take(out, &fd);
			queue_writing = true;
		}
		pthread_mutex_unlock(&queue_lock);

		if (size > 0) {
			write_all(fd, out, size);
			unsynced = true;
		}
		if (unsynced && queue_now_ms() - synced_at >= QUEUE_SYNC_MS) {
			sync_card();
			synced_at = queue_now_ms();
			unsynced = false;
		}
	}
	return NULL;
}

// Waits until everything queued so far is written, for at most
// QUEUE_DRAIN_MS: the card may be the thing that is stuck.
static void queue_drain(void) {
	if (!queue_running) {
		return;
	}
	struct timespec due;
	clock_gettime(CLOCK_MONOTONIC, &due);
	long ns = due.tv_nsec + (QUEUE_DRAIN_MS % 1000) * 1000000L;
	due.tv_sec += QUEUE_DRAIN_MS / 1000 + ns / 1000000000L;
	due.tv_nsec = ns % 1000000000L;
	pthread_mutex_lock(&queue_lock);
	while (queue_len > 0 || queue_writing) {
		if (pthread_cond_timedwait(&queue_emptied, &queue_lock, &due) == ETIMEDOUT) {
			break;
		}
	}
	pthread_mutex_unlock(&queue_lock);
}

// A forked child has no writer and writes straight through, so it never needs
// the queue; it gets a fresh lock all the same. The lock is priority
// inheriting, which stores the owner's thread id: one taken in the parent can
// be neither unlocked nor taken in the child, whose thread has another id.
static void queue_after_fork_child(void) {
	queue_running = false;
	log_discarding = false; // the child's descriptors are its own business
	pthread_mutex_init(&queue_lock, NULL);
}

static void queue_start(void) {
	pthread_mutexattr_t mattr;
	pthread_mutexattr_init(&mattr);
	// The playback thread prints at real-time priority.
	pthread_mutexattr_setprotocol(&mattr, PTHREAD_PRIO_INHERIT);
	pthread_mutex_init(&queue_lock, &mattr);
	pthread_mutexattr_destroy(&mattr);

	pthread_condattr_t cattr;
	pthread_condattr_init(&cattr);
	pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
	pthread_cond_init(&queue_filled, &cattr);
	pthread_cond_init(&queue_emptied, &cattr);
	pthread_condattr_destroy(&cattr);

	pthread_atfork(NULL, NULL, queue_after_fork_child);

	pthread_t thread;
	pthread_attr_t tattr;
	pthread_attr_init(&tattr);
	pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&thread, &tattr, queue_writer, NULL) == 0) {
		queue_running = true;
	}
	pthread_attr_destroy(&tattr);
}

void logging_flush(void) {
	fflush(stdout);
	fflush(stderr);
	queue_drain();
}

// ---------------------------------------------------------------------------
// The daemons' output
//
// A daemon started by the player outlives every move of the log, and its own
// descriptor on the card's file would keep the card busy through the USB
// export and the power-off. So the daemons write into a pipe, and a thread
// copies what comes out of it into the log, a line at a time, stamped like the
// rest.
//
// The write end is non-blocking: with the player stuck or gone, a daemon loses
// its output, not its time. Each daemon also keeps a descriptor on the read
// end, so a write never finds the pipe without a reader and never raises
// SIGPIPE.
// ---------------------------------------------------------------------------

#define RELAY_LINE_MAX 512

static int relay_read = -1;
static int relay_write = -1;

static void relay_line(const char *line, size_t len) {
	if (len > 0) {
		fprintf(stderr, "%.*s\n", (int)len, line);
	}
}

static void *relay_reader(void *arg) {
	(void)arg;
	thread_be_background("logrelay");
	char chunk[256];
	char line[RELAY_LINE_MAX];
	size_t used = 0;

	for (;;) {
		ssize_t got = read(relay_read, chunk, sizeof(chunk));
		if (got < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (got == 0) {
			break;
		}
		for (ssize_t i = 0; i < got; i++) {
			if (chunk[i] == '\n') {
				relay_line(line, used);
				used = 0;
				continue;
			}
			line[used++] = chunk[i];
			if (used == sizeof(line)) {
				relay_line(line, used);
				used = 0;
			}
		}
	}
	return NULL;
}

static void relay_start(void) {
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) != 0) {
		return;
	}
	int flags = fcntl(fds[1], F_GETFL);
	if (flags >= 0) {
		fcntl(fds[1], F_SETFL, flags | O_NONBLOCK);
	}
	relay_read = fds[0];
	relay_write = fds[1];

	pthread_t thread;
	pthread_attr_t tattr;
	pthread_attr_init(&tattr);
	pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&thread, &tattr, relay_reader, NULL) != 0) {
		close(fds[0]);
		close(fds[1]);
		relay_read = relay_write = -1;
	}
	pthread_attr_destroy(&tattr);
}

void logging_child_stdio(void) {
	if (relay_write < 0) {
		return;
	}
	dup2(relay_write, STDOUT_FILENO);
	dup2(relay_write, STDERR_FILENO);
	// Not close-on-exec, so it lives as long as the program the child becomes.
	int reader = dup(relay_read);
	(void)reader;
}

static void write_all(int fd, const char *data, size_t size) {
	while (size > 0) {
		ssize_t done = write(fd, data, size);
		if (done < 0) {
			if (errno == EINTR) {
				continue;
			}
			return; // a card that went away mid-line; nothing to be done here
		}
		data += done;
		size -= (size_t)done;
	}
}

static void write_stamp(int fd) {
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	time_t seconds = now.tv_sec;
	struct tm local;
	if (!localtime_r(&seconds, &local)) {
		return;
	}

	char text[48];
	int day = local.tm_year * 366 + local.tm_yday;
	if (day != stamp_day) {
		stamp_day = day;
		size_t len = strftime(text, sizeof(text), "---- %Y-%m-%d ----\n", &local);
		emit(fd, text, len);
	}

	int len = snprintf(text, sizeof(text), "%02d:%02d:%02d.%03ld ", local.tm_hour, local.tm_min, local.tm_sec,
					   now.tv_nsec / 1000000L);
	if (len > 0) {
		emit(fd, text, (size_t)len);
	}
}

// Called by stdio with whatever it has buffered: usually one line, but a
// single printf can carry several, and a full buffer can end mid-line. The
// stream's lock is held, so line_start needs none of its own. An empty line
// stays empty.
static ssize_t stamp_write(void *cookie, const char *data, size_t size) {
	stamp_stream_t *stream = cookie;
	size_t done = 0;
	while (done < size) {
		const char *start = data + done;
		const char *newline = memchr(start, '\n', size - done);
		size_t len = newline ? (size_t)(newline - start) + 1 : size - done;
		if (stream->line_start && *start != '\n') {
			write_stamp(stream->fd);
			// Only lines that arrive whole, which with line buffering is all
			// but the rare one longer than the stream's buffer.
			if (line_tap && newline) {
				line_tap(start, len - 1);
			}
		}
		emit(stream->fd, start, len);
		stream->line_start = newline != NULL;
		done += len;
	}
	// Always all of it. A write that failed (the card pulled) must not leave
	// the stream in an error state that outlives the card coming back.
	return (ssize_t)size;
}

static FILE *stamp_open(stamp_stream_t *stream) {
	cookie_io_functions_t io = {.write = stamp_write};
	FILE *file = fopencookie(stream, "w", io);
	if (file) {
		setvbuf(file, NULL, _IOLBF, 0);
	}
	return file;
}

// Puts the stamping streams in place of stdout and stderr. Either one that
// cannot be made leaves the original where it was, which prints as before.
static void stamp_install(void) {
	fflush(stdout);
	fflush(stderr);
	FILE *out = stamp_open(&stamp_out);
	if (out) {
		stdout = out;
	} else {
		setvbuf(stdout, NULL, _IOLBF, 0);
	}
	FILE *err = stamp_open(&stamp_err);
	if (err) {
		stderr = err;
	} else {
		setvbuf(stderr, NULL, _IOLBF, 0);
	}
}

// Points descriptors 1 and 2 -- and so stdout and stderr, which write to them
// -- at `stream`. Line buffered, so a crash still leaves everything up to the
// last newline behind. Closes the stream they fed before.
//
// False when the descriptors could not be moved, and then nothing is closed:
// closing the previous stream after a failed dup2 would leave stdout writing
// into a descriptor nobody owns any more.
static bool redirect_to(FILE *stream) {
	if (!stream) {
		return false;
	}

	// Whatever stdout and stderr still hold belongs to the old destination.
	logging_flush();

	setvbuf(stream, NULL, _IOLBF, 0);
	int fd = fileno(stream);
	// The numbers and not fileno(stdout): the stamping streams have no
	// descriptor of their own, and fileno() on them is -1.
	if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
		return false;
	}

	if (active_stream && active_stream != stream) {
		fclose(active_stream);
	}
	active_stream = stream;
	synced_size = -1;
	log_discarding = false; // open_null_log() sets it again for its sink
	stamp_day = -1; // a new file starts with the date, even if /dev/null had it
	return true;
}

void logging_init(void) {
	stamp_install();
	queue_start();
	relay_start();

	const char *override = getenv("SONIX_LOG");
	if (override && override[0]) {
		FILE *file = fopen(override, "we");
		if (file && redirect_to(file)) {
			on_sd = true;
			snprintf(sd_log_path, sizeof(sd_log_path), "%s", override);
		} else if (file) {
			fclose(file);
		}
		return;
	}

#ifndef HOST_BUILD
	// Everything printed from here is held until the card turns up.
	early_stream = open_held(EARLY_HELD_PATH);
	if (early_stream && !redirect_to(early_stream)) {
		fclose(early_stream);
		early_stream = NULL;
	}
#endif
}

// Stops holding startup output and lets it go, either into the card's log or
// nowhere at all.
static void flush_early(FILE *destination) {
	if (!early_stream) {
		return;
	}

	if (active_stream == early_stream) {
		active_stream = NULL; // drain_held closes it, not redirect_to
	}
	drain_held(early_stream, destination);
	early_stream = NULL;
}

// Opens the card's log and makes it the destination. Returns false if the card
// will not take it.
static bool open_sd_log(void) {
	if (!sd_log_path[0]) {
		return false;
	}

	struct stat st;
	if (stat(sd_log_path, &st) == 0 && st.st_size > LOG_ROTATE_BYTES) {
		char old_path[sizeof(sd_log_path) + 8];
		snprintf(old_path, sizeof(old_path), "%s.old", sd_log_path);
		rename(sd_log_path, old_path); // failing leaves it appending, as before
	}

	FILE *file = fopen(sd_log_path, "ae");
	if (!file) {
		return false;
	}

	flush_early(file);
	if (!redirect_to(file)) { // closes whatever was feeding stdout, usb_stream included
		fclose(file);
		return false;
	}
	usb_stream = NULL;
	on_sd = true;

	printf("log: writing to %s\n", sd_log_path);
	return true;
}

// Sends output nowhere. /dev/null keeps every printf in the code harmless
// without any of them having to check whether logging is on.
static void open_null_log(void) {
	flush_early(NULL);

	FILE *sink = fopen("/dev/null", "we");
	if (sink) {
		if (redirect_to(sink)) {
			usb_stream = NULL; // redirect_to has just closed it
			log_discarding = true;
		} else {
			fclose(sink);
		}
	}
	on_sd = false;
}

static void logging_attach_sd_locked(const char *sd_root) {
	if (getenv("SONIX_LOG")) {
		return; // an explicit path wins over everything
	}

	if (sd_root && sd_root[0]) {
		char folder[512];
		if (snprintf(folder, sizeof(folder), "%s/%s", sd_root, LOG_SUBDIR) < (int)sizeof(folder)) {
			mkdir(folder, 0755);
			if (snprintf(sd_log_path, sizeof(sd_log_path), "%s/%s", folder, LOG_FILE_NAME) >=
				(int)sizeof(sd_log_path)) {
				sd_log_path[0] = '\0';
			}
		}
	}

	// If the card has just come back, append the lines buffered while it was
	// away rather than starting fresh and losing the part that explains what
	// happened.
	logging_resume_after_usb();
	if (on_sd) {
		return;
	}

	if (config_get_bool("system", "log_to_sd", false) && open_sd_log()) {
		return;
	}

	open_null_log();
}

void logging_attach_sd(const char *sd_root) {
	pthread_mutex_lock(&log_lock);
	logging_attach_sd_locked(sd_root);
	pthread_mutex_unlock(&log_lock);
}

// ---------------------------------------------------------------------------
// The USB mass-storage export unmounts the card, and so does pulling it out;
// the log usually lives on the card. See the buffer declared at the top.
// ---------------------------------------------------------------------------

static void logging_suspend_for_usb_locked(void) {
	if (!on_sd || usb_stream) {
		// Nothing holding the card, nothing to release, and usb_was_on_sd must
		// not be touched here. Pulling a card fires two matching uevents, so
		// this runs twice: the first call recorded that the log was on the
		// card, the second overwrote that with "no", and the log could then
		// never be brought back.
		return;
	}
	usb_was_on_sd = true;

	printf("log: card log released, holding output until the card is back\n");
	fflush(stdout);

	usb_stream = open_held(USB_HELD_PATH);
	if (usb_stream && !redirect_to(usb_stream)) { // closes the file on the card
		fclose(usb_stream);
		usb_stream = NULL;
	}
	if (!usb_stream) {
		open_null_log();
	}
	on_sd = false;
}

void logging_suspend_for_usb(void) {
	pthread_mutex_lock(&log_lock);
	logging_suspend_for_usb_locked();
	pthread_mutex_unlock(&log_lock);
}

static void logging_resume_after_usb_locked(void) {
	if (!usb_was_on_sd) {
		return;
	}

	FILE *file = sd_log_path[0] ? fopen(sd_log_path, "ae") : NULL;
	if (file) {
		// The held lines go in before the redirect, because draining closes
		// the stream stdout is still pointing at.
		if (active_stream == usb_stream) {
			active_stream = NULL;
		}
		drain_held(usb_stream, file);
		usb_stream = NULL;

		if (!redirect_to(file)) {
			fclose(file);
			open_null_log();
			return;
		}
		on_sd = true;
		usb_was_on_sd = false; // only now: the flag is spent when it has paid off
		printf("log: card log resumed\n");
	} else {
		// The card did not come back writable. usb_was_on_sd stays set so a
		// later attempt (a reinsert, or the switch being toggled) can still
		// put the log back on the card.
		if (active_stream == usb_stream) {
			active_stream = NULL;
		}
		drain_held(usb_stream, NULL);
		usb_stream = NULL;
		open_null_log();
	}
}

void logging_resume_after_usb(void) {
	pthread_mutex_lock(&log_lock);
	logging_resume_after_usb_locked();
	pthread_mutex_unlock(&log_lock);
}

static void logging_set_to_sd_locked(bool enabled) {
	config_set_bool("system", "log_to_sd", enabled);
	config_save();

	if (enabled == on_sd) {
		return;
	}

	if (enabled) {
		if (!open_sd_log()) {
			open_null_log(); // no card, or it would not take the file
		}
		return;
	}

	printf("log: card logging off\n");
	fflush(stdout);
	open_null_log();
}

void logging_set_to_sd(bool enabled) {
	pthread_mutex_lock(&log_lock);
	logging_set_to_sd_locked(enabled);
	pthread_mutex_unlock(&log_lock);
}

// ---------------------------------------------------------------------------
// Lines that survive a reset
// ---------------------------------------------------------------------------

void logging_sync(void) {
	if (!queue_running) {
		sync_card();
	}
}

static void sync_card(void) {
	if (pthread_mutex_trylock(&log_lock) != 0) {
		return; // the log is moving to another file; the next call catches up
	}
	if (on_sd && active_stream) {
		int fd = fileno(active_stream);
		struct stat st;
		if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size != synced_size) {
			if (fdatasync(fd) == 0) {
				synced_size = st.st_size;
			}
		}
	}
	pthread_mutex_unlock(&log_lock);
}

// ---------------------------------------------------------------------------
// When the kernel dies under the player
//
// An oops leaves the kernel running but no longer sound. The thread it struck
// is gone, perhaps holding a lock of the process's memory map or of a driver,
// and what follows is a device that stops answering one part at a time: in
// the report this was written for, the card stopped being readable three
// seconds later and the device sat frozen until it was reset by hand.
//
// The kernel cannot be mended from here, but the device can be restarted.
// The whole report goes into the log -- normally capped at
// KMSG_LINES_PER_CALL, which cut off the call trace that says where it
// happened -- then the log is written out, the card synced and the machine
// rebooted; playback picks up where it was remembered, as after any restart.
//
// Each of those steps can itself hang on a kernel in that state, and in a
// process whose memory map may be locked a new thread may never start. So the
// guard thread is made at startup and waits. It is told the moment the report
// begins, before anything else is printed or written, and then:
//
//   - turns kernel.panic_on_oops on, so that a second oops -- in any thread,
//     the restart below included -- panics instead of killing one more thread;
//   - gives the orderly way OOPS_DEADLINE_S;
//   - restarts the machine through /proc/sysrq-trigger, which asks nothing of
//     this process or of the card.
//
// And kernel.panic is set at startup, when it is 0: a panic restarts the
// device after KERNEL_PANIC_RESTART_S instead of leaving it frozen.
// ---------------------------------------------------------------------------

#define OOPS_DEADLINE_S 15
#define KERNEL_PANIC_RESTART_S "10"

static sem_t oops_go;
static bool oops_guard_running;
static bool oops_seen;

// Writes `value` into a /proc file. False when it could not be opened or
// written.
static bool proc_write(const char *path, const char *value) {
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	bool ok = write(fd, value, strlen(value)) == (ssize_t)strlen(value);
	close(fd);
	return ok;
}

static void *oops_guard(void *arg) {
	(void)arg;
	thread_be_realtime("oops guard", 20);
	while (sem_wait(&oops_go) != 0) {
	}
	proc_write("/proc/sys/kernel/panic_on_oops", "1");
	sleep(OOPS_DEADLINE_S);
	int fd = open("/proc/sysrq-trigger", O_WRONLY | O_CLOEXEC);
	if (fd >= 0) {
		// An emergency sync first, done by the kernel's own threads, then the
		// reset itself.
		ssize_t done = write(fd, "s", 1);
		sleep(2);
		done = write(fd, "b", 1);
		(void)done;
		close(fd);
	}
	reboot(RB_AUTOBOOT);
	return NULL;
}

static void oops_guard_start(void) {
#ifdef HOST_BUILD
	// The simulator's kernel is the desk's own: nothing here restarts it.
	return;
#endif
	// Left alone when the system already chose a timeout.
	char current[16] = "";
	int fd = open("/proc/sys/kernel/panic", O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		ssize_t got = read(fd, current, sizeof(current) - 1);
		current[got > 0 ? got : 0] = '\0';
		close(fd);
		if (atoi(current) == 0 && proc_write("/proc/sys/kernel/panic", KERNEL_PANIC_RESTART_S)) {
			fprintf(stderr, "kernel: a kernel panic restarts the device after %s s\n", KERNEL_PANIC_RESTART_S);
		}
	}

	if (sem_init(&oops_go, 0, 0) != 0) {
		return;
	}
	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	oops_guard_running = pthread_create(&thread, &attr, oops_guard, NULL) == 0;
	pthread_attr_destroy(&attr);
}

// The first line of a die() report on MIPS: the reason, then "[#n]:" --
// "Oops[#1]:", "Unhandled kernel unaligned access[#1]:", "Kernel bug
// detected[#1]:".
static bool kernel_died_line(const char *text) {
	size_t len = strlen(text);
	return len > 2 && strcmp(text + len - 2, "]:") == 0 && strstr(text, "[#") != NULL;
}

// The guard's countdown, started once.
static void oops_guard_arm(void) {
	static bool armed;
	if (oops_guard_running && !armed) {
		armed = true;
		sem_post(&oops_go);
	}
}

static void restart_after_oops(void) {
	fprintf(stderr, "kernel: the kernel failed under the player; restarting the device\n");
#ifdef HOST_BUILD
	logging_flush();
	return;
#endif
	oops_guard_arm();
	logging_flush();
	sync_card();
	sync();
	reboot(RB_AUTOBOOT);
}

// /dev/kmsg, opened on the first call and read from its end: what the ring
// already held at that point is logging_report_previous_run()'s.
static int kmsg_fd = -1;
static bool kmsg_tried;

// Warnings and worse. Below that the kernel talks about every card and
// interface that comes and goes.
#define KMSG_LEVEL_MAX 4

// Vendor drivers that print routine events at warning level, every time. On
// a log from the Blaze they were most of what the kernel put in the player's
// log -- 43% of the file's bytes, each one a read here, a stamped line through
// the queue and a write to the card: the touch controller's ten lines for
// every screen-off and wake, the codec's line per volume step, the SDIO card
// reader's CIS tuples, the Wi-Fi driver's line per DHCP and EAPOL packet.
// None of them ever explained anything. Dropped before they are counted
// against KMSG_LINES_PER_CALL; an oops is looked for before this, so nothing
// here can hide one. The codec's mute and rate lines stay: those are what a
// pop or a silent output looks like from the kernel's side.
static const char *const KERNEL_NOISE[] = {
	"<<-GTP-INFO->>", "(GTP INT IRQ)", "queuing unknown CIS tuple", "DHCP - ", "ETHER_TYPE_",
	"Got BRCM event packet", "lch volume: ", "rch volume: ",
};

static bool kernel_line_noise(const char *text) {
	for (size_t i = 0; i < sizeof(KERNEL_NOISE) / sizeof(KERNEL_NOISE[0]); i++) {
		if (strstr(text, KERNEL_NOISE[i])) {
			return true;
		}
	}
	return false;
}

// Lines copied per call; the rest are counted, not printed. No cap once the
// kernel has died: the whole report is wanted.
#define KMSG_LINES_PER_CALL 20

// Copies what the kernel has said since the last call. True when a die()
// report began in it.
static bool kmsg_copy(void) {
	bool died = false;
	int printed = 0;
	int skipped = 0;
	char record[1024];
	for (;;) {
		// One record per read, "level,seq,usec,flags;text\n" followed by
		// " KEY=value" lines. EPIPE: records were overwritten before this read.
		ssize_t got = read(kmsg_fd, record, sizeof(record) - 1);
		if (got < 0) {
			if (errno == EINTR || errno == EPIPE) {
				continue;
			}
			break; // EAGAIN: nothing more for now
		}
		if (got == 0) {
			break;
		}
		record[got] = '\0';

		char *text = strchr(record, ';');
		if (!text) {
			continue;
		}
		*text++ = '\0';
		text[strcspn(text, "\n")] = '\0';

		int level = 0;
		unsigned long long usec = 0;
		if (sscanf(record, "%d,%*u,%llu", &level, &usec) < 1 || (level & 7) > KMSG_LEVEL_MAX) {
			continue;
		}
		if (!oops_seen && kernel_died_line(text)) {
			oops_seen = true;
			died = true;
			// Before the report is printed: printing and writing it out go
			// through the same kernel that has just failed.
			oops_guard_arm();
		}
		if (!oops_seen && kernel_line_noise(text)) {
			continue;
		}
		if (!oops_seen && printed >= KMSG_LINES_PER_CALL) {
			skipped++;
			continue;
		}
		fprintf(stderr, "kernel: <%d> [%5llu.%06llu] %s\n", level & 7, usec / 1000000ULL, usec % 1000000ULL, text);
		printed++;
	}
	if (skipped > 0) {
		fprintf(stderr, "kernel: %d more lines not copied\n", skipped);
	}
	return died;
}

void logging_follow_kernel(void) {
	if (!kmsg_tried) {
		kmsg_tried = true;
		kmsg_fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (kmsg_fd < 0) {
			fprintf(stderr, "kernel: /dev/kmsg: %s; kernel messages stay out of the log\n", strerror(errno));
			return;
		}
		lseek(kmsg_fd, 0, SEEK_END);
		oops_guard_start();
	}
	if (kmsg_fd < 0) {
		return;
	}

	if (kmsg_copy()) {
		// The report can still be coming out: a moment more for its tail.
		usleep(300 * 1000);
		kmsg_copy();
		restart_after_oops();
	}
}

// ---------------------------------------------------------------------------
// What the kernel says about the previous run
// ---------------------------------------------------------------------------

// Lines worth repeating. The out-of-memory killer announces itself with the
// first three; the last two are what a card that stops answering looks like,
// and that ends a scan just as abruptly.
static const char *const KERNEL_INTERESTING[] = {
	"Out of memory", "oom-kill", "Killed process", "lowmemorykiller",
	"mmc0: ",		 "I/O error", "EXT4-fs error",  "FAT-fs error",
};

static bool kernel_line_interesting(const char *line) {
	for (size_t i = 0; i < sizeof(KERNEL_INTERESTING) / sizeof(KERNEL_INTERESTING[0]); i++) {
		if (strstr(line, KERNEL_INTERESTING[i])) {
			return true;
		}
	}
	return false;
}

// How much of the ring buffer to take, and how many lines to repeat. The buffer
// is a few hundred kilobytes on a desktop and far less here; the cap is on this
// side so a card throwing errors by the thousand cannot fill the player's log
// with them.
#define KERNEL_BUFFER_BYTES 16384
#define KERNEL_LINES_MAX 12

// ---------------------------------------------------------------------------
// What the kernel kept from before a reset
//
// A kernel with pstore (ramoops) or /proc/last_kmsg keeps its log across a
// warm reset. Those are the only records of a panic: the log on the card stops
// wherever the page cache was last written out.
// ---------------------------------------------------------------------------

#define KEPT_BYTES_MAX (512 * 1024)
#define KEPT_LINES 40

static const char *const KEPT_SIGNS[] = {
	"Kernel panic", "Oops", "Unable to handle", "BUG:", "Call Trace", "Internal error",
};

// The whole file, NUL-terminated, up to KEPT_BYTES_MAX; *size is its length.
// /proc files report a size of 0, so this reads rather than stats.
static char *slurp(const char *path, size_t *size) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	size_t cap = 16384;
	size_t len = 0;
	char *data = malloc(cap + 1);
	while (data) {
		size_t got = fread(data + len, 1, cap - len, f);
		len += got;
		if (got == 0 || len >= KEPT_BYTES_MAX) {
			break;
		}
		if (len == cap) {
			char *bigger = realloc(data, cap * 2 + 1);
			if (!bigger) {
				break;
			}
			data = bigger;
			cap *= 2;
		}
	}
	fclose(f);
	if (data) {
		// Records can carry NULs; they would end every string search early.
		for (size_t i = 0; i < len; i++) {
			if (data[i] == '\0') {
				data[i] = ' ';
			}
		}
		data[len] = '\0';
		*size = len;
	}
	return data;
}

static bool kept_shows_a_crash(const char *data) {
	for (size_t i = 0; i < sizeof(KEPT_SIGNS) / sizeof(KEPT_SIGNS[0]); i++) {
		if (strstr(data, KEPT_SIGNS[i])) {
			return true;
		}
	}
	return false;
}

// Prints the last KEPT_LINES lines of `data`.
static void print_tail(const char *name, char *data, size_t len) {
	size_t start = len;
	int lines = 0;
	while (start > 0) {
		if (data[start - 1] == '\n' && start != len && ++lines >= KEPT_LINES) {
			break;
		}
		start--;
	}
	fprintf(stderr, "kernel: the end of %s --\n", name);
	char *line = strtok(data + start, "\n");
	while (line) {
		fprintf(stderr, "kernel:   %s\n", line);
		line = strtok(NULL, "\n");
	}
	fflush(stderr);
}

// True when the file was there. `always`: print it even without a crash in
// it, and delete it afterwards (pstore's dmesg records exist only for a crash
// and would otherwise be reported at every start).
static bool report_kept(const char *path, bool always) {
	size_t len = 0;
	char *data = slurp(path, &len);
	if (!data) {
		return false;
	}
	if (len > 0 && (always || kept_shows_a_crash(data))) {
		print_tail(path, data, len);
	}
	free(data);
	if (always) {
		unlink(path);
	}
	return true;
}

static void report_kept_kernel_log(void) {
	bool found = false;

	DIR *dir = opendir("/sys/fs/pstore");
	if (dir) {
		struct dirent *de;
		while ((de = readdir(dir)) != NULL) {
			if (de->d_name[0] == '.') {
				continue;
			}
			char path[320];
			snprintf(path, sizeof(path), "/sys/fs/pstore/%.255s", de->d_name);
			found |= report_kept(path, strncmp(de->d_name, "dmesg-", 6) == 0);
		}
		closedir(dir);
	}
	found |= report_kept("/proc/last_kmsg", false);

	if (!found) {
		fprintf(stderr, "kernel: nothing kept from before this start (no pstore, no /proc/last_kmsg)\n");
	}
}

void logging_report_previous_run(void) {
	report_kept_kernel_log();

	char *buffer = malloc(KERNEL_BUFFER_BYTES);
	if (!buffer) {
		return;
	}

	// SYSLOG_ACTION_READ_ALL (3): the whole buffer without consuming it, so
	// this can run at every start and nothing else loses the messages.
	int got = klogctl(3, buffer, KERNEL_BUFFER_BYTES - 1);
	if (got <= 0) {
		// Not an error worth a line: a kernel built without the syscall, or a
		// build that reserves it for root, both land here.
		free(buffer);
		return;
	}
	buffer[got] = '\0';

	int said = 0;
	char *line = strtok(buffer, "\n");
	while (line && said < KERNEL_LINES_MAX) {
		if (kernel_line_interesting(line)) {
			if (said == 0) {
				printf("kernel: what the kernel said before this start --\n");
			}
			printf("kernel:   %s\n", line);
			said++;
		}
		line = strtok(NULL, "\n");
	}
	if (said > 0) {
		fflush(stdout);
	}

	free(buffer);
}
