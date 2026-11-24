# PuebloCTR

<p align="center">
  <a href="https://www.youtube.com/watch?v=PYH2Y7hJE_o"><img src="https://i.ytimg.com/vi/PYH2Y7hJE_o/maxresdefault.jpg" alt="PuebloCTR beta - the demonstration video" width="560"></a><br>
  <a href="https://www.youtube.com/watch?v=PYH2Y7hJE_o">▶ Watch the demonstration on YouTube</a>
</p>

An unofficial port of a 2005 GameCube game to the **New Nintendo 3DS / New 2DS XL**, built on
[CubeEvil4](https://github.com/Andiweli/CubeEvil4-Android): a port that runs the game's reconstructed (decompiled) code
natively, from the [decompilation by adonis-singh](https://github.com/adonis-singh/re4). PuebloCTR adds the 3DS side: a renderer on the PICA200 GPU (citro3d), sound through the DSP, movies through
the hardware H.264 decoder, a second-screen HUD with a map, gyro aiming, and a settings screen.

**Beta.** Playable; tested on the console through roughly the first third of the game, and a full playthrough is
still in progress. Both
the European and the US discs are checked from a clean install (PuebloPrep, first start, the movies, saving and
loading); the Japanese discs are new in beta5 (checked through the village, saving and loading). Speed is about 25-30 frames per second, lower in the heaviest scenes.

**No game data is included**, and this is a non-commercial fan project. You need your own copy of the game: both
GameCube discs, game ID **G4BP08** (Europe), **G4BE08** (USA) or **G4BJ08** (Japan, *biohazard 4*). PuebloPrep, a small program for your computer, makes
everything the port needs from images of your discs.

## Features

- **Gyro aiming**: while you hold R to aim (or L to ready the knife), turning the console moves the aim too - fine adjustments without the
  Circle Pad. On by default at 2x; the strength (1x to 4x) or off is chosen on the settings screen.
- **The whole bottom screen is used** during play:
  - the health of both characters with live heartbeat lines, the ammo of the current weapon and the money;
  - **CASE** and **MAP** buttons: open the attache case or the full map with one tap;
  - a **navigable map** of the area: your position and direction, the current objective, typewriters and merchants,
    treasures and blue medallions still to find; separate floors; **+ / -** to zoom, a button to jump back to you, and
    drag it with a finger to look around.
- **A settings screen** before you play (bottom screen): cheats one by one (infinite health, infinite ammo, one-hit
  kills, automatic button prompts, +1,000,000 pesetas), the gyro, the Circle Pad's sensitivity and dead zone, your
  own button mapping, the picture's fit to the screen (with or without black bars) and a frame counter.
- **The original movies** play through the console's hardware H.264 decoder, with sound; the European and the Japanese
  subtitles show as on the GameCube.
- **Made for the New 3DS hardware**: a renderer on its PICA200 GPU, sound on its DSP, the game, the renderer and the
  animation work on separate CPU cores at 804 MHz, a texture pack prepared for the 3DS GPU for smoother loading.
- **European, US and Japanese discs**; the European ones in five languages (English, French, German, Spanish, Italian -
  taken from the console's system language), the Japanese ones in Japanese, with the gore of the US release; the bottom
  screen follows the game's language.
- **Your saves**: bring GameCube saves from Dolphin or a memory card (`.gci` / `.gcs`) with PuebloPrep.
- **Easy setup**: PuebloPrep (Windows, macOS, Linux) makes everything from your own disc images - ISO, GCM or Dolphin's
  RVZ - in one go; install as a CIA (HOME Menu icon and banner) or a `.3dsx`.

## Contents

- [Features](#features)
- [What you need](#what-you-need)
- [Step 1: images of your discs](#step-1-images-of-your-discs)
- [Step 2: ffmpeg (for the movies)](#step-2-ffmpeg-for-the-movies)
- [Step 3: PuebloPrep](#step-3-puebloprep)
- [Step 4: copy to the SD card](#step-4-copy-to-the-sd-card)
- [Step 5: install the game](#step-5-install-the-game)
- [Playing](#playing)
- [Saves](#saves)
- [Updating](#updating)
- [Troubleshooting](#troubleshooting)
- [Known issues](#known-issues) · [Bugs](#bugs) · [Building](#building-for-developers)

## What you need

- A **New Nintendo 3DS, New 3DS XL or New 2DS XL**. The original 3DS / 3DS XL / 2DS are too slow and are not
  supported.
- An **SD card with about 3.5 GB free**, and a way to copy files to it (a card reader, or FTP with ftpd).
- **Images of both of your discs** (step 1).
- A computer with **Windows 10/11 (64-bit), macOS 11 or later, or Linux (64-bit)** and about 7 GB free while
  PuebloPrep works.
- **ffmpeg** for the movies (step 2; free). Without it the game runs, but its full-motion movies do not show properly.

## Step 1: images of your discs

PuebloPrep reads disc images in the **ISO / GCM** (a plain 1:1 copy, about 1.4 GB per disc) or **RVZ** (Dolphin's
compressed format) form. Make them from your own discs, for example with
[CleanRip](https://github.com/emukidid/cleanrip) on a Wii, or use the copies you already play in Dolphin.

You need **both discs of the same edition**: disc 1 and disc 2 of the European release (G4BP08), both of the US
release (G4BE08) or both of the Japanese one (G4BJ08, *biohazard 4*). Other editions (later re-releases for other
systems) do not work.

## Step 2: ffmpeg (for the movies)

The game's movies are converted for the 3DS video decoder with [ffmpeg](https://ffmpeg.org), a free program that
PuebloPrep runs. It must be a build **with the libx264 encoder** (most full builds have it). PuebloPrep checks this
when it starts and shows the result under the "ffmpeg" field. Pick your system:

**Windows**

1. Download [ffmpeg-release-essentials.zip](https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip)
   (from [gyan.dev](https://www.gyan.dev/ffmpeg/builds/), the builds linked from ffmpeg.org; they include libx264).
2. Open the zip. Inside, in the folder `bin`, there is **`ffmpeg.exe`**.
3. Copy `ffmpeg.exe` into the PuebloPrep folder (next to `PuebloPrep.exe`, step 3). PuebloPrep then finds it by
   itself. Alternatively, unpack the whole zip anywhere and choose its `bin\ffmpeg.exe` with **Browse...** in
   PuebloPrep. (PuebloPrep also looks in `C:\ffmpeg\bin` and on the PATH.)

**macOS**

1. Install [Homebrew](https://brew.sh) if you do not have it: open **Terminal** and paste the command shown on
   brew.sh, then follow what it says.
2. In Terminal: `brew install ffmpeg`
3. That is all: PuebloPrep finds Homebrew's ffmpeg by itself (`/opt/homebrew/bin/ffmpeg`, or `/usr/local/bin/ffmpeg`
   on Intel Macs). If you have an `ffmpeg` file from elsewhere, put it next to `PuebloPrep.app` or choose it with
   **Browse...**.

**Linux**

- Debian, Ubuntu, Mint: `sudo apt install ffmpeg`
- Arch: `sudo pacman -S ffmpeg`
- Fedora: the `ffmpeg-free` package has **no** libx264; install `ffmpeg` from
  [RPM Fusion](https://rpmfusion.org/Configuration) instead.

PuebloPrep finds `/usr/bin/ffmpeg` and anything on the PATH.

## Step 3: PuebloPrep

1. **Download** PuebloPrep for your computer from the [releases](../../releases) and unpack it into a folder of its
   own (for example `Documents\PuebloPrep`).
2. **Start it.** The programs are not signed by a paid certificate, so the system warns the first time:
   - **Windows**: "Windows protected your PC" - click **More info**, then **Run anyway**.
   - **macOS**: right-click `PuebloPrep.app` -> **Open** -> **Open**. On macOS 15 and later: try to open it once,
     then **System Settings -> Privacy & Security**, scroll down, **Open Anyway**.
   - **Linux**: run `./PuebloPrep` from its folder (`chmod +x PuebloPrep` if needed).
3. **Disc 1 image**, **Disc 2 image**: click **Browse...** and choose your two images.
4. **Output folder**: where PuebloPrep makes the files for the SD card. By default a new folder `PuebloCTR-SD` next
   to the program - fine to keep. Nothing is written to the SD card directly.
5. **Texture pack** (about 350 MB): keep it ticked. Without it the game stutters whenever new textures appear.
6. **Movies** (about 220 MB): keep it ticked, and look at the line under the **ffmpeg** field:
   - green **"ffmpeg works, with libx264"** - all set;
   - red **"ffmpeg not found"** - go back to step 2, or choose `ffmpeg` / `ffmpeg.exe` with **Browse...** (choosing
     the unpacked ffmpeg folder works too);
   - yellow **"no libx264 encoder"** - that ffmpeg is too minimal; take the build from step 2.

   **Prepare** stays grey while Movies is ticked and ffmpeg does not work. You can untick Movies and add them later
   (run PuebloPrep again with the same output folder).
7. **Saves to import** (optional): your GameCube saves as `.gci` / `.gcs` files (from Dolphin's memory card manager or
   a copy tool) - **Add...**. They go onto the port's memory card.
8. Click **Prepare** and wait. The progress shows next to the button and in the log below; unpacking, the texture
   pack and the movies take a while (minutes, longer on slower computers). At the end it says **Done.** and what to
   copy where. If something fails, the reason is in the red line and the log - see
   [Troubleshooting](#troubleshooting).

The output folder then holds a folder `3ds`, laid out like this:

```
3ds/pueblo/data/disc0, disc1    the discs' files
3ds/pueblo/textures.pak         the textures, made for the 3DS GPU
3ds/pueblo/movie/               the movies (H.264)
3ds/pueblo/saves/memcard.bin    the memory card (only if you imported saves)
```

PuebloPrep also has a console version, `puebloprep-cli` (`puebloprep-cli --help` lists what it does).

## Step 4: copy to the SD card

1. Turn the console off and put its SD card into your computer (or start ftpd on the console and connect to it).
2. Copy the folder **`3ds`** from the output folder to the **root** of the SD card. A `3ds` folder is already there:
   **merge** them (on macOS, copy the folder `pueblo` into the existing `3ds` folder instead, because Finder would
   replace the whole folder).
3. Check that the SD card now has `/3ds/pueblo/data/disc0` and `/3ds/pueblo/data/disc1`.

## Step 5: install the game

The quickest way: start **FBI** on the console (connected to Wi-Fi), choose **Remote Install -> Scan QR Code** and
point the camera at this code. It always gives the newest release, so the same code works for updates too.

<p align="center">
  <img src=".github/fbi-qr.png" alt="QR code for FBI: the newest PuebloCTR.cia" width="220">
</p>

Or download from the [releases](../../releases) one of:

- **`PuebloCTR.cia`** (recommended): copy it to the SD card (for example into `/cia/`), start **FBI** on the console,
  open **SD -> cia**, select `PuebloCTR.cia`, **Install CIA**. PuebloCTR then appears on the HOME Menu.
- **`PuebloCTR.3dsx`**: copy it to `/3ds/pueblo/` and start it from the **Homebrew Launcher**. Start the Homebrew
  Launcher through a game (title takeover), not as an applet - as an applet the system gives it too little memory.

Both use the same files and the same memory card.

## Playing

**At the start** PuebloCTR checks the console and the files on the SD card. If something is missing, it says so on the
bottom screen: **CANNOT START** (START exits) or **BEFORE YOU PLAY** (A continues anyway). The messages are listed
under [Troubleshooting](#troubleshooting).

**The language** of the European discs follows the console's system language (German, French, Spanish or Italian;
any other language gives English). To play in another one, change the system language in System Settings before
starting. The US discs are in English, the Japanese ones in Japanese. The Japanese discs play with the gore of the
US release (the original Japanese release cut it).

**The settings screen** is on the bottom screen while the game is at its title. It has three tabs - CHEATS,
CONTROLS and SCREEN; tap a tab to open it and a row to change it:

- **CHEATS**: infinite health, infinite ammo, one-hit kills, automatic button prompts (QTEs), +1,000,000 pesetas once;
- **CONTROLS**:
  - **Gyro aim** (on at 2x by default): while you hold R to aim (or L to ready the knife), turning the console turns the aim too; the arrows set
    how strongly, down to off;
  - **Circle Pad sensitivity** (1x by default): how far the Circle Pad has to be pushed. The game was made for the
    GameCube's long stick and wants about half of the pad's way before the character moves; if you have to push the
    pad almost to its edge, raise it (1.25x, 1.5x, 2x). Higher values also make aiming with the pad quicker;
  - **Circle Pad dead zone** (off by default, up to 50 %): how far the pad can move before it counts - raise it if the
    aim or Leon creeps on its own while you don't touch the pad;
  - **Button mapping**: any game action on any button or a combination of two (also the Circle Pad's and the C-Stick's
    directions); **Quick turn** puts the 180-degree turn on one button. Tap an action, then press the button(s);
    SELECT clears it; **Reset to defaults** brings back the table below;
- **SCREEN**:
  - **Picture**: **Fit** (by default) shows the whole picture with black bars at the sides, as the game made it;
    **Stretch** fills the screen by widening the picture a little; **Zoom** fills it keeping the shape, cutting a
    little off the top and bottom. The change shows at once on the title screen;
  - **Frame counter** in the corner of the game screen.

The choices apply when a game starts or loads, and are kept on the SD card (`/3ds/pueblo/settings.bin`). The version
of your build is in the top right corner of this screen.

**Controls** (by default, as the GameCube controller; change them under CONTROLS -> Button mapping):

| 3DS | GameCube |
|---|---|
| Circle Pad | Control stick |
| C-Stick (the small nub) | C stick |
| A, B, X, Y | A, B, X, Y |
| R | R |
| L | L |
| ZR | Z |
| D-Pad | D-Pad |
| START | START |
| START + SELECT, held for a second | quit PuebloCTR (back to the HOME Menu) |

**During the game** the bottom screen shows the health, the ammo and the money, and a map of the area: tap **CASE** or
**MAP** to open them, the map's **+ / -** buttons to zoom, and drag it with a finger to look around.

HOME and closing the lid work as usual. **Save in the game** before you quit; PuebloCTR writes the memory card to the
SD card right after the game saves.

## Saves

- The memory card is one file: `/3ds/pueblo/saves/memcard.bin` (made, empty, on the first start). The CIA and the
  `.3dsx` share it, and all editions use the same file.
- To bring saves from Dolphin or a real memory card: export them as `.gci` / `.gcs` and add them in PuebloPrep
  (step 3, item 7). To take saves out of the port: `puebloprep-cli --help` shows how to export the card's saves as
  `.gci`.
- **Copy `memcard.bin` somewhere safe** now and then, and always before an update.

## Updating

1. Back up `/3ds/pueblo/saves/memcard.bin`.
2. Install the new `PuebloCTR.cia` with FBI over the old one - scanning the [QR code](#step-5-install-the-game) again
   is enough (or replace `PuebloCTR.3dsx`).
3. The release notes say when the files from PuebloPrep must be made again; otherwise keep them. The start check says
   so too ("The texture pack does not fit this version").

## Troubleshooting

**On the console (the start check):**

| Message | What to do |
|---|---|
| A New Nintendo 3DS is needed | The original 3DS / 2DS are too slow; there is no way around it. |
| Not enough memory | Start the Homebrew Launcher through a game (title takeover), or install the CIA. |
| Game data not found | `/3ds/pueblo/data/disc0` is missing: copy the `3ds` folder from PuebloPrep (step 4). |
| Game data incomplete | The copy to the SD card was cut short: copy the `data` folder again. |
| This disc is not supported | Only the European (G4BP08), US (G4BE08) and Japanese (G4BJ08) discs work. |
| The discs are swapped / do not match | `disc0` must be disc 1 and `disc1` disc 2, of the same edition: run PuebloPrep again with the right images. |
| Disc 2 not found | You can play until the game asks for disc 2; add disc 2 with PuebloPrep. |
| Movies not found / Some movies are missing | Run PuebloPrep again with Movies ticked and a working ffmpeg (step 2). |
| Texture pack not found / does not fit this version | Run PuebloPrep again with Texture pack ticked, and copy the new `textures.pak`. |

**In PuebloPrep:**

- *ffmpeg not found* / *no libx264 encoder* - see step 2.
- *no GameCube disc header* / *... is no GameCube disc* - the file is not a disc image PuebloPrep reads (CISO and
  other compressed formats are not read; convert them to ISO or RVZ with Dolphin: right-click the game ->
  **Convert File...**).
- *disc 1 is missing* / *disc N given twice* / *the discs are of different editions* - check which image is which.
- *not enough room* - free space on the drive of the output folder (about 7 GB while it works).
- PuebloPrep writes `puebloprep.log` next to itself: attach it to a bug report.

**If the game stops with an error**, it writes `/3ds/pueblo/crash.txt`. Please attach it to a bug report.

## Known issues

This is a beta. Known so far:

- **Speed**: about 25-30 frames per second, down to about 20 in the heaviest scenes (heavy rain, big fights).
- **Glows of lamps and fires can show through walls.** The game hides a glow by reading the depth of the finished
  picture around the light; the 3DS renderer does not hand that back yet. To be fixed in a later beta.
- **Effects that read the picture back are missing**: some blur, depth of field and heat haze, part of the shadows.
  Bump mapping on some clothes is missing.
- **Loading between areas** shows a still picture for up to about 2 seconds.
- **The HOME Menu** could crash when opened during play. The cause is found and fixed in beta5, and I hope it is gone
  completely; please report it if it still happens. If you copy files to the SD card over the network (ftpd), restart the
  console before you play: otherwise the HOME Menu can still crash. Save before you open it.
- **Not yet checked on the console**: the rest of the game after roughly its first third (the playthrough is in progress -
  problems there are possible; please report them) and the extra modes. The movies' subtitles were checked in German; French, Spanish and
  Italian work the same way but were not watched.
- **Saves**: all editions use the same memory card file (`/3ds/pueblo/saves/memcard.bin`), for the CIA and the
  `.3dsx` alike. Copy it somewhere safe before you update.

## Bugs

Please open an issue with the version from the settings screen, what happened, where in the game, your disc's game ID and edition (PuebloPrep's log says
it), and `/3ds/pueblo/crash.txt` if there is one.

## Building (for developers)

The game is built on Linux (or in the image on any system): CubeEvil4 holds `src/Tools` and `src/tools` side by side,
which needs a case-sensitive file system - on macOS and Windows, clone inside the container or a Linux VM.
PuebloPrep only needs CubeEvil4's `port/` and builds anywhere.

Everything is built in one Docker image ([`docker/Dockerfile`](docker/Dockerfile): devkitPro with devkitARM r68, libctru 2.7.0,
citro3d 1.7.1, LLVM 22, makerom, bannertool); CI uses the same image.

```
git clone --recursive <this repository>
docker build -t puebloctr-build docker
docker run --rm -v "$PWD":/work/repo -w /work/repo puebloctr-build bash -c \
  'tools/build.sh --release --version dev && tools/build_cia.sh dev'
# -> build/tree/port/build/3ds-release/PuebloCTR.3dsx, PuebloCTR.cia
```

PuebloPrep (CMake; SDL 3 and Dear ImGui are fetched at fixed versions):

```
tools/fetch_cubeevil4.sh --port-only      # or: git submodule update --init
tools/build_puebloprep.sh linux|windows|macos [VERSION]
```

`windows` is cross-built on Linux with mingw-w64; `macos` makes a universal app.

### How the repository is laid out

PuebloCTR is kept as changes over CubeEvil4, which is a submodule at a fixed commit ([`patches/BASE`](patches/BASE)):

```
external/CubeEvil4               CubeEvil4 (submodule)
overlay/                         PuebloCTR's own files, at their paths in CubeEvil4's tree
patches/cubeevil4.patch          PuebloCTR's changes to CubeEvil4's files (git diff -U0)
patches/cubeevil4-removed.txt    CubeEvil4's files the build does not use
tools/make_tree.py               submodule + overlay + patch -> build/tree
tools/minimap_gen.py             the map screen's rules, read from the game's source at build time
tools/ui_assets.py               the bottom screen's pictures and strings -> overlay/.../3ds/ui_assets.h (committed)
tools/cia_assets.py              the icon, the banner and its sound -> overlay/port/whole/3ds/cia/ (committed)
tools/fetch_fonts.sh             the Noto Serif JP faces those two draw with (only needed to change them)
```

`tools/build.sh` makes `build/tree` first; to change the port, edit the tree, then carry the change back into
`overlay/` or `patches/`.

## Thanks

- [CubeEvil4](https://github.com/Andiweli/CubeEvil4-Android) by Andiweli: the port to modern systems that PuebloCTR is
  built on.
- The decompilation by [adonis-singh](https://github.com/adonis-singh/re4) that CubeEvil4 is a fork of: the game's
  debug build reconstructed function by function, which made all of this possible; and
  [dolsdk2004](https://github.com/doldecomp/dolsdk2004) (doldecomp), the GameCube SDK sources it builds with.
- devkitPro (devkitARM, libctru, citro3d), Luma3DS and the 3DS homebrew community.
- The New 3DS ports this one learned from: sms-3ds, REGTA, MKW-3DS, OpenNectar-3DS, GalaxyCTR.

## Licence

PuebloCTR's own work ([overlay/](overlay), [patches/](patches), [tools/](tools), [docker/](docker)) is under the MIT licence ([LICENSE](LICENSE), with its scope).
CubeEvil4 has its own licence; the game's code it reconstructs belongs to its owners. Third-party parts:
[THIRD-PARTY.md](THIRD-PARTY.md).

This project is not affiliated with or endorsed by Nintendo, Capcom or CRI Middleware.
