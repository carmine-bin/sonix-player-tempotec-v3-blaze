# Features

This is everything Sonix Player can do today, on both the **R1** and the **R3 Pro II** unless it says otherwise.

> If something is not on this page, Sonix Player does not do it yet. It is not a bug: open a **Feature request**.

---

## Supported audio formats

`.wav` `.flac` `.mp3` `.ogg` `.opus` `.m4a` `.m4b` `.mp4` `.aac` `.wv` `.wvc` `.ape` `.dsf` `.dff` `.aif` `.aiff` `.aifc` `.caf`

- **CUE sheets** - a whole disc ripped into one file with a `.cue` next to it shows up as separate tracks.
- **Tags in WAV and AIFF** are read too, cover included.

## First start

- Choose the language, then connect to Wi-Fi so the date and time are set by themselves. You can skip the Wi-Fi step.
- If there is no microSD card in the player, a message tells you.

## Music library

> Scan the library from Settings > Music. You choose which folders of the card are scanned.

- **Main page** - All tracks, Albums, Artists, Album artists, Genres, Years, Favourites, Most played, Playlists and Browse. You can rearrange the tiles and hide the ones you don't use.
- **Browse** - go through the folders of the card as they are.
- **Search** across tracks, albums and artists.
- **Sorting** - by name (A-Z or Z-A), by date added (newest or oldest first) or by year.
- **A-Z bar** on the side when you scroll a long list.
- **Sorting works with every alphabet**, ignores upper and lower case and accents, and skips words like "The" at the start (you can turn that off).
- **Albums with the same name stay apart** when they belong to different artists or folders.
- **Split artists and genres** - a track with "A & B" or "A feat. B" shows up under both names. You choose the symbols that split them, and you can add names that must never be split.
- **Join albums** - tracks of the same album in the same folder become one album, even if the artists are different.
- **Detect changes** - new tracks are added by themselves when the player starts, when the microSD goes back in, after USB or after Wi-Fi transfer. It can also pick up tracks you retagged.
- **Alternative album view** - the album opens with its big cover on top, with title, artist, genre and year, and the tracks underneath.
- **Tapping an artist, a genre or a year** can open its albums instead of its tracks.
- **Show artist** under the title in the lists you choose.
- **Quality badge** under the title: lossy, CD, Hi-Res or DSD.
- **Go to current track** - the lists open on what is playing.
- **Favourites** and **Most played** - the player counts how many times you listen to every track.
- **Selection** - long press a track, album or artist to select more, then add them all to the queue, to the favourites or to a playlist, or remove them from a playlist.
- **Delete tracks** from the card, after asking (turn it on in the settings first).
- **Playlists** - create, rename, reorder and delete. You can back them up to the card as `.m3u` files that other players can read, and import `.m3u` playlists back, even when some files were moved.

## Playback

- **Playback modes** - normal, repeat all, repeat one, shuffle, and a continuous shuffle that deals a new order every round.
- **Play albums back to back** - when an album ends, the next one starts.
- **Play by folder** - when a folder ends, playback goes on with the next folder.
- **Add to queue** - as the next track or at the end.
- **Queue** - reorder it, select tracks in it, and see the **history** of what you played today, yesterday and before.
- **Gapless** - no silence between tracks of the same album, also over Bluetooth.
- **Direct mode** - one switch that bypasses the equaliser, PEQ, MSEB, soundfield, crossfeed, mono, balance and ReplayGain, so the decoded samples reach the DAC untouched. Each of them keeps its settings and comes back when you turn it off.
- **Fade** - the end of a track fades into the next one, from 1 to 12 seconds.
- **ReplayGain** - off, by track or by album, read from the tags.
- **Remember track** - the queue and the position are still there after a restart. You can also open Now Playing straight away at startup.
- **Sleep timer** - set a time and the playback pauses by itself when it runs out. There is one for music, one for audiobooks and one for podcasts.

## Now Playing

- The cover fills the top of the screen, with the same picture blurred behind the controls.
- **Three layouts** - Standard, Waveform (the progress bar becomes the waveform of the track) and Alternative.
- **Cover colour** - the buttons and the bar take the main colour of the cover.
- **Cover Flow** - swipe up from the bottom edge of the Music page to flip through your albums.
- **Queue**, **Details** (format, bitrate, size, ReplayGain and more) and **Show album** from the menu.
- **Chapters** and **speed** when an audiobook or a podcast is playing.

## Lyrics

- Swipe on the cover to show the lyrics. Synced lyrics follow the song line by line, and the screen stays on while you read them.
- Lyrics are read from a `.lrc` or `.txt` file with the same name as the track, or from the tags of the file.
- **Download from LRCLIB** - when a track has no lyrics, the player can look for them online and you pick the one you want (synced or text only). If Wi-Fi is off, it asks to turn it on and starts the search by itself.

## Sound

- **Graphic EQ** - 10 bands, ±12 dB, with 10 presets (Flat, Rock, Pop, Jazz, Classical, Dance, Bass, Treble, Vocal, Acoustic) and as many of your own as you like.
- **Parametric EQ (PEQ)** - 10 bands, each with its own frequency, gain, Q and shape (peak, low shelf, high shelf, low pass, high pass), plus a preamp. The graph shows the real curve. Tap a number to type it on a keypad.
  > PEQ presets are `.txt` files in the `PEQ` folder of the card, in the Equalizer APO format.
- **AutoEq** - type the name of your headphones and the player downloads their correction over Wi-Fi.
- **MSEB** - the same sound tuning as the stock HiBy player, with 10 sliders (Temperature, Extended bass, Bass texture, Note thickness, Vocals, Female overtones, Male vocal, Female vocal, Instruments, Air).
- **Soundfield** - make the stereo image narrower (down to mono) or wider.
- **Crossfeed** - mixes a bit of each channel into the other, like speakers do.
- **Channel balance** and **Mono audio**.
- The EQ, PEQ and MSEB also work on radio and streaming.
- **DAC settings** - 4 digital filters, DRE, NOS, high gain, and gain compensation for DSD.
- **DSD** - played natively, or converted to PCM when the output cannot take it (for example over Bluetooth).

## Volume and outputs

- **The same volume curve** as the stock player.
- **Remember volume** - one level for each output: 3.5 mm, 4.4 mm, USB-C and Bluetooth.
- **Volume limit** - a maximum volume for each output.
- **Volume lock** - a control centre button that freezes the volume: the keys, the cable's remote, the on-screen slider, the app and DLNA stop changing it until you turn it off. A padlock shows in the status bar. Bluetooth headphones' own buttons still work.
- **Volume steps** - how much one press of the volume key changes the volume.
- **Line out** - the jack goes to a fixed level for an amplifier, and your volume comes back when you turn it off.
- **USB DAC** - connect the player to a computer and it becomes its sound card, from 32 kHz to 384 kHz. You can choose whether it charges at the same time.
- **USB audio out** - plug a USB DAC or USB-C headphones into the player.

## Streaming

> Streaming needs Wi-Fi.

- **Tidal** - sign in with the code on screen or the QR code. Low, High, CD and Hi-Res quality. Search, albums, artists, playlists, favourites, new releases, most streamed and recommended. You can add and remove favourites and manage your playlists from the player.
- **Qobuz** - sign in with your account. MP3 320, FLAC 16/44.1, 24/96 and 24/192. Search, albums, artists, playlists, favourites and new releases. You can add and remove favourites and manage your playlists from the player.
- **Radio** - thousands of stations by country, language or genre, plus search, favourites and recently played. MP3, AAC and FLAC stations.
  > **My stations** - write your own list in a `radio.txt` file in the root of the card, one station per line: `name, address`.
- **Last.fm** - scrobbles what you listen to. Without Wi-Fi the plays are kept and sent the next time the player connects.

## Podcasts

- **Search** and **Trending** from the Podcast Index catalogue, or add a podcast by its feed address.
- **Follow** up to 100 podcasts.
- **Download** episodes to the card, one at a time or many together, and listen to them without Wi-Fi.
- **Podcasts on the card** - your own podcasts are played too.
  > One folder per podcast inside the `Podcast` folder. Title, date and cover are read from the tags.
- **Continue listening** - every episode remembers where you stopped.
- **Heard and to hear** - filter the episodes and mark them as heard or not. An episode counts as heard when you stop in its last 30 seconds, 1 minute or 2 minutes (you choose).
- **Play next**, delete downloaded episodes, newest or oldest first.
- **Speed** and **stop at the end of the episode**.
- **OPML** - import the podcasts you follow in another app, and export yours.
  > Put the `.opml` file in the root of the card or in the `Podcast` folder.

## Audiobooks

> Audiobooks go in the `Audiobooks` folder, in any subfolder. `Audiobooks/Author/Series/...` is used for the author and the series when the tags don't say.

- **Library, Series, Authors and Continue**, each sortable. Continue shows the books you started, with the finished ones one tap away.
- **A book can be one file or a whole folder** - an `.m4b`, an `.mp3` or any other file with chapters, or a folder of files (with `CD 1`, `Disc 2` subfolders too).
- **Chapters** are read from the file, and the position is remembered for every book.
- **Speed** from 0.5x to 2.0x without changing the pitch.
- **Skip buttons** - choose how far back and forward they jump: 10, 30 or 60 seconds.
- **Rewind after pause** by a few seconds, so you don't lose the thread.
- **Stop at the end of the chapter**.
- **Bookmarks** (up to 64 per book) and the **summary** from the tags.
- The progress bar shows the whole book or just the current chapter.

## E-books

> `.epub` files go in the `Ebook` folder.

- EPUB 2 and 3, with covers, pictures, headings, lists, quotes and the table of contents.
- Japanese books that are written vertically are shown vertically.
- Text size, line spacing, word spacing and margins.
- Three reading themes (Paper, Sepia, Night) and three page-turn animations.
- **Bookmarks** - double tap the top of a page. Up to 64 per book.
- **Reading bar** at the bottom with chapter, page, progress, battery and clock, each one optional.

## More

- **File manager** - browse the card, create folders, rename, move, copy and delete. Pictures and text files open right there.
- **Metronome** - 30 to 250 BPM, up to 7 beats per bar, and tap tempo.
- **Game Boy** (Gearboy) - plays Game Boy and Game Boy Color games, with their real names, save states, five palettes, shaders and color correction.
  > ROMs go in `Games/GB` and `Games/GBC`. The boot ROMs (optional) go in `Games/Bios`: `dmg_boot.bin` and `cgb_boot.bin`.
- **Flappy Bird**, with sounds.

## Wireless

- **Wi-Fi** - see the networks around you, connect, forget, and see the details of the one you are on.
- **Bluetooth headphones and speakers** - pair, connect and forget. The best codec your device supports is chosen by itself, and you can see the list. The volume can be synced with the headphones, and you can rename your paired devices.
- **AirPods** - battery for each earbud and the case, and noise control (off, noise cancellation, transparency, adaptive). You choose which modes the stem switches between.
- **Bluetooth receiver** - a phone or a computer connects to the player and plays through it, like a speaker. The codec and the quality of what comes in are shown.
- **AirPlay** - play from an iPhone, iPad or Mac on the player.
- **DLNA** - send music to the player from an app on your phone (needs a microSD card).
- **Wi-Fi transfer** - open `sonix-transfer.local` (or the address on screen) in a browser on the same Wi-Fi to upload files and folders, download, rename, move and delete, and see hidden files. Music stops while it is on.
- **SonixLink** - control the player from the SonixLink app on your phone, over Wi-Fi or Bluetooth: browse the library, the queue and the favourites, with covers.
- **USB storage** - with the player on, connect it to a computer or a phone with the cable and the microSD shows up as a drive. With Detect changes on, the library picks up the new music when you unplug it.

## Control centre and status bar

- Pull down from the status bar (or tap it) to open the **Control centre**: what is playing, brightness, and buttons for Wi-Fi, Bluetooth, AirPlay, DLNA, SonixLink, Wi-Fi transfer, EQ, PEQ, MSEB, Fade, Gapless, Gain, Line out, Volume lock, Direct mode and the three sleep timers.
- Choose which buttons are shown and in what order.
- The status bar shows the clock and the battery, and can stay on in Now Playing too.

## Buttons and remotes

- **Remap buttons** - give each media and volume button another action.
- **Double click** - two quick presses of a button do the action you choose.
- **In-line remote** - on wired headphones: one click play/pause, two clicks next, three clicks previous. USB-C headphones with buttons work too.
- **Lock the buttons** with the screen off: volume down + power. The power button and the remote always work.
- **Keyboard** - ten layouts (English, Italian, German, Spanish, French, Dutch, Polish, Brazilian Portuguese, Czech, Russian), as a full keyboard or as a phone keypad (T9).
- **Screenshots** - turn them on in Settings > More, then press volume up + power to save the screen to the card.

## Screen and appearance

- **Dark and light theme**, accent colours, and a **dynamic tint** that colours the whole interface with the accent.
- **Text size** normal or large, **rounded corners**, battery percentage, and clock on the left, in the centre, on the right or hidden.
- **Brightness** down to 1%, **screen-off time**, **double tap to wake**, and **rotate the screen** by 180°.
- **Screensaver** - shows the cover of what is playing, or pictures from the `Screensaver` folder.
  > Pictures must be JPEG (not progressive), portrait, and no larger than 480x720 on the R3 Pro II or 480x800 on the R1.
- **Boot logo** - the default one follows your theme and accent colour, or pick your own picture.
  > Put JPEG or PNG pictures in the `Bootlogo` folder. They are cropped to fill the screen.

## Power and battery

- **Standby** after the time you choose, and **auto power-off** when nothing is playing.
- **Battery screen when off** - plug in the charger with the player off and you see the battery level. Press the power button to see it again, hold it to start the player.
- **Charge limit** - stop charging before 100% to make the battery last longer: at 80% on the R1, or anywhere from 80% to 100% on the R3 Pro II. On the R1 you can also limit the charging current to 500 mA.
- **Low battery** - a warning at 5%, and at 1% the player turns itself off with a 10 second countdown, also in standby.
- **LED** - on or off, and off in standby.
- **Power menu** - slide to power off or restart.

## System

- **Date and time** - automatic over Wi-Fi (the time zone is found by itself) or set by hand, with 24-hour or AM/PM clock and daylight saving time.
- **Languages** - English, Italiano, Deutsch, Español, Français, Nederlands, Polski, Português BR, Čeština, Russian, Chinese and Japanese. Titles in Korean, Thai and Arabic are shown too.
- **Firmware update** - over the internet with the full list of changes, or from a `.upt` file on the card.
- **About** - device, serial number, card space and version.
- **Factory reset** - settings, Wi-Fi networks and Bluetooth devices are erased. The music on the card is not touched.
- **Developer options** - tap "Build number" five times in About: ADB, log to the microSD, database log, Bluetooth log, and running processes and memory.
