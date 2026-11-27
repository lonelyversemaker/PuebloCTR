// The first renderer of the 3DS port (stage 3.3a, docs 09-stage3-gx.md of the port research): the
// draws of the core's GX decoder drawn with citro3d on the top screen, as a reference picture
// before the direct path (3.3b/c). Each vertex goes through the GX position matrix it names and the
// GX projection; the depth test and the blending of the BP registers (0x40, 0x41); the TEV stages
// as PICA texture combiners (tev(): a GX stage d +- lerp(a, b, c) in one or two of the six PICA
// stages, its constants, registers and konstant colours, up to three textures) and the alpha test.
// The colour channel as XF lights it, per vertex in the shader (stage 3.9: setChannel). Textures: gx_texture.cpp to RGBA8, to 16 bits, PICA tiles, in linear memory, kept by
// the decoder's content key (in VRAM each upload is a GX command, and the queue of citro3d, not run
// during a frame, overflowed on the title screen: svcBreak in gxCmdQueueAdd). No texture matrices,
// texture coordinate generation, indirect stages, viewport or EFB copies yet.
//
// The decoder hands each draw over as it meets it (runtime/re4_gx_direct.h), on the game's
// thread: the chunks of re4_gx_frame.h hold a frame's vertices (~9 MB in r100), which the 3DS's
// heap does not have (the first try, 01.11.2026: the program ended entering r100). The decoder
// converts the vertices straight into a staging area in linear memory; when it is full, the frame
// is ended without showing it (C3D_FrameBegin waits for the GPU) and drawing goes on into the same
// target. A display copy ends the frame and shows it.
#include <3ds.h>
#include <citro3d.h>

#include <algorithm>
#include <malloc.h>
#include <math.h>
#include <map>
#include <mutex>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re4_gx_direct.h"
#include "re4_runtime.h"

#include "gx_3ds_shbin.h"  // build3ds.py: picasso gx_3ds.v.pica
#include "gx_texture.h"
#include "pica_texture.h"
#include "re4_movie_hw.h"
#include "re4_tex_dump.h"

extern "C" volatile u64 re4_hud_gx_busy;  // (stage98) gx_thread_3ds.cpp: the GX thread's busy ticks so far

// (2026-11-24) textures ahead: runtime/dvd.cpp calls it as the game opens a file (its path below files/)
extern "C" void (*re4_dvd_open_fn)(const char* path);
extern "C" void re4_3ds_tex_preload_open(const char* path);

extern "C" volatile u32 re4_hud_gpu_busy, re4_hud_gpu_all;
extern "C" int re4_occ_on;  // (occlusion cull, the end of this file)
extern "C" volatile int re4_occ_sparse;
extern "C" int re4_gxq_on;
extern "C" void re4_gxq_fn(void (*fn)(uint32_t, uint32_t), uint32_t a, uint32_t b);  // settings_3ds.cpp (6.6): the panel's, from the start

// citro3d 1.7.1 (source/internal.h): its context begins with the GX command queue, which
// C3D_FrameBegin stops and only C3D_FrameEnd runs; the renderer runs it inside a frame (kick()).
extern "C" gxCmdQueue_s __C3D_Context;
extern "C" u32 __ctru_linear_heap, __ctru_linear_heap_size;
extern "C" void C3Di_UpdateContext(void);  // citro3d 1.7.1: its state into commands (C3D_DrawElements calls it first)

// (X, review 2026-11-24 finding 4) vramSpaceFree() walks the free lists of both VRAM pools, and toVram asked it at
// every use of a texture still in linear memory (~360 a picture on bench7, nearly all to hear "no room"). The total is
// kept until the next vramAlloc / vramFree of anyone: the linker's --wrap (build3ds.py) brings this file's calls and
// citro3d's here. RE4_3DS_VRAMKEPT=0: asked every time, as before; 2: kept and checked against the walk
extern "C" {
void* __real_vramAlloc(size_t size);
void* __real_vramAllocAt(size_t size, vramAllocPos pos);
void* __real_vramMemAlign(size_t size, size_t alignment);
void* __real_vramMemAlignAt(size_t size, size_t alignment, vramAllocPos pos);
void __real_vramFree(void* mem);
u32 re4_vram_epoch = 1;
void* __wrap_vramAlloc(size_t size)
{
    re4_vram_epoch++;
    return __real_vramAlloc(size);
}
void* __wrap_vramAllocAt(size_t size, vramAllocPos pos)
{
    re4_vram_epoch++;
    return __real_vramAllocAt(size, pos);
}
void* __wrap_vramMemAlign(size_t size, size_t alignment)
{
    re4_vram_epoch++;
    return __real_vramMemAlign(size, alignment);
}
void* __wrap_vramMemAlignAt(size_t size, size_t alignment, vramAllocPos pos)
{
    re4_vram_epoch++;
    return __real_vramMemAlignAt(size, alignment, pos);
}
void __wrap_vramFree(void* mem)
{
    re4_vram_epoch++;
    __real_vramFree(mem);
}
}

namespace {

// 2 MB until the display list cache (3.3b) took the models, 1 MB until its dynamic lists were split (3.8)
const u32 kStageBytes = 768u << 10;
const u32 kStageIndices = 128u << 10;
const u32 kCmdBufBytes = 1u << 20;
const u32 kCmdBufReserve = 2048;  // words left free before a frame is split

const u32 kDisplayTransfer = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                             GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                             GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

bool gOn;
C3D_RenderTarget* gTarget;
DVLB_s* gDvlb;
shaderProgram_s gProgram;
int gLocRows, gLocProj, gLocColor, gLocTexSel, gLocLights, gLocConst, gLocFar, gLocBrief, gLocWhole;
float gFarLight = 50000.0f;  // RE4_3DS_FARLIGHT: the distance from which a light may be taken as far (0: none)
u32 gFarFrom;  // RE4_3DS_FARFROM: far lights from picture N on (two log windows of one run compared)
u8* gStage;
u32 gStageUsed;
u16* gIndices;
u32 gIndicesUsed;

bool gInFrame;
bool gKeeping = true;  // RE4_3DS_DLCACHE=0: no display list cache (gx_decode.cpp keptList)
std::vector<void*> gDropped;  // lasting room the decoder let go: freed once the GPU is past it
u32 gKeptLive;                 // bytes of lasting room taken (until freed)
const u32 kKeepReserve = 3072u << 10;  // the decoder's budget (kKeptBudget)
// The display list cache's own room (stage 5.6): one block of linear memory taken at the start, its blocks given out
// first fit (neighbours joined when freed). Before, its ~450 lasting blocks lay among the textures: the game's font
// (512 KB) found no room with 4 MB free (r100-3ds-audio3.log). The budget (kKeptBudget, 3328 KB) counts no block's
// 256 bytes past the vertices nor the alignment. RE4_3DS_KEEPARENA=0: linearAlloc as before.
// Stage 5.7b: with the blocks moved to VRAM (RE4_3DS_KEPTMOVE) the room holds those not yet copied there and those
// with no room in VRAM: RE4_3DS_KEEPARENAKB, by default kArenaMoved (the rest of linear memory to the textures)
const u32 kArenaAlign = 128, kArenaMoved = 1536u << 10;
u32 kArenaBytes = 3840u << 10;
u8* gArena;
std::map<u32, u32> gArenaFree;                    // offset -> bytes, in order
std::unordered_map<const void*, u32> gArenaUsed;  // block -> bytes
u32 gArenaFailed;                                 // blocks with no room in it, per log window

void* arenaAlloc(u32 bytes)
{
    bytes = (bytes + kArenaAlign - 1) & ~(kArenaAlign - 1);
    for (auto it = gArenaFree.begin(); it != gArenaFree.end(); ++it) {
        if (it->second >= bytes) {
            const u32 at = it->first, left = it->second - bytes;
            gArenaFree.erase(it);
            if (left) {
                gArenaFree[at + bytes] = left;
            }
            gArenaUsed[gArena + at] = bytes;
            return gArena + at;
        }
    }
    gArenaFailed++;
    return nullptr;
}

void arenaFree(void* p)
{
    auto used = gArenaUsed.find(p);
    if (used == gArenaUsed.end()) {
        linearFree(p);
        return;
    }
    u32 at = (u32) ((u8*) p - gArena), bytes = used->second;
    gArenaUsed.erase(used);
    auto next = gArenaFree.lower_bound(at);
    if (next != gArenaFree.end() && next->first == at + bytes) {
        bytes += next->second;
        next = gArenaFree.erase(next);
    }
    if (next != gArenaFree.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == at) {
            prev->second += bytes;
            return;
        }
    }
    gArenaFree[at] = bytes;
}

// (a block with no room in it is taken from linear memory as before: the cache's budget counts no room lost to its
// pieces, and keptEvict would not make any)
u32 keepSize(void* p)
{
    auto used = gArenaUsed.find(p);
    return used == gArenaUsed.end() ? linearGetSize(p) : used->second;
}

// The largest piece of linear memory free (the log: textures need theirs in one piece), by trying
u32 linearLargest()
{
    u32 lo = 0, hi = linearSpaceFree() >> 14;
    while (lo < hi) {
        const u32 mid = (lo + hi + 1) / 2;
        void* p = linearAlloc(mid << 14);
        if (p) {
            linearFree(p);
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo << 14;
}
// pueblo.cfg switches: RE4_3DS_MIPMAP=0 without mipmaps (with them the GPU's frame in r100 went from
// 98 to 49 ms and the trees stopped shimmering), RE4_3DS_TEXGEN=0 takes coordinate n from TEXn as is
bool gMipmaps = true, gTexGen = true;
bool gBatching = true;  // RE4_3DS_BATCH=0: each draw on its own
bool gLighting = true;  // RE4_3DS_LIGHTS=0: no lights (the material alone)
int gCullMode = 1;      // RE4_3DS_CULL: 0 none, 1 as GX, 2 the winding of gx_gl.cpp (mirrored here)
bool gNoCull;           // the draw of a line's quads (drawLines): GX culls no lines
bool gLines = true;     // RE4_3DS_LINES=0: GX lines left out (before stage 6.2)
// (2026-11-09) the quads of consecutive GX line draws in one batch (the rain's ~1360 lines a picture, Filter06Render,
// were each a GPU draw: 20 ms of the GX thread in r11b); RE4_3DS_LINEJOIN=0: each line draw on its own as before
bool gLineJoin = true;
u32 gLineDraws, gLineJoined;  // per log window: line draws, those joined to the line draw before
u32 gCull = 0xFFFFFFFF; // the GX cull mode (BP 0x00 bits 14-15) as last set on the GPU
int gFogMode = 2;       // RE4_3DS_FOG: 0 no fog, 1 the fog table on -z/w, 2 GX's depth with the w-buffer (setFog)
// The GPU draws a frame while the CPU still builds it (RE4_3DS_OVERLAP=0: not): every gKickEvery
// GPU draws the commands so far go to it (kick()). Before, it drew the frame only after
// C3D_FrameEnd, and the next frame's first draw waited for it in C3D_FrameBegin: 46 of 183 ms
// in r100 (01.11.2026); C3D_FRAME_SYNCDRAW also waited for a VBlank at every begin.
bool gOverlap = true;
u32 gKickEvery = 128;  // RE4_3DS_KICKEVERY (32 before: ~0.6 ms of the GX thread a kick, bench7 -8 ms)
// and when the GX thread has caught up with the game (re4_3ds_gx_idle, stage 3.8: the frame's last part went at the
// display copy, which comes after the game's logic: the GPU stood ~24 ms with it); RE4_3DS_IDLEKICK=N from picture N
u32 gIdleKickFrom = 1;
// The vertex buffers in the GPU's registers (stage 3.8): only what changed is written. citro3d writes the base and all
// 12 buffers (39 words) at every draw that takes its BufInfo (C3Di_BufInfoBind), the draws use 1-3 (bench7: 118
// words a draw in all). citro3d's own copy (C3D_Context +64) is kept the same without its flag: C3D_DrawElements takes
// the indices' offset from its base. RE4_3DS_BUFLEAN: 0 citro3d's way, N from picture N
u32 gBufLeanFrom = 1;
C3D_BufInfo* gBufCtx;  // citro3d's (taken once: C3D_GetBufInfo raises its flag), as in the registers
bool gBufKnown;        // false: not known to be (a new picture, citro3d about to write its own)
// C3D_DrawElements as one block (stage 3.8): citro3d's state as it has it (C3Di_UpdateContext), then the draw's
// registers written straight into the command buffer, the words citro3d 1.7.1 writes (drawElements.o: 11 GPUCMD_Add
// calls of a word a strip, 15 a triangle list; ~2.2 ms a picture on bench7). RE4_3DS_DRAWOWN: 0 citro3d's, N from
// picture N
u32 gDrawOwnFrom = 1;
u32 gKicksMax = 20;  // RE4_3DS_KICKSMAX (citro3d's queue holds 128 entries a frame: clear, DMAs, kicks, end, transfer)
u32 gKickDraws, gKicks;
// Pictures in a pipeline (2026-11-10; RE4_3DS_PIPE=N: from picture N, 0 never). The GX
// thread waited at a picture's begin until the GPU had drawn the one before (C3D_FrameBegin: 6.4 ms in the fight of
// bench8), and the GPU then stood ~5 ms until the first list came. Now the next picture is built at once: the pictures
// take two GX queues of libctru in turn (citro3d's and gQueue1), two command buffers and two staging areas; picture N
// waits for N-2 only, whose queue and buffers it takes. A queue that is not libctru's running one only collects; when
// the running one (the picture before) has run out, the GX thread makes the current one run (pump(); on the GX thread
// alone: gxCmdQueueRun beside another thread's gxCmdQueueAdd can leave a queue marked idle with a command at GSP).
// So the commands of two pictures never mix at GSP: the clear of the target and the transfer to the screen come in
// the queues' order. The picture's end is the renderer's (pipeEnd(): no C3D_FrameEnd, citro3d stays in its frame);
// the screen's buffers are swapped when the queue's entry of the transfer is done (queueDone(), libctru's event
// thread). What a picture lets go (the cache's blocks, rooms copied to VRAM, textures' linear copies) is freed a
// picture later. Up to a picture more between the pad and the screen where the GPU holds the frame.
u32 gPipeFrom = 1;
bool gPipe;                             // on (from the first picture begun with it)
gxCmdQueue_s gQueue1;                   // the second queue (its entries: re4_3ds_gx_init)
gxCmdQueue_s* gQueues[2];               // citro3d's, gQueue1 (nullptr: no pipeline)
gxCmdQueue_s* gQueue = &__C3D_Context;  // the current picture's: GX commands go there (GX_BindQueue)
volatile u32 gCur, gRun;                // the current picture's queue; the one libctru runs (the picture before's, or it)
volatile u32 gPresentAt[2];             // a queue's entries done with which its picture is shown (0: nothing to show)
volatile u32 gShown;                    // pictures shown by queueDone()
u32* gCmdBufs[2];                       // citro3d's command buffer and a second one, kCmdBufBytes each
u32 gCmdWords;
u8* gStageBufs[2];
u16* gIndexBufs[2];
u8* gTopBufs[2];                        // the top screen's two buffers, as met
u32 gPipeWaited, gPipeHandoffs;         // per log window: begins that waited for the GPU; queues made to run by pump()
// The display list cache's blocks in VRAM (stage 3.8): a block (keep: vertices, 256 bytes, indices) is
// copied there by a GX DMA at its first draw, in citro3d's queue before the commands that read it (the
// queue runs at the kicks), and drawn from the copy from then on; the copy goes with the block (drop,
// once the GPU is past it). The CPU never touches a copy (VRAM is not the CPU's to write).
// RE4_3DS_KEPTVRAM: 0 off, 1 always, N > 1 from picture N on (two log windows of one run compared).
// Stage 5.7b: a block is moved to VRAM when the decoder has written it (kept): named by its place there from then on,
// copied at its first draw (drawn from its room until then), its room freed at the next frame's begin; the CPU
// reads it no more (the box and the matrix indices made before). RE4_3DS_KEPTMOVE=0: a copy beside the room, as before.
struct Block {
    u8* block;  // its name: the room in linear memory, or its place in VRAM (moved)
    u32 bytes;
    const void* indices;
    u8* vram;  // the copy (nullptr: none yet; moved: the block itself)
    u8* room = nullptr;    // the room in linear memory it holds (moved: until copied to VRAM)
    bool pending = false;  // moved, not copied yet
    float lo[3] = {}, hi[3] = {};  // the box of a static draw's positions (boxed: made, at its first draw)
    bool boxed = false;
    int rowsTop = -1;  // the largest position matrix index of its vertices (rowsFit; -1: not known)
};
std::unordered_map<const void*, Block> gBlocks;          // by the block
std::unordered_map<const void*, const void*> gBlockOf;  // a block's indices -> the block
u32 gVramFrom = 1, gVramBytes, gDmas;
const u32 kVramKept = 3072u << 10;
const u32 kDmasPerPicture = 48;
bool gKeptMove = true;
std::vector<void*> gRoomsCopied;  // rooms of moved blocks copied to VRAM: freed at the next frame's begin
// citro3d's GX queue: 32 entries (C3D_Init), made kQueueEntries (stage 5.7b: the copies to VRAM at a room's load)
const u16 kQueueEntries = 128;
u32 gViewCullFrom = 1;  // RE4_3DS_VIEWCULL=N: the cache's draws outside the view left out from picture N (0 never)
// citro3d's GX queue is not emptied inside a picture: the DMAs leave kQueueForDma entries, a kick
// kQueueForKick (the rest for the picture's end: C3D_FrameSplit, the transfer to the screen)
const u32 kQueueForDma = 16, kQueueForKick = 4;
// RE4_3DS_GPUTEST=1: everything drawn into one pixel (scissor), the GPU's pixel work gone;
// 2: no texture read (TEV takes the texture as white). (VRAM is not the CPU's to write: a try with
// textures written there stopped at a data abort, permission, 01.11.2026.)
int gGpuTest;
// GX's scissor (BP 0x20/0x21) as last set, with the viewport it was turned by (~0: none set in this part of the frame;
// RE4_3DS_SCISSOR=N: from picture N, 0 never)
u32 gScissorKey[6] = {~0u};
u32 gScissorFrom = 1;

// Draws in a row with the same state (no BP, XF or texture change between them, the same vertex
// layout and default matrices) go to the GPU as one: a model's display list has ~30 strips of
// ~11 vertices each, and each draw cost ~120 command words (r100: 17 000 draws, 2 million words
// a frame, 01.11.2026). The state is set at the batch's first draw (citro3d sends it at
// C3D_DrawElements, and the hooks change none of it, they only mark it), the later draws add
// their indices, moved behind the batch's and offset by their first vertex.
struct Batch {
    bool open;
    u32 layout, stride, matrices[3];
    const u8* base;     // the first draw's vertices
    u32 vertexEnd;      // vertices from base
    u16* indices;
    u32 count;
    bool kept;  // the display list cache's draw: its indices are lasting, nothing joins it
    bool strip; // (the cache's only) a triangle strip
    bool noCull;  // (2026-11-09) drawn without face culling (gNoCull: a line's quads); only such draws join it
};
Batch gBatch;
// The XF rows the shader holds (rows[]): the position matrices (rows 0..29), not all 64: the lights
// take the room (stage 3.9). The texture matrices go apart (tmtx); a draw beyond them is left out.
const int kRows = 48;
int gRowsMin = 0, gRowsMax = kRows - 1;  // XF rows written since their upload (min > max: none)
u32 gLightGen[8];  // XF lights 0..7: writes of each (a slot holds the count at its upload)
bool gProjDirty = true;
u32 gOwnerData;
// The GX picture on the top screen (stage 5.4): the EFB (gPicW x gPicH, the last display copy's: the game 512 x 448,
// the menus 640 x 448) at screen pixel (x, y down) = gMapO + gMapS * EFB pixel. RE4_3DS_ASPECT: 0 stretched over the
// screen; 1 4:3 (320 x 240, black at the sides); 2 (the default) the scene's band (the game's scissor inside the
// letterbox, rows 56-392) over the whole width as a TV shows it (x 0.78, y 0.67: the bars nearly gone), 4:3 on a
// picture without the band (the menus). Chosen at a picture's begin by the picture before.
u32 gPicW = 512, gPicH = 448;
int gAspect = 2;
// (2026-11-19, the settings screen's "Picture") the band on the top screen: 0 fitted in a TV's proportions (as before),
// 1 stretched to the screen's height, 2 zoomed to it (the sides cut). re4_3ds_gx_fill, RE4_3DS_FILL
volatile int gFill;
// The screens before the game (the logos, the title menu, its load screen: STA_TITLE, title.cpp titleInit) draw their
// own black bars (rows 0-56, 392-448) into 640 x 448: laid out as the scene's band (runtime/picture.cpp)
extern "C" int re4_title_picture(void);
const int kTitleBandTop = 56, kTitleBandBottom = 393;
float gMapSx = 400.0f / 512.0f, gMapSy = 240.0f / 448.0f, gMapOx, gMapOy;
bool gBandSeen, gBand;
int gBandTop = 56, gBandBottom = 393;
// RE4_3DS_BANDKEEP (1): a scene picture (512 x 448) without the band laid out by the last band seen (r108 draws
// without the letterbox scissor: 4:3 before); counted per log window, with the last scissor of such a picture
bool gBandKeep = true, gBandKnown;
u32 gBandKept;
int gNoBandRect[4] = {-1, -1, -1, -1}, gLastRect[4];
const uint32_t* gLastBp;  // the decoder's BP registers (a draw's; BP 0x52 comes without them)
// (2026-11-19, the GX thread's split, step 2) The renderer's own BP / XF state, led by bpWritten / xfChanged: the
// draws read it, not the decoder's arrays (docs/21-gx-split.md). RE4_3DS_GXSHADOW=0 (the default since the split was
// shelved, 2026-11-19): the decoder's, as before; 1: the shadow; 2: each draw's compared with the decoder's (gOwnDiff*:
// draws that differ, the first word that does)
int gOwnState = 0;
u32 gOwnBp[0x100], gOwnXfMem[0x800], gOwnXfReg[0x58];
u32 gOwnChecked, gOwnDiffBp, gOwnDiffXfMem, gOwnDiffXfReg, gOwnFirstAt = ~0u, gOwnFirstOwn, gOwnFirstTheirs;
u32 gClearFrom = 1, gClears;  // RE4_3DS_COPYCLEAR=N: EFB copies clear their rectangle from picture N (0 never)

void texRenderReplace();  // (R4)
void placePicture()
{
    gBand = gBandSeen;
    gBandSeen = false;
    gBandKnown = gBandKnown || gBand;
    if (!gBand && gAspect == 2 && gPicW == 512 && gPicH == 448) {
        memcpy(gNoBandRect, gLastRect, sizeof(gLastRect));
        if (gBandKeep && gBandKnown) {
            gBand = true;
            gBandKept++;
        }
    }
    const float w = (float) gPicW, h = (float) gPicH;
    if (gAspect == 0) {
        gMapSx = 400.0f / w;
        gMapSy = 240.0f / h;
        gMapOx = gMapOy = 0.0f;
    } else if (gAspect == 2 && !gBand && (re4_title_picture() || (gBandKeep && gPicW == 640 && gPicH == 448))) {
        const float band = (float) (kTitleBandBottom - kTitleBandTop);
        const float fit = fminf(300.0f / h, 240.0f / band);
        const int fill = gFill;  // (2026-11-19) the picture setting here too: stretched / zoomed to the screen's height
        gMapSy = fill ? 240.0f / band : fit;
        gMapSx = (fill == 2 ? gMapSy : fit) * (h * 4.0f / 3.0f) / w;
        gMapOx = 200.0f - gMapSx * w * 0.5f;
        gMapOy = 120.0f - gMapSy * (float) (kTitleBandTop + kTitleBandBottom) * 0.5f;
    } else if (gAspect == 1 || !gBand) {
        gMapSy = 240.0f / h;
        gMapSx = 320.0f / w;
        gMapOx = 40.0f;
        gMapOy = 0.0f;
    } else {
        // the picture as wide as the screen; its height as a TV shows it (4:3), less if the band is taller than the screen
        const float band = (float) (gBandBottom - gBandTop);
        const float fit = fminf(300.0f / h, 240.0f / band);
        const int fill = gFill;  // (2026-11-19) stretched / zoomed: the band as tall as the screen
        gMapSy = fill ? 240.0f / band : fit;
        gMapSx = (fill == 2 ? gMapSy : fit) * (h * 4.0f / 3.0f) / w;
        gMapOx = 200.0f - gMapSx * w * 0.5f;
        gMapOy = 120.0f - gMapSy * (float) (gBandTop + gBandBottom) * 0.5f;
    }
    texRenderReplace();  // (R4: a render target's pass begun before the picture)
}

// the depth and blend registers (BP 0x40, 0x41) as last set on the GPU
u32 gZMode = 0xFFFFFFFF, gCMode0 = 0xFFFFFFFF;
bool gBlending;

// the attribute setup of the draw before (which attributes come from the vertex)
u32 gAttrKey = 0xFFFFFFFF;
bool gTexDiv;  // a unit's coordinate from the packed normal: over its w (K)
bool gTexFromNrm[3];  // (2026-11-18) the unit's coordinate from the normal
int gColorAttr, gMatrixAttr, gTexAttr[3], gNormalAttr;

// the TEV colour registers (0 PREV, 1..3 C0..C2) and konstant colours (BP 0xE0-0xE7, bit 23 tells
// them apart), RGBA 0..1; the TEV registers changed since the combiners were set
float gTevReg[4][4], gKonst[4][4];
bool gTevDirty = true;
// (X, review 2026-11-24 finding 11) The stages citro3d holds, where known (bit i of the mask): a cache hit gives it
// those that differ alone - C3D_SetTexEnv copies a stage and marks it to be sent with no compare (C3D_GetTexEnv marks
// it too: hence a copy of ours). Forgotten wherever the stages are written another way (the builder, the clear's and
// the contrast's quads, the start) or are to go again (a picture's begin, the GPU given back).
// RE4_3DS_TEVGIVEN=0: every stage of a hit given, as before
C3D_TexEnv gTevGiven[6];
u32 gTevGivenMask;
bool gTevGivenOn = true;
static_assert(sizeof(C3D_TexEnv) == 20, "a combiner stage: five words, no padding (compared whole)");
u32 gTevBuilt;  // the combiners built (buildTev), per log window
// (stage 7.0) Sprites in a row (the rain of r10b: ~700 a picture, each its position matrix and the TEV register 0xC1
// written twice back to its value): one GPU draw. BP writes of the TEV's registers leave the batch open: compared at
// the next draw with those the combiners were built from, rebuilt only where changed. XF writes of position (and
// normal) matrices leave it open too where the next draw can join: its positions moved into the batch's first
// matrix on the CPU (M0^-1 Mi). RE4_3DS_SPRITEJOIN=0: as before.
bool gSpriteJoin = true;
bool gTevBpDirty;          // TEV registers written (BP) since the combiners were built, the batch left open
u32 gTevBpSnap[49];        // the TEV's BP registers the combiners were built from
float gTevRegSnap[32];     // and its colour registers and konstants
u64 gXfDeferRows;          // XF rows 0..63 written with the batch left open
bool gXfDeferOther;        // normal matrices written likewise (the draw that joins: unlit)
float gBatchM0[12];        // the batch's first draw's position matrix (as uploaded)
float gBatchInv[12];       // its inverse, where gBatchInvOk
bool gBatchInvOk;
u32 gJoinedXf, gJoinedTev; // draws joined over a matrix; TEV writes found back at their values (per log window)
bool gTevScaleOn = true;  // RE4_3DS_TEVSCALE (stage 6.8): the stages left that only scale PREV, into the PICA's scales
u32 gTevScaled, gTevScaleShort;
// what the combiners of the draw before need of each draw: the GX map of each texture unit
int gUnitMap[3], gUnitCoord[3], gUnits;
u32 gTevApprox;  // GX stages the combiners only approximate (counted, per log window)
// Stage 6.9: a GX stage's channel put into the PICA stage before (per log window); RE4_3DS_TEVPACK=0: none (nor the units'
// order by the alpha, nor a map's average for none)
u32 gTevPacked, gTevSkipped, gTevAveraged;  // (and GX stages left out as minor; draws with a map as its average)
bool gTevPack = true;
u32 gTevDropMask;  // the maps the TEV built last has no unit for (their texture changed: built again)
struct MapSource {  // the texels last bound to a map (game memory: valid while the frame lasts)
    Re4GxTexture t;
    const uint8_t* data;
    const uint8_t* tlut;
    uint32_t entries;
};
MapSource gMapSource[8];

// --- textures --------------------------------------------------------------------------------
// (made by pica_texture.cpp: the PICA formats and sizes, mipmaps; or read from the pack of them, stage 5.7a)
struct Texture {
    C3D_Tex tex;
    u32 lastFrame;
    void* linear;  // the texels in linear memory (tex.data while not in VRAM; nullptr once there, stage 5.7b)
    u32 busyUntil = 0;  // copied back from VRAM by a DMA in this picture: its linear memory not to go before this one
    void* vram;    // the copy in VRAM (nullptr: none)
    u32 bytes;
    bool biased;   // RE4_3DS_TEXLOD done
    bool avgKnown = false;  // (stage 6.9) avg: its average colour (RGBA 0..1), for a map with no unit left
    float avg[4];
    bool mipNear;  // the mipmap filter is GPU_NEAREST (RE4_3DS_MIPNEAR)
    bool turned = false;  // (R4) a render target's copy: its s the GX t, its t the GX s, each the other way round
    bool preloaded = false;  // (2026-11-24) read ahead (RE4_3DS_TEXPRELOAD), not bound since
};
std::unordered_map<u64, Texture*> gTextures;
u32 gPreUsed, gPreWasted, gPreEvicted;  // (per log window) read ahead and bound later; gone unbound; others pushed out
bool gPreloading;  // (2026-11-24) reading ahead now: no texture made smaller, none read ahead pushed out
// Textures in VRAM (stage 3.8): copied there by a DMA of the GX queue when bound from picture
// gTexVramFrom on (RE4_3DS_TEXVRAM, 0: never), at most kTexDmas a picture, kVramTex in all and
// kVramSpare left free; RE4_3DS_TEXLOD=N: a LOD bias of +1 from picture N (an experiment). Where that is short,
// the copies of textures not bound for kVramStale pictures go (the oldest first; the texture stays in linear)
// from picture gTexEvictFrom on (RE4_3DS_TEXEVICT, 0: never)
u32 gTexVramFrom = 1, gTexLodFrom, gTexVramBytes, gTexDmas;
// Stage 5.7b: a texture in VRAM keeps no copy in linear memory (r101 has no room for it): the copy goes at the next
// frame's begin (the GPU and the queue past its DMA), and one taken out of VRAM goes whole (made again when bound:
// from the pack or anew, as MKW-3DS does). RE4_3DS_TEXKEEPLINEAR=1: the copy kept, as before.
bool gTexKeepLinear;
std::vector<void*> gTexLinearLater;  // linear copies of textures now in VRAM, freed at the next frame's begin
// one taken out of VRAM goes back to linear memory by a DMA where kBackSpare is left there (made again: r100 bench3
// 70-260 -> 340-710 ms a log window making textures); gTexGone: those that went whole, per log window
const u32 kBackSpare = 512u << 10;
u32 gTexGone;
// RE4_3DS_MIPNEAR=N: from picture N the mipmap filter GPU_NEAREST (bilinear in one level, no blend of two): MKW-3DS
// saw the picture spoiled on the console with GPU_LINEAR, its ETC1 with mipmaps works with NEAREST (13-mkw-3ds.md);
// the default since r100-3ds-mipnear1.log (GPU 40.6 -> 34.6 ms a picture), 0: GPU_LINEAR as before
u32 gMipNearFrom = 1;
const u32 kVramTex = 3072u << 10, kVramSpare = 256u << 10, kTexDmas = 32, kVramStale = 30;
u32 gTexEvicted;  // VRAM copies taken back, per log window
u32 gTexEvictFrom = 1;
u32 gTexMadeBy[16], gTexMadeKb[16];  // textures made by GX format (all along)
u32 gTexBindVram, gTexBindLinear, gTexVramFailed;  // per log window
u64 gTexTicks;  // making textures, per log window
// The PICA's own formats (RE4_3DS_TEXNATIVE=N from picture N, 0: never; all textures made anew at its begin):
// GX I4 / IA4 as LA4 and I8 / IA8 as LA8 (GX intensity is the alpha too), CMPR as ETC1A4
u32 gTexNativeFrom = 1;
bool gTexNative;
// RE4_3DS_TEXSTEPS=P1:M1,...: from picture Pn the native formats of mask Mn (1 LA4, 2 LA8, 4 opaque CMPR, 8 CMPR with alpha, 16 grey CMPR as L8,
// 32 the other grey ones as LA4 / LA8 / L8: stage 5.6, RE4_3DS_TEXGREY=0 without), all textures made anew at its begin
// (a diagnosis: which of them Leon's hair flickers with)
u32 gTexMask = 63, gTexSteps, gTexStep, gTexStepFrom[8], gTexStepMask[8];
// Room for a texture (stage 5.6): the textures not drawn in this picture nor the one before go (kTexAge; before, all
// not yet drawn in this one: what came later in it went, and was made anew, 816 a log window); still short, it is made
// at half its longer side, to kTexShrinkMin texels; one not made is tried again kTexRetryAfter pictures later.
const u32 kTexAge = 2, kTexShrinkMin = 128 * 128, kTexRetryAfter = 30;
// (2026-11-24) the age initTexture lets go (kTexAge; reading ahead: not drawn since the file was opened); reading ahead
// a texture is not made smaller for room
u32 gTexAgeNow = kTexAge;
std::unordered_map<u64, u32> gTexRetry;  // a texture not made -> the picture to try it again
u32 gTexShrunk, gTexNotRetried;          // per log window

bool gEtcLog;  // RE4_3DS_ETC1LOG=1: each CMPR texture's error on the log

Texture* gMaps[8];  // what each texture map holds (NULL: nothing usable)
u32 gFrameNo;
u32 gTexMade, gTexFailed, gTexDropped;

void forgetBound(const Texture* tex);
void flushBatch();

// A texture a texture map holds now: not to go. The decoder sends a map's texture only when the map changes; one gone
// from under it left the map empty until then: Leon's draws without texture in the village's binoculars event
// (r101-3ds-padrec4.log, from before stage 5.7b: dropped after two pictures not drawn, at a cut of the event)
bool mapped(const Texture* tex)
{
    for (const Texture* m : gMaps) {
        if (m == tex) {
            return true;
        }
    }
    return false;
}

// A texture gone: its texels in VRAM and in linear memory
void deleteTexture(u64 key, Texture* tex)
{
    gPreWasted += tex->preloaded;
    for (Texture*& m : gMaps) {
        m = m == tex ? nullptr : m;
    }
    forgetBound(tex);
    if (tex->vram) {
        vramFree(tex->vram);
        gTexVramBytes -= tex->bytes;
    }
    if (tex->linear) {
        linearFree(tex->linear);
    }
    delete tex;
    gTextures.erase(key);
}

// Room in linear memory: the textures not used in this frame go, the longest unused first (linearOnly: of those with
// texels there)
bool dropOldest(u32 age = 1, bool linearOnly = false)
{
    age = gPipe && age < 2 ? 2 : age;  // (the pipeline: the GPU may still draw the picture before)
    Texture* oldest = nullptr;
    u64 key = 0;
    for (auto& e : gTextures) {
        if (e.second->lastFrame + age <= gFrameNo && (!linearOnly || e.second->linear) && e.second->busyUntil <= gFrameNo &&
            (!oldest || e.second->lastFrame < oldest->lastFrame) && !mapped(e.second) &&
            !(gPreloading && e.second->preloaded)) {  // (2026-11-24: reading ahead pushes out no texture read ahead)
            oldest = e.second;
            key = e.first;
        }
    }
    if (!oldest) {
        return false;
    }
    deleteTexture(key, oldest);
    gTexDropped++;
    gPreEvicted += gPreloading;  // (2026-11-24: pushed out by reading ahead)
    return true;
}

const GPU_TEXCOLOR kPackColor[] = {GPU_RGB565, GPU_RGBA4, GPU_LA4, GPU_LA8, GPU_ETC1, GPU_ETC1A4, GPU_L8};

// The PICA formats and mipmaps textures are made with now (RE4_3DS_TEXNATIVE, RE4_3DS_TEXSTEPS, RE4_3DS_MIPMAP)
re4pica::Options textureOptions()
{
    return {gTexNative ? gTexMask : 0, gMipmaps};
}

// The texels are made (or read) in the C heap and copied over at once
u8* buildBuffer()
{
    static u8* build;
    if (!build) {
        build = (u8*) malloc(re4pica::kTexArea * 2 * 4 / 3 + 64);
    }
    return build;
}

// A texture W x H of `pack` in linear memory, with all its mipmaps or none; the textures not drawn in this picture nor
// the one before go for room (kTexAge)
bool initTexture(Texture* tex, re4pica::Pack pack, u32 W, u32 H, bool mipmaps)
{
    const C3D_TexInitParams params = {(u16) W, (u16) H, (u8) (mipmaps ? re4pica::maxLevel(W, H) : 0),
                                      kPackColor[pack], GPU_TEX_2D, false};
    // the display list cache keeps its room (gx_decode.cpp kKeptBudget): textures go before it (in its own: no need)
    const u32 texBytes = W * H * re4pica::kPackBits[pack] / 8 * (mipmaps ? 4 : 3) / 3;
    while (linearSpaceFree() <
               texBytes + (!gArena && gKeeping && gKeptLive < kKeepReserve ? kKeepReserve - gKeptLive : 0) &&
           dropOldest(gTexAgeNow, true)) {
    }
    bool made;
    while (!(made = C3D_TexInitWithParams(&tex->tex, nullptr, params)) && dropOldest(gTexAgeNow, true)) {
    }
    return made;
}

// A texture made: its texels (all levels at `texels`) to linear memory, its filters
void finishTexture(Texture* tex, const u8* texels)
{
    tex->bytes = C3D_TexCalcTotalSize(tex->tex.size, tex->tex.maxLevel);
    tex->linear = tex->tex.data;
    tex->vram = nullptr;
    tex->biased = false;
    tex->mipNear = false;
    memcpy(tex->tex.data, texels, tex->bytes);
    C3D_TexFlush(&tex->tex);
    C3D_TexSetFilter(&tex->tex, GPU_LINEAR, GPU_LINEAR);
    if (gMipmaps) {
        C3D_TexSetFilterMipmap(&tex->tex, GPU_LINEAR);
    }
}

// The pack of textures made ahead of time (stage 5.7a; pica_texture.h, tools/texpak.cpp): RE4_3DS_TEXPAK a path on the
// SD card (0: none), its index in the C heap; a texture in it is read when first bound instead of made (ETC1A4
// 256 x 256: about 90 ms). RE4_3DS_TEXMISS=1: the textures not in it to kMissPath (runtime/re4_tex_dump.h, each key once
// a run) for the next pack.
const char kMissPath[] = "sdmc:/3ds/pueblo/texmiss.dump";
Handle gPakFile;
re4pica::PakHead gPakHead;
// (2026-11-08) The index on the SD card: here the first key of each block of kPakBlock entries (all-7's whole index was
// 154 KB of the C heap and grows with the languages and the disc's textures); a block read when looked in (1 KB),
// the last kPakCached kept
const u32 kPakBlock = 64, kPakCached = 4;
std::vector<u64> gPakTop;
u32 gPakCount;
struct PakCached {
    u32 block, n, used;
    re4pica::PakEntry e[kPakBlock];
};
PakCached gPakCache[kPakCached];
u32 gPakUse, gPakBlockReads;  // (the reads per log window)
// (2026-11-24) RE4_3DS_PAKINDEX (1): the whole index in the C heap (18728 textures: 293 KB), one piece taken when the
// pack is opened (before the heap breaks up); a new texture in view then costs one SD card request, not two (a block
// of the index ~6 ms: r118's first seconds 261 of them, ~1.6 s of the GX thread). No room: the index on the SD card
re4pica::PakEntry* gPakAll;
// (2026-11-10) The keys, folded to 32 bits and in order, of the pack's textures in a format made here (not of
// RE4_3DS_TEXPAKFORMATS): such a texture was looked up in the index all the same, a block read from the SD card each
// (~4.7 ms; bench7: 173 reads in a log window, 140 of them for textures then made here). 4 bytes an entry. Another
// key folding alike by chance: that texture is made here too.
std::vector<u32> gPakHere;
u32 gPakHereHits;  // per log window
u32 pakFold(u64 key)
{
    return (u32) (key ^ key >> 32);
}
re4pica::PakEntry gPakFound;
FILE* gMissFile;
// RE4_3DS_MISSBUF (1): the misses collected in a stdio buffer of kMissBuffer, to the SD card when it is full and at
// the end (re4_3ds_texmiss_flush): a flush of each record on the GX thread took up to 1.2 s (play-cheats1.log)
bool gMissBuf = true;
const size_t kMissBuffer = 512 * 1024;
std::unordered_set<u64> gMissed;
u32 gTexFromPak, gTexPakShrunk, gTexNotInPak, gTexMissed;  // per log window
u64 gPakTicks;                                             // reading them, per log window
// The cost of textures by PICA format, per log window: made here, read from the pack. RE4_3DS_TEXPAKFORMATS: a mask of the
// formats read from it (1 << re4pica::Pack); the others made here. A read is ~6 ms and ~0.1 ms a KB (r100-3ds-texhalf1.log,
// made / read in the same windows): ETC1A4 41.5 / 9.8 ms, LA4 13.9 / 7.2, L8 9.7 / 6.8, but RGBA4 5.1 / 9.3, LA8 5.6 / 8.8;
// so ETC1, ETC1A4, LA4, L8 from the pack (0x74)
u32 gMadeN[re4pica::kPacks], gReadN[re4pica::kPacks];
u64 gMadeTicks[re4pica::kPacks], gReadTicks[re4pica::kPacks], gReadBytes[re4pica::kPacks];
u32 gPakFormats = 1u << re4pica::kEtc1 | 1u << re4pica::kEtc1a4 | 1u << re4pica::kLa4 | 1u << re4pica::kL8;

bool pakUsable()
{
    const re4pica::Options o = textureOptions();
    return gPakCount != 0 && gPakHead.mask == o.mask && gPakHead.mipmaps == (u32) o.mipmaps;
}

void openPak(const char* path)
{
    path += strncmp(path, "sdmc:", 5) == 0 ? 5 : 0;
    if (R_FAILED(FSUSER_OpenFileDirectly(&gPakFile, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                                         fsMakePath(PATH_ASCII, path), FS_OPEN_READ, 0))) {
        gPakFile = 0;
        re4_log("3ds: texture pack %s: none (textures made here)", path);
        return;
    }
    u32 read = 0;
    bool ok = R_SUCCEEDED(FSFILE_Read(gPakFile, &read, 0, &gPakHead, sizeof(gPakHead))) && read == sizeof(gPakHead) &&
              gPakHead.magic == re4pica::kPakMagic && gPakHead.version == re4pica::kPakVersion;
    if (ok) {
        // the index read once by pieces (64 KB for the moment), each block's first key kept; in order, or not used
        const u32 piece = 4096;
        std::vector<re4pica::PakEntry> buf(piece);
        const char* formats = getenv("RE4_3DS_TEXPAKFORMATS");  // (read into gPakFormats after this: the same here)
        const u32 fromPak = formats ? (u32) strtoul(formats, nullptr, 0) : gPakFormats;
        gPakTop.reserve((gPakHead.count + kPakBlock - 1) / kPakBlock);
        const char* whole = getenv("RE4_3DS_PAKINDEX");
        if (!(whole && *whole == '0') && gPakHead.count) {
            gPakAll = (re4pica::PakEntry*) malloc((size_t) gPakHead.count * sizeof(re4pica::PakEntry));
        }
        u64 last = 0;
        for (u32 at = 0; ok && at < gPakHead.count; at += piece) {
            const u32 n = gPakHead.count - at < piece ? gPakHead.count - at : piece;
            const u32 bytes = n * sizeof(re4pica::PakEntry);
            re4pica::PakEntry* into = gPakAll ? gPakAll + at : buf.data();
            ok = R_SUCCEEDED(FSFILE_Read(gPakFile, &read, (u64) gPakHead.indexOffset + (u64) at * sizeof(re4pica::PakEntry),
                                         into, bytes)) &&
                 read == bytes;
            if (ok && gPakAll) {
                memcpy(buf.data(), into, bytes);
            }
            for (u32 i = 0; ok && i < n; i++) {
                ok = at + i == 0 || buf[i].key > last;
                last = buf[i].key;
                if ((at + i) % kPakBlock == 0) {
                    gPakTop.push_back(buf[i].key);
                }
                if (!(fromPak >> buf[i].pack & 1)) {
                    gPakHere.push_back(pakFold(buf[i].key));
                }
            }
        }
        for (PakCached& c : gPakCache) {
            c.block = ~0u;
        }
        gPakCount = ok ? gPakHead.count : 0;
        std::sort(gPakHere.begin(), gPakHere.end());
        gPakHere.shrink_to_fit();
    }
    if (!ok) {
        re4_log("3ds: texture pack %s: not used (magic %08lx version %lu, this one reads version %lu)", path,
                (unsigned long) gPakHead.magic, (unsigned long) gPakHead.version, (unsigned long) re4pica::kPakVersion);
        std::vector<u64>().swap(gPakTop);
        std::vector<u32>().swap(gPakHere);
        free(gPakAll);
        gPakAll = nullptr;
        gPakCount = 0;
        FSFILE_Close(gPakFile);
        gPakFile = 0;
        return;
    }
    re4_log("3ds: texture pack %s: %u textures (index %s, %u KB here: %s %u), made with formats mask "
            "%lu, mipmaps %lu; %s", path, (unsigned) gPakCount,
            gPakAll ? "in memory, RE4_3DS_PAKINDEX 1" : "on the SD card",
            (unsigned) (gPakAll ? (gPakCount * sizeof(re4pica::PakEntry) + 1023) >> 10 : (gPakTop.size() * 8 + 1023) >> 10),
            gPakAll ? "the whole of it, blocks of" : "a key per", (unsigned) kPakBlock,
            (unsigned long) gPakHead.mask, (unsigned long) gPakHead.mipmaps,
            pakUsable() ? "used" : "not used (the textures made otherwise here)");
}

const re4pica::PakEntry* inPak(u64 key)
{
    if (!pakUsable()) {
        return nullptr;
    }
    if (gPakAll) {
        const re4pica::PakEntry* all = gPakAll;
        const re4pica::PakEntry* end = all + gPakCount;
        const re4pica::PakEntry* it =
            std::lower_bound(all, end, key, [](const re4pica::PakEntry& e, u64 k) { return e.key < k; });
        if (it == end || it->key != key) {
            return nullptr;
        }
        gPakFound = *it;
        return &gPakFound;
    }
    // the block: the last whose first key is not above it
    auto top = std::upper_bound(gPakTop.begin(), gPakTop.end(), key);
    if (top == gPakTop.begin()) {
        return nullptr;
    }
    const u32 block = (u32) (top - gPakTop.begin()) - 1;
    PakCached* c = nullptr;
    for (PakCached& k : gPakCache) {
        c = k.block == block ? &k : c;
    }
    if (!c) {
        c = &gPakCache[0];
        for (PakCached& k : gPakCache) {
            c = k.block == ~0u || k.used < c->used ? &k : c;
        }
        const u32 first = block * kPakBlock;
        const u32 n = gPakCount - first < kPakBlock ? gPakCount - first : kPakBlock;
        u32 read = 0;
        const u32 bytes = n * sizeof(re4pica::PakEntry);
        c->block = ~0u;
        if (R_FAILED(FSFILE_Read(gPakFile, &read,
                                 (u64) gPakHead.indexOffset + (u64) first * sizeof(re4pica::PakEntry), c->e, bytes)) ||
            read != bytes) {
            return nullptr;
        }
        c->block = block;
        c->n = n;
        gPakBlockReads++;
    }
    c->used = ++gPakUse;
    auto it = std::lower_bound(c->e, c->e + c->n, key, [](const re4pica::PakEntry& e, u64 k) { return e.key < k; });
    if (it == c->e + c->n || it->key != key) {
        return nullptr;
    }
    gPakFound = *it;  // (a copy: the block may go at the next lookup)
    return &gPakFound;
}

// A texture from the pack; short of room, from its next level on (half the size; as makeTexture, not under
// kTexShrinkMin). nullptr: no room, or not read
Texture* loadPacked(const re4pica::PakEntry& e)
{
    const u64 started = svcGetSystemTick();
    const re4pica::Pack pack = (re4pica::Pack) e.pack;
    const u32 fullW = 1u << e.widthLog2, fullH = 1u << e.heightLog2;
    u32 W = fullW, H = fullH, skip = 0;
    Texture* tex = new Texture;
    tex->lastFrame = gFrameNo;
    while (!initTexture(tex, pack, W, H, e.mipmaps)) {
        if (gPreloading || !e.mipmaps || re4pica::maxLevel(W, H) == 0 || W * H <= kTexShrinkMin) {
            delete tex;
            return nullptr;
        }
        W >>= 1;
        H >>= 1;
        skip++;
    }
    const u32 from = skip ? (u32) re4pica::totalBytes(pack, fullW, fullH, skip - 1) : 0;
    const u32 bytes = C3D_TexCalcTotalSize(tex->tex.size, tex->tex.maxLevel);
    u32 read = 0;
    const Result r = FSFILE_Read(gPakFile, &read, (u64) e.offset + from, buildBuffer(), bytes);
    if (R_FAILED(r) || read != bytes) {
        static u32 lines;
        if (lines++ < 8) {
            re4_log("3ds: texture pack: %u bytes at %lu not read (0x%08lx, %u read)", (unsigned) bytes,
                    (unsigned long) (e.offset + from), (unsigned long) r, (unsigned) read);
        }
        C3D_TexDelete(&tex->tex);
        delete tex;
        return nullptr;
    }
    finishTexture(tex, buildBuffer());
    gTexFromPak++;
    gTexPakShrunk += skip != 0;
    const u64 ticks = svcGetSystemTick() - started;
    gPakTicks += ticks;
    gReadN[pack]++;
    gReadTicks[pack] += ticks;
    gReadBytes[pack] += bytes;
    return tex;
}

// --- (2026-11-24) Textures ahead (RE4_3DS_TEXPRELOAD): a file the game opens -> its textures from the pack while the
// GX thread has nothing else (re4_3ds_tex_preload_step). The list: /3ds/pueblo/textures.pre (stage94_prelist.py)
struct PreHead {
    u32 magic, version, files, keys;
};
struct PreFile {
    u32 hash, first, count;
};
const u32 kPreMagic = 0x314C5052u;  // "RPL1"
const u32 kPreMaxAge = 900;         // pictures: a file's list older than that is let go (made when bound anyway)
u32 kPreSpare = 3u << 20;     // linear memory left free by reading ahead (stage98: RE4_3DS_TEXPRELOAD_SPARE, KB; 3072:
                              // r118 18 slow frames against 29 with 1024 - the game's own textures need the room)
const char kPrePath[] = "/3ds/pueblo/textures.pre";
int gPreOn;
Handle gPreFile;
PreHead gPreHead;
std::vector<PreFile> gPreFiles;
struct PreItem {
    u32 hash, frame;
};
LightLock gPreLock;
std::vector<PreItem> gPreQueue;          // (under gPreLock) the files opened
u32 gPreQueuedN;                         // its size (atomic: the GX thread looks without the lock)
std::vector<u64> gPreKeys;               // the file being read ahead
u32 gPreAt, gPreEpoch;                   // its next key; the picture it was opened at
std::vector<PreItem> gPreDone;           // files read ahead lately (not again within kPreMaxAge)
u32 gPreOpened, gPreListed, gPreKeysN, gPreMade, gPreMadeLater, gPreHave, gPreNoPak, gPreNoRoom, gPreOld;  // per window
u64 gPreTicks, gPreTicksLater;  // read in the picture the file was opened in (a load), in the pictures after it
u32 gPreSteps = 4, gPreStepFrame, gPreStepsNow;  // RE4_3DS_TEXPRELOAD_STEPS: textures a picture at most
// (2026-11-24, stage98) the pace by time: the end of the last picture (gFrameNo's step), the time a picture may go to
// (RE4_3DS_TEXPRELOAD_MS from that end), a read's cost (a running mean); no picture for kPreFreeMs: loading, no limit
u64 gPreFrameEnd;
u64 gPreBudgetTicks, gPreReadTicks = (u64) (10 * SYSCLOCK_ARM11 / 1000);
const u32 kPreFreeMs = 100;
// the game goes on: reads only after a picture whose GX work (reads ahead left out) was under RE4_3DS_TEXPRELOAD_BUSY
// (ms, 20): bench7 (the GX thread holds the frame) +0.47 ms a frame without it
u64 gPreBusyLimit, gPreBusyPrev, gPreBusyMark, gPreAllTicks;

void openPreList()
{
    const char* v = getenv("RE4_3DS_TEXPRELOAD");
    gPreOn = v ? atoi(v) : 2;  // 2 (default): may push out textures not drawn since the file was opened; 1: only free room; 0: off
    const char* steps = getenv("RE4_3DS_TEXPRELOAD_STEPS");
    gPreSteps = steps ? (u32) atoi(steps) : 4;
    const char* spare = getenv("RE4_3DS_TEXPRELOAD_SPARE");
    if (spare) {
        kPreSpare = (u32) atoi(spare) << 10;
    }
    const char* ms = getenv("RE4_3DS_TEXPRELOAD_MS");
    gPreBudgetTicks = (u64) ((ms ? atoi(ms) : 22) * (SYSCLOCK_ARM11 / 1000));
    const char* busy = getenv("RE4_3DS_TEXPRELOAD_BUSY");
    gPreBusyLimit = (u64) ((busy ? atoi(busy) : 20) * (SYSCLOCK_ARM11 / 1000));
    LightLock_Init(&gPreLock);
    if (!gPreOn || !gPakCount) {
        gPreOn = 0;
        return;
    }
    const char* path = kPrePath;
    u32 read = 0;
    bool ok = R_SUCCEEDED(FSUSER_OpenFileDirectly(&gPreFile, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                                                  fsMakePath(PATH_ASCII, path), FS_OPEN_READ, 0));
    if (!ok) {
        gPreFile = 0;
    }
    ok = ok && R_SUCCEEDED(FSFILE_Read(gPreFile, &read, 0, &gPreHead, sizeof(gPreHead))) && read == sizeof(gPreHead) &&
         gPreHead.magic == kPreMagic && gPreHead.version == 1 && gPreHead.files < 65536;
    if (ok) {
        gPreFiles.resize(gPreHead.files);
        const u32 bytes = gPreHead.files * sizeof(PreFile);
        ok = R_SUCCEEDED(FSFILE_Read(gPreFile, &read, sizeof(gPreHead), gPreFiles.data(), bytes)) && read == bytes;
    }
    if (gPreFile) {  // (not kept open: the system's places for open files are few - the HOME menu's panic, 71a4b9a1)
        FSFILE_Close(gPreFile);
    }
    gPreFile = 0;
    if (!ok) {
        re4_log("3ds: textures ahead (RE4_3DS_TEXPRELOAD %d): no list %s - off", gPreOn, path);
        std::vector<PreFile>().swap(gPreFiles);
        gPreOn = 0;
        return;
    }
    re4_dvd_open_fn = re4_3ds_tex_preload_open;
    re4_log("3ds: textures ahead (RE4_3DS_TEXPRELOAD %d): list %s, %u files, %u keys", gPreOn, path,
            (unsigned) gPreHead.files, (unsigned) gPreHead.keys);
}

// the next file's keys (true: one taken, maybe with no list)
bool preNextFile()
{
    PreItem it;
    LightLock_Lock(&gPreLock);
    const bool any = !gPreQueue.empty();
    if (any) {
        it = gPreQueue.front();
        gPreQueue.erase(gPreQueue.begin());
        __atomic_sub_fetch(&gPreQueuedN, 1, __ATOMIC_SEQ_CST);
    }
    LightLock_Unlock(&gPreLock);
    if (!any) {
        return false;
    }
    if (gFrameNo - it.frame > kPreMaxAge) {
        gPreOld++;
        return true;
    }
    for (const PreItem& d : gPreDone) {
        if (d.hash == it.hash && gFrameNo - d.frame <= kPreMaxAge) {
            return true;
        }
    }
    auto f = std::lower_bound(gPreFiles.begin(), gPreFiles.end(), it.hash,
                              [](const PreFile& p, u32 h) { return p.hash < h; });
    if (f == gPreFiles.end() || f->hash != it.hash || !f->count) {
        return true;
    }
    gPreKeys.resize(f->count);
    u32 read = 0;
    const u32 bytes = f->count * 8;
    const u64 at = sizeof(PreHead) + (u64) gPreHead.files * sizeof(PreFile) + (u64) f->first * 8;
    Handle file = 0;  // (opened for the read only, as the list at the start)
    const bool ok = R_SUCCEEDED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                                                        fsMakePath(PATH_ASCII, kPrePath), FS_OPEN_READ, 0)) &&
                    R_SUCCEEDED(FSFILE_Read(file, &read, at, gPreKeys.data(), bytes)) && read == bytes;
    if (file) {
        FSFILE_Close(file);
    }
    if (!ok) {
        gPreKeys.clear();
        return true;
    }
    gPreAt = 0;
    gPreEpoch = it.frame;
    gPreListed++;
    gPreKeysN += f->count;
    if (gPreDone.size() >= 64) {
        gPreDone.erase(gPreDone.begin());
    }
    gPreDone.push_back({it.hash, gFrameNo});
    return true;
}

// one step on the GX thread with nothing in its ring: a texture read ahead, or a file's list (false: nothing to do)
bool preStep()
{
    if (!gPreOn) {
        return false;
    }
    if (gPreAt < gPreKeys.size() && gFrameNo - gPreEpoch > kPreMaxAge) {
        gPreOld++;
        gPreAt = (u32) gPreKeys.size();  // (the file's list too old: the rest when bound)
    }
    // (stage98) the game goes on: while the picture's time lasts, gPreSteps a picture at most; loading: no limit
    const u64 sinceEnd = svcGetSystemTick() - gPreFrameEnd;
    const bool loading = sinceEnd > (u64) kPreFreeMs * (SYSCLOCK_ARM11 / 1000);
    if (gPreAt < gPreKeys.size() && !loading) {
        if (gPreStepFrame != gFrameNo) {
            gPreStepFrame = gFrameNo;
            gPreStepsNow = 0;
        }
        if (gPreStepsNow >= gPreSteps || sinceEnd + gPreReadTicks > gPreBudgetTicks || gPreBusyPrev > gPreBusyLimit) {
            return false;
        }
    }
    while (gPreAt < gPreKeys.size()) {
        const u64 key = gPreKeys[gPreAt++];
        if (gTextures.count(key)) {
            gPreHave++;
            continue;
        }
        const re4pica::PakEntry* e = inPak(key);
        if (!e) {
            gPreNoPak++;
            continue;
        }
        const re4pica::PakEntry entry = *e;
        const u64 from = svcGetSystemTick();
        const u32 W = 1u << entry.widthLog2, H = 1u << entry.heightLog2;
        const u32 bytes = W * H * re4pica::kPackBits[entry.pack] / 8 * (entry.mipmaps ? 4 : 3) / 3;
        // mode 1: only in what is free over kPreSpare; mode 2: textures not drawn since the file was opened may go
        const u32 age = gPreOn < 2 ? gFrameNo + 1 : gFrameNo - gPreEpoch + 1 > kTexAge ? gFrameNo - gPreEpoch + 1 : kTexAge;
        gPreloading = true;
        while (gPreOn >= 2 && linearSpaceFree() < bytes + kPreSpare && dropOldest(age, true)) {
        }
        Texture* tex = nullptr;
        if (linearSpaceFree() >= bytes + kPreSpare) {
            gTexAgeNow = age;
            tex = loadPacked(entry);
            gTexAgeNow = kTexAge;
        }
        gPreloading = false;
        if (!tex) {
            gPreNoRoom++;
            gPreAt = (u32) gPreKeys.size();  // the rest of this file: when bound
            return true;
        }
        tex->preloaded = true;
        tex->lastFrame = gPreEpoch;  // (not younger than the textures it may push out later)
        gTextures[key] = tex;
        const bool inOpen = loading;  // (stage98: "while loading")
        const u64 took = svcGetSystemTick() - from;
        (inOpen ? gPreMade : gPreMadeLater)++;
        (inOpen ? gPreTicks : gPreTicksLater) += took;
        gPreReadTicks = (gPreReadTicks * 7 + took) / 8;
        gPreAllTicks += took;
        gPreStepsNow += !inOpen;
        return true;
    }
    return preNextFile();
}

void preReport()
{
    if (!gPreOn) {
        return;
    }
    u32 live = 0;
    for (auto& e : gTextures) {
        live += e.second->preloaded;
    }
    re4_log("3ds: textures ahead (RE4_3DS_TEXPRELOAD %d, %u a picture at most, RE4_3DS_TEXPRELOAD_MS %u): %u files "
            "opened, %u with a list "
            "(%u keys); read %u in %.1f ms in the loads' pictures, %u in %.1f ms after; %u there already, %u not in the "
            "pack, %u stops for room, %u lists too old; %u others pushed out for them; bound later %u, gone unbound %u, "
            "unbound now %u, linear free %lu KB", gPreOn, (unsigned) gPreSteps,
            (unsigned) (gPreBudgetTicks / (SYSCLOCK_ARM11 / 1000)), gPreOpened, gPreListed, gPreKeysN,
            gPreMade, (double) gPreTicks / CPU_TICKS_PER_MSEC, gPreMadeLater, (double) gPreTicksLater / CPU_TICKS_PER_MSEC,
            gPreHave, gPreNoPak, gPreNoRoom, gPreOld, gPreEvicted, gPreUsed, gPreWasted, live,
            (unsigned long) linearSpaceFree() >> 10);
    gPreOpened = gPreListed = gPreKeysN = gPreMade = gPreMadeLater = gPreHave = gPreNoPak = gPreNoRoom = gPreOld = 0;
    gPreUsed = gPreWasted = gPreEvicted = 0;
    gPreTicks = gPreTicksLater = 0;
}

// A texture not in the pack: counted, and with RE4_3DS_TEXMISS to kMissPath; not after kMissDynamic keys of one address
// and size (a movie's pictures, 295 a minute at r100's start; those the game writes: of no use in a pack)
const u32 kMissDynamic = 8;
std::unordered_map<u64, u32> gMissAt;

void dumpMiss(const Re4GxTexture* t, const uint8_t* data, const uint8_t* tlut, uint32_t entries)
{
    gTexNotInPak++;
    const u64 key = (u64) t->keyHi << 32 | t->keyLo;
    if (!gMissFile || gMissed.count(key)) {
        return;
    }
    u32& keys = gMissAt[(u64) t->address << 32 ^ (t->format << 24 | (t->width & 0xFFF) << 12 | (t->height & 0xFFF))];
    if (keys >= kMissDynamic) {
        return;
    }
    keys++;
    gMissed.insert(key);
    const size_t level0 = re4gx::GxTextureLevelBytes(t->format, t->width, t->height);
    Re4TexDumpHead head = {};
    head.magic = RE4_TEX_DUMP_MAGIC;
    head.keyLo = t->keyLo;
    head.keyHi = t->keyHi;
    head.address = t->address;
    head.format = (u16) t->format;
    head.width = (u16) t->width;
    head.height = (u16) t->height;
    head.tlutFormat = (u16) ((t->tlut >> 10) & 3);
    head.dataBytes = (u32) (level0 < t->dataBytes ? level0 : t->dataBytes);
    head.entries = tlut ? entries : 0;
    gTexMissed += re4_tex_dump_write(gMissFile, &head, data, tlut) != 0;
    if (!gMissBuf) {
        fflush(gMissFile);  // whole records (the game may end without closing it: the last one was cut short)
    }
}

// (2026-11-08) A large texture decoded at once at the size it is made at: the halvings across (sx) and down (sy) to
// it - the size pica_texture.cpp choose() makes (a power of 2 from 8, a side to 256 for CMPR / kTexMax, the area to
// kTexArea), only exact ones (w, h divisible), only above 256 x 256 texels (RE4_3DS_TEXREDUCE=0: never)
int gTexReduce = -1;
u32 gTexReducedN;  // made so (all the run)

void reduceShifts(u32 format, u32 w, u32 h, u32& sx, u32& sy)
{
    sx = sy = 0;
    if (gTexReduce < 0) {
        const char* e = getenv("RE4_3DS_TEXREDUCE");
        gTexReduce = !(e && e[0] == '0');
    }
    if (!gTexReduce || w * h <= 256 * 256) {
        return;
    }
    auto pow2 = [](u32 v) {
        u32 p = 8;
        while (p < v && p < re4pica::kTexMax) {
            p <<= 1;
        }
        return p;
    };
    u32 W = pow2(w), H = pow2(h);
    const u32 side = format == re4gx::kTexCMPR ? 256 : re4pica::kTexMax;
    W = W < side ? W : side;
    H = H < side ? H : side;
    while (W * H > re4pica::kTexArea) {
        (W >= H ? W : H) >>= 1;
    }
    while ((w >> (sx + 1)) >= W && w % (2u << sx) == 0) {
        sx++;
    }
    while ((h >> (sy + 1)) >= H && h % (2u << sy) == 0) {
        sy++;
    }
    // only onto the size made (else build() resamples the rest from other texels: 640 x 360 CMPR up to 8 of 15 apart)
    sx = (w >> sx) == W ? sx : 0;
    sy = (h >> sy) == H ? sy : 0;
}

// Level 0 decoded by bands of rows, reduced 2^sx across and 2^sy down (the mean, rounded as resampleLine: across first)
bool decodeReduced(const Re4GxTexture* t, const uint8_t* data, const uint8_t* tlut, uint32_t entries, u32 sx, u32 sy,
                   std::vector<std::uint8_t>& out)
{
    const u32 w = t->width, h = t->height, fx = 1u << sx, fy = 1u << sy, ow = w >> sx;
    const size_t level0 = re4gx::GxTextureLevelBytes(t->format, w, h);
    if (!data || t->dataBytes < level0) {
        return false;
    }
    const u32 band = fy > 8 ? fy : 8;  // whole tile rows (tiles 4 or 8 high), whole groups of fy rows
    static std::vector<std::uint8_t> rows, across;
    across.resize((size_t) ow * band * 4);
    out.resize((size_t) ow * (h >> sy) * 4);
    for (u32 r = 0; r < h; r += band) {
        const u32 n = h - r < band ? h - r : band;  // (h and band divisible by fy: so is n)
        const size_t at = re4gx::GxTextureLevelBytes(t->format, w, r);
        if (!re4gx::GxDecodeTexture(data + at, level0 - at, t->format, w, n, tlut, entries, (t->tlut >> 10) & 3,
                                    rows)) {
            return false;
        }
        for (u32 y = 0; y < n; y++) {
            const u8* src = rows.data() + (size_t) y * w * 4;
            u8* dst = across.data() + (size_t) y * ow * 4;
            for (u32 x = 0; x < ow; x++, src += fx * 4, dst += 4) {
                for (int k = 0; k < 4; k++) {
                    u32 sum = 0;
                    for (u32 j = 0; j < fx; j++) {
                        sum += src[j * 4 + k];
                    }
                    dst[k] = (u8) ((sum + fx / 2) / fx);
                }
            }
        }
        for (u32 y = 0; y < n / fy; y++) {
            u8* dst = out.data() + ((size_t) (r >> sy) + y) * ow * 4;
            const u8* src = across.data() + (size_t) y * fy * ow * 4;
            for (u32 i = 0; i < ow * 4; i++) {
                u32 sum = 0;
                for (u32 j = 0; j < fy; j++) {
                    sum += src[(size_t) j * ow * 4 + i];
                }
                dst[i] = (u8) ((sum + fy / 2) / fy);
            }
        }
    }
    return true;
}

Texture* makeTexture(const Re4GxTexture* t, const uint8_t* data, const uint8_t* tlut, uint32_t entries)
{
    // (pica_texture.cpp, stage 5.7a: the conversion the host tool for the pack makes as well)
    const u64 started = svcGetSystemTick();
    static std::vector<std::uint8_t> rgba;
    struct Trim {  // (2026-11-08) the room of one over 256 KB given back
        std::vector<std::uint8_t>& v;
        ~Trim()
        {
            if (v.capacity() > (256u << 10)) {
                std::vector<std::uint8_t>().swap(v);
            }
        }
    } trim{rgba};
    u32 sx, sy;
    reduceShifts(t->format, t->width, t->height, sx, sy);
    if (sx || sy) {
        if (!decodeReduced(t, data, tlut, entries, sx, sy, rgba)) {
            return nullptr;
        }
        if (gTexReducedN++ < 20) {
            re4_log("3ds: texture %ux%u format %u decoded at %ux%u (RE4_3DS_TEXREDUCE)", (unsigned) t->width,
                    (unsigned) t->height, (unsigned) t->format, (unsigned) t->width >> sx, (unsigned) t->height >> sy);
        }
    } else if (!re4pica::decode(t->format, t->width, t->height, (t->tlut >> 10) & 3, data, t->dataBytes, tlut,
                                entries, rgba)) {
        return nullptr;
    }
    const u32 w = t->width >> sx, h = t->height >> sy;
    const re4pica::Shape shape = re4pica::choose(t->format, w, h, rgba, textureOptions());
    if (shape.grey && gEtcLog) {
        re4_log("3ds: cmpr grey %ux%u key %08x%08x", (unsigned) w, (unsigned) h, (unsigned) t->keyHi,
                (unsigned) t->keyLo);
    }
    const re4pica::Pack pack = shape.pack;
    u32 W = shape.width, H = shape.height;
    // the room to make it taken before the texture: out of memory (std::bad_alloc, uploadTexture) nothing is left behind
    re4pica::reserve(h, W, H);
    Texture* tex = new Texture;
    tex->lastFrame = gFrameNo;
    while (!initTexture(tex, pack, W, H, shape.mipmaps)) {
        if (W * H <= kTexShrinkMin) {
            delete tex;
            gTexFailed++;
            static u32 lines;
            if (lines++ < 20) {
                re4_log("3ds: texture %ux%u format %u (%ux%u here) not made: no room in linear memory (%lu KB free, "
                        "%lu KB the largest piece, %u textures, none unused for %u pictures)", (unsigned) w,
                        (unsigned) h, (unsigned) t->format, (unsigned) W, (unsigned) H,
                        (unsigned long) linearSpaceFree() >> 10, (unsigned long) linearLargest() >> 10,
                        (unsigned) gTextures.size(), (unsigned) kTexAge);
            }
            return nullptr;
        }
        (W >= H ? W : H) >>= 1;
    }
    if (W != shape.width || H != shape.height) {
        gTexShrunk++;
        static u32 lines;
        if (lines++ < 20) {
            re4_log("3ds: texture %ux%u format %u (%ux%u here) made at %ux%u: no room in linear memory (%lu KB free, "
                    "%lu KB the largest piece, %u textures)", (unsigned) w, (unsigned) h, (unsigned) t->format,
                    (unsigned) shape.width, (unsigned) shape.height, (unsigned) W, (unsigned) H,
                    (unsigned long) linearSpaceFree() >> 10, (unsigned long) linearLargest() >> 10,
                    (unsigned) gTextures.size());
        }
    }
    double etcError = -1.0;
    const bool etc = pack == re4pica::kEtc1 || pack == re4pica::kEtc1a4;
    re4pica::build(rgba.data(), w, h, pack, W, H, tex->tex.maxLevel, buildBuffer(), gEtcLog && etc ? &etcError : nullptr);
    if (etcError >= 0.0) {
        re4_log("3ds: etc1 %ux%u (GX %ux%u) alpha %d key %08x%08x: PSNR %.1f dB", (unsigned) W, (unsigned) H,
                (unsigned) w, (unsigned) h, pack == re4pica::kEtc1a4, (unsigned) t->keyHi, (unsigned) t->keyLo,
                etcError > 0 ? 10.0 * log10(65025.0 / etcError) : 99.0);
    }
    finishTexture(tex, buildBuffer());
    gMadeN[pack]++;
    gMadeTicks[pack] += svcGetSystemTick() - started;
    gTexMadeBy[t->format & 15]++;
    gTexMadeKb[t->format & 15] += tex->bytes >> 10;
    gTexMade++;
    static u32 largeLines;
    if (w * h >= 128 * 1024 && largeLines < 60) {
        largeLines++;
        re4_log("3ds: texture %ux%u format %u made at %ux%u %s (%u KB) in %.1f ms, picture %u, key %08x%08x, palette "
                "%08x", (unsigned) w, (unsigned) h, (unsigned) t->format, (unsigned) W, (unsigned) H,
                re4pica::kPackName[pack], (unsigned) tex->bytes >> 10,
                (double) (svcGetSystemTick() - started) / CPU_TICKS_PER_MSEC, gFrameNo, (unsigned) t->keyHi,
                (unsigned) t->keyLo, (unsigned) t->tlut);
    }
    return tex;
}

// (X, finding 4) VRAM's free total as of the last alloc / free (re4_vram_epoch, the top of this file)
int gVramKept = 1;
size_t vramSpaceFreeKept()
{
    static u32 epoch;
    static size_t total;
    if (!gVramKept) {
        return vramSpaceFree();
    }
    if (epoch != re4_vram_epoch) {
        epoch = re4_vram_epoch;
        total = vramSpaceFree();
    } else if (gVramKept == 2) {
        const size_t now = vramSpaceFree();
        if (now != total) {
            static u32 lines;
            if (lines++ < 8) {
                re4_log("3ds: VRAM's kept free total is wrong: %lu, the walk %lu (RE4_3DS_VRAMKEPT)",
                        (unsigned long) total, (unsigned long) now);
            }
            total = now;
        }
    }
    return total;
}

// A texture's copy in VRAM made where there is room (VRAM, the queue, the DMAs of this picture); the GPU
// reads it from there once bound again (true: moved)
__attribute__((noinline)) bool toVram(Texture* tex)
{
    const gxCmdQueue_s& q = *gQueue;
    if (gTexDmas >= kTexDmas || q.numEntries + kQueueForDma > q.maxEntries) {
        return false;
    }
    // (none stale found in this picture: not looked for again in it; each bind from linear looked through all the
    // textures, ~290 with the room of stage 5.7b: draw() +1.5 ms a picture, r100-3ds-vram57b2.log)
    static u32 noStaleIn = ~0u;
    while (gTexVramBytes + tex->bytes > kVramTex || vramSpaceFreeKept() < tex->bytes + kVramSpare) {
        if (noStaleIn == gFrameNo) {
            return false;
        }
        Texture* stale = nullptr;
        u64 staleKey = 0;
        for (auto& e : gTextures) {
            Texture* t = e.second;
            if (t->vram && t->lastFrame + kVramStale < gFrameNo && (!stale || t->lastFrame < stale->lastFrame) &&
                !mapped(t)) {
                stale = t;
                staleKey = e.first;
            }
        }
        if (!stale || !gTexEvictFrom || gFrameNo + 1 < gTexEvictFrom) {
            noStaleIn = gFrameNo;
            return false;
        }
        gTexEvicted++;
        if (!stale->linear) {  // (stage 5.7b) copied back to linear memory by a DMA where there is room, else it goes
            void* back = q.numEntries + kQueueForDma + 2 <= q.maxEntries &&
                                 linearSpaceFree() >= stale->bytes + kBackSpare
                             ? linearAlloc(stale->bytes)
                             : nullptr;
            if (!back) {
                deleteTexture(staleKey, stale);
                gTexGone++;
                continue;
            }
            GSPGPU_InvalidateDataCache(back, stale->bytes);  // (no line of the CPU's written over it later)
            GX_RequestDma((u32*) stale->vram, (u32*) back, stale->bytes);  // (before the copy into its place)
            stale->linear = back;
            stale->busyUntil = gFrameNo + 1 + gPipe;  // (the pipeline: the DMA may run a picture later)
            gTexDmas++;
        }
        forgetBound(stale);
        vramFree(stale->vram);
        stale->vram = nullptr;
        stale->tex.data = stale->linear;
        gTexVramBytes -= stale->bytes;
    }
    tex->vram = vramAlloc(tex->bytes);
    if (!tex->vram) {
        gTexVramFailed++;
        return false;
    }
    GX_RequestDma((u32*) tex->linear, (u32*) tex->vram, tex->bytes);  // flushed when made
    tex->tex.data = tex->vram;
    gTexVramBytes += tex->bytes;
    gTexDmas++;
    if (!gTexKeepLinear) {
        gTexLinearLater.push_back(tex->linear);
        tex->linear = nullptr;
    }
    return true;
}

// Stage 6.1: the movie's frame (movie_3ds.cpp) where the game's GX texture names it (re4_movie_3ds_mark): MVD's output
// (512x256 RGB565, the top row first) put into PICA tiles by a display transfer of the GX queue, in its order before
// the draws, when a new one is out; upside down (RE4_3DS_MOVIEFLIP, 1), as pica_texture.cpp lays the GX rows. Never
// to VRAM; it goes at a frame's begin once the movie is over.
Texture gMovie;
bool gMovieOn;
// The water's copy of the frame (patch 0328, game/trans.cpp GetEfbTex): the colour buffer copied whole where the game's
// copy goes (gWaterDest), read by unit 0 in the PICA's projective mode (gWaterMap: A_s, B_s, A_t, B_t of the copy's
// coordinates into the buffer's); gTexRow / gTexProj: each unit's XF texture matrix row, projective (STQ from the position)
}  // namespace
extern "C" volatile uint32_t re4_gx_water_addr, re4_gx_water_calls, re4_gx_refract_calls;  // runtime/gx_decode.cpp
namespace {
Texture gWater;
bool gWaterOn = true;  // RE4_3DS_WATER
u32 gWaterDest, gWaterFrame, gWaterCopies, gWaterNoQueue, gWaterDraws, gWaterOdd;
float gWaterMap[4], gWaterRect[4];
u32 gWaterArmed;  // the address of the next copy that is the water's (the game's tag 'WATR' in the stream)
// the draws after the water's copy in its picture read it, up to another copy to that buffer: a filter reading the
// buffer before its own copy (the copy of the picture before) showed the water's copy over the scene (r109, 2026-11-07)
bool gWaterLive;
// (2026-11-18) The models' refraction shader (game/trans.cpp RefractShaderSetup; the tag's kind 1, RE4_3DS_REFRACT):
// drawn where it reads another map besides the copy (r10c's river and dam: the copy, a render target, a highlight
// map); the copy alone is a distortion volume (mist, glass) - without the indirect warp a light block over the scene
// (r109, r10b, 2026-11-07), left out as before. Its copy: this picture's water copy again where there is one (the
// same buffer and rectangle), else made only while such draws are there.
bool gWaterArmedRefract;    // the armed copy is that shader's
bool gWaterRefract;         // the draws now read a copy it named
u32 gRefractWanted;         // the last picture one of its draws read another map (0: none yet)
u32 gRefractPending;        // its copy not made: the buffer (its draws known by the map it goes to)
int gRefractPendingMap = -1;
u32 gWaterRectBp[2];        // BP 0x49 / 0x4A of the copy made
u32 gRefractCopies, gRefractShared, gRefractNotMade, gRefractDraws, gRefractLeft;
u32 gWaterDiv;              // water draws next to a map from the normal, down the shader's plain path
// R4: the game's render targets (TexRenderMng, game/TexRender.cpp; patch 0337 tags their pass 'TXRN'): r300's sea and
// the cave's water read particles drawn into a corner of the EFB and copied (RGBA8 at half size, with a clear). Between
// the tags the EFB maps onto the frame (gMap) so that the copy's rectangle is the TH x TW pixels of its lower left
// corner (TH from the target's width, TW from its height: powers of 2 up to 128; the buffer's columns 0..TW and rows of
// memory 0..TH, whole tiles); the copy those tiles into its texture by GX_TextureCopy (gTexRenders, by the copy's
// destination), read with s and t turned (the frame is a quarter round). RE4_3DS_TEXRENDER=0: as before.
bool gTexRenderOn = true;
std::unordered_map<u32, Texture*> gTexRenders;
bool gTexRenderPass;
int gTexRenderW, gTexRenderH;  // the texture's width (columns: the GX t) and height (rows: the GX s)
float gTexRenderSaved[4];
u32 gTexRenderSize[3];  // the pass's: the copy's EFB x, the target's width and height
u32 gTexRenderPasses, gTexRenderCopies, gTexRenderDraws, gTexRenderOdd, gTexRenderNoQueue, gTexRenderDropped;
// An EFB copy read as an intensity texture (I4/I8/IA4/IA8) beside a texture from memory (stage 6.8, 2026-11-07): the
// projected shadow of a model (its silhouette in the EFB's corner, blurred by copies, read by a further map: Leon in
// the cutscene of the boat r10b, 0x01735c60) is one colour here — the copy's clear colour, what its rectangle holds
// outside the silhouette (no shadow) — instead of the whole draw left out (Leon gone). Draws of copies alone (the
// glass and the windows' light r120, the depth of field) are left out as before. RE4_3DS_COPYWHITE=0: as before.
std::unordered_map<u32, Texture*> gCopyFill;  // by the texel (LA8: L << 8 | A)
std::unordered_map<u32, u32> gCopyFillOf;     // the texel by the copy's destination (the last copy there)
bool gCopyFillOn = true;
bool gTevFill;  // the draw's TEV reads one of them (as a constant: no unit)
u32 gCopyFillDraws, gCopyFillLeft;
// (5.5) The game's contrast pass without the frame's copy (contrastBegin): RE4_3DS_CONTRAST=1
bool gContrastOn;
int gContrastPasses;  // the draw's multiply passes (0: no contrast draw)
float gContrastMul, gContrastAdd[3];  // the multiply passes' constant; the last pass's d (minus: taken off)
u32 gContrastDraws;
const u32 kWaterTag = 0x57415452u;
const u32 kTexRenderTag = 0x5458524Eu;  // 'TXRN' (R4)
int gTexRow[3];
bool gTexProj[3];
u32 gMovieSeq, gMovieTransfers;
int gMovieFlip = 1;

__attribute__((noinline)) void movieUpdate()
{
    const void* out;
    uint32_t seq;
    if (!re4_movie_3ds_frame(&out, &seq) || seq == gMovieSeq) {
        return;
    }
    const gxCmdQueue_s& q = *gQueue;
    if (q.numEntries + kQueueForDma > q.maxEntries) {
        return;  // (at the next draw)
    }
    GX_DisplayTransfer((u32*) out, GX_BUFFER_DIM(512, 256), (u32*) gMovie.tex.data, GX_BUFFER_DIM(512, 256),
                       GX_TRANSFER_FLIP_VERT(gMovieFlip) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |
                           GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
                           GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    gMovieSeq = seq;
    gMovieTransfers++;
}

bool movieTexture(u32 map)
{
    const void* out;
    uint32_t seq;
    if (!re4_movie_3ds_frame(&out, &seq)) {
        return false;
    }
    if (!gMovieOn) {
        gMovie = Texture();
        if (!C3D_TexInit(&gMovie.tex, 512, 256, GPU_RGB565)) {
            re4_log("3ds: movie texture 512x256 not made: no room in linear memory (%lu KB free, %lu KB the largest piece)",
                    (unsigned long) linearSpaceFree() >> 10, (unsigned long) linearLargest() >> 10);
            return false;
        }
        C3D_TexSetFilter(&gMovie.tex, GPU_LINEAR, GPU_LINEAR);
        gMovie.linear = gMovie.tex.data;
        gMovie.bytes = gMovie.tex.size;
        gMovie.lastFrame = gFrameNo;
        const char* e = getenv("RE4_3DS_MOVIEFLIP");
        gMovieFlip = e ? atoi(e) != 0 : 1;
        gMovieOn = true;
        gMovieSeq = 0;
        gMovieTransfers = 0;
        re4_log("3ds: movie texture 512x256 RGB565 from picture %u, upside down %d", gFrameNo, gMovieFlip);
    }
    movieUpdate();
    gMaps[map] = &gMovie;
    return true;
}

Texture* copyFill(u32 address)
{
    auto of = gCopyFillOf.find(address);
    const u32 texel = of != gCopyFillOf.end() ? of->second : 0;  // (no copy seen: black, as the GameCube's clear)
    auto it = gCopyFill.find(texel);
    if (it != gCopyFill.end()) {
        return it->second;
    }
    void* texels = linearMemAlign(8 * 8 * 2, 0x80);
    if (!texels) {
        return nullptr;
    }
    u16* p = (u16*) texels;
    for (int i = 0; i < 8 * 8; i++) {
        p[i] = (u16) texel;
    }
    GSPGPU_FlushDataCache(texels, 8 * 8 * 2);  // (flushData comes later)
    Texture* f = new Texture();
    C3D_Tex& t = f->tex;
    t.data = texels;
    t.fmt = GPU_LA8;
    t.size = 8 * 8 * 2;
    t.width = 8;
    t.height = 8;
    t.param = GPU_TEXTURE_MAG_FILTER(GPU_NEAREST) | GPU_TEXTURE_MIN_FILTER(GPU_NEAREST) |
              GPU_TEXTURE_WRAP_S(GPU_REPEAT) | GPU_TEXTURE_WRAP_T(GPU_REPEAT);
    f->linear = texels;
    f->bytes = t.size;
    f->mipNear = gMipNearFrom != 0;  // (no mipmaps, no LOD bias: those switches left alone)
    f->biased = true;
    gCopyFill[texel] = f;
    re4_log("3ds: intensity EFB copies beside textures: one colour L %u A %u from picture %u (stage 6.8)",
            (unsigned) (texel >> 8), (unsigned) (texel & 0xFF), gFrameNo);
    return f;
}

float copyFillValue(const Texture* t)  // (-1: not one of them)
{
    if (!t || gCopyFill.empty()) {
        return -1.0f;
    }
    for (const auto& f : gCopyFill) {
        if (f.second == t) {
            return (float) (f.first >> 8) / 255.0f;
        }
    }
    return -1.0f;
}

void texture(const Re4GxTexture* t, const uint8_t* data, const uint8_t* tlut, uint32_t entries)
{
    flushBatch();
    if (t->map >= 8) {
        return;
    }
    gMapSource[t->map] = MapSource{*t, data, tlut, entries};  // (stage 6.9: a map with no unit: its average)
    if (gTevDropMask >> t->map & 1) {
        gTevDirty = true;
    }
    if (copyFillValue(gMaps[t->map]) >= 0) {
        gTevDirty = true;  // (stage 6.8: the colour was a constant of its stages)
    }
    gMaps[t->map] = nullptr;
    if ((t->flags & RE4_GX_TEX_COPY) && gRefractPending && t->address == gRefractPending) {
        gRefractPendingMap = (int) t->map;  // (2026-11-18: the refraction shader's copy, not made)
    } else if (gRefractPendingMap == (int) t->map) {
        gRefractPendingMap = -1;
    }
    if ((t->flags & RE4_GX_TEX_COPY) && !gTexRenders.empty()) {
        auto tr = gTexRenders.find(t->address);
        if (tr != gTexRenders.end()) {
            gMaps[t->map] = tr->second;  // (R4: a render target's copy)
            return;
        }
    }
    if ((t->flags & RE4_GX_TEX_COPY) && gWater.tex.data && gWaterLive && t->address == gWaterDest) {
        gMaps[t->map] = &gWater;  // (the water's copy, made before in this picture)
        // GX's s over t->width texels of the copy, each (copy width / t->width) EFB pixels (2 at half size), t alike;
        // into the buffer through the picture's place (x = 240 - screen y, y = 400 - screen x, t = (y + 112) / 512)
        const float f = t->width ? gWaterRect[2] / (float) t->width : 1.0f;
        const float x = gWaterRect[0], y = gWaterRect[1], w = (float) t->width * f, h = (float) t->height * f;
        gWaterMap[0] = (240.0f - gMapOy - gMapSy * y) / 256.0f;  // s from the GX t
        gWaterMap[1] = -gMapSy * h / 256.0f;
        gWaterMap[2] = (512.0f - gMapOx - gMapSx * x) / 512.0f;  // t from the GX s
        gWaterMap[3] = -gMapSx * w / 512.0f;
        return;
    }
    if ((t->flags & RE4_GX_TEX_COPY) && t->format <= 3 && gCopyFillOn) {
        gMaps[t->map] = copyFill(t->address);  // (an intensity copy: one colour, where the draw has a texture too)
        gTevDirty = true;  // (its stages: a constant, no unit)
        return;
    }
    if (!data || !t->dataBytes) {
        return;  // an EFB copy (later), or not in memory
    }
    if (data == re4_movie_3ds_mark && movieTexture(t->map)) {
        return;  // (stage 6.1)
    }
    const u64 key = (u64) t->keyHi << 32 | t->keyLo;
    auto it = gTextures.find(key);
    if (it != gTextures.end()) {
        gMaps[t->map] = it->second;
        if (it->second->preloaded) {  // (2026-11-24) read ahead: of use
            it->second->preloaded = false;
            gPreUsed++;
        }
        return;
    }
    if (!gTexRetry.empty()) {
        auto again = gTexRetry.find(key);
        if (again != gTexRetry.end()) {
            if (gFrameNo < again->second) {
                gTexNotRetried++;
                return;
            }
            gTexRetry.erase(again);
        }
    }
    const u64 from = svcGetSystemTick();
    Texture* tex = nullptr;
    // (in the pack in a format made here: known without its index's block read from the SD card)
    const bool here = pakUsable() && std::binary_search(gPakHere.begin(), gPakHere.end(), pakFold(key));
    gPakHereHits += here;
    const re4pica::PakEntry* packed = here ? nullptr : inPak(key);
    if (!packed && !here) {
        dumpMiss(t, data, tlut, entries);
    }
    try {
        tex = packed && (gPakFormats >> packed->pack & 1) ? loadPacked(*packed) : nullptr;
        tex = tex ? tex : makeTexture(t, data, tlut, entries);
    } catch (const std::bad_alloc&) {
        // the C heap without room for the decoded texels (stage 6.2: the radio's screen, r100-3ds-live3.log): its
        // draws are left out, as a texture not made
        gTexFailed++;
        static u32 lines;
        if (lines < 20) {
            lines++;
            struct mallinfo mi = mallinfo();
            re4_log("3ds: texture %ux%u format %u not made: no room in the C heap (%u KB in use, %u KB free)",
                    (unsigned) t->width, (unsigned) t->height, (unsigned) t->format, (unsigned) mi.uordblks >> 10,
                    (unsigned) mi.fordblks >> 10);
        }
    }
    gTexTicks += svcGetSystemTick() - from;
    if (tex) {
        gTextures[key] = tex;
        gMaps[t->map] = tex;
    } else if (gTexRetry.size() < 1024) {
        gTexRetry[key] = gFrameNo + kTexRetryAfter;
    }
}

// The BP register of texture map `map` (TX_SETMODE0 0x80.., the maps 4..7 at 0xA0..)
u32 mapRegister(u32 map, u32 offset)
{
    return (map < 4 ? 0x80 + map : 0xA0 + map - 4) + offset;
}

GPU_TEXTURE_WRAP_PARAM wrapOf(u32 v)
{
    return v == 1 ? GPU_REPEAT : v == 2 ? GPU_MIRRORED_REPEAT : GPU_CLAMP_TO_EDGE;
}

// Where the renderer's time goes (stage 3.3b): the CPU in draw(); the waits for the GPU in
// C3D_FrameBegin (each part of a frame waits for the one before, whose staging area it reuses);
// the GPU's own time (C3D_GetDrawingTime of each part); command words and uniform rows sent.
struct Stats {
    u32 frames, draws, gpuDraws, skipped, untextured, vertices, splits, keptDraws, keptVertices, kicks, culled;
    u32 fogDraws, fogTables;
    u32 litDraws, lights, lightsFull, lightsFar, lightApprox, rowsOut;
    u32 litVertices, divVertices, unitVertices[4];
    u32 vramDraws, vramVertices, dmas, vramFailed;
    u32 moved, roomDraws;  // blocks moved to VRAM, draws of moved blocks from their room (stage 5.7b)
    u32 viewCulled, viewCulledIndices;
    u32 idleKicks;
    u32 indices;
    u64 ticks, drawTicks, waitTicks, cmdWords, rowVectors;
    u64 flushTicks, flushMax;  // the linear memory's flushes in kick() and frameEnd() (stage 3.8)
    u32 flushes;
    double gpuMs;
};
Stats gStats;

// The GPU's busy time (RE4_3DS_GPUBUSY, stage 3.5): a thread on core 1 looks at citro3d's command queue
// every ~1 ms; busy while an entry sent has not completed (a command list, the transfer to the screen)
volatile u32 gBusySamples, gAllSamples;
u64 gBusyFrom;  // the clock at the last log line
bool gBusyOn;

// (occlusion cull, 2026-11-20; docs/22-occlusion.md) The depth of a picture after the room's models (before table
// RE4_3DS_OCC_COPYAT), as 8 x 8 tiles of the farthest distance, for the game's thread (re4_occ_info, the end of
// this file): the copy in the GX queue (as the water's), the tiles two pictures on (that queue done)
struct OccHiz {
    u32 seq, tag;
    float m[4][4];       // eye -> the screen's clip (x over 400, y over 240, +1 at the top): the last perspective's
    float scale;         // the w-buffer's depth (0..1) -> the distance (p5 / p4); 0: not a w-buffer picture
    float tile[50][30];  // the copy's 8 x 8 tiles (its x = 239 - screen y, its rows = screen x / 8): the farthest
};                       // distance; INFINITY: cleared there
int gOccOn = 1, gOccCopyAt = 12;
u8* gOccBuf[2];
u32 gOccBufFrame[2] = {~0u, ~0u}, gOccBufTag[2];
float gOccBufM[2][4][4], gOccBufScale[2];
bool gOccCopied;
u32 gOccPicTag;
float gOccM[4][4], gOccScale;
u32 gOccCopies, gOccBuilds, gOccNoQueue, gOccSvcFail;
u64 gOccBuildTicks;
std::mutex gOccLock;
OccHiz gOccPub;
extern "C" unsigned re4_gx_timer_every;  // (X, finding 8; gx_thread_3ds.cpp)
void occCopy();
void occBuild();

void busySampler(void*)
{
    for (;;) {
        const gxCmdQueue_s& q = gQueue1.lastEntry < gQueue1.curEntry ? gQueue1 : __C3D_Context;  // (the pipeline: either)
        gAllSamples++;
        re4_hud_gpu_all++;
        if (q.lastEntry < q.curEntry) {
            gBusySamples++;
            re4_hud_gpu_busy++;
        }
        svcSleepThread(1000000);  // (0.1 ms before, 10 000 wake-ups a second on core 1)
    }
}

// The commands so far to the GPU, now: the CPU's writes to linear memory flushed (the vertices,
// indices and lasting room since the last kick; all of it: about as fast as C3D_FrameEnd's), the
// command list split off and the queue run (it stops when empty, and runs on as more are added).
// C3D_FrameEnd with the queue possibly running: the CPU's writes flushed before its list goes
// The CPU's writes to linear memory flushed by the OS kernel itself (stage 3.8): svcFlushProcessDataCache of the
// whole heap 0.18 ms, the request to GSP 0.30 (r100-3ds-flushbench1.log). RE4_3DS_FLUSHSVC: 0 through GSP, N from
// picture N
u32 gFlushSvcFrom = 1;
// C3D_UpdateUniforms only when a uniform of the shader type is dirty (stage 3.8): citro3d's C3Di_UpdateContext calls it
// for the vertex and the geometry shader at every draw, each looking at the 96 float flags byte by byte (~1 300
// instructions a draw); this renderer has no geometry shader. After C3D_BindProgram the first call of each type goes
// to citro3d all the same (the shader's own constants: their flag is citro3d's own). RE4_3DS_UNIFSKIP: 0 always
// citro3d, N from picture N
u32 gUnifSkipFrom = 1;
bool gUnifBound[2];  // a call of the type since the program's bind
// The whole linear memory before a kick only cleaned (stage 3.8): svcFlushProcessDataCache of all 30 MB cleans and
// invalidates the whole cache (the L2 of all cores), the threads read memory again after it (~0.5 ms a flush besides
// its own 0.14, bench5); the GPU needs the writes only. Not the CPU's writes there: the texture DMA back (invalidated
// before it), the GPU's copies. RE4_3DS_FLUSHSTORE: 0 flushed, N from picture N
u32 gFlushStoreFrom = 1;

void flushData(void* p, u32 bytes)
{
    if (gFlushSvcFrom && gFrameNo + 1 >= gFlushSvcFrom) {
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32) p, bytes);
    } else {
        GSPGPU_FlushDataCache(p, bytes);
    }
}

// The CPU's writes to linear memory flushed, all of it, timed (stage 3.8: since 5.7c 30 MB a flush; a range costs
// more than all of it from ~64 KB on: the kernel cleans the whole cache for large ones)
void flushLinear()
{
    const u64 t0 = svcGetSystemTick();
    if (gFlushStoreFrom && gFrameNo + 1 >= gFlushStoreFrom) {
        svcStoreProcessDataCache(CUR_PROCESS_HANDLE, __ctru_linear_heap, __ctru_linear_heap_size);
    } else {
        flushData((void*) __ctru_linear_heap, __ctru_linear_heap_size);
    }
    const u64 t = svcGetSystemTick() - t0;
    gStats.flushTicks += t;
    gStats.flushMax = t > gStats.flushMax ? t : gStats.flushMax;
    gStats.flushes++;
}

void frameEnd()
{
    if (gOverlap) {
        flushLinear();
        C3D_FrameEnd(GX_CMDLIST_FLUSH);
    } else {
        C3D_FrameEnd(0);
    }
}

bool unifDirty(const bool* flags, u32 n)
{
    if (!((uintptr_t) flags & 3)) {
        const u32* w = (const u32*) flags;
        u32 any = 0;
        for (u32 i = 0; i < n / 4; i++) {
            any |= w[i];
        }
        return any;
    }
    bool any = false;
    for (u32 i = 0; i < n; i++) {
        any |= flags[i];
    }
    return any;
}

}  // namespace

extern "C" void __real_C3D_UpdateUniforms(GPU_SHADER_TYPE type);
extern "C" void __wrap_C3D_UpdateUniforms(GPU_SHADER_TYPE type)
{
    if (gUnifSkipFrom && gFrameNo + 1 >= gUnifSkipFrom && (type == 0 || type == 1) && gUnifBound[type] &&
        !C3D_BoolUnifsDirty[type] && !unifDirty(C3D_IVUnifDirty[type], C3D_IVUNIF_COUNT) &&
        !unifDirty(C3D_FVUnifDirty[type], C3D_FVUNIF_COUNT)) {
        return;
    }
    if (type == 0 || type == 1) {
        gUnifBound[type] = true;
    }
    __real_C3D_UpdateUniforms(type);
}

extern "C" {
void (*re4_3ds_gx_wake_fn)(void);  // gx_thread_3ds.cpp: the GX thread woken from its sleep on the game (the pipeline)
}

namespace {

// (the pipeline) The queue of the picture before has run out: the current picture's is libctru's running one from now
// on (what it holds goes to the GPU; what is added later starts by itself, gxCmdQueueAdd). On the GX thread only.
void pump()
{
    if (gRun == gCur) {
        return;
    }
    const volatile gxCmdQueue_s* before = gQueues[gRun];
    if (before->lastEntry >= before->numEntries && gxCmdQueueWait(gQueues[gRun], 0)) {
        gxCmdQueueRun(gQueues[gCur]);
        gRun = gCur;
        gPipeHandoffs++;
    }
}

// A GX queue has run out (libctru's event thread; citro3d's own callback first: its swap of the screen's buffers when
// not in the pipeline). The pipeline: the picture whose transfer to the screen is done is shown; the GX thread, if it
// sleeps on the game, is woken to make the next queue run.
void (*gC3dQueueDone)(gxCmdQueue_s*);
void queueDone(gxCmdQueue_s* q)
{
    const u32 i = q == &gQueue1;
    if (!i && gC3dQueueDone) {
        gC3dQueueDone(q);
    }
    const u32 at = gPresentAt[i];
    if (at && q->lastEntry >= at) {
        gfxScreenSwapBuffers(GFX_TOP, false);
        gPresentAt[i] = 0;
        gShown = gShown + 1;
    }
    if (gPipe && gRun != gCur && re4_3ds_gx_wake_fn) {
        re4_3ds_gx_wake_fn();
    }
}

void kick()
{
    const gxCmdQueue_s& q = *gQueue;
    if (q.numEntries + kQueueForKick > q.maxEntries) {
        return;
    }
    flushLinear();
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    if (gPipe) {
        pump();  // (the current queue runs by itself once it is libctru's)
    } else {
        gxCmdQueueRun(&__C3D_Context);
    }
    gKicks++;
    gStats.kicks++;
}

// A draw of count u16 indices (C3D_DrawElements(prim, count, C3D_UNSIGNED_SHORT, indices), gDrawOwnFrom)
void drawElements(GPU_Primitive_t prim, u32 count, const void* indices)
{
    if (!gDrawOwnFrom || gFrameNo + 1 < gDrawOwnFrom || !gBufCtx) {
        C3D_DrawElements(prim, (int) count, C3D_UNSIGNED_SHORT, indices);
        return;
    }
    const u32 pa = osConvertVirtToPhys(indices);
    const u32 base = gBufCtx->base_paddr;
    if (pa < base) {
        return;
    }
    C3Di_UpdateContext();
    if (gpuCmdBufSize - gpuCmdBufOffset < 64) {
        C3D_DrawElements(prim, (int) count, C3D_UNSIGNED_SHORT, indices);  // (its state written: the draw alone)
        return;
    }
    // Each register write a pair: the value, then the header (GPUCMD_Add of a word)
    u32* w = gpuCmdBuf + gpuCmdBufOffset;
    const bool tri = prim == GPU_TRIANGLES;
    w[0] = tri ? GPU_GEOMETRY_PRIM : prim;  // GPUREG_PRIMITIVE_CONFIG, mask 2
    w[1] = 0x0002025e;
    w[2] = 1;  // GPUREG_RESTART_PRIMITIVE
    w[3] = 0x000f025f;
    w[4] = (pa - base) | 0x80000000;  // GPUREG_INDEXBUFFER_CONFIG: the offset, u16 indices
    w[5] = 0x000f0227;
    w[6] = count;  // GPUREG_NUMVERTICES
    w[7] = 0x000f0228;
    w[8] = 0;  // GPUREG_VERTEX_OFFSET
    w[9] = 0x000f022a;
    w += 10;
    if (tri) {
        w[0] = 0x100;  // GPUREG_GEOSTAGE_CONFIG, mask 2: triangle elements
        w[1] = 0x00020229;
        w[2] = 0x100;  // GPUREG_GEOSTAGE_CONFIG2, mask 2
        w[3] = 0x00020253;
        w += 4;
    }
    w[0] = 0;  // GPUREG_START_DRAW_FUNC0, mask 1: drawing
    w[1] = 0x00010245;
    w[2] = 1;  // GPUREG_DRAWELEMENTS
    w[3] = 0x000f022f;
    w[4] = 1;  // GPUREG_START_DRAW_FUNC0: configuration again
    w[5] = 0x00010245;
    w += 6;
    if (tri) {
        w[0] = 0;
        w[1] = 0x00020229;
        w[2] = 0;
        w[3] = 0x00020253;
        w += 4;
    }
    w[0] = 1;  // GPUREG_VTX_FUNC: the post-vertex cache cleared
    w[1] = 0x000f0231;
    w[2] = 0;  // GPUREG_PRIMITIVE_CONFIG, mask 8 (twice, as citro3d)
    w[3] = 0x0008025e;
    w[4] = 0;
    w[5] = 0x0008025e;
    w += 6;
    gpuCmdBufOffset = w - gpuCmdBuf;
    *(u32*) ((u8*) gBufCtx - 32) |= 2;  // C3DiF_DrawUsed (the context's flags +32, its bufInfo +64)
}

void flushBatch()
{
    gXfDeferRows = 0;  // (stage 7.0)
    gXfDeferOther = false;
    if (gBatch.open) {
        drawElements(gBatch.strip ? GPU_TRIANGLE_STRIP : GPU_TRIANGLES, gBatch.count, gBatch.indices);
        gStats.indices += gBatch.count;
        gStats.gpuDraws++;
        gBatch.open = false;
        if (gOverlap && ++gKickDraws >= gKickEvery && gKicks < gKicksMax &&
            (gPipe || gxCmdQueueWait(&__C3D_Context, 0))) {
            gKickDraws = 0;  // (the queue idle: the GPU done with all before; a list at a time, as citro3d has it;
            kick();          // the pipeline: into a running queue too, libctru hands GSP up to 3 commands)
        } else {
            pump();
        }
    }
}

// Fixed attribute values as last given (citro3d sends all fixed attributes when one is written)
float gFixed[12][4];
bool gFixedKnown[12];

void setFixed(int id, float x, float y, float z, float w)
{
    if (gFixedKnown[id] && gFixed[id][0] == x && gFixed[id][1] == y && gFixed[id][2] == z && gFixed[id][3] == w) {
        return;
    }
    C3D_FixedAttribSet(id, x, y, z, w);
    gFixed[id][0] = x;
    gFixed[id][1] = y;
    gFixed[id][2] = z;
    gFixed[id][3] = w;
    gFixedKnown[id] = true;
}

// What each texture unit has bound (and its wrap bits), to bind only on a change
Texture* gBound[3];
u32 gBoundWrap[3];
// The channel's uniforms as last set (chan[]: matv, matc, ambv, ambc); what each light slot of the
// shader holds (the XF light, 0xFF nothing yet; its mode: 1 no attenuation, 2 short, 3 whole, 4 far; the
// light's write count at the upload) and the loops over the slots as set (nfar, nbrief, nwhole)
float gColor[4][4];
bool gColorKnown;
u8 gSlotLight[8], gSlotMode[8];
u32 gSlotGen[8];
int gLoopFar, gLoopBrief, gLoopBriefFrom, gLoopWhole, gLoopWholeFrom;
// The shader's switches (bool uniforms, by name) and their state as set (-1 not yet): lit (a light
// on), far / brief / whole (lights far, the short / the whole way), tdiv (a coordinate over its w: the packed
// normal), tu1 / tu2 (units 1 / 2 read)
enum { kLit, kBrief, kWhole, kTexDiv, kTex1, kTex2, kFar, kTexW0, kFarPos, kSwitches };
const char* const kSwitchName[kSwitches] = {"lit", "brief", "whole", "tdiv", "tu1", "tu2", "far", "tw0", "farpos"};
bool gFarPos;  // (5.5) the far lights' direction from each vertex (their slot: the position); RE4_3DS_FARPOS=0: as before
int gSwitchLoc[kSwitches];
s8 gSwitch[kSwitches];

void setSwitch(int i, bool on)
{
    if (gSwitch[i] != (s8) on) {
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, gSwitchLoc[i], on);
        gSwitch[i] = (s8) on;
    }
}

// A draw's vertices counted, by the shader's path (the batch's: its switches and units)
void countVertices(u32 n)
{
    gStats.vertices += n;
    gStats.litVertices += gSwitch[kLit] == 1 ? n : 0;
    gStats.divVertices += gSwitch[kTexDiv] == 1 ? n : 0;
    gStats.unitVertices[gUnits] += n;
}

void forgetBound(const Texture* tex)
{
    for (Texture*& b : gBound) {
        b = b == tex ? nullptr : b;
    }
}

float asFloat(u32 v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

// GX projection (XF 0x1020-0x1026), turned a quarter for the 3DS screen (x' = y, y' = -x, as
// citro3d's Mtx_PerspTilt). The depth range of GX ([-w, 0], near at -1) is the PICA's.
// (5.5) The last perspective projection's p4, p5 (orthoDepth); RE4_3DS_ORTHODEPTH=0: off
float gPerspP4, gPerspP5;
bool gOrthoDepthOn = true;

void uploadProjection(const u32* xfReg)
{
    const float* p = (const float*) &xfReg[0x20];
    if (xfReg[0x26] == 0) {
        gPerspP4 = p[4];
        gPerspP5 = p[5];
    }
    float m[4][4] = {};
    if (xfReg[0x26] == 0) {  // perspective
        m[0][0] = p[0];
        m[0][2] = p[1];
        m[1][1] = p[2];
        m[1][2] = p[3];
        m[2][2] = p[4];
        m[2][3] = p[5];
        m[3][2] = -1.0f;
    } else {  // orthographic
        m[0][0] = p[0];
        m[0][3] = p[1];
        m[1][1] = p[2];
        m[1][3] = p[3];
        m[2][2] = p[4];
        m[2][3] = p[5];
        m[3][3] = 1.0f;
    }
    // the viewport (XF 0x101A-0x101F: half width, -half height, offsets + 342): GX's NDC to EFB pixels, those to the
    // screen (gMap), to the screen's NDC: x' = a x + b w, y' = c y + d w (stage 5.4)
    const float vsx = asFloat(xfReg[0x1A]), vsy = asFloat(xfReg[0x1B]);
    if (vsx != 0.0f && vsy != 0.0f) {
        const float vox = asFloat(xfReg[0x1D]) - 342.0f, voy = asFloat(xfReg[0x1E]) - 342.0f;
        const float a = gMapSx * vsx / 200.0f, b = (gMapOx + gMapSx * vox) / 200.0f - 1.0f;
        const float c = -gMapSy * vsy / 120.0f, d = 1.0f - (gMapOy + gMapSy * voy) / 120.0f;
        for (int i = 0; i < 4; i++) {
            m[0][i] = a * m[0][i] + b * m[3][i];
            m[1][i] = c * m[1][i] + d * m[3][i];
        }
    }
    if (gOccOn && xfReg[0x26] == 0) {  // (occlusion cull) the last perspective's eye -> the screen's clip
        memcpy(gOccM, m, sizeof(gOccM));
        gOccScale = gFogMode == 2 && p[5] < 0.0f ? p[5] / p[4] : 0.0f;
    }
    C3D_FVec* u = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, gLocProj, 4);
    const float* rows[4] = {m[1], m[0], m[2], m[3]};
    for (int r = 0; r < 4; r++) {
        const float sign = r == 1 ? -1.0f : 1.0f;
        u[r].x = sign * rows[r][0];
        u[r].y = sign * rows[r][1];
        u[r].z = sign * rows[r][2];
        u[r].w = sign * rows[r][3];
    }
    // the depth: -z/w (near at 1) or, for the fog (RE4_3DS_FOG=2), GX's way round (near at 0): a perspective's with the
    // w-buffer, distance/far (w p4/p5, the same for every projection with that far plane: the fog table's steps over the
    // distance; 1 - distance/far lost the float24's bits near 1, Leon's layers fought, 01.11.2026), else z/w + 1
    if (gFogMode == 2) {
        const bool w = xfReg[0x26] == 0 && p[5] < 0.0f;
        C3D_DepthMap(!w, w ? 0.0f : 1.0f, w ? p[4] / p[5] : 1.0f);
    }
    gProjDirty = false;
}

// (5.5, 2026-11-09) With RE4_3DS_FOG=2 the perspective draws write distance / far (the w-buffer), the orthographic ones
// GX's depth z/w + 1: the two do not compare. The screen-space sprites the game tests against the scene (a lamp's glow
// on the wall, r318: an orthographic quad whose z gives the GX depth of its point, D = 1 - p4 + p5 / d) were behind
// every wall. Such a draw (orthographic, the depth compared) gets its depth in the scene's scale, constant: the GX depth
// of its first vertex to the distance over far, p4 / (D - 1 + p4) of the last perspective projection. True: the
// projection to be uploaded again at the next draw (and no join to this one).
bool orthoDepth(const Re4GxDraw& d, const void* vertices, const uint32_t* bp, const uint32_t* xfMem,
                const uint32_t* xfReg)
{
    if (!gOrthoDepthOn || gFogMode != 2 || xfReg[0x26] == 0 || !(bp[0x40] & 1) || gPerspP5 >= 0.0f ||
        d.vertexCount == 0) {
        return false;
    }
    const u8* v = (const u8*) vertices;
    const u32 row = (d.layout & RE4_GX_V_MATRIX) ? v[d.stride - 12] : d.matrices[0] & 0xFF;
    if (row * 4 + 12 > 0x100) {
        return false;
    }
    const float* q = (const float*) &xfMem[row * 4];
    float pos[3];
    memcpy(pos, v, sizeof(pos));
    const float z = q[8] * pos[0] + q[9] * pos[1] + q[10] * pos[2] + q[11];
    const float* p = (const float*) &xfReg[0x20];
    const float den = p[4] * z + p[5] + gPerspP4;  // D - 1 + p4s (minus in front of the far plane)
    const float depth = den < 0.0f ? fminf(gPerspP4 / den, 1.0f) : 1.0f;
    C3D_DepthMap(true, 0.0f, depth);
    return true;
}

// GX fog (BP 0xEE-0xF2, as gx_gl_shader.cpp) as the PICA's fog table: 128 steps over the depth here (-z/w; with
// RE4_3DS_FOG=2 z/w + 1, or distance/far with the w-buffer), each the share of the fragment's colour left (1 - fog) and
// the step to the next.
void makeFogLut(C3D_FogLut* lut, const u32* bp, const u32* xfReg, bool w)
{
    const auto fogFloat = [](u32 v) {
        return asFloat(((v >> 19) & 1) << 31 | ((v >> 11) & 0xFF) << 23 | (v & 0x7FF) << 12);
    };
    const float a = fogFloat(bp[0xEE]), c = fogFloat(bp[0xF1]), b = (float) (bp[0xEF] & 0xFFFFFF);
    const u32 shift = bp[0xF0] & 31, ortho = (bp[0xF1] >> 20) & 1, fsel = (bp[0xF1] >> 21) & 7;
    const float zRange = asFloat(xfReg[0x1C]), zFar = asFloat(xfReg[0x1F]), p4 = asFloat(xfReg[0x24]);
    float data[256];
    for (int i = 0; i <= 128; i++) {
        const float d = (float) i / 128.0f;
        // GX's z on the screen: the viewport's far + range * z/w (z/w in [-1, 0])
        float zw = gFogMode != 2 ? -d : !w ? d - 1.0f : i > 0 ? p4 / d - p4 : -1.0f;
        zw = zw < -1.0f ? -1.0f : zw;
        float zs = zFar + zRange * zw;
        zs = zs < 0.0f ? 0.0f : zs > 16777215.0f ? 16777215.0f : zs;
        const u32 z = (u32) zs;
        const float ze = ortho ? a * (float) z / 16777216.0f : a * 16777216.0f / (b - (float) (z >> shift));
        float f = ze - c;
        f = f > 0.0f ? (f < 1.0f ? f : 1.0f) : 0.0f;
        if (fsel == 4) {
            f = 1.0f - exp2f(-8.0f * f);
        } else if (fsel == 5) {
            f = 1.0f - exp2f(-8.0f * f * f);
        } else if (fsel == 6) {
            f = exp2f(-8.0f * (1.0f - f));
        } else if (fsel == 7) {
            f = exp2f(-8.0f * (1.0f - f) * (1.0f - f));
        }
        if (i < 128) {
            data[i] = 1.0f - f;
        }
        if (i > 0) {
            data[i + 127] = 1.0f - f - data[i - 1];
        }
    }
    FogLut_FromArray(lut, data);
}

// The fog of a draw: off, or the table of its registers, projection and viewport (kept, the last 4 made).
struct FogTable {
    u32 key[10];
    C3D_FogLut lut;
};
FogTable gFogTables[4];
int gFogNext;
u32 gFogKey[10] = {0xFFFFFFFF};
bool gFogOn;
C3D_FogLut* gFogBound;

void setFog(const u32* bp, const u32* xfReg)
{
    const bool on = gFogMode && ((bp[0xF1] >> 21) & 7) != 0;
    if (on != gFogOn) {
        C3D_FogGasMode(on ? GPU_FOG : GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
        gFogOn = on;
    }
    if (!on) {
        return;
    }
    gStats.fogDraws++;
    const u32 key[10] = {bp[0xEE], bp[0xEF], bp[0xF0], bp[0xF1], bp[0xF2],
                         xfReg[0x1C], xfReg[0x1F], xfReg[0x24], xfReg[0x25], xfReg[0x26]};
    if (!memcmp(key, gFogKey, sizeof(key))) {
        return;
    }
    memcpy(gFogKey, key, sizeof(key));
    const u32 rgb = bp[0xF2];
    C3D_FogColor((rgb >> 16 & 0xFF) | (rgb & 0xFF00) | (rgb & 0xFF) << 16);
    FogTable* t = nullptr;
    for (FogTable& f : gFogTables) {
        if (!memcmp(f.key, key, sizeof(key))) {
            t = &f;
        }
    }
    if (!t) {
        t = &gFogTables[gFogNext];
        gFogNext = (gFogNext + 1) & 3;
        memcpy(t->key, key, sizeof(key));
        makeFogLut(&t->lut, bp, xfReg, gFogMode == 2 && xfReg[0x26] == 0 && asFloat(xfReg[0x25]) < 0.0f);
        gStats.fogTables++;
        gFogBound = nullptr;  // (the table bound may be the one made again)
    }
    if (gFogBound != &t->lut) {
        C3D_FogLutBind(&t->lut);
        gFogBound = &t->lut;
    }
}

void uploadRows(const u32* xfMem)
{
    const int n = gRowsMax - gRowsMin + 1;
    gStats.rowVectors += n;
    C3D_FVec* u = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, gLocRows + gRowsMin, n);
    for (int i = 0; i < n; i++) {
        const u32* w = &xfMem[(gRowsMin + i) * 4];
        u[i].x = asFloat(w[0]);
        u[i].y = asFloat(w[1]);
        u[i].z = asFloat(w[2]);
        u[i].w = asFloat(w[3]);
    }
    gRowsMin = kRows;
    gRowsMax = -1;
}

void xfChanged(uint32_t address, uint32_t count, const uint32_t* words)
{
    if (gOwnState) {  // (2026-11-19) the renderer's own state
        if (address + count <= 0x800) {
            memcpy(&gOwnXfMem[address], words, 4 * count);
        } else {
            for (u32 i = 0; i < count; i++) {
                const u32 a = address + i;
                if (a < 0x800) {
                    gOwnXfMem[a] = words[i];
                } else if (a >= 0x1000 && a < 0x1058) {
                    gOwnXfReg[a - 0x1000] = words[i];
                }
            }
        }
    }
    // (stage 7.0) position and normal matrices with a batch open: the next draw may join it over them (joinMoved)
    if (gSpriteJoin && gBatch.open && !gBatch.kept && count && address + count <= 0x100) {
        for (u32 r = address / 4; r <= (address + count - 1) / 4; r++) {
            gXfDeferRows |= 1ull << r;
        }
    } else if (gSpriteJoin && gBatch.open && !gBatch.kept && count && address >= 0x400 && address + count <= 0x460) {
        gXfDeferOther = true;
    } else {
        flushBatch();
    }
    if (address < 4 * kRows) {
        const int first = (int) (address / 4);
        int last = (int) ((address + count - 1) / 4);
        last = last < kRows - 1 ? last : kRows - 1;
        gRowsMin = first < gRowsMin ? first : gRowsMin;
        gRowsMax = last > gRowsMax ? last : gRowsMax;
    }
    if (address < 0x680 && address + count > 0x600) {  // the lights, 16 words each
        const u32 first = address < 0x600 ? 0 : (address - 0x600) / 16;
        const u32 end = address + count >= 0x680 ? 8 : (address + count - 0x600 + 15) / 16;
        for (u32 n = first; n < end; n++) {
            gLightGen[n]++;
        }
    }
    if (address <= 0x1026 && address + count > 0x101A) {  // the viewport and the projection
        gProjDirty = true;
    }
}

// (R4) The render target's pass laid out onto the frame's lower left corner: screen x = s TH (the buffer's rows of
// memory 0..TH), screen y = 240 - TW + t TW (its columns TW..0); the picture's place kept for the pass's end
void texRenderPlace()
{
    gTexRenderSaved[0] = gMapSx, gTexRenderSaved[1] = gMapOx, gTexRenderSaved[2] = gMapSy, gTexRenderSaved[3] = gMapOy;
    gMapSx = (float) gTexRenderH / (float) (2 * gTexRenderSize[1]);
    gMapOx = -(float) gTexRenderSize[0] * gMapSx;
    gMapSy = (float) gTexRenderW / (float) (2 * gTexRenderSize[2]);
    gMapOy = 240.0f - (float) gTexRenderW;
    gProjDirty = true;
    gScissorKey[0] = ~0u;
}

// (R4) its end, or the picture's: the picture's place back
void texRenderUnplace()
{
    if (!gTexRenderPass) {
        return;
    }
    gMapSx = gTexRenderSaved[0], gMapOx = gTexRenderSaved[1], gMapSy = gTexRenderSaved[2], gMapOy = gTexRenderSaved[3];
    gTexRenderPass = false;
    gProjDirty = true;
    gScissorKey[0] = ~0u;
}

// (R4) placePicture: a pass begun before the picture laid out again over the picture's new place
void texRenderReplace()
{
    if (gTexRenderPass) {
        texRenderPlace();
    }
}

// (R4) A render target's pass: its begin (owner[2]: the copy's EFB x | the target's width << 16, owner[3]: its height),
// its end. The begin may come before the picture's (the picture begins at its first draw): placePicture lays it again
void texRenderTag(const uint32_t owner[4])
{
    flushBatch();  // (the draws before with the place they had)
    if (owner[1] == 1 && gTexRenderOn && !gTexRenderPass) {
        const u32 x0 = owner[2] & 0xFFFF, w = owner[2] >> 16, h = owner[3];
        if (!w || !h || w > 0x140 || h > 0x108) {
            return;
        }
        auto side = [](u32 n) {
            int s = 8;
            while ((u32) s * 2 <= n && s < 128) {
                s *= 2;
            }
            return s;
        };
        gTexRenderH = side(w);
        gTexRenderW = side(h);
        gTexRenderSize[0] = x0, gTexRenderSize[1] = w, gTexRenderSize[2] = h;
        texRenderPlace();
        gTexRenderPass = true;
        gTexRenderPasses++;
    } else if (owner[1] == 2) {
        texRenderUnplace();
    }
}

void ownerTag(const uint32_t owner[4])
{
    if (owner[0] == kTexRenderTag) {
        texRenderTag(owner);
        return;
    }
    if (owner[0] == kWaterTag) {
        gWaterArmed = owner[1];  // (the water's copy next; the owner of the draws left as it was)
        gWaterArmedRefract = owner[2] == 1;  // (2026-11-18)
        return;
    }
    if (gOccOn && gInFrame && !gOccCopied && owner[2] >= gOccCopyAt && owner[2] != 0xFF) {
        occCopy();  // (occlusion cull) the room's models drawn: their depth
    }
    gOwnerData = owner[0];
}

// (the pipeline) What the picture before let go, freed a picture later (frameBegin())
std::vector<void*> gDroppedBefore, gRoomsCopiedBefore, gTexLinearBefore;
// (stage 41) the decoder's pieces of linear memory (the heads by the skinning jobs) let go: back to it at the begin
// of the next picture (the pipeline: of the one after)
std::vector<void*> gHeadLater, gHeadBefore;

// The pipeline from this picture on (frameBegin() at a picture's begin: C3D_FrameBegin has left citro3d's queue stopped
// and empty and taken its command buffer; the GPU is done with all before)
void pipeStart()
{
    if (gPipe || gInFrame || !gPipeFrom || !gOverlap || !gQueues[1] || gFrameNo + 1 < gPipeFrom) {
        return;
    }
    gPipe = true;
    gCmdBufs[0] = gpuCmdBuf;
    gCmdWords = gpuCmdBufSize;
    gCur = 0;
    gRun = 0;
    gQueue = gQueues[0];
    gxCmdQueueRun(gQueue);  // (libctru's running queue: its entries start as they are added)
    re4_log("3ds: pictures in a pipeline from picture %u (two GX queues, command buffers of %u words)", gFrameNo,
            (unsigned) gCmdWords);
}

// A picture's begin in the pipeline: the picture before the last one drawn and shown (its queue, command buffer and
// staging area are this picture's now); the one before may still be the GPU's
void pipeBegin(u64 from)
{
    bool waited = false, told = false;
    for (;;) {
        pump();
        if (gRun == gCur && !gPresentAt[gCur ^ 1]) {
            break;
        }
        if (!told && svcGetSystemTick() - from > 3ull * SYSCLOCK_ARM11) {
            told = true;
            const gxCmdQueue_s& a = *gQueues[0];
            const gxCmdQueue_s& b = *gQueues[1];
            re4_log("3ds: GPU busy for 3 s at a picture's begin (pipeline): queues %u/%u entries, %u/%u sent, %u/%u done; "
                    "current %u, running %u, to show at %u/%u; kicks %u, frame %u",
                    (unsigned) a.numEntries, (unsigned) b.numEntries, (unsigned) a.curEntry, (unsigned) b.curEntry,
                    (unsigned) a.lastEntry, (unsigned) b.lastEntry, (unsigned) gCur, (unsigned) gRun,
                    (unsigned) gPresentAt[0], (unsigned) gPresentAt[1], gKicks, gFrameNo);
        }
        waited = true;
        svcSleepThread(100000);
    }
    gPipeWaited += waited;
    gCur = gCur ^ 1;
    gQueue = gQueues[gCur];
    gxCmdQueueClear(gQueue);  // (not libctru's running one)
    GX_BindQueue(gQueue);
    pump();
    GPUCMD_SetBuffer(gCmdBufs[gCur], gCmdWords, 0);
    gStage = gStageBufs[gCur];
    gIndices = gIndexBufs[gCur];
}

// A picture split in the pipeline (the staging area or the command buffer full): all of it so far drawn, the picture
// before too; then on from the buffers' begin, into the same target
void pipeSplit()
{
    gStats.cmdWords += gpuCmdBufOffset;
    const u64 from = svcGetSystemTick();
    flushLinear();
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    for (;;) {
        pump();
        const volatile gxCmdQueue_s* q = gQueue;
        if (gRun == gCur && q->lastEntry >= q->numEntries && gxCmdQueueWait(gQueue, 0)) {
            break;
        }
        svcSleepThread(100000);
    }
    gStats.waitTicks += svcGetSystemTick() - from;
    gxCmdQueueClear(gQueue);  // (libctru's running one, idle: what is added starts by itself)
    GPUCMD_SetBuffer(gCmdBufs[gCur], gCmdWords, 0);
    gKickDraws = gKicks = 0;
    C3D_FrameDrawOn(gTarget);
    if (gGpuTest == 1) {
        C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, 1, 1);
    }
    gStageUsed = 0;
    gIndicesUsed = 0;
    gScissorKey[0] = ~0u;
    gStats.splits++;
}

// A picture's end in the pipeline: the rest of its commands, then the transfer to the top screen's buffer that is not
// shown when the transfer runs — the back one now, or the other one while the picture before is yet to be shown (its
// swap comes first: the queues run in order). queueDone() shows it when the transfer's entry is done.
void pipeEnd()
{
    gStats.cmdWords += gpuCmdBufOffset;
    flushLinear();
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    u8* out;
    u32 before, after;
    do {
        before = gPresentAt[gCur ^ 1];
        out = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
        after = gPresentAt[gCur ^ 1];
    } while (!before != !after);
    if (before) {
        u8* other = out == gTopBufs[0] ? gTopBufs[1] : out == gTopBufs[1] ? gTopBufs[0] : nullptr;
        if (!other) {  // (not met yet: after the swap, the back one)
            while (gPresentAt[gCur ^ 1]) {
                svcSleepThread(100000);
            }
            other = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
        }
        out = other;
    }
    if (out != gTopBufs[0] && out != gTopBufs[1]) {
        gTopBufs[gTopBufs[0] ? 1 : 0] = out;
    }
    const C3D_FrameBuf& fb = gTarget->frameBuf;
    const u32 dim = GX_BUFFER_DIM((u32) fb.width, (u32) fb.height);
    gPresentAt[gCur] = gQueue->numEntries + 1u;  // (before the entry is there: queueDone() compares the entries done)
    GX_DisplayTransfer((u32*) fb.colorBuf, dim, (u32*) out, dim, kDisplayTransfer);
    pump();
}

// C3D_FrameBegin, waiting for the part of the frame before; the GPU's time for that part
void frameBegin()
{
    const u64 from = svcGetSystemTick();
    if (gPipe) {
        pipeBegin(from);
    } else if (gOverlap) {
        // the queue's state on the log when the GPU seems stuck (3 s), then waited for as before
        while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
            if (svcGetSystemTick() - from > 3ull * SYSCLOCK_ARM11) {
                const gxCmdQueue_s& q = __C3D_Context;
                re4_log("3ds: GPU busy for 3 s at a frame's begin: queue %u entries, %u sent, %u done; kicks %u, frame %u",
                        (unsigned) q.numEntries, (unsigned) q.curEntry, (unsigned) q.lastEntry, gKicks, gFrameNo);
                C3D_FrameBegin(0);
                break;
            }
            svcSleepThread(100000);
        }
    } else {
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    }
    pipeStart();
    gStats.waitTicks += svcGetSystemTick() - from;
    gKickDraws = gKicks = 0;
    auto freeRoom = [](void* p) {
        gKeptLive -= keepSize(p);
        if (gArena) {
            arenaFree(p);
        } else {
            linearFree(p);
        }
    };
    if (gPipe) {  // what the picture before let go may still be the GPU's: freed now what the one before it did
        gDropped.swap(gDroppedBefore);
        gRoomsCopied.swap(gRoomsCopiedBefore);
        gTexLinearLater.swap(gTexLinearBefore);
        gHeadLater.swap(gHeadBefore);
    }
    for (void* p : gHeadLater) {  // (stage 41)
        re4_gx_head_back(p);
    }
    gHeadLater.clear();
    for (void* p : gDropped) {
        void* room = p;
        auto b = gBlocks.find(p);
        if (b != gBlocks.end()) {
            if (b->second.vram) {
                vramFree(b->second.vram);
                gVramBytes -= b->second.bytes;
            }
            room = b->second.room;  // (moved: none once copied)
            gBlockOf.erase(b->second.indices);
            gBlocks.erase(b);
        }
        if (room) {
            freeRoom(room);
        }
    }
    for (void* p : gRoomsCopied) {
        freeRoom(p);
    }
    gRoomsCopied.clear();
    gDropped.clear();
    for (void* p : gTexLinearLater) {
        linearFree(p);
    }
    gTexLinearLater.clear();
    static u32 movieOver;  // (the pipeline: the GPU is past the frame that drew it a picture later)
    movieOver = gMovieOn && !re4_movie_3ds_mark ? movieOver + 1 : 0;
    if (movieOver > (gPipe ? 1u : 0u)) {  // (stage 6.1: the movie over, the GPU past the frame that drew it)
        for (Texture*& m : gMaps) {
            if (m == &gMovie) {
                m = nullptr;
            }
        }
        forgetBound(&gMovie);
        C3D_TexDelete(&gMovie.tex);
        gMovieOn = false;
        re4_log("3ds: movie texture gone at picture %u, %u frames transferred", gFrameNo, gMovieTransfers);
    }
    if (gTexStep < gTexSteps && gFrameNo + 1 >= gTexStepFrom[gTexStep]) {
        gTexMask = gTexStepMask[gTexStep];
        const char* s = "step %c from picture %u: native formats%s%s%s%s%s\n";
        const u32 m = gTexMask;
        printf(s, 'A' + gTexStep, gTexStepFrom[gTexStep], m & 1 ? " LA4" : "", m & 2 ? " LA8" : "", m & 4 ? " ETC1" : "",
               m & 8 ? " ETC1A4" : "", m ? "" : " none");
        re4_log("3ds: texture step %c from picture %u: mask %u", 'A' + gTexStep, gTexStepFrom[gTexStep], m);
        gTexStep++;
        while (dropOldest()) {
        }
    }
    if (!gTexNative && gTexNativeFrom && gFrameNo + 1 >= gTexNativeFrom) {
        gTexNative = true;
        while (dropOldest()) {
        }
    }
    gStats.gpuMs += C3D_GetDrawingTime();
}

void beginFrame()
{
    frameBegin();
    occBuild();  // (occlusion cull)
    placePicture();
    gUnifBound[0] = gUnifBound[1] = false;  // (the program's bind below)
    C3D_RenderTargetClear(gTarget, C3D_CLEAR_ALL, 0x000000FF, gFogMode == 2 ? 0xFFFFFF : 0);
    C3D_FrameDrawOn(gTarget);
    if (gGpuTest == 1) {
        C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, 1, 1);
    }
    C3D_BindProgram(&gProgram);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocConst, 1.0f, 0.0f, 1.0e-12f, 0.0f);  // the shader's constants (k)
    gAttrKey = 0xFFFFFFFF;
    if (!gBufCtx) {
        gBufCtx = C3D_GetBufInfo();
    }
    gBufKnown = false;
    memset(gFixedKnown, 0, sizeof(gFixedKnown));
    memset(gBound, 0, sizeof(gBound));
    gColorKnown = false;
    memset(gSlotLight, 0xFF, sizeof(gSlotLight));
    memset(gSwitch, 0xFF, sizeof(gSwitch));
    gLoopFar = gLoopBrief = gLoopBriefFrom = gLoopWhole = gLoopWholeFrom = -1;
    gDmas = 0;
    gTexDmas = 0;
    gTevDirty = true;
    gTevGivenMask = 0;  // (X, finding 11: a picture's first combiners go whole, as before)
    gZMode = gCMode0 = 0xFFFFFFFF;
    gProjDirty = true;
    gRowsMin = 0;
    gRowsMax = kRows - 1;
    gStageUsed = 0;
    gIndicesUsed = 0;
    gScissorKey[0] = ~0u;
    gInFrame = true;
}

// The end of the GPU's part of the frame so far: ended without showing it (no output), the next
// part draws on into the same target once the GPU is done with this one.
void split()
{
    flushBatch();
    if (gPipe) {
        pipeSplit();
        return;
    }
    C3D_RenderTargetSetOutput(nullptr, GFX_TOP, GFX_LEFT, 0);
    gStats.cmdWords += gpuCmdBufOffset;
    frameEnd();
    frameBegin();
    C3D_FrameDrawOn(gTarget);
    if (gGpuTest == 1) {
        C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, 1, 1);
    }
    gStageUsed = 0;
    gIndicesUsed = 0;
    gScissorKey[0] = ~0u;
    gStats.splits++;
}

// --- TEV -> PICA texture combiners ------------------------------------------------------------
//
// A GX stage: out = (d +- ((1 - c) a + c b) + bias) * scale into PREV or a register. A PICA stage:
// one function of up to three sources (each a colour or alpha of: the vertex colour, a texture
// unit, the stage's constant, the stage before, the combiner buffer). The common shapes take one
// PICA stage (c = 0 or 1: one input; a = 0: b * c; b = 0: a * (1 - c); with d: add, multiply-add,
// subtract), the general lerp with d two. Bias, the 0.5 scale and the compare modes are left out
// (counted). A GX register written and read later comes from the combiner buffer, or, read with
// PREV by the next stage only, from PREVIOUS with PREV in the buffer (swapped); the constants
// (C0..C2 read as inputs, konstants, 1/2, 1) share the PICA stage's one constant colour.

struct In {
    GPU_TEVSRC src;
    int op;       // GPU_TEVOP_RGB_* / GPU_TEVOP_A_*
    bool zero, one;
    bool constant;
    float value[3];  // a constant's RGB (colour) or alpha in [0] (alpha)
};

struct PicaOp {
    GPU_COMBINEFUNC func;
    In in[3];
};

// The PICA stages being built for a draw
struct Combiner {
    GPU_TEVSRC src[3];
    int op[3];
    GPU_COMBINEFUNC func;
    int scale;
};
struct Stage {
    Combiner rgb, alpha;
    bool rgbConst, alphaConst;
    float constant[4];
    bool bufferOut;  // the buffer takes this stage's output
};
Stage gStages[6];
int gStageCount;

const float kFraction[8] = {1.0f, 0.875f, 0.75f, 0.625f, 0.5f, 0.375f, 0.25f, 0.125f};

__attribute__((noinline)) void clearCopy(const uint32_t* bp);

void konstColor(u32 sel, float out[3])
{
    if (sel < 8) {
        out[0] = out[1] = out[2] = kFraction[sel];
    } else if (sel >= 12 && sel < 16) {
        memcpy(out, gKonst[sel - 12], 3 * sizeof(float));
    } else if (sel >= 16) {
        const float v = gKonst[sel & 3][(sel - 16) >> 2];  // 16..19 R, 20..23 G, 24..27 B, 28..31 A
        out[0] = out[1] = out[2] = v;
    } else {
        out[0] = out[1] = out[2] = 1.0f;
    }
}

float konstAlpha(u32 sel)
{
    if (sel < 8) {
        return kFraction[sel];
    }
    return sel >= 16 ? gKonst[sel & 3][(sel - 16) >> 2] : 1.0f;
}

In zeroIn()
{
    In in = {GPU_PREVIOUS, 0, true, false, false, {0, 0, 0}};
    return in;
}

In constIn(float r, float g, float b, bool alpha)
{
    In in = {GPU_CONSTANT, alpha ? GPU_TEVOP_A_SRC_ALPHA : GPU_TEVOP_RGB_SRC_COLOR, false, false, true, {r, g, b}};
    in.zero = r == 0 && g == 0 && b == 0;
    in.one = r == 1 && g == 1 && b == 1;
    return in;
}

// The context of one GX stage's inputs
struct StageInputs {
    int unit;        // its texture's unit, -1: none
    const float* avg;  // (stage 6.9) with no unit: the map's average colour (nullptr: 1)
    float fill;      // its map an intensity EFB copy (stage 6.8): this value, no unit (-1: not one)
    bool rasZero;    // its colour channel is "zero"
    u32 kc, ka;      // konstant selections
    int bufferReg;   // the GX register the combiner buffer holds (-1: none)
    int regInPrev;   // swapped: the GX register PREVIOUS holds (its colour), PREV's colour in the buffer
    int alphaRegInPrev;  // (2026-11-19) swapped with its alpha too: PREVIOUS's alpha that register's, APREV the buffer's
    bool first;      // the first GX stage: PREV is the register's value
};

In colorIn(u32 i, const StageInputs& c)
{
    const int rgb = GPU_TEVOP_RGB_SRC_COLOR, a = GPU_TEVOP_RGB_SRC_ALPHA;
    switch (i) {
    case 0:
    case 1:
        if (c.first) {
            return i == 0 ? constIn(gTevReg[0][0], gTevReg[0][1], gTevReg[0][2], false)
                          : constIn(gTevReg[0][3], gTevReg[0][3], gTevReg[0][3], false);
        }
        if (c.regInPrev > 0 && (i == 0 || c.alphaRegInPrev > 0)) {
            return In{GPU_PREVIOUS_BUFFER, i == 0 ? rgb : a, false, false, false, {0, 0, 0}};
        }
        return In{GPU_PREVIOUS, i == 0 ? rgb : a, false, false, false, {0, 0, 0}};
    case 2: case 3: case 4: case 5: case 6: case 7: {
        const int r = (int) (i - 2) / 2 + 1;
        const bool alpha = i & 1;
        if ((c.regInPrev == r && !alpha) || (c.alphaRegInPrev == r && alpha)) {
            return In{GPU_PREVIOUS, alpha ? a : rgb, false, false, false, {0, 0, 0}};
        }
        if (c.bufferReg == r) {
            return In{GPU_PREVIOUS_BUFFER, alpha ? a : rgb, false, false, false, {0, 0, 0}};
        }
        const float* v = gTevReg[r];
        return alpha ? constIn(v[3], v[3], v[3], false) : constIn(v[0], v[1], v[2], false);
    }
    case 8:
    case 9:
        if (c.fill >= 0) {
            return constIn(c.fill, c.fill, c.fill, false);  // (stage 6.8)
        }
        if (c.unit < 0) {
            if (c.avg) {  // (stage 6.9)
                return i == 8 ? constIn(c.avg[0], c.avg[1], c.avg[2], false) : constIn(c.avg[3], c.avg[3], c.avg[3], false);
            }
            return constIn(1, 1, 1, false);
        }
        return In{(GPU_TEVSRC) (GPU_TEXTURE0 + c.unit), i == 8 ? rgb : a, false, false, false, {0, 0, 0}};
    case 10:
    case 11:
        if (c.rasZero) {
            return zeroIn();
        }
        return In{GPU_PRIMARY_COLOR, i == 10 ? rgb : a, false, false, false, {0, 0, 0}};
    case 12:
        return constIn(1, 1, 1, false);
    case 13:
        return constIn(0.5f, 0.5f, 0.5f, false);
    case 14: {
        float v[3];
        konstColor(c.kc, v);
        return constIn(v[0], v[1], v[2], false);
    }
    default:
        return zeroIn();
    }
}

In alphaIn(u32 i, const StageInputs& c)
{
    const int a = GPU_TEVOP_A_SRC_ALPHA;
    switch (i) {
    case 0:
        if (c.first) {
            return constIn(gTevReg[0][3], 0, 0, true);
        }
        if (c.alphaRegInPrev > 0) {
            return In{GPU_PREVIOUS_BUFFER, a, false, false, false, {0, 0, 0}};  // (2026-11-19: swapped with the alpha)
        }
        return In{GPU_PREVIOUS, a, false, false, false, {0, 0, 0}};
    case 1:
    case 2:
    case 3:
        if (c.alphaRegInPrev == (int) i) {
            return In{GPU_PREVIOUS, a, false, false, false, {0, 0, 0}};
        }
        if (c.bufferReg == (int) i) {
            return In{GPU_PREVIOUS_BUFFER, a, false, false, false, {0, 0, 0}};
        }
        return constIn(gTevReg[i][3], 0, 0, true);
    case 4:
        if (c.fill >= 0) {
            return constIn(c.fill, 0, 0, true);  // (stage 6.8)
        }
        if (c.unit < 0) {
            return constIn(c.avg ? c.avg[3] : 1, 0, 0, true);  // (stage 6.9)
        }
        return In{(GPU_TEVSRC) (GPU_TEXTURE0 + c.unit), a, false, false, false, {0, 0, 0}};
    case 5:
        if (c.rasZero) {
            return zeroIn();
        }
        return In{GPU_PRIMARY_COLOR, a, false, false, false, {0, 0, 0}};
    case 6:
        return constIn(konstAlpha(c.ka), 0, 0, true);
    default:
        return zeroIn();
    }
}

// An input that is the stage before's output (GX PREV after the first stage): a second PICA
// operation reads the first one's output there instead
bool readsPrev(const In& in)
{
    return !in.constant && !in.zero && in.src == GPU_PREVIOUS;
}

// One channel of a GX stage as one or two PICA operations (the second reads the first as
// PREVIOUS, so it takes no input of the GX stage that is PREVIOUS; *approx where it must)
int lower(In a, In b, In c, In d, bool sub, bool alpha, PicaOp out[2], bool* approx, u32 bias = 0, bool* biasDone = 0)
{
    if (alpha) {  // an alpha constant's value is in [0]
        a.one = a.constant && a.value[0] == 1;
        b.one = b.constant && b.value[0] == 1;
        c.one = c.constant && c.value[0] == 1;
        a.zero = a.zero || (a.constant && a.value[0] == 0);
        b.zero = b.zero || (b.constant && b.value[0] == 0);
        c.zero = c.zero || (c.constant && c.value[0] == 0);
        d.zero = d.zero || (d.constant && d.value[0] == 0);
    }
    // x = lerp(a, b, c); ONE_MINUS_* is the operand + 1 (SRC_COLOR, SRC_ALPHA)
    In nc = c;
    nc.op += 1;
    PicaOp x = {GPU_REPLACE, {a, a, a}};
    if (c.zero || (a.zero && b.zero)) {
        x = PicaOp{GPU_REPLACE, {a, a, a}};
    } else if (c.one) {
        x = PicaOp{GPU_REPLACE, {b, b, b}};
    } else if (a.zero) {
        x = PicaOp{GPU_MODULATE, {b, c, c}};
    } else if (b.zero) {
        x = PicaOp{GPU_MODULATE, {a, nc, nc}};
    } else {
        x = PicaOp{GPU_INTERPOLATE, {b, a, c}};
    }
    // the bias (1: +0.5, 2: -0.5) where GPU_ADD_SIGNED (a + b - 0.5) takes it exactly (stage67_tevbias_edits.py)
    auto oneMinus = [](In in) { in.op += 1; return in; };
    const bool biasSub = bias == 1 && sub, biasAdd = bias == 2 && !sub;
    if (d.zero && biasAdd && x.func == GPU_REPLACE && !x.in[0].zero) {
        out[0] = PicaOp{GPU_ADD_SIGNED, {x.in[0], constIn(0, 0, 0, alpha), constIn(0, 0, 0, alpha)}};
        *biasDone = true;
        return 1;
    }
    if (d.zero) {
        if (sub) {
            out[0] = PicaOp{GPU_REPLACE, {zeroIn(), zeroIn(), zeroIn()}};
            out[0].in[0] = alpha ? constIn(0, 0, 0, true) : constIn(0, 0, 0, false);
            return 1;
        }
        out[0] = x;
        return 1;
    }
    const In prev = {GPU_PREVIOUS, alpha ? GPU_TEVOP_A_SRC_ALPHA : GPU_TEVOP_RGB_SRC_COLOR, false, false, false, {0, 0, 0}};
    if (x.func == GPU_REPLACE && !x.in[0].zero && (biasSub || biasAdd)) {
        out[0] = biasSub ? PicaOp{GPU_ADD_SIGNED, {d, oneMinus(x.in[0]), oneMinus(x.in[0])}}
                         : PicaOp{GPU_ADD_SIGNED, {x.in[0], d, d}};
        *biasDone = true;
        return 1;
    }
    if (x.func == GPU_REPLACE && x.in[0].zero) {
        out[0] = PicaOp{GPU_REPLACE, {d, d, d}};  // (5.5) d +- 0: d alone (ADD {0, d} put both into the one constant)
        return 1;
    }
    if (x.func == GPU_REPLACE) {
        out[0] = sub ? PicaOp{GPU_SUBTRACT, {d, x.in[0], x.in[0]}} : PicaOp{GPU_ADD, {x.in[0], d, d}};
        return 1;
    }
    if (x.func == GPU_MODULATE && !sub) {
        out[0] = PicaOp{GPU_MULTIPLY_ADD, {x.in[0], x.in[1], d}};
        return 1;
    }
    // two operations: x first, then d with it; or, where d is PREVIOUS, the lerp's halves each
    // with a multiply-add (a (1 - c) + d, then b c + that), which b and c must allow
    if (!readsPrev(d) && (biasSub || biasAdd)) {
        out[0] = x;
        out[1] = biasSub ? PicaOp{GPU_ADD_SIGNED, {d, oneMinus(prev), oneMinus(prev)}} : PicaOp{GPU_ADD_SIGNED, {prev, d, d}};
        *biasDone = true;
        return 2;
    }
    if (!readsPrev(d)) {
        out[0] = x;
        out[1] = sub ? PicaOp{GPU_SUBTRACT, {d, prev, prev}} : PicaOp{GPU_ADD, {prev, d, d}};
        return 2;
    }
    if (!sub && x.func == GPU_INTERPOLATE && !readsPrev(b) && !readsPrev(c)) {
        out[0] = PicaOp{GPU_MULTIPLY_ADD, {a, nc, d}};
        out[1] = PicaOp{GPU_MULTIPLY_ADD, {b, c, prev}};
        return 2;
    }
    *approx = true;  // d - x with d PREVIOUS: x alone
    out[0] = x;
    return 1;
}

// The inputs of a PICA operation into a combiner of `st`; false where a constant does not fit
bool place(const PicaOp& o, bool alpha, Stage& st)
{
    Combiner& cb = alpha ? st.alpha : st.rgb;
    bool& used = alpha ? st.alphaConst : st.rgbConst;
    bool fits = true;
    cb.func = o.func;
    for (int k = 0; k < 3; k++) {
        const In& in = o.in[k];
        In use = in;
        if (in.zero && !in.constant) {
            use = alpha ? constIn(0, 0, 0, true) : constIn(0, 0, 0, false);
        }
        if (use.constant) {
            if (alpha) {
                if (used && st.constant[3] != use.value[0]) {
                    fits = false;
                }
                st.constant[3] = use.value[0];
            } else {
                if (used && (st.constant[0] != use.value[0] || st.constant[1] != use.value[1] ||
                             st.constant[2] != use.value[2])) {
                    fits = false;
                }
                memcpy(st.constant, use.value, 3 * sizeof(float));
            }
            used = true;
        }
        cb.src[k] = use.src;
        cb.op[k] = use.op;
    }
    return fits;
}

void passThrough(Combiner& cb, bool alpha)
{
    cb.func = GPU_REPLACE;
    for (int k = 0; k < 3; k++) {
        cb.src[k] = GPU_PREVIOUS;
        cb.op[k] = alpha ? GPU_TEVOP_A_SRC_ALPHA : GPU_TEVOP_RGB_SRC_COLOR;
    }
    cb.scale = 0;
}

// GX stages s.. write colour register r (1..3), their alpha PREV, up to a stage that writes PREV (the run):
// swapped (colorIn) - r in PREVIOUS, PREV in the buffer - where a stage of the run reads PREV's colour, no stage
// after the run reads r, and the register the buffer held (bufferReg) is not read from s on
// (stage67_tevswap_edits.py; before: one stage only, which took the run's middle for PREV)
bool readsReg(const uint32_t* bp, u32 t, int r)
{
    const u32 ce = bp[0xC0 + 2 * t], ae = bp[0xC1 + 2 * t];
    for (int k = 0; k < 4; k++) {
        const u32 ci = (ce >> (4 * k)) & 15, ai = (ae >> (4 + 3 * k)) & 7;
        if (ci == 2u * r || ci == 2u * r + 1 || ai == (u32) r) {
            return true;
        }
    }
    return false;
}

bool swapFits(const uint32_t* bp, u32 s, u32 stages, u32 r, int bufferReg)
{
    if (s == 0) {
        return false;
    }
    static const bool alphaToo = !getenv("RE4_3DS_TEVSWAPA") || atoi(getenv("RE4_3DS_TEVSWAPA")) != 0;  // (2026-11-19)
    const u32 ad0 = (bp[0xC1 + 2 * s] >> 22) & 3;
    const u32 adRun = alphaToo && ad0 == r ? r : 0;
    u32 t = s;
    for (; t < stages; t++) {
        const u32 cd = (bp[0xC0 + 2 * t] >> 22) & 3, ad = (bp[0xC1 + 2 * t] >> 22) & 3;
        if (cd == 0) {
            break;
        }
        if (cd != r || ad != adRun) {
            return false;
        }
    }
    if (t >= stages) {
        return false;
    }
    bool prev = false;
    for (u32 u = s + 1; u <= t; u++) {
        const u32 ce = bp[0xC0 + 2 * u];
        for (int k = 0; k < 4; k++) {
            prev |= ((ce >> (4 * k)) & 15) == 0;
        }
    }
    if (!prev) {
        return false;
    }
    for (u32 u = t + 1; u < stages; u++) {
        if (readsReg(bp, u, (int) r)) {
            return false;
        }
    }
    if (bufferReg > 0) {
        for (u32 u = s; u < stages; u++) {
            if ((u == s || bufferReg != (int) r) && readsReg(bp, u, bufferReg)) {
                return false;
            }
        }
    }
    return true;
}

// (stage 6.8) The GX stages from 0 whose colour PREV is one value whatever the textures and the colour channel give,
// among them a one-colour copy (Leon's projected shadow: PREV = TEXA + (1 - 0) saturates to 1, squared twice still 1),
// writing PREV, the alpha after stage 0 passed on: as one PICA stage (that colour, stage 0's alpha). Their count
// (0: none folded); gFoldColor the colour. RE4_3DS_TEVFOLD=0: as before (the stages past the PICA's 6 left out).
bool gTevFoldOn = true;
u32 gTevFolded, gTevFoldStages;
float gFoldColor[3];

struct Span {
    float lo, hi;
};

Span spanCombine(Span a, Span b, Span c, Span d, u32 word)
{
    // d +- lerp(a, b; c) + bias, times the scale, clamped (GX: [0, 1], or the register's range)
    float lo = 1e9f, hi = -1e9f;
    for (int k = 0; k < 8; k++) {
        const float av = k & 1 ? a.hi : a.lo, bv = k & 2 ? b.hi : b.lo, cv = k & 4 ? c.hi : c.lo;
        const float v = av * (1.0f - cv) + bv * cv;
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
    }
    const bool sub = (word >> 18) & 1;
    Span r = sub ? Span{d.lo - hi, d.hi - lo} : Span{d.lo + lo, d.hi + hi};
    const u32 bias = (word >> 16) & 3, scale = (word >> 20) & 3;
    const float add = bias == 1 ? 0.5f : bias == 2 ? -0.5f : 0.0f;
    const float mul = scale == 1 ? 2.0f : scale == 2 ? 4.0f : scale == 3 ? 0.5f : 1.0f;
    r.lo = (r.lo + add) * mul;
    r.hi = (r.hi + add) * mul;
    const float top = (word >> 19) & 1 ? 1.0f : 1023.0f / 255.0f, bottom = (word >> 19) & 1 ? 0.0f : -4.0f;
    r.lo = r.lo < bottom ? bottom : r.lo > top ? top : r.lo;
    r.hi = r.hi < bottom ? bottom : r.hi > top ? top : r.hi;
    return r;
}

u32 tevFold(const uint32_t* bp, u32 stages)
{
    if (!gTevFoldOn || gCopyFill.empty()) {
        return 0;
    }
    Span prev[4];  // r g b a
    bool fill = false;
    u32 fold = 0;
    for (u32 s = 0; s < stages; s++) {
        const u32 word = bp[0x28 + s / 2] >> ((s & 1) ? 12 : 0);
        const u32 map = word & 7, enabled = (word >> 6) & 1, chan = (word >> 7) & 7;
        const u32 ce = bp[0xC0 + 2 * s], ae = bp[0xC1 + 2 * s];
        if (((ce >> 22) & 3) || ((ae >> 22) & 3) || ((ce >> 16) & 3) == 3 || ((ae >> 16) & 3) == 3) {
            break;  // a register written, or a compare
        }
        if (s > 0 && ((ae & 0xFFF0) != 0xFF80 || ((ae >> 16) & 0x37))) {
            break;  // the alpha after stage 0: APREV as it is
        }
        const float f = enabled ? copyFillValue(gMaps[map]) : -1.0f;
        const Span tex = f >= 0 ? Span{f, f} : enabled ? Span{0, 1} : Span{1, 1};
        const Span ras = chan == 7 ? Span{0, 0} : Span{0, 1};
        const u32 ksel = bp[0xF6 + s / 2];
        const u32 kc = (s & 1) ? (ksel >> 14) & 31 : (ksel >> 4) & 31;
        const u32 ka = (s & 1) ? (ksel >> 19) & 31 : (ksel >> 9) & 31;
        float kv[3];
        konstColor(kc, kv);
        auto colorSpan = [&](u32 i, int ch) -> Span {
            switch (i) {
            case 0: return s == 0 ? Span{gTevReg[0][ch], gTevReg[0][ch]} : prev[ch];
            case 1: return s == 0 ? Span{gTevReg[0][3], gTevReg[0][3]} : prev[3];
            case 2: case 3: case 4: case 5: case 6: case 7: {
                const float v = gTevReg[(i - 2) / 2 + 1][(i & 1) ? 3 : ch];
                return Span{v, v};
            }
            case 8: case 9: return tex;
            case 10: case 11: return ras;
            case 12: return Span{1, 1};
            case 13: return Span{0.5f, 0.5f};
            case 14: return Span{kv[ch], kv[ch]};
            default: return Span{0, 0};
            }
        };
        auto alphaSpan = [&](u32 i) -> Span {
            switch (i) {
            case 0: return s == 0 ? Span{gTevReg[0][3], gTevReg[0][3]} : prev[3];
            case 1: case 2: case 3: return Span{gTevReg[i][3], gTevReg[i][3]};
            case 4: return tex;
            case 5: return ras;
            case 6: { const float v = konstAlpha(ka); return Span{v, v}; }
            default: return Span{0, 0};
            }
        };
        Span next[4];
        for (int ch = 0; ch < 3; ch++) {
            next[ch] = spanCombine(colorSpan((ce >> 12) & 15, ch), colorSpan((ce >> 8) & 15, ch),
                                   colorSpan((ce >> 4) & 15, ch), colorSpan(ce & 15, ch), ce);
        }
        next[3] = spanCombine(alphaSpan((ae >> 13) & 7), alphaSpan((ae >> 10) & 7), alphaSpan((ae >> 7) & 7),
                              alphaSpan((ae >> 4) & 7), ae);
        memcpy(prev, next, sizeof(prev));
        fill |= f >= 0;
        bool one = true;
        for (int ch = 0; ch < 3; ch++) {
            one &= prev[ch].hi - prev[ch].lo < 0.5f / 255.0f;
        }
        if (one && fill) {
            fold = s + 1;
            for (int ch = 0; ch < 3; ch++) {
                gFoldColor[ch] = prev[ch].lo;
            }
        }
    }
    return fold > 1 ? fold : 0;
}

bool alphaReadsTex(u32 ae)
{
    for (int k = 0; k < 4; k++) {
        if (((ae >> (4 + 3 * k)) & 7) == 4) {
            return true;
        }
    }
    return false;
}

// Stage 6.9: the average colour of the texture map `map` holds (nullptr: not known): up to 16 rows of tiles of level 0,
// spread over it, once a texture
const float* mapAverage(u32 map)
{
    Texture* tex = gMaps[map];
    const MapSource& m = gMapSource[map];
    if (!tex || copyFillValue(tex) >= 0 || tex == &gWater || tex->turned) {
        return nullptr;
    }
    if (tex->avgKnown) {
        return tex->avg;
    }
    if (!m.data || !m.t.dataBytes) {
        return nullptr;
    }
    const u32 f = m.t.format, w = m.t.width, h = m.t.height;
    const u32 tileH = f == 0 || f == 8 || f == 14 ? 8 : 4;
    const size_t rowBytes = re4gx::GxTextureLevelBytes(f, w, tileH);
    const u32 rows = (h + tileH - 1) / tileH;
    const u32 take = rows < 16 ? rows : 16;
    double sum[4] = {0, 0, 0, 0};
    u32 n = 0;
    std::vector<std::uint8_t> rgba;
    for (u32 k = 0; k < take; k++) {
        const u32 r = rows * k / take;
        if ((r + 1) * rowBytes > m.t.dataBytes ||
            !re4gx::GxDecodeTexture(m.data + r * rowBytes, rowBytes, f, w, tileH, m.tlut, m.entries,
                                    (m.t.tlut >> 10) & 3, rgba)) {
            continue;
        }
        for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
            for (int c = 0; c < 4; c++) {
                sum[c] += rgba[i + c];
            }
            n++;
        }
    }
    if (!n) {
        return nullptr;
    }
    for (int c = 0; c < 4; c++) {
        tex->avg[c] = (float) (sum[c] / n / 255.0);
    }
    tex->avgKnown = true;
    return tex->avg;
}

// (2026-11-10: the TEV cache) The combiners built, kept by what they were built from: the stage count, the
// TEV's BP registers (tevBpGather), the colour registers and konstants. Not kept: what a texture shapes - an intensity
// copy's colour (gTevFill, stage 6.8's fold among it), a map's average with no unit (stage 6.9, gTevDropMask); found
// only while none of the maps its stages read is such a copy. RE4_3DS_TEVCACHE=0: built every time, as before.
void tevBpGather(const uint32_t* bp, u32* w);
bool gTevCacheOn = true;
u32 gTevCacheHits, gTevCacheKept;  // per log window: builds found in the cache; built and kept
constexpr int kTevKeyWords = 1 + 49 + 32;
constexpr u32 kTevCacheSlots = 256;
struct TevCached {
    u32 key[kTevKeyWords];
    u32 hash;
    bool used;
    u8 maps;  // the maps its enabled stages read
    u8 bufferMask;
    s8 units, unitMap[3], unitCoord[3];
    u16 approx, packed, skipped, scaled, scaleShort;  // the counters it added when built
    C3D_TexEnv env[6];
};
TevCached* gTevCache;
u32 gTevCacheNext, gTevCacheKey[kTevKeyWords], gTevCacheHash;
bool gTevCacheMiss;  // the build going on: keep it at its end (tevToCache)
u32 gTevCacheCount[5];  // the counters when it began

bool tevFromCache(const uint32_t* bp)
{
    gTevCacheMiss = false;
    if (!gTevCacheOn) {
        return false;
    }
    if (!gTevCache) {
        gTevCache = (TevCached*) calloc(kTevCacheSlots, sizeof(TevCached));
        if (!gTevCache) {
            gTevCacheOn = false;
            return false;
        }
    }
    u32* key = gTevCacheKey;
    key[0] = (bp[0x00] >> 10) & 15;
    tevBpGather(bp, key + 1);
    memcpy(key + 50, gTevReg, sizeof(gTevReg));
    memcpy(key + 66, gKonst, sizeof(gKonst));
    u32 h = 2166136261u;
    for (int i = 0; i < kTevKeyWords; i++) {
        h = (h ^ key[i]) * 16777619u;
    }
    gTevCacheHash = h;
    const u32 at = h ^ (h >> 15);
    for (u32 k = 0; k < 4; k++) {
        const TevCached& e = gTevCache[(at + k) % kTevCacheSlots];
        if (!e.used || e.hash != h || memcmp(e.key, key, sizeof(e.key))) {
            continue;
        }
        for (u32 m = 0; m < 8 && !gCopyFill.empty(); m++) {
            if ((e.maps >> m & 1) && copyFillValue(gMaps[m]) >= 0) {
                return false;  // (an intensity copy now: built, not kept)
            }
        }
        for (int i = 0; i < 6; i++) {
            if (!(gTevGivenMask >> i & 1) || memcmp(&gTevGiven[i], &e.env[i], sizeof(C3D_TexEnv))) {  // (X, finding 11)
                C3D_SetTexEnv(i, const_cast<C3D_TexEnv*>(&e.env[i]));
                gTevGiven[i] = e.env[i];
            }
        }
        gTevGivenMask = gTevGivenOn ? 0x3F : 0;
        C3D_TexEnvBufUpdate(C3D_Both, e.bufferMask);
        gUnits = e.units;
        for (int u = 0; u < 3; u++) {
            gUnitMap[u] = e.unitMap[u];
            gUnitCoord[u] = e.unitCoord[u];
        }
        gTevFill = false;
        gTevDropMask = 0;
        gTevApprox += e.approx;
        gTevPacked += e.packed;
        gTevSkipped += e.skipped;
        gTevScaled += e.scaled;
        gTevScaleShort += e.scaleShort;
        gTevCacheHits++;
        return true;
    }
    gTevCacheMiss = true;
    gTevCacheCount[0] = gTevApprox;
    gTevCacheCount[1] = gTevPacked;
    gTevCacheCount[2] = gTevSkipped;
    gTevCacheCount[3] = gTevScaled;
    gTevCacheCount[4] = gTevScaleShort;
    return false;
}

void tevToCache(const uint32_t* bp, u8 bufferMask)
{
    if (!gTevCacheMiss || gTevFill || gTevDropMask) {
        return;
    }
    gTevCacheMiss = false;
    const u32 at = gTevCacheHash ^ (gTevCacheHash >> 15);
    TevCached* e = nullptr;
    for (u32 k = 0; k < 4 && !e; k++) {
        TevCached& c = gTevCache[(at + k) % kTevCacheSlots];
        e = c.used ? nullptr : &c;
    }
    if (!e) {
        e = &gTevCache[(at + (gTevCacheNext++ & 3)) % kTevCacheSlots];
    }
    memcpy(e->key, gTevCacheKey, sizeof(e->key));
    e->hash = gTevCacheHash;
    e->used = true;
    const u32 stages = ((bp[0x00] >> 10) & 15) + 1;
    e->maps = 0;
    for (u32 s = 0; s < stages; s++) {
        const u32 word = bp[0x28 + s / 2] >> ((s & 1) ? 12 : 0);
        if ((word >> 6) & 1) {
            e->maps |= 1u << (word & 7);
        }
    }
    e->bufferMask = bufferMask;
    e->units = (s8) gUnits;
    for (int u = 0; u < 3; u++) {
        e->unitMap[u] = (s8) gUnitMap[u];
        e->unitCoord[u] = (s8) gUnitCoord[u];
    }
    e->approx = (u16) (gTevApprox - gTevCacheCount[0]);
    e->packed = (u16) (gTevPacked - gTevCacheCount[1]);
    e->skipped = (u16) (gTevSkipped - gTevCacheCount[2]);
    e->scaled = (u16) (gTevScaled - gTevCacheCount[3]);
    e->scaleShort = (u16) (gTevScaleShort - gTevCacheCount[4]);
    for (int i = 0; i < 6; i++) {
        e->env[i] = *C3D_GetTexEnv(i);
        gTevGiven[i] = e->env[i];  // (X, finding 11: citro3d's stages as built just now)
    }
    gTevGivenMask = gTevGivenOn ? 0x3F : 0;
    gTevCacheKept++;
}

// The texture maps the enabled stages read, on up to three units (gUnitMap, gUnitCoord), and the
// combiners for the TEV stages of the BP registers.
__attribute__((noinline)) void buildTevMiss(const uint32_t* bp);  // (X, finding 16: the builder out of drawTimed)
void buildTev(const uint32_t* bp)
{
    if (tevFromCache(bp)) {
        return;  // (the TEV cache)
    }
    buildTevMiss(bp);
}

void buildTevMiss(const uint32_t* bp)
{
    const u32 stages = ((bp[0x00] >> 10) & 15) + 1;
    gUnits = 0;
    gStageCount = 0;
    gTevFill = false;
    int bufferReg = -1, regInPrev = -1, alphaRegInPrev = -1;  // (alphaRegInPrev: 2026-11-19)
    bool prevOut = false;  // the last PICA stage's output is GX PREV
    const u32 fold = tevFold(bp, stages);  // (stage 6.8)
    gTevFolded += fold > 0;
    gTevFoldStages += fold;
    gTevFill |= fold > 0;  // (copies alone, all folded: still left out at the draw - no unit)
    // (stage 6.9) more stages than fit: the minor ones left out, PREV = CPREV + KONST x (or by) X with the alpha as it
    // was and KONST at most 1/8, then those writing a register only left-out ones read
    u32 skip = 0;
    if (gTevPack && stages > 6) {
        for (u32 s = 0; s < stages; s++) {
            const u32 ce = bp[0xC0 + 2 * s], ae = bp[0xC1 + 2 * s];
            const u32 b = (ce >> 8) & 15, c = (ce >> 4) & 15;
            if ((ce & 0xF7F00F) != 0x00F000 || (ae & 0xF3FFF0) != 0x00FF80 || !((b == 14) != (c == 14))) {
                continue;  // (d CPREV, a ZERO, no bias, add, scale 1, PREV; the alpha as it was; KONST once)
            }
            const u32 ksel = bp[0xF6 + s / 2];
            float k[3];
            konstColor((s & 1) ? (ksel >> 14) & 31 : (ksel >> 4) & 31, k);
            if (k[0] <= 0.125f && k[1] <= 0.125f && k[2] <= 0.125f) {
                skip |= 1u << s;
            }
        }
        for (u32 s = 0; skip && s < stages; s++) {
            const u32 r = (bp[0xC0 + 2 * s] >> 22) & 3;
            if (r == 0 || ((bp[0xC1 + 2 * s] >> 22) & 3) != 0) {
                continue;
            }
            bool read = false, readSkipped = false;
            for (u32 t = s + 1; t < stages; t++) {
                const u32 ce = bp[0xC0 + 2 * t], ae = bp[0xC1 + 2 * t];
                bool reads = false;
                for (int q = 0; q < 4; q++) {
                    const u32 ci = (ce >> (4 * q)) & 15, ai = (ae >> (4 + 3 * q)) & 7;
                    reads |= ci == 2 * r || ci == 2 * r + 1 || ai == r;
                }
                (skip >> t & 1 ? readSkipped : read) |= reads;
                if (((ce >> 22) & 3) == r) {
                    break;  // (written again)
                }
            }
            if (readSkipped && !read) {
                skip |= 1u << s;
            }
        }
    }
    // (stage 6.9) more than three maps wanted (an EFB copy's fill wants none): first those whose alpha the alpha
    // combiners read (a mask: what is drawn), then the rest as the stages come; a map left without a unit reads as its
    // average colour
    u32 wanted = 0;
    for (u32 s = 0; s < stages; s++) {
        const u32 word = bp[0x28 + s / 2] >> ((s & 1) ? 12 : 0);
        if (((word >> 6) & 1) && !(skip >> s & 1) && !(s > 0 && s < fold) && copyFillValue(gMaps[word & 7]) < 0) {
            wanted |= 1u << (word & 7);
        }
    }
    gTevDropMask = 0;
    if (gTevPack && __builtin_popcount(wanted) > 3 && gGpuTest != 2) {
        for (u32 s = 0; s < stages && gUnits < 3; s++) {
            const u32 word = bp[0x28 + s / 2] >> ((s & 1) ? 12 : 0);
            bool known = false;
            for (int u = 0; u < gUnits; u++) {
                known |= gUnitMap[u] == (int) (word & 7);
            }
            if ((wanted >> (word & 7) & 1) && alphaReadsTex(bp[0xC1 + 2 * s]) && !known && !(skip >> s & 1)) {
                gUnitMap[gUnits] = (int) (word & 7);
                gUnitCoord[gUnits] = (int) ((word >> 3) & 7);
                gUnits++;
            }
        }
    }
    // (a PICA combiner that passes its channel on as it was)
    auto passes = [](const Combiner& cb, bool alpha) {
        return cb.func == GPU_REPLACE && cb.src[0] == GPU_PREVIOUS && cb.scale == 0 &&
               cb.op[0] == (alpha ? (int) GPU_TEVOP_A_SRC_ALPHA : (int) GPU_TEVOP_RGB_SRC_COLOR);
    };
    u32 lastDest = 1;  // the GX stage before's destinations (0: both PREV)
    bool averaged = false;
    for (u32 s = 0; s < stages; s++) {
        if (s > 0 && s < fold) {
            continue;  // (stage 6.8: folded into the first)
        }
        if (skip >> s & 1) {
            gTevSkipped++;  // (stage 6.9)
            continue;
        }
        const u32 word = bp[0x28 + s / 2] >> ((s & 1) ? 12 : 0);
        const u32 map = word & 7, coord = (word >> 3) & 7, enabled = (word >> 6) & 1, chan = (word >> 7) & 7;
        StageInputs c;
        c.unit = -1;
        c.avg = nullptr;
        c.fill = enabled ? copyFillValue(gMaps[map]) : -1.0f;
        gTevFill |= c.fill >= 0;
        if (enabled && gGpuTest != 2 && c.fill < 0) {
            for (int u = 0; u < gUnits; u++) {
                if (gUnitMap[u] == (int) map) {
                    c.unit = u;
                }
            }
            if (c.unit < 0 && gUnits < 3) {
                c.unit = gUnits;
                gUnitMap[gUnits] = (int) map;
                gUnitCoord[gUnits] = (int) coord;
                gUnits++;
            }
            if (c.unit < 0 && gTevPack) {  // (stage 6.9)
                c.avg = mapAverage(map);
                gTevDropMask |= 1u << map;
                averaged = true;
            }
        }
        if (fold && s == 0 && c.unit == 0 && gUnits == 1 && !alphaReadsTex(bp[0xC1])) {
            gUnits = 0;  // (stage 6.8: its colour folded, its texture not read)
            c.unit = -1;
        }
        c.rasZero = chan == 7;
        const u32 ksel = bp[0xF6 + s / 2];
        c.kc = (s & 1) ? (ksel >> 14) & 31 : (ksel >> 4) & 31;
        c.ka = (s & 1) ? (ksel >> 19) & 31 : (ksel >> 9) & 31;
        c.bufferReg = bufferReg;
        c.regInPrev = regInPrev;
        c.alphaRegInPrev = alphaRegInPrev;
        c.first = s == 0;
        const u32 ce = bp[0xC0 + 2 * s], ae = bp[0xC1 + 2 * s];
        PicaOp rgb[2], alpha[2];
        bool approx = false, rgbBias = false, alphaBias = false;
        int nr = lower(colorIn((ce >> 12) & 15, c), colorIn((ce >> 8) & 15, c), colorIn((ce >> 4) & 15, c),
                             colorIn(ce & 15, c), (ce >> 18) & 1, false, rgb, &approx, (ce >> 16) & 3, &rgbBias);
        const int na = lower(alphaIn((ae >> 13) & 7, c), alphaIn((ae >> 10) & 7, c), alphaIn((ae >> 7) & 7, c),
                             alphaIn((ae >> 4) & 7, c), (ae >> 18) & 1, true, alpha, &approx, (ae >> 16) & 3, &alphaBias);
        if (fold && s == 0) {  // (stage 6.8)
            const In v = constIn(gFoldColor[0], gFoldColor[1], gFoldColor[2], false);
            rgb[0] = PicaOp{GPU_REPLACE, {v, v, v}};
            nr = 1;
            rgbBias = true;
            approx = false;
        }
        gTevApprox += approx;
        // (stage 6.9) one channel PREV as it was ("+ 0"): the other into the PICA stage before where that one passes
        // its channel on (the PICA combines colour and alpha apart); inputs PREV, textures, the rasterised colour,
        // konstants (no register: the buffer), the GX stage before writing PREV
        const bool rgbSame = gTevPack && (ce & 0xF3FFFF) == 0x00FFF0, alphaSame = gTevPack && (ae & 0xF3FFF0) == 0x00FF80;
        const u32 dests = ((ce >> 22) & 3) | ((ae >> 22) & 3);
        auto plainInputs = [](u32 e, bool alpha) {
            for (int k = 0; k < 4; k++) {
                const u32 i = alpha ? (e >> (4 + 3 * k)) & 7 : (e >> (4 * k)) & 15;
                if (alpha ? (i >= 1 && i <= 3) : (i >= 1 && i <= 7)) {
                    return false;
                }
            }
            return true;
        };
        if (gStageCount > 0 && lastDest == 0 && dests == 0 && rgbSame != alphaSame && !(fold && s == 0)) {
            Stage& last = gStages[gStageCount - 1];
            const u32 cs = (ce >> 20) & 3, as = (ae >> 20) & 3;
            Stage trial = last;
            if (rgbSame && passes(last.alpha, true) && na == 1 && as < 3 && plainInputs(ae, true) && place(alpha[0], true, trial)) {
                trial.alpha.scale = (int) as;
                last = trial;
                gTevPacked++;
                continue;
            }
            if (alphaSame && passes(last.rgb, false) && nr == 1 && cs < 3 && plainInputs(ce, false) && place(rgb[0], false, trial)) {
                trial.rgb.scale = (int) cs;
                last = trial;
                gTevPacked++;
                continue;
            }
        }
        lastDest = dests;
        const int n = nr > na ? nr : na;
        if (gStageCount + n > 6) {
            // (stage 6.8) the GX stages left only scale PREV (PREV = CPREV x2 / x4, the alpha as it is): into the
            // scales of the PICA's last stages, back over those that multiply PREV (at most x4 each)
            bool onlyScale = gTevScaleOn && prevOut && regInPrev < 0 && bufferReg < 0;
            int need = 0;
            for (u32 t = s; t < stages && onlyScale; t++) {
                const u32 tc = bp[0xC0 + 2 * t], ta = bp[0xC1 + 2 * t];
                const u32 sc = (tc >> 20) & 3;
                onlyScale = (tc & 0xFFFF) == 0xFFF0 && ((tc >> 16) & 7) == 0 && sc < 3 && ((tc >> 22) & 3) == 0 &&
                            (ta & 0xFFF0) == 0xFF80 && ((ta >> 16) & 0x37) == 0 && ((ta >> 22) & 3) == 0;
                need += (int) sc;
            }
            if (onlyScale && need > 0) {
                for (int i = gStageCount - 1; i >= 0 && need > 0; i--) {
                    Stage& st = gStages[i];
                    const int add = need < 2 - st.rgb.scale ? need : 2 - st.rgb.scale;
                    st.rgb.scale += add > 0 ? add : 0;
                    need -= add > 0 ? add : 0;
                    if (st.rgb.func != GPU_MODULATE || (i > 0 && gStages[i - 1].bufferOut)) {
                        break;  // (further back only over a stage that multiplies PREV)
                    }
                }
                gTevScaled++;
                gTevScaleShort += need > 0;
                break;
            }
            gTevApprox++;
            break;
        }
        if ((((ce >> 16) & 3) && !rgbBias) || (((ae >> 16) & 3) && !alphaBias) || ((ce >> 20) & 3) == 3 ||
            ((ae >> 20) & 3) == 3) {
            gTevApprox++;  // bias, compare modes, scale 1/2
        }
        for (int k = 0; k < n; k++) {
            Stage& st = gStages[gStageCount + k];
            memset(&st, 0, sizeof(st));
            bool fits = true;
            if (k < nr && !(rgbSame && !(fold && s == 0))) {  // (stage 6.9: "+ 0" as a pass, no constant)
                fits &= place(rgb[k], false, st);
            } else {
                passThrough(st.rgb, false);
            }
            if (k < na && !alphaSame) {
                fits &= place(alpha[k], true, st);
            } else {
                passThrough(st.alpha, true);
            }
            if (!fits) {
                gTevApprox++;
            }
            const u32 cs = (ce >> 20) & 3, as = (ae >> 20) & 3;
            st.rgb.scale = k == nr - 1 && cs < 3 ? (int) cs : 0;
            st.alpha.scale = k == na - 1 && as < 3 ? (int) as : 0;
            if (fold && s == 0) {
                st.rgb.scale = 0;  // (stage 6.8: in the folded colour)
            }
        }
        gStageCount += n;
        // a register written: the buffer keeps it for the stages after (PREV goes on as the PICA's);
        // or swapped: the buffer keeps PREV (the stage before's output), the register is PREVIOUS
        const u32 dest = (ce >> 22) & 3;
        if (dest != 0) {
            const int before = gStageCount - n - 1;
            if (regInPrev == (int) dest) {
                // the run goes on: r again in PREVIOUS, PREV still in the buffer
            } else if (prevOut && before >= 0 && before < 4 && swapFits(bp, s, stages, dest, bufferReg)) {
                gStages[before].bufferOut = true;
                regInPrev = (int) dest;
                alphaRegInPrev = ((ae >> 22) & 3) == dest ? (int) dest : -1;  // (2026-11-19)
                bufferReg = -1;
            } else {
                gStages[gStageCount - 1].bufferOut = true;
                bufferReg = (int) dest;
                regInPrev = -1;
                alphaRegInPrev = -1;
            }
            prevOut = false;
        } else {
            regInPrev = -1;
            alphaRegInPrev = -1;
            prevOut = true;
        }
    }
    gTevAveraged += averaged;  // (stage 6.9)
    // onto the GPU
    u8 bufferMask = 0;
    gTevGivenMask = 0;  // (X, finding 11: written here)
    for (int i = 0; i < 6; i++) {
        C3D_TexEnv* env = C3D_GetTexEnv(i);
        C3D_TexEnvInit(env);
        if (i >= gStageCount) {
            continue;
        }
        const Stage& st = gStages[i];
        C3D_TexEnvSrc(env, C3D_RGB, st.rgb.src[0], st.rgb.src[1], st.rgb.src[2]);
        C3D_TexEnvSrc(env, C3D_Alpha, st.alpha.src[0], st.alpha.src[1], st.alpha.src[2]);
        C3D_TexEnvOpRgb(env, (GPU_TEVOP_RGB) st.rgb.op[0], (GPU_TEVOP_RGB) st.rgb.op[1], (GPU_TEVOP_RGB) st.rgb.op[2]);
        C3D_TexEnvOpAlpha(env, (GPU_TEVOP_A) st.alpha.op[0], (GPU_TEVOP_A) st.alpha.op[1], (GPU_TEVOP_A) st.alpha.op[2]);
        C3D_TexEnvFunc(env, C3D_RGB, st.rgb.func);
        C3D_TexEnvFunc(env, C3D_Alpha, st.alpha.func);
        C3D_TexEnvScale(env, C3D_RGB, (GPU_TEVSCALE) st.rgb.scale);
        C3D_TexEnvScale(env, C3D_Alpha, (GPU_TEVSCALE) st.alpha.scale);
        auto byte = [](float v) { return (u32) (v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f); };
        C3D_TexEnvColor(env, byte(st.constant[0]) | byte(st.constant[1]) << 8 | byte(st.constant[2]) << 16 |
                                 byte(st.constant[3]) << 24);
        if (st.bufferOut && i < 4) {
            bufferMask |= 1 << i;
        }
    }
    C3D_TexEnvBufUpdate(C3D_Both, bufferMask);
    tevToCache(bp, bufferMask);  // (the TEV cache)
}

// The alpha test (BP 0xF3: two compares and their logic; the PICA has one)
void setAlphaTest(const uint32_t* bp)
{
    static const GPU_TESTFUNC kFunc[8] = {GPU_NEVER, GPU_LESS, GPU_EQUAL, GPU_LEQUAL,
                                          GPU_GREATER, GPU_NOTEQUAL, GPU_GEQUAL, GPU_ALWAYS};
    const u32 v = bp[0xF3];
    const u32 c0 = (v >> 16) & 7, c1 = (v >> 19) & 7, logic = (v >> 22) & 3;
    const u32 r0 = v & 0xFF, r1 = (v >> 8) & 0xFF;
    // AND with one ALWAYS, OR with one NEVER: the other compare alone
    bool on = true;
    GPU_TESTFUNC f = kFunc[c0];
    u32 ref = r0;
    if (logic == 0 && c0 == 7) {
        f = kFunc[c1];
        ref = r1;
    } else if (logic == 1 && c0 == 0) {
        f = kFunc[c1];
        ref = r1;
    }
    if (f == GPU_ALWAYS) {
        on = false;
    }
    C3D_AlphaTest(on, f, (int) ref);
}

// The water's copy: an EFB copy (BP 0x52, bit 14 clear) to the address game/trans.cpp GetEfbTex named. The draws before
// kicked, then the colour buffer (240 x 400, its 50 rows of 30 tiles) into gWater (32 tiles a row; the rest 0) by
// GX_TextureCopy in the GX queue, before the draws after it (stage54f_freeze_edits.py, the layout of the 5.4 tries:
// x = 240 - screen y, y = 400 - screen x, rows in memory from the top). The copy's rectangle (0x49/0x4A, EFB pixels)
// through the picture's place (gMap) into the buffer: the turn of the GX coordinates for the draws that read it.
__attribute__((noinline)) void waterCopy(const uint32_t* bp)
{
    const u32 dest = (bp[0x4B] & 0x1FFFFF) << 5;
    const bool armed = dest && dest == gWaterArmed;
    const bool refract = armed && gWaterArmedRefract;  // (2026-11-18)
    gWaterArmed = 0;
    gWaterArmedRefract = false;
    gRefractPending = 0;
    gRefractPendingMap = -1;
    if (!armed) {
        if (dest && dest == gWaterDest) {
            gWaterLive = false;  // another copy to the same buffer (the door, a filter): not the water's, as before
        }
        return;
    }
    if (!gWaterOn || !gInFrame) {
        return;
    }
    if (refract) {
        if (gWaterLive && gWaterFrame == gFrameNo && dest == gWaterDest && (bp[0x49] & 0xFFFFF) == gWaterRectBp[0] &&
            (bp[0x4A] & 0xFFFFF) == gWaterRectBp[1]) {
            gWaterRefract = true;  // this picture's copy again (the draws since: the water's own, other pieces of it)
            gRefractShared++;
            return;
        }
        if (!gRefractWanted || gRefractWanted + 2 < gFrameNo) {
            if (dest == gWaterDest) {
                gWaterLive = false;  // (as any other copy to the buffer)
            }
            gRefractPending = dest;  // none of its draws drawn lately: no copy (volumes only)
            gRefractNotMade++;
            return;
        }
    }
    gWaterRefract = refract;
    const gxCmdQueue_s& q = *gQueue;
    if (q.numEntries + kQueueForKick + 2 > q.maxEntries) {
        gWaterNoQueue++;
        return;
    }
    if (!gWater.tex.data) {
        void* texels = linearMemAlign(256 * 512 * 4, 0x80);
        while (!texels && dropOldest(kTexAge, true)) {  // (R4: room as for the textures - old rooms' ones go)
            texels = linearMemAlign(256 * 512 * 4, 0x80);
        }
        if (!texels) {
            gWaterNoQueue++;
            return;
        }
        memset(texels, 0, 256 * 512 * 4);
        flushData(texels, 256 * 512 * 4);
        gWater = Texture();
        C3D_Tex& t = gWater.tex;
        t.data = texels;
        t.fmt = GPU_RGBA8;
        t.size = 256 * 512 * 4;
        t.width = 256;
        t.height = 512;
        t.param = GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_LINEAR) |
                  GPU_TEXTURE_WRAP_S(GPU_CLAMP_TO_EDGE) | GPU_TEXTURE_WRAP_T(GPU_CLAMP_TO_EDGE) |
                  GPU_TEXTURE_MODE(GPU_TEX_PROJECTION);
        gWater.linear = texels;
        gWater.bytes = t.size;
        gWater.mipNear = gMipNearFrom != 0;  // (no mipmaps, no LOD bias: those switches left alone)
        gWater.biased = true;
        re4_log("3ds: the water's copy of the picture from picture %u (0x%08x)", gFrameNo, (unsigned) dest);
    }
    kick();
    GX_TextureCopy((u32*) gTarget->frameBuf.colorBuf, GX_BUFFER_DIM(30 * 256 >> 4, 0), (u32*) gWater.tex.data,
                   GX_BUFFER_DIM(30 * 256 >> 4, 2 * 256 >> 4), 240 * 400 * 4, GX_TRANSFER_RAW_COPY(1));
    forgetBound(&gWater);  // (bound again: the GPU's texture cache cleared)
    // the copy's rectangle (EFB pixels); the turn of the coordinates at the texture's bind: its size, not this one,
    // spans GX's 0..1 (GetEfbTex / Espgen42: 392 rows copied at half size into a texture of 168 rows - 336 of them)
    gWaterRect[0] = (float) (bp[0x49] & 0x3FF);
    gWaterRect[1] = (float) ((bp[0x49] >> 10) & 0x3FF);
    gWaterRect[2] = (float) ((bp[0x4A] & 0x3FF) + 1);
    gWaterRect[3] = (float) (((bp[0x4A] >> 10) & 0x3FF) + 1);
    gWaterDest = dest;
    gWaterFrame = gFrameNo;
    gWaterLive = true;
    gWaterCopies++;
    gWaterRectBp[0] = bp[0x49] & 0xFFFFF;  // (2026-11-18)
    gWaterRectBp[1] = bp[0x4A] & 0xFFFFF;
    gRefractCopies += refract;
}

// (occlusion cull) The depth buffer into gOccBuf (the picture's parity), as the water's copy: the draws before
// kicked, GX_TextureCopy in the GX queue
void occCopy()
{
    gOccCopied = true;
    if (re4_occ_sparse && (gFrameNo & 7)) {  // (little hidden for a while: a copy every 8th picture, re4_occ_frame)
        return;
    }
    const int b = gFrameNo & 1;
    const gxCmdQueue_s& q = *gQueue;
    if (!gOccBuf[b] || q.numEntries + kQueueForKick + 2 > q.maxEntries) {
        gOccNoQueue++;
        return;
    }
    flushBatch();
    kick();
    GX_TextureCopy((u32*) gTarget->frameBuf.depthBuf, GX_BUFFER_DIM(30 * 256 >> 4, 0), (u32*) gOccBuf[b],
                   GX_BUFFER_DIM(30 * 256 >> 4, 0), 240 * 400 * 4, GX_TRANSFER_RAW_COPY(1));
    gOccBufFrame[b] = gFrameNo;
    gOccBufTag[b] = gOccPicTag;
    memcpy(gOccBufM[b], gOccM, sizeof(gOccM));
    gOccBufScale[b] = gOccScale;
    gOccCopies++;
}

// (occlusion cull) The game's frame of the pictures from here (re4_occ_frame, a record in the ring)
void occTagFn(uint32_t tag, uint32_t)
{
    gOccPicTag = tag;
}

// (occlusion cull) At a picture's begin: the copies whose queue is done (two pictures on) into tiles, the oldest
// first, to the game's thread
void occBuild()
{
    if (!gOccOn) {
        return;
    }
    for (int pass = 0; pass < 2; pass++) {
        int b = gOccBufFrame[0] <= gOccBufFrame[1] ? 0 : 1;
        if (pass) {
            b ^= 1;
        }
        if (gOccBufFrame[b] == ~0u || gFrameNo < gOccBufFrame[b] + 2) {
            continue;
        }
        const u64 from = svcGetSystemTick();
        const u32* px = (const u32*) gOccBuf[b];
        // the GPU wrote it: the CPU's cache lines of it are old (the kernel's call: 0.5 ms, GSP's 0.95)
        if (R_FAILED(svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32) px, 240 * 400 * 4))) {
            gOccSvcFail++;
            GSPGPU_InvalidateDataCache((void*) px, 240 * 400 * 4);
        }
        static OccHiz h;
        const float s = gOccBufScale[b] / 16777215.0f;
        for (int t = 0; t < 1500; t++, px += 64) {
            __builtin_prefetch(px + 128);
            __builtin_prefetch(px + 136);
            u32 most = 0;  // (D24S8: the depth in the top 24 bits once the stencil byte is shifted out)
            for (int i = 0; i < 64; i += 4) {
                const u32 d0 = px[i] << 8, d1 = px[i + 1] << 8, d2 = px[i + 2] << 8, d3 = px[i + 3] << 8;
                const u32 m01 = d0 > d1 ? d0 : d1, m23 = d2 > d3 ? d2 : d3, mm = m01 > m23 ? m01 : m23;
                most = mm > most ? mm : most;
            }
            most >>= 8;
            h.tile[t / 30][t % 30] = most >= 0xFFFFFF ? INFINITY : (float) most * s;
        }
        h.tag = gOccBufTag[b];
        memcpy(h.m, gOccBufM[b], sizeof(h.m));
        h.scale = gOccBufScale[b];
        gOccBufFrame[b] = ~0u;
        {
            std::lock_guard<std::mutex> lock(gOccLock);
            h.seq = gOccPub.seq + 1;
            gOccPub = h;
        }
        gOccBuilds++;
        gOccBuildTicks += svcGetSystemTick() - from;
    }
}

// (R4) A render target's copy (BP 0x52 between its tags): the draws before kicked, then the TW x TH pixels of the
// frame's lower left corner (rows of memory 0..TH, 30 tiles a row in the buffer, the first TW / 8 of them) into the
// target's texture by GX_TextureCopy in the GX queue. A copy outside a pass to such a destination: that one forgotten.
void texRenderDrop(Texture* t)
{
    forgetBound(t);
    for (Texture*& m : gMaps) {
        m = m == t ? nullptr : m;
    }
    gTexLinearLater.push_back(t->tex.data);  // (freed when the GPU is past this picture)
    delete t;
}

__attribute__((noinline)) void texRenderCopy(const uint32_t* bp)
{
    const u32 dest = (bp[0x4B] & 0x1FFFFF) << 5;
    if (!dest) {
        return;
    }
    auto it = gTexRenders.find(dest);
    if (!gTexRenderPass) {
        if (it != gTexRenders.end()) {
            texRenderDrop(it->second);
            gTexRenders.erase(it);
            gTexRenderDropped++;
        }
        return;
    }
    if (!gInFrame) {
        return;
    }
    const gxCmdQueue_s& q = *gQueue;
    if (q.numEntries + kQueueForKick + 2 > q.maxEntries) {
        gTexRenderNoQueue++;
        return;
    }
    const int tw = gTexRenderW, th = gTexRenderH;
    Texture* t = it == gTexRenders.end() ? nullptr : it->second;
    if (t && ((int) t->tex.width != tw || (int) t->tex.height != th)) {
        texRenderDrop(t);
        gTexRenders.erase(it);
        t = nullptr;
    }
    if (!t) {
        const u32 bytes = (u32) (tw * th * 4);
        void* texels = linearMemAlign(bytes, 0x80);
        while (!texels && dropOldest(kTexAge, true)) {  // (R4: room as for the textures)
            texels = linearMemAlign(bytes, 0x80);
        }
        if (!texels) {
            gTexRenderNoQueue++;
            return;
        }
        memset(texels, 0, bytes);
        flushData(texels, bytes);
        t = new Texture();
        C3D_Tex& c = t->tex;
        c.data = texels;
        c.fmt = GPU_RGBA8;
        c.size = bytes;
        c.width = (u16) tw;
        c.height = (u16) th;
        c.param = GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_LINEAR) |
                  GPU_TEXTURE_WRAP_S(GPU_REPEAT) | GPU_TEXTURE_WRAP_T(GPU_REPEAT);
        t->linear = texels;
        t->bytes = bytes;
        t->mipNear = gMipNearFrom != 0;  // (no mipmaps, no LOD bias: those switches left alone)
        t->biased = true;
        t->turned = true;
        gTexRenders[dest] = t;
        re4_log("3ds: a render target's copy %d x %d (0x%08x, R4) from picture %u", tw, th, (unsigned) dest, gFrameNo);
    }
    kick();
    const u32 line = (u32) (tw / 8 * 16);  // (16-byte units: a tile of RGBA8 is 16 of them)
    GX_TextureCopy((u32*) gTarget->frameBuf.colorBuf, GX_BUFFER_DIM(line, 30 * 16 - line), (u32*) t->tex.data,
                   GX_BUFFER_DIM(line, 0), (u32) (tw * th * 4), GX_TRANSFER_RAW_COPY(1));
    forgetBound(t);  // (bound again: the GPU's texture cache cleared)
    t->lastFrame = gFrameNo;
    gTexRenderCopies++;
}

// The TEV colour registers and konstants (BP 0xE0-0xE7: R/A, then B/G of register n; 11-bit
// signed for the registers, 8-bit for the konstants), and the registers that shape the combiners.
void bpWritten(uint32_t reg, uint32_t value)
{
    if (gOwnState) {  // (2026-11-19) the renderer's own state, before gLastBp is read below
        gOwnBp[reg & 0xFF] = value;
    }
    // (stage 7.0) the TEV's registers: compared at the next draw (tevBpResolve), the batch left open
    const bool tevOnly = gSpriteJoin && ((reg >= 0x28 && reg <= 0x2F) || (reg >= 0xC0 && reg <= 0xE7) || reg == 0xF3 ||
                                         (reg >= 0xF6 && reg <= 0xFD));
    if (!tevOnly) {
        flushBatch();
    }
    if (reg == 0x52 && !(value & (1u << 14)) && gLastBp) {
        texRenderCopy(gLastBp);  // (R4: a render target's)
        waterCopy(gLastBp);  // (the water's copy)
        // (stage 6.8) what the copy's rectangle holds outside what was drawn: the clear colour (A R | G B), R or Y
        const u32 r = gLastBp[0x4F] & 0xFF;
        const u32 g = (gLastBp[0x50] >> 8) & 0xFF, b = gLastBp[0x50] & 0xFF;
        const u32 y = (value & (1u << 15)) ? (66 * r + 129 * g + 25 * b + 128) / 256 + 16 : r;
        const u32 l = y > 255 ? 255 : y;
        gCopyFillOf[(gLastBp[0x4B] & 0x1FFFFF) << 5] = l << 8 | l;  // (the alpha: the intensity; IA alike)
    }
    if (reg == 0x52 && (value & (1u << 11)) && !(value & (1u << 14)) && gLastBp) {
        clearCopy(gLastBp);  // (a display copy's clear: the next picture's)
    }
    if (reg >= 0xE0 && reg <= 0xE7) {
        const int n = (int) (reg - 0xE0) / 2;
        const bool konst = value & 0x800000;
        float* v = konst ? gKonst[n] : gTevReg[n];
        auto field = [konst](u32 x) {
            if (konst) {
                return (float) (x & 0xFF) / 255.0f;
            }
            int s = (int) (x & 0x7FF);
            s = s & 0x400 ? s - 0x800 : s;
            return (float) s / 255.0f;
        };
        if (!(reg & 1)) {
            v[0] = field(value);
            v[3] = field(value >> 12);
        } else {
            v[2] = field(value);
            v[1] = field(value >> 12);
        }
        (tevOnly ? gTevBpDirty : gTevDirty) = true;
    } else if (reg == 0x00 || (reg >= 0x28 && reg <= 0x2F) || (reg >= 0xC0 && reg <= 0xDF) || reg == 0xF3 ||
               (reg >= 0xF6 && reg <= 0xFD)) {  // (0xF3: the alpha test, set with the combiners)
        (tevOnly ? gTevBpDirty : gTevDirty) = true;
    }
}

// The lights of channel 0 (XF 0x100E bits 2-5, 11-14) where it is lit (bit 1); none with RE4_3DS_LIGHTS=0
u32 channelLights(const uint32_t* xfReg)
{
    const u32 cc = xfReg[0x0E];
    return gLighting && (cc & 2) ? ((cc >> 2) & 15) | ((cc >> 11) & 15) << 4 : 0;
}

// The channel reads the vertex's colour: as the material (bit 0) or as the ambient (bit 6, lit)
bool channelFromVertex(const uint32_t* xfReg)
{
    const u32 cc = xfReg[0x0E], ac = xfReg[0x10];
    return (cc & 1) || (ac & 1) || (cc & 0x42) == 0x42 || (ac & 0x42) == 0x42;
}

// Light n as the uniforms of a slot (XF 0x600 + 16 n: colour, cosine attenuation a0..a2, distance attenuation
// k0..k2, position, direction; in view space): colour; position, k0; direction, k1; a0..a2, k2.
// Without attenuation (channel bit 9 clear) the factor is 1: a = k = (1, 0, 0).
// The mode (gSlotMode): 1 no attenuation, 2 short (a1 = a2 = k1 = 0: the colour times max(0, a0)),
// 3 whole, 4 far (stage 3.8: short, k2 = 0 and k0 > 0, further than gFarLight in view space: a directional light,
// the direction to it the same over the scene within ~ the scene's size / its distance; r100's L0 and L2, ~100 000 off).
int lightMode(int n, bool attenuated, const uint32_t* xfMem)
{
    const u32* l = xfMem + 0x600 + 16 * n;
    const int mode = !attenuated ? 1 : asFloat(l[5]) == 0.0f && asFloat(l[6]) == 0.0f && asFloat(l[8]) == 0.0f ? 2 : 3;
    if (mode == 2 && gFarLight > 0.0f && gFrameNo >= gFarFrom && asFloat(l[9]) == 0.0f && asFloat(l[7]) > 0.0f) {
        const float x = asFloat(l[10]), y = asFloat(l[11]), z = asFloat(l[12]);
        return x * x + y * y + z * z > gFarLight * gFarLight ? 4 : 2;
    }
    return mode;
}

void uploadLight(int slot, int n, int mode, const uint32_t* xfMem)
{
    const bool attenuated = mode != 1;
    const u32* l = xfMem + 0x600 + 16 * n;
    float a[3], k[3];
    for (int i = 0; i < 3; i++) {
        a[i] = attenuated ? asFloat(l[4 + i]) : i == 0 ? 1.0f : 0.0f;
        k[i] = attenuated ? asFloat(l[7 + i]) : i == 0 ? 1.0f : 0.0f;
    }
    const float f = mode == 3 ? 1.0f / 255.0f : (a[0] > 0.0f ? a[0] : 0.0f) / 255.0f / (mode == 4 ? k[0] : 1.0f);
    C3D_FVec* u = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, gLocLights + 4 * slot, 4);
    const u32 c = l[3];
    u[0].x = (float) (c >> 24) * f;
    u[0].y = (float) ((c >> 16) & 0xFF) * f;
    u[0].z = (float) ((c >> 8) & 0xFF) * f;
    u[0].w = (float) (c & 0xFF) * f;
    u[1].x = asFloat(l[10]);
    u[1].y = asFloat(l[11]);
    u[1].z = asFloat(l[12]);
    u[1].w = k[0];
    u[2].x = asFloat(l[13]);
    u[2].y = asFloat(l[14]);
    u[2].z = asFloat(l[15]);
    u[2].w = k[1];
    u[3].x = a[0];
    u[3].y = a[1];
    u[3].z = a[2];
    u[3].w = k[2];
    if (mode == 4 && !gFarPos) {  // the direction to it (5.5: with RE4_3DS_FARPOS the position stays)
        const float r = 1.0f / sqrtf(u[1].x * u[1].x + u[1].y * u[1].y + u[1].z * u[1].z);
        u[1].x *= r;
        u[1].y *= r;
        u[1].z *= r;
        u[1].w = 0.0f;
    }
    gStats.rowVectors += 4;
}

// The colour channel 0 as XF has it (0x100E colour, 0x1010 alpha): the material (the vertex's COLOR0,
// bit 0, or the register 0x100C) times the light clamped to 0..1; the light: the ambient (the
// vertex, bit 6, or the register 0x100A) plus the lights (setLights), or 1 unlit (bit 1 clear). The
// shader: material = colour * matv + matc, light = colour * ambv + ambc, the colour 0..255 (the
// vertex's, else a fixed 255). The lights light the colour, not the alpha (lit in r100 only without
// lights: its ambient is kept).
void setChannel(const uint32_t* xfReg, bool vertexColor)
{
    const u32 cc = xfReg[0x0E], ac = xfReg[0x10];
    const u32 mat = xfReg[0x0C], amb = xfReg[0x0A];
    const float k = 1.0f / 255.0f;
    float u[4][4];
    for (int c = 0; c < 4; c++) {
        const u32 ctl = c < 3 ? cc : ac;
        const int shift = 24 - 8 * c;
        const bool lit = (ctl & 2) && (gLighting || c == 3);
        u[0][c] = (ctl & 1) ? k : 0.0f;
        u[1][c] = (ctl & 1) ? 0.0f : (float) ((mat >> shift) & 0xFF) * k;
        u[2][c] = lit && (ctl & 64) ? k : 0.0f;
        u[3][c] = !lit ? 1.0f : (ctl & 64) ? 0.0f : (float) ((amb >> shift) & 0xFF) * k;
    }
    if (!gColorKnown || memcmp(u, gColor, sizeof(u))) {
        C3D_FVec* w = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, gLocColor, 4);
        for (int r = 0; r < 4; r++) {
            w[r].x = u[r][0];
            w[r].y = u[r][1];
            w[r].z = u[r][2];
            w[r].w = u[r][3];
        }
        memcpy(gColor, u, sizeof(u));
        gColorKnown = true;
    }
    if (!vertexColor) {
        setFixed(gColorAttr, 255.0f, 255.0f, 255.0f, 255.0f);
    }
}

// The shader's lights for channel 0, in its slots one after another: the far ones (loop nfar), the short
// way (nbrief), then the whole (nwhole); a slot uploaded where it holds another light, mode or an older write of it.
// The light functions other than diffuse clamped with spot attenuation (bits 7-10; r100: all lit draws
// with lights) are drawn as it, counted.
void setLights(const uint32_t* xfReg, const uint32_t* xfMem)
{
    const u32 cc = xfReg[0x0E];
    const u32 on = channelLights(xfReg);
    int far = 0, brief = 0, whole = 0;
    if (on) {
        const bool attenuated = cc & 0x200;
        gStats.litDraws++;
        gStats.lights += (u32) __builtin_popcount(on);
        gStats.lightApprox += (cc & 0x780) != 0x700;
        int mode[8];
        for (int n = 0; n < 8; n++) {
            mode[n] = (on >> n) & 1 ? lightMode(n, attenuated, xfMem) : 0;
            far += mode[n] == 4;
            brief += mode[n] == 1 || mode[n] == 2;
        }
        int next[3] = {0, far, far + brief};  // the next slot of the far, the short and the whole lights
        for (int n = 0; n < 8; n++) {
            if (!mode[n]) {
                continue;
            }
            const int slot = next[mode[n] == 4 ? 0 : mode[n] == 3 ? 2 : 1]++;
            if (gSlotLight[slot] != n || gSlotMode[slot] != mode[n] || gSlotGen[slot] != gLightGen[n]) {
                uploadLight(slot, n, mode[n], xfMem);
                gSlotLight[slot] = (u8) n;
                gSlotMode[slot] = (u8) mode[n];
                gSlotGen[slot] = gLightGen[n];
            }
        }
        whole = next[2] - far - brief;
        gStats.lightsFull += (u32) whole;
        gStats.lightsFar += (u32) far;
    }
    setSwitch(kLit, on != 0);
    setSwitch(kFar, far > 0);
    setSwitch(kFarPos, gFarPos);
    setSwitch(kBrief, brief > 0);
    setSwitch(kWhole, whole > 0);
    if (far && far != gLoopFar) {
        C3D_IVUnifSet(GPU_VERTEX_SHADER, gLocFar, far - 1, 0, 4, 0);
        gLoopFar = far;
    }
    if (brief && (brief != gLoopBrief || far != gLoopBriefFrom)) {
        C3D_IVUnifSet(GPU_VERTEX_SHADER, gLocBrief, brief - 1, 4 * far, 4, 0);
        gLoopBrief = brief;
        gLoopBriefFrom = far;
    }
    if (whole && (whole != gLoopWhole || far + brief != gLoopWholeFrom)) {
        C3D_IVUnifSet(GPU_VERTEX_SHADER, gLocWhole, whole - 1, 4 * (far + brief), 4, 0);
        gLoopWhole = whole;
        gLoopWholeFrom = far + brief;
    }
}

// The offset of a vertex part in the layout of re4_gx_frame.h
// A split draw (drawSplit, stage 3.8: a dynamic list of the display list cache): the rest of each vertex (the
// draw's stride less the position and the normal, headBytes) and the box of the positions (min, max)
const u8* gRest;
const float* gBox;

u32 headBytes(const Re4GxDraw& d)
{
    return (d.layout & RE4_GX_V_NORMAL_PACKED) ? 16 : 12;
}

u32 texOffset(const Re4GxDraw& d, int coord)
{
    u32 o = 12;
    o += (d.layout & RE4_GX_V_NORMAL) ? 12 : 0;
    o += (d.layout & RE4_GX_V_BINORMAL_TANGENT) ? 24 : 0;
    o += (d.layout & RE4_GX_V_NORMAL_PACKED) ? 4 : 0;
    o += (d.layout & RE4_GX_V_COLOR0) ? 4 : 0;
    o += (d.layout & RE4_GX_V_COLOR1) ? 4 : 0;
    for (int t = 0; t < coord; t++) {
        o += (d.layout & (RE4_GX_V_TEX0 << t)) ? 8 : 0;
    }
    return o;
}

// The texture matrix of unit u (rows s and t from XF memory) as uniforms. A vertex coordinate
// is (s, t, 1, 1) to GX and (s, t, 0, 1) from its loader: the matrix's third column goes into the
// fourth. Set where it changed. (The first try picked the rows in the shader, rows[a0.y + n] after
// mova a0.y: the console stopped whole at the first draws, 01.11.2026.)
float gTexMtx[3][8];

void setTexMatrix(int u, int row, bool fromPos, const uint32_t* xfMem)
{
    float m[8];
    for (int i = 0; i < 8; i++) {
        m[i] = asFloat(xfMem[row * 4 + i]);
    }
    if (!fromPos) {
        m[3] += m[2];
        m[7] += m[6];
        m[2] = m[6] = 0.0f;
    }
    if (!memcmp(m, gTexMtx[u], sizeof(m))) {
        return;
    }
    memcpy(gTexMtx[u], m, sizeof(m));
    C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocTexSel + 2 * u, m[0], m[1], m[2], m[3]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocTexSel + 2 * u + 1, m[4], m[5], m[6], m[7]);
}

// One attribute buffer from byte `from` of the vertex: the attributes (index, offset, bytes; by
// rising offset) and the parts between and after them up to the next vertex skipped as padding
// (codes 0xC..0xF: 4..16 bytes), as the first renderer had it. False: more than 12 parts.
struct Part {
    int attr;
    u32 offset, bytes;
};

// The fast path of setAttributes (2026-11-19, the idea of CitroWaker's recorded draws, our own state): the plan of the
// last draw that went the slow way - what it read (the inputs) and what it made (the coordinates' sources, the fixed
// attributes, the buffers: each its base - the vertices or the second buffer - its first offset and its flags). The
// same inputs: the plan again, only the addresses new (one conversion to physical a base). RE4_3DS_ATTRFAST: 0 the
// slow way always, 1 the fast path, 2 both and compared
u32 gAttrFast = 1;
struct AttrRec {
    const u8* base;
    u32 from;
};
AttrRec gAttrRec[12];
int gAttrRecN;
struct AttrPlan {
    bool valid;
    u32 layout, stride, key;
    bool vertexColor, lit, rest, texGen;
    int units;
    int unitCoord[3];
    u32 gen[3];
    bool tc[3], fromPos[3], fromNrm[3], again[3];
    bool normal, matrix, texDiv;
    int bufCount;
    u8 bufRest[12];
    u32 bufFrom[12], bufFlags0[12], bufFlags1[12];
};
AttrPlan gPlan, gPlanIn;
const u8* gAttrV;  // the slow way's vertices and second buffer base (to tell the recorded bases apart)
const u8* gAttrRestBase;
bool gAttrChecking;
C3D_BufInfo gAttrFastBuf;
u32 gAttrFastDraws, gAttrSlowDraws, gAttrMismatch, gAttrPlanFail;

// The vertex buffers into the registers: what changed since citro3d's copy (gBufLeanFrom)
void bindBufInfo(const C3D_BufInfo& b)
{
    if (!gBufLeanFrom || gFrameNo + 1 < gBufLeanFrom || !gBufCtx || !gBufKnown) {
        C3D_SetBufInfo((C3D_BufInfo*) &b);  // all of it, by citro3d
        gBufKnown = gBufLeanFrom && gFrameNo + 1 >= gBufLeanFrom && gBufCtx;
        return;
    }
    C3D_BufInfo& set = *gBufCtx;
    if (b.base_paddr != set.base_paddr) {
        GPUCMD_AddWrite(GPUREG_ATTRIBBUFFERS_LOC, b.base_paddr >> 3);
        set.base_paddr = b.base_paddr;
    }
    const int n = b.bufCount > set.bufCount ? b.bufCount : set.bufCount;
    int first = 0, last = n - 1;
    while (first < n && !memcmp(&b.buffers[first], &set.buffers[first], sizeof(C3D_BufCfg))) {
        first++;
    }
    while (last > first && !memcmp(&b.buffers[last], &set.buffers[last], sizeof(C3D_BufCfg))) {
        last--;
    }
    if (first < n) {
        const u32 words = 3 * (u32) (last - first + 1);
        GPUCMD_AddIncrementalWrites(GPUREG_ATTRIBBUFFER0_OFFSET + 3 * first, (const u32*) &b.buffers[first], words);
        memcpy(&set.buffers[first], &b.buffers[first], 4 * words);
    }
    set.bufCount = b.bufCount;
}

bool addBuffer(C3D_BufInfo* bi, const u8* v, u32 stride, const Part* parts, int n)
{
    u64 perm = 0;
    int count = 0;
    const u32 from = parts[0].offset;
    u32 at = from;
    auto pad = [&](u32 bytes) {
        while (bytes && count < 12) {
            const u32 take = bytes >= 16 ? 16 : bytes;
            perm |= (u64) (0xB + take / 4) << (4 * count++);
            bytes -= take;
        }
        return bytes == 0;
    };
    for (int i = 0; i < n; i++) {
        if (!pad(parts[i].offset - at) || count >= 12) {
            return false;
        }
        perm |= (u64) parts[i].attr << (4 * count++);
        at = parts[i].offset + parts[i].bytes;
    }
    if (!pad(stride + from - at)) {
        return false;
    }
    if (gAttrRecN < 12) {  // (2026-11-19, the fast path's plan)
        gAttrRec[gAttrRecN].base = v;
        gAttrRec[gAttrRecN].from = from;
    }
    gAttrRecN++;
    return BufInfo_Add(bi, v + from, stride, count, perm) >= 0;
}

// The draw's position matrices within the shader's rows: its default, or each vertex's index
// (a block's matrix indices read once, while the CPU may: stage 5.7b)
bool rowsFit(const Re4GxDraw& d, const void* vertices, Block* b)
{
    if ((int) (d.matrices[0] & 0xFF) + 2 >= kRows) {
        return false;
    }
    if (d.layout & RE4_GX_V_MATRIX) {
        const u32 head = gRest ? headBytes(d) : 0;
        const u8* from = gRest ? gRest : (const u8*) vertices;
        const bool inBlock = b && from >= b->block && from < b->block + b->bytes;
        if (inBlock && b->rowsTop >= 0) {
            return b->rowsTop + 2 < kRows;
        }
        if (inBlock && b->vram == b->block) {
            if (!b->room) {
                return false;  // (moved and copied with them not read: not for the CPU to read)
            }
            from = b->room + (from - b->block);
        }
        const u8* m = from + texOffset(d, 8) - head;
        int top = 0;
        for (u32 v = 0; v < d.vertexCount; v++, m += d.stride - head) {
            top = *m > top ? *m : top;
        }
        if (inBlock) {
            b->rowsTop = top;
        }
        return top + 2 < kRows;
    }
    return true;
}

// The vertex attributes: each from the vertex where it has it, else fixed for the draw. One buffer
// holds them all; a coordinate a second unit also reads comes again from a buffer of its own. The
// normal only for a draw with lights (`lit`). False: the layout does not fit the PICA's buffers.
void attrSlowDone(const C3D_BufInfo& b, u32 key, const bool* tc, const bool* fromPos, const bool* fromNrm,
                  const bool* again, bool normal, bool matrix);

bool setAttributesSlow(const Re4GxDraw& d, const void* vertices, bool vertexColor, bool lit, const uint32_t* xfMem,
                       const uint32_t* xfReg)
{
    const bool matrix = d.layout & RE4_GX_V_MATRIX;
    const int nrmKind = (d.layout & RE4_GX_V_NORMAL) ? 1 : (d.layout & RE4_GX_V_NORMAL_PACKED) ? 2 : 0;
    const int normal = lit ? nrmKind : 0;
    // each unit's coordinate as XF generates it (0x1040 + n): from a vertex coordinate, the position
    // or the normal (x y z 1; the packed one x y z w, the shader divides by w), through the texture matrix of
    // the draw (MATINDEX; 60 is the identity); the other sources (colours, emboss) give 0
    bool tc[3], fromPos[3], fromNrm[3];
    int source[3];
    u32 key = (vertexColor ? 1 : 0) | (matrix ? 2 : 0);
    for (int u = 0; u < 3; u++) {
        tc[u] = fromPos[u] = fromNrm[u] = false;
        source[u] = -1;
        if (u < gUnits) {
            const u32 gen = gTexGen ? xfReg[0x40 + gUnitCoord[u]] : (u32) (5 + gUnitCoord[u]) << 7;
            const u32 row = (gen >> 7) & 31;
            if (((gen >> 4) & 7) == 0) {
                if (row >= 5 && row <= 12 && (d.layout & (RE4_GX_V_TEX0 << (row - 5)))) {
                    source[u] = (int) row - 5;
                    tc[u] = true;
                } else if (row == 0) {
                    fromPos[u] = tc[u] = true;
                } else if (row == 1 && nrmKind) {
                    fromNrm[u] = tc[u] = true;
                }
            }
            const int c = gUnitCoord[u];
            const u32 m = c < 3 ? d.matrices[0] >> (8 * (c + 1)) : c < 7 ? d.matrices[1] >> (8 * (c - 3)) : d.matrices[2];
            setTexMatrix(u, gTexGen ? (int) ((m & 0xFF) < 62 ? m & 0xFF : 62) : 60, fromPos[u] || fromNrm[u], xfMem);
            gTexRow[u] = gTexGen ? (int) ((m & 0xFF) < 62 ? m & 0xFF : 62) : 60;
            gTexProj[u] = gTexGen && (gen & 2) && fromPos[u] && gTexRow[u] < 60;  // (STQ: the water's coordinate)
        }
        key |= (tc[u] ? 4u : 0) << u | (fromPos[u] ? 32u : 0) << u | (fromNrm[u] ? 1024u : 0) << u;
    }
    gTexDiv = nrmKind == 2 && (fromNrm[0] || fromNrm[1] || fromNrm[2]);
    gTexFromNrm[0] = fromNrm[0];  // (2026-11-18)
    gTexFromNrm[1] = fromNrm[1];
    gTexFromNrm[2] = fromNrm[2];
    key |= (u32) nrmKind << 13;
    key |= (u32) normal << 8;
    if (key != gAttrKey) {
        C3D_AttrInfo* ai = C3D_GetAttrInfo();
        AttrInfo_Init(ai);
        AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
        gColorAttr = vertexColor ? AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4) : AttrInfo_AddFixed(ai, 1);
        gMatrixAttr = matrix ? AttrInfo_AddLoader(ai, 2, GPU_UNSIGNED_BYTE, 4) : AttrInfo_AddFixed(ai, 2);
        for (int u = 0; u < 3; u++) {
            const bool bytes = fromNrm[u] && nrmKind == 2;
            gTexAttr[u] = tc[u] ? AttrInfo_AddLoader(ai, 3 + u, bytes ? GPU_BYTE : GPU_FLOAT,
                                                     bytes ? 4 : fromPos[u] || fromNrm[u] ? 3 : 2)
                                : AttrInfo_AddFixed(ai, 3 + u);
        }
        gNormalAttr = normal ? AttrInfo_AddLoader(ai, 6, normal == 1 ? GPU_FLOAT : GPU_BYTE, normal == 1 ? 3 : 4)
                             : AttrInfo_AddFixed(ai, 6);
        gAttrKey = key;
        memset(gFixedKnown, 0, sizeof(gFixedKnown));
    }
    const u8* v = (const u8*) vertices;
    C3D_BufInfo buffers;
    C3D_BufInfo* bi = &buffers;
    BufInfo_Init(bi);
    Part parts[8];
    int n = 0;
    parts[n++] = Part{0, 0, 12};
    if (normal) {
        parts[n++] = Part{gNormalAttr, 12, normal == 1 ? 12u : 4u};
    } else {
        setFixed(gNormalAttr, 0.0f, 0.0f, 1.0f, 0.0f);
    }
    if (vertexColor) {
        parts[n++] = Part{gColorAttr, texOffset(d, 0) - ((d.layout & RE4_GX_V_COLOR1) ? 8 : 4), 4};
    }
    // the position (offset 0) and the normal (12) again, in a buffer of their own
    bool again[3] = {fromPos[0] || fromNrm[0], fromPos[1] || fromNrm[1], fromPos[2] || fromNrm[2]};
    for (int coord = 0; coord < 8; coord++) {
        bool first = true;
        for (int u = 0; u < 3; u++) {
            if (tc[u] && source[u] == coord) {
                if (first) {
                    parts[n++] = Part{gTexAttr[u], texOffset(d, coord), 8};
                    first = false;
                } else {
                    again[u] = true;
                }
            }
        }
    }
    if (matrix) {
        parts[n++] = Part{gMatrixAttr, texOffset(d, 8), 4};
    } else {
        setFixed(gMatrixAttr, (float) (d.matrices[0] & 0xFF), 0.0f, 0.0f, 0.0f);
    }
    // a split draw: the position and the normal from the vertices, the rest from gRest (its offsets less the head's)
    const u32 head = gRest ? headBytes(d) : 0;
    int inHead = n;
    if (gRest) {
        for (inHead = 0; inHead < n && parts[inHead].offset < head; inHead++) {
        }
    }
    if (!addBuffer(bi, v, gRest ? head : d.stride, parts, inHead) ||
        (inHead < n && !addBuffer(bi, gRest - head, d.stride - head, parts + inHead, n - inHead))) {
        return false;
    }
    for (int u = 0; u < 3; u++) {
        if (again[u]) {
            const Part p = fromPos[u]   ? Part{gTexAttr[u], 0, 12}
                           : fromNrm[u] ? Part{gTexAttr[u], 12, nrmKind == 2 ? 4u : 12u}
                                        : Part{gTexAttr[u], texOffset(d, source[u]), 8};
            const bool rest = gRest && p.offset >= head;
            if (!addBuffer(bi, rest ? gRest - head : v, rest ? d.stride - head : gRest ? head : d.stride, &p, 1)) {
                return false;
            }
        } else if (!tc[u]) {
            setFixed(gTexAttr[u], 0.0f, 0.0f, 0.0f, 1.0f);  // (w: the shader divides by it)
        }
    }
    attrSlowDone(buffers, key, tc, fromPos, fromNrm, again, normal != 0, matrix);
    bindBufInfo(buffers);
    return true;
}

void attrSlowDone(const C3D_BufInfo& b, u32 key, const bool* tc, const bool* fromPos, const bool* fromNrm,
                  const bool* again, bool normal, bool matrix)
{
    if (gAttrChecking) {  // (RE4_3DS_ATTRFAST=2) the fast path's buffers and choices against these
        bool same = gAttrFastBuf.base_paddr == b.base_paddr && gAttrFastBuf.bufCount == b.bufCount &&
                    !memcmp(gAttrFastBuf.buffers, b.buffers, sizeof(C3D_BufCfg) * (size_t) b.bufCount) &&
                    key == gPlan.key && gTexDiv == gPlan.texDiv && normal == gPlan.normal && matrix == gPlan.matrix;
        for (int u = 0; u < 3; u++) {
            same &= tc[u] == gPlan.tc[u] && fromPos[u] == gPlan.fromPos[u] && fromNrm[u] == gPlan.fromNrm[u] &&
                    again[u] == gPlan.again[u];
        }
        if (!same) {
            if (gAttrMismatch < 8) {
                re4_log("3ds: attributes fast: differs from the slow way: layout %08lx stride %lu, buffers %d/%d, "
                        "first offsets %08lx/%08lx", (unsigned long) gPlan.layout, (unsigned long) gPlan.stride,
                        gAttrFastBuf.bufCount, b.bufCount, (unsigned long) gAttrFastBuf.buffers[0].offset,
                        (unsigned long) b.buffers[0].offset);
            }
            gAttrMismatch++;
        }
        return;
    }
    AttrPlan& p = gPlanIn;
    p.valid = gAttrRecN == b.bufCount && b.bufCount <= 12;
    p.key = key;
    p.texDiv = gTexDiv;
    p.normal = normal;
    p.matrix = matrix;
    for (int u = 0; u < 3; u++) {
        p.tc[u] = tc[u];
        p.fromPos[u] = fromPos[u];
        p.fromNrm[u] = fromNrm[u];
        p.again[u] = again[u];
    }
    p.bufCount = b.bufCount;
    for (int i = 0; p.valid && i < b.bufCount; i++) {
        const u8* base = gAttrRec[i].base;
        p.valid = base == gAttrV || (p.rest && base == gAttrRestBase);
        p.bufRest[i] = base != gAttrV;
        p.bufFrom[i] = gAttrRec[i].from;
        p.bufFlags0[i] = b.buffers[i].flags[0];
        p.bufFlags1[i] = b.buffers[i].flags[1];
    }
    if (p.valid) {
        gPlan = p;
    } else {
        gPlan.valid = false;
        gAttrPlanFail++;
    }
}

// The fast path: false when the plan does not hold (the slow way then); `bind` false: the buffers into gAttrFastBuf
bool attrFast(const Re4GxDraw& d, const void* vertices, bool vertexColor, bool lit, const uint32_t* xfMem,
              const uint32_t* xfReg, bool bind)
{
    const AttrPlan& p = gPlan;
    if (!p.valid || p.layout != d.layout || p.stride != d.stride || p.vertexColor != vertexColor || p.lit != lit ||
        p.rest != (gRest != nullptr) || p.units != gUnits || p.texGen != gTexGen || p.key != gAttrKey) {
        return false;
    }
    for (int u = 0; u < gUnits; u++) {
        if (p.unitCoord[u] != gUnitCoord[u] || (gTexGen && p.gen[u] != xfReg[0x40 + gUnitCoord[u]])) {
            return false;
        }
    }
    const u8* v = (const u8*) vertices;
    const u32 paV = osConvertVirtToPhys(v);
    const u32 paR = p.rest ? osConvertVirtToPhys(gRest) - headBytes(d) : 0;
    C3D_BufInfo& b = gAttrFastBuf;
    BufInfo_Init(&b);
    if (!paV || (p.rest && paR + headBytes(d) == 0)) {
        return false;
    }
    for (int i = 0; i < p.bufCount; i++) {
        const u32 pa = (p.bufRest[i] ? paR : paV) + p.bufFrom[i];
        if (pa < b.base_paddr) {
            return false;
        }
        b.buffers[i].offset = pa - b.base_paddr;
        b.buffers[i].flags[0] = p.bufFlags0[i];
        b.buffers[i].flags[1] = p.bufFlags1[i];
    }
    b.bufCount = p.bufCount;
    for (int u = 0; u < gUnits; u++) {
        const int c = gUnitCoord[u];
        const u32 m = c < 3 ? d.matrices[0] >> (8 * (c + 1)) : c < 7 ? d.matrices[1] >> (8 * (c - 3)) : d.matrices[2];
        const int row = gTexGen ? (int) ((m & 0xFF) < 62 ? m & 0xFF : 62) : 60;
        setTexMatrix(u, row, p.fromPos[u] || p.fromNrm[u], xfMem);
        gTexRow[u] = row;
        gTexProj[u] = gTexGen && (p.gen[u] & 2) && p.fromPos[u] && row < 60;
    }
    gTexDiv = p.texDiv;
    gTexFromNrm[0] = p.fromNrm[0];
    gTexFromNrm[1] = p.fromNrm[1];
    gTexFromNrm[2] = p.fromNrm[2];
    if (!p.normal) {
        setFixed(gNormalAttr, 0.0f, 0.0f, 1.0f, 0.0f);
    }
    if (!p.matrix) {
        setFixed(gMatrixAttr, (float) (d.matrices[0] & 0xFF), 0.0f, 0.0f, 0.0f);
    }
    for (int u = 0; u < 3; u++) {
        if (!p.again[u] && !p.tc[u]) {
            setFixed(gTexAttr[u], 0.0f, 0.0f, 0.0f, 1.0f);
        }
    }
    if (bind) {
        bindBufInfo(b);
    }
    return true;
}

bool setAttributes(const Re4GxDraw& d, const void* vertices, bool vertexColor, bool lit, const uint32_t* xfMem,
                   const uint32_t* xfReg)
{
    if (gAttrFast == 1 && attrFast(d, vertices, vertexColor, lit, xfMem, xfReg, true)) {
        gAttrFastDraws++;
        return true;
    }
    if (gAttrFast == 2 && attrFast(d, vertices, vertexColor, lit, xfMem, xfReg, false)) {
        gAttrFastDraws++;
        int row[3] = {gTexRow[0], gTexRow[1], gTexRow[2]};
        bool proj[3] = {gTexProj[0], gTexProj[1], gTexProj[2]};
        gAttrChecking = true;
        gAttrRecN = 0;
        const bool ok = setAttributesSlow(d, vertices, vertexColor, lit, xfMem, xfReg);
        gAttrChecking = false;
        bool same = ok;
        for (int u = 0; u < gUnits; u++) {
            same &= row[u] == gTexRow[u] && proj[u] == gTexProj[u];
        }
        if (!same) {
            if (gAttrMismatch < 8) {
                re4_log("3ds: attributes fast: rows or projection differ (slow way %s)", ok ? "drawn" : "failed");
            }
            gAttrMismatch++;
        }
        return ok;
    }
    gAttrSlowDraws++;
    AttrPlan& in = gPlanIn;
    in.layout = d.layout;
    in.stride = d.stride;
    in.vertexColor = vertexColor;
    in.lit = lit;
    in.rest = gRest != nullptr;
    in.texGen = gTexGen;
    in.units = gUnits;
    for (int u = 0; u < 3; u++) {
        in.unitCoord[u] = u < gUnits ? gUnitCoord[u] : -1;
        in.gen[u] = u < gUnits && gTexGen ? xfReg[0x40 + gUnitCoord[u]] : 0;
    }
    gAttrV = (const u8*) vertices;
    gAttrRestBase = gRest ? gRest - headBytes(d) : nullptr;
    gAttrRecN = 0;
    if (!setAttributesSlow(d, vertices, vertexColor, lit, xfMem, xfReg)) {
        gPlan.valid = false;
        return false;
    }
    return true;
}

// GX blend factors (BP 0x41) on the PICA; the source's 2/3 read the destination colour, the
// destination's the source colour.
GPU_BLENDFACTOR blendFactor(u32 f, bool source)
{
    switch (f & 7) {
    case 0: return GPU_ZERO;
    case 1: return GPU_ONE;
    case 2: return source ? GPU_DST_COLOR : GPU_SRC_COLOR;
    case 3: return source ? GPU_ONE_MINUS_DST_COLOR : GPU_ONE_MINUS_SRC_COLOR;
    case 4: return GPU_SRC_ALPHA;
    case 5: return GPU_ONE_MINUS_SRC_ALPHA;
    case 6: return GPU_DST_ALPHA;
    default: return GPU_ONE_MINUS_DST_ALPHA;
    }
}

// The depth test (BP 0x40: enable, function, update) and the blending (BP 0x41: enable, logic op,
// subtract, factors, colour and alpha update). The depth of this renderer grows towards the eye
// (C3D_DepthMap of citro3d: -z/w, and GX's near plane is at -1), GX's shrinks: the functions turn.
void setBlendDepth(const uint32_t* bp)
{
    const u32 zmode = bp[0x40] & 0x1F, cmode0 = bp[0x41] & 0xFFF;
    if (zmode == gZMode && cmode0 == gCMode0) {
        return;
    }
    static const GPU_TESTFUNC kDepth[8] = {GPU_NEVER, GPU_GREATER, GPU_EQUAL, GPU_GEQUAL,
                                           GPU_LESS,  GPU_NOTEQUAL, GPU_LEQUAL, GPU_ALWAYS};
    static const GPU_TESTFUNC kDepthGx[8] = {GPU_NEVER,   GPU_LESS,     GPU_EQUAL,  GPU_LEQUAL,  // RE4_3DS_FOG=2
                                             GPU_GREATER, GPU_NOTEQUAL, GPU_GEQUAL, GPU_ALWAYS};
    const bool zOn = zmode & 1, zUpdate = zmode & 0x10;
    const bool colorUpdate = cmode0 & 8, alphaUpdate = cmode0 & 0x10;
    const int mask = (colorUpdate ? GPU_WRITE_COLOR : 0) | (alphaUpdate ? GPU_WRITE_ALPHA : 0) |
                     (zOn && zUpdate ? GPU_WRITE_DEPTH : 0);
    C3D_DepthTest(true, zOn ? (gFogMode == 2 ? kDepthGx : kDepth)[(zmode >> 1) & 7] : GPU_ALWAYS, (GPU_WRITEMASK) mask);
    gBlending = (cmode0 & 1) && !(cmode0 & 2);
    if (cmode0 & 0x800) {  // subtract: destination - source
        C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_REVERSE_SUBTRACT, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
        gBlending = true;
    } else if (gBlending) {
        const GPU_BLENDFACTOR src = blendFactor(cmode0 >> 8, true), dst = blendFactor(cmode0 >> 5, false);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, src, dst, src, dst);
    } else {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    }
    gZMode = zmode;
    gCMode0 = cmode0;
}

// GX's scissor (BP 0x20 top/left, 0x21 bottom/right, inclusive; EFB pixels + 342) and an EFB copy's rectangle (stage
// 5.4): EFB pixels to the screen by the picture's place there (gMap), within the picture (no draw beyond the EFB: the
// sides of 4:3 stay black), then to the PICA's buffer turned a quarter as the projection (x' = y, y' = -x; the
// buffer's x 0-240 the screen's height). The whole buffer: no scissor; nothing: nothing drawn. (The game's band
// inside the letterbox, SetScissorState: the scene under the bars was drawn for nothing, ~6 ms of the GPU in r100.)
void scissorRect(float x0, float y0, float x1, float y1)
{
    x0 = fmaxf(x0, 0.0f), y0 = fmaxf(y0, 0.0f);
    x1 = fminf(x1, (float) gPicW), y1 = fminf(y1, (float) gPicH);
    auto clamp = [](float v, int hi) {
        const int i = (int) lroundf(v);
        return i < 0 ? 0 : i > hi ? hi : i;
    };
    const int sx0 = clamp(gMapOx + gMapSx * x0, 400), sx1 = clamp(gMapOx + gMapSx * x1, 400);
    const int sy0 = clamp(gMapOy + gMapSy * y0, 240), sy1 = clamp(gMapOy + gMapSy * y1, 240);
    if (sx1 <= sx0 || sy1 <= sy0) {
        C3D_SetScissor(GPU_SCISSOR_INVERT, 0, 0, 240, 400);
    } else if (sx0 == 0 && sy0 == 0 && sx1 == 400 && sy1 == 240) {
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    } else {
        C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32) (240 - sy1), (u32) (400 - sx1), (u32) (240 - sy0), (u32) (400 - sx0));
    }
}

// A draw's scissor (RE4_3DS_SCISSOR=N: GX's from picture N, before it and with 0 the whole picture); a band of the
// whole width inside the letterbox (rows 56-392 in the game) noted for the next picture's place (gBandSeen)
void setScissor(const uint32_t* bp, const uint32_t* xfReg)
{
    (void) xfReg;
    if (gGpuTest == 1) {
        return;
    }
    const u32 key[6] = {bp[0x20] & 0xFFFFFF, bp[0x21] & 0xFFFFFF, 0, 0, 0, 0};
    if (!memcmp(key, gScissorKey, sizeof(key))) {
        return;
    }
    memcpy(gScissorKey, key, sizeof(key));
    const int left = (int) ((bp[0x20] >> 12) & 0x7FF) - 342, top = (int) (bp[0x20] & 0x7FF) - 342;
    const int right = (int) ((bp[0x21] >> 12) & 0x7FF) - 341, bottom = (int) (bp[0x21] & 0x7FF) - 341;
    const int w = (int) gPicW, h = (int) gPicH;
    gLastRect[0] = left;
    gLastRect[1] = top;
    gLastRect[2] = right;
    gLastRect[3] = bottom;
    if (left <= 0 && right >= w - 1 && top >= 16 && bottom <= h - 16 && bottom - top >= h / 2) {
        gBandSeen = true;
        gBandTop = top;
        gBandBottom = bottom;
    }
    if (!gScissorFrom || gFrameNo + 1 < gScissorFrom) {
        scissorRect(0.0f, 0.0f, (float) w, (float) h);
    } else {
        scissorRect((float) left, (float) top, (float) right, (float) bottom);
    }
}

// (stage 7.0) A small list from the display list cache as a staged draw (drawTimed): its limits; its block not moved
// to VRAM (kept: the CPU reads linear memory only); the draws made so (per log window)
const u32 kUnkeepVertices = 16, kUnkeepIndices = 96, kUnkeepBlock = 1024;
u32 gUnkept;

bool inVram(const void* p)
{
    const u32 a = (u32) (uintptr_t) p;
    return a >= 0x1F000000 && a < 0x1F600000;
}

void* stage(uint32_t vertexBytes, uint32_t indices, uint16_t** indexOut)
{
    const u32 bytes = (vertexBytes + 3) & ~3u;  // in a row: a batch's draws share its buffer
    const u32 room = (indices + 7) & ~7u;
    if (bytes > kStageBytes || room > kStageIndices) {
        gStats.skipped++;
        return nullptr;
    }
    if (!gInFrame) {
        beginFrame();
    }
    if (gStageUsed + bytes > kStageBytes || gIndicesUsed + room > kStageIndices ||
        gpuCmdBufSize - gpuCmdBufOffset < kCmdBufReserve) {
        split();
    }
    void* v = gStage + gStageUsed;
    gStageUsed += bytes;
    *indexOut = gIndices + gIndicesUsed;
    gIndicesUsed += room;
    return v;
}

// Textures that go for a list with no room in linear memory (stage 6.3: textures went only for textures; 19 minutes
// into the user's game linear memory was theirs, the largest piece 96 KB, and r106's lists were made anew each picture,
// 70-136 ms a frame, play-texmiss1.log). RE4_3DS_KEEPDROP=0: none.
bool gKeepDrop = true;
const u32 kKeepDrops = 64;  // at most a list (no room still: the decoder tries the list again two frames later)
u32 gKeepDropped;           // per log window

// Lasting room for the display list cache: the vertices (and 256 bytes a coordinate's own buffer
// reads past the last one), then the indices.
void* keep(uint32_t vertexBytes, uint32_t indices, uint16_t** indexOut)
{
    const u32 bytes = (vertexBytes + 3) & ~3u;
    u8* p = gArena ? (u8*) arenaAlloc(bytes + 256 + indices * 2) : nullptr;
    p = p ? p : (u8*) linearAlloc(bytes + 256 + indices * 2);
    for (u32 n = 0; !p && gKeepDrop && n < kKeepDrops && dropOldest(kTexAge, true); n++) {
        gKeepDropped++;
        p = (u8*) linearAlloc(bytes + 256 + indices * 2);
    }
    if (p) {
        *indexOut = (u16*) (p + bytes + 256);
        gKeptLive += keepSize(p);
        gBlocks[p] = Block{p, (bytes + 256 + indices * 2 + 15) & ~15u, *indexOut, nullptr, p};
        gBlockOf[*indexOut] = p;
    }
    return p;
}

void drop(void* vertices)
{
    gDropped.push_back(vertices);
}

// (stage 41) Linear memory of the decoder's that draws up to now read: back to it once the GPU is past them
void later(void* p)
{
    gHeadLater.push_back(p);
}

// An EFB copy that clears its rectangle (BP 0x52 bit 11, a texture copy; stage 5.4): the rectangle (BP 0x49/0x4A)
// filled with the clear colour (0x4F/0x50) and depth (0x51) where colour, alpha and depth update (0x41 bits 3, 4;
// 0x40 bit 4), as gx_gl.cpp's ClearAfterCopy. The copies themselves are not made, but what the game drew for them
// (the shadow pass: a 148 x 128 viewport in the EFB's corner) went away with the clear on the GameCube. A quad
// through the renderer's shader: rows 45-47 and the projection the identity (put back at the next draw), the depth
// by C3D_DepthMap with no scale, the scissor the rectangle.
const int kClearRow = 45;
void clearCopy(const uint32_t* bp)
{
    const u32 cmode0 = bp[0x41], zmode = bp[0x40];
    const bool colorUpdate = cmode0 & 8, alphaUpdate = cmode0 & 0x10, zUpdate = zmode & 0x10;
    if (!gInFrame || !(colorUpdate || alphaUpdate || zUpdate) || !gClearFrom || gFrameNo + 1 < gClearFrom) {
        return;
    }
    uint16_t* unused;
    float* v = (float*) stage(4 * 12, 0, &unused);  // (may split the frame: before any state is set)
    if (!v) {
        return;
    }
    static const float kQuad[12] = {-1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, -1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f};
    memcpy(v, kQuad, sizeof(kQuad));
    const float x0 = (float) (bp[0x49] & 0x3FF), y0 = (float) ((bp[0x49] >> 10) & 0x3FF);
    scissorRect(x0, y0, x0 + (float) ((bp[0x4A] & 0x3FF) + 1), y0 + (float) (((bp[0x4A] >> 10) & 0x3FF) + 1));
    gScissorKey[0] = ~0u;
    C3D_AttrInfo* ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
    int matrixAttr = -1;
    for (int r = 1; r <= 6; r++) {
        const int id = AttrInfo_AddFixed(ai, r);
        matrixAttr = r == 2 ? id : matrixAttr;
    }
    gAttrKey = 0xFFFFFFFF;
    memset(gFixedKnown, 0, sizeof(gFixedKnown));
    C3D_FixedAttribSet(matrixAttr, (float) kClearRow, 0.0f, 0.0f, 0.0f);
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, v, 12, 1, 0x0);
    gBufKnown = false;  // (citro3d writes these)
    C3D_FVec* rows = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, gLocRows + kClearRow, 3);
    C3D_FVec* proj = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, gLocProj, 4);
    for (int i = 0; i < 4; i++) {
        C3D_FVec* row[2] = {i < 3 ? &rows[i] : nullptr, &proj[i]};
        for (C3D_FVec* e : row) {
            if (e) {
                e->x = i == 0 ? 1.0f : 0.0f;
                e->y = i == 1 ? 1.0f : 0.0f;
                e->z = i == 2 ? 1.0f : 0.0f;
                e->w = i == 3 ? 1.0f : 0.0f;
            }
        }
    }
    gRowsMin = gRowsMin < kClearRow ? gRowsMin : kClearRow;
    gRowsMax = kRows - 1;
    gProjDirty = true;
    setSwitch(kLit, false);
    setSwitch(kTexDiv, false);
    setSwitch(kTex1, false);
    setSwitch(kTex2, false);
    const u32 r = bp[0x4F] & 0xFF, a = (bp[0x4F] >> 8) & 0xFF, b = bp[0x50] & 0xFF, g = (bp[0x50] >> 8) & 0xFF;
    gTevGivenMask = 0;  // (X, finding 11: written here)
    for (int i = 0; i < 6; i++) {
        C3D_TexEnv* env = C3D_GetTexEnv(i);
        C3D_TexEnvInit(env);
        if (i == 0) {
            C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
            C3D_TexEnvColor(env, r | g << 8 | b << 16 | a << 24);
        }
    }
    C3D_TexEnvBufUpdate(C3D_Both, 0);
    gTevDirty = true;
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_DepthTest(true, GPU_ALWAYS, (GPU_WRITEMASK) ((colorUpdate ? GPU_WRITE_COLOR : 0) |
                                                     (alphaUpdate ? GPU_WRITE_ALPHA : 0) | (zUpdate ? GPU_WRITE_DEPTH : 0)));
    gZMode = gCMode0 = 0xFFFFFFFF;
    C3D_CullFace(GPU_CULL_NONE);
    gCull = 0xFFFFFFFF;
    if (gFogOn) {
        C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
        gFogOn = false;
    }
    // the depth here: -z/w (near at 1), or GX's way round with RE4_3DS_FOG=2 (z/w + 1, near at 0)
    const float z = (float) (bp[0x51] & 0xFFFFFF) / 16777215.0f;
    C3D_DepthMap(true, 0.0f, gFogMode == 2 ? z : 1.0f - z);
    C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
    if (gFogMode != 2) {
        C3D_DepthMap(true, -1.0f, 0.0f);  // (with RE4_3DS_FOG=2 uploadProjection sets it)
    }
    gClears++;
}

// A kept draw's vertices (where in its block) and indices from the block's copy in VRAM, the copy made
// first where there is room (VRAM, the queue, the DMAs of this picture)
// The block of a kept draw (by its indices; nullptr: none)
Block* blockOf(const uint16_t* indices)
{
    auto of = gBlockOf.find(indices);
    return of == gBlockOf.end() ? nullptr : &gBlocks[of->second];
}

// A kept draw wholly outside the view: the box of its positions (a static draw's kept in its block, the vertices
// there; a dynamic one's, the template with this picture's positions, made each time), its 8 corners through the
// position matrix and the projection, all beyond one clip plane (GX's depth in [-w, 0]: beyond the far plane z > 0,
// the near one z < -w)
bool outsideView(const Re4GxDraw& d, const void* vertices, Block* b, const uint32_t* xfMem, const uint32_t* xfReg)
{
    const u8* v0 = (const u8*) vertices;
    const bool inBlock = b && v0 >= b->block && v0 < b->block + b->bytes;
    float lo[3], hi[3];
    if (gBox) {
        memcpy(lo, gBox, sizeof(lo));
        memcpy(hi, gBox + 3, sizeof(hi));
    } else if (inBlock && b->boxed) {
        memcpy(lo, b->lo, sizeof(lo));
        memcpy(hi, b->hi, sizeof(hi));
    } else if (inBlock && b->vram == b->block && !b->room) {
        return false;  // (moved and copied with no box made: not for the CPU to read)
    } else {
        if (inBlock && b->room) {
            v0 = b->room + (v0 - b->block);  // (moved: read from the room)
        }
        lo[0] = lo[1] = lo[2] = 1e30f;
        hi[0] = hi[1] = hi[2] = -1e30f;
        for (u32 i = 0; i < d.vertexCount; i++) {
            const float* pos = (const float*) (v0 + i * d.stride);
            for (int k = 0; k < 3; k++) {
                lo[k] = pos[k] < lo[k] ? pos[k] : lo[k];
                hi[k] = pos[k] > hi[k] ? pos[k] : hi[k];
            }
        }
        if (inBlock) {
            memcpy(b->lo, lo, sizeof(lo));
            memcpy(b->hi, hi, sizeof(hi));
            b->boxed = true;
        }
    }
    const float* m = (const float*) &xfMem[(d.matrices[0] & 0xFF) * 4];
    const float* p = (const float*) &xfReg[0x20];
    const bool persp = xfReg[0x26] == 0;
    u32 outside = 0x3F;  // a bit a plane: all corners so far beyond it
    for (int c = 0; c < 8 && outside; c++) {
        const float x = c & 1 ? hi[0] : lo[0], y = c & 2 ? hi[1] : lo[1], z = c & 4 ? hi[2] : lo[2];
        const float ex = m[0] * x + m[1] * y + m[2] * z + m[3];
        const float ey = m[4] * x + m[5] * y + m[6] * z + m[7];
        const float ez = m[8] * x + m[9] * y + m[10] * z + m[11];
        const float cx = persp ? p[0] * ex + p[1] * ez : p[0] * ex + p[1];
        const float cy = persp ? p[2] * ey + p[3] * ez : p[2] * ey + p[3];
        const float cz = p[4] * ez + p[5];
        const float cw = persp ? -ez : 1.0f;
        outside &= (cx > cw ? 1u : 0) | (cx < -cw ? 2u : 0) | (cy > cw ? 4u : 0) | (cy < -cw ? 8u : 0) |
                   (cz > 0.0f ? 16u : 0) | (cz < -cw ? 32u : 0);
    }
    return outside != 0;
}

// A moved block copied to VRAM where there is room (the DMAs of this picture, the queue); its room freed at the next
// frame's begin
void sendBlock(Block& b)
{
    const gxCmdQueue_s& q = *gQueue;
    if (!b.pending || gDmas >= kDmasPerPicture || q.numEntries + kQueueForDma > q.maxEntries) {
        return;
    }
    flushData(b.room, b.bytes);
    GX_RequestDma((u32*) b.room, (u32*) b.vram, b.bytes);
    gDmas++;
    gStats.dmas++;
    gRoomsCopied.push_back(b.room);
    b.room = nullptr;
    b.pending = false;
}

// A block moved to VRAM by its name: the decoder has written it (Re4GxDirect kept); the room itself where VRAM has
// no room (or RE4_3DS_KEPTMOVE=0)
void* kept(void* room)
{
    auto b = gBlocks.find(room);
    if (b == gBlocks.end() || (gSpriteJoin && b->second.bytes <= kUnkeepBlock) || !gKeptMove || !gVramFrom ||
        gFrameNo + 1 < gVramFrom ||
        gVramBytes + b->second.bytes > kVramKept) {
        return room;
    }
    u8* v = (u8*) vramAlloc(b->second.bytes);
    if (!v) {
        gStats.vramFailed++;
        return room;
    }
    Block moved = b->second;
    moved.block = moved.vram = v;
    moved.room = (u8*) room;
    moved.pending = true;
    moved.indices = v + ((const u8*) moved.indices - (const u8*) room);
    gBlockOf.erase(b->second.indices);
    gBlocks.erase(b);
    gBlocks[v] = moved;
    gBlockOf[moved.indices] = v;
    gVramBytes += moved.bytes;
    gStats.moved++;
    return v;
}

void fromVram(const void*& vertices, const uint16_t*& indices, u32 count, Block* bp)
{
    if (!bp) {
        return;
    }
    Block& b = *bp;
    sendBlock(b);
    if (b.pending) {  // moved, not copied yet: from its room
        const u8* v = (const u8*) vertices;
        if (v >= b.block && v < b.block + b.bytes) {
            vertices = b.room + (v - b.block);
        }
        if (gRest >= b.block && gRest < b.block + b.bytes) {
            gRest = b.room + (gRest - b.block);
        }
        indices = (const uint16_t*) (b.room + ((const u8*) indices - b.block));
        gStats.roomDraws++;
        return;
    }
    if (!b.vram) {
        const gxCmdQueue_s& q = *gQueue;
        if (gDmas >= kDmasPerPicture || gVramBytes + b.bytes > kVramKept || q.numEntries + kQueueForDma > q.maxEntries) {
            return;
        }
        b.vram = (u8*) vramAlloc(b.bytes);
        if (!b.vram) {
            gStats.vramFailed++;
            return;
        }
        flushData(b.block, b.bytes);
        GX_RequestDma((u32*) b.block, (u32*) b.vram, b.bytes);
        gVramBytes += b.bytes;
        gDmas++;
        gStats.dmas++;
    }
    const u8* v = (const u8*) vertices;
    if (v >= b.block && v < b.block + b.bytes) {
        vertices = b.vram + (v - b.block);
    }
    if (gRest >= b.block && gRest < b.block + b.bytes) {
        gRest = b.vram + (gRest - b.block);
    }
    indices = (const uint16_t*) (b.vram + ((const u8*) indices - b.block));
    gStats.vramDraws++;
    gStats.vramVertices += count;
}

__attribute__((noinline)) void drawLines(const Re4GxDraw& d, const void* vertices, const uint16_t* indices, const uint32_t* bp,
               const uint32_t* xfMem, const uint32_t* xfReg);

// (stage 7.0) The TEV's BP registers (orders, combiners, alpha test, konstant selections)
void tevBpGather(const uint32_t* bp, u32* w)
{
    int n = 0;
    for (u32 r = 0x28; r <= 0x2F; r++) {
        w[n++] = bp[r];
    }
    for (u32 r = 0xC0; r <= 0xDF; r++) {
        w[n++] = bp[r];
    }
    w[n++] = bp[0xF3];
    for (u32 r = 0xF6; r <= 0xFD; r++) {
        w[n++] = bp[r];
    }
}

// (5.5, 2026-11-09) The game's contrast pass (game/filter00.cpp Filter00RenderContrast) and its glow (the blur pass of
// blur type 2): the frame's half-size copy, taken just before, added back - dst += A0 x scale x max(0, copy - C0)
// (TEV stage 0: d TEXC minus C0 - a ZERO, b C0, c ONE or a C0, b c ZERO; blend SRCALPHA, ONE). The copy is none here (5.4), the draw was left out: the rooms 10-40 % darker than the
// GameCube's (tour55, 2026-11-09). The copy is the frame itself: f(x) = x + k max(0, x - b), the game's quad drawn
// again: c taken off (reverse subtract: below c nothing left), times 1 + k (a constant times DST_COLOR added, n passes
// for k > 1: (1 + a)^n = 1 + k), d added (or taken off). Exact above b; below it the error at most k b / (1 + 2 k)
// either way (c = 2 k b / (1 + 2 k), d = (1 + k) c - k b): the tones under b x(1+k) - kb crushed them (r112, k large:
// Leon black). The copy's slight blur is lost.
// (5.5) Stage 0 of the PICA a constant colour (0..1 each)
void contrastConst(const float* v)
{
    gTevGivenMask = 0;  // (X, finding 11: written here)
    C3D_TexEnv* e = C3D_GetTexEnv(0);
    C3D_TexEnvInit(e);
    C3D_TexEnvSrc(e, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
    u32 q[3];
    for (int c = 0; c < 3; c++) {
        q[c] = (u32) (fminf(fmaxf(v[c], 0.0f), 1.0f) * 255.0f + 0.5f);
    }
    C3D_TexEnvColor(e, 0xFF000000u | q[2] << 16 | q[1] << 8 | q[0]);
}

bool contrastBegin(const uint32_t* bp)
{
    const u32 env = bp[0xC0], cm = bp[0x41];
    const u32 in = env & 0xFFFF;  // TEXC - C0: the contrast (a ZERO, b C0, c ONE) or the glow (a C0, b c ZERO)
    if (!gContrastOn || ((bp[0x00] >> 10) & 15) != 0 || (in != 0xF2C8 && in != 0x2FF8) || !((env >> 18) & 1) ||
        ((env >> 16) & 3) != 0 || (cm & 0xFE3) != 0x421 || gUnits != 1 || gMaps[gUnitMap[0]]) {
        return false;
    }
    static const float kScale[4] = {1.0f, 2.0f, 4.0f, 0.5f};
    const float k = gTevReg[1][3] * kScale[(env >> 20) & 3];
    if (k <= 0.0f) {
        return false;
    }
    // passes of one picture reading the same copy (r101: the glow, then the contrast) add to the frame as it was at
    // the copy: dst = x (1 + K) - B so far, wanted x (1 + K + k) - B - k b: dst x m + B (m - 1) - k b, m = (1 + K + k) /
    // (1 + K)
    static u32 copyAt, copyFrame;
    static float sumK, sumB[3];
    const u32 at = (bp[mapRegister((u32) gUnitMap[0], 0x14)] & 0xFFFFFF) << 5;
    if (at != copyAt || gFrameNo != copyFrame) {
        copyAt = at;
        copyFrame = gFrameNo;
        sumK = sumB[0] = sumB[1] = sumB[2] = 0.0f;
    }
    // in the frame as it is now (dst = x (1 + K) - B): slope k / (1 + K), the bias at b (1 + K) - B
    const float mk = k / (1.0f + sumK);
    const int n = (int) ceilf(mk);
    gContrastPasses = n;
    gContrastMul = n == 1 ? mk : powf(1.0f + mk, 1.0f / (float) n) - 1.0f;
    float sub[3];
    for (int c = 0; c < 3; c++) {
        const float b = fmaxf(gTevReg[1][c] * (1.0f + sumK) - sumB[c], 0.0f);
        sub[c] = 2.0f * mk * b / (1.0f + 2.0f * mk);
        gContrastAdd[c] = (1.0f + mk) * sub[c] - mk * b;
        sumB[c] += k * gTevReg[1][c];
    }
    sumK += k;
    contrastConst(sub);
    gTevGivenMask = 0;  // (X, finding 11: written here)
    for (int i = 1; i < 6; i++) {
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    }
    C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ZERO, GPU_ONE);
    gCMode0 = 0xFFFFFFFFu;  // the blend set again at the next draw
    gTevDirty = true;       // and the combiners
    gUnits = 0;
    gContrastDraws++;
    return true;
}

// (5.5) The contrast draw's further passes, at the draw's end (its first took c off): times 1 + k, then d
__attribute__((noinline)) void contrastFinish()
{
    flushBatch();
    const float m[3] = {gContrastMul, gContrastMul, gContrastMul};
    contrastConst(m);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_DST_COLOR, GPU_ONE, GPU_ZERO, GPU_ONE);
    for (int i = 0; i < gContrastPasses; i++) {
        gBatch.open = true;
        flushBatch();
    }
    float plus[3], minus[3];
    bool anyPlus = false, anyMinus = false;
    for (int c = 0; c < 3; c++) {
        plus[c] = fmaxf(gContrastAdd[c], 0.0f);
        minus[c] = fmaxf(-gContrastAdd[c], 0.0f);
        anyPlus |= plus[c] * 255.0f >= 0.5f;
        anyMinus |= minus[c] * 255.0f >= 0.5f;
    }
    if (anyPlus) {
        contrastConst(plus);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ZERO, GPU_ONE);
        gBatch.open = true;
        flushBatch();
    }
    if (anyMinus) {
        contrastConst(minus);
        C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ZERO, GPU_ONE);
        gBatch.open = true;
        flushBatch();
    }
    gContrastPasses = 0;
}

// (stage 7.0) The combiners built: what they were built from
void tevBpSnap(const uint32_t* bp)
{
    tevBpGather(bp, gTevBpSnap);
    memcpy(gTevRegSnap, gTevReg, sizeof(gTevReg));
    memcpy(gTevRegSnap + 16, gKonst, sizeof(gKonst));
    gTevBpDirty = false;
}

// (stage 7.0) TEV registers written since: the combiners built again (gTevDirty) only where they changed
void tevBpResolve(const uint32_t* bp)
{
    if (!gTevBpDirty) {
        return;
    }
    gTevBpDirty = false;
    u32 w[49];
    tevBpGather(bp, w);
    if (memcmp(w, gTevBpSnap, sizeof(w)) || memcmp(gTevRegSnap, gTevReg, sizeof(gTevReg)) ||
        memcmp(gTevRegSnap + 16, gKonst, sizeof(gKonst))) {
        gTevDirty = true;
    } else {
        gJoinedTev++;
    }
}

// (stage 7.0) The inverse of a GX 3x4 position matrix (rows: x y z t); false where singular
bool invertAffine(const float* m, float* out)
{
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (fabsf(det) < 1e-20f) {
        return false;
    }
    const float r = 1.0f / det;
    out[0] = (e * i - f * h) * r;
    out[1] = (c * h - b * i) * r;
    out[2] = (b * f - c * e) * r;
    out[4] = (f * g - d * i) * r;
    out[5] = (a * i - c * g) * r;
    out[6] = (c * d - a * f) * r;
    out[8] = (d * h - e * g) * r;
    out[9] = (b * g - a * h) * r;
    out[10] = (a * e - b * d) * r;
    for (int k = 0; k < 3; k++) {
        out[4 * k + 3] = -(out[4 * k] * m[3] + out[4 * k + 1] * m[7] + out[4 * k + 2] * m[11]);
    }
    return true;
}

// (stage 7.0) A draw joining the open batch over the matrices written since its first draw: only the rows of its own
// position matrix, unlit, no matrix index in its vertices, no coordinate from the position or the normal (nor through
// a texture matrix in those rows), its vertices staged; its positions moved into the batch's first matrix, p' = M0^-1 Mi p
// (with face culling: only where that keeps the faces' turn)
bool joinMoved(const Re4GxDraw& d, const u8* v, const uint32_t* bp, const uint32_t* xfMem, const uint32_t* xfReg)
{
    const u32 pm = d.matrices[0] & 0xFF;
    if ((d.layout & RE4_GX_V_MATRIX) || gRest || pm + 2 >= (u32) kRows || (gXfDeferRows & ~(7ull << pm)) ||
        channelLights(xfReg) || v < gStage || v + d.vertexCount * d.stride > gStage + kStageBytes) {
        return false;
    }
    for (int u = 0; u < gUnits; u++) {
        if (gTexGen) {
            const u32 gen = xfReg[0x40 + gUnitCoord[u]], row = (gen >> 7) & 31;
            if (((gen >> 4) & 7) != 0 || row <= 4) {
                return false;  // (from the position, the normal, binormals; emboss, colours)
            }
        }
        if (gTexRow[u] < 60 && (gXfDeferRows & (7ull << gTexRow[u]))) {
            return false;
        }
    }
    if (!gXfDeferRows) {
        return true;  // (only normal matrices written: unlit, nothing to move)
    }
    if (!gBatchInvOk) {
        if (!invertAffine(gBatchM0, gBatchInv)) {
            return false;
        }
        gBatchInvOk = true;
    }
    float mi[12], t[12];
    for (int k = 0; k < 12; k++) {
        mi[k] = asFloat(xfMem[pm * 4 + k]);
    }
    const float* n = gBatchInv;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            t[4 * r + c] = n[4 * r] * mi[c] + n[4 * r + 1] * mi[4 + c] + n[4 * r + 2] * mi[8 + c] + (c == 3 ? n[4 * r + 3] : 0.0f);
        }
    }
    const float det = t[0] * (t[5] * t[10] - t[6] * t[9]) - t[1] * (t[4] * t[10] - t[6] * t[8]) +
                      t[2] * (t[4] * t[9] - t[5] * t[8]);
    if (((bp[0x00] >> 14) & 3) && !(det > 0.0f)) {
        return false;
    }
    for (u32 k = 0; k < d.vertexCount; k++) {
        float* p = (float*) (v + k * d.stride);
        const float x = p[0], y = p[1], z = p[2];
        p[0] = t[0] * x + t[1] * y + t[2] * z + t[3];
        p[1] = t[4] * x + t[5] * y + t[6] * z + t[7];
        p[2] = t[8] * x + t[9] * y + t[10] * z + t[11];
    }
    gJoinedXf++;
    return true;
}

void drawTimed(const Re4GxDraw* dp, const void* vertices, const uint16_t* indices, const uint32_t* bp,
               const uint32_t* xfMem, const uint32_t* xfReg)
{
    const Re4GxDraw& d = *dp;
    gLastBp = bp;
    const u32 primitive = d.primitive & 0xFF;
    const bool kept = d.primitive & RE4_GX_DIRECT_KEPT;
    if (primitive == RE4_GX_LINES && !kept && gLines && d.indexCount) {
        drawLines(d, vertices, indices, bp, xfMem, xfReg);
        return;
    }
    if ((primitive != RE4_GX_TRIANGLES && !(primitive == RE4_GX_TRIANGLE_STRIP && kept)) || d.indexCount == 0) {
        gStats.skipped++;
        return;
    }
    // (stage 7.0) a small list from the display list cache (the rain's drop: one cached quad drawn with each its
    // matrix) as a staged draw, its vertices and indices copied: it joins the batch as the others do
    if (kept && gSpriteJoin && primitive == RE4_GX_TRIANGLES && d.indexOffset == 0 && d.vertexCount <= kUnkeepVertices &&
        d.indexCount <= kUnkeepIndices && !(d.layout & RE4_GX_V_MATRIX) && !gRest && !inVram(vertices) &&
        !inVram(indices)) {
        bool inside = true;
        for (u32 i = 0; i < d.indexCount; i++) {
            inside &= indices[i] < d.vertexCount;
        }
        uint16_t* w = nullptr;
        u8* out = inside ? (u8*) stage(d.vertexCount * d.stride, d.indexCount, &w) : nullptr;
        if (out) {
            memcpy(out, vertices, d.vertexCount * d.stride);
            memcpy(w, indices, d.indexCount * sizeof(u16));
            Re4GxDraw c = d;
            c.primitive &= ~RE4_GX_DIRECT_KEPT;
            gUnkept++;
            drawTimed(&c, out, w, bp, xfMem, xfReg);
            return;
        }
    }
    Block* block = nullptr;
    if (kept) {  // not staged: the frame and the room for its commands here
        if (!gInFrame) {
            beginFrame();
        }
        if (gpuCmdBufSize - gpuCmdBufOffset < kCmdBufReserve) {
            split();
        }
        block = blockOf(indices);  // (after the frame's begin and the split: they free the blocks let go)
    }
    if (!rowsFit(d, vertices, block)) {
        gStats.rowsOut++;
        return;
    }
    if (kept) {
        if (gViewCullFrom && gFrameNo + 1 >= gViewCullFrom && !(d.layout & RE4_GX_V_MATRIX) &&
            outsideView(d, vertices, block, xfMem, xfReg)) {
            if (block) {
                sendBlock(*block);  // (moved: its room goes all the same, the box made)
            }
            gStats.viewCulled++;
            gStats.viewCulledIndices += d.indexCount;
            return;
        }
        gStats.keptDraws++;
        gStats.keptVertices += d.vertexCount;
        if (gVramFrom && gFrameNo + 1 >= gVramFrom) {
            fromVram(vertices, indices, d.vertexCount, block);
        }
        indices += d.indexOffset;  // (plan 5b: a piece of a kept strip; its block found by the whole's indices)
    }
    const u8* v = (const u8*) vertices;
    tevBpResolve(bp);  // (stage 7.0)
    if (gBatch.open && !kept && !gBatch.kept && gBatch.noCull == gNoCull && d.layout == gBatch.layout &&
        d.stride == gBatch.stride &&
        !memcmp(d.matrices, gBatch.matrices, sizeof(d.matrices)) && v >= gBatch.base &&
        (u32) (v - gBatch.base) % d.stride == 0 && !gTevDirty &&
        (u32) (v - gBatch.base) / d.stride + d.vertexCount <= 0x10000 &&
        (!(gXfDeferRows || gXfDeferOther) || joinMoved(d, v, bp, xfMem, xfReg))) {
        const u32 first = (u32) (v - gBatch.base) / d.stride;
        if (first + d.vertexCount <= 0x10000) {
            u16* out = gBatch.indices + gBatch.count;  // at or before the draw's own indices
            for (u32 i = 0; i < d.indexCount; i++) {
                out[i] = (u16) (indices[i] + first);
            }
            gBatch.count += d.indexCount;
            gBatch.vertexEnd = first + d.vertexCount;
            gStats.draws++;
            countVertices(d.vertexCount);
            return;
        }
    }
    flushBatch();
    // the face culling of GX (BP 0x00 bits 14-15: 1 the back faces, 2 the front ones, 3 all); the triangles
    // turn the other way here than in gx_gl.cpp (CCW in front there): with its faces Leon showed inside out
    // (none at first: the inner shells of Leon's hair showed through, 01.11.2026)
    setScissor(bp, xfReg);
    const u32 cull = gCullMode && !gNoCull ? (bp[0x00] >> 14) & 3 : 0;
    if (cull == 3) {
        gStats.culled++;
        return;
    }
    if (cull != gCull) {
        const bool back = (cull == 1) == (gCullMode == 2);
        C3D_CullFace(cull == 0 ? GPU_CULL_NONE : back ? GPU_CULL_BACK_CCW : GPU_CULL_FRONT_CCW);
        gCull = cull;
    }
    setBlendDepth(bp);
    if (gTevDirty) {
        gTevBuilt++;
        buildTev(bp);
        setAlphaTest(bp);
        gTevDirty = false;
        tevBpSnap(bp);  // (stage 7.0)
    }
    const bool contrast = contrastBegin(bp);  // (5.5)
    if (!contrast && gTevFill && gUnits == 0) {  // (stage 6.8)
        gCopyFillLeft++;
        gStats.untextured++;  // copies alone: left out, as before
        return;
    }
    gCopyFillDraws += gTevFill;
    // (2026-11-18) the models' refraction shader: a draw that reads another map too is drawn, the copy alone left out
    if (gUnits > 0 && gWaterRefract && gMaps[gUnitMap[0]] == &gWater) {
        if (gUnits < 2) {
            gRefractLeft++;
            gStats.untextured++;
            return;
        }
        gRefractWanted = gFrameNo;
        gRefractDraws++;
    } else if (gUnits > 0 && gRefractPendingMap >= 0 && gUnitMap[0] == gRefractPendingMap && !gMaps[gUnitMap[0]]) {
        if (gUnits >= 2) {
            gRefractWanted = gFrameNo;  // (its copy from the next picture on; this draw without it)
        }
        gRefractLeft++;
    }
    for (int u = 0; u < gUnits; u++) {
        if (!gMaps[gUnitMap[u]] || (gMaps[gUnitMap[u]] == &gWater && u != 0)) {
            gWaterOdd += gMaps[gUnitMap[u]] == &gWater;  // (the water's copy: unit 0 only, the projective one)
            gStats.untextured++;  // an EFB copy or a texture not made: left out
            return;
        }
    }
    for (int u = 0; u < gUnits; u++) {
        gMaps[gUnitMap[u]]->lastFrame = gFrameNo;  // (before any copy to VRAM: none of them stale for its room)
    }
    for (int u = 0; u < gUnits; u++) {
        Texture* tex = gMaps[gUnitMap[u]];
        const u32 wrapGx = bp[mapRegister((u32) gUnitMap[u], 0x00)] & 15;
        const u32 wrap = tex->turned ? ((wrapGx >> 2) & 3) | (wrapGx & 3) << 2 : wrapGx;  // (R4: s and t turned)
        if (tex == &gWater || tex->turned) {
            // (the water's copy, R4 a render target's: never to VRAM)
        } else if (tex == &gMovie) {
            movieUpdate();  // (stage 6.1: a new frame transferred; never to VRAM)
        } else if (!tex->vram && gTexVramFrom && gFrameNo + 1 >= gTexVramFrom && toVram(tex)) {
            forgetBound(tex);
        }
        const bool mipNear = gMipNearFrom && gFrameNo + 1 >= gMipNearFrom;
        if (gMipmaps && tex->mipNear != mipNear) {
            C3D_TexSetFilterMipmap(&tex->tex, mipNear ? GPU_NEAREST : GPU_LINEAR);
            tex->mipNear = mipNear;
            forgetBound(tex);
        }
        if (gTexLodFrom && !tex->biased && gFrameNo + 1 >= gTexLodFrom) {
            C3D_TexSetLodBias(&tex->tex, 1.0f);
            tex->biased = true;
            forgetBound(tex);
        }
        (tex->vram ? gTexBindVram : gTexBindLinear)++;
        if (gBound[u] != tex || gBoundWrap[u] != wrap) {
            C3D_TexSetWrap(&tex->tex, wrapOf(wrap & 3), wrapOf((wrap >> 2) & 3));
            C3D_TexBind(u, &tex->tex);
            gBound[u] = tex;
            gBoundWrap[u] = wrap;
        }
        tex->lastFrame = gFrameNo;
    }
    const bool vertexColor = (d.layout & RE4_GX_V_COLOR0) && channelFromVertex(xfReg);
    if (!setAttributes(d, vertices, vertexColor, channelLights(xfReg) != 0, xfMem, xfReg)) {
        gStats.skipped++;
        return;
    }
    bool water = false;
    // (2026-11-18) the copy next to a map whose coordinates come from the packed normal (the shader's divide mode:
    // r10c's and r11b's surfaces, the refraction shader's draws): down the shader's plain path as the lake's water -
    // tdiv off for it, the rows of the units from the normal times 1/127 below. The shader's tdiv branch has no water
    // mode: two edits of it hung the GPU (journal 2026-11-18). RE4_3DS_WATERDIV=0: as before (not projective)
    bool waterPlain = false;
    if (gUnits > 0 && gMaps[gUnitMap[0]] == &gWater) {
        static const bool divPlain = !getenv("RE4_3DS_WATERDIV") || atoi(getenv("RE4_3DS_WATERDIV")) != 0;
        waterPlain = gTexProj[0] && gTexDiv && divPlain;
        gWaterDiv += waterPlain;
        if (gTexProj[0] && (!gTexDiv || waterPlain)) {
            // the water's copy: s' = B_s T + A_s Q, t' = B_t S + A_t Q (GX: s = S/Q, t = T/Q over the copy); the PICA
            // divides by texcoord0w (the position's w = Q); rows put back at the next setTexMatrix
            const float* m = gTexMtx[0];
            float qr[4];
            for (int i = 0; i < 4; i++) {
                qr[i] = asFloat(xfMem[(gTexRow[0] + 2) * 4 + i]);
            }
            const float* f = gWaterMap;
            C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocTexSel, f[1] * m[4] + f[0] * qr[0], f[1] * m[5] + f[0] * qr[1],
                          f[1] * m[6] + f[0] * qr[2], f[1] * m[7] + f[0] * qr[3]);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocTexSel + 1, f[3] * m[0] + f[2] * qr[0], f[3] * m[1] + f[2] * qr[1],
                          f[3] * m[2] + f[2] * qr[2], f[3] * m[3] + f[2] * qr[3]);
            gTexMtx[0][0] = NAN;
            water = true;
            gWaterDraws++;
        } else {
            gWaterOdd++;
        }
    }
    setSwitch(kTexW0, water);
    for (int u = 0; u < gUnits; u++) {
        if (!gMaps[gUnitMap[u]]->turned) {
            continue;
        }
        if (gTexProj[u] && !gTexDiv) {
            gTexRenderOdd++;  // (R4: projective, not turned yet - left out, as before 0337: r120's car window)
            gStats.skipped++;
            return;
        }
        // (R4) a render target's copy: s' = 1 - t, t' = 1 - s (the input's w is 1; over the packed normal's w, w / w);
        // the rows put back at the next setTexMatrix
        const float* m = gTexMtx[u];
        C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocTexSel + 2 * u, -m[4], -m[5], -m[6], 1.0f - m[7]);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocTexSel + 2 * u + 1, -m[0], -m[1], -m[2], 1.0f - m[3]);
        gTexMtx[u][0] = NAN;
        gTexRenderDraws++;
    }
    if (waterPlain) {  // (2026-11-18) (m . nK + m3 K) / 127 = m . n + m3 for K = 127; put back at the next setTexMatrix
        for (int u = 1; u < gUnits; u++) {
            if (!gTexFromNrm[u] || gMaps[gUnitMap[u]]->turned) {
                continue;
            }
            const float* m = gTexMtx[u];
            const float s = 1.0f / 127.0f;
            C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocTexSel + 2 * u, m[0] * s, m[1] * s, m[2] * s, m[3] * s);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocTexSel + 2 * u + 1, m[4] * s, m[5] * s, m[6] * s, m[7] * s);
            gTexMtx[u][0] = NAN;
        }
    }
    setChannel(xfReg, vertexColor);
    setLights(xfReg, xfMem);
    setSwitch(kTexDiv, gTexDiv && !waterPlain);
    setSwitch(kTex1, gUnits > 1);
    setSwitch(kTex2, gUnits > 2);
    if (gProjDirty) {
        uploadProjection(xfReg);
    }
    setFog(bp, xfReg);
    if (contrast && gFogOn) {  // (5.5: the passes unfogged - the game's quad brightens the frame, r101's fog ate it)
        C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
        gFogOn = false;
    }
    if (gRowsMin <= gRowsMax) {
        uploadRows(xfMem);
    }
    const bool orthoZ = orthoDepth(d, vertices, bp, xfMem, xfReg);  // (5.5; after the projection's upload)
    gBatch.open = true;
    gBatch.layout = d.layout;
    gBatch.stride = d.stride;
    memcpy(gBatch.matrices, d.matrices, sizeof(d.matrices));
    gBatch.base = v;
    gBatch.vertexEnd = d.vertexCount;
    gBatch.indices = (u16*) indices;
    gBatch.count = d.indexCount;
    gBatch.kept = kept;
    gBatch.strip = primitive == RE4_GX_TRIANGLE_STRIP;
    gBatch.noCull = gNoCull;
    {
        const u32 pm = d.matrices[0] & 0xFF;  // (stage 7.0: the matrix a draw joining over a matrix is moved into)
        gBatchInvOk = false;
        for (int k = 0; pm + 2 < (u32) kRows && k < 12; k++) {
            gBatchM0[k] = asFloat(xfMem[pm * 4 + k]);
        }
    }
    if (orthoZ) {  // (5.5) its own depth: drawn now, the projection again at the next draw, no join to it
        flushBatch();
        gProjDirty = true;
        gTevDirty = true;
    }
    if (!gBatching) {
        flushBatch();
    }
    if (contrast) {
        contrastFinish();  // (5.5)
    }
    gStats.draws++;
    countVertices(d.vertexCount);
}

// An offset in eye space (x, y) into object space through the position matrix `m` (its 3x3 part solved)
bool objectOffset(const float* m, float x, float y, float* out)
{
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (fabsf(det) < 1e-20f) {
        return false;  // a model scaled to nothing (the knife put away)
    }
    const float r = 1.0f / det;
    out[0] = ((e * i - f * h) * x + (c * h - b * i) * y) * r;
    out[1] = ((f * g - d * i) * x + (a * i - c * g) * y) * r;
    out[2] = ((d * h - e * g) * x + (b * g - a * h) * y) * r;
    return true;
}

// GX lines (stage 6.2: the laser sight and the tracers, game/esp19.cpp, esp03/esp09; the item screen): PICA draws
// none, so each segment becomes a quad as wide on the screen as GX draws it (BP 0x22 bits 0-7: sixths of an EFB
// pixel, as gx_gl.cpp; at least a pixel of the top screen). Its ends move across the segment's direction on the
// screen, the offset taken back through the projection and the position matrix into object space (the shader and
// its uniforms stay); drawn as triangles without face culling. An end behind the eye is moved along the segment
// to just in front of it.
void drawLines(const Re4GxDraw& d, const void* vertices, const uint16_t* indices, const uint32_t* bp,
               const uint32_t* xfMem, const uint32_t* xfReg)
{
    static u8 src[65536];   // (2026-11-09: the rain's lines in one draw, patch 0330 - up to 4096 vertices of 16 bytes)
    static u16 ends[8192];
    const u32 segments = d.indexCount / 2;
    if (d.vertexCount * d.stride > sizeof(src) || segments * 2 > 8192 || segments * 4 > 0x10000) {
        gStats.skipped++;
        return;
    }
    memcpy(src, vertices, d.vertexCount * d.stride);  // stage() may split: the staged vertices go
    memcpy(ends, indices, segments * 2 * sizeof(u16));
    uint16_t* w = nullptr;
    u8* out = (u8*) stage(segments * 4 * d.stride, segments * 6, &w);
    if (!out) {
        return;
    }
    const float* p = (const float*) &xfReg[0x20];
    const bool persp = xfReg[0x26] == 0;
    const float half = fmaxf((float) (bp[0x22] & 0xFF) / 6.0f, 2.0f) * 0.5f;  // EFB pixels
    float vw = fabsf(asFloat(xfReg[0x1A])), vh = fabsf(asFloat(xfReg[0x1B]));  // the viewport's half size (EFB pixels)
    vw = vw > 0.0f ? vw : 256.0f;
    vh = vh > 0.0f ? vh : 224.0f;
    const float kNear = 0.1f;
    u32 made = 0;
    for (u32 s = 0; s < segments; s++) {
        const u8* v[2] = {src + ends[2 * s] * d.stride, src + ends[2 * s + 1] * d.stride};
        float pos[2][3], eye[2][3], clip[2][2], wv[2];
        const float* m[2];
        for (int k = 0; k < 2; k++) {
            const u32 row = (d.layout & RE4_GX_V_MATRIX) ? v[k][d.stride - 12] : d.matrices[0] & 0xFF;
            m[k] = (const float*) &xfMem[row * 4];
            memcpy(pos[k], v[k], sizeof(pos[k]));
        }
        bool behind = false;
        for (int pass = 0; pass < 2; pass++) {
            for (int k = 0; k < 2; k++) {
                const float* q = m[k];
                eye[k][0] = q[0] * pos[k][0] + q[1] * pos[k][1] + q[2] * pos[k][2] + q[3];
                eye[k][1] = q[4] * pos[k][0] + q[5] * pos[k][1] + q[6] * pos[k][2] + q[7];
                eye[k][2] = q[8] * pos[k][0] + q[9] * pos[k][1] + q[10] * pos[k][2] + q[11];
                wv[k] = persp ? -eye[k][2] : 1.0f;
                clip[k][0] = persp ? p[0] * eye[k][0] + p[1] * eye[k][2] : p[0] * eye[k][0] + p[1];
                clip[k][1] = persp ? p[2] * eye[k][1] + p[3] * eye[k][2] : p[2] * eye[k][1] + p[3];
            }
            if (wv[0] >= kNear && wv[1] >= kNear) {
                break;
            }
            if ((wv[0] < kNear && wv[1] < kNear) || pass == 1) {
                behind = true;
                break;
            }
            const int k = wv[0] < kNear ? 0 : 1;  // the end behind: along the segment (object space: affine)
            const float t = (kNear - wv[k]) / (wv[1 - k] - wv[k]) * 1.001f;
            for (int c = 0; c < 3; c++) {
                pos[k][c] += t * (pos[1 - k][c] - pos[k][c]);
            }
        }
        if (behind || p[0] == 0.0f || p[2] == 0.0f) {
            continue;
        }
        float dx = (clip[1][0] / wv[1] - clip[0][0] / wv[0]) * vw;
        float dy = (clip[1][1] / wv[1] - clip[0][1] / wv[0]) * vh;
        const float len = sqrtf(dx * dx + dy * dy);
        if (len > 1e-6f) {
            dx /= len;
            dy /= len;
        } else {
            dx = 1.0f;
            dy = 0.0f;
        }
        const float nx = -dy * half / vw, ny = dx * half / vh;  // across, in NDC
        u8* o = out + made * 4 * d.stride;
        bool ok = true;
        for (int k = 0; k < 2 && ok; k++) {
            float off[3];
            ok = objectOffset(m[k], nx * wv[k] / p[0], ny * wv[k] / p[2], off);
            for (int side = 0; side < 2 && ok; side++) {
                u8* t = o + (2 * k + side) * d.stride;
                memcpy(t, v[k], d.stride);
                const float sign = side ? -1.0f : 1.0f;
                float* tp = (float*) t;
                for (int c = 0; c < 3; c++) {
                    tp[c] = pos[k][c] + sign * off[c];
                }
            }
        }
        if (!ok) {
            continue;
        }
        const u16 b = (u16) (made * 4);
        u16* ix = w + made * 6;
        ix[0] = b;
        ix[1] = (u16) (b + 1);
        ix[2] = (u16) (b + 2);
        ix[3] = (u16) (b + 2);
        ix[4] = (u16) (b + 1);
        ix[5] = (u16) (b + 3);
        made++;
    }
    if (!made) {
        return;
    }
    Re4GxDraw t = d;
    t.primitive = (d.primitive & ~0xFFu) | RE4_GX_TRIANGLES;
    t.vertexOffset = 0;
    t.vertexCount = made * 4;
    t.indexOffset = 0;
    t.indexCount = made * 6;
    gLineDraws++;
    if (!gLineJoin) {
        flushBatch();
    }
    const u32 before = gStats.gpuDraws;
    const bool open = gBatch.open && gBatch.noCull;
    gNoCull = true;
    drawTimed(&t, out, w, bp, xfMem, xfReg);
    if (!gLineJoin) {
        flushBatch();
    }
    gNoCull = false;
    gLineJoined += open && gBatch.open && gStats.gpuDraws == before;
}

// (2026-11-19, RE4_3DS_GXSHADOW=2) The renderer's own state against the decoder's at a draw
void ownCheck(const uint32_t* bp, const uint32_t* xfMem, const uint32_t* xfReg)
{
    gOwnChecked++;
    auto first = [](const u32* own, const u32* theirs, u32 n, u32 base) {
        for (u32 i = 0; i < n; i++) {
            if (own[i] != theirs[i]) {
                if (gOwnFirstAt == ~0u) {
                    gOwnFirstAt = base + i, gOwnFirstOwn = own[i], gOwnFirstTheirs = theirs[i];
                }
                return;
            }
        }
    };
    if (memcmp(gOwnBp, bp, sizeof(gOwnBp))) {
        gOwnDiffBp++;
        first(gOwnBp, bp, 0x100, 0x20000);  // (BP: 0x20000 + the register)
    }
    if (memcmp(gOwnXfMem, xfMem, sizeof(gOwnXfMem))) {
        gOwnDiffXfMem++;
        first(gOwnXfMem, xfMem, 0x800, 0);
    }
    if (memcmp(gOwnXfReg, xfReg, sizeof(gOwnXfReg))) {
        gOwnDiffXfReg++;
        first(gOwnXfReg, xfReg, 0x58, 0x1000);
    }
}

void draw(const Re4GxDraw* dp, const void* vertices, const uint16_t* indices, const uint32_t* bp,
          const uint32_t* xfMem, const uint32_t* xfReg)
{
    // (X, finding 8) every re4_gx_timer_every-th draw timed, times that (gx_decode.cpp PartTimer)
    static u32 timedCalls;
    const u64 from = (++timedCalls & (re4_gx_timer_every - 1)) == 0 ? svcGetSystemTick() : 0;
    if (gOwnState) {  // (2026-11-19) the renderer's own state for the decoder's
        if (gOwnState == 2) {
            ownCheck(bp, xfMem, xfReg);
        }
        bp = gOwnBp, xfMem = gOwnXfMem, xfReg = gOwnXfReg;
    }
    drawTimed(dp, vertices, indices, bp, xfMem, xfReg);
    if (from) {
        gStats.drawTicks += (svcGetSystemTick() - from) * re4_gx_timer_every;
    }
}

void drawSplit(const Re4GxDraw* dp, const void* vertices, const void* rest, const float* box, const uint16_t* indices,
               const uint32_t* bp, const uint32_t* xfMem, const uint32_t* xfReg)
{
    gRest = (const u8*) rest;
    gBox = box;
    draw(dp, vertices, indices, bp, xfMem, xfReg);
    gRest = nullptr;
    gBox = nullptr;
}

// A display copy: the frame shown.
void frame(uint32_t xfb, uint32_t width, uint32_t height)
{
    (void) xfb;
    (void) width;
    (void) height;
    if (!gInFrame) {
        return;
    }
    if (width >= 64 && width <= 1024 && height >= 64 && height <= 1024) {
        gPicW = width;  // the next picture's place
        gPicH = height;
    }
    const u64 from = svcGetSystemTick();
    flushBatch();
    if (gPipe) {
        pipeEnd();
    } else {
        C3D_RenderTargetSetOutput(gTarget, GFX_TOP, GFX_LEFT, kDisplayTransfer);
        gStats.cmdWords += gpuCmdBufOffset;
        frameEnd();
    }
    gInFrame = false;
    gOccCopied = false;  // (occlusion cull)
    gWaterLive = false;  // (the water's copy read in its picture only)
    gWaterRefract = false;  // (2026-11-18)
    gRefractPending = 0;
    gRefractPendingMap = -1;
    if (gWater.tex.data && gWaterFrame + 600 < gFrameNo && gWater.lastFrame + 600 < gFrameNo) {
        forgetBound(&gWater);  // (the water's copy: its memory back)
        linearFree(gWater.tex.data);
        gWater = Texture();
        gWaterDest = 0;
    }
    texRenderUnplace();  // (R4: a pass left open)
    for (auto it = gTexRenders.begin(); it != gTexRenders.end();) {
        if (it->second->lastFrame + 600 < gFrameNo) {  // (R4: a render target's copy, neither made nor read)
            texRenderDrop(it->second);
            it = gTexRenders.erase(it);
        } else {
            ++it;
        }
    }
    gFrameNo++;
    gPreFrameEnd = svcGetSystemTick();  // (stage98: the pace of textures ahead; the picture's GX work without them)
    {
        const u64 busyNow = re4_hud_gx_busy - gPreAllTicks;
        gPreBusyPrev = (s64) (busyNow - gPreBusyMark) > 0 ? busyNow - gPreBusyMark : 0;  // (the busy line: a game frame's)
        gPreBusyMark = busyNow;
    }
    gStats.frames++;
    gStats.ticks += svcGetSystemTick() - from;
}

Re4GxDirect gDirect = {stage, draw,      xfChanged, ownerTag, frame, texture,
                       bpWritten, keep, drop,      drawSplit, kept,  later};

}  // namespace

// Before the game runs (main_3ds.cpp, RE4_3DS_GX=1): citro3d, the shader, the staging area.
extern "C" int re4_3ds_gx_init(void)
{
    if (!C3D_Init(kCmdBufBytes)) {
        return 0;
    }
    {
        gxCmdQueue_s& q = __C3D_Context;
        gxCmdEntry_s* entries = (gxCmdEntry_s*) malloc(kQueueEntries * sizeof(gxCmdEntry_s));
        if (entries && q.numEntries == 0) {
            free(q.entries);
            q.entries = entries;
            q.maxEntries = kQueueEntries;
        } else {
            free(entries);
        }
    }
    gTarget = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    gStage = (u8*) linearAlloc(kStageBytes + 256);  // a coordinate's own buffer reads past the last vertex
    gIndices = (u16*) linearAlloc(kStageIndices * 2);
    if (!gTarget || !gStage || !gIndices) {
        return 0;
    }
    gDvlb = DVLB_ParseFile((u32*) gx_3ds_shbin, gx_3ds_shbin_size);
    shaderProgramInit(&gProgram);
    shaderProgramSetVsh(&gProgram, &gDvlb->DVLE[0]);
    gLocRows = shaderInstanceGetUniformLocation(gProgram.vertexShader, "rows");
    gLocProj = shaderInstanceGetUniformLocation(gProgram.vertexShader, "proj");
    gLocColor = shaderInstanceGetUniformLocation(gProgram.vertexShader, "chan");
    gLocTexSel = shaderInstanceGetUniformLocation(gProgram.vertexShader, "tmtx");
    gLocLights = shaderInstanceGetUniformLocation(gProgram.vertexShader, "lights");
    gLocConst = shaderInstanceGetUniformLocation(gProgram.vertexShader, "k");
    if (gLocRows < 0 || gLocProj < 0 || gLocColor < 0 || gLocTexSel < 0 || gLocLights < 0 || gLocConst < 0) {
        re4_log("3ds: a shader uniform not found (rows %d proj %d chan %d tmtx %d lights %d k %d)", gLocRows, gLocProj,
                gLocColor, gLocTexSel, gLocLights, gLocConst);
        return 0;
    }
    // bool uniforms: registers 0x78..0x87, located less 0x10 as all uniforms (0x68 + n: C3D_BoolUnifSet takes
    // that and subtracts; given b0 it set nothing, every light stayed off, 01.11.2026); the loops' 0x70..0x73
    // at 0x60 + n (C3D_IVUnifSet subtracts that)
    gLocFar = shaderInstanceGetUniformLocation(gProgram.vertexShader, "nfar");
    gLocBrief = shaderInstanceGetUniformLocation(gProgram.vertexShader, "nbrief");
    gLocWhole = shaderInstanceGetUniformLocation(gProgram.vertexShader, "nwhole");
    bool found = gLocFar >= 0 && gLocBrief >= 0 && gLocWhole >= 0;
    for (int i = 0; i < kSwitches; i++) {
        gSwitchLoc[i] = shaderInstanceGetUniformLocation(gProgram.vertexShader, kSwitchName[i]);
        found = found && gSwitchLoc[i] >= 0;
    }
    if (!found) {
        re4_log("3ds: a shader switch or loop not found (nbrief %d nwhole %d lit %d)", gLocBrief, gLocWhole,
                gSwitchLoc[kLit]);
        return 0;
    }
    C3D_BindProgram(&gProgram);
    gUnifBound[0] = gUnifBound[1] = false;
    gTevGivenMask = 0;  // (X, finding 11: written here)
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    const char* mip = getenv("RE4_3DS_MIPMAP");
    const char* gen = getenv("RE4_3DS_TEXGEN");
    gMipmaps = !mip || *mip != '0';
    gTexGen = !gen || *gen != '0';
    const char* batch = getenv("RE4_3DS_BATCH");
    gBatching = !batch || *batch != '0';
    const char* farPos = getenv("RE4_3DS_FARPOS");  // (5.5)
    gFarPos = !farPos || *farPos != '0';
    const char* farLight = getenv("RE4_3DS_FARLIGHT");
    gFarLight = farLight ? (float) atof(farLight) : 50000.0f;
    const char* farFrom = getenv("RE4_3DS_FARFROM");
    gFarFrom = farFrom ? (u32) atoi(farFrom) : 0;
    const char* lights = getenv("RE4_3DS_LIGHTS");
    gLighting = !lights || *lights != '0';
    const char* cullMode = getenv("RE4_3DS_CULL");
    gCullMode = cullMode ? atoi(cullMode) : 1;
    if (const char* lines = getenv("RE4_3DS_LINES")) {
        gLines = *lines != '0';
    }
    if (const char* lineJoin = getenv("RE4_3DS_LINEJOIN")) {
        gLineJoin = *lineJoin != '0';
    }
    const char* fog = getenv("RE4_3DS_FOG");
    gFogMode = fog ? atoi(fog) : 2;
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    if (gFogMode == 2) {
        C3D_DepthMap(true, 1.0f, 1.0f);
    }
    const char* test = getenv("RE4_3DS_GPUTEST");
    gGpuTest = test ? atoi(test) : 0;
    const char* overlap = getenv("RE4_3DS_OVERLAP");
    gOverlap = !overlap || *overlap != '0';
    const char* keptVram = getenv("RE4_3DS_KEPTVRAM");
    gVramFrom = keptVram ? (u32) atoi(keptVram) : 1;
    const char* texVram = getenv("RE4_3DS_TEXVRAM");
    gTexVramFrom = texVram ? (u32) atoi(texVram) : 1;
    const char* keepLinear = getenv("RE4_3DS_TEXKEEPLINEAR");
    gTexKeepLinear = keepLinear && keepLinear[0] == '1';
    const char* texNative = getenv("RE4_3DS_TEXNATIVE");
    gTexNativeFrom = texNative ? (u32) atoi(texNative) : 1;
    gTexNative = gTexNativeFrom == 1;
    if (const char* steps = getenv("RE4_3DS_TEXSTEPS")) {
        unsigned from, mask;
        int used;
        while (gTexSteps < 8 && sscanf(steps, "%u:%u%n", &from, &mask, &used) == 2) {
            gTexStepFrom[gTexSteps] = from;
            gTexStepMask[gTexSteps++] = mask;
            steps += used;
            steps += *steps == ',';
        }
        gTexNative = true;
        gTexNativeFrom = 1;
    }
    const char* etcLog = getenv("RE4_3DS_ETC1LOG");
    gEtcLog = etcLog && *etcLog == '1';
    const char* texEvict = getenv("RE4_3DS_TEXEVICT");
    gTexEvictFrom = texEvict ? (u32) atoi(texEvict) : 1;
    if (const char* idleKick = getenv("RE4_3DS_IDLEKICK")) {
        gIdleKickFrom = (u32) atoi(idleKick);
    }
    if (const char* flushSvc = getenv("RE4_3DS_FLUSHSVC")) {
        gFlushSvcFrom = (u32) atoi(flushSvc);
    }
    if (const char* unifSkip = getenv("RE4_3DS_UNIFSKIP")) {
        gUnifSkipFrom = (u32) atoi(unifSkip);
    }
    if (const char* bufLean = getenv("RE4_3DS_BUFLEAN")) {
        gBufLeanFrom = (u32) atoi(bufLean);
    }
    if (const char* unifSkip = getenv("RE4_3DS_UNIFSKIP")) {
        gUnifSkipFrom = (u32) atoi(unifSkip);
    }
    if (const char* bufLean = getenv("RE4_3DS_BUFLEAN")) {
        gBufLeanFrom = (u32) atoi(bufLean);
    }
    const char* viewCull = getenv("RE4_3DS_VIEWCULL");
    gViewCullFrom = viewCull ? (u32) atoi(viewCull) : 1;
    if (const char* flushStore = getenv("RE4_3DS_FLUSHSTORE")) {
        gFlushStoreFrom = (u32) atoi(flushStore);
    }
    if (const char* kickEvery = getenv("RE4_3DS_KICKEVERY")) {
        gKickEvery = (u32) atoi(kickEvery);
    }
    if (const char* kicksMax = getenv("RE4_3DS_KICKSMAX")) {
        gKicksMax = (u32) atoi(kicksMax);
    }
    if (const char* pipe = getenv("RE4_3DS_PIPE")) {
        gPipeFrom = (u32) atoi(pipe);
    }
    gQueues[0] = &__C3D_Context;
    gStageBufs[0] = gStage;
    gIndexBufs[0] = gIndices;
    if (gPipeFrom) {  // (the pipeline) the second queue, command buffer and staging area
        gQueue1.entries = (gxCmdEntry_s*) malloc(kQueueEntries * sizeof(gxCmdEntry_s));
        gQueue1.maxEntries = kQueueEntries;
        gCmdBufs[1] = (u32*) linearAlloc(kCmdBufBytes);
        gStageBufs[1] = (u8*) linearAlloc(kStageBytes + 256);
        gIndexBufs[1] = (u16*) linearAlloc(kStageIndices * 2);
        if (gQueue1.entries && gCmdBufs[1] && gStageBufs[1] && gIndexBufs[1]) {
            gQueues[1] = &gQueue1;
            gC3dQueueDone = __C3D_Context.callback;
            gxCmdQueueSetCallback(&gQueue1, queueDone, nullptr);
            gxCmdQueueSetCallback(&__C3D_Context, queueDone, nullptr);
        }
        re4_log("3ds: pictures in a pipeline (RE4_3DS_PIPE) from picture %u%s", (unsigned) gPipeFrom,
                gQueues[1] ? "" : ": no memory for it, off");
    }
    if (const char* drawOwn = getenv("RE4_3DS_DRAWOWN")) {
        gDrawOwnFrom = (u32) atoi(drawOwn);
    }
    if (const char* attrFastEnv = getenv("RE4_3DS_ATTRFAST")) {  // (2026-11-19)
        gAttrFast = (u32) atoi(attrFastEnv);
    }
    if (const char* fold = getenv("RE4_3DS_TEVFOLD")) {  // (stage 6.8)
        gTevFoldOn = *fold != '0';
    }
    if (const char* scale = getenv("RE4_3DS_TEVSCALE")) {  // (stage 6.8)
        gTevScaleOn = *scale != '0';
    }
    if (const char* aspect = getenv("RE4_3DS_ASPECT")) {
        gAspect = atoi(aspect);
    }
    if (const char* fill = getenv("RE4_3DS_FILL")) {  // (2026-11-19; the settings screen sets it at the gameplay)
        gFill = atoi(fill) < 0 || atoi(fill) > 2 ? 0 : atoi(fill);
    }
    if (const char* water = getenv("RE4_3DS_WATER")) {  // (the water's copy)
        gWaterOn = *water != '0';
    }
    if (const char* occ = getenv("RE4_3DS_OCC")) {  // (occlusion cull)
        gOccOn = atoi(occ);
    }
    if (const char* at = getenv("RE4_3DS_OCC_COPYAT")) {
        gOccCopyAt = (int) strtol(at, nullptr, 0);
    }
    for (int b = 0; b < 2 && gOccOn; b++) {
        gOccBuf[b] = (u8*) linearMemAlign(240 * 400 * 4, 0x80);
        gOccOn = gOccBuf[b] != nullptr;
    }
    re4_occ_on = gOccOn;
    re4_log("3ds: occlusion cull %s (RE4_3DS_OCC; the depth before table %d)",
            gOccOn ? "on" : getenv("RE4_3DS_OCC") && !atoi(getenv("RE4_3DS_OCC")) ? "off" : "off (no linear memory)",
            gOccCopyAt);
    if (const char* texRender = getenv("RE4_3DS_TEXRENDER")) {  // (R4)
        gTexRenderOn = *texRender != '0';
    }
    if (const char* fill = getenv("RE4_3DS_COPYWHITE")) {  // (stage 6.8)
        gCopyFillOn = *fill != '0';
    }
    if (const char* contrast = getenv("RE4_3DS_CONTRAST")) {  // (5.5)
        gContrastOn = *contrast == '1';
    }
    if (const char* ortho = getenv("RE4_3DS_ORTHODEPTH")) {  // (5.5)
        gOrthoDepthOn = *ortho != '0';
    }
    if (const char* keep = getenv("RE4_3DS_BANDKEEP")) {
        gBandKeep = atoi(keep) != 0;
    }
    if (const char* join = getenv("RE4_3DS_SPRITEJOIN")) {  // (stage 7.0)
        gSpriteJoin = *join != '0';
    }
    if (const char* vk = getenv("RE4_3DS_VRAMKEPT")) {  // (X, finding 4)
        gVramKept = atoi(vk);
    }
    re4_log("3ds: VRAM's free total kept between allocs (RE4_3DS_VRAMKEPT) %d", gVramKept);
    if (const char* tc = getenv("RE4_3DS_TEVCACHE")) {  // (the TEV cache)
        gTevCacheOn = *tc != '0';
    }
    if (const char* tg = getenv("RE4_3DS_TEVGIVEN")) {  // (X, finding 11)
        gTevGivenOn = *tg != '0';
    }
    if (const char* pack = getenv("RE4_3DS_TEVPACK")) {  // (stage 6.9)
        gTevPack = *pack != '0';
    }
    if (const char* clear = getenv("RE4_3DS_COPYCLEAR")) {
        gClearFrom = (u32) atoi(clear);
    }
    const char* scissor = getenv("RE4_3DS_SCISSOR");
    gScissorFrom = scissor ? (u32) atoi(scissor) : 1;
    const char* texLod = getenv("RE4_3DS_TEXLOD");
    gTexLodFrom = texLod ? (u32) atoi(texLod) : 0;
    const char* busy = getenv("RE4_3DS_GPUBUSY");
    if (!busy || *busy != '0') {
        u32 share = 0;  // (2026-11-18) not below what the sound took (RE4_3DS_AUDIO_CORE1, 50 %)
        if (R_FAILED(APT_GetAppCpuTimeLimit(&share)) || share < 30) {
            APT_SetAppCpuTimeLimit(30);
        }
        gBusyOn = threadCreate(busySampler, nullptr, 4096, 0x18, 1, true) != nullptr;
        gBusyFrom = svcGetSystemTick();
        re4_log("3ds: GPU busy sampler %s", gBusyOn ? "on core 1" : "did not start (no thread on core 1)");
    }
    const char* cache = getenv("RE4_3DS_DLCACHE");
    gKeeping = !cache || *cache != '0';
    if (!gKeeping) {
        gDirect.keep = nullptr;
    }
    const char* move = getenv("RE4_3DS_KEPTMOVE");
    gKeptMove = !move || *move != '0';
    const char* arenaKb = getenv("RE4_3DS_KEEPARENAKB");
    kArenaBytes = arenaKb ? (u32) atoi(arenaKb) << 10 : gKeptMove ? kArenaMoved : kArenaBytes;
    const char* keepDrop = getenv("RE4_3DS_KEEPDROP");
    gKeepDrop = !keepDrop || *keepDrop != '0';
    const char* arena = getenv("RE4_3DS_KEEPARENA");
    if (gKeeping && (!arena || *arena != '0')) {
        gArena = (u8*) linearMemAlign(kArenaBytes, kArenaAlign);
        if (gArena) {
            gArenaFree[0] = kArenaBytes;
        }
        re4_log("3ds: display list cache in its own room: %s (%u KB)", gArena ? "yes" : "no room", kArenaBytes >> 10);
    }
    const char* grey = getenv("RE4_3DS_TEXGREY");
    if (grey && *grey == '0') {
        gTexMask &= ~32u;
    }
    if (const char* mipNear = getenv("RE4_3DS_MIPNEAR")) {
        gMipNearFrom = (u32) atoi(mipNear);
    }
    re4_log("3ds: the mipmap filter GPU_NEAREST from picture %u (0: never, GPU_LINEAR)", gMipNearFrom);
    const char* pak = getenv("RE4_3DS_TEXPAK");
    if (!pak || *pak != '0') {
        openPak(pak ? pak : "/3ds/pueblo/textures.pak");
        openPreList();  // (2026-11-24) textures ahead
    }
    if (const char* formats = getenv("RE4_3DS_TEXPAKFORMATS")) {
        gPakFormats = (u32) strtoul(formats, nullptr, 0);
    }
    const char* miss = getenv("RE4_3DS_TEXMISS");
    if (miss && *miss == '1') {
        gMissFile = fopen(kMissPath, "ab");
        if (const char* buf = getenv("RE4_3DS_MISSBUF")) {
            gMissBuf = atoi(buf) != 0;
        }
        if (gMissFile && gMissBuf) {
            setvbuf(gMissFile, nullptr, _IOFBF, kMissBuffer);
        }
        re4_log("3ds: textures not in the pack to %s%s", kMissPath, gMissFile ? "" : ": not opened");
    }
    gOn = true;
    if (const char* own = getenv("RE4_3DS_GXSHADOW")) {  // (2026-11-19)
        gOwnState = atoi(own);
    }
    re4_log("3ds: GX renderer: its own BP / XF state (RE4_3DS_GXSHADOW) %d", gOwnState);
    re4_gx_set_direct(&gDirect);
    re4_log("3ds: GX renderer on: staging %u KB vertices, %u KB indices, command buffer %u KB; mipmaps %d, texgen %d, batching %d, "
            "GPU test %d, display list cache %d, CPU and GPU together %d, lights %d (switches from b%d)",
            kStageBytes >> 10, kStageIndices * 2 >> 10, kCmdBufBytes >> 10, gMipmaps, gTexGen, gBatching, gGpuTest, gKeeping,
            gOverlap, gLighting, gSwitchLoc[kLit] - 0x68);
    return 1;
}

// The GX thread has nothing more to do (gx_thread_3ds.cpp, before it sleeps): the draws since the last kick to the GPU
// (plan 2d) Room for a movie (movie_3ds.cpp; on the GX thread, in its place among the records): the textures with
// texels in linear memory not used in this picture go, the longest unused first, until `largest` bytes are free in one
// piece and `total` in all (after r100 and r10b their textures left 3.4 MB: the CRI player, an orange screen)
extern "C" void re4_3ds_gx_movie_room(uint32_t largest, uint32_t total)
{
    const u32 before = linearSpaceFree(), largestBefore = linearLargest();
    u32 dropped = 0;
    while ((linearSpaceFree() < total || linearLargest() < largest) && dropOldest(1, true)) {
        dropped++;
    }
    re4_log("3ds: room for a movie (%lu KB in one piece, %lu KB in all): %lu textures dropped, linear free %lu -> %lu KB "
            "(the largest piece %lu -> %lu KB)",
            (unsigned long) largest >> 10, (unsigned long) total >> 10, (unsigned long) dropped,
            (unsigned long) before >> 10, (unsigned long) linearSpaceFree() >> 10, (unsigned long) largestBefore >> 10,
            (unsigned long) linearLargest() >> 10);
}

// HOME (stage 6.2c; gx_thread_3ds.cpp re4_gxq_park, on the renderer's thread). away: the GPU given up — what of the
// open picture is not with the GPU yet goes to it, then both GX queues through (citro3d's APT hook waits only while
// libctru runs a queue: the pipeline's other one could hold entries), then (RE4_3DS_HOMENEUTRAL, 1 by default) a list of
// a neutral state (OpenNectar-3DS, docs/15 item 1: the HOME menu takes the registers as the app left them); VRAM on
// the log; RE4_3DS_HOMETEX=1: the textures with a copy in VRAM deleted (made again after the return). Back: citro3d's
// hook stopped libctru's queue (gxCmdQueueStop) — the current one made to run again, else nothing more reaches the
// GPU; the renderer's knowledge of the registers as at a picture's begin (the HOME menu wrote them all).
// (R1b) The end, on the GX thread after re4_3ds_gx_home(1) (RE4_3DS_EXITMODE=3): citro3d closed.
extern "C" void re4_3ds_gx_quit(void)
{
    if (!gOn) {
        return;
    }
    gOn = false;
    C3D_Fini();
}

extern "C" void re4_3ds_gx_home(int away)
{
    if (!gOn) {
        return;
    }
    static int neutral = -1, texDrop = 0;
    if (neutral < 0) {
        const char* n = getenv("RE4_3DS_HOMENEUTRAL");
        neutral = !(n && *n == '0');
        const char* t = getenv("RE4_3DS_HOMETEX");
        texDrop = t && *t == '1';
    }
    if (!away) {
        if (gPipe) {
            gxCmdQueueRun(gQueues[gRun]);
        }
        gUnifBound[0] = gUnifBound[1] = false;
        gAttrKey = 0xFFFFFFFF;
        gBufKnown = false;
        memset(gFixedKnown, 0, sizeof(gFixedKnown));
        memset(gBound, 0, sizeof(gBound));
        gColorKnown = false;
        memset(gSlotLight, 0xFF, sizeof(gSlotLight));
        memset(gSwitch, 0xFF, sizeof(gSwitch));
        gTevDirty = true;
        gTevGivenMask = 0;  // (X, finding 11: the GPU given back - all of them again)
        gZMode = gCMode0 = 0xFFFFFFFF;
        gProjDirty = true;
        gScissorKey[0] = ~0u;
        gFogKey[0] = 0xFFFFFFFF;
        gFogBound = nullptr;
        if (gInFrame) {
            C3D_FrameDrawOn(gTarget);
            if (gGpuTest == 1) {
                C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, 1, 1);
            }
            C3D_BindProgram(&gProgram);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocConst, 1.0f, 0.0f, 1.0e-12f, 0.0f);
        }
        re4_log("3ds: HOME: the renderer back (a picture open: %d, the pipeline %d)", gInFrame ? 1 : 0, gPipe ? 1 : 0);
        return;
    }
    const u64 from = svcGetSystemTick();
    auto drain = [&](const char* what) {
        for (;;) {
            if (gPipe) {
                pump();
            }
            const gxCmdQueue_s& q = gPipe ? *gQueues[gCur] : __C3D_Context;
            const bool done = (!gPipe || (gRun == gCur && !gPresentAt[0] && !gPresentAt[1])) &&
                              q.lastEntry >= q.numEntries && gxCmdQueueWait(gQueue, 0);
            if (done) {
                return true;
            }
            if (svcGetSystemTick() - from > 2ull * SYSCLOCK_ARM11) {
                const gxCmdQueue_s& a = gQueues[0] ? *gQueues[0] : __C3D_Context;
                const gxCmdQueue_s& b = gQueues[1] ? *gQueues[1] : a;
                re4_log("3ds: HOME: the GPU not done after 2 s (%s): queues %u/%u entries, %u/%u done; current %u, "
                        "running %u, to show at %u/%u", what, (unsigned) a.numEntries, (unsigned) b.numEntries,
                        (unsigned) a.lastEntry, (unsigned) b.lastEntry, (unsigned) gCur, (unsigned) gRun,
                        (unsigned) gPresentAt[0], (unsigned) gPresentAt[1]);
                return false;
            }
            svcSleepThread(100000);
        }
    };
    if (gInFrame) {
        flushBatch();
        if (gpuCmdBufOffset && (gPipe || gOverlap)) {
            flushLinear();
            C3D_FrameSplit(GX_CMDLIST_FLUSH);
            if (!gPipe) {
                gxCmdQueueRun(&__C3D_Context);
            }
        }
    }
    bool ok = drain("the app's commands");
    bool neutralSent = false;
    if (ok && neutral && gPipe) {  // (citro3d stays in its frame in the pipeline: a list can go at any time)
        gxCmdQueueClear(gQueue);    // (libctru's running one, idle: what is added starts by itself)
        const u32 w = gTarget->frameBuf.width, h = gTarget->frameBuf.height;
        GPUCMD_AddWrite(GPUREG_EARLYDEPTH_TEST1, 0);
        GPUCMD_AddWrite(GPUREG_EARLYDEPTH_TEST2, 0);
        GPUCMD_AddWrite(GPUREG_FACECULLING_CONFIG, 0);
        GPUCMD_AddWrite(GPUREG_SCISSORTEST_MODE, GPU_SCISSOR_NORMAL);
        GPUCMD_AddWrite(GPUREG_SCISSORTEST_POS, 0);
        GPUCMD_AddWrite(GPUREG_SCISSORTEST_DIM, ((h - 1) << 16) | (w - 1));
        GPUCMD_AddWrite(GPUREG_VSH_BOOLUNIFORM, 0x7FFF0000);
        GPUCMD_AddWrite(GPUREG_TEXUNIT_CONFIG, BIT(12));  // (units and proctex off)
        GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0x4, BIT(16));  // (the texture cache cleared)
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
        C3D_FrameSplit(GX_CMDLIST_FLUSH);
        ok = drain("the neutral state");
        neutralSent = ok;
        gxCmdQueueClear(gQueue);
    }
    u32 texN = 0, texBytes = 0, lo = ~0u, hi = 0;
    for (auto& e : gTextures) {
        if (e.second->vram) {
            const u32 a = (u32) (uintptr_t) e.second->vram;
            texN++;
            texBytes += e.second->bytes;
            lo = a < lo ? a : lo;
            hi = a + e.second->bytes > hi ? a + e.second->bytes : hi;
        }
    }
    u32 blockN = 0, blockBytes = 0;
    for (auto& e : gBlocks) {
        if (e.second.vram && !e.second.pending) {
            const u32 a = (u32) (uintptr_t) e.second.vram;
            blockN++;
            blockBytes += e.second.bytes;
            lo = a < lo ? a : lo;
            hi = a + e.second.bytes > hi ? a + e.second.bytes : hi;
        }
    }
    u32 dropped = 0;
    if (texDrop && ok) {
        std::vector<u64> keys;
        for (auto& e : gTextures) {
            if (e.second->vram) {
                keys.push_back(e.first);
            }
        }
        for (u64 k : keys) {
            deleteTexture(k, gTextures[k]);
            dropped++;
        }
    }
    re4_log("3ds: HOME: the GPU done (%d) in %.1f ms, a neutral state %d (RE4_3DS_HOMENEUTRAL), a picture open %d; VRAM: "
            "%u textures %u KB, %u lists %u KB, from 0x%08lx to 0x%08lx; free %lu KB; %u textures deleted (RE4_3DS_HOMETEX)",
            ok ? 1 : 0, (double) (svcGetSystemTick() - from) / CPU_TICKS_PER_MSEC, neutralSent ? 1 : 0, gInFrame ? 1 : 0,
            texN, texBytes >> 10, blockN, blockBytes >> 10, (unsigned long) (lo == ~0u ? 0 : lo), (unsigned long) hi,
            (unsigned long) vramSpaceFree() >> 10, dropped);
}

// (2026-11-19; setup_3ds.cpp take, the game's thread as the gameplay starts) the settings screen's "Picture": 0 Fit,
// 1 Stretch, 2 Zoom (placePicture: from the next picture)
extern "C" void re4_3ds_gx_fill(int mode)
{
    gFill = mode < 0 || mode > 2 ? 0 : mode;
    re4_log("3ds: picture: %s (the settings screen)", gFill == 1 ? "stretched to the screen" : gFill == 2 ? "zoomed to the "
            "screen, the sides cut" : "fitted");
}

extern "C" void re4_3ds_gx_idle(void)
{
    if (gOn && gPipe) {
        pump();  // (between pictures too: the picture built waits for the queue of the one before)
    }
    if (!gOn || !gInFrame || !gOverlap || !gIdleKickFrom || gFrameNo + 1 < gIdleKickFrom) {
        return;
    }
    flushBatch();
    if (gKickDraws > 0 && gKicks < gKicksMax) {
        gKickDraws = 0;
        const u32 before = gKicks;
        kick();
        gStats.idleKicks += gKicks - before;
    }
}

// One game frame done (system_3ds.cpp); with `log`, the renderer's line for the frames so far.
extern "C" void re4_3ds_gx_frame(int log)
{
    if (!gOn || !log || !gStats.frames) {
        return;
    }
    const double f = gStats.frames;
    struct mallinfo mi = mallinfo();
    if (gOccOn) {  // (occlusion cull)
        re4_log("3ds: occlusion cull (GX thread): per picture %.2f depth copies, %.2f built (%.3f ms each), %.2f "
                "without room in the queue; %u kernel invalidates failed",
                gOccCopies / f, gOccBuilds / f,
                gOccBuilds ? (double) gOccBuildTicks / CPU_TICKS_PER_MSEC / gOccBuilds : 0.0, gOccNoQueue / f,
                gOccSvcFail);
        gOccCopies = gOccBuilds = gOccNoQueue = gOccSvcFail = 0;
        gOccBuildTicks = 0;
    }
    re4_log("3ds: GX renderer: %lu pictures, per picture %.0f draws (%.0f skipped, %.0f without texture), %.0f vertices, "
            "%.1f splits, %.2f ms to end, %.1f TEV stages approximated; textures %u kept, %u made, %u failed, %u dropped, "
            "VRAM free %lu KB; C heap in use %u KB, linear free %lu KB (the largest piece %lu KB); the display list "
            "cache %u KB (its room %u KB, %u pieces free there; %u blocks made outside it, %u textures dropped "
            "for them)",
            (unsigned long) gStats.frames, gStats.draws / f, gStats.skipped / f, gStats.untextured / f,
            gStats.vertices / f, gStats.splits / f, (double) gStats.ticks / CPU_TICKS_PER_MSEC / f, gTevApprox / f,
            (unsigned) gTextures.size(), gTexMade, gTexFailed, gTexDropped, (unsigned long) vramSpaceFree() >> 10, (unsigned) mi.uordblks >> 10,
            (unsigned long) linearSpaceFree() >> 10, (unsigned long) linearLargest() >> 10, gKeptLive >> 10,
            gArena ? kArenaBytes >> 10 : 0, (unsigned) gArenaFree.size(), gArenaFailed, gKeepDropped);
    gArenaFailed = 0;
    gKeepDropped = 0;
    re4_log("3ds: TEV (stage 6.9): per picture %.1f GX stages packed into the PICA stage before, %.1f minor ones left "
            "out, %.1f TEV setups with a map as its average; the combiners built %.1f times",
            gTevPacked / f, gTevSkipped / f, gTevAveraged / f, gTevBuilt / f);
    gTevBuilt = 0;
    gTevPacked = gTevSkipped = gTevAveraged = 0;
    re4_log("3ds: TEV cache (RE4_3DS_TEVCACHE %d): per picture %.1f of the builds found in it, %.1f built and kept",
            (int) gTevCacheOn, gTevCacheHits / f, gTevCacheKept / f);
    gTevCacheHits = gTevCacheKept = 0;
    re4_log("3ds: sprites (stage 7.0): per picture %.1f draws joined over a position matrix, %.1f TEV writes found back "
            "at the combiners' values", gJoinedXf / f, gJoinedTev / f);
    gJoinedXf = gJoinedTev = 0;
    re4_log("3ds: sprites (stage 7.0, the cache): per picture %.1f small lists from the display list cache staged",
            gUnkept / f);
    gUnkept = 0;
    re4_log("3ds: lines (2026-11-09, RE4_3DS_LINEJOIN %d): per picture %.1f GX line draws, %.1f of them joined to the line "
            "draw before", (int) gLineJoin, gLineDraws / f, gLineJoined / f);
    gLineDraws = gLineJoined = 0;
    const double ms = CPU_TICKS_PER_MSEC * f;
    re4_log("3ds: GX renderer time per picture: draw() %.2f ms (%.2f us a draw), waits for the GPU %.2f ms, GPU drawing "
            "%.2f ms; %.0f GPU draws (%.0f indices), %.0f command words, %.0f uniform rows; %.0f draws (%.0f vertices) from the display list cache, "
            "%.1f kicks, %.1f on the GX thread's idle (GPU drawing: only after the last kick when kicked); left out outside the view (from picture %u) %.0f "
            "draws (%.0f indices)",
            gStats.drawTicks / ms, gStats.draws ? gStats.drawTicks / (double) CPU_TICKS_PER_MSEC * 1000.0 / gStats.draws : 0.0,
            gStats.waitTicks / ms, gStats.gpuMs / f, gStats.gpuDraws / f, gStats.indices / f, gStats.cmdWords / f, gStats.rowVectors / f,
            gStats.keptDraws / f, gStats.keptVertices / f, gStats.kicks / f, gStats.idleKicks / f, gViewCullFrom, gStats.viewCulled / f,
            gStats.viewCulledIndices / f);
    if (gPipe) {
        static u32 shownBefore;
        const u32 shown = gShown;
        re4_log("3ds: pictures in a pipeline (RE4_3DS_PIPE %u): per picture %.2f begins waited for the GPU (the picture "
                "before the last), %.2f queues made to run by the GX thread, %.2f shown",
                (unsigned) gPipeFrom, gPipeWaited / f, gPipeHandoffs / f, (shown - shownBefore) / f);
        shownBefore = shown;
        gPipeWaited = gPipeHandoffs = 0;
    }
    re4_log("3ds: linear memory flushes per picture (stage 3.8, kick and the frame's end): %.1f, %.2f ms (%.3f ms each, at "
            "most %.3f); %lu KB each",
            gStats.flushes / f, gStats.flushTicks / ms,
            gStats.flushes ? gStats.flushTicks / (double) CPU_TICKS_PER_MSEC / gStats.flushes : 0.0,
            gStats.flushMax / (double) CPU_TICKS_PER_MSEC, (unsigned long) __ctru_linear_heap_size >> 10);
    re4_log("3ds: attributes fast (RE4_3DS_ATTRFAST %u): per picture %.1f by the plan, %.1f the slow way, %.1f plans "
            "not made; %lu differences from the slow way in all", (unsigned) gAttrFast, gAttrFastDraws / f,
            gAttrSlowDraws / f, gAttrPlanFail / f, (unsigned long) gAttrMismatch);
    gAttrFastDraws = gAttrSlowDraws = gAttrPlanFail = 0;
    re4_log("3ds: GX lights per picture: %.0f lit draws, %.2f lights a lit draw (%.2f the whole way, %.2f far), %.0f with another "
            "light function (drawn as diffuse clamped, spot), %.0f left out (a position matrix beyond row %d); "
            "vertices %.0f lit, %.0f with a coordinate over w, by units read 0/1/2/3: %.0f/%.0f/%.0f/%.0f",
            gStats.litDraws / f, gStats.litDraws ? (double) gStats.lights / gStats.litDraws : 0.0,
            gStats.litDraws ? (double) gStats.lightsFull / gStats.litDraws : 0.0,
            gStats.litDraws ? (double) gStats.lightsFar / gStats.litDraws : 0.0, gStats.lightApprox / f,
            gStats.rowsOut / f, kRows - 1, gStats.litVertices / f, gStats.divVertices / f, gStats.unitVertices[0] / f,
            gStats.unitVertices[1] / f, gStats.unitVertices[2] / f, gStats.unitVertices[3] / f);
    if (gBusyOn && gAllSamples) {
        const u64 now = svcGetSystemTick();
        const double wall = (double) (now - gBusyFrom) / CPU_TICKS_PER_MSEC;
        const double share = (double) gBusySamples / gAllSamples;
        re4_log("3ds: GPU busy %.2f ms per picture (%.0f%% of %.0f ms a picture; %lu samples)", share * wall / f,
                share * 100, wall / f, (unsigned long) gAllSamples);
        gBusySamples = gAllSamples = 0;
        gBusyFrom = now;
    }
    re4_log("3ds: GX fog per picture (mode %d): %.0f draws, %.2f tables made", gFogMode, gStats.fogDraws / f,
            gStats.fogTables / f);
    re4_log("3ds: screen (stage 5.4): aspect mode %d, fill %d, picture %ux%u, band %d (rows %d-%d), EFB x %.4f + %.1f, "
            "y %.4f + %.1f; %.2f copy clears per picture (from picture %u)",
            gAspect, gFill, gPicW, gPicH, gBand, gBandTop, gBandBottom, gMapSx, gMapOx, gMapSy, gMapOy, gClears / f,
            gClearFrom);
    if (gBandKept || gNoBandRect[0] != -1) {
        re4_log("3ds: screen: %u scene pictures without the band laid out by the last one (RE4_3DS_BANDKEEP %d); the "
                "last such picture's scissor x %d-%d, y %d-%d", gBandKept, gBandKeep ? 1 : 0, gNoBandRect[0],
                gNoBandRect[2], gNoBandRect[1], gNoBandRect[3]);
    }
    if (gOwnState == 2) {  // (2026-11-19)
        re4_log("3ds: GX shadow (RE4_3DS_GXSHADOW 2): %u draws checked since the last line, differ: BP %u, XF memory %u, "
                "XF registers %u; the first word that differed 0x%x: own 0x%08x, the decoder's 0x%08x",
                gOwnChecked, gOwnDiffBp, gOwnDiffXfMem, gOwnDiffXfReg, gOwnFirstAt, gOwnFirstOwn, gOwnFirstTheirs);
        gOwnChecked = gOwnDiffBp = gOwnDiffXfMem = gOwnDiffXfReg = 0;
        gOwnFirstAt = ~0u;
    }
    static u32 waterCalls, refractCalls;
    const u32 calls = re4_gx_water_calls, refract = re4_gx_refract_calls;
    if (gWaterCopies || gWaterDraws || gWaterOdd || gWaterNoQueue || calls != waterCalls || refract != refractCalls) {
        re4_log("3ds: screen: the water's copy (patch 0328): %u named by the game (address 0x%08x; %u by the refraction "
                "shader of models, RE4_3DS_REFRACT), %u copies, %u draws, %u not through unit 0 / projective, %u without "
                "room", calls - waterCalls, (unsigned) re4_gx_water_addr, refract - refractCalls, gWaterCopies,
                gWaterDraws, gWaterOdd, gWaterNoQueue);
    }
    if (gRefractCopies || gRefractShared || gRefractNotMade || gRefractDraws || gRefractLeft || gWaterDiv) {  // (2026-11-18)
        re4_log("3ds: screen: the models' refraction shader (RE4_3DS_REFRACT): its copy %u made, %u this picture's again, "
                "%u not made; %u draws drawn (another map read), %u left out; the water's draws next to a map from the "
                "normal, down the plain path (RE4_3DS_WATERDIV): %u", gRefractCopies, gRefractShared, gRefractNotMade,
                gRefractDraws, gRefractLeft, gWaterDiv);
    }
    gRefractCopies = gRefractShared = gRefractNotMade = gRefractDraws = gRefractLeft = gWaterDiv = 0;
    waterCalls = calls;
    refractCalls = refract;
    if (gTexRenderPasses || gTexRenderCopies || gTexRenderDraws || gTexRenderOdd || gTexRenderNoQueue) {
        re4_log("3ds: screen: render targets (R4, patch 0337): %u passes, %u copies, %u draws, %u projective, %u "
                "without room, %u forgotten; %u targets", gTexRenderPasses, gTexRenderCopies, gTexRenderDraws,
                gTexRenderOdd, gTexRenderNoQueue, gTexRenderDropped, (unsigned) gTexRenders.size());
    }
    gTexRenderPasses = gTexRenderCopies = gTexRenderDraws = gTexRenderOdd = gTexRenderNoQueue = gTexRenderDropped = 0;
    if (gTevScaled) {
        re4_log("3ds: TEV (stage 6.8): %u times the GX stages past the PICA's 6 only scaled PREV, put into its scales "
                "(%u of them short of x4 a stage)", gTevScaled, gTevScaleShort);
    }
    gTevScaled = gTevScaleShort = 0;
    if (gTevFolded) {
        re4_log("3ds: TEV (stage 6.8): %u times the first GX stages, one colour with a one-colour copy, folded into "
                "one (%u stages)", gTevFolded, gTevFoldStages);
    }
    gTevFolded = gTevFoldStages = 0;
    gWaterCopies = gWaterDraws = gWaterOdd = gWaterNoQueue = 0;
    if (gCopyFillDraws || gCopyFillLeft) {
        re4_log("3ds: screen: intensity EFB copies (stage 6.8): %u draws with one colour for them, %u of copies alone "
                "left out", gCopyFillDraws, gCopyFillLeft);
    }
    gCopyFillDraws = gCopyFillLeft = 0;
    if (gContrastDraws) {
        re4_log("3ds: screen: the game's contrast pass without the copy (5.5): %u draws", gContrastDraws);
    }
    gContrastDraws = 0;
    gBandKept = 0;
    gNoBandRect[0] = -1;
    gClears = 0;
    u32 roomBlocks = 0, roomKb = 0;  // blocks holding linear memory
    for (auto& e : gBlocks) {
        if (e.second.room) {
            roomBlocks++;
            roomKb += e.second.bytes;
        }
    }
    re4_log("3ds: display list cache in VRAM (from picture %u; now picture %u): per picture %.0f draws, %.0f vertices, "
            "%.2f copies made, %.2f VRAM failed; %u blocks, %u KB in VRAM, VRAM free %lu KB; moved (stage 5.7b, %s): "
            "%u blocks moved, %.1f draws from their room a picture, %u of them in linear (%u KB), GX queue %u entries",
            gVramFrom, gFrameNo, gStats.vramDraws / f, gStats.vramVertices / f, gStats.dmas / f, gStats.vramFailed / f,
            (unsigned) gBlocks.size(), gVramBytes >> 10, (unsigned long) vramSpaceFree() >> 10, gKeptMove ? "on" : "off",
            gStats.moved, gStats.roomDraws / f, roomBlocks, roomKb >> 10, (unsigned) __C3D_Context.maxEntries);
    static const char* const kFormat[16] = {"I4", "I8", "IA4", "IA8", "RGB565", "RGB5A3", "RGBA8", "7",
                                            "C4", "C8", "C14X2", "11", "12", "13", "CMPR", "15"};
    char made[256];
    int at = 0;
    for (int i = 0; i < 16; i++) {
        if (gTexMadeBy[i] && at < (int) sizeof(made) - 32) {
            at += snprintf(made + at, sizeof(made) - at, " %s %u (%u KB)", kFormat[i], gTexMadeBy[i], gTexMadeKb[i]);
        }
    }
    u32 inVram = 0;
    for (auto& e : gTextures) {
        inVram += e.second->vram != nullptr;
    }
    u32 liveKb = 0;
    for (auto& e : gTextures) {
        liveKb += e.second->bytes;
    }
    re4_log("3ds: textures (VRAM from picture %u, taken back from picture %u, LOD +1 from picture %u, PICA formats from "
            "picture %u): per picture %.0f "
            "texture uses from VRAM, %.0f from linear; %u of %u in VRAM (%u of %u KB), %u VRAM failed, %u taken back%s%u); "
            "%.1f ms making them; %u made smaller for room, %u uses of ones not made not tried again; made by GX format:%s",
            gTexVramFrom, gTexEvictFrom, gTexLodFrom, gTexNativeFrom, gTexBindVram / f, gTexBindLinear / f, inVram,
            (unsigned) gTextures.size(), gTexVramBytes >> 10, liveKb >> 10, gTexVramFailed, gTexEvicted,
            gTexKeepLinear ? " to linear (gone: " : " (to linear by a DMA; gone, no room there: ", gTexGone,
            (double) gTexTicks / CPU_TICKS_PER_MSEC, gTexShrunk, gTexNotRetried, made);
    re4_log("3ds: texture pack (stage 5.7a; %u textures, %s): %u read in %.1f ms (%u from a smaller level), %u uses "
            "of ones not in it, %u of them to %s; %u index blocks read, %u made here by the key list (%u keys)",
            (unsigned) gPakCount,
            pakUsable() ? "used" : "not used", gTexFromPak, (double) gPakTicks / CPU_TICKS_PER_MSEC, gTexPakShrunk,
            gTexNotInPak, gTexMissed, gMissFile ? kMissPath : "nowhere", gPakBlockReads, gPakHereHits,
            (unsigned) gPakHere.size());
    gPakBlockReads = 0;
    preReport();  // (2026-11-24) textures ahead
    gPakHereHits = 0;
    char costs[512];
    int used = 0;
    for (int p = 0; p < re4pica::kPacks && used < (int) sizeof(costs) - 96; p++) {
        if (gMadeN[p] || gReadN[p]) {
            used += snprintf(costs + used, sizeof(costs) - used, " %s %u made %.1f ms each, %u read %.1f ms each %.0f KB",
                             re4pica::kPackName[p], gMadeN[p],
                             gMadeN[p] ? (double) gMadeTicks[p] / CPU_TICKS_PER_MSEC / gMadeN[p] : 0.0, gReadN[p],
                             gReadN[p] ? (double) gReadTicks[p] / CPU_TICKS_PER_MSEC / gReadN[p] : 0.0,
                             gReadN[p] ? gReadBytes[p] / 1024.0 / gReadN[p] : 0.0);
        }
    }
    re4_log("3ds: texture costs by PICA format (formats read from the pack: mask 0x%02lx):%s", (unsigned long) gPakFormats,
            used ? costs : " none");
    memset(gMadeN, 0, sizeof(gMadeN));
    memset(gReadN, 0, sizeof(gReadN));
    memset(gMadeTicks, 0, sizeof(gMadeTicks));
    memset(gReadTicks, 0, sizeof(gReadTicks));
    memset(gReadBytes, 0, sizeof(gReadBytes));
    gTexFromPak = gTexPakShrunk = gTexNotInPak = gTexMissed = 0;
    gPakTicks = 0;
    if (gMissFile && !gMissBuf) {
        fflush(gMissFile);
    }
    gTexBindVram = gTexBindLinear = gTexVramFailed = gTexEvicted = gTexGone = 0;
    gTexShrunk = gTexNotRetried = 0;
    gTexTicks = 0;
    re4_gx_kept_report();
    gTexMade = gTexFailed = gTexDropped = gTevApprox = 0;
    memset(&gStats, 0, sizeof(gStats));
}

// The texture misses still in the buffer (RE4_3DS_MISSBUF) to the SD card: START + SELECT (input_3ds.cpp)
extern "C" void re4_3ds_texmiss_flush(void)
{
    if (gMissFile) {
        fflush(gMissFile);
    }
}

// (occlusion cull, 2026-11-20; docs/22-occlusion.md) The game's side: runtime/part_cull.cpp re4_info_cull asks for
// each info of a model without bones inside the view (re4_occ_info), include/re4_part_cull.h gives the camera
// (re4_occ_view), part_cull.cpp frame() the frame (re4_occ_frame); the game's thread
extern "C" {
int re4_occ_on;
volatile int re4_occ_sparse;  // little hidden for a while: the GX thread copies the depth every 8th picture only
extern int re4_occ_table;  // runtime/gx_owner.cpp: the table of the entry drawn now, -1 outside one
}

namespace {

struct OccView {
    u32 tag;
    bool ok;
    float v[3][4];  // the frame's camera (world -> eye)
};
OccView gOccV[8];  // the last frames' cameras, by the frame's number
OccHiz gOccH;      // the newest depth
bool gOccHave, gOccViewSame, gOccTOk, gOccCalm;
u32 gOccSeen, gOccTag, gOccTFor;
float gOccT[3][4];  // this frame's eye -> that picture's eye
float gOccP[4][4];  // this frame's eye -> that picture's screen clip (gOccH.m * gOccT)
u32 gOccFrameTested, gOccFrameLeft, gOccSparseFrames;
float gOccShare = 1.0f;  // the share of the parts tested left out, smoothed over frames
// (2026-11-25, RE4_3DS_OCC_SPARSE=N) the parts left out a frame, smoothed: under N the sparse mode too, over 1.5 N (and
// the share over 8 %) every picture again (45 by default: recording 16 and bench7, 2026-11-25); 0: by the share only
float gOccLeftEma = 1000.0f;
float gOccSparseMin = 45.0f;
u32 gOccEmaFrames, gOccEmaUnder[5];  // (the count for N) tested frames; of them under 20 / 30 / 45 / 60 / 90
int gOccMargin = 16;
float gOccTurnMax = 4.0f, gOccMoveMax = 500.0f;
u32 gOccLeftParts, gOccLeftInfos, gOccCalmNot, gOccTested;
u64 gOccGameTicks;

void occMul(const float a[3][4], const float b[3][4], float o[3][4])
{
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            o[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] + (c == 3 ? a[r][3] : 0.0f);
        }
    }
}

bool occInv(const float a[3][4], float o[3][4])
{
    const float c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1], c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2],
                c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0];
    const float det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;
    if (!(fabsf(det) > 1e-12f)) {
        return false;
    }
    const float k = 1.0f / det;
    o[0][0] = c00 * k;
    o[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * k;
    o[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * k;
    o[1][0] = c01 * k;
    o[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * k;
    o[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * k;
    o[2][0] = c02 * k;
    o[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * k;
    o[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * k;
    for (int r = 0; r < 3; r++) {
        o[r][3] = -(o[r][0] * a[0][3] + o[r][1] * a[1][3] + o[r][2] * a[2][3]);
    }
    return true;
}

// The camera calm from view a to view b: the view axis turned at most gOccTurnMax degrees, the eye moved at most
// gOccMoveMax (a cut or the aim's camera: no cull - bench8, the late ones came in those)
bool occCalm(const float a[3][4], const float b[3][4])
{
    float d = a[2][0] * b[2][0] + a[2][1] * b[2][1] + a[2][2] * b[2][2];
    d = d > 1.0f ? 1.0f : d < -1.0f ? -1.0f : d;
    if (acosf(d) * 57.29578f > gOccTurnMax) {
        return false;
    }
    float m2 = 0.0f;
    for (int j = 0; j < 3; j++) {
        const float ea = -(a[0][j] * a[0][3] + a[1][j] * a[1][3] + a[2][j] * a[2][3]);
        const float eb = -(b[0][j] * b[0][3] + b[1][j] * b[1][3] + b[2][j] * b[2][3]);
        m2 += (ea - eb) * (ea - eb);
    }
    return m2 <= gOccMoveMax * gOccMoveMax;
}

inline int occClamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

// The box (centre c, half size h) through P (-> the depth's picture's screen clip): its nearest point beyond the
// farthest distance of every tile under its rectangle on that screen, with the margin around it. The corners from
// the centre and the three half axes (4 products, not 8)
bool occHidden(const OccHiz& z, const float P[4][4], const float c[3], const float h[3])
{
    float qc[4], ax[4], ay[4], az[4];
    for (int r = 0; r < 4; r++) {
        qc[r] = P[r][0] * c[0] + P[r][1] * c[1] + P[r][2] * c[2] + P[r][3];
        ax[r] = P[r][0] * h[0];
        ay[r] = P[r][1] * h[1];
        az[r] = P[r][2] * h[2];
    }
    // the nearest w first: across the eye's plane - not known
    const float nearest = qc[3] - fabsf(ax[3]) - fabsf(ay[3]) - fabsf(az[3]);
    if (!(nearest > 1.0f)) {
        return false;
    }
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (int k = 0; k < 8; k++) {
        const float sx = (k & 1) ? 1.0f : -1.0f, sy = (k & 2) ? 1.0f : -1.0f, sz = (k & 4) ? 1.0f : -1.0f;
        const float qx = qc[0] + sx * ax[0] + sy * ay[0] + sz * az[0];
        const float qy = qc[1] + sx * ax[1] + sy * ay[1] + sz * az[1];
        const float qw = qc[3] + sx * ax[3] + sy * ay[3] + sz * az[3];
        const float iw = 1.0f / qw;
        const float px = (qx * iw + 1.0f) * 200.0f, py = (1.0f - qy * iw) * 120.0f;
        x0 = px < x0 ? px : x0;
        x1 = px > x1 ? px : x1;
        y0 = py < y0 ? py : y0;
        y1 = py > y1 ? py : y1;
    }
    if (x1 < 0.0f || x0 >= 400.0f || y1 < 0.0f || y0 >= 240.0f) {
        return false;  // off that picture: not known
    }
    const int m = gOccMargin;
    const int a0 = occClamp((int) floorf(x0) - m, 0, 399), a1 = occClamp((int) ceilf(x1) + m, 0, 399);
    const int b0 = occClamp((int) floorf(y0) - m, 0, 239), b1 = occClamp((int) ceilf(y1) + m, 0, 239);
    const int r0 = a0 >> 3, r1 = a1 >> 3, c0 = (239 - b1) >> 3, c1 = (239 - b0) >> 3;
    for (int r = r0; r <= r1; r++) {
        for (int col = c0; col <= c1; col++) {
            if (!(z.tile[r][col] < nearest)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

// The camera of the info asked next (include/re4_part_cull.h re4InfoCulled): the frame's camera is the one at the
// first info of the world's tables (6 .. 0x12)
extern "C" void re4_occ_view(const float (*v)[4])
{
    const int t = re4_occ_table;
    OccView& F = gOccV[gOccTag & 7];
    if (!F.ok && t >= 6 && t < 0x13) {
        memcpy(F.v, v, sizeof(F.v));
        F.ok = true;
    }
    gOccViewSame = F.ok && memcmp(F.v, v, sizeof(F.v)) == 0;
}

// 1: the info (its box: centre c, half size h, through mv) is hidden behind the depth of an older picture
extern "C" int re4_occ_info(const float mv[3][4], const float c[3], const float h[3], unsigned parts)
{
    const int t = re4_occ_table;
    if (t < 0 || t >= 0x13 || !gOccViewSame || !gOccHave || gOccH.scale == 0.0f || gOccTag - gOccH.tag > 6) {
        return 0;
    }
    const u64 from = svcGetSystemTick();
    if (gOccTFor != gOccTag) {  // (once a frame: this camera against that picture's)
        gOccTFor = gOccTag;
        const OccView& F = gOccV[gOccTag & 7];
        const OccView& O = gOccV[gOccH.tag & 7];
        float inv[3][4];
        gOccTOk = O.tag == gOccH.tag && O.ok && occInv(F.v, inv);
        gOccCalm = gOccTOk && occCalm(O.v, F.v);
        if (gOccTOk) {
            occMul(O.v, inv, gOccT);
            for (int r = 0; r < 4; r++) {
                for (int c4 = 0; c4 < 4; c4++) {
                    gOccP[r][c4] = gOccH.m[r][0] * gOccT[0][c4] + gOccH.m[r][1] * gOccT[1][c4] +
                                   gOccH.m[r][2] * gOccT[2][c4] + (c4 == 3 ? gOccH.m[r][3] : 0.0f);
                }
            }
        }
    }
    int hidden = 0;
    if (!gOccCalm) {
        gOccCalmNot += gOccTOk ? parts : 0;
    } else {
        float P[4][4];  // the info's model -> that picture's screen clip
        for (int r = 0; r < 4; r++) {
            for (int c4 = 0; c4 < 4; c4++) {
                P[r][c4] = gOccP[r][0] * mv[0][c4] + gOccP[r][1] * mv[1][c4] + gOccP[r][2] * mv[2][c4] +
                           (c4 == 3 ? gOccP[r][3] : 0.0f);
            }
        }
        hidden = occHidden(gOccH, P, c, h);
        gOccTested += parts;
        gOccLeftParts += hidden ? parts : 0;
        gOccLeftInfos += hidden;
        gOccFrameTested += parts;
        gOccFrameLeft += hidden ? parts : 0;
    }
    gOccGameTicks += svcGetSystemTick() - from;
    return hidden;
}

// The game's frame f begins (runtime/part_cull.cpp frame()): the pictures from here carry f, the newest depth taken
extern "C" void re4_occ_frame(unsigned f)
{
    if (!re4_occ_on) {
        return;
    }
    static bool told;
    if (!told) {
        told = true;
        if (const char* v = getenv("RE4_3DS_OCC_MARGIN")) {
            gOccMargin = occClamp(atoi(v), 0, 64);
        }
        if (const char* v = getenv("RE4_3DS_OCC_TURN")) {
            gOccTurnMax = (float) atof(v);
        }
        if (const char* v = getenv("RE4_3DS_OCC_MOVE")) {
            gOccMoveMax = (float) atof(v);
        }
        if (const char* v = getenv("RE4_3DS_OCC_SPARSE")) {
            gOccSparseMin = (float) atof(v);
            re4_log("3ds: occlusion cull: a depth every 8th picture also while under %.0f parts a frame are left out "
                    "(RE4_3DS_OCC_SPARSE)", gOccSparseMin);
        }
        re4_log("3ds: occlusion cull: margin %d px, not when the camera turned more than %.1f deg or moved more than "
                "%.0f since the depth's picture (RE4_3DS_OCC_MARGIN, _TURN, _MOVE)",
                gOccMargin, gOccTurnMax, gOccMoveMax);
    }
    // (sparse) the share left out over the frames tested: under 5 % the depth every 8th picture only (the copy and
    // its tiles ≈ 2 ms of the GX thread, the tests ≈ 1 ms of this one - more than the cull gives in the open), over 8 %
    // every picture again
    if (gOccFrameTested) {
        gOccShare = gOccShare * 0.9f + 0.1f * (float) gOccFrameLeft / (float) gOccFrameTested;
        gOccLeftEma = gOccLeftEma * 0.9f + 0.1f * (float) gOccFrameLeft;
        {
            static const float kUnder[5] = {20.0f, 30.0f, 45.0f, 60.0f, 90.0f};
            gOccEmaFrames++;
            for (int k = 0; k < 5; k++) {
                gOccEmaUnder[k] += gOccLeftEma < kUnder[k];
            }
        }
        if (gOccSparseMin > 0.0f) {  // (2026-11-25) by how many are left out too: little hidden is not worth the copy
            re4_occ_sparse = gOccShare < 0.05f || gOccLeftEma < gOccSparseMin                        ? 1
                             : gOccShare > 0.08f && gOccLeftEma > gOccSparseMin * 1.5f ? 0
                                                                                        : re4_occ_sparse;
        } else {
            re4_occ_sparse = gOccShare < 0.05f ? 1 : gOccShare > 0.08f ? 0 : re4_occ_sparse;
        }
    }
    gOccSparseFrames += re4_occ_sparse != 0;
    gOccFrameTested = gOccFrameLeft = 0;
    gOccTag = f;
    OccView& F = gOccV[f & 7];
    F.tag = f;
    F.ok = false;
    gOccViewSame = false;
    if (re4_gxq_on) {
        re4_gxq_fn(occTagFn, f, 0);
    } else {
        occTagFn(f, 0);  // (the renderer on this thread)
    }
    {
        std::lock_guard<std::mutex> lock(gOccLock);
        if (gOccPub.seq != gOccSeen) {
            gOccSeen = gOccPub.seq;
            gOccH = gOccPub;
            gOccHave = true;
            gOccTFor = ~0u;
        }
    }
    if (f && f % 300 == 0) {
        const double n = 300.0;
        re4_log("3ds: occlusion cull (game, frame %u, 300 frames): per frame %.1f parts tested, %.1f left out (%.1f "
                "infos), %.1f not tested (the camera turned or moved); %.3f ms on the game's thread; sparse (a depth "
                "every 8th picture) %u frames",
                f, gOccTested / n, gOccLeftParts / n, gOccLeftInfos / n, gOccCalmNot / n,
                (double) gOccGameTicks / CPU_TICKS_PER_MSEC / n, gOccSparseFrames);
        gOccTested = gOccLeftParts = gOccLeftInfos = gOccCalmNot = gOccSparseFrames = 0;
        re4_log("3ds: occlusion cull (game, frame %u): %u frames tested; the parts left out a frame (smoothed, a model 4) "
                "under 20 / 30 / 45 / 60 / 90 on %u / %u / %u / %u / %u of them",
                f, gOccEmaFrames, gOccEmaUnder[0], gOccEmaUnder[1], gOccEmaUnder[2], gOccEmaUnder[3], gOccEmaUnder[4]);
        gOccEmaFrames = gOccEmaUnder[0] = gOccEmaUnder[1] = gOccEmaUnder[2] = gOccEmaUnder[3] = gOccEmaUnder[4] = 0;
        gOccGameTicks = 0;
    }
}

// (occlusion cull, models: patch 0338) A model of table 13 (enemies, Leon, Ashley: game/trans_ot.cpp
// AddOtModelPosRadius) left out when its sphere (world centre, radius) is behind the room in the newest depth: the
// cube around it through that picture's camera (gOccV) and projection, as an info's box. view: this frame's camera
namespace {
int gOccModels = -1;  // RE4_3DS_OCC_MODELS
u32 gOccSphSeq = ~0u, gOccSphFor = ~0u, gOccSphLogged;
bool gOccSphOk, gOccSphCalm;
float gOccSphP[4][4];   // world -> that picture's screen clip
float gOccSphOV[3][4];  // that picture's camera
float gOccSphV[3][4];   // the camera the calm was found for
u32 gOccSphTested, gOccSphLeft, gOccSphNotCalm;
u64 gOccSphTicks;
}  // namespace

extern "C" int re4_occ_sphere3(const float* pos, float radius, const float (*v)[4])
{
    if (gOccModels < 0) {
        const char* e = getenv("RE4_3DS_OCC_MODELS");
        gOccModels = e ? atoi(e) : 1;
        re4_log("3ds: occlusion cull of the models (table 13: enemies, Leon, Ashley) %s (RE4_3DS_OCC_MODELS)",
                gOccModels ? "on" : "off");
    }
    if (gOccSphLogged != gOccTag && gOccTag % 300 == 0 && gOccTag) {
        gOccSphLogged = gOccTag;
        const double n = 300.0;
        re4_log("3ds: occlusion cull (models, frame %u, 300 frames): per frame %.1f tested, %.1f left out, %.1f not "
                "tested (the camera turned or moved); %.3f ms",
                gOccTag, gOccSphTested / n, gOccSphLeft / n, gOccSphNotCalm / n,
                (double) gOccSphTicks / CPU_TICKS_PER_MSEC / n);
        gOccSphTested = gOccSphLeft = gOccSphNotCalm = 0;
        gOccSphTicks = 0;
    }
    if (!gOccModels || !gOccHave || gOccH.scale == 0.0f || gOccTag - gOccH.tag > 6) {
        return 0;
    }
    const u64 from = svcGetSystemTick();
    if (gOccSphSeq != gOccH.seq) {  // (a new depth: its camera -> its screen)
        gOccSphSeq = gOccH.seq;
        gOccSphFor = ~0u;
        const OccView& O = gOccV[gOccH.tag & 7];
        gOccSphOk = O.tag == gOccH.tag && O.ok;
        if (gOccSphOk) {
            memcpy(gOccSphOV, O.v, sizeof(gOccSphOV));
            for (int r = 0; r < 4; r++) {
                for (int c = 0; c < 4; c++) {
                    gOccSphP[r][c] = gOccH.m[r][0] * O.v[0][c] + gOccH.m[r][1] * O.v[1][c] +
                                     gOccH.m[r][2] * O.v[2][c] + (c == 3 ? gOccH.m[r][3] : 0.0f);
                }
            }
        }
    }
    int hidden = 0;
    if (gOccSphOk) {
        if (gOccSphFor != gOccTag || memcmp(gOccSphV, v, sizeof(gOccSphV)) != 0) {
            gOccSphFor = gOccTag;
            memcpy(gOccSphV, v, sizeof(gOccSphV));
            gOccSphCalm = occCalm(gOccSphOV, v);
        }
        if (!gOccSphCalm) {
            gOccSphNotCalm++;
        } else {
            const float h[3] = {radius, radius, radius};
            hidden = occHidden(gOccH, gOccSphP, pos, h);
            gOccSphTested++;
            gOccSphLeft += hidden;
            gOccFrameTested += 4;  // (the sparse mode's share: a model as 4 parts)
            gOccFrameLeft += hidden ? 4 : 0;
        }
    }
    gOccSphTicks += svcGetSystemTick() - from;
    return hidden;
}

// (2026-11-24) textures ahead: entry points (stage94_texpreload_edits.py)
void (*re4_dvd_open_fn)(const char* path);  // (runtime/dvd.cpp declares it too; defined here for the 3DS)

// any thread (the game's, the DVD reader's): the file's hash queued for the GX thread, which is woken
extern "C" void re4_3ds_tex_preload_open(const char* path)
{
    if (!gPreOn || !path) {
        return;
    }
    u32 h = 0x811C9DC5u;
    for (const char* c = path; *c; c++) {
        const u8 ch = (u8) (*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
        h = (h ^ ch) * 0x01000193u;
    }
    LightLock_Lock(&gPreLock);
    if (gPreQueue.size() < 256) {
        gPreQueue.push_back({h, gFrameNo});
        __atomic_add_fetch(&gPreQueuedN, 1, __ATOMIC_SEQ_CST);
    }
    gPreOpened++;
    LightLock_Unlock(&gPreLock);
    if (re4_3ds_gx_wake_fn) {
        re4_3ds_gx_wake_fn();
    }
}

// the GX thread, its ring empty: true when it did something (then it looks at the ring again)
extern "C" int re4_3ds_tex_preload_step(void)
{
    return preStep() ? 1 : 0;
}

// the GX thread: anything to read ahead (off: never - the loop as before)
extern "C" int re4_3ds_tex_preload_wants(void)
{
    return gPreOn && (gPreAt < gPreKeys.size() || __atomic_load_n(&gPreQueuedN, __ATOMIC_SEQ_CST) != 0);
}
