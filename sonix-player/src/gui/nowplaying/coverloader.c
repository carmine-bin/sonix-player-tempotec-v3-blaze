#include "coverloader.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "src/system/device/power.h"
#include "src/system/core/utils.h"

// One slot's worth of state. `generation` is what makes abandoning safe: the
// worker records the generation it started with and throws its result away if
// the slot has been asked for something else in the meantime.
typedef struct {
	char path[512];
	int size;
	// Above zero: the picture is made from the player's cover at this size
	// (cover_load_shared) rather than as a list thumbnail.
	int share_w, share_h;

	uint32_t generation; // bumped by every request and every release
	bool pending;		 // asked for, not started
	bool running;		 // the worker is on it now

	bool done;	// a result is waiting to be collected
	bool found; // ... and whether that result is a picture or "no artwork"
	cover_image_t image;
} slot_t;

static slot_t slots[COVERLOADER_SLOTS];

// One file, two pictures out of a single decode: the player's cover and its
// blurred backdrop, or the screensaver's photograph and the blurred block over
// its lower part. Same shape and same generation discipline as the slots; the
// only difference between the two is which loader turns the file into the pair.
typedef struct {
	char path[512];
	int first_w, first_h;
	int second_w, second_h;

	uint32_t generation;
	bool pending;
	bool running;

	bool done;
	bool found;
	cover_image_t first;
	cover_image_t second;
} dual_job_t;

typedef bool (*dual_loader_t)(const char *path, int w, int h, cover_image_t *first_out, int second_w,
							  int second_h, cover_image_t *second_out);

// The "what is it" job: the same newest-wins discipline as the others, with
// one number for an answer instead of two pictures.
//
// It has a thread of its own (id_worker). The player keeps the last track's
// cover up until this answer comes, so that a record's next track does not
// blink to the placeholder and back; behind a decode already running on the
// other thread -- the next track's cover being made ahead, a list thumbnail --
// the answer took as long as that decode, and the last track's cover stayed
// over the new track's title for all of it.
typedef struct {
	char path[512];
	uint32_t generation;
	bool pending;
	bool running;
	bool done;
	uint64_t id;
} id_job_t;

static id_job_t player_id_job;

// The next track's cover, made only to be kept in the cache. No generation and
// no result: a newer request just replaces the path while it waits.
static struct {
	char path[512];
	int w, h;
	bool pending;
} prefetch_job;

static dual_job_t player_job;
static dual_job_t saver_job;
static dual_job_t warm_job;		// the next track's player artwork, into the disk cache
static dual_job_t warm_back_job; // and the previous track's, after it

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wakeup = PTHREAD_COND_INITIALIZER;
// The warm-ups' own thread is woken on its own condition: a signal on `wakeup`
// must reach the worker, and with two threads waiting it might not.
static pthread_cond_t warm_wakeup = PTHREAD_COND_INITIALIZER;
static pthread_cond_t id_wakeup = PTHREAD_COND_INITIALIZER;
// Only when id_worker could not be started: the worker answers the questions
// itself, first of all its jobs.
static bool id_on_worker;
static bool started;

// Said the moment a player question is answered, from the worker: see
// coverloader_set_player_ready_cb().
static void (*player_ready_cb)(void);

void coverloader_set_player_ready_cb(void (*cb)(void)) { player_ready_cb = cb; }

static void player_ready(void) {
	if (player_ready_cb) {
		player_ready_cb();
	}
}

// Picks up the next slot waiting to be decoded. Called with the lock held.
static int next_pending(void) {
	for (int i = 0; i < COVERLOADER_SLOTS; i++) {
		if (slots[i].pending && !slots[i].running) {
			return i;
		}
	}
	return -1;
}

// Runs one dual job. Called with the lock held; drops it for the decode and
// returns WITHOUT it, so the caller must not unlock again. Unlocking a mutex
// this thread no longer owns is undefined behaviour, and on glibc it clears
// the futex word: whatever the other thread was holding is released under it,
// and from that moment the mutex excludes nothing.
static void run_dual_job(dual_job_t *job, dual_loader_t load) {
	char path[sizeof(job->path)];
	memcpy(path, job->path, sizeof(path));
	int fw = job->first_w, fh = job->first_h;
	int sw = job->second_w, sh = job->second_h;
	uint32_t generation = job->generation;

	job->pending = false;
	job->running = true;
	pthread_mutex_unlock(&lock);

	cover_image_t first, second;
	bool found = load(path, fw, fh, &first, sw, sh, &second);

	pthread_mutex_lock(&lock);
	job->running = false;

	if (job->generation != generation) {
		// Another track (or another picture) was asked for while this one
		// decoded. Nobody wants it. Freed whatever the verdict was: the loaders
		// fill the second image even when they report failure, so gating this on
		// `found` would leak a full-screen buffer per coverless track.
		cover_free(&first);
		cover_free(&second);
	} else {
		job->done = true;
		job->found = found;
		job->first = first;
		job->second = second;
		if (found) {
			job->first.dsc.data = job->first.pixels;
			if (job->second.pixels) {
				job->second.dsc.data = job->second.pixels;
			}
		}
	}
	pthread_mutex_unlock(&lock);
}

// Answers the question waiting in player_id_job. Called without the lock.
static void answer_id(void) {
	pthread_mutex_lock(&lock);
	if (!player_id_job.pending || player_id_job.running) {
		pthread_mutex_unlock(&lock);
		return;
	}
	char path[sizeof(player_id_job.path)];
	memcpy(path, player_id_job.path, sizeof(path));
	uint32_t generation = player_id_job.generation;
	player_id_job.pending = false;
	player_id_job.running = true;
	pthread_mutex_unlock(&lock);

	uint64_t id = cover_source_id(path);

	pthread_mutex_lock(&lock);
	player_id_job.running = false;
	if (player_id_job.generation == generation) {
		player_id_job.id = id;
		player_id_job.done = true;
	}
	pthread_mutex_unlock(&lock);
	player_ready();
}

static void *worker(void *arg) {
	(void)arg;

	// One core, so it stays below the interface -- but how far below depends on
	// whether anybody is looking. See the note above the loop.
	thread_be_low_priority("coverloader");

	for (;;) {
		// Where in the background this thread sits, decided again before every
		// job.
		//
		// SCHED_IDLE gets almost nothing from a core the interface is keeping
		// busy, so pictures only arrive once the finger stops; a plain nice
		// value gives it enough to keep up with a scroll.
		//
		// That share is not free with the screen off, where the only thing this
		// thread can be racing is playback -- and over Bluetooth playback is the
		// one stream with no clock behind it: a block written late is a block
		// lost, and after two seconds without progress the link is taken for
		// gone. A picture for a screen nobody can see is not worth that, so with
		// the screen off the thread goes back to the bottom of the pile.
		thread_set_background_level(power_screen_is_on());

		pthread_mutex_lock(&lock);

		while (next_pending() < 0 && !(id_on_worker && player_id_job.pending && !player_id_job.running) &&
			   !(player_job.pending && !player_job.running) && !(saver_job.pending && !saver_job.running) &&
			   !prefetch_job.pending) {
			pthread_cond_wait(&wakeup, &lock);
		}

		// First of all, when it is the worker's: the question that can spare
		// every other piece of work on this list.
		if (id_on_worker && player_id_job.pending && !player_id_job.running) {
			pthread_mutex_unlock(&lock);
			answer_id();
			continue;
		}

		// The player's artwork is the picture the user is actually looking
		// at; it goes before any list thumbnail.
		if (player_job.pending && !player_job.running) {
			cover_defer_player_store(true);
			run_dual_job(&player_job, cover_load_player_images); // returns unlocked
			cover_defer_player_store(false);
			player_ready();
			cover_store_deferred(); // the picture is on its way; now its row in the database
			continue;
		}

		int index = next_pending();
		if (index < 0 && saver_job.pending && !saver_job.running) {
			// It is a picture for a screen that is off, and nothing on screen is
			// waiting for it.
			run_dual_job(&saver_job, cover_load_screensaver_images); // returns unlocked
			continue;
		}
		if (index < 0 && prefetch_job.pending) {
			// Last of all: a guess at what comes next, wanted by nobody yet.
			char path[sizeof(prefetch_job.path)];
			memcpy(path, prefetch_job.path, sizeof(path));
			int w = prefetch_job.w, h = prefetch_job.h;
			prefetch_job.pending = false;
			pthread_mutex_unlock(&lock);
			cover_prefetch_player(path, w, h);
			continue;
		}
		if (index < 0) {
			// Cannot happen while the lock is held from the wait above, but a
			// negative index here would index before the array.
			pthread_mutex_unlock(&lock);
			continue;
		}

		// Copy what the job needs, then let go of the lock: the decode is the
		// slow part and the interface must not wait behind it for anything.
		slot_t *slot = &slots[index];
		char path[sizeof(slot->path)];
		memcpy(path, slot->path, sizeof(path));
		int size = slot->size;
		int share_w = slot->share_w, share_h = slot->share_h;
		uint32_t generation = slot->generation;

		slot->pending = false;
		slot->running = true;
		pthread_mutex_unlock(&lock);

		cover_image_t image;
		cover_defer_player_store(true);
		bool found = share_w > 0 ? cover_load_shared(path, share_w, share_h, size, &image)
								  : cover_thumb_load(path, false, size, &image);
		cover_defer_player_store(false);

		pthread_mutex_lock(&lock);
		slot->running = false;

		if (slot->generation != generation) {
			// The row moved on during the decode. Nobody wants this.
			if (found) {
				cover_free(&image);
			}
		} else {
			slot->done = true;
			slot->found = found;
			slot->image = image;
			if (found) {
				// The descriptor points into its own pixel buffer, which has
				// not moved; only the struct describing it was copied.
				slot->image.dsc.data = slot->image.pixels;
			}
		}
		pthread_mutex_unlock(&lock);
		cover_store_deferred();
	}

	return NULL;
}

// The next and the previous track's artwork, worked out ahead into the disk
// cache. On a thread of their own, at the bottom of the pile: on the worker
// they queued in front of the picture the user was waiting for -- a warm-up
// decode takes the better part of a second here, and a skip that landed in the
// middle of one waited for all of it before its own picture, already in the
// cache, was even looked up. Here a warm-up only ever has the core when
// nothing else wants it, and the worker is free for the player.
static void *warm_worker(void *arg) {
	(void)arg;
	thread_be_background("cover warmer");
	pthread_mutex_lock(&lock);
	for (;;) {
		while (!(warm_job.pending && !warm_job.running) && !(warm_back_job.pending && !warm_back_job.running)) {
			pthread_cond_wait(&warm_wakeup, &lock);
		}
		// The next track first: "previous" is pressed far less often.
		dual_job_t *job = warm_job.pending && !warm_job.running ? &warm_job : &warm_back_job;
		run_dual_job(job, cover_warm_player_images); // returns unlocked
		pthread_mutex_lock(&lock);
	}
	return NULL;
}

// Answers the player's "what is it" questions, and nothing else, so none waits
// behind a decode. It reads a picture and hashes it -- or finds the answer in
// the cache's table -- which is a fraction of what decoding one costs; what it
// shares with the decodes (the cache, the mapped pictures) is already locked
// for the sonixlink threads that read the same files.
static void *id_worker(void *arg) {
	(void)arg;
	thread_be_low_priority("coverid");

	for (;;) {
		thread_set_background_level(power_screen_is_on()); // as in worker()

		pthread_mutex_lock(&lock);
		while (!(player_id_job.pending && !player_id_job.running)) {
			pthread_cond_wait(&id_wakeup, &lock);
		}
		pthread_mutex_unlock(&lock);
		answer_id();
	}

	return NULL;
}

void coverloader_start(void) {
	pthread_mutex_lock(&lock);

	if (started) {
		pthread_mutex_unlock(&lock);
		return;
	}
	started = true;
	pthread_mutex_unlock(&lock);

	pthread_t thread;
	if (pthread_create(&thread, NULL, worker, NULL) != 0) {
		fprintf(stderr, "coverloader: could not start the worker; artwork will not load\n");
		started = false;
		return;
	}
	pthread_detach(thread);
	if (pthread_create(&thread, NULL, warm_worker, NULL) == 0) {
		pthread_detach(thread);
	}

	if (pthread_create(&thread, NULL, id_worker, NULL) != 0) {
		// The questions then go to the worker after all (see worker()).
		fprintf(stderr, "coverloader: could not start the id thread; the worker answers instead\n");
		pthread_mutex_lock(&lock);
		id_on_worker = true;
		pthread_cond_signal(&wakeup); // a question may be waiting already
		pthread_mutex_unlock(&lock);
		return;
	}
	pthread_detach(thread);
}

// Drops whatever the slot is holding. Called with the lock held.
static void discard(slot_t *slot) {
	slot->generation++;
	slot->pending = false;

	if (slot->done && slot->found) {
		cover_free(&slot->image);
	}
	slot->done = false;
	slot->found = false;
	memset(&slot->image, 0, sizeof(slot->image));
}

void coverloader_request(int slot_index, const char *path, int size) {
	if (slot_index < 0 || slot_index >= COVERLOADER_SLOTS || !path) {
		return;
	}

	pthread_mutex_lock(&lock);

	slot_t *slot = &slots[slot_index];
	discard(slot);

	snprintf(slot->path, sizeof(slot->path), "%s", path);
	slot->size = size;
	slot->share_w = 0;
	slot->share_h = 0;
	slot->pending = true;

	pthread_cond_signal(&wakeup);
	pthread_mutex_unlock(&lock);
}

void coverloader_request_shared(int slot_index, const char *path, int size, int player_w, int player_h) {
	if (slot_index < 0 || slot_index >= COVERLOADER_SLOTS || !path) {
		return;
	}

	pthread_mutex_lock(&lock);

	slot_t *slot = &slots[slot_index];
	discard(slot);

	snprintf(slot->path, sizeof(slot->path), "%s", path);
	slot->size = size;
	slot->share_w = player_w;
	slot->share_h = player_h;
	slot->pending = true;

	pthread_cond_signal(&wakeup);
	pthread_mutex_unlock(&lock);
}

void coverloader_release(int slot_index) {
	if (slot_index < 0 || slot_index >= COVERLOADER_SLOTS) {
		return;
	}

	pthread_mutex_lock(&lock);
	discard(&slots[slot_index]);
	pthread_mutex_unlock(&lock);
}

bool coverloader_take(int slot_index, cover_image_t *out, bool *finished) {
	if (finished) {
		*finished = false;
	}

	if (slot_index < 0 || slot_index >= COVERLOADER_SLOTS || !out) {
		return false;
	}

	pthread_mutex_lock(&lock);
	slot_t *slot = &slots[slot_index];

	if (!slot->done) {
		pthread_mutex_unlock(&lock);
		return false;
	}

	if (finished) {
		*finished = true;
	}

	bool found = slot->found;
	if (found) {
		*out = slot->image;
		out->dsc.data = out->pixels;
	}

	// Handed over: the slot keeps nothing, so releasing it later cannot free
	// pixels the caller is drawing from.
	slot->done = false;
	slot->found = false;
	memset(&slot->image, 0, sizeof(slot->image));

	pthread_mutex_unlock(&lock);
	return found;
}

// Called with the lock held: forgets whatever the job holds.
static void dual_discard(dual_job_t *job) {
	job->generation++;
	job->pending = false;

	if (job->done) {
		// Not gated on `found`: the second image is filled even when the decode
		// reports failure, so a coverless track would leave one behind.
		cover_free(&job->first);
		cover_free(&job->second);
	}
	job->done = false;
	job->found = false;
	memset(&job->first, 0, sizeof(job->first));
	memset(&job->second, 0, sizeof(job->second));
}

static void dual_request(dual_job_t *job, const char *path, int first_w, int first_h, int second_w,
						 int second_h) {
	if (!path) {
		return;
	}

	pthread_mutex_lock(&lock);
	dual_discard(job);

	snprintf(job->path, sizeof(job->path), "%s", path);
	job->first_w = first_w;
	job->first_h = first_h;
	job->second_w = second_w;
	job->second_h = second_h;
	job->pending = true;

	pthread_cond_signal(&wakeup);
	pthread_mutex_unlock(&lock);
}

static bool dual_take(dual_job_t *job, cover_image_t *first, cover_image_t *second, bool *finished) {
	if (finished) {
		*finished = false;
	}
	if (!first || !second) {
		return false;
	}

	pthread_mutex_lock(&lock);

	if (!job->done) {
		pthread_mutex_unlock(&lock);
		return false;
	}

	if (finished) {
		*finished = true;
	}

	bool found = job->found;
	if (found) {
		*first = job->first;
		first->dsc.data = first->pixels;
		*second = job->second;
		if (second->pixels) {
			second->dsc.data = second->pixels;
		}
	}

	// Handed over: the job keeps nothing. Freed rather than merely zeroed when
	// nothing was handed over, the same way dual_discard() does it -- a job that
	// reported failure can still have filled the second image, which at
	// full-screen size is most of a megabyte per track.
	if (!found) {
		cover_free(&job->first);
		cover_free(&job->second);
	}
	job->done = false;
	job->found = false;
	memset(&job->first, 0, sizeof(job->first));
	memset(&job->second, 0, sizeof(job->second));

	pthread_mutex_unlock(&lock);
	return found;
}

void coverloader_request_player(const char *path, int cover_w, int cover_h, int backdrop_w, int backdrop_h) {
	dual_request(&player_job, path, cover_w, cover_h, backdrop_w, backdrop_h);
}

void coverloader_warm_player(const char *path, int cover_w, int cover_h, int backdrop_w, int backdrop_h) {
	// Never collected: the result is in the cache, not in the job, and the
	// next request discards whatever verdict this one left.
	dual_request(&warm_job, path, cover_w, cover_h, backdrop_w, backdrop_h);
	pthread_mutex_lock(&lock);
	pthread_cond_signal(&warm_wakeup);
	pthread_mutex_unlock(&lock);
}

void coverloader_warm_player_back(const char *path, int cover_w, int cover_h, int backdrop_w, int backdrop_h) {
	dual_request(&warm_back_job, path, cover_w, cover_h, backdrop_w, backdrop_h);
	pthread_mutex_lock(&lock);
	pthread_cond_signal(&warm_wakeup);
	pthread_mutex_unlock(&lock);
}

bool coverloader_take_player(cover_image_t *cover, cover_image_t *backdrop, bool *finished) {
	return dual_take(&player_job, cover, backdrop, finished);
}

void coverloader_request_player_id(const char *path) {
	pthread_mutex_lock(&lock);
	player_id_job.generation++;
	player_id_job.done = false;
	player_id_job.id = 0;
	snprintf(player_id_job.path, sizeof(player_id_job.path), "%s", path ? path : "");
	player_id_job.pending = true;
	pthread_cond_signal(id_on_worker ? &wakeup : &id_wakeup);
	pthread_mutex_unlock(&lock);
}

bool coverloader_take_player_id(uint64_t *id, bool *finished) {
	if (finished) {
		*finished = false;
	}
	pthread_mutex_lock(&lock);
	bool done = player_id_job.done;
	if (done) {
		if (id) {
			*id = player_id_job.id;
		}
		player_id_job.done = false;
		if (finished) {
			*finished = true;
		}
	}
	pthread_mutex_unlock(&lock);
	return done;
}

void coverloader_prefetch_player(const char *path, int cover_w, int cover_h) {
	if (!path || !path[0] || cover_w <= 0 || cover_h <= 0) {
		return;
	}
	pthread_mutex_lock(&lock);
	snprintf(prefetch_job.path, sizeof(prefetch_job.path), "%s", path);
	prefetch_job.w = cover_w;
	prefetch_job.h = cover_h;
	prefetch_job.pending = true;
	pthread_cond_signal(&wakeup);
	pthread_mutex_unlock(&lock);
}

void coverloader_request_screensaver(const char *path, int w, int h, int strip_w, int strip_h) {
	dual_request(&saver_job, path, w, h, strip_w, strip_h);
}

bool coverloader_take_screensaver(cover_image_t *image, cover_image_t *strip, bool *finished) {
	return dual_take(&saver_job, image, strip, finished);
}

void coverloader_release_screensaver(void) {
	pthread_mutex_lock(&lock);
	dual_discard(&saver_job);
	pthread_mutex_unlock(&lock);
}
