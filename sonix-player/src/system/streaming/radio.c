#define _GNU_SOURCE 1 // strcasestr
#include "radio.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/audio/audio.h"
#include "src/system/audio/eq.h"
#include "src/system/core/bootprof.h"
#include "src/system/core/config.h"
#include "src/system/decode/aacdec.h"
#include "src/system/net/hls.h"
#include "src/system/net/http.h"
#include "src/system/core/lang.h"
#include "src/system/net/tls.h"
#include "src/system/streaming/onair.h"
#include "src/system/streaming/radioflac.h"
#include "src/system/playback/playhistory.h"
#include "src/system/core/utils.h"

// The MP3 frame decoder on its own -- no file, no seeking, no length. This is
// minimp3's core, which dr_mp3 carries; the implementation is compiled once in
// decode.c, so only the declarations are wanted here.
#include "src/system/decode/dr_mp3.h"

// ---------------------------------------------------------------------------
// The directory server
//
// radio-browser.info is a set of mirrors behind one name. There is a DNS-based
// way to pick one, but a plain list tried in order is enough for a player that
// makes a handful of requests a session, and it works the same on a device
// whose resolver only does A records.
// ---------------------------------------------------------------------------

static const char *const API_HOSTS[] = {
	"de2.api.radio-browser.info",
	"de1.api.radio-browser.info",
	"all.api.radio-browser.info",
};
#define API_HOST_COUNT ((int)(sizeof(API_HOSTS) / sizeof(API_HOSTS[0])))

// Which mirror answered last. Tried first next time, so a session that got
// through once keeps talking to the same machine.
static int api_host_index;

#define API_TIMEOUT_SECS 12
#define API_MAX_BYTES (1024 * 1024)

#define RADIO_MAX_TERMS 300
#define RADIO_MAX_STATIONS RADIO_PAGE_SIZE

// ---------------------------------------------------------------------------
// A very small JSON reader
//
// The API answers with an array of flat objects: every value is a string, a
// number or a bool, and nothing nests. That is a small enough language to read
// directly, and it saves carrying a parser for a grammar nothing here uses.
// ---------------------------------------------------------------------------

// Walks a string literal starting at the opening quote and returns the
// position just past the closing one, honouring backslash escapes.
static const char *skip_string(const char *p) {
	p++; // the opening quote
	while (*p) {
		if (*p == '\\' && p[1]) {
			p += 2;
			continue;
		}
		if (*p == '"') {
			return p + 1;
		}
		p++;
	}
	return p;
}

// The next top-level object in an array, or NULL. On return *end points just
// past its closing brace.
static const char *next_object(const char *p, const char **end) {
	while (*p && *p != '{') {
		if (*p == '"') {
			p = skip_string(p);
			continue;
		}
		p++;
	}
	if (!*p) {
		return NULL;
	}

	const char *start = p;
	int depth = 0;
	while (*p) {
		if (*p == '"') {
			p = skip_string(p);
			continue;
		}
		if (*p == '{') {
			depth++;
		} else if (*p == '}') {
			depth--;
			if (depth == 0) {
				*end = p + 1;
				return start;
			}
		}
		p++;
	}
	return NULL;
}

// Appends one code point as UTF-8. Returns how many bytes it wrote.
static int utf8_put(char *out, size_t room, unsigned cp) {
	if (cp < 0x80 && room >= 1) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800 && room >= 2) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000 && room >= 3) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	if (room >= 4) {
		out[0] = (char)(0xF0 | (cp >> 18));
		out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[3] = (char)(0x80 | (cp & 0x3F));
		return 4;
	}
	return 0;
}

static unsigned hex4(const char *p) {
	unsigned v = 0;
	for (int i = 0; i < 4; i++) {
		char c = p[i];
		v <<= 4;
		if (c >= '0' && c <= '9') {
			v |= (unsigned)(c - '0');
		} else if (c >= 'a' && c <= 'f') {
			v |= (unsigned)(c - 'a' + 10);
		} else if (c >= 'A' && c <= 'F') {
			v |= (unsigned)(c - 'A' + 10);
		} else {
			return 0;
		}
	}
	return v;
}

// Unescapes a JSON string literal (from the opening quote) into out.
static void unescape_into(const char *p, char *out, size_t out_size) {
	size_t o = 0;
	p++; // opening quote

	while (*p && *p != '"' && o + 1 < out_size) {
		if (*p != '\\') {
			out[o++] = *p++;
			continue;
		}

		p++;
		switch (*p) {
		case 'n':
			out[o++] = '\n';
			p++;
			break;
		case 't':
			out[o++] = '\t';
			p++;
			break;
		case 'r':
			p++;
			break; // a CR in a station name is noise
		case 'b':
		case 'f':
			p++;
			break;
		case 'u': {
			unsigned cp = hex4(p + 1);
			p += 5;
			// A surrogate pair is two escapes; join them or the name comes
			// out as two replacement characters.
			if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
				unsigned low = hex4(p + 2);
				if (low >= 0xDC00 && low <= 0xDFFF) {
					cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
					p += 6;
				}
			}
			o += (size_t)utf8_put(out + o, out_size - o - 1, cp);
			break;
		}
		case '\0':
			break;
		default:
			out[o++] = *p++;
			break;
		}
	}
	out[o] = '\0';
}

// Finds `"key":` inside one object and returns the value position, or NULL.
static const char *find_value(const char *obj, const char *obj_end, const char *key) {
	size_t key_len = strlen(key);
	const char *p = obj + 1;

	while (p < obj_end) {
		if (*p != '"') {
			p++;
			continue;
		}
		const char *name = p;
		const char *after = skip_string(p);

		// A name is a key only when a colon follows it.
		const char *colon = after;
		while (colon < obj_end && (*colon == ' ' || *colon == '\t' || *colon == '\n')) {
			colon++;
		}
		if (colon < obj_end && *colon == ':') {
			if ((size_t)(after - name - 2) == key_len && strncmp(name + 1, key, key_len) == 0) {
				const char *v = colon + 1;
				while (v < obj_end && (*v == ' ' || *v == '\t' || *v == '\n')) {
					v++;
				}
				return v;
			}
			// Skip the value so its contents cannot be mistaken for keys.
			const char *v = colon + 1;
			while (v < obj_end && (*v == ' ' || *v == '\t' || *v == '\n')) {
				v++;
			}
			p = (*v == '"') ? skip_string(v) : v;
			continue;
		}
		p = after;
	}
	return NULL;
}

static void json_string(const char *obj, const char *obj_end, const char *key, char *out, size_t out_size) {
	out[0] = '\0';
	const char *v = find_value(obj, obj_end, key);
	if (!v) {
		return;
	}
	if (*v == '"') {
		unescape_into(v, out, out_size);
		return;
	}
	// A number where a string was expected (stationcount comes back both ways
	// depending on the endpoint) -- copy it verbatim.
	size_t n = 0;
	while (v[n] && n + 1 < out_size && v[n] != ',' && v[n] != '}' && v[n] != ' ') {
		n++;
	}
	memcpy(out, v, n);
	out[n] = '\0';
}

static int json_int(const char *obj, const char *obj_end, const char *key) {
	char buf[32];
	json_string(obj, obj_end, key, buf, sizeof(buf));
	return atoi(buf);
}

// ---------------------------------------------------------------------------
// Fetching
// ---------------------------------------------------------------------------

// GETs `path` from whichever mirror answers, trying them in order from the one
// that worked last.
//
// [radio] api_host in the config is tried first when it is set. The directory
// has changed which machines it answers on before, and a player that can be
// pointed at the new one by editing a line beats a player that needs a new
// build.
static char *api_get(const char *path) {
	const char *override = config_get("radio", "api_host", "");
	if (override && *override) {
		char url[1400];
		snprintf(url, sizeof(url), "http://%s%s", override, path);

		char *body = NULL;
		if (http_get(url, &body, NULL, API_MAX_BYTES, API_TIMEOUT_SECS)) {
			return body;
		}
		fprintf(stderr, "radio: %s (from the config) did not answer\n", override);
	}

	for (int i = 0; i < API_HOST_COUNT; i++) {
		int index = (api_host_index + i) % API_HOST_COUNT;

		char url[1400];
		snprintf(url, sizeof(url), "http://%s%s", API_HOSTS[index], path);

		char *body = NULL;
		if (http_get(url, &body, NULL, API_MAX_BYTES, API_TIMEOUT_SECS)) {
			api_host_index = index;
			return body;
		}
		fprintf(stderr, "radio: %s did not answer\n", API_HOSTS[index]);
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// Worker state
// ---------------------------------------------------------------------------

typedef enum {
	JOB_NONE = 0,
	JOB_TERMS,
	JOB_STATIONS,
	JOB_SEARCH,
} job_kind_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_wake = PTHREAD_COND_INITIALIZER;

static job_kind_t pending_kind;
static radio_browse_t pending_browse;
static char pending_value[128];
static int pending_offset;
static bool worker_started;

static radio_job_t job_state = RADIO_JOB_IDLE;
static unsigned job_serial;

static radio_term_t result_terms[RADIO_MAX_TERMS];
static int result_term_count;
static radio_station_t result_stations[RADIO_MAX_STATIONS];
static int result_station_count;

// How many station objects the server sent, including the ones parse_stations
// dropped. The page needs this rather than the kept count to tell whether the
// page was full (and so whether to ask for another) and to work out the offset
// of the next one.
static int result_raw_count;

// ---------------------------------------------------------------------------
// Country names
//
// The directory names countries the ISO 3166 way: "The Russian Federation",
// "The United States Of America", "Korea, Republic Of". Sorted as they come,
// Russia sits under T and both Koreas under K with nothing to tell them
// apart. The label shown and sorted on is the everyday name instead: from the
// ISO code where the official one is not what anybody looks for, otherwise the
// official one without a leading "The" and without anything after a comma or
// an opening bracket. The search still sends the directory's own name.
// ---------------------------------------------------------------------------

static const struct {
	const char code[3];
	const char *name;
} COUNTRY_NAMES[] = {
	{"AE", "United Arab Emirates"}, {"BN", "Brunei"},		  {"BO", "Bolivia"},
	{"BQ", "Caribbean Netherlands"}, {"BS", "Bahamas"},		  {"CD", "DR Congo"},
	{"CF", "Central African Republic"}, {"CG", "Congo"},	  {"CV", "Cape Verde"},
	{"CZ", "Czechia"},				{"DO", "Dominican Republic"}, {"FK", "Falkland Islands"},
	{"FM", "Micronesia"},			{"FO", "Faroe Islands"},  {"GB", "United Kingdom"},
	{"GM", "Gambia"},				{"HK", "Hong Kong"},	  {"IR", "Iran"},
	{"KM", "Comoros"},				{"KP", "North Korea"},	  {"KR", "South Korea"},
	{"KY", "Cayman Islands"},		{"LA", "Laos"},			  {"MD", "Moldova"},
	{"MF", "Saint Martin"},			{"MH", "Marshall Islands"}, {"MK", "North Macedonia"},
	{"MO", "Macao"},				{"MV", "Maldives"},		  {"NE", "Niger"},
	{"NL", "Netherlands"},			{"PH", "Philippines"},	  {"PS", "Palestine"},
	{"RU", "Russia"},				{"SD", "Sudan"},		  {"SY", "Syria"},
	{"SZ", "Eswatini"},				{"TC", "Turks and Caicos Islands"}, {"TL", "Timor-Leste"},
	{"TW", "Taiwan"},				{"TZ", "Tanzania"},		  {"US", "United States"},
	{"VA", "Vatican City"},			{"VE", "Venezuela"},	  {"VG", "British Virgin Islands"},
	{"VI", "US Virgin Islands"},	{"VN", "Vietnam"},
};

static void country_label(const char *name, const char *code, char *out, size_t size) {
	for (size_t i = 0; i < sizeof(COUNTRY_NAMES) / sizeof(COUNTRY_NAMES[0]); i++) {
		if (strcasecmp(code, COUNTRY_NAMES[i].code) == 0) {
			snprintf(out, size, "%s", COUNTRY_NAMES[i].name);
			return;
		}
	}

	if (strncasecmp(name, "the ", 4) == 0) {
		name += 4;
	}
	snprintf(out, size, "%s", name);
	char *cut = strpbrk(out, ",(");
	if (cut && cut > out) {
		*cut = '\0';
		for (char *e = cut - 1; e >= out && *e == ' '; e--) {
			*e = '\0';
		}
	}
}

// Parses a terms response ("[{\"name\":\"italian\",\"stationcount\":123}, ...]").
static int parse_terms(const char *json, radio_browse_t kind, radio_term_t *out, int max) {
	int count = 0;
	const char *p = json;
	const char *end;

	while (count < max) {
		const char *obj = next_object(p, &end);
		if (!obj) {
			break;
		}
		p = end;

		radio_term_t *t = &out[count];
		memset(t, 0, sizeof(*t));

		json_string(obj, end, "name", t->name, sizeof(t->name));
		if (!t->name[0]) {
			continue;
		}
		t->stationcount = json_int(obj, end, "stationcount");

		// The lists come back lower case; the label carries a leading capital.
		if (kind == RADIO_BROWSE_COUNTRIES) {
			char code[8];
			json_string(obj, end, "iso_3166_1", code, sizeof(code));
			country_label(t->name, code, t->label, sizeof(t->label));
		} else {
			snprintf(t->label, sizeof(t->label), "%s", t->name);
		}
		if (t->label[0] >= 'a' && t->label[0] <= 'z') {
			t->label[0] = (char)(t->label[0] - 'a' + 'A');
		}

		count++;
	}

	return count;
}

// `raw_out`, when given, receives how many objects were in the response,
// including the ones dropped.
static int parse_stations(const char *json, radio_station_t *out, int max, int *raw_out) {
	int count = 0;
	int raw = 0;
	const char *p = json;
	const char *end;

	while (count < max) {
		const char *obj = next_object(p, &end);
		if (!obj) {
			break;
		}
		p = end;
		raw++;

		radio_station_t *s = &out[count];
		memset(s, 0, sizeof(*s));

		json_string(obj, end, "stationuuid", s->uuid, sizeof(s->uuid));
		json_string(obj, end, "name", s->name, sizeof(s->name));

		// url_resolved whenever there is one: it is what the directory itself
		// plays, with the redirects followed and the playlist unwrapped. `url`
		// is the address the submitter typed and is not always the same stream,
		// so it is only a fallback for entries that have no url_resolved.
		json_string(obj, end, "url_resolved", s->url, sizeof(s->url));
		if (!s->url[0]) {
			json_string(obj, end, "url", s->url, sizeof(s->url));
		}

		json_string(obj, end, "favicon", s->favicon, sizeof(s->favicon));
		json_string(obj, end, "countrycode", s->country, sizeof(s->country));
		json_string(obj, end, "tags", s->tags, sizeof(s->tags));
		json_string(obj, end, "codec", s->codec, sizeof(s->codec));
		s->bitrate = json_int(obj, end, "bitrate");

		// Only the nameless and the address-less are dropped. Everything else
		// the directory lists is kept, including codecs and schemes this device
		// may not be able to open: radio_station_problem() explains those where
		// the user can see them, which beats hiding the station entirely.
		if (!s->name[0] || !s->url[0]) {
			continue;
		}

		// Trim the trailing whitespace the directory is full of.
		for (int i = (int)strlen(s->name) - 1; i >= 0 && (s->name[i] == ' ' || s->name[i] == '\t'); i--) {
			s->name[i] = '\0';
		}

		count++;
	}
	// Anything past `max` is still counted: the caller has to know the page
	// was full even when it had no room for all of it.
	while (next_object(p, &end)) {
		p = end;
		raw++;
	}

	if (raw_out) {
		*raw_out = raw;
	}
	return count;
}

static int term_by_label(const void *a, const void *b) {
	const radio_term_t *x = a;
	const radio_term_t *y = b;
	int order = strcasecmp(x->label, y->label);
	return order ? order : strcmp(x->name, y->name);
}

// The directory is asked for the 250 terms with the most stations behind them,
// so the cut is by popularity. Languages and countries are then re-sorted
// alphabetically, because that is how a person scans for a known name. Genres
// keep the popularity order: an alphabetical tag list opens on "00s".
static void sort_terms(radio_browse_t kind, radio_term_t *terms, int count) {
	if (kind != RADIO_BROWSE_LANGUAGES && kind != RADIO_BROWSE_COUNTRIES) {
		return;
	}
	qsort(terms, (size_t)count, sizeof(*terms), term_by_label);
}

static void run_terms_job(radio_browse_t kind) {
	const char *path = NULL;
	switch (kind) {
	case RADIO_BROWSE_LANGUAGES:
		path = "/json/languages?order=stationcount&reverse=true&limit=250";
		break;
	case RADIO_BROWSE_COUNTRIES:
		// /json/countries, not /json/countrycodes: the codes are exact and
		// easy to search but "IT" and "DE" are not a list anyone wants to
		// read. The names are the directory's own English ones, which is the
		// same vocabulary the languages and the genres come back in.
		path = "/json/countries?order=stationcount&reverse=true&limit=250";
		break;
	case RADIO_BROWSE_GENRES:
	default:
		path = "/json/tags?order=stationcount&reverse=true&limit=250";
		break;
	}

	char *body = api_get(path);
	if (!body) {
		pthread_mutex_lock(&job_lock);
		job_state = RADIO_JOB_FAILED;
		job_serial++;
		pthread_mutex_unlock(&job_lock);
		return;
	}

	radio_term_t *terms = calloc(RADIO_MAX_TERMS, sizeof(*terms));
	int count = terms ? parse_terms(body, kind, terms, RADIO_MAX_TERMS) : 0;
	free(body);
	if (terms) {
		sort_terms(kind, terms, count);
	}

	pthread_mutex_lock(&job_lock);
	result_term_count = count;
	result_station_count = 0;
	if (terms) {
		memcpy(result_terms, terms, sizeof(*terms) * (size_t)count);
	}
	job_state = terms ? RADIO_JOB_OK : RADIO_JOB_FAILED;
	job_serial++;
	pthread_mutex_unlock(&job_lock);

	free(terms);
}

// Hands the parsed result over and wakes the page. Shared by the two jobs
// that produce stations.
static void publish_stations(char *body) {
	radio_station_t *parsed = body ? calloc(RADIO_MAX_STATIONS, sizeof(*parsed)) : NULL;
	int raw = 0;
	int count = parsed ? parse_stations(body, parsed, RADIO_MAX_STATIONS, &raw) : 0;
	free(body);

	pthread_mutex_lock(&job_lock);
	result_raw_count = raw;
	result_station_count = count;
	result_term_count = 0;
	if (parsed) {
		memcpy(result_stations, parsed, sizeof(*parsed) * (size_t)count);
	}
	job_state = parsed ? RADIO_JOB_OK : RADIO_JOB_FAILED;
	job_serial++;
	pthread_mutex_unlock(&job_lock);

	free(parsed);
}

static void run_search_job(const char *text, int offset) {
	char encoded[512];
	http_url_encode(text, encoded, sizeof(encoded));

	// `name` (not nameExact): the user is typing part of a station's name,
	// which is exactly the case the loose form is for.
	char path[1400];
	// No hidebroken: it defaults to false, and the directory's own website does
	// not set it either. It drops every station whose last automated check
	// failed, which is a large share of ones that play perfectly well.
	snprintf(path, sizeof(path),
			 "/json/stations/search?name=%s"
			 "&order=clickcount&reverse=true&limit=%d&offset=%d",
			 encoded, RADIO_PAGE_SIZE, offset);

	char *body = api_get(path);
	if (!body) {
		snprintf(path, sizeof(path),
				 "/json/stations/byname/%s?order=clickcount&reverse=true&limit=%d&offset=%d",
				 encoded, RADIO_PAGE_SIZE, offset);
		body = api_get(path);
	}
	publish_stations(body);
}

static void run_stations_job(radio_browse_t browse, const char *value, int offset) {
	char encoded[512];
	http_url_encode(value, encoded, sizeof(encoded));

	// Every browse goes through /search rather than the by-language/by-tag
	// paths, because only /search takes the exact-match flag as a parameter.
	const char *field = "language";
	if (browse == RADIO_BROWSE_COUNTRIES) {
		field = "country";
	} else if (browse == RADIO_BROWSE_GENRES) {
		field = "tag";
	}

	// Two parameters, not one. `<field>Exact` is a BOOLEAN saying how `<field>`
	// is to be matched; it does not carry the value. The value goes in
	// `<field>`, and the flag asks for an exact match rather than a substring
	// -- without it "tag=rock" also drags in "punk rock" and "rockabilly".
	char path[1400];
	snprintf(path, sizeof(path),
			 "/json/stations/search?%s=%s&%sExact=true"
			 "&order=clickcount&reverse=true&limit=%d&offset=%d",
			 field, encoded, field, RADIO_PAGE_SIZE, offset);

	char *body = api_get(path);

	if (!body) {
		// The by-<field>exact paths, as a second chance. /search and these are
		// different code paths on the server, so a mirror that is unhappy with
		// one may well answer the other.
		const char *simple = "bylanguageexact";
		if (browse == RADIO_BROWSE_COUNTRIES) {
			simple = "bycountryexact";
		} else if (browse == RADIO_BROWSE_GENRES) {
			simple = "bytagexact";
		}
		snprintf(path, sizeof(path),
				 "/json/stations/%s/%s?order=clickcount&reverse=true&limit=%d&offset=%d",
				 simple, encoded, RADIO_PAGE_SIZE, offset);
		fprintf(stderr, "radio: the search endpoint did not answer; trying /%s\n", simple);
		body = api_get(path);
	}

	publish_stations(body);
}

static void *worker_main(void *arg) {
	(void)arg;
	thread_be_background("radio directory");

	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (pending_kind == JOB_NONE) {
			pthread_cond_wait(&job_wake, &job_lock);
		}
		job_kind_t kind = pending_kind;
		radio_browse_t browse = pending_browse;
		int offset = pending_offset;
		char value[128];
		snprintf(value, sizeof(value), "%s", pending_value);
		pending_kind = JOB_NONE;
		job_state = RADIO_JOB_BUSY;
		pthread_mutex_unlock(&job_lock);

		if (kind == JOB_TERMS) {
			run_terms_job(browse);
		} else if (kind == JOB_SEARCH) {
			run_search_job(value, offset);
		} else {
			run_stations_job(browse, value, offset);
		}
	}
	return NULL;
}

static void queue_job(job_kind_t kind, radio_browse_t browse, const char *value, int offset) {
	pthread_mutex_lock(&job_lock);
	pending_kind = kind;
	pending_browse = browse;
	pending_offset = offset < 0 ? 0 : offset;
	snprintf(pending_value, sizeof(pending_value), "%s", value ? value : "");
	job_state = RADIO_JOB_BUSY;
	pthread_cond_signal(&job_wake);
	pthread_mutex_unlock(&job_lock);
}

void radio_request_terms(radio_browse_t kind) { queue_job(JOB_TERMS, kind, NULL, 0); }

void radio_request_stations(radio_browse_t kind, const char *value, int offset) {
	queue_job(JOB_STATIONS, kind, value, offset);
}

void radio_request_search(const char *text, int offset) {
	queue_job(JOB_SEARCH, RADIO_BROWSE_LANGUAGES, text, offset);
}

radio_job_t radio_job_state(void) {
	pthread_mutex_lock(&job_lock);
	radio_job_t s = job_state;
	pthread_mutex_unlock(&job_lock);
	return s;
}

unsigned radio_job_serial(void) {
	pthread_mutex_lock(&job_lock);
	unsigned s = job_serial;
	pthread_mutex_unlock(&job_lock);
	return s;
}

int radio_get_terms(radio_term_t *out, int max) {
	pthread_mutex_lock(&job_lock);
	int n = result_term_count < max ? result_term_count : max;
	if (n > 0) {
		memcpy(out, result_terms, sizeof(*out) * (size_t)n);
	}
	pthread_mutex_unlock(&job_lock);
	return n;
}

int radio_last_raw_count(void) {
	pthread_mutex_lock(&job_lock);
	int n = result_raw_count;
	pthread_mutex_unlock(&job_lock);
	return n;
}

int radio_get_stations(radio_station_t *out, int max) {
	pthread_mutex_lock(&job_lock);
	int n = result_station_count < max ? result_station_count : max;
	if (n > 0) {
		memcpy(out, result_stations, sizeof(*out) * (size_t)n);
	}
	pthread_mutex_unlock(&job_lock);
	return n;
}

const char *radio_station_problem(const radio_station_t *station) {
	if (!station || !station->url[0]) {
		return tr("radio_no_address");
	}
	if (strncasecmp(station->url, "https://", 8) == 0) {
		// Playable only when OpenSSL loaded and a certificate bundle was found.
		if (!tls_available()) {
			return tr("radio_https_unsupported");
		}
		return NULL;
	}
	if (strncasecmp(station->url, "http://", 7) != 0) {
		return tr("radio_bad_address");
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// The store
//
// <card>/.local/radio.db, an SQLite file next to the music library's own.
// Two tables: the stations the user starred, and the last few he played.
//
// On the card rather than beside the config because that is where this
// player keeps the things it builds -- library.db and audiobooks.db are in
// the same folder -- and because a list of radio stations is the user's data,
// not the device's settings.
//
// Every call here is safe with the database closed (no card, or a card that
// could not be written): they do nothing and say so by returning zero.
// ---------------------------------------------------------------------------

#include "src/system/db/sqlite3.h"

static pthread_mutex_t store_lock = PTHREAD_MUTEX_INITIALIZER;
static sqlite3 *store;
// The file the store was opened from, and which file that was.
static char store_path[512];
static file_identity_t store_identity;

static const char *const STORE_SCHEMA[] = {
	"CREATE TABLE IF NOT EXISTS FAVOURITES ("
	" uuid TEXT PRIMARY KEY, name TEXT, url TEXT, favicon TEXT,"
	" codec TEXT, bitrate INTEGER, country TEXT, tags TEXT,"
	" added INTEGER)",

	// One row per station, not one per play: the same station listened to
	// twice is one entry that moves back to the top, which is what "recent"
	// means to a person.
	"CREATE TABLE IF NOT EXISTS RECENT ("
	" uuid TEXT PRIMARY KEY, name TEXT, url TEXT, favicon TEXT,"
	" codec TEXT, bitrate INTEGER, country TEXT, tags TEXT,"
	" played INTEGER)",

	"CREATE INDEX IF NOT EXISTS RECENT_PLAYED ON RECENT(played DESC)",
};

static bool store_exec(const char *sql) {
	if (!store) {
		return false;
	}
	char *error = NULL;
	if (sqlite3_exec(store, sql, NULL, NULL, &error) != SQLITE_OK) {
		fprintf(stderr, "radio: %s (%s)\n", error ? error : "sql failed", sql);
		sqlite3_free(error);
		return false;
	}
	return true;
}

// Where radio.txt is looked for. Kept from the same call that opens the
// database, so nothing else has to be told where the card is.
static char store_root[256];

bool radio_store_open(const char *sd_root) {
	if (sd_root && *sd_root) {
		snprintf(store_root, sizeof(store_root), "%s", sd_root);
	}

	pthread_mutex_lock(&store_lock);

	if (store) {
		pthread_mutex_unlock(&store_lock);
		return true;
	}
	if (!sd_root || !*sd_root) {
		pthread_mutex_unlock(&store_lock);
		return false;
	}

	char path[512];
	snprintf(path, sizeof(path), "%s/.local/radio.db", sd_root);

	if (sqlite3_open(path, &store) != SQLITE_OK) {
		fprintf(stderr, "radio: cannot open %s: %s\n", path, sqlite3_errmsg(store));
		sqlite3_close(store);
		store = NULL;
		pthread_mutex_unlock(&store_lock);
		return false;
	}

	// The card is slow and can be pulled at any moment; the same settings the
	// music library uses, and for the same reasons.
	store_exec("PRAGMA synchronous=NORMAL");
	store_exec("PRAGMA journal_mode=TRUNCATE");
	store_exec("PRAGMA cache_size=-64");

	for (size_t i = 0; i < sizeof(STORE_SCHEMA) / sizeof(STORE_SCHEMA[0]); i++) {
		store_exec(STORE_SCHEMA[i]);
	}

	snprintf(store_path, sizeof(store_path), "%s", path);
	file_identity_read(path, &store_identity);

	printf("radio: %s open\n", path);
	pthread_mutex_unlock(&store_lock);
	return true;
}

void radio_store_reopen_if_replaced(void) {
	pthread_mutex_lock(&store_lock);
	bool replaced = store && store_path[0] && file_identity_changed(store_path, &store_identity);
	pthread_mutex_unlock(&store_lock);
	if (!replaced || !store_root[0]) {
		return;
	}

	printf("radio: %s was deleted or replaced; opening it again\n", store_path);
	radio_store_close();
	char dir[sizeof(store_root) + 16];
	snprintf(dir, sizeof(dir), "%s/.local", store_root);
	mkdir(dir, 0777);
	char root[sizeof(store_root)];
	snprintf(root, sizeof(root), "%s", store_root);
	radio_store_open(root);
}

void radio_store_close(void) {
	pthread_mutex_lock(&store_lock);
	if (store) {
		sqlite3_close(store);
		store = NULL;
	}
	pthread_mutex_unlock(&store_lock);
}

// Fills a station from a SELECT that listed the columns in schema order.
static void station_from_row(sqlite3_stmt *stmt, radio_station_t *out) {
	memset(out, 0, sizeof(*out));

	const char *text;
	if ((text = (const char *)sqlite3_column_text(stmt, 0))) {
		snprintf(out->uuid, sizeof(out->uuid), "%s", text);
	}
	if ((text = (const char *)sqlite3_column_text(stmt, 1))) {
		snprintf(out->name, sizeof(out->name), "%s", text);
	}
	if ((text = (const char *)sqlite3_column_text(stmt, 2))) {
		snprintf(out->url, sizeof(out->url), "%s", text);
	}
	if ((text = (const char *)sqlite3_column_text(stmt, 3))) {
		snprintf(out->favicon, sizeof(out->favicon), "%s", text);
	}
	if ((text = (const char *)sqlite3_column_text(stmt, 4))) {
		snprintf(out->codec, sizeof(out->codec), "%s", text);
	}
	out->bitrate = sqlite3_column_int(stmt, 5);
	if ((text = (const char *)sqlite3_column_text(stmt, 6))) {
		snprintf(out->country, sizeof(out->country), "%s", text);
	}
	if ((text = (const char *)sqlite3_column_text(stmt, 7))) {
		snprintf(out->tags, sizeof(out->tags), "%s", text);
	}
}

// INSERT OR REPLACE into whichever table, with `stamp` in the last column.
static void store_put(const char *table, const char *stamp_column, const radio_station_t *s) {
	if (!store || !s || !s->uuid[0]) {
		return;
	}

	char sql[256];
	snprintf(sql, sizeof(sql),
			 "INSERT OR REPLACE INTO %s (uuid,name,url,favicon,codec,bitrate,country,tags,%s)"
			 " VALUES (?,?,?,?,?,?,?,?,?)",
			 table, stamp_column);

	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(store, sql, -1, &stmt, NULL) != SQLITE_OK) {
		return;
	}

	sqlite3_bind_text(stmt, 1, s->uuid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, s->name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, s->url, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, s->favicon, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, s->codec, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 6, s->bitrate);
	sqlite3_bind_text(stmt, 7, s->country, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 8, s->tags, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 9, (sqlite3_int64)time(NULL));

	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

// The n-th row of `table` in `order`, or false when there is no such row.
static bool store_get(const char *table, const char *order, int index, radio_station_t *out) {
	if (!store || index < 0) {
		return false;
	}

	char sql[256];
	snprintf(sql, sizeof(sql),
			 "SELECT uuid,name,url,favicon,codec,bitrate,country,tags FROM %s ORDER BY %s LIMIT 1 OFFSET %d",
			 table, order, index);

	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(store, sql, -1, &stmt, NULL) != SQLITE_OK) {
		return false;
	}

	bool found = sqlite3_step(stmt) == SQLITE_ROW;
	if (found && out) {
		station_from_row(stmt, out);
	}
	sqlite3_finalize(stmt);
	return found;
}

static int store_count(const char *table) {
	if (!store) {
		return 0;
	}

	char sql[64];
	snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);

	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(store, sql, -1, &stmt, NULL) != SQLITE_OK) {
		return 0;
	}
	int count = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : 0;
	sqlite3_finalize(stmt);
	return count;
}

int radio_fav_count(void) {
	pthread_mutex_lock(&store_lock);
	int n = store_count("FAVOURITES");
	pthread_mutex_unlock(&store_lock);
	return n;
}

bool radio_fav_get(int index, radio_station_t *out) {
	pthread_mutex_lock(&store_lock);
	bool ok = store_get("FAVOURITES", "added DESC", index, out);
	pthread_mutex_unlock(&store_lock);
	return ok;
}

bool radio_fav_contains(const char *uuid) {
	if (!uuid || !*uuid) {
		return false;
	}

	pthread_mutex_lock(&store_lock);
	bool found = false;
	sqlite3_stmt *stmt = NULL;
	if (store && sqlite3_prepare_v2(store, "SELECT 1 FROM FAVOURITES WHERE uuid=?", -1, &stmt, NULL) ==
					 SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, uuid, -1, SQLITE_TRANSIENT);
		found = sqlite3_step(stmt) == SQLITE_ROW;
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&store_lock);
	return found;
}

void radio_fav_add(const radio_station_t *station) {
	pthread_mutex_lock(&store_lock);
	store_put("FAVOURITES", "added", station);
	pthread_mutex_unlock(&store_lock);
}

void radio_fav_remove(const char *uuid) {
	if (!uuid || !*uuid) {
		return;
	}

	pthread_mutex_lock(&store_lock);
	sqlite3_stmt *stmt = NULL;
	if (store && sqlite3_prepare_v2(store, "DELETE FROM FAVOURITES WHERE uuid=?", -1, &stmt, NULL) ==
					 SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, uuid, -1, SQLITE_TRANSIENT);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&store_lock);
}

int radio_recent_count(void) {
	pthread_mutex_lock(&store_lock);
	int n = store_count("RECENT");
	pthread_mutex_unlock(&store_lock);
	return n > RADIO_RECENT_MAX ? RADIO_RECENT_MAX : n;
}

bool radio_recent_get(int index, radio_station_t *out) {
	if (index >= RADIO_RECENT_MAX) {
		return false;
	}
	pthread_mutex_lock(&store_lock);
	bool ok = store_get("RECENT", "played DESC", index, out);
	pthread_mutex_unlock(&store_lock);
	return ok;
}

// ---------------------------------------------------------------------------
// radio.txt
// ---------------------------------------------------------------------------

static radio_station_t custom[RADIO_CUSTOM_MAX];
// The heading each station opens, empty for the ones that open none.
static char custom_heading[RADIO_CUSTOM_MAX][RADIO_NAME_MAX];
static int custom_count;
static bool custom_present;
static pthread_mutex_t custom_lock = PTHREAD_MUTEX_INITIALIZER;

// Trims spaces, tabs and the carriage return a file written on Windows leaves
// at the end of every line.
static char *trim(char *s) {
	while (*s == ' ' || *s == '\t') {
		s++;
	}
	char *end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
		end--;
	}
	*end = '\0';
	return s;
}

// What a line is.
typedef enum {
	CUSTOM_LINE_STATION,
	CUSTOM_LINE_HEADING, // "[ROCK]": the stations below belong to it
	CUSTOM_LINE_IGNORED, // blank, or a comment: not something to complain about
	CUSTOM_LINE_BAD,
} custom_line_t;

// A heading is a whole line in square brackets with something inside. A
// station whose name starts with a bracket ("[Live] Radio, http://...") does
// not end with one, so it stays a station. `heading` receives the text.
static bool custom_parse_heading(char *text, char *heading, size_t heading_size) {
	size_t len = strlen(text);
	if (len < 3 || text[0] != '[' || text[len - 1] != ']') {
		return false;
	}
	text[len - 1] = '\0';
	char *inside = trim(text + 1);
	if (!inside[0]) {
		return false;
	}
	snprintf(heading, heading_size, "%s", inside);
	return true;
}

// One line into a station.
//
// The name is everything before the FIRST comma and the address everything
// after it: an address cannot contain a comma before its scheme, and a station
// called "Radio Uno, Roma" is more likely than one whose URL needs the split to
// happen later.
static custom_line_t custom_parse_line(char *line, radio_station_t *out, char *heading, size_t heading_size) {
	char *text = trim(line);
	if (!text[0] || text[0] == '#') {
		return CUSTOM_LINE_IGNORED;
	}
	if (custom_parse_heading(text, heading, heading_size)) {
		return CUSTOM_LINE_HEADING;
	}

	char *comma = strchr(text, ',');
	if (!comma) {
		return CUSTOM_LINE_BAD;
	}
	*comma = '\0';

	char *name = trim(text);
	char *url = trim(comma + 1);
	if (!name[0] || !url[0]) {
		return CUSTOM_LINE_BAD;
	}

	memset(out, 0, sizeof(*out));
	snprintf(out->name, sizeof(out->name), "%s", name);
	snprintf(out->url, sizeof(out->url), "%s", url);
	// A uuid of its own so the favourites and the recent list can tell one of
	// these from a station off the directory, and from each other. The line's
	// address is what makes it that station.
	snprintf(out->uuid, sizeof(out->uuid), "txt:%.30s", url);
	return CUSTOM_LINE_STATION;
}

void radio_custom_reload(void) {
	pthread_mutex_lock(&custom_lock);
	custom_count = 0;
	custom_present = false;

	if (!store_root[0]) {
		pthread_mutex_unlock(&custom_lock);
		return;
	}

	char path[320];
	snprintf(path, sizeof(path), "%s/radio.txt", store_root);
	FILE *f = fopen(path, "r");
	if (!f) {
		pthread_mutex_unlock(&custom_lock);
		return;
	}
	custom_present = true;

	char line[RADIO_URL_MAX + RADIO_NAME_MAX + 8];
	int bad = 0;
	int headings = 0;
	// A heading waits for the station under it; two in a row, the second wins,
	// and one with no station after it shows nothing.
	char pending[RADIO_NAME_MAX] = "";
	while (custom_count < RADIO_CUSTOM_MAX && fgets(line, sizeof(line), f)) {
		char heading[RADIO_NAME_MAX];
		switch (custom_parse_line(line, &custom[custom_count], heading, sizeof(heading))) {
		case CUSTOM_LINE_STATION:
			snprintf(custom_heading[custom_count], sizeof(custom_heading[0]), "%s", pending);
			pending[0] = '\0';
			custom_count++;
			break;
		case CUSTOM_LINE_HEADING:
			snprintf(pending, sizeof(pending), "%s", heading);
			headings++;
			break;
		case CUSTOM_LINE_BAD:
			bad++;
			break;
		case CUSTOM_LINE_IGNORED:
			break;
		}
	}
	fclose(f);

	// Only the lines somebody meant as a station and got wrong are counted:
	// blanks and comments are how the file is meant to be written.
	fprintf(stderr, "radio: %s -- %d stations, %d headings, %d unreadable lines\n", path, custom_count, headings,
			bad);
	pthread_mutex_unlock(&custom_lock);
}

int radio_custom_count(void) {
	pthread_mutex_lock(&custom_lock);
	int n = custom_count;
	pthread_mutex_unlock(&custom_lock);
	return n;
}

bool radio_custom_get(int index, radio_station_t *out) {
	if (!out) {
		return false;
	}
	pthread_mutex_lock(&custom_lock);
	bool ok = index >= 0 && index < custom_count;
	if (ok) {
		*out = custom[index];
	}
	pthread_mutex_unlock(&custom_lock);
	return ok;
}

bool radio_custom_heading(int index, char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return false;
	}
	out[0] = '\0';
	pthread_mutex_lock(&custom_lock);
	bool ok = index >= 0 && index < custom_count && custom_heading[index][0];
	if (ok) {
		snprintf(out, out_size, "%s", custom_heading[index]);
	}
	pthread_mutex_unlock(&custom_lock);
	return ok;
}

int radio_custom_current_index(void) {
	radio_station_t current;
	if (!radio_current_station(&current) || strncmp(current.uuid, "txt:", 4) != 0) {
		return -1;
	}
	int found = -1;
	pthread_mutex_lock(&custom_lock);
	for (int i = 0; i < custom_count; i++) {
		if (strcmp(custom[i].url, current.url) == 0) {
			found = i;
			break;
		}
	}
	pthread_mutex_unlock(&custom_lock);
	return found;
}

bool radio_custom_can_step(void) { return radio_custom_count() > 1 && radio_custom_current_index() >= 0; }

bool radio_custom_step(int step) {
	int index = radio_custom_current_index();
	int count = radio_custom_count();
	if (index < 0 || count <= 0) {
		return false;
	}
	int next = ((index + step) % count + count) % count;
	radio_station_t station;
	if (!radio_custom_get(next, &station)) {
		return false;
	}
	radio_play(&station);
	return true;
}

// ---------------------------------------------------------------------------
// The list a station was started from
//
// Previous and next move along it: the stations of a country, a language or a
// genre, a search's hits, the starred or the recent -- whichever list the tap
// came from, as it stood then. A copy, because the page goes on to show other
// lists, and the order the stations were offered in is the one to walk.
//
// radio.txt keeps its own: it is read from the card, and its stations are known
// by a "txt:" uuid (see radio_custom_reload).
// ---------------------------------------------------------------------------

static pthread_mutex_t list_lock = PTHREAD_MUTEX_INITIALIZER;
static radio_station_t *play_list;
static int play_list_count;

void radio_set_list(const radio_station_t *list, int count) {
	radio_station_t *copy = NULL;
	if (list && count > 0) {
		copy = malloc((size_t)count * sizeof(*copy));
		if (copy) {
			memcpy(copy, list, (size_t)count * sizeof(*copy));
		}
	}
	pthread_mutex_lock(&list_lock);
	free(play_list);
	play_list = copy;
	play_list_count = copy ? count : 0;
	pthread_mutex_unlock(&list_lock);
}

// The directory's uuid when both have one; the address otherwise.
static bool same_station(const radio_station_t *a, const radio_station_t *b) {
	if (a->uuid[0] && b->uuid[0]) {
		return strcmp(a->uuid, b->uuid) == 0;
	}
	return strcmp(a->url, b->url) == 0;
}

// Where `station` is in the list. Call with list_lock held.
static int list_find_locked(const radio_station_t *station) {
	for (int i = 0; i < play_list_count; i++) {
		if (same_station(&play_list[i], station)) {
			return i;
		}
	}
	return -1;
}

static bool is_custom(const radio_station_t *station) { return strncmp(station->uuid, "txt:", 4) == 0; }

bool radio_list_position(int *index_out, int *count_out) {
	radio_station_t current;
	if (!radio_current_station(&current)) {
		return false;
	}
	int index;
	int count;
	if (is_custom(&current)) {
		index = radio_custom_current_index();
		count = radio_custom_count();
	} else {
		pthread_mutex_lock(&list_lock);
		index = list_find_locked(&current);
		count = play_list_count;
		pthread_mutex_unlock(&list_lock);
	}
	if (index < 0 || count < 2) {
		return false;
	}
	if (index_out) {
		*index_out = index;
	}
	if (count_out) {
		*count_out = count;
	}
	return true;
}

bool radio_can_step(void) { return radio_list_position(NULL, NULL); }

bool radio_step(int step) {
	radio_station_t current;
	if (!radio_current_station(&current)) {
		return false;
	}
	if (is_custom(&current)) {
		return radio_custom_can_step() && radio_custom_step(step);
	}

	// The next one that can be played here: a list from the directory holds
	// stations this device cannot open, and stopping on one of them would be a
	// dead end in the middle of the list.
	radio_station_t next;
	bool found = false;
	pthread_mutex_lock(&list_lock);
	int index = list_find_locked(&current);
	for (int tries = 1; index >= 0 && tries < play_list_count; tries++) {
		int at = ((index + step * tries) % play_list_count + play_list_count) % play_list_count;
		if (!radio_station_problem(&play_list[at])) {
			next = play_list[at];
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&list_lock);

	return found && radio_play(&next);
}

bool radio_custom_file_present(void) {
	pthread_mutex_lock(&custom_lock);
	bool present = custom_present;
	pthread_mutex_unlock(&custom_lock);
	return present;
}

// Records a station as just played and trims the list back to ten. Called
// from radio_play(), so nothing else has to remember to.
static void recent_note(const radio_station_t *station) {
	pthread_mutex_lock(&store_lock);
	BOOTPROF_CALL("radio store_put RECENT", store_put("RECENT", "played", station));

	// PRIMARY KEY means the same station cannot be in here twice, so the trim
	// only ever has to drop what has fallen off the end.
	char sql[160];
	snprintf(sql, sizeof(sql),
			 "DELETE FROM RECENT WHERE uuid NOT IN"
			 " (SELECT uuid FROM RECENT ORDER BY played DESC LIMIT %d)",
			 RADIO_RECENT_MAX);
	store_exec(sql);

	pthread_mutex_unlock(&store_lock);
}

// ---------------------------------------------------------------------------
// Playlists
// ---------------------------------------------------------------------------
//
// A good number of directory entries do not point at a stream at all: they
// point at a .pls or .m3u file that CONTAINS the address of the stream.
//
// The address is checked two ways, because neither alone is enough. The file
// extension catches the common case without a round trip; the Content-Type
// catches the servers that hand out a playlist from a URL that looks like a
// stream.

// .m3u8 is HLS, which is a different thing entirely: a manifest of numbered
// segments to be fetched in turn, re-fetched as it is rewritten. It belongs to
// the HLS transport in hls.c; read as a plain playlist it yields one segment's
// worth of audio and then silence.
static bool url_is_hls(const char *url) {
	const char *q = strpbrk(url, "?#");
	size_t len = q ? (size_t)(q - url) : strlen(url);
	return len >= 5 && strncasecmp(url + len - 5, ".m3u8", 5) == 0;
}

static bool url_is_playlist(const char *url) {
	const char *q = strpbrk(url, "?#");
	size_t len = q ? (size_t)(q - url) : strlen(url);
	static const char *const EXT[] = {".pls", ".m3u", ".asx", ".xspf", NULL};
	for (int i = 0; EXT[i]; i++) {
		size_t n = strlen(EXT[i]);
		if (len >= n && strncasecmp(url + len - n, EXT[i], n) == 0) {
			return true;
		}
	}
	return false;
}

static bool content_type_is_playlist(const char *ct) {
	if (!ct || !ct[0]) {
		return false;
	}
	return strcasestr(ct, "mpegurl") != NULL	   // audio/x-mpegurl, .m3u
		   || strcasestr(ct, "scpls") != NULL	   // audio/x-scpls, .pls
		   || strcasestr(ct, "pls") != NULL		   // audio/pls
		   || strcasestr(ct, "xspf") != NULL	   //
		   || strcasestr(ct, "ms-asf") != NULL;	   // video/x-ms-asf, .asx
}

// Pulls the first playable address out of playlist text. Handles the two
// formats that actually turn up:
//
//   .pls    an INI file: File1=http://..., with Title1 and Length1 beside it
//   .m3u    one address per line, '#' lines are comments
//
// and, because some servers answer with neither, falls back to "the first
// thing on any line that looks like an address" -- which also covers .asx and
// .xspf without parsing XML, since the address inside them is on a line of its
// own often enough to be worth the two lines of code.
static bool playlist_first_url(const char *text, char *out, size_t out_size) {
	const char *line = text;

	while (line && *line) {
		const char *end = strpbrk(line, "\r\n");
		size_t len = end ? (size_t)(end - line) : strlen(line);

		// Trim.
		const char *begin = line;
		while (len > 0 && (*begin == ' ' || *begin == '\t')) {
			begin++;
			len--;
		}
		while (len > 0 && (begin[len - 1] == ' ' || begin[len - 1] == '\t')) {
			len--;
		}

		if (len > 0 && *begin != '#' && *begin != ';') {
			// FileN=... in a .pls, or the address on its own in an .m3u. In
			// either case what is wanted is from "http" to the end of the
			// line, so find it rather than special-case the formats.
			const char *http = NULL;
			for (size_t i = 0; i + 7 <= len; i++) {
				if (strncasecmp(begin + i, "http://", 7) == 0 ||
					(i + 8 <= len && strncasecmp(begin + i, "https://", 8) == 0)) {
					http = begin + i;
					len -= i;
					break;
				}
			}
			if (http) {
				// An XML attribute would leave a closing quote or bracket on
				// the end; cut at the first character no URL contains.
				size_t take = 0;
				while (take < len && http[take] != '"' && http[take] != '\'' && http[take] != '<' &&
					   http[take] != '>' && http[take] != ' ') {
					take++;
				}
				if (take > 0 && take < out_size) {
					memcpy(out, http, take);
					out[take] = '\0';
					return true;
				}
			}
		}

		line = end ? end + strspn(end, "\r\n") : NULL;
	}
	return false;
}

// Downloads a playlist and replaces `url` with what it points at. Follows a
// playlist that points at another one, up to PLAYLIST_MAX_HOPS, because that
// happens too.
//
// Returns false only when the address is one this player is known not to play;
// `why` then holds the reason for the user. A playlist that fails to download
// is not a failure here -- the caller tries the original address, which
// occasionally works anyway.
#define PLAYLIST_MAX_HOPS 3
#define PLAYLIST_MAX_BYTES (64 * 1024)

static bool resolve_playlist(char *url, size_t url_size, char *why, size_t why_size, bool *is_hls) {
	if (is_hls) {
		*is_hls = false;
	}
	for (int hop = 0; hop < PLAYLIST_MAX_HOPS; hop++) {
		// HLS is not a refusal but a different transport: stop here and tell the
		// caller, which opens the right one. Downloading the manifest as if it
		// were a plain .m3u playlist plays one segment and then stops.
		if (url_is_hls(url)) {
			if (is_hls) {
				*is_hls = true;
			}
			return true;
		}
		if (!url_is_playlist(url)) {
			return true; // an ordinary stream address: nothing to do
		}

		char *text = NULL;
		if (!http_get(url, &text, NULL, PLAYLIST_MAX_BYTES, 10)) {
			printf("radio: playlist %s could not be downloaded, trying it as a stream\n", url);
			return true;
		}

		char next[RADIO_URL_MAX];
		bool found = playlist_first_url(text, next, sizeof(next));
		free(text);

		if (!found) {
			snprintf(why, why_size, "%s", tr("radio_playlist_empty"));
			return false;
		}

		printf("radio: playlist -> %s\n", next);
		snprintf(url, url_size, "%s", next);
	}

	snprintf(why, why_size, "%s", tr("radio_playlist_loop"));
	return false;
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

// A connection that has produced no audio at all after this many seconds is
// not going to, and the format is the usual reason.
//
// A byte counter cannot stand in for the clock: minimp3 resynchronises, and on
// non-MP3 data it periodically finds a byte sequence that passes for a header,
// reports a frame length, consumes it and returns zero samples. The buffer
// keeps draining and never fills, so the "full buffer with nothing decodable
// in it" check below never fires.
#define RADIO_SILENCE_TIMEOUT 12

// The decoder's window on the stream, taken from the network buffer above a
// piece at a time. Two frames' worth would do; this is what one turn round the
// loop works through.
#define RADIO_IN_BUFFER 32768

// The decoder's working set, shared by the two ways a station can arrive.
// play_main() calls play_one_connection() (a plain stream) or play_hls() (a
// manifest), never both, and always on the same thread, so one set does for
// both. Static rather than local because these do not belong on a stack.
static drmp3_uint8 radio_input[RADIO_IN_BUFFER];

// Sized for the longest frame either decoder produces: an MP3 frame is 1152
// sample frames, HE-AAC's is 2048. fdk-aac refuses outright rather than
// truncating when the buffer cannot hold a whole frame, and aacdec_pull() then
// reports a damaged frame indefinitely. Eight channels because the ADTS header
// decides the count (see ADTS_PCM_CAPACITY in decode.c).
#define RADIO_AAC_FRAME 2048
#define RADIO_PCM_CAPACITY (RADIO_AAC_FRAME * 8)
static short radio_pcm[RADIO_PCM_CAPACITY];
// Room to widen a mono frame into a stereo one -- see the note where the
// output is opened.
static short radio_stereo[RADIO_AAC_FRAME * 2];

// A station that drops the connection is usually back within a moment, and
// having to tap it again would be silly. Give up after this many tries in a
// row so a station that has gone for good does not sit there forever.
#define RADIO_MAX_RETRIES 4

static pthread_mutex_t now_lock = PTHREAD_MUTEX_INITIALIZER;
static radio_now_t now_state;
static unsigned now_serial;
static radio_station_t now_station;

// The transport, and the rule that keeps it single-writer.
//
// Every start gets a generation number. The playing thread carries its own and
// stops the moment it is no longer the current one, and a new station is not
// started until the old thread has been joined. Exactly one thread may be
// inside audio_external_write() at a time; a shared stop flag is not enough,
// because a thread parked in a socket read can wake after the flag has been
// cleared for its successor.
static pthread_t play_thread;
static bool play_thread_valid; // a thread exists and has not been joined
static volatile bool play_thread_running;
static volatile unsigned play_generation;

// The live connection, so a stop can shut its socket down and bring the reader
// out of recv() at once instead of waiting out the receive timeout.
static pthread_mutex_t stream_lock = PTHREAD_MUTEX_INITIALIZER;
static http_stream_t *live_stream;

static void now_touch(void) { now_serial++; }

// Every point in the playing thread that could carry on asks this instead of
// reading a shared flag: it is true only while this thread is still the one
// the rest of the program thinks is playing.
static bool still_mine(unsigned mine) { return play_generation == mine; }

// ---------------------------------------------------------------------------
// The network buffer
//
// A station sends at the speed its music plays, so nothing is ever spare: a
// pause in the delivery is a pause at the sound card. This holds seconds of the
// undecoded stream between the two.
//
// The thread is what makes it a buffer rather than a bigger array. The decode
// loop is paced by a blocking write to the card, so a read on that thread
// happens only between two writes and stops everything for as long as the
// network takes to answer. A thread with nothing else to do keeps pulling while
// the frames already decoded are still playing.
//
// What is being avoided is not a click: the card is opened with a start
// threshold of a full buffer (see open_pcm_device), so a stream that starves
// goes quiet for the whole 750 ms before the next sample is heard.
// ---------------------------------------------------------------------------

// Six seconds at 320 kbps, sixteen at the 128 kbps most stations send.
#define RADIO_BUFFER_BYTES (256 * 1024)

// The same for a station that may be FLAC, allocated while it plays: some
// fifteen seconds of a 16/44.1 station, five of a 24/96 one.
#define RADIO_FLAC_BUFFER_BYTES (2 * 1024 * 1024)

// What is gathered before the first frame is decoded, and how long the filling
// thread is given to gather it. Six seconds of a 128 kbps station, three of a
// 256 kbps one.
//
// This is the reserve the station then plays from for as long as it is on: a
// server sends at the speed its music plays, so what is not taken in front of
// the first frame is never offered again. Most open with a burst of several
// seconds and reach the target at once; one that sends strictly in real time
// hands over what it can in the time allowed, and the wait is the price of the
// station not breaking up afterwards.
#define RADIO_PREFILL_BYTES (96 * 1024)
#define RADIO_PREFILL_MS 3000

// A FLAC station spends RADIO_PREFILL_BYTES in well under a second, so once
// the stream is known to be FLAC it is given this much longer to fill three
// quarters of its buffer.
#define RADIO_FLAC_PREFILL_MS 2000

// A station announces a song ahead of the audio it belongs to, by however much
// of the stream is being held here. Each announcement is kept with the position
// it arrived at and goes on screen when the decoder reaches it.
#define RADIO_TITLE_MARKS 4

typedef struct {
	long long at; // bytes taken by the decoder when this becomes the current one
	char title[256];
} radio_title_mark_t;

typedef struct {
	// radio_buffer_storage, or for a FLAC station a larger block of its own.
	unsigned char *data;
	int capacity;
	int head;  // where the decoder reads
	int level; // bytes held

	// Positions in the stream as a whole, which is what the titles are placed
	// against. They do not wrap, and that is why they are counted apart from
	// `head`.
	long long written;
	long long taken;

	bool ended; // the source has nothing more to give
	bool stop;  // the decode loop is finished with it

	// Exactly one of the two: a plain connection, or a list of segments.
	http_stream_t *stream;
	hls_t *hls;

	radio_title_mark_t marks[RADIO_TITLE_MARKS];
	int mark_count;

	pthread_mutex_t lock;
	pthread_cond_t filled;
	pthread_cond_t drained;
	pthread_t thread;
	bool thread_valid;
} radio_buffer_t;

// One station plays at a time and both transports run on the same thread, so
// one of these does for both. Static for the same reason as the decoder's
// buffers below: a quarter of a megabyte does not go on a stack.
static unsigned char radio_buffer_storage[RADIO_BUFFER_BYTES];

static radio_buffer_t radio_buffer = {
	.data = radio_buffer_storage,
	.capacity = RADIO_BUFFER_BYTES,
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.filled = PTHREAD_COND_INITIALIZER,
	.drained = PTHREAD_COND_INITIALIZER,
};

// Under the lock. Places what the station has just announced at the position
// the bytes carrying it arrived at.
static void radio_buffer_note_title(radio_buffer_t *b) {
	if (!b->stream || !b->stream->title_changed) {
		return;
	}
	b->stream->title_changed = false;

	// A full queue means announcements are arriving faster than they are being
	// reached, and the newest is the one that will still be right.
	int slot = b->mark_count < RADIO_TITLE_MARKS ? b->mark_count++ : RADIO_TITLE_MARKS - 1;
	b->marks[slot].at = b->written;
	snprintf(b->marks[slot].title, sizeof(b->marks[slot].title), "%s", b->stream->stream_title);
}

static void *radio_buffer_main(void *arg) {
	radio_buffer_t *b = (radio_buffer_t *)arg;

	for (;;) {
		pthread_mutex_lock(&b->lock);
		while (!b->stop && b->level >= b->capacity) {
			pthread_cond_wait(&b->drained, &b->lock);
		}
		bool stop = b->stop;
		int tail = (b->head + b->level) % b->capacity;
		int room = b->capacity - b->level;
		pthread_mutex_unlock(&b->lock);

		if (stop) {
			break;
		}

		// Filled up to the end of the array and no further; the wrap is the
		// next turn's business. The read is outside the lock, and can be: this
		// is the only thread that writes, and it writes into space the decoder
		// has already passed.
		int to_end = b->capacity - tail;
		int want = room < to_end ? room : to_end;
		int got = b->stream ? http_stream_read(b->stream, b->data + tail, want)
							: hls_read(b->hls, b->data + tail, want);

		pthread_mutex_lock(&b->lock);
		if (got > 0) {
			b->level += got;
			b->written += got;
			radio_buffer_note_title(b);
			pthread_cond_signal(&b->filled);
			pthread_mutex_unlock(&b->lock);
			continue;
		}
		b->ended = true;
		pthread_cond_broadcast(&b->filled);
		pthread_mutex_unlock(&b->lock);
		break;
	}
	return NULL;
}

// Gives back a block radio_buffer_start() allocated. The filling thread must be
// gone.
static void radio_buffer_release(radio_buffer_t *b) {
	if (b->data != radio_buffer_storage) {
		free(b->data);
	}
	b->data = radio_buffer_storage;
	b->capacity = RADIO_BUFFER_BYTES;
}

// Starts filling from a source that is already open, into `capacity` bytes:
// RADIO_BUFFER_BYTES, or more when there is the memory for it. False when the
// thread cannot be created, which leaves the caller with nothing to read from.
static bool radio_buffer_start(radio_buffer_t *b, http_stream_t *stream, hls_t *hls, int capacity) {
	unsigned char *data = capacity > RADIO_BUFFER_BYTES ? malloc((size_t)capacity) : NULL;
	if (!data) {
		if (capacity > RADIO_BUFFER_BYTES) {
			fprintf(stderr, "radio: no memory for a %d KB buffer\n", capacity / 1024);
		}
		data = radio_buffer_storage;
		capacity = RADIO_BUFFER_BYTES;
	}

	pthread_mutex_lock(&b->lock);
	b->data = data;
	b->capacity = capacity;
	b->head = 0;
	b->level = 0;
	b->written = 0;
	b->taken = 0;
	b->ended = false;
	b->stop = false;
	b->stream = stream;
	b->hls = hls;
	b->mark_count = 0;
	pthread_mutex_unlock(&b->lock);

	if (pthread_create(&b->thread, NULL, radio_buffer_main, b) != 0) {
		fprintf(stderr, "radio: no thread for the buffer\n");
		b->stream = NULL;
		b->hls = NULL;
		radio_buffer_release(b);
		return false;
	}
	b->thread_valid = true;
	return true;
}

// Brings the filling thread down and waits for it to be gone, which is what
// makes it safe to close the source afterwards. The nudge comes first: the
// thread is normally sitting in a read, and a station that has gone quiet would
// otherwise hold it for the whole receive timeout.
static void radio_buffer_stop(radio_buffer_t *b) {
	if (!b->thread_valid) {
		return;
	}

	pthread_mutex_lock(&b->lock);
	b->stop = true;
	pthread_cond_broadcast(&b->filled);
	pthread_cond_broadcast(&b->drained);
	pthread_mutex_unlock(&b->lock);

	if (b->stream) {
		http_stream_wake(b->stream);
	} else if (b->hls) {
		hls_abort(b->hls);
	}

	pthread_join(b->thread, NULL);
	b->thread_valid = false;
	b->stream = NULL;
	b->hls = NULL;
	radio_buffer_release(b);
}

// Hands the decoder what has been gathered, and takes the place of reading the
// source directly. It blocks only when there is nothing at all, which is the
// one case where there would have been nothing to play from either. Returns 0
// when the source has ended and the last byte has been taken.
static int radio_buffer_read(radio_buffer_t *b, unsigned char *dst, int max) {
	pthread_mutex_lock(&b->lock);
	while (b->level == 0 && !b->ended && !b->stop) {
		pthread_cond_wait(&b->filled, &b->lock);
	}

	int n = b->level < max ? b->level : max;
	if (n > 0) {
		int to_end = b->capacity - b->head;
		int first = n < to_end ? n : to_end;
		memcpy(dst, b->data + b->head, (size_t)first);
		if (n > first) {
			memcpy(dst + first, b->data, (size_t)(n - first));
		}
		b->head = (b->head + n) % b->capacity;
		b->level -= n;
		b->taken += n;
		pthread_cond_signal(&b->drained);
	}
	pthread_mutex_unlock(&b->lock);
	return n;
}

// The announcement the decoder has now reached, if it has reached one.
static bool radio_buffer_take_title(radio_buffer_t *b, char *out, size_t size) {
	bool got = false;

	pthread_mutex_lock(&b->lock);
	if (b->mark_count > 0 && b->taken >= b->marks[0].at) {
		snprintf(out, size, "%s", b->marks[0].title);
		b->mark_count--;
		memmove(b->marks, b->marks + 1, (size_t)b->mark_count * sizeof(b->marks[0]));
		got = true;
	}
	pthread_mutex_unlock(&b->lock);
	return got;
}

// Waits for `bytes` to be held, `ms` at most, before the first frame is
// decoded: this is the reserve the station plays from for as long as it is on.
static void radio_buffer_prefill(radio_buffer_t *b, unsigned mine, int bytes, int ms) {
	int waited = 0;
	int level = 0;

	for (;;) {
		pthread_mutex_lock(&b->lock);
		level = b->level;
		bool ended = b->ended;
		pthread_mutex_unlock(&b->lock);

		if (ended || level >= bytes || waited >= ms || !still_mine(mine)) {
			break;
		}
		usleep(50 * 1000);
		waited += 50;
	}

	printf("radio: %d KB buffered before the first frame\n", level / 1024);
}

static bool is_png(const unsigned char *b, size_t len) {
	return len >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G';
}

// The biggest picture inside a Windows icon that is a PNG, which is how icons
// of any size have been made for years; the older bitmap kind is not drawn.
// False when there is none.
static bool ico_png(const unsigned char *b, size_t len, size_t *at, size_t *size) {
	if (len < 6 || b[0] != 0 || b[1] != 0 || b[2] != 1 || b[3] != 0) {
		return false;
	}
	int count = b[4] | (b[5] << 8);
	size_t best = 0;
	for (int i = 0; i < count && 6 + (size_t)(i + 1) * 16 <= len; i++) {
		const unsigned char *e = b + 6 + (size_t)i * 16;
		size_t bytes = (size_t)e[8] | ((size_t)e[9] << 8) | ((size_t)e[10] << 16) | ((size_t)e[11] << 24);
		size_t offset = (size_t)e[12] | ((size_t)e[13] << 8) | ((size_t)e[14] << 16) | ((size_t)e[15] << 24);
		if (offset < len && bytes <= len - offset && bytes > best && is_png(b + offset, bytes)) {
			best = bytes;
			*at = offset;
			*size = bytes;
		}
	}
	return best > 0;
}

// The icon of the station before, deleted when the next one's comes in. Under
// now_lock.
static char favicon_file[64];

// Pulls the station's icon down so the player has something to show. Best
// effort in every sense: no icon, an icon that is not an image, a server that
// does not answer -- all of them just mean the radio placeholder stays up.
//
// Over HTTPS as well as HTTP, which is how most of the directory's icons are
// served now: the same connection code as the stations' own, so wherever a
// station on HTTPS plays, its icon comes.
static void fetch_favicon(const char *url) {
	bool secure = url && strncasecmp(url, "https://", 8) == 0;
	if (!url || (!secure && strncasecmp(url, "http://", 7) != 0) || (secure && !tls_available())) {
		return;
	}

	char *body = NULL;
	size_t len = 0;
	if (!http_get(url, &body, &len, 2 * 1024 * 1024, 8) || len < 64) {
		free(body);
		return;
	}

	// Trust the bytes, not the file name: half the favicons in the directory
	// are called .png and are JPEGs, or the other way round. A GIF is kept
	// under .png too, because a picture file is told apart by its extension
	// and only .jpg and .png are looked at; what decodes it goes by the bytes.
	const unsigned char *b = (const unsigned char *)body;
	const char *ext = NULL;
	size_t at = 0, size = len;
	if (is_png(b, len)) {
		ext = ".png";
	} else if (b[0] == 0xFF && b[1] == 0xD8) {
		ext = ".jpg";
	} else if (memcmp(b, "GIF8", 4) == 0) {
		ext = ".png";
	} else if (ico_png(b, len, &at, &size)) {
		ext = ".png";
	} else {
		free(body);
		return; // not something the cover decoder can read
	}

	// One name per icon, from its address: the player compares the path to
	// tell a new picture from the one it is showing, and two stations with
	// the same kind of icon must not look like the same picture to it.
	uint32_t hash = 2166136261u;
	for (const char *p = url; *p; p++) {
		hash = (hash ^ (uint8_t)*p) * 16777619u;
	}
	char path[64];
	snprintf(path, sizeof(path), "/tmp/sonix-radio-cover-%08x%s", (unsigned)hash, ext);

	FILE *f = fopen(path, "wb");
	if (f) {
		bool ok = fwrite(body + at, 1, size, f) == size;
		ok = fclose(f) == 0 && ok;
		if (ok) {
			pthread_mutex_lock(&now_lock);
			if (favicon_file[0] && strcmp(favicon_file, path) != 0) {
				unlink(favicon_file);
			}
			snprintf(favicon_file, sizeof(favicon_file), "%s", path);
			snprintf(now_state.cover_path, sizeof(now_state.cover_path), "%s", path);
			now_touch();
			pthread_mutex_unlock(&now_lock);
		}
	}
	free(body);
}

// How much undecoded data must be sitting in the buffer before a frame is
// handed to the decoder.
//
// This is a correctness knob, not a performance one. minimp3 takes its fast
// path -- reusing the previous frame header -- only when the buffer holds the
// whole current frame AND the first four bytes of the next one. Without that
// it zeroes its entire state, including the bit reservoir the current frame is
// decoded from, and the frame comes back with zero samples. Decoding right up
// to the end of the buffer therefore loses a frame at every refill boundary.
//
// A frame is at most 1441 bytes plus padding, so this holds more than two.
#define RADIO_DECODE_SLACK 4096

// ---------------------------------------------------------------------------
// What comes out of either decoder
//
// The two decoders agree on one thing: some frames came out, at some rate,
// with some number of channels, and the card has to be opened for them and
// written to. The MP3 transport and both HLS branches share that step here.
// ---------------------------------------------------------------------------

typedef struct {
	bool open;
	int rate;
	int channels; // what the STREAM is, not what the card was opened as
	int bits;	  // what the card was opened with: 16, or 32 for a deep FLAC
} radio_out_t;

// Opens or reopens the card when the stream changes under it, widens mono, and
// writes. Returns false only when the card refuses the rate, which is not worth
// reconnecting for: the next attempt meets the same refusal.
//
// `frames` is sample frames, one per channel -- what both decoders return and
// what audio_external_write() takes -- not total samples.
//
// The write is blocking, and that is the point: it is what paces the decode
// loop to real time. The stream is pulled in ahead of it by the filling thread,
// which is why a pause on the network no longer stops the loop here.
static bool radio_push_pcm(radio_out_t *out, short *pcm, short *stereo, int frames, int rate, int channels,
						   int bitrate_kbps, const char *codec_name) {
	if (frames <= 0 || rate <= 0 || channels <= 0) {
		return true;
	}

	// The output is ALWAYS opened in stereo and a mono station is widened below,
	// because a DAC that will not take a one-channel stream does not refuse it:
	// the frames are read as half-pairs and play at double speed with the
	// channels crossed.
	if (!out->open || rate != out->rate || channels != out->channels || out->bits != 16) {
		if (out->open) {
			audio_external_end();
		}
		out->rate = rate;
		out->channels = channels;
		out->bits = 16;
		out->open = audio_external_begin(out->rate, 2, 16);
		if (!out->open) {
			fprintf(stderr, "radio: cannot open the output at %d Hz\n", out->rate);
			return false;
		}

		pthread_mutex_lock(&now_lock);
		now_state.sample_rate = out->rate;
		now_state.channels = out->channels;
		if (bitrate_kbps > 0) {
			now_state.bitrate = bitrate_kbps;
		}
		snprintf(now_state.codec, sizeof(now_state.codec), "%s", codec_name ? codec_name : "");
		now_state.connecting = false;
		now_state.playing = true;
		now_touch();
		pthread_mutex_unlock(&now_lock);
	}

	// The equaliser, the parametric one and MSEB, on the stereo pair about to
	// be written -- the same chain a track from the card goes through, and a
	// no-op when all three are off. A station is not a file, but what the
	// listener has tuned is for the headphones, not for the source.
	if (channels == 1) {
		for (int i = frames - 1; i >= 0; i--) {
			stereo[2 * i] = pcm[i];
			stereo[2 * i + 1] = pcm[i];
		}
		eq_process(stereo, frames, 2, rate);
		audio_external_write(stereo, frames);
	} else if (channels == 2) {
		eq_process(pcm, frames, 2, rate);
		audio_external_write(pcm, frames);
	} else {
		// More than two channels, which on a radio station is AAC carrying 5.1.
		// Not a real downmix -- the even channels are averaged into the left
		// and the odd ones into the right -- but a card opened for two channels
		// and fed six interleaved ones plays at three times the speed. This
		// keeps the timing right and every channel audible.
		int pairs = channels / 2;
		for (int i = 0; i < frames; i++) {
			int left = 0, right = 0;
			for (int c = 0; c < pairs; c++) {
				left += pcm[i * channels + 2 * c];
				right += pcm[i * channels + 2 * c + 1];
			}
			stereo[2 * i] = (short)(left / pairs);
			stereo[2 * i + 1] = (short)(right / pairs);
		}
		eq_process(stereo, frames, 2, rate);
		audio_external_write(stereo, frames);
	}
	return true;
}

// radio_push_pcm() for samples deeper than 16 bits, left-justified in 32, which
// go to the card as they are: the card is opened for 32-bit frames. False when
// it refuses them.
static bool radio_push_pcm32(radio_out_t *out, int32_t *pcm, int32_t *stereo, int frames, int rate, int channels, const char *codec_name) {
	if (frames <= 0 || rate <= 0 || channels <= 0) {
		return true;
	}

	if (!out->open || rate != out->rate || channels != out->channels || out->bits != 32) {
		if (out->open) {
			audio_external_end();
		}
		out->rate = rate;
		out->channels = channels;
		out->bits = 32;
		out->open = audio_external_begin(out->rate, 2, 32);
		if (!out->open) {
			fprintf(stderr, "radio: cannot open the output at %d Hz, 32 bit\n", out->rate);
			return false;
		}

		pthread_mutex_lock(&now_lock);
		now_state.sample_rate = out->rate;
		now_state.channels = out->channels;
		snprintf(now_state.codec, sizeof(now_state.codec), "%s", codec_name ? codec_name : "");
		now_state.connecting = false;
		now_state.playing = true;
		now_touch();
		pthread_mutex_unlock(&now_lock);
	}

	// Mono widened, and more than two channels folded into two, as above.
	int32_t *frames_out = pcm;
	if (channels == 1) {
		for (int i = frames - 1; i >= 0; i--) {
			stereo[2 * i] = pcm[i];
			stereo[2 * i + 1] = pcm[i];
		}
		frames_out = stereo;
	} else if (channels > 2) {
		int pairs = channels / 2;
		for (int i = 0; i < frames; i++) {
			int64_t left = 0, right = 0;
			for (int c = 0; c < pairs; c++) {
				left += pcm[i * channels + 2 * c];
				right += pcm[i * channels + 2 * c + 1];
			}
			stereo[2 * i] = (int32_t)(left / pairs);
			stereo[2 * i + 1] = (int32_t)(right / pairs);
		}
		frames_out = stereo;
	}
	eq_process_s32(frames_out, frames, 2, rate);
	audio_external_write(frames_out, frames);
	return true;
}

// ---------------------------------------------------------------------------
// Which decoder a plain stream needs
//
// A plain HTTP connection can carry AAC as bare ADTS frames -- no manifest, no
// segments. That is a large share of internet radio: a station at 48 kbps or
// below is almost certainly HE-AAC.
//
// The Content-Type is asked first and the bytes second, because neither alone
// is enough. Plenty of AAC stations are served as audio/mpeg by a misconfigured
// Icecast, and plenty send no Content-Type at all; on the other side, the first
// bytes of a stream can land mid-frame, where a sniff finds nothing.
//
// FLAC comes native or in Ogg, with its header at the start of the connection
// either way; radioflac.c decodes both.
// ---------------------------------------------------------------------------

typedef enum {
	RADIO_CODEC_UNKNOWN = 0,
	RADIO_CODEC_MP3,
	RADIO_CODEC_AAC,
	RADIO_CODEC_FLAC,
	RADIO_CODEC_OGG_FLAC,
} radio_codec_t;

static const char *codec_label(radio_codec_t codec) {
	switch (codec) {
	case RADIO_CODEC_MP3:
		return "MP3";
	case RADIO_CODEC_AAC:
		return "AAC";
	case RADIO_CODEC_FLAC:
		return "FLAC";
	case RADIO_CODEC_OGG_FLAC:
		return "Ogg FLAC";
	default:
		return "nothing";
	}
}

static bool codec_is_flac(radio_codec_t codec) { return codec == RADIO_CODEC_FLAC || codec == RADIO_CODEC_OGG_FLAC; }

static radio_codec_t codec_from_content_type(const char *ct) {
	if (!ct || !ct[0]) {
		return RADIO_CODEC_UNKNOWN;
	}
	// audio/flac, audio/x-flac, and "audio/ogg; codecs=flac": which of the two
	// containers it is, the bytes say.
	if (strcasestr(ct, "flac")) {
		return RADIO_CODEC_FLAC;
	}
	// audio/aac, audio/aacp, audio/x-aac, application/aacp -- and "mp4a" for
	// the servers that answer with the codec string instead of a media type.
	if (strcasestr(ct, "aac") || strcasestr(ct, "mp4a")) {
		return RADIO_CODEC_AAC;
	}
	if (strcasestr(ct, "mpeg") || strcasestr(ct, "mp3")) {
		return RADIO_CODEC_MP3;
	}
	return RADIO_CODEC_UNKNOWN;
}

// The ID3v2 tag a few streams put in front, so the sniff below looks at audio.
static int radio_skip_id3(const drmp3_uint8 *data, int len) {
	int off = 0;
	while (off + 10 <= len && data[off] == 'I' && data[off + 1] == 'D' && data[off + 2] == '3') {
		int size = ((data[off + 6] & 0x7F) << 21) | ((data[off + 7] & 0x7F) << 14) | ((data[off + 8] & 0x7F) << 7) |
				   (data[off + 9] & 0x7F);
		int step = 10 + size;
		if (step <= 0 || off + step > len) {
			break;
		}
		off += step;
	}
	return off;
}

// One ADTS header at `off`, and how long the frame it announces is. Zero when
// there is no header there.
//
// A single syncword match proves nothing: a 0xFF byte inside payload data, with
// almost any byte after it, passes the twelve-bit test. What makes a header a
// header is that the length it declares lands on another one.
static int adts_frame_len(const drmp3_uint8 *data, int len, int off) {
	if (off < 0 || off + 7 > len) {
		return 0;
	}
	// Twelve sync bits, then the layer bits at zero -- which is what tells ADTS
	// from MPEG audio, where the layer is never zero.
	if (data[off] != 0xFF || (data[off + 1] & 0xF6) != 0xF0) {
		return 0;
	}
	// 13, 14 and 15 are reserved sampling frequencies: a header carrying one is
	// not a header.
	if (((data[off + 2] >> 2) & 0x0F) > 12) {
		return 0;
	}
	int frame_len = ((data[off + 3] & 0x03) << 11) | (data[off + 4] << 3) | ((data[off + 5] >> 5) & 0x07);
	// The length covers the seven-byte header (nine with the CRC), so anything
	// shorter is not a frame. The field is thirteen bits, so it cannot overrun.
	return frame_len >= 7 ? frame_len : 0;
}

// How many ADTS frames chain from `off`, each one's length landing on the next.
static int adts_chain(const drmp3_uint8 *data, int len, int off, int want) {
	int seen = 0;
	while (seen < want) {
		int frame_len = adts_frame_len(data, len, off);
		if (frame_len == 0) {
			return seen;
		}
		seen++;
		off += frame_len;
	}
	return seen;
}

// What the bytes are, and it only ever answers AAC or "no".
//
// A positive answer needs a chain of this many ADTS frames, each one's declared
// length landing on the next; anything else is left to the MP3 decoder. The
// asymmetry is deliberate: minimp3 resynchronises and copes with the middle of
// a frame, so guessing MP3 wrongly costs a moment of silence and then the
// buffer-full message, while handing MP3 to fdk-aac yields nothing at all.
//
// The chain is searched for rather than expected at the front: a connection
// joins a live stream wherever it happens to be.
#define ADTS_CHAIN_PROOF 3

static bool bytes_look_like_adts(const drmp3_uint8 *data, int len) {
	for (int off = radio_skip_id3(data, len); off + 7 <= len; off++) {
		if (data[off] != 0xFF) {
			continue;
		}
		if (adts_chain(data, len, off, ADTS_CHAIN_PROOF) >= ADTS_CHAIN_PROOF) {
			return true;
		}
	}
	return false;
}

// A FLAC header at the very front: "fLaC", behind an ID3 tag if there is one,
// or an Ogg page whose first packet is FLAC's (0x7F "FLAC"). UNKNOWN otherwise.
static radio_codec_t bytes_look_like_flac(const drmp3_uint8 *data, int len) {
	int off = radio_skip_id3(data, len);
	if (off + 4 <= len && memcmp(data + off, "fLaC", 4) == 0) {
		return RADIO_CODEC_FLAC;
	}
	if (len >= 27 && memcmp(data, "OggS", 4) == 0) {
		int packet = 27 + data[26];
		if (packet + 5 <= len && data[packet] == 0x7F && memcmp(data + packet + 1, "FLAC", 4) == 0) {
			return RADIO_CODEC_OGG_FLAC;
		}
	}
	return RADIO_CODEC_UNKNOWN;
}

// Whether a connection could turn out to be FLAC, before a byte has been read:
// the Content-Type says FLAC or Ogg, the address says FLAC, or the directory
// lists the station as FLAC. It decides only how large a buffer it gets.
static bool stream_may_be_flac(const char *url, const char *content_type) {
	if (strcasestr(url, "flac") || strcasestr(content_type, "flac") || strcasestr(content_type, "ogg")) {
		return true;
	}
	pthread_mutex_lock(&now_lock);
	bool listed = strcasestr(now_station.codec, "flac") != NULL;
	pthread_mutex_unlock(&now_lock);
	return listed;
}

// ---------------------------------------------------------------------------
// HLS
//
// An HLS station does not send a stream: it sends a list of segments to fetch
// one after another. The fetching is in hls.c; what arrives here is what comes
// out of it -- bare audio, MP3 or AAC -- plus the loop that decodes it and
// feeds the sound card.
//
// A loop of its own rather than the one above because AAC needs another
// decoder, and that decoder works the opposite way round: minimp3 is handed a
// buffer and reports how many bytes it consumed, while fdk-aac keeps them and
// returns frames when it chooses. Fitting both shapes into one loop would make
// it unreadable.
// ---------------------------------------------------------------------------

// What is downloading right now, so another thread can tell it to give up.
// Same reason and same lock as `live_stream`.
static hls_t *live_hls;

static bool play_hls(const char *url, unsigned mine) {
	char why[192] = "";
	fprintf(stderr, "radio: %s is HLS, opening the segmented transport\n", url);

	hls_t *h = hls_open(url, why, sizeof(why));
	if (!h) {
		pthread_mutex_lock(&now_lock);
		snprintf(now_state.error, sizeof(now_state.error), "%.*s", (int)sizeof(now_state.error) - 1,
				 why[0] ? why : tr("radio_unreadable"));
		now_state.playing = false;
		now_state.connecting = false;
		now_touch();
		pthread_mutex_unlock(&now_lock);
		return true; // nothing to retry: it is the format, not the network
	}

	if (!still_mine(mine)) {
		hls_close(h);
		return true;
	}

	pthread_mutex_lock(&stream_lock);
	live_hls = h;
	pthread_mutex_unlock(&stream_lock);

	hls_codec_t codec = hls_codec(h);
	bool clean = false;
	bool ever_decoded = false;
	radio_out_t out = {0};

	drmp3_uint8 *input = radio_input;
	short *pcm = radio_pcm;
	short *stereo = radio_stereo;
	int have = 0;

	drmp3dec mp3;
	drmp3dec_init(&mp3);
	aacdec_t *aac = NULL;

	if (codec == HLS_CODEC_AAC) {
		aac = aacdec_open_adts();
		if (!aac) {
			pthread_mutex_lock(&now_lock);
			snprintf(now_state.error, sizeof(now_state.error), "%s",
					 tr("radio_aac_unsupported"));
			now_state.playing = false;
			now_state.connecting = false;
			now_touch();
			pthread_mutex_unlock(&now_lock);
			clean = true;
			goto done_hls;
		}
	}

	if (!radio_buffer_start(&radio_buffer, NULL, h, RADIO_BUFFER_BYTES)) {
		goto done_hls;
	}
	radio_buffer_prefill(&radio_buffer, mine, RADIO_PREFILL_BYTES, RADIO_PREFILL_MS);

	while (still_mine(mine)) {
		if (have < RADIO_IN_BUFFER) {
			int got = radio_buffer_read(&radio_buffer, input + have, RADIO_IN_BUFFER - have);
			if (got <= 0) {
				break; // ended, or aborted
			}
			have += got;
		}
		if (!still_mine(mine)) {
			clean = true;
			break;
		}

		int consumed = 0;
		int frames = 0;
		int rate = 0, channels = 0;

		if (codec == HLS_CODEC_MP3) {
			// The same slack as the ordinary transport, for the same reason:
			// minimp3 has to see the NEXT frame header before it trusts the
			// frame it is looking at.
			drmp3dec_frame_info info;
			int keep = have > RADIO_DECODE_SLACK ? RADIO_DECODE_SLACK : 0;
			while (have - consumed > keep) {
				frames = drmp3dec_decode_frame(&mp3, input + consumed, have - consumed, pcm, &info);
				if (info.frame_bytes == 0) {
					break;
				}
				consumed += info.frame_bytes;
				if (frames > 0) {
					rate = info.sample_rate;
					channels = info.channels;
					break;
				}
			}
		} else {
			// fdk-aac is handed whatever is there, takes the part it needs,
			// and returns frames through a separate call.
			int taken = aacdec_fill(aac, input, have);
			if (taken < 0) {
				break;
			}
			consumed = taken;
			frames = aacdec_pull(aac, pcm, RADIO_PCM_CAPACITY);
			if (frames > 0) {
				rate = aacdec_sample_rate(aac);
				channels = aacdec_channels(aac);
			}
			if (taken == 0 && frames <= 0) {
				// Took nothing and returned nothing: the buffer holds bytes the
				// decoder cannot use. Dropping it beats stalling.
				have = 0;
				continue;
			}
		}

		if (consumed > 0) {
			memmove(input, input + consumed, (size_t)(have - consumed));
			have -= consumed;
		}

		if (frames <= 0 || rate <= 0 || channels <= 0) {
			continue;
		}
		ever_decoded = true;

		if (!radio_push_pcm(&out, pcm, stereo, frames, rate, channels, 0,
							codec == HLS_CODEC_AAC ? "AAC" : "MP3")) {
			clean = true;
			break;
		}

		if (!still_mine(mine)) {
			clean = true;
			break;
		}
	}

done_hls:
	pthread_mutex_lock(&stream_lock);
	live_hls = NULL;
	pthread_mutex_unlock(&stream_lock);

	radio_buffer_stop(&radio_buffer); // before the source it is reading closes

	if (out.open) {
		audio_external_end();
	}
	aacdec_close(aac);
	hls_close(h);

	if (!ever_decoded && still_mine(mine) && !clean) {
		return false; // worth reconnecting
	}
	return true;
}
// ---------------------------------------------------------------------------
// FLAC
//
// The decoder pulls its bytes rather than being handed them (see radioflac.h):
// first the ones already read for the sniff, then the network buffer's.
// ---------------------------------------------------------------------------

// Sample frames per read: what radio_pcm holds at eight channels, once
// narrowed to 16 bits.
#define RADIO_FLAC_CHUNK (RADIO_PCM_CAPACITY / 8)

typedef struct {
	const drmp3_uint8 *prefix;
	int prefix_len;
	int prefix_at;
	unsigned mine;
} radio_flac_source_t;

static int radio_flac_source_read(void *user, unsigned char *dst, int max) {
	radio_flac_source_t *src = user;
	if (!still_mine(src->mine)) {
		return 0;
	}
	if (src->prefix_at < src->prefix_len) {
		int n = src->prefix_len - src->prefix_at;
		if (n > max) {
			n = max;
		}
		memcpy(dst, src->prefix + src->prefix_at, (size_t)n);
		src->prefix_at += n;
		return n;
	}
	return radio_buffer_read(&radio_buffer, dst, max);
}

// Plays a FLAC station until it ends or is stopped. `prefix` is what was read
// for the sniff. *give_up is set when the stream holds no FLAC at all, which
// another connection would only repeat.
static void play_flac(bool ogg, const drmp3_uint8 *prefix, int prefix_len, unsigned mine, radio_out_t *out, bool *ever_decoded, bool *clean, bool *give_up) {
	radio_flac_source_t src = {prefix, prefix_len, 0, mine};
	radioflac_t *f = radioflac_open(ogg, radio_flac_source_read, &src);
	int32_t *pcm = malloc(sizeof(int32_t) * RADIO_FLAC_CHUNK * 8);
	int32_t *stereo = malloc(sizeof(int32_t) * RADIO_FLAC_CHUNK * 2);
	if (!f || !pcm || !stereo) {
		fprintf(stderr, "radio: no memory for the FLAC decoder\n");
		*clean = true;
		*give_up = true;
		goto done_flac;
	}

	radio_buffer_prefill(&radio_buffer, mine, radio_buffer.capacity / 4 * 3, RADIO_FLAC_PREFILL_MS);

	bool narrow = false; // the card would not take 32-bit frames
	int shown_bits = -1;
	while (still_mine(mine)) {
		int frames = radioflac_read(f, pcm, RADIO_FLAC_CHUNK);
		if (frames < 0) {
			fprintf(stderr, "radio: no FLAC frame in the stream\n");
			pthread_mutex_lock(&now_lock);
			snprintf(now_state.error, sizeof(now_state.error), "%s", tr("radio_unreadable"));
			now_state.playing = false;
			now_state.connecting = false;
			now_touch();
			pthread_mutex_unlock(&now_lock);
			*clean = true;
			*give_up = true;
			break;
		}
		if (frames == 0 || !still_mine(mine)) {
			break; // the station closed, went quiet past the timeout, or was stopped
		}

		// What the station announces over ICY, and what the stream's own
		// comments name at each new song of an Ogg chain.
		char announced[256];
		while (radio_buffer_take_title(&radio_buffer, announced, sizeof(announced)) || radioflac_take_title(f, announced, sizeof(announced))) {
			onair_clean(announced, sizeof(announced));
			pthread_mutex_lock(&now_lock);
			snprintf(now_state.title, sizeof(now_state.title), "%s", announced);
			now_touch();
			pthread_mutex_unlock(&now_lock);
		}

		int rate, channels, bits;
		radioflac_format(f, &rate, &channels, &bits);
		if (bits != shown_bits) {
			shown_bits = bits;
			pthread_mutex_lock(&now_lock);
			now_state.bits = bits;
			now_touch();
			pthread_mutex_unlock(&now_lock);
		}
		*ever_decoded = true;

		if (bits > 16 && !narrow) {
			if (radio_push_pcm32(out, pcm, stereo, frames, rate, channels, "FLAC")) {
				continue;
			}
			fprintf(stderr, "radio: playing the %d-bit station at 16 bit\n", bits);
			narrow = true;
		}
		short *pcm16 = radio_pcm;
		for (int i = 0; i < frames * channels; i++) {
			pcm16[i] = (short)(pcm[i] >> 16);
		}
		if (!radio_push_pcm(out, pcm16, radio_stereo, frames, rate, channels, 0, "FLAC")) {
			*clean = true; // no point reconnecting: the DAC said no
			break;
		}
	}

done_flac:
	radioflac_close(f);
	free(pcm);
	free(stereo);
}

// Decodes one connection's worth of stream. Returns true if it ended cleanly
// or was stopped, false if it broke and is worth reconnecting.
static bool play_one_connection(const char *url, unsigned mine, char *redirect, size_t redirect_size,
								bool *redirect_hls) {
	redirect[0] = '\0';

	http_stream_t stream;
	if (!http_stream_open(&stream, url, 10)) {
		return false;
	}

	// Published so a stop from another thread can shut the socket down and
	// bring the reads below back at once. Cleared again before this returns.
	pthread_mutex_lock(&stream_lock);
	if (!still_mine(mine)) {
		// Stopped while the connection was being made: let go of it here
		// rather than start playing something nobody is waiting for.
		pthread_mutex_unlock(&stream_lock);
		http_stream_close(&stream);
		return true;
	}
	live_stream = &stream;
	pthread_mutex_unlock(&stream_lock);

	// On stderr rather than stdout, which is buffered when the player's output
	// is redirected to a file.
	fprintf(stderr, "radio: connected (%s%s)\n", stream.content_type[0] ? stream.content_type : "?",
			stream.icy_metaint ? ", icy metadata" : "");

	// The address looked like a stream but the server is handing out a
	// playlist. Read the little of it there is, take the address out and tell
	// the caller to start again there. This is the half of the playlist
	// problem the file extension cannot catch.
	if (content_type_is_playlist(stream.content_type)) {
		char text[PLAYLIST_MAX_BYTES];
		int got = 0;
		while (got < (int)sizeof(text) - 1) {
			int n = http_stream_read(&stream, text + got, (int)sizeof(text) - 1 - got);
			if (n <= 0) {
				break;
			}
			got += n;
		}
		text[got > 0 ? got : 0] = '\0';

		// An HLS manifest is a list too, but of segments of one stream, so
		// playlist_first_url() would take the first segment and stop after it.
		// A manifest often arrives from an address without .m3u8, so the name
		// check is not enough: what decides is what the server actually sent.
		bool served_hls = got > 0 && hls_text_looks_like(text, (size_t)got);

		char next[RADIO_URL_MAX];
		bool found = !served_hls && got > 0 && playlist_first_url(text, next, sizeof(next));

		pthread_mutex_lock(&stream_lock);
		if (live_stream == &stream) {
			live_stream = NULL;
		}
		pthread_mutex_unlock(&stream_lock);
		http_stream_close(&stream);

		if (served_hls) {
			printf("radio: the server sent an HLS manifest, switching transport\n");
			snprintf(redirect, redirect_size, "%s", url);
			*redirect_hls = true;
			return true;
		}

		if (found) {
			printf("radio: server sent a playlist -> %s\n", next);
			snprintf(redirect, redirect_size, "%s", next);
		} else {
			pthread_mutex_lock(&now_lock);
			snprintf(now_state.error, sizeof(now_state.error), "%s",
					 tr("radio_playlist_empty"));
			now_touch();
			pthread_mutex_unlock(&now_lock);
		}
		return true; // handled either way: no reconnect to the same address
	}

	// The station keeps the name it had in the list, which is the one the user
	// picked. The stream's own icy-name only fills in when there is no name.
	pthread_mutex_lock(&now_lock);
	if (!now_state.station[0] && stream.icy_name[0]) {
		snprintf(now_state.station, sizeof(now_state.station), "%s", stream.icy_name);
		now_touch();
	}
	pthread_mutex_unlock(&now_lock);

	drmp3dec decoder;
	drmp3dec_init(&decoder);

	drmp3_uint8 *input = radio_input;
	short *pcm = radio_pcm;
	short *stereo = radio_stereo;
	int have = 0;

	radio_out_t out = {0};
	bool clean = false;
	bool ended = false;
	bool ever_decoded = false;
	bool give_up = false; // the stream is not what it claimed, and will not be on the next try
	time_t opened_at = time(NULL);

	// What the header claims. The bytes get the last word, once there are some:
	// see the note over codec_from_content_type().
	radio_codec_t codec = codec_from_content_type(stream.content_type);
	bool codec_settled = false;
	aacdec_t *aac = NULL;

	int capacity = stream_may_be_flac(url, stream.content_type) ? RADIO_FLAC_BUFFER_BYTES : RADIO_BUFFER_BYTES;
	if (!radio_buffer_start(&radio_buffer, &stream, NULL, capacity)) {
		goto done;
	}
	radio_buffer_prefill(&radio_buffer, mine, RADIO_PREFILL_BYTES, RADIO_PREFILL_MS);

	while (still_mine(mine)) {
		// Connected, receiving, and nothing has come out of the decoder for
		// RADIO_SILENCE_TIMEOUT seconds. Stop and say so rather than reconnect,
		// which would only repeat it.
		if (!ever_decoded && difftime(time(NULL), opened_at) >= RADIO_SILENCE_TIMEOUT) {
			fprintf(stderr, "radio: %d s connected with no decodable audio (%s)\n", RADIO_SILENCE_TIMEOUT,
					stream.content_type[0] ? stream.content_type : "no content-type");
			pthread_mutex_lock(&now_lock);
			if (stream.content_type[0] && strcasestr(stream.content_type, "mpeg") == NULL) {
				snprintf(now_state.error, sizeof(now_state.error),
						 tr("radio_format_unsupported"),
						 stream.content_type);
			} else {
				snprintf(now_state.error, sizeof(now_state.error), "%s",
						 tr("radio_not_mp3"));
			}
			now_state.playing = false;
			now_state.connecting = false;
			now_touch();
			pthread_mutex_unlock(&now_lock);
			clean = true;
			break;
		}

		if (!ended && have < RADIO_IN_BUFFER) {
			int got = radio_buffer_read(&radio_buffer, input + have, RADIO_IN_BUFFER - have);
			if (got <= 0) {
				ended = true; // the station closed, or went quiet past the timeout
			} else {
				have += got;
			}
		}

		char announced[sizeof(stream.stream_title)];
		if (radio_buffer_take_title(&radio_buffer, announced, sizeof(announced))) {
			onair_clean(announced, sizeof(announced));
			pthread_mutex_lock(&now_lock);
			snprintf(now_state.title, sizeof(now_state.title), "%s", announced);
			now_touch();
			pthread_mutex_unlock(&now_lock);
		}

		// The bytes decide, once there are enough of them to look at, and they
		// override a Content-Type that said MP3 over an AAC stream.
		//
		// Once only, and before anything is decoded: the MP3 decoder needs
		// RADIO_DECODE_SLACK bytes in hand before it touches the buffer, and
		// that is the same threshold, so the answer is always in before the
		// first frame. A second look later would swap the decoder out from
		// under a stream that is already playing.
		//
		// A FLAC header is looked for first. Without one, a Content-Type that
		// says FLAC is still taken at its word unless the bytes are AAC: a
		// native stream joined midway is opened from its first whole frame.
		if (!codec_settled && have >= RADIO_DECODE_SLACK) {
			radio_codec_t sniffed = bytes_look_like_flac(input, have);
			if (sniffed == RADIO_CODEC_UNKNOWN) {
				sniffed = bytes_look_like_adts(input, have) ? RADIO_CODEC_AAC : codec == RADIO_CODEC_FLAC ? RADIO_CODEC_FLAC : RADIO_CODEC_MP3;
			}
			if (sniffed != codec) {
				fprintf(stderr, "radio: header said %s, the stream is %s\n", codec_label(codec), codec_label(sniffed));
			}
			codec = sniffed;
			codec_settled = true;
		}

		if (codec_settled && codec_is_flac(codec)) {
			play_flac(codec == RADIO_CODEC_OGG_FLAC, input, have, mine, &out, &ever_decoded, &clean, &give_up);
			goto done;
		}

		if (codec == RADIO_CODEC_AAC && !aac) {
			aac = aacdec_open_adts();
			if (!aac) {
				pthread_mutex_lock(&now_lock);
				snprintf(now_state.error, sizeof(now_state.error), "%s", tr("radio_aac_unsupported"));
				now_state.playing = false;
				now_state.connecting = false;
				now_touch();
				pthread_mutex_unlock(&now_lock);
				clean = true;
				break;
			}
			fprintf(stderr, "radio: stream AAC (ADTS), decoder open\n");
		}

		// Leave RADIO_DECODE_SLACK bytes behind unless the stream is over and
		// there is nothing more coming to complete them.
		int keep = ended ? 0 : RADIO_DECODE_SLACK;
		int consumed = 0;

		if (aac) {
			// fdk-aac works the opposite way round from minimp3: it is handed
			// whatever is there, takes the part it needs, and returns frames
			// through a separate call. One frame per pass round the outer loop
			// is enough -- an HE-AAC frame is 2048 samples, some 46 ms, and the
			// buffer refills while that plays.
			int taken = aacdec_fill(aac, input, have);
			if (taken < 0) {
				break;
			}
			consumed = taken;
			int frames = aacdec_pull(aac, pcm, RADIO_PCM_CAPACITY);
			if (frames > 0) {
				ever_decoded = true;
				if (!radio_push_pcm(&out, pcm, stereo, frames, aacdec_sample_rate(aac), aacdec_channels(aac), 0,
									"AAC")) {
					clean = true; // no point reconnecting: the DAC said no
					goto done;
				}
				if (!still_mine(mine)) {
					clean = true;
					goto done;
				}
			} else if (taken == 0) {
				// Took nothing and returned nothing: the buffer holds bytes the
				// decoder cannot use. Dropping it beats stalling.
				have = 0;
				continue;
			}
		} else {
			while (have - consumed > keep) {
				drmp3dec_frame_info info;
				int frames = drmp3dec_decode_frame(&decoder, input + consumed, have - consumed, pcm, &info);

				if (info.frame_bytes == 0) {
					break; // not a whole frame in hand yet
				}
				consumed += info.frame_bytes;

				if (frames == 0) {
					continue; // an ID3 block, or the frame a resync lands on
				}
				ever_decoded = true;

				if (!radio_push_pcm(&out, pcm, stereo, frames, info.sample_rate, info.channels, info.bitrate_kbps,
									"MP3")) {
					clean = true; // no point reconnecting: the DAC said no
					goto done;
				}

				if (!still_mine(mine)) {
					clean = true;
					goto done;
				}
			}
		}

		if (consumed > 0) {
			have -= consumed;
			memmove(input, input + consumed, (size_t)have);
		} else if (ended) {
			break; // nothing left that can be decoded
		} else if (have == RADIO_IN_BUFFER) {
			// A full buffer with not one decodable frame in it is neither of
			// the two formats this can read. Say so where the user is looking
			// instead of leaving a station that is silent for no visible
			// reason.
			fprintf(stderr, "radio: this stream is neither MP3, AAC nor FLAC\n");
			pthread_mutex_lock(&now_lock);
			snprintf(now_state.error, sizeof(now_state.error), "%s",
					 tr("radio_format_unsupported_generic"));
			now_touch();
			pthread_mutex_unlock(&now_lock);
			clean = true;
			break;
		}
	}

	if (!still_mine(mine)) {
		clean = true;
	}

done:
	// Off the shared slot before anything is torn down, so a stop arriving now
	// cannot shut down a socket this thread is already closing.
	pthread_mutex_lock(&stream_lock);
	if (live_stream == &stream) {
		live_stream = NULL;
	}
	pthread_mutex_unlock(&stream_lock);

	radio_buffer_stop(&radio_buffer); // before the source it is reading closes

	if (out.open) {
		audio_external_end();
	}
	aacdec_close(aac); // a no-op when the stream was MP3 and one was never opened
	http_stream_close(&stream);

	// A connection that produced no audio at all is not worth retrying the
	// same way -- but one that played for a while and dropped is.
	if (!ever_decoded && still_mine(mine) && !give_up) {
		return false;
	}
	return clean;
}

// The click counter, on a thread of its own. It must not run on the playing
// thread: the answer is of no interest -- the resolved address is already in
// hand -- and with no network the three mirrors are tried in turn, which would
// sit in front of the audio for the length of three timeouts.
static void *click_main(void *arg) {
	char *path = arg;
	thread_be_background("radio click");
	free(api_get(path));
	free(path);
	return NULL;
}

static void report_click(const char *uuid) {
	if (!uuid || !uuid[0]) {
		return;
	}

	char path[128];
	snprintf(path, sizeof(path), "/json/url/%s", uuid);

	char *copy = strdup(path);
	if (!copy) {
		return;
	}

	pthread_t t;
	if (pthread_create(&t, NULL, click_main, copy) == 0) {
		pthread_detach(t);
	} else {
		free(copy);
	}
}

static void *play_main(void *arg) {
	unsigned mine = (unsigned)(uintptr_t)arg;
	// NOT background: this thread feeds the DAC, and starving it is a dropout.

	char url[RADIO_URL_MAX];
	char favicon[RADIO_URL_MAX];
	char uuid[40];
	pthread_mutex_lock(&now_lock);
	snprintf(url, sizeof(url), "%s", now_station.url);
	snprintf(favicon, sizeof(favicon), "%s", now_station.favicon);
	snprintf(uuid, sizeof(uuid), "%s", now_station.uuid);
	pthread_mutex_unlock(&now_lock);

	// The icon first and on this thread: it is one small download, and having
	// it before the audio starts means the player opens with the artwork
	// already there rather than popping it in a second later.
	fetch_favicon(favicon);

	// The directory asks to be told when a station is started -- it is how the
	// popularity ordering everybody browses by gets its numbers.
	report_click(uuid);

	// If the address is a .pls or .m3u, swap it for what it points at before
	// the first connection. Cheap, and it is the difference between a station
	// that plays and one that connects and stays silent.
	char why[128] = "";
	bool is_hls = false;
	if (!resolve_playlist(url, sizeof(url), why, sizeof(why), &is_hls)) {
		pthread_mutex_lock(&now_lock);
		snprintf(now_state.error, sizeof(now_state.error), "%s", why);
		now_state.playing = false;
		now_state.connecting = false;
		now_touch();
		pthread_mutex_unlock(&now_lock);
		play_thread_running = false;
		printf("radio: transport finished (%s)\n", why);
		return NULL;
	}

	// A playlist the SERVER handed back rather than the address advertising
	// itself as one. Following it must not eat a retry, so it is counted
	// separately and bounded on its own.
	int redirects_left = PLAYLIST_MAX_HOPS;

	for (int attempt = 0; attempt <= RADIO_MAX_RETRIES && still_mine(mine); attempt++) {
		if (attempt > 0) {
			pthread_mutex_lock(&now_lock);
			now_state.connecting = true;
			now_state.playing = false;
			now_touch();
			pthread_mutex_unlock(&now_lock);

			printf("radio: reconnecting (%d/%d)\n", attempt, RADIO_MAX_RETRIES);
			for (int i = 0; i < attempt * 2 && still_mine(mine); i++) {
				sleep(1);
			}
		}

		char redirect[RADIO_URL_MAX];
		redirect[0] = '\0';
		bool done;
		bool redirect_hls = false;
		if (is_hls) {
			done = play_hls(url, mine);
		} else {
			done = play_one_connection(url, mine, redirect, sizeof(redirect), &redirect_hls);
		}

		if (redirect[0] && redirects_left-- > 0 && still_mine(mine)) {
			snprintf(url, sizeof(url), "%s", redirect);
			// The hop can land on HLS two ways: the server just sent the
			// manifest, or the new address ends in .m3u8. Both count.
			is_hls = redirect_hls || url_is_hls(url);
			attempt--; // following a playlist is not a failed attempt
			continue;
		}
		if (done) {
			break;
		}
	}

	// Only the current thread may touch the shared state on its way out: a
	// thread that has been superseded is finishing late, and the station it
	// would clear belongs to whoever replaced it.
	if (still_mine(mine)) {
		pthread_mutex_lock(&now_lock);
		if (!now_state.playing && !now_state.error[0]) {
			snprintf(now_state.error, sizeof(now_state.error), "%s", tr("radio_station_unreachable"));
		}
		now_state.playing = false;
		now_state.connecting = false;
		// The station stays loaded and stopped, which is also what the Stop
		// button leaves behind. A station that dropped for good and one the
		// user stopped look the same on screen, and press play to come back.
		now_touch();
		pthread_mutex_unlock(&now_lock);
	}

	play_thread_running = false;
	printf("radio: transport finished\n");
	return NULL;
}

// Brings the playing thread down and waits for it to be gone. Bumping the
// generation is noticed at every step of the loop, and shutting the socket down
// brings a blocked read back at once. The join is required, not a courtesy: two
// playing threads must never be alive at the same time -- see play_generation.
static void transport_stop(void) {
	play_generation++;

	pthread_mutex_lock(&stream_lock);
	if (live_stream) {
		http_stream_wake(live_stream);
	}
	// The same nudge for HLS: there is no open socket to shut down -- segments
	// are fetched one at a time -- so the abort flag is the only way to stop it
	// without waiting out the download in progress.
	if (live_hls) {
		hls_abort(live_hls);
	}
	pthread_mutex_unlock(&stream_lock);

	if (play_thread_valid) {
		pthread_join(play_thread, NULL);
		play_thread_valid = false;
	}
	play_thread_running = false;
}

// Starts the transport for whatever is in now_station. The caller owns the
// now_state bookkeeping.
static bool transport_start(void) {
	transport_stop();

	unsigned mine = ++play_generation;
	play_thread_running = true;

	if (pthread_create(&play_thread, NULL, play_main, (void *)(uintptr_t)mine) != 0) {
		play_thread_running = false;
		return false;
	}
	play_thread_valid = true;
	return true;
}

bool radio_play(const radio_station_t *station) {
	if (!station || radio_station_problem(station)) {
		return false;
	}

	transport_stop();

	// One PCM takes one writer: whatever was playing locally has to go first.
	// A track it stops has been left.
	playhistory_left();
	audio_stop();

	pthread_mutex_lock(&now_lock);
	now_station = *station;
	memset(&now_state, 0, sizeof(now_state));
	now_state.active = true;
	now_state.connecting = true;
	snprintf(now_state.station, sizeof(now_state.station), "%s", station->name);
	now_state.bitrate = station->bitrate;
	now_touch();
	pthread_mutex_unlock(&now_lock);

	recent_note(station);

	if (!transport_start()) {
		pthread_mutex_lock(&now_lock);
		memset(&now_state, 0, sizeof(now_state));
		now_touch();
		pthread_mutex_unlock(&now_lock);
		return false;
	}

	printf("radio: playing '%s' (%s)\n", station->name, station->url);
	return true;
}

void radio_stop(void) {
	transport_stop();

	pthread_mutex_lock(&now_lock);
	// The station stays loaded: stopping a live stream is not leaving it. The
	// player keeps showing which station it is on, with a play button that
	// reconnects -- there is no position to resume from, so starting again is
	// a fresh connection either way.
	now_state.playing = false;
	now_state.connecting = false;
	now_state.title[0] = '\0'; // whatever was on air has moved on by now
	now_touch();
	pthread_mutex_unlock(&now_lock);
}

bool radio_resume(void) {
	pthread_mutex_lock(&now_lock);
	bool loaded = now_state.active && now_station.url[0];
	if (loaded) {
		now_state.connecting = true;
		now_state.playing = false;
		now_state.error[0] = '\0';
		now_touch();
	}
	pthread_mutex_unlock(&now_lock);

	if (!loaded) {
		return false;
	}

	audio_stop(); // the local track may have been started in the meantime
	return transport_start();
}

void radio_clear(void) {
	transport_stop();

	pthread_mutex_lock(&now_lock);
	memset(&now_state, 0, sizeof(now_state));
	memset(&now_station, 0, sizeof(now_station));
	now_touch();
	pthread_mutex_unlock(&now_lock);
}

bool radio_is_playing(void) {
	pthread_mutex_lock(&now_lock);
	bool on = now_state.active && (now_state.playing || now_state.connecting);
	pthread_mutex_unlock(&now_lock);
	return on;
}

bool radio_is_connecting(void) {
	pthread_mutex_lock(&now_lock);
	bool connecting = now_state.active && now_state.connecting;
	pthread_mutex_unlock(&now_lock);
	return connecting;
}

bool radio_is_active(void) {
	pthread_mutex_lock(&now_lock);
	bool active = now_state.active;
	pthread_mutex_unlock(&now_lock);
	return active;
}

void radio_get_now(radio_now_t *out) {
	if (!out) {
		return;
	}
	pthread_mutex_lock(&now_lock);
	*out = now_state;
	pthread_mutex_unlock(&now_lock);
}

unsigned radio_now_serial(void) {
	pthread_mutex_lock(&now_lock);
	unsigned s = now_serial;
	pthread_mutex_unlock(&now_lock);
	return s;
}

bool radio_current_station(radio_station_t *out) {
	pthread_mutex_lock(&now_lock);
	bool active = now_state.active;
	if (active && out) {
		*out = now_station;
	}
	pthread_mutex_unlock(&now_lock);
	return active;
}

void radio_init(void) {
	if (worker_started) {
		return;
	}
	worker_started = true;

	pthread_t t;
	if (pthread_create(&t, NULL, worker_main, NULL) == 0) {
		pthread_detach(t);
	}
}
