// The game's sound on the 3DS (stage 3.4): the AI's ring (runtime/ai.cpp: the 32 kHz stereo the DSP of
// runtime/dsp.cpp mixed) goes out on one stereo channel of ndsp, as REGTA's OpenAL backend does it:
// a few wave buffers in linear memory, a thread that refills each one the DSP has played,
// woken by ndsp's frame callback.
//
// The frames are copied as they are (re4_audio_take); the 3DS's DSP converts the rate, and the rate matching of
// re4_audio_read is done by the channel's rate instead: about 60 ms stay in the ring (the game delivers its blocks
// in bursts, with the retraces), at most 0.5 % faster or slower, and after a pause the reader skips ahead.
// re4_audio_read's cubic interpolation in floats took 10 % of a core (the first try, r100-3ds-audio1.log).
//
// The game's clock must run in real time for this (RE4_VI_FAST=0, main_3ds.cpp): with RE4_VI_FAST=1 the game
// makes its sound as fast as it runs (r100 at 47 ms a frame: 70 %, the ring ran dry in most buffers).
//
// The thread runs on core 1 (APT_SetAppCpuTimeLimit), else on core 0 above the game. Needs the DSP firmware
// (sdmc:/3ds/dspfirm.cdc, Luma3DS dumps it); without it the game runs silent.
//
// RE4_3DS_AUDIO=0: no sound. RE4_3DS_AUDIO_BUFFERS (4) wave buffers of RE4_3DS_AUDIO_FRAMES (640, 20 ms) frames.
// RE4_3DS_AUDIO_CORE1 (30): the share of core 1 asked for, 0: the thread on core 0.
//
// RE4_3DS_DSPTHREAD=1: the DSP's mixing (runtime/dsp.cpp: AX's command list, about 0.38 ms an AI block, 2.5 ms a game
// frame in r100) on core 1 (re4_3ds_dsp_thread_start), while the game goes on; the game waits for it at the next AI
// block (re4_dsp_sync); N > 1: from game frame N on (comparisons within one run). No gain on core 1 (stage 3.4,
// r100-3ds-dspt2/3.log): with 30 % of core 1 a list takes 1.3 ms there and the game, whose AI blocks come in bursts
// with the retraces, waited about as long as the mixing took it before; with 80 % (0.46 ms a list, waits 1.5 ms a
// frame) the system's services on core 1 fall behind and the GPU and the GX thread slowed by 5-7 ms a frame.
// RE4_3DS_DSPTHREAD_CORE (1): the worker's core; 2 is the GX thread's (idle 10-20 ms a frame in the village, stage
// 3.6). RE4_3DS_DSPTHREAD_PRIO: its priority (core 1: 0x18; core 2: 0x2e, above the GX thread's 0x2f, so a list
// takes the core at once instead of after the GX thread's polling). With the lists behind the game (RE4_DSP_BEHIND,
// stage 3.4b: the game does not wait for them, only for room in their queue and before it writes ARAM) core 1's 30 %
// are enough (a list 2.1 ms there, 0.55 ms of work with the reverb): on by default on core 1, behind (main_3ds.cpp
// kDefaults).
#include <3ds.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "re4_runtime.h"

// runtime/ai.cpp
extern "C" int re4_audio_take(short* out, int frames);
extern "C" void re4_audio_stats(unsigned* blocks, unsigned* fill, unsigned* overruns, unsigned* underruns,
                                unsigned* skips);
extern "C" volatile unsigned re4_audio_speed_q16;  // stage 6.1b: the AI clock's speed (65536: real time)
extern "C" volatile int re4_audio_stretch_on;

namespace {

const int kMaxBuffers = 8;
// frames kept in the ring (60 ms), as runtime/ai.cpp's reader; (2026-11-26) RE4_3DS_AUDIO_TARGET: other ms (30-150)
// - the sound's cushion when the game's thread is late (a dry ring gets silence, whatever the buffers), its latency
unsigned gTarget = 1920;
const unsigned kSkip = 8000;    // more than this (250 ms): skip to the target

ndspWaveBuf gBuf[kMaxBuffers];
int gBuffers = 4;
int gFrames = 640;
int16_t* gPcm;
LightEvent gWake;
Thread gThread;
volatile bool gRun;
bool gOn;
int gCore;

// for the log (the thread's own counts): buffers queued, of them with silence where the ring ran dry, the ticks
// the thread spent filling them
u32 gQueued, gShort, gSkips;
u64 gFillTicks;
// the DSP's thread (re4_3ds_dsp_thread_start): lists, the worker's ticks on them, the game's waits that found the list
// still running and their ticks
u32 gDspLists, gDspBlocked;
u64 gDspWorkTicks, gDspWaitTicks;
int16_t gDrop[1024 * 2];

// ---------------------------------------------------------------- time stretch (stage 6.1b)
// WSOLA between the AI's ring and the buffers: the game's sound comes at re4_audio_speed_q16 of real time (slower in
// cutscenes, runtime/cutscene_sound.cpp) and goes out in real time with its pitch. Frames of kL (20 ms) under a
// periodic Hann window, one every kH (10 ms) of output (the windows add up to 1); frame k starts within kDelta (8 ms)
// of the time map (s x kH a frame) where it is most like the natural continuation of frame k - 1 (the kH input
// frames after its half): normalized cross-correlation on the mono mix at a quarter of the rate, in steps of 4, then
// +-3 at the full rate. At speed 1 the natural continuation itself: the output is the input (after the first hop).
// RE4_3DS_STRETCH=0: off (the ring copied as before).
const int kL = 640, kH = 320, kDelta = 256;
const int kIn = 8192;  // input frames held (from gInBase)
bool gStretch;
float gWin[kL];
int16_t gIn[kIn * 2];
int gInLen;
long long gInBase;      // the input index of gIn[0]
long long gPrev = -kH;  // the start of the frame before
double gPos;            // where the time map puts the next frame
float gOla[kH * 2];     // the frame before's second half, windowed
int16_t gOut[kH * 2];
int gOutLen, gOutAt;
u32 gHops, gSearched, gDryFrames;
double gSpeedSum;
float gRef[kH / 4], gCand[(2 * kDelta + kH) / 4 + 2];

inline const int16_t* inAt(long long i)
{
    return gIn + (i - gInBase) * 2;
}

// The input up to index `end` (exclusive) in gIn, from the ring (silence where it ran dry); what lies before `keep`
// may go.
void inNeed(long long end, long long keep)
{
    if (keep - gInBase > kIn / 4) {
        const int drop = (int) (keep - gInBase);
        const int left = gInLen - drop;
        if (left > 0) {
            memmove(gIn, gIn + drop * 2, (size_t) left * 4);
        }
        gInLen = left > 0 ? left : 0;
        gInBase += drop;
    }
    while (gInBase + gInLen < end && gInLen < kIn) {
        int n = (int) (end - (gInBase + gInLen));
        n = n > kIn - gInLen ? kIn - gInLen : n;
        const int got = re4_audio_take(gIn + gInLen * 2, n);
        if (got < n) {
            memset(gIn + (gInLen + got) * 2, 0, (size_t) (n - got) * 4);
            gDryFrames += (u32) (n - got);
        }
        gInLen += n;
    }
}

// One hop: kH output frames into gOut.
void hop(double s)
{
    const long long nat = gPrev + kH;
    long long x = nat;
    if (s == 1.0) {
        gPos = (double) nat;  // (the time map follows: within kDelta of it already)
        inNeed(x + kL, gPrev);
    } else {
        const long long nominal = (long long) (gPos + 0.5);
        long long lo = nominal - kDelta, hi = nominal + kDelta;
        lo = lo < gInBase ? gInBase : lo;
        lo = lo < 0 ? 0 : lo;
        hi = hi < lo ? lo : hi;
        const long long keep = (nat < lo ? nat : lo);
        inNeed((hi > nat ? hi : nat) + kL, keep);
        if (nat >= gInBase) {
            // the reference: the natural continuation's first kH frames, mono at a quarter of the rate
            for (int k = 0; k < kH / 4; k++) {
                const int16_t* p = inAt(nat + 4 * k);
                gRef[k] = (float) p[0] + (float) p[1];
            }
            const int span = (int) (hi - lo) + kH;
            const int n4 = span / 4;
            for (int k = 0; k < n4; k++) {
                const int16_t* p = inAt(lo + 4 * k);
                gCand[k] = (float) p[0] + (float) p[1];
            }
            // energy of the candidate windows (kH / 4 values), sliding
            float energy = 0.0f;
            for (int k = 0; k < kH / 4; k++) {
                energy += gCand[k] * gCand[k];
            }
            float best = -1e30f;
            int bestK = 0;
            const int lags = (int) (hi - lo) / 4;
            for (int j = 0; j <= lags; j++) {
                float c = 0.0f;
                const float* q = gCand + j;
                for (int k = 0; k < kH / 4; k++) {
                    c += gRef[k] * q[k];
                }
                const float score = c / sqrtf(energy + 1.0f);
                if (score > best) {
                    best = score;
                    bestK = j;
                }
                if (j < lags) {
                    energy += q[kH / 4] * q[kH / 4] - q[0] * q[0];
                    energy = energy < 0.0f ? 0.0f : energy;
                }
            }
            x = lo + 4 * bestK;
            // +-3 at the full rate (mono)
            long long bestX = x;
            float bestF = -1e30f;
            for (long long t = x - 3; t <= x + 3; t++) {
                if (t < lo || t > hi) {
                    continue;
                }
                float c = 0.0f, e = 0.0f;
                for (int k = 0; k < kH; k += 2) {
                    const int16_t* a = inAt(nat + k);
                    const int16_t* b = inAt(t + k);
                    const float r = (float) a[0] + (float) a[1], v = (float) b[0] + (float) b[1];
                    c += r * v;
                    e += v * v;
                }
                const float score = c / sqrtf(e + 1.0f);
                if (score > bestF) {
                    bestF = score;
                    bestX = t;
                }
            }
            x = bestX;
            gSearched++;
        } else {
            x = nominal < gInBase ? gInBase : nominal;
        }
        gPos += s * kH;
    }
    const int16_t* p = inAt(x);
    for (int n = 0; n < kH; n++) {
        for (int c = 0; c < 2; c++) {
            float v = gOla[n * 2 + c] + gWin[n] * (float) p[n * 2 + c];
            v = v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v;
            gOut[n * 2 + c] = (int16_t) (v >= 0.0f ? v + 0.5f : v - 0.5f);
            gOla[n * 2 + c] = gWin[n + kH] * (float) p[(n + kH) * 2 + c];
        }
    }
    gOutLen = kH;
    gOutAt = 0;
    gPrev = x;
    gHops++;
    gSpeedSum += s;
}

void stretchInit()
{
    for (int n = 0; n < kL; n++) {
        gWin[n] = 0.5f - 0.5f * cosf(6.28318530718f * (float) n / (float) kL);
    }
}

// `frames` output frames into pcm through the stretch. `fill`: the ring's frames now. The game's speed and the
// stretch's meet in the ring only on the whole (each return to speed 1 puts the time map on the natural continuation,
// up to kDelta off): over 40 ms above or below its target the stretch goes up to 10 % faster or slower, the surplus in about
// 2 s (the first console run, stretch1: without it the ring crept to 226 ms and skipped 0.2 s every 10 s); within
// 0.3 % of 1, the input itself.
void stretchTake(int16_t* pcm, int frames, unsigned fill)
{
    // (within 40 ms of the target the channel's rate does it, as ever: refill)
    double excess = (double) fill - (double) gTarget;
    excess = excess > 1280.0 ? excess - 1280.0 : excess < -1280.0 ? excess + 1280.0 : 0.0;
    double drain = excess / (32000.0 * 2.0);
    drain = drain > 0.1 ? 0.1 : drain < -0.1 ? -0.1 : drain;
    double s = re4_audio_speed_q16 / 65536.0 * (1.0 + drain);
    s = s > 0.997 && s < 1.003 ? 1.0 : s;
    for (int done = 0; done < frames;) {
        if (gOutAt == gOutLen) {
            hop(s);
        }
        int n = gOutLen - gOutAt;
        n = n > frames - done ? frames - done : n;
        memcpy(pcm + done * 2, gOut + gOutAt * 2, (size_t) n * 4);
        gOutAt += n;
        done += n;
    }
}

// The next buffer's frames from the ring and the channel's rate for them.
void refill(int16_t* pcm)
{
    unsigned blocks, fill, overruns, underruns, skips;
    re4_audio_stats(&blocks, &fill, &overruns, &underruns, &skips);
    if (fill > kSkip) {
        // after a pause: what the game played meanwhile is gone
        for (unsigned n = fill - gTarget; n;) {
            const unsigned part = n < 1024 ? n : 1024;
            re4_audio_take(gDrop, (int) part);
            n -= part;
        }
        fill = gTarget;
        gSkips++;
    }
    // a little faster when the ring fills up, slower when it runs low (1/65536 steps, as runtime/ai.cpp)
    int adjust = ((int) fill - (int) gTarget) / 8;
    adjust = adjust > 328 ? 328 : adjust < -328 ? -328 : adjust;
    ndspChnSetRate(0, 32000.0f * (1.0f + adjust / 65536.0f));
    if (gStretch) {
        const u32 dry = gDryFrames;
        stretchTake(pcm, gFrames, fill);
        gShort += gDryFrames != dry;
        return;
    }
    const int got = re4_audio_take(pcm, gFrames);
    if (got < gFrames) {
        memset(pcm + got * 2, 0, (size_t) (gFrames - got) * 4);
        gShort++;
    }
}

void frameCallback(void*)
{
    LightEvent_Signal(&gWake);
}

extern "C" void re4_3ds_log_fpscr(const char* who);  // main_3ds.cpp: FPSCR of the calling thread

void audioMain(void*)
{
    re4_3ds_log_fpscr("sound thread");
    u64 logAt = svcGetSystemTick();
    while (gRun) {
        for (int i = 0; i < gBuffers; i++) {
            ndspWaveBuf& b = gBuf[i];
            if (b.status != NDSP_WBUF_FREE && b.status != NDSP_WBUF_DONE) {
                continue;
            }
            const u64 t0 = svcGetSystemTick();
            int16_t* pcm = (int16_t*) b.data_pcm16;
            refill(pcm);
            DSP_FlushDataCache(pcm, gFrames * 4);
            ndspChnWaveBufAdd(0, &b);
            gFillTicks += svcGetSystemTick() - t0;
            gQueued++;
        }
        const u64 now = svcGetSystemTick();
        if (now - logAt >= 10ull * SYSCLOCK_ARM11) {
            const double s = (double) (now - logAt) / SYSCLOCK_ARM11;
            unsigned blocks, fill, overruns, underruns, skips;
            re4_audio_stats(&blocks, &fill, &overruns, &underruns, &skips);
            re4_log("3ds: sound out: %u buffers in %.1f s, %u of them short (the ring ran dry), %u skips; ring %u ms; "
                    "filling %.2f ms/s (core %d)",
                    gQueued, s, gShort, gSkips, fill / 32, gFillTicks / (double) CPU_TICKS_PER_MSEC / s, gCore);
            if (gStretch && gHops) {
                re4_log("3ds: sound stretch (6.1b): %u hops, %u of them searched, speed mean %.3f", gHops,
                        gSearched, gSpeedSum / gHops);
                gHops = gSearched = 0;
                gSpeedSum = 0.0;
            }
            gQueued = gShort = gSkips = 0;
            if (gDspLists) {
                // (the counts of the other threads, read without a lock: for the log)
                const double ms = (double) CPU_TICKS_PER_MSEC;
                re4_log("3ds: DSP worker: %u lists in %.1f s, %.3f ms each (%.2f ms/s); the game waited for %u of "
                        "them, %.2f ms/s",
                        gDspLists, s, gDspWorkTicks / ms / gDspLists, gDspWorkTicks / ms / s, gDspBlocked,
                        gDspWaitTicks / ms / s);
                gDspLists = gDspBlocked = 0;
                gDspWorkTicks = gDspWaitTicks = 0;
            }
            gFillTicks = 0;
            logAt = now;
        }
        LightEvent_Wait(&gWake);
    }
}

int setting(const char* name, int fallback)
{
    const char* v = getenv(name);
    return v && *v ? atoi(v) : fallback;
}

// ---------------------------------------------------------------- the DSP's lists on core 1
LightEvent gDspGo, gDspDone;

void dspMain(void*)
{
    re4_3ds_log_fpscr("DSP worker thread");
    for (;;) {
        LightEvent_Wait(&gDspGo);
        __dmb();
        const u64 t0 = svcGetSystemTick();
        re4_dsp_work();
        gDspWorkTicks += svcGetSystemTick() - t0;
        __dmb();
        LightEvent_Signal(&gDspDone);
    }
}

void dspPost(void)
{
    gDspLists++;
    __dmb();
    LightEvent_Signal(&gDspGo);
}

void dspWait(void)
{
    if (!LightEvent_TryWait(&gDspDone)) {
        const u64 t0 = svcGetSystemTick();
        LightEvent_Wait(&gDspDone);
        gDspWaitTicks += svcGetSystemTick() - t0;
        gDspBlocked++;
    }
    __dmb();
}

}  // namespace

extern "C" void re4_add_frame_hook(void (*hook)(unsigned));  // runtime/pad.cpp

namespace {

unsigned gDspFrom;  // RE4_3DS_DSPTHREAD > 1: the frame the worker takes over

void dspFrame(unsigned frame)
{
    if (gDspFrom && frame >= gDspFrom) {
        gDspFrom = 0;
        re4_dsp_set_worker(dspPost, dspWait);
        re4_log("3ds: the DSP's mixing moves to its thread at frame %u", frame);
    }
}

}  // namespace

extern "C" {

// Starts the output (main_3ds.cpp); 0 when there is none (off, no DSP firmware, no memory or thread).
int re4_3ds_audio_start(void)
{
    if (setting("RE4_3DS_AUDIO", 1) == 0) {
        re4_log("3ds: sound off (RE4_3DS_AUDIO=0)");
        return 0;
    }
    gBuffers = setting("RE4_3DS_AUDIO_BUFFERS", 4);
    gBuffers = gBuffers < 2 ? 2 : gBuffers > kMaxBuffers ? kMaxBuffers : gBuffers;
    gFrames = setting("RE4_3DS_AUDIO_FRAMES", 640);
    gFrames = gFrames < 160 ? 160 : gFrames > 4096 ? 4096 : gFrames;
    const int targetMs = setting("RE4_3DS_AUDIO_TARGET", 60);
    if (targetMs != 60) {
        gTarget = (unsigned) (targetMs < 30 ? 30 : targetMs > 150 ? 150 : targetMs) * 32;
        re4_log("3ds: sound: %u ms kept in the ring (RE4_3DS_AUDIO_TARGET; 60 before)", gTarget / 32);
    }
    const Result r = ndspInit();
    if (R_FAILED(r)) {
        re4_log("3ds: no sound: ndspInit failed (0x%08lx; the DSP firmware sdmc:/3ds/dspfirm.cdc missing?)",
                (unsigned long) r);
        return 0;
    }
    gPcm = (int16_t*) linearAlloc((size_t) gBuffers * gFrames * 4);
    if (!gPcm) {
        re4_log("3ds: no sound: no linear memory for the buffers");
        ndspExit();
        return 0;
    }
    memset(gPcm, 0, (size_t) gBuffers * gFrames * 4);
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(0, 32000.0f);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = {1.0f, 1.0f};  // left and right to the front
    ndspChnSetMix(0, mix);
    memset(gBuf, 0, sizeof(gBuf));
    for (int i = 0; i < gBuffers; i++) {
        gBuf[i].data_pcm16 = gPcm + (size_t) i * gFrames * 2;
        gBuf[i].nsamples = (u32) gFrames;
        gBuf[i].status = NDSP_WBUF_FREE;
    }
    LightEvent_Init(&gWake, RESET_ONESHOT);
    ndspSetCallback(frameCallback, nullptr);
    gRun = true;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    // (2026-11-18) 50 %, was 30: the castle's insects (25-47 voices) did not fit (journal, castle2)
    const int share = setting("RE4_3DS_AUDIO_CORE1", 50);
    gThread = nullptr;
    if (share > 0) {
        APT_SetAppCpuTimeLimit((u32) share);
        gCore = 1;
        gThread = threadCreate(audioMain, nullptr, 16 * 1024, 0x18, 1, false);
    }
    if (!gThread) {
        gCore = 0;
        gThread = threadCreate(audioMain, nullptr, 16 * 1024, priority - 2, 0, false);
    }
    if (!gThread) {
        re4_log("3ds: no sound: no thread for the output");
        gRun = false;
        ndspSetCallback(nullptr, nullptr);
        linearFree(gPcm);
        gPcm = nullptr;
        ndspExit();
        return 0;
    }
    gOn = true;
    gStretch = setting("RE4_3DS_STRETCH", 1) != 0;
    if (gStretch) {
        stretchInit();
        re4_audio_stretch_on = 1;
    }
    re4_log("3ds: sound stretch in time (6.1b, cutscenes at the game's speed): %s", gStretch ? "on" : "off");
    re4_log("3ds: sound out on ndsp channel 0: 32 kHz stereo, %d buffers of %d frames (%.0f ms), thread on core %d",
            gBuffers, gFrames, gBuffers * gFrames / 32.0, gCore);
    return 1;
}

// The DSP's lists on a thread on core 1 (main_3ds.cpp, after re4_3ds_audio_start: the core's share).
void re4_3ds_dsp_thread_start(void)
{
    const int from = setting("RE4_3DS_DSPTHREAD", 0);
    if (from == 0) {
        return;  // the DSP's mixing on the game's thread
    }
    const int core = setting("RE4_3DS_DSPTHREAD_CORE", 1);
    const int priority = setting("RE4_3DS_DSPTHREAD_PRIO", core == 1 ? 0x18 : 0x2e);
    if (core == 1) {
        APT_SetAppCpuTimeLimit((u32) setting("RE4_3DS_AUDIO_CORE1", 50));
    }
    LightEvent_Init(&gDspGo, RESET_ONESHOT);
    LightEvent_Init(&gDspDone, RESET_ONESHOT);
    if (!threadCreate(dspMain, nullptr, 32 * 1024, priority, core, true)) {
        re4_log("3ds: no thread on core %d (priority 0x%x) for the DSP: its mixing stays on the game's thread", core,
                priority);
        return;
    }
    if (from > 1) {
        gDspFrom = (unsigned) from;
        re4_add_frame_hook(dspFrame);
        re4_log("3ds: the DSP's mixing on a thread on core %d (priority 0x%x) from frame %d", core, priority, from);
        return;
    }
    re4_dsp_set_worker(dspPost, dspWait);
    re4_log("3ds: the DSP's mixing on a thread on core %d (priority 0x%x)", core, priority);
}

// Stops the output before the app ends (system_3ds.cpp re4_3ds_finish). The thread is woken here: on HOME or the
// power button the DSP's callbacks may stop coming (REGTA's order), and the thread ends before the buffers go.
void re4_3ds_audio_stop(void)
{
    if (!gOn) {
        return;
    }
    gOn = false;
    gRun = false;
    LightEvent_Signal(&gWake);
    threadJoin(gThread, 1000000000ull);  // done with the buffers
    threadFree(gThread);
    gThread = nullptr;
    ndspSetCallback(nullptr, nullptr);
    ndspChnWaveBufClear(0);
    ndspExit();
    if (gPcm) {
        linearFree(gPcm);
        gPcm = nullptr;
    }
}

}  // extern "C"
