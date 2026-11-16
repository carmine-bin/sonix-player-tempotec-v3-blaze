#include "playhistory.h"

#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "src/system/core/bootprof.h"
#include "src/system/device/clock.h"
#include "src/system/library/library.h"

static pthread_mutex_t live_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
	bool pending; // to be written down when it is left
	bool waiting; // to become pending once it plays
	char path[512];
	char title[256];
	char artist[256];
} live;

void playhistory_started(const char *path, const char *title, const char *artist, bool kept_out, bool waiting) {
	bool counts = path && path[0] && !kept_out;
	// The library's title first, as the lists show it; the tags' when the file
	// is not in the index. Looked up before the lock: it is a query.
	char shown[sizeof(live.title)];
	if (!counts || !library_track_title(path, shown, sizeof(shown))) {
		snprintf(shown, sizeof(shown), "%s", title ? title : "");
	}

	pthread_mutex_lock(&live_lock);
	live.pending = counts && !waiting;
	live.waiting = counts && waiting;
	snprintf(live.path, sizeof(live.path), "%s", path ? path : "");
	snprintf(live.title, sizeof(live.title), "%s", shown);
	snprintf(live.artist, sizeof(live.artist), "%s", artist ? artist : "");
	pthread_mutex_unlock(&live_lock);
}

void playhistory_resumed(void) {
	pthread_mutex_lock(&live_lock);
	if (live.waiting) {
		live.waiting = false;
		live.pending = true;
	}
	pthread_mutex_unlock(&live_lock);
}

void playhistory_left(void) {
	char path[sizeof(live.path)];
	char title[sizeof(live.title)];
	char artist[sizeof(live.artist)];
	pthread_mutex_lock(&live_lock);
	bool pending = live.pending;
	live.pending = false;
	live.waiting = false;
	snprintf(path, sizeof(path), "%s", live.path);
	snprintf(title, sizeof(title), "%s", live.title);
	snprintf(artist, sizeof(artist), "%s", live.artist);
	pthread_mutex_unlock(&live_lock);
	if (!pending) {
		return;
	}
	// A time that is not known is not written down as one: the history would
	// file the entry under a day in 1970.
	long long when = clock_is_set() ? (long long)time(NULL) : 0;
	BOOTPROF_CALL("library_history_add", library_history_add(path, title, artist, when));
}
