package io.github.xoaocardoso.medidor;

import android.Manifest;
import android.app.Activity;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.Locale;

public class MainActivity extends Activity {
    static {
        System.loadLibrary("medidor");
    }

    native String ancOpen();
    native void ancClose();
    native void ancCmd(int cmd, int val);
    native double[] ancService();
    native float[] ancGetCal();
    native void ancSetCal(float[] cal);

    static final String[] STATES = {"parado", "medindo latência (cliques)", "calculando latência", "calibrando (tons)", "rodando", "erro: cliques não detectados, suba o volume e tente de novo"};

    TextView out;
    Button anc, ab;
    boolean ancOn = false, abOn = false, opened = false;
    final Handler h = new Handler(Looper.getMainLooper());
    SharedPreferences prefs;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        prefs = getSharedPreferences("anc", MODE_PRIVATE);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(32, 32, 32, 32);
        LinearLayout row = new LinearLayout(this);
        Button cal = button("Calibrar");
        anc = button("ANC: desligado");
        ab = button("Teste A/B: desligado");
        row.addView(cal);
        row.addView(anc);
        row.addView(ab);
        out = new TextView(this);
        out.setTypeface(Typeface.MONOSPACE);
        out.setTextSize(20);
        root.addView(row);
        root.addView(out);
        setContentView(root);
        cal.setOnClickListener(v -> {
            if (ready()) ancCmd(1, 0);
        });
        anc.setOnClickListener(v -> {
            if (!ready()) return;
            ancOn = !ancOn;
            ancCmd(2, ancOn ? 1 : 0);
            anc.setText(ancOn ? "ANC: LIGADO" : "ANC: desligado");
        });
        ab.setOnClickListener(v -> {
            if (!ready()) return;
            abOn = !abOn;
            ancCmd(3, abOn ? 1 : 0);
            ab.setText(abOn ? "Teste A/B: rodando" : "Teste A/B: desligado");
        });
        out.setText("Calibrar: uma vez, com o motor DESLIGADO, carro em silêncio e o volume no nível que vai usar (~10 s de tons).\nDepois: ligue o carro, toque em ANC e dirija.\nTeste A/B: alterna ANC 3 s ligado / 3 s desligado e mede a diferença sozinho.");
        ready();
    }

    Button button(String t) {
        Button b = new Button(this);
        b.setText(t);
        b.setTextSize(18);
        return b;
    }

    boolean ready() {
        if (opened) return true;
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, 1);
            return false;
        }
        String err = ancOpen();
        if (err != null) {
            out.setText("Erro ao abrir o áudio: " + err);
            return false;
        }
        opened = true;
        float[] cal = loadCal();
        if (cal != null) ancSetCal(cal);
        h.post(tick);
        return true;
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] p, int[] g) {
        if (g.length > 0 && g[0] == PackageManager.PERMISSION_GRANTED) ready();
        else out.setText("Sem permissão do microfone.");
    }

    final Runnable tick = new Runnable() {
        @Override
        public void run() {
            if (!opened) return;
            double[] v = ancService();
            float[] cal = ancGetCal();
            if (cal != null) saveCal(cal);
            int st = (int) v[0];
            StringBuilder s = new StringBuilder();
            s.append("Estado: ").append(STATES[Math.max(0, Math.min(st, STATES.length - 1))]);
            if (st == 3) s.append(String.format(Locale.US, " %.0f%%", v[15] * 100));
            s.append(v[14] > 0 ? " | calibrado" : " | SEM calibração").append("\n");
            boolean locked = v[2] > 0.7 && v[3] > 0.15;
            s.append(String.format(Locale.US, "Drone: %.1f Hz (%.0f rpm) %s\n", v[1], v[1] * 30, locked ? "TRAVADO" : "procurando"));
            s.append(String.format(Locale.US, "Drone estimado: %.1f dB | no microfone: %.1f dB\n", v[4], v[5]));
            if (v[7] > 0) s.append(String.format(Locale.US, "ANC agindo: redução estimada %.1f dB\n", v[4] - v[5]));
            else s.append("ANC parado\n");
            s.append(String.format(Locale.US, "Latência real: %s | buffer %.0f × %.1f ms | relógio %s\n",
                    v[16] > 0 ? String.format(Locale.US, "%.0f ms", v[16]) : "ainda não medida", v[17], v[19], v[18] > 0 ? "ok" : "sem"));
            s.append(String.format(Locale.US, "Engasgos (xruns): %.0f | resets do limitador: %.0f\n", v[12], v[13]));
            if (v[8] > 0) {
                if (v[11] > 0) s.append(String.format(Locale.US, "A/B: desligado %.1f dB × ligado %.1f dB → REDUÇÃO MEDIDA %.1f dB (n=%.0f)\n", v[10], v[9], v[10] - v[9], v[11]));
                else s.append("A/B: juntando dados com o drone travado...\n");
            }
            out.setText(s.toString());
            h.postDelayed(this, 200);
        }
    };

    void saveCal(float[] c) {
        StringBuilder s = new StringBuilder();
        for (float f : c) s.append(f).append(',');
        prefs.edit().putString("cal", s.toString()).apply();
    }

    float[] loadCal() {
        String s = prefs.getString("cal", null);
        if (s == null) return null;
        String[] p = s.split(",");
        float[] c = new float[p.length];
        try {
            for (int i = 0; i < p.length; i++) c[i] = Float.parseFloat(p[i]);
        } catch (Exception e) {
            return null;
        }
        return c;
    }

    @Override
    protected void onDestroy() {
        h.removeCallbacks(tick);
        if (opened) ancClose();
        opened = false;
        super.onDestroy();
    }
}
