#include "sonixlink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/library/medialist.h"
#include "src/gui/library/playlistpage.h"
#include "src/gui/settings/musicsettings.h"
#include "src/gui/shell/toast.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/nowplaying/trackmenu.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/gui/bluetooth/btsettings.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/audio.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/device/power.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/library/playlists.h"
#include "src/system/playback/playlist.h"
#include "src/system/remote/sonixlink.h"
#include "src/system/net/wifi.h"
#include "src/gui/shell/uiscale.h"

lv_obj_t *sonixlink_screen;

// The pump is not the page's timer: commands arrive while the page is closed,
// and that is when a remote control has to work at all.
#define PUMP_MS 200
#define PUMP_WAITING_MS 1000
#define PUMP_IDLE_MS 2000
#define PAGE_POLL_MS 500

#define SL_OFF_HELP "sonixlink_off_note"

// Where a scan asked for from the app starts, as the Library page uses it.
static const char *sd_root;

static lv_obj_t *toggle;
static lv_obj_t *glyph;
static lv_obj_t *status_label;
// Shown while neither link is up: Wi-Fi and Bluetooth side by side, each
// leading to its own page.
static lv_obj_t *links_row;
static lv_timer_t *page_timer;
static lv_timer_t *pump_timer;

// ---------------------------------------------------------------------------

static void hide(lv_obj_t *obj) { lv_obj_set_hidden(obj, true); }
static void show(lv_obj_t *obj) { lv_obj_set_hidden(obj, false); }

static bool switch_is_on(void) { return lv_obj_has_state(toggle, LV_STATE_CHECKED); }

static bool wifi_is_connected(void) {
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
}

// Something the phone can reach the player over: the Wi-Fi network, or
// Bluetooth for a phone paired with it (sonixlink_bt.h).
static bool link_available(void) { return wifi_is_connected() || bluetooth_get_enabled(); }

// ---------------------------------------------------------------------------
// the pump: the player's state out, the app's commands in
// ---------------------------------------------------------------------------

static int mode_to_wire(playback_mode_t mode) {
	switch (mode) {
	case PLAYBACK_MODE_REPEAT_ALL:
		return SONIXLINK_MODE_REPEAT_ALL;
	case PLAYBACK_MODE_REPEAT_ONE:
		return SONIXLINK_MODE_REPEAT_ONE;
	case PLAYBACK_MODE_SHUFFLE:
		return SONIXLINK_MODE_SHUFFLE;
	case PLAYBACK_MODE_SHUFFLE_REPEAT:
		return SONIXLINK_MODE_SHUFFLE_REPEAT;
	case PLAYBACK_MODE_NORMAL:
	default:
		return SONIXLINK_MODE_NORMAL;
	}
}

static void wire_to_mode(int wire) {
	switch (wire) {
	case SONIXLINK_MODE_REPEAT_ALL:
		playlist_set_mode(PLAYBACK_MODE_REPEAT_ALL);
		break;
	case SONIXLINK_MODE_REPEAT_ONE:
		playlist_set_mode(PLAYBACK_MODE_REPEAT_ONE);
		break;
	case SONIXLINK_MODE_SHUFFLE:
		playlist_set_mode(PLAYBACK_MODE_SHUFFLE);
		break;
	case SONIXLINK_MODE_SHUFFLE_REPEAT:
		playlist_set_mode(PLAYBACK_MODE_SHUFFLE_REPEAT);
		break;
	case SONIXLINK_MODE_NORMAL:
	default:
		playlist_set_mode(PLAYBACK_MODE_NORMAL);
		break;
	}
}

// Whether the track playing now is starred. The answer is remembered by path:
// this runs five times a second while a phone is watching, and asking the
// database every time would take the library lock for nothing.
static bool current_is_favourite(const char *path) {
	static char known_path[512];
	static bool known_answer;

	if (!path || !path[0]) {
		known_path[0] = '\0';
		return false;
	}
	if (strcmp(known_path, path) != 0) {
		snprintf(known_path, sizeof(known_path), "%s", path);
		known_answer = library_fav_contains(path);
	}
	return known_answer;
}

// After a star is set or cleared the remembered answer is wrong: forget it.
static void forget_favourite_memory(void) { current_is_favourite(NULL); }

// The window of the queue the phone is shown, refreshed only when it moves.
// Sending it on every pump would mean copying two hundred paths five times a
// second for a page that is usually not even open.
//
// The window is the stretch the phone last asked for when it has asked for
// one (its queue page pages through the whole queue that way), and otherwise
// the stretch around the track playing now.
static void publish_queue_window(void) {
	static unsigned last_generation = (unsigned)-1;
	static int last_position = -2;
	static int last_count = -1;
	static int last_wanted = -2;

	int count = playlist_count();
	int position = playlist_current_index();
	unsigned generation = playlist_revision();
	int wanted = sonixlink_queue_wanted();
	if (wanted >= count) {
		wanted = -1;
	}

	if (generation == last_generation && position == last_position && count == last_count && wanted == last_wanted) {
		return;
	}
	last_generation = generation;
	last_position = position;
	last_count = count;
	last_wanted = wanted;

	int first = wanted >= 0 ? wanted : position > 0 ? position - 20 : 0;
	if (first < 0) {
		first = 0;
	}
	if (wanted < 0 && first + SONIXLINK_QUEUE_WINDOW > count) {
		first = count - SONIXLINK_QUEUE_WINDOW;
		if (first < 0) {
			first = 0;
		}
	}

	// A hundred kilobytes of paths, borrowed for the length of this call. Held
	// as static storage it would sit in the resident set of every run of the
	// player, for a page most people never open.
	enum { PATH_MAX_HERE = 512 };
	char *paths = malloc((size_t)SONIXLINK_QUEUE_WINDOW * PATH_MAX_HERE);
	const char **pointers = malloc(sizeof(char *) * SONIXLINK_QUEUE_WINDOW);
	if (!paths || !pointers) {
		free(paths);
		free(pointers);
		return;
	}

	int written = 0;
	for (int i = 0; i < SONIXLINK_QUEUE_WINDOW && first + i < count; i++) {
		char *slot = paths + (size_t)written * PATH_MAX_HERE;
		if (!playlist_path_at(first + i, slot, PATH_MAX_HERE)) {
			slot[0] = '\0';
		}
		pointers[written] = slot;
		written++;
	}
	sonixlink_publish_queue(count, position, first, pointers, written, generation);
	free(paths);
	free(pointers);
}

static void publish_state(void) {
	device_state_t d;
	device_state_get(&d);

	sonixlink_state_t s;
	memset(&s, 0, sizeof(s));

	switch (d.status) {
	case AUDIO_STATUS_PLAYING:
		s.play_state = 1;
		break;
	case AUDIO_STATUS_PAUSED:
		s.play_state = 2;
		break;
	default:
		s.play_state = 0;
		break;
	}

	s.play_mode = mode_to_wire(playlist_get_mode());
	s.volume = get_volume_percent();
	s.progress_secs = (unsigned)d.progress_current_secs;
	s.duration_secs = (unsigned)d.progress_total_secs;

	snprintf(s.title, sizeof(s.title), "%s", d.metadata.title);
	snprintf(s.artist, sizeof(s.artist), "%s", d.metadata.artist);
	snprintf(s.album, sizeof(s.album), "%s", d.metadata.album);
	snprintf(s.album_artist, sizeof(s.album_artist), "%s", d.metadata.album_artist);
	snprintf(s.path, sizeof(s.path), "%s", d.current_file);

	s.sample_rate = (unsigned)(d.stream_sample_rate > 0 ? d.stream_sample_rate : 0);
	s.bits = (uint8_t)audio_get_stream_bits();
	s.bitrate = (unsigned)audio_get_stream_bitrate_kbps() * 1000u;
	s.lossless = !audio_stream_is_lossy();

	s.battery_percent = atoi(d.battery_percent);
	s.charging = d.battery_charging;

	// While a scan runs the app watches the counter climb; at rest what it
	// wants is how much the library holds.
	s.scanning = library_scan_running();
	s.scan_count = (unsigned)library_scan_found();
	s.track_count = (unsigned)(s.scanning ? 0 : library_track_count());
	// A star set anywhere -- the player's own screens included -- moves the
	// favourites revision, and the remembered answer goes with it.
	static unsigned favourites_seen;
	s.favourites_revision = library_revision(LIBRARY_LIST_FAVOURITES);
	s.playlists_revision = library_revision(LIBRARY_LIST_PLAYLIST);
	if (s.favourites_revision != favourites_seen) {
		favourites_seen = s.favourites_revision;
		forget_favourite_memory();
	}
	s.favourite = current_is_favourite(s.path);

	s.queue_position = playlist_current_index();
	s.queue_count = playlist_count();
	s.queue_revision = playlist_revision();
	s.history_revision = library_history_revision();

	medialist_sort_prefs(&s.sort_desc, &s.sort_added, &s.artist_by_album, &s.favourites_reversed);
	player_queue_position(&s.display_position, &s.display_count);

	// The app wears the player's own accent.
	lv_color_t accent = theme()->accent;
	snprintf(s.accent, sizeof(s.accent), "#%02x%02x%02x", accent.red, accent.green, accent.blue);

	sonixlink_publish(&s);
	publish_queue_window();
}

// The library list a SonixLink list names, narrowed and ordered as the player's
// own list of it is right now.
static library_index_t *open_list(int list, const char *value) {
	library_list_t kind = LIBRARY_LIST_TRACKS;
	library_filter_t filter = LIBRARY_FILTER_NONE;
	library_order_t order = LIBRARY_ORDER_DEFAULT;
	bool desc = false;
	bool records = false;
	switch (list) {
	case SONIXLINK_LIST_ALBUM:
		filter = LIBRARY_FILTER_ALBUM;
		break;
	case SONIXLINK_LIST_ARTIST:
		filter = LIBRARY_FILTER_ARTIST;
		break;
	case SONIXLINK_LIST_ALBUM_ARTIST:
		filter = LIBRARY_FILTER_ALBUM_ARTIST;
		break;
	case SONIXLINK_LIST_GENRE:
		filter = LIBRARY_FILTER_GENRE;
		break;
	case SONIXLINK_LIST_FAVOURITES:
		kind = LIBRARY_LIST_FAVOURITES;
		value = NULL;
		break;
	case SONIXLINK_LIST_PLAYLIST:
		kind = LIBRARY_LIST_PLAYLIST;
		break;
	case SONIXLINK_LIST_ALBUMS:
		// Every record, gathered by record and each in its running order:
		// what the menu on the player's Albums page queues.
		records = true;
		value = NULL;
		break;
	default:
		break;
	}
	if (filter != LIBRARY_FILTER_NONE && !value) {
		return NULL;
	}
	if (kind == LIBRARY_LIST_PLAYLIST && !value) {
		return NULL;
	}
	if (records) {
		order = LIBRARY_ORDER_ALBUM;
	} else {
		medialist_list_order(kind, filter, &order, &desc);
	}
	return library_index_open(kind, filter, value, order, desc);
}

static bool take_first_path_cb(const char *name, const char *path, const char *artist, void *user) {
	(void)name;
	(void)artist;
	if (path && path[0]) {
		snprintf((char *)user, 512, "%s", path);
	}
	return false;
}

// The circle-play menu: the whole list in order, shuffled, or one of its tracks
// at random after the one playing -- play_artist() and the random-track row of
// medialist.c, for a list the phone names.
static void play_all(const sonixlink_command_t *cmd) {
	library_index_t *ix = open_list(cmd->list, cmd->value[0] ? cmd->value : NULL);
	int count = ix ? library_index_count(ix) : 0;
	if (count <= 0) {
		library_index_close(ix);
		return;
	}

	if (cmd->arg == SONIXLINK_PLAY_RANDOM_TO_QUEUE) {
		char path[512] = "";
		int pick = count > 1 ? (int)lv_rand(0, (uint32_t)(count - 1)) : 0;
		library_index_window(ix, pick, 1, take_first_path_cb, path);
		library_index_close(ix);
		if (path[0] && playlist_insert_next(path)) {
			device_state_queue_changed();
			toast_success("added_to_the_queue");
		}
		return;
	}

	bool shuffled = cmd->arg == SONIXLINK_PLAY_SHUFFLE;
	// Mode first, as on the player: with shuffle on, the queue deals its random
	// order as it is loaded.
	playback_mode_t shuffle_mode =
		musicsettings_endless_shuffle() ? PLAYBACK_MODE_SHUFFLE_REPEAT : PLAYBACK_MODE_SHUFFLE;
	playlist_set_mode(shuffled ? shuffle_mode : PLAYBACK_MODE_NORMAL);
	playlist_set_album("");
	int start = shuffled && count > 1 ? (int)lv_rand(0, (uint32_t)(count - 1)) : 0;
	if (device_state_play_index(ix, start)) {
		player_refresh_now_playing();
	}
}

// What the player's selection mode does with a selection, for one the phone
// made.
static void apply_selection(const sonixlink_command_t *cmd) {
	if (!cmd->paths || cmd->path_count <= 0) {
		return;
	}
	const char **paths = malloc((size_t)cmd->path_count * sizeof(*paths));
	if (!paths) {
		return;
	}
	const char *at = cmd->paths;
	for (int i = 0; i < cmd->path_count; i++) {
		paths[i] = at;
		at += strlen(at) + 1;
	}

	switch (cmd->kind) {
	case SONIXLINK_CMD_QUEUE_NEXT: {
		// Each right after the one playing, the first chosen first, as
		// sel_queue_cb() in medialist.c does it.
		int queued = 0;
		for (int i = 0; i < cmd->path_count; i++) {
			queued += playlist_insert_next(paths[i]) ? 1 : 0;
		}
		if (queued > 0) {
			device_state_queue_changed();
			toast_success(queued == 1 ? "added_to_the_queue" : "queue_tracks_added");
		}
		break;
	}
	case SONIXLINK_CMD_FAVOURITES_ADD:
		playlistpage_add_favourites(paths, cmd->path_count);
		forget_favourite_memory();
		break;
	case SONIXLINK_CMD_FAVOURITES_REMOVE: {
		int removed = library_fav_remove_many(paths, cmd->path_count);
		forget_favourite_memory();
		if (removed > 0) {
			toast_success(removed == 1 ? "medialist_track_removed" : "medialist_tracks_removed");
		}
		break;
	}
	case SONIXLINK_CMD_PLAYLIST_ADD:
		playlistpage_add_tracks_to(cmd->value, paths, cmd->path_count);
		break;
	case SONIXLINK_CMD_PLAYLIST_REMOVE: {
		int removed = 0;
		for (int i = 0; i < cmd->path_count; i++) {
			removed += playlists_remove_track(cmd->value, paths[i]) ? 1 : 0;
		}
		if (removed > 0) {
			toast_success(removed == 1 ? "medialist_track_removed" : "medialist_tracks_removed");
		}
		break;
	}
	default:
		break;
	}
	free(paths);
}

static void apply_queue_edit(const sonixlink_command_t *cmd);

static void apply_command(const sonixlink_command_t *cmd) {
	switch (cmd->kind) {
	case SONIXLINK_CMD_PLAY:
		if (audio_get_status() != AUDIO_STATUS_PLAYING) {
			player_key_play_pause();
		}
		break;
	case SONIXLINK_CMD_PAUSE:
		if (audio_get_status() == AUDIO_STATUS_PLAYING) {
			player_key_play_pause();
		}
		break;
	case SONIXLINK_CMD_TOGGLE:
		player_key_play_pause();
		break;
	case SONIXLINK_CMD_STOP:
		device_state_stop();
		player_refresh_now_playing();
		break;
	case SONIXLINK_CMD_NEXT:
		player_key_next();
		break;
	case SONIXLINK_CMD_PREV:
		player_key_prev();
		break;
	case SONIXLINK_CMD_SEEK:
		device_state_seek((double)cmd->arg);
		break;
	case SONIXLINK_CMD_VOLUME:
		if (!volume_lock_enabled()) { // locked: the app reads the level back unchanged
			set_volume_percent(cmd->arg);
		}
		gui_notify_volume(get_volume_percent());
		break;
	case SONIXLINK_CMD_MODE:
		wire_to_mode(cmd->arg);
		player_refresh_now_playing();
		break;
	case SONIXLINK_CMD_PLAY_PATH: {
		// The app names a track by the path it read out of the index. It is
		// checked before it is played: the app's copy of the index can outlive
		// a rescan, and a path that has gone is not something to start.
		char path[512];
		snprintf(path, sizeof(path), "%s", cmd->path);
		if (!path[0] || access(path, R_OK) != 0) {
			printf("sonixlink: the app asks for a track that is not there: %s\n", path);
			break;
		}

		// The queue becomes the list the track was picked from, not the whole
		// library: tapping a track inside a playlist of four hundred has to give
		// a queue of four hundred.
		library_list_t kind = LIBRARY_LIST_TRACKS;
		library_filter_t filter = LIBRARY_FILTER_NONE;
		library_order_t order = LIBRARY_ORDER_DEFAULT;
		bool desc = false;
		const char *value = cmd->value[0] ? cmd->value : NULL;

		switch (cmd->list) {
		case SONIXLINK_LIST_ALBUM:
			// The value is the album as the app's list gives it: the name and,
			// after a 0x1f, whose it is -- the same value the player's own album
			// rows carry, so a record shares its name with no other here.
			filter = LIBRARY_FILTER_ALBUM;
			break;
		case SONIXLINK_LIST_ARTIST:
			filter = LIBRARY_FILTER_ARTIST;
			break;
		case SONIXLINK_LIST_ALBUM_ARTIST:
			filter = LIBRARY_FILTER_ALBUM_ARTIST;
			break;
		case SONIXLINK_LIST_GENRE:
			filter = LIBRARY_FILTER_GENRE;
			break;
		case SONIXLINK_LIST_FAVOURITES:
			kind = LIBRARY_LIST_FAVOURITES;
			value = NULL;
			break;
		case SONIXLINK_LIST_PLAYLIST:
			kind = LIBRARY_LIST_PLAYLIST;
			break;
		case SONIXLINK_LIST_QUEUE:
			// Already in the queue: move to it instead of building another.
			for (int i = 0, n = playlist_count(); i < n; i++) {
				char at[512];
				if (playlist_path_at(i, at, sizeof(at)) && strcmp(at, path) == 0) {
					if (device_state_play_queue_index(i)) {
						player_refresh_now_playing();
						goto play_path_done;
					}
					break;
				}
			}
			break;
		case SONIXLINK_LIST_FOLDER:
			// What the file browser does with a tap: the folder becomes the
			// queue, the tapped file its current track.
			player_play_file(path);
			goto play_path_done;
		case SONIXLINK_LIST_ALL:
		default:
			break;
		}

		// A filtered list with nothing to filter on would quietly become the
		// whole library, which is the very thing this is here to avoid.
		if (filter != LIBRARY_FILTER_NONE && !value) {
			filter = LIBRARY_FILTER_NONE;
		}
		if (kind == LIBRARY_LIST_PLAYLIST && !value) {
			kind = LIBRARY_LIST_TRACKS;
		}

		// In the order the player's own list is in right now (by name or by
		// date, which way round, an artist by record or not), which is the
		// order the app shows it in too.
		medialist_list_order(kind, filter, &order, &desc);

		library_index_t *ix = library_index_open(kind, filter, value, order, desc);
		int at = ix ? library_index_find_path(ix, path) : -1;

		// The handle is given away by device_state_play_index() whether it
		// succeeds or not, so once that is called it must not be touched again.
		bool started = false;
		if (ix && at >= 0) {
			started = device_state_play_index(ix, at);
		} else if (ix) {
			library_index_close(ix);
		}
		if (!started) {
			device_state_play_file(path);
		}
		player_refresh_now_playing();
	play_path_done:
		break;
	}
	case SONIXLINK_CMD_SCAN:
		if (library_scan_running()) {
			printf("sonixlink: scan already running\n");
		} else if (!sd_root || !sd_root[0]) {
			printf("sonixlink: cannot scan, no card\n");
		} else if (!library_scan_start(sd_root)) {
			printf("sonixlink: scan refused by library_scan_start(%s)\n", sd_root);
		} else {
			printf("sonixlink: scan started on %s\n", sd_root);
		}
		break;
	case SONIXLINK_CMD_FAVOURITE: {
		if (!cmd->path[0]) {
			break;
		}
		bool starred = library_fav_contains(cmd->path);
		bool want = cmd->arg < 0 ? !starred : (cmd->arg != 0);
		if (want != starred) {
			// The name and artist are stored at star time so the favourites
			// list draws without a join. For the track playing now they are in
			// hand; for any other one the index has them.
			device_state_t d;
			device_state_get(&d);
			const char *name = "";
			const char *artist = "";
			if (strcmp(d.current_file, cmd->path) == 0) {
				name = d.metadata.title;
				artist = d.metadata.artist;
			}
			library_fav_toggle(cmd->path, name, artist);
			forget_favourite_memory();
			player_refresh_now_playing();
		}
		break;
	}
	case SONIXLINK_CMD_QUEUE_INDEX:
		if (device_state_play_queue_index(cmd->arg)) {
			player_refresh_now_playing();
		}
		break;
	case SONIXLINK_CMD_PLAY_ALL:
		play_all(cmd);
		break;
	case SONIXLINK_CMD_QUEUE_NEXT:
	case SONIXLINK_CMD_FAVOURITES_ADD:
	case SONIXLINK_CMD_FAVOURITES_REMOVE:
	case SONIXLINK_CMD_PLAYLIST_ADD:
	case SONIXLINK_CMD_PLAYLIST_REMOVE:
		apply_selection(cmd);
		break;
	case SONIXLINK_CMD_QUEUE_REMOVE:
	case SONIXLINK_CMD_QUEUE_MOVE:
	case SONIXLINK_CMD_HISTORY_PLAY:
	case SONIXLINK_CMD_HISTORY_INSERT:
		apply_queue_edit(cmd);
		break;
	case SONIXLINK_CMD_NONE:
	default:
		break;
	}
	sonixlink_command_free((sonixlink_command_t *)cmd);
}

// The phone's queue page, doing what the player's does (trackmenu.c): only
// what comes after the track playing can be taken out, moved or put in, and a
// command worked out against a queue that has changed since is dropped rather
// than applied to the wrong entries.
static bool queue_still_as_seen(const sonixlink_command_t *cmd) {
	if (cmd->has_revision && cmd->revision != playlist_revision()) {
		printf("sonixlink: queue command for an older queue (%u, now %u); dropped\n", cmd->revision, playlist_revision());
		return false;
	}
	return true;
}

// The file of an entry of the history, if it is still there. Says so when it
// is not, as the player's page does.
static bool history_file(long long id, char *path, size_t size) {
	struct stat st;
	if (!library_history_entry(id, path, size, NULL, 0) || stat(path, &st) != 0) {
		toast_error("history_track_gone");
		return false;
	}
	return true;
}

static int compare_ints(const void *a, const void *b) {
	int x = *(const int *)a, y = *(const int *)b;
	return (x > y) - (x < y);
}

static void apply_queue_edit(const sonixlink_command_t *cmd) {
	int count = playlist_count();
	int current = playlist_current_index();
	char path[SONIXLINK_PATH_MAX];
	unsigned revision = playlist_revision();

	switch (cmd->kind) {
	case SONIXLINK_CMD_QUEUE_REMOVE: {
		if (!queue_still_as_seen(cmd) || !cmd->paths || cmd->path_count <= 0) {
			return;
		}
		int *indices = malloc((size_t)cmd->path_count * sizeof(*indices));
		if (!indices) {
			return;
		}
		int n = 0;
		const char *at = cmd->paths;
		for (int i = 0; i < cmd->path_count; i++) {
			int index = atoi(at);
			if (index > current && index < count) {
				indices[n++] = index;
			}
			at += strlen(at) + 1;
		}
		// In order and each once, as the player's selection hands them over.
		qsort(indices, (size_t)n, sizeof(*indices), compare_ints);
		int unique = 0;
		for (int i = 0; i < n; i++) {
			if (unique == 0 || indices[unique - 1] != indices[i]) {
				indices[unique++] = indices[i];
			}
		}
		bool removed = unique > 0 && playlist_remove(indices, unique);
		free(indices);
		if (!removed) {
			return;
		}
		device_state_queue_changed();
		toast_success("queue_tracks_removed");
		break;
	}
	case SONIXLINK_CMD_QUEUE_MOVE: {
		int from = cmd->arg, to = cmd->arg2;
		if (!queue_still_as_seen(cmd) || from == to || from <= current || to <= current || from >= count || to >= count) {
			return;
		}
		playlist_move(from, to);
		if (playlist_revision() != revision) {
			device_state_queue_changed();
		}
		break;
	}
	case SONIXLINK_CMD_HISTORY_PLAY: {
		// Right after the track playing, so what was coming still comes
		// after it.
		if (!history_file(cmd->id, path, sizeof(path))) {
			return;
		}
		int index = count > 0 ? current + 1 : 0;
		if (!playlist_insert_at(path, index)) {
			return;
		}
		if (device_state_play_queue_index(index)) {
			player_refresh_now_playing();
		}
		break;
	}
	case SONIXLINK_CMD_HISTORY_INSERT: {
		int slot = cmd->arg;
		if (!queue_still_as_seen(cmd) || slot <= current || slot > count) {
			return;
		}
		if (!history_file(cmd->id, path, sizeof(path)) || !playlist_insert_at(path, slot)) {
			return;
		}
		device_state_queue_changed();
		break;
	}
	default:
		return;
	}
	// The player's own queue page, if it is up, lays itself out again.
	trackmenu_notify_now_playing();
}

// The rate the current state calls for: fast enough that the app sees a
// progress bar move, slow while nothing is listening, and the slowest (the
// timer is paused anyway) while the service is off -- nearly always.
static int pump_period(void) {
	if (!sonixlink_get_enabled()) {
		return PUMP_IDLE_MS;
	}
	return sonixlink_is_connected() ? PUMP_MS : PUMP_WAITING_MS;
}

// SonixLink goes off by itself when both radios have been switched off: with
// neither there is nothing a phone could reach it over. The switches and not
// the connections are what counts -- Wi-Fi on but between networks will be
// back -- and a radio the power code parked for idleness is not switched off.
// Not in the first minute after boot, while the radios are still coming up.
#define AUTO_OFF_AFTER_MS 3000
#define AUTO_OFF_BOOT_GRACE_MS 60000

static void auto_off_check(void) {
	static uint32_t radios_off_since;
	bool radios = wifi_get_enabled() || bluetooth_get_enabled() || power_radios_parked();
	uint32_t now = lv_tick_get();
	if (radios || now < AUTO_OFF_BOOT_GRACE_MS) {
		radios_off_since = 0;
		return;
	}
	if (radios_off_since == 0) {
		radios_off_since = now;
		return;
	}
	if (now - radios_off_since >= AUTO_OFF_AFTER_MS) {
		radios_off_since = 0;
		printf("sonixlink: Wi-Fi and Bluetooth are both off; switching SonixLink off\n");
		sonixlink_set_enabled(false);
		if (toggle) {
			lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		}
	}
}

static void pump_cb(lv_timer_t *timer) {
	lv_timer_set_period(timer, pump_period());
	if (!sonixlink_get_enabled()) {
		sonixlink_pump_pace();
		return;
	}
	auto_off_check();
	if (!sonixlink_get_enabled()) {
		sonixlink_pump_pace();
		return;
	}

	sonixlink_command_t cmd;
	while (sonixlink_take_command(&cmd)) {
		apply_command(&cmd);
	}
	publish_state();
}

// Puts the pump back at the rate the current state calls for, or to sleep:
// while SonixLink is off no command can arrive and nothing is listening for
// the state, so the timer is paused rather than ticking every two seconds to
// find that out. Whoever switches the service on wakes it (sonixlink_pump_pace).
void sonixlink_pump_pace(void) {
	if (!pump_timer) {
		return;
	}
	if (!sonixlink_get_enabled()) {
		lv_timer_pause(pump_timer);
		return;
	}
	lv_timer_set_period(pump_timer, pump_period());
	lv_timer_resume(pump_timer);
}

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

// Two states and nothing else, the way the DAC page reads: the picture says
// what this is, the line under it says whether a phone is there.
static void refresh(void) {
	if (!link_available()) {
		lv_label_set_text(status_label, tr("sonixlink_needs_wifi_or_bluetooth"));
		lv_obj_set_style_text_color(status_label, theme()->text_primary, 0);
		lv_obj_add_state(toggle, LV_STATE_DISABLED);
		show(links_row);
		return;
	}
	lv_obj_remove_local_style_prop(status_label, LV_STYLE_TEXT_COLOR, 0);
	lv_obj_remove_state(toggle, LV_STATE_DISABLED);
	hide(links_row);

	if (!switch_is_on()) {
		lv_label_set_text(status_label, tr(SL_OFF_HELP));
		return;
	}

	lv_label_set_text(status_label, sonixlink_is_connected() ? tr("connected") : tr("sonixlink_waiting"));
}

static void page_poll_cb(lv_timer_t *timer) {
	(void)timer;

	refresh();

	if (!switch_is_on()) {
		return;
	}

	// The network and Bluetooth both went away under a running service: there
	// is nothing left to be found on.
	if (!link_available()) {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		sonixlink_set_enabled(false);
		sonixlink_pump_pace();
		refresh();
	}
}

static void toggle_changed_cb(lv_event_t *e) {
	(void)e;

	bool on = switch_is_on();

	if (on && !link_available()) {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		gui_notify_popup("sonixlink_needs_wifi_or_bluetooth");
		return;
	}

	sonixlink_set_enabled(on);
	if (on) {
		// The app asks for the state the moment it connects, so the service
		// must not be holding a stale picture when it does.
		publish_state();
	}
	sonixlink_pump_pace();
	refresh();
}

// The control centre switched the service with the page open under it: the
// switch and the lines under it follow, and nothing else is touched.
static void switched_elsewhere_cb(lv_event_t *e) {
	(void)e;
	if (sonixlink_get_enabled()) {
		lv_obj_add_state(toggle, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
	}
	refresh();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;

	if (sonixlink_get_enabled()) {
		lv_obj_add_state(toggle, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
	}

	refresh();
	lv_timer_resume(page_timer);
	lv_timer_ready(page_timer);
}

// Like AirPlay and DLNA, leaving the page does not switch the service off: the
// switch owns that state, the page is only where it lives.
static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(page_timer);
}

static void wifi_row_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

static void bluetooth_row_cb(lv_event_t *e) {
	(void)e;
	switch_screen(btsettings_screen);
}

// ---------------------------------------------------------------------------

void sonixlink_page_init(gui_config_t *cfg) {
	sd_root = cfg->sd_root_path;
	sonixlink_set_card_root(sd_root);

	lv_obj_t *container = settingsrow_page(sonixlink_screen, cfg, "sonixlink");

	settingsrow_toggle(container, "sonixlink", &toggle, toggle_changed_cb);

	glyph = lv_image_create(container);
	lv_image_set_src(glyph, &icon_sonixlink_page);
	lv_obj_set_style_margin_top(glyph, ui_px(40), 0);

	status_label = lv_label_create(container);
	lv_obj_set_width(status_label, lv_pct(100));
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_22, 0);
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_hor(status_label, ui_px(4), 0);
	lv_obj_set_style_margin_top(status_label, ui_px(20), 0);

	// The notice says what is missing; these lead to where it is fixed. Two
	// halves of one row, since either link will do.
	links_row = lv_obj_create(container);
	lv_obj_remove_style_all(links_row);
	lv_obj_set_size(links_row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(links_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(links_row, ui_px(12), 0);
	lv_obj_set_style_margin_top(links_row, ui_px(16), 0);
	lv_obj_set_scrollable(links_row, false);
	lv_obj_set_event_bubble(links_row, true);
	lv_obj_t *wifi_half = settingsrow_add(links_row, "wi_fi", NULL, wifi_row_cb, NULL);
	lv_obj_t *bluetooth_half = settingsrow_add(links_row, "bluetooth", NULL, bluetooth_row_cb, NULL);
	lv_obj_set_width(wifi_half, ui_px(1));
	lv_obj_set_flex_grow(wifi_half, 1);
	lv_obj_set_width(bluetooth_half, ui_px(1));
	lv_obj_set_flex_grow(bluetooth_half, 1);
	hide(links_row);

	page_timer = lv_timer_create(page_poll_cb, PAGE_POLL_MS, NULL);
	lv_timer_pause(page_timer);

	// While SonixLink is on this one never stops: it is the bridge between the
	// network thread and playback, and the app pressing next while another page
	// is open still has to be served. While it is off the timer is paused
	// (sonixlink_pump_pace), and the switches -- this page's, the control
	// centre's -- wake it.
	pump_timer = lv_timer_create(pump_cb, PUMP_IDLE_MS, NULL);
	sonixlink_pump_pace();

	lv_obj_add_event_cb(sonixlink_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(sonixlink_screen, switched_elsewhere_cb, LV_EVENT_REFRESH, NULL);
	lv_obj_add_event_cb(sonixlink_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	refresh();
}
