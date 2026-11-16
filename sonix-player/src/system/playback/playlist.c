#include "playlist.h"

#include "src/system/core/bootprof.h"
#include "src/system/core/config.h"
#include "src/system/library/cue.h"
#include "src/system/library/folderorder.h"
#include "src/system/library/library.h"

#include "src/system/core/utils.h"

#include <dirent.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

// The queue is only ever touched from the LVGL/UI thread (browser selection,
// the progress poll timer, and the player control buttons all run there), so
// like the metadata cache in device_state.c it needs no locking.
//
// Two kinds of queue live here. A *folder* queue is built by walking a
// directory (the browser's way); a *custom* queue is handed in as a list of
// full paths (the library index pages). The distinction matters to
// device_state's resync logic: a folder queue can be rebuilt from the disk at
// any time, a custom one must never be replaced by the folder of whatever
// happens to be playing.
//
// Playback order is a layer of its own: `order` is a permutation of the entries
// and `pos` walks it. In the plain modes it is the identity; a shuffled queue is
// dealt a random permutation up front, which is what lets the queue page show
// the real upcoming order instead of guessing at it. A queue started in shuffle
// begins at the front and fills upwards as it is listened to; switching to
// shuffle with something already playing deals only what is ahead and leaves
// the history where it is.

// A queue is one of two things.
//
// A list of paths: a folder, a playlist file, a streaming album -- entries that
// exist nowhere else and have to be held.
//
// Or a library list, in which case the queue holds the same handle the list
// does: four bytes a track, and the path is read back from the database when
// something actually needs it. Materialising the paths instead would build a
// second full set beside the list's own, which on a large library is megabytes
// for a queue nobody is looking at.
static char **paths; // full path per entry, each its own allocation
static library_index_t *source_ix;
// How many entries the handle serves. The rest -- what "add to queue" put
// there after the fact -- are in `paths`, starting at zero, so a library queue
// can still be added to without materialising the library.
static size_t ix_count = 0;
static size_t entry_count = 0;
static size_t capacity = 0;

// The last few paths read back out of the handle. Everything that walks the
// queue does so in steps -- the queue page's visible rows, the once-a-second
// cache guards, the resync after a track change -- so a handful of slots turns
// a walk into one query per new position rather than one per ask.
#define PATH_CACHE_SLOTS 8

static struct {
	size_t entry;
	unsigned age;
	bool valid;
	char path[512];
} path_cache[PATH_CACHE_SLOTS];
static unsigned path_cache_clock;

// How many entries live in `paths` rather than in the handle. Never a
// subtraction at the call sites: order_identity() zeroes entry_count when it
// cannot allocate, and on 32-bit the difference then wraps to four billion --
// once into a free() loop, once into an index.
static size_t appended_count(void) { return entry_count > ix_count ? entry_count - ix_count : 0; }

static void path_cache_clear(void) {
	for (int i = 0; i < PATH_CACHE_SLOTS; i++) {
		path_cache[i].valid = false;
	}
}

static bool resolve_cb(const char *name, const char *path, const char *artist, void *user) {
	(void)name;
	(void)artist;
	char *out = user;
	snprintf(out, 512, "%s", path ? path : "");
	return true;
}

// The path of one entry, in storage order. NULL when there is none -- a stale
// handle, or a row that has gone. The pointer is the caller's only until the
// next call.
static const char *entry_path(size_t entry) {
	if (entry >= entry_count) {
		return NULL;
	}
	if (entry >= ix_count) {
		size_t appended = entry - ix_count;
		return paths ? paths[appended] : NULL;
	}

	int oldest = 0;
	for (int i = 0; i < PATH_CACHE_SLOTS; i++) {
		if (path_cache[i].valid && path_cache[i].entry == entry) {
			path_cache[i].age = ++path_cache_clock;
			return path_cache[i].path;
		}
		if (!path_cache[i].valid) {
			oldest = i;
		} else if (path_cache[oldest].valid && path_cache[i].age < path_cache[oldest].age) {
			oldest = i;
		}
	}

	char resolved[512] = {0};
	if (library_index_window(source_ix, (int)entry, 1, resolve_cb, resolved) != 1 || !resolved[0]) {
		return NULL;
	}
	path_cache[oldest].valid = true;
	path_cache[oldest].entry = entry;
	path_cache[oldest].age = ++path_cache_clock;
	snprintf(path_cache[oldest].path, sizeof(path_cache[oldest].path), "%s", resolved);
	return path_cache[oldest].path;
}

static size_t *order;	// permutation of [0, entry_count)
static size_t pos = 0;	// where playback is, as an index into `order`

static bool custom_queue = false;
static playback_mode_t mode = PLAYBACK_MODE_NORMAL;

// A queue whose order is part of its content: podcast episodes.
//
// Other queues are a set of tracks an order can be applied to -- alphabetical,
// random, looping -- and which one is a preference. A queue of episodes is a
// sequence, newest going back in time, and shuffling it is not another order
// but a broken one.
//
// Hence this flag: the queue carries its own order, and neither shuffle nor
// repeat touch it. It clears itself with every new queue (playlist_clear).
static bool ordered_queue = false;

// Shuffle only applies to a queue that does not carry an order of its own. Both
// shuffled modes are shuffling here: they are dealt the same, and differ only
// in what happens when the deal runs out.
static bool shuffling(void) {
	return (mode == PLAYBACK_MODE_SHUFFLE || mode == PLAYBACK_MODE_SHUFFLE_REPEAT) && !ordered_queue;
}

// Bumped by everything that changes the entries or the order. The on-disk
// mirror of the queue only rewrites its list when this moves.
static unsigned revision = 0;

// Where the last "add to queue" landed, so the next one goes after it rather
// than in front of it. Only valid while playback still sits on the track the
// chain started from: `chain_from` is that position, and anything that moves
// `pos` breaks the chain by no longer matching.
static size_t chain_from;  // the pos the chain was anchored at
static size_t chain_slot;  // order-index of the last track added
static bool chain_valid;

// The album this queue is, when it is one. See playlist.h.
static char queue_album[256];

// What is on the card, so the write happens when the label moves and not on
// every queue that is built.
static char persisted_album[256];

// The label goes down with the queue rather than at the next track change. The
// two do not happen together: the queue is written the moment it is built, and
// the page says which record it is straight afterwards -- so a boot in the
// middle of the first track came back without it, which is exactly the case
// "play albums back to back" is for.
static void persist_album(void) {
	if (strcmp(persisted_album, queue_album) == 0) {
		return;
	}
	snprintf(persisted_album, sizeof(persisted_album), "%s", queue_album);
	BOOTPROF_CALL("library_queue_save_album", library_queue_save_album(queue_album));
}

void playlist_set_album(const char *album) {
	snprintf(queue_album, sizeof(queue_album), "%s", album ? album : "");
	persist_album();
}

// After a restore: the label came off the card, so putting it back is not a
// change to write down again.
void playlist_note_album_restored(const char *album) {
	snprintf(persisted_album, sizeof(persisted_album), "%s", album ? album : "");
}

const char *playlist_album(void) { return queue_album; }

bool playlist_is_junk_name(const char *name) {
	if (!name || !name[0]) {
		return true;
	}

	// The AppleDouble sidecars macOS leaves on a FAT or exFAT card: for every
	// "Song.flac" copied over, a four-kilobyte "._Song.flac" holding the
	// resource fork and the finder flags. The name ends in an audio extension
	// and the contents are not audio, so every list that goes by extension has
	// to know about them by name.
	if (name[0] == '.' && name[1] == '_') {
		return true;
	}

	// And the rest of what a desktop scatters over a card. Nothing here is a
	// track, and a folder of them is not a folder of music.
	static const char *const JUNK[] = {".DS_Store", "__MACOSX", ".Spotlight-V100", ".Trashes",
									   ".fseventsd", "$RECYCLE.BIN", "System Volume Information"};
	for (size_t i = 0; i < sizeof(JUNK) / sizeof(JUNK[0]); i++) {
		if (strcasecmp(name, JUNK[i]) == 0) {
			return true;
		}
	}
	return false;
}

bool playlist_is_playable_file(const char *name) {
	// .m4b and .m4a joined the list when AAC did: an audiobook is a file like
	// any other in the browser, and a folder of them has to queue.
	static const char *const playable_exts[] = {".wav",  ".mp3",  ".flac", ".ogg", ".m4b", ".m4a",
												 ".alac", ".aac",  ".dsf",  ".dff", ".aif", ".aiff",
												 ".aifc", ".caf",  ".opus", ".wv",   ".ape"};

	if (!name || playlist_is_junk_name(name)) {
		return false;
	}

	for (size_t i = 0; i < sizeof(playable_exts) / sizeof(playable_exts[0]); i++) {
		if (has_extension(name, playable_exts[i]))
			return true;
	}
	return false;
}

// How many long files the cue sheets of one folder may claim between them.
// One sheet per album is the normal case; a handful covers a box set kept in a
// single folder.
#define PLAYLIST_CUE_CLAIM_MAX 8

// A folder's track and its place on the record, while the folder is sorted.
typedef struct {
	char *path;
	folder_place_t place;
} placed_path_t;

// The browser's order (browser.c, entry_cmp): by disc and track number, then
// by name. Two tracks of one cue sheet go in the order the record is in, not
// in the order their virtual names spell: "?track=10" sorts between 1 and 2.
static int placed_path_cmp(const void *a, const void *b) {
	const placed_path_t *pa = a;
	const placed_path_t *pb = b;
	int by_place = folderorder_cmp(&pa->place, &pb->place);
	if (by_place) {
		return by_place;
	}
	int by_track = 0;
	if (cue_order_tracks(pa->path, pb->path, &by_track)) {
		return by_track;
	}
	return strcasecmp(pa->path, pb->path);
}

// Sorts the folder just loaded into `paths`. Files are not opened for their
// tags here -- this can run on the interface thread -- so a file the library
// has not indexed has the place the browser read for it when it listed the
// folder, or none, and goes by its name.
static void sort_folder(const char *folder) {
	if (entry_count < 2) {
		return;
	}
	placed_path_t *placed = calloc(entry_count, sizeof(*placed));
	if (!placed) {
		return; // out of memory: the queue keeps the order the card gave
	}
	const char **names = malloc(entry_count * sizeof(*names));
	folder_place_t *places = malloc(entry_count * sizeof(*places));
	if (names && places) {
		for (size_t i = 0; i < entry_count; i++) {
			const char *slash = strrchr(paths[i], '/');
			names[i] = slash ? slash + 1 : paths[i];
		}
		folderorder_places(folder, names, entry_count, false, places);
	}
	for (size_t i = 0; i < entry_count; i++) {
		placed[i].path = paths[i];
		if (names && places) {
			placed[i].place = places[i];
		}
	}
	qsort(placed, entry_count, sizeof(*placed), placed_path_cmp);
	for (size_t i = 0; i < entry_count; i++) {
		paths[i] = placed[i].path;
	}
	free(placed);
	free(names);
	free(places);
}

void playlist_clear(void) {
	for (size_t i = 0; i < appended_count(); i++) {
		free(paths ? paths[i] : NULL);
	}
	free(paths);
	free(order);
	library_index_close(source_ix);
	source_ix = NULL;
	ix_count = 0;
	path_cache_clear();
	paths = NULL;
	order = NULL;
	entry_count = 0;
	capacity = 0;
	pos = 0;
	custom_queue = false;
	ordered_queue = false;
	chain_valid = false;
	queue_album[0] = '\0'; // a new queue is not an album until somebody says so
	persist_album();
	revision++;
}

static bool push_path(const char *path) {
	size_t held = appended_count(); // how many are already in `paths`
	if (held == capacity) {
		size_t grown_cap = capacity ? capacity * 2 : 32;
		char **grown = realloc(paths, grown_cap * sizeof(*paths));
		if (!grown)
			return false; // keep the entries already gathered rather than crashing
		paths = grown;
		capacity = grown_cap;
	}

	paths[held] = strdup(path);
	if (!paths[held])
		return false;
	entry_count++;
	return true;
}

// ---------------------------------------------------------------------------
// the order layer
// ---------------------------------------------------------------------------

static void seed_once(void) {
	static bool seeded;
	if (!seeded) {
		seeded = true;
		srand((unsigned)time(NULL));
	}
}

// (Re)allocates `order` as the identity and points pos at `start_entry`.
static void order_identity(size_t start_entry) {
	free(order);
	order = malloc(entry_count * sizeof(*order));
	if (!order) {
		// Out of memory this deep is unrecoverable anyway, but the queue has to
		// be left describable: entry_count alone would drop below ix_count and
		// every "how many are appended" reading after it would wrap.
		for (size_t i = 0; i < appended_count(); i++) {
			free(paths[i]);
		}
		free(paths);
		paths = NULL;
		capacity = 0;
		entry_count = 0;
		ix_count = 0;
		library_index_close(source_ix);
		source_ix = NULL;
		path_cache_clear();
		return;
	}
	for (size_t i = 0; i < entry_count; i++) {
		order[i] = i;
	}
	pos = start_entry < entry_count ? start_entry : 0;
	revision++;
}

// Deals the shuffle over the whole queue and starts it from the entry it was
// started from, which goes to the front.
//
// Keeping the place the deal gives it would be wrong: at the moment of the deal
// nothing before it has been played, so the queue page would show hundreds of
// tracks nobody had heard above the first one. Above the playing track is what
// has been played, and it fills up as the listening goes on.
//
// The counter under the artwork does not come from here -- it shows
// playlist_current_entry(), the track's place in the list it came from, which a
// deal does not touch -- so starting at the front costs it nothing.
//
// Switching to shuffle with something already playing is a different question
// and has its own answer -- see order_shuffle_upcoming(), which deals only what
// is ahead and leaves the history alone.
static void order_shuffle_from(size_t current_entry) {
	if (!order || entry_count == 0) {
		return;
	}
	seed_once();

	for (size_t i = 0; i < entry_count; i++) {
		order[i] = i;
	}
	for (size_t i = entry_count - 1; i > 0; i--) {
		size_t j = (size_t)(rand() % (int)(i + 1)); // in [0, i]
		size_t tmp = order[i];
		order[i] = order[j];
		order[j] = tmp;
	}

	// And the one the user picked to the front. Swapped out of wherever the deal
	// put it rather than dealt around: taking one element out of a uniform
	// permutation and putting it first leaves the rest uniform, so the shuffle
	// is still a shuffle.
	for (size_t i = 0; i < entry_count; i++) {
		if (order[i] == current_entry) {
			order[i] = order[0];
			order[0] = current_entry;
			break;
		}
	}
	pos = 0;
	revision++;
}

// A fresh deal for the round that is starting, played from its first track.
//
// This is the end of a shuffled queue that repeats: what comes round is the
// shuffle, not the order it dealt last time. Hearing the same random sequence
// over and over is only random once.
static void order_shuffle_round(void) {
	if (!order || entry_count == 0) {
		return;
	}
	seed_once();

	for (size_t i = 0; i < entry_count; i++) {
		order[i] = i;
	}
	for (size_t i = entry_count - 1; i > 0; i--) {
		size_t j = (size_t)(rand() % (int)(i + 1));
		size_t tmp = order[i];
		order[i] = order[j];
		order[j] = tmp;
	}

	pos = 0;
	revision++;
}

// Deals the shuffle over what is still to come, leaving everything already
// played where it is.
//
// The order array is a permutation, so the tracks behind the playing one are
// order[0 .. pos-1] and the ones ahead are order[pos+1 ..]; shuffling only the
// tail keeps it a permutation and keeps the history. That history is what the
// queue page shows above the playing track; re-rooting the deal at zero -- what
// the full deal above does -- would throw it away and make the queue look as
// though it began at the current track.
static void order_shuffle_upcoming(void) {
	if (!order || entry_count < 3 || pos + 2 >= entry_count) {
		return; // nothing ahead but the next one, and one entry cannot be shuffled
	}
	seed_once();

	for (size_t i = entry_count - 1; i > pos + 1; i--) {
		size_t span = i - pos; // in [1, i - pos]
		size_t j = pos + 1 + (size_t)(rand() % (int)span);
		size_t tmp = order[i];
		order[i] = order[j];
		order[j] = tmp;
	}
	revision++;
}

// ---------------------------------------------------------------------------
// loading
// ---------------------------------------------------------------------------

// A folder's cue sheets, expanded into tracks, and the big files they claim.
//
// The same treatment the library gives them (library.c, scan_cue_sheet) and the
// same the browser gives them: a sheet describes one long file cut into tracks,
// so the queue holds the tracks and not the file. Without this the browser
// lists the tracks while the queue behind them is still the hour-long file.
//
// The sheet is parsed into a buffer allocated for the call: thirty-five
// kilobytes is too much for a stack, and this runs rarely enough that keeping
// one around costs more than it saves.
static size_t load_folder_cues(const char *folder, char claimed[][512], size_t claim_max) {
	DIR *dir = opendir(folder);
	if (!dir) {
		return 0;
	}

	cue_sheet_t *sheet = malloc(sizeof(*sheet));
	if (!sheet) {
		closedir(dir);
		return 0;
	}

	size_t claims = 0;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.' || !cue_is_sheet(de->d_name)) {
			continue;
		}
		char sheet_path[768];
		snprintf(sheet_path, sizeof(sheet_path), "%s/%s", folder, de->d_name);
		// A sheet over files split one per track leaves them as they are, as
		// the library and the browser do.
		if (!cue_parse(sheet_path, sheet) || sheet->split) {
			continue;
		}
		for (int t = 0; t < sheet->track_count; t++) {
			char virtual_path[900];
			cue_virtual_path(sheet_path, sheet->tracks[t].number, virtual_path, sizeof(virtual_path));
			if (!push_path(virtual_path)) {
				break;
			}
		}
		const char *slash = strrchr(sheet->audio_path, '/');
		if (claims < claim_max) {
			snprintf(claimed[claims], 512, "%s", slash ? slash + 1 : sheet->audio_path);
			claims++;
		}
	}

	free(sheet);
	closedir(dir);
	return claims;
}

void playlist_load_folder(const char *folder, const char *selected_filename) {
	playlist_clear();

	char claimed[PLAYLIST_CUE_CLAIM_MAX][512];
	size_t claims = load_folder_cues(folder, claimed, PLAYLIST_CUE_CLAIM_MAX);

	DIR *dir = opendir(folder);
	if (!dir)
		return;

	char full[768];
	struct dirent *de;
	errno = 0;
	while ((de = readdir(dir)) != NULL) {
		if (!playlist_is_playable_file(de->d_name))
			continue;
		bool skip = false;
		for (size_t i = 0; i < claims && !skip; i++) {
			skip = strcmp(claimed[i], de->d_name) == 0;
		}
		if (skip)
			continue; // a sheet in this folder already queued it, track by track
		snprintf(full, sizeof(full), "%s/%s", folder, de->d_name);
		if (!push_path(full))
			break;
	}
	// A readdir() that stops early leaves a queue missing most of the album,
	// which shows up as playback ending after the first track.
	if (errno != 0) {
		fprintf(stderr, "playlist: readdir stopped after %zu entries in '%s': %s\n", entry_count, folder,
				strerror(errno));
	}

	closedir(dir);

	sort_folder(folder);

	// Point the current position at the selected file, defaulting to the first.
	size_t start = 0;
	if (selected_filename) {
		for (size_t i = 0; i < entry_count; i++) {
			const char *slash = strrchr(paths[i], '/');
			const char *name = slash ? slash + 1 : paths[i];
			if (strcmp(name, selected_filename) == 0) {
				start = i;
				break;
			}
		}
	}

	order_identity(start);
	if (shuffling()) {
		order_shuffle_from(start);
	}
}

void playlist_load_paths(const char *const *list, int count, int start_index) {
	playlist_clear();

	for (int i = 0; i < count; i++) {
		if (list[i] && list[i][0]) {
			push_path(list[i]);
		}
	}

	custom_queue = true;

	size_t start = (start_index >= 0 && (size_t)start_index < entry_count) ? (size_t)start_index : 0;
	order_identity(start);
	if (shuffling()) {
		order_shuffle_from(start);
	}
}

void playlist_load_paths_ordered(const char *const *list, int count, int start_index) {
	playlist_load_paths(list, count, start_index);
	ordered_queue = true;
	// The list order is already the right one. playlist_load_paths left the
	// identity unless shuffle was on, in which case it has to be restored.
	size_t start = (start_index >= 0 && (size_t)start_index < entry_count) ? (size_t)start_index : 0;
	order_identity(start);
}

bool playlist_insert_next(const char *path) {
	if (!path || !path[0]) {
		return false;
	}

	// Nothing playing: there is no "after the current track" to speak of, so
	// the addition simply becomes the queue.
	if (entry_count == 0 || !order) {
		if (!push_path(path)) {
			return false;
		}
		custom_queue = true;
		order_identity(0);
		chain_valid = false;
		return true;
	}

	if (!push_path(path)) {
		return false;
	}
	size_t entry = entry_count - 1; // push_path appended it

	size_t *grown = realloc(order, entry_count * sizeof(*order));
	if (!grown) {
		// The path is in `paths` but has no slot in the order, which would
		// leave playlist_path_at() reading past the end. Drop it again.
		entry_count--;
		free(paths[appended_count()]);
		return false;
	}
	order = grown;

	// After the previous addition when the chain still holds, otherwise
	// straight after whatever is playing.
	size_t slot = (chain_valid && chain_from == pos && chain_slot > pos && chain_slot < entry_count) ? chain_slot + 1
																									 : pos + 1;
	if (slot > entry_count - 1) {
		slot = entry_count - 1; // append
	}

	// `entry_count` already counts the new entry, so the tail to move ends at
	// entry_count - 1.
	for (size_t i = entry_count - 1; i > slot; i--) {
		order[i] = order[i - 1];
	}
	order[slot] = entry;

	// Re-anchor the chain on the slot just filled. `pos` needs no adjustment for
	// the shift above, since slot is always greater than it.
	chain_from = pos;
	chain_slot = slot;
	chain_valid = true;

	// A folder queue would be rebuilt from the directory the next time
	// device_state resynced, taking the addition with it.
	custom_queue = true;
	revision++;
	return true;
}

bool playlist_insert_at(const char *path, int slot) {
	if (!path || !path[0]) {
		return false;
	}

	if (entry_count == 0 || !order) {
		if (!push_path(path)) {
			return false;
		}
		custom_queue = true;
		order_identity(0);
		chain_valid = false;
		return true;
	}

	if (!push_path(path)) {
		return false;
	}
	size_t entry = entry_count - 1;

	size_t *grown = realloc(order, entry_count * sizeof(*order));
	if (!grown) {
		entry_count--;
		free(paths[appended_count()]);
		return false;
	}
	order = grown;

	size_t at = slot < 0 ? 0 : (size_t)slot;
	if (at > entry_count - 1) {
		at = entry_count - 1; // past the end: append
	}

	for (size_t i = entry_count - 1; i > at; i--) {
		order[i] = order[i - 1];
	}
	order[at] = entry;

	// Everything from `at` onwards moved up a slot, so a playback position at or
	// after it points at the track before the one it was on. Unlike
	// playlist_insert_next(), which can only ever insert ahead of playback, this
	// one has to follow it.
	if (at <= pos) {
		pos++;
	}

	// Not a queued track following another: the chain belongs to what the user
	// does, and a restore must not leave the next "add to queue" hanging off the
	// last row of a saved list.
	chain_valid = false;

	custom_queue = true;
	revision++;
	return true;
}

bool playlist_enqueue_at_end(void) { return config_get_int("player", "queue_add_end", 0) != 0; }

void playlist_set_enqueue_at_end(bool at_end) {
	config_set_int("player", "queue_add_end", at_end ? 1 : 0);
	config_save();
}

bool playlist_enqueue(const char *path) {
	if (!playlist_enqueue_at_end()) {
		return playlist_insert_next(path);
	}
	// A slot past the last appends: the end of the queue in playing order,
	// which with shuffle on is the end of the order dealt. Ahead of playback
	// unless the queue is empty, when the track simply becomes the queue.
	return playlist_insert_at(path, INT_MAX);
}

static void refresh_stale_source(void);

typedef struct {
	char **out;
	size_t want;
	size_t got;
} collect_t;

static bool collect_cb(const char *name, const char *path, const char *artist, void *user) {
	(void)name;
	(void)artist;
	collect_t *c = user;
	if (c->got >= c->want) {
		return false;
	}
	c->out[c->got] = strdup(path ? path : "");
	if (!c->out[c->got]) {
		return false;
	}
	c->got++;
	return true;
}

// Rebuilds the queue as a list of paths in the order it is played, without the
// entries `gone` marks (by where they are stored), and with playback on the
// track it was on. Takes `gone`, and frees it whatever happens.
static bool rebuild_as_paths(uint8_t *gone) {
	// Every entry becomes a path: the rows of a library list are read in one
	// pass rather than one lookup each. What is left of the queue is no longer
	// that list, so it is not held as one either.
	size_t total = entry_count;
	char **stored = calloc(total, sizeof(*stored));
	size_t *old_order = malloc(total * sizeof(*old_order));
	bool ok = stored && old_order;
	if (ok && ix_count > 0) {
		collect_t c = {stored, ix_count, 0};
		library_index_window(source_ix, 0, (int)ix_count, collect_cb, &c);
		ok = c.got == ix_count;
	}
	for (size_t i = ix_count; ok && i < total; i++) {
		stored[i] = strdup(paths[i - ix_count]);
		ok = stored[i] != NULL;
	}
	if (!ok) {
		for (size_t i = 0; stored && i < total; i++) {
			free(stored[i]);
		}
		free(stored);
		free(old_order);
		free(gone);
		return false;
	}
	memcpy(old_order, order, total * sizeof(*old_order));
	size_t old_pos = pos;

	// The new place of every entry that stays, in the order they are stored.
	size_t *kept_at = malloc(total * sizeof(*kept_at));
	if (!kept_at) {
		for (size_t i = 0; i < total; i++) {
			free(stored[i]);
		}
		free(stored);
		free(old_order);
		free(gone);
		return false;
	}

	bool was_ordered = ordered_queue;
	char album[sizeof(queue_album)];
	snprintf(album, sizeof(album), "%s", queue_album);

	playlist_clear();
	size_t kept = 0;
	for (size_t i = 0; i < total; i++) {
		kept_at[i] = (size_t)-1;
		// A row of the library gone meanwhile comes back blank.
		if (gone[i] || !stored[i][0]) {
			continue;
		}
		if (push_path(stored[i])) {
			kept_at[i] = kept++;
		}
	}
	custom_queue = true;
	ordered_queue = was_ordered;
	order_identity(0);
	if (order && kept == entry_count) {
		// The order the queue was in, without what was taken out of it.
		size_t at = 0;
		for (size_t i = 0; i < total; i++) {
			size_t entry = old_order[i];
			if (gone[entry] || kept_at[entry] == (size_t)-1) {
				continue;
			}
			if (i == old_pos) {
				pos = at;
			}
			order[at++] = kept_at[entry];
		}
	}
	revision++;
	playlist_set_album(album);

	for (size_t i = 0; i < total; i++) {
		free(stored[i]);
	}
	free(stored);
	free(old_order);
	free(kept_at);
	free(gone);
	return true;
}

bool playlist_remove(const int *indices, int count) {
	if (!indices || count <= 0 || !order || entry_count == 0) {
		return false;
	}
	refresh_stale_source();

	// Which entries go, by where they are stored. The one playing stays.
	uint8_t *gone = calloc(entry_count, 1);
	if (!gone) {
		return false;
	}
	size_t removed = 0;
	for (int i = 0; i < count; i++) {
		int at = indices[i];
		if (at < 0 || (size_t)at >= entry_count || (size_t)at == pos || gone[order[at]]) {
			continue;
		}
		gone[order[at]] = 1;
		removed++;
	}
	if (removed == 0) {
		free(gone);
		return false;
	}

	return rebuild_as_paths(gone);
}

bool playlist_move(int from, int to) {
	if (!order || from < 0 || to < 0 || (size_t)from >= entry_count || (size_t)to >= entry_count || from == to) {
		return false;
	}
	// The library list under the queue read again: the positions the caller
	// counted may stand for other tracks now.
	unsigned seen = revision;
	refresh_stale_source();
	if (revision != seen) {
		return false;
	}

	// An order of its own is something the query a library list is kept as
	// cannot say: the queue becomes the paths, as a removal makes it.
	if (ix_count > 0) {
		size_t before = entry_count;
		uint8_t *none = calloc(entry_count, 1);
		if (!none || !rebuild_as_paths(none)) {
			return false;
		}
		// A row of the library that has gone meanwhile was dropped, and the
		// positions the caller counted are off by it.
		if (entry_count != before) {
			return false;
		}
	}

	size_t entry = order[from];
	if (from < to) {
		memmove(&order[from], &order[from + 1], (size_t)(to - from) * sizeof(*order));
	} else {
		memmove(&order[to + 1], &order[to], (size_t)(from - to) * sizeof(*order));
	}
	order[to] = entry;

	// Playback stays on its track, wherever that has gone.
	size_t f = (size_t)from, t = (size_t)to;
	if (pos == f) {
		pos = t;
	} else if (f < pos && t >= pos) {
		pos--;
	} else if (f > pos && t <= pos) {
		pos++;
	}

	chain_valid = false;
	custom_queue = true;
	revision++;
	return true;
}

bool playlist_is_custom(void) { return custom_queue; }

void playlist_set_custom(bool custom) { custom_queue = custom; }

unsigned playlist_revision(void) { return revision; }

// How far either side of where playback is a library-backed queue will look
// for a track. A queue of paths is walked whole -- the strings are right there
// -- but on a handle every step is a query, and a queue can now be as long as
// the library. The answer, when there is one, is almost always a step or two
// away: this runs when the loaded track and the queue disagree, which happens
// because something moved the track, not because it teleported.
#define LOCATE_RADIUS 512

bool playlist_load_index(library_index_t *ix, int start) {
	int count = library_index_count(ix);
	if (!ix || count <= 0) {
		library_index_close(ix);
		return false;
	}

	playlist_clear();
	source_ix = ix; // ours now
	ix_count = (size_t)count;
	entry_count = ix_count;
	custom_queue = true; // a library list, not the folder a file happens to be in

	if (start < 0 || start >= count) {
		start = 0;
	}
	order_identity((size_t)start);
	if (shuffling()) {
		order_shuffle_from((size_t)start);
	}
	if (!order) {
		playlist_clear();
		return false;
	}
	revision++;
	return true;
}

bool playlist_is_library_backed(void) { return source_ix != NULL; }

size_t playlist_current_entry(void) { return (order && entry_count) ? order[pos] : 0; }

int playlist_appended_count(void) { return (int)appended_count(); }

bool playlist_appended_at(int index, char *out, size_t out_size) {
	if (index < 0 || (size_t)index >= appended_count() || !paths || !out) {
		return false;
	}
	snprintf(out, out_size, "%s", paths[index]);
	return true;
}

int playlist_appended_slots(int *slots, int max) {
	int count = (int)appended_count();
	if (count > max) {
		count = max;
	}
	if (!slots || count <= 0) {
		return count > 0 ? count : 0;
	}

	for (int i = 0; i < count; i++) {
		slots[i] = -1; // not in the order at all, which should not happen
	}
	if (!order) {
		return count;
	}

	// One walk of the order rather than one search per entry: this is called
	// every time the queue is written down, which is every track change, and on
	// a library-sized queue a search each would be a hundred thousand
	// comparisons per added track.
	for (size_t s = 0; s < entry_count; s++) {
		size_t entry = order[s];
		if (entry < ix_count) {
			continue; // a row of the library list, not an addition
		}
		size_t appended = entry - ix_count;
		if (appended < (size_t)count) {
			slots[appended] = (int)s;
		}
	}
	return count;
}

bool playlist_library_spec(library_index_spec_t *out) {
	if (!source_ix) {
		if (out) {
			out->valid = false;
		}
		return false;
	}
	// Anything added to the queue afterwards is not in the query; the caller
	// asks for it separately (playlist_appended_at) rather than falling back to
	// writing out every path, which on a library-sized queue is the one thing
	// that must not happen.
	library_index_describe(source_ix, out);
	return out && out->valid;
}

bool playlist_locate(const char *path) {
	if (!path || !path[0] || !order) {
		return false;
	}

	if (!source_ix) {
		for (size_t i = 0; i < entry_count; i++) {
			if (strcmp(paths[order[i]], path) == 0) {
				pos = i;
				return true;
			}
		}
		return false;
	}

	// Outwards from where playback is, and only so far.
	for (size_t step = 0; step <= LOCATE_RADIUS; step++) {
		size_t candidates[2];
		int n = 0;
		if (pos + step < entry_count) {
			candidates[n++] = pos + step;
		}
		if (step > 0 && pos >= step) {
			candidates[n++] = pos - step;
		}
		if (n == 0) {
			break;
		}
		for (int i = 0; i < n; i++) {
			const char *at = entry_path(order[candidates[i]]);
			if (at && strcmp(at, path) == 0) {
				pos = candidates[i];
				return true;
			}
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// reading (everything is in playback order: index i = i-th slot of `order`)
// ---------------------------------------------------------------------------

int playlist_count(void) { return (int)entry_count; }

int playlist_current_index(void) { return entry_count ? (int)pos : -1; }

// A queue built on row ids goes stale when the table under it is rewritten --
// a rescan, a card taken out and put back. Left alone, next and prev would
// simply stop working in the middle of an album, so the query is run again and
// playback holds its place by path rather than by number.
static void refresh_stale_source(void) {
	if (!source_ix || !library_index_stale(source_ix)) {
		return;
	}

	library_index_spec_t spec;
	library_index_describe(source_ix, &spec);

	// What is playing, read out before the numbers underneath it change. Read
	// properly rather than from the cache: it is the only thing that survives a
	// rebuild, since every index into the old list is about to mean something
	// else.
	char playing[512] = {0};
	if (order && entry_count) {
		const char *at = entry_path(order[pos]);
		if (at) {
			snprintf(playing, sizeof(playing), "%s", at);
		}
	}

	library_index_t *fresh = NULL;
	if (spec.valid) {
		fresh = library_index_open(spec.kind, spec.filter, spec.value, spec.order, spec.desc);
	}
	int fresh_count = library_index_count(fresh);
	if (fresh_count <= 0) {
		// The list this queue was is not there any more. Better an empty queue
		// than one that answers with somebody else's tracks.
		library_index_close(fresh);
		playlist_clear();
		return;
	}

	// Where that track sits in the rebuilt list: an indexed lookup and a walk
	// of an array of ints, not the bounded outward search playlist_locate does
	// -- after a rescan the track can have moved anywhere.
	int start = playing[0] ? library_index_find_path(fresh, playing) : -1;
	// Nothing could be read of the old list, not even the track the queue was
	// just started on: the same entry of the list read again, which is the
	// track that was asked for when the list itself has not changed.
	if (start < 0 && !playing[0] && order && entry_count && order[pos] < (size_t)fresh_count) {
		start = (int)order[pos];
	}

	size_t appended = appended_count();
	library_index_close(source_ix);
	source_ix = fresh;
	ix_count = (size_t)fresh_count;
	entry_count = ix_count + appended;
	path_cache_clear();

	// A storage index, which is what these two take. `pos` is an index into the
	// order, and after a fresh shuffle deal the two have nothing to do with
	// each other.
	size_t start_entry = start >= 0 ? (size_t)start : 0;
	order_identity(start_entry);
	if (shuffling()) {
		order_shuffle_from(start_entry);
	}
	revision++;
}

bool playlist_path_at(int index, char *out, size_t out_size) {
	refresh_stale_source();
	if (index < 0 || (size_t)index >= entry_count || !out || !order)
		return false;
	const char *path = entry_path(order[index]);
	if (!path) {
		return false;
	}
	snprintf(out, out_size, "%s", path);
	return true;
}

bool playlist_set_current(int index) {
	if (index < 0 || (size_t)index >= entry_count)
		return false;
	pos = (size_t)index;
	return true;
}

bool playlist_current_path(char *out, size_t out_size) {
	refresh_stale_source();
	if (entry_count == 0 || !out || !order)
		return false;
	const char *path = entry_path(order[pos]);
	if (!path) {
		return false;
	}
	snprintf(out, out_size, "%s", path);
	return true;
}

// ---------------------------------------------------------------------------
// advancing
// ---------------------------------------------------------------------------

bool playlist_at_end(void) { return entry_count == 0 || pos + 1 >= entry_count; }

bool playlist_advance_auto(char *out, size_t out_size) {
	if (entry_count == 0)
		return false;

	// A queue that carries its own order is played straight through: one step
	// forward, then stop. Shuffle and repeat mean nothing on a sequence of
	// episodes, which is also why the player hides that button during a
	// podcast.
	if (ordered_queue) {
		if (pos + 1 >= entry_count) {
			return false;
		}
		pos++;
		return playlist_current_path(out, out_size);
	}

	switch (mode) {
	case PLAYBACK_MODE_REPEAT_ONE:
		break; // replay the same track
	case PLAYBACK_MODE_REPEAT_ALL:
		pos = (pos + 1) % entry_count;
		break;
	case PLAYBACK_MODE_SHUFFLE:
		// Once through the deal, then stop -- the same ending normal play has,
		// in a different order. Going round again is what the mode after this
		// one is for.
		if (pos + 1 >= entry_count) {
			return false;
		}
		pos++;
		break;
	case PLAYBACK_MODE_SHUFFLE_REPEAT:
		// And round again -- on a new deal. What repeats is the shuffling, not
		// the sequence it produced: the same random order for ever is random
		// exactly once.
		if (pos + 1 >= entry_count) {
			order_shuffle_round();
		} else {
			pos++;
		}
		break;
	case PLAYBACK_MODE_NORMAL:
	default:
		if (pos + 1 >= entry_count)
			return false; // reached the end of the queue; stop
		pos++;
		break;
	}

	return playlist_current_path(out, out_size);
}

bool playlist_peek_auto(char *out, size_t out_size) {
	if (entry_count == 0 || !out) {
		return false;
	}
	size_t next;
	if (ordered_queue) {
		if (pos + 1 >= entry_count) {
			return false;
		}
		next = pos + 1;
	} else {
		switch (mode) {
		case PLAYBACK_MODE_REPEAT_ONE:
			next = pos;
			break;
		case PLAYBACK_MODE_REPEAT_ALL:
			next = (pos + 1) % entry_count;
			break;
		case PLAYBACK_MODE_SHUFFLE:
		case PLAYBACK_MODE_SHUFFLE_REPEAT:
		case PLAYBACK_MODE_NORMAL:
		default:
			// Past the end of a shuffle-repeat deal comes a new deal, which
			// is not dealt yet: nothing to say about it.
			if (pos + 1 >= entry_count) {
				return false;
			}
			next = pos + 1;
			break;
		}
	}
	return playlist_path_at((int)next, out, out_size);
}

bool playlist_next(char *out, size_t out_size) {
	if (entry_count == 0)
		return false;
	// Pressing next past the end of a shuffled deal asks for more, so it deals
	// again -- unlike a track ending there, which stops (or comes round, in
	// shuffle-repeat). A button pressed on purpose is not the end of a queue.
	if (shuffling() && pos + 1 >= entry_count) {
		// Pressing next past the end of a deal asks for more, so a new one is
		// dealt and played from its start.
		order_shuffle_round();
	} else {
		pos = (pos + 1) % entry_count;
	}
	return playlist_current_path(out, out_size);
}

bool playlist_prev(char *out, size_t out_size) {
	if (entry_count == 0)
		return false;
	pos = (pos + entry_count - 1) % entry_count;
	return playlist_current_path(out, out_size);
}

// ---------------------------------------------------------------------------
// modes
// ---------------------------------------------------------------------------

static void apply_mode_change(playback_mode_t old_mode) {
	if (entry_count == 0 || mode == old_mode) {
		return;
	}

	if (ordered_queue) {
		return; // the queue carries its own order; a mode change cannot touch it
	}

	bool shuffling = mode == PLAYBACK_MODE_SHUFFLE || mode == PLAYBACK_MODE_SHUFFLE_REPEAT;
	bool was_shuffling = old_mode == PLAYBACK_MODE_SHUFFLE || old_mode == PLAYBACK_MODE_SHUFFLE_REPEAT;

	if (shuffling && !was_shuffling) {
		// Deal the random order now, so the queue page can show exactly what is
		// coming. Only what is coming: the tracks already played keep their
		// places, because the page shows those too and a switch of mode is not
		// a reason to forget where the listening has been.
		order_shuffle_upcoming();
	} else if (was_shuffling && !shuffling) {
		// Back to the listed order, without moving off the playing track.
		size_t current_entry = order ? order[pos] : 0;
		order_identity(current_entry);
	}
	// Between the two shuffled modes nothing is re-dealt: the difference is
	// only what happens at the end of the deal.
}

// The chosen mode survives a power cycle: saved on every change, applied
// again at boot by main().
static void mode_save(void) {
	config_set_int("player", "playback_mode", (int)mode);
	config_save();
}

void playlist_set_mode(playback_mode_t new_mode) {
	playback_mode_t old = mode;
	mode = new_mode;
	apply_mode_change(old);
	mode_save();
}

playback_mode_t playlist_get_mode(void) { return mode; }

playback_mode_t playlist_cycle_mode(void) {
	playback_mode_t old = mode;
	switch (mode) {
	case PLAYBACK_MODE_NORMAL:
		mode = PLAYBACK_MODE_REPEAT_ALL;
		break;
	case PLAYBACK_MODE_REPEAT_ALL:
		mode = PLAYBACK_MODE_REPEAT_ONE;
		break;
	case PLAYBACK_MODE_REPEAT_ONE:
		mode = PLAYBACK_MODE_SHUFFLE;
		break;
	case PLAYBACK_MODE_SHUFFLE:
		mode = PLAYBACK_MODE_SHUFFLE_REPEAT;
		break;
	case PLAYBACK_MODE_SHUFFLE_REPEAT:
	default:
		mode = PLAYBACK_MODE_NORMAL;
		break;
	}
	apply_mode_change(old);
	mode_save();
	return mode;
}

// ---------------------------------------------------------------------------
// Walking the card, folder after folder
//
// What "play by folder" needs when a folder's tracks run out: the folder that
// comes next. The order is a depth-first walk in which a folder's own files
// come AFTER its subfolders, because that is what makes the common layout read
// the way people expect it to:
//
//     Music/Artist/Album 1     <- start a track in here
//     Music/Artist/Album 2     <- ...and this follows
//     Music/Artist             <- ...then the artist's loose singles
//     Music/Artist 2/Album A   <- and only then the next artist
//     Music/Artist 2
//
// A folder's own files coming first (the other way round) would mean leaving
// the artist behind the moment their last record finished, with the singles
// stranded until the card came round again.
//
// It is not computed by listing the card: the successor of one folder is found
// by reading that folder's parent and, at most, descending one chain of first
// subfolders. A card of ten thousand folders costs the same as a card of ten.
// ---------------------------------------------------------------------------

// The root the walk is allowed to move around in: the card, as the browser
// sees it. Set once at startup rather than asked of the storage layer, because
// the two do not agree on the host build -- and because a walk that could leave
// the card is not a walk anybody asked for.
static char card_root[512];

void playlist_set_card_root(const char *root) { snprintf(card_root, sizeof(card_root), "%s", root ? root : ""); }

const char *playlist_card_root(void) { return card_root; }

// How many folders are looked at before giving up. A card whose music sits in
// one folder among four thousand empty ones is not worth walking to the end of
// on every track change.
#define FOLDER_WALK_MAX 4096

// By name, the way the browser lists them.
static int name_cmp(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

// The subdirectories of `dir`, by name, junk left out. Returns how many were
// written; `out` holds bare names, not paths.
static int subdirs_of(const char *dir, char (*out)[256], int max) {
	DIR *d = opendir(dir);
	if (!d) {
		return 0;
	}

	int count = 0;
	struct dirent *de;
	while (count < max && (de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
			continue;
		}
		if (de->d_name[0] == '.' || playlist_is_junk_name(de->d_name)) {
			continue;
		}
		char full[768];
		if (snprintf(full, sizeof(full), "%s/%s", dir, de->d_name) >= (int)sizeof(full)) {
			continue;
		}
		struct stat st;
		if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) {
			continue;
		}
		snprintf(out[count], sizeof(out[0]), "%s", de->d_name);
		count++;
	}
	closedir(d);

	qsort(out, (size_t)count, sizeof(out[0]), name_cmp);
	return count;
}

// Whether `dir` holds at least one track this player can open. Only the folder
// itself: its subfolders are separate stops on the walk.
static bool folder_has_tracks(const char *dir) {
	DIR *d = opendir(dir);
	if (!d) {
		return false;
	}
	bool found = false;
	struct dirent *de;
	while (!found && (de = readdir(d)) != NULL) {
		found = playlist_is_playable_file(de->d_name);
	}
	closedir(d);
	return found;
}

// The first stop of the walk inside `dir`: the deepest first subfolder, since a
// folder's own files come after everything below it.
static void descend_to_first(char *dir, size_t size) {
	for (int depth = 0; depth < 16; depth++) {
		char (*names)[256] = malloc(sizeof(*names) * 64);
		if (!names) {
			return;
		}
		int count = subdirs_of(dir, names, 64);
		if (count == 0) {
			free(names);
			return;
		}
		size_t len = strlen(dir);
		if (len + 1 + strlen(names[0]) >= size) {
			free(names);
			return;
		}
		snprintf(dir + len, size - len, "/%s", names[0]);
		free(names);
	}
}

// One step of the walk: the folder that comes after `dir`, whether or not it
// holds any music. False once `dir` is the root itself, which is the last stop.
static bool walk_step(const char *root, const char *dir, char *out, size_t out_size) {
	if (strcmp(dir, root) == 0) {
		return false; // the root's own files are the end of the walk
	}

	char parent[512];
	snprintf(parent, sizeof(parent), "%s", dir);
	char *slash = strrchr(parent, '/');
	if (!slash || slash == parent) {
		return false;
	}
	*slash = '\0';
	const char *name = slash + 1;

	char (*names)[256] = malloc(sizeof(*names) * 256);
	if (!names) {
		return false;
	}
	int count = subdirs_of(parent, names, 256);

	int at = -1;
	for (int i = 0; i < count; i++) {
		if (strcmp(names[i], name) == 0) {
			at = i;
			break;
		}
	}

	bool ok = true;
	if (at >= 0 && at + 1 < count) {
		// The next sibling, entered at its own first stop. A name that does not
		// fit would be a path pointing at the wrong folder, so it is refused
		// rather than truncated -- and the walk carries on from the parent.
		if (snprintf(out, out_size, "%s/%s", parent, names[at + 1]) >= (int)out_size) {
			ok = false;
		} else {
			descend_to_first(out, out_size);
		}
	} else {
		// The last of them: the parent's own files come next.
		ok = snprintf(out, out_size, "%s", parent) < (int)out_size;
	}

	free(names);
	if (!ok) {
		return false;
	}
	return true;
}

// One step backwards: the folder that comes before `dir`. The mirror of
// walk_step(), and it falls out of the same rule -- a folder is visited after
// its subfolders, so:
//
//   * the stop before a folder that HAS subfolders is its last subfolder (that
//     subfolder is the last thing in its own subtree, so nothing of it comes
//     between the two);
//   * otherwise it is the previous sibling, for the same reason;
//   * and a first child has nothing before it inside its parent, so the walk
//     climbs until it finds an ancestor with a previous sibling.
//
// False once there is nothing before `dir` at all, which is the first stop of
// the whole card.
static bool walk_step_back(const char *root, const char *dir, char *out, size_t out_size) {
	char (*names)[256] = malloc(sizeof(*names) * 256);
	if (!names) {
		return false;
	}

	bool ok = false;
	int count = subdirs_of(dir, names, 256);
	if (count > 0) {
		ok = snprintf(out, out_size, "%s/%s", dir, names[count - 1]) < (int)out_size;
		free(names);
		return ok;
	}

	// Up the tree, looking for something before us.
	char at[512];
	snprintf(at, sizeof(at), "%s", dir);
	for (int depth = 0; depth < 16; depth++) {
		if (strcmp(at, root) == 0) {
			break; // the root has nothing before it
		}
		char parent[512];
		snprintf(parent, sizeof(parent), "%s", at);
		char *slash = strrchr(parent, '/');
		if (!slash || slash == parent) {
			break;
		}
		*slash = '\0';
		const char *name = slash + 1;

		count = subdirs_of(parent, names, 256);
		int index = -1;
		for (int i = 0; i < count; i++) {
			if (strcmp(names[i], name) == 0) {
				index = i;
				break;
			}
		}
		if (index > 0) {
			ok = snprintf(out, out_size, "%s/%s", parent, names[index - 1]) < (int)out_size;
			break;
		}
		// The first of them: keep climbing.
		snprintf(at, sizeof(at), "%s", parent);
	}

	free(names);
	return ok;
}

bool playlist_prev_folder(const char *root, const char *folder, char *out, size_t out_size) {
	if (!root || !root[0] || !folder || !folder[0] || !out || out_size == 0) {
		return false;
	}
	size_t root_len = strlen(root);
	if (strncmp(folder, root, root_len) != 0 || (folder[root_len] != '\0' && folder[root_len] != '/')) {
		return false;
	}

	char at[512];
	snprintf(at, sizeof(at), "%s", folder);

	for (int steps = 0; steps < FOLDER_WALK_MAX; steps++) {
		char prev[512];
		if (!walk_step_back(root, at, prev, sizeof(prev))) {
			// Before the first stop: round to the last one, which is the root
			// itself -- its own files are what the walk ends on.
			snprintf(prev, sizeof(prev), "%s", root);
		}

		if (strcmp(prev, folder) == 0) {
			return false; // all the way round: this folder is the only music there is
		}
		if (folder_has_tracks(prev)) {
			snprintf(out, out_size, "%s", prev);
			return true;
		}
		snprintf(at, sizeof(at), "%s", prev);
	}

	fprintf(stderr, "playlist: %d folders before '%s' and none of them holds a track\n", FOLDER_WALK_MAX, folder);
	return false;
}

bool playlist_next_folder(const char *root, const char *folder, char *out, size_t out_size) {
	if (!root || !root[0] || !folder || !folder[0] || !out || out_size == 0) {
		return false;
	}
	// Only what is under the card. A queue built somewhere else has no walk to
	// continue along.
	size_t root_len = strlen(root);
	if (strncmp(folder, root, root_len) != 0 || (folder[root_len] != '\0' && folder[root_len] != '/')) {
		return false;
	}

	char at[512];
	snprintf(at, sizeof(at), "%s", folder);

	for (int steps = 0; steps < FOLDER_WALK_MAX; steps++) {
		char next[512];
		if (!walk_step(root, at, next, sizeof(next))) {
			// Past the end: round to the first stop of the whole card.
			snprintf(next, sizeof(next), "%s", root);
			descend_to_first(next, sizeof(next));
		}

		if (strcmp(next, folder) == 0) {
			return false; // all the way round: this folder is the only music there is
		}
		if (folder_has_tracks(next)) {
			snprintf(out, out_size, "%s", next);
			return true;
		}
		snprintf(at, sizeof(at), "%s", next);
	}

	fprintf(stderr, "playlist: %d folders after '%s' and none of them holds a track\n", FOLDER_WALK_MAX, folder);
	return false;
}
