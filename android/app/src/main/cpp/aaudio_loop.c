#include <jni.h>
#include <aaudio/AAudio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    AAudioStream *in;
    int16_t *rec;
    int recCap;
    int recLen;
    int16_t *tmp;
    int tmpCap;
    const int16_t *chirp;
    int chirpLen;
    int chirpPos;
    int *emits;
    int nEmit;
    int maxEmit;
    int gap;
    int startAt;
    int sr;
} State;

static State S;

static void pullInput(int32_t want) {
    for (;;) {
        int32_t n = want < S.tmpCap ? want : S.tmpCap;
        aaudio_result_t got = AAudioStream_read(S.in, S.tmp, n, 0);
        if (got <= 0) break;
        int room = S.recCap - S.recLen;
        int c = got < room ? got : room;
        if (c > 0) {
            memcpy(S.rec + S.recLen, S.tmp, c * sizeof(int16_t));
            S.recLen += c;
        }
        if (got < n) break;
    }
}

static aaudio_data_callback_result_t cb(AAudioStream *stream, void *user, void *data, int32_t frames) {
    int16_t *out = (int16_t *) data;
    pullInput(frames);
    memset(out, 0, frames * sizeof(int16_t));
    if (S.chirpPos < 0 && S.nEmit < S.maxEmit && S.recLen >= S.startAt + S.nEmit * S.gap) {
        S.emits[S.nEmit++] = S.recLen;
        S.chirpPos = 0;
    }
    if (S.chirpPos >= 0) {
        int m = S.chirpLen - S.chirpPos;
        if (m > frames) m = frames;
        memcpy(out, S.chirp + S.chirpPos, m * sizeof(int16_t));
        S.chirpPos += m;
        if (S.chirpPos >= S.chirpLen) S.chirpPos = -1;
    }
    return S.recLen >= S.recCap ? AAUDIO_CALLBACK_RESULT_STOP : AAUDIO_CALLBACK_RESULT_CONTINUE;
}

JNIEXPORT jstring JNICALL
Java_io_github_xoaocardoso_medidor_MainActivity_aaudioRun(JNIEnv *env, jobject thiz, jint exclusive, jint bursts,
                                                          jshortArray jchirpOut, jshortArray jrec, jintArray jemits,
                                                          jintArray jinfo, jdouble gapSec, jint sampleRate) {
    AAudioStreamBuilder *b;
    AAudioStream *out = NULL, *in = NULL;
    aaudio_sharing_mode_t sm = exclusive ? AAUDIO_SHARING_MODE_EXCLUSIVE : AAUDIO_SHARING_MODE_SHARED;

    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return (*env)->NewStringUTF(env, "builder falhou");
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, sm);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 1);
    if (sampleRate > 0) AAudioStreamBuilder_setSampleRate(b, sampleRate);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_MEDIA);
    AAudioStreamBuilder_setDataCallback(b, cb, NULL);
    if (AAudioStreamBuilder_openStream(b, &out) != AAUDIO_OK) {
        AAudioStreamBuilder_delete(b);
        return (*env)->NewStringUTF(env, "saida nao abriu");
    }
    AAudioStreamBuilder_delete(b);
    int sr = AAudioStream_getSampleRate(out);

    AAudio_createStreamBuilder(&b);
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, sm);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 1);
    AAudioStreamBuilder_setSampleRate(b, sr);
    AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_PERFORMANCE);
    if (AAudioStreamBuilder_openStream(b, &in) != AAUDIO_OK) {
        AAudioStreamBuilder_delete(b);
        AAudioStream_close(out);
        return (*env)->NewStringUTF(env, "entrada nao abriu");
    }
    AAudioStreamBuilder_delete(b);

    int burst = AAudioStream_getFramesPerBurst(out);
    AAudioStream_setBufferSizeInFrames(out, burst * bursts);

    jsize chirpLen = (*env)->GetArrayLength(env, jchirpOut);
    int16_t *chirp = malloc(chirpLen * sizeof(int16_t));
    (*env)->GetShortArrayRegion(env, jchirpOut, 0, chirpLen, chirp);

    memset(&S, 0, sizeof(S));
    S.in = in;
    S.recCap = (*env)->GetArrayLength(env, jrec);
    S.rec = calloc(S.recCap, sizeof(int16_t));
    S.tmpCap = 8192;
    S.tmp = malloc(S.tmpCap * sizeof(int16_t));
    S.chirp = chirp;
    S.chirpLen = chirpLen;
    S.chirpPos = -1;
    S.maxEmit = (*env)->GetArrayLength(env, jemits);
    S.emits = calloc(S.maxEmit, sizeof(int));
    S.sr = sr;
    S.gap = (int) (gapSec * sr);
    S.startAt = sr / 2;

    AAudioStream_requestStart(in);
    AAudioStream_requestStart(out);
    long limitUs = (long) ((double) S.recCap / sr * 1e6) + 3000000;
    for (long t = 0; t < limitUs && S.recLen < S.recCap; t += 20000) usleep(20000);
    AAudioStream_requestStop(out);
    AAudioStream_requestStop(in);

    jint info[6];
    info[0] = sr;
    info[1] = burst;
    info[2] = AAudioStream_getBufferSizeInFrames(out);
    info[3] = AAudioStream_getSharingMode(out) == AAUDIO_SHARING_MODE_EXCLUSIVE;
    info[4] = AAudioStream_getSharingMode(in) == AAUDIO_SHARING_MODE_EXCLUSIVE;
    info[5] = AAudioStream_getXRunCount(out);
    (*env)->SetIntArrayRegion(env, jinfo, 0, 6, info);
    (*env)->SetShortArrayRegion(env, jrec, 0, S.recLen, S.rec);
    (*env)->SetIntArrayRegion(env, jemits, 0, S.nEmit, S.emits);

    AAudioStream_close(out);
    AAudioStream_close(in);
    free(chirp);
    free(S.rec);
    free(S.tmp);
    free(S.emits);
    return NULL;
}
