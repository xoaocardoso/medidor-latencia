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
    static final int N = 10;
    static final double GAP = 1.0, WIN = 0.9, CH = 0.01;

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
        out.setTextSize(16);
        out.setTextIsSelectable(true);
        out.setText("Nativo v1. Volume médio, carro em silêncio. 10 cliques, ~12 s.");
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
        out.setText("Tocando 10 cliques...");
        new Thread(() -> {
            String r;
            try {
                r = measure();
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

    static int parse(String s, int def) {
        try {
            return Integer.parseInt(s);
        } catch (Exception e) {
            return def;
        }
    }

    AudioRecord openRec(int sr, int bytes, int[] sources, String[] names, String[] used) {
        for (int i = 0; i < sources.length; i++) {
            try {
                AudioRecord r = new AudioRecord.Builder()
                        .setAudioSource(sources[i])
                        .setAudioFormat(new AudioFormat.Builder().setSampleRate(sr)
                                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                                .setChannelMask(AudioFormat.CHANNEL_IN_MONO).build())
                        .setBufferSizeInBytes(bytes).build();
                if (r.getState() == AudioRecord.STATE_INITIALIZED) {
                    used[0] = names[i];
                    return r;
                }
                r.release();
            } catch (Exception e) {
            }
        }
        throw new RuntimeException("microfone não abriu");
    }

    String measure() {
        AudioManager am = (AudioManager) getSystemService(AUDIO_SERVICE);
        int sr = parse(am.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE), 48000);
        int burst = parse(am.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER), 256);
        boolean unp = "true".equals(am.getProperty(AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED));
        int recMin = AudioRecord.getMinBufferSize(sr, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
        String[] used = new String[1];
        int[] srcs = unp
                ? new int[]{MediaRecorder.AudioSource.UNPROCESSED, MediaRecorder.AudioSource.VOICE_RECOGNITION, MediaRecorder.AudioSource.MIC}
                : new int[]{MediaRecorder.AudioSource.VOICE_RECOGNITION, MediaRecorder.AudioSource.MIC};
        String[] names = unp ? new String[]{"UNPROCESSED", "VOICE_RECOGNITION", "MIC"} : new String[]{"VOICE_RECOGNITION", "MIC"};
        AudioRecord rec = openRec(sr, Math.max(recMin, burst * 8), srcs, names, used);
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
        int trkFrames = trk.getBufferSizeInFrames();

        short[] chirp = makeChirp(sr);
        int block = burst;
        int total = (int) ((0.5 + N * GAP + 1.0) * sr);
        short[] rbuf = new short[total + block];
        int[] emit = new int[N];
        short[] in = new short[block];
        short[] ob = new short[block];
        int read = 0, k = 0, cpos = -1;

        rec.startRecording();
        trk.play();
        trk.write(new short[block], 0, block);
        while (read < total) {
            int n = rec.read(in, 0, block);
            if (n <= 0) throw new RuntimeException("leitura do mic falhou: " + n);
            System.arraycopy(in, 0, rbuf, read, Math.min(n, rbuf.length - read));
            read += n;
            Arrays.fill(ob, 0, n, (short) 0);
            if (cpos < 0 && k < N && read >= (int) ((0.5 + k * GAP) * sr)) {
                cpos = 0;
                emit[k] = read;
                k++;
            }
            if (cpos >= 0) {
                int m = Math.min(n, chirp.length - cpos);
                System.arraycopy(chirp, cpos, ob, 0, m);
                cpos += m;
                if (cpos >= chirp.length) cpos = -1;
            }
            trk.write(ob, 0, n);
        }
        int under = trk.getUnderrunCount();
        rec.stop();
        rec.release();
        trk.stop();
        trk.release();

        float[] r = new float[Math.min(read, rbuf.length)];
        for (int i = 0; i < r.length; i++) r[i] = rbuf[i] / 32768f;
        float[] t = new float[chirp.length];
        for (int i = 0; i < t.length; i++) t[i] = chirp[i] / 32768f;

        List<Double> ok = new ArrayList<>();
        StringBuilder list = new StringBuilder();
        for (int i = 0; i < k; i++) {
            int a = emit[i], b2 = emit[i] + (int) (WIN * sr);
            double[] h = find(r, t, a, b2);
            if (h != null && h[1] >= 10) {
                double ms = (h[0] - emit[i]) * 1000.0 / sr;
                ok.add(ms);
                list.append(String.format(Locale.US, "%.0f ", ms));
            } else list.append("x ");
        }
        StringBuilder s = new StringBuilder();
        s.append(String.format(Locale.US, "Nativo v1 | mic %s | %d Hz | bloco %d | saida %d frames (%.0f ms) | underruns %d\n",
                used[0], sr, burst, trkFrames, trkFrames * 1000.0 / sr, under));
        s.append("cliques ").append(ok.size()).append("/").append(k).append(": ").append(list).append("\n");
        if (ok.size() >= 3) {
            double sum = 0;
            for (double v : ok) sum += v;
            double avg = sum / ok.size(), var = 0;
            for (double v : ok) var += (v - avg) * (v - avg);
            List<Double> o = new ArrayList<>(ok);
            Collections.sort(o);
            s.append(String.format(Locale.US, "media %.1f | mediana %.1f | variacao %.1f | DP %.1f ms\n",
                    avg, o.get(o.size() / 2), o.get(o.size() - 1) - o.get(0), Math.sqrt(var / ok.size())));
        } else s.append("poucos cliques: suba o volume e repita\n");
        s.append(Build.MANUFACTURER).append(" ").append(Build.MODEL).append(" | Android ").append(Build.VERSION.RELEASE);
        return s.toString();
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
