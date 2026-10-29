<p align="center">
  <img src="aaks-logo.png" height="160" alt="Aaks">
</p>

# Aaks

A 3D movie player for the Meta Quest 3, running as a native OpenXR app. It plays side by side and
over under 3D files, 3D Blu-ray disc images, and ordinary 2D films, which it can turn into 3D as
they play.

## What it does

- Plays video files through Media3, with TrueHD and DTS sound decoded in the app.
- Plays 3D Blu-ray ISOs: the MVC video decoded on the headset, Dolby and DTS sound, PGS subtitles.
- Turns a 2D film into 3D while it plays, with the ZipDepth depth model on the headset's GPU.
- Shows HDR10 and HLG films tone mapped for the headset's screen, with Natural and Vivid colours.
- Plays at 0.5x to 2x, the sound keeping its pitch, and shows where a skip will land.
- Styled subtitles with their own depth and sync, a curved screen, and passthrough or a dark room.
- Double tap the left or right of the picture to skip 10 seconds back or forward.
- Hand tracking: the pointer hides when the hand is idle; show an open palm to bring it back.

## Building

Requires Android Studio with the NDK. Debug build:

    ./gradlew assembleDebug

The APK lands in `app/build/outputs/apk/debug/`. Install it with `adb install -r`. The app
is `com.aaks.player`, and the debug build `com.aaks.player.debug`.

### Tests

The parts of the renderer with no GL, OpenXR or Android in them build and run on a desktop with any
C compiler, and the Java side has plain unit tests:

    make -C app/src/test/cpp test
    ./gradlew testDebugUnitTest

Release builds are signed when `keystore.properties` exists; see `keystore.properties.example`.

## Licences

GPLv3, as the projects it is built on (see `LICENSE.txt`). Added dependencies are all compatible: the Khronos OpenXR loader (Apache 2.0),
LiteRT and its GPU delegate (Apache 2.0), and the MiDaS v2.1 small depth model (MIT), converted to
TensorFlow Lite by `tools/convert_midas.py` and committed as an asset.

The default depth model is [ZipDepth](https://github.com/fabiotosi92/ZipDepth) by Fabio Tosi (MIT),
converted to TensorFlow Lite by `tools/convert_zipdepth.py` (fp16, 512x288) and
`tools/quantize_depth.py` (the int8 copy the XR2 Gen 1 headsets run) and committed as assets. Its
licence ships in the APK as `assets/licenses/zipdepth_LICENSE.txt`. Two ideas in the depth path
came from [Nightfall](https://github.com/tB0nE/nightfall) (GPLv3), another Moonlight client with
realtime 3D: exporting ZipDepth's learned upsampling head for the GPU delegate, and smoothing the
depth map over real time rather than per frame. Both are implemented independently here.

The Blu-ray path bundles [FFmpeg](https://ffmpeg.org) audio decoders (LGPL 2.1),
[edge264](https://github.com/tvlabs/edge264) for MVC video (BSD) and
[libudfread](https://code.videolan.org/videolan/libudfread) for reading disc images (LGPL 2.1),
each with its licence in `app/src/main/jni/disc/`.

The controller model drawn when "Show the controllers" is on is this repository's own work,
Copyright (c) 2026 Sean Gilleece / Woodford XR, licensed under
[Creative Commons Attribution 4.0](http://creativecommons.org/licenses/by/4.0/) with attribution to
Sean Gilleece / Woodford XR. It is made in code by `tools/make_controller.py` and baked by
`tools/bake_room.py`, as `tools/models/controller/NOTE.txt` describes.

Aaks is built on [Moonlight for Android](https://github.com/moonlight-stream/moonlight-android) by
Cameron Gutman, Diego Waxemberg, Aaron Neyer and Andrew Hennessy, through the OpenXR fork of it by
Sean Gilleece, both GPLv3; their copyright notices stay in the source.
