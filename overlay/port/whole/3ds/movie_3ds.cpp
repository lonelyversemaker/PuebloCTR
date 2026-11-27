// The movies on the New 3DS (stage 6.1): its H.264 decoder (MVD, service mvd:STD) in place of the CRI player, whose
// MPEG-1 512x336 at 59.94 frames the ARM11 decodes at about 6 a second (and whose YUV -> RGB of the TEV the
// renderer does not do: an orange screen, 2026-11-05). Each movie converted beforehand
// (PuebloPrep): sdmc:/3ds/pueblo/movie/NAME.mvd (H.264 baseline 512x256 at 29.97, its NAL units with the
// "00 00 01" prefix, each after its length) and NAME.pcm (s16 stereo 32 kHz). A movie without them: the CRI player.
//
// game/sofdec.cpp (patch 0327) through runtime/movie_status.cpp: open at startApp, step at each appMain (on the game's
// thread, idle while a movie plays): the frames whose time the sound has reached decoded (all of them: H.264 skips
// none), the last one written by MVD into one of three output buffers in linear memory and published; the sound
// streamed from the SD to ndsp channel 1 (channel 0 is the game's) in wave buffers refilled here. The game draws its
// quad with a GX texture over a buffer of its own (mark); the renderer binds the frame there instead (gx_3ds.cpp
// movieTexture: a display transfer of the GX queue into PICA tiles before the draw).
//
// RE4_3DS_MOVIE=0: the CRI player as before.
#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_internal.h"
#include "re4_movie_hw.h"
#include "re4_runtime.h"

const void* volatile re4_movie_3ds_mark;
extern "C" void re4_3ds_gx_movie_room(uint32_t largest, uint32_t total);  // gx_3ds.cpp (plan 2d)

namespace {

const u32 kOutBuffers = 3;      // the GX thread and the GPU a frame or two behind the game
const u32 kNalBytes = 256u << 10;
const u32 kRate = 32000;
const int kAudioBuffers = 4;
const u32 kAudioFrames = 4096;  // 128 ms each
const int kChannel = 1;
const u32 kDecodesPerStep = 4;  // behind by more: caught up over the next steps

struct Header {
    char magic[8];
    u16 width, height;
    u32 fpsNum, fpsDen, frames, records, reserved;
};

bool gOn;
FILE* gVideo;
FILE* gSound;
Header gHead;
u32 gRecord;                   // records fed
u32 gDecoded;                  // frames decoded
u8* gNal;
u8* gOut[kOutBuffers];
volatile u32 gPublished;       // the newest frame: its number << 2 | its buffer; 0: none yet
MVDSTD_Config gConfig;
u32 gWorkBytes;
char gName[48];
// (R2b) the MVD's work room held from the start: linear memory in pieces after other rooms (the largest 1.6 MB of
// 17 free) had no 1.9 MB for mvdstdInit (libctru's linearAlloc: -1, the CRI player, an orange screen)
void* gReserve;
u32 gReserveBytes;

// 2 MB: mvdstdCalculateBufferSize says 1923 KB for 512x256 at level 2.1 (the console, 2026-11-12); not asked at the
// start: it opens mvd:STD (IPC), which the Azahar stand does not answer there
const u32 kReserveBytes = 2u << 20;

void reserveTake()
{
    if (gReserveBytes && !gReserve) {
        gReserve = linearMemAlign(gReserveBytes, 0x80);  // (libctru's linearAlloc: the same alignment)
    }
}

int16_t* gPcm;
ndspWaveBuf gWave[kAudioBuffers];
int gWaveNext;                 // the oldest queued
u64 gPlayed;                   // frames of the wave buffers done
bool gSoundEnd;
bool gSoundOn;

// for the log
u64 gStartTick, gDecodeTicks, gLastLine;
u32 gShownFrames, gSkippedFrames, gLate;

u64 soundTime()
{
    if (!gSoundOn) {
        return (svcGetSystemTick() - gStartTick) * kRate / SYSCLOCK_ARM11;
    }
    return gPlayed + ndspChnGetSamplePos(kChannel);
}

bool fillWave(ndspWaveBuf& w)
{
    if (gSoundEnd) {
        return false;
    }
    const size_t got = fread((void*) w.data_pcm16, 4, kAudioFrames, gSound);
    if (got < kAudioFrames) {
        gSoundEnd = true;
        if (!got) {
            return false;
        }
    }
    w.nsamples = (u32) got;
    DSP_FlushDataCache(w.data_pcm16, got * 4);
    ndspChnWaveBufAdd(kChannel, &w);
    return true;
}

// the wave buffers played: counted and refilled in their order
void pumpSound()
{
    if (!gSoundOn) {
        return;
    }
    for (int n = 0; n < kAudioBuffers; n++) {
        ndspWaveBuf& w = gWave[gWaveNext];
        if (w.status != NDSP_WBUF_DONE) {
            break;
        }
        gPlayed += w.nsamples;
        w.status = NDSP_WBUF_FREE;
        gWaveNext = (gWaveNext + 1) % kAudioBuffers;
        fillWave(w);
    }
}

bool soundPlaying()
{
    for (int i = 0; i < kAudioBuffers; i++) {
        if (gWave[i].status == NDSP_WBUF_QUEUED || gWave[i].status == NDSP_WBUF_PLAYING) {
            return true;
        }
    }
    return false;
}

// the next frame through MVD into `out`: its NAL units up to the one that yields a picture; false: none left or an error
bool decodeFrame(u8* out)
{
    while (gRecord < gHead.records) {
        u32 n = 0;
        if (fread(&n, 4, 1, gVideo) != 1 || n < 4 || n > kNalBytes || fread(gNal, 1, n, gVideo) != n) {
            re4_log("movie: %s: record %u of %u unreadable (%u bytes)", gName, (unsigned) gRecord,
                    (unsigned) gHead.records, (unsigned) n);
            gRecord = gHead.records;
            return false;
        }
        gRecord++;
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32) gNal, n);
        Result r = mvdstdProcessVideoFrame(gNal, n, 0, nullptr);
        if (!MVD_CHECKNALUPROC_SUCCESS(r)) {
            re4_log("movie: %s: mvdstdProcessVideoFrame 0x%08lx at record %u (NAL type %u, %u bytes)", gName,
                    (unsigned long) r, (unsigned) gRecord - 1, (unsigned) (gNal[3] & 31), (unsigned) n);
            continue;  // (as the browser: the next unit)
        }
        if (r == MVD_STATUS_PARAMSET || r == MVD_STATUS_INCOMPLETEPROCESSING) {
            continue;
        }
        gConfig.physaddr_outdata0 = osConvertVirtToPhys(out);
        r = mvdstdRenderVideoFrame(&gConfig, true);
        if (r != MVD_STATUS_OK) {
            re4_log("movie: %s: mvdstdRenderVideoFrame 0x%08lx at frame %u", gName, (unsigned long) r,
                    (unsigned) gDecoded);
        }
        gDecoded++;
        return true;
    }
    return false;
}

void report(const char* what)
{
    const double s = (double) (svcGetSystemTick() - gStartTick) / SYSCLOCK_ARM11;
    re4_log("movie: %s %s: %.1f s, %u of %u frames decoded (%.2f ms each), %u shown, %u decoded unshown, %u steps "
            "late by 2+ frames; sound %.1f s",
            gName, what, s, (unsigned) gDecoded, (unsigned) gHead.frames,
            gDecoded ? (double) gDecodeTicks / gDecoded / CPU_TICKS_PER_MSEC : 0.0, (unsigned) gShownFrames,
            (unsigned) gSkippedFrames, (unsigned) gLate, (double) soundTime() / kRate);
}

// (2026-11-27) the D-pad weapons' files: their room given back for a movie (its buffers want linear memory in one
// piece: with the room taken, the intro's picture had no room - 208 KB the largest piece), taken again after it
extern "C" void* re4_dvd_weapon_arena_off(unsigned* size);  // runtime/dvd.cpp
extern "C" void re4_dvd_weapon_arena(void* room, unsigned size);
extern "C" unsigned re4_3ds_weapon_room_bytes(void);  // setup_3ds.cpp: the room wanted now (weapons on the arrows)

void release()
{
    if (gSoundOn) {
        ndspChnWaveBufClear(kChannel);
        ndspChnReset(kChannel);
        gSoundOn = false;
    }
    if (gWorkBytes) {
        mvdstdExit();
        gWorkBytes = 0;
    }
    re4_movie_3ds_mark = nullptr;
    gPublished = 0;
    gOn = false;
    // the GX thread past the records naming the buffers (the GPU's transfer of the last one may still be queued: the
    // texture it fills is not drawn any more)
    if (re4_gxq_on) {
        re4_gxq_sync();
    }
    for (u32 i = 0; i < kOutBuffers; i++) {
        linearFree(gOut[i]);
        gOut[i] = nullptr;
    }
    linearFree(gNal);
    linearFree(gPcm);
    gNal = nullptr;
    gPcm = nullptr;
    reserveTake();  // (R2b) for the next movie
    const unsigned want = re4_3ds_weapon_room_bytes();
    if (want) {  // (2026-11-27) the weapon files' room (again, or the first time: weapons on the arrows now): linear
        // memory in one piece made as for a movie (the oldest textures dropped - made again when drawn), then half of
        // it; none: read from the card as before. No weapon on the arrows: the room stays the textures'
        void* room = nullptr;
        unsigned bytes = want;
        for (int t = 0; t < 2 && !room; t++) {
            bytes = want >> t;
            re4_3ds_gx_movie_room(bytes, bytes);
            room = linearAlloc(bytes);
        }
        bytes = room ? bytes : 0;
        re4_dvd_weapon_arena(room, bytes);
        re4_log("movie: the weapon files' room %s (%u KB of %u)", room ? "taken" : "NOT taken", bytes >> 10, want >> 10);
    }
    if (gReserveBytes && !gReserve) {
        re4_log("movie: the work room (%u KB) not taken back after the movie", (unsigned) gReserveBytes >> 10);
    }
    if (gVideo) {
        fclose(gVideo);
    }
    if (gSound) {
        fclose(gSound);
    }
    gVideo = gSound = nullptr;
}

int hwOpen(const char* fname)
{
    const char* e = getenv("RE4_3DS_MOVIE");
    if (e && atoi(e) == 0) {
        return 0;
    }
    const char* base = strrchr(fname, '/');
    base = base ? base + 1 : fname;
    snprintf(gName, sizeof(gName), "%s", base);
    char* dot = strrchr(gName, '.');
    if (dot) {
        *dot = 0;
    }
    char path[96];
    snprintf(path, sizeof(path), "sdmc:/3ds/pueblo/movie/%s.mvd", gName);
    gVideo = fopen(path, "rb");
    if (!gVideo) {
        re4_log("movie: %s: no %s, the CRI player", gName, path);
        return 0;
    }
    setvbuf(gVideo, nullptr, _IOFBF, 64 << 10);
    if (fread(&gHead, sizeof(gHead), 1, gVideo) != 1 || memcmp(gHead.magic, "RE4MVD01", 8) || gHead.width != 512 ||
        gHead.height != 256 || !gHead.fpsNum || !gHead.fpsDen) {
        re4_log("movie: %s: %s is not a 512x256 RE4MVD01 file, the CRI player", gName, path);
        fclose(gVideo);
        gVideo = nullptr;
        return 0;
    }
    snprintf(path, sizeof(path), "sdmc:/3ds/pueblo/movie/%s.pcm", gName);
    gSound = fopen(path, "rb");
    if (gSound) {
        setvbuf(gSound, nullptr, _IOFBF, 32 << 10);
    }
    unsigned wepBytes = 0;
    if (void* room = re4_dvd_weapon_arena_off(&wepBytes)) {  // (2026-11-27) the weapon files' room given back
        linearFree(room);
    }
    const u32 linearBefore = linearSpaceFree();
    MVDSTD_CalculateWorkBufSizeConfig calc;
    memset(&calc, 0, sizeof(calc));
    calc.level.enable = 1;
    calc.level.flag = MVD_CALC_WITH_LEVEL_FLAG_ENABLE_CALC;
    calc.level.level = MVD_H264_LEVEL_2_1;
    calc.width = gHead.width;
    calc.height = gHead.height;
    u32 work = 0;
    Result r = mvdstdCalculateBufferSize(&calc, &work);
    if (R_FAILED(r) || !work) {
        re4_log("movie: %s: mvdstdCalculateBufferSize 0x%08lx (%u bytes): the default", gName, (unsigned long) r,
                (unsigned) work);
        work = MVD_DEFAULT_WORKBUF_SIZE;
    }
    // (plan 2d) the renderer's textures out of the way first where linear memory is short (after other rooms: the CRI
    // player, an orange screen); the MVD's work room (the largest piece) taken first, the buffers after it
    const char* roomOn = getenv("RE4_3DS_MOVIEROOM");
    if (!roomOn || *roomOn != '0') {
        const u32 held = gReserve && gReserveBytes >= work ? work : 0;  // (R2b) the work room held already
        const u32 total = work - held + kOutBuffers * 512 * 256 * 2 + kNalBytes + kAudioBuffers * kAudioFrames * 4 +
                          (512u << 10);
        const u32 largest = held ? kNalBytes + (64u << 10) : work + (64u << 10);
        if (re4_gxq_on) {
            re4_gxq_fn(re4_3ds_gx_movie_room, largest, total);
            re4_gxq_sync();
        } else {
            re4_3ds_gx_movie_room(largest, total);
        }
    }
    if (gReserve && gReserveBytes < work) {
        re4_log("movie: %s: the work room held (%u KB) is less than asked (%u KB)", gName,
                (unsigned) gReserveBytes >> 10, (unsigned) work >> 10);
    }
    if (gReserve) {  // (R2b) its place to libctru's linearAlloc in mvdstdInit
        linearFree(gReserve);
        gReserve = nullptr;
    }
    r = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_BGR565, work, nullptr);
    bool ok = R_SUCCEEDED(r);
    if (ok) {
        gWorkBytes = work;
    } else {
        re4_log("movie: %s: mvdstdInit 0x%08lx with %u KB of work room", gName, (unsigned long) r,
                (unsigned) work >> 10);
    }
    if (ok) {
        // (R2b) each in a piece of its own: the textures dropped until it fits (after other rooms the free linear
        // memory is in pieces: plan 2d's one large piece held the three pictures, the NAL buffer found none)
        const bool room = !roomOn || *roomOn != '0';
        auto take = [room](u32 size, u32 align) -> void* {
            void* p = linearMemAlign(size, align);
            if (!p && room) {
                if (re4_gxq_on) {
                    re4_gxq_fn(re4_3ds_gx_movie_room, size + (64u << 10), 0u);
                    re4_gxq_sync();
                } else {
                    re4_3ds_gx_movie_room(size + (64u << 10), 0u);
                }
                p = linearMemAlign(size, align);
            }
            return p;
        };
        for (u32 i = 0; i < kOutBuffers; i++) {
            gOut[i] = (u8*) take(512 * 256 * 2, 0x80);
        }
        gNal = (u8*) take(kNalBytes, 0x40);
        gPcm = (int16_t*) take(kAudioBuffers * kAudioFrames * 4, 0x80);
        ok = gOut[0] && gOut[1] && gOut[2] && gNal && gPcm;
    }
    if (!ok) {
        re4_log("movie: %s: no room (linear %lu KB free; work room %u KB): the CRI player", gName,
                (unsigned long) linearSpaceFree() >> 10, (unsigned) work >> 10);
        release();
        return 0;
    }
    for (u32 i = 0; i < kOutBuffers; i++) {
        memset(gOut[i], 0, 512 * 256 * 2);
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32) gOut[i], 512 * 256 * 2);
    }
    mvdstdGenerateDefaultConfig(&gConfig, gHead.width, gHead.height, gHead.width, gHead.height, nullptr,
                                (u32*) gOut[0], (u32*) gOut[0]);
    gRecord = gDecoded = 0;
    gPublished = 0;
    gShownFrames = gSkippedFrames = gLate = 0;
    gDecodeTicks = 0;
    gPlayed = 0;
    gSoundEnd = !gSound;
    gWaveNext = 0;
    gSoundOn = false;
    if (gSound) {
        ndspChnReset(kChannel);
        ndspChnSetInterp(kChannel, NDSP_INTERP_LINEAR);
        ndspChnSetRate(kChannel, (float) kRate);
        ndspChnSetFormat(kChannel, NDSP_FORMAT_STEREO_PCM16);
        float mix[12] = {1.0f, 1.0f};
        ndspChnSetMix(kChannel, mix);
        memset(gWave, 0, sizeof(gWave));
        for (int i = 0; i < kAudioBuffers; i++) {
            gWave[i].data_pcm16 = gPcm + (size_t) i * kAudioFrames * 2;
            gWave[i].status = NDSP_WBUF_FREE;
        }
        for (int i = 0; i < kAudioBuffers; i++) {
            fillWave(gWave[i]);
        }
        gSoundOn = true;
    }
    gOn = true;
    gStartTick = gLastLine = svcGetSystemTick();
    re4_log("movie: %s through MVD: %ux%u at %.2f, %u frames (%u records), work room %u KB (linear %u -> %lu KB free), "
            "sound %s",
            gName, (unsigned) gHead.width, (unsigned) gHead.height, (double) gHead.fpsNum / gHead.fpsDen,
            (unsigned) gHead.frames, (unsigned) gHead.records, (unsigned) work >> 10, (unsigned) linearBefore >> 10,
            (unsigned long) linearSpaceFree() >> 10, gSound ? "yes" : "none");
    return 1;
}

int hwStep()
{
    if (!gOn) {
        return 0;
    }
    pumpSound();
    const u64 due = soundTime() * gHead.fpsNum / ((u64) kRate * gHead.fpsDen);  // the frame whose time it is
    if (due > gDecoded + 1) {
        gLate++;
    }
    u32 decoded = 0;
    u32 buffer = gPublished ? ((gPublished & 3) + 1) % kOutBuffers : 0;
    while (gDecoded <= due && decoded < kDecodesPerStep) {
        const u64 from = svcGetSystemTick();
        const bool ok = decodeFrame(gOut[buffer]);
        gDecodeTicks += svcGetSystemTick() - from;
        if (!ok) {
            break;
        }
        decoded++;
    }
    if (decoded) {
        gPublished = gDecoded << 2 | buffer;
        gShownFrames++;
        gSkippedFrames += decoded - 1;
    }
    if (svcGetSystemTick() - gLastLine > 5ull * SYSCLOCK_ARM11) {
        gLastLine = svcGetSystemTick();
        report("playing");
    }
    const bool videoEnd = gRecord >= gHead.records;
    if (videoEnd && (!gSoundOn || (gSoundEnd && !soundPlaying()))) {
        return 0;
    }
    return 1;
}

int hwShown()
{
    return gPublished != 0;
}

void hwClose(int skipped)
{
    if (!gOn) {
        return;
    }
    report(skipped ? "skipped (START)" : "ended");
    release();
}

void hwTime(int* count, int* scale)
{
    *count = gOn ? (int) soundTime() : 0;
    *scale = (int) kRate;
}

void hwMark(const void* texels)
{
    re4_movie_3ds_mark = texels;
}

const Re4MovieHw kHw = {hwOpen, hwStep, hwShown, hwClose, hwTime, hwMark};

struct Install {
    Install() { re4_movie_hw = &kHw; }
} gInstall;

}  // namespace

// (R2b) main_3ds.cpp, before the game (linear memory still in one piece): the MVD's work room held for the movies
extern "C" void re4_3ds_movie_reserve(void)
{
    const char* e = getenv("RE4_3DS_MOVIE");
    const char* v = getenv("RE4_3DS_MOVIERESERVE");
    if ((e && atoi(e) == 0) || (v && *v == '0')) {
        return;
    }
    gReserveBytes = kReserveBytes;
    reserveTake();
    re4_log("movie: the work room held for the movies: %u KB %s (linear %lu KB free)", (unsigned) gReserveBytes >> 10,
            gReserve ? "taken" : "NOT taken", (unsigned long) linearSpaceFree() >> 10);
}

extern "C" int re4_movie_3ds_frame(const void** out, uint32_t* seq)
{
    const u32 p = gPublished;
    if (!gOn || !p) {
        return 0;
    }
    *out = gOut[p & 3];
    *seq = p >> 2;
    return 1;
}
