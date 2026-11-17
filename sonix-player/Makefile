# Makefile for sonix_player

# Target executables
HOST_BIN = sonix_player_host
TARGET_BIN = sonix_player
# Built from launcher/, see "sonix_launch" below.
LAUNCH_BIN = sonix_launch

# The unstripped copy of the target binary, kept beside the stripped one.
#
# It is what a backtrace, addr2line or gdb need: the stripped binary that goes
# on the device carries no symbol table and no DWARF, so an address out of a
# crash log means nothing without this file. The two are the same build, so an
# address from one resolves in the other.
TARGET_DEBUG_BIN = sonix_player_debug

LVGL_DIR = $(CURDIR)/lvgl
ROCKBOX_TOOLCHAIN_DIR = $(CURDIR)/rockbox-toolchain

CC = gcc
CXX = g++
CROSS_CC ?= $(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-gcc
CROSS_CXX ?= $(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-g++

# The interface uses the per-flag object API (lv_obj_set_hidden() and the
# rest) and lv_text_ap_strlen(), neither of which exists before 9.6. It also
# includes some of LVGL's private headers (lv_timer_private.h,
# lv_event_private.h, lv_roller_private.h and others) from src/.
LVGL_VER = 9.6.0

# Self-bootstrap: clone LVGL if it doesn't exist yet before evaluating variables
ifeq ($(wildcard $(LVGL_DIR)),)
$(info Cloning LVGL v$(LVGL_VER)...)
$(shell git clone --depth 1 -b v$(LVGL_VER) https://github.com/lvgl/lvgl.git)
endif

# Read out of the headers, so a checkout with no git history still answers.
# All three places are tried: the macros sit in lvgl.h up to 9.1, in
# lv_version.h from 9.2 and in include/lvgl/lv_version.h from 9.6.
LVGL_VER_FOUND := $(shell sed -n 's/^\#define LVGL_VERSION_\(MAJOR\|MINOR\|PATCH\) *\([0-9]*\).*/\2/p' \
					$(LVGL_DIR)/include/lvgl/lv_version.h $(LVGL_DIR)/lv_version.h $(LVGL_DIR)/lvgl.h \
					2>/dev/null | head -3 | paste -sd. -)
ifneq ($(LVGL_VER_FOUND),)
ifneq ($(LVGL_VER_FOUND),$(LVGL_VER))
$(warning lvgl/ holds $(LVGL_VER_FOUND) and this player needs $(LVGL_VER).)
$(warning Remove the lvgl directory and run make again to fetch the right one.)
$(error wrong LVGL version)
endif
endif

# Compile flags.
#
#   -DLV_CONF_INCLUDE_SIMPLE=1  lets lv_conf.h be included as "lv_conf.h".
#   -MMD -MP                    write a .d per object, pulled back in at the
#                               bottom of this file so a header edit rebuilds.
#   -D_FILE_OFFSET_BITS=64      required on the 32-bit target: without it
#                               readdir()/stat() fail with EOVERFLOW on an inode
#                               or offset wider than 32 bits, which an exFAT
#                               card produces routinely.
CFLAGS = -O3 -g -Wall -MMD -MP -D_FILE_OFFSET_BITS=64 -I. -I$(LVGL_DIR) -DLV_CONF_INCLUDE_SIMPLE=1 -DGB_CORE=1
# On the host the audio libraries are the system ones (libopusfile-dev,
# libwavpack-dev); on the target they are the ones this Makefile builds below.
# opusfile.h includes <opus_multistream.h> unprefixed, so the opus/ directory
# has to be among the -I in both cases.
HOST_CFLAGS = $(CFLAGS) -DHOST_BUILD=1 $(shell sdl2-config --cflags) $(shell pkg-config --cflags freetype2) \
			  $(shell pkg-config --cflags opusfile wavpack)
# -include glibc_compat.h must enter EVERY translation unit of the target
# build: it carries the .symver lines that keep five maths functions on the
# names the device's glibc 2.22 knows, and a .symver applies only to its own
# unit. Target only.
#
# -march=mips32r2: the toolchain is configured without --with-arch, and for the
# o32 ABI GCC then defaults to MIPS I. The X1600's core runs MIPS32r2 (the
# device's kernel modules and bluealsa are built for it), which gives mul, madd,
# conditional moves and no load delay slots.
#
# -mno-madd4: no madd.s/msub.s. Every float multiply and add stays a separate,
# separately rounded instruction, as in a MIPS I build.
#
# -mplt: the player is an executable at a fixed address, so its code need not
# be position-independent. Without this GCC compiles it as if it were a shared
# library: every function that calls another sets up $gp, every call loads its
# target from the GOT and jumps through a register, $gp is reloaded after each
# call, and every global is two loads. With it a call is a plain jal and a
# global is lui+lw; calls into the shared libraries go through a PLT the linker
# builds, and their data (stdout, stderr) is copied into the executable. The
# arithmetic is untouched: only how code and data are reached. Needs a dynamic
# linker with MIPS PLT support, which glibc has had since 2.9 (the device has
# 2.22). Ignored for anything built with -fPIC.
#
# -ffunction-sections -fdata-sections: every function and every object in a
# section of its own, so that --gc-sections (TARGET_LDFLAGS) can leave out the
# ones nothing calls -- the LVGL widgets the interface never creates, the parts
# of SQLite it never asks for. The player locks its code into RAM at startup
# (lock_code_pages in main.c), so code that is never run is memory that is
# never given back. What stays is compiled exactly as before.
TARGET_CFLAGS = $(CFLAGS) -march=mips32r2 -mno-madd4 -mplt -ffunction-sections -fdata-sections \
				-I$(LVGL_DIR)/src -I$(FREETYPE_PREFIX)/include/freetype2 \
				-I$(AUDIO_PREFIX)/include -I$(AUDIO_PREFIX)/include/opus \
				-include src/system/core/glibc_compat.h \
				-fno-builtin-printf
#
# -fno-builtin-printf: gcc otherwise turns printf("text\n") and printf("%s\n",
# s) into puts(), and glibc 2.22's puts() writes through libc's own stdout. This
# executable is not position-independent and has a copy of the stdout variable
# (R_MIPS_COPY), the copy logging.c points at its stamping stream -- so those
# lines missed the stamp and the queue, and sat unflushed in the raw
# descriptor's buffer. The simulator links as a PIE against a newer glibc and
# never showed it.

# How hard each target object is optimised, on top of TARGET_CFLAGS (the last
# -O on the command line is the one that counts).
#
# -O3 inlines and unrolls, and the code grows with it: worth it where the
# player spends its time with music playing or a game running, and nowhere
# else. Locked into RAM as all the code is (see above), the rest at -O2 is
# about half a megabyte given back, and on a core with a small instruction
# cache, smaller code is seldom slower code.
#
# The hot paths: the audio chain and its effects, the decoders, the Game Boy
# core, and LVGL's software renderer and colour mixing. On the Blaze also main.c, which shrinks every
# frame 2/3 onto the panel (blit_scaled_2_3: measured on the device, it is the
# biggest single cost of the interface thread), and the cover decoding and
# resampling (system/image, gui/nowplaying/cover.c).
#
# The cold paths go the other way, to -Os: code that runs when a page is
# built, a list is filled or a catalogue answers, and never per frame or per
# audio block -- the library and streaming pages, the settings pages, the
# library database, tag reading, the podcast, Qobuz and Tidal clients, the
# e-book reader, Last.fm. Its speed is lost in the card or the network it
# waits on, but every byte of it is pinned in RAM all the same. SQLite counts
# most: of the 368 kB of .text that -Os takes off the whole player, 180 kB is
# SQLite alone (whose own documentation builds it -Os for small devices), and
# its queries here read a few hundred rows off an SD card. This list takes
# 245 kB off the pinned code (etext, see lock_code_pages). Left at -O2 on
# purpose, although they shrink too: LVGL's core (style lookup and layout run
# every frame, for a few kB), the radio stream and the network code under it,
# AirPlay and DLNA (they carry audio), Bluetooth, the player page and the
# shell.
#
# TARGET_OPT is per object: the default level everywhere, the hot one on the
# objects matching TARGET_HOT_PATTERNS, the cold one on TARGET_COLD_PATTERNS.
# The two lists must not overlap.
TARGET_DEFAULT_OPT = -O2
TARGET_HOT_OPT = -O3
TARGET_COLD_OPT = -Os
TARGET_HOT_PATTERNS = build_target/system/audio/% build_target/system/decode/% build_target/gb/% \
					  build_target/lvgl/src/draw/sw/% build_target/lvgl/src/misc/lv_color% \
					  build_target/main.o build_target/system/image/% build_target/gui/nowplaying/cover.o
TARGET_COLD_PATTERNS = build_target/system/db/% build_target/system/library/% build_target/gui/library/% \
					   build_target/gui/streaming/% build_target/gui/settings/% build_target/gui/ebook/% \
					   build_target/system/ebook/% build_target/system/lastfm/% \
					   build_target/system/streaming/podcast% build_target/system/streaming/qobuz% \
					   build_target/system/streaming/tidal% build_target/system/streaming/opml.o \
					   build_target/system/streaming/onair.o
TARGET_OPT = $(TARGET_DEFAULT_OPT)
$(TARGET_HOT_PATTERNS): TARGET_OPT = $(TARGET_HOT_OPT)
$(TARGET_COLD_PATTERNS): TARGET_OPT = $(TARGET_COLD_OPT)

# The C++ in this project is the Game Boy core (src/gb, Gearboy) plus the facade
# that makes it callable from the player, which is C.
#
#   -DGEARBOY_DISABLE_DISASSEMBLER  drops the disassembler and the breakpoints:
#       they are for Gearboy's own debugger, which is not here.
#   -fno-exceptions -fno-rtti       the core uses neither, and without them a
#       few tables and some dead code go away.
#   The -I are needed because Gearboy's sources include each other by short name
#       ("Video.h"), as if they were in their own directory -- which is how they
#       sit upstream, and they are not touched so that the next update does not
#       have to be merged by hand.
#   -Wno-attributes                 Gearboy declares InvalidOPCode() noinline
#       first and inline afterwards. Upstream code, left alone on purpose (see
#       src/gb/README.md).
GB_CFLAGS = -DGEARBOY_DISABLE_DISASSEMBLER -fno-exceptions -fno-rtti -Wno-attributes \
			-Isrc/gb/core -Isrc/gb/miniz
HOST_CXXFLAGS = $(HOST_CFLAGS) $(GB_CFLAGS)
TARGET_CXXFLAGS = $(TARGET_CFLAGS) $(GB_CFLAGS)

# Link flags
#
# -static-libstdc++ -static-libgcc are not optional on the target: the device
# has libstdc++.so.6.0.21, that is GLIBCXX_3.4.21 (the GCC 5 ABI), and the
# toolchain is GCC 9, which emits references up to 3.4.26. Linked dynamically
# the binary does not start at all.
#
# Target only. On the host they ask for a static libstdc++ that most
# distributions do not ship, and the link fails with "cannot find -lstdc++".
GB_LDFLAGS = -static-libstdc++ -static-libgcc
# Every interface timer runs through src/gui/shell/timerprof.c, and in a
# profiled run every event callback through eventprof.c, so a slow-pass report
# (frameprof.c) can name the callback the time went to.
TIMER_WRAP = -Wl,--wrap=lv_timer_create -Wl,--wrap=lv_timer_set_cb -Wl,--wrap=lv_obj_add_event_cb \
			 -Wl,--wrap=lv_obj_remove_event_cb
# Three LVGL functions with a shortcut in front of them, for the cost of
# building pages: src/gui/shell/lvglfast.c says which and why.
LVGL_FAST_WRAP = -Wl,--wrap=lv_display_send_event -Wl,--wrap=lv_obj_get_display \
				 -Wl,--wrap=lv_text_ap_strlen -Wl,--wrap=lv_text_ap_proc
HOST_LDFLAGS = $(TIMER_WRAP) $(LVGL_FAST_WRAP) $(shell sdl2-config --libs) -lpthread -lm -ldl -lasound $(shell pkg-config --libs freetype2) \
			   $(shell pkg-config --libs opusfile wavpack)
#
# --gc-sections: the other half of -ffunction-sections (see TARGET_CFLAGS).
TARGET_LDFLAGS = $(TIMER_WRAP) $(LVGL_FAST_WRAP) $(FREETYPE_PREFIX)/lib/libfreetype.a $(AUDIO_LIBS) -lpthread -lm -ldl -lasound \
				 $(GB_LDFLAGS) -Wl,--gc-sections

# FreeType for the target, built static: the rootfs has no libfreetype.so. The
# host uses the system one. The device's fonts are plain OTF/TTF, so zlib,
# bzip2, png, harfbuzz and brotli are all off.
FREETYPE_VER = 2.13.3
FREETYPE_TARBALL = freetype-$(FREETYPE_VER).tar.xz
FREETYPE_URL = https://download.savannah.gnu.org/releases/freetype/$(FREETYPE_TARBALL)
FREETYPE_URL_MIRROR = https://downloads.sourceforge.net/project/freetype/freetype2/$(FREETYPE_VER)/$(FREETYPE_TARBALL)
FREETYPE_SRC = $(CURDIR)/freetype-$(FREETYPE_VER)
FREETYPE_PREFIX = $(CURDIR)/freetype-target

# The audio libraries carried inside the binary, static like FreeType. The
# firmware's /usr/lib holds a libopus.so, but it is the raw codec with no Ogg
# demuxer, so linking against it is not an option.
#
#   libogg + libopus + opusfile -> .opus (src/system/decode/opusdec.c)
#   libwavpack                  -> .wv   (src/system/decode/wavpackdec.c)
#
# The configure options below were settled against these versions.
# ALAC is not here: its decoder is a single file under src/system/decode/alac/.
AUDIO_PREFIX = $(CURDIR)/audio-target

OGG_VER = 1.3.5
OGG_TARBALL = libogg-$(OGG_VER).tar.gz
OGG_URL = https://downloads.xiph.org/releases/ogg/$(OGG_TARBALL)
OGG_URL_MIRROR = https://ftp.osuosl.org/pub/xiph/releases/ogg/$(OGG_TARBALL)
OGG_SRC = $(CURDIR)/libogg-$(OGG_VER)
OGG_LIB = $(AUDIO_PREFIX)/lib/libogg.a

OPUS_VER = 1.4
OPUS_TARBALL = opus-$(OPUS_VER).tar.gz
OPUS_URL = https://downloads.xiph.org/releases/opus/$(OPUS_TARBALL)
OPUS_URL_MIRROR = https://ftp.osuosl.org/pub/xiph/releases/opus/$(OPUS_TARBALL)
OPUS_SRC = $(CURDIR)/opus-$(OPUS_VER)
OPUS_LIB = $(AUDIO_PREFIX)/lib/libopus.a

OPUSFILE_VER = 0.12
OPUSFILE_TARBALL = opusfile-$(OPUSFILE_VER).tar.gz
OPUSFILE_URL = https://downloads.xiph.org/releases/opus/$(OPUSFILE_TARBALL)
OPUSFILE_URL_MIRROR = https://ftp.osuosl.org/pub/xiph/releases/opus/$(OPUSFILE_TARBALL)
OPUSFILE_SRC = $(CURDIR)/opusfile-$(OPUSFILE_VER)
OPUSFILE_LIB = $(AUDIO_PREFIX)/lib/libopusfile.a

WAVPACK_VER = 5.6.0
WAVPACK_TARBALL = wavpack-$(WAVPACK_VER).tar.bz2
WAVPACK_URL = https://www.wavpack.com/$(WAVPACK_TARBALL)
WAVPACK_URL_MIRROR = https://github.com/dbry/WavPack/releases/download/$(WAVPACK_VER)/$(WAVPACK_TARBALL)
WAVPACK_SRC = $(CURDIR)/wavpack-$(WAVPACK_VER)
WAVPACK_LIB = $(AUDIO_PREFIX)/lib/libwavpack.a

# The order matters: with static libraries the linker resolves left to right,
# and opusfile calls opus which calls ogg.
AUDIO_LIBS = $(OPUSFILE_LIB) $(OPUS_LIB) $(OGG_LIB) $(WAVPACK_LIB)


# Source files. src/gb holds a .c as well (the miniz shell) alongside the C++
# core, so both searches reach into it.
APP_SRCS := $(shell find src -name '*.c')
LVGL_SRCS = $(shell find $(LVGL_DIR)/src -type f -name '*.c')

# The C++ here is the Game Boy core under src/gb/core plus its facade. Separate
# list because it takes different flags, and the link goes through the C++
# compiler so that libstdc++ comes along.
APP_CXX_SRCS := $(shell find src/gb -name '*.cpp')
HOST_LD = $(CXX)
TARGET_LD = $(CROSS_CXX)

# Object files
HOST_OBJS = $(APP_SRCS:src/%.c=build_host/%.o) $(APP_CXX_SRCS:src/%.cpp=build_host/%.opp) \
			$(LVGL_SRCS:$(LVGL_DIR)/%.c=build_host/lvgl/%.o)
TARGET_OBJS = $(APP_SRCS:src/%.c=build_target/%.o) $(APP_CXX_SRCS:src/%.cpp=build_target/%.opp) \
			  $(LVGL_SRCS:$(LVGL_DIR)/%.c=build_target/lvgl/%.o)

# PANEL_WIDTH=W builds for a panel W px wide: the icons are drawn at the size
# ui_px() gives (tools/svg_to_lvgl.py --width, needs cairosvg; ICON_PYTHON picks
# the interpreter) in place of the checked-in icons.c, which is drawn for 480.
# The rest of the interface scales at run time and needs nothing here.
# This fork ships the V3 Blaze only, whose panel is 320 wide; the simulator stays
# at 480 so its pages can be compared with the checked-in icons.
ifneq ($(filter target,$(MAKECMDGOALS)),)
PANEL_WIDTH ?= 320
endif
PANEL_WIDTH ?= 480
ICON_PYTHON ?= python3
ifneq ($(PANEL_WIDTH),480)
ICONS_GEN = build_gen/icons-$(PANEL_WIDTH)/icons.c
APP_SRCS := $(filter-out src/gui/shell/icons.c,$(APP_SRCS))
HOST_OBJS += build_host/icons-$(PANEL_WIDTH).o
TARGET_OBJS += build_target/icons-$(PANEL_WIDTH).o

$(ICONS_GEN): tools/svg_to_lvgl.py $(wildcard assets/icons/*)
	@mkdir -p $(dir $@)
	$(ICON_PYTHON) tools/svg_to_lvgl.py --width $(PANEL_WIDTH) --out $(dir $@)

build_host/icons-$(PANEL_WIDTH).o: $(ICONS_GEN)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

build_target/icons-$(PANEL_WIDTH).o: $(ICONS_GEN)
	$(CROSS_CC) $(TARGET_CFLAGS) -c $< -o $@
endif

# Rewritten only when PANEL_WIDTH changes, so that switching back to a width
# whose objects are older than the binary still relinks it.
PANEL_WIDTH_STAMP = build_gen/panel_width
$(PANEL_WIDTH_STAMP): FORCE
	@mkdir -p $(dir $@)
	@echo $(PANEL_WIDTH) > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

.PHONY: all host target clean compile_commands.json

all: host compile_commands.json

host: $(HOST_BIN) compile_commands.json

$(HOST_BIN): $(HOST_OBJS) $(PANEL_WIDTH_STAMP)
	$(HOST_LD) -o $@ $(HOST_OBJS) $(HOST_LDFLAGS)
	@echo "Host build complete: Run './$(HOST_BIN)' to start the simulator."

# The key that opens streaming-keys.bin (src/system/streaming/streamkeys.h),
# kept outside git and compiled into streamkeys.c. The header is rewritten only
# when the key changes. Without the key file the build still works and the
# player reads only a plain streaming-keys.ini.
STREAMKEYS_KEY ?= streaming-keys.key
STREAMKEYS_KEY_H = build_gen/streamkeys_key.h
$(STREAMKEYS_KEY_H): FORCE
	@mkdir -p $(dir $@)
	@{ echo '// Generated by the Makefile from $(STREAMKEYS_KEY). Not in git.'; \
	  if [ -f "$(STREAMKEYS_KEY)" ]; then \
		hex=$$(tr -dc '0-9a-fA-F' < "$(STREAMKEYS_KEY)"); \
		[ $${#hex} -eq 64 ] || { echo "$(STREAMKEYS_KEY) is not 32 bytes of hex" >&2; exit 1; }; \
		echo '#define STREAMKEYS_KEY_LEN 32'; \
		echo "static const unsigned char streamkeys_key[STREAMKEYS_KEY_LEN] = {$$(echo $$hex | sed 's/../0x&,/g')};"; \
	  else \
		echo '#define STREAMKEYS_KEY_LEN 0'; \
	  fi; } > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; \
		if [ -f "$(STREAMKEYS_KEY)" ]; then echo "streamkeys: key from $(STREAMKEYS_KEY)"; \
		else echo "streamkeys: no $(STREAMKEYS_KEY), this build reads only a plain streaming-keys.ini"; fi; fi
build_host/system/streaming/streamkeys.o build_target/system/streaming/streamkeys.o: $(STREAMKEYS_KEY_H)

build_host/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

build_host/%.opp: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(HOST_CXXFLAGS) -c $< -o $@

build_host/lvgl/%.o: $(LVGL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

target: $(TARGET_BIN) $(LAUNCH_BIN) compile_commands.json

$(TARGET_BIN): $(TARGET_OBJS) $(PANEL_WIDTH_STAMP)
	$(TARGET_LD) -o $@ $(TARGET_OBJS) $(TARGET_LDFLAGS)
	@$(MAKE) --no-print-directory check-abi
	@$(MAKE) --no-print-directory strip-target
	@echo "Target build complete: File ready at '$(TARGET_BIN)'"

# ---------------------------------------------------------------------------
# sonix_launch
#
# The process sonix_player.sh hands itself over to: it starts the player and
# reboots the device when the player exits (launcher/sonix_launch.c). No libc
# and no loader, so it is built on its own: static, freestanding, non-PIC, and
# with the loop idioms GCC would turn into memset or strlen calls left as loops.
# The packer takes it from next to itself, like sonix_player.
# ---------------------------------------------------------------------------

LAUNCH_CFLAGS = -Os -Wall -Wextra -march=mips32r2 -mno-abicalls -fno-pic -G0 -static -nostdlib \
				-ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-stack-protector \
				-fno-asynchronous-unwind-tables -Wl,-e,__start

$(LAUNCH_BIN): launcher/sonix_launch.c $(CROSS_CC)
	$(CROSS_CC) $(LAUNCH_CFLAGS) -o $@ $<
	@if [ -x "$(CROSS_STRIP)" ]; then $(CROSS_STRIP) $@; fi
	@echo "sonix_launch: $$(wc -c < $@) bytes"

# ---------------------------------------------------------------------------
# Splitting the link output in two
#
# Runs after check-abi, which deletes both files when it fails: there is then
# nothing to copy, and no debug binary left claiming to match a build that was
# refused.
#
# The cross strip and not the host one. /usr/bin/strip is built for x86-64 and
# answers "file format not recognized" on a MIPS binary -- and it says so on
# stderr while exiting non-zero, which would take the whole build down.
# ---------------------------------------------------------------------------

CROSS_STRIP ?= $(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-strip

.PHONY: strip-target
strip-target:
	@if [ ! -f "$(TARGET_BIN)" ]; then \
		exit 0; \
	fi; \
	if [ ! -x "$(CROSS_STRIP)" ]; then \
		echo "strip-target: $(CROSS_STRIP) is not there, '$(TARGET_BIN)' left unstripped"; \
		exit 0; \
	fi; \
	cp -f $(TARGET_BIN) $(TARGET_DEBUG_BIN); \
	$(CROSS_STRIP) $(TARGET_BIN); \
	echo "strip-target: symbols in '$(TARGET_DEBUG_BIN)' ($$(du -h $(TARGET_DEBUG_BIN) | cut -f1)), stripped '$(TARGET_BIN)' ($$(du -h $(TARGET_BIN) | cut -f1))"

# ---------------------------------------------------------------------------
# The check the linker does not do
#
# The toolchain compiles against glibc 2.27, the device has 2.22. A binary
# asking for a newer versioned symbol links without a word and then refuses to
# start, and on this device that means a boot loop: sonix_launch runs
# `sleep 1; reboot` as soon as the player exits.
#
# glibc_compat.h holds the known functions down; this catches the next ones.
# ---------------------------------------------------------------------------

CROSS_READELF ?= $(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-readelf

# The device's glibc, as major.minor.
DEVICE_GLIBC_MAJOR = 2
DEVICE_GLIBC_MINOR = 22

.PHONY: check-abi
check-abi:
	@if [ ! -x "$(CROSS_READELF)" ]; then \
		echo "check-abi: $(CROSS_READELF) is not there, check skipped"; \
		exit 0; \
	fi; \
	bad=$$($(CROSS_READELF) --dyn-syms -W $(TARGET_BIN) 2>/dev/null \
		| awk '{ for (i = 1; i <= NF; i++) if (match($$i, /@GLIBC_[0-9.]+$$/)) { \
			ver = substr($$i, RSTART + 7); n = split(ver, v, "."); \
			if (v[1] > $(DEVICE_GLIBC_MAJOR) || (v[1] == $(DEVICE_GLIBC_MAJOR) && v[2] > $(DEVICE_GLIBC_MINOR))) print "  " $$i; \
		} }' | sort -u); \
	if [ -n "$$bad" ]; then \
		echo ""; \
		echo "*** ERROR: the binary asks for symbols the device does not have."; \
		echo "*** The device has glibc $(DEVICE_GLIBC_MAJOR).$(DEVICE_GLIBC_MINOR); these want more:"; \
		echo "$$bad"; \
		echo ""; \
		echo "*** As it stands the binary DOES NOT START, and the device boot loops."; \
		echo "*** Add a .symver in src/system/core/glibc_compat.h, which explains how."; \
		echo ""; \
		rm -f $(TARGET_BIN) $(TARGET_DEBUG_BIN); \
		exit 1; \
	fi; \
	echo "check-abi: no symbol beyond glibc $(DEVICE_GLIBC_MAJOR).$(DEVICE_GLIBC_MINOR)."

# The host self-checks in tools/*_selftest.c, built straight from the sources
# with plain gcc: no SDL, no bundled libraries, nothing from make host. Each
# stops on the first failed assert, and make with it.
TEST_DIR = build_test
IMAGE_TEST_SRCS = src/system/image/art_shrink.c src/system/image/jpeg_scaled.c src/system/image/png_scaled.c \
				  src/system/image/png_write.c src/system/image/stb_image_impl.c

.PHONY: test
test:
	@mkdir -p $(TEST_DIR)
	$(CC) -O2 -Wall -DHOST_BUILD -I. -I$(LVGL_DIR) tools/batterystats_selftest.c -lm -o $(TEST_DIR)/batterystats_selftest
	$(TEST_DIR)/batterystats_selftest
	$(CC) -O2 -Wall -I. tools/dsd_selftest.c src/system/decode/dsd.c -lpthread -lm -o $(TEST_DIR)/dsd_selftest
	$(TEST_DIR)/dsd_selftest $(TEST_DIR)
	$(CC) -O2 -Wall -I. -DBOOTLOGO_CUSTOM_FILE='"$(CURDIR)/$(TEST_DIR)/bootlogo.jpeg"' tools/bootlogo_selftest.c \
		src/system/device/bootlogo.c $(IMAGE_TEST_SRCS) -lpthread -lm -ldl -o $(TEST_DIR)/bootlogo_selftest
	$(TEST_DIR)/bootlogo_selftest $(TEST_DIR)
	$(CC) -O2 -Wall -DHOST_BUILD -I. -I$(LVGL_DIR) tools/wifi_selftest.c -lpthread -o $(TEST_DIR)/wifi_selftest
	$(TEST_DIR)/wifi_selftest $(TEST_DIR)
	$(CC) -O2 -Wall -I. tools/axpcharge_selftest.c -o $(TEST_DIR)/axpcharge_selftest
	$(TEST_DIR)/axpcharge_selftest $(TEST_DIR) > $(TEST_DIR)/axpcharge_selftest.log
	@echo "test: all self-checks passed."

# The flags every target object was compiled with, the optimisation levels
# included. Rewritten only when they differ, so a change of flags rebuilds
# everything and nothing else does.
TARGET_FLAGS_STAMP = build_target/.flags
TARGET_FLAGS_LINE = $(TARGET_CFLAGS) | $(TARGET_CXXFLAGS) | $(TARGET_DEFAULT_OPT) $(TARGET_HOT_OPT) $(TARGET_HOT_PATTERNS) | $(TARGET_COLD_OPT) $(TARGET_COLD_PATTERNS)
$(TARGET_FLAGS_STAMP): FORCE
	@mkdir -p $(dir $@)
	@echo '$(TARGET_FLAGS_LINE)' | cmp -s - $@ || \
		echo '$(TARGET_FLAGS_LINE)' > $@
FORCE:

build_target/%.o: src/%.c $(TARGET_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(TARGET_CFLAGS) $(TARGET_OPT) -c $< -o $@

build_target/%.opp: src/%.cpp $(TARGET_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CROSS_CXX) $(TARGET_CXXFLAGS) $(TARGET_OPT) -c $< -o $@

build_target/lvgl/%.o: $(LVGL_DIR)/%.c $(TARGET_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(TARGET_CFLAGS) $(TARGET_OPT) -c $< -o $@

# For the Zed/clangd LSP: autofill and hover popups.
compile_commands.json:
	@python3 generate_compile_commands.py

clean:
	rm -rf build_host build_target build_gen $(TEST_DIR) $(HOST_BIN) $(TARGET_BIN) $(TARGET_DEBUG_BIN) $(LAUNCH_BIN) compile_commands.json compile_flags.txt

# The bundled libraries and their extracted sources. The downloaded tarballs stay.
.PHONY: libs-clean
libs-clean:
	rm -rf $(FREETYPE_PREFIX) $(AUDIO_PREFIX) $(FREETYPE_SRC) $(OGG_SRC) $(OPUS_SRC) $(OPUSFILE_SRC) $(WAVPACK_SRC)

# Make sure the cross compiler is built before trying to make any of the target
# objects with it.
$(TARGET_OBJS): $(CROSS_CC) $(FREETYPE_PREFIX)/lib/libfreetype.a $(AUDIO_LIBS)
$(TARGET_BIN): $(FREETYPE_PREFIX)/lib/libfreetype.a $(AUDIO_LIBS)

$(CROSS_CC):
	@echo "Building Rockbox toolchain..."
	./rockboxdev/rockboxdev.sh

# The flags FreeType and the audio libraries are built with. -march and -mplt as
# in TARGET_CFLAGS: they are static, so they end up in the same executable. They
# are built once: `make libs-clean` removes them, with their extracted sources,
# so the next `make target` builds them again.
LIB_CFLAGS = -O2 -march=mips32r2 -mplt

# Cross-build FreeType, once. configure needs the toolchain on PATH for
# ranlib/ar to be found under their host-triplet names.
$(FREETYPE_PREFIX)/lib/libfreetype.a: $(CROSS_CC)
	@echo "Building static FreeType $(FREETYPE_VER) for the target..."
	@if [ ! -f $(FREETYPE_TARBALL) ]; then \
		wget -O $(FREETYPE_TARBALL) $(FREETYPE_URL) || \
		wget -O $(FREETYPE_TARBALL) $(FREETYPE_URL_MIRROR); \
	fi
	rm -rf $(FREETYPE_SRC) && tar xf $(FREETYPE_TARBALL)
	cd $(FREETYPE_SRC) && PATH="$(ROCKBOX_TOOLCHAIN_DIR)/bin:$$PATH" ./configure \
		--host=mipsel-rockbox-linux-gnu \
		--prefix=$(FREETYPE_PREFIX) \
		--disable-shared --enable-static \
		--with-zlib=no --with-bzip2=no --with-png=no \
		--with-harfbuzz=no --with-brotli=no \
		CC=$(CROSS_CC) CFLAGS="$(LIB_CFLAGS)" \
		AR=$(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-ar \
		RANLIB=$(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-ranlib
	PATH="$(ROCKBOX_TOOLCHAIN_DIR)/bin:$$PATH" $(MAKE) -C $(FREETYPE_SRC)
	PATH="$(ROCKBOX_TOOLCHAIN_DIR)/bin:$$PATH" $(MAKE) -C $(FREETYPE_SRC) install

# ---------------------------------------------------------------------------
# Downloaded, configured with the toolchain on PATH (configure looks for ar and
# ranlib under the triplet name) and installed into AUDIO_PREFIX. The .a is the
# make target, so each is built once.
# ---------------------------------------------------------------------------

CROSS_PATH = PATH="$(ROCKBOX_TOOLCHAIN_DIR)/bin:$$PATH"
CROSS_TOOLS = CC=$(CROSS_CC) CFLAGS="$(LIB_CFLAGS)" \
			  AR=$(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-ar \
			  RANLIB=$(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-ranlib
CROSS_COMMON = --host=mipsel-rockbox-linux-gnu --prefix=$(AUDIO_PREFIX) --disable-shared --enable-static

$(OGG_LIB): $(CROSS_CC)
	@echo "Building static libogg $(OGG_VER) for the target..."
	@if [ ! -f $(OGG_TARBALL) ]; then \
		wget -O $(OGG_TARBALL) $(OGG_URL) || wget -O $(OGG_TARBALL) $(OGG_URL_MIRROR); \
	fi
	rm -rf $(OGG_SRC) && tar xf $(OGG_TARBALL)
	cd $(OGG_SRC) && $(CROSS_PATH) ./configure $(CROSS_COMMON) $(CROSS_TOOLS)
	$(CROSS_PATH) $(MAKE) -C $(OGG_SRC)
	$(CROSS_PATH) $(MAKE) -C $(OGG_SRC) install

# --enable-fixed-point: integer decoder, bit-exact across platforms. The float
# API stays compiled in -- removing it would take --disable-float-api.
$(OPUS_LIB): $(CROSS_CC)
	@echo "Building static libopus $(OPUS_VER) for the target..."
	@if [ ! -f $(OPUS_TARBALL) ]; then \
		wget -O $(OPUS_TARBALL) $(OPUS_URL) || wget -O $(OPUS_TARBALL) $(OPUS_URL_MIRROR); \
	fi
	rm -rf $(OPUS_SRC) && tar xf $(OPUS_TARBALL)
	cd $(OPUS_SRC) && $(CROSS_PATH) ./configure $(CROSS_COMMON) \
		--enable-fixed-point --disable-doc --disable-extra-programs \
		$(CROSS_TOOLS)
	$(CROSS_PATH) $(MAKE) -C $(OPUS_SRC)
	$(CROSS_PATH) $(MAKE) -C $(OPUS_SRC) install

# DEPS_CFLAGS/DEPS_LIBS by hand: opusfile's configure finds ogg and opus with
# PKG_CHECK_MODULES, and no pkg-config here knows the cross sysroot. Passing
# them skips the search. --disable-http drops the stream support that would
# pull in OpenSSL.
$(OPUSFILE_LIB): $(CROSS_CC) $(OGG_LIB) $(OPUS_LIB)
	@echo "Building static opusfile $(OPUSFILE_VER) for the target..."
	@if [ ! -f $(OPUSFILE_TARBALL) ]; then \
		wget -O $(OPUSFILE_TARBALL) $(OPUSFILE_URL) || wget -O $(OPUSFILE_TARBALL) $(OPUSFILE_URL_MIRROR); \
	fi
	rm -rf $(OPUSFILE_SRC) && tar xf $(OPUSFILE_TARBALL)
	cd $(OPUSFILE_SRC) && $(CROSS_PATH) ./configure $(CROSS_COMMON) \
		--disable-http --disable-examples --disable-doc \
		DEPS_CFLAGS="-I$(AUDIO_PREFIX)/include -I$(AUDIO_PREFIX)/include/opus" \
		DEPS_LIBS="-L$(AUDIO_PREFIX)/lib -lopus -logg" \
		$(CROSS_TOOLS)
	$(CROSS_PATH) $(MAKE) -C $(OPUSFILE_SRC)
	$(CROSS_PATH) $(MAKE) -C $(OPUSFILE_SRC) install

# --enable-legacy: also reads the .wv written by WavPack 4.0 and earlier, which
# costs a few kilobytes. --enable-dsd keeps DSD inside WavPack, which the
# wrapper opens with OPEN_DSD_AS_PCM. --disable-asm because the assembly wavpack
# ships is for x86 and ARM.
$(WAVPACK_LIB): $(CROSS_CC)
	@echo "Building static libwavpack $(WAVPACK_VER) for the target..."
	@if [ ! -f $(WAVPACK_TARBALL) ]; then \
		wget -O $(WAVPACK_TARBALL) $(WAVPACK_URL) || wget -O $(WAVPACK_TARBALL) $(WAVPACK_URL_MIRROR); \
	fi
	rm -rf $(WAVPACK_SRC) && tar xf $(WAVPACK_TARBALL)
	cd $(WAVPACK_SRC) && $(CROSS_PATH) ./configure $(CROSS_COMMON) \
		--disable-apps --disable-man --disable-asm \
		--enable-dsd --enable-legacy --without-libiconv-prefix \
		$(CROSS_TOOLS)
	$(CROSS_PATH) $(MAKE) -C $(WAVPACK_SRC)
	$(CROSS_PATH) $(MAKE) -C $(WAVPACK_SRC) install

# Header dependencies recorded by -MMD (see CFLAGS). The dashes keep make quiet
# on the first build, when no .d files exist yet. The C++ objects end in .opp
# so the two compile rules cannot overlap, which is why both suffixes are
# substituted -- otherwise make reads an object file as a makefile.
-include $(patsubst %.opp,%.d,$(patsubst %.o,%.d,$(HOST_OBJS)))
-include $(patsubst %.opp,%.d,$(patsubst %.o,%.d,$(TARGET_OBJS)))
