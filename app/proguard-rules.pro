# Don't obfuscate code
-dontobfuscate

# LiteRT GPU delegate, loaded by name from native code. The runtime AAR
# ships its own rules, the GPU one does not.
-keep class org.tensorflow.lite.gpu.** {*;}

# The disc reader calls back into the engine by name from native code
-keep interface com.limelight.player.DiscNative$Sink { *; }
-keepclassmembers class * implements com.limelight.player.DiscNative$Sink {
    public *;
}
