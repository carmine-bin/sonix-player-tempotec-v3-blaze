#include "wifitransfer.h"

#include "src/system/core/respath.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>

#include "src/system/net/mdns.h"
#include "src/system/device/system.h"
#include "src/system/net/webpage.h"
#include "src/system/net/wifi.h"
#include "src/system/core/lang.h"
#include "src/system/core/logging.h"

// The port is the stock player's, and it is not arbitrary: HiBy's phone app
// looks for the beacon on it, and the address printed on screen has to be the
// one the firmware's own page is served from or nothing else about this
// matches.
#define WT_PORT 4399

// And the port the same page is served on for people typing the name, where a
// ":4399" would defeat the point of having a name at all.
#define WT_PORT_NAMED 80

// What the page answers to on the network: http://sonix-transfer.local
#define WT_MDNS_NAME "sonix-transfer"

#define THTTPD_BIN "/usr/bin/thttpd"
#define UDP_SERVER_BIN "/usr/bin/udp_server"
#define CGIC_DISABLE "/usr/bin/cgic_disable"

#define WEB_SRC "/usr/share/web"	   // read-only, as shipped
#define WEB_PARENT "/usr/data/share"   // where the stock script puts its copy
#define WEB_DIR WEB_PARENT "/web"

// The two colour literals the template carries, and what a replacement must
// look like. Kept next to each other because they are one contract with
// web/index.html: seven characters, one occurrence each.
#define ACCENT_TOKEN "#3584e4"
#define ACCENT_PRESSED_TOKEN "#2b6fc4"
#define ACCENT_TOKEN_LEN 7
#define ACCENT_DEFAULT 0x3584e4u

// In /tmp on purpose: both are rewritten on every start and there is no reason
// to spend a flash erase block on them.
#define CONF_PATH "/tmp/sonix-thttpd.conf"
#define CONF_NAMED "/tmp/sonix-thttpd-80.conf"

// Where the CGI programs look for the card. Not a choice: it is compiled into
// all six of them, along with sd_1 and the two udisk names.
#define CGI_CARD_ROOT "/data/mnt/sd_0"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_enabled;
static bool g_card_bound;

// ---------------------------------------------------------------------------
// running things
// ---------------------------------------------------------------------------

// fork+exec, never a shell: nothing here takes a string from the user, but the
// habit is what keeps it that way.
static int run(const char *path, char *const argv[]) {
	pid_t pid = fork();
	if (pid < 0) {
		perror("wifitransfer: fork");
		return -1;
	}
	if (pid == 0) {
		logging_child_stdio(); // thttpd and the UDP server stay running
		execv(path, argv);
		_exit(127);
	}

	int status = 0;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) {
			return -1;
		}
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static bool process_running(const char *name) {
	DIR *proc = opendir("/proc");
	if (!proc) {
		return false;
	}

	bool found = false;
	struct dirent *de;

	while (!found && (de = readdir(proc)) != NULL) {
		if (de->d_name[0] < '0' || de->d_name[0] > '9') {
			continue; // not a pid
		}

		char path[320];
		snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);

		FILE *f = fopen(path, "r");
		if (!f) {
			continue;
		}

		char comm[64] = "";
		if (fgets(comm, sizeof(comm), f)) {
			comm[strcspn(comm, "\r\n")] = '\0';
			found = (strcmp(comm, name) == 0);
		}
		fclose(f);
	}

	closedir(proc);
	return found;
}

static bool exists(const char *path) { return access(path, F_OK) == 0; }

// ---------------------------------------------------------------------------
// putting the card where the CGIs will find it
//
// The six CGI programs have their four paths compiled in -- /data/mnt/sd_0,
// /data/mnt/sd_1, /data/mnt/udisk_0, /data/mnt/udisk_1 -- and there is no way
// to tell them about another one.
//
// HiBy OS has three names for the same directory (/mnt, /data/mnt,
// /usr/data/mnt) and which of them are symlinks to which differs between
// builds. This player mounts the card on whichever of the three it finds, which
// on this firmware is /mnt/sd_0 -- a different directory from /data/mnt/sd_0,
// so the CGIs would read an empty one.
//
// Rather than guess at the symlink layout, the mount point is bound into place:
// same directory, two names, no copying and no second mount of the card itself.
// It goes away with the server.
// ---------------------------------------------------------------------------

static bool same_directory(const char *a, const char *b) {
	struct stat sa, sb;
	if (stat(a, &sa) != 0 || stat(b, &sb) != 0) {
		return false;
	}
	return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static void bind_card(void) {
	const char *root = storage_sd_root();
	if (!root || !root[0]) {
		printf("wifitransfer: no card mounted; the page will have nothing to show\n");
		return;
	}

	if (same_directory(root, CGI_CARD_ROOT)) {
		return; // already one and the same: nothing to do
	}

	mkdir("/data", 0755);
	mkdir("/data/mnt", 0755);
	mkdir(CGI_CARD_ROOT, 0755);

	if (mount(root, CGI_CARD_ROOT, NULL, MS_BIND, NULL) == 0) {
		g_card_bound = true;
		printf("wifitransfer: %s is also %s now (bind mount)\n", root, CGI_CARD_ROOT);
	} else {
		printf("wifitransfer: cannot bind %s onto %s: %s\n", root, CGI_CARD_ROOT, strerror(errno));
	}
}

static void unbind_card(void) {
	if (!g_card_bound) {
		return;
	}

	if (umount(CGI_CARD_ROOT) != 0 && umount2(CGI_CARD_ROOT, MNT_DETACH) != 0) {
		printf("wifitransfer: cannot unbind %s: %s\n", CGI_CARD_ROOT, strerror(errno));
	}
	g_card_bound = false;
}

// ---------------------------------------------------------------------------
// the document root
//
// There are two places the stock page lives, which is worth being clear about:
//
//   /usr/share/web        as shipped, read-only, with the six CGI programs
//   /usr/data/share/web   the copy thttpd is actually pointed at
//                         (dir= in /etc/thttpd.conf), made once by cgic_enable
//
// cgic_enable copies the first onto the second only when the second is missing,
// then overwrites index.html and js/index.js with the Chinese or English
// variant from pages/, chosen from /usr/data/region. So a player that has ever
// had the feature on keeps whatever tree it got the first time -- which is why
// the stock page can be several firmware versions old.
//
// The copy still has to happen here, because the CGI programs must sit in the
// document root and the shipped directory is where they are. What does not
// happen here is the language dance: the page written into the root is this
// player's own (webpage.h, generated from web/index.html), every start. It has
// no CSS or JS files of its own -- everything is in the one file -- so the
// css/, js/ and pages/ directories that come along with the copy are simply
// never asked for.
//
// The one thing the page does fetch is the typeface, and that is why fonts/
// gets built below.
//
// If the copy will not go -- a full userdata partition being the realistic
// reason -- the shipped tree is served read-only instead, and then the page is
// whatever HiBy shipped, in Chinese. Everything still works.
// ---------------------------------------------------------------------------

// The player draws with /usr/resource/sonix/fonts/default.otf (MiSans Regular, see
// src/gui/fonts/fonts.c). The page asks for it as fonts/default.otf so it can
// render in the same face as the screen it came from.
//
// Symlinked, not copied: it is 6.5 MB of full CJK coverage and there is no
// reason to have two of it on a device with ten megabytes free. thttpd follows
// symlinks -- nosymlink is off unless it is chrooted, and it is not.
//
// Regular and bold, in whichever container this firmware ships them: some have
// them as .ttf, some as .otf. Both names get linked when both are there, and the
// page's @font-face lists both URLs so the browser keeps the one that loads.
//
// Nothing here is fatal. The page declares the faces with font-display: swap
// behind a system-font fallback, so a name that is not there costs it the
// typeface and nothing else.
#define FIRMWARE_FONT_DIR SONIX_RESOURCE_DIR "/fonts"

// The player, linked into the document root as a CGI.
//
// The firmware's list.cgi drops every entry whose name starts with a dot -- it
// is a byte compare on d_name[0] in a compiled MIPS binary, so "show hidden
// files" cannot be answered by the page. This gives thttpd a listing program
// that can: the player itself, which recognises the name and prints JSON
// instead of starting up (see src/system/webcgi.h).
//
// Symlinked, so it is always the running build, and named distinctly so it
// never shadows the firmware's own.
static void link_self_as_cgi(const char *web_root) {
	char exe[PATH_MAX];
	ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (len <= 0) {
		printf("wifitransfer: cannot find my own path; the listing falls back to the firmware's\n");
		return;
	}
	exe[len] = '\0';

	char to[PATH_MAX];
	snprintf(to, sizeof(to), "%s/sonix-list", web_root);

	// Always point it at this build, never at a link left behind by another.
	unlink(to);

	// And the older name for the same thing, so a document root that has seen
	// an earlier build is not left with a link to a binary that is not there.
	char legacy[PATH_MAX];
	if (snprintf(legacy, sizeof(legacy), "%s/ohp-list", web_root) < (int)sizeof(legacy)) {
		unlink(legacy);
	}

	// A hard link if the two are on the same filesystem, because then there is
	// no symlink for thttpd to resolve and refuse (see write_config below).
	// Usually they are not, so the symlink is the normal case and the config
	// line is what makes it work.
	if (link(exe, to) == 0) {
		printf("wifitransfer: %s == %s (directory listing)\n", to, exe);
		return;
	}

	if (symlink(exe, to) != 0) {
		printf("wifitransfer: cannot link %s -> %s: %s\n", to, exe, strerror(errno));
		return;
	}

	printf("wifitransfer: %s -> %s (directory listing)\n", to, exe);
}

static void link_fonts(const char *web_root) {
	static const char *const faces[] = {"default.ttf", "default.otf", "bold.ttf", "bold.otf"};

	char dir[512];
	snprintf(dir, sizeof(dir), "%s/fonts", web_root);

	bool made_dir = false;
	int linked = 0;

	for (unsigned i = 0; i < sizeof(faces) / sizeof(faces[0]); i++) {
		char from[512];
		snprintf(from, sizeof(from), FIRMWARE_FONT_DIR "/%s", faces[i]);
		if (access(from, R_OK) != 0) {
			continue; // not the name this firmware uses
		}

		if (!made_dir) {
			mkdir(dir, 0755);
			made_dir = true;
		}

		char to[600];
		snprintf(to, sizeof(to), "%s/%s", dir, faces[i]);

		if (symlink(from, to) == 0 || errno == EEXIST) {
			linked++;
		} else {
			printf("wifitransfer: cannot link %s -> %s: %s\n", to, from, strerror(errno));
		}
	}

	printf("wifitransfer: %d font file(s) from " FIRMWARE_FONT_DIR " reachable as fonts/\n", linked);
}

// The accent the page should wear, as 0xRRGGBB. Set from the interface before
// the server comes up, so the browser gets the colour the device is actually
// wearing rather than the blue the template was written in.
static uint32_t accent_rgb = ACCENT_DEFAULT;

void wifitransfer_set_accent(uint32_t rgb) { accent_rgb = rgb & 0xFFFFFFu; }

// The pressed shade: the same colour at four fifths, which is how the template
// derives its own and close enough to what the interface does.
static uint32_t accent_pressed(uint32_t rgb) {
	uint32_t r = ((rgb >> 16) & 0xFF) * 4 / 5;
	uint32_t g = ((rgb >> 8) & 0xFF) * 4 / 5;
	uint32_t b = (rgb & 0xFF) * 4 / 5;
	return (r << 16) | (g << 8) | b;
}

// Writes the page: the two accent literals swapped for the device's own, and
// every {{...}} marker replaced by its translation.
//
// The accent is a byte-for-byte swap rather than a placeholder, because
// "#rrggbb" is seven characters either way and nothing has to be allocated to
// do it. The text cannot be: a translation is not the length of the source.
//
// So the page is written in one pass over the template, copying the stretches
// between the interesting bits and substituting where they are. Nothing bigger
// than one marker is ever held in memory -- the page is 80 KB and this runs on
// a device with ten megabytes free.
//
// A marker is {{text}} or {{js|text}}, written into web/index.html and
// collected into the language files by tools/extract_strings.py, so a word that
// appears both on the screen and on this page is translated once. The text is
// the key either way; the prefix only says how the translation has to be
// spelled once it is in the page. A marker with no translation behind it comes
// back from tr() unchanged, in the source language.
#define MARKER_MAX 256
#define MARKER_JS "js|"

// HTML escaping. Deliberately also escapes the two quote characters even in
// text content, because several of these markers sit inside HTML that
// JavaScript builds as a string literal: with no quote and no backslash left
// in the output, the result is safe in single quotes, in double quotes, and in
// an attribute, all at once.
static size_t escape_html(const char *in, char *out, size_t out_size) {
	size_t n = 0;
	for (const char *p = in; *p; p++) {
		const char *entity = NULL;
		switch (*p) {
		case '&': entity = "&amp;"; break;
		case '<': entity = "&lt;"; break;
		case '>': entity = "&gt;"; break;
		case '"': entity = "&quot;"; break;
		case '\'': entity = "&#39;"; break;
		default: break;
		}
		size_t want = entity ? strlen(entity) : 1;
		if (n + want >= out_size) {
			break;
		}
		if (entity) {
			memcpy(out + n, entity, want);
		} else {
			out[n] = *p;
		}
		n += want;
	}
	out[n] = '\0';
	return n;
}

// JavaScript string escaping, for text that reaches the page as a value rather
// than as markup. Both quote characters, so the result drops into either kind
// of string literal -- and the backslash first, or the escapes would escape
// each other. The French "n'est pas" is the reason this exists: an apostrophe
// closes a single-quoted string and takes the whole page down with it.
static size_t escape_js(const char *in, char *out, size_t out_size) {
	size_t n = 0;
	for (const char *p = in; *p; p++) {
		bool quoted = (*p == '\\' || *p == '"' || *p == '\'');
		size_t want = quoted ? 2u : 1u;
		if (n + want >= out_size) {
			break;
		}
		if (quoted) {
			out[n++] = '\\';
		}
		out[n++] = *p;
	}
	out[n] = '\0';
	return n;
}

static bool install_page(const char *web_root) {
	char page[512];
	snprintf(page, sizeof(page), "%s/index.html", web_root);

	FILE *f = fopen(page, "w");
	if (!f) {
		printf("wifitransfer: cannot write %s: %s\n", page, strerror(errno));
		return false;
	}

	const char *src = WIFITRANSFER_PAGE;
	size_t len = sizeof(WIFITRANSFER_PAGE) - 1; // the NUL is not part of the page

	char accent_now[8], accent_down[8];
	snprintf(accent_now, sizeof(accent_now), "#%06x", (unsigned)accent_rgb);
	snprintf(accent_down, sizeof(accent_down), "#%06x", (unsigned)accent_pressed(accent_rgb));

	bool ok = true;
	size_t i = 0, run = 0; // `run` starts the stretch still waiting to be written
	int translated = 0;

	while (ok && i < len) {
		const char *replacement = NULL;
		size_t consumed = 0;
		char text[MARKER_MAX];
		char escaped[MARKER_MAX * 6]; // every character could become "&quot;"

		if (src[i] == '{' && i + 1 < len && src[i + 1] == '{') {
			const char *close = strstr(src + i + 2, "}}");
			size_t inner = close ? (size_t)(close - (src + i + 2)) : 0;
			if (close && inner < sizeof(text)) {
				memcpy(text, src + i + 2, inner);
				text[inner] = '\0';

				const char *key = text;
				bool as_js = strncmp(text, MARKER_JS, strlen(MARKER_JS)) == 0;
				if (as_js) {
					key += strlen(MARKER_JS);
				}

				if (as_js) {
					escape_js(tr(key), escaped, sizeof(escaped));
				} else {
					escape_html(tr(key), escaped, sizeof(escaped));
				}
				replacement = escaped;
				consumed = inner + 4; // the braces at both ends
				translated++;
			}
		} else if (src[i] == '#' && i + ACCENT_TOKEN_LEN <= len) {
			if (memcmp(src + i, ACCENT_TOKEN, ACCENT_TOKEN_LEN) == 0) {
				replacement = accent_now;
				consumed = ACCENT_TOKEN_LEN;
			} else if (memcmp(src + i, ACCENT_PRESSED_TOKEN, ACCENT_TOKEN_LEN) == 0) {
				replacement = accent_down;
				consumed = ACCENT_TOKEN_LEN;
			}
		}

		if (!replacement) {
			i++;
			continue;
		}

		size_t before = i - run;
		if (before) {
			ok = fwrite(src + run, 1, before, f) == before;
		}
		if (ok) {
			size_t n = strlen(replacement);
			ok = fwrite(replacement, 1, n, f) == n;
		}
		i += consumed;
		run = i;
	}

	if (ok && run < len) {
		size_t rest = len - run;
		ok = fwrite(src + run, 1, rest, f) == rest;
	}

	if (fclose(f) != 0) {
		ok = false;
	}

	printf("wifitransfer: %s <- our page (%zu bytes, accent #%06x, %d testi in %s): %s\n", page, len,
		   (unsigned)accent_rgb, translated, lang_current(), ok ? "ok" : "FAILED");
	return ok;
}

// thttpd serves nothing that "others" may not read, root or not: a directory
// without o+r and o+x, the document root above all, answers 403 "resolves to
// a file which is not world-readable", and a CGI without o+x is not run.
//
// The tree is only as open as it arrived. The copy under /usr/data is made
// once, by `cp -a`, and keeps the modes of the shipped tree at that moment --
// which a firmware image packed from a checkout made under a strict umask
// carries as 700 and 600 -- for as long as the copy exists. So the modes are
// put right here, every start, rather than trusted: read for everyone, and
// execute for everyone wherever the owner has it. Nothing is taken away.
// Symbolic links are left alone; they have no mode of their own.
#define WEB_TREE_DEPTH 4

static int open_up_tree(const char *path, int depth) {
	struct stat st;
	if (lstat(path, &st) != 0 || S_ISLNK(st.st_mode)) {
		return 0;
	}
	int fixed = 0;
	mode_t want = st.st_mode | S_IRUSR | S_IRGRP | S_IROTH;
	if (S_ISDIR(st.st_mode) || (st.st_mode & S_IXUSR)) {
		want |= S_IXUSR | S_IXGRP | S_IXOTH;
	}
	if ((want & 07777) != (st.st_mode & 07777)) {
		if (chmod(path, want & 07777) == 0) {
			fixed++;
		} else if (errno != EROFS) { // the shipped tree, served as it is
			printf("wifitransfer: cannot open up %s: %s\n", path, strerror(errno));
		}
	}
	if (!S_ISDIR(st.st_mode) || depth >= WEB_TREE_DEPTH) {
		return fixed;
	}
	DIR *dir = opendir(path);
	if (!dir) {
		return fixed;
	}
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
			continue;
		}
		char child[PATH_MAX];
		if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >= (int)sizeof(child)) {
			continue;
		}
		fixed += open_up_tree(child, depth + 1);
	}
	closedir(dir);
	return fixed;
}

static void open_up_web_root(const char *web_root) {
	int fixed = open_up_tree(web_root, 0);
	if (fixed > 0) {
		printf("wifitransfer: %d entr%s under %s made readable for the server\n", fixed, fixed == 1 ? "y" : "ies",
			   web_root);
	}
}

static const char *prepare_web_root(void) {
	if (!exists(WEB_DIR)) {
		mkdir(WEB_PARENT, 0755);

		char *const argv[] = {(char *)"cp", (char *)"-a", (char *)WEB_SRC, (char *)WEB_PARENT "/", NULL};
		int rc = run("/bin/cp", argv);
		if (rc != 0) {
			// Serve the shipped tree instead. This player's page still goes in
			// if that partition happens to be mounted writable; if it does not,
			// what gets served is HiBy's own page, in Chinese, and everything
			// still works. That is the one case where dropping web/index.html
			// into /usr/share/web by hand is worth doing.
			printf("wifitransfer: could not copy the web tree (cp -> %d); serving %s\n", rc, WEB_SRC);
			install_page(WEB_SRC);
			link_fonts(WEB_SRC);
			link_self_as_cgi(WEB_SRC);
			open_up_web_root(WEB_SRC);
			return WEB_SRC;
		}
	}

	install_page(WEB_DIR);
	link_fonts(WEB_DIR);
	link_self_as_cgi(WEB_DIR);
	open_up_web_root(WEB_DIR);
	return WEB_DIR;
}

// Everything /etc/thttpd.conf says, plus a cgipat that also matches the six
// extension-less programs the page actually asks for. Without that last line
// every button on the page answers 403.
//
// One of these per port, because thttpd binds one port per process.
static bool write_config(const char *web_root, int port, const char *conf_path, const char *pidfile) {
	FILE *f = fopen(conf_path, "w");
	if (!f) {
		printf("wifitransfer: cannot write %s: %s\n", conf_path, strerror(errno));
		return false;
	}

	fprintf(f, "dir=%s\n", web_root);
	fprintf(f, "user=root\n");
	fprintf(f, "port=%d\n", port);
	fprintf(f, "pidfile=%s\n", pidfile);
	fprintf(f, "logfile=/dev/null\n");
	fprintf(f, "cgipat=**.cgi|list|upload|download|create|move|delete|sonix-list\n");

	// Nothing served here may be cached without asking first.
	//
	// The page is written out fresh on every start -- a different language, a
	// different accent, a different build of the player all change it -- and it
	// is always at the same URL, so a browser that decides on its own that its
	// copy is still good keeps running last week's page. The <meta> tag in the
	// document is not enough: it is advice, and it arrives only once the
	// document has been fetched. This is a header, and it makes the browser
	// ask, which with a file whose timestamp changes every start means it gets
	// the new one.
	fprintf(f, "max_age=0\n");

	// And the line without which `sonix-list` answers 403 every single time.
	//
	// thttpd resolves symlinks in the requested path itself, and refuses
	// anything that ends up outside the document root -- "The requested URL
	// resolves to a file outside the permitted web server directory tree". The
	// listing program IS such a symlink: it points at the player binary, which
	// lives nowhere near /usr/data/share/web. The check normally defaults to
	// on, because the stock config leaves `nosymlink` commented out.
	//
	// Nothing is given away by switching it off here that the server does not
	// already give away: it runs as root, without chroot, without a password,
	// and its own CGIs hand request paths to system(). It also only exists
	// while the transfer page is open.
	fprintf(f, "nosymlinkcheck\n");

	fclose(f);
	return true;
}

// thttpd forks into the background and the parent exits 0, so the exit status
// only says it got that far.
static bool start_thttpd(const char *web_root, int port, const char *conf_path, const char *pidfile) {
	if (!write_config(web_root, port, conf_path, pidfile)) {
		return false;
	}

	char *const argv[] = {(char *)"thttpd", (char *)"-C", (char *)conf_path, NULL};
	int rc = run(THTTPD_BIN, argv);

	printf("wifitransfer: thttpd on port %d (root %s) -> %d\n", port, web_root, rc);
	return rc == 0;
}

// ---------------------------------------------------------------------------
// up and down
// ---------------------------------------------------------------------------

static void stop_server(void) {
	mdns_stop();

	// The firmware's own teardown, which also reaps the CGI programs a browser
	// may have left running. It kills thttpd by name, so both instances go.
	// Doing it by hand otherwise.
	if (exists(CGIC_DISABLE)) {
		char *const argv[] = {(char *)"cgic_disable", NULL};
		run(CGIC_DISABLE, argv);
	} else {
		static const char *const names[] = {"thttpd",		"udp_server", "list.cgi", "upload.cgi",
											"download.cgi", "create.cgi", "move.cgi",  "delete.cgi"};
		for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			char *const argv[] = {(char *)"killall", (char *)names[i], NULL};
			run("/usr/bin/killall", argv);
		}
	}

	// After the programs that were reading through it, not before.
	unbind_card();
}

static bool start_server(void) {
	bind_card();

	if (process_running("thttpd")) {
		return true; // already up; nothing to do
	}

	const char *web_root = prepare_web_root();

	// The beacon first, the way cgic_enable orders them. It daemonises itself,
	// so this returns straight away. Not fatal if it is missing: it is only
	// how HiBy's app finds the player, and a typed address does not need it.
	if (exists(UDP_SERVER_BIN)) {
		char port[16];
		snprintf(port, sizeof(port), "%d", WT_PORT);
		char *const argv[] = {(char *)"udp_server", (char *)"-p", port, NULL};
		run(UDP_SERVER_BIN, argv);
	}

	// Two instances, and the reason is the name.
	//
	// A .local name resolves to an address, and nothing more: mDNS has no way to
	// say "and the port is 4399". So for "http://sonix-transfer.local" to work
	// with nothing typed after it, something has to be on port 80.
	//
	// 4399 stays as well, because that is the port HiBy's own phone app expects
	// after finding the udp_server beacon, and there is no reason to break it.
	start_thttpd(web_root, WT_PORT_NAMED, CONF_NAMED, "/var/run/thttpd-80.pid");
	start_thttpd(web_root, WT_PORT, CONF_PATH, "/var/run/thttpd.pid");

	for (int i = 0; i < 20 && !process_running("thttpd"); i++) {
		usleep(100 * 1000);
	}

	bool up = process_running("thttpd");
	printf("wifitransfer: thttpd %s\n", up ? "up" : "NOT RUNNING");

	// And the name itself. Failing to publish it is not failing to serve: the
	// page's address still works, which is why the player's screen shows both.
	if (up) {
		mdns_start(WT_MDNS_NAME, WT_PORT_NAMED);
	}

	return up;
}

static void apply(bool on) {
	pthread_mutex_lock(&lock);
	if (on) {
		start_server();
	} else {
		stop_server();
	}
	pthread_mutex_unlock(&lock);
}

static void *apply_thread(void *arg) {
	apply(arg != NULL);
	return NULL;
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

bool wifitransfer_available(void) { return exists(THTTPD_BIN) && (exists(WEB_DIR) || exists(WEB_SRC)); }

// Bumped by every change of intent, so the cached answer below is thrown away
// rather than outliving the toggle by up to a second.
static uint32_t running_cache_generation;

void wifitransfer_set_enabled(bool on) {
	g_enabled = on;
	running_cache_generation++;

	if (!wifitransfer_available()) {
		return;
	}

	pthread_t thread;
	if (pthread_create(&thread, NULL, apply_thread, on ? (void *)1 : NULL) != 0) {
		apply(on); // no thread to be had: do it here rather than not at all
		return;
	}
	pthread_detach(thread);
}

bool wifitransfer_get_enabled(void) { return g_enabled; }

// Whether the server is up.
//
// Cached for a second, because the answer costs a walk of /proc -- an opendir
// and one open+read per process -- and the transfer page asks twice a second
// for as long as it is open. On a single core with an upload running, that is
// work taken from the upload to answer a question whose answer changes about
// once a session.
#define WT_RUNNING_CACHE_MS 1000

bool wifitransfer_running(void) {
	static bool cached;
	static uint32_t cached_at;
	static bool have;
	static uint32_t cached_generation;

	// Switched off, there is no server to find: the power tick asks on every
	// pass (idle_links_tick), and each answer was a walk of /proc.
	if (!g_enabled) {
		return false;
	}

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint32_t now = (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);

	if (have && cached_generation == running_cache_generation && (uint32_t)(now - cached_at) < WT_RUNNING_CACHE_MS) {
		return cached;
	}
	cached_generation = running_cache_generation;
	cached = process_running("thttpd");
	cached_at = now;
	have = true;
	return cached;
}

void wifitransfer_url(char *out, int size) {
	if (!out || size <= 0) {
		return;
	}
	out[0] = '\0';

	if (!mdns_running()) {
		return;
	}

	char host[80];
	mdns_hostname(host, sizeof(host));
	if (!host[0]) {
		return;
	}

	// No port: that is the whole point of the name, and why there is a thttpd
	// on 80 for it to land on.
	snprintf(out, (size_t)size, "http://%s", host);
}

void wifitransfer_address(char *out, int size) {
	if (!out || size <= 0) {
		return;
	}
	out[0] = '\0';

	// The kernel's answer rather than the supplicant's, for the same reason the
	// page's own link test uses it: a status poll that lands mid-scan comes back
	// with no address while the server is happily serving on one.
	char ip[INET_ADDRSTRLEN];
	if (!wifi_interface_address(ip, sizeof(ip))) {
		return;
	}

	// Also without a port, for the same reason.
	snprintf(out, (size_t)size, "http://%s", ip);
}

void wifitransfer_watchdog(void) {
	if (!g_enabled || !wifitransfer_available() || process_running("thttpd")) {
		return;
	}

	printf("wifitransfer: the server went away; starting it again\n");
	wifitransfer_set_enabled(true);
}
