package io.github.xoaocardoso.medidor;

import android.Manifest;
import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.pm.PackageManager;
import android.graphics.Typeface;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.AudioTrack;
import android.media.MediaRecorder;
import android.os.Build;
import android.os.Bundle;
import android.os.Process;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

public class MainActivity extends Activity {
    static final int N = 6;
    static final double GAP = 0.8, WIN = 0.75, CH = 0.01;

    static {
        System.loadLibrary("medidor");
    }

    native String aaudioRun(int exclusive, int bursts, short[] chirp, short[] rec, int[] emits, int[] info, double gap, int sampleRate);

    TextView out;
    Button go;
    String txt = "";
    volatile boolean running = false;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(32, 32, 32, 32);
        LinearLayout row = new LinearLayout(this);
        go = new Button(this);
        go.setText("Medir");
        go.setTextSize(22);
        Button cp = new Button(this);
        cp.setText("Copiar");
        cp.setTextSize(22);
        row.addView(go);
        row.addView(cp);
        out = new TextView(this);
        out.setTypeface(Typeface.MONOSPACE);
        out.setTextSize(15);
        out.setTextIsSelectable(true);
        out.setText("Nativo v2. Testa 6 jeitos de tocar e gravar. Volume médio, carro em silêncio, ~40 s.");
        ScrollView sv = new ScrollView(this);
        sv.addView(out);
        root.addView(row);
        root.addView(sv);
        setContentView(root);
        go.setOnClickListener(v -> start());
        cp.setOnClickListener(v -> {
            ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
            cm.setPrimaryClip(ClipData.newPlainText("latencia", txt));
            Toast.makeText(this, "Copiado", Toast.LENGTH_SHORT).show();
        });
    }

    void start() {
        if (running) return;
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, 1);
            return;
        }
        running = true;
        go.setEnabled(false);
        new Thread(() -> {
            Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
            String r;
            try {
                r = runAll();
            } catch (Throwable e) {
                r = "Erro: " + e;
            }
            final String f = r;
            runOnUiThread(() -> {
                txt = f;
                out.setText(f);
                go.setEnabled(true);
                running = false;
            });
        }).start();
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] p, int[] g) {
        if (g.length > 0 && g[0] == PackageManager.PERMISSION_GRANTED) start();
        else out.setText("Sem permissão do microfone.");
    }

    void status(String s) {
        runOnUiThread(() -> out.setText(s));
    }

    static int parse(String s, int def) {
        try {
            return Integer.parseInt(s);
        } catch (Exception e) {
            return def;
        }
    }

    String runAll() {
        AudioManager am = (AudioManager) getSystemService(AUDIO_SERVICE);
        int devSr = parse(am.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE), 48000);
        int burst = parse(am.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER), 256);
        StringBuilder s = new StringBuilder();
        s.append(String.format(Locale.US, "Nativo v2 | taxa do aparelho %d Hz | bloco %d | %s %s | Android %s\n",
                devSr, burst, Build.MANUFACTURER, Build.MODEL, Build.VERSION.RELEASE));
        String[] names = {"1 Java 44k1 REC (v1)", "2 Java 44k1 PERF+dreno", "3 Java 48k PERF+dreno",
                "4 AAudio compartilhado", "5 AAudio exclusivo", "6 AAudio exclusivo 1 bloco"};
        double best = 1e9;
        String bestName = "";
        for (int i = 0; i < names.length; i++) {
            status(s + "\nMedindo " + names[i] + "...");
            String line;
            double avg;
            try {
                double[] res = new double[2];
                switch (i) {
                    case 0: line = javaRun(44100, MediaRecorder.AudioSource.VOICE_RECOGNITION, burst, false, res); break;
                    case 1: line = javaRun(44100, MediaRecorder.AudioSource.VOICE_PERFORMANCE, burst, true, res); break;
                    case 2: line = javaRun(48000, MediaRecorder.AudioSource.VOICE_PERFORMANCE, burst, true, res); break;
                    case 3: line = aRun(0, 2, res); break;
                    case 4: line = aRun(1, 2, res); break;
                    default: line = aRun(1, 1, res); break;
                }
                avg = res[0];
            } catch (Throwable e) {
                line = "falhou: " + e.getMessage();
                avg = 1e9;
            }
            s.append(names[i]).append(" | ").append(line).append("\n");
            if (avg < best) {
                best = avg;
                bestName = names[i];
            }
        }
        if (best < 1e8) s.append(String.format(Locale.US, "MELHOR: %s = %.1f ms", bestName, best));
        return s.toString();
    }

    String javaRun(int sr, int source, int burst, boolean drain, double[] res) {
        int recMin = AudioRecord.getMinBufferSize(sr, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
        AudioRecord rec = new AudioRecord.Builder()
                .setAudioSource(source)
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(sr)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setChannelMask(AudioFormat.CHANNEL_IN_MONO).build())
                .setBufferSizeInBytes(Math.max(recMin, burst * 8)).build();
        if (rec.getState() != AudioRecord.STATE_INITIALIZED) {
            rec.release();
            throw new RuntimeException("mic não abriu");
        }
        int trkMin = AudioTrack.getMinBufferSize(sr, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT);
        AudioTrack trk = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(sr)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
                .setBufferSizeInBytes(trkMin)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                .build();
        trk.setBufferSizeInFrames(burst * 2);

        short[] chirp = makeChirp(sr);
        int block = burst;
        int total = (int) ((0.5 + N * GAP + 1.0) * sr);
        short[] rbuf = new short[total + 16384];
        int[] emit = new int[N];
        short[] in = new short[block];
        short[] ob = new short[block];
        int read = 0, k = 0, cpos = -1;

        rec.startRecording();
        trk.play();
        trk.write(new short[block], 0, block);
        while (read < total) {
            int n = rec.read(in, 0, block);
            if (n <= 0) throw new RuntimeException("leitura falhou " + n);
            int c = Math.min(n, rbuf.length - read);
            System.arraycopy(in, 0, rbuf, read, c);
            read += c;
            if (drain) {
                int m;
                while ((m = rec.read(in, 0, block, AudioRecord.READ_NON_BLOCKING)) > 0) {
                    c = Math.min(m, rbuf.length - read);
                    System.arraycopy(in, 0, rbuf, read, c);
                    read += c;
                    if (read >= rbuf.length) break;
                }
            }
            Arrays.fill(ob, (short) 0);
            if (cpos < 0 && k < N && read >= (int) ((0.5 + k * GAP) * sr)) {
                cpos = 0;
                emit[k++] = read;
            }
            if (cpos >= 0) {
                int m = Math.min(block, chirp.length - cpos);
                System.arraycopy(chirp, cpos, ob, 0, m);
                cpos += m;
                if (cpos >= chirp.length) cpos = -1;
            }
            trk.write(ob, 0, block);
        }
        int under = trk.getUnderrunCount();
        int frames = trk.getBufferSizeInFrames();
        rec.stop();
        rec.release();
        trk.stop();
        trk.release();
        return summarize(rbuf, Math.min(read, rbuf.length), emit, k, chirp, sr, res,
                String.format(Locale.US, "%d Hz | saida %d fr | underruns %d", sr, frames, under));
    }

    String aRun(int exclusive, int bursts, double[] res) {
        int sr0 = 48000;
        short[] chirp = makeChirp(sr0);
        int total = (int) ((0.5 + N * GAP + 1.0) * sr0);
        short[] rec = new short[total];
        int[] emits = new int[N];
        int[] info = new int[6];
        String err = aaudioRun(exclusive, bursts, chirp, rec, emits, info, GAP, 0);
        if (err != null) throw new RuntimeException(err);
        int sr = info[0];
        if (sr != sr0) {
            chirp = makeChirp(sr);
            total = (int) ((0.5 + N * GAP + 1.0) * sr);
            rec = new short[total];
            emits = new int[N];
            err = aaudioRun(exclusive, bursts, chirp, rec, emits, info, GAP, 0);
            if (err != null) throw new RuntimeException(err);
        }
        int k = 0;
        while (k < N && emits[k] > 0) k++;
        return summarize(rec, rec.length, emits, k, chirp, sr, res,
                String.format(Locale.US, "%d Hz | bloco %d | saida %d fr | exclusivo saida %s entrada %s | xruns %d",
                        sr, info[1], info[2], info[3] == 1 ? "sim" : "nao", info[4] == 1 ? "sim" : "nao", info[5]));
    }

    String summarize(short[] rbuf, int len, int[] emit, int k, short[] chirp, int sr, double[] res, String head) {
        float[] r = new float[len];
        for (int i = 0; i < len; i++) r[i] = rbuf[i] / 32768f;
        float[] t = new float[chirp.length];
        for (int i = 0; i < t.length; i++) t[i] = chirp[i] / 32768f;
        List<Double> ok = new ArrayList<>();
        StringBuilder list = new StringBuilder();
        for (int i = 0; i < k; i++) {
            double[] h = find(r, t, emit[i], emit[i] + (int) (WIN * sr));
            if (h != null && h[1] >= 10) {
                double ms = (h[0] - emit[i]) * 1000.0 / sr;
                ok.add(ms);
                list.append(String.format(Locale.US, "%.0f ", ms));
            } else list.append("x ");
        }
        res[0] = 1e9;
        if (ok.size() >= 3) {
            double sum = 0;
            for (double v : ok) sum += v;
            double avg = sum / ok.size();
            List<Double> o = new ArrayList<>(ok);
            Collections.sort(o);
            res[0] = avg;
            return String.format(Locale.US, "%s | MEDIA %.1f ms | var %.1f | %d/%d: %s",
                    head, avg, o.get(o.size() - 1) - o.get(0), ok.size(), k, list);
        }
        return head + " | poucos cliques " + ok.size() + "/" + k + ": " + list;
    }

    static short[] makeChirp(int sr) {
        int n = (int) Math.round(CH * sr);
        short[] o = new short[n];
        for (int i = 0; i < n; i++) {
            double tt = (double) i / sr;
            double w = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / (n - 1));
            double v = w * Math.sin(2 * Math.PI * (800 * tt + 4200 * tt * tt / (2 * CH)));
            o[i] = (short) Math.round(v * 0.8 * 32767);
        }
        return o;
    }

    static double[] find(float[] r, float[] t, int a, int b) {
        b = Math.min(b, r.length - t.length);
        if (a < 1 || b <= a) return null;
        int B = 96;
        List<Double> e = new ArrayList<>();
        for (int i = a; i + B <= b; i += B) {
            double s = 0;
            for (int j = i; j < i + B; j++) {
                double d = r[j] - r[j - 1];
                s += d * d;
            }
            e.add(s);
        }
        if (e.isEmpty()) return null;
        int m = 0;
        for (int i = 1; i < e.size(); i++) if (e.get(i) > e.get(m)) m = i;
        List<Double> sorted = new ArrayList<>(e);
        Collections.sort(sorted);
        double med = Math.max(sorted.get(sorted.size() / 2), 1e-12);
        int c0 = Math.max(a, a + m * B - 960), c1 = Math.min(b, a + m * B + 960);
        double best = -1;
        int bi = c0;
        for (int i = c0; i < c1; i++) {
            double v = 0;
            for (int j = 0; j < t.length; j++) v += r[i + j] * t[j];
            v = Math.abs(v);
            if (v > best) {
                best = v;
                bi = i;
            }
        }
        return new double[]{bi, e.get(m) / med};
    }
}
