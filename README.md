# Sonix Player for the TempoTec V3 Blaze

A port of [Sonix Player](https://github.com/Jepl4r/sonix-player) by
[@Jepl4r](https://github.com/Jepl4r) to the **TempoTec V3 Blaze**
(`V3_ANALOG_2025`). It replaces the stock music player and keeps TempoTec's
kernel, drivers and recovery. Everything Sonix does on the HiBy R3 Pro II and
R1 is here, laid out for the Blaze's 320x480 panel and its AK4493 DAC, plus the
fixes and speed-ups listed below.

Only for the **V3 Blaze**, not the older V3 Analog. Flashing always carries
some risk: read [Going back to stock and recovery](#going-back-to-stock-and-recovery)
first.

The upstream README, with the full build documentation and the source tree, is
in [UPSTREAM-README.md](UPSTREAM-README.md). Features are in
[FEATURES.md](FEATURES.md).

The released images carry Last.fm keys only, so Tidal, Qobuz and podcasts stay
off in them; see [Building](#building) to make an image with keys of your own.

## Install

1. Charge the player above 30 %.
2. **Keep a copy of the official firmware.** If the root of your microSD
   already has a `v3_analog_2025.upt` (the stock OTA leaves one there), rename
   it to `v3_analog_2025.upt.stock` and copy it to your computer as well.
   Remove any `update.upt`: the updater prefers it over every other name.
3. Download `v3_analog_2025.upt` from the
   [latest release](https://github.com/carmine-bin/sonix-player-tempotec-v3-blaze/releases/latest)
   and check its SHA-256 against the release notes.
4. Copy it to the **root of the microSD card**, keeping the name
   `v3_analog_2025.upt`.
5. Start the update:
   - **from the stock firmware**: Settings → Firmware update → Update via
     micro SD card;
   - **from Sonix**: Settings → System → Update firmware → From SD card.
6. Let it flash and reboot by itself.

## Updates over the internet

Once Sonix is installed, new versions arrive without visiting this page:
Settings → System → Update firmware → **Via internet** (Wi-Fi on). The player
looks at the releases of this repository, offers the newest one above the
version it runs, shows its notes, downloads `v3_analog_2025.upt` to the card,
checks its SHA-256 and installs it.

The download is written to the card root as `v3_analog_2025.upt`, so keep your
copy of the official firmware under another name (`.upt.stock`).

Versions read `<upstream version>.<Blaze revision>-blaze`: `1.2.0.1-blaze` is
upstream Sonix 1.2.0 with the first set of Blaze changes.

## What is different on the Blaze

**Hardware**

- The interface is laid out and drawn natively at 320x480 (since
  1.3.0.3), with the touch panel calibrated to match.
- AK4493SEQ audio board: three gain levels like the stock player (LDB, MDB,
  HDB), TempoTec's default DAC filter, the analogue output muted before
  power-off and reboot, the amplifier left off until the first track.
- The hardware buttons the right way round, TempoTec's boot logo, the button
  list page.

**Bluetooth**

- `main.conf` with `ControllerMode=bredr` and `FastConnectable=false`: the
  receiver range fix from the
  [V3 Blaze mod](https://github.com/carmine-bin/tempotec-v3-blaze-mod)
  (LDAC 990 kbps holds).
- Receiver: aptX HD decoded correctly (it used to be noise).
- Receiver: a paired phone comes back without pairing again. Opening the
  receiver invites every paired phone or computer, starts on the best codec
  the phone offers (LDAC first), and lets go of it when the receiver closes.
  Tapping a phone in the list links it without audio.

**Speed and battery**

- Covers: a persistent RGB565 cache, decoded off the interface thread; the old
  cover stays up until the next is ready, and follows a skip at once.
- Skips: the audio output stays open, and the next tracks' tags, favourite
  star and glyphs are ready before the press (skip-to-sound about 46 ms).
- Lists: fewer database reads while scrolling, the first rows bound first,
  the letters of the library's names warmed in the background.
- Start-up: OpenSSL and the code pinning off the path to the first frame
  (first frame 4.4 s → 3.2 s).
- Waking the screen is instant inside a minute after it went off (about
  17 ms), with no black frame.
- Hidden spinners, marquees and carets stop; with the screen off and music
  playing the interface wakes about 7 times a second instead of 44.

**Interface**

- AMOLED theme beside Dark and Light.
- Now playing: Show artist, beside Show album.
- About names the DAC as the AK4493SEQ.

The full list is the commit history of the
[`blaze-port`](https://github.com/carmine-bin/sonix-player-tempotec-v3-blaze/commits/blaze-port)
branch.

## Going back to stock and recovery

**Back to stock:** put the official firmware in the card root as
`v3_analog_2025.upt` and install it from Settings → System → Update firmware →
From SD card. The official v1.2 is on
[TempoTec's firmware page](https://www.tempotec.net/pages/firmware-download)
("TempoTec V3 Blaze Firmware V1.2", not the V3 V1.0); v1.3 comes over the air
from there.

**If the player does not boot:** with the player off, hold **Power + Previous
track** past the TempoTec logo until **updater** appears. A valid
`v3_analog_2025.upt` in the card root is flashed without asking, so check which
file is there first. The image only writes the main kernel and rootfs; the
bootloader and the recovery system are never touched.

## Bugs and feedback

Questions, bug reports and ideas go in the Telegram group:
[V3 Blaze Discussion](https://t.me/V3_Blaze_Discussion). When you report a bug,
say which version you run and what you did before it happened.

## Building

Sonix builds as described in [UPSTREAM-README.md](UPSTREAM-README.md)
(`make target` in `sonix-player/`, Rockbox MIPS toolchain). `make target` draws
the icons for the Blaze's 320 px wide panel, so it needs a Python with
`cairosvg` (`ICON_PYTHON=/path/to/python` if `python3` lacks it). The packer builds
the Blaze image from the **official v1.3 firmware** (SHA-256
`5aa1bf262e9241737086076eef0f238e54e75ae226fa0c845d126de11ac01e95`), which is
not redistributed here:

```
sonix-packer/
├── sonix_firmware_packer.sh
├── v3_analog_2025_original.upt   the official v1.3 UPT, renamed
├── sonix_player                  from make target
├── sonix_launch                  from make target
└── assets/BLAZE/                 the Blaze overlay
```

`./sonix_firmware_packer.sh` writes `v3_analog_2025.upt`. It needs
`mksquashfs`, `unsquashfs` and `mkisofs` (or `xorrisofs`).

Streaming services (Tidal, Qobuz, Podcast Index, Last.fm) need keys of your
own, sealed into `assets/BLAZE/usr/resource/sonix/components/streaming-keys.bin`;
see "Streaming keys" in the upstream README.

## Working on the code

- `make host` in `sonix-player/` builds a desktop simulator
  (`sonix_player_host`, SDL2) for trying the interface without the device.
- `make test` runs the host self-checks in `tools/*_selftest.c`: the battery
  usage counts, the DSD filter and the custom boot logo. Plain `gcc`, no
  toolchain, a few seconds. GitHub runs it on every push to `blaze-port`.
- `make target` ends with `make check-abi`, which refuses a binary that asks
  for anything newer than the device's glibc 2.22.
- Where things live: `src/gui/` is the interface (one folder per area,
  `nowplaying/`, `settings/`, `shell/`...), `src/system/` the rest (playback,
  library, devices, network); the tree is described in
  [UPSTREAM-README.md](UPSTREAM-README.md).
- Each Blaze change is one commit on `blaze-port` whose message says what it
  does and why; `git log -- <file>` explains a line faster than any comment.

## License and credits

GPL-3.0, like upstream ([LICENSE](LICENSE)). Sonix Player is the work of
[@Jepl4r](https://github.com/Jepl4r) and its contributors; see the special
thanks at the end of [UPSTREAM-README.md](UPSTREAM-README.md). The Blaze port
is by [@carmine-bin](https://github.com/carmine-bin). Not affiliated with
TempoTec or HiBy.
