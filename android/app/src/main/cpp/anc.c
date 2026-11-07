#include <jni.h>
#include <aaudio/AAudio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RING 16384
#define MASK (RING - 1)
#define NCAL 25
#define CAL_F0 60.0
#define CAL_DF 5.0
#define CAL_AMP 0.3
#define DEC 21
#define DRING 1024
#define NCHIRP 4
#define TWO_PI 6.283185307179586

enum { ST_IDLE, ST_DMEAS, ST_DWAIT, ST_CAL, ST_RUN, ST_ERR };

static AAudioStream *gOut, *gIn;
static int gSr;
static volatile int gState = ST_IDLE;
static volatile int gNext = ST_RUN;
static volatile int gAnc = 0, gAB = 0, gReset = 0;
static long long oCount, inCount;
static volatile int gK = -1;
static float cm[RING], hc[RING], hs[RING];
static signed char tk[RING];
static int16_t tmpIn[8192];

static float chirpT[1024];
static int chirpLen;
static float *rec;
static int recCap;
static long long recStart;
static long long emitO[NCHIRP], emitN[NCHIRP];
static int nEmit, chirpPos = -1;
static long long dStartO;

static double calRe[NCAL], calIm[NCAL];
static int calCnt[NCAL];
static float Gre[NCAL], Gim[NCAL];
static volatile int gHasCal = 0, gCalNew = 0;
static long long calStartO, calEndO;
static int toneLen;
static double calPh;

static double pRe = 1, pIm = 0, w, alpha;
static double Zr, Zi, Er, Ei, P, lockQ;
static double Wr, Wi;
static double BETA, BETA2, BETA3, KP, KI;
static float dec[DRING];
static int decPos, decCnt;
static double bq[2][5], bz[2][2];
static int cooldown, badCnt, resets, lastXruns;
static double abOn, abOff;
static int abOnN, abOffN;
static long long abT0;
static volatile int gActive;
static volatile double gLms = -1;
static double gC, kHist[5];
static int kHistN, gTsOk, burstF, bufBursts, xrWindow;
static long long xrT;
static double cwB = 1, swB = 0, WpR, WpI, pOR = 1, pOI = 0;
static int gOutCh = 1, gInCh = 1, gExO, gExI;

static inline void put(int16_t *out, int j, double v) {
    int16_t s = (int16_t) (v * 32767);
    for (int c = 0; c < gOutCh; c++) out[j * gOutCh + c] = s;
}

static int kts(double *out) {
    int64_t fpO, tO, fpI, tI;
    if (AAudioStream_getTimestamp(gOut, CLOCK_MONOTONIC, &fpO, &tO) != AAUDIO_OK) return 0;
    if (AAudioStream_getTimestamp(gIn, CLOCK_MONOTONIC, &fpI, &tI) != AAUDIO_OK) return 0;
    *out = (double) fpI + (double) (tO - tI) * gSr / 1e9 - (double) fpO;
    return 1;
}

static double biq(int s, double x) {
    double *c = bq[s];
    double y = c[0] * x + bz[s][0];
    bz[s][0] = c[1] * x - c[3] * y + bz[s][1];
    bz[s][1] = c[2] * x - c[4] * y;
    return y;
}

static void lowpass(int s, double fc) {
    double k = tan(M_PI * fc / gSr), q = 0.7071;
    double n = 1 / (1 + k / q + k * k);
    bq[s][0] = k * k * n;
    bq[s][1] = 2 * bq[s][0];
    bq[s][2] = bq[s][0];
    bq[s][3] = 2 * (k * k - 1) * n;
    bq[s][4] = (1 - k / q + k * k) * n;
    bz[s][0] = bz[s][1] = 0;
}

static void getG(double f, double *gr, double *gi) {
    double x = (f - CAL_F0) / CAL_DF;
    if (x < 0) x = 0;
    if (x > NCAL - 1) x = NCAL - 1;
    int i = (int) x;
    if (i >= NCAL - 1) i = NCAL - 2;
    double t = x - i;
    *gr = Gre[i] * (1 - t) + Gre[i + 1] * t;
    *gi = Gim[i] * (1 - t) + Gim[i + 1] * t;
}

static void resetTracker(void) {
    pRe = 1;
    pIm = 0;
    w = TWO_PI * 120 / gSr;
    alpha = 0;
    Zr = Zi = Er = Ei = 0;
    P = 1e-9;
    lockQ = 0;
    Wr = Wi = 0;
    WpR = WpI = 0;
    badCnt = 0;
    cooldown = 0;
}

static void inSample(double e) {
    long long m = inCount;
    int idx = (int) (m & MASK);
    double c = cm[idx];
    int k = tk[idx];
    double h1 = hc[idx], h2 = hs[idx];
    cm[idx] = 0;
    tk[idx] = 0;
    if (gState == ST_DMEAS && rec && m >= recStart && m - recStart < recCap) rec[m - recStart] = (float) e;
    if (gState == ST_CAL && k > 0) {
        calRe[k - 1] += e * h1;
        calIm[k - 1] -= e * h2;
        calCnt[k - 1]++;
    }
    double d = e - c;
    double y = biq(1, biq(0, d));
    if (++decCnt >= DEC) {
        decCnt = 0;
        dec[decPos] = (float) y;
        decPos = (decPos + 1) & (DRING - 1);
    }
    Zr += BETA * (d * pRe - Zr);
    Zi += BETA * (-d * pIm - Zi);
    Er += BETA * (e * pRe - Er);
    Ei += BETA * (-e * pIm - Ei);
    P += BETA2 * (y * y - P);
    double mag = sqrt(Zr * Zr + Zi * Zi) + 1e-12;
    double err = Zi / mag;
    lockQ += BETA3 * (Zr / mag - lockQ);
    w += KI * err;
    double wmin = TWO_PI * 50 / gSr, wmax = TWO_PI * 200 / gSr;
    if (w < wmin) w = wmin;
    if (w > wmax) w = wmax;
    alpha += BETA3 * (KI * err - alpha);
    double a = KP * err, cr = cwB - swB * a, sr = swB + cwB * a;
    double nr = pRe * cr - pIm * sr;
    pIm = pRe * sr + pIm * cr;
    pRe = nr;
    inCount++;
}

static void startDmeas(void) {
    memset(cm, 0, sizeof(cm));
    memset(tk, 0, sizeof(tk));
    nEmit = 0;
    chirpPos = -1;
    recStart = inCount;
    dStartO = oCount + gSr / 4;
    gState = ST_DMEAS;
}

static void startCal(void) {
    memset(calRe, 0, sizeof(calRe));
    memset(calIm, 0, sizeof(calIm));
    memset(calCnt, 0, sizeof(calCnt));
    toneLen = (int) (0.3 * gSr);
    calStartO = oCount + gSr / 5;
    calEndO = calStartO + (long long) toneLen * NCAL;
    calPh = 0;
    gState = ST_CAL;
}

static void startRun(void) {
    memset(cm, 0, sizeof(cm));
    resetTracker();
    gState = ST_RUN;
}

static void finishCal(void) {
    for (int k = 0; k < NCAL; k++) {
        if (calCnt[k] > 0) {
            Gre[k] = (float) (2 * calRe[k] / (CAL_AMP * calCnt[k]));
            Gim[k] = (float) (2 * calIm[k] / (CAL_AMP * calCnt[k]));
        }
    }
    gHasCal = 1;
    gCalNew = 1;
    startRun();
}

static aaudio_data_callback_result_t cb(AAudioStream *s, void *u, void *data, int32_t frames) {
    int16_t *out = (int16_t *) data;
    cwB = cos(w);
    swB = sin(w);
    for (;;) {
        int cap = 8192 / gInCh;
        aaudio_result_t got = AAudioStream_read(gIn, tmpIn, cap, 0);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) inSample(tmpIn[i * gInCh] / 32768.0);
        if (got < cap) break;
    }
    if (gReset) {
        gReset = 0;
        startDmeas();
    }
    int st = gState;
    if (st == ST_DMEAS) {
        for (int j = 0; j < frames; j++) {
            double v = 0;
            if (chirpPos < 0 && nEmit < NCHIRP && oCount >= dStartO + (long long) nEmit * (long long) (0.6 * gSr)) {
                emitO[nEmit] = oCount;
                emitN[nEmit] = inCount;
                nEmit++;
                chirpPos = 0;
            }
            if (chirpPos >= 0) {
                v = chirpT[chirpPos++];
                if (chirpPos >= chirpLen) chirpPos = -1;
            }
            put(out, j, v * 0.8);
            oCount++;
        }
        if (nEmit == NCHIRP && chirpPos < 0 && inCount > emitN[NCHIRP - 1] + (long long) (0.6 * gSr)) gState = ST_DWAIT;
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    if (st == ST_CAL) {
        int K = gK;
        for (int j = 0; j < frames; j++) {
            double v = 0;
            long long rel = oCount - calStartO;
            if (rel >= 0 && oCount < calEndO) {
                int k = (int) (rel / toneLen), pos = (int) (rel % toneLen);
                double f = CAL_F0 + CAL_DF * k;
                calPh += TWO_PI * f / gSr;
                if (calPh > TWO_PI) calPh -= TWO_PI;
                double fade = (double) gSr * 0.01, env = 1;
                if (pos < fade) env = pos / fade;
                if (toneLen - pos < fade) env = (toneLen - pos) / fade;
                double c = cos(calPh), sn = sin(calPh);
                v = CAL_AMP * env * c;
                int idx = (int) ((oCount + K) & MASK);
                hc[idx] = (float) c;
                hs[idx] = (float) sn;
                tk[idx] = (pos > 0.12 * gSr && pos < toneLen - fade) ? (signed char) (k + 1) : 0;
            }
            put(out, j, v);
            oCount++;
        }
        if (inCount > calEndO + K + gSr / 20) finishCal();
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    if (st != ST_RUN) {
        memset(out, 0, (size_t) frames * gOutCh * sizeof(int16_t));
        oCount += frames;
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    int K = gK;
    double f = w * gSr / TWO_PI;
    double mag = sqrt(Zr * Zr + Zi * Zi), emag = sqrt(Er * Er + Ei * Ei);
    double tone = 2 * mag * mag / (P + 1e-12);
    int locked = lockQ > 0.7 && tone > 0.15 && f > 62 && f < 178;
    int abPhaseOn = 1;
    if (gAB) {
        long long t = oCount - abT0, half = (long long) gSr * 3;
        abPhaseOn = ((t / half) % 2) == 0;
        long long pos = t % half;
        if (locked && pos > gSr) {
            if (abPhaseOn) {
                abOn += emag;
                abOnN++;
            } else {
                abOff += emag;
                abOffN++;
            }
        }
    }
    double gr = 0, gi = 0;
    getG(f, &gr, &gi);
    int active = gAnc && gHasCal && locked && cooldown <= 0 && abPhaseOn;
    gActive = active;
    if (active) {
        double den = gr * gr + gi * gi + 1e-12;
        double tr = -2 * (Zr * gr + Zi * gi) / den, ti = -2 * (Zi * gr - Zr * gi) / den;
        double mu = frames / (gSr * 0.1);
        Wr += mu * (tr - Wr);
        Wi += mu * (ti - Wi);
        double wm = sqrt(Wr * Wr + Wi * Wi);
        if (wm > 0.7) {
            Wr *= 0.7 / wm;
            Wi *= 0.7 / wm;
        }
        if (emag > 1.4 * mag) badCnt += frames;
        else badCnt = badCnt > frames ? badCnt - frames : 0;
        if (badCnt > 0.4 * gSr) {
            Wr = Wi = 0;
            cooldown = (int) (1.5 * gSr);
            badCnt = 0;
            resets++;
        }
    } else {
        Wr *= 0.9;
        Wi *= 0.9;
    }
    if (cooldown > 0) cooldown -= frames;

    double h0 = (double) (oCount + K - inCount);
    double tgt = atan2(pIm, pRe) + w * h0 + 0.5 * alpha * h0 * h0;
    double pe = remainder(tgt - atan2(pOI, pOR), TWO_PI);
    double wr = w + alpha * h0 + pe / frames;
    int silent = (Wr * Wr + Wi * Wi) < 1e-8 && (WpR * WpR + WpI * WpI) < 1e-8;
    if (silent || fabs(pe) > 2.5) {
        pOR = cos(tgt);
        pOI = sin(tgt);
        wr = w + alpha * h0;
    }
    double c = pOR, sn = pOI, rc = cos(wr), rs = sin(wr);
    for (int j = 0; j < frames; j++) {
        double t = (double) (j + 1) / frames;
        double wr2 = WpR + (Wr - WpR) * t, wi2 = WpI + (Wi - WpI) * t;
        double y = wr2 * c - wi2 * sn;
        if (y > 0.9) y = 0.9;
        if (y < -0.9) y = -0.9;
        put(out, j, y);
        double gwr = gr * wr2 - gi * wi2, gwi = gr * wi2 + gi * wr2;
        cm[(int) ((oCount + K) & MASK)] = (float) (gwr * c - gwi * sn);
        double nc = c * rc - sn * rs;
        sn = c * rs + sn * rc;
        c = nc;
        oCount++;
    }
    WpR = Wr;
    WpI = Wi;
    double on = sqrt(c * c + sn * sn);
    pOR = c / on;
    pOI = sn / on;
    double n = sqrt(pRe * pRe + pIm * pIm);
    pRe /= n;
    pIm /= n;
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static int findChirp(const float *r, int len, int a, int b) {
    if (b > len - chirpLen) b = len - chirpLen;
    if (a < 1 || b <= a) return -1;
    int B = 96, nb = (b - a) / B;
    if (nb < 4) return -1;
    double *e = malloc(nb * sizeof(double)), *s = malloc(nb * sizeof(double));
    int m = 0;
    for (int i = 0; i < nb; i++) {
        double acc = 0;
        for (int j = a + i * B; j < a + i * B + B; j++) {
            double d = r[j] - r[j - 1];
            acc += d * d;
        }
        e[i] = s[i] = acc;
        if (acc > e[m]) m = i;
    }
    for (int i = 1; i < nb; i++) {
        double v = s[i];
        int j = i - 1;
        while (j >= 0 && s[j] > v) {
            s[j + 1] = s[j];
            j--;
        }
        s[j + 1] = v;
    }
    double ratio = e[m] / (s[nb / 2] + 1e-12);
    free(e);
    free(s);
    if (ratio < 10) return -1;
    int c0 = a + m * B - 960, c1 = a + m * B + 960;
    if (c0 < a) c0 = a;
    if (c1 > b) c1 = b;
    double best = -1;
    int bi = c0;
    for (int i = c0; i < c1; i++) {
        double v = 0;
        for (int j = 0; j < chirpLen; j++) v += r[i + j] * chirpT[j];
        v = fabs(v);
        if (v > best) {
            best = v;
            bi = i;
        }
    }
    return bi;
}

static void coarseSearch(void) {
    int n = 840;
    double fs = (double) gSr / DEC, best = 0, bestF = 0, mags[241];
    float buf[840];
    for (int i = 0; i < n; i++) {
        double win = 0.5 - 0.5 * cos(TWO_PI * i / (n - 1));
        buf[i] = (float) (dec[(decPos - n + i) & (DRING - 1)] * win);
    }
    for (int k = 0; k < 241; k++) {
        double f = 60 + 0.5 * k, co = 2 * cos(TWO_PI * f / fs), s1 = 0, s2 = 0;
        for (int i = 0; i < n; i++) {
            double s0 = buf[i] + co * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        mags[k] = s1 * s1 + s2 * s2 - co * s1 * s2;
        if (mags[k] > best) {
            best = mags[k];
            bestF = f;
        }
    }
    double sorted[241];
    memcpy(sorted, mags, sizeof(sorted));
    for (int i = 1; i < 241; i++) {
        double v = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > v) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = v;
    }
    if (best > 6 * sorted[120]) w = TWO_PI * bestF / gSr;
}

JNIEXPORT jstring JNICALL Java_io_github_xoaocardoso_medidor_MainActivity_ancOpen(JNIEnv *env, jobject t) {
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return (*env)->NewStringUTF(env, "builder falhou");
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 1);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_MEDIA);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_EXCLUSIVE);
    AAudioStreamBuilder_setDataCallback(b, cb, NULL);
    if (AAudioStreamBuilder_openStream(b, &gOut) != AAUDIO_OK) {
        AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
        if (AAudioStreamBuilder_openStream(b, &gOut) != AAUDIO_OK) {
            AAudioStreamBuilder_delete(b);
            return (*env)->NewStringUTF(env, "saida nao abriu");
        }
    }
    AAudioStreamBuilder_delete(b);
    gSr = AAudioStream_getSampleRate(gOut);
    gOutCh = AAudioStream_getChannelCount(gOut);
    if (gOutCh < 1) gOutCh = 1;
    gExO = AAudioStream_getSharingMode(gOut) == AAUDIO_SHARING_MODE_EXCLUSIVE;
    AAudio_createStreamBuilder(&b);
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 1);
    AAudioStreamBuilder_setSampleRate(b, gSr);
    AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_PERFORMANCE);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_EXCLUSIVE);
    if (AAudioStreamBuilder_openStream(b, &gIn) != AAUDIO_OK) {
        AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
        if (AAudioStreamBuilder_openStream(b, &gIn) != AAUDIO_OK) {
            AAudioStreamBuilder_delete(b);
            AAudioStream_close(gOut);
            return (*env)->NewStringUTF(env, "entrada nao abriu");
        }
    }
    gInCh = AAudioStream_getChannelCount(gIn);
    if (gInCh < 1) gInCh = 1;
    gExI = AAudioStream_getSharingMode(gIn) == AAUDIO_SHARING_MODE_EXCLUSIVE;
    AAudioStreamBuilder_delete(b);
    burstF = AAudioStream_getFramesPerBurst(gOut);
    bufBursts = 2;
    AAudioStream_setBufferSizeInFrames(gOut, burstF * bufBursts);
    xrT = 0;
    xrWindow = 0;
    gLms = -1;
    gTsOk = 0;

    chirpLen = (int) (0.01 * gSr);
    for (int i = 0; i < chirpLen; i++) {
        double tt = (double) i / gSr, wn = 0.5 - 0.5 * cos(TWO_PI * i / (chirpLen - 1));
        chirpT[i] = (float) (wn * sin(TWO_PI * (800 * tt + 4200 * tt * tt / 0.02)));
    }
    recCap = gSr * 3;
    rec = calloc(recCap, sizeof(float));
    BETA = 1 - exp(-TWO_PI * 40.0 / gSr);
    BETA2 = 1 - exp(-TWO_PI * 5.0 / gSr);
    BETA3 = 1 - exp(-TWO_PI * 3.0 / gSr);
    double wn = TWO_PI * 4.0;
    KP = 2 * 0.7 * wn / gSr;
    KI = (wn / gSr) * (wn / gSr);
    lowpass(0, 250);
    lowpass(1, 250);
    oCount = inCount = 0;
    gK = -1;
    resets = 0;
    resetTracker();
    gState = ST_IDLE;
    AAudioStream_requestStart(gIn);
    AAudioStream_requestStart(gOut);
    lastXruns = AAudioStream_getXRunCount(gOut);
    return NULL;
}

JNIEXPORT void JNICALL Java_io_github_xoaocardoso_medidor_MainActivity_ancClose(JNIEnv *env, jobject t) {
    if (gOut) {
        AAudioStream_requestStop(gOut);
        AAudioStream_close(gOut);
        gOut = NULL;
    }
    if (gIn) {
        AAudioStream_requestStop(gIn);
        AAudioStream_close(gIn);
        gIn = NULL;
    }
    free(rec);
    rec = NULL;
    gState = ST_IDLE;
}

JNIEXPORT void JNICALL Java_io_github_xoaocardoso_medidor_MainActivity_ancCmd(JNIEnv *env, jobject t, jint cmd, jint val) {
    if (cmd == 1) {
        gNext = ST_CAL;
        gReset = 1;
    } else if (cmd == 2) {
        gAnc = val;
        if (val && (gK < 0 || gState == ST_IDLE || gState == ST_ERR)) {
            gNext = ST_RUN;
            gReset = 1;
        }
    } else if (cmd == 3) {
        abOn = abOff = 0;
        abOnN = abOffN = 0;
        abT0 = oCount;
        gAB = val;
    }
}

JNIEXPORT jdoubleArray JNICALL Java_io_github_xoaocardoso_medidor_MainActivity_ancService(JNIEnv *env, jobject t) {
    if (gState == ST_DWAIT) {
        int ks[NCHIRP], ls[NCHIRP], n = 0;
        for (int i = 0; i < NCHIRP; i++) {
            int a = (int) (emitN[i] - recStart), m = findChirp(rec, recCap, a, a + (int) (0.45 * gSr));
            if (m >= 0) {
                ks[n] = (int) (m + recStart - emitO[i]);
                ls[n] = (int) (m + recStart - emitN[i]);
                n++;
            }
        }
        if (n >= 2) {
            for (int i = 1; i < n; i++) {
                int v = ks[i], u = ls[i], j = i - 1;
                while (j >= 0 && ks[j] > v) {
                    ks[j + 1] = ks[j];
                    ls[j + 1] = ls[j];
                    j--;
                }
                ks[j + 1] = v;
                ls[j + 1] = u;
            }
            gK = ks[n / 2];
            gLms = ls[n / 2] * 1000.0 / gSr;
            double kt;
            gTsOk = kts(&kt);
            if (gTsOk) gC = gK - kt;
            kHistN = 0;
            if (gNext == ST_CAL) startCal();
            else startRun();
        } else gState = ST_ERR;
    }
    if (gState == ST_RUN) {
        if (lockQ < 0.5) coarseSearch();
        double kt;
        if (gTsOk && kts(&kt)) {
            kHist[kHistN % 5] = kt + gC;
            kHistN++;
            if (kHistN >= 5) {
                double s[5];
                memcpy(s, kHist, sizeof(s));
                for (int i = 1; i < 5; i++) {
                    double v = s[i];
                    int j = i - 1;
                    while (j >= 0 && s[j] > v) {
                        s[j + 1] = s[j];
                        j--;
                    }
                    s[j + 1] = v;
                }
                int nk = (int) lround(s[2]);
                if (abs(nk - gK) > gSr / 1000) {
                    gLms += (nk - gK) * 1000.0 / gSr;
                    gK = nk;
                }
            }
        }
        int xr = AAudioStream_getXRunCount(gOut);
        long long now = oCount;
        if (xr != lastXruns) {
            xrWindow += xr - lastXruns;
            lastXruns = xr;
        }
        if (now - xrT > (long long) gSr * 10) {
            xrT = now;
            xrWindow = 0;
        } else if (xrWindow >= 2 && bufBursts < 4) {
            bufBursts++;
            AAudioStream_setBufferSizeInFrames(gOut, burstF * bufBursts);
            xrWindow = 0;
            xrT = now;
        }
    }
    double mag = sqrt(Zr * Zr + Zi * Zi), emag = sqrt(Er * Er + Ei * Ei);
    double v[22];
    v[0] = gState;
    v[1] = w * gSr / TWO_PI;
    v[2] = lockQ;
    v[3] = 2 * mag * mag / (P + 1e-12);
    v[4] = 20 * log10(2 * mag + 1e-9);
    v[5] = 20 * log10(2 * emag + 1e-9);
    v[6] = gK > 0 ? gK * 1000.0 / gSr : -1;
    v[7] = gActive;
    v[8] = gAB;
    v[9] = abOnN ? 20 * log10(abOn / abOnN + 1e-9) : 0;
    v[10] = abOffN ? 20 * log10(abOff / abOffN + 1e-9) : 0;
    v[11] = abOnN < abOffN ? abOnN : abOffN;
    v[12] = lastXruns;
    v[13] = resets;
    v[14] = gHasCal;
    v[15] = gState == ST_CAL ? (double) (oCount - calStartO) / (calEndO - calStartO) : 0;
    v[16] = gLms;
    v[17] = bufBursts;
    v[18] = gTsOk;
    v[19] = burstF * 1000.0 / gSr;
    v[20] = gExO * 2 + gExI;
    v[21] = gOutCh;
    jdoubleArray r = (*env)->NewDoubleArray(env, 22);
    (*env)->SetDoubleArrayRegion(env, r, 0, 22, v);
    return r;
}

JNIEXPORT jfloatArray JNICALL Java_io_github_xoaocardoso_medidor_MainActivity_ancGetCal(JNIEnv *env, jobject t) {
    if (!gCalNew) return NULL;
    gCalNew = 0;
    float v[2 * NCAL];
    for (int i = 0; i < NCAL; i++) {
        v[2 * i] = Gre[i];
        v[2 * i + 1] = Gim[i];
    }
    jfloatArray r = (*env)->NewFloatArray(env, 2 * NCAL);
    (*env)->SetFloatArrayRegion(env, r, 0, 2 * NCAL, v);
    return r;
}

JNIEXPORT void JNICALL Java_io_github_xoaocardoso_medidor_MainActivity_ancSetCal(JNIEnv *env, jobject t, jfloatArray a) {
    if ((*env)->GetArrayLength(env, a) != 2 * NCAL) return;
    float v[2 * NCAL];
    (*env)->GetFloatArrayRegion(env, a, 0, 2 * NCAL, v);
    for (int i = 0; i < NCAL; i++) {
        Gre[i] = v[2 * i];
        Gim[i] = v[2 * i + 1];
    }
    gHasCal = 1;
}
