#ifndef PLAYLIST_H
#define PLAYLIST_H

#include "src/system/library/library.h"

#include <stdbool.h>
#include <stddef.h>

// How playback proceeds when a track finishes.
typedef enum {
	PLAYBACK_MODE_NORMAL,	  // play through the queue, then stop
	PLAYBACK_MODE_REPEAT_ALL, // loop the whole queue
	PLAYBACK_MODE_REPEAT_ONE, // loop the current track
	PLAYBACK_MODE_SHUFFLE,	  // random order within the queue, once through
	// Shuffle that keeps going: at the end of the deal the queue is dealt again
	// rather than stopping. A new deal each round and not the old one over
	// again -- what repeats is the shuffling, and the same random sequence for
	// ever is random exactly once. Added last so the numbers the earlier modes
	// are saved under do not move.
	PLAYBACK_MODE_SHUFFLE_REPEAT,
} playback_mode_t;

// Builds the queue from every playable file in `folder`, ordered the same way
// the browser lists them (case-insensitive alphabetical). `selected_filename`
// is the bare name (not a full path) that becomes the current track; if it
// isn't found, the current index falls back to 0. The playback mode is left
// unchanged. This is a *folder* queue: device_state may rebuild it from the
// disk when it drifts.
void playlist_load_folder(const char *folder, const char *selected_filename);

// Builds the queue from an explicit list of full paths -- what the library
// index pages hand over, so next/prev walk the list the track was picked
// from, not its folder. `start_index` is the entry to start at. This is a
// *custom* queue: it is never replaced behind the user's back.
void playlist_load_paths(const char *const *list, int count, int start_index);

// The same, for a queue whose order is part of its meaning: podcast episodes,
// newest first going back in time. Neither shuffle nor repeat touches it --
// shuffling a sequence breaks it rather than reordering it. Stops at the end,
// like a normal queue.
void playlist_load_paths_ordered(const char *const *list, int count, int start_index);

// Puts `path` into the queue right after the track playing now ("add to
// queue"). Adding a second track puts it after the first one added, a third
// after the second, and so on -- tracks queued this way keep the order they
// were chosen in instead of each one jumping the last.
//
// The chain resets as soon as playback moves on: once the queued tracks start
// coming up, the next addition goes right after whatever is playing then,
// which is where the user is looking.
//
// The queue becomes a custom one, because a folder queue can be rebuilt from
// the directory at any moment and that would quietly throw the addition away.
// Returns false only when the track could not be stored (out of memory).
bool playlist_insert_next(const char *path);

// "Add to queue" from the library, the search and the browser, where the
// user's setting puts it (Music settings > Playback options, [player]
// queue_add_end): right after the track playing, as playlist_insert_next(), or
// at the end of the queue, after everything already in it. Several added one
// after another keep the order they were added in either way. Returns false
// only when the track could not be stored (out of memory).
bool playlist_enqueue(const char *path);

// Whether playlist_enqueue() adds at the end. Read and written through the
// configuration, so the setting page and the queue agree without a cache.
bool playlist_enqueue_at_end(void);
void playlist_set_enqueue_at_end(bool at_end);

// The same, but at a slot chosen by the caller rather than after the current
// track. For putting a saved queue back exactly as it was left.
//
// It is a separate function and not a parameter because the two have opposite
// rules. "Add to queue" may only ever put something ahead of playback; this one
// must be able to put it behind, and when it does, playback stays on the track
// it is on -- which means `pos` moves with the shift. It also breaks the chain
// rather than extending it: restoring a queue is not the user queuing a track.
//
// A slot past the end appends. Returns false only when the track could not be
// stored (out of memory).
bool playlist_insert_at(const char *path, int slot);

// Takes the entries at `indices` (positions in playback order) out of the
// queue. The one playing is never taken out, and the others keep their order.
// The queue becomes a custom list of paths. False when nothing was taken out.
bool playlist_remove(const int *indices, int count);

// Moves the entry at `from` to `to` (positions in playback order; `to` is
// where it ends up). Playback stays on the track it is on, and the queue
// becomes a custom list of paths, as with a removal. False when either
// position is out of range, they are the same, or the library list under the
// queue changed meanwhile and the positions no longer stand for the same tracks.
bool playlist_move(int from, int to);

// True while the queue is a custom list rather than a folder.
bool playlist_is_custom(void);

// Whether the queue is sitting on its last entry, in queue order. Says nothing
// about what happens next -- that is the playback mode's business -- only that
// there is nothing after this one. Album chaining needs to know it before the
// mode gets a chance to wrap back to the top.
bool playlist_at_end(void);

// Whether a file name is one this player can play, by extension. The queue's
// own test, exposed because a playlist has to apply the same rule: an entry
// naming something unplayable is a row that does nothing.
bool playlist_is_playable_file(const char *name);

// The card, as the browser sees it: the only tree the folder walk below is
// allowed to move around in. Set once at startup from the same path the browser
// and the playlists are given.
void playlist_set_card_root(const char *root);
const char *playlist_card_root(void);

// The folder that follows `folder` when a folder queue runs out, for the "play
// by folder" option. The walk is depth-first with a folder's own files coming
// AFTER its subfolders, so an artist's albums are followed by that artist's
// loose singles and only then by the next artist; past the last folder it comes
// round to the first. Only folders that hold at least one playable track are
// offered. False when `folder` is not under `root`, when nothing else on the
// card holds music, or when the walk came all the way back to where it started.
bool playlist_next_folder(const char *root, const char *folder, char *out, size_t out_size);

// The other direction, for the prev key: the folder before `folder` on the same
// walk, coming round to the last one from the first. Same rules, same refusals.
bool playlist_prev_folder(const char *root, const char *folder, char *out, size_t out_size);

// Whether a name is something a desktop left behind rather than something to
// play or to walk into: macOS's "._Song.flac" resource forks above all, which
// carry an audio extension and four kilobytes of nothing. Exposed because every
// list that reads a folder has to skip the same set.
bool playlist_is_junk_name(const char *name);

// Which album the queue is, when it was built from one on the Album page --
// empty otherwise. It exists for continuous album playback: when the last
// track runs out, that option needs to know which record just ended in order
// to pick the one after it. Set right after the queue is loaded (the loaders
// clear it, so a folder queue or a plain list never claims to be an album).
//
// The label is written down beside the queue as soon as it is set, so that a
// reboot brings it back with the queue. Without that the option stays dead
// until a record is started by hand again.
void playlist_set_album(const char *album);
const char *playlist_album(void);

// Called by the boot restore with the label the card holds, before it is put
// back, so that putting it back is not itself written down again.
void playlist_note_album_restored(const char *album);

// Forces the custom flag. Only the boot-time restore uses this: the saved
// queue comes back through playlist_load_paths() whatever its origin, and a
// queue that was a folder queue before the reboot has to stay one.
void playlist_set_custom(bool custom);

// Bumped whenever the entries or the playback order change (a new queue, a
// fresh shuffle deal). Anything mirroring the queue -- the on-disk copy that
// survives a power cycle -- watches this to know when a full rewrite is due
// rather than rewriting on every track change.
unsigned playlist_revision(void);

// A queue that IS a library list: the handle is taken over, and the queue holds
// four bytes a track instead of a path each. Paths are read back from the
// database as they are needed, so starting "All tracks" on a large library
// costs the same as starting a short one. Takes ownership of `ix` either way.
bool playlist_load_index(library_index_t *ix, int start);

// True while the queue is one of those. What it means in practice: the entries
// are not held here, so anything that would walk the whole queue has to think
// twice.
bool playlist_is_library_backed(void);

// The query behind a library-backed queue, for writing it down instead of its
// rows. False when the queue is not one, or when something was added to it
// after it was built and the query no longer describes it.
bool playlist_library_spec(library_index_spec_t *out);

// Where playback is, as an index into the entries as stored rather than into
// the order they are being played in. What gets written down for a queue that
// will be dealt a fresh shuffle when it comes back: the position in the old
// deal means nothing in the new one.
//
// It is also the number the now playing screen shows, for the same reason: a
// place in the deal is a random number that happens to go up by one each track,
// while a place in the list says where on the card the track came from.
size_t playlist_current_entry(void);

// The entries added to a library-backed queue after it was built. They are not
// in the query, so anything writing the queue down has to take them separately.
int playlist_appended_count(void);
bool playlist_appended_at(int index, char *out, size_t out_size);

// Where each of those entries sits in the playing order.
//
// Without the slots the restore has nowhere to put them but after the current
// track, so tracks played hours ago reappear in front of playback at every boot
// and are played again.
//
// Fills `slots` with the slot of appended entry 0, 1, ... and returns how many
// it wrote (never more than `max`). One pass over the order, not one search per
// entry: this runs on every track change.
int playlist_appended_slots(int *slots, int max);

// Points the current index at `path` if the queue contains it. Returns whether
// it was found. (The custom-queue equivalent of a folder resync.) On a
// library-backed queue the search is bounded to a few hundred entries either
// side of where playback is -- every step there is a database read, and the
// answer, when there is one, is always close by.
bool playlist_locate(const char *path);

// Empties the queue.
void playlist_clear(void);

// --- Reading the queue (for the queue page) ---
// Everything here speaks playback order: under shuffle that is the dealt
// random order (decided up front, so the page shows what will really play),
// otherwise the listed order.
int playlist_count(void);
int playlist_current_index(void); // position in playback order; -1 when empty
bool playlist_path_at(int index, char *out, size_t out_size);
bool playlist_set_current(int index);

// Copies the current track's full path into `out`. Returns false (leaving
// `out` untouched) if the queue is empty.
bool playlist_current_path(char *out, size_t out_size);

// Advances for an automatic (track-finished) transition, honoring the mode:
// REPEAT_ONE keeps the current track, REPEAT_ALL wraps to the start, SHUFFLE
// picks a random other track, NORMAL stops after the last track. On success
// copies the next track's path into `out` and returns true; returns false
// when playback should stop.
bool playlist_advance_auto(char *out, size_t out_size);
// The track playlist_advance_auto() would move to, without moving. False when
// it would stop, or when what comes next is not decided yet (a new shuffle
// deal).
bool playlist_peek_auto(char *out, size_t out_size);

// User-initiated next/prev, walking the playback order. Copy the new track's
// path into `out`; return false only when the queue is empty.
bool playlist_next(char *out, size_t out_size);
bool playlist_prev(char *out, size_t out_size);

void playlist_set_mode(playback_mode_t mode);
playback_mode_t playlist_get_mode(void);
// Advances to the next mode (NORMAL -> REPEAT_ALL -> REPEAT_ONE -> SHUFFLE ->
// NORMAL) and returns the new mode.
playback_mode_t playlist_cycle_mode(void);

#endif // PLAYLIST_H
