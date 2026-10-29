<p align="center">
  <img src="aaks-logo.png" height="160" alt="AaksVR">
</p>

# AaksVR

A 3D movie player for the Meta Quest 3, running as a native OpenXR app. It plays side by side and
over under 3D files, 3D Blu-ray disc images, and ordinary 2D films, which it can turn into 3D as
they play.

![AaksVR playing a film on a giant screen in a living room](docs/screenshots/1_hero.jpg)

## Install

1. Download the latest `AaksVR-*.apk` from [Releases](https://github.com/jashwanth176/aaks-player/releases).
2. Turn on Developer Mode for your Quest in the Meta Horizon phone app.
3. Install the APK with [SideQuest](https://sidequestvr.com) or `adb install AaksVR-0.9.0.apk`.
4. On the headset, open AaksVR from **Library > Unknown Sources**, and allow file access so it can
   find your films.

## Features

### Every kind of 3D
- **3D files:** side by side and over under, full or half width. The layout is read from the file
  name (`HSBS`, `FSBS`, `OU`, `TAB` and similar) or the shape of the frame, or set by hand in
  **Settings > 3D** (Auto, 2D, Side by side, Over under).
- **3D Blu-ray discs:** plays `.iso` disc images directly, with the MVC 3D video decoded on the
  headset, Dolby and DTS sound, and the disc's PGS subtitles.
- **2D films in 3D:** converts any 2D film live as it plays (see below).
- **Depth:** choose how far 3D films pop out: Pop out, Balanced or Full.

### Picture
- **HDR10 and HLG** films are tone mapped for the Quest's screen, so highlights keep their detail.
- **Colours:** Natural shows the film as it was graded; Vivid uses the headset's wider colour range.
- **Black bars** are detected and trimmed, so the screen fits the picture.
- **A screen your way:** move it, resize it from its corners, and curve it from flat to wrapped
  around you.

### Your room
- **Passthrough or Dark:** watch with your room around you, or in darkness.
- **Walk around:** in passthrough the guardian boundary steps aside, so you can move about the room.
- **Immersion and Brightness:** let the screen's light spill into the room, and dim the room as much
  as you like.

### Sound and subtitles
- Plays the sound in films that the headset can't decode on its own, such as Dolby TrueHD and DTS.
- **Playback speed** from 0.5x to 2x, with the voice keeping its pitch.
- Pick any **audio or subtitle track**; your choice survives switching between 2D and 3D.
- **Subtitles** from the file, or `.srt`, `.ass`, `.ssa` and `.vtt` files placed next to it with the
  same name.
- **Subtitle style:** size, colour, background, edge, height on the screen, depth in 3D, and whether
  to use the file's own styling.
- **Subtitle sync:** shift them earlier or later, remembered for each film.

### Library
- Finds your films anywhere on the headset's storage: `mp4`, `m4v`, `mkv`, `mov`, `webm`, `ts`,
  `m2ts`, `mts`, `avi` and `iso`.
- **Continue** for films you're part way through, **Films** for everything (newest first), and
  **Folders** to browse.
- Thumbnails, a 3D badge and a progress bar on each film, and every film resumes where you left it.
- Films can also be opened straight from a file manager.

### Controls
- **Controllers:** point and pull the trigger to click, A to play or pause, and push the thumbstick
  left or right to skip 10 seconds (hold it to skip 30 seconds at a time).
- **Hands:** pinch to click. **Double tap** the left or right of the picture to skip 10 seconds, and
  keep tapping to skip further.
- **Hand pointer:** hides when your hand is idle; show an open palm to bring it back. Prefer it
  always visible? Set **Hand pointer** to **Always on** in Settings.
- Pauses when you take the headset off or open the Quest menu, and carries on when you're back.

## Watch any 2D film in 3D, live

No conversion step and nothing to wait for: AaksVR estimates depth on the headset as the film plays
and shows each frame in 3D.

![A 2D film beside the depth AaksVR works out for it](docs/screenshots/2_2d_to_3d.jpg)

1. Open a regular 2D film from the library.
2. Open **Settings** on the control bar.
3. Set **2D films in 3D** to **On**. The setting is remembered for every 2D film after that.

## Screenshots

| | |
|---|---|
| ![Library](docs/screenshots/3_library.jpg) | ![Double tap to skip](docs/screenshots/4_double_tap.jpg) |
| ![Settings](docs/screenshots/5_settings.jpg) | ![Giant screen](docs/screenshots/1_hero.jpg) |

## Licence

GPLv3 (see `LICENSE.txt`). Third-party credits and licences are in [CREDITS.md](CREDITS.md).
