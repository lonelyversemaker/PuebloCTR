// The 3DS launcher (stage 4.4): the game core with the picture (gx_3ds.cpp) and the sound (audio_3ds.cpp), as
// the headless Linux runs of re4_host (host/main.cpp), with the time of each game frame (system_3ds.cpp).
//
// Settings are the RE4_* variables of the Linux runs, set before the static constructors. The
// defaults below play room r100 with the 3DS buttons (stage 6.2, input_3ds.cpp): the room menu is
// passed by a script that ends when the room loads, then the buttons; sdmc:/3ds/pueblo/pueblo.cfg
// overrides or adds them, one VAR=VALUE per line, # comments. The test runs of stage 3 (Leon
// walks, RE4_PAD_FREE_SCRIPT, RE4_TEST_PLACE, RE4_STOP_AFTER) set theirs there.
// The extracted disc files are read from sdmc:/3ds/pueblo/data (RE4_DATA_DIR), the log goes to
// sdmc:/3ds/pueblo/pueblo.log (RE4_3DS_LOG=0: none, for live play).
#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "re4_runtime.h"
#include "re4_version.h"  // build3ds.py: RE4_3DS_VERSION (R1)

#ifndef RE4_3DS_RELEASE
#define RE4_3DS_RELEASE 0  // (R1) build3ds.py RE4_3DS_RELEASE=1: the release build
#endif

extern "C" int re4_gxq_start(void);  // gx_thread_3ds.cpp: the renderer, on the GX thread (core 2)
extern "C" int re4_3ds_memory_ok(void);
extern "C" void re4_3ds_memory_report(void);
extern "C" void re4_3ds_finish(int code);
extern "C" void re4_3ds_input_start(void);  // input_3ds.cpp: the 3DS buttons as the game's controller
extern "C" int re4_3ds_audio_start(void);   // audio_3ds.cpp: the game's sound through ndsp (stage 3.4)
extern "C" void re4_3ds_dsp_thread_start(void);  // audio_3ds.cpp: the DSP's mixing on core 1
extern "C" void re4_log_file_defer(int on);  // runtime/log.cpp
extern "C" void re4_log_file_drain(void);
extern "C" size_t re4_log_file_pending(void);
extern "C" void re4_3ds_settings_menu(void);  // settings_3ds.cpp (6.6): the menu before the game
extern "C" void re4_3ds_console_font(PrintConsole* console);
extern "C" void re4_3ds_hud_start(void);
extern "C" void re4_3ds_icons_start(void);  // (2026-11-27) icons_3ds.cpp: the inventory's item pictures
extern "C" void re4_3ds_movie_reserve(void);  // movie_3ds.cpp (R2b)
extern "C" void re4_dvd_weapon_arena(void* room, unsigned size);  // runtime/dvd.cpp (2026-11-27)
extern "C" unsigned re4_3ds_weapon_room_bytes(void);  // setup_3ds.cpp (2026-11-27)
extern "C" void re4_3ds_crash_start(void);    // crash_3ds.cpp (R1)
extern "C" int re4_3ds_check_system(void);    // startcheck_3ds.cpp (R3): the Old 3DS, the memory
extern "C" int re4_3ds_check_data(void);      // the discs, the movies, the texture pack; the warnings
extern "C" void re4_3ds_check_exit(void);     // the end after a check's screen

// FPSCR of the calling thread (stage 3.6): crt0 and threadCreate leave the OS kernel's value. RunFast:
// flush to zero (bit 24) and the default NaN (25); bits 22-23 the rounding; LEN/STRIDE (16-21) the vector mode
extern "C" void re4_3ds_log_fpscr(const char* who)
{
    u32 v;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(v));
    static const char* const kRound[4] = {"to nearest", "towards +inf", "towards -inf", "towards zero"};
    re4_log("3ds: FPSCR %s: 0x%08lx (flush to zero %d, default NaN %d, rounding %s, LEN/STRIDE bits 0x%02lx, "
            "trap enables 0x%02lx)",
            who, (unsigned long) v, (int) (v >> 24 & 1), (int) (v >> 25 & 1), kRound[v >> 22 & 3],
            (unsigned long) (v >> 16 & 0x37), (unsigned long) (v >> 8 & 0x9F));
}

namespace {

#if RE4_3DS_RELEASE
// (R1) the release: the title, the memory card in saves/ (runtime/memcard.cpp makes it,
// formatted and empty, when missing), no log, the owner's choices (the draw distance 70, the cutscenes' sound at the
// game's speed); the cheats, the gyro and the frame counter from the settings screen (setup_3ds.cpp); no texture
// misses. sdmc:/3ds/pueblo/pueblo.cfg read over them as in the other builds (hidden: not made, not described)
const char* const kDefaults[] = {
    "RE4_DATA_DIR=sdmc:/3ds/pueblo/data",
    "RE4_VI_FAST=0",
    "RE4_START_ROOM=",
    "RE4_PAD_UNTIL_ROOM=0",
    "RE4_ROOM_MENU=0",
    "RE4_PAD_SCRIPT=",
    "RE4_MEMCARD=sdmc:/3ds/pueblo/saves/memcard.bin",
    "RE4_MEMCARD_DEFER=1",
    "RE4_3DS_GX=1",
    "RE4_DEBUG_HUD=0",
    "RE4_3DS_DSPTHREAD=1",
    "RE4_3DS_DSPTHREAD_CORE=1",
    "RE4_DSP_BEHIND=1",
    // (2026-11-26) the sound: the stream's ARAM writes through the DSP lists' queue (the game does not wait for the DSP
    // thread at a refill), the streams' file kept open, a new stream read ahead from its start (runtime/dsp.cpp, dvd.cpp);
    // recording 16: refill frames +4.2 -> +1.5 ms, the game's heavy frames -0.5 ms. 0: as before
    "RE4_SND_ARAMQ=1",
    "RE4_DVD_STRPIN=1",
    "RE4_DVD_STRHINT=3",
    "RE4_EVT_SNDSPEED=1",
    "RE4_DRAWDIST=70",
    "RE4_3DS_TEXMISS=0",
    "RE4_3DS_HUD=3",  // (the frame counter: the settings screen, 4)
    // the log only with RE4_3DS_LOG=1 in pueblo.cfg (a tester's); then as in the other builds
    "RE4_3DS_LOG_FLUSH=0",
    "RE4_LOG_FILE_RESERVE=1048576",
    "RE4_3DS_LOG=0",
    "RE4_3DS_CHECKS=2",  // (R3) the start's checks with their warnings (the other builds: the end only)
    "RE4_3DS_GPUBUSY=0",  // (2026-11-18) the GPU's busy sampler: 1000 wake-ups a second on core 1, for the log only
    0,
};
#else
const char* const kDefaults[] = {
    "RE4_DATA_DIR=sdmc:/3ds/pueblo/data",
    // the game's clock in real time (stage 3.4: the sound; RE4_VI_FAST=1 runs it as fast as the game goes: slower in
    // r100, faster in the menus); the measured routes set RE4_VI_FAST=1 in pueblo.cfg
    "RE4_VI_FAST=0",
    "RE4_START_ROOM=0x100",
    "RE4_PAD_UNTIL_ROOM=1",
    "RE4_MEMCARD=none",
    // a card file (RE4_MEMCARD=sdmc:/3ds/pueblo/card-a.bin, re4-play.cfg) written in one piece a second after the save
    // (stage 6.3, runtime/memcard.cpp): small writes to the SD card cost tens of ms each
    "RE4_MEMCARD_DEFER=1",
    "RE4_ROOM_MENU=1",
    "RE4_PAD_SCRIPT=UP@100-60000/60 A@115-60000/60 START@130-60000/60",
    "RE4_3DS_GX=1",  // the picture on the top screen (gx_3ds.cpp, stage 3.3a); 0: the core alone
    // the debug build's own displays off, as the Android app has it (stage 5.4: the process time bars flickered at the
    // picture's left and right edges, src/game/debug.cpp processBarDisp; state_trace.cpp)
    "RE4_DEBUG_HUD=0",
    // the log onto the SD card every 250 ms; N: every N seconds (stage 6.2e, logWriter below)
    "RE4_3DS_LOG_FLUSH=0",
    // the log file 1 MB longer with zeros when it opens, before the game (stage 6.2e, runtime/log.cpp gReserve)
    "RE4_LOG_FILE_RESERVE=1048576",
    // the DSP's mixing on a thread (stage 3.6, audio_3ds.cpp re4_3ds_dsp_thread_start); RE4_3DS_DSPTHREAD=0: on the
    // game's thread, as before. Its lists behind the game (stage 3.4b, runtime/dsp.cpp RE4_DSP_BEHIND: AX does not
    // wait for them; the aux reverb goes there too), on core 1: the village -1.0..-1.9 ms a frame against the lists
    // in step on core 2 (r101-3ds-behind4.log vs axparts1); behind on core 2 the GX thread lost its idle (behind2/3).
    // RE4_DSP_BEHIND=0, RE4_3DS_DSPTHREAD_CORE=2: as before (r101-3ds-dspcore2.log)
    "RE4_3DS_DSPTHREAD=1",
    "RE4_3DS_DSPTHREAD_CORE=1",
    "RE4_DSP_BEHIND=1",
    // (2026-11-26) the sound: the stream's ARAM writes through the DSP lists' queue (the game does not wait for the DSP
    // thread at a refill), the streams' file kept open, a new stream read ahead from its start (runtime/dsp.cpp, dvd.cpp);
    // recording 16: refill frames +4.2 -> +1.5 ms, the game's heavy frames -0.5 ms. 0: as before
    "RE4_SND_ARAMQ=1",
    "RE4_DVD_STRPIN=1",
    "RE4_DVD_STRHINT=3",
    0,
};
#endif  // RE4_3DS_RELEASE

void setting(const char* line)
{
    const char* eq = strchr(line, '=');
    if (!eq || eq == line) {
        return;
    }
    char name[64];
    size_t n = (size_t) (eq - line) < sizeof(name) - 1 ? (size_t) (eq - line) : sizeof(name) - 1;
    memcpy(name, line, n);
    name[n] = 0;
    setenv(name, eq + 1, 1);
}

// The log's writer (stage 3.4): a line written to the SD card took about 12 ms of the game's thread, a frame report
// 300 ms (heard as a break in the sound); the lines collect in memory and this thread writes them, on core 1 (its
// share is asked for by audio_3ds.cpp), else on core 0 below the game. RE4_3DS_LOG_THREAD=0: each line at once, as
// before. Stage 6.2e: a write holds the game's reads of the SD card (the FS service does one request at a time):
// about 1.2 s now and then as the old log file grew (bench5, r101-3ds-sdstall1.log: a read of em15.drs waited
// 1.2 s); with the file's room made before the game (runtime/log.cpp gReserve) 25-150 ms (r101-3ds-sdstall5.log).
// RE4_3DS_LOG_FLUSH=N: every N seconds instead, or at kLogFlushBytes collected; at the end (re4_3ds_finish, abort,
// exit) in any case. A freeze or a data abort loses what was not written.
const size_t kLogFlushBytes = 256 << 10;
u64 gLogFlushTicks;

void logWriter(void*)
{
    u64 last = svcGetSystemTick();
    for (;;) {
        svcSleepThread(250000000LL);
        if (svcGetSystemTick() - last >= gLogFlushTicks || re4_log_file_pending() >= kLogFlushBytes) {
            re4_log_file_drain();
            last = svcGetSystemTick();
        }
    }
}

void startLogWriter()
{
    const char* v = getenv("RE4_3DS_LOG_THREAD");
    if (v && *v == '0') {
        return;
    }
    const char* flush = getenv("RE4_3DS_LOG_FLUSH");
    const int seconds = flush ? atoi(flush) : 0;
    gLogFlushTicks = seconds > 0 ? (u64) seconds * SYSCLOCK_ARM11 : 0;
    int core = 1;
    Thread t = threadCreate(logWriter, nullptr, 16 * 1024, 0x30, 1, true);
    if (!t) {
        core = 0;
        t = threadCreate(logWriter, nullptr, 16 * 1024, 0x3F, 0, true);
    }
    if (t) {
        re4_log_file_defer(1);
        if (seconds > 0) {
            re4_log("3ds: the log is written by a thread on core %d, every %d s or at %u KB (RE4_3DS_LOG_FLUSH)", core,
                    seconds, (unsigned) (kLogFlushBytes >> 10));
        } else {
            re4_log("3ds: the log is written by a thread on core %d, every 250 ms", core);
        }
    }
}

const char kSettings[] = "sdmc:/3ds/pueblo/pueblo.cfg";
bool gSettingsFile;

// Before every other static constructor (priority 101; 3dsx.ld sorts .init_array.* first): the
// runtime reads some settings in its own (state_trace.cpp Install: RE4_STOP_AFTER, RE4_TEST_PLACE,
// RE4_PAD_UNTIL_ROOM). libctru has the SD card open by then (__appInit runs before them).
__attribute__((constructor(101))) void readSettings()
{
    for (const char* const* d = kDefaults; *d; d++) {
        setting(*d);
    }
#if !RE4_3DS_RELEASE  // (R1) not in the release: the settings screen before the gameplay instead (R2)
    re4_3ds_settings_menu();  // L held at the start (or RE4_3DS_MENU=1): the menu writes the file first (6.6)
#endif
    FILE* f = fopen(kSettings, "r");
    if (!f) {
        return;
    }
    gSettingsFile = true;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] && line[0] != '#') {
            setting(line);
        }
    }
    fclose(f);
}

// (R4) The European discs' language (runtime/game_language.cpp reads RE4_GAME_LANGUAGE) from the system's language
// when pueblo.cfg sets none: German, French, Spanish, Italian, else English (the GameCube's setLanguage does the same
// from OSGetLanguage). After readSettings (priority 101); the US discs ignore it.
const char* gLanguageFrom = "pueblo.cfg";
__attribute__((constructor(102))) void systemLanguage()
{
    u8 lang = CFG_LANGUAGE_EN;
    if (R_SUCCEEDED(cfguInit())) {
        if (R_FAILED(CFGU_GetSystemLanguage(&lang))) {
            lang = CFG_LANGUAGE_EN;
        }
        cfguExit();
    }
    // (PuebloCTR pull request 3, DeanCharpen: the bottom screen - a simplified Chinese system gets its Chinese strings;
    // the discs have no Chinese, only the port's screens do; pueblo.cfg's RE4_3DS_LANG, read before this, keeps the
    // last word)
    if (lang == CFG_LANGUAGE_ZH && !getenv("RE4_3DS_LANG")) {
        setenv("RE4_3DS_LANG", "chs", 1);
    }
    if (getenv("RE4_GAME_LANGUAGE")) {
        return;
    }
    const char* v = lang == CFG_LANGUAGE_DE ? "ger" : lang == CFG_LANGUAGE_FR ? "fra" : lang == CFG_LANGUAGE_ES ? "esp"
                  : lang == CFG_LANGUAGE_IT ? "ita" : "eng";
    setenv("RE4_GAME_LANGUAGE", v, 1);
    static char from[32];
    snprintf(from, sizeof(from), "the system's language %u", (unsigned) lang);
    gLanguageFrom = from;
}

// The New 3DS speed-up (stage 3.6): libctru's osSetSpeedupEnable says nothing when ptm:sysm is not to be had
// (the codes are not kept), so they are asked for again here and the clock looked at: a chain of 32 dependent
// adds (single issue, about 34 cycles a pass) and a walk through 1 MB at random, a cache line a step (beyond
// the L1 data cache, inside the L2 of 2 MB); first as the start left it, then in the modes of
// PTMSYSM_ConfigureNew3DSCPU 0 (268 MHz), 1 (804 MHz), 3 (804 MHz and the L2), then 3 again.
// svcGetSystemTick runs at 268 MHz in each.
volatile u32 gSpeedSink;
// (written out: clang takes a .rept for one instruction, and the double constants' vldr went out of reach)
#define RE4_ADD1 "add %1, %1, #1\n"
#define RE4_ADD8 RE4_ADD1 RE4_ADD1 RE4_ADD1 RE4_ADD1 RE4_ADD1 RE4_ADD1 RE4_ADD1 RE4_ADD1

double addsPass()  // ns a pass
{
    const u32 passes = 200000;
    u32 n = passes, x = 1;
    const u64 t0 = svcGetSystemTick();
    __asm__ volatile("1:\n" RE4_ADD8 RE4_ADD8 RE4_ADD8 RE4_ADD8
                     "subs %0, %0, #1\n"
                     "bne 1b\n"
                     : "+r"(n), "+r"(x)
                     :
                     : "cc");
    const u64 t = svcGetSystemTick() - t0;
    gSpeedSink = x;
    return t * 1e9 / SYSCLOCK_ARM11 / passes;
}

double walkStep(const u32* next)  // ns a step
{
    const u32 steps = 100000;
    u32 i = 0;
    for (u32 s = 0; s < steps; s++) {  // warm: the lines into the caches
        i = next[i];
    }
    const u64 t0 = svcGetSystemTick();
    for (u32 s = 0; s < steps; s++) {
        i = next[i];
    }
    const u64 t = svcGetSystemTick() - t0;
    gSpeedSink = i;
    return t * 1e9 / SYSCLOCK_ARM11 / steps;
}

void speedupReport()
{
    const u32 lines = (1u << 20) / 32, words = 8;  // a u32 a 32-byte line
    u32* next = (u32*) malloc((size_t) lines * 32);
    u32* order = (u32*) malloc((size_t) lines * 4);
    if (!next || !order) {
        free(next);
        free(order);
        re4_log("3ds: speed-up check: no 1 MB for it");
        return;
    }
    for (u32 k = 0; k < lines; k++) {
        order[k] = k;
    }
    u32 seed = 12345;
    for (u32 k = lines - 1; k > 0; k--) {  // Sattolo: one cycle through all the lines
        seed = seed * 1664525u + 1013904223u;
        const u32 j = (u32) (((u64) seed * k) >> 32);
        const u32 t = order[k];
        order[k] = order[j];
        order[j] = t;
    }
    for (u32 k = 0; k < lines; k++) {
        next[order[k] * words] = order[(k + 1) % lines] * words;
    }
    free(order);
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    const double adds = addsPass(), walk = walkStep(next);
    const Result init = ptmSysmInit();
    Result codes[3] = {0, 0, 0};
    double modeAdds[3] = {0, 0, 0}, modeWalk[3] = {0, 0, 0};
    if (R_SUCCEEDED(init)) {
        static const u8 kModes[3] = {0, 1, 3};
        for (int m = 0; m < 3; m++) {
            codes[m] = PTMSYSM_ConfigureNew3DSCPU(kModes[m]);
            modeAdds[m] = addsPass();
            modeWalk[m] = walkStep(next);
        }
        PTMSYSM_ConfigureNew3DSCPU(3);
        ptmSysmExit();
    }
    osSetSpeedupEnable(true);
    free(next);
    re4_log("3ds: speed-up (New 3DS %d): at the start adds %.1f ns a pass (%.1f cycles if 804 MHz; about 34 there), a step "
            "through 1 MB %.1f ns; ptmSysmInit 0x%08lx; modes 0/1/3 (codes 0x%08lx 0x%08lx 0x%08lx): adds %.1f / %.1f / %.1f "
            "ns, steps %.1f / %.1f / %.1f ns",
            (int) isNew, adds, adds * 0.804, walk, (unsigned long) init, (unsigned long) codes[0], (unsigned long) codes[1],
            (unsigned long) codes[2], modeAdds[0], modeAdds[1], modeAdds[2], modeWalk[0], modeWalk[1], modeWalk[2]);
}

}  // namespace

// (stream read-ahead, runtime/dvd.cpp) The block after a sound stream's refill is read here, on core 1 like the
// log's writer, not by the game's thread inside its audio frame (8-11 ms of the SD card about every 0.8 s).
// Without a thread on core 1: as before. RE4_DVD_AHEAD=0: off.
extern "C" void re4_dvd_ahead_start(void (*wake)(void));
extern "C" void re4_dvd_ahead_work(void);
static LightEvent gAheadEvent;

static void aheadWake(void)
{
    LightEvent_Signal(&gAheadEvent);
}

static void aheadReader(void*)
{
    for (;;) {
        LightEvent_Wait(&gAheadEvent);
        re4_dvd_ahead_work();
    }
}

static void startAheadReader()
{
    LightEvent_Init(&gAheadEvent, RESET_ONESHOT);
    if (threadCreate(aheadReader, nullptr, 32 * 1024, 0x30, 1, true)) {
        re4_dvd_ahead_start(aheadWake);
    } else {
        re4_log("3ds: no thread on core 1 for the stream read-ahead: the game's thread reads as before");
    }
}

int main(void)
{
    gfxInitDefault();
    const char* gx = getenv("RE4_3DS_GX");
    const bool picture = gx && *gx == '1';
    re4_3ds_console_font(consoleInit(picture ? GFX_BOTTOM : GFX_TOP, 0));  // with Cyrillic (6.6)
    consoleDebugInit(debugDevice_NULL);  // the core's log (stderr) into the file only: text output is slow
    osSetSpeedupEnable(true);  // New 3DS: 804 MHz and the L2 cache
#if RE4_3DS_RELEASE
    printf("PuebloCTR %s\n\n", RE4_3DS_VERSION);  // (R1)
#else
    printf(picture ? "RE4 core on the 3DS: the picture on the top screen\n" : "RE4 core on the 3DS: game logic, no graphics\n");
#endif
    if (!re4_3ds_check_system()) {  // (R3) an Old 3DS, too little memory: said on the bottom screen
        re4_3ds_check_exit();
    }
    re4_log("3ds: the game's language with the European discs: %s (from %s)", getenv("RE4_GAME_LANGUAGE"), gLanguageFrom);
#if !RE4_3DS_RELEASE
    printf(gSettingsFile ? "settings: the r100 defaults and %s\n" : "settings: the r100 defaults (no %s)\n", kSettings);
    printf("controls: circle pad, C stick, A B X Y, L knife, R aim, ZR map, START;\n"
           "START + SELECT for a second: exit\n");
#endif
    const char* logOn = getenv("RE4_3DS_LOG");
    const bool log = !(logOn && *logOn == '0');
    if (log) {
        re4_log_file("sdmc:/3ds/pueblo/pueblo.log");
    } else {
        printf("no log (RE4_3DS_LOG=0)\n");
    }
    re4_3ds_memory_report();
    speedupReport();
    re4_3ds_log_fpscr("main thread (the game) at the start");
    mkdir("sdmc:/3ds/pueblo/saves", 0777);  // (R1) the release's memory card folder (there already: nothing)
    re4_3ds_crash_start();                // (R1) sdmc:/3ds/pueblo/crash.txt; the CPU's faults to it
    if (!re4_3ds_check_data()) {  // (R3) the discs, the movies, the texture pack: before any thread of the game
        re4_3ds_check_exit();
    }
    if (picture && !re4_gxq_start()) {
        re4_log("3ds: the GX renderer did not start");
        printf("the GX renderer did not start\n");
    }
    re4_3ds_audio_start();
    re4_3ds_dsp_thread_start();
    if (log) {
        startLogWriter();
    }
    startAheadReader();
    re4_set_data_dir(getenv("RE4_DATA_DIR"));
    // as re4_host_main: the address checks know the image (RE4_IS_ADDR) before the game runs
    re4_check_game_address("re4.3dsx", (const void*) &re4_game_main);
    re4_static_range_init((const void*) &re4_game_main);
    re4_3ds_movie_reserve();  // (R2b) the movies' work room before the textures take linear memory
    re4_3ds_input_start();
    re4_3ds_icons_start();  // (2026-11-27) the item pictures, once, before the panel and the game (their own screen)
    re4_3ds_hud_start();
    {  // (2026-11-27) the D-pad weapons' files: their room of linear memory now (the game not begun: later its allocator
       // would race the GX thread's - it has no lock), only with weapons on the arrows (settings.bin, read by
       // re4_3ds_hud_start; the room costs the textures' cache 3 MB: +0.4 ms of a heavy frame, recording 16) - the
       // user's choice; else after a movie once weapons are assigned (movie_3ds.cpp). RE4_WEP_CACHE=KB (3072), 0: never
        const unsigned bytes = re4_3ds_weapon_room_bytes();
        void* room = bytes ? linearAlloc(bytes) : nullptr;
        re4_dvd_weapon_arena(room, room ? bytes : 0);
        re4_log("3ds: the weapon files' room: %u KB%s (linear %u KB free)", room ? bytes >> 10 : 0,
                bytes && !room ? " - NOT taken" : bytes ? "" : " (no weapons on the arrows)",
                (unsigned) (linearSpaceFree() >> 10));
    }
    re4_log("starting re4_game_main");
#if RE4_3DS_RELEASE
    printf("starting...\n");
#else
    printf("running (frame times every 300 frames)...\n");
#endif
    int ret = re4_os_run(re4_game_main);
    re4_log("re4_game_main returned %d", ret);
    re4_3ds_finish(ret);
    gfxExit();
    return 0;
}
