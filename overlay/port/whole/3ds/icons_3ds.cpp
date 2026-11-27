// (2026-11-27) The bottom screen's inventory: the case's item pictures (3ds/item_icons.cpp) - the user's choice: drawn
// by the game itself once, at the first start, before the game begins (the user: the player waits once and sees the
// progress - "Preparing item pictures" and a bar on the bottom screen; the C heap still free then, core 0 at full speed;
// a first try on a background thread ran out of memory once the game had begun). The inventory fills the screen above
// the strip, so a cell is as large as the case allows: 29 px S, 25 M, 22 L, 20 XL (the D-pad weapons' list takes the
// 20 px pictures) - a file sdmc:/3ds/pueblo/icons-NN.bin for each, made from this disc's ss_pzzl.dat (its size and
// FNV-1a in the head; the sizes of the game's piece_info, runtime/cheats.cpp re4_case_piece_sizes). The panel's thread
// reads only the pictures of the items it draws (re4_3ds_icon; ≈ 0.3 MB at most).
// RE4_3DS_ICONS=0: none (the inventory draws boxes), 2: drawn anew at each start (the files ignored).
#include <3ds.h>

#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "item_icons.h"

extern "C" {
void re4_log(const char* fmt, ...);
void re4_ui_prep(unsigned short* fb, int percent);  // ui_bottom_3ds.cpp: the progress screen
int re4_case_piece_sizes(unsigned short* ids, unsigned char* sx, unsigned char* sy, int max);  // runtime/cheats.cpp
}

namespace {

const int kSizes = 4;
const int kPpc[kSizes] = {29, 25, 22, 20};  // (the cases S, M, L, XL: their order of making)
const int kSamples = 2;
std::atomic<int> gHave[kSizes];             // 1: icons-NN.bin is this disc's
std::atomic<int> gDone(0), gBusy(0);        // the sizes made / looked at; 1 while the thread works

struct Head {
    char magic[4];  // "PICF"
    unsigned srcSize, srcHash, blobSize;
};

unsigned fnv(const unsigned char* p, size_t n)
{
    unsigned h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

void pathOf(int ppc, char* out, size_t n)
{
    snprintf(out, n, "sdmc:/3ds/pueblo/icons-%d.bin", ppc);
}

// the case archive of the disc on the SD card (its piece models are one for every language of an edition)
unsigned char* readPzzl(size_t* size)
{
    static const char* const kLang[] = {"Eng", "Jpn", "Fra", "Ger", "Ita", "Esp"};
    const char* dir = getenv("RE4_DATA_DIR");
    char path[256];
    FILE* f = nullptr;
    for (int i = 0; i < 6 && !f; i++) {
        snprintf(path, sizeof(path), "%s/disc0/files/ss/%s/ss_pzzl.dat", dir ? dir : "sdmc:/3ds/pueblo/data", kLang[i]);
        f = fopen(path, "rb");
    }
    if (!f) {
        re4_log("3ds: item pictures: no ss/*/ss_pzzl.dat in the data - the inventory draws boxes");
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* p = n > 0 ? (unsigned char*) malloc((size_t) n) : nullptr;
    size_t got = 0;
    while (p && got < (size_t) n) {  // (in pieces: the SD card's service takes one request at a time)
        const size_t k = fread(p + got, 1, (size_t) n - got < (64u << 10) ? (size_t) n - got : (64u << 10), f);
        if (k == 0) {
            break;
        }
        got += k;
        svcSleepThread(1000000LL);
    }
    fclose(f);
    if (!p || got != (size_t) n) {
        free(p);
        re4_log("3ds: item pictures: %s not read", path);
        return nullptr;
    }
    *size = got;
    return p;
}

bool fileIsThis(int ppc, size_t srcSize, unsigned hash)
{
    char path[64];
    pathOf(ppc, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    Head h;
    const bool ok = fread(&h, 1, sizeof(h), f) == sizeof(h) && !memcmp(h.magic, "PICF", 4) && h.srcSize == srcSize &&
                    h.srcHash == hash && h.blobSize > 12;
    fclose(f);
    return ok;
}

// the progress screen on the bottom screen (the top one black), redrawn when the percent moves
struct Progress {
    int base, total, last;
};

void showProgress(int percent)
{
    u16 w, h;
    u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
    re4_ui_prep((unsigned short*) fb, percent);
    GSPGPU_FlushDataCache(fb, (u32) w * h * 2);
    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
    gspWaitForVBlank();
}

void step(int done, void* ctx)
{
    Progress* p = (Progress*) ctx;
    const int percent = (p->base + done) * 100 / (p->total > 0 ? p->total : 1);
    if (percent != p->last) {
        p->last = percent;
        showProgress(percent);
    }
}

void work(void*)
{
    const char* e = getenv("RE4_3DS_ICONS");
    const bool fresh = e && *e == '2';
    size_t pn = 0;
    unsigned char* pzzl = readPzzl(&pn);
    if (!pzzl) {
        gBusy.store(0);
        return;
    }
    const unsigned hash = fnv(pzzl, pn);
    static unsigned short ids[128];
    static unsigned char sx[128], sy[128];
    static Re4IconItem items[128];
    const int n = re4_case_piece_sizes(ids, sx, sy, 128);
    for (int i = 0; i < n; i++) {
        items[i] = Re4IconItem{ids[i], sx[i], sy[i]};
    }
    bool need[kSizes];
    int missing = 0;
    for (int s = 0; s < kSizes; s++) {
        need[s] = fresh || !fileIsThis(kPpc[s], pn, hash);
        missing += need[s] ? 1 : 0;
        if (!need[s]) {
            gHave[s].store(1, std::memory_order_release);
            gDone.fetch_add(1);
        }
    }
    Progress prog = {0, missing * n, -1};
    if (missing) {
        u16 tw, th;  // (the top screen black: whatever its framebuffers held)
        for (int i = 0; i < 2; i++) {
            u8* top = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &tw, &th);
            memset(top, 0, (size_t) tw * th * 3);
            gfxFlushBuffers();
            gfxScreenSwapBuffers(GFX_TOP, false);
            gspWaitForVBlank();
        }
        showProgress(0);
    }
    for (int s = 0; s < kSizes; s++) {
        if (!need[s]) {
            continue;
        }
        const u64 t0 = svcGetSystemTick();
        size_t bn = 0;
        unsigned char* blob = re4_icons_render(pzzl, pn, items, n, kPpc[s], kSamples, &bn, step, &prog);
        prog.base += n;
        const u64 t1 = svcGetSystemTick();
        if (!blob) {
            re4_log("3ds: item pictures: the case archive not as expected - the inventory draws boxes");
            break;
        }
        char path[64];
        pathOf(kPpc[s], path, sizeof(path));
        Head h = {{'P', 'I', 'C', 'F'}, (unsigned) pn, hash, (unsigned) bn};
        FILE* f = fopen(path, "wb");
        bool ok = f && fwrite(&h, 1, sizeof(h), f) == sizeof(h);
        for (size_t at = 0; ok && at < bn; at += 64u << 10) {  // (in pieces, the game's reads between)
            const size_t k = bn - at < (64u << 10) ? bn - at : (64u << 10);
            ok = fwrite(blob + at, 1, k, f) == k;
        }
        if (f) {
            fclose(f);
        }
        free(blob);
        re4_log("3ds: item pictures %d px a cell: %d items, %u KB, drawn in %u ms, %s %s", kPpc[s], n,
                (unsigned) (bn >> 10), (unsigned) ((t1 - t0) / (SYSCLOCK_ARM11 / 1000)), ok ? "kept in" : "NOT written to",
                path);
        if (ok) {
            gHave[s].store(1, std::memory_order_release);
        }
        gDone.fetch_add(1);
    }
    free(pzzl);
    gBusy.store(0);
}

// --- the panel's thread: the pictures of the items drawn, from the file of the case's size ---
int gOpenPpc;             // the file open (0 none)
FILE* gFile;
unsigned char* gToc;      // its table: count entries of 12 bytes
unsigned gCount;
struct Kept {
    unsigned short id;
    unsigned char side;
    unsigned short w, h;
    unsigned char* px;
};
Kept gKept[64];
int gKeptN;
size_t gKeptBytes;

void dropKept()
{
    for (int i = 0; i < gKeptN; i++) {
        free(gKept[i].px);
    }
    gKeptN = 0, gKeptBytes = 0;
}

bool openSize(int ppc)
{
    if (gOpenPpc == ppc) {
        return gFile != nullptr;
    }
    dropKept();
    if (gFile) {
        fclose(gFile);
    }
    free(gToc);
    gFile = nullptr, gToc = nullptr, gCount = 0, gOpenPpc = ppc;
    char path[64];
    pathOf(ppc, path, sizeof(path));
    gFile = fopen(path, "rb");
    unsigned char head[sizeof(Head) + 12];
    if (!gFile || fread(head, 1, sizeof(head), gFile) != sizeof(head) || memcmp(head + sizeof(Head), "PIC1", 4)) {
        re4_log("3ds: item pictures: %s not readable", path);
        if (gFile) {
            fclose(gFile);
        }
        gFile = nullptr;
        return false;
    }
    memcpy(&gCount, head + sizeof(Head) + 8, 4);
    gCount = gCount > 512 ? 0 : gCount;
    gToc = (unsigned char*) malloc(12 * gCount + 1);
    if (!gToc || fread(gToc, 1, 12 * gCount, gFile) != 12 * gCount) {
        gCount = 0;
    }
    return gCount != 0;
}

}  // namespace

// main_3ds.cpp, after the start's checks, before the panel and the game
extern "C" void re4_3ds_icons_start(void)
{
    const char* e = getenv("RE4_3DS_ICONS");
    if (e && *e == '0') {
        re4_log("3ds: item pictures off (RE4_3DS_ICONS=0)");
        return;
    }
    gBusy.store(1);
    work(nullptr);  // (at once, before the game: the player waits once and sees the progress)
}

// The pictures' making: 0..100 percent done (the sizes made), -1 none under way and none to come (made or failed)
extern "C" int re4_3ds_icons_progress(void)
{
    return gBusy.load() ? gDone.load() * 100 / kSizes : -1;
}

// The panel's thread: the picture of item `id` at `ppc` pixels a cell (one of 29, 25, 22, 20), `side` 0 front / 1 back:
// w * h RGB565, then w * h alpha bytes; null while that size is not made or for an item without one
extern "C" const unsigned char* re4_3ds_icon(int ppc, int id, int side, int* w, int* h)
{
    int s = 0;
    while (s < kSizes && kPpc[s] != ppc) {
        s++;
    }
    if (s == kSizes || !gHave[s].load(std::memory_order_acquire) || !openSize(ppc)) {
        return nullptr;
    }
    for (int i = 0; i < gKeptN; i++) {
        if (gKept[i].id == id && gKept[i].side == side) {
            *w = gKept[i].w, *h = gKept[i].h;
            return gKept[i].px;
        }
    }
    for (unsigned i = 0; i < gCount; i++) {
        const unsigned char* t = gToc + 12 * i;
        if ((t[0] | t[1] << 8) != id || t[2] != side) {
            continue;
        }
        unsigned off;
        memcpy(&off, t + 8, 4);
        const int pw = t[4] | t[5] << 8, ph = t[6] | t[7] << 8;
        const size_t bytes = (size_t) pw * ph * 3;
        if (gKeptN == 64 || gKeptBytes + bytes > (320u << 10)) {
            dropKept();  // (full: begun anew - the items of one screen fit)
        }
        unsigned char* px = (unsigned char*) malloc(bytes);
        if (!px || fseek(gFile, (long) (sizeof(Head) + off), SEEK_SET) || fread(px, 1, bytes, gFile) != bytes) {
            free(px);
            return nullptr;
        }
        gKept[gKeptN++] = Kept{(unsigned short) id, (unsigned char) side, (unsigned short) pw, (unsigned short) ph, px};
        gKeptBytes += bytes;
        *w = pw, *h = ph;
        return px;
    }
    return nullptr;
}
