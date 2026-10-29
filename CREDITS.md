# Credits and licences

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

The screenshots show frames from [Sintel](https://durian.blender.org) and
[Tears of Steel](https://mango.blender.org), © Blender Foundation, CC BY 3.0, and NASA
photographs from the International Space Station (public domain). The rooms are
panoramas from [Poly Haven](https://polyhaven.com) (CC0), and the depth map was made by the app's
own ZipDepth model.
