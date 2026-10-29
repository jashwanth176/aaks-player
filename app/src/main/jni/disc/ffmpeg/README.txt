FFmpeg n7.1.1, a minimal LGPL-2.1 build (DTS and TrueHD decoders with parsers only):
source https://github.com/FFmpeg/FFmpeg tag n7.1.1, configured with
  --disable-everything --enable-decoder=dca,truehd,mlp --enable-parser=dca,mlp --disable-programs --disable-avformat --enable-static
for aarch64, built by disc-work/build_ffmpeg.sh. The static libraries can be relinked with a different FFmpeg build.
