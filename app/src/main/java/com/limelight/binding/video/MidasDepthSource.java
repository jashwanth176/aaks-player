package com.limelight.binding.video;

import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.os.SystemClock;

import com.limelight.LimeLog;
import com.limelight.preferences.PreferenceConfiguration;

import org.tensorflow.lite.Interpreter;
import org.tensorflow.lite.gpu.CompatibilityList;
import org.tensorflow.lite.gpu.GpuDelegate;
import org.tensorflow.lite.gpu.GpuDelegateFactory;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.MappedByteBuffer;
import java.nio.channels.FileChannel;
import java.util.ArrayList;
import java.util.List;

/**
 * A relative depth model on LiteRT: ZipDepth, or MiDaS v2.1 small on XR2 Gen 2.
 * The asset, size and runtime per headset come from Spec and Route; the assets
 * are built by tools/convert_zipdepth.py, tools/quantize_depth.py and
 * tools/convert_midas.py. Input is RGB in 0..1 (normalization is in the graph),
 * output is relative inverse depth, larger is nearer.
 *
 * Create and use on the thread holding the GL context; the GPU delegate binds to it.
 */
public class MidasDepthSource implements DepthSource {

    /** How one headset generation runs a model. */
    public static final class Route {
        public final String asset;
        // The export's input size, which the session's depth map uses
        public final DepthSize size;
        public final boolean gpu;
        public final int threads;

        Route(String asset, DepthSize size, boolean gpu, int threads) {
            this.asset = asset;
            this.size = size;
            this.gpu = gpu;
            this.threads = threads;
        }

        /** Asset and runtime, for the log. */
        public String label() {
            return asset+" on "+(gpu ? "gpu" : "cpu ("+threadCount(threads)+")");
        }
    }

    /**
     * One model: its list_vr_depth_source value, its default separation and
     * convergence (comfortable separation depends on the model's map), and the
     * route for each headset generation.
     */
    public static final class Spec {
        public final String key;
        public final String name;
        // Separation in tenths of a percent of frame width, convergence in percent
        public final int defaultSeparation;
        public final int defaultConvergence;
        private final Route gen2Route;
        private final Route gen1Route;

        Spec(String key, String name, int defaultSeparation, int defaultConvergence,
             Route gen2Route, Route gen1Route) {
            this.key = key;
            this.name = name;
            this.defaultSeparation = defaultSeparation;
            this.defaultConvergence = defaultConvergence;
            this.gen2Route = gen2Route;
            this.gen1Route = gen1Route;
        }

        public Route route(boolean gen1) {
            return gen1 ? gen1Route : gen2Route;
        }
    }

    // LiteRT's default XNNPACK path is single threaded, so CPU routes bring their
    // own delegate. Quest 2 gives an app three big cores: 2 threads took a map
    // from 54.6 to 31.2 ms, a third only to 27.2 ms at a cost to other threads.
    private static final int CPU_THREADS = 2;
    // The default path, used when that delegate is unavailable, runs one thread anyway
    private static final int DEFAULT_PATH_THREADS = 1;
    private static final int FALLBACK_CPU_THREADS = 2;

    // One step past MiDaS: 6 reads clearly deeper than 5 for little extra strain;
    // 7 and 8 strain more for no more depth
    public static final int ZIPDEPTH_SEPARATION = 6;

    // 16:9 so a 16:9 frame is not squashed, with the pixel count of ZipDepth's 384 square
    public static final Spec ZIPDEPTH = new Spec(PreferenceConfiguration.VR_DEPTH_SOURCE_ZIPDEPTH,
            "ZipDepth", ZIPDEPTH_SEPARATION, PreferenceConfiguration.DEFAULT_VR_CONVERGENCE,
            new Route("zipdepth_512x288_fp16.tflite", new DepthSize(512, 288), true, CPU_THREADS),
            // XR2 Gen 1 GPUs reject this graph, so Gen 1 runs an int8 dynamic range copy on the CPU
            new Route("zipdepth_256_int8dr.tflite", DepthSize.square(256), false, CPU_THREADS));

    // Same route on both generations, though Gen 1 is never offered it
    public static final Spec MIDAS = new Spec(PreferenceConfiguration.VR_DEPTH_SOURCE_MIDAS,
            "MiDaS", PreferenceConfiguration.DEFAULT_VR_SEPARATION,
            PreferenceConfiguration.DEFAULT_VR_CONVERGENCE,
            new Route("midas_v21_small_256_fp16.tflite", DepthSize.square(256), true, CPU_THREADS),
            new Route("midas_v21_small_256_fp16.tflite", DepthSize.square(256), true, CPU_THREADS));

    // GPU kernel cache under the app's cache dir: the first compile takes seconds,
    // reading back a fraction of that
    private static final String KERNEL_CACHE_DIR = "depth-kernels";

    private static final int WARMUP_RUNS = 3;
    private static final int BENCHMARK_RUNS = 10;
    private static final int CPU_BENCHMARK_RUNS = 3;

    // Times a CPU interpreter beside the GPU one at startup, to confirm the delegate
    // is really on the GPU (measured 183-265 ms CPU vs 13.5 ms GPU). Costs about a second.
    private static final boolean BENCHMARK_CPU = false;

    private final Route route;
    // The interpreter reads the model for as long as it lives
    private MappedByteBuffer model;
    private Interpreter interpreter;
    private GpuDelegate gpuDelegate;
    private XnnpackDelegate cpuDelegate;
    // Staging for every pair, and the pair the next run uses. Warmup and benchmark use pair 0.
    private ByteBuffer[] inputs;
    private ByteBuffer[] outputs;
    private ByteBuffer input;
    private ByteBuffer output;
    private boolean gpuAccelerated;
    private boolean kernelCache;
    private int cpuThreads;
    private long loadMs;
    private long lastInferenceNs;

    public MidasDepthSource(Route route) {
        this.route = route;
    }

    /** The model a list_vr_depth_source value names: MiDaS, else ZipDepth. */
    public static Spec specFor(String value) {
        return MIDAS.key.equals(value) ? MIDAS : ZIPDEPTH;
    }

    public static List<Spec> offeredSpecs(boolean gen1) {
        List<Spec> offered = new ArrayList<>();
        for (Spec spec : new Spec[] { ZIPDEPTH, MIDAS }) {
            if (PreferenceConfiguration.isDepthSourceOffered(spec.key, gen1)) {
                offered.add(spec);
            }
        }
        return offered;
    }

    /** The session's depth map size: the input size of the export this headset loads. */
    public static DepthSize sessionSize(String value, boolean gen1) {
        return specFor(value).route(gen1).size;
    }

    @Override
    public boolean initialize(Context context, ByteBuffer[] inputBuffers,
                              ByteBuffer[] outputBuffers) {
        long start = SystemClock.elapsedRealtime();
        int pairs = Math.min(inputBuffers.length, outputBuffers.length);
        if (pairs == 0) {
            LimeLog.severe("No depth staging handed over");
            return false;
        }
        inputs = new ByteBuffer[pairs];
        outputs = new ByteBuffer[pairs];
        for (int i = 0; i < pairs; i++) {
            inputs[i] = inputBuffers[i].order(ByteOrder.nativeOrder());
            outputs[i] = outputBuffers[i].order(ByteOrder.nativeOrder());
            if (!stagingFits(inputs[i].capacity(), outputs[i].capacity(), route.size)) {
                LimeLog.severe("Depth staging is "+inputs[i].capacity()+" and "
                        +outputs[i].capacity()+" bytes, not a "+route.size+" map's");
                return false;
            }
        }
        usePair(0);

        try {
            model = loadModel(context, route.asset);
        } catch (IOException e) {
            LimeLog.severe("Depth model asset unreadable: "+route.asset+": "+e.getMessage());
            return false;
        }
        if (!buildInterpreter(context)) {
            return false;
        }
        loadMs = SystemClock.elapsedRealtime() - start;

        int[] inputShape = interpreter.getInputTensor(0).shape();
        int[] outputShape = interpreter.getOutputTensor(0).shape();
        LimeLog.info("Depth model loaded in "+loadMs+" ms, "+route.asset+" on "+runtimeLabel()
                +", kernel cache "+(kernelCache ? "on" : "off")+", input "
                +shapeToString(inputShape)+" output "+shapeToString(outputShape));

        if (!inputFits(inputShape, route.size) || !outputFits(outputShape, route.size)) {
            LimeLog.severe("Depth model shape mismatch: input "+shapeToString(inputShape)
                    +" and output "+shapeToString(outputShape)+" for a "+route.size+" map");
            release();
            return false;
        }

        for (int i = 0; i < WARMUP_RUNS; i++) {
            if (!estimate()) {
                release();
                return false;
            }
        }
        LimeLog.info("Depth model warmup done, "+runtimeLabel()+" inference "
                +String.format("%.1f", benchmark(interpreter, BENCHMARK_RUNS))
                +" ms avg over "+BENCHMARK_RUNS+" runs");

        if (gpuAccelerated && BENCHMARK_CPU) {
            benchmarkCpuForComparison();
        }
        return true;
    }

    /**
     * Builds the interpreter on the route's runtime, falling back to the CPU if the
     * GPU delegate rejects the model, and to the default path if the CPU delegate does.
     */
    private boolean buildInterpreter(Context context) {
        if (usesCpuDelegate(route) && buildOnCpuDelegate()) {
            return true;
        }

        Interpreter.Options options = new Interpreter.Options();
        if (route.gpu) {
            useGpu(context, options);
        }
        else {
            useCpu(options, DEFAULT_PATH_THREADS);
        }

        try {
            interpreter = new Interpreter(model, options);
        } catch (Exception e) {
            LimeLog.warning("Depth model failed to load on "+runtimeLabel()+": "+e.getMessage());
            releaseDelegate();
            try {
                Interpreter.Options cpuOptions = new Interpreter.Options();
                useCpu(cpuOptions, fallbackThreads(route));
                interpreter = new Interpreter(model, cpuOptions);
            } catch (Exception e2) {
                LimeLog.severe("Depth model failed to load: "+e2.getMessage());
                return false;
            }
        }
        return true;
    }

    static boolean usesCpuDelegate(Route route) {
        return !route.gpu && route.threads > 1;
    }

    /** Threads for the default path after the route's first choice failed. */
    static int fallbackThreads(Route route) {
        return route.gpu ? FALLBACK_CPU_THREADS : DEFAULT_PATH_THREADS;
    }

    /** Builds on the route's XNNPACK delegate; false, with nothing held, if that fails. */
    private boolean buildOnCpuDelegate() {
        cpuDelegate = XnnpackDelegate.create(route.threads);
        if (cpuDelegate == null) {
            LimeLog.warning("No XNNPACK delegate on "+threadCount(route.threads)
                    +", depth model on cpu ("+threadCount(DEFAULT_PATH_THREADS)+")");
            return false;
        }
        Interpreter.Options options = new Interpreter.Options();
        // For ops the delegate does not take
        options.setNumThreads(route.threads);
        // Instead of the default delegate, not alongside it
        options.setUseXNNPACK(false);
        options.addDelegate(cpuDelegate);
        try {
            interpreter = new Interpreter(model, options);
        } catch (Exception e) {
            LimeLog.warning("Depth model failed to load on an XNNPACK delegate on "
                    +threadCount(route.threads)+", trying cpu ("
                    +threadCount(DEFAULT_PATH_THREADS)+"): "+e.getMessage());
            releaseDelegate();
            return false;
        }
        gpuAccelerated = false;
        kernelCache = false;
        cpuThreads = route.threads;
        return true;
    }

    private void useGpu(Context context, Interpreter.Options options) {
        // The compatibility list is an allowlist of known devices, which headsets are
        // not on. Log it but try the delegate anyway; loading the model is the real test.
        CompatibilityList compatibility = new CompatibilityList();
        LimeLog.info("Depth model: GPU allowlist says "
                +compatibility.isDelegateSupportedOnThisDevice()+", trying the delegate anyway");

        try {
            GpuDelegateFactory.Options gpuOptions = new GpuDelegateFactory.Options();
            // fp16 math, matching the model's weights
            gpuOptions.setPrecisionLossAllowed(true);
            gpuOptions.setInferencePreference(
                    GpuDelegateFactory.Options.INFERENCE_PREFERENCE_SUSTAINED_SPEED);
            String cacheDir = kernelCacheDir(context);
            if (cacheDir != null) {
                gpuOptions.setSerializationParams(cacheDir,
                        kernelToken(route.asset, model.capacity()));
                kernelCache = true;
            }
            gpuDelegate = new GpuDelegate(gpuOptions);
            options.addDelegate(gpuDelegate);
            gpuAccelerated = true;
        } catch (Exception e) {
            LimeLog.warning("GPU delegate creation failed, using CPU: "+e.getMessage());
            useCpu(options, FALLBACK_CPU_THREADS);
        }
    }

    private void useCpu(Interpreter.Options options, int threads) {
        gpuAccelerated = false;
        kernelCache = false;
        cpuThreads = threads;
        options.setNumThreads(threads);
        // The default for float models, set explicitly. XNNPACK also has the int8
        // dynamic range kernels.
        options.setUseXNNPACK(true);
    }

    /**
     * Kernel cache key: asset name and size in bytes, so a changed asset never reuses old kernels.
     */
    static String kernelToken(String asset, int bytes) {
        return asset.substring(asset.lastIndexOf('/') + 1)+"-"+bytes;
    }

    // Null if the folder cannot be created; the kernels are then compiled each time
    private static String kernelCacheDir(Context context) {
        File dir = new File(context.getCacheDir(), KERNEL_CACHE_DIR);
        if (!dir.isDirectory() && !dir.mkdirs()) {
            LimeLog.warning("No depth kernel cache at "+dir);
            return null;
        }
        return dir.getAbsolutePath();
    }

    static boolean inputFits(int[] shape, DepthSize size) {
        return shape != null && shape.length == 4 && shape[0] == 1 && shape[1] == size.height
                && shape[2] == size.width && shape[3] == 3;
    }

    /**
     * Whether a model output is one value per pixel of this size, rows then columns.
     * Axes of 1 are ignored, so 1xHxW, 1xHxWx1 and 1x1xHxW all fit.
     */
    static boolean outputFits(int[] shape, DepthSize size) {
        if (shape == null) {
            return false;
        }
        int[] kept = new int[2];
        int count = 0;
        for (int dim : shape) {
            if (dim == 1) {
                continue;
            }
            if (count == 2) {
                return false;
            }
            kept[count++] = dim;
        }
        return count == 2 && kept[0] == size.height && kept[1] == size.width;
    }

    static boolean stagingFits(int inputBytes, int outputBytes, DepthSize size) {
        return inputBytes == size.pixels() * 3 * 4 && outputBytes == size.pixels() * 4;
    }

    private void benchmarkCpuForComparison() {
        Interpreter cpu = null;
        try {
            Interpreter.Options options = new Interpreter.Options();
            options.setNumThreads(FALLBACK_CPU_THREADS);
            cpu = new Interpreter(model, options);
            for (int i = 0; i < WARMUP_RUNS; i++) {
                input.rewind();
                output.rewind();
                cpu.run(input, output);
            }
            LimeLog.info("Depth model CPU reference: "
                    +String.format("%.1f", benchmark(cpu, CPU_BENCHMARK_RUNS))
                    +" ms avg over "+CPU_BENCHMARK_RUNS+" runs");
        } catch (Exception e) {
            LimeLog.warning("CPU reference benchmark failed: "+e.getMessage());
        } finally {
            if (cpu != null) {
                cpu.close();
            }
        }
    }

    private float benchmark(Interpreter target, int runs) {
        long total = 0;
        for (int i = 0; i < runs; i++) {
            long start = System.nanoTime();
            input.rewind();
            output.rewind();
            target.run(input, output);
            total += System.nanoTime() - start;
        }
        return total / (float)runs / 1000000.0f;
    }

    static String shapeToString(int[] shape) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < shape.length; i++) {
            sb.append(i == 0 ? "" : "x").append(shape[i]);
        }
        return sb.toString();
    }

    private static MappedByteBuffer loadModel(Context context, String asset) throws IOException {
        AssetFileDescriptor fd = context.getAssets().openFd(asset);
        try {
            FileInputStream stream = new FileInputStream(fd.getFileDescriptor());
            try {
                return stream.getChannel().map(FileChannel.MapMode.READ_ONLY,
                        fd.getStartOffset(), fd.getDeclaredLength());
            } finally {
                stream.close();
            }
        } finally {
            fd.close();
        }
    }

    private void usePair(int pair) {
        input = inputs[pair];
        output = outputs[pair];
    }

    /**
     * Runs the model on a pair. The renderer never writes a pair's buffers while the model has it.
     */
    @Override
    public boolean estimate(int pair) {
        if (inputs == null || pair < 0 || pair >= inputs.length) {
            return false;
        }
        usePair(pair);
        return estimate();
    }

    private boolean estimate() {
        if (interpreter == null) {
            return false;
        }
        long start = System.nanoTime();
        try {
            input.rewind();
            output.rewind();
            interpreter.run(input, output);
        } catch (Exception e) {
            LimeLog.severe("Depth inference failed: "+e.getMessage());
            return false;
        }
        lastInferenceNs = System.nanoTime() - start;
        return true;
    }

    @Override
    public float getLastInferenceMs() {
        return lastInferenceNs / 1000000.0f;
    }

    @Override
    public boolean isGpuAccelerated() {
        return gpuAccelerated;
    }

    /** "gpu", or "cpu" with its thread count, as actually running. */
    public String runtimeLabel() {
        return gpuAccelerated ? "gpu" : "cpu ("+threadCount(cpuThreads)+")";
    }

    static String threadCount(int threads) {
        return threads+(threads == 1 ? " thread" : " threads");
    }

    /** Model map and build time, including kernel compile or cache read. */
    public long getLoadMs() {
        return loadMs;
    }

    private void releaseDelegate() {
        if (gpuDelegate != null) {
            gpuDelegate.close();
            gpuDelegate = null;
        }
        if (cpuDelegate != null) {
            cpuDelegate.close();
            cpuDelegate = null;
        }
    }

    @Override
    public void release() {
        if (interpreter != null) {
            interpreter.close();
            interpreter = null;
        }
        releaseDelegate();
    }
}
