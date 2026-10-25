#include "device_state.h"

#include "src/system/core/frameprof.h"
#include "src/system/core/utils.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/readahead.h"
#include "src/system/audio/replaygain.h"
#include "src/system/device/power.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/decode/growfile.h"
#include "src/system/device/system.h"
#include "src/system/library/library.h"
#include "src/system/library/metadata.h"
#include "src/system/playback/audiobook.h"
#include "src/system/playback/playhistory.h"
#include "src/system/playback/playlist.h"
#include "src/system/remote/dlna.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcaststate.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/radio.h"
#include "src/system/streaming/tidalcache.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

// Cache of the currently loaded track's metadata, refreshed whenever a new
// file is loaded via device_state_play_file(). Only ever touched from the
// LVGL/UI thread (same as the rest of player.c today), so no lock needed.
static song_metadata_t current_metadata;
static char current_metadata_file[512] = {0};

// ---------------------------------------------------------------------------
// "Remember track": where playback was, kept across a power cycle
//
// It lives here and not in the player screen for one reason: the two things
// that end a session -- the power menu and the automatic shutdown -- do not go
// anywhere near the interface, so a position noticed only by a UI poll would
// never be written by either of them.
//
// When it writes:
//
//   the file changed, or the state did (play, pause, stop) -- at once;
//
//   the position moved by more than REMEMBER_MOVE_SECS since the last write.
//   Without this a seek made while PAUSED is never written at all: nothing
//   about the state has changed and the ten-second timer only runs while
//   playing;
//
//   every REMEMBER_PERIOD_MS while playing, so a flat battery loses that much
//   and no more;
//
//   once when playback stops, so a queue that runs to its end does not come
//   back up to ten seconds short of where it finished.
//
// And the one thing it refuses to do: write a zero over a position it has just
// restored. A track brought back at boot goes through a moment where the
// decoder has opened the file and the seek has not landed yet, and the engine
// reports 0; a poll in that window would write 0 over the very position being
// restored. The latch below holds every write until the engine reports the
// position that was asked for, and lets go on its own after
// REMEMBER_RESTORE_GRACE_MS so a seek that never lands cannot silence the
// feature for the session.
// ---------------------------------------------------------------------------

#define REMEMBER_PERIOD_MS 10000
// With the screen dark the position is written every half minute instead: the
// card is woken for each write, and a crash then loses at most that much.
#define REMEMBER_PERIOD_DARK_MS 30000
#define REMEMBER_MOVE_SECS 1.0
#define REMEMBER_RESTORE_GRACE_MS 8000

// Everything the decision needs to remember between calls. In a struct, and
// the decision itself a pure function of it, so a bench can walk a whole
// session through the real rules without a decoder or a database anywhere near
// it (tools/test_remember.c).
typedef struct {
	char file[512];
	audio_status_t status;
	double pos;
	uint32_t saved_ms;
	bool have;
	double restore_pos; // what the boot restore asked for, -1 for nothing pending
	uint32_t restore_ms;
} remember_state_t;

static remember_state_t remember = {.pos = -1.0, .restore_pos = -1.0};

// Whether this sample is worth writing. Updates `st` when it says yes, so the
// caller has nothing to remember on its own.
static bool remember_should_write(remember_state_t *st, const char *file, audio_status_t status, double pos,
								  uint32_t now) {
	// The restore latch: hold everything until the engine reports the position
	// that was asked for, and let go on its own if it never does.
	if (st->restore_pos >= 0) {
		bool arrived = pos >= st->restore_pos - REMEMBER_MOVE_SECS;
		bool waited = (uint32_t)(now - st->restore_ms) > REMEMBER_RESTORE_GRACE_MS;
		if (!arrived && !waited) {
			return false;
		}
		st->restore_pos = -1.0;
	}

	bool same_file = st->have && strncmp(st->file, file, sizeof(st->file)) == 0;
	double moved = pos - st->pos;
	if (moved < 0) {
		moved = -moved;
	}

	bool changed = !same_file || status != st->status;
	// A position that moved while nothing was PLAYING is a seek: nobody else
	// can have moved it. A drag of the slider with the track paused changes no
	// state, and the ten-second timer only runs while the music does, so this is
	// the only rule that catches it.
	//
	// While playing it deliberately does NOT apply: the position moves by
	// itself, and writing every time it has moved a second is a write to the
	// card every second.
	bool drifted = same_file && st->pos >= 0 && status != AUDIO_STATUS_PLAYING && moved >= REMEMBER_MOVE_SECS;
	bool due = status == AUDIO_STATUS_PLAYING &&
			   (uint32_t)(now - st->saved_ms) >= (power_screen_is_on() ? REMEMBER_PERIOD_MS : REMEMBER_PERIOD_DARK_MS);

	if (!changed && !drifted && !due) {
		return false;
	}

	snprintf(st->file, sizeof(st->file), "%s", file);
	st->status = status;
	st->pos = pos;
	st->saved_ms = now;
	st->have = true;
	return true;
}

static uint32_t remember_now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static bool remember_enabled(void) { return config_get_int("player", "remember_track", 0) != 0; }

// A track this player must not offer to come back to: a live stream has no
// position, and a streaming cache file will not exist at the next boot.
static bool remember_worth_saving(const device_state_t *state) {
	return !state->live && state->current_file[0] && !qobuzcache_owns(state->current_file) &&
		   !tidalcache_owns(state->current_file);
}

void device_state_remember_restored(const char *path, double position) {
	snprintf(remember.file, sizeof(remember.file), "%s", path ? path : "");
	remember.status = AUDIO_STATUS_PAUSED;
	remember.pos = position;
	remember.saved_ms = remember_now_ms();
	remember.have = true;
	remember.restore_pos = position;
	remember.restore_ms = remember.saved_ms;
}

static void state_job_submit(const char *path, double position);

void device_state_remember_note(void) {
	if (!remember_enabled()) {
		return;
	}
	device_state_t state;
	device_state_get(&state);
	if (!remember_worth_saving(&state)) {
		return;
	}
	if (remember_should_write(&remember, state.current_file, state.status, state.progress_current_secs,
							  remember_now_ms())) {
		// A database transaction on the card: with the file changed it is due
		// on the first poll after every skip, so it goes to the writer thread.
		state_job_submit(state.current_file, state.progress_current_secs);
	}
}

// Where a book got to, written through on the way out: the power going off or
// the card going away. Not subject to "remember track", as the player's own
// saves are not. Nothing under a second, which is a book whose resume seek
// the engine has not reached yet rather than a place anyone stopped at.
static void flush_book_position(const device_state_t *state) {
	// And the podcast episode's, which is kept the same way.
	podcaststate_flush();
	if (state->live || !audiobook_is_playing() || state->progress_current_secs <= 1.0) {
		return;
	}
	audiobook_note_position(state->progress_current_secs, state->progress_total_secs, true);
}

static void queue_job_flush(void);

void device_state_remember_flush(void) {
	// The queue first: a write still waiting on its thread (queue_job_flush).
	queue_job_flush();
	device_state_t state;
	device_state_get(&state);
	flush_book_position(&state);
	if (!remember_enabled()) {
		return;
	}
	if (!remember_worth_saving(&state)) {
		return;
	}
	// The latch still applies: on the way out, a position the engine has not
	// reached yet is still the one that was asked for, and writing the zero it
	// is currently reporting would throw it away at the last moment.
	if (remember.restore_pos >= 0 && state.progress_current_secs < remember.restore_pos - REMEMBER_MOVE_SECS) {
		return;
	}
	remember.restore_pos = -1.0;
	library_playback_state_save(state.current_file, state.progress_current_secs);
	snprintf(remember.file, sizeof(remember.file), "%s", state.current_file);
	remember.status = state.status;
	remember.pos = state.progress_current_secs;
	remember.saved_ms = remember_now_ms();
	remember.have = true;
}

void device_state_get(device_state_t *out) {
	if (!out)
		return;

	// A radio has the DAC through audio_external_*, which audio.c's own
	// status does not know about -- so the live source is asked first and
	// answers for the whole now-playing picture when it is on.
	out->live = false;
	out->live_cover[0] = '\0';

	if (radio_is_active()) {
		radio_now_t now;
		radio_get_now(&now);

		out->live = true;
		// Connecting counts as playing: it is on, and the button that would
		// stop it should say so. A station that has been stopped reads as
		// stopped, and its button goes back to play.
		out->status = (now.playing || now.connecting) ? AUDIO_STATUS_PLAYING : AUDIO_STATUS_STOPPED;
		out->current_file[0] = '\0';
		out->progress_current_secs = 0;
		out->progress_total_secs = 0;
		out->stream_sample_rate = now.sample_rate;
		out->stream_channels = now.channels;
		snprintf(out->live_cover, sizeof(out->live_cover), "%s", now.cover_path);

		memset(&out->metadata, 0, sizeof(out->metadata));
		snprintf(out->metadata.title, sizeof(out->metadata.title), "%s", now.station);

		// The second line: what the stream says is on air, or -- before the
		// first frame has come out of it -- what the player is busy doing.
		// Opening a connection to a radio server takes a moment, and a blank
		// line for that moment reads as a station that does not work.
		if (now.error[0]) {
			// Whatever went wrong goes where the on-air title would be: it is
			// the line the user is already reading, and a station that simply
			// sits there silent is the worst way to say "this one does not
			// work here".
			snprintf(out->metadata.artist, sizeof(out->metadata.artist), "%s", now.error);
		} else if (now.title[0]) {
			snprintf(out->metadata.artist, sizeof(out->metadata.artist), "%s", now.title);
		} else if (now.connecting) {
			snprintf(out->metadata.artist, sizeof(out->metadata.artist), "%s", tr("connecting"));
		}
		out->metadata.has_tags = true;
	} else {
		out->status = audio_get_status();
		audio_get_current_file(out->current_file, sizeof(out->current_file));
		audio_get_progress(&out->progress_current_secs, &out->progress_total_secs);

		// While a skip button is held the position shown is the one the thumb
		// has run to, not the one the decoder is at: the seek itself happens
		// when the button comes up (see the scrub section below).
		double scrub = 0;
		if (out->progress_total_secs > 0 && device_state_scrub_active(&scrub)) {
			out->progress_current_secs = scrub;
		}
		audio_get_stream_info(&out->stream_sample_rate, &out->stream_channels);

		// Starting a radio stops local playback, which leaves audio.c with no
		// file at all -- so when the radio is switched off the player would
		// come back to a blank screen instead of to the track it was on.
		// The loaded track is still known here; report it, stopped. Pressing
		// play then restarts it, which is what the button already does from a
		// stopped state.
		if (!out->current_file[0] && current_metadata_file[0]) {
			snprintf(out->current_file, sizeof(out->current_file), "%s", current_metadata_file);
		}

		out->metadata = current_metadata;
	}

	char *battery = read_battery_percent();
	strncpy(out->battery_percent, battery ? battery : "!!", sizeof(out->battery_percent) - 1);
	out->battery_percent[sizeof(out->battery_percent) - 1] = '\0';
	out->battery_charging = read_battery_charging();

	out->volume = get_volume_percent();
}

const song_metadata_t *device_state_loaded_metadata(const char **file_out) {
	if (file_out) {
		*file_out = current_metadata_file;
	}
	return &current_metadata;
}

static void playlist_resync_to_current(void);

// ---------------------------------------------------------------------------
// Mirroring the queue on disk
//
// Without this a power cycle leaves the player with the remembered track and no
// queue around it, so the first next/prev lands back on the same song. The
// queue is written out with every track change, but the list itself only when
// it actually changed (playlist_revision): rewriting a few thousand rows on a
// class-10 card for every track would be felt.
// ---------------------------------------------------------------------------

static unsigned saved_queue_revision;
static bool queue_ever_saved;

// The writing itself happens on a thread of its own. It is a transaction on
// the card, and on the Blaze it took up to 370 ms -- on the interface thread,
// at the moment a row was tapped, before the player could slide in. What is
// written is gathered here first (the queue is in memory and cheap to copy);
// the thread only hands it to the library. Each job is the whole state, so a
// newer one simply replaces a job still waiting.
typedef enum { QUEUE_JOB_INDEX, QUEUE_JOB_QUERY, QUEUE_JOB_LIST } queue_job_kind_t;

typedef struct {
	queue_job_kind_t kind;
	int current;			  // INDEX: the position; QUERY: the entry; LIST: the index
	library_index_spec_t spec; // QUERY
	char **paths;			  // QUERY: the appended tracks; LIST: the queue
	int *slots;				  // QUERY
	int count;
	bool custom; // LIST
} queue_job_t;

// The writer waits this long after the newest job before writing it. A track
// is starting at that moment, and the playback thread saves to the library as
// it closes one track and opens the next -- with the audio mutex held, which
// play then waits on. A queue write in the same instant held the database for
// a few hundred milliseconds, and the tap that started the track waited for
// all of it. Writing once things have settled also lets a burst of taps end
// in a single write. The way out does not wait (queue_job_flush).
#define QUEUE_WRITE_DELAY_MS 1500

static pthread_mutex_t queue_job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_job_cond = PTHREAD_COND_INITIALIZER;
static queue_job_t *queue_job_waiting;
// The "remember track" position rides on the same thread and the same settling
// delay: both are transactions on the one database, and written back to back
// they cost one burst instead of two.
static char state_job_path[512];
static double state_job_pos;
static bool state_job_waiting;
static unsigned job_serial; // bumped by every submit, queue or state
static bool queue_job_running;
static bool queue_job_hurry; // set by queue_job_flush: no settling delay
static bool queue_writer_started;

static void queue_job_free(queue_job_t *job) {
	if (!job) {
		return;
	}
	for (int i = 0; job->paths && i < job->count; i++) {
		free(job->paths[i]);
	}
	free(job->paths);
	free(job->slots);
	free(job);
}

static void queue_job_write(const queue_job_t *job) {
	switch (job->kind) {
	case QUEUE_JOB_INDEX:
		library_queue_save_index(job->current);
		break;
	case QUEUE_JOB_QUERY:
		library_queue_save_query(&job->spec, job->current, (const char *const *)job->paths, job->slots, job->count);
		break;
	case QUEUE_JOB_LIST:
		library_queue_save((const char *const *)job->paths, job->count, job->current, job->custom);
		break;
	}
}

static void *queue_writer(void *arg) {
	(void)arg;
	thread_be_low_priority("queue writer");
	pthread_mutex_lock(&queue_job_lock);
	for (;;) {
		while (!queue_job_waiting && !state_job_waiting) {
			pthread_cond_wait(&queue_job_cond, &queue_job_lock);
		}
		// Settle: until QUEUE_WRITE_DELAY_MS have passed with no newer job, or
		// until the way out asks for it now.
		for (;;) {
			unsigned seen = job_serial;
			struct timespec until;
			clock_gettime(CLOCK_REALTIME, &until);
			until.tv_sec += QUEUE_WRITE_DELAY_MS / 1000;
			until.tv_nsec += (long)(QUEUE_WRITE_DELAY_MS % 1000) * 1000000L;
			if (until.tv_nsec >= 1000000000L) {
				until.tv_sec++;
				until.tv_nsec -= 1000000000L;
			}
			while (!queue_job_hurry && job_serial == seen &&
				   pthread_cond_timedwait(&queue_job_cond, &queue_job_lock, &until) == 0) {
			}
			if (queue_job_hurry || job_serial == seen) {
				break; // asked for now, or quiet for the whole delay
			}
		}
		queue_job_t *job = queue_job_waiting;
		queue_job_waiting = NULL;
		char state_path[sizeof(state_job_path)];
		double state_pos = state_job_pos;
		bool have_state = state_job_waiting;
		if (have_state) {
			memcpy(state_path, state_job_path, sizeof(state_path));
			state_job_waiting = false;
		}
		queue_job_running = true;
		pthread_mutex_unlock(&queue_job_lock);

		if (job) {
			queue_job_write(job);
			queue_job_free(job);
		}
		if (have_state) {
			library_playback_state_save(state_path, state_pos);
		}

		pthread_mutex_lock(&queue_job_lock);
		queue_job_running = false;
		pthread_cond_broadcast(&queue_job_cond);
	}
	return NULL;
}

static bool queue_writer_ensure(void) {
	if (!queue_writer_started) {
		pthread_t thread;
		queue_writer_started = pthread_create(&thread, NULL, queue_writer, NULL) == 0;
		if (queue_writer_started) {
			pthread_detach(thread);
		}
	}
	return queue_writer_started;
}

// Hands a job to the writer, or writes it here when there is no writer.
static void queue_job_submit(queue_job_t *job) {
	if (!queue_writer_ensure()) {
		queue_job_write(job);
		queue_job_free(job);
		return;
	}
	pthread_mutex_lock(&queue_job_lock);
	queue_job_free(queue_job_waiting); // superseded
	queue_job_waiting = job;
	job_serial++;
	pthread_cond_broadcast(&queue_job_cond);
	pthread_mutex_unlock(&queue_job_lock);
}

// The same for the position "remember track" keeps. Only the newest is kept.
static void state_job_submit(const char *path, double position) {
	if (!queue_writer_ensure()) {
		library_playback_state_save(path, position);
		return;
	}
	pthread_mutex_lock(&queue_job_lock);
	snprintf(state_job_path, sizeof(state_job_path), "%s", path);
	state_job_pos = position;
	state_job_waiting = true;
	job_serial++;
	pthread_cond_broadcast(&queue_job_cond);
	pthread_mutex_unlock(&queue_job_lock);
}

// Returns once nothing is waiting to be written: for the way out, where the
// queue has to be on the card before the power goes.
static void queue_job_flush(void) {
	pthread_mutex_lock(&queue_job_lock);
	queue_job_hurry = true;
	pthread_cond_broadcast(&queue_job_cond);
	while (queue_job_waiting || state_job_waiting || queue_job_running) {
		pthread_cond_wait(&queue_job_cond, &queue_job_lock);
	}
	queue_job_hurry = false;
	pthread_mutex_unlock(&queue_job_lock);
}

// ---------------------------------------------------------------------------
// Skip prefetch (device_state.h). The tags of a track are a read from the card
// -- 25 to 55 ms measured on the device, on the interface thread, at every
// skip -- and so is the starred flag. Both are read here, for the two tracks a
// skip can land on, a moment after the current one is up, on a thread below the
// interface. What the thread stores is checked again when it is used: the file
// must still have the size and time it had, the answer is not older than
// PREFETCH_MAX_AGE_MS (a sidecar or a cue sheet is not covered by the stat),
// and for the star, no favourite may have changed since.
// ---------------------------------------------------------------------------
#define PREFETCH_SLOTS 4       // next, previous, and one more each way: a burst of skips keeps hitting
#define PREFETCH_SETTLE_MS 150 // a burst of skips reads only where it ends
#define PREFETCH_MAX_AGE_MS 120000

typedef struct {
	char path[512];
	song_metadata_t meta;
	bool used;
	bool fav;
	unsigned fav_gen;
	off_t size;
	time_t mtime;
	uint32_t stamp_ms;
} prefetch_slot_t;

static pthread_mutex_t prefetch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t prefetch_cond = PTHREAD_COND_INITIALIZER;
static prefetch_slot_t prefetch_slots[PREFETCH_SLOTS];
static char prefetch_wanted[PREFETCH_SLOTS][512];
static unsigned prefetch_serial;
static bool prefetch_pending;
static bool prefetch_started;

// Call with prefetch_lock held.
static prefetch_slot_t *prefetch_find(const char *path) {
	uint32_t now = remember_now_ms();
	for (int i = 0; i < PREFETCH_SLOTS; i++) {
		prefetch_slot_t *slot = &prefetch_slots[i];
		if (slot->used && strcmp(slot->path, path) == 0 && (uint32_t)(now - slot->stamp_ms) < PREFETCH_MAX_AGE_MS) {
			return slot;
		}
	}
	return NULL;
}

// Files that are still being written (a streaming cache) or that are not
// files at all must never be read ahead of time.
static bool prefetch_allowed(const char *path) {
	return path[0] == '/' && !qobuzcache_owns(path) && !tidalcache_owns(path) && !podcastcache_is_episode(path);
}

static void *prefetch_thread(void *arg) {
	(void)arg;
	thread_be_low_priority("skip prefetch");
	prefetch_slot_t *fresh = malloc(sizeof(*fresh));
	pthread_mutex_lock(&prefetch_lock);
	for (;;) {
		while (!prefetch_pending || !fresh) {
			pthread_cond_wait(&prefetch_cond, &prefetch_lock);
		}
		for (;;) { // settle
			unsigned seen = prefetch_serial;
			struct timespec until;
			clock_gettime(CLOCK_REALTIME, &until);
			until.tv_nsec += (long)PREFETCH_SETTLE_MS * 1000000L;
			until.tv_sec += until.tv_nsec / 1000000000L;
			until.tv_nsec %= 1000000000L;
			while (prefetch_serial == seen && pthread_cond_timedwait(&prefetch_cond, &prefetch_lock, &until) == 0) {
			}
			if (prefetch_serial == seen) {
				break;
			}
		}
		char wanted[PREFETCH_SLOTS][512];
		memcpy(wanted, prefetch_wanted, sizeof(wanted));
		prefetch_pending = false;

		unsigned asked = prefetch_serial;
		for (int i = 0; i < PREFETCH_SLOTS; i++) {
			if (prefetch_serial != asked) {
				break; // another skip: the new request settles and starts over
			}
			if (!wanted[i][0] || !prefetch_allowed(wanted[i])) {
				continue;
			}
			prefetch_slot_t *have = prefetch_find(wanted[i]);
			if (have) {
				if (have != &prefetch_slots[i]) { // the other direction's answer, now this one's
					prefetch_slot_t swap = *have;
					*have = prefetch_slots[i];
					prefetch_slots[i] = swap;
				}
				continue;
			}
			pthread_mutex_unlock(&prefetch_lock);

			struct stat st;
			bool ok = stat(wanted[i], &st) == 0 && S_ISREG(st.st_mode);
			if (ok) {
				snprintf(fresh->path, sizeof(fresh->path), "%.511s", wanted[i]);
				fresh->size = st.st_size;
				fresh->mtime = st.st_mtime;
				fresh->fav_gen = library_favourites_generation(); // before the read, so a change in between shows
				metadata_read(fresh->path, &fresh->meta);
				fresh->fav = library_fav_contains(fresh->path);
				fresh->stamp_ms = remember_now_ms();
				fresh->used = true;
			}

			pthread_mutex_lock(&prefetch_lock);
			if (ok) {
				prefetch_slots[i] = *fresh;
			}
		}
	}
	return NULL;
}

void device_state_prefetch(const char *const *paths, int count) {
	pthread_mutex_lock(&prefetch_lock);
	if (!prefetch_started) {
		pthread_t thread;
		prefetch_started = pthread_create(&thread, NULL, prefetch_thread, NULL) == 0;
		if (prefetch_started) {
			pthread_detach(thread);
		}
	}
	if (prefetch_started) {
		for (int i = 0; i < PREFETCH_SLOTS; i++) {
			const char *path = paths && i < count && paths[i] ? paths[i] : "";
			snprintf(prefetch_wanted[i], sizeof(prefetch_wanted[i]), "%s", path);
		}
		prefetch_serial++;
		prefetch_pending = true;
		pthread_cond_broadcast(&prefetch_cond);
	}
	pthread_mutex_unlock(&prefetch_lock);
}

bool device_state_prefetch_peek(const char *path, song_metadata_t *out) {
	pthread_mutex_lock(&prefetch_lock);
	prefetch_slot_t *slot = path && path[0] ? prefetch_find(path) : NULL;
	if (slot) {
		*out = slot->meta;
	}
	pthread_mutex_unlock(&prefetch_lock);
	return slot != NULL;
}

// The tags of `path` when they were read ahead and are still good.
static bool prefetch_take_meta(const char *path, song_metadata_t *out) {
	off_t size;
	time_t mtime;
	pthread_mutex_lock(&prefetch_lock);
	prefetch_slot_t *slot = prefetch_find(path);
	if (slot) {
		*out = slot->meta;
		size = slot->size;
		mtime = slot->mtime;
	}
	pthread_mutex_unlock(&prefetch_lock);
	if (!slot) {
		return false;
	}
	struct stat st;
	return stat(path, &st) == 0 && st.st_size == size && st.st_mtime == mtime;
}

bool device_state_prefetched_fav(const char *path, bool *starred) {
	bool good = false;
	pthread_mutex_lock(&prefetch_lock);
	prefetch_slot_t *slot = path && path[0] ? prefetch_find(path) : NULL;
	if (slot && slot->fav_gen == library_favourites_generation()) {
		*starred = slot->fav;
		good = true;
	}
	pthread_mutex_unlock(&prefetch_lock);
	return good;
}

// Writes a library-backed queue down as its query. The position is the entry as
// stored, not where it sits in the order being played: a shuffled queue is
// dealt again when it comes back, and the old deal's position would land on an
// unrelated track. Anything "add to queue" put there afterwards is not in the
// query and goes along beside it.
static void queue_save_query_form(const library_index_spec_t *spec) {
	int extra_count = playlist_appended_count();
	char **extra = NULL;
	int *slots = NULL;
	int filled = 0;

	if (extra_count > 0) {
		extra = calloc((size_t)extra_count, sizeof(*extra));
		slots = calloc((size_t)extra_count, sizeof(*slots));
		if (extra && slots) {
			// Where each one sits, taken before the loop: one walk of the order
			// for all of them rather than a search each.
			playlist_appended_slots(slots, extra_count);
			for (int i = 0; i < extra_count; i++) {
				char path[512];
				if (!playlist_appended_at(i, path, sizeof(path))) {
					continue;
				}
				extra[filled] = strdup(path);
				if (!extra[filled]) {
					break;
				}
				slots[filled] = slots[i]; // a skipped entry must not leave its slot behind
				filled++;
			}
		}
	}

	queue_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		for (int i = 0; i < filled; i++) {
			free(extra[i]);
		}
		free(extra);
		free(slots);
		return;
	}
	job->kind = QUEUE_JOB_QUERY;
	job->spec = *spec;
	job->current = (int)playlist_current_entry();
	job->paths = extra;
	job->slots = slots;
	job->count = filled;
	queue_job_submit(job);
}

static void queue_persist(void) {
	int count = playlist_count();
	int current = playlist_current_index();
	if (count <= 0 || current < 0) {
		return;
	}

	unsigned revision = playlist_revision();
	if (queue_ever_saved && revision == saved_queue_revision) {
		// Only the position moved. Which of the two forms holds the queue
		// decides where that goes.
		library_index_spec_t moved;
		if (playlist_library_spec(&moved)) {
			queue_save_query_form(&moved);
		} else {
			queue_job_t *job = calloc(1, sizeof(*job));
			if (job) {
				job->kind = QUEUE_JOB_INDEX;
				job->current = current;
				queue_job_submit(job);
			}
		}
		return;
	}

	// A queue that is a library list is written down as the query behind it.
	// The rows would be the same list at ten thousand times the cost, and on a
	// large library the write alone would outlast the track.
	library_index_spec_t spec;
	if (playlist_library_spec(&spec)) {
		queue_save_query_form(&spec);
		saved_queue_revision = revision;
		queue_ever_saved = true;
		return;
	}

	char **list = calloc((size_t)count, sizeof(*list));
	if (!list) {
		return;
	}

	int filled = 0;
	bool from_stream = false;
	for (int i = 0; i < count; i++) {
		char path[512];
		if (!playlist_path_at(i, path, sizeof(path))) {
			continue;
		}
		// A streaming queue is not saved: the files are transient (the caches
		// empty themselves) and the next boot would restore a queue of paths
		// that no longer exist, mixed in with the user's own tracks. Both
		// services, for the same reason.
		if (qobuzcache_owns(path) || tidalcache_owns(path)) {
			from_stream = true;
			break;
		}
		list[filled] = strdup(path);
		if (!list[filled]) {
			break;
		}
		filled++;
	}

	if (filled > 0 && !from_stream) {
		queue_job_t *job = calloc(1, sizeof(*job));
		if (job) {
			job->kind = QUEUE_JOB_LIST;
			job->paths = list; // the job owns it now
			job->count = filled;
			job->current = current;
			job->custom = playlist_is_custom();
			queue_job_submit(job);
			saved_queue_revision = revision;
			queue_ever_saved = true;
			return;
		}
	}

	for (int i = 0; i < filled; i++) {
		free(list[i]);
	}
	free(list);
}

void device_state_queue_changed(void) { queue_persist(); }

// One per streaming service plus headroom: four is already twice as many as
// exist.
#define PREPARE_CB_MAX 4
static device_state_prepare_cb prepare_cbs[PREPARE_CB_MAX];
static int prepare_cb_count;

void device_state_add_prepare_cb(device_state_prepare_cb cb) {
	if (!cb || prepare_cb_count >= PREPARE_CB_MAX) {
		return;
	}
	// Registering twice would mean preparing the same track twice. The cost is
	// one walk over four entries, once at start-up.
	for (int i = 0; i < prepare_cb_count; i++) {
		if (prepare_cbs[i] == cb) {
			return;
		}
	}
	prepare_cbs[prepare_cb_count++] = cb;
}

// Defined with the rest of the scrub state further down.
static void scrub_cancel(void);

// Where playback was when the storage under it went away: the card pulled out,
// or the card exported to a computer. Both stop the engine, and from the
// outside a stop is a stop -- pressing play afterwards started the track again
// from zero, whatever it had been paused at. See device_state_note_storage_gone().
static char interrupted_file[512];
static double interrupted_pos = -1.0;

void device_state_note_storage_gone(void) {
	interrupted_file[0] = '\0';
	interrupted_pos = -1.0;

	device_state_t state;
	device_state_get(&state);
	// The databases on the card close right after this.
	flush_book_position(&state);
	// A second in is not a position worth coming back to, and a live stream has
	// none at all.
	if (state.live || !state.current_file[0] || state.progress_current_secs <= 1.0) {
		return;
	}

	snprintf(interrupted_file, sizeof(interrupted_file), "%s", state.current_file);
	interrupted_pos = state.progress_current_secs;
}

// A book plays at the audiobook speed, an episode at the podcast speed, and
// anything else at 1.0. Set at every track change and not only from the
// pop-over, so a song that follows a book at 1.5x is not also played at 1.5x.
static void apply_track_speed(void) {
	if (audiobook_is_playing()) {
		audio_set_speed(audiobook_speed());
	} else if (podcastcache_is_episode(current_metadata_file)) {
		audio_set_speed(podcast_speed());
	} else {
		audio_set_speed(1.0);
	}
}

// The file the engine was last started on. A podcast episode picks up where it
// was left only when it is started afresh: the same file started again by the
// player itself -- repeat-one, a seek after the end -- goes back to its
// beginning. A tap in a list clears this first.
static char started_file[512];

// The history (playhistory.h) is told about every track that starts, and about
// the one it replaces. A track started again where it already is -- the same
// file picked up at a position, as a book or an interrupted track is, or
// restarted to be sought in -- has not been left.
static bool history_same_listen;

static void history_leave_for(const char *next, double position) {
	bool same = next && strcmp(next, current_metadata_file) == 0 && (position > 0 || history_same_listen);
	history_same_listen = false;
	if (!same) {
		playhistory_left();
	}
}

// `restored`: put back by a reboot, paused, and not played again until play is
// pressed.
static void note_started(bool restored) {
	snprintf(started_file, sizeof(started_file), "%s", current_metadata_file);
	bool kept_out = audiobook_is_playing() || podcastcache_is_episode(current_metadata_file) || dlna_owns_path(current_metadata_file);
	playhistory_started(current_metadata_file, current_metadata.title, metadata_shown_artist(&current_metadata), kept_out, restored);
}

// Where a podcast episode should start when nobody said: where it was left.
// Not on a file still coming down -- the point may not have arrived, and a seek
// past what has arrived is refused rather than waited for; the podcast page
// waits for it itself before starting one (podcastpage.c).
static double podcast_resume(const char *filepath) {
	if (!podcastcache_is_episode(filepath) || strcmp(started_file, filepath) == 0 || growfile_is_growing(filepath)) {
		return -1.0;
	}
	double at = podcaststate_resume_point(filepath);
	if (at > 0) {
		fprintf(stderr, "podcast: '%s' picks up at %.0f s\n", filepath, at);
		return at;
	}
	return -1.0;
}

// Loads metadata for `filepath` and starts playback, without touching the
// folder queue. Shared by fresh selections, queue advances, and replays.
//
// `position` is where to start; below zero means the beginning, which is what
// every caller but the resume below wants.
// Profiling marks for the steps of a track change, which runs on the
// interface thread right under the finger (frameprof.h).
static const char *const PROF_META = "play_metadata_read";
static const char *const PROF_BOOK = "play_audiobook_check";
static const char *const PROF_RG = "play_replaygain";
static const char *const PROF_START = "play_audio_start";
static const char *const PROF_PERSIST = "play_queue_persist";

static void load_and_play_at(const char *filepath, double position) {
	// A position picked on the outgoing track means nothing on this one, and
	// the track can change under a held button -- the queue advances on its
	// own when the outgoing one ends.
	scrub_cancel();
	history_leave_for(filepath, position);

	// A file and a stream cannot both have the output. Starting a track ends
	// the radio outright -- not merely stops it -- because from here on the
	// player belongs to the track.
	radio_clear();

	// Callers may replay the already-loaded track by passing current_metadata_file
	// itself, so guard against copying a buffer onto itself.
	if (filepath != current_metadata_file) {
		strncpy(current_metadata_file, filepath, sizeof(current_metadata_file) - 1);
		current_metadata_file[sizeof(current_metadata_file) - 1] = '\0';
	}

	frameprof_begin(PROF_META);
	if (!prefetch_take_meta(current_metadata_file, &current_metadata)) {
		metadata_read(current_metadata_file, &current_metadata);
	}
	frameprof_end(PROF_META);
	// Whether the loaded file is an audiobook is decided here, once per track
	// change, and everything that dresses the player for one reads the answer
	// from it. Every route by which the track can change passes through one of
	// these calls.
	frameprof_begin(PROF_BOOK);
	audiobook_track_changed(current_metadata_file);
	frameprof_end(PROF_BOOK);
	// The ReplayGain the tags asked for, worked out once per track.
	frameprof_begin(PROF_RG);
	replaygain_load(&current_metadata);
	apply_track_speed();
	frameprof_end(PROF_RG);

	// The file may not be there yet: with Qobuz and Tidal the queue holds the
	// whole album and playback can reach a track before its download does. The
	// registered callback takes over and restarts this once the file is ready;
	// stop here rather than hand the decoder a path to nothing.
	for (int i = 0; i < prepare_cb_count; i++) {
		if (!prepare_cbs[i](current_metadata_file)) {
			queue_persist();
			return;
		}
	}

	if (position <= 0) {
		position = podcast_resume(current_metadata_file);
	}
	frameprof_begin(PROF_START);
	if (position > 0) {
		audio_play_at(current_metadata_file, position);
	} else {
		audio_play(current_metadata_file);
	}
	frameprof_end(PROF_START);
	note_started(false);

	// Any track load supersedes the note: it belongs to the one press that
	// follows the storage coming back.
	interrupted_file[0] = '\0';
	interrupted_pos = -1.0;

	frameprof_begin(PROF_PERSIST);
	queue_persist();
	frameprof_end(PROF_PERSIST);
}

static void load_and_play(const char *filepath) { load_and_play_at(filepath, -1.0); }

// The same without starting: the track comes up paused at its beginning, and
// play starts it. The queue is left as it is.
static void load_paused(const char *filepath) {
	scrub_cancel();
	history_leave_for(filepath, -1.0);
	radio_clear();

	if (filepath != current_metadata_file) {
		snprintf(current_metadata_file, sizeof(current_metadata_file), "%s", filepath);
	}
	metadata_read(current_metadata_file, &current_metadata);
	audiobook_track_changed(current_metadata_file);
	replaygain_load(&current_metadata);
	apply_track_speed();

	audio_play_paused(current_metadata_file, 0);
	note_started(false);
	audiobook_suppress_rewind_once();

	interrupted_file[0] = '\0';
	interrupted_pos = -1.0;

	queue_persist();
}

// The boot-time "remember track" restore: the same folder queue and metadata a
// tap on the file would build, but the track comes up paused at `position`
// instead of playing.
void device_state_restore_file(const char *filepath, double position) {
	// Same reason as load_and_play: this changes the track too.
	scrub_cancel();

	char folder[512];
	strncpy(folder, filepath, sizeof(folder) - 1);
	folder[sizeof(folder) - 1] = '\0';

	const char *filename = filepath;
	char *slash = strrchr(folder, '/');
	if (slash) {
		*slash = '\0';
		filename = slash + 1;
	}

	playlist_load_folder(folder, filename);

	strncpy(current_metadata_file, filepath, sizeof(current_metadata_file) - 1);
	current_metadata_file[sizeof(current_metadata_file) - 1] = '\0';
	metadata_read(current_metadata_file, &current_metadata);
	// Whether the loaded file is an audiobook is decided here, once per track
	// change, and everything that dresses the player for one reads the answer
	// from it. Every route by which the track can change passes through one of
	// these calls.
	audiobook_track_changed(current_metadata_file);
	// The ReplayGain the tags asked for, worked out once per track.
	replaygain_load(&current_metadata);
	apply_track_speed();

	audio_play_paused(current_metadata_file, position);
	note_started(true);
	device_state_remember_restored(current_metadata_file, position);

	queue_persist();
}

// The record the restored queue is, put back where playlist_clear() wiped it.
// Both restores go through here: without it the queue comes back and the label
// does not, and "play albums back to back" stays dead until a record is started
// by hand.
static void restore_queue_album(void) {
	char album[256];
	if (!library_queue_load_album(album, (int)sizeof(album))) {
		album[0] = '\0';
	}
	// Noted before it is set: the label goes back as the card already has it.
	playlist_note_album_restored(album);
	if (album[0]) {
		playlist_set_album(album);
		char shown[sizeof(album)];
		library_album_title(album, shown, sizeof(shown));
		fprintf(stderr, "queue: the restored queue is the record \"%s\"\n", shown);
	}
}

// The same restore, but with the queue that was saved next to the track, so
// next and prev work from the first press.
void device_state_restore_index(library_index_t *ix, int start_index, const char *const *extra,
								const int *extra_slots, int extra_count, const char *filepath, double position) {
	// Same reason as load_and_play: this changes the track too.
	scrub_cancel();

	if (!playlist_load_index(ix, start_index)) {
		device_state_restore_file(filepath, position);
		return;
	}

	// Whatever "add to queue" had put into the list. Restored before the track
	// is located, so a remembered position inside one of them still resolves.
	//
	// Each goes back to the slot it was written down at. playlist_insert_next()
	// cannot be used for that: it can only insert after the current track, which
	// would lift tracks played hours ago back in front of playback. They arrive
	// already sorted by slot, which is what makes inserting them one after
	// another land each in the right place -- every insert shifts the rest up by
	// one.
	for (int i = 0; i < extra_count; i++) {
		if (!extra || !extra[i] || !extra[i][0]) {
			continue;
		}
		int slot = extra_slots ? extra_slots[i] : -1;
		if (slot < 0) {
			playlist_insert_next(extra[i]); // saved before the slot was written down
		} else {
			playlist_insert_at(extra[i], slot);
		}
	}

	// Prefer the remembered track over the saved position: the two agree unless
	// the library was rescanned since, and then the track is what the user last
	// saw while the position is a number about a list that has moved.
	//
	// When it cannot be found the saved offset goes with it: seeking an
	// unrelated track to three minutes in is worse than starting one at the top.
	if (filepath && filepath[0] && !playlist_locate(filepath)) {
		position = 0;
	}

	char path[512];
	if (!playlist_current_path(path, sizeof(path))) {
		device_state_restore_file(filepath, position);
		return;
	}

	strncpy(current_metadata_file, path, sizeof(current_metadata_file) - 1);
	current_metadata_file[sizeof(current_metadata_file) - 1] = '\0';
	metadata_read(current_metadata_file, &current_metadata);
	audiobook_track_changed(current_metadata_file);
	replaygain_load(&current_metadata);
	apply_track_speed();

	// The queue just loaded is the saved one: do not write it straight back.
	saved_queue_revision = playlist_revision();
	queue_ever_saved = true;
	restore_queue_album();

	audio_play_paused(current_metadata_file, position);
	note_started(true);
	device_state_remember_restored(current_metadata_file, position);
}

void device_state_restore_list(const char *const *list, int count, int start_index, bool custom, const char *filepath,
							   double position) {
	// Same reason as load_and_play: this changes the track too.
	scrub_cancel();

	if (!list || count <= 0) {
		device_state_restore_file(filepath, position);
		return;
	}

	playlist_load_paths(list, count, start_index);
	// playlist_load_paths() always marks the queue custom; a queue that was a
	// folder queue before the reboot has to stay one, or it would never be
	// refreshed from the disk again.
	playlist_set_custom(custom);

	// Prefer the remembered track over the saved index: the two agree unless
	// the position row was written after the list, and the track is what the
	// user last saw.
	if (filepath && filepath[0]) {
		playlist_locate(filepath);
	}

	char path[512];
	if (!playlist_current_path(path, sizeof(path))) {
		device_state_restore_file(filepath, position);
		return;
	}

	strncpy(current_metadata_file, path, sizeof(current_metadata_file) - 1);
	current_metadata_file[sizeof(current_metadata_file) - 1] = '\0';
	metadata_read(current_metadata_file, &current_metadata);
	// Whether the loaded file is an audiobook is decided here, once per track
	// change, and everything that dresses the player for one reads the answer
	// from it. Every route by which the track can change passes through one of
	// these calls.
	audiobook_track_changed(current_metadata_file);
	// The ReplayGain the tags asked for, worked out once per track.
	replaygain_load(&current_metadata);
	apply_track_speed();

	// The queue just loaded is the saved one: do not write it straight back.
	saved_queue_revision = playlist_revision();
	queue_ever_saved = true;
	restore_queue_album();

	audio_play_paused(current_metadata_file, position);
	note_started(true);
	device_state_remember_restored(current_metadata_file, position);
}

// The same, but starting somewhere other than the beginning: an audiobook
// resumed where it was left. Split out rather than folded into a flag on
// load_and_play() because only a book has a position to come back to, and the
// difference should be visible at the call site.
void device_state_play_file_at(const char *filepath, double position) {
	// Same reason as load_and_play: this changes the track too.
	scrub_cancel();

	char folder[512];
	strncpy(folder, filepath, sizeof(folder) - 1);
	folder[sizeof(folder) - 1] = '\0';

	const char *filename = filepath;
	char *slash = strrchr(folder, '/');
	if (slash) {
		*slash = '\0';
		filename = slash + 1;
	}

	playlist_load_folder(folder, filename);

	history_leave_for(filepath, position);
	radio_clear();
	if (filepath != current_metadata_file) {
		strncpy(current_metadata_file, filepath, sizeof(current_metadata_file) - 1);
		current_metadata_file[sizeof(current_metadata_file) - 1] = '\0';
	}
	metadata_read(current_metadata_file, &current_metadata);
	audiobook_track_changed(current_metadata_file);
	// The ReplayGain the tags asked for, worked out once per track.
	replaygain_load(&current_metadata);
	apply_track_speed();

	audio_play_at(current_metadata_file, position);
	note_started(false);

	queue_persist();
}

void device_state_play_file(const char *filepath) {
	// The queue is built from the selected file's directory, so playback can
	// continue past this track.
	char folder[512];
	strncpy(folder, filepath, sizeof(folder) - 1);
	folder[sizeof(folder) - 1] = '\0';

	const char *filename = filepath;
	char *slash = strrchr(folder, '/');
	if (slash) {
		*slash = '\0';		   // terminate the folder portion
		filename = slash + 1;  // the bare file name within the folder
	}

	playlist_load_folder(folder, filename);

	load_and_play(filepath);
}

// Starts playback from an explicit list of paths: the queue becomes exactly
// that list, so next/prev walk it. What the library index pages call.
bool device_state_play_index(library_index_t *ix, int start_index) {
	if (!playlist_load_index(ix, start_index)) {
		return false;
	}

	char path[512];
	if (!playlist_current_path(path, sizeof(path))) {
		return false;
	}
	load_and_play(path);
	return true;
}

void device_state_play_list(const char *const *list, int count, int start_index) {
	if (!list || count <= 0) {
		return;
	}
	playlist_load_paths(list, count, start_index);

	char path[512];
	if (playlist_current_path(path, sizeof(path))) {
		load_and_play(path);
	}
}

// The same for a queue that already carries its own order: a podcast's
// episodes. See playlist_load_paths_ordered().
void device_state_play_list_ordered(const char *const *list, int count, int start_index) { device_state_play_list_ordered_at(list, count, start_index, -1.0); }

void device_state_play_list_ordered_at(const char *const *list, int count, int start_index, double position) {
	if (!list || count <= 0) {
		return;
	}
	playlist_load_paths_ordered(list, count, start_index);

	// Chosen from a list: an episode picks up where it was left even when it is
	// the one already loaded.
	started_file[0] = '\0';
	char path[512];
	if (playlist_current_path(path, sizeof(path))) {
		load_and_play_at(path, position);
	}
}

// Jumps to a specific entry of the current queue: the queue page's tap.
bool device_state_play_queue_index(int index) {
	if (!playlist_set_current(index)) {
		return false;
	}
	started_file[0] = '\0';
	char path[512];
	if (!playlist_current_path(path, sizeof(path))) {
		return false;
	}
	load_and_play(path);
	return true;
}

// ---------------------------------------------------------------------------
// Consecutive album playback
//
// Normally a queue that runs out simply stops. With this on, the next album
// starts instead: the next one alphabetically, wrapping round at Z, which is
// the order the albums page lists them in, so what follows is where the finger
// would have gone anyway.
//
// It only fires on a queue that is an album (see playlist_album): a folder
// queue or a hand-made list running out is not a record ending.
// ---------------------------------------------------------------------------

bool device_state_album_chaining(void) { return config_get_int("player", "album_chaining", 0) != 0; }

void device_state_set_album_chaining(bool on) {
	config_set_int("player", "album_chaining", on ? 1 : 0);
	config_save();
}

bool device_state_folder_chaining(void) { return config_get_int("player", "folder_chaining", 0) != 0; }

void device_state_set_folder_chaining(bool on) {
	config_set_int("player", "folder_chaining", on ? 1 : 0);
	config_save();
}

// One pass over the records, keeping both neighbours of the current one. It
// cannot stop at the next any more: going backwards needs the last record on
// the card, which is only known once the walk has run out.
typedef struct {
	const char *current;
	char first[256]; // for the wrap round at the end of the alphabet
	char last[256];	 // ... and at the start of it
	char prev[256];
	char next[256];
	bool seen_current;
	bool found_next;
	bool found_prev;
} album_walk_t;

static bool album_walk_cb(const char *name, const char *path, const char *artist, void *user) {
	(void)path;
	(void)artist;
	album_walk_t *w = user;

	if (!w->first[0]) {
		snprintf(w->first, sizeof(w->first), "%s", name);
	}

	if (w->seen_current && !w->found_next) {
		snprintf(w->next, sizeof(w->next), "%s", name);
		w->found_next = true;
	} else if (!w->seen_current && library_album_same(name, w->current)) {
		// Whatever came immediately before is the previous record; nothing
		// before it means the current one is the first, and the wrap uses
		// `last` instead.
		if (w->last[0]) {
			snprintf(w->prev, sizeof(w->prev), "%s", w->last);
			w->found_prev = true;
		}
	}

	if (library_album_same(name, w->current)) {
		w->seen_current = true;
	}
	snprintf(w->last, sizeof(w->last), "%s", name);
	return true;
}

typedef struct {
	char **paths;
	int count;
	int capacity;
} album_tracks_t;

static bool album_track_cb(const char *name, const char *path, const char *artist, void *user) {
	(void)name;
	(void)artist;
	album_tracks_t *t = user;
	if (!path || !path[0]) {
		return true;
	}
	if (t->count >= t->capacity) {
		int want = t->capacity ? t->capacity * 2 : 32;
		char **grown = realloc(t->paths, (size_t)want * sizeof(*grown));
		if (!grown) {
			return false;
		}
		t->paths = grown;
		t->capacity = want;
	}
	t->paths[t->count] = strdup(path);
	if (!t->paths[t->count]) {
		return false;
	}
	t->count++;
	return true;
}

// Starts the record `wanted` (an album value), its tracks in their order: on the
// first track going forward, on the last going back. False when the record has
// no tracks.
static bool start_album(const char *wanted, bool forward) {
	album_tracks_t tracks = {NULL, 0, 0};
	library_for_each_ordered(LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ALBUM, wanted, LIBRARY_ORDER_DEFAULT,
							 album_track_cb, &tracks);

	bool started = false;
	if (tracks.count > 0) {
		device_state_play_list((const char *const *)tracks.paths, tracks.count,
							   forward ? 0 : tracks.count - 1);
		playlist_set_album(wanted); // so the record beyond that one can follow too
		char shown[256];
		library_album_title(wanted, shown, sizeof(shown));
		fprintf(stderr, "album: %s \"%s\" (%d tracks)\n", forward ? "on to" : "back to", shown, tracks.count);
		started = true;
	}

	for (int i = 0; i < tracks.count; i++) {
		free(tracks.paths[i]);
	}
	free(tracks.paths);
	return started;
}

// ---------------------------------------------------------------------------
// Records in random order
//
// "Play albums in random order" starts one record of a list of records picked
// at random, its tracks in their order, and when "play albums back to back"
// takes over at its end, the next record is another one of the same list
// picked at random rather than the one after it by name. Back from the first
// track goes to the record played before, while there is one.
//
// It holds for as long as the queue is one of the records it started: the
// album label is what says so. Anything else that starts a queue sets a label
// of its own, or none, and the next record is again the one after by name.
// ---------------------------------------------------------------------------

#define RANDOM_ALBUM_HISTORY 16
#define RANDOM_ALBUM_TRIES 8

static struct {
	bool on;
	library_filter_t filter;
	char value[256];
	// The records played, the one playing last.
	char history[RANDOM_ALBUM_HISTORY][256];
	int depth;
} random_albums;

static bool album_value_cb(const char *name, const char *path, const char *artist, void *user) {
	(void)path;
	(void)artist;
	snprintf(user, 256, "%s", name ? name : "");
	return false;
}

static bool random_albums_recent(const char *album, int how_far) {
	for (int i = random_albums.depth - 1; i >= 0 && how_far > 0; i--, how_far--) {
		if (library_album_same(random_albums.history[i], album)) {
			return true;
		}
	}
	return false;
}

static void random_albums_push(const char *album) {
	if (random_albums.depth == RANDOM_ALBUM_HISTORY) {
		memmove(random_albums.history[0], random_albums.history[1],
				(RANDOM_ALBUM_HISTORY - 1) * sizeof(random_albums.history[0]));
		random_albums.depth--;
	}
	snprintf(random_albums.history[random_albums.depth++], sizeof(random_albums.history[0]), "%s", album);
}

// A record of the list at random into `out`: never `avoid`, and where the list
// is long enough, none of the last few played either. False when the list has
// no record to give.
static bool random_albums_pick(const char *avoid, char *out, size_t size) {
	library_index_t *ix = library_index_open(LIBRARY_LIST_ALBUMS, random_albums.filter,
											 random_albums.value[0] ? random_albums.value : NULL,
											 LIBRARY_ORDER_DEFAULT, false);
	int count = library_index_count(ix);
	if (count <= 0) {
		library_index_close(ix);
		return false;
	}

	// How many of the records played last are kept out of the draw: never all
	// of them, or a short list would have nothing left to give.
	int keep_out = count / 2;
	if (keep_out > RANDOM_ALBUM_HISTORY) {
		keep_out = RANDOM_ALBUM_HISTORY;
	}

	static bool seeded;
	if (!seeded) {
		srand((unsigned)time(NULL));
		seeded = true;
	}

	char name[256] = "";
	bool found = false;
	for (int tries = 0; tries < RANDOM_ALBUM_TRIES && !found; tries++) {
		name[0] = '\0';
		library_index_window(ix, rand() % count, 1, album_value_cb, name);
		if (!name[0]) {
			continue;
		}
		bool is_avoided = avoid && avoid[0] && library_album_same(name, avoid);
		found = count == 1 || (!is_avoided && !random_albums_recent(name, keep_out));
	}
	if (!found && count > 1) {
		// Unlucky draws: the first record that is not the one playing.
		for (int i = 0; i < count && !found; i++) {
			name[0] = '\0';
			library_index_window(ix, i, 1, album_value_cb, name);
			found = name[0] && !(avoid && avoid[0] && library_album_same(name, avoid));
		}
	}
	library_index_close(ix);
	if (found) {
		snprintf(out, size, "%s", name);
	}
	return found;
}

bool device_state_play_random_album(library_filter_t filter, const char *value) {
	random_albums.on = false;
	random_albums.depth = 0;
	random_albums.filter = filter;
	snprintf(random_albums.value, sizeof(random_albums.value), "%s", value ? value : "");

	char album[256];
	if (!random_albums_pick(playlist_album(), album, sizeof(album)) || !start_album(album, true)) {
		return false;
	}
	random_albums_push(album);
	random_albums.on = true;
	return true;
}

// The record after `current` when the queue is one the random order started:
// another at random going forward, the one before going back. False when the
// queue is not one of those records, which leaves the step to the order by name.
static bool random_albums_beside(const char *current, bool forward) {
	if (!random_albums.on || random_albums.depth == 0 ||
		!library_album_same(random_albums.history[random_albums.depth - 1], current)) {
		random_albums.on = false;
		return false;
	}

	if (!forward && random_albums.depth >= 2) {
		random_albums.depth--;
		return start_album(random_albums.history[random_albums.depth - 1], false);
	}

	char album[256];
	if (!random_albums_pick(current, album, sizeof(album)) || !start_album(album, forward)) {
		return false;
	}
	if (!forward) {
		// Back past the first record played: a record at random before it.
		random_albums.depth = 0;
	}
	random_albums_push(album);
	return true;
}

// Starts the record either side of `album`. Going forward it opens on the first
// track, going back on the last: "previous" from the top of a record means the
// end of the one before it, the same way it means the end of the track before
// inside one. False when there is no library, no record that side, or the one
// found is empty.
static bool play_album_beside(const char *album, bool forward) {
	album_walk_t walk = {album, "", "", "", "", false, false, false};
	library_for_each(LIBRARY_LIST_ALBUMS, LIBRARY_FILTER_NONE, NULL, album_walk_cb, &walk);

	const char *wanted = NULL;
	if (forward) {
		if (walk.found_next) {
			wanted = walk.next;
		} else if (walk.first[0] && !library_album_same(walk.first, album)) {
			wanted = walk.first; // the current record was the last: round to the top
		}
	} else {
		if (walk.found_prev) {
			wanted = walk.prev;
		} else if (walk.last[0] && !library_album_same(walk.last, album)) {
			wanted = walk.last; // the current record was the first: round to the bottom
		}
	}
	if (!wanted || !wanted[0]) {
		return false;
	}
	return start_album(wanted, forward);
}

// Starts the folder either side of the one the queue was built from, opening on
// its first track going forward and on its last going back. False when there is
// nothing that side -- a queue that is not a folder's, a folder outside the
// card, or a card whose only music is right here.
static bool play_folder_beside(const char *file, bool forward) {
	const char *root = playlist_card_root();
	if (!root || !root[0] || !file || !file[0]) {
		return false;
	}

	char folder[512];
	snprintf(folder, sizeof(folder), "%s", file);
	char *slash = strrchr(folder, '/');
	if (!slash) {
		return false;
	}
	*slash = '\0';

	char wanted[512];
	bool have = forward ? playlist_next_folder(root, folder, wanted, sizeof(wanted))
						: playlist_prev_folder(root, folder, wanted, sizeof(wanted));
	if (!have) {
		return false;
	}

	// A folder queue and not a list: the option only means anything if the
	// folder after this one can be followed by the one after that.
	playlist_load_folder(wanted, NULL);

	if (!forward) {
		int count = playlist_count();
		if (count > 0) {
			playlist_set_current(count - 1);
		}
	}

	char path[512];
	if (!playlist_current_path(path, sizeof(path))) {
		return false; // it looked like it held a track and does not any more
	}
	load_and_play(path);
	fprintf(stderr, "folder: %s \"%s\"\n", forward ? "on to" : "back to", wanted);
	return true;
}

// Either option, whichever applies: the record beside this one if the queue is
// a record and records are chained, otherwise the folder beside it if the queue
// is a folder and folders are chained. The one place both rules live, so that
// the prev and next keys and a track running out all take the same step.
//
// Says nothing about WHEN to take it: the caller decides that, and the two
// callers differ. A track running out has to ignore repeat-one, which is an
// instruction to stay put; a key pressed on purpose is not the end of a queue
// and does not.
static bool chain_beside(bool forward) {
	const char *album = playlist_album();
	if (album && album[0] && device_state_album_chaining()) {
		char held[256];
		snprintf(held, sizeof(held), "%s", album); // the queue is about to be replaced
		if (random_albums_beside(held, forward) || play_album_beside(held, forward)) {
			return true;
		}
	}
	if (device_state_folder_chaining() && !playlist_is_custom() &&
		play_folder_beside(current_metadata_file, forward)) {
		return true;
	}
	return false;
}

bool device_state_advance_auto(char *out_path, size_t out_size) {
	if (radio_is_active()) {
		return false; // a stream never finishes, so there is nothing to advance to
	}

	// A book ends where it ends. The queue behind a book in one file is its
	// folder, and on a card that folder is a shelf of other books -- rolling
	// straight into somebody else's novel at the end of this one is not what
	// finishing a book should do. (The position was already cleared by then,
	// so opening it again starts it over.)
	//
	// A folder book goes on into its next file: its queue is its parts. With
	// "stop at end of chapter" on, that file is loaded paused at its start,
	// since for a book of this shape a file is a chapter.
	if (audiobook_is_playing()) {
		if (!audiobook_has_next_part()) {
			return false;
		}
		char next[512];
		if (!playlist_advance_auto(next, sizeof(next))) {
			return false;
		}
		if (audiobook_stop_at_chapter_end()) {
			load_paused(next);
		} else {
			load_and_play(next);
		}
		if (out_path && out_size > 0) {
			snprintf(out_path, out_size, "%s", next);
		}
		return true;
	}

	char path[512];
	playlist_resync_to_current();

	// The end of a record or of a folder, asked before the queue is advanced
	// rather than after it fails to advance.
	//
	// Asked afterwards it would almost never fire: only PLAYBACK_MODE_NORMAL
	// runs a queue out, and repeat-all wraps back to the first track instead, so
	// the album would play through and start again -- exactly what the option is
	// meant to stop. Repeat-one still wins here: it is an explicit instruction
	// to stay on this track, not a queue that reached its end.
	if (playlist_at_end() && playlist_get_mode() != PLAYBACK_MODE_REPEAT_ONE && chain_beside(true)) {
		if (out_path && out_size > 0) {
			playlist_current_path(out_path, out_size);
		}
		return true;
	}

	if (!playlist_advance_auto(path, sizeof(path))) {
		// The queue is spent, and there was no record to follow: the last
		// track has been heard to its end.
		playhistory_left();
		return false;
	}

	load_and_play(path);

	if (out_path && out_size > 0) {
		strncpy(out_path, path, out_size - 1);
		out_path[out_size - 1] = '\0';
	}
	return true;
}

// How long before the end of a track the next one is read ahead
// (readahead.h), and how far into the track at the earliest, so the read does
// not land on the moment a track starts and its output is being opened.
#define READAHEAD_LEAD_SECS 20.0
#define READAHEAD_SETTLE_SECS 3.0

void device_state_read_ahead(double position, double total) {
	// Only where gapless can use it: what it buys is the next track opening
	// before the queue held at the end of this one runs out.
	if (!audio_get_gapless() || radio_is_active() || total <= 0.0 || position < READAHEAD_SETTLE_SECS ||
		total - position > READAHEAD_LEAD_SECS) {
		return;
	}
	// The same ends device_state_advance_auto() stops at. At the end of the
	// queue, which record or folder follows (if any) is decided when the track
	// ends, so there is nothing to read ahead of.
	if (audiobook_is_playing() && !audiobook_has_next_part()) {
		return;
	}
	if (playlist_at_end() && playlist_get_mode() != PLAYBACK_MODE_REPEAT_ONE) {
		return;
	}

	char next[512];
	if (!playlist_peek_auto(next, sizeof(next)) || strcmp(next, current_metadata_file) == 0) {
		return; // nothing next, or this track again, which is already read
	}
	// Asked once per track: the poll comes round twice a second.
	static char asked[512];
	if (strcmp(next, asked) == 0) {
		return;
	}
	snprintf(asked, sizeof(asked), "%s", next);
	readahead_file(next);
}

// Re-anchors the queue to the track that is actually loaded. If the two ever
// drift apart -- whatever the cause -- a "next" from a stale index replays a
// track the user already heard, so the first press restarts the same song and
// only the second moves on. Cheap to check, and it makes every manual advance
// immune to the drift instead of hunting each source of it.
static void playlist_resync_to_current(void) {
	if (!current_metadata_file[0]) {
		return;
	}

	char queue_now[512];
	if (playlist_current_path(queue_now, sizeof(queue_now)) && strcmp(queue_now, current_metadata_file) == 0) {
		return; // in agreement, nothing to do
	}

	// A custom queue (a library list, the favourites) must never be replaced by
	// the folder of whatever is playing: that would turn "next within all
	// tracks" back into "next within the album". Point it at the loaded track
	// instead, if it is in there.
	if (playlist_is_custom()) {
		playlist_locate(current_metadata_file);
		return;
	}

	char folder[512];
	strncpy(folder, current_metadata_file, sizeof(folder) - 1);
	folder[sizeof(folder) - 1] = '\0';

	const char *filename = current_metadata_file;
	char *slash = strrchr(folder, '/');
	if (slash) {
		*slash = '\0';
		filename = slash + 1;
	}

	playlist_load_folder(folder, filename);
}

static const char *const PROF_RESYNC = "skip_resync";
static const char *const PROF_QUEUE_STEP = "skip_playlist_next";

bool device_state_next(char *out_path, size_t out_size) {
	char path[512];
	frameprof_begin(PROF_RESYNC);
	playlist_resync_to_current();
	frameprof_end(PROF_RESYNC);

	// The same step a track running out would take. Without this the two
	// options only worked when the player was left alone: pressing next on the
	// last track of a record went round to its first instead of moving on to
	// the record after, which is the opposite of what "play albums back to
	// back" says.
	if (playlist_at_end() && chain_beside(true)) {
		if (out_path && out_size > 0) {
			playlist_current_path(out_path, out_size);
		}
		return true;
	}

	frameprof_begin(PROF_QUEUE_STEP);
	bool stepped = playlist_next(path, sizeof(path));
	frameprof_end(PROF_QUEUE_STEP);
	if (!stepped)
		return false;

	load_and_play(path);

	if (out_path && out_size > 0) {
		strncpy(out_path, path, out_size - 1);
		out_path[out_size - 1] = '\0';
	}
	return true;
}

bool device_state_prev(char *out_path, size_t out_size) {
	char path[512];
	playlist_resync_to_current();

	// And the mirror of it: from the first track of a record, back is the END
	// of the record before, not the end of this one. Nothing automatic ever
	// goes this way, so this direction exists only for the key.
	if (playlist_current_index() <= 0 && chain_beside(false)) {
		if (out_path && out_size > 0) {
			playlist_current_path(out_path, out_size);
		}
		return true;
	}

	if (!playlist_prev(path, sizeof(path)))
		return false;

	load_and_play(path);

	if (out_path && out_size > 0) {
		strncpy(out_path, path, out_size - 1);
		out_path[out_size - 1] = '\0';
	}
	return true;
}

bool device_state_take_completion(void) { return audio_take_completion(); }

audio_status_t device_state_toggle_play_pause(void) {
	// A live stream cannot be paused: there is no position to come back to,
	// and starting again means reconnecting. Whoever pressed play/pause --
	// the player's button, the control centre, the physical key -- means
	// "stop this". Without this, the paused track underneath would resume on
	// top of the stream and both would be writing to one output.
	if (radio_is_active()) {
		// While it is still connecting the key does nothing: stopping a
		// connection halfway only makes the wait for the next one longer.
		if (radio_is_connecting()) {
			return AUDIO_STATUS_PLAYING;
		}
		if (radio_is_playing()) {
			radio_stop();
			return AUDIO_STATUS_STOPPED;
		}
		// Stopped on a station: play means reconnect to it, not go back to
		// whatever track was loaded before.
		radio_resume();
		return AUDIO_STATUS_PLAYING;
	}

	switch (audio_get_status()) {
	case AUDIO_STATUS_PLAYING:
		audio_pause();
		return AUDIO_STATUS_PAUSED;
	case AUDIO_STATUS_PAUSED:
		// Rewind after pause: on a book, resuming winds back a few seconds,
		// because whatever was being said when the pause landed was almost
		// certainly mid-sentence. Done here rather than in the player, so the
		// side key and the control centre get it too.
		if (audiobook_is_playing() && audiobook_rewind_enabled() && !audiobook_take_rewind_suppression()) {
			double current = 0, total = 0;
			audio_get_progress(&current, &total);
			double back = current - (double)audiobook_rewind_seconds();
			audio_seek(back > 0 ? back : 0);
		}
		playhistory_resumed();
		audio_resume();
		return AUDIO_STATUS_PLAYING;
	case AUDIO_STATUS_STOPPED:
	default:
		// Playback ended (or was stopped): restart the currently-loaded track
		// from the beginning so pressing play always plays the shown song --
		// unless the engine was stopped by the storage going away rather than
		// by the track ending, in which case play means carry on from where the
		// card was pulled.
		if (current_metadata_file[0]) {
			double resume = -1.0;
			if (interrupted_pos > 0 && strcmp(interrupted_file, current_metadata_file) == 0) {
				resume = interrupted_pos;
			}
			load_and_play_at(current_metadata_file, resume);
			return AUDIO_STATUS_PLAYING;
		}
		return AUDIO_STATUS_STOPPED;
	}
}

// ---------------------------------------------------------------------------
// Scrubbing: holding a skip button to run through the track
//
// A held button asks for a new position four times a second, and a real seek is
// not a cheap thing to ask for four times a second. Every one of them drops
// what the sound card has queued and prepares the stream again, and on this
// hardware the stream only starts once its buffer is full -- about three
// quarters of a second of audio. Emptying it every quarter second means it
// never gets there: nothing comes out while the button is held, and the DAC is
// left being reprogrammed under a stream that never ran, which ends playback
// altogether.
//
// So a hold moves a number, not the decoder. The position the interface draws
// follows the thumb, the audio thread is told nothing, and the one real seek
// happens when the button comes up -- which is also what it looks like from the
// outside: the bar runs to where it is wanted and the music starts there.
// ---------------------------------------------------------------------------
static bool scrubbing;
static double scrub_target;
// The track the open scrub was measured on. A hold that runs to the end of a
// track lets the queue move on underneath it, and seconds counted on the track
// that finished mean nothing on the one that started.
static char scrub_file[512];

// Guards the three above: they are written from the input thread and read from
// the interface's. Nothing else in this file is shared like that, so the lock
// is theirs alone.
static pthread_mutex_t scrub_lock = PTHREAD_MUTEX_INITIALIZER;

static void scrub_cancel(void) {
	pthread_mutex_lock(&scrub_lock);
	scrubbing = false;
	pthread_mutex_unlock(&scrub_lock);
}

bool device_state_scrub_active(double *position_out) {
	pthread_mutex_lock(&scrub_lock);
	bool active = scrubbing;
	if (active && position_out) {
		*position_out = scrub_target;
	}
	pthread_mutex_unlock(&scrub_lock);
	return active;
}

void device_state_scrub_by(double delta) {
	double current = 0;
	double total = 0;
	audio_get_progress(&current, &total);
	if (total <= 0) {
		return; // nothing with a length to run through: a live stream
	}

	pthread_mutex_lock(&scrub_lock);
	if (!scrubbing) {
		scrubbing = true;
		scrub_target = current; // from where the music actually is
		snprintf(scrub_file, sizeof(scrub_file), "%s", current_metadata_file);
	}
	scrub_target += delta;
	if (scrub_target < 0) {
		scrub_target = 0;
	}
	// A whisker short of the end, so running off the edge does not finish the
	// track and start the next one.
	if (scrub_target > total - 1) {
		scrub_target = total - 1;
		if (scrub_target < 0) {
			scrub_target = 0;
		}
	}
	pthread_mutex_unlock(&scrub_lock);
}

void device_state_scrub_commit(void) {
	pthread_mutex_lock(&scrub_lock);
	bool was = scrubbing;
	double target = scrub_target;
	char file[sizeof(scrub_file)];
	snprintf(file, sizeof(file), "%s", scrub_file);
	scrubbing = false;
	pthread_mutex_unlock(&scrub_lock);

	// The track can end while the button is held, and the button comes up
	// after the queue has already started the next one: seek only the track
	// the position was counted on.
	if (was && strcmp(file, current_metadata_file) == 0) {
		device_state_seek(target);
	}
}

void device_state_seek(double seconds) {
	// A seek from anywhere else -- the slider, the queue -- ends a scrub that
	// was still open, or the interface would go on drawing a thumb that no
	// longer means anything.
	scrub_cancel();

	// If playback already ended, restart the current track first so seeking
	// (e.g. dragging the slider or hitting prev) resumes playback intuitively
	// instead of silently doing nothing.
	if (audio_get_status() == AUDIO_STATUS_STOPPED && current_metadata_file[0]) {
		history_same_listen = true;
		load_and_play(current_metadata_file);
	}
	audio_seek(seconds);
}

void device_state_stop(void) { audio_stop(); }

void device_state_set_volume(long volume) { set_volume(volume); }

void device_state_change_volume(long amount) { change_volume(amount); }

int device_state_get_volume_percent(void) { return get_volume_percent(); }

void device_state_change_volume_percent(int delta) { change_volume_percent(delta); }

void device_state_refresh_battery(void) { sync_battery_from_sysfs(); }
