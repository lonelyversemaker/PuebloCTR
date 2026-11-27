// The button mapping (2026-11-21, the project owner's wish): the game's actions on the 3DS's buttons, one or two of them
// (3ds/controls_map.h: the logic, a host test of it). Here: the mapping in sdmc:/3ds/pueblo/controls.bin, the keys
// held -> the GameCube buttons each game frame (input_3ds.cpp readPad, the game's thread), the menus' usual layout
// (the game's menu bit: runtime/pad.cpp re4_pad_rumble bit 4, state_trace.cpp re4_game_menu_open - the title, the sub
// screens), catching a binding for the settings screen (setup_3ds.cpp: the page "Button mapping" of the CONTROLS tab),
// the quick turn on a button of its own (the stick back, then B).
// The mapping only with the settings screen on (setup_3ds.cpp re4_3ds_setup_start): the benches, the replays and
// RE4_3DS_SETUP=0 keep the usual layout. controls.bin: 52 bytes, a checksum; bad or missing - the defaults; written
// by the panel's thread half a second after the last change (in place, as settings.bin).
#include <3ds.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "controls_map.h"

extern "C" {
unsigned re4_pad_rumble(void);  // runtime/pad.cpp: bit 4 the game shows a menu
extern volatile int re4_wep_want;  // runtime/cheats.cpp (patch 0385): the D-pad weapons - the item id asked for
extern volatile unsigned char re4_wep_slot[4];  // each arrow's weapon (3ds settings)
void re4_log(const char* fmt, ...);
}

namespace {

const char kPath[] = "sdmc:/3ds/pueblo/controls.bin";

struct File {
    char magic[4];  // "PCB1"
    u8 version;     // 5 (2: the quick turn added, 3: the moves, 4: the C stick's, 5: the D-pad weapons; 4 still read)
    u8 actions;     // ctl::kActions (24)
    u8 keys[ctl::kActions][2];
    u8 pad[2];
    u32 sum;  // FNV-1a of the bytes before (at 56)
};
static_assert(sizeof(File) == 60, "controls.bin is 60 bytes");
const int kActions4 = 20;  // (version 4: 52 bytes, its sum at 48)

ctl::Map gMap = ctl::defaults();
bool gOn;                     // the mapping applies (the settings screen is on)
ctl::Switcher gSwitch;
ctl::Catch gCatch;
volatile int gCatching = -1;  // the action whose binding is being caught (-1 none)
volatile bool gDirty;         // the settings screen: a new picture due
volatile u64 gWriteAt;        // the file due at this tick (0: none)
int gLastMenu = -1;
int gTurnFrames;               // the quick turn's binding held for so many frames (0: not held)
unsigned gLastKeys;            // the keys of the last frame and its layout (re4_3ds_controls_stick reads them)
bool gMenuNow = true;
// the quick turn as the GameCube player does it (game/pl_class.cpp actionSelect: B while Leon steps back): the stick back
// for its first frames, B from its second frame on; the stick let go after them (no walking back after the turn)
const int kTurnBackFrames = 6;

const char* const kKeyNames[ctl::kKeys] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Up", "Down", "Left", "Right",
                                           "START", "PadUp", "PadDown", "PadLeft", "PadRight", "CUp", "CDown", "CLeft",
                                           "CRight"};
const char* const kActionNames[ctl::kActions] = {"forward", "back", "left", "right", "look up", "look down",
                                                 "look left", "look right", "action", "run", "quick turn", "aim",
                                                 "fire", "reload", "knife", "slash", "Ashley", "case", "map", "options",
                                                 "weapon up", "weapon down", "weapon left", "weapon right"};

u32 fnv(const u8* p, size_t n)
{
    u32 h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

void describe(char* out, size_t n)
{
    size_t o = 0;
    out[0] = 0;
    for (int a = 0; a < ctl::kActions && o + 24 < n; a++) {
        const ctl::Binding& b = gMap.b[a];
        o += snprintf(out + o, n - o, "%s%s=", a ? ", " : "", kActionNames[a]);
        if (!b.keys()) {
            o += snprintf(out + o, n - o, "none");
        }
        for (int i = 0; i < b.keys(); i++) {
            o += snprintf(out + o, n - o, "%s%s", i ? "+" : "", kKeyNames[b.k[i]]);
        }
    }
}

void load()
{
    gMap = ctl::defaults();
    FILE* fp = fopen(kPath, "rb");
    if (!fp) {
        re4_log("3ds: buttons: no %s - the usual layout", kPath);
        return;
    }
    File f;
    const size_t n = fread(&f, 1, sizeof(f), fp);
    fclose(fp);
    u32 sum4;  // (2026-11-27) version 4: 20 actions, the D-pad weapons' on their arrows where no action has them (from
    // 52 bytes on: a beta of version 4 on the same card writes over the first 52 of the 60)
    memcpy(&sum4, (const u8*) &f + 48, 4);
    const bool v4 = n >= 52 && !memcmp(f.magic, "PCB1", 4) && f.version == 4 && f.actions == kActions4 &&
                    sum4 == fnv((const u8*) &f, 48);
    if (v4) {
        const ctl::Map d = ctl::defaults();
        for (int a = kActions4; a < ctl::kActions; a++) {
            f.keys[a][0] = d.b[a].k[0];
            f.keys[a][1] = ctl::kNone;
            for (int b = 0; b < kActions4; b++) {
                if (f.keys[b][0] == d.b[a].k[0] || f.keys[b][1] == d.b[a].k[0]) {
                    f.keys[a][0] = ctl::kNone;
                }
            }
        }
        re4_log("3ds: buttons: %s of version 4 - the D-pad weapons on the free arrows", kPath);
    }
    if (!v4 && (n != sizeof(f) || memcmp(f.magic, "PCB1", 4) || f.version != 5 || f.actions != ctl::kActions ||
        f.sum != fnv((const u8*) &f, offsetof(File, sum)))) {
        re4_log("3ds: buttons: %s not ours or damaged (%u bytes) - the usual layout", kPath, (unsigned) n);
        return;
    }
    ctl::Map m;
    for (int a = 0; a < ctl::kActions; a++) {
        for (int i = 0; i < 2; i++) {
            const u8 k = f.keys[a][i];
            m.b[a].k[i] = k < ctl::kKeys ? k : ctl::kNone;
        }
        if (m.b[a].k[0] == ctl::kNone) {
            m.b[a].k[1] = ctl::kNone;
        }
    }
    gMap = m;
}

bool save()
{
    File f;
    memset(&f, 0, sizeof(f));
    memcpy(f.magic, "PCB1", 4);
    f.version = 5;
    f.actions = ctl::kActions;
    const ctl::Map m = gMap;
    for (int a = 0; a < ctl::kActions; a++) {
        f.keys[a][0] = m.b[a].k[0];
        f.keys[a][1] = m.b[a].k[1];
    }
    f.sum = fnv((const u8*) &f, offsetof(File, sum));
    FILE* fp = fopen(kPath, "r+b");  // (in place: the file keeps its cluster)
    if (!fp) {
        fp = fopen(kPath, "wb");
    }
    if (!fp) {
        return false;
    }
    const bool ok = fwrite(&f, 1, sizeof(f), fp) == sizeof(f);
    fclose(fp);
    return ok;
}

void changed()
{
    gDirty = true;
    gWriteAt = svcGetSystemTick() + SYSCLOCK_ARM11 / 2;
}

}  // namespace

// setup_3ds.cpp re4_3ds_setup_start (the main thread, before the game): the mapping on (the settings screen is) or off
extern "C" void re4_3ds_controls_start(int on)
{
    gOn = on != 0;
    if (!gOn) {
        return;
    }
    load();
    char d[400];
    describe(d, sizeof(d));
    re4_log("3ds: buttons%s: %s", ctl::same(gMap, ctl::defaults()) ? " (the usual layout)" : "", d);
}

// input_3ds.cpp readPad (the game's thread, each frame): the keys held (a bit a ctl::Key), SELECT -> the GameCube
// buttons. While a binding is caught (and until its keys are let go) the game gets none.
extern "C" unsigned re4_3ds_controls_pad(unsigned keys, int select)
{
    const bool menu = (re4_pad_rumble() & 0x10) != 0;
    gLastKeys = keys;
    gMenuNow = menu;
    if (gLastMenu != (int) menu) {
        if (gOn && gLastMenu >= 0) {
            re4_log("3ds: buttons: %s", menu ? "a menu - the usual layout" : "in the game - the mapping");
        }
        gLastMenu = menu;
    }
    if (!gOn) {
        return ctl::usual(keys);
    }
    if (gCatch.active()) {
        if (gCatch.step(keys, select != 0) && gCatching >= 0) {
            const int a = gCatching;
            ctl::Map m = gMap;
            if (ctl::assign(m, a, gCatch.got)) {
                gMap = m;
                char d[400];
                describe(d, sizeof(d));
                re4_log("3ds: buttons: %s set: %s", kActionNames[a], d);
            }
            gCatching = -1;
            changed();
        }
        gSwitch.step(gMap, 0, true);  // (the title's layout, nothing held)
        gTurnFrames = 0;
        return 0;
    }
    bool turn = false;
    // (2026-11-27, patch 0385) the D-pad weapons: an arrow's action without a weapon is as unbound (its key passes
    // through as the GameCube D-pad, as before); one with a weapon asks the game for it when its binding acts
    ctl::Map eff = gMap;
    for (int i = 0; i < 4; i++) {
        if (!re4_wep_slot[i]) {
            eff.b[ctl::kWepUp + i].k[0] = eff.b[ctl::kWepUp + i].k[1] = ctl::kNone;
        }
    }
    unsigned out = gSwitch.step(eff, keys, menu, &turn);
    {
        static unsigned last;
        for (int i = 0; i < 4 && !menu; i++) {
            const int a = ctl::kWepUp + i;
            if (ctl::acting(eff, keys, a) && !ctl::acting(eff, last, a)) {
                re4_wep_want = re4_wep_slot[i];
            }
        }
        last = keys;
    }
    gTurnFrames = turn ? gTurnFrames + 1 : 0;
    if (gTurnFrames >= 2) {
        out |= ctl::kGcB;
    }
    return out;
}

// input_3ds.cpp readPad, after re4_3ds_controls_pad: the control stick and the C stick under the mapping (the moves, the
// looks) from the Circle Pad's (*px, *py) and the C stick's (*cx, *cy) positions (right, up, the dead zone off; `rim`
// the whole way) - as they are in the menus and without the mapping, nothing while a binding is caught
extern "C" void re4_3ds_controls_sticks(int* px, int* py, int* cx, int* cy, int rim)
{
    if (!gOn || gMenuNow) {
        return;
    }
    if (gCatch.active()) {
        *px = *py = *cx = *cy = 0;
        return;
    }
    const int p[2] = {*px, *py}, c[2] = {*cx, *cy};
    ctl::stick(gMap, gLastKeys, ctl::kUp, p[0], p[1], c[0], c[1], rim, px, py);
    ctl::stick(gMap, gLastKeys, ctl::kLookUp, p[0], p[1], c[0], c[1], rim, cx, cy);
}

// input_3ds.cpp readPad, after re4_3ds_controls_pad: 1 - the quick turn wants the control stick back now
extern "C" int re4_3ds_controls_turn(void)
{
    return gTurnFrames >= 1 && gTurnFrames <= kTurnBackFrames;
}


// The settings screen (the game's thread: its touch): catch a binding for `action`, or stop catching (-1)
extern "C" void re4_3ds_controls_catch(int action)
{
    if (action < 0 || action >= ctl::kActions) {
        if (gCatch.waiting()) {
            gCatch.cancel();
        }
        gCatching = -1;
        gDirty = true;
        return;
    }
    gCatch.start();
    gCatching = action;
    gDirty = true;
}

// the action being caught (-1 none)
extern "C" int re4_3ds_controls_catching(void)
{
    return gCatching;
}

extern "C" void re4_3ds_controls_reset(void)
{
    gMap = ctl::defaults();
    re4_log("3ds: buttons: back to the usual layout");
    changed();
}

// The panel's thread: action a's keys (k[0], k[1]; 0xFF none) for the page
extern "C" void re4_3ds_controls_binding(int a, unsigned char* k)
{
    const ctl::Binding b = a >= 0 && a < ctl::kActions ? gMap.b[a] : ctl::Binding{{ctl::kNone, ctl::kNone}};
    k[0] = b.k[0];
    k[1] = b.k[1];
}

// The panel's thread: a new picture due since the last call
extern "C" int re4_3ds_controls_dirty(void)
{
    if (!gDirty) {
        return 0;
    }
    gDirty = false;
    return 1;
}

// The panel's thread, each pass: the file half a second after the last change
extern "C" void re4_3ds_controls_poll(void)
{
    const u64 at = gWriteAt;
    if (at && svcGetSystemTick() >= at) {
        gWriteAt = 0;
        const bool ok = save();
        re4_log("3ds: buttons: %s %s", kPath, ok ? "written" : "NOT written");
    }
}
