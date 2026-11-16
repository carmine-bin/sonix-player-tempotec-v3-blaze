#include "library.h"

#include "src/system/library/albumart.h"
#include "src/system/core/bootprof.h"
#include "src/system/core/frameprof.h"

// SQLite's page cache for the library, in the form PRAGMA cache_size takes
// (negative: kibibytes). See library_open().
#define DB_CACHE_KIB "-2048"

#include "src/system/library/audiobookdb.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/podcastdl.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/db/sqlite3.h"
#include "src/system/library/cue.h"
#include "src/system/decode/mp4.h"
#include "src/system/decode/apedec.h"
#include "src/system/library/metadata.h"
#include "src/system/core/utils.h"

// The schema.
//
// Close to the stock player's, with the `sortkey` columns added: the ordering
// the stock schema implies cannot be indexed, so without them every list sorts
// the whole table on every open. See library_sort_key(). The two databases are
// separate -- this index lives under .local, the stock one under .temp -- so
// neither player reads the other's.
//
// Only the tables a scan actually fills are created here; the rest of the
// firmware's tables (history, collections, bookmarks) are its own business.

// The `format` column's codes. The first five are the stock player's; the rest
// are local additions, and extending the set breaks nothing because the column
// is read only in here.
#define FORMAT_WAV 0
#define FORMAT_MP3 1
#define FORMAT_FLAC 2
#define FORMAT_OGG 3
#define FORMAT_DSD 4
#define FORMAT_AAC 5
#define FORMAT_OPUS 6
#define FORMAT_WAVPACK 7
#define FORMAT_ALAC 8
#define FORMAT_APE 9

static const char *const SCHEMA[] = {
	"CREATE TABLE IF NOT EXISTS MEDIA_TABLE(id INT,path TEXT COLLATE NOCASE,name TEXT COLLATE NOCASE,"
	"album TEXT COLLATE NOCASE,artist TEXT COLLATE NOCASE,genre TEXT COLLATE NOCASE,year INT,dis_id INT,ck_id INT,"
	"has_child_file INT,begin_time INT,end_time INT,cue_id INT,character TEXT COLLATE NOCASE,size INT,"
	"sample_rate INT,bit_rate INT,bit INT,channel INT,format INT,quality TEXT COLLATE NOCASE,"
	"album_pic_path TEXT COLLATE NOCASE,lrc_path TEXT COLLATE NOCASE,track_gain REAL,track_peak REAL,"
	"album_artist TEXT COLLATE NOCASE,ctime INT,mtime INT,sortkey TEXT,disc INT,PRIMARY KEY(id, path, cue_id))",

	"CREATE TABLE IF NOT EXISTS ALBUM_TABLE(id INT, album TEXT COLLATE NOCASE,character TEXT COLLATE NOCASE,"
	" cn INT, sortkey TEXT, PRIMARY KEY(album))",

	// The albums as this player lists them: one row per name AND per whose it
	// is (see album_key()), so two records called "Greatest Hits" by two
	// artists are two rows. ALBUM_TABLE stays as the stock schema has it, one
	// row per name, for anything that reads the database the stock way.
	"CREATE TABLE IF NOT EXISTS ALBUM_GROUP_TABLE(album TEXT COLLATE NOCASE, album_key TEXT, sortkey TEXT,"
	" PRIMARY KEY(album, album_key))",

	"CREATE TABLE IF NOT EXISTS ARTIST_TABLE(id INT, artist TEXT COLLATE NOCASE,character TEXT COLLATE NOCASE,"
	" cn INT, sortkey TEXT, PRIMARY KEY(artist))",

	"CREATE TABLE IF NOT EXISTS ALBUM_ARTIST_TABLE(id INT, album_artist TEXT COLLATE NOCASE,"
	// pinyin_charater is the stock schema's, misspelling and all: nothing here
	// reads it and the scan writes an empty string into it. Kept because
	// dropping a column means rebuilding the table on every card that already
	// has one, for nothing.
	"character TEXT COLLATE NOCASE, cn INT, ctime INT, mtime INT,mqa INT,pinyin_charater TEXT,sortkey TEXT,"
	"PRIMARY KEY(album_artist))",

	"CREATE TABLE IF NOT EXISTS GENRE_TABLE( id INT,genre TEXT COLLATE NOCASE,character TEXT COLLATE NOCASE,"
	" cn INT, sortkey TEXT, PRIMARY KEY(genre))",

	// Not in the stock schema: the starred tracks, in order of starring.
	// Deliberately independent of MEDIA_TABLE so favourites survive rescans
	// and can point at files the index has never seen.
	"CREATE TABLE IF NOT EXISTS FAVOURITES(path TEXT PRIMARY KEY, name TEXT, artist TEXT, added_at INT)",

	// Also local: how many times each track has been played through, by the
	// rule Last.fm counts a play by, and when last. Keyed by path and kept apart
	// from MEDIA_TABLE like the favourites, so a rescan does not forget it.
	"CREATE TABLE IF NOT EXISTS PLAY_COUNTS(path TEXT PRIMARY KEY, plays INT NOT NULL, last_played INT)",

	// Also local: every track the queue has played, one row each time, in the
	// order it happened (the queue page's History). The title and artist are
	// written down with the row, so a rescan, or the file going, does not take
	// the entry with it. `played_at` is 0 when the clock was not set.
	"CREATE TABLE IF NOT EXISTS PLAY_HISTORY(id INTEGER PRIMARY KEY AUTOINCREMENT, path TEXT NOT NULL, name TEXT,"
	" artist TEXT, played_at INT)",

	// Also local: where playback was left (the "Ricorda traccia" option). One
	// row only; id is always 0.
	"CREATE TABLE IF NOT EXISTS PLAYBACK_STATE(id INT PRIMARY KEY, path TEXT, position REAL, updated_at INT)",

	// Also local: the playback queue as it will play, so a reboot comes back
	// with next and prev still meaning something. One row per entry, plus a
	// single row of state (the current position, whether it is a library list).
	// `slot` is where the row sits in the playing order. It matters only for the
	// query form below, where these rows are what "add to queue" put beside a
	// library list: without it the restore has nowhere to put them but after the
	// current track. -1, or the column missing on an older card, means no slot
	// was recorded.
	"CREATE TABLE IF NOT EXISTS PLAYBACK_QUEUE(idx INT PRIMARY KEY, path TEXT, slot INT)",
	"CREATE TABLE IF NOT EXISTS PLAYBACK_QUEUE_STATE(id INT PRIMARY KEY, pos INT, custom INT, updated_at INT)",

	// A queue that is a library list is written down as the question, not the
	// answer: which list, ordered how, and where playback had got to. Two
	// hundred thousand rows of PLAYBACK_QUEUE for a queue that one query
	// rebuilds is minutes of card writes on every track change, and it is the
	// same list either way -- unless the library was rescanned in between, in
	// which case re-running the query is the more correct of the two.
	"CREATE TABLE IF NOT EXISTS PLAYBACK_QUEUE_QUERY(id INT PRIMARY KEY, kind INT, filter INT, ord INT, desc INT,"
	" value TEXT, pos INT, updated_at INT)",

	// Which record the queue IS, when it is one. Neither of the two forms above
	// carries it: the rows are paths and the query is a filter, and a queue
	// built from Cover Flow or the Albums page is a record in a way that neither
	// spells out. "Play albums back to back" reads it after a reboot. Its own
	// table because a new table needs no migration on a card that already holds
	// the others.
	"CREATE TABLE IF NOT EXISTS PLAYBACK_QUEUE_ALBUM(id INT PRIMARY KEY, album TEXT)",

	// Local: how many tracks each playlist holds, so the playlists page does not
	// have to walk every entry against the card to draw a subtitle. Keyed by
	// the file's own path, with the modification time and size it had when the
	// number was counted -- edit the playlist and the row stops matching.
	"CREATE TABLE IF NOT EXISTS PLAYLIST_COUNTS(path TEXT PRIMARY KEY, mtime INT, size INT, tracks INT, mount INT)",

	// Local: which mount of this card the counts above were checked against.
	// One row, bumped every time the card is mounted. See next_mount_serial().
	"CREATE TABLE IF NOT EXISTS MOUNT_SERIAL(id INT PRIMARY KEY, serial INT)",

	// Local: whether the card's .m3u playlist files have already been taken into
	// tables of their own. One row, set once per card.
	"CREATE TABLE IF NOT EXISTS PLAYLIST_MIGRATION(id INT PRIMARY KEY, done INT)",

	// Local: every folder the walk has read, with what it looked like then --
	// its modification time, how many tracks and subfolders it holds, and a
	// hash of their names (folder_sig_t). Detect changes only looks inside a
	// folder whose names differ.
	"CREATE TABLE IF NOT EXISTS FOLDER_TABLE(path TEXT COLLATE NOCASE PRIMARY KEY, mtime INT, files INT, names INT)",

	// Local: what each CUE sheet was found to cut up, keyed by the sheet's
	// modification time and size. A sheet that still matches is not parsed
	// again. `version` is the CUE_STATE_VERSION the row was written under.
	"CREATE TABLE IF NOT EXISTS CUE_STATE(path TEXT COLLATE NOCASE PRIMARY KEY, mtime INT, size INT, audio TEXT,"
	" version INT)",

	// Local: the names a multi-artist or multi-genre tag splits into (see
	// split_names()), one row per track and name. A track whose tag does not
	// split has no rows here and is found by MEDIA_TABLE's own column.
	"CREATE TABLE IF NOT EXISTS ARTIST_LINK(path TEXT COLLATE NOCASE, artist TEXT COLLATE NOCASE)",
	"CREATE TABLE IF NOT EXISTS ALBUM_ARTIST_LINK(path TEXT COLLATE NOCASE, album_artist TEXT COLLATE NOCASE)",
	"CREATE TABLE IF NOT EXISTS GENRE_LINK(path TEXT COLLATE NOCASE, genre TEXT COLLATE NOCASE)",

	// Local: the settings the index was last filed under (rules_signature()),
	// one row. A database without it was filed with nothing split or joined.
	"CREATE TABLE IF NOT EXISTS ORGANIZE_STATE(id INT PRIMARY KEY, rules TEXT)",

	"CREATE TABLE IF NOT EXISTS VERSION_TABLE( version_id INT,PRIMARY KEY(version_id))",
	"CREATE TABLE IF NOT EXISTS COUNT_TABLE( cn INT)",
};

// Added to a database that predates them. CREATE TABLE IF NOT EXISTS leaves an
// existing table alone, columns and all, so a new column has to be asked for
// separately; "duplicate column name" is the ordinary answer here and means the
// column is already there.
static const char *const SCHEMA_COLUMNS[] = {
	"ALTER TABLE MEDIA_TABLE ADD COLUMN sortkey TEXT",
	// Unlike the sort keys, this one cannot be worked out from anything already
	// in the table: it is in the file's tags and nowhere else. An index written
	// before the column existed therefore carries NULL until the card is
	// scanned again. See TRACK_ORDER_IN_ALBUM.
	"ALTER TABLE MEDIA_TABLE ADD COLUMN disc INT",
	// Whose album the track is on (album_key(), or the key album_join() gave
	// it), written down with the track. Every query that tells albums apart
	// reads this column, and so does the SonixLink app on the phone.
	"ALTER TABLE MEDIA_TABLE ADD COLUMN album_key TEXT",
	"ALTER TABLE ALBUM_TABLE ADD COLUMN sortkey TEXT",
	"ALTER TABLE ARTIST_TABLE ADD COLUMN sortkey TEXT",
	"ALTER TABLE ALBUM_ARTIST_TABLE ADD COLUMN sortkey TEXT",
	"ALTER TABLE GENRE_TABLE ADD COLUMN sortkey TEXT",
	"ALTER TABLE PLAYLIST_COUNTS ADD COLUMN mount INT",
	"ALTER TABLE PLAYBACK_QUEUE ADD COLUMN slot INT",
	"ALTER TABLE CUE_STATE ADD COLUMN version INT",
};

// After the columns exist, never before: an index on a column the table does
// not have yet is an error, and on an old database that is exactly the order
// things happen in.
static const char *const SCHEMA_INDEXES[] = {
	// By path. The primary key is (id, path, cue_id), and SQLite can only use a
	// composite index from its leftmost column, so without this "WHERE path=?"
	// is a full table scan. One such scan goes unnoticed; a playlist asking for
	// a name per entry is one per row.
	"CREATE INDEX IF NOT EXISTS media_path_idx ON MEDIA_TABLE(path)",
	// One album's tracks, told apart by album_key: the subqueries behind every
	// album row (its cover, its artist, its year, when it arrived) are a
	// range of this index rather than every track that shares the name. Year
	// and ctime ride along, so the album lists by year and by date read no
	// table rows at all. It also serves every lookup by album name alone,
	// which the single-column index it replaces did.
	"CREATE INDEX IF NOT EXISTS media_album_key_idx ON MEDIA_TABLE(album, album_key, year, ctime)",
	"DROP INDEX IF EXISTS media_album_idx",
	// An album's first track in disc and track order (ALBUM_GROUP_FIRST): read
	// off this index instead of sorting every track of the album per row.
	"CREATE INDEX IF NOT EXISTS media_album_first_idx ON MEDIA_TABLE(album, album_key, COALESCE(disc,1), dis_id)",
	// The tracks album_keys_fill() still has to fill: none, once it has run. Its
	// check at every open is then one empty b-tree, not a walk of every track.
	"CREATE INDEX IF NOT EXISTS media_album_key_null_idx ON MEDIA_TABLE(album_key) WHERE album_key IS NULL",
	// The same for the artist and album artist lists by date.
	"CREATE INDEX IF NOT EXISTS media_artist_ctime_idx ON MEDIA_TABLE(artist, ctime)",
	"DROP INDEX IF EXISTS media_artist_idx",
	"CREATE INDEX IF NOT EXISTS media_album_artist_ctime_idx ON MEDIA_TABLE(album_artist, ctime)",
	"DROP INDEX IF EXISTS media_album_artist_idx",
	"CREATE INDEX IF NOT EXISTS artist_link_name_idx ON ARTIST_LINK(artist)",
	"CREATE INDEX IF NOT EXISTS artist_link_path_idx ON ARTIST_LINK(path)",
	"CREATE INDEX IF NOT EXISTS album_artist_link_name_idx ON ALBUM_ARTIST_LINK(album_artist)",
	"CREATE INDEX IF NOT EXISTS album_artist_link_path_idx ON ALBUM_ARTIST_LINK(path)",
	"CREATE INDEX IF NOT EXISTS genre_link_name_idx ON GENRE_LINK(genre)",
	"CREATE INDEX IF NOT EXISTS genre_link_path_idx ON GENRE_LINK(path)",
};

// Built only once every row has a key: an index on a column that is still half
// empty is a b-tree updated a row at a time for nothing. On a database that
// already has its keys these run at open; on one that does not, they run when
// the backfill has been through it, as one bulk build instead of two hundred
// thousand insertions.
static const char *const SORT_INDEXES[] = {
	// By sort key: what makes a list open without sorting anything.
	//
	// The row id rides in every index entry, so "SELECT rowid, sortkey ...
	// ORDER BY sortkey" is answered from the index alone -- a sequential walk,
	// no table lookups, no temporary b-tree, nothing spilled to the card. The
	// A-Z strip is counted from the key rather than from the name, which is why
	// the name does not have to be in here (see letter_from_key).
	"CREATE INDEX IF NOT EXISTS media_sort_idx ON MEDIA_TABLE(sortkey)",

	// The same for one genre's tracks, which on a real library is a large
	// slice of it. Albums and artists keep their plain single-column index:
	// those lists are tens or hundreds of rows, and sorting that many costs
	// less than the wider index would cost the scan.
	"CREATE INDEX IF NOT EXISTS media_genre_sort_idx ON MEDIA_TABLE(genre, sortkey)",
	// Which makes the plain one redundant: a two-column index serves a lookup
	// on its first column just as well, and a scan pays for every index it has
	// to keep.
	"DROP INDEX IF EXISTS media_genre_idx",

	// The lookup tables carry their name along in the index as well. Their
	// query leaves out the rows with no name at all ("WHERE album <> ''"), and
	// a filter on a column the index does not hold means reading the row to
	// answer it -- which is the table lookup per row that the index was there
	// to avoid. They hold thousands of rows, not hundreds of thousands, so the
	// extra width costs nothing worth counting.
	"CREATE INDEX IF NOT EXISTS album_sort_idx ON ALBUM_TABLE(sortkey, album)",
	"CREATE INDEX IF NOT EXISTS album_group_sort_idx ON ALBUM_GROUP_TABLE(sortkey, album)",
	"CREATE INDEX IF NOT EXISTS artist_sort_idx ON ARTIST_TABLE(sortkey, artist)",
	"CREATE INDEX IF NOT EXISTS album_artist_sort_idx ON ALBUM_ARTIST_TABLE(sortkey, album_artist)",
	"CREATE INDEX IF NOT EXISTS genre_sort_idx ON GENRE_TABLE(sortkey, genre)",
};

// A record's running order: the disc, then the track number inside it, so a
// two-disc album gives 1-1, 1-2, ... then 2-1, 2-2, and not the two track
// ones together.
//
// COALESCE because `disc` is NULL on every row of an index written before the
// column existed, and in SQLite NULL sorts ahead of every number: a table
// where some rows have been scanned since and some have not would otherwise
// deal each record out in two. NULL means disc one here, which is the order
// those rows had anyway.
#define TRACK_ORDER_IN_ALBUM "COALESCE(disc,1), dis_id"

// When a file arrived. st_ctime, which the scan stores: on the FAT and exFAT
// cards this player reads, that is when the file was written to the card, and
// unlike mtime it is not carried over from wherever the file was copied from.
#define TRACK_ORDER_ADDED "ctime"

// Rows are committed in small batches. This is not a tuning knob: an open
// transaction holds its dirty pages in memory, and on a device with ten
// megabytes free a scan that only commits at the end is a scan that gets the
// player killed. Fifty rows is frequent enough that the index on disk is never
// far behind what the counter says, and each commit is followed by handing the
// page cache back.
#define SCAN_COMMIT_EVERY 50

// Deepest directory nesting the walk will follow. Guards against a symlink
// loop turning into an infinite scan.
#define SCAN_MAX_DEPTH 12

// How many subdirectories of one level are held aside while it is walked. Not a
// limit on the library: past this many the directory is read again for the
// next batch, so a card with every album in one folder is walked whole while
// the names held in memory stay bounded.
#define SCAN_MAX_SUBDIRS 4096

// How often the scan logs a line with the free memory. If it ever dies partway
// through, the log says where it got to and how much RAM was left.
#define SCAN_LOG_EVERY 200

static sqlite3 *db;
static pthread_mutex_t db_lock = PTHREAD_MUTEX_INITIALIZER;

// Bumped whenever a row's identity can have changed underneath a list that is
// already open.
//
// The lists hold SQLite row ids rather than copies of the rows, and a row id is
// only a promise for as long as nobody rewrites the table. It is not: INSERT OR
// REPLACE is a delete and an insert, so a re-scanned track comes back with a
// different one; a scan begins by emptying the tables, and with no AUTOINCREMENT
// the numbering starts again at one. A list built before that and read after it
// would show the wrong tracks rather than none, which is the kind of wrong
// nobody notices.
//
// One counter per group of tables that move together, not one for everything:
// favourites are a table of their own precisely so that starring a track cannot
// touch the index (see the FAVOURITES schema), and a single counter would let
// the star button invalidate the handle the playback queue was built on.
// A favourite's artist: the index's, where it has the track -- what the list
// of all the tracks shows -- and the one stored with the star otherwise. A
// track starred from a list is stored with no artist at all.
#define FAV_ARTIST \
	"COALESCE(NULLIF((SELECT m.artist FROM MEDIA_TABLE m WHERE m.path = FAVOURITES.path LIMIT 1), ''), artist)"
// The title the same way: the library's, then the one written down at starring.
#define FAV_NAME "COALESCE(NULLIF((SELECT m.name FROM MEDIA_TABLE m WHERE m.path = FAVOURITES.path LIMIT 1), ''), name)"

// The most played tracks: the ones still in the index, most plays first and,
// between equals, the one played last. The title and the artist are the
// index's, which is the only place they are kept.
#define PLAYS_IN_INDEX "EXISTS(SELECT 1 FROM MEDIA_TABLE m WHERE m.path = PLAY_COUNTS.path)"
#define PLAYS_ORDER "plays DESC, last_played DESC, rowid"
#define PLAYS_NAME "(SELECT m.name FROM MEDIA_TABLE m WHERE m.path = PLAY_COUNTS.path LIMIT 1)"
#define PLAYS_ARTIST "(SELECT m.artist FROM MEDIA_TABLE m WHERE m.path = PLAY_COUNTS.path LIMIT 1)"

typedef enum {
	GEN_MEDIA = 0, // MEDIA_TABLE and the four lookup tables the scan fills
	GEN_FAVOURITES,
	GEN_PLAYLISTS, // the M3U_ tables: their own, so editing one does not
				   // invalidate a list of tracks and vice versa
	GEN_COUNT,
} gen_domain_t;

static unsigned generation[GEN_COUNT] = {1, 1, 1};

static gen_domain_t domain_of(library_list_t kind) {
	switch (kind) {
	case LIBRARY_LIST_FAVOURITES:
		return GEN_FAVOURITES;
	case LIBRARY_LIST_PLAYLIST:
		return GEN_PLAYLISTS;
	default:
		return GEN_MEDIA;
	}
}

// Call with db_lock held, or from somewhere no query can be in flight.
static void bump_generation(gen_domain_t domain) { generation[domain]++; }

static void bump_all_generations(void) {
	for (int i = 0; i < GEN_COUNT; i++) {
		generation[i]++;
	}
}

unsigned library_revision(library_list_t kind) {
	pthread_mutex_lock(&db_lock);
	unsigned value = generation[domain_of(kind)];
	pthread_mutex_unlock(&db_lock);
	return value;
}

static pthread_t scan_thread;
static volatile bool scan_running;
static volatile bool scan_cancel;
// Set when the database itself stops answering -- a failed prepare, a
// transaction that will not commit. The walk stops on scan_cancel, and this
// says the stop was a fault rather than the user pressing back.
static volatile bool scan_db_failed;
// The user asked the scan to stop, as opposed to the scan stopping itself on a
// database fault.
static volatile bool scan_stop_asked;
static volatile int scan_found;
static char scan_root[512];
static char scan_folder[512];
static pthread_mutex_t scan_folder_lock = PTHREAD_MUTEX_INITIALIZER;

// Whether every file is named in the log as it is read. Developer options has
// the switch; see library_set_log_database.
static bool scan_log_files;

// What the scan thread is doing: building the index from nothing, bringing it
// up to date with the card (Detect changes, see library_card_returned), or
// filing what is already in it again under changed settings
// (library_reorganize).
typedef enum {
	SCAN_FULL,
	SCAN_UPDATE,
	SCAN_REORGANIZE,
} scan_mode_t;
static volatile scan_mode_t scan_mode;

// Settings changed while the scan thread was busy: it files the index again
// under `pending_rules` before it finishes.
static pthread_mutex_t rules_lock = PTHREAD_MUTEX_INITIALIZER;
static bool reorganize_pending;

// Added to the id column of every row written. Zero for a scan, which starts
// from an empty table; past the highest id already there for an update, so the
// new rows do not take numbers the old ones have.
static int scan_id_base;

static library_update_listener_t update_listener;

// Whether ARTIST_LINK, ALBUM_ARTIST_LINK and GENRE_LINK hold anything, and so
// whether a filter by artist, album artist or genre has to look there as well
// as in MEDIA_TABLE's own column.
static volatile bool artist_links;
static volatile bool album_artist_links;
static volatile bool genre_links;

// The file the index was opened from, and which file that was, so that a
// delete or a replacement under the open handle can be noticed.
static char db_file[512];
static file_identity_t db_identity;

// The database file itself is damaged: SQLite answered SQLITE_CORRUPT or
// SQLITE_NOTADB. A file left so by a power cut mid-write stays so through every
// reboot and every factory reset, and every scan into it fails. The next full
// scan puts it aside and starts a new one (replace_damaged()).
static volatile bool db_damaged;

static void note_damage(int rc) {
	rc &= 0xff;
	if ((rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB) && !db_damaged) {
		db_damaged = true;
		fprintf(stderr, "library: the database file is damaged; the next scan starts a new one\n");
	}
}

// ---------------------------------------------------------------------------
// collation
// ---------------------------------------------------------------------------

// Groups, in the order they sort. A string is placed by its first character,
// so "…" comes before "9 Crimes" comes before "Zappa" comes before a title in
// kana, which comes before one in Chinese.
typedef enum {
	SORT_GROUP_EMPTY = 0,
	SORT_GROUP_SYMBOL,
	SORT_GROUP_DIGIT,
	SORT_GROUP_LATIN,
	SORT_GROUP_GREEK,
	SORT_GROUP_CYRILLIC,
	SORT_GROUP_KANA,	// Japanese hiragana/katakana
	SORT_GROUP_CJK,		// Chinese (and Japanese kanji, which share the block)
	SORT_GROUP_HANGUL,	// Korean
	SORT_GROUP_OTHER,
} sort_group_t;

// Decodes one UTF-8 character. Advances *pos past it. Invalid bytes are
// returned as themselves so a mis-encoded tag still sorts somewhere sensible
// instead of stopping the comparison.
static uint32_t utf8_next(const char *text, int length, int *pos) {
	if (*pos >= length) {
		return 0;
	}

	unsigned char first = (unsigned char)text[*pos];
	int extra;
	uint32_t code;

	if (first < 0x80) {
		(*pos)++;
		return first;
	} else if ((first & 0xE0) == 0xC0) {
		extra = 1;
		code = first & 0x1F;
	} else if ((first & 0xF0) == 0xE0) {
		extra = 2;
		code = first & 0x0F;
	} else if ((first & 0xF8) == 0xF0) {
		extra = 3;
		code = first & 0x07;
	} else {
		(*pos)++;
		return first;
	}

	if (*pos + extra >= length) {
		(*pos)++;
		return first;
	}

	for (int i = 1; i <= extra; i++) {
		unsigned char continuation = (unsigned char)text[*pos + i];
		if ((continuation & 0xC0) != 0x80) {
			(*pos)++;
			return first; // truncated sequence
		}
		code = (code << 6) | (continuation & 0x3F);
	}

	*pos += extra + 1;
	return code;
}

// The other direction: one code point into `out`, which needs four bytes.
// Returns how many were written.
//
// Encoded by hand rather than through a library: what has to hold is that
// these bytes compare the way the code points do, and that is a property of
// this encoder, not of somebody else's. The sort key leans on it (see
// library_sort_key), and utf8_next never returns more than four bytes' worth,
// so the last branch is the top of the range and nothing is lost.
static int utf8_encode(uint32_t code, char *out) {
	if (code < 0x80) {
		out[0] = (char)code;
		return 1;
	}
	if (code < 0x800) {
		out[0] = (char)(0xC0 | (code >> 6));
		out[1] = (char)(0x80 | (code & 0x3F));
		return 2;
	}
	if (code < 0x10000) {
		out[0] = (char)(0xE0 | (code >> 12));
		out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
		out[2] = (char)(0x80 | (code & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | ((code >> 18) & 0x07));
	out[1] = (char)(0x80 | ((code >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((code >> 6) & 0x3F));
	out[3] = (char)(0x80 | (code & 0x3F));
	return 4;
}

static sort_group_t sort_group_of(uint32_t code) {
	if (code == 0) {
		return SORT_GROUP_EMPTY;
	}
	if (code >= '0' && code <= '9') {
		return SORT_GROUP_DIGIT;
	}
	if ((code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z')) {
		return SORT_GROUP_LATIN;
	}
	if (code < 0x80) {
		return SORT_GROUP_SYMBOL; // the rest of ASCII: punctuation and space
	}
	if (code >= 0xC0 && code <= 0x24F) {
		return SORT_GROUP_LATIN; // accented Latin, Latin Extended-A/B
	}
	if (code >= 0x370 && code <= 0x3FF) {
		return SORT_GROUP_GREEK;
	}
	if (code >= 0x400 && code <= 0x4FF) {
		return SORT_GROUP_CYRILLIC;
	}
	if ((code >= 0x3040 && code <= 0x30FF) || (code >= 0x31F0 && code <= 0x31FF) ||
		(code >= 0xFF66 && code <= 0xFF9D)) {
		return SORT_GROUP_KANA;
	}
	if ((code >= 0x3400 && code <= 0x4DBF) || (code >= 0x4E00 && code <= 0x9FFF) ||
		(code >= 0xF900 && code <= 0xFAFF)) {
		return SORT_GROUP_CJK;
	}
	if ((code >= 0x1100 && code <= 0x11FF) || (code >= 0xAC00 && code <= 0xD7AF)) {
		return SORT_GROUP_HANGUL;
	}
	if (code >= 0x2000 && code <= 0x2BFF) {
		return SORT_GROUP_SYMBOL; // punctuation, arrows, symbols
	}
	return SORT_GROUP_OTHER;
}

// Accented Latin folded to the letter underneath, so "Evolution" and
// "Evolution" with an acute land next to each other instead of the accented
// one being exiled past Z. Covers Latin-1 Supplement and Latin Extended-A,
// which is every accent a European tag realistically carries.
static uint32_t fold_latin_accent(uint32_t code) {
	static const char LATIN1[] = "aaaaaaaceeeeiiii" // C0-CF
								 "dnooooo\0ouuuuyps"  // D0-DF
								 "aaaaaaaceeeeiiii" // E0-EF
								 "dnooooo\0ouuuuypy"; // F0-FF

	if (code >= 0xC0 && code <= 0xFF) {
		char folded = LATIN1[code - 0xC0];
		return folded ? (uint32_t)folded : code; // D7/F7 are the maths signs
	}

	// Latin Extended-A, one entry per code point from 0x100 to 0x17F. Written
	// out run by run rather than counted by hand: the table this replaces was
	// one letter short at U+0134 and every entry after it was shifted, which
	// put S-caron among the Ts and L-stroke among the Ns -- in the sort and,
	// once there was one, on the A-Z strip.
	if (code >= 0x100 && code <= 0x17F) {
		static const char EXT_A[] = "aaaaaa"		// 100-105 A with macron, breve, ogonek
									"cccccccc"		// 106-10D
									"dddd"			// 10E-111
									"eeeeeeeeee"	// 112-11B
									"gggggggg"		// 11C-123
									"hhhh"			// 124-127
									"iiiiiiiiiiii"	// 128-133, the IJ ligature included
									"jj"			// 134-135
									"kkk"			// 136-138
									"llllllllll"	// 139-142, L with stroke included
									"nnnnnnnnn"		// 143-14B
									"oooooooo"		// 14C-153, the OE ligature included
									"rrrrrr"		// 154-159
									"ssssssss"		// 15A-161
									"tttttt"		// 162-167
									"uuuuuuuuuuuu"	// 168-173
									"ww"			// 174-175
									"yyy"			// 176-178
									"zzzzzz"		// 179-17E
									"s";			// 17F the long s
		size_t index = code - 0x100;
		if (index < sizeof(EXT_A) - 1) {
			return (uint32_t)EXT_A[index];
		}
	}

	return code;
}

// Case folding, far enough for the alphabets that have a case at all.
static uint32_t fold_case(uint32_t code) {
	if (code >= 'A' && code <= 'Z') {
		return code + 32;
	}
	if (code >= 0xC0 && code <= 0x17F) {
		return fold_latin_accent(code);
	}
	if (code >= 0x391 && code <= 0x3A9) {
		return code + 32; // Greek
	}
	if (code >= 0x410 && code <= 0x42F) {
		return code + 32; // Cyrillic
	}
	if (code == 0x401 || code == 0x451) {
		return 0x435; // Ё files under Е, the way an accent files under its letter
	}
	if (code >= 0x400 && code <= 0x40F) {
		return code + 80; // Cyrillic Ё and friends
	}
	return code;
}

// Whether a leading article is skipped when a name is filed and compared. On by
// default, which is what puts "The Beatles" under B.
//
// It reaches the lists through the sort key, which is written at scan time, so
// turning it off only changes what is on screen after the next scan -- and that
// is the honest behaviour to promise, because the alternative is a strip of
// letters that no longer matches the order of the rows beside it. The
// comparison itself follows the switch straight away, which is what the
// audiobook index (no stored key, see audiobookdb.c) needs.
static bool skip_articles = true;

void library_set_skip_articles(bool on) { skip_articles = on; }
bool library_skip_articles(void) { return skip_articles; }

// Skips a leading article so "The Beatles" files under B. Only English and
// Italian, which is what a tag on this device is most likely to be in.
static int skip_article(const char *text, int length) {
	if (!skip_articles) {
		return 0;
	}
	static const char *const ARTICLES[] = {"the ", "a ",  "an ", "il ",  "lo ", "la ", "i ",
										   "gli ", "le ", "l'",  "un ", "uno ", "una "};

	for (size_t i = 0; i < sizeof(ARTICLES) / sizeof(ARTICLES[0]); i++) {
		int article_length = (int)strlen(ARTICLES[i]);
		if (length > article_length && strncasecmp(text, ARTICLES[i], (size_t)article_length) == 0) {
			return article_length;
		}
	}
	return 0;
}

// The collation the schema names. Compares group first, then folded code
// points, then length -- so the result is a total order and SQLite is happy.
// Non-static: the audiobook database registers the very same ordering (see
// library.h), so "The Hobbit" files under H there just as "The Beatles" files
// under B here.
int library_collate_listorder(void *unused, int len_a, const void *a, int len_b, const void *b) {
	(void)unused;

	const char *text_a = a;
	const char *text_b = b;

	int pos_a = skip_article(text_a, len_a);
	int pos_b = skip_article(text_b, len_b);

	uint32_t first_a = 0, first_b = 0;
	int peek_a = pos_a, peek_b = pos_b;
	first_a = utf8_next(text_a, len_a, &peek_a);
	first_b = utf8_next(text_b, len_b, &peek_b);

	sort_group_t group_a = sort_group_of(first_a);
	sort_group_t group_b = sort_group_of(first_b);

	if (group_a != group_b) {
		return group_a < group_b ? -1 : 1;
	}

	for (;;) {
		uint32_t code_a = utf8_next(text_a, len_a, &pos_a);
		uint32_t code_b = utf8_next(text_b, len_b, &pos_b);

		if (code_a == 0 || code_b == 0) {
			if (code_a == code_b) {
				return 0;
			}
			return code_a == 0 ? -1 : 1;
		}

		code_a = fold_case(code_a);
		code_b = fold_case(code_b);

		if (code_a != code_b) {
			return code_a < code_b ? -1 : 1;
		}
	}
}

// The collation's order, written down as bytes.
//
// library_collate_listorder() above compares two things, in order: the script
// group of the first character, then the folded code points one at a time.
// Both go into a string whose plain byte order is that same order -- the group
// as one printable digit, then the folded code points re-encoded as UTF-8,
// whose byte order is code point order, which is exactly what the collation
// compares. Where one string runs out first, memcmp over the shorter length
// and then the lengths is the same answer the collation gives, and that is what
// SQLite's BINARY collation does.
//
// So "ORDER BY sortkey" answers what "ORDER BY name COLLATE listorder" answers,
// and an ordinary index can serve it. That is the whole point: the collation is
// registered at runtime, so no index can be built on it, and a list ordered by
// it has to sort the entire table into a temporary b-tree before drawing a row.
//
// The key holds no zero byte and is never empty, so it stores as TEXT.
void library_sort_key(const char *text, char *out, size_t out_size) {
	if (!out || out_size < 2) {
		return;
	}
	if (!text) {
		text = "";
	}

	int length = (int)strlen(text);
	int pos = skip_article(text, length);

	// The group of the first character, unfolded -- the collation looks at it
	// before it folds anything. The enum's numbering is the order the groups
	// sort in, and there are ten of them, so one digit says it and compares the
	// right way round.
	int peek = pos;
	uint32_t first = utf8_next(text, length, &peek);
	size_t at = 0;
	out[at++] = (char)('0' + (int)sort_group_of(first));

	for (;;) {
		uint32_t code = utf8_next(text, length, &pos);
		if (code == 0) {
			break;
		}
		code = fold_case(code);

		char encoded[4];
		int bytes = utf8_encode(code, encoded);

		if (at + (size_t)bytes >= out_size) {
			// Two names sharing five hundred bytes of prefix get the same key
			// and land next to each other in an order nothing else decides.
			break;
		}
		memcpy(out + at, encoded, (size_t)bytes);
		at += (size_t)bytes;
	}

	out[at] = '\0';
}

// sortkey(x) inside SQL, so a database written before the column existed can be
// filled in with one statement per batch instead of a round trip per row.
static void sortkey_sql(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
	const char *text = argc > 0 ? (const char *)sqlite3_value_text(argv[0]) : NULL;
	char key[LIBRARY_SORT_KEY_MAX];
	library_sort_key(text, key, sizeof(key));
	sqlite3_result_text(ctx, key, -1, SQLITE_TRANSIENT);
}

// The same folding the collation applies, over a whole string: case dropped and
// an accented Latin letter reduced to the letter underneath. Truncates rather
// than overflows. Returns the bytes written.
//
// Folding never lengthens a character -- an accented letter shrinks to one
// byte, Cyrillic and Greek keep their two -- so a buffer the size of the input
// is always enough.
static size_t fold_text(const char *text, char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return 0;
	}
	if (!text) {
		text = "";
	}

	int length = (int)strlen(text);
	int pos = 0;
	size_t at = 0;

	for (;;) {
		uint32_t code = utf8_next(text, length, &pos);
		if (code == 0) {
			break;
		}
		char encoded[4];
		int bytes = utf8_encode(fold_case(code), encoded);
		if (at + (size_t)bytes >= out_size) {
			break;
		}
		memcpy(out + at, encoded, (size_t)bytes);
		at += (size_t)bytes;
	}

	out[at] = '\0';
	return at;
}

// foldcase(x) inside SQL, which is what makes the search case-insensitive.
//
// SQLite's LIKE folds case for ASCII and for ASCII only, so on its own "кино"
// does not match "Кино" and "BJORK" does not match "Björk". The COLLATE
// NOCASE on the columns does not help: LIKE ignores collations. So both sides
// are folded instead, the row here and the query in library_search(), and LIKE
// is left comparing two strings that have already had the question of case
// taken out of them.
//
// It costs a call per row on a scan that was already reading every row: the
// pattern starts with a wildcard, so no index could serve it either way.
static void foldcase_sql(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
	const char *text = argc > 0 ? (const char *)sqlite3_value_text(argv[0]) : NULL;
	char folded[LIBRARY_SORT_KEY_MAX];
	fold_text(text, folded, sizeof(folded));
	sqlite3_result_text(ctx, folded, -1, SQLITE_TRANSIENT);
}

// Whose an album is, as a short key: what tells apart two records with the same
// name. The album artist, or the artist on a file that has none -- the scan
// keeps whichever it found in MEDIA_TABLE.album_artist -- and the folder on a
// file that has neither.
//
// A hash and not the name itself, so that a value naming an album is its name
// plus ten characters and still fits where a name alone did. The name is folded
// first, the way the collation compares it, so "ABBA" and "Abba" are one
// artist; the folder only has ASCII case dropped, the way FAT and exFAT
// compare paths. The leading letter keeps an artist from ever meeting a folder
// with the same hash.
#define ALBUM_KEY_MAX 12

static uint32_t fnv1a(const char *text, uint32_t hash) {
	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		hash = (hash ^ *p) * 16777619u;
	}
	return hash;
}

static void album_key(const char *album_artist, const char *path, char *out, size_t size) {
	if (album_artist && album_artist[0]) {
		char folded[LIBRARY_SORT_KEY_MAX];
		fold_text(album_artist, folded, sizeof(folded));
		snprintf(out, size, "a%08x", (unsigned)fnv1a(folded, 2166136261u));
		return;
	}
	char folder[512];
	snprintf(folder, sizeof(folder), "%s", path ? path : "");
	char *slash = strrchr(folder, '/');
	if (slash) {
		*slash = '\0';
	}
	for (char *c = folder; *c; c++) {
		*c = (char)tolower((unsigned char)*c);
	}
	snprintf(out, size, "d%08x", (unsigned)fnv1a(folder, 2166136261u));
}

static void albumkey_sql(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
	const char *album_artist = argc > 0 ? (const char *)sqlite3_value_text(argv[0]) : NULL;
	const char *path = argc > 1 ? (const char *)sqlite3_value_text(argv[1]) : NULL;
	char key[ALBUM_KEY_MAX];
	album_key(album_artist, path, key, sizeof(key));
	sqlite3_result_text(ctx, key, -1, SQLITE_TRANSIENT);
}

// A track row that belongs to the album value bound as ?1: its name, and its key
// when the value has one. A value saved before albums had keys is a name alone
// and still matches every album of that name. Constant in the row, so the
// lookup still goes through media_album_key_idx.
#define ALBUM_VALUE_MATCH                                                                                             \
	"album = (CASE WHEN instr(?1, char(31)) > 0 THEN substr(?1, 1, instr(?1, char(31)) - 1) ELSE ?1 END)"             \
	" AND (instr(?1, char(31)) = 0 OR album_key = substr(?1, instr(?1, char(31)) + 1))"

// A row of ALBUM_GROUP_TABLE as an album value: the name, the separator and the key.
#define ALBUM_GROUP_VALUE "album || char(31) || album_key"

// When the newest file of the ALBUM_GROUP_TABLE row `g` arrived: how new the
// album is, for the lists that run by date.
#define ALBUM_GROUP_NEWEST                                                                                            \
	"(SELECT MAX(m.ctime) FROM MEDIA_TABLE m WHERE m.album = g.album AND m.album_key = g.album_key)"

// The year of the ALBUM_GROUP_TABLE row `g`: the latest its tracks carry, NULL
// when none of them carries one. A reissue tagged track by track with the
// original year and one bonus track with its own still files under the reissue.
#define ALBUM_GROUP_YEAR                                                                                              \
	"(SELECT MAX(CASE WHEN m.year > 0 THEN m.year END) FROM MEDIA_TABLE m"                                           \
	" WHERE m.album = g.album AND m.album_key = g.album_key)"

// A year as a sort key that sends the rows without one to the end, whichever
// way the years run: past any real year going up, below any going down.
#define YEAR_KEY_UP(expr) "IFNULL(" expr ", 1000000)"
#define YEAR_KEY_DOWN(expr) "IFNULL(" expr ", -1) DESC"
#define TRACK_YEAR "(CASE WHEN year > 0 THEN year END)"

// The first track of the ALBUM_GROUP_TABLE row `g`, by disc and track number,
// with the columns asked for: the one whose cover and artist the row shows.
#define ALBUM_GROUP_FIRST(columns)                                                                                    \
	"(SELECT " columns " FROM MEDIA_TABLE m WHERE m.album = g.album AND m.album_key = g.album_key" \
	" ORDER BY COALESCE(m.disc,1), m.dis_id LIMIT 1)"

// The single character a name is filed under in an A-Z index, and the group it
// belongs to. Latin letters come back upper-cased; everything else is stored
// as-is.
static void index_character(const char *text, char *out, size_t out_size, int *group_out) {
	snprintf(out, out_size, "%s", "#");
	*group_out = SORT_GROUP_SYMBOL;

	if (!text || !text[0]) {
		*group_out = SORT_GROUP_EMPTY;
		return;
	}

	int length = (int)strlen(text);
	int pos = skip_article(text, length);
	int start = pos;

	uint32_t code = utf8_next(text, length, &pos);
	sort_group_t group = sort_group_of(code);
	*group_out = group;

	if (group == SORT_GROUP_LATIN && code < 0x80) {
		char letter = (char)(code >= 'a' && code <= 'z' ? code - 32 : code);
		snprintf(out, out_size, "%c", letter);
		return;
	}

	if (group == SORT_GROUP_DIGIT) {
		snprintf(out, out_size, "%s", "0-9");
		return;
	}

	// Everything else is filed under the character itself.
	int bytes = pos - start;
	if (bytes > 0 && (size_t)bytes < out_size) {
		memcpy(out, text + start, (size_t)bytes);
		out[bytes] = '\0';
	}
}

// The A-Z letter a sort key stands for.
//
// The key already holds the answer: its first byte is the script group and the
// byte after it is the first folded character, which for a Latin name is the
// lower-case letter itself. So the strip can be counted from the key the list
// is ordered by, and the name never has to be read -- which is what lets the
// sortkey index answer the whole ordered pass on its own.
//
// It has to agree with library_index_letter() on every name there is.
char library_index_letter_of_key(const char *key) {
	if (!key || !key[0]) {
		return '#';
	}

	int group = key[0] - '0';
	if (group > (int)SORT_GROUP_LATIN) {
		return LIBRARY_INDEX_PAST_Z;
	}
	if (group != (int)SORT_GROUP_LATIN) {
		return '#';
	}

	unsigned char folded = (unsigned char)key[1];
	if (folded >= 'a' && folded <= 'z') {
		return (char)(folded - 32);
	}
	// A Latin letter with no plain form to fold to sorts past every a-z, which
	// is the answer library_index_letter() gives it too.
	return LIBRARY_INDEX_PAST_Z;
}

int library_index_slot(char letter) {
	if (letter >= 'A' && letter <= 'Z') {
		return letter - 'A' + 1;
	}
	if (letter == LIBRARY_INDEX_PAST_Z) {
		return LIBRARY_INDEX_PAST_Z_SLOT;
	}
	return 0;
}

char library_index_letter(const char *name) {
	if (!name || !name[0]) {
		return '#';
	}

	int length = (int)strlen(name);
	int pos = skip_article(name, length);
	uint32_t code = utf8_next(name, length, &pos);

	// '#' is the head of the list -- the empty names, the punctuation and the
	// digits, which the collation files above A. What it is NOT is a bin for
	// everything unusual: Greek, Cyrillic, kana, Hangul and CJK are filed BELOW
	// Z, and lumping them in with the digits would make one bucket stand for
	// two ends of the same list. They get '~' instead, which says "past Z" and
	// which an index leaves out rather than pointing at the wrong end.
	sort_group_t group = sort_group_of(code);
	if (group > SORT_GROUP_LATIN) {
		return LIBRARY_INDEX_PAST_Z;
	}
	if (group != SORT_GROUP_LATIN) {
		return '#';
	}

	code = fold_case(code); // an accented letter files under the plain one
	if (code >= 'a' && code <= 'z') {
		return (char)(code - 32);
	}
	// A Latin letter with no plain form to fold to (Latin Extended-B and the
	// odd unmapped sign) sorts by its own code point, which is past every
	// a-z: same answer as the other scripts.
	return LIBRARY_INDEX_PAST_Z;
}

// ---------------------------------------------------------------------------
// database
// ---------------------------------------------------------------------------

static bool exec(const char *sql) {
	char *error = NULL;
	int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
	if (rc != SQLITE_OK) {
		note_damage(rc);
		fprintf(stderr, "library: %s\n", error ? error : "unknown error");
		sqlite3_free(error);
		return false;
	}
	return true;
}

// The same, for a statement whose failure is the expected answer: adding a
// column that is already there. Logging that on every boot would be five lines
// of noise saying nothing went wrong.
static bool exec_quiet(const char *sql) {
	return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
}

// A monotonic millisecond reading, only ever used as a difference.
static uint32_t now_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)(now.tv_sec * 1000u) + (uint32_t)(now.tv_nsec / 1000000);
}

// See the call site. Points SQLite's scratch files at the folder the database
// lives in. Held in a static because sqlite3_temp_directory is a bare pointer
// SQLite keeps and never copies.
static char temp_dir[512];

static void set_temp_directory(const char *db_path) {
	// Built somewhere else first. This runs again at every card change, and
	// rewriting the buffer SQLite is holding would move the ground under it;
	// and a failure has to put the pointer back, not leave it on the folder of
	// a card that has gone.
	char wanted[sizeof(temp_dir)];
	snprintf(wanted, sizeof(wanted), "%s", db_path);
	char *slash = strrchr(wanted, '/');
	if (!slash) {
		sqlite3_temp_directory = NULL; // a bare name: no folder to point at
		return;
	}
	*slash = '\0';
	if (wanted[0] == '\0') {
		wanted[0] = '/';
		wanted[1] = '\0';
	}

	// A card that cannot be written to would turn every sort into an error
	// instead of a slow sort, so the default stands where the card is
	// read-only.
	if (access(wanted, W_OK) != 0) {
		fprintf(stderr, "library: %s is not writable; sorts will spill to the default temp folder\n", wanted);
		sqlite3_temp_directory = NULL;
		return;
	}

	snprintf(temp_dir, sizeof(temp_dir), "%s", wanted);
	sqlite3_temp_directory = temp_dir;
}

// ---------------------------------------------------------------------------
// Filling in the sort keys
//
// A database written before the column existed gains the column empty. A list
// ordered by a column full of nulls is not ordered, so the lists name the key
// only once every row has one; until then they order by the collation, which is
// correct and slow.
//
// Filling it in is one UPDATE per slice of row ids rather than a round trip per
// row, on a thread of its own at the lowest priority there is. The card is
// slow, the user is looking at a library that already works, and this is the
// one job in the player nobody is waiting for.
// ---------------------------------------------------------------------------

// True once every row of the media tables carries a key. Read without the lock
// while a query is being built: the SQL that comes out is right either way, and
// the worst a stale read can do is order one list by the collation instead.
static volatile bool sortkeys_ready;

static pthread_t backfill_thread;
static bool backfill_started;
static volatile bool backfill_stop;

// Row ids one UPDATE covers. The same reasoning as the scan's commit batch: an
// open transaction holds its dirty pages, and this device has ten megabytes.
#define BACKFILL_BATCH 2000

// The tables that have the column, with the name each key is made from.
static const struct {
	const char *table;
	const char *column;
} KEYED_TABLES[] = {
	{"MEDIA_TABLE", "name"},
	{"ALBUM_TABLE", "album"},
	{"ARTIST_TABLE", "artist"},
	{"ALBUM_ARTIST_TABLE", "album_artist"},
	{"GENRE_TABLE", "genre"},
	{"ALBUM_GROUP_TABLE", "album"},
};

// Runs a list of index statements. A DROP INDEX follows the CREATE of the index
// that replaces it, and runs only if that one exists: on a card too full to
// build it, the old index is all the queries have.
static void index_steps(const char *const *steps, size_t count, const char *what) {
	bool made = true;
	for (size_t i = 0; i < count; i++) {
		if (strncmp(steps[i], "DROP INDEX", 10) == 0 && !made) {
			continue;
		}
		made = exec(steps[i]);
		if (!made) {
			fprintf(stderr, "library: %s step %zu failed\n", what, i);
		}
	}
}

// Whether anything is still without a key. Cheap whatever the library's size:
// nulls sort at the head of the sortkey index, so this is an index seek and not
// a walk of the table.
static int count_rows(const char *sql);

static bool sortkeys_missing(void) {
	bool missing = false;
	pthread_mutex_lock(&db_lock);
	for (size_t i = 0; i < sizeof(KEYED_TABLES) / sizeof(KEYED_TABLES[0]) && !missing; i++) {
		char sql[128];
		snprintf(sql, sizeof(sql), "SELECT 1 FROM %s WHERE sortkey IS NULL LIMIT 1", KEYED_TABLES[i].table);
		missing = count_rows(sql) > 0;
	}
	pthread_mutex_unlock(&db_lock);
	return missing;
}

// The indexes over the sort key. Separate from the rest because they are worth
// having only once every row has a key: kept up to date through a backfill they
// would be one b-tree insertion per row instead of a single bulk build, an
// order of magnitude slower on a large library.
static void build_sort_indexes(void) {
	pthread_mutex_lock(&db_lock);
	if (!db) {
		pthread_mutex_unlock(&db_lock);
		return;
	}
	index_steps(SORT_INDEXES, sizeof(SORT_INDEXES) / sizeof(SORT_INDEXES[0]), "sort index");
	pthread_mutex_unlock(&db_lock);
}

static void *backfill_worker(void *arg) {
	(void)arg;
	thread_be_background("sortkeys");

	uint32_t started_ms = now_ms();
	int filled = 0;

	// The four lookup tables in one statement each: a card holds thousands of
	// albums, not hundreds of thousands.
	for (size_t i = 1; i < sizeof(KEYED_TABLES) / sizeof(KEYED_TABLES[0]) && !backfill_stop; i++) {
		char sql[192];
		snprintf(sql, sizeof(sql), "UPDATE %s SET sortkey=sortkey(%s) WHERE sortkey IS NULL", KEYED_TABLES[i].table,
				 KEYED_TABLES[i].column);
		pthread_mutex_lock(&db_lock);
		if (db && exec(sql)) {
			filled += sqlite3_changes(db);
		}
		pthread_mutex_unlock(&db_lock);
	}

	// And the tracks a slice of row ids at a time, so the lock is never held
	// long and no transaction grows past a few hundred rows.
	sqlite3_int64 highest = 0;
	pthread_mutex_lock(&db_lock);
	highest = db ? (sqlite3_int64)count_rows("SELECT IFNULL(MAX(rowid),0) FROM MEDIA_TABLE") : 0;
	pthread_mutex_unlock(&db_lock);

	for (sqlite3_int64 from = 0; from < highest && !backfill_stop; from += BACKFILL_BATCH) {
		// A scan writes the key with the row, so it does this job better than
		// this does; and it empties the tables first, which would leave this
		// walking row ids that mean something else now.
		if (scan_running) {
			break;
		}
		pthread_mutex_lock(&db_lock);
		sqlite3_stmt *stmt = NULL;
		// By row id and nothing else. Adding "AND sortkey IS NULL" reads well
		// and costs dearly: SQLite then has the choice of the sortkey index,
		// whose null entries are all at its head, and walks them for every
		// batch. Rewriting a key that is already right is free by comparison.
		if (db && sqlite3_prepare_v2(db,
									 "UPDATE MEDIA_TABLE SET sortkey=sortkey(name)"
									 " WHERE rowid > ? AND rowid <= ?",
									 -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, from);
			sqlite3_bind_int64(stmt, 2, from + BACKFILL_BATCH);
			sqlite3_step(stmt);
			sqlite3_finalize(stmt);
			filled += sqlite3_changes(db);
		}
		pthread_mutex_unlock(&db_lock);
	}

	if (!backfill_stop && !sortkeys_missing()) {
		build_sort_indexes();
		sortkeys_ready = true;
		printf("library: %d sort keys filled in in %u ms; the lists are indexed from now on\n", filled,
			   now_ms() - started_ms);
	}
	return NULL;
}

static void sortkeys_start_backfill(void) {
	// The ordinary case: every boot but the first one after the upgrade.
	if (!sortkeys_missing()) {
		build_sort_indexes();
		sortkeys_ready = true;
		return;
	}

	printf("library: the index predates the sort keys; filling them in the background\n");
	backfill_stop = false;
	if (pthread_create(&backfill_thread, NULL, backfill_worker, NULL) == 0) {
		backfill_started = true;
	} else {
		fprintf(stderr, "library: cannot fill in the sort keys; the lists keep the old ordering\n");
	}
}

static void backfill_stop_and_wait(void) {
	if (!backfill_started) {
		return;
	}
	backfill_stop = true;
	pthread_join(backfill_thread, NULL);
	backfill_started = false;
}

// Whether the ordered query may name the key. Favourites never may: they have
// no such column and are ordered by when they were starred, not by name.
static bool list_uses_sortkey(library_list_t kind) {
	// Favourites run by when they were starred and a playlist by the order it
	// was built, so neither has a sort key to read.
	return sortkeys_ready && kind != LIBRARY_LIST_FAVOURITES && kind != LIBRARY_LIST_PLAYLIST &&
		   kind != LIBRARY_LIST_MOST_PLAYED;
}

static void next_mount_serial(void);
static void links_check(void);

// A database indexed by a build that did not tell same-named albums apart has
// ALBUM_TABLE and nothing in ALBUM_GROUP_TABLE. A scan fills the new table as
// it goes; an index that is already there gets it here, once, in one statement
// over the tracks rather than a rescan of the card.
static void album_groups_fill(void) {
	if (count_rows("SELECT 1 FROM ALBUM_GROUP_TABLE LIMIT 1") > 0 ||
		count_rows("SELECT 1 FROM MEDIA_TABLE WHERE album <> '' LIMIT 1") == 0) {
		return;
	}
	uint32_t started_ms = now_ms();
	exec("INSERT OR IGNORE INTO ALBUM_GROUP_TABLE(album, album_key, sortkey)"
		 " SELECT album, albumkey(album_artist, path), sortkey(album) FROM MEDIA_TABLE WHERE album <> ''");
	printf("library: %d albums told apart by artist in %u ms\n", count_rows("SELECT COUNT(*) FROM ALBUM_GROUP_TABLE"),
		   now_ms() - started_ms);
}

// The album_key column on an index written before it existed: one statement
// over the tracks, once, the same as album_groups_fill() above. A scan writes
// it with every row from then on.
static void album_keys_fill(void) {
	if (count_rows("SELECT 1 FROM MEDIA_TABLE WHERE album_key IS NULL LIMIT 1") == 0) {
		return;
	}
	uint32_t started_ms = now_ms();
	exec("UPDATE MEDIA_TABLE SET album_key = albumkey(album_artist, path) WHERE album_key IS NULL");
	printf("library: album keys written down for %d tracks in %u ms\n", sqlite3_changes(db), now_ms() - started_ms);
}

// library_history_revision()'s answer, read from the table once and then kept
// up by library_history_add(). -1 until it has been read.
static long long history_newest = -1;

// Opens the file and brings its schema up to date: everything library_open()
// does before the index is shared with other threads.
static bool db_open_file(const char *db_path) {
	history_newest = -1; // a file opened afresh is read afresh

	// A ceiling SQLite will stay under by recycling instead of growing: every
	// database in the player together, the library's DB_CACHE_KIB of page
	// cache included. At 1.5 MB that cache could never fill, and the lists'
	// reads went to the card every time -- the kernel's own cache does not keep
	// the file either on a device this short of memory. The scans still give
	// their pages back when they are done (sqlite3_db_release_memory below).
	sqlite3_soft_heap_limit64(4 * 1024 * 1024);

	if (sqlite3_open(db_path, &db) != SQLITE_OK) {
		fprintf(stderr, "library: cannot open %s: %s\n", db_path, sqlite3_errmsg(db));
		sqlite3_close(db);
		db = NULL;
		return false;
	}

	// Every ORDER BY in the schema names this collation; without it the
	// queries fail outright rather than sorting badly.
	sqlite3_create_collation(db, "listorder", SQLITE_UTF8, NULL, library_collate_listorder);

	// And the same ordering as a value the SQL can compute, for filling in a
	// database written before the column existed.
	sqlite3_create_function(db, "sortkey", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, sortkey_sql, NULL, NULL);

	// And the folding on its own, which the search matches through.
	sqlite3_create_function(db, "foldcase", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, foldcase_sql, NULL,
							NULL);

	// And whose an album is, for telling apart two records with one name.
	sqlite3_create_function(db, "albumkey", 2, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, albumkey_sql, NULL, NULL);

	// Where SQLite spills a sort it cannot hold in memory.
	//
	// The lists themselves read the sortkey index in order, but the queries
	// that do sort (one album by disc position, one artist's records) go
	// through a temporary b-tree, and anything past about a megabyte of it goes
	// to a scratch file. SQLITE_TEMP_STORE is 1 (file, not memory) precisely so
	// it does not sit in RAM, but with nothing set the unix VFS falls back to
	// /tmp, which on this device is tmpfs -- the RAM it was avoiding, by another
	// name. Next to the database instead: the card is slow, but a sort that
	// reaches here is already the slow part.
	set_temp_directory(db_path);

	// A card is slow and may be pulled out at any moment. WAL needs shared
	// memory the FAT driver does not provide, so stay on the default journal
	// and just stop waiting for the platter on every commit.
	exec("PRAGMA synchronous=NORMAL");
	exec("PRAGMA journal_mode=TRUNCATE");
	// The page cache the player keeps for the life of the database: big enough
	// to hold most of a library's index. It once stood at 256 KB, and the lists
	// read their rows a windowful at a time, by row id -- pages that had gone
	// by then were read again from the card. On the Blaze that put up to 140 ms
	// of card reads into a single step of a scroll (a profiled run, the
	// db_list_window mark). Two megabytes hold a library of a few thousand
	// tracks nearly whole once it has been read.
	exec("PRAGMA cache_size=" DB_CACHE_KIB);

	for (size_t i = 0; i < sizeof(SCHEMA) / sizeof(SCHEMA[0]); i++) {
		if (!exec(SCHEMA[i])) {
			fprintf(stderr, "library: schema step %zu failed\n", i);
		}
	}
	// Quietly: on every database but the first one opened after the upgrade,
	// each of these is "duplicate column name", which is the answer that says
	// there is nothing to do.
	for (size_t i = 0; i < sizeof(SCHEMA_COLUMNS) / sizeof(SCHEMA_COLUMNS[0]); i++) {
		exec_quiet(SCHEMA_COLUMNS[i]);
	}
	index_steps(SCHEMA_INDEXES, sizeof(SCHEMA_INDEXES) / sizeof(SCHEMA_INDEXES[0]), "index");

	album_groups_fill();
	album_keys_fill();
	return true;
}

bool library_open(const char *db_path) {
	if (db) {
		return true;
	}
	if (!db_path || !db_path[0] || !db_open_file(db_path)) {
		return false;
	}

	pthread_mutex_lock(&db_lock);
	links_check();
	bump_all_generations(); // a different card is a different set of row ids
	pthread_mutex_unlock(&db_lock);

	BOOTPROF_CALL("next_mount_serial", next_mount_serial());

	snprintf(db_file, sizeof(db_file), "%s", db_path);
	file_identity_read(db_path, &db_identity);

	// No count here: COUNT(*) walks a whole index, on the way to the first frame.
	printf("library: %s open\n", db_path);

	sortkeys_start_backfill();
	return true;
}

static void library_scan_stop_and_wait(void);
static void finalize_statements(void);
static void finalize_known(void);

void library_close(void) {
	// Waits, rather than just asking: a scan still in flight owns prepared
	// statements, sqlite3_close() then answers SQLITE_BUSY and leaves the file
	// open -- and on this device an open file on the card makes the unmount
	// fail, so the next card cannot be mounted in its place.
	library_scan_stop_and_wait();
	// And the same for the backfill, which holds the lock in short bursts and
	// would be writing to a handle that had just been closed.
	backfill_stop_and_wait();
	sortkeys_ready = false;

	pthread_mutex_lock(&db_lock);
	if (db) {
		finalize_statements();
		finalize_known();
		int rc = sqlite3_close(db);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "library: sqlite3_close returned %d; the handle may be leaking\n", rc);
		}
		db = NULL;
		artist_links = album_artist_links = genre_links = false;
		bump_all_generations(); // every open list is now looking at nothing
	}
	pthread_mutex_unlock(&db_lock);
}

#define PLAYLIST_TABLE_MAX 512
static bool playlist_table(const char *name, char *out, size_t out_size);

// The tables worth carrying out of a damaged file: everything the user made or
// left where it was, which no scan can rebuild. The playlists, one table each,
// are found by name.
static const char *const KEPT_TABLES[] = {
	"FAVOURITES",			"PLAYBACK_STATE",		"PLAYBACK_QUEUE", "PLAYBACK_QUEUE_STATE",
	"PLAYBACK_QUEUE_QUERY", "PLAYBACK_QUEUE_ALBUM", "PLAYLIST_MIGRATION",   "PLAY_COUNTS",
	"PLAY_HISTORY",
};

// Copies one table from the attached damaged file. A table the damage reaches
// fails on its own and is left behind; the transaction keeps a half-copied one
// out. With db_lock held.
static void rescue_table(const char *quoted, const char *create) {
	if (create && !exec(create)) {
		return;
	}
	char sql[2 * PLAYLIST_TABLE_MAX + 96];
	snprintf(sql, sizeof(sql), "INSERT OR IGNORE INTO main.%s SELECT * FROM damaged.%s", quoted, quoted);
	if (!exec("SAVEPOINT rescue")) {
		return;
	}
	if (!exec(sql)) {
		exec("ROLLBACK TO rescue");
	}
	exec("RELEASE rescue");
}

static void rescue_user_tables(const char *damaged) {
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "ATTACH ?1 AS damaged", -1, &stmt, NULL) != SQLITE_OK) {
		return;
	}
	sqlite3_bind_text(stmt, 1, damaged, -1, SQLITE_STATIC);
	int rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE) {
		return;
	}

	for (size_t i = 0; i < sizeof(KEPT_TABLES) / sizeof(KEPT_TABLES[0]); i++) {
		rescue_table(KEPT_TABLES[i], NULL);
	}

	// The playlists: created here with the statement they were made with.
	if (sqlite3_prepare_v2(db,
						   "SELECT name, sql FROM damaged.sqlite_master WHERE type = 'table'"
						   " AND substr(name, 1, length(?1)) = ?1",
						   -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, LIBRARY_PLAYLIST_PREFIX, -1, SQLITE_STATIC);
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *name = (const char *)sqlite3_column_text(stmt, 0);
			const char *create = (const char *)sqlite3_column_text(stmt, 1);
			char quoted[PLAYLIST_TABLE_MAX];
			if (!name || !create || !playlist_table(name + strlen(LIBRARY_PLAYLIST_PREFIX), quoted, sizeof(quoted))) {
				continue;
			}
			rescue_table(quoted, create);
		}
		sqlite3_finalize(stmt);
	}
	exec("DETACH damaged");
}

// Puts the damaged file aside as "<name>.damaged", its journal with it, and
// opens a new one in its place with what can be carried out of the old one.
// Called with no scan writing: before a full scan starts, or from the scan
// thread itself once its run has failed.
static bool replace_damaged(void) {
	char path[sizeof(db_file)];
	char aside[sizeof(db_file) + 16];
	char journal[sizeof(db_file) + 16];
	char aside_journal[sizeof(db_file) + 32];
	snprintf(path, sizeof(path), "%s", db_file);
	if (!path[0]) {
		return false;
	}
	snprintf(aside, sizeof(aside), "%s.damaged", path);
	snprintf(journal, sizeof(journal), "%s-journal", path);
	snprintf(aside_journal, sizeof(aside_journal), "%s-journal", aside);

	backfill_stop_and_wait();
	sortkeys_ready = false;

	pthread_mutex_lock(&db_lock);
	finalize_statements();
	finalize_known();
	if (db) {
		sqlite3_close_v2(db); // statements still held elsewhere keep it alive until they go
		db = NULL;
	}
	artist_links = album_artist_links = genre_links = false;

	unlink(aside);
	unlink(aside_journal);
	bool moved = rename(path, aside) == 0;
	if (moved) {
		rename(journal, aside_journal); // a hot journal belongs to the old file
	}
	bool opened = db_open_file(path);
	if (opened && moved) {
		rescue_user_tables(aside);
	}
	if (opened) {
		links_check();
	}
	bump_all_generations();
	pthread_mutex_unlock(&db_lock);

	if (!opened) {
		fprintf(stderr, "library: cannot open %s again\n", path);
		return false;
	}
	db_damaged = false;
	file_identity_read(path, &db_identity);
	BOOTPROF_CALL("next_mount_serial", next_mount_serial());
	sortkeys_start_backfill();
	printf("library: the damaged database is kept as %s; scanning into a new one\n", moved ? aside : "(not moved)");
	return moved;
}

bool library_reopen_if_replaced(void) {
	if (!library_is_open() || !db_file[0] || !file_identity_changed(db_file, &db_identity)) {
		return false;
	}

	char path[sizeof(db_file)];
	snprintf(path, sizeof(path), "%s", db_file);
	printf("library: %s was deleted or replaced; opening it again\n", path);

	library_close();
	// The whole folder may have gone with it, and SQLite does not make the
	// folders a path needs.
	char *slash = strrchr(path, '/');
	if (slash) {
		*slash = '\0';
		mkdir(path, 0777);
		*slash = '/';
	}
	library_open(path);
	return true;
}

static int count_rows(const char *sql) {
	if (!db) {
		return 0;
	}

	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		note_damage(rc);
		return 0;
	}

	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		count = sqlite3_column_int(stmt, 0);
	}
	sqlite3_finalize(stmt);
	return count;
}

int library_track_count(void) {
	pthread_mutex_lock(&db_lock);
	int count = count_rows("SELECT COUNT(*) FROM MEDIA_TABLE");
	pthread_mutex_unlock(&db_lock);
	return count;
}

int library_list(library_list_t kind, char out[][256], int max) {
	static const struct {
		const char *table;
		const char *column;
	} SOURCES[] = {
		{"MEDIA_TABLE", "name"},
		{"ALBUM_TABLE", "album"},
		{"ARTIST_TABLE", "artist"},
		{"ALBUM_ARTIST_TABLE", "album_artist"},
		{"GENRE_TABLE", "genre"},
	};

	if (kind < 0 || kind >= (int)(sizeof(SOURCES) / sizeof(SOURCES[0])) || max <= 0) {
		return 0;
	}

	char sql[256];
	snprintf(sql, sizeof(sql), "SELECT %s FROM %s WHERE %s <> '' ORDER BY %s COLLATE listorder LIMIT %d",
			 SOURCES[kind].column, SOURCES[kind].table, SOURCES[kind].column, SOURCES[kind].column, max);

	pthread_mutex_lock(&db_lock);

	int count = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		while (count < max && sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *text = sqlite3_column_text(stmt, 0);
			snprintf(out[count], 256, "%s", text ? (const char *)text : "");
			count++;
		}
		sqlite3_finalize(stmt);
	}

	pthread_mutex_unlock(&db_lock);
	return count;
}

// ---------------------------------------------------------------------------
// single-row lookups, favourites and row iteration for the Musica pages
// ---------------------------------------------------------------------------

// Whitelisted SQL fragments per kind/filter -- nothing user-controlled is ever
// pasted into the statement text; the filter *value* travels as a bound
// parameter.
static const char *filter_column(library_filter_t filter) {
	switch (filter) {
	case LIBRARY_FILTER_ALBUM:
		return "album";
	case LIBRARY_FILTER_ARTIST:
		return "artist";
	case LIBRARY_FILTER_ALBUM_ARTIST:
		return "album_artist";
	case LIBRARY_FILTER_GENRE:
		return "genre";
	case LIBRARY_FILTER_YEAR:
		return "year";
	default:
		return NULL;
	}
}

// The same filter as a condition on MEDIA_TABLE with its value bound as ?1. An
// album is matched by name and key (see ALBUM_VALUE_MATCH); the rest by the
// column, and an artist or a genre also by the link rows of the tags that split
// into it, when there are any.
static const char *filter_where(library_filter_t filter) {
	switch (filter) {
	case LIBRARY_FILTER_ALBUM:
		return ALBUM_VALUE_MATCH;
	case LIBRARY_FILTER_ARTIST:
		return artist_links ? "(artist=?1 OR path IN (SELECT path FROM ARTIST_LINK WHERE artist=?1))" : "artist=?1";
	case LIBRARY_FILTER_ALBUM_ARTIST:
		return album_artist_links
				   ? "(album_artist=?1 OR path IN (SELECT path FROM ALBUM_ARTIST_LINK WHERE album_artist=?1))"
				   : "album_artist=?1";
	case LIBRARY_FILTER_GENRE:
		return genre_links ? "(genre=?1 OR path IN (SELECT path FROM GENRE_LINK WHERE genre=?1))" : "genre=?1";
	case LIBRARY_FILTER_YEAR:
		// The tracks of the albums of that year, by the album's year and not
		// the track's own, so the tracks are those of the records listed.
		return "(album, album_key) IN (SELECT g.album, g.album_key FROM ALBUM_GROUP_TABLE g WHERE " ALBUM_GROUP_YEAR
			   " = CAST(?1 AS INTEGER))";
	default:
		return NULL;
	}
}

bool library_track_title(const char *path, char *out, size_t out_size) {
	if (!path || !path[0] || !out) {
		return false;
	}

	pthread_mutex_lock(&db_lock);
	bool found = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT name FROM MEDIA_TABLE WHERE path=? LIMIT 1", -1, &stmt, NULL) ==
				  SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *name = sqlite3_column_text(stmt, 0);
			if (name && name[0]) {
				snprintf(out, out_size, "%s", (const char *)name);
				found = true;
			}
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return found;
}

// The lock is let go between batches, so a scan writing rows or the interface
// opening a list is never held behind a folder of eight hundred lookups.
#define TRACK_PLACE_BATCH 64

size_t library_track_places(const char *const *paths, size_t count, library_place_cb cb, void *user) {
	if (!paths || !cb) {
		return 0;
	}

	size_t found = 0;
	for (size_t start = 0; start < count; start += TRACK_PLACE_BATCH) {
		size_t end = start + TRACK_PLACE_BATCH < count ? start + TRACK_PLACE_BATCH : count;

		pthread_mutex_lock(&db_lock);
		sqlite3_stmt *stmt = NULL;
		if (!db || sqlite3_prepare_v2(db,
									  "SELECT album, COALESCE(disc,1), dis_id FROM MEDIA_TABLE WHERE path=? LIMIT 1",
									  -1, &stmt, NULL) != SQLITE_OK) {
			pthread_mutex_unlock(&db_lock);
			break;
		}
		for (size_t i = start; i < end; i++) {
			if (!paths[i] || !paths[i][0]) {
				continue;
			}
			sqlite3_bind_text(stmt, 1, paths[i], -1, SQLITE_STATIC);
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				const unsigned char *album = sqlite3_column_text(stmt, 0);
				cb(i, album ? (const char *)album : "", sqlite3_column_int(stmt, 1), sqlite3_column_int(stmt, 2),
				   user);
				found++;
			}
			sqlite3_reset(stmt);
			sqlite3_clear_bindings(stmt);
		}
		sqlite3_finalize(stmt);
		pthread_mutex_unlock(&db_lock);
	}
	return found;
}

// Title and artist in one go, for a list built from paths rather than from a
// query -- a playlist. The scan already put both in the row, so a lookup here
// costs one indexed SELECT instead of opening the file and parsing its tags.
// Either output may be left empty: a track can be indexed without an artist.
bool library_album_artist(const char *album, char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';
	if (!album || !album[0]) {
		return false;
	}

	// The same first track, and the same choice, as the album row names.
	pthread_mutex_lock(&db_lock);
	bool found = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db,
								 "SELECT COALESCE(NULLIF(album_artist,''), artist) FROM MEDIA_TABLE WHERE " ALBUM_VALUE_MATCH
								 " ORDER BY COALESCE(disc,1), dis_id LIMIT 1",
								 -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, album, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *artist = sqlite3_column_text(stmt, 0);
			if (artist && artist[0]) {
				snprintf(out, size, "%s", (const char *)artist);
				found = true;
			}
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return found;
}

bool library_album_info(const char *album, library_album_info_t *out) {
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (!album || !album[0]) {
		return false;
	}

	// The artist and the genre of the first track, as library_album_artist()
	// picks it; the year as ALBUM_GROUP_YEAR works it out for the Albums list.
	pthread_mutex_lock(&db_lock);
	bool found = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db,
								 "SELECT COALESCE(NULLIF(album_artist,''), artist), genre,"
								 " (SELECT MAX(CASE WHEN year > 0 THEN year END) FROM MEDIA_TABLE WHERE " ALBUM_VALUE_MATCH
								 ") FROM MEDIA_TABLE WHERE " ALBUM_VALUE_MATCH " ORDER BY COALESCE(disc,1), dis_id LIMIT 1",
								 -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, album, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *artist = sqlite3_column_text(stmt, 0);
			const unsigned char *genre = sqlite3_column_text(stmt, 1);
			snprintf(out->artist, sizeof(out->artist), "%s", artist ? (const char *)artist : "");
			snprintf(out->genre, sizeof(out->genre), "%s", genre ? (const char *)genre : "");
			out->year = sqlite3_column_type(stmt, 2) == SQLITE_NULL ? 0 : sqlite3_column_int(stmt, 2);
			found = true;
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return found;
}

int library_album_places(const char *album, uint16_t *disc, uint16_t *track, int max) {
	if (!album || !album[0]) {
		return 0;
	}

	// The same rows and the same order as the album's track list (see
	// list_sql and TRACK_ORDER_IN_ALBUM). Rows the list tells apart only by
	// name share their disc and number, so which of them comes first does not
	// change what is written at any position.
	pthread_mutex_lock(&db_lock);
	int count = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db,
								 "SELECT COALESCE(disc,1), dis_id FROM MEDIA_TABLE WHERE " ALBUM_VALUE_MATCH
								 " ORDER BY " TRACK_ORDER_IN_ALBUM,
								 -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, album, -1, SQLITE_TRANSIENT);
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			if (count < max) {
				int d = sqlite3_column_int(stmt, 0);
				int t = sqlite3_column_int(stmt, 1);
				if (disc) {
					disc[count] = (uint16_t)(d < 1 ? 1 : (d > UINT16_MAX ? UINT16_MAX : d));
				}
				if (track) {
					track[count] = (uint16_t)(t < 0 ? 0 : (t > UINT16_MAX ? UINT16_MAX : t));
				}
			}
			count++;
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return count;
}

bool library_track_names(const char *path, char *title_out, size_t title_size, char *artist_out, size_t artist_size) {
	if (!path || !path[0]) {
		return false;
	}
	if (title_out && title_size) {
		title_out[0] = '\0';
	}
	if (artist_out && artist_size) {
		artist_out[0] = '\0';
	}

	pthread_mutex_lock(&db_lock);
	bool found = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT name, artist FROM MEDIA_TABLE WHERE path=? LIMIT 1", -1, &stmt, NULL) ==
				  SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *name = sqlite3_column_text(stmt, 0);
			const unsigned char *artist = sqlite3_column_text(stmt, 1);
			if (title_out && title_size && name && name[0]) {
				snprintf(title_out, title_size, "%s", (const char *)name);
				found = true;
			}
			if (artist_out && artist_size && artist && artist[0]) {
				snprintf(artist_out, artist_size, "%s", (const char *)artist);
			}
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return found;
}

// Profiling marks for the reads the interface thread waits on (frameprof.h):
// how long it queued for db_lock -- another thread in the database -- and how
// long the read itself took. Free unless a profiled run is going on.
static const char *const PROF_DB_WAIT = "db_lock_wait";
static const char *const PROF_DB_WINDOW = "db_list_window";
static const char *const PROF_DB_QUALITY = "db_track_quality";
static const char *const PROF_DB_OPEN = "db_list_open";

static void db_lock_profiled(void) {
	frameprof_begin(PROF_DB_WAIT);
	pthread_mutex_lock(&db_lock);
	frameprof_end(PROF_DB_WAIT);
}

static library_quality_t quality_of(int format, int rate, int bits);

library_quality_t library_track_quality(const char *path, int *rate_out, int *bits_out) {
	if (rate_out) {
		*rate_out = 0;
	}
	if (bits_out) {
		*bits_out = 0;
	}
	if (!path || !path[0]) {
		return LIBRARY_QUALITY_NONE;
	}

	int format = -1, rate = 0, bits = 0;
	db_lock_profiled();
	frameprof_begin(PROF_DB_QUALITY);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT format, sample_rate, bit FROM MEDIA_TABLE WHERE path=? LIMIT 1", -1,
								 &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			format = sqlite3_column_int(stmt, 0);
			rate = sqlite3_column_int(stmt, 1);
			bits = sqlite3_column_int(stmt, 2);
		}
		sqlite3_finalize(stmt);
	}
	frameprof_end(PROF_DB_QUALITY);
	pthread_mutex_unlock(&db_lock);

	if (rate_out) {
		*rate_out = rate;
	}
	if (bits_out) {
		*bits_out = bits;
	}
	return quality_of(format, rate, bits);
}

// The badge for what the scan wrote down about a file.
static library_quality_t quality_of(int format, int rate, int bits) {
	switch (format) {
	case FORMAT_DSD:
		return LIBRARY_QUALITY_DSD;
	case FORMAT_MP3:
	case FORMAT_OGG:
	case FORMAT_AAC:
	case FORMAT_OPUS:
		return LIBRARY_QUALITY_LOSSY;
	case FORMAT_WAV:
	case FORMAT_FLAC:
	case FORMAT_WAVPACK:
	case FORMAT_ALAC:
	case FORMAT_APE:
		break;
	default:
		return LIBRARY_QUALITY_NONE; // no row, or a code from an older index
	}

	// Lossless, so the numbers decide. WavPack has none -- nothing reads its
	// header -- and neither does a card indexed before the scan started
	// storing them, and both are better off with no badge.
	if (rate <= 0 || bits <= 0) {
		return LIBRARY_QUALITY_NONE;
	}
	return (bits <= 16 && rate <= 44100) ? LIBRARY_QUALITY_CD : LIBRARY_QUALITY_HIFI;
}

// One name and the slot it came from, for the batch below.
typedef struct {
	const char *name;
	int slot;
} name_slot_t;

static int name_slot_cmp(const void *a, const void *b) {
	const name_slot_t *x = a;
	const name_slot_t *y = b;
	int order = strcasecmp(x->name, y->name);
	return order ? order : (x->slot - y->slot);
}

// Finds the tracks whose file names are in `names`, in one pass.
//
// The index stores whole paths and nothing else, so asking for a file name
// means `LIKE '%/name'` -- a leading wildcard, which no index can serve, so one
// name costs a scan of the table. A playlist of four hundred entries would cost
// four hundred scans. This turns it around: the names are sorted once and every
// row of the table is looked for among them, so the batch costs one scan
// whatever its size.
//
// Matching is case-insensitive by byte, which is how the card itself compares
// file names.
int library_match_basenames(const char *const *names, int count, library_basename_cb cb, void *user) {
	if (!names || count <= 0 || !cb) {
		return 0;
	}

	name_slot_t *sorted = calloc((size_t)count, sizeof(*sorted));
	if (!sorted) {
		return 0;
	}
	int wanted = 0;
	for (int i = 0; i < count; i++) {
		if (names[i] && names[i][0]) {
			sorted[wanted].name = names[i];
			sorted[wanted].slot = i;
			wanted++;
		}
	}
	if (wanted == 0) {
		free(sorted);
		return 0;
	}
	qsort(sorted, (size_t)wanted, sizeof(*sorted), name_slot_cmp);

	int matched = 0;
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT path FROM MEDIA_TABLE", -1, &stmt, NULL) == SQLITE_OK) {
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *path = (const char *)sqlite3_column_text(stmt, 0);
			if (!path || !path[0]) {
				continue;
			}
			const char *slash = strrchr(path, '/');
			const char *base = slash ? slash + 1 : path;

			int low = 0, high = wanted;
			while (low < high) {
				int mid = low + (high - low) / 2;
				if (strcasecmp(sorted[mid].name, base) < 0) {
					low = mid + 1;
				} else {
					high = mid;
				}
			}
			for (int i = low; i < wanted && strcasecmp(sorted[i].name, base) == 0; i++) {
				cb(sorted[i].slot, path, user);
				matched++;
			}
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);

	free(sorted);
	return matched;
}

// The album a track belongs to: its name alone (`with_key` false), or the value
// that names that one record (true). Both read the row the scan wrote.
static bool track_album(const char *path, bool with_key, char *out, size_t out_size) {
	if (!path || !path[0] || !out) {
		return false;
	}

	pthread_mutex_lock(&db_lock);
	bool found = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT album, album_key FROM MEDIA_TABLE WHERE path=? LIMIT 1",
								 -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *album = sqlite3_column_text(stmt, 0);
			const unsigned char *key = sqlite3_column_text(stmt, 1);
			if (album && album[0]) {
				if (with_key && key) {
					snprintf(out, out_size, "%s%c%s", (const char *)album, LIBRARY_ALBUM_KEY_SEP, (const char *)key);
				} else {
					snprintf(out, out_size, "%s", (const char *)album);
				}
				found = true;
			}
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return found;
}

bool library_track_album(const char *path, char *out, size_t out_size) { return track_album(path, false, out, out_size); }

bool library_track_album_value(const char *path, char *out, size_t out_size) {
	return track_album(path, true, out, out_size);
}

bool library_track_artist(const char *path, char *out, size_t out_size) {
	if (!path || !path[0] || !out) {
		return false;
	}

	pthread_mutex_lock(&db_lock);
	bool found = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT artist FROM MEDIA_TABLE WHERE path=? LIMIT 1", -1, &stmt, NULL) ==
				  SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *artist = sqlite3_column_text(stmt, 0);
			if (artist && artist[0]) {
				snprintf(out, out_size, "%s", (const char *)artist);
				found = true;
			}
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return found;
}

void library_album_title(const char *value, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	const char *text = value ? value : "";
	const char *sep = strchr(text, LIBRARY_ALBUM_KEY_SEP);
	size_t length = sep ? (size_t)(sep - text) : strlen(text);
	if (length >= size) {
		length = size - 1;
	}
	memcpy(out, text, length);
	out[length] = '\0';
}

bool library_album_same(const char *a, const char *b) {
	if (!a || !b) {
		return false;
	}
	if (strcmp(a, b) == 0) {
		return true;
	}
	// One of the two without a key: the names alone decide.
	if (strchr(a, LIBRARY_ALBUM_KEY_SEP) && strchr(b, LIBRARY_ALBUM_KEY_SEP)) {
		return false;
	}
	size_t la = strcspn(a, "\x1f");
	size_t lb = strcspn(b, "\x1f");
	return la == lb && strncasecmp(a, b, la) == 0;
}

unsigned library_favourites_generation(void) { return __atomic_load_n(&generation[GEN_FAVOURITES], __ATOMIC_RELAXED); }

bool library_fav_contains(const char *path) {
	if (!path || !path[0]) {
		return false;
	}

	pthread_mutex_lock(&db_lock);
	bool found = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT 1 FROM FAVOURITES WHERE path=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		found = sqlite3_step(stmt) == SQLITE_ROW;
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return found;
}

bool library_fav_toggle(const char *path, const char *name, const char *artist) {
	if (!path || !path[0]) {
		return false;
	}

	bool now_favourite = !library_fav_contains(path);

	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db) {
		if (now_favourite) {
			if (sqlite3_prepare_v2(db,
								   "INSERT OR REPLACE INTO FAVOURITES(path,name,artist,added_at)"
								   " VALUES(?,?,?,strftime('%s','now'))",
								   -1, &stmt, NULL) == SQLITE_OK) {
				sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
				sqlite3_bind_text(stmt, 2, name ? name : "", -1, SQLITE_TRANSIENT);
				sqlite3_bind_text(stmt, 3, artist ? artist : "", -1, SQLITE_TRANSIENT);
				sqlite3_step(stmt);
				sqlite3_finalize(stmt);
			}
		} else {
			if (sqlite3_prepare_v2(db, "DELETE FROM FAVOURITES WHERE path=?", -1, &stmt, NULL) == SQLITE_OK) {
				sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
				sqlite3_step(stmt);
				sqlite3_finalize(stmt);
			}
		}
		// Starring rewrites the row, which on this table means a new row id --
		// and only on this table, so a list of tracks is untouched.
		bump_generation(GEN_FAVOURITES);
	}
	pthread_mutex_unlock(&db_lock);
	return now_favourite;
}

void library_note_play(const char *path) {
	if (!path || !path[0]) {
		return;
	}
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	// An upsert and not INSERT OR REPLACE: the row keeps its row id, so an open
	// list of the most played -- and a queue built on one -- is not left
	// holding ids that no longer exist. Neither is told: the list takes the
	// new order the next time it is opened, and a queue playing it goes on in
	// the order it started with.
	//
	// Only a track the index has: a station, a stream's cache file or a file
	// played from the browser that was never scanned would only fill the table
	// with rows the list never shows.
	if (db && sqlite3_prepare_v2(db,
								 "INSERT INTO PLAY_COUNTS(path, plays, last_played)"
								 " SELECT ?1, 1, strftime('%s','now') WHERE EXISTS"
								 " (SELECT 1 FROM MEDIA_TABLE m WHERE m.path = ?1)"
								 " ON CONFLICT(path) DO UPDATE SET plays = plays + 1, last_played = excluded.last_played",
								 -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) != SQLITE_DONE) {
			fprintf(stderr, "library: play of '%s' not counted: %s\n", path, sqlite3_errmsg(db));
		} else if (sqlite3_changes(db) > 0) {
			printf("library: play of '%s' counted\n", path);
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
}

void library_history_add(const char *path, const char *name, const char *artist, long long played_at) {
	if (!path || !path[0]) {
		return;
	}
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "INSERT INTO PLAY_HISTORY(path, name, artist, played_at) VALUES(?1, ?2, ?3, ?4)", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 2, name ? name : "", -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, artist ? artist : "", -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 4, played_at);
		if (sqlite3_step(stmt) != SQLITE_DONE) {
			fprintf(stderr, "library: '%s' not added to the history: %s\n", path, sqlite3_errmsg(db));
		} else {
			history_newest = (long long)sqlite3_last_insert_rowid(db);
		}
		sqlite3_finalize(stmt);
		// Only the latest are kept: the ids only grow, so everything this far
		// below the newest is older than what is kept.
		char sql[128];
		snprintf(sql, sizeof(sql), "DELETE FROM PLAY_HISTORY WHERE id <= (SELECT MAX(id) FROM PLAY_HISTORY) - %d", LIBRARY_HISTORY_MAX);
		exec(sql);
	}
	pthread_mutex_unlock(&db_lock);
}

int library_history_items(library_history_item_t **out) {
	if (!out) {
		return 0;
	}
	*out = NULL;
	pthread_mutex_lock(&db_lock);
	int count = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT id, played_at FROM PLAY_HISTORY ORDER BY id", -1, &stmt, NULL) == SQLITE_OK) {
		int room = 0;
		library_history_item_t *items = NULL;
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			if (count == room) {
				int grown_room = room ? room * 2 : 64;
				library_history_item_t *grown = realloc(items, (size_t)grown_room * sizeof(*grown));
				if (!grown) {
					break;
				}
				items = grown;
				room = grown_room;
			}
			items[count].id = sqlite3_column_int64(stmt, 0);
			items[count].played_at = sqlite3_column_int64(stmt, 1);
			count++;
		}
		sqlite3_finalize(stmt);
		*out = items;
	}
	pthread_mutex_unlock(&db_lock);
	return count;
}

long long library_history_revision(void) {
	pthread_mutex_lock(&db_lock);
	if (history_newest < 0 && db) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(id), 0) FROM PLAY_HISTORY", -1, &stmt, NULL) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
			history_newest = sqlite3_column_int64(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	long long newest = history_newest < 0 ? 0 : history_newest;
	pthread_mutex_unlock(&db_lock);
	return newest;
}

bool library_history_entry(long long id, char *path, size_t path_size, char *name, size_t name_size) {
	pthread_mutex_lock(&db_lock);
	bool found = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT path, name FROM PLAY_HISTORY WHERE id = ?1", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, id);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *p = sqlite3_column_text(stmt, 0);
			const unsigned char *n = sqlite3_column_text(stmt, 1);
			if (path && path_size > 0) {
				snprintf(path, path_size, "%s", p ? (const char *)p : "");
			}
			if (name && name_size > 0) {
				snprintf(name, name_size, "%s", n ? (const char *)n : "");
			}
			found = p && p[0];
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return found;
}

int library_fav_add_many(const library_fav_row_t *rows, int count) {
	if (!rows || count <= 0) {
		return 0;
	}

	pthread_mutex_lock(&db_lock);
	int starred = 0;
	if (db) {
		exec("BEGIN");
		sqlite3_stmt *stmt = NULL;
		bool ok = sqlite3_prepare_v2(db,
									 "INSERT OR IGNORE INTO FAVOURITES(path,name,artist,added_at)"
									 " VALUES(?,?,?,strftime('%s','now'))",
									 -1, &stmt, NULL) == SQLITE_OK;
		for (int i = 0; i < count && ok; i++) {
			if (!rows[i].path || !rows[i].path[0]) {
				continue;
			}
			sqlite3_bind_text(stmt, 1, rows[i].path, -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(stmt, 2, rows[i].name ? rows[i].name : "", -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(stmt, 3, rows[i].artist ? rows[i].artist : "", -1, SQLITE_TRANSIENT);
			if (sqlite3_step(stmt) == SQLITE_DONE) {
				starred++;
			}
			sqlite3_reset(stmt);
		}
		sqlite3_finalize(stmt);
		exec(ok ? "COMMIT" : "ROLLBACK");
		if (!ok) {
			starred = 0;
		}
		bump_generation(GEN_FAVOURITES);
	}
	pthread_mutex_unlock(&db_lock);
	return starred;
}

int library_fav_remove_many(const char *const *paths, int count) {
	if (!paths || count <= 0) {
		return 0;
	}

	pthread_mutex_lock(&db_lock);
	int removed = 0;
	if (db) {
		exec("BEGIN");
		sqlite3_stmt *stmt = NULL;
		bool ok = sqlite3_prepare_v2(db, "DELETE FROM FAVOURITES WHERE path=?", -1, &stmt, NULL) == SQLITE_OK;
		for (int i = 0; i < count && ok; i++) {
			if (!paths[i] || !paths[i][0]) {
				continue;
			}
			sqlite3_bind_text(stmt, 1, paths[i], -1, SQLITE_TRANSIENT);
			ok = sqlite3_step(stmt) == SQLITE_DONE;
			removed += ok && sqlite3_changes(db) > 0 ? 1 : 0;
			sqlite3_reset(stmt);
		}
		sqlite3_finalize(stmt);
		exec(ok ? "COMMIT" : "ROLLBACK");
		if (!ok) {
			removed = 0;
		}
		bump_generation(GEN_FAVOURITES);
	}
	pthread_mutex_unlock(&db_lock);
	return removed;
}

int library_forget_under(const char *prefix) {
	if (!prefix || !prefix[0]) {
		return 0;
	}

	char pattern[600];
	snprintf(pattern, sizeof(pattern), "%.500s%%", prefix);

	pthread_mutex_lock(&db_lock);
	int removed = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "DELETE FROM FAVOURITES WHERE path LIKE ?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_DONE) {
			removed = sqlite3_changes(db);
		}
		sqlite3_finalize(stmt);
	}
	// And the remembered playback position, if it pointed at a cache file.
	if (db && sqlite3_prepare_v2(db, "DELETE FROM PLAYBACK_STATE WHERE path LIKE ?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	bump_generation(GEN_FAVOURITES);
	pthread_mutex_unlock(&db_lock);

	if (removed > 0) {
		printf("library: removed %d favourites that pointed at the Qobuz cache\n", removed);
	}
	return removed;
}

int library_for_each(library_list_t kind, library_filter_t filter, const char *value, library_row_cb cb, void *user) {
	return library_for_each_ordered(kind, filter, value, LIBRARY_ORDER_DEFAULT, cb, user);
}

// The ORDER BY of a track list. Inside one album the disc order is the natural
// one; everywhere else the titles read best alphabetically. The orders asked
// for are the other cases: an artist's tracks with each record kept together
// and in its own running order, every track by when it arrived, and every
// track by year -- each year's records kept together and in their running
// order, so playing the list plays the records through.
// The albums in name order, one record after another: the sort key worked out
// once per row, then compared as bytes -- the same order as the collation,
// which would instead be called for every comparison of the sort -- and the
// album key, so two records of one name do not mix their tracks.
#define ALBUM_ORDER_KEY "sortkey(album), album_key"

static void track_order_sql(char *out, size_t size, library_order_t order, bool in_album, const char *by_name) {
	if (order == LIBRARY_ORDER_ALBUM) {
		snprintf(out, size, ALBUM_ORDER_KEY ", " TRACK_ORDER_IN_ALBUM ", %s", by_name);
	} else if (order == LIBRARY_ORDER_ADDED) {
		snprintf(out, size, TRACK_ORDER_ADDED ", %s", by_name);
	} else if (order == LIBRARY_ORDER_YEAR || order == LIBRARY_ORDER_YEAR_DESC) {
		snprintf(out, size, "%s, " ALBUM_ORDER_KEY ", " TRACK_ORDER_IN_ALBUM ", %s",
				 order == LIBRARY_ORDER_YEAR ? YEAR_KEY_UP(TRACK_YEAR) : YEAR_KEY_DOWN(TRACK_YEAR), by_name);
	} else if (in_album) {
		snprintf(out, size, TRACK_ORDER_IN_ALBUM ", %s", by_name);
	} else {
		snprintf(out, size, "%s", by_name);
	}
}

int library_for_each_ordered(library_list_t kind, library_filter_t filter, const char *value, library_order_t order,
							 library_row_cb cb, void *user) {
	if (!cb || filter == LIBRARY_FILTER_SEARCH) {
		return 0; // a search is read through a handle
	}

	char sql[512];
	const char *col = filter_column(filter);
	// Same choice the handles make, for the same reason (see list_sql): the two
	// expressions order identically, one of them with an index behind it.
	const char *by_name = list_uses_sortkey(kind) ? "sortkey" : "name COLLATE listorder";

	if (kind == LIBRARY_LIST_FAVOURITES) {
		snprintf(sql, sizeof(sql), "SELECT name, path, %s FROM FAVOURITES ORDER BY added_at, rowid", FAV_ARTIST);
	} else if (kind == LIBRARY_LIST_MOST_PLAYED) {
		snprintf(sql, sizeof(sql), "SELECT " PLAYS_NAME ", path, " PLAYS_ARTIST " FROM PLAY_COUNTS WHERE " PLAYS_IN_INDEX
								   " ORDER BY " PLAYS_ORDER);
	} else if (kind == LIBRARY_LIST_TRACKS) {
		char order_sql[256];
		track_order_sql(order_sql, sizeof(order_sql), order, col && value && filter == LIBRARY_FILTER_ALBUM, by_name);
		if (col && value) {
			snprintf(sql, sizeof(sql), "SELECT name, path, artist FROM MEDIA_TABLE WHERE %s ORDER BY %s",
					 filter_where(filter), order_sql);
		} else {
			snprintf(sql, sizeof(sql), "SELECT name, path, artist FROM MEDIA_TABLE ORDER BY %s", order_sql);
		}
	} else {
		static const struct {
			const char *table;
			const char *column;
		} SOURCES[] = {
			{"MEDIA_TABLE", "name"},
			{"ALBUM_TABLE", "album"},
			{"ARTIST_TABLE", "artist"},
			{"ALBUM_ARTIST_TABLE", "album_artist"},
			{"GENRE_TABLE", "genre"},
		};
		if (kind < 0 || kind >= (int)(sizeof(SOURCES) / sizeof(SOURCES[0]))) {
			return 0;
		}
		if (kind == LIBRARY_LIST_ALBUMS) {
			// An album row wants its cover, and covers hang off files: hand the
			// caller one representative track per album to load the art from.
			snprintf(sql, sizeof(sql),
					 "SELECT " ALBUM_GROUP_VALUE ", " ALBUM_GROUP_FIRST("m.path") ", NULL FROM ALBUM_GROUP_TABLE g"
					 " WHERE album <> '' ORDER BY %s",
					 list_uses_sortkey(kind) ? "sortkey" : "album COLLATE listorder");
		} else if (list_uses_sortkey(kind)) {
			snprintf(sql, sizeof(sql), "SELECT %s, NULL, NULL FROM %s WHERE %s <> '' ORDER BY sortkey",
					 SOURCES[kind].column, SOURCES[kind].table, SOURCES[kind].column);
		} else {
			snprintf(sql, sizeof(sql), "SELECT %s, NULL, NULL FROM %s WHERE %s <> '' ORDER BY %s COLLATE listorder",
					 SOURCES[kind].column, SOURCES[kind].table, SOURCES[kind].column, SOURCES[kind].column);
		}
	}

	pthread_mutex_lock(&db_lock);

	int count = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		if (col && value && kind == LIBRARY_LIST_TRACKS) {
			sqlite3_bind_text(stmt, 1, value, -1, SQLITE_TRANSIENT);
		}
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *name = (const char *)sqlite3_column_text(stmt, 0);
			const char *path = (const char *)sqlite3_column_text(stmt, 1);
			const char *artist = (const char *)sqlite3_column_text(stmt, 2);
			if (!cb(name ? name : "", path, artist, user)) {
				break; // the caller has all it can hold
			}
			count++;
		}
		sqlite3_finalize(stmt);
	}

	pthread_mutex_unlock(&db_lock);
	return count;
}

// ---------------------------------------------------------------------------
// List handles
//
// Reading a list into RAM in full costs a name and a path strdup'd per row,
// about a hundred bytes each: two hundred thousand tracks is nineteen
// megabytes, on a device with ten free, to draw the twelve rows that fit on the
// screen.
//
// A handle holds the row ids instead -- four bytes a row, eight hundred
// kilobytes for the same library -- and the rows themselves are read back a
// windowful at a time, by row id, which is a b-tree descent and costs nothing.
//
// Row ids rather than LIMIT/OFFSET paging because they are what lets a row be
// reached by position (the A-Z strip lands on one) and what the queue is
// written down as.
// ---------------------------------------------------------------------------

struct library_index {
	// What it was built from, kept so a queue made out of a handle can be
	// written down as the question instead of the two hundred thousand rows of
	// its answer.
	library_list_t kind;
	library_filter_t filter;
	library_order_t order;
	char value[256];

	bool desc; // the list reads backwards; the rows are always stored ascending
	gen_domain_t domain;
	unsigned generation;

	int32_t *rows;
	int count;

	int buckets[LIBRARY_INDEX_BUCKETS];
	bool buckets_valid;
};

// ---------------------------------------------------------------------------
// A playlist is a table
//
// See library.h for what the shape is and why. What follows is the mechanics of
// naming one: a table name cannot be bound like a value, so it is written into
// the statement, and the only safe way to do that with a name the user typed is
// SQLite's own quoting -- double quotes around it, and any double quote inside
// it doubled. Nothing else is trusted about the name; it goes in exactly as
// typed so that the table is called what the playlist is called.
// ---------------------------------------------------------------------------

// "M3U_<name>", quoted. False when the name is too long to be one, which is the
// only thing that can go wrong here.
static bool playlist_table(const char *name, char *out, size_t out_size) {
	if (!name || !name[0] || !out || out_size < 8) {
		return false;
	}
	size_t at = 0;
	out[at++] = '"';
	const char *prefix = LIBRARY_PLAYLIST_PREFIX;
	while (*prefix && at + 2 < out_size) {
		out[at++] = *prefix++;
	}
	for (const char *c = name; *c; c++) {
		if (at + 3 >= out_size) {
			return false;
		}
		if (*c == '"') {
			out[at++] = '"'; // doubled, which is how a quote lives inside one
		}
		out[at++] = *c;
	}
	if (at + 2 > out_size) {
		return false;
	}
	out[at++] = '"';
	out[at] = '\0';
	return true;
}

// Which of a playlist's entries are shown, for a query that names the
// playlist's table `p`: the tracks the library has, and the entries it does not
// have that were on the card when they went in (`present`). The scans keep
// `present` for the rest (playlists_follow_library).
#define PLAYLIST_SHOWN "(p.present<>0 OR EXISTS(SELECT 1 FROM MEDIA_TABLE m WHERE m.path = p.path))"

// The title and artist a playlist row shows: the library's, which follow the
// tags as they are now, and the ones written down with the entry for a track
// the library does not have.
#define PLAYLIST_TITLE "COALESCE(NULLIF((SELECT m.name FROM MEDIA_TABLE m WHERE m.path = p.path LIMIT 1), ''), p.title)"
#define PLAYLIST_ARTIST                                                                                        \
	"COALESCE(NULLIF((SELECT m.artist FROM MEDIA_TABLE m WHERE m.path = p.path LIMIT 1), ''), p.artist)"

// The ordered query behind a list, as SQL. `select` is what to ask for, so the
// same builder serves both the row-id pass and the streaming reader.
// The LIKE pattern a query becomes: folded the way foldcase() folds the rows,
// wrapped in wildcards, and with LIKE's own two wildcards escaped -- a query is
// a piece of a name, so a user typing "_" means an underscore and not "any
// character". False for a query that folds to nothing.
static bool search_pattern(const char *query, char *out, size_t size) {
	char folded[256];
	if (!query || size < 3 || fold_text(query, folded, sizeof(folded)) == 0) {
		return false;
	}
	size_t at = 0;
	out[at++] = '%';
	for (size_t i = 0; folded[i] && at + 3 < size; i++) {
		if (folded[i] == '%' || folded[i] == '_' || folded[i] == '\\') {
			out[at++] = '\\';
		}
		out[at++] = folded[i];
	}
	out[at++] = '%';
	out[at] = '\0';
	return true;
}

static void list_sql(char *sql, size_t size, const char *select, library_list_t kind, library_filter_t filter,
					 const char *value, library_order_t order) {
	const char *col = filter_column(filter);

	// A search: the names that contain the query, in list order, with the
	// pattern bound as ?1. Tracks, albums and artists only.
	if (filter == LIBRARY_FILTER_SEARCH) {
		static const struct {
			library_list_t kind;
			const char *table;
			const char *column;
		} SEARCHED[] = {
			{LIBRARY_LIST_TRACKS, "MEDIA_TABLE", "name"},
			{LIBRARY_LIST_ALBUMS, "ALBUM_GROUP_TABLE", "album"},
			{LIBRARY_LIST_ARTISTS, "ARTIST_TABLE", "artist"},
		};
		sql[0] = '\0';
		for (size_t i = 0; i < sizeof(SEARCHED) / sizeof(SEARCHED[0]); i++) {
			if (SEARCHED[i].kind != kind) {
				continue;
			}
			if (list_uses_sortkey(kind)) {
				snprintf(sql, size, "SELECT %s FROM %s WHERE %s <> '' AND foldcase(%s) LIKE ?1 ESCAPE '\\' ORDER BY sortkey",
						 select, SEARCHED[i].table, SEARCHED[i].column, SEARCHED[i].column);
			} else {
				snprintf(sql, size,
						 "SELECT %s FROM %s WHERE %s <> '' AND foldcase(%s) LIKE ?1 ESCAPE '\\'"
						 " ORDER BY %s COLLATE listorder",
						 select, SEARCHED[i].table, SEARCHED[i].column, SEARCHED[i].column, SEARCHED[i].column);
			}
		}
		return;
	}

	if (kind == LIBRARY_LIST_FAVOURITES) {
		snprintf(sql, size, "SELECT %s FROM FAVOURITES ORDER BY added_at, rowid", select);
		return;
	}

	if (kind == LIBRARY_LIST_MOST_PLAYED) {
		snprintf(sql, size, "SELECT %s FROM PLAY_COUNTS WHERE " PLAYS_IN_INDEX " ORDER BY " PLAYS_ORDER, select);
		return;
	}

	// A playlist is a table of its own, so the name of the table is the value
	// the caller passed. It runs in the order it was built -- `idx` -- and
	// shows what PLAYLIST_SHOWN lets through.
	if (kind == LIBRARY_LIST_PLAYLIST) {
		char quoted[PLAYLIST_TABLE_MAX];
		if (!playlist_table(value, quoted, sizeof(quoted))) {
			sql[0] = '\0';
			return;
		}
		// A truncated statement is not a slower statement, it is a different
		// one, so it is refused rather than run.
		if (snprintf(sql, size, "SELECT %s FROM %s p WHERE " PLAYLIST_SHOWN " ORDER BY p.idx", select, quoted) >=
			(int)size) {
			sql[0] = '\0';
		}
		return;
	}

	// The sort key where every row has one, the collation where they do not.
	// The two give the same order (see sort_key) -- one of them off an index,
	// the other by sorting the whole table into a temporary b-tree.
	const char *by_name = list_uses_sortkey(kind) ? "sortkey" : "name COLLATE listorder";

	if (kind == LIBRARY_LIST_TRACKS) {
		char order_sql[256];
		track_order_sql(order_sql, sizeof(order_sql), order, col && value && filter == LIBRARY_FILTER_ALBUM, by_name);
		if (col && value) {
			snprintf(sql, size, "SELECT %s FROM MEDIA_TABLE WHERE %s ORDER BY %s", select, filter_where(filter),
					 order_sql);
		} else {
			snprintf(sql, size, "SELECT %s FROM MEDIA_TABLE ORDER BY %s", select, order_sql);
		}
		return;
	}

	// The years, latest first: each year once, named by the row id of one of
	// its albums, which is how row_by_id_sql() finds the year again. Counted
	// as the distinct years, since there is no table of them to count.
	if (kind == LIBRARY_LIST_YEARS) {
		const char *years =
			"SELECT g.rowid AS rid, " ALBUM_GROUP_YEAR " AS y FROM ALBUM_GROUP_TABLE g WHERE g.album <> ''";
		if (strncmp(select, "COUNT(", 6) == 0) {
			snprintf(sql, size, "SELECT COUNT(DISTINCT y) FROM (%s) WHERE y IS NOT NULL", years);
		} else {
			snprintf(sql, size, "SELECT MIN(rid), y FROM (%s) WHERE y IS NOT NULL GROUP BY y ORDER BY y DESC", years);
		}
		return;
	}

	// One year's records, by name.
	if (kind == LIBRARY_LIST_ALBUMS && filter == LIBRARY_FILTER_YEAR && value) {
		snprintf(sql, size,
				 "SELECT %s FROM ALBUM_GROUP_TABLE g WHERE album <> '' AND " ALBUM_GROUP_YEAR
				 " = CAST(?1 AS INTEGER) ORDER BY %s",
				 select, list_uses_sortkey(kind) ? "sortkey" : "album COLLATE listorder");
		return;
	}

	// One artist's or one genre's records, rather than their tracks: the album
	// list narrowed to the albums with at least one track of theirs.
	// ALBUM_GROUP_TABLE holds the albums and their sort keys, and which of them
	// qualify is a question for MEDIA_TABLE -- answered off media_artist_ctime_idx,
	// media_album_artist_ctime_idx or media_genre_sort_idx, once, so the subquery is
	// a lookup and not a scan.
	if (kind == LIBRARY_LIST_ALBUMS && col && value &&
		(filter == LIBRARY_FILTER_ARTIST || filter == LIBRARY_FILTER_ALBUM_ARTIST || filter == LIBRARY_FILTER_GENRE)) {
		snprintf(sql, size,
				 "SELECT %s FROM ALBUM_GROUP_TABLE WHERE album <> ''"
				 " AND (album, album_key) IN (SELECT album, album_key FROM MEDIA_TABLE WHERE %s)"
				 " ORDER BY %s",
				 select, filter_where(filter), list_uses_sortkey(kind) ? "sortkey" : "album COLLATE listorder");
		return;
	}

	// Every record, artist and album artist by when it arrived. Their tables
	// have no date of their own, so each is as new as the newest of its files:
	// a record topped up with a bonus track moves up with it, and so does an
	// artist with a new album. One range per row of media_album_key_idx,
	// media_artist_ctime_idx or media_album_artist_ctime_idx, read from the
	// index alone.
	if (order == LIBRARY_ORDER_ADDED && kind == LIBRARY_LIST_ALBUMS) {
		snprintf(sql, size, "SELECT %s FROM ALBUM_GROUP_TABLE g WHERE album <> '' ORDER BY " ALBUM_GROUP_NEWEST
				 ", %s",
				 select, list_uses_sortkey(kind) ? "sortkey" : "album COLLATE listorder");
		return;
	}
	// Every record by year, the albums of one year by name.
	if ((order == LIBRARY_ORDER_YEAR || order == LIBRARY_ORDER_YEAR_DESC) && kind == LIBRARY_LIST_ALBUMS) {
		snprintf(sql, size, "SELECT %s FROM ALBUM_GROUP_TABLE g WHERE album <> '' ORDER BY %s, %s", select,
				 order == LIBRARY_ORDER_YEAR ? YEAR_KEY_UP(ALBUM_GROUP_YEAR) : YEAR_KEY_DOWN(ALBUM_GROUP_YEAR),
				 list_uses_sortkey(kind) ? "sortkey" : "album COLLATE listorder");
		return;
	}
	if (order == LIBRARY_ORDER_ADDED && (kind == LIBRARY_LIST_ARTISTS || kind == LIBRARY_LIST_ALBUM_ARTISTS)) {
		const char *table = kind == LIBRARY_LIST_ARTISTS ? "ARTIST_TABLE" : "ALBUM_ARTIST_TABLE";
		const char *column = kind == LIBRARY_LIST_ARTISTS ? "artist" : "album_artist";
		char by_name[64];
		snprintf(by_name, sizeof(by_name), "%s%s", list_uses_sortkey(kind) ? "sortkey" : column,
				 list_uses_sortkey(kind) ? "" : " COLLATE listorder");
		// An artist that is one of the names a tag split into is also as new as
		// the newest of those tracks.
		const char *linked =
			kind == LIBRARY_LIST_ARTISTS && artist_links
				? " OR m.path IN (SELECT l.path FROM ARTIST_LINK l WHERE l.artist = ARTIST_TABLE.artist)"
			: kind == LIBRARY_LIST_ALBUM_ARTISTS && album_artist_links
				? " OR m.path IN (SELECT l.path FROM ALBUM_ARTIST_LINK l"
				  " WHERE l.album_artist = ALBUM_ARTIST_TABLE.album_artist)"
				: "";
		snprintf(sql, size,
				 "SELECT %s FROM %s WHERE %s <> ''"
				 " ORDER BY (SELECT MAX(m.ctime) FROM MEDIA_TABLE m WHERE m.%s = %s.%s%s), %s",
				 select, table, column, column, table, column, linked, by_name);
		return;
	}

	static const struct {
		const char *table;
		const char *column;
	} SOURCES[] = {
		{"MEDIA_TABLE", "name"},
		{"ALBUM_GROUP_TABLE", "album"},
		{"ARTIST_TABLE", "artist"},
		{"ALBUM_ARTIST_TABLE", "album_artist"},
		{"GENRE_TABLE", "genre"},
	};
	if (kind < 0 || kind >= (int)(sizeof(SOURCES) / sizeof(SOURCES[0]))) {
		sql[0] = '\0';
		return;
	}
	if (list_uses_sortkey(kind)) {
		snprintf(sql, size, "SELECT %s FROM %s WHERE %s <> '' ORDER BY sortkey", select, SOURCES[kind].table,
				 SOURCES[kind].column);
	} else {
		snprintf(sql, size, "SELECT %s FROM %s WHERE %s <> '' ORDER BY %s COLLATE listorder", select,
				 SOURCES[kind].table, SOURCES[kind].column, SOURCES[kind].column);
	}
}

// One row of a list, by row id. The row id already says which row, so none of
// the filtering the ordered query does is needed here.
//
// `out` is only written for the kinds whose table is named by the list itself,
// which today is the playlists; everything else answers with a literal.
static const char *row_by_id_sql(library_list_t kind, const char *value, char *out, size_t out_size) {
	if (kind == LIBRARY_LIST_PLAYLIST) {
		char quoted[PLAYLIST_TABLE_MAX];
		if (!playlist_table(value, quoted, sizeof(quoted))) {
			return NULL;
		}
		if (snprintf(out, out_size, "SELECT " PLAYLIST_TITLE ", p.path, " PLAYLIST_ARTIST " FROM %s p WHERE p.rowid=?",
					 quoted) >= (int)out_size) {
			return NULL;
		}
		return out;
	}
	switch (kind) {
	case LIBRARY_LIST_TRACKS:
		return "SELECT name, path, artist FROM MEDIA_TABLE WHERE rowid=?";
	case LIBRARY_LIST_ALBUMS:
		// An album row wants its cover, and covers hang off files: hand the
		// caller one representative track per album to load the art from. This
		// is why it is not in the ordered pass -- there it would run the
		// subquery for every album on the card, to be thrown away.
		//
		// The same first track names the album's artist: its album artist, or
		// its artist on a file that has none. Lists that show it read it from
		// here; a window of rows is a dozen lookups on the album index.
		return "SELECT " ALBUM_GROUP_VALUE ", " ALBUM_GROUP_FIRST("m.path") ","
			   " " ALBUM_GROUP_FIRST("COALESCE(NULLIF(m.album_artist,''), m.artist)")
			   " FROM ALBUM_GROUP_TABLE g WHERE rowid=?";
	case LIBRARY_LIST_ARTISTS:
		return "SELECT artist, NULL, NULL FROM ARTIST_TABLE WHERE rowid=?";
	case LIBRARY_LIST_ALBUM_ARTISTS:
		return "SELECT album_artist, NULL, NULL FROM ALBUM_ARTIST_TABLE WHERE rowid=?";
	case LIBRARY_LIST_GENRES:
		return "SELECT genre, NULL, NULL FROM GENRE_TABLE WHERE rowid=?";
	case LIBRARY_LIST_YEARS:
		// The row is one of the year's albums, and the name is its year.
		return "SELECT " ALBUM_GROUP_YEAR ", NULL, NULL FROM ALBUM_GROUP_TABLE g WHERE rowid=?";
	case LIBRARY_LIST_FAVOURITES:
		return "SELECT " FAV_NAME ", path, " FAV_ARTIST " FROM FAVOURITES WHERE rowid=?";
	case LIBRARY_LIST_MOST_PLAYED:
		return "SELECT " PLAYS_NAME ", path, " PLAYS_ARTIST " FROM PLAY_COUNTS WHERE rowid=?";
	default:
		return NULL;
	}
}

// The column a list is named by, for the A-Z buckets.
static const char *name_column(library_list_t kind) {
	switch (kind) {
	case LIBRARY_LIST_ALBUMS:
		return "album";
	case LIBRARY_LIST_PLAYLIST:
		return "title";
	case LIBRARY_LIST_ARTISTS:
		return "artist";
	case LIBRARY_LIST_ALBUM_ARTISTS:
		return "album_artist";
	case LIBRARY_LIST_GENRES:
		return "genre";
	case LIBRARY_LIST_MOST_PLAYED:
		// No name of its own: the table holds the path, and the list is not
		// filed by letter anyway.
		return "path";
	default:
		return "name";
	}
}

library_index_t *library_index_open(library_list_t kind, library_filter_t filter, const char *value,
									library_order_t order, bool desc) {
	struct library_index *ix = calloc(1, sizeof(*ix));
	if (!ix) {
		return NULL;
	}
	ix->kind = kind;
	ix->filter = filter;
	ix->order = order;
	ix->desc = desc;
	snprintf(ix->value, sizeof(ix->value), "%s", value ? value : "");

	const char *col = filter_column(filter);
	// The filtered lists take a bound value: a track list narrowed to one
	// album/artist/genre, and an album list narrowed to one artist or genre.
	bool bound = col && value &&
				 (kind == LIBRARY_LIST_TRACKS ||
				  (kind == LIBRARY_LIST_ALBUMS &&
				   (filter == LIBRARY_FILTER_ARTIST || filter == LIBRARY_FILTER_ALBUM_ARTIST ||
					filter == LIBRARY_FILTER_GENRE || filter == LIBRARY_FILTER_YEAR)));
	// A search binds the pattern the value becomes, not the value.
	char pattern[2 * 256 + 3];
	if (filter == LIBRARY_FILTER_SEARCH) {
		if (!search_pattern(value, pattern, sizeof(pattern))) {
			free(ix);
			return NULL;
		}
		bound = true;
		value = pattern;
	}

	char count_sql[PLAYLIST_TABLE_MAX + 256];
	char rows_sql[PLAYLIST_TABLE_MAX + 256];
	char rows_select[64];
	// A second column, for the A-Z buckets counted in this same pass.
	//
	// The sort key where there is one: the query is ordered by it, so the index
	// holds it and the whole pass is answered without ever reading a table row.
	// It also says which letter the row files under, because the byte after the
	// group is the first character folded -- see letter_from_key.
	//
	// The name otherwise. The `character` column the scan writes would be
	// cheaper still, but on a database written by the stock player it holds a
	// pinyin initial, which files a Chinese title under Z while this collation
	// sorts it past Z: the counts and the list would disagree and the strip
	// would land on the wrong rows, with the totals still adding up.
	bool keyed = list_uses_sortkey(kind);
	snprintf(rows_select, sizeof(rows_select), "rowid, %s", keyed ? "sortkey" : name_column(kind));
	list_sql(count_sql, sizeof(count_sql), "COUNT(*)", kind, filter, value, order);
	list_sql(rows_sql, sizeof(rows_sql), rows_select, kind, filter, value, order);
	if (!rows_sql[0]) {
		free(ix);
		return NULL;
	}
	// COUNT(*) has no business sorting: the ORDER BY the builder appended is
	// dead weight on a query that returns one row, and SQLite would still build
	// the sorter for it.
	char *order_by = strstr(count_sql, " ORDER BY ");
	if (order_by) {
		*order_by = '\0';
	}

	db_lock_profiled();
	frameprof_begin(PROF_DB_OPEN);

	// No database to ask. It happens for real: exporting the card to a computer
	// closes the index (usb.c, storage_export) and library_close() sets the
	// handle to NULL, so every list opened while the card is over there arrives
	// here with nothing behind it. Answering NULL is what the callers already
	// expect from a list that cannot be built -- medialist shows its empty
	// label -- and it also skips the whole prologue below, which has no meaning
	// without a handle.
	if (!db) {
		frameprof_end(PROF_DB_OPEN);
		pthread_mutex_unlock(&db_lock);
		free(ix);
		return NULL;
	}

	ix->domain = domain_of(kind);
	ix->generation = generation[ix->domain];

	// Room to work in, for the length of this one query.
	//
	// A list ordered by the sort key reads an index in order and sorts nothing,
	// so this is mostly page cache for that walk. It still matters for the
	// queries that do sort -- one album by disc position, one artist's records,
	// and every list at all while an upgraded database is still having its keys
	// filled in -- where the whole result goes through a temporary b-tree and
	// what does not fit spills to a scratch file on the card. The budget is the
	// page cache, DB_CACHE_KIB for the life of the player.
	uint32_t started_ms = now_ms();

	// The count first, so the row ids land in one allocation of the right size
	// rather than a dozen doublings with a copy each. Both queries run under
	// the same lock, so nothing can change between them.
	int expected = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, count_sql, -1, &stmt, NULL) == SQLITE_OK) {
		if (bound) {
			sqlite3_bind_text(stmt, 1, value, -1, SQLITE_TRANSIENT);
		}
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			expected = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}

	if (expected > 0) {
		ix->rows = malloc((size_t)expected * sizeof(*ix->rows));
		if (!ix->rows) {
			frameprof_end(PROF_DB_OPEN);
			pthread_mutex_unlock(&db_lock);
			free(ix);
			return NULL;
		}
	}

	stmt = NULL;
	if (ix->rows && db && sqlite3_prepare_v2(db, rows_sql, -1, &stmt, NULL) == SQLITE_OK) {
		if (bound) {
			sqlite3_bind_text(stmt, 1, value, -1, SQLITE_TRANSIENT);
		}
		while (ix->count < expected && sqlite3_step(stmt) == SQLITE_ROW) {
			ix->rows[ix->count++] = (int32_t)sqlite3_column_int64(stmt, 0);
			const char *second = (const char *)sqlite3_column_text(stmt, 1);
			char letter = keyed ? library_index_letter_of_key(second) : library_index_letter(second ? second : "");
			ix->buckets[library_index_slot(letter)]++;
		}
		sqlite3_finalize(stmt);

		// The counts say where each letter begins, which is only true of a list
		// ordered by that letter. Favourites run by when they were starred, an
		// album by disc position, an artist's records by album -- three lists
		// whose order has nothing to do with the alphabet.
		bool by_name = kind != LIBRARY_LIST_FAVOURITES && kind != LIBRARY_LIST_PLAYLIST && kind != LIBRARY_LIST_YEARS &&
					   kind != LIBRARY_LIST_MOST_PLAYED &&
					   order != LIBRARY_ORDER_ALBUM && order != LIBRARY_ORDER_ADDED && order != LIBRARY_ORDER_YEAR &&
					   order != LIBRARY_ORDER_YEAR_DESC &&
					   !(kind == LIBRARY_LIST_TRACKS && filter == LIBRARY_FILTER_ALBUM);
		ix->buckets_valid = by_name && ix->count > 0;
	}

	// The pages this read brought in stay: the list is about to be scrolled,
	// and each step of that reads rows by id again (library_index_window).
	// Emptying the cache here was what sent every one of those reads back to
	// the card.

	uint32_t elapsed = now_ms() - started_ms;
	frameprof_end(PROF_DB_OPEN);
	pthread_mutex_unlock(&db_lock);

	// The one line that says whether opening a list is the slow part. It holds
	// the database lock for its whole length, on the interface thread, so it is
	// also how long everything else waited.
	if (elapsed > 250) {
		fprintf(stderr, "library: list of %d rows built in %u ms\n", ix->count, elapsed);
	}

	return ix;
}

int library_index_count(const library_index_t *ix) { return ix ? ix->count : 0; }

bool library_index_stale(const library_index_t *ix) {
	if (!ix) {
		return true;
	}
	// No lock: the handle's domain and generation are set once at open, and the
	// counter is a word that only moves up. Every playlist_* call asks this
	// (refresh_stale_source), so taking db_lock here made each next/prev wait
	// for whatever transaction -- a queue write, a scan batch -- held it.
	return ix->generation != __atomic_load_n(&generation[ix->domain], __ATOMIC_RELAXED);
}

bool library_index_buckets(const library_index_t *ix, int counts[LIBRARY_INDEX_BUCKETS]) {
	if (!ix || !counts || !ix->buckets_valid) {
		return false;
	}
	memcpy(counts, ix->buckets, sizeof(ix->buckets));
	return true;
}

// A descending list is the same row ids read from the other end. No DESC query
// is ever issued: the collation would have to sort the table a second time to
// answer one.
static int row_slot(const struct library_index *ix, int index) {
	return ix->desc ? ix->count - 1 - index : index;
}

// Both window readers: `cb` gets the row, `qcb` the row and its badge.
static int index_window(const library_index_t *ix, int offset, int count, library_row_cb cb,
						library_row_quality_cb qcb, void *user) {
	if (!ix || (!cb && !qcb) || offset < 0 || count <= 0 || offset >= ix->count) {
		return 0;
	}
	if (offset + count > ix->count) {
		count = ix->count - offset;
	}

	char sql_buf[PLAYLIST_TABLE_MAX + 384];
	const char *sql = row_by_id_sql(ix->kind, ix->value, sql_buf, sizeof(sql_buf));
	// A track row is already open: its three numbers come with it.
	bool with_quality = qcb && ix->kind == LIBRARY_LIST_TRACKS;
	if (with_quality) {
		sql = "SELECT name, path, artist, format, sample_rate, bit FROM MEDIA_TABLE WHERE rowid=?";
	}
	if (!sql) {
		return 0;
	}

	db_lock_profiled();

	// A handle from before a rescan or a card change names rows that are now
	// somebody else's. Answering nothing is what makes the caller reload.
	if (ix->generation != generation[ix->domain]) {
		pthread_mutex_unlock(&db_lock);
		return 0;
	}

	frameprof_begin(PROF_DB_WINDOW);
	int delivered = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		for (int i = 0; i < count; i++) {
			sqlite3_reset(stmt);
			sqlite3_bind_int64(stmt, 1, ix->rows[row_slot(ix, offset + i)]);
			if (sqlite3_step(stmt) != SQLITE_ROW) {
				// The row went while the window was being read. The window is
				// positional, so it gets a blank rather than the next row
				// shifted into its place.
				if (qcb ? !qcb("", NULL, NULL, -1, user) : !cb("", NULL, NULL, user)) {
					break;
				}
				delivered++;
				continue;
			}
			const char *name = (const char *)sqlite3_column_text(stmt, 0);
			const char *path = (const char *)sqlite3_column_text(stmt, 1);
			const char *artist = (const char *)sqlite3_column_text(stmt, 2);
			bool more;
			if (qcb) {
				int quality = -1;
				if (with_quality) {
					quality = (int)quality_of(sqlite3_column_type(stmt, 3) == SQLITE_NULL ? -1
																						   : sqlite3_column_int(stmt, 3),
											  sqlite3_column_int(stmt, 4), sqlite3_column_int(stmt, 5));
				}
				more = qcb(name ? name : "", path, artist, quality, user);
			} else {
				more = cb(name ? name : "", path, artist, user);
			}
			if (!more) {
				break;
			}
			delivered++;
		}
		sqlite3_finalize(stmt);
	}
	frameprof_end(PROF_DB_WINDOW);

	pthread_mutex_unlock(&db_lock);
	return delivered;
}

int library_index_window(const library_index_t *ix, int offset, int count, library_row_cb cb, void *user) {
	return index_window(ix, offset, count, cb, NULL, user);
}

int library_index_window_quality(const library_index_t *ix, int offset, int count, library_row_quality_cb cb,
								 void *user) {
	return index_window(ix, offset, count, NULL, cb, user);
}

library_index_t *library_index_clone(const library_index_t *ix) {
	if (!ix) {
		return NULL;
	}
	struct library_index *copy = malloc(sizeof(*copy));
	if (!copy) {
		return NULL;
	}
	*copy = *ix;
	copy->rows = NULL;
	if (ix->count > 0) {
		copy->rows = malloc((size_t)ix->count * sizeof(*copy->rows));
		if (!copy->rows) {
			free(copy);
			return NULL;
		}
		memcpy(copy->rows, ix->rows, (size_t)ix->count * sizeof(*copy->rows));
	}
	return copy;
}

// What the handle was built from, so a queue made out of one can be written
// down as a question rather than as its answer.
void library_index_describe(const library_index_t *ix, library_index_spec_t *out) {
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	if (!ix) {
		return;
	}
	out->kind = ix->kind;
	out->filter = ix->filter;
	out->order = ix->order;
	out->desc = ix->desc;
	snprintf(out->value, sizeof(out->value), "%s", ix->value);
	out->valid = true;
}

// Where a row id sits in the list as it reads, -1 when it is not in it. A walk
// of an array of ints: two hundred thousand comparisons is microseconds, where
// asking the database for each position in turn would be two hundred thousand
// queries.
static int position_of_row(const struct library_index *ix, int32_t rowid) {
	if (rowid < 0) {
		return -1;
	}
	for (int i = 0; i < ix->count; i++) {
		if (ix->rows[i] == rowid) {
			return ix->desc ? ix->count - 1 - i : i;
		}
	}
	return -1;
}

int library_index_find_path(const library_index_t *ix, const char *path) {
	if (!ix || !path || !path[0] || ix->kind == LIBRARY_LIST_ALBUMS) {
		return -1;
	}

	// Which table the handle's row ids belong to. A playlist's rows are the
	// playlist's own table: looking the path up in MEDIA_TABLE and comparing
	// that row id against them matches by coincidence, and the queue then jumps
	// to whatever track happens to sit at that number.
	char sql[PLAYLIST_TABLE_MAX + 160];
	if (ix->kind == LIBRARY_LIST_PLAYLIST) {
		char quoted[PLAYLIST_TABLE_MAX];
		if (!playlist_table(ix->value, quoted, sizeof(quoted))) {
			return -1;
		}
		if (snprintf(sql, sizeof(sql), "SELECT p.rowid FROM %s p WHERE p.path = ? AND " PLAYLIST_SHOWN, quoted) >=
			(int)sizeof(sql)) {
			return -1;
		}
	} else {
		const char *table = ix->kind == LIBRARY_LIST_FAVOURITES	 ? "FAVOURITES"
							: ix->kind == LIBRARY_LIST_MOST_PLAYED ? "PLAY_COUNTS"
																   : "MEDIA_TABLE";
		snprintf(sql, sizeof(sql), "SELECT rowid FROM %s WHERE path = ?", table);
	}

	pthread_mutex_lock(&db_lock);
	int32_t rowid = -1;
	sqlite3_stmt *stmt = NULL;
	if (db && ix->generation == generation[ix->domain] && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			rowid = (int32_t)sqlite3_column_int64(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);

	return position_of_row(ix, rowid);
}

int library_index_find_name(const library_index_t *ix, const char *name) {
	if (!ix || !name || !name[0]) {
		return -1;
	}

	// The table the handle's row ids belong to, and the column it is named by.
	// An album is one row of ALBUM_GROUP_TABLE per name and key, so a value
	// with a key finds its own record and a name alone the first of that name.
	const char *sql;
	char album[256];
	const char *key = NULL;
	switch (ix->kind) {
	case LIBRARY_LIST_ALBUMS: {
		library_album_title(name, album, sizeof(album));
		const char *sep = strchr(name, LIBRARY_ALBUM_KEY_SEP);
		key = sep ? sep + 1 : NULL;
		sql = key ? "SELECT rowid FROM ALBUM_GROUP_TABLE WHERE album = ?1 AND album_key = ?2"
				  : "SELECT rowid FROM ALBUM_GROUP_TABLE WHERE album = ?1 ORDER BY rowid";
		name = album;
		break;
	}
	case LIBRARY_LIST_ARTISTS:
		sql = "SELECT rowid FROM ARTIST_TABLE WHERE artist = ?1";
		break;
	case LIBRARY_LIST_ALBUM_ARTISTS:
		sql = "SELECT rowid FROM ALBUM_ARTIST_TABLE WHERE album_artist = ?1";
		break;
	case LIBRARY_LIST_GENRES:
		sql = "SELECT rowid FROM GENRE_TABLE WHERE genre = ?1";
		break;
	default:
		return -1;
	}

	pthread_mutex_lock(&db_lock);
	int32_t rowid = -1;
	sqlite3_stmt *stmt = NULL;
	if (db && ix->generation == generation[ix->domain] && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
		if (key) {
			sqlite3_bind_text(stmt, 2, key, -1, SQLITE_TRANSIENT);
		}
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			rowid = (int32_t)sqlite3_column_int64(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);

	return position_of_row(ix, rowid);
}

void library_index_close(library_index_t *ix) {
	if (!ix) {
		return;
	}
	free(ix->rows);
	free(ix);
}

// ---------------------------------------------------------------------------
// scanning
// ---------------------------------------------------------------------------

// Free memory, for the scan log. -1 where /proc/meminfo does not exist (the
// simulator on a system without it).
static long mem_available_kb(void) {
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) {
		return -1;
	}
	char line[160];
	long kb = -1;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) {
			break;
		}
	}
	fclose(f);
	return kb;
}

// A growable list of names: what the walk holds while it reads one level (see
// scan_directory).
typedef struct {
	char **names;
	int count;
	int capacity;
} namelist_t;

static void namelist_init(namelist_t *l) {
	l->names = NULL;
	l->count = 0;
	l->capacity = 0;
}

// Adds a copy of `name`, unless the list already holds `limit` names or memory
// runs out.
static bool namelist_push(namelist_t *l, const char *name, int limit) {
	if (l->count >= limit) {
		return false;
	}
	if (l->count == l->capacity) {
		int grown = l->capacity ? l->capacity * 2 : 16;
		if (grown > limit) {
			grown = limit;
		}
		char **bigger = realloc(l->names, (size_t)grown * sizeof(*bigger));
		if (!bigger) {
			return false; // out of memory: index whatever was collected so far
		}
		l->names = bigger;
		l->capacity = grown;
	}

	char *copy = strdup(name);
	if (!copy) {
		return false;
	}
	l->names[l->count++] = copy;
	return true;
}

static bool namelist_add(namelist_t *l, const char *name) { return namelist_push(l, name, SCAN_MAX_SUBDIRS); }

static void namelist_free(namelist_t *l) {
	for (int i = 0; i < l->count; i++) {
		free(l->names[i]);
	}
	free(l->names);
	namelist_init(l);
}

// Everything the player can play, with two deliberate exclusions.
//
// This list must stay in step with decode_detect_format() plus WAV (which has
// its own path in audio.c): a format that plays but does not index is an album
// that exists in Explore and not in Albums, Artists, Genres or search -- which
// is exactly what happened to AAC and ALAC, both inside a .m4a.
//
// .m4b is excluded: it is an audiobook and has its own scanner in
// audiobookdb.c, feeding the Audiobooks page. Indexing it here too would put
// ten hours of novel in among the tracks.
//
// .mp4 is excluded as well. The decoder can read it -- same container as a
// .m4a -- but a file named .mp4 is a film, and the exceptions are not worth the
// certainty of finding an episode among the albums. Music in this container is
// named .m4a: that is indexed, and still inspected (see is_video_file) because
// the extension alone proves nothing.
static bool is_playable(const char *name) {
	static const char *const EXTENSIONS[] = {".wav",  ".mp3", ".flac", ".ogg",  ".dsf",  ".dff",
											 ".aif",  ".aiff", ".aifc", ".caf", ".opus", ".wv",
											 ".m4a",  ".alac", ".aac",  ".ape",  ".cue"};

	for (size_t i = 0; i < sizeof(EXTENSIONS) / sizeof(EXTENSIONS[0]); i++) {
		if (has_extension(name, EXTENSIONS[i])) {
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// What a file actually is
//
// The quality badge in the lists needs three things about a track: whether it
// is DSD, whether it is lossy, and -- for the lossless ones -- whether it is CD
// or better than CD. The first two are the container; the third is the sample
// rate and the bit depth, which nothing was writing down.
//
// Opening a decoder to ask costs an order of magnitude more per file than
// reading the tags, which is minutes on a large library for two integers. So
// the header is read directly instead -- a few bytes at a known place -- and
// only for the containers where the answer is not already settled by the
// extension:
//
//   FLAC  STREAMINFO, the first metadata block, at a fixed offset
//   WAV   the `fmt ` chunk, found by walking the chunk list
//   DSF   the `fmt ` chunk of a DSD stream, at a fixed offset
//   MP4   through the parser the player already has (rate, and ALAC's depth)
//   APE   the Monkey's Audio header, through apedec_probe()
//
// Anything else keeps zeros, and a row with zeros gets no badge: a wrong badge
// is worse than none. That is mp3, ogg, opus, raw aac -- all lossy, so the
// badge is decided by the container anyway -- and WavPack, which is the one
// lossless format left without an answer.
// ---------------------------------------------------------------------------


static uint32_t le32(const unsigned char *p) {
	return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

// STREAMINFO packs the rate in 20 bits and the depth in 5, straddling bytes.
static void probe_flac(FILE *f, int *rate, int *bits) {
	unsigned char head[4];
	if (fread(head, 1, 4, f) != 4) {
		return;
	}
	// An ID3 tag before the stream is legal and common: skip it and look again.
	if (head[0] == 'I' && head[1] == 'D' && head[2] == '3') {
		unsigned char rest[6];
		if (fread(rest, 1, 6, f) != 6) {
			return;
		}
		long skip = ((long)(rest[2] & 0x7f) << 21) | ((long)(rest[3] & 0x7f) << 14) | ((long)(rest[4] & 0x7f) << 7) |
					(rest[5] & 0x7f);
		if (fseek(f, 10 + skip, SEEK_SET) != 0 || fread(head, 1, 4, f) != 4) {
			return;
		}
	}
	if (memcmp(head, "fLaC", 4) != 0) {
		return;
	}

	unsigned char block[38]; // 4 bytes of block header, then 34 of STREAMINFO
	if (fread(block, 1, sizeof(block), f) != sizeof(block)) {
		return;
	}
	if ((block[0] & 0x7f) != 0) {
		return; // the first block is meant to be STREAMINFO
	}
	const unsigned char *si = block + 4;
	*rate = (int)(((uint32_t)si[10] << 12) | ((uint32_t)si[11] << 4) | (uint32_t)(si[12] >> 4));
	*bits = (int)((((si[12] & 0x01) << 4) | (si[13] >> 4)) + 1);
}

static void probe_wav(FILE *f, int *rate, int *bits) {
	unsigned char head[12];
	if (fread(head, 1, sizeof(head), f) != sizeof(head) || memcmp(head, "RIFF", 4) != 0 ||
		memcmp(head + 8, "WAVE", 4) != 0) {
		return;
	}
	// The chunks, until `fmt `. Bounded: a file that is not one would otherwise
	// be walked to its end.
	for (int i = 0; i < 32; i++) {
		unsigned char chunk[8];
		if (fread(chunk, 1, sizeof(chunk), f) != sizeof(chunk)) {
			return;
		}
		uint32_t size = le32(chunk + 4);
		if (memcmp(chunk, "fmt ", 4) == 0) {
			unsigned char fmt[16];
			if (size < sizeof(fmt) || fread(fmt, 1, sizeof(fmt), f) != sizeof(fmt)) {
				return;
			}
			*rate = (int)le32(fmt + 4);
			*bits = (int)(fmt[14] | ((uint32_t)fmt[15] << 8));
			return;
		}
		if (fseek(f, (long)size + (size & 1), SEEK_CUR) != 0) {
			return;
		}
	}
}

// DSF: a 28-byte "DSD " chunk, then a `fmt ` chunk carrying the rate 28 bytes
// in. The same offsets parse_dsf() in the decoder reads.
static void probe_dsf(FILE *f, int *rate, int *bits) {
	unsigned char head[80]; // 28 of DSD chunk, then the head of the fmt chunk
	if (fread(head, 1, sizeof(head), f) != sizeof(head) || memcmp(head, "DSD ", 4) != 0 ||
		memcmp(head + 28, "fmt ", 4) != 0) {
		return;
	}
	*rate = (int)le32(head + 28 + 28);
	*bits = 1; // one bit per sample is what DSD is
}

// Fills the sample rate and bit depth of a file, or leaves them at zero when
// they cannot be had cheaply. `alac` says the file is lossless MP4, which the
// extension cannot say: a .m4a is AAC or ALAC and only the sample entry knows
// which.
static void probe_stream_format(const char *path, const char *filename, int *rate, int *bits, bool *alac) {
	*rate = 0;
	*bits = 0;
	*alac = false;

	bool flac = has_extension(filename, ".flac");
	bool wav = has_extension(filename, ".wav");
	bool dsf = has_extension(filename, ".dsf");

	if (has_extension(filename, ".ape")) {
		if (!apedec_probe(path, rate, bits)) {
			*rate = 0;
			*bits = 0;
		}
		return;
	}
	bool mp4 = has_extension(filename, ".m4a") || has_extension(filename, ".alac") || has_extension(filename, ".mp4");

	if (mp4) {
		// The player's own parser: it is already opening these files to find out
		// whether they are video, and it knows the rate.
		mp4_file_t *m = mp4_open(path);
		if (m) {
			*rate = mp4_audio_sample_rate(m);
			if (mp4_audio_codec(m) == MP4_CODEC_ALAC) {
				*alac = true;
				// ALACSpecificConfig: frameLength(4), compatibleVersion(1),
				// then the depth. AAC has no depth to read -- it is lossy and
				// the number would describe the decoder, not the recording.
				int len = 0;
				const unsigned char *cfg = mp4_audio_config(m, &len);
				if (cfg && len >= 6) {
					*bits = cfg[5];
				}
			}
			mp4_close(m);
		}
		return;
	}
	if (!flac && !wav && !dsf) {
		return;
	}

	FILE *f = fopen(path, "rb");
	if (!f) {
		return;
	}
	if (flac) {
		probe_flac(f, rate, bits);
	} else if (wav) {
		probe_wav(f, rate, bits);
	} else {
		probe_dsf(f, rate, bits);
	}
	fclose(f);
}

// Which format code the row records, matching the stock numbering where it has
// one. The column is informational: nothing outside this file reads it.
static int format_of(const char *name) {
	if (has_extension(name, ".flac")) {
		return FORMAT_FLAC;
	}
	if (has_extension(name, ".mp3")) {
		return FORMAT_MP3;
	}
	if (has_extension(name, ".ogg")) {
		return FORMAT_OGG;
	}
	if (has_extension(name, ".dsf") || has_extension(name, ".dff")) {
		return FORMAT_DSD;
	}
	// Codes the stock numbering never had. Without them half the library is
	// reported as WAV.
	if (has_extension(name, ".m4a") || has_extension(name, ".alac")) {
		return FORMAT_AAC; // overridden to FORMAT_ALAC when the container says so
	}
	if (has_extension(name, ".opus")) {
		return FORMAT_OPUS;
	}
	if (has_extension(name, ".wv")) {
		return FORMAT_WAVPACK;
	}
	if (has_extension(name, ".ape")) {
		return FORMAT_APE;
	}
	return FORMAT_WAV;
}

static void set_scan_folder(const char *path) {
	pthread_mutex_lock(&scan_folder_lock);
	snprintf(scan_folder, sizeof(scan_folder), "%s", path);
	pthread_mutex_unlock(&scan_folder_lock);
}

// ---------------------------------------------------------------------------
// Lists kept as text files beside device_config.ini, one entry per line: the
// folders chosen for the scan and the artists never split.
// ---------------------------------------------------------------------------

static void lines_path(const char *file, char *out, size_t size) {
	const char *cfg = config_path();
	const char *slash = cfg ? strrchr(cfg, '/') : NULL;
	if (slash) {
		snprintf(out, size, "%.*s/%s", (int)(slash - cfg), cfg, file);
	} else {
		snprintf(out, size, "%s", file);
	}
}

// Appends `name` to a growing array. False when memory runs out.
static bool names_add(char ***names, int *count, int *capacity, const char *name, size_t len) {
	if (*count == *capacity) {
		int grown = *capacity ? *capacity * 2 : 32;
		char **bigger = realloc(*names, (size_t)grown * sizeof(*bigger));
		if (!bigger) {
			return false;
		}
		*names = bigger;
		*capacity = grown;
	}
	char *copy = malloc(len + 1);
	if (!copy) {
		return false;
	}
	memcpy(copy, name, len);
	copy[len] = '\0';
	(*names)[(*count)++] = copy;
	return true;
}

static void names_free(char **names, int count) {
	for (int i = 0; i < count; i++) {
		free(names[i]);
	}
	free(names);
}

// The non-empty lines of `file`. False when the file does not exist.
static bool lines_read(const char *file, char ***names_out, int *count_out) {
	*names_out = NULL;
	*count_out = 0;
	char path[512];
	lines_path(file, path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	int capacity = 0;
	char *line = NULL;
	size_t cap = 0;
	while (getline(&line, &cap, f) >= 0) {
		size_t len = strcspn(line, "\r\n");
		if (len > 0 && !names_add(names_out, count_out, &capacity, line, len)) {
			break;
		}
	}
	free(line);
	fclose(f);
	return true;
}

// Replaces `file` with `names`, one per line, through a temporary file and a
// rename so a pulled card leaves either list whole.
static bool lines_write(const char *file, const char *const *names, int count) {
	char path[512];
	lines_path(file, path, sizeof(path));
	char tmp[520];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		fprintf(stderr, "library: cannot write %s: %s\n", tmp, strerror(errno));
		return false;
	}
	for (int i = 0; names && i < count; i++) {
		if (names[i] && names[i][0]) {
			fprintf(f, "%s\n", names[i]);
		}
	}
	bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, path) != 0) {
		fprintf(stderr, "library: cannot write %s\n", path);
		unlink(tmp);
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// How the index is organised: multi-name tags split, albums joined, retagged
// files read again. The settings live in [library]; a scan works from a copy
// taken when it starts (rules_load), so changing one mid-scan cannot leave half
// the index built one way and half the other.
// ---------------------------------------------------------------------------

#define ARTIST_EXCEPTIONS_FILE "artist_exceptions.txt"
#define ARTIST_EXCEPTIONS_DEFAULT "AC/DC"
#define ARTIST_SEPARATORS_DEFAULT                                                                                    \
	(LIBRARY_SPLIT_SEMICOLON | LIBRARY_SPLIT_SLASH | LIBRARY_SPLIT_AMPERSAND | LIBRARY_SPLIT_FEAT)
#define GENRE_SEPARATORS_DEFAULT (LIBRARY_SPLIT_SEMICOLON | LIBRARY_SPLIT_SLASH | LIBRARY_SPLIT_COMMA)

bool library_split_artists(void) { return config_get_bool("library", "split_artists", false); }

void library_set_split_artists(bool on) {
	config_set_bool("library", "split_artists", on);
	config_save();
}

unsigned library_artist_separators(void) {
	return (unsigned)config_get_int("library", "artist_separators", ARTIST_SEPARATORS_DEFAULT);
}

void library_set_artist_separators(unsigned separators) {
	config_set_int("library", "artist_separators", (long)separators);
	config_save();
}

bool library_split_genres(void) { return config_get_bool("library", "split_genres", false); }

void library_set_split_genres(bool on) {
	config_set_bool("library", "split_genres", on);
	config_save();
}

unsigned library_genre_separators(void) {
	return (unsigned)config_get_int("library", "genre_separators", GENRE_SEPARATORS_DEFAULT);
}

void library_set_genre_separators(unsigned separators) {
	config_set_int("library", "genre_separators", (long)separators);
	config_save();
}

bool library_split_album_artists(void) { return config_get_bool("library", "split_album_artists", false); }

void library_set_split_album_artists(bool on) {
	config_set_bool("library", "split_album_artists", on);
	config_save();
}

char **library_artist_exceptions(int *count) {
	char **names = NULL;
	if (!lines_read(ARTIST_EXCEPTIONS_FILE, &names, count)) {
		int capacity = 0;
		names_add(&names, count, &capacity, ARTIST_EXCEPTIONS_DEFAULT, strlen(ARTIST_EXCEPTIONS_DEFAULT));
	}
	return names;
}

void library_artist_exceptions_free(char **names, int count) { names_free(names, count); }

void library_set_artist_exceptions(const char *const *names, int count) {
	lines_write(ARTIST_EXCEPTIONS_FILE, names, count);
}

bool library_join_albums(void) { return config_get_bool("library", "join_albums", false); }

void library_set_join_albums(bool on) {
	config_set_bool("library", "join_albums", on);
	config_save();
}

bool library_detect_retagged(void) { return config_get_bool("library", "detect_retagged", true); }

void library_set_detect_retagged(bool on) {
	config_set_bool("library", "detect_retagged", on);
	config_save();
}

typedef struct {
	unsigned artist_separators;		  // 0: artists are not split
	unsigned album_artist_separators; // 0: album artists are not split
	unsigned genre_separators;		  // 0: genres are not split
	char **exceptions;
	int exception_count;
	bool join_albums;
	bool retagged;
} scan_rules_t;

// The copy the scan thread works from, and the one waiting for it when the
// settings changed while it was busy.
static scan_rules_t rules;
static scan_rules_t pending_rules;

// On the interface thread -- the one that writes the settings -- never on the
// scan thread.
static void rules_load(scan_rules_t *r) {
	names_free(r->exceptions, r->exception_count);
	r->exceptions = NULL;
	r->exception_count = 0;
	r->artist_separators = library_split_artists() ? library_artist_separators() : 0;
	r->album_artist_separators = r->artist_separators && library_split_album_artists() ? r->artist_separators : 0;
	r->genre_separators = library_split_genres() ? library_genre_separators() : 0;
	if (r->artist_separators) {
		r->exceptions = library_artist_exceptions(&r->exception_count);
	}
	r->join_albums = library_join_albums();
	r->retagged = library_detect_retagged();
}

// The settings in `r` that decide how the index is filed, as a short string.
static void rules_signature(const scan_rules_t *r, char *out, size_t size) {
	uint32_t hash = 2166136261u;
	for (int i = 0; r->artist_separators && i < r->exception_count; i++) {
		for (const unsigned char *p = (const unsigned char *)r->exceptions[i]; *p; p++) {
			hash = (hash ^ (unsigned char)tolower(*p)) * 16777619u;
		}
		hash = (hash ^ '\n') * 16777619u;
	}
	// The album artist part only when it is on, so an index filed before it
	// existed still reads as filed under the same settings.
	char album_artists[16] = "";
	if (r->album_artist_separators) {
		snprintf(album_artists, sizeof(album_artists), "/b%u", r->album_artist_separators);
	}
	snprintf(out, size, "a%u/g%u/j%d/x%08x%s", r->artist_separators, r->genre_separators, r->join_albums ? 1 : 0,
			 r->artist_separators ? (unsigned)hash : 0u, album_artists);
}

// With db_lock held.
static void organize_state_put(const scan_rules_t *r) {
	char signature[64];
	rules_signature(r, signature, sizeof(signature));
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO ORGANIZE_STATE(id, rules) VALUES(0, ?1)", -1, &stmt, NULL) ==
				  SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, signature, -1, SQLITE_TRANSIENT);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
}

// Whether the index is filed under the settings as they are now. On the
// interface thread, like rules_load().
static bool organized_as_set(void) {
	scan_rules_t now = {0};
	rules_load(&now);
	char want[64];
	rules_signature(&now, want, sizeof(want));
	names_free(now.exceptions, now.exception_count);

	scan_rules_t off = {0};
	char have[64];
	rules_signature(&off, have, sizeof(have));
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT rules FROM ORGANIZE_STATE WHERE id = 0", -1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_text(stmt, 0)) {
			snprintf(have, sizeof(have), "%s", (const char *)sqlite3_column_text(stmt, 0));
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return strcmp(want, have) == 0;
}

// With db_lock held.
static void links_check(void) {
	artist_links = count_rows("SELECT 1 FROM ARTIST_LINK LIMIT 1") > 0;
	album_artist_links = count_rows("SELECT 1 FROM ALBUM_ARTIST_LINK LIMIT 1") > 0;
	genre_links = count_rows("SELECT 1 FROM GENRE_LINK LIMIT 1") > 0;
}

#define SPLIT_MAX 16
#define SPLIT_NAME_MAX 256

typedef struct {
	char text[SPLIT_NAME_MAX];
	int len;
	bool in_brackets; // began inside "(...)" or "[...]", as "B" in "A (feat. B)"
} split_part_t;

// A word separator ("feat", "vs") at `p`: preceded by a space or an opening
// bracket, followed by a space or, for an abbreviation, a full stop. Returns
// its length with the stop, or 0. "x" counts in lower case only, so that
// "Malcolm X" stays one name. "x", "with" and "and" are ordinary words inside
// brackets -- "A (Live and Acoustic)" -- and split only outside them.
static int split_word_at(const char *text, const char *p, unsigned separators, bool in_brackets) {
	static const struct {
		unsigned bit;
		const char *word;
		bool stop;	   // may end in a full stop: "feat.", "vs."
		bool exact;	   // the case as written
		bool outside; // outside brackets only
	} WORDS[] = {
		{LIBRARY_SPLIT_FEAT, "featuring", true, false, false},
		{LIBRARY_SPLIT_FEAT, "feat", true, false, false},
		{LIBRARY_SPLIT_FEAT, "ft", true, false, false},
		{LIBRARY_SPLIT_VS, "versus", true, false, false},
		{LIBRARY_SPLIT_VS, "vs", true, false, false},
		{LIBRARY_SPLIT_X, "x", false, true, true},
		{LIBRARY_SPLIT_WITH, "with", false, false, true},
		{LIBRARY_SPLIT_AND, "and", false, false, true},
	};
	if (p == text || (p[-1] != ' ' && p[-1] != '(' && p[-1] != '[')) {
		return 0;
	}
	for (size_t i = 0; i < sizeof(WORDS) / sizeof(WORDS[0]); i++) {
		if (!(separators & WORDS[i].bit) || (WORDS[i].outside && in_brackets)) {
			continue;
		}
		size_t n = strlen(WORDS[i].word);
		if ((WORDS[i].exact ? strncmp(p, WORDS[i].word, n) : strncasecmp(p, WORDS[i].word, n)) != 0) {
			continue;
		}
		if (WORDS[i].stop && p[n] == '.') {
			return (int)n + 1;
		}
		if (p[n] == ' ') {
			return (int)n;
		}
	}
	return 0;
}

// A symbol separator at `p`, ASCII or one of its full-width forms (see
// library.h). Returns its length in bytes, or 0.
static int split_symbol_at(const char *p, unsigned separators) {
	static const struct {
		unsigned bit;
		const char *symbol;
	} SYMBOLS[] = {
		{LIBRARY_SPLIT_SEMICOLON, ";"},
		{LIBRARY_SPLIT_SEMICOLON, "\xef\xbc\x9b"}, // U+FF1B
		{LIBRARY_SPLIT_SLASH, "/"},
		{LIBRARY_SPLIT_SLASH, "\xef\xbc\x8f"}, // U+FF0F
		{LIBRARY_SPLIT_AMPERSAND, "&"},
		{LIBRARY_SPLIT_AMPERSAND, "\xef\xbc\x86"}, // U+FF06
		{LIBRARY_SPLIT_COMMA, ","},
		{LIBRARY_SPLIT_COMMA, "\xef\xbc\x8c"}, // U+FF0C
		{LIBRARY_SPLIT_COMMA, "\xe3\x80\x81"}, // U+3001, the ideographic comma
		{LIBRARY_SPLIT_PLUS, "+"},
		{LIBRARY_SPLIT_PLUS, "\xef\xbc\x8b"}, // U+FF0B
		{LIBRARY_SPLIT_X, "\xc3\x97"},		   // U+00D7, the multiplication sign
	};
	for (size_t i = 0; i < sizeof(SYMBOLS) / sizeof(SYMBOLS[0]); i++) {
		size_t n = strlen(SYMBOLS[i].symbol);
		if ((separators & SYMBOLS[i].bit) && strncmp(p, SYMBOLS[i].symbol, n) == 0) {
			return (int)n;
		}
	}
	return 0;
}

// Spaces off both ends; a bracket left open at the end of a name ("A (" from
// "A (feat. B)"); and, on a name that began inside brackets, the bracket that
// closed them ("B)").
static void split_trim(split_part_t *part) {
	char *s = part->text;
	int start = 0;
	int end = part->len;
	for (;;) {
		while (start < end && (s[start] == ' ' || s[start] == '\t')) {
			start++;
		}
		while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t')) {
			end--;
		}
		if (end > start && (s[end - 1] == '(' || s[end - 1] == '[' || s[end - 1] == '-')) {
			end--;
			continue;
		}
		if (part->in_brackets && end > start && (s[end - 1] == ')' || s[end - 1] == ']')) {
			end--;
			part->in_brackets = false;
			continue;
		}
		break;
	}
	memmove(s, s + start, (size_t)(end - start));
	s[end - start] = '\0';
	part->len = end - start;
}

// Splits a tag holding several names -- "A & B", "A feat. B", "Rock; Pop" -- at
// the separators chosen, keeping an exception ("AC/DC") whole wherever it
// appears. Inside brackets a symbol splits only after a word separator in the
// same brackets: "A (feat. B & C)" is three names, "A (B & C)" and
// "Rock (Hard/Soft)" one each. Writes the names, trimmed and without repeats,
// and returns how many; 0 when the text is one name, which is then the text
// itself, untouched.
static int split_names(const char *text, unsigned separators, char *const *exceptions, int exception_count,
					   char out[][SPLIT_NAME_MAX], int max) {
	if (!text || !text[0] || !separators) {
		return 0;
	}
	split_part_t *parts = calloc(SPLIT_MAX, sizeof(*parts));
	if (!parts) {
		return 0;
	}
	int count = 1;
	int depth = 0;
	bool word_in_brackets = false;
	const char *p = text;
	while (*p) {
		split_part_t *cur = &parts[count - 1];
		size_t keep = 0;
		for (int i = 0; i < exception_count && !keep; i++) {
			size_t n = strlen(exceptions[i]);
			if (n > 0 && strncasecmp(p, exceptions[i], n) == 0) {
				keep = n;
			}
		}
		int cut = 0;
		if (!keep) {
			int symbol = split_symbol_at(p, separators);
			if (symbol > 0) {
				cut = depth == 0 || word_in_brackets ? symbol : 0;
			} else {
				cut = split_word_at(text, p, separators, depth > 0);
				word_in_brackets |= cut > 0 && depth > 0;
			}
		}
		if (cut > 0) {
			p += cut;
			if (count < SPLIT_MAX) {
				count++;
				parts[count - 1].in_brackets = depth > 0;
			}
			continue;
		}
		size_t n = keep ? keep : 1;
		if (!keep) {
			if (*p == '(' || *p == '[') {
				depth++;
			} else if ((*p == ')' || *p == ']') && depth > 0) {
				depth--;
				word_in_brackets = word_in_brackets && depth > 0;
			}
		}
		if (cur->len + (int)n < SPLIT_NAME_MAX) {
			memcpy(cur->text + cur->len, p, n);
			cur->len += (int)n;
		}
		p += n;
	}

	int written = 0;
	for (int i = 0; i < count && written < max; i++) {
		parts[i].text[parts[i].len] = '\0';
		split_trim(&parts[i]);
		if (!parts[i].text[0]) {
			continue;
		}
		bool repeat = false;
		for (int j = 0; j < written && !repeat; j++) {
			repeat = strcasecmp(out[j], parts[i].text) == 0;
		}
		if (!repeat) {
			snprintf(out[written++], SPLIT_NAME_MAX, "%s", parts[i].text);
		}
	}
	free(parts);
	return written > 1 ? written : 0;
}

// The statements a scan runs over and over. Prepared once when the scan starts
// and reused for every row: re-parsing the same INSERT for each of ten thousand
// tracks costs both time and, because each parse allocates, memory churn.
static sqlite3_stmt *stmt_track;
static sqlite3_stmt *stmt_album;
static sqlite3_stmt *stmt_artist;
static sqlite3_stmt *stmt_genre;
static sqlite3_stmt *stmt_album_artist;
static sqlite3_stmt *stmt_album_group;
static sqlite3_stmt *stmt_artist_link;
static sqlite3_stmt *stmt_album_artist_link;
static sqlite3_stmt *stmt_genre_link;

static void finalize_statements(void) {
	sqlite3_finalize(stmt_track);
	sqlite3_finalize(stmt_album);
	sqlite3_finalize(stmt_artist);
	sqlite3_finalize(stmt_genre);
	sqlite3_finalize(stmt_album_artist);
	sqlite3_finalize(stmt_album_group);
	sqlite3_finalize(stmt_artist_link);
	sqlite3_finalize(stmt_album_artist_link);
	sqlite3_finalize(stmt_genre_link);
	stmt_track = stmt_album = stmt_artist = stmt_genre = stmt_album_artist = stmt_album_group = NULL;
	stmt_artist_link = stmt_album_artist_link = stmt_genre_link = NULL;
}

static bool prepare_statements(void) {
	finalize_statements();

	static const char *const LOOKUP_SQL[] = {
		"INSERT OR IGNORE INTO ALBUM_TABLE(id,album,character,cn,sortkey) VALUES(0,?,?,?,?)",
		"INSERT OR IGNORE INTO ARTIST_TABLE(id,artist,character,cn,sortkey) VALUES(0,?,?,?,?)",
		"INSERT OR IGNORE INTO GENRE_TABLE(id,genre,character,cn,sortkey) VALUES(0,?,?,?,?)",
	};

	sqlite3_stmt **targets[] = {&stmt_album, &stmt_artist, &stmt_genre};
	for (int i = 0; i < 3; i++) {
		if (sqlite3_prepare_v2(db, LOOKUP_SQL[i], -1, targets[i], NULL) != SQLITE_OK) {
			fprintf(stderr, "library: prepare failed: %s\n", sqlite3_errmsg(db));
			return false;
		}
	}

	if (sqlite3_prepare_v2(db,
						   "INSERT OR IGNORE INTO ALBUM_ARTIST_TABLE"
						   "(id,album_artist,character,cn,ctime,mtime,mqa,pinyin_charater,sortkey)"
						   " VALUES(0,?,?,?,0,0,0,'',?)",
						   -1, &stmt_album_artist, NULL) != SQLITE_OK) {
		return false;
	}

	if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO ALBUM_GROUP_TABLE(album,album_key,sortkey) VALUES(?,?,?)", -1,
						   &stmt_album_group, NULL) != SQLITE_OK) {
		return false;
	}

	if (sqlite3_prepare_v2(db, "INSERT INTO ARTIST_LINK(path,artist) VALUES(?,?)", -1, &stmt_artist_link, NULL) !=
			SQLITE_OK ||
		sqlite3_prepare_v2(db, "INSERT INTO ALBUM_ARTIST_LINK(path,album_artist) VALUES(?,?)", -1,
						   &stmt_album_artist_link, NULL) != SQLITE_OK ||
		sqlite3_prepare_v2(db, "INSERT INTO GENRE_LINK(path,genre) VALUES(?,?)", -1, &stmt_genre_link, NULL) !=
			SQLITE_OK) {
		return false;
	}

	if (sqlite3_prepare_v2(db,
						   "INSERT OR REPLACE INTO MEDIA_TABLE"
						   "(id,path,name,album,artist,genre,year,dis_id,ck_id,has_child_file,begin_time,end_time,"
						   "cue_id,character,size,sample_rate,bit_rate,bit,channel,format,quality,album_pic_path,"
						   "lrc_path,track_gain,track_peak,album_artist,ctime,mtime,sortkey,disc,album_key)"
						   " VALUES(?,?,?,?,?,?,?,?,0,0,0,0,0,?,?,?,0,?,0,?,\'\',NULL,NULL,0,0,?,?,?,?,?,?)",
						   -1, &stmt_track, NULL) != SQLITE_OK) {
		fprintf(stderr, "library: prepare failed: %s\n", sqlite3_errmsg(db));
		return false;
	}

	return true;
}

// Adds a name to one of the lookup tables, if it isn't there already.
static void run_lookup(sqlite3_stmt *stmt, const char *value) {
	if (!stmt || !value || !value[0]) {
		return;
	}

	char character[16];
	int group = 0;
	index_character(value, character, sizeof(character), &group);

	char key[LIBRARY_SORT_KEY_MAX];
	library_sort_key(value, key, sizeof(key));

	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_bind_text(stmt, 1, value, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, character, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 3, group);
	sqlite3_bind_text(stmt, 4, key, -1, SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_reset(stmt);
}



// A link row: the track at `path` is filed under `name` as well.
static void link_insert(sqlite3_stmt *stmt, const char *path, const char *name) {
	if (!stmt) {
		return;
	}
	sqlite3_reset(stmt);
	sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_reset(stmt);
}

// The lookup row for a tag, or for each name it splits into, with a link row
// per name.
static void lookup_names(sqlite3_stmt *lookup, sqlite3_stmt *link, const char *path, const char *value,
						 unsigned separators, char *const *exceptions, int exception_count) {
	char names[SPLIT_MAX][SPLIT_NAME_MAX];
	int count = split_names(value, separators, exceptions, exception_count, names, SPLIT_MAX);
	if (count == 0) {
		run_lookup(lookup, value);
		return;
	}
	for (int i = 0; i < count; i++) {
		run_lookup(lookup, names[i]);
		link_insert(link, path, names[i]);
	}
}

static void insert_track(const char *path, const char *filename, const song_metadata_t *tags, const struct stat *st) {
	if (!stmt_track) {
		return;
	}

	// A file with no title tag is filed under its own name, minus the
	// extension: an untitled track still has to be findable.
	char title[256];
	if (tags->title[0]) {
		snprintf(title, sizeof(title), "%s", tags->title);
	} else {
		snprintf(title, sizeof(title), "%s", filename);
		char *dot = strrchr(title, '.');
		if (dot) {
			*dot = '\0';
		}
	}

	const char *album_artist = tags->album_artist[0] ? tags->album_artist : tags->artist;

	char character[16];
	int group = 0;
	index_character(title, character, sizeof(character), &group);

	sqlite3_reset(stmt_track);
	sqlite3_clear_bindings(stmt_track);

	int column = 1;
	sqlite3_bind_int(stmt_track, column++, scan_id_base + scan_found + 1);		 // id
	sqlite3_bind_text(stmt_track, column++, path, -1, SQLITE_TRANSIENT);		 // path
	sqlite3_bind_text(stmt_track, column++, title, -1, SQLITE_TRANSIENT);		 // name
	sqlite3_bind_text(stmt_track, column++, tags->album, -1, SQLITE_TRANSIENT);	 // album
	sqlite3_bind_text(stmt_track, column++, tags->artist, -1, SQLITE_TRANSIENT); // artist
	sqlite3_bind_text(stmt_track, column++, tags->genre, -1, SQLITE_TRANSIENT);	 // genre
	sqlite3_bind_int(stmt_track, column++, tags->year);							 // year
	sqlite3_bind_int(stmt_track, column++, tags->track_number);					 // dis_id: track number
	sqlite3_bind_text(stmt_track, column++, character, -1, SQLITE_TRANSIENT);	 // character
	sqlite3_bind_int64(stmt_track, column++, (sqlite3_int64)st->st_size);		 // size

	// The two the quality badge is worked out from. Zero when the container
	// does not give them up cheaply -- see probe_stream_format().
	int rate = 0, bits = 0;
	bool alac = false;
	probe_stream_format(path, filename, &rate, &bits, &alac);
	sqlite3_bind_int(stmt_track, column++, rate);									 // sample_rate
	sqlite3_bind_int(stmt_track, column++, bits);									 // bit
	sqlite3_bind_int(stmt_track, column++, alac ? FORMAT_ALAC : format_of(filename)); // format
	sqlite3_bind_text(stmt_track, column++, album_artist, -1, SQLITE_TRANSIENT); // album_artist
	sqlite3_bind_int64(stmt_track, column++, (sqlite3_int64)st->st_ctime);		 // ctime
	sqlite3_bind_int64(stmt_track, column++, (sqlite3_int64)st->st_mtime);		 // mtime

	char key[LIBRARY_SORT_KEY_MAX];
	library_sort_key(title, key, sizeof(key));
	sqlite3_bind_text(stmt_track, column++, key, -1, SQLITE_TRANSIENT); // sortkey

	// A file with no disc tag is disc one. A record where the tagger wrote the
	// number on some files and not on others is the common case, and writing 0
	// for those would put them on a shelf of their own, ahead of the disc they
	// belong to.
	sqlite3_bind_int(stmt_track, column++, tags->disc_number > 0 ? tags->disc_number : 1); // disc

	char track_album_key[ALBUM_KEY_MAX];
	album_key(album_artist, path, track_album_key, sizeof(track_album_key));
	sqlite3_bind_text(stmt_track, column++, track_album_key, -1, SQLITE_TRANSIENT); // album_key

	int step_rc = sqlite3_step(stmt_track);
	if (step_rc != SQLITE_DONE) {
		note_damage(step_rc);
		fprintf(stderr, "library: insert failed: %s\n", sqlite3_errmsg(db));
	}
	sqlite3_reset(stmt_track);

	run_lookup(stmt_album, tags->album);
	lookup_names(stmt_artist, stmt_artist_link, path, tags->artist, rules.artist_separators, rules.exceptions,
				 rules.exception_count);
	lookup_names(stmt_genre, stmt_genre_link, path, tags->genre, rules.genre_separators, NULL, 0);
	lookup_names(stmt_album_artist, stmt_album_artist_link, path, album_artist, rules.album_artist_separators,
				 rules.exceptions, rules.exception_count);

	// And the album as this player lists it: its name and whose it is.
	if (stmt_album_group && tags->album[0]) {
		char akey[ALBUM_KEY_MAX];
		album_key(album_artist, path, akey, sizeof(akey));
		char skey[LIBRARY_SORT_KEY_MAX];
		library_sort_key(tags->album, skey, sizeof(skey));
		sqlite3_reset(stmt_album_group);
		sqlite3_clear_bindings(stmt_album_group);
		sqlite3_bind_text(stmt_album_group, 1, tags->album, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt_album_group, 2, akey, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt_album_group, 3, skey, -1, SQLITE_TRANSIENT);
		sqlite3_step(stmt_album_group);
		sqlite3_reset(stmt_album_group);
	}
}

// ---------------------------------------------------------------------------
// Join albums
//
// A record is its name and whose it is (album_key()), so a disc where a few
// tracks carry an album artist and the rest only "A feat. B" comes out as
// several albums of one name. With [library] join_albums on, the keys of one
// album name that meet in the same folder are one record: the tracks are
// re-keyed to whichever of those keys holds the most of them. Records of the
// same name in folders that share no key stay apart, as two "Greatest Hits" by
// two artists should.
//
// Off, every track keeps the key of its own tags, which is what insert_track()
// writes; this pass then only puts back keys an earlier pass had changed.
// ---------------------------------------------------------------------------

typedef struct {
	char key[ALBUM_KEY_MAX];
	int parent;
	int tracks;
} join_node_t;

typedef struct {
	sqlite3_int64 rowid;
	uint32_t folder;
	int node;
	char current[ALBUM_KEY_MAX];
} join_row_t;

static int join_find(join_node_t *nodes, int i) {
	while (nodes[i].parent != i) {
		nodes[i].parent = nodes[nodes[i].parent].parent;
		i = nodes[i].parent;
	}
	return i;
}

static int join_folder_cmp(const void *a, const void *b) {
	uint32_t x = ((const join_row_t *)a)->folder, y = ((const join_row_t *)b)->folder;
	return (x > y) - (x < y);
}

// The folder of `path` as album_key() hashes it: ASCII case dropped.
static uint32_t join_folder_hash(const char *path) {
	const char *slash = strrchr(path, '/');
	size_t len = slash ? (size_t)(slash - path) : strlen(path);
	uint32_t hash = 2166136261u;
	for (size_t i = 0; i < len; i++) {
		hash = (hash ^ (unsigned char)tolower((unsigned char)path[i])) * 16777619u;
	}
	return hash;
}

// Re-keys the tracks of one album name. With db_lock held. Returns how many
// rows took a new key, -1 when memory ran out.
static int album_join_one(const char *album) {
	sqlite3_stmt *read = NULL;
	if (sqlite3_prepare_v2(db, "SELECT rowid, album_artist, path, album_key FROM MEDIA_TABLE WHERE album = ?1", -1,
						   &read, NULL) != SQLITE_OK) {
		return 0;
	}
	sqlite3_bind_text(read, 1, album, -1, SQLITE_TRANSIENT);

	join_row_t *rows = NULL;
	join_node_t *nodes = NULL;
	int row_count = 0, row_cap = 0, node_count = 0, node_cap = 0;
	bool failed = false;
	while (!failed && sqlite3_step(read) == SQLITE_ROW) {
		const char *artist = (const char *)sqlite3_column_text(read, 1);
		const char *path = (const char *)sqlite3_column_text(read, 2);
		const char *current = (const char *)sqlite3_column_text(read, 3);
		char key[ALBUM_KEY_MAX];
		album_key(artist, path, key, sizeof(key));

		int node = -1;
		for (int i = 0; i < node_count && node < 0; i++) {
			if (strcmp(nodes[i].key, key) == 0) {
				node = i;
			}
		}
		if (node < 0) {
			if (node_count == node_cap) {
				int grown = node_cap ? node_cap * 2 : 8;
				join_node_t *bigger = realloc(nodes, (size_t)grown * sizeof(*bigger));
				if (!bigger) {
					failed = true;
					break;
				}
				nodes = bigger;
				node_cap = grown;
			}
			node = node_count++;
			snprintf(nodes[node].key, sizeof(nodes[node].key), "%s", key);
			nodes[node].parent = node;
			nodes[node].tracks = 0;
		}
		nodes[node].tracks++;

		if (row_count == row_cap) {
			int grown = row_cap ? row_cap * 2 : 32;
			join_row_t *bigger = realloc(rows, (size_t)grown * sizeof(*bigger));
			if (!bigger) {
				failed = true;
				break;
			}
			rows = bigger;
			row_cap = grown;
		}
		join_row_t *r = &rows[row_count++];
		r->rowid = sqlite3_column_int64(read, 0);
		r->folder = join_folder_hash(path ? path : "");
		r->node = node;
		snprintf(r->current, sizeof(r->current), "%s", current ? current : "");
	}
	sqlite3_finalize(read);

	int changed = failed ? -1 : 0;
	if (!failed && row_count > 0) {
		if (rules.join_albums && node_count > 1) {
			qsort(rows, (size_t)row_count, sizeof(rows[0]), join_folder_cmp);
			for (int i = 1; i < row_count; i++) {
				if (rows[i].folder == rows[i - 1].folder) {
					int a = join_find(nodes, rows[i].node), b = join_find(nodes, rows[i - 1].node);
					if (a != b) {
						nodes[b].parent = a;
					}
				}
			}
			// Each set's tracks counted at its root, then the key that holds the
			// most of them -- the smaller key on a tie, so the answer does not
			// depend on the order the rows came in.
			int *best = malloc((size_t)node_count * sizeof(*best));
			if (best) {
				for (int i = 0; i < node_count; i++) {
					best[i] = -1;
				}
				for (int i = 0; i < node_count; i++) {
					int root = join_find(nodes, i);
					int b = best[root];
					if (b < 0 || nodes[i].tracks > nodes[b].tracks ||
						(nodes[i].tracks == nodes[b].tracks && strcmp(nodes[i].key, nodes[b].key) < 0)) {
						best[root] = i;
					}
				}
				for (int i = 0; i < row_count; i++) {
					rows[i].node = best[join_find(nodes, rows[i].node)];
				}
				free(best);
			}
		}

		sqlite3_stmt *write = NULL;
		if (sqlite3_prepare_v2(db, "UPDATE MEDIA_TABLE SET album_key = ?1 WHERE rowid = ?2", -1, &write, NULL) ==
			SQLITE_OK) {
			for (int i = 0; i < row_count; i++) {
				const char *key = nodes[rows[i].node].key;
				if (strcmp(key, rows[i].current) == 0) {
					continue;
				}
				sqlite3_bind_text(write, 1, key, -1, SQLITE_TRANSIENT);
				sqlite3_bind_int64(write, 2, rows[i].rowid);
				sqlite3_step(write);
				sqlite3_reset(write);
				changed++;
			}
			sqlite3_finalize(write);
		}

		// The records of this name, as they are now.
		if (changed > 0) {
			sqlite3_stmt *stmt = NULL;
			if (sqlite3_prepare_v2(db, "DELETE FROM ALBUM_GROUP_TABLE WHERE album = ?1", -1, &stmt, NULL) ==
				SQLITE_OK) {
				sqlite3_bind_text(stmt, 1, album, -1, SQLITE_TRANSIENT);
				sqlite3_step(stmt);
				sqlite3_finalize(stmt);
			}
			if (sqlite3_prepare_v2(db,
								   "INSERT OR IGNORE INTO ALBUM_GROUP_TABLE(album, album_key, sortkey)"
								   " SELECT album, album_key, sortkey(album) FROM MEDIA_TABLE WHERE album = ?1"
								   " GROUP BY album_key",
								   -1, &stmt, NULL) == SQLITE_OK) {
				sqlite3_bind_text(stmt, 1, album, -1, SQLITE_TRANSIENT);
				sqlite3_step(stmt);
				sqlite3_finalize(stmt);
			}
		}
	}
	free(rows);
	free(nodes);
	return changed;
}

// The album names of the rows matching `sql` (no parameters), one each.
static char **album_names(const char *sql, int *count) {
	*count = 0;
	char **names = NULL;
	int capacity = 0;
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *name = (const char *)sqlite3_column_text(stmt, 0);
			if (name && name[0] && !names_add(&names, count, &capacity, name, strlen(name))) {
				break;
			}
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return names;
}

// Runs the pass over the named albums, a few at a time under the lock so a list
// opened meanwhile is not kept waiting for the whole of it. Returns how many
// tracks took a new key.
static int album_join(char **albums, int count) {
	int changed = 0;
	for (int i = 0; i < count && !scan_cancel; i += 20) {
		pthread_mutex_lock(&db_lock);
		if (db && exec("BEGIN")) {
			for (int j = i; j < count && j < i + 20; j++) {
				int n = album_join_one(albums[j]);
				if (n > 0) {
					changed += n;
				}
			}
			if (!exec("COMMIT")) {
				exec("ROLLBACK");
			}
		}
		pthread_mutex_unlock(&db_lock);
	}
	return changed;
}

// Keeps video out of the music library.
//
// .mp4 never reaches here (it is not in the list above), but a .m4a is the same
// container: renaming a film would have put it in Albums, Artists and Genres as
// a track, with its audio stream playable. Only what is inside the container
// can decide.
//
// Costs one container open -- a few reads, no decoding -- and only for this
// format.
static bool is_video_file(const char *path, const char *name) {
	if (!has_extension(name, ".m4a") && !has_extension(name, ".alac")) {
		return false;
	}
	// An .alac in a CAF is not an MP4 and answers "no video" here, which is
	// right: mp4_file_has_video() opens the container, and on something that is
	// not a readable MP4 there is nothing to keep out.
	return mp4_file_has_video(path);
}

// Indexes one file: read the tags, write the row, commit every so often. Split
// out of the walk, which has two phases with this sitting between them.
static void scan_indexed(const char *path, const char *name, const song_metadata_t *tags, const struct stat *st) {
	pthread_mutex_lock(&db_lock);
	if (db) {
		insert_track(path, name, tags, st);
		scan_found++;

		if (scan_found % SCAN_COMMIT_EVERY == 0) {
			bool ok = exec("COMMIT");
			// Give the pages back rather than letting the cache grow to
			// its limit and stay there for the rest of the scan.
			if (db) {
				sqlite3_db_release_memory(db);
			}
			ok = exec("BEGIN") && ok;
			// A card that has stopped taking writes -- the usual reason a
			// commit fails -- will not start again on the next thousand
			// files, and every one of those would be read for nothing.
			if (!ok) {
				// Whatever the commit did, the transaction must not be left
				// open: the next unrelated COMMIT anywhere would otherwise
				// write out half an abandoned scan.
				exec("ROLLBACK");
				fprintf(stderr, "library: database write failed, stopping the scan\n");
				scan_db_failed = true;
				scan_cancel = true;
			}
		}

		if (scan_found % SCAN_LOG_EVERY == 0) {
			long free_kb = mem_available_kb();
			printf("library: %d tracks, %s (%ld kB free)\n", scan_found, path, free_kb);
			fflush(stdout);
		}
	}
	pthread_mutex_unlock(&db_lock);
}

// The audio files claimed by the CUE sheets of the directory being walked.
//
// A disc must appear once. When a sheet claims a file, that file itself is not
// a track: it is the container the sheet cuts up. The list is reset for each
// directory and only consulted inside it, which is safe because the walk
// finishes a directory's files before descending (see scan_directory).
#define CUE_CLAIMED_MAX 16
static char cue_claimed[CUE_CLAIMED_MAX][512];
static int cue_claimed_count;

// Rows a sheet took out after they were indexed: their album, artist and genre
// names went in with them, and are pruned once the walk is over.
static bool cue_claimed_rows;

static void cue_claim(const char *audio_path) {
	if (cue_claimed_count >= CUE_CLAIMED_MAX) {
		return;
	}
	snprintf(cue_claimed[cue_claimed_count], sizeof(cue_claimed[0]), "%s", audio_path);
	cue_claimed_count++;

	// The sheet may have been read after the file it claims was already
	// indexed, so the row is removed as well as skipped later.
	pthread_mutex_lock(&db_lock);
	if (db) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db, "DELETE FROM MEDIA_TABLE WHERE path = ?1", -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, audio_path, -1, SQLITE_STATIC);
			sqlite3_step(stmt);
			sqlite3_finalize(stmt);
			// The counter on the progress page must lose the row too.
			int gone = sqlite3_changes(db);
			scan_found -= gone;
			cue_claimed_rows |= gone > 0;
			bump_generation(GEN_MEDIA);
		}
	}
	pthread_mutex_unlock(&db_lock);
}

static bool cue_claims(const char *path) {
	for (int i = 0; i < cue_claimed_count; i++) {
		if (strcmp(cue_claimed[i], path) == 0) {
			return true;
		}
	}
	return false;
}

// Raised whenever what a sheet is taken to cut up changes: a row written under
// another version is not believed, and its sheet is parsed again. Version 1: a
// sheet over files split one per track claims none of them.
#define CUE_STATE_VERSION 1

// What CUE_STATE remembers of a sheet: the audio file it cuts up, valid for as
// long as the sheet keeps the modification time and size it had. Only sheets
// that parsed are remembered: whether one does also depends on its audio being
// there, which can change while the sheet does not. With db_lock held.
static bool cue_state_get(const char *path, const struct stat *st, char *audio, size_t size) {
	sqlite3_stmt *stmt = NULL;
	bool found = false;
	if (db && sqlite3_prepare_v2(db,
								 "SELECT audio FROM CUE_STATE WHERE path = ?1 AND mtime = ?2 AND size = ?3"
								 " AND version = ?4",
								 -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 2, (sqlite3_int64)st->st_mtime);
		sqlite3_bind_int64(stmt, 3, (sqlite3_int64)st->st_size);
		sqlite3_bind_int(stmt, 4, CUE_STATE_VERSION);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *text = (const char *)sqlite3_column_text(stmt, 0);
			snprintf(audio, size, "%s", text ? text : "");
			found = true;
		}
		sqlite3_finalize(stmt);
	}
	return found;
}

// With db_lock held.
static void cue_state_put(const char *path, const struct stat *st, const char *audio) {
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db,
								 "INSERT OR REPLACE INTO CUE_STATE(path, mtime, size, audio, version)"
								 " VALUES(?1, ?2, ?3, ?4, ?5)",
								 -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 2, (sqlite3_int64)st->st_mtime);
		sqlite3_bind_int64(stmt, 3, (sqlite3_int64)st->st_size);
		sqlite3_bind_text(stmt, 4, audio, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt, 5, CUE_STATE_VERSION);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
}

// What a folder holds, as FOLDER_TABLE keeps it: how many tracks and
// subfolders, and the sum of a hash of each name. A sum, so the order readdir
// gives them in does not matter; any name added, taken away or renamed moves it.
// The scan and the walk of Detect changes feed it the same entries: every
// subfolder and every playable name, before any other filter.
typedef struct {
	int entries;
	uint64_t names;
} folder_sig_t;

static void folder_sig_add(folder_sig_t *sig, const char *name, bool is_dir) {
	uint64_t hash = 14695981039346656037ull;
	for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
		hash = (hash ^ (unsigned char)tolower(*p)) * 1099511628211ull;
	}
	if (is_dir) {
		hash = (hash ^ '/') * 1099511628211ull;
	}
	sig->names += hash;
	sig->entries++;
}

// With db_lock held.
static void folder_mark(const char *path, time_t mtime, const folder_sig_t *sig) {
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO FOLDER_TABLE(path, mtime, files, names) VALUES(?1, ?2, ?3, ?4)",
								 -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 2, (sqlite3_int64)mtime);
		sqlite3_bind_int(stmt, 3, sig->entries);
		sqlite3_bind_int64(stmt, 4, (sqlite3_int64)sig->names);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
}

// Whether FOLDER_TABLE has `path` holding the same names. The folder's own
// modification time is not compared: it also moves for a cover, a log or a
// hidden file, none of which is the walk's business, and a file replaced under
// the same name is the retag check's (update_retagged).
static bool folder_unchanged(const char *path, const folder_sig_t *sig) {
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	bool same = false;
	if (db && sqlite3_prepare_v2(db, "SELECT files, names FROM FOLDER_TABLE WHERE path = ?1", -1, &stmt, NULL) ==
				  SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			same = sqlite3_column_int(stmt, 0) == sig->entries && (uint64_t)sqlite3_column_int64(stmt, 1) == sig->names;
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return same;
}

// A whole disc in one file, split by a CUE sheet.
//
// Each TRACK becomes its own row, pointing at a virtual path -- the sheet plus
// a track number -- that the decoder turns back into a window onto the audio
// file. Without this the file is one row and one hour-long track.
//
// Returns false when the sheet is unusable, and the caller then treats it as
// an ordinary file it cannot index.
static bool scan_cue_sheet(const char *path, const struct stat *st) {
	// A sheet is thirty-five kilobytes. This runs several frames deep in a
	// directory walk, on the scan thread, so it goes on the heap.
	cue_sheet_t *cue = malloc(sizeof(*cue));
	if (!cue) {
		return false;
	}
	// A sheet over files already split one per track leaves them to be
	// indexed as the files they are.
	if (!cue_parse(path, cue) || cue->split) {
		free(cue);
		return false;
	}

	for (int i = 0; i < cue->track_count; i++) {
		const cue_track_t *t = &cue->tracks[i];

		char vpath[600];
		cue_virtual_path(path, t->number, vpath, sizeof(vpath));

		song_metadata_t tags;
		memset(&tags, 0, sizeof(tags));
		snprintf(tags.title, sizeof(tags.title), "%s", t->title);
		snprintf(tags.artist, sizeof(tags.artist), "%s", t->performer[0] ? t->performer : cue->performer);
		snprintf(tags.album, sizeof(tags.album), "%s", cue->album);
		snprintf(tags.album_artist, sizeof(tags.album_artist), "%s", cue->performer);
		snprintf(tags.genre, sizeof(tags.genre), "%s", cue->genre);
		tags.year = cue->year;
		tags.track_number = t->number;
		tags.has_tags = tags.title[0] || tags.artist[0] || tags.album[0];

		// The name shown when the sheet gave no title: better a track number
		// than the sheet's file name repeated for every entry.
		char shown[64];
		if (!tags.title[0]) {
			snprintf(shown, sizeof(shown), "%02d", t->number);
			snprintf(tags.title, sizeof(tags.title), "%s", shown);
		}

		scan_indexed(vpath, tags.title, &tags, st);
	}

	if (cue->track_count > 0) {
		cue_claim(cue->audio_path);
	}
	if (cue_is_sheet(path) && cue->track_count > 0) {
		pthread_mutex_lock(&db_lock);
		cue_state_put(path, st, cue->audio_path);
		pthread_mutex_unlock(&db_lock);
	}
	printf("library: %s holds %d tracks\n", path, cue->track_count);
	bool indexed = cue->track_count > 0;
	free(cue);
	return indexed;
}

static void scan_one_file(const char *child, const char *name, const struct stat *st) {
	// Before the file is touched, not after: what is wanted is the name of the
	// file the player was on when it died, and by then no code here runs.
	crumb_set(child);
	if (scan_log_files) {
		printf("library: reading %s\n", child);
		fflush(stdout);
	}

	if (cue_is_sheet(name)) {
		scan_cue_sheet(child, st);
		return;
	}
	if (cue_claims(child)) {
		return; // a sheet in this folder already indexed it, track by track
	}

	// A WAV can hold its own track list, in the RIFF markers a vinyl rip
	// leaves behind. It is read the same way a sheet is, with the file
	// standing in for the sheet; when there are no markers this returns false
	// and the file is indexed whole. A .cue beside it wins: it was checked
	// above and has already claimed the audio.
	if (cue_wav_has_markers(name) && scan_cue_sheet(child, st)) {
		return;
	}

	song_metadata_t tags;
	metadata_read(child, &tags);
	scan_indexed(child, name, &tags, st);
}

// One directory open at a time.
//
// An open DIR is not just a pointer: the C library allocates it a read buffer,
// 32 KB here. Recursing from inside the readdir() loop would hold one of those,
// and one file descriptor, per level of nesting -- over four hundred kilobytes
// at the twelve-level limit.
//
// So the walk is in two passes: read the directory to the end (indexing files
// as they come, which costs no memory) keeping only the names of the
// subdirectories, close it, and only then descend. One directory is open at any
// depth, and what is carried down is names, a few dozen bytes each.
// The folder filter: the chosen folders at the root of the card, one name per
// line in scan_folders.txt beside device_config.ini, read when a scan starts.
// Without that file the '/'-separated [library] scan_folders key is read.
#define SCAN_FOLDERS_FILE "scan_folders.txt"
static char **scan_folder_names;
static int scan_folder_count;

char **library_scan_folders(int *count) {
	char **names = NULL;
	if (lines_read(SCAN_FOLDERS_FILE, &names, count)) {
		return names;
	}

	int capacity = 0;
	const char *p = config_get("library", "scan_folders", "");
	while (p && *p) {
		const char *end = strchr(p, '/');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		if (len > 0 && !names_add(&names, count, &capacity, p, len)) {
			break;
		}
		p = end ? end + 1 : NULL;
	}
	return names;
}

void library_scan_folders_free(char **names, int count) { names_free(names, count); }

void library_scan_folders_set(const char *const *names, int count) {
	// An empty file is the whole card, the same as no file, and keeps the
	// config key from being read in its place.
	if (!lines_write(SCAN_FOLDERS_FILE, names, count)) {
		return;
	}
	if (config_get("library", "scan_folders", "")[0]) {
		config_set("library", "scan_folders", "");
		config_save();
	}
}

static void scan_folders_load(void) {
	library_scan_folders_free(scan_folder_names, scan_folder_count);
	scan_folder_names = library_scan_folders(&scan_folder_count);
}

// With a filter, only the chosen folders at the root of the card are read.
static bool scan_folder_chosen(const char *name) {
	for (int i = 0; i < scan_folder_count; i++) {
		if (strcasecmp(name, scan_folder_names[i]) == 0) {
			return true;
		}
	}
	return false;
}

// Whether a directory entry is a subfolder the walk goes into, by the same
// rules as in scan_directory() -- or, with `by_type`, as in update_folder(),
// which believes the entry's d_type and asks the card only when it has none.
static bool walked_subdir(const char *path, const struct dirent *de, int depth, bool by_type) {
	const char *name = de->d_name;
	if (name[0] == '.' || playlist_is_junk_name(name)) {
		return false;
	}
	char child[512];
	if (snprintf(child, sizeof(child), "%s/%s", path, name) >= (int)sizeof(child)) {
		if (de->d_type != DT_REG) {
			printf("library: %s/%s is a path too long to keep, skipped\n", path, name);
		}
		return false;
	}
	bool is_dir = by_type && de->d_type == DT_DIR;
	if (!is_dir && !(by_type && de->d_type == DT_REG)) {
		struct stat st;
		is_dir = lstat(child, &st) == 0 && S_ISDIR(st.st_mode);
	}
	if (!is_dir) {
		return false;
	}
	if (depth == 0 && (strcasecmp(name, AUDIOBOOKDB_FOLDER) == 0 || strcasecmp(name, PODCASTDL_FOLDER) == 0)) {
		return false;
	}
	return !(depth == 0 && scan_folder_count > 0 && !scan_folder_chosen(name));
}

// The next batch of a directory's subfolders: the first `skip` of them in
// readdir order are passed over. True when there are more after the batch.
static bool collect_subdirs(const char *path, int depth, int skip, bool by_type, namelist_t *out) {
	namelist_init(out);
	DIR *dir = opendir(path);
	if (!dir) {
		return false;
	}
	bool more = false;
	int seen = 0;
	struct dirent *de;
	while (!scan_cancel && (de = readdir(dir)) != NULL) {
		if (!walked_subdir(path, de, depth, by_type) || seen++ < skip) {
			continue;
		}
		if (!namelist_add(out, de->d_name)) {
			more = true;
			break;
		}
	}
	closedir(dir);
	return more;
}

static void scan_directory(const char *path, int depth) {
	if (scan_cancel) {
		return;
	}
	if (depth > SCAN_MAX_DEPTH) {
		printf("library: %s is more than %d folders deep, skipped\n", path, SCAN_MAX_DEPTH);
		return;
	}

	struct stat dir_st;
	if (stat(path, &dir_st) != 0) {
		return;
	}
	DIR *dir = opendir(path);
	if (!dir) {
		return;
	}

	set_scan_folder(path);
	cue_claimed_count = 0; // the claims below belong to this directory only
	folder_sig_t sig = {0, 0};

	namelist_t subdirs;
	namelist_init(&subdirs);
	bool more_subdirs = false;

	// WAVs are held back to a second pass. A WAV can carry its own track list
	// in its markers, and a .cue in the same folder overrules it -- but
	// readdir returns names in the order the filesystem kept them, and a
	// ripper writes the audio before the sheet, so the sheet is usually the
	// later of the two. Deciding while the directory is still being read would
	// index the disc twice.
	namelist_t wavs;
	namelist_init(&wavs);

	struct dirent *de;
	while (!scan_cancel && (de = readdir(dir)) != NULL) {
		// ".", "..", the player's own hidden folders -- and the folders a
		// desktop leaves behind, which do not begin with a dot.
		if (de->d_name[0] == '.' || playlist_is_junk_name(de->d_name)) {
			continue;
		}

		char child[512];
		if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >= (int)sizeof(child)) {
			// Every path the player keeps is at most this long. A track or a
			// folder left out is said in the log, so it can be told from one
			// never read.
			if (de->d_type != DT_REG || is_playable(de->d_name)) {
				printf("library: %s/%s is a path too long to keep, skipped\n", path, de->d_name);
			}
			continue;
		}

		// lstat() rather than stat(): a symlink pointing at an ancestor
		// directory would re-read the same branch at every level down to the
		// twelfth -- not an infinite loop, but an explosion of work and
		// duplicate rows. Symlinks to files are followed, which is harmless:
		// the stat() below resolves them.
		struct stat st;
		if (lstat(child, &st) != 0) {
			continue;
		}
		if (S_ISLNK(st.st_mode)) {
			if (stat(child, &st) != 0 || !S_ISREG(st.st_mode)) {
				continue; // broken, or points at a directory: leave it alone
			}
		}

		if (S_ISDIR(st.st_mode)) {
			folder_sig_add(&sig, de->d_name, true);
			// The audiobooks have an index of their own (audiobookdb.h), and the
			// downloaded podcasts a page of their own (podcastdl.h).
			if (depth == 0 && (strcasecmp(de->d_name, AUDIOBOOKDB_FOLDER) == 0 ||
							   strcasecmp(de->d_name, PODCASTDL_FOLDER) == 0)) {
				continue;
			}
			if (depth == 0 && scan_folder_count > 0 && !scan_folder_chosen(de->d_name)) {
				continue;
			}
			// Once one is left out, so are the rest: the next batch starts
			// after the ones held here, counted in readdir order.
			if (!more_subdirs && !namelist_add(&subdirs, de->d_name)) {
				more_subdirs = true;
			}
			continue;
		}

		if (!S_ISREG(st.st_mode) || !is_playable(de->d_name)) {
			continue;
		}
		folder_sig_add(&sig, de->d_name, false);
		if (depth == 0 && scan_folder_count > 0) {
			continue; // loose files at the root belong to no chosen folder
		}

		if (is_video_file(child, de->d_name)) {
			printf("library: %s is a video; it does not go into the music library\n", child);
			continue;
		}

		// Held back unless the list is full, in which case indexing it now is
		// better than dropping it.
		if (cue_wav_has_markers(de->d_name) && namelist_add(&wavs, de->d_name)) {
			continue;
		}

		scan_one_file(child, de->d_name, &st);
	}

	// The 32 KB buffer goes back to the system before descending a level.
	closedir(dir);

	// Every sheet in this directory has claimed its audio by now.
	for (int i = 0; i < wavs.count && !scan_cancel; i++) {
		char child[512];
		if (snprintf(child, sizeof(child), "%s/%s", path, wavs.names[i]) >= (int)sizeof(child)) {
			continue;
		}
		struct stat st;
		if (stat(child, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		scan_one_file(child, wavs.names[i], &st);
	}
	namelist_free(&wavs);

	// Every file of this folder is in: Detect changes may skip it from now on
	// for as long as it looks the same.
	if (!scan_cancel) {
		pthread_mutex_lock(&db_lock);
		folder_mark(path, dir_st.st_mtime, &sig);
		pthread_mutex_unlock(&db_lock);
	}

	// The subfolders, SCAN_MAX_SUBDIRS at a time: past the first batch the
	// directory is read again for the next one.
	int walked = 0;
	for (;;) {
		for (int i = 0; i < subdirs.count && !scan_cancel; i++) {
			char child[512];
			if (snprintf(child, sizeof(child), "%s/%s", path, subdirs.names[i]) >= (int)sizeof(child)) {
				continue;
			}
			scan_directory(child, depth + 1);
			set_scan_folder(path); // back to this level for the progress display
		}
		walked += subdirs.count;
		namelist_free(&subdirs);
		if (!more_subdirs || scan_cancel) {
			break;
		}
		more_subdirs = collect_subdirs(path, depth, walked, false, &subdirs);
		if (subdirs.count == 0) {
			break;
		}
	}
}

static void prune_links(void);
static void prune_names(void);

// A scan: the tables emptied, the card walked, every file read.
static void full_scan_run(void) {
	if (scan_folder_count > 0) {
		printf("library: scanning %s, %d chosen folder(s)\n", scan_root, scan_folder_count);
	} else {
		printf("library: scanning %s\n", scan_root);
	}

	pthread_mutex_lock(&db_lock);
	exec("DELETE FROM MEDIA_TABLE");
	exec("DELETE FROM ALBUM_TABLE");
	exec("DELETE FROM ALBUM_GROUP_TABLE");
	exec("DELETE FROM ARTIST_TABLE");
	exec("DELETE FROM ALBUM_ARTIST_TABLE");
	exec("DELETE FROM GENRE_TABLE");
	exec("DELETE FROM ARTIST_LINK");
	exec("DELETE FROM ALBUM_ARTIST_LINK");
	exec("DELETE FROM GENRE_LINK");
	exec("DELETE FROM FOLDER_TABLE");
	exec("DELETE FROM CUE_STATE");
	// Emptying the tables restarts the row ids at one, so anything holding
	// them is now pointing at other people's tracks.
	bump_generation(GEN_MEDIA);
	// Without the statements every insert is a silent no-op, and the scan
	// would walk the whole card to produce an empty library.
	if (!prepare_statements() || !exec("BEGIN")) {
		fprintf(stderr, "library: cannot start the scan, the database is not writable\n");
		scan_db_failed = true;
		scan_cancel = true;
	}
	pthread_mutex_unlock(&db_lock);

	if (!scan_cancel) {
		scan_directory(scan_root, 0);
	}

	pthread_mutex_lock(&db_lock);
	if (scan_db_failed) {
		exec("ROLLBACK"); // no-op when the BEGIN itself never took
	} else {
		exec("COMMIT");
	}
	finalize_statements();

	if (cue_claimed_rows && !scan_db_failed) {
		prune_links();
		prune_names();
	}
	cue_claimed_rows = false;

	// One row so the count is readable without walking the table.
	exec("DELETE FROM COUNT_TABLE");
	char sql[128];
	snprintf(sql, sizeof(sql), "INSERT INTO COUNT_TABLE(cn) VALUES(%d)", scan_found);
	exec(sql);

	// Nothing is going to read the index for a while; give the cache back
	// rather than sitting on it until the player is killed for it.
	if (db) {
		sqlite3_db_release_memory(db);
	}
	pthread_mutex_unlock(&db_lock);

	// insert_track() wrote every track with the key of its own tags.
	if (rules.join_albums && !scan_db_failed) {
		int count = 0;
		char **albums = album_names("SELECT DISTINCT album FROM MEDIA_TABLE WHERE album <> ''", &count);
		int joined = album_join(albums, count);
		names_free(albums, count);
		printf("library: %d tracks joined into the album of their folder\n", joined);
	}

	pthread_mutex_lock(&db_lock);
	links_check();
	if (!scan_cancel && !scan_db_failed) {
		organize_state_put(&rules);
	}
	bump_generation(GEN_MEDIA); // and again at the end, so a list opened mid-scan reloads
	pthread_mutex_unlock(&db_lock);

	// Every row a scan writes carries its sort key, and a scan starts by
	// emptying the tables, so after one there is nothing left to fill in --
	// whatever the database looked like before.
	if (!scan_db_failed) {
		build_sort_indexes(); // no-op unless the upgrade never got to them
		sortkeys_ready = true;
	}

	printf("library: scan finished, %d tracks%s\n", scan_found,
		   scan_db_failed ? " (database error)" : scan_cancel ? " (stopped early)" : "");
}

// ---------------------------------------------------------------------------
// Detect changes: what came, what went and what changed
//
// The folders chosen for the scan are walked again, and each one is compared
// with what FOLDER_TABLE says it held (folder_sig_t). Only a folder that no
// longer matches is looked into: its names against the index's rows for it,
// its subfolders against the ones the index has tracks in. That gives
//
//   - what went: the rows of files, and of whole folders, no longer there;
//   - what came: the files the index has no rows for, read afterwards -- which
//     also lets the listener be told how many there are before the slow part;
//   - what changed: with [library] detect_retagged on, every indexed file whose
//     modification time is not the one stored with its rows, read again.
//
// A folder is written into FOLDER_TABLE only once the files it was found to
// need are in the index, so a run cut short looks into it again next time.
// ---------------------------------------------------------------------------

typedef struct {
	sqlite3_int64 *ids;
	int count;
	int capacity;
} idlist_t;

typedef struct {
	char *path;
	time_t mtime;
	folder_sig_t sig;
} folder_note_t;

typedef struct {
	namelist_t found;		 // files the index has no rows for
	namelist_t reread;		 // indexed files changed since: rows replaced when read
	idlist_t gone;			 // rows whose file is not in its folder any more
	namelist_t gone_folders; // folders no longer there, with every row under them
	folder_note_t *notes;	 // the folders to write into FOLDER_TABLE afterwards
	int note_count;
	int note_capacity;
	bool full;		   // found and reread hit SCAN_MAX_SUBDIRS: walk again after
	bool root_failed;  // the card itself would not be read
	bool lists_failed; // memory ran out: nothing is taken out this time
} walk_t;

static sqlite3_stmt *stmt_under_media;
static sqlite3_stmt *stmt_under_folders;

static bool idlist_add(idlist_t *l, sqlite3_int64 id) {
	if (l->count == l->capacity) {
		int grown = l->capacity ? l->capacity * 2 : 256;
		sqlite3_int64 *bigger = realloc(l->ids, (size_t)grown * sizeof(*bigger));
		if (!bigger) {
			return false;
		}
		l->ids = bigger;
		l->capacity = grown;
	}
	l->ids[l->count++] = id;
	return true;
}

static void walk_init(walk_t *w) {
	memset(w, 0, sizeof(*w));
	namelist_init(&w->found);
	namelist_init(&w->reread);
	namelist_init(&w->gone_folders);
}

static void walk_free(walk_t *w) {
	namelist_free(&w->found);
	namelist_free(&w->reread);
	namelist_free(&w->gone_folders);
	free(w->gone.ids);
	for (int i = 0; i < w->note_count; i++) {
		free(w->notes[i].path);
	}
	free(w->notes);
	memset(w, 0, sizeof(*w));
}

static int walk_pending(const walk_t *w) { return w->found.count + w->reread.count; }

static void walk_note(walk_t *w, const char *path, time_t mtime, const folder_sig_t *sig) {
	if (w->note_count == w->note_capacity) {
		int grown = w->note_capacity ? w->note_capacity * 2 : 64;
		folder_note_t *bigger = realloc(w->notes, (size_t)grown * sizeof(*bigger));
		if (!bigger) {
			return; // looked into again next time, which costs time and nothing else
		}
		w->notes = bigger;
		w->note_capacity = grown;
	}
	char *copy = strdup(path);
	if (!copy) {
		return;
	}
	w->notes[w->note_count].path = copy;
	w->notes[w->note_count].mtime = mtime;
	w->notes[w->note_count].sig = *sig;
	w->note_count++;
}

static void finalize_known(void) {
	sqlite3_finalize(stmt_under_media);
	sqlite3_finalize(stmt_under_folders);
	stmt_under_media = stmt_under_folders = NULL;
}

// Everything indexed under a folder, as a range of media_path_idx: "dir/" up
// to "dir0", '0' being the character after '/'. Only the row id and the path
// are read, which that index holds without going to the table.
static bool prepare_known(void) {
	finalize_known();
	return sqlite3_prepare_v2(db, "SELECT rowid, path FROM MEDIA_TABLE WHERE path >= ?1 AND path < ?2", -1,
							  &stmt_under_media, NULL) == SQLITE_OK &&
		   sqlite3_prepare_v2(db, "SELECT path FROM FOLDER_TABLE WHERE path >= ?1 AND path < ?2", -1, &stmt_under_folders,
							  NULL) == SQLITE_OK;
}

static void bind_under(sqlite3_stmt *stmt, const char *folder) {
	char low[600], high[600];
	snprintf(low, sizeof(low), "%s/", folder);
	snprintf(high, sizeof(high), "%s0", folder);
	sqlite3_reset(stmt);
	sqlite3_bind_text(stmt, 1, low, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, high, -1, SQLITE_TRANSIENT);
}

// Case-blind, as the card is: a sheet's FILE line need not spell the name the
// way the folder does, and the index matches paths the same way.
static bool namelist_has(const namelist_t *l, const char *name) {
	for (int i = 0; i < l->count; i++) {
		if (strcasecmp(l->names[i], name) == 0) {
			return true;
		}
	}
	return false;
}

static int name_cmp(const void *a, const void *b) {
	return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

// Whether `name` is in `names`, sorted by name_cmp.
static bool sorted_has(char *const *names, int count, const char *name) {
	return count > 0 && bsearch(&name, names, (size_t)count, sizeof(*names), name_cmp) != NULL;
}

// A row directly in the folder being compared: the file it comes from, which
// for a sheet's track is the sheet ("x.cue?track=3" comes from "x.cue").
typedef struct {
	sqlite3_int64 rowid;
	char *file;
	bool seen;
} folder_row_t;

static int folder_row_cmp(const void *a, const void *b) {
	return strcasecmp(((const folder_row_t *)a)->file, ((const folder_row_t *)b)->file);
}

// The first row of `file` in `rows`, sorted by folder_row_cmp; -1 when none.
static int folder_row_find(folder_row_t *rows, int count, const char *file) {
	int lo = 0, hi = count - 1, hit = -1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		int c = strcasecmp(rows[mid].file, file);
		if (c == 0) {
			hit = mid;
			hi = mid - 1;
		} else if (c < 0) {
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	return hit;
}

// The audio a sheet cuts up, from CUE_STATE when the sheet is the one it
// remembers and from the sheet itself otherwise. Empty when it is unusable;
// `split`, when not NULL, says whether that is because the sheet is over files
// split one per track.
static void sheet_audio(const char *sheet, char *audio, size_t size, bool *split) {
	audio[0] = '\0';
	if (split) {
		*split = false;
	}
	struct stat st;
	if (stat(sheet, &st) != 0) {
		return;
	}
	pthread_mutex_lock(&db_lock);
	bool known_sheet = cue_state_get(sheet, &st, audio, size);
	pthread_mutex_unlock(&db_lock);
	if (known_sheet && access(audio, F_OK) == 0) {
		return;
	}
	audio[0] = '\0';
	cue_sheet_t *cue = malloc(sizeof(*cue));
	bool parsed = cue && cue_parse(sheet, cue);
	if (parsed && cue->split && split) {
		*split = true;
	}
	if (parsed && cue->track_count > 0 && !cue->split) {
		snprintf(audio, size, "%s", cue->audio_path);
		pthread_mutex_lock(&db_lock);
		cue_state_put(sheet, &st, audio);
		pthread_mutex_unlock(&db_lock);
	}
	free(cue);
}

// One folder that no longer matches its FOLDER_TABLE row: `files` are its
// playable names as the walk reads them, `dirs` its subfolders -- every one,
// or with `dirs_listed` false only the first SCAN_MAX_SUBDIRS, in which case a
// subfolder the index knows and the list does not is asked of the card. Without
// `own_files` -- the root of a card read only for its chosen folders -- the
// files directly in it are none of the walk's business.
//
// Two rules more than the folder's own names, so that nothing is collected
// that a scan would leave out, or it would be announced as new every time the
// card came back: the audio a sheet claims is the sheet's tracks and never a
// file of its own, and a sheet that cannot be read is not new music.
//
// Returns whether the folder was compared in full. Memory running out, or a
// subfolder name too long to hold, leaves part of it unjudged -- nothing is
// taken out there, and the folder is looked into again next time.
static bool update_compare(const char *path, bool own_files, const namelist_t *files, const namelist_t *dirs,
						   bool dirs_listed, walk_t *w) {
	size_t plen = strlen(path);
	folder_row_t *rows = NULL;
	int row_count = 0, row_cap = 0;
	// The subfolders the index has tracks or a FOLDER_TABLE row in and the
	// list does not: gone, or with the list cut short, to be asked of the card.
	namelist_t missing_dirs;
	namelist_init(&missing_dirs);
	char last_dir[512] = "";
	bool rows_complete = true; // every row directly in the folder, and every claim
	bool dirs_complete = true; // every subfolder the index has tracks in

	// Sorted, so that each subfolder the index names is a binary search and
	// not a walk of the list -- this runs with db_lock held.
	char **sorted_dirs = NULL;
	if (dirs->count > 0) {
		sorted_dirs = malloc((size_t)dirs->count * sizeof(*sorted_dirs));
		if (sorted_dirs) {
			memcpy(sorted_dirs, dirs->names, (size_t)dirs->count * sizeof(*sorted_dirs));
			qsort(sorted_dirs, (size_t)dirs->count, sizeof(*sorted_dirs), name_cmp);
		} else {
			dirs_complete = false;
		}
	}

	pthread_mutex_lock(&db_lock);
	if (db && stmt_under_media) {
		bind_under(stmt_under_media, path);
		while (sqlite3_step(stmt_under_media) == SQLITE_ROW) {
			const char *row_path = (const char *)sqlite3_column_text(stmt_under_media, 1);
			if (!row_path || strlen(row_path) <= plen + 1) {
				continue;
			}
			const char *rest = row_path + plen + 1;
			const char *slash = strchr(rest, '/');
			if (slash) {
				size_t n = (size_t)(slash - rest);
				if (n >= sizeof(last_dir)) {
					dirs_complete = false;
				} else if (strncasecmp(last_dir, rest, n) != 0 || last_dir[n] != '\0') {
					memcpy(last_dir, rest, n);
					last_dir[n] = '\0';
					if (dirs_complete && !sorted_has(sorted_dirs, dirs->count, last_dir) &&
						!namelist_push(&missing_dirs, last_dir, INT_MAX)) {
						dirs_complete = false;
					}
				}
				continue;
			}
			if (!own_files) {
				continue;
			}
			if (row_count == row_cap) {
				int grown = row_cap ? row_cap * 2 : 32;
				folder_row_t *bigger = realloc(rows, (size_t)grown * sizeof(*bigger));
				if (!bigger) {
					rows_complete = false;
					break;
				}
				rows = bigger;
				row_cap = grown;
			}
			const char *track = strstr(rest, "?track=");
			size_t n = track ? (size_t)(track - rest) : strlen(rest);
			char *file = malloc(n + 1);
			if (!file) {
				rows_complete = false;
				break;
			}
			memcpy(file, rest, n);
			file[n] = '\0';
			rows[row_count].rowid = sqlite3_column_int64(stmt_under_media, 0);
			rows[row_count].file = file;
			rows[row_count].seen = false;
			row_count++;
		}
		sqlite3_reset(stmt_under_media);
	}
	pthread_mutex_unlock(&db_lock);
	if (row_count > 1) {
		qsort(rows, (size_t)row_count, sizeof(rows[0]), folder_row_cmp);
	}

	// What the sheets here claim, by full path; and the sheets over files
	// split one per track, whose rows, if the index has any, are taken out:
	// the files stand for themselves.
	namelist_t claimed, split_sheets;
	namelist_init(&claimed);
	namelist_init(&split_sheets);
	for (int i = 0; i < files->count && !scan_cancel; i++) {
		if (!cue_is_sheet(files->names[i])) {
			continue;
		}
		char sheet[600], audio[600];
		snprintf(sheet, sizeof(sheet), "%s/%s", path, files->names[i]);
		bool split = false;
		sheet_audio(sheet, audio, sizeof(audio), &split);
		if (audio[0] && !namelist_add(&claimed, audio)) {
			rows_complete = false;
		}
		if (split && !namelist_add(&split_sheets, files->names[i])) {
			rows_complete = false;
		}
	}

	for (int i = 0; i < files->count && rows_complete && !scan_cancel; i++) {
		const char *name = files->names[i];
		int hit = folder_row_find(rows, row_count, name);
		if (hit >= 0) {
			bool keep = !namelist_has(&split_sheets, name);
			for (int j = hit; keep && j < row_count && strcasecmp(rows[j].file, name) == 0; j++) {
				rows[j].seen = true;
			}
			continue;
		}
		char child[600];
		if (snprintf(child, sizeof(child), "%s/%s", path, name) >= (int)sizeof(child)) {
			continue;
		}
		if (cue_is_sheet(name)) {
			char audio[600];
			sheet_audio(child, audio, sizeof(audio), NULL);
			if (!audio[0]) {
				continue;
			}
		} else if (namelist_has(&claimed, child) || is_video_file(child, name)) {
			continue;
		}
		if (walk_pending(w) >= SCAN_MAX_SUBDIRS || !namelist_add(&w->found, child)) {
			w->full = true;
		}
	}
	namelist_free(&claimed);
	namelist_free(&split_sheets);

	// A full list leaves the rest of this folder's names unread, and an unread
	// name is not a missing file.
	bool judged = !w->full && !scan_cancel;
	for (int i = 0; judged && rows_complete && i < row_count; i++) {
		if (!rows[i].seen && !idlist_add(&w->gone, rows[i].rowid)) {
			w->lists_failed = true;
		}
	}
	if (judged && dirs_complete) {
		pthread_mutex_lock(&db_lock);
		if (db && stmt_under_folders) {
			bind_under(stmt_under_folders, path);
			while (sqlite3_step(stmt_under_folders) == SQLITE_ROW) {
				const char *folder = (const char *)sqlite3_column_text(stmt_under_folders, 0);
				const char *rest = folder && strlen(folder) > plen + 1 ? folder + plen + 1 : NULL;
				if (rest && !strchr(rest, '/') && !sorted_has(sorted_dirs, dirs->count, rest) &&
					!namelist_push(&missing_dirs, rest, INT_MAX)) {
					dirs_complete = false;
				}
			}
			sqlite3_reset(stmt_under_folders);
		}
		pthread_mutex_unlock(&db_lock);
	}
	if (judged && dirs_complete) {
		// Subfolders the index has tracks or a FOLDER_TABLE row in, and the
		// card no longer has. Only a name the card says is not there is gone:
		// one it cannot answer for is left for the next run. A folder with
		// both tracks and a FOLDER_TABLE row is in the list twice.
		if (missing_dirs.count > 1) {
			qsort(missing_dirs.names, (size_t)missing_dirs.count, sizeof(*missing_dirs.names), name_cmp);
		}
		for (int i = 0; i < missing_dirs.count; i++) {
			if (i > 0 && strcasecmp(missing_dirs.names[i], missing_dirs.names[i - 1]) == 0) {
				continue;
			}
			char gone[600];
			snprintf(gone, sizeof(gone), "%s/%s", path, missing_dirs.names[i]);
			struct stat st;
			if (!dirs_listed && (lstat(gone, &st) == 0 || errno != ENOENT)) {
				continue;
			}
			if (!namelist_add(&w->gone_folders, gone)) {
				w->lists_failed = true;
			}
		}
	}

	for (int i = 0; i < row_count; i++) {
		free(rows[i].file);
	}
	free(rows);
	free(sorted_dirs);
	namelist_free(&missing_dirs);
	return rows_complete && dirs_complete;
}

// Walks `path` with a scan's rules, comparing the folders that changed.
static void update_folder(const char *path, int depth, walk_t *w) {
	if (scan_cancel || depth > SCAN_MAX_DEPTH) {
		return;
	}
	if (walk_pending(w) >= SCAN_MAX_SUBDIRS) {
		w->full = true;
		return;
	}

	struct stat dir_st;
	DIR *dir = stat(path, &dir_st) == 0 ? opendir(path) : NULL;
	if (!dir) {
		if (depth == 0) {
			w->root_failed = true;
		}
		return;
	}
	set_scan_folder(path);

	namelist_t subdirs, dirs, files;
	namelist_init(&subdirs);
	namelist_init(&dirs);
	namelist_init(&files);
	folder_sig_t sig = {0, 0};
	bool overflow = false;		// more files than a list holds
	bool dirs_listed = true;	// every subfolder in `dirs`
	bool more_subdirs = false; // subfolders past the batch in `subdirs`

	for (;;) {
		errno = 0;
		struct dirent *de = readdir(dir);
		if (!de || scan_cancel) {
			break;
		}
		if (de->d_name[0] == '.' || playlist_is_junk_name(de->d_name)) {
			continue;
		}

		char child[512];
		if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >= (int)sizeof(child)) {
			continue;
		}

		// The directory says what the name is, on every filesystem a card
		// comes with; asking the card again for each of thousands of names is
		// most of what a walk would cost. Only the names it cannot vouch for
		// -- a link, or no answer -- are looked at.
		bool is_dir = de->d_type == DT_DIR;
		bool is_file = de->d_type == DT_REG;
		if (!is_dir && !is_file) {
			struct stat st;
			if (lstat(child, &st) != 0) {
				continue;
			}
			if (S_ISLNK(st.st_mode) && (stat(child, &st) != 0 || !S_ISREG(st.st_mode))) {
				continue; // broken, or points at a directory: a scan leaves it too
			}
			is_dir = S_ISDIR(st.st_mode);
			is_file = S_ISREG(st.st_mode);
		}

		if (is_dir) {
			folder_sig_add(&sig, de->d_name, true);
			if (dirs_listed && !namelist_add(&dirs, de->d_name)) {
				dirs_listed = false;
			}
			if (depth == 0 && (strcasecmp(de->d_name, AUDIOBOOKDB_FOLDER) == 0 ||
							   strcasecmp(de->d_name, PODCASTDL_FOLDER) == 0)) {
				continue;
			}
			if (depth == 0 && scan_folder_count > 0 && !scan_folder_chosen(de->d_name)) {
				continue;
			}
			// As in scan_directory(): the rest come in later batches.
			if (!more_subdirs && !namelist_add(&subdirs, de->d_name)) {
				more_subdirs = true;
			}
			continue;
		}

		if (!is_file || !is_playable(de->d_name)) {
			continue;
		}
		folder_sig_add(&sig, de->d_name, false);
		if (depth == 0 && scan_folder_count > 0) {
			continue;
		}
		overflow |= !namelist_add(&files, de->d_name);
	}
	// A read that failed partway, or names that did not all fit, is not the
	// folder: comparing it would take the unread names for missing files.
	bool readable = errno == 0 && !overflow;
	closedir(dir);

	if (overflow) {
		printf("library: %s holds more than %d tracks; Detect changes leaves it to a scan\n", path, SCAN_MAX_SUBDIRS);
	}
	if (readable && !scan_cancel && !folder_unchanged(path, &sig)) {
		bool complete = update_compare(path, !(depth == 0 && scan_folder_count > 0), &files, &dirs, dirs_listed, w);
		if (complete && !w->full) {
			walk_note(w, path, dir_st.st_mtime, &sig);
		}
	}
	namelist_free(&files);
	namelist_free(&dirs);

	int walked = 0;
	for (;;) {
		for (int i = 0; i < subdirs.count && !scan_cancel && !w->full; i++) {
			char child[512];
			if (snprintf(child, sizeof(child), "%s/%s", path, subdirs.names[i]) >= (int)sizeof(child)) {
				continue;
			}
			update_folder(child, depth + 1, w);
			set_scan_folder(path);
		}
		walked += subdirs.count;
		namelist_free(&subdirs);
		if (!more_subdirs || scan_cancel || w->full) {
			break;
		}
		more_subdirs = collect_subdirs(path, depth, walked, true, &subdirs);
		if (subdirs.count == 0) {
			break;
		}
	}
}

// Whether a track's path lies in what the walk covered: the whole card, or the
// chosen folders. Case-blind, as the card is.
static bool in_walked_folders(const char *path) {
	size_t root_len = strlen(scan_root);
	if (strncmp(path, scan_root, root_len) != 0 || path[root_len] != '/') {
		return false;
	}
	if (scan_folder_count == 0) {
		return true;
	}
	const char *rest = path + root_len + 1;
	for (int i = 0; i < scan_folder_count; i++) {
		size_t n = strlen(scan_folder_names[i]);
		if (strncasecmp(rest, scan_folder_names[i], n) == 0 && rest[n] == '/') {
			return true;
		}
	}
	return false;
}

// Keeps `present` (PLAYLIST_SHOWN) in step with the library, after a scan or a
// Detect changes run that went to the end. An entry inside the walked folders
// that the library does not have is a file that is not on the card, or not one
// the player plays, and is hidden; one outside them is left as it is, since
// nothing here has looked. A track the library has is shown whatever `present`
// says. On the scan thread, with nothing locked.
static void playlists_follow_library(void) {
	char **names = NULL;
	int count = library_playlist_names(&names);
	int hidden = 0;

	for (int i = 0; i < count && !scan_cancel; i++) {
		char quoted[PLAYLIST_TABLE_MAX];
		if (!playlist_table(names[i], quoted, sizeof(quoted))) {
			continue;
		}
		char read_sql[PLAYLIST_TABLE_MAX + 160];
		char write_sql[PLAYLIST_TABLE_MAX + 64];
		snprintf(read_sql, sizeof(read_sql),
				 "SELECT p.rowid, p.path FROM %s p WHERE p.present<>0"
				 " AND NOT EXISTS(SELECT 1 FROM MEDIA_TABLE m WHERE m.path = p.path)",
				 quoted);
		snprintf(write_sql, sizeof(write_sql), "UPDATE %s SET present=0 WHERE rowid=?", quoted);

		pthread_mutex_lock(&db_lock);
		sqlite3_int64 *ids = NULL;
		int found = 0, capacity = 0;
		sqlite3_stmt *stmt = NULL;
		if (db && sqlite3_prepare_v2(db, read_sql, -1, &stmt, NULL) == SQLITE_OK) {
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *path = (const char *)sqlite3_column_text(stmt, 1);
				if (!path || !in_walked_folders(path)) {
					continue;
				}
				if (found == capacity) {
					int wanted = capacity ? capacity * 2 : 64;
					sqlite3_int64 *grown = realloc(ids, (size_t)wanted * sizeof(*ids));
					if (!grown) {
						break;
					}
					ids = grown;
					capacity = wanted;
				}
				ids[found++] = sqlite3_column_int64(stmt, 0);
			}
			sqlite3_finalize(stmt);
		}
		if (found > 0 && exec("BEGIN")) {
			stmt = NULL;
			bool ok = sqlite3_prepare_v2(db, write_sql, -1, &stmt, NULL) == SQLITE_OK;
			for (int k = 0; k < found && ok; k++) {
				sqlite3_bind_int64(stmt, 1, ids[k]);
				ok = sqlite3_step(stmt) == SQLITE_DONE;
				sqlite3_reset(stmt);
			}
			sqlite3_finalize(stmt);
			exec(ok ? "COMMIT" : "ROLLBACK");
			if (ok) {
				hidden += found;
				bump_generation(GEN_PLAYLISTS);
			}
		}
		pthread_mutex_unlock(&db_lock);
		free(ids);
	}

	library_playlist_names_free(names, count);
	if (hidden > 0) {
		printf("library: %d playlist entr%s no longer on the card\n", hidden, hidden == 1 ? "y is" : "ies are");
	}
}

static int id_cmp(const void *a, const void *b) {
	sqlite3_int64 x = *(const sqlite3_int64 *)a, y = *(const sqlite3_int64 *)b;
	return (x > y) - (x < y);
}

static bool under_folders(const namelist_t *folders, const char *path) {
	for (int i = 0; i < folders->count; i++) {
		size_t n = strlen(folders->names[i]);
		if (strncasecmp(path, folders->names[i], n) == 0 && path[n] == '/') {
			return true;
		}
	}
	return false;
}

// Every indexed file under the walked folders whose modification time is not
// the one its rows were written with: retagged, or replaced by another copy.
// One stat a file, and the table read in slices of row ids so the lock is
// never held across them.
#define RETAG_SLICE 64

static void update_retagged(walk_t *w) {
	if (w->gone.count > 1) {
		qsort(w->gone.ids, (size_t)w->gone.count, sizeof(w->gone.ids[0]), id_cmp);
	}
	typedef struct {
		sqlite3_int64 rowid;
		sqlite3_int64 mtime;
		char path[600];
	} slice_row_t;
	slice_row_t *slice = malloc(RETAG_SLICE * sizeof(*slice));
	if (!slice) {
		return;
	}

	char last_file[600] = "";
	sqlite3_int64 after = 0;
	int changed_files = 0;
	while (!scan_cancel && !w->full) {
		int n = 0;
		pthread_mutex_lock(&db_lock);
		sqlite3_stmt *stmt = NULL;
		if (db && sqlite3_prepare_v2(db, "SELECT rowid, path, mtime FROM MEDIA_TABLE WHERE rowid > ?1 ORDER BY rowid LIMIT ?2",
									 -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, after);
			sqlite3_bind_int(stmt, 2, RETAG_SLICE);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *path = (const char *)sqlite3_column_text(stmt, 1);
				slice[n].rowid = sqlite3_column_int64(stmt, 0);
				slice[n].mtime = sqlite3_column_int64(stmt, 2);
				snprintf(slice[n].path, sizeof(slice[n].path), "%s", path ? path : "");
				n++;
			}
			sqlite3_finalize(stmt);
		}
		pthread_mutex_unlock(&db_lock);
		if (n == 0) {
			break;
		}
		after = slice[n - 1].rowid;

		for (int i = 0; i < n && !scan_cancel && !w->full; i++) {
			const char *path = slice[i].path;
			if (!path[0] || !in_walked_folders(path) || under_folders(&w->gone_folders, path) ||
				bsearch(&slice[i].rowid, w->gone.ids, (size_t)w->gone.count, sizeof(sqlite3_int64), id_cmp)) {
				continue;
			}
			// A sheet's tracks, and a WAV's marker tracks, are one file.
			char file[600];
			const char *track = strstr(path, "?track=");
			snprintf(file, sizeof(file), "%.*s", track ? (int)(track - path) : (int)strlen(path), path);
			if (strcmp(file, last_file) == 0) {
				continue; // that file is decided already, either way
			}
			snprintf(last_file, sizeof(last_file), "%s", file);

			struct stat st;
			bool changed = stat(file, &st) == 0 && (sqlite3_int64)st.st_mtime != slice[i].mtime;
			if (!changed || namelist_has(&w->reread, file)) {
				continue;
			}
			if (walk_pending(w) >= SCAN_MAX_SUBDIRS || !namelist_add(&w->reread, file)) {
				w->full = true;
				break;
			}
			changed_files++;
		}
	}
	free(slice);
	if (changed_files > 0) {
		printf("library: %d indexed file(s) changed since they were read\n", changed_files);
	}
}

// How many rows `w` would take out. With db_lock held.
static int walk_gone_rows(const walk_t *w) {
	int rows = w->gone.count;
	sqlite3_stmt *stmt = NULL;
	if (db && w->gone_folders.count > 0 &&
		sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM MEDIA_TABLE WHERE path >= ?1 AND path < ?2", -1, &stmt, NULL) ==
			SQLITE_OK) {
		for (int i = 0; i < w->gone_folders.count; i++) {
			bind_under(stmt, w->gone_folders.names[i]);
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				rows += sqlite3_column_int(stmt, 0);
			}
		}
		sqlite3_finalize(stmt);
	}
	return rows;
}

// Link rows whose track is gone. With db_lock held.
static void prune_links(void) {
	exec("DELETE FROM ARTIST_LINK WHERE NOT EXISTS (SELECT 1 FROM MEDIA_TABLE m WHERE m.path = ARTIST_LINK.path)");
	exec("DELETE FROM ALBUM_ARTIST_LINK WHERE NOT EXISTS"
		 " (SELECT 1 FROM MEDIA_TABLE m WHERE m.path = ALBUM_ARTIST_LINK.path)");
	exec("DELETE FROM GENRE_LINK WHERE NOT EXISTS (SELECT 1 FROM MEDIA_TABLE m WHERE m.path = GENRE_LINK.path)");
}

// The names nothing is filed under any more. Each check is a lookup on the
// index of the column it names. With db_lock held.
static void prune_names(void) {
	exec("DELETE FROM ARTIST_TABLE WHERE NOT EXISTS (SELECT 1 FROM MEDIA_TABLE m WHERE m.artist = ARTIST_TABLE.artist)"
		 " AND NOT EXISTS (SELECT 1 FROM ARTIST_LINK l WHERE l.artist = ARTIST_TABLE.artist)");
	exec("DELETE FROM ALBUM_ARTIST_TABLE WHERE NOT EXISTS"
		 " (SELECT 1 FROM MEDIA_TABLE m WHERE m.album_artist = ALBUM_ARTIST_TABLE.album_artist)"
		 " AND NOT EXISTS (SELECT 1 FROM ALBUM_ARTIST_LINK l WHERE l.album_artist = ALBUM_ARTIST_TABLE.album_artist)");
	exec("DELETE FROM GENRE_TABLE WHERE NOT EXISTS (SELECT 1 FROM MEDIA_TABLE m WHERE m.genre = GENRE_TABLE.genre)"
		 " AND NOT EXISTS (SELECT 1 FROM GENRE_LINK l WHERE l.genre = GENRE_TABLE.genre)");
	exec("DELETE FROM ALBUM_TABLE WHERE NOT EXISTS (SELECT 1 FROM MEDIA_TABLE m WHERE m.album = ALBUM_TABLE.album)");
	exec("DELETE FROM ALBUM_GROUP_TABLE WHERE NOT EXISTS (SELECT 1 FROM MEDIA_TABLE m WHERE m.album = "
		 "ALBUM_GROUP_TABLE.album AND m.album_key = ALBUM_GROUP_TABLE.album_key)");
}

// The file a row stands for: its own path, or for a track of a sheet the
// sheet's, which is everything before "?track=".
static void row_file(const char *path, char *out, size_t size) {
	const char *mark = strstr(path, "?track=");
	size_t len = mark ? (size_t)(mark - path) : strlen(path);
	snprintf(out, size, "%.*s", (int)(len < size ? len : size - 1), path);
}

int library_forget_deleted(const char *const *paths, int count) {
	if (!paths || count <= 0) {
		return 0;
	}
	pthread_mutex_lock(&db_lock);
	if (!db) {
		pthread_mutex_unlock(&db_lock);
		return 0;
	}

	// The rows the paths stand for -- a file's own, a sheet's tracks, and
	// everything under a folder -- and of those, the ones whose file is gone.
	// Asked of the card and not assumed: a delete that stopped halfway leaves
	// some of a folder behind, and those stay.
	static const char *const SELECT_SQL =
		"SELECT rowid, path FROM MEDIA_TABLE WHERE path = ?1 OR (path >= ?1 || '?track=' AND path < ?1 || '?track>')"
		" OR (path >= ?1 || '/' AND path < ?1 || '0')";
	int removed = 0, unfavoured = 0;
	bool in_transaction = exec("BEGIN");
	sqlite3_stmt *select = NULL, *del = NULL, *fav = NULL, *plays = NULL;
	sqlite3_prepare_v2(db, SELECT_SQL, -1, &select, NULL);
	sqlite3_prepare_v2(db, "DELETE FROM MEDIA_TABLE WHERE rowid = ?1", -1, &del, NULL);
	sqlite3_prepare_v2(db, "DELETE FROM FAVOURITES WHERE path = ?1", -1, &fav, NULL);
	sqlite3_prepare_v2(db, "DELETE FROM PLAY_COUNTS WHERE path = ?1", -1, &plays, NULL);
	for (int i = 0; select && del && i < count; i++) {
		if (!paths[i] || !paths[i][0]) {
			continue;
		}
		sqlite3_reset(select);
		sqlite3_bind_text(select, 1, paths[i], -1, SQLITE_TRANSIENT);
		while (sqlite3_step(select) == SQLITE_ROW) {
			sqlite3_int64 rowid = sqlite3_column_int64(select, 0);
			const char *path = (const char *)sqlite3_column_text(select, 1);
			char file[600];
			struct stat st;
			row_file(path ? path : "", file, sizeof(file));
			if (!file[0] || stat(file, &st) == 0) {
				continue;
			}
			sqlite3_reset(del);
			sqlite3_bind_int64(del, 1, rowid);
			sqlite3_step(del);
			removed += sqlite3_changes(db);
			// A favourite or a count of plays of a file deleted on purpose has
			// nothing left to point at.
			sqlite3_stmt *const owned[] = {fav, plays};
			for (size_t k = 0; k < sizeof(owned) / sizeof(owned[0]); k++) {
				if (!owned[k]) {
					continue;
				}
				sqlite3_reset(owned[k]);
				sqlite3_bind_text(owned[k], 1, path, -1, SQLITE_TRANSIENT);
				sqlite3_step(owned[k]);
				if (owned[k] == fav) {
					unfavoured += sqlite3_changes(db);
				}
			}
		}
	}
	sqlite3_finalize(select);
	sqlite3_finalize(del);
	sqlite3_finalize(fav);
	sqlite3_finalize(plays);

	// A folder that is gone takes what the walk wrote down about it too, so the
	// next look for changes does not go looking for it.
	static const char *const UNDER[] = {
		"DELETE FROM FOLDER_TABLE WHERE path = ?3 OR (path >= ?1 AND path < ?2)",
		"DELETE FROM CUE_STATE WHERE path = ?3 OR (path >= ?1 AND path < ?2)",
	};
	for (int i = 0; i < count; i++) {
		struct stat st;
		if (!paths[i] || !paths[i][0] || stat(paths[i], &st) == 0) {
			continue;
		}
		for (size_t q = 0; q < sizeof(UNDER) / sizeof(UNDER[0]); q++) {
			sqlite3_stmt *stmt = NULL;
			if (sqlite3_prepare_v2(db, UNDER[q], -1, &stmt, NULL) == SQLITE_OK) {
				bind_under(stmt, paths[i]);
				sqlite3_bind_text(stmt, 3, paths[i], -1, SQLITE_TRANSIENT);
				sqlite3_step(stmt);
				sqlite3_finalize(stmt);
			}
		}
	}

	if (removed > 0) {
		prune_links();
		prune_names();
		exec("DELETE FROM COUNT_TABLE");
		char sql[128];
		snprintf(sql, sizeof(sql), "INSERT INTO COUNT_TABLE(cn) VALUES(%d)",
				 count_rows("SELECT COUNT(*) FROM MEDIA_TABLE"));
		exec(sql);
	}
	if (in_transaction && !exec("COMMIT")) {
		exec("ROLLBACK");
		removed = 0;
		unfavoured = 0;
	}
	if (removed > 0) {
		// The open lists hold row ids, some of which are gone now.
		bump_generation(GEN_MEDIA);
	}
	if (unfavoured > 0) {
		bump_generation(GEN_FAVOURITES);
	}
	pthread_mutex_unlock(&db_lock);

	if (removed > 0) {
		printf("library: %d deleted track(s) taken out of the index\n", removed);
	}
	return removed;
}

// Takes out what the walk found gone. Returns how many tracks went, or -1
// when it would not.
//
// Not when that would be every track there is: a mount point that moved, or a
// card that died halfway, looks exactly like a card emptied on purpose, and
// would empty the library with it.
static int update_take_out(const walk_t *w) {
	if (w->lists_failed || (w->gone.count == 0 && w->gone_folders.count == 0)) {
		return 0;
	}
	pthread_mutex_lock(&db_lock);
	int total = count_rows("SELECT COUNT(*) FROM MEDIA_TABLE");
	int going = walk_gone_rows(w);
	if (going > 0 && going >= total) {
		printf("library: none of the %d indexed files under %s was found; leaving the index alone\n", total, scan_root);
		pthread_mutex_unlock(&db_lock);
		return -1;
	}

	int removed = 0;
	sqlite3_stmt *del = NULL;
	if (exec("BEGIN") && sqlite3_prepare_v2(db, "DELETE FROM MEDIA_TABLE WHERE rowid = ?1", -1, &del, NULL) == SQLITE_OK) {
		for (int i = 0; i < w->gone.count; i++) {
			sqlite3_bind_int64(del, 1, w->gone.ids[i]);
			sqlite3_step(del);
			removed += sqlite3_changes(db);
			sqlite3_reset(del);
		}
		sqlite3_finalize(del);

		static const char *const UNDER[] = {
			"DELETE FROM MEDIA_TABLE WHERE path >= ?1 AND path < ?2",
			"DELETE FROM FOLDER_TABLE WHERE path >= ?1 AND path < ?2",
			"DELETE FROM CUE_STATE WHERE path >= ?1 AND path < ?2",
		};
		for (size_t q = 0; q < sizeof(UNDER) / sizeof(UNDER[0]); q++) {
			sqlite3_stmt *stmt = NULL;
			if (sqlite3_prepare_v2(db, UNDER[q], -1, &stmt, NULL) != SQLITE_OK) {
				continue;
			}
			for (int i = 0; i < w->gone_folders.count; i++) {
				bind_under(stmt, w->gone_folders.names[i]);
				sqlite3_step(stmt);
				if (q == 0) {
					removed += sqlite3_changes(db);
				}
			}
			sqlite3_finalize(stmt);
		}
		sqlite3_stmt *folder = NULL;
		if (sqlite3_prepare_v2(db, "DELETE FROM FOLDER_TABLE WHERE path = ?1", -1, &folder, NULL) == SQLITE_OK) {
			for (int i = 0; i < w->gone_folders.count; i++) {
				sqlite3_bind_text(folder, 1, w->gone_folders.names[i], -1, SQLITE_TRANSIENT);
				sqlite3_step(folder);
				sqlite3_reset(folder);
			}
			sqlite3_finalize(folder);
		}
		prune_links();
		if (!exec("COMMIT")) {
			exec("ROLLBACK");
			removed = 0;
		}
	} else {
		sqlite3_finalize(del);
		exec("ROLLBACK");
	}
	pthread_mutex_unlock(&db_lock);

	if (removed > 0) {
		printf("library: %d track(s) whose file is gone taken out\n", removed);
	}
	return removed;
}

// The rows of one file about to be read again: its own, or a sheet's tracks,
// with their link rows. Returns how many tracks there were.
static int forget_file(const char *file) {
	static const char *const SQL[] = {
		"DELETE FROM MEDIA_TABLE WHERE path = ?1 OR (path >= ?1 || '?track=' AND path < ?1 || '?track>')",
		"DELETE FROM ARTIST_LINK WHERE path = ?1 OR (path >= ?1 || '?track=' AND path < ?1 || '?track>')",
		"DELETE FROM ALBUM_ARTIST_LINK WHERE path = ?1 OR (path >= ?1 || '?track=' AND path < ?1 || '?track>')",
		"DELETE FROM GENRE_LINK WHERE path = ?1 OR (path >= ?1 || '?track=' AND path < ?1 || '?track>')",
	};
	int tracks = 0;
	pthread_mutex_lock(&db_lock);
	for (size_t i = 0; db && i < sizeof(SQL) / sizeof(SQL[0]); i++) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db, SQL[i], -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, file, -1, SQLITE_TRANSIENT);
			sqlite3_step(stmt);
			if (i == 0) {
				tracks = sqlite3_changes(db);
			}
			sqlite3_finalize(stmt);
		}
	}
	pthread_mutex_unlock(&db_lock);
	return tracks;
}

// How many tracks the index holds for one file: its own row, or a sheet's.
static int file_tracks(const char *file) {
	int tracks = 0;
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db,
								 "SELECT COUNT(*) FROM MEDIA_TABLE WHERE path = ?1"
								 " OR (path >= ?1 || '?track=' AND path < ?1 || '?track>')",
								 -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, file, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			tracks = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return tracks;
}

// A sheet read again changes what its folder holds without changing a name in
// it: the audio it claimed may now be a file of its own, or another file
// claimed instead. The folder's FOLDER_TABLE row goes, so the next walk
// compares it.
static void folder_forget(const char *file) {
	char folder[600];
	const char *slash = strrchr(file, '/');
	snprintf(folder, sizeof(folder), "%.*s", slash ? (int)(slash - file) : 0, file);
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "DELETE FROM FOLDER_TABLE WHERE path = ?1", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, folder, -1, SQLITE_TRANSIENT);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
}

// The audio a sheet claimed when it last parsed, whatever the sheet is now.
static void cue_state_audio(const char *sheet, char *audio, size_t size) {
	audio[0] = '\0';
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT audio FROM CUE_STATE WHERE path = ?1", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, sheet, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *text = (const char *)sqlite3_column_text(stmt, 0);
			snprintf(audio, size, "%s", text ? text : "");
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
}

// Reads files into the index, through the same path as a scan: a sheet becomes
// its tracks and takes the place of the audio it claims. With `replace`, the
// rows each file had go first, in the same transaction, so a run stopped
// halfway leaves every file either as it was or read again. Returns how many
// tracks were replaced.
//
// A sheet read again that no longer parses leaves the audio it cut up without
// a row: that file is read whole, as a scan would.
static int update_index(const namelist_t *files, bool replace) {
	int replaced = 0;
	for (int i = 0; i < files->count && !scan_cancel; i++) {
		const char *path = files->names[i];
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		char claimed[600] = "";
		if (replace) {
			if (cue_is_sheet(path)) {
				cue_state_audio(path, claimed, sizeof(claimed));
			}
			replaced += forget_file(path);
		}
		const char *slash = strrchr(path, '/');
		cue_claimed_count = 0; // the claims were settled while collecting
		scan_one_file(path, slash ? slash + 1 : path, &st);

		if (replace && cue_is_sheet(path)) {
			folder_forget(path);
			struct stat audio_st;
			if (claimed[0] && file_tracks(path) == 0 && file_tracks(claimed) == 0 && stat(claimed, &audio_st) == 0 &&
				S_ISREG(audio_st.st_mode)) {
				const char *name = strrchr(claimed, '/');
				cue_claimed_count = 0;
				scan_one_file(claimed, name ? name + 1 : claimed, &audio_st);
			}
		}
	}
	return replaced;
}

// How the last Detect changes run ended, told by the scan thread as its last
// word (see scan_thread_func).
static library_update_event_t update_outcome;
static int update_added, update_removed, update_replaced;

static void update_run(void) {
	uint32_t started_ms = now_ms();
	printf("library: looking for changes in %s%s\n", scan_root,
		   scan_folder_count > 0 ? " (the chosen folders)" : "");
	pthread_mutex_lock(&db_lock);
	if (!prepare_statements() || !prepare_known()) {
		fprintf(stderr, "library: cannot look for changes, the database is not answering\n");
		scan_db_failed = true;
		scan_cancel = true;
	}
	scan_id_base = count_rows("SELECT MAX(id) FROM MEDIA_TABLE");
	// Room for the path index, which every folder looked into is compared
	// against: the page cache, DB_CACHE_KIB.
	pthread_mutex_unlock(&db_lock);

	int before = library_track_count();
	int removed = 0, replaced = 0;
	bool reading = false;
	int folders_noted = 0;
	while (!scan_cancel) {
		walk_t walk;
		walk_init(&walk);
		update_folder(scan_root, 0, &walk);
		if (walk.root_failed) {
			printf("library: %s cannot be read; nothing looked at\n", scan_root);
			walk_free(&walk);
			break;
		}
		if (rules.retagged && !scan_cancel) {
			update_retagged(&walk);
		}
		// A walk that saw an empty card is not written down either, or the
		// next one would take the empty card for the one to compare against.
		bool refused = false;
		if (!scan_cancel) {
			int went = update_take_out(&walk);
			refused = went < 0;
			removed += went > 0 ? went : 0;
		}

		if (walk_pending(&walk) > 0 && !scan_cancel) {
			printf("library: %d new file(s) and %d changed one(s) to read\n", walk.found.count, walk.reread.count);
			if (!reading && update_listener) {
				update_listener(LIBRARY_UPDATE_ADDING, walk.found.count, 0, walk.reread.count);
			}
			reading = true;

			pthread_mutex_lock(&db_lock);
			if (!exec("BEGIN")) {
				scan_db_failed = true;
				scan_cancel = true;
			}
			pthread_mutex_unlock(&db_lock);

			if (!scan_cancel) {
				replaced += update_index(&walk.reread, true);
				update_index(&walk.found, false);
			}

			pthread_mutex_lock(&db_lock);
			exec(scan_db_failed ? "ROLLBACK" : "COMMIT");
			pthread_mutex_unlock(&db_lock);
		}

		// A full list means the walk stopped early; once more for the rest.
		// The folders are written down only after a walk that went everywhere,
		// with what it found read in.
		bool more = walk.full && !refused;
		if (!more && !refused && !scan_cancel && !scan_db_failed) {
			pthread_mutex_lock(&db_lock);
			if (exec("BEGIN")) {
				for (int i = 0; i < walk.note_count; i++) {
					folder_mark(walk.notes[i].path, walk.notes[i].mtime, &walk.notes[i].sig);
				}
				exec("COMMIT");
				folders_noted = walk.note_count;
			}
			pthread_mutex_unlock(&db_lock);
		}
		walk_free(&walk);
		if (!more) {
			break;
		}
	}

	bool changed = reading || removed > 0;
	if (changed && rules.join_albums && !scan_db_failed) {
		char sql[160];
		snprintf(sql, sizeof(sql), "SELECT DISTINCT album FROM MEDIA_TABLE WHERE id > %d AND album <> ''", scan_id_base);
		int count = 0;
		char **albums = album_names(sql, &count);
		album_join(albums, count);
		names_free(albums, count);
	}

	pthread_mutex_lock(&db_lock);
	finalize_statements();
	finalize_known();
	scan_id_base = 0;
	if (changed) {
		if (cue_claimed_rows) {
			prune_links();
		}
		prune_names();
		links_check();
		exec("DELETE FROM COUNT_TABLE");
		char sql[128];
		snprintf(sql, sizeof(sql), "INSERT INTO COUNT_TABLE(cn) VALUES(%d)",
				 count_rows("SELECT COUNT(*) FROM MEDIA_TABLE"));
		exec(sql);
		// The open lists hold the row ids they were built over: the new rows
		// are not among them, and the removed ones still are.
		bump_generation(GEN_MEDIA);
	}
	cue_claimed_rows = false;
	if (db) {
		sqlite3_db_release_memory(db);
	}
	pthread_mutex_unlock(&db_lock);

	// What is there now, against what was there less what went: the tracks
	// read in, of which the ones that replaced a changed file's are not new.
	int added = library_track_count() - (before - removed - replaced) - replaced;
	if (added < 0) {
		added = 0;
	}
	printf("library: %d new track(s), %d removed, %d read again, %d folder(s) noted, in %u ms%s\n", added, removed,
		   replaced, folders_noted, now_ms() - started_ms,
		   scan_db_failed ? " (database error)" : scan_cancel ? " (stopped early)" : "");
	// Stopped with nothing done -- the card pulled again, a scan asked for --
	// is not "no changes": nothing was looked at to the end.
	bool stopped = (scan_cancel || scan_db_failed) && !changed;
	update_outcome = stopped ? LIBRARY_UPDATE_STOPPED : LIBRARY_UPDATE_FINISHED;
	update_added = added;
	update_removed = removed;
	update_replaced = replaced;
}

// ---------------------------------------------------------------------------
// Filing the index again
//
// Splitting artists and genres and joining albums are decided from what
// MEDIA_TABLE already holds -- the artist, genre and album artist of every
// track -- so a change of setting is applied without reading a single file:
// the link rows and the artist and genre lists are rebuilt from the distinct
// values, and the album keys worked out again.
// ---------------------------------------------------------------------------

// Rebuilds one side: the names of `column` of MEDIA_TABLE through `lookup`,
// and `link_table`.
static void relink(const char *column, const char *link_table, sqlite3_stmt **lookup, unsigned separators,
				   char *const *exceptions, int exception_count) {
	char sql[200];
	snprintf(sql, sizeof(sql), "SELECT DISTINCT %s FROM MEDIA_TABLE WHERE %s <> ''", column, column);
	int count = 0;
	char **values = album_names(sql, &count);

	char insert_sql[200];
	snprintf(insert_sql, sizeof(insert_sql), "INSERT INTO %s(path, %s) SELECT path, ?2 FROM MEDIA_TABLE WHERE %s = ?1",
			 link_table, column, column);

	char names[SPLIT_MAX][SPLIT_NAME_MAX];
	for (int i = 0; i < count && !scan_cancel; i += 50) {
		pthread_mutex_lock(&db_lock);
		sqlite3_stmt *link = NULL;
		if (db && exec("BEGIN") && sqlite3_prepare_v2(db, insert_sql, -1, &link, NULL) == SQLITE_OK) {
			for (int j = i; j < count && j < i + 50; j++) {
				int n = split_names(values[j], separators, exceptions, exception_count, names, SPLIT_MAX);
				if (n == 0) {
					run_lookup(*lookup, values[j]);
					continue;
				}
				for (int k = 0; k < n; k++) {
					run_lookup(*lookup, names[k]);
					sqlite3_reset(link);
					sqlite3_bind_text(link, 1, values[j], -1, SQLITE_TRANSIENT);
					sqlite3_bind_text(link, 2, names[k], -1, SQLITE_TRANSIENT);
					sqlite3_step(link);
				}
			}
			sqlite3_finalize(link);
			if (!exec("COMMIT")) {
				exec("ROLLBACK");
			}
		}
		pthread_mutex_unlock(&db_lock);
	}
	names_free(values, count);
}

// Returns whether it went all the way through. One stopped halfway leaves the
// lists partly rebuilt and the settings not recorded, so organized_as_set()
// asks for it again.
static bool reorganize_run(void) {
	uint32_t started_ms = now_ms();
	pthread_mutex_lock(&db_lock);
	bool ready = db && prepare_statements() && exec("BEGIN");
	if (ready) {
		exec("DELETE FROM ARTIST_LINK");
		exec("DELETE FROM ALBUM_ARTIST_LINK");
		exec("DELETE FROM GENRE_LINK");
		exec("DELETE FROM ARTIST_TABLE");
		exec("DELETE FROM ALBUM_ARTIST_TABLE");
		exec("DELETE FROM GENRE_TABLE");
		ready = exec("COMMIT");
		if (!ready) {
			exec("ROLLBACK");
		}
	}
	pthread_mutex_unlock(&db_lock);

	if (ready) {
		relink("artist", "ARTIST_LINK", &stmt_artist, rules.artist_separators, rules.exceptions, rules.exception_count);
		relink("album_artist", "ALBUM_ARTIST_LINK", &stmt_album_artist, rules.album_artist_separators, rules.exceptions,
			   rules.exception_count);
		relink("genre", "GENRE_LINK", &stmt_genre, rules.genre_separators, NULL, 0);

		int count = 0;
		char **albums = album_names("SELECT DISTINCT album FROM MEDIA_TABLE WHERE album <> ''", &count);
		int rekeyed = album_join(albums, count);
		names_free(albums, count);
		printf("library: filed again in %u ms, %d album key(s) changed\n", now_ms() - started_ms, rekeyed);
	}

	bool done = ready && !scan_cancel;
	pthread_mutex_lock(&db_lock);
	finalize_statements();
	if (db) {
		prune_names();
		links_check();
		if (done) {
			organize_state_put(&rules);
		}
		sqlite3_db_release_memory(db);
	}
	bump_generation(GEN_MEDIA);
	pthread_mutex_unlock(&db_lock);
	return done;
}

static void *scan_thread_func(void *arg) {
	(void)arg;

	// One core: the scan reads thousands of files; at normal priority it
	// would starve the interface for its whole duration. Detect changes and
	// filing the index again run a level higher: they run while the player is
	// in use, and under a busy interface an idle-class thread would barely move.
	if (scan_mode == SCAN_FULL) {
		thread_be_background("library scan");
	} else {
		thread_be_low_priority("library update");
	}

	switch (scan_mode) {
	case SCAN_FULL:
		full_scan_run();
		// A run that failed on a damaged file goes again into a new one.
		if (scan_db_failed && db_damaged && !scan_stop_asked && replace_damaged()) {
			scan_db_failed = false;
			scan_cancel = false;
			scan_found = 0;
			scan_id_base = 0;
			full_scan_run();
		}
		if (!scan_cancel && !scan_db_failed) {
			playlists_follow_library();
			if (update_listener) {
				update_listener(LIBRARY_UPDATE_SCANNED, 0, 0, 0);
			}
		}
		break;
	case SCAN_UPDATE:
		update_run();
		if (update_outcome == LIBRARY_UPDATE_FINISHED && !scan_cancel && !scan_db_failed) {
			playlists_follow_library();
		}
		break;
	case SCAN_REORGANIZE:
		break;
	}

	set_scan_folder("");
	crumb_set(NULL); // the scan is over: a later crash must not blame its last file

	// The run this thread was started for, then the settings that changed
	// meanwhile. A stopped run leaves those waiting for the next one: the card
	// may be on its way out. The thread is over only under rules_lock, so a
	// change library_reorganize() hands over is either taken here or finds the
	// thread gone and starts its own.
	bool attempted = false;
	bool done = false;
	for (;;) {
		pthread_mutex_lock(&rules_lock);
		bool wanted = reorganize_pending || (scan_mode == SCAN_REORGANIZE && !attempted);
		bool again = wanted && done == attempted && !scan_cancel && !scan_db_failed;
		if (again && reorganize_pending) {
			names_free(rules.exceptions, rules.exception_count);
			rules = pending_rules;
			memset(&pending_rules, 0, sizeof(pending_rules));
			reorganize_pending = false;
		}
		if (!again) {
			// The notice that went up for it comes down either way.
			if ((wanted || attempted) && update_listener) {
				update_listener(done && !wanted ? LIBRARY_UPDATE_REORGANIZED : LIBRARY_UPDATE_STOPPED, 0, 0, 0);
			}
			// A Detect changes run ends here rather than where it stopped
			// reading, so its outcome covers the filing done after it.
			if (scan_mode == SCAN_UPDATE && update_listener) {
				update_listener(update_outcome, update_added, update_removed, update_replaced);
			}
			scan_running = false;
			pthread_mutex_unlock(&rules_lock);
			break;
		}
		pthread_mutex_unlock(&rules_lock);
		attempted = true;
		done = reorganize_run();
	}
	return NULL;
}

static bool scan_launch(const char *root, scan_mode_t mode) {
	if (!db || scan_running || ((!root || !root[0]) && mode != SCAN_REORGANIZE)) {
		return false;
	}

	if (root && root[0]) {
		snprintf(scan_root, sizeof(scan_root), "%s", root);
	}
	scan_folders_load();
	rules_load(&rules);
	scan_mode = mode;
	scan_id_base = 0;
	scan_found = 0;
	scan_cancel = false;
	scan_stop_asked = false;
	scan_db_failed = false;
	scan_running = true;
	set_scan_folder(mode == SCAN_REORGANIZE ? "" : root);

	// Off by default: a line per file is a write to the card per file, which
	// slows the scan and wears the card. It is turned on to catch a death that
	// leaves nothing behind -- a process killed from outside runs no handler,
	// so the last line written is the only evidence of which file it was on.
	scan_log_files = library_log_database();
	if (scan_log_files) {
		printf("library: naming every file (Developer options > Database log)\n");
		fflush(stdout);
	}

	if (pthread_create(&scan_thread, NULL, scan_thread_func, NULL) != 0) {
		scan_running = false;
		fprintf(stderr, "library: could not start the scan thread\n");
		return false;
	}

	pthread_detach(scan_thread);
	return true;
}

bool library_scan_start(const char *root) {
	if (scan_running && scan_mode != SCAN_FULL) {
		library_scan_stop_and_wait();
	}
	if (db_damaged && !scan_running) {
		replace_damaged();
	}
	return scan_launch(root, SCAN_FULL);
}

// Leaves the settings as they are now for the scan thread to file the index
// under before it finishes. False when the thread is not running.
static bool reorganize_when_done(bool notify) {
	pthread_mutex_lock(&rules_lock);
	bool waiting = scan_running;
	if (waiting) {
		rules_load(&pending_rules);
		reorganize_pending = true;
		// Under the lock, so the thread's REORGANIZED cannot overtake it.
		if (notify && update_listener) {
			update_listener(LIBRARY_UPDATE_REORGANIZING, 0, 0, 0);
		}
	}
	pthread_mutex_unlock(&rules_lock);
	return waiting;
}

bool library_reorganize(void) {
	if (!library_is_open() || library_track_count() == 0) {
		return false;
	}
	// Behind a scan, no notice: the scan page is up, and the toast at the end
	// says it was done.
	if (reorganize_when_done(scan_mode != SCAN_FULL)) {
		return true;
	}
	if (update_listener) {
		update_listener(LIBRARY_UPDATE_REORGANIZING, 0, 0, 0);
	}
	if (scan_launch(NULL, SCAN_REORGANIZE) || reorganize_when_done(false)) {
		return true;
	}
	if (update_listener) {
		update_listener(LIBRARY_UPDATE_STOPPED, 0, 0, 0);
	}
	return false;
}

bool library_organize_check(void) {
	if (!library_is_open() || library_track_count() == 0 || organized_as_set()) {
		return false;
	}
	printf("library: the index was filed under other settings; filing it again\n");
	return library_reorganize();
}

bool library_detect_changes(void) { return config_get_bool("library", "detect_changes", false); }

bool library_detect_changes_chosen(void) { return config_get("library", "detect_changes", NULL) != NULL; }

void library_set_detect_changes(bool on) {
	config_set_bool("library", "detect_changes", on);
	config_save();
}

void library_set_update_listener(library_update_listener_t listener) { update_listener = listener; }

bool library_card_returned(const char *root) {
	if (!library_is_open() || scan_running) {
		return false;
	}
	// A card filed under other settings -- changed while it was out, or a
	// run stopped halfway -- is filed again: on its own, or after Detect
	// changes.
	bool unfiled = !organized_as_set();
	if (!library_detect_changes()) {
		return unfiled && library_organize_check();
	}
	if (library_track_count() == 0) {
		printf("library: detect changes: nothing indexed yet, a scan builds the library\n");
		return false;
	}
	if (unfiled) {
		printf("library: the index was filed under other settings; filing it again after the check\n");
		pthread_mutex_lock(&rules_lock);
		rules_load(&pending_rules);
		reorganize_pending = true;
		pthread_mutex_unlock(&rules_lock);
	}
	// Told from here, on the caller's thread, rather than from the run: the
	// notice is up the moment the card is back, whatever the scan thread is
	// waiting for.
	if (update_listener) {
		update_listener(LIBRARY_UPDATE_LOOKING, 0, 0, 0);
	}
	if (!scan_launch(root, SCAN_UPDATE)) {
		if (update_listener) {
			update_listener(LIBRARY_UPDATE_STOPPED, 0, 0, 0);
		}
		return false;
	}
	return true;
}

bool library_log_database(void) { return config_get_bool("library", "log_database", false); }

void library_set_log_database(bool enabled) {
	config_set_bool("library", "log_database", enabled);
	config_save();
	// A scan already under way starts naming files at once, rather than at the
	// next one: the switch is thrown precisely while trying to catch a crash,
	// and asking for a restart of the very scan that crashes is asking a lot.
	scan_log_files = enabled;
}

// Whether there is an index to ask at all. False while the card is exported to
// a computer, while it is out, and before the first open.
#define NAMES_CHUNK 400

// One column of one table, from the first row after `*after` (a rowid), up to
// NAMES_CHUNK rows, under the lock. Returns the rows read; -1 on an error or
// no database.
static int names_chunk(const char *sql, sqlite3_int64 *after, bool (*cb)(const char *, void *), void *user,
					   bool *stop) {
	int n = -1;
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, *after);
		n = 0;
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			*after = sqlite3_column_int64(stmt, 0);
			const char *text = (const char *)sqlite3_column_text(stmt, 1);
			n++;
			if (text && text[0] && !cb(text, user)) {
				*stop = true;
				break;
			}
		}
	}
	if (stmt) {
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return n;
}

void library_names_walk(bool (*cb)(const char *text, void *user), void *user, int max_titles) {
	static const struct {
		const char *sql;
		bool titles;
	} WALK[] = {
		{"SELECT rowid, album FROM ALBUM_TABLE WHERE rowid > ?1 ORDER BY rowid LIMIT 400", false},
		{"SELECT rowid, artist FROM ARTIST_TABLE WHERE rowid > ?1 ORDER BY rowid LIMIT 400", false},
		{"SELECT rowid, name FROM MEDIA_TABLE WHERE rowid > ?1 ORDER BY rowid LIMIT 400", true},
	};
	bool stop = false;
	int titles = 0;
	for (size_t i = 0; i < sizeof(WALK) / sizeof(WALK[0]) && !stop; i++) {
		sqlite3_int64 after = 0;
		for (;;) {
			int n = names_chunk(WALK[i].sql, &after, cb, user, &stop);
			if (n <= 0 || stop) {
				break;
			}
			if (WALK[i].titles && (titles += n) >= max_titles) {
				break;
			}
			usleep(2000); // the interface's queries get the lock between chunks
		}
	}
}

void library_album_names_walk(bool (*cb)(const char *text, void *user), void *user) {
	sqlite3_int64 after = 0;
	bool stop = false;
	while (names_chunk("SELECT rowid, album FROM ALBUM_GROUP_TABLE WHERE rowid > ?1 ORDER BY rowid LIMIT 400", &after,
					   cb, user, &stop) > 0 &&
		   !stop) {
		usleep(2000); // the interface's queries get the lock between chunks
	}
}

bool library_is_open(void) {
	pthread_mutex_lock(&db_lock);
	bool open = db != NULL;
	pthread_mutex_unlock(&db_lock);
	return open;
}

bool library_scan_running(void) { return scan_running; }

int library_scan_found(void) { return scan_found; }

void library_scan_current_folder(char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	pthread_mutex_lock(&scan_folder_lock);
	snprintf(out, size, "%s", scan_folder);
	pthread_mutex_unlock(&scan_folder_lock);
}

void library_scan_stop(void) {
	if (!scan_running) {
		return;
	}
	scan_stop_asked = true;
	scan_cancel = true;
}

// The same, but it does not return until the scan thread has actually let go.
// The thread tests the flag once per directory entry, so this is a handful of
// milliseconds in practice; the ceiling is there so a wedged card reader can
// never hold the interface hostage.
static void library_scan_stop_and_wait(void) {
	if (!scan_running) {
		return;
	}
	scan_stop_asked = true;
	scan_cancel = true;

	for (int i = 0; i < 400 && scan_running; i++) {
		usleep(10 * 1000);
	}
	if (scan_running) {
		fprintf(stderr, "library: the scan did not stop in time; closing anyway\n");
	}
}

// ---------------------------------------------------------------------------
// Playback state: the single remembered track + position ("Ricorda traccia").
// ---------------------------------------------------------------------------

void library_queue_save_query(const library_index_spec_t *spec, int position, const char *const *extra,
							  const int *extra_slots, int extra_count) {
	pthread_mutex_lock(&db_lock);
	if (db) {
		// One transaction: this runs on every track change, and four separate
		// statements is four journal round trips to a card. A savepoint and not
		// BEGIN, because a scan holds a transaction open across the whole card
		// and a nested BEGIN would fail -- silently ending the scan's batch on
		// the COMMIT below.
		exec("SAVEPOINT queue_save");
		// The two ways of writing a queue down are exclusive: whichever is
		// stored last is the one the next boot restores, so the other has to go.
		exec("DELETE FROM PLAYBACK_QUEUE");
		exec("DELETE FROM PLAYBACK_QUEUE_STATE");
		exec("DELETE FROM PLAYBACK_QUEUE_QUERY");

		sqlite3_stmt *stmt = NULL;
		if (spec && spec->valid &&
			sqlite3_prepare_v2(db,
							   "INSERT INTO PLAYBACK_QUEUE_QUERY(id,kind,filter,ord,desc,value,pos,updated_at)"
							   " VALUES(0,?,?,?,?,?,?,strftime('%s','now'))",
							   -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_int(stmt, 1, (int)spec->kind);
			sqlite3_bind_int(stmt, 2, (int)spec->filter);
			sqlite3_bind_int(stmt, 3, (int)spec->order);
			sqlite3_bind_int(stmt, 4, spec->desc ? 1 : 0);
			sqlite3_bind_text(stmt, 5, spec->value, -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(stmt, 6, position);
			sqlite3_step(stmt);
			sqlite3_finalize(stmt);
		}

		// Whatever "add to queue" put there after the fact. A handful of rows
		// beside the query, rather than the two hundred thousand the query
		// stands for.
		stmt = NULL;
		if (extra && extra_count > 0 &&
			sqlite3_prepare_v2(db, "INSERT INTO PLAYBACK_QUEUE(idx,path,slot) VALUES(?,?,?)", -1, &stmt, NULL) ==
				SQLITE_OK) {
			for (int i = 0; i < extra_count; i++) {
				if (!extra[i] || !extra[i][0]) {
					continue;
				}
				sqlite3_reset(stmt);
				sqlite3_bind_int(stmt, 1, i);
				sqlite3_bind_text(stmt, 2, extra[i], -1, SQLITE_TRANSIENT);
				// Where it actually sits, so the restore puts it back there
				// instead of in front of playback.
				sqlite3_bind_int(stmt, 3, extra_slots ? extra_slots[i] : -1);
				sqlite3_step(stmt);
			}
			sqlite3_finalize(stmt);
		}
		exec("RELEASE queue_save");
	}
	pthread_mutex_unlock(&db_lock);
}

bool library_queue_load_query(library_index_spec_t *spec, int *position_out) {
	if (!spec) {
		return false;
	}
	memset(spec, 0, sizeof(*spec));

	bool found = false;
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT kind, filter, ord, desc, value, pos FROM PLAYBACK_QUEUE_QUERY WHERE id=0",
								 -1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			spec->kind = (library_list_t)sqlite3_column_int(stmt, 0);
			spec->filter = (library_filter_t)sqlite3_column_int(stmt, 1);
			spec->order = (library_order_t)sqlite3_column_int(stmt, 2);
			spec->desc = sqlite3_column_int(stmt, 3) != 0;
			const char *value = (const char *)sqlite3_column_text(stmt, 4);
			snprintf(spec->value, sizeof(spec->value), "%s", value ? value : "");
			if (position_out) {
				*position_out = sqlite3_column_int(stmt, 5);
			}
			spec->valid = true;
			found = true;
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return found;
}

// ---------------------------------------------------------------------------
// Playlist counts, and what makes one worth re-counting
//
// The number under a playlist's name is how many of its entries are actually on
// the card, and finding it out is one path lookup per entry -- several thousand
// for a page of full playlists, which on a microSD is seconds of the only core
// there is. So it is cached, keyed by the playlist file's own path, size and
// modification time: edit the playlist and the row stops matching.
//
// The stamp alone is not enough on its own, because deleting a track does not
// touch the .m3u that names it. What settles it is that the player cannot
// delete a track from the card: files go missing when the card is taken to a
// computer, and the card then comes back mounted again -- which is what this
// counter tracks. A row therefore needs re-checking when its stamp does not
// match, or when it was checked against an earlier mount than the one in the
// reader now.
//
// The counter lives on the card and not in a variable, because a variable
// starting from zero on every boot would let the second boot's first mount
// match rows written by the first boot's -- verified against a card that has
// been out of the device since.
// ---------------------------------------------------------------------------

static long long mount_serial;

static void next_mount_serial(void) {
	long long serial = 0;
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT serial FROM MOUNT_SERIAL WHERE id=0", -1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			serial = sqlite3_column_int64(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	serial++;
	stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO MOUNT_SERIAL(id,serial) VALUES(0,?)", -1, &stmt, NULL) ==
				  SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, serial);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	mount_serial = serial;
	pthread_mutex_unlock(&db_lock);
}


// The statement a playlist's table is made with. `idx` orders the rows and is
// what the user sees as the order of the list. `mount` is neither read nor
// written; it stays so every playlist table has the one shape.
static bool playlist_create_locked(const char *quoted) {
	char sql[PLAYLIST_TABLE_MAX + 200];
	snprintf(sql, sizeof(sql),
			 "CREATE TABLE IF NOT EXISTS %s(idx INTEGER PRIMARY KEY, path TEXT, title TEXT, artist TEXT,"
			 " seconds INT, present INT, mount INT)",
			 quoted);
	return exec(sql);
}

static bool playlist_table_exists_locked(const char *name) {
	char table[PLAYLIST_TABLE_MAX];
	if (!db) {
		return false;
	}
	// By value, not by name in the statement: this one can be bound.
	snprintf(table, sizeof(table), LIBRARY_PLAYLIST_PREFIX "%s", name);
	sqlite3_stmt *stmt = NULL;
	bool found = false;
	if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1, &stmt, NULL) ==
		SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, table, -1, SQLITE_TRANSIENT);
		found = sqlite3_step(stmt) == SQLITE_ROW;
		sqlite3_finalize(stmt);
	}
	return found;
}

bool library_playlist_exists(const char *name) {
	if (!name || !name[0]) {
		return false;
	}
	pthread_mutex_lock(&db_lock);
	bool found = playlist_table_exists_locked(name);
	pthread_mutex_unlock(&db_lock);
	return found;
}

int library_playlist_names(char ***names_out) {
	if (names_out) {
		*names_out = NULL;
	}
	char **names = NULL;
	int count = 0, capacity = 0;

	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	// The underscore in the prefix is a wildcard to LIKE, hence the escape:
	// without it "M3U_" would also match "M3UX...".
	if (db && sqlite3_prepare_v2(db,
								 "SELECT name FROM sqlite_master WHERE type='table'"
								 " AND name LIKE 'M3U\\_%' ESCAPE '\\' ORDER BY name COLLATE listorder",
								 -1, &stmt, NULL) == SQLITE_OK) {
		size_t prefix_len = strlen(LIBRARY_PLAYLIST_PREFIX);
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *table = (const char *)sqlite3_column_text(stmt, 0);
			if (!table || strlen(table) <= prefix_len) {
				continue;
			}
			if (count == capacity) {
				int grown = capacity ? capacity * 2 : 32;
				char **bigger = realloc(names, (size_t)grown * sizeof(*names));
				if (!bigger) {
					break;
				}
				names = bigger;
				capacity = grown;
			}
			names[count] = strdup(table + prefix_len);
			if (!names[count]) {
				break;
			}
			count++;
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);

	if (names_out) {
		*names_out = names;
	} else {
		library_playlist_names_free(names, count);
	}
	return count;
}

void library_playlist_names_free(char **names, int count) {
	if (!names) {
		return;
	}
	for (int i = 0; i < count; i++) {
		free(names[i]);
	}
	free(names);
}

bool library_playlist_create(const char *name) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (!playlist_table(name, quoted, sizeof(quoted))) {
		return false;
	}
	pthread_mutex_lock(&db_lock);
	bool ok = false;
	if (db && !playlist_table_exists_locked(name)) {
		ok = playlist_create_locked(quoted);
	}
	if (ok) {
		bump_generation(GEN_PLAYLISTS);
	}
	pthread_mutex_unlock(&db_lock);
	return ok;
}

bool library_playlist_drop(const char *name) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (!playlist_table(name, quoted, sizeof(quoted))) {
		return false;
	}
	pthread_mutex_lock(&db_lock);
	bool ok = false;
	if (db && playlist_table_exists_locked(name)) {
		char sql[PLAYLIST_TABLE_MAX + 32];
		snprintf(sql, sizeof(sql), "DROP TABLE %s", quoted);
		ok = exec(sql);
	}
	if (ok) {
		bump_generation(GEN_PLAYLISTS);
	}
	pthread_mutex_unlock(&db_lock);
	return ok;
}

bool library_playlist_rename(const char *name, const char *new_name) {
	char from[PLAYLIST_TABLE_MAX], to[PLAYLIST_TABLE_MAX];
	if (!playlist_table(name, from, sizeof(from)) || !playlist_table(new_name, to, sizeof(to))) {
		return false;
	}
	if (strcmp(name, new_name) == 0) {
		return true;
	}
	pthread_mutex_lock(&db_lock);
	bool ok = false;
	if (db && playlist_table_exists_locked(name) && !playlist_table_exists_locked(new_name)) {
		char sql[2 * PLAYLIST_TABLE_MAX + 32];
		snprintf(sql, sizeof(sql), "ALTER TABLE %s RENAME TO %s", from, to);
		ok = exec(sql);
	}
	if (ok) {
		bump_generation(GEN_PLAYLISTS);
	}
	pthread_mutex_unlock(&db_lock);
	return ok;
}

// One row in, with the table already made and the lock already held.
static bool playlist_append_locked(const char *quoted, const library_playlist_row_t *row) {
	char sql[PLAYLIST_TABLE_MAX + 160];
	snprintf(sql, sizeof(sql), "INSERT INTO %s(path,title,artist,seconds,present) VALUES(?,?,?,?,?)", quoted);

	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		return false;
	}
	sqlite3_bind_text(stmt, 1, row->path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, row->title, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, row->artist, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 4, row->seconds);
	sqlite3_bind_int(stmt, 5, row->present ? 1 : 0);
	bool ok = sqlite3_step(stmt) == SQLITE_DONE;
	sqlite3_finalize(stmt);
	return ok;
}

bool library_playlist_append(const char *name, const library_playlist_row_t *row) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (!row || !playlist_table(name, quoted, sizeof(quoted))) {
		return false;
	}
	pthread_mutex_lock(&db_lock);
	bool ok = db && playlist_create_locked(quoted) && playlist_append_locked(quoted, row);
	if (ok) {
		bump_generation(GEN_PLAYLISTS);
	}
	pthread_mutex_unlock(&db_lock);
	return ok;
}

// One row out of a statement that selected path,title,artist,seconds,present.
static void playlist_row_read(sqlite3_stmt *stmt, library_playlist_row_t *row) {
	const char *text = (const char *)sqlite3_column_text(stmt, 0);
	snprintf(row->path, sizeof(row->path), "%s", text ? text : "");
	text = (const char *)sqlite3_column_text(stmt, 1);
	snprintf(row->title, sizeof(row->title), "%s", text ? text : "");
	text = (const char *)sqlite3_column_text(stmt, 2);
	snprintf(row->artist, sizeof(row->artist), "%s", text ? text : "");
	row->seconds = (long)sqlite3_column_int64(stmt, 3);
	row->present = sqlite3_column_int(stmt, 4) != 0;
}

int library_playlist_page(const char *name, int offset, int count, library_playlist_row_t *out) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (!out || offset < 0 || count <= 0 || !playlist_table(name, quoted, sizeof(quoted))) {
		return 0;
	}

	char sql[PLAYLIST_TABLE_MAX + 384];
	if (snprintf(sql, sizeof(sql),
				 "SELECT p.path, " PLAYLIST_TITLE ", " PLAYLIST_ARTIST ", p.seconds, p.present FROM %s p"
				 " ORDER BY p.idx LIMIT ? OFFSET ?",
				 quoted) >= (int)sizeof(sql)) {
		return 0;
	}

	int got = 0;
	pthread_mutex_lock(&db_lock);
	if (db && playlist_table_exists_locked(name)) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_int(stmt, 1, count);
			sqlite3_bind_int(stmt, 2, offset);
			while (got < count && sqlite3_step(stmt) == SQLITE_ROW) {
				playlist_row_read(stmt, &out[got]);
				got++;
			}
			sqlite3_finalize(stmt);
		}
	}
	pthread_mutex_unlock(&db_lock);
	return got;
}

// How many rows there are at all, present or not: what a full read has to make
// room for, and not the number under the playlist's name.
static int playlist_row_total(const char *name) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (!playlist_table(name, quoted, sizeof(quoted))) {
		return 0;
	}
	pthread_mutex_lock(&db_lock);
	int count = 0;
	if (db && playlist_table_exists_locked(name)) {
		char sql[PLAYLIST_TABLE_MAX + 64];
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", quoted);
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				count = sqlite3_column_int(stmt, 0);
			}
			sqlite3_finalize(stmt);
		}
	}
	pthread_mutex_unlock(&db_lock);
	return count;
}

int library_playlist_rows(const char *name, library_playlist_row_t **rows_out) {
	if (rows_out) {
		*rows_out = NULL;
	}
	int total = playlist_row_total(name);
	if (total <= 0) {
		return 0;
	}
	library_playlist_row_t *rows = malloc((size_t)total * sizeof(*rows));
	if (!rows) {
		return 0;
	}
	int got = library_playlist_page(name, 0, total, rows);
	if (rows_out) {
		*rows_out = rows;
	} else {
		free(rows);
	}
	return got;
}

// ---------------------------------------------------------------------------
// Writing one, a row at a time
//
// An import resolves its entries one by one and a backup reads them back one by
// one, so neither has any reason to hold the whole playlist -- and at two
// kilobytes a row, holding five thousand of them is nine megabytes on a device
// with ten. The transaction stays: one commit for the lot, because a commit
// each would be one card write each.
//
// The lock is taken per row and given back between them, not held for the
// length of the write. That is not tidiness: what a caller does between two
// rows is work out what the next one says, and working that out means asking
// the index for a track's name -- which takes this same lock, which is not
// recursive. Held across the write, the first such question would be a deadlock.
// The scan works the same way for the same reason.
//
// A transaction that will not start -- a scan has one open, and this connection
// allows one at a time -- is not a failure: the rows go in one at a time
// instead, which is slower and just as correct.
// ---------------------------------------------------------------------------

struct library_playlist_writer {
	sqlite3_stmt *stmt;
	bool failed;
	bool owns_transaction;
};

library_playlist_writer_t *library_playlist_write_begin(const char *name) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (!playlist_table(name, quoted, sizeof(quoted))) {
		return NULL;
	}
	library_playlist_writer_t *writer = calloc(1, sizeof(*writer));
	if (!writer) {
		return NULL;
	}

	char sql[PLAYLIST_TABLE_MAX + 160];
	snprintf(sql, sizeof(sql), "INSERT INTO %s(path,title,artist,seconds,present) VALUES(?,?,?,?,?)", quoted);

	pthread_mutex_lock(&db_lock);
	if (!db || !playlist_create_locked(quoted) || sqlite3_prepare_v2(db, sql, -1, &writer->stmt, NULL) != SQLITE_OK) {
		pthread_mutex_unlock(&db_lock);
		free(writer);
		return NULL;
	}
	writer->owns_transaction = exec_quiet("BEGIN");
	pthread_mutex_unlock(&db_lock);
	return writer;
}

bool library_playlist_write_row(library_playlist_writer_t *writer, const library_playlist_row_t *row) {
	if (!writer || !row || writer->failed) {
		return false;
	}
	pthread_mutex_lock(&db_lock);
	sqlite3_reset(writer->stmt);
	sqlite3_bind_text(writer->stmt, 1, row->path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(writer->stmt, 2, row->title, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(writer->stmt, 3, row->artist, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(writer->stmt, 4, row->seconds);
	sqlite3_bind_int(writer->stmt, 5, row->present ? 1 : 0);
	bool ok = sqlite3_step(writer->stmt) == SQLITE_DONE;
	pthread_mutex_unlock(&db_lock);

	if (!ok) {
		writer->failed = true;
	}
	return ok;
}

bool library_playlist_write_end(library_playlist_writer_t *writer, bool keep) {
	if (!writer) {
		return false;
	}
	bool ok = keep && !writer->failed;
	pthread_mutex_lock(&db_lock);
	sqlite3_finalize(writer->stmt);
	if (writer->owns_transaction) {
		exec(ok ? "COMMIT" : "ROLLBACK");
	} else if (!ok) {
		// Nothing to roll back to, since every row went in on its own. The
		// half-written playlist is dropped instead, which is what a rollback
		// would have left behind.
		ok = false;
	}
	bump_generation(GEN_PLAYLISTS);
	pthread_mutex_unlock(&db_lock);
	free(writer);
	return ok;
}

bool library_playlist_append_all(const char *name, const library_playlist_row_t *rows, int count) {
	if (!rows || count < 0) {
		return false;
	}
	library_playlist_writer_t *writer = library_playlist_write_begin(name);
	if (!writer) {
		return false;
	}
	bool ok = true;
	for (int i = 0; i < count && ok; i++) {
		ok = library_playlist_write_row(writer, &rows[i]);
	}
	return library_playlist_write_end(writer, ok);
}

// Moves one entry of a playlist from one position to another.
//
// Positions, not row ids: `idx` is the primary key and orders the table, but it
// is not a rank. Removing an entry leaves a hole, so the entry at position 7 can
// perfectly well have idx 11, and the page the user is dragging on counts rows
// from the top.
//
// The set of idx values in the stretch between the two positions does not
// change -- only which row sits on which of them -- so the work is proportional
// to the distance dragged and not to the length of the list. The dragged row is
// parked on a key nothing else uses first, because updating straight onto an
// occupied primary key is a constraint failure, and the rows in between are
// shifted in the direction that always writes into a slot just vacated.
bool library_playlist_move(const char *name, int from, int to) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (from < 0 || to < 0 || from == to || !playlist_table(name, quoted, sizeof(quoted))) {
		return false;
	}

	char read_sql[PLAYLIST_TABLE_MAX + 160];
	char write_sql[PLAYLIST_TABLE_MAX + 80];
	// PLAYLIST_SHOWN, exactly as the list the user is dragging on is built
	// (list_sql). The page leaves out entries whose file is not on the card, so
	// its positions count only those; reading them back unfiltered would count
	// the missing ones too and move the wrong row.
	//
	// An entry that is not shown keeps its idx, so it is never moved and never
	// lost -- but it does not keep its neighbours either, because the shown ones
	// are permuted among the idx values they occupied and a hidden entry between
	// two of them can end up on the other side. There is no better answer: it is
	// an order between rows nobody is looking at.
	snprintf(read_sql, sizeof(read_sql), "SELECT p.idx FROM %s p WHERE " PLAYLIST_SHOWN " ORDER BY p.idx LIMIT ? OFFSET ?",
			 quoted);
	snprintf(write_sql, sizeof(write_sql), "UPDATE %s SET idx=? WHERE idx=?", quoted);

	int low = from < to ? from : to;
	int high = from < to ? to : from;
	int span = high - low + 1;

	pthread_mutex_lock(&db_lock);
	bool ok = db != NULL && playlist_table_exists_locked(name);

	// Only the stretch that moves is read back, which is what keeps a drag of
	// two rows from touching a list of ten thousand.
	int64_t *keys = ok ? calloc((size_t)span, sizeof(int64_t)) : NULL;
	int got = 0;
	if (keys) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db, read_sql, -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_int(stmt, 1, span);
			sqlite3_bind_int(stmt, 2, low);
			while (got < span && sqlite3_step(stmt) == SQLITE_ROW) {
				keys[got++] = sqlite3_column_int64(stmt, 0);
			}
			sqlite3_finalize(stmt);
		}
	}
	ok = keys != NULL && got == span;

	if (ok) {
		exec("BEGIN");
		sqlite3_stmt *stmt = NULL;
		ok = sqlite3_prepare_v2(db, write_sql, -1, &stmt, NULL) == SQLITE_OK;

		// A key below every real one: idx is INTEGER PRIMARY KEY and the table
		// is written with positive values only, so nothing can be sitting here.
		const int64_t parked = -1;
		int64_t moving = keys[from - low];

		if (ok) {
			sqlite3_bind_int64(stmt, 1, parked);
			sqlite3_bind_int64(stmt, 2, moving);
			ok = sqlite3_step(stmt) == SQLITE_DONE;
			sqlite3_reset(stmt);
		}

		// Dragged down: everything under it comes up one slot, taken in the
		// order that leaves the destination free. Dragged up: the mirror.
		if (from < to) {
			for (int i = from + 1; i <= to && ok; i++) {
				sqlite3_bind_int64(stmt, 1, keys[i - 1 - low]);
				sqlite3_bind_int64(stmt, 2, keys[i - low]);
				ok = sqlite3_step(stmt) == SQLITE_DONE;
				sqlite3_reset(stmt);
			}
		} else {
			for (int i = from - 1; i >= to && ok; i--) {
				sqlite3_bind_int64(stmt, 1, keys[i + 1 - low]);
				sqlite3_bind_int64(stmt, 2, keys[i - low]);
				ok = sqlite3_step(stmt) == SQLITE_DONE;
				sqlite3_reset(stmt);
			}
		}

		if (ok) {
			sqlite3_bind_int64(stmt, 1, keys[to - low]);
			sqlite3_bind_int64(stmt, 2, parked);
			ok = sqlite3_step(stmt) == SQLITE_DONE;
			sqlite3_reset(stmt);
		}

		sqlite3_finalize(stmt);
		exec(ok ? "COMMIT" : "ROLLBACK");
		if (ok) {
			bump_generation(GEN_PLAYLISTS);
		}
	}

	free(keys);
	pthread_mutex_unlock(&db_lock);
	return ok;
}

bool library_playlist_remove_path(const char *name, const char *path) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (!path || !path[0] || !playlist_table(name, quoted, sizeof(quoted))) {
		return false;
	}
	char sql[PLAYLIST_TABLE_MAX * 2 + 160];
	snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE idx=(SELECT idx FROM %s WHERE path=? ORDER BY idx LIMIT 1)",
			 quoted, quoted);

	pthread_mutex_lock(&db_lock);
	bool ok = false;
	if (db && playlist_table_exists_locked(name)) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
			ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0;
			sqlite3_finalize(stmt);
		}
	}
	if (ok) {
		bump_generation(GEN_PLAYLISTS);
	}
	pthread_mutex_unlock(&db_lock);
	return ok;
}

bool library_playlist_contains(const char *name, const char *path) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (!path || !path[0] || !playlist_table(name, quoted, sizeof(quoted))) {
		return false;
	}
	char sql[PLAYLIST_TABLE_MAX + 48];
	snprintf(sql, sizeof(sql), "SELECT 1 FROM %s WHERE path=? LIMIT 1", quoted);

	pthread_mutex_lock(&db_lock);
	bool found = false;
	if (db && playlist_table_exists_locked(name)) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
			found = sqlite3_step(stmt) == SQLITE_ROW;
			sqlite3_finalize(stmt);
		}
	}
	pthread_mutex_unlock(&db_lock);
	return found;
}

int library_playlist_remove_positions(const char *name, const int *positions, int count) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (!positions || count <= 0 || !playlist_table(name, quoted, sizeof(quoted))) {
		return 0;
	}
	// Positions counted as the page counts them, PLAYLIST_SHOWN (see
	// library_playlist_move). Every idx is read before anything is deleted,
	// since each deletion moves the positions after it.
	char read_sql[PLAYLIST_TABLE_MAX + 160];
	char delete_sql[PLAYLIST_TABLE_MAX + 40];
	snprintf(read_sql, sizeof(read_sql), "SELECT p.idx FROM %s p WHERE " PLAYLIST_SHOWN " ORDER BY p.idx LIMIT 1 OFFSET ?",
			 quoted);
	snprintf(delete_sql, sizeof(delete_sql), "DELETE FROM %s WHERE idx=?", quoted);

	int64_t *keys = calloc((size_t)count, sizeof(*keys));
	if (!keys) {
		return 0;
	}

	pthread_mutex_lock(&db_lock);
	int removed = 0;
	if (db && playlist_table_exists_locked(name)) {
		int found = 0;
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db, read_sql, -1, &stmt, NULL) == SQLITE_OK) {
			for (int i = 0; i < count; i++) {
				if (positions[i] < 0) {
					continue;
				}
				sqlite3_bind_int(stmt, 1, positions[i]);
				if (sqlite3_step(stmt) == SQLITE_ROW) {
					keys[found++] = sqlite3_column_int64(stmt, 0);
				}
				sqlite3_reset(stmt);
			}
			sqlite3_finalize(stmt);
		}

		exec("BEGIN");
		bool ok = sqlite3_prepare_v2(db, delete_sql, -1, &stmt, NULL) == SQLITE_OK;
		for (int i = 0; i < found && ok; i++) {
			sqlite3_bind_int64(stmt, 1, keys[i]);
			ok = sqlite3_step(stmt) == SQLITE_DONE;
			removed += ok && sqlite3_changes(db) > 0 ? 1 : 0;
			sqlite3_reset(stmt);
		}
		sqlite3_finalize(stmt);
		exec(ok ? "COMMIT" : "ROLLBACK");
		if (!ok) {
			removed = 0;
		}
		if (removed > 0) {
			bump_generation(GEN_PLAYLISTS);
		}
	}
	pthread_mutex_unlock(&db_lock);
	free(keys);
	return removed;
}

int library_playlist_count(const char *name) {
	char quoted[PLAYLIST_TABLE_MAX];
	if (!playlist_table(name, quoted, sizeof(quoted))) {
		return 0;
	}
	pthread_mutex_lock(&db_lock);
	int count = 0;
	if (db && playlist_table_exists_locked(name)) {
		char sql[PLAYLIST_TABLE_MAX + 160];
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s p WHERE " PLAYLIST_SHOWN, quoted);
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				count = sqlite3_column_int(stmt, 0);
			}
			sqlite3_finalize(stmt);
		}
	}
	pthread_mutex_unlock(&db_lock);
	return count;
}

bool library_playlists_migrated(void) {
	pthread_mutex_lock(&db_lock);
	bool done = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT done FROM PLAYLIST_MIGRATION WHERE id=0", -1, &stmt, NULL) == SQLITE_OK) {
		done = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) != 0;
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return done;
}

void library_playlists_set_migrated(void) {
	pthread_mutex_lock(&db_lock);
	if (db) {
		exec("INSERT OR REPLACE INTO PLAYLIST_MIGRATION(id,done) VALUES(0,1)");
	}
	pthread_mutex_unlock(&db_lock);
}

int library_playlist_count_get(const char *path, long mtime, long size) {
	if (!path || !path[0]) {
		return -1;
	}
	int tracks = -1;
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT tracks FROM PLAYLIST_COUNTS WHERE path=? AND mtime=? AND size=?", -1,
								 &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 2, mtime);
		sqlite3_bind_int64(stmt, 3, size);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			tracks = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return tracks;
}

bool library_playlist_count_is_verified(const char *path, long mtime, long size) {
	if (!path || !path[0]) {
		return false;
	}
	bool verified = false;
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT 1 FROM PLAYLIST_COUNTS WHERE path=? AND mtime=? AND size=? AND mount=?",
								 -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 2, mtime);
		sqlite3_bind_int64(stmt, 3, size);
		sqlite3_bind_int64(stmt, 4, mount_serial);
		verified = sqlite3_step(stmt) == SQLITE_ROW;
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return verified;
}

void library_playlist_count_save(const char *path, long mtime, long size, int tracks) {
	if (!path || !path[0] || tracks < 0) {
		return;
	}
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db &&
		sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO PLAYLIST_COUNTS(path,mtime,size,tracks,mount) VALUES(?,?,?,?,?)",
						   -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 2, mtime);
		sqlite3_bind_int64(stmt, 3, size);
		sqlite3_bind_int(stmt, 4, tracks);
		sqlite3_bind_int64(stmt, 5, mount_serial);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
}

void library_playback_state_save(const char *path, double position) {
	if (!path || !path[0]) {
		return;
	}
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db,
								 "INSERT OR REPLACE INTO PLAYBACK_STATE(id,path,position,updated_at)"
								 " VALUES(0,?,?,strftime('%s','now'))",
								 -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		sqlite3_bind_double(stmt, 2, position);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
}

bool library_playback_state_load(char *path_out, size_t path_size, double *position_out) {
	bool found = false;
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT path, position FROM PLAYBACK_STATE WHERE id=0", -1, &stmt,
								 NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *path = sqlite3_column_text(stmt, 0);
			if (path && path[0] && path_out) {
				snprintf(path_out, path_size, "%s", (const char *)path);
				if (position_out) {
					*position_out = sqlite3_column_double(stmt, 1);
				}
				found = true;
			}
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return found;
}

// ---------------------------------------------------------------------------
// The playback queue, mirrored on disk. A full rewrite is one transaction --
// thousands of separate inserts on a class-10 card would take seconds -- and
// the common case (the position moved) only touches the one state row.
// ---------------------------------------------------------------------------

void library_queue_save(const char *const *paths, int count, int current_index, bool custom) {
	pthread_mutex_lock(&db_lock);
	if (!db) {
		pthread_mutex_unlock(&db_lock);
		return;
	}

	sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
	sqlite3_exec(db, "DELETE FROM PLAYBACK_QUEUE", NULL, NULL, NULL);
	// The two forms are exclusive: a queue written out as rows has to retire
	// whichever query was standing, or the next boot would restore that instead
	// -- it is the one main.c looks at first.
	sqlite3_exec(db, "DELETE FROM PLAYBACK_QUEUE_QUERY", NULL, NULL, NULL);

	sqlite3_stmt *stmt = NULL;
	if (paths && count > 0 &&
		sqlite3_prepare_v2(db, "INSERT INTO PLAYBACK_QUEUE(idx,path) VALUES(?,?)", -1, &stmt, NULL) == SQLITE_OK) {
		for (int i = 0; i < count; i++) {
			if (!paths[i] || !paths[i][0]) {
				continue;
			}
			sqlite3_bind_int(stmt, 1, i);
			sqlite3_bind_text(stmt, 2, paths[i], -1, SQLITE_TRANSIENT);
			sqlite3_step(stmt);
			sqlite3_reset(stmt);
		}
		sqlite3_finalize(stmt);
	}

	stmt = NULL;
	if (sqlite3_prepare_v2(db,
						   "INSERT OR REPLACE INTO PLAYBACK_QUEUE_STATE(id,pos,custom,updated_at)"
						   " VALUES(0,?,?,strftime('%s','now'))",
						   -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int(stmt, 1, current_index);
		sqlite3_bind_int(stmt, 2, custom ? 1 : 0);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}

	sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
	pthread_mutex_unlock(&db_lock);
}

// Written beside whichever form holds the queue, and cleared when the queue
// stops being a record. Its own statement rather than a column in either table:
// the two saves delete each other's rows, and this outlives both.
void library_queue_save_album(const char *album) {
	pthread_mutex_lock(&db_lock);
	if (db) {
		if (album && album[0]) {
			sqlite3_stmt *stmt = NULL;
			if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO PLAYBACK_QUEUE_ALBUM(id,album) VALUES(0,?)", -1, &stmt,
								   NULL) == SQLITE_OK) {
				sqlite3_bind_text(stmt, 1, album, -1, SQLITE_TRANSIENT);
				sqlite3_step(stmt);
				sqlite3_finalize(stmt);
			}
		} else {
			sqlite3_exec(db, "DELETE FROM PLAYBACK_QUEUE_ALBUM", NULL, NULL, NULL);
		}
	}
	pthread_mutex_unlock(&db_lock);
}

bool library_queue_load_album(char *out, int size) {
	if (!out || size <= 0) {
		return false;
	}
	out[0] = '\0';

	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT album FROM PLAYBACK_QUEUE_ALBUM WHERE id=0", -1, &stmt, NULL) ==
				  SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *text = sqlite3_column_text(stmt, 0);
			if (text) {
				snprintf(out, (size_t)size, "%s", (const char *)text);
			}
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return out[0] != '\0';
}

void library_queue_save_index(int current_index) {
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "UPDATE PLAYBACK_QUEUE_STATE SET pos=?, updated_at=strftime('%s','now') WHERE id=0",
								 -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int(stmt, 1, current_index);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
}

int library_queue_load(char ***paths_out, int *index_out, bool *custom_out) {
	if (paths_out) {
		*paths_out = NULL;
	}
	if (index_out) {
		*index_out = 0;
	}
	if (custom_out) {
		*custom_out = false;
	}

	pthread_mutex_lock(&db_lock);
	if (!db) {
		pthread_mutex_unlock(&db_lock);
		return 0;
	}

	int count = 0;
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM PLAYBACK_QUEUE", -1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			count = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	if (count <= 0) {
		pthread_mutex_unlock(&db_lock);
		return 0;
	}

	char **list = calloc((size_t)count, sizeof(*list));
	if (!list) {
		pthread_mutex_unlock(&db_lock);
		return 0;
	}

	int filled = 0;
	stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT path FROM PLAYBACK_QUEUE ORDER BY idx", -1, &stmt, NULL) == SQLITE_OK) {
		while (filled < count && sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *path = sqlite3_column_text(stmt, 0);
			if (path && path[0]) {
				list[filled] = strdup((const char *)path);
				if (!list[filled]) {
					break;
				}
				filled++;
			}
		}
		sqlite3_finalize(stmt);
	}

	stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT pos, custom FROM PLAYBACK_QUEUE_STATE WHERE id=0", -1, &stmt, NULL) ==
		SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			if (index_out) {
				*index_out = sqlite3_column_int(stmt, 0);
			}
			if (custom_out) {
				*custom_out = sqlite3_column_int(stmt, 1) != 0;
			}
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);

	if (filled == 0) {
		free(list);
		return 0;
	}

	if (paths_out) {
		*paths_out = list;
	} else {
		library_queue_free(list, filled);
	}
	return filled;
}

void library_queue_free(char **paths, int count) {
	if (!paths) {
		return;
	}
	for (int i = 0; i < count; i++) {
		free(paths[i]);
	}
	free(paths);
}

int library_queue_load_extra(char ***paths_out, int **slots_out) {
	if (paths_out) {
		*paths_out = NULL;
	}
	if (slots_out) {
		*slots_out = NULL;
	}

	pthread_mutex_lock(&db_lock);
	if (!db) {
		pthread_mutex_unlock(&db_lock);
		return 0;
	}

	int count = 0;
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM PLAYBACK_QUEUE", -1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			count = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	if (count <= 0) {
		pthread_mutex_unlock(&db_lock);
		return 0;
	}

	char **list = calloc((size_t)count, sizeof(*list));
	int *slots = calloc((size_t)count, sizeof(*slots));
	if (!list || !slots) {
		free(list);
		free(slots);
		pthread_mutex_unlock(&db_lock);
		return 0;
	}

	// By slot, not by idx: they go back into the queue in the order they appear
	// in it, and each insert shifts everything after it up by one -- so the
	// later ones are only right if the earlier ones are already in place. A row
	// with no slot (a card written before the column existed) sorts last and is
	// appended after the current track.
	int filled = 0;
	stmt = NULL;
	if (sqlite3_prepare_v2(db,
						   "SELECT path, IFNULL(slot,-1) FROM PLAYBACK_QUEUE"
						   " ORDER BY (IFNULL(slot,-1) < 0), IFNULL(slot,-1), idx",
						   -1, &stmt, NULL) == SQLITE_OK) {
		while (filled < count && sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *path = sqlite3_column_text(stmt, 0);
			if (!path || !path[0]) {
				continue;
			}
			list[filled] = strdup((const char *)path);
			if (!list[filled]) {
				break;
			}
			slots[filled] = sqlite3_column_int(stmt, 1);
			filled++;
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);

	if (filled == 0) {
		free(list);
		free(slots);
		return 0;
	}

	if (paths_out) {
		*paths_out = list;
	} else {
		library_queue_free(list, filled);
	}
	if (slots_out) {
		*slots_out = slots;
	} else {
		free(slots);
	}
	return filled;
}

// ---------------------------------------------------------------------------
// Search: names LIKE %query% across tracks, albums and artists, capped per
// category -- the search page's data source.
// ---------------------------------------------------------------------------

int library_search(const char *query, int per_category, library_search_cb_t cb, void *user) {
	if (!query || !query[0] || !cb) {
		return 0;
	}

	char like[2 * 256 + 3];
	if (!search_pattern(query, like, sizeof(like))) {
		return 0;
	}

	int total = 0;

	pthread_mutex_lock(&db_lock);
	if (db) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db,
							   "SELECT name, path FROM MEDIA_TABLE WHERE foldcase(name) LIKE ? ESCAPE '\\'"
							   " ORDER BY name LIMIT ?",
							   -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, like, -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(stmt, 2, per_category);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *name = (const char *)sqlite3_column_text(stmt, 0);
				const char *path = (const char *)sqlite3_column_text(stmt, 1);
				cb(LIBRARY_SEARCH_TRACK, name ? name : "", path ? path : "", user);
				total++;
			}
			sqlite3_finalize(stmt);
		}
		// The album's first track rides along as the `path`, so the search page
		// can load its artwork the same way the album list does.
		if (sqlite3_prepare_v2(db,
							   "SELECT " ALBUM_GROUP_VALUE ", " ALBUM_GROUP_FIRST("m.path")
							   " FROM ALBUM_GROUP_TABLE g WHERE foldcase(album) LIKE ?"
							   " ESCAPE '\\' ORDER BY album LIMIT ?",
							   -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, like, -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(stmt, 2, per_category);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *name = (const char *)sqlite3_column_text(stmt, 0);
				const char *path = (const char *)sqlite3_column_text(stmt, 1);
				if (name && name[0]) {
					cb(LIBRARY_SEARCH_ALBUM, name, path ? path : "", user);
					total++;
				}
			}
			sqlite3_finalize(stmt);
		}
		if (sqlite3_prepare_v2(db,
							   "SELECT artist FROM ARTIST_TABLE WHERE foldcase(artist) LIKE ? ESCAPE '\\'"
							   " ORDER BY artist LIMIT ?",
							   -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, like, -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(stmt, 2, per_category);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *name = (const char *)sqlite3_column_text(stmt, 0);
				if (name && name[0]) {
					cb(LIBRARY_SEARCH_ARTIST, name, NULL, user);
					total++;
				}
			}
			sqlite3_finalize(stmt);
		}
	}
	pthread_mutex_unlock(&db_lock);
	return total;
}
