// R2 (2026-11-12; the owner's decisions R0 / R2 and their choice of the mockup): the bottom screen before the gameplay - the settings. The cheats one by one
// (runtime/cheats.cpp bits 1 health, 2 ammo, 4 one-hit kill, 8 automatic QTEs), +1 000 000 pesetas once a launch
// (runtime/cheats.cpp re4_cheats_money_arm), the gyro aim's gain (input_3ds.cpp re4_3ds_gyro_set; on 2x by default),
// the frame counter in the game screen's corner (off by default).
//
// Shown until the gameplay starts and again when the game is back at its title (STA_TITLE: from the logos to the
// title's exit, again after "quit" or a reset; runtime/cheats.cpp hudSample re4_hud_title), drawn by the panel's
// thread (settings_3ds.cpp hudThread -> 3ds/ui_bottom_3ds.cpp re4_ui_setup), changed by touch (the game's thread,
// settings_3ds.cpp re4_3ds_hud_pad): a row - its box; the gyro - the left arrow less, elsewhere more (4x -> off).
// Taken when the title is left for the gameplay (a new game, a load, the extra games; the game's thread, its frame
// hook): only after the title was seen - the runs that start in a room (the benches, RE4_START_ROOM) take nothing, and
// pueblo.cfg's RE4_CHEATS / RE4_GYRO / RE4_3DS_HUD rule them as before. The cheats are not changed during the gameplay
// (R0: fewer new faults). RE4_3DS_SETUP=0: no settings screen, nothing taken (the game's screen at the title, as before);
// off by default under a replay or a pad script (RE4_PAD_REPLAY / RE4_PAD_SCRIPT: the benches, the start in r100).
//
// Kept in sdmc:/3ds/pueblo/settings.bin (16 bytes, a checksum; bad or missing - the defaults), written by the panel's
// thread half a second after the last change: only the settings screen changes it, so at the title (a write holds the
// game's reads of the SD card, 6.2e; rename over a file does not work there - written in place).
#include <3ds.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
extern volatile int re4_hud_title, re4_hud_life, re4_hud_lang;  // runtime/cheats.cpp hudSample
extern volatile int re4_top_hud_off;     // (2026-11-27) runtime/cheats.cpp (patch 0383: IDSystem::trans)
extern volatile int re4_ui_lang_choice;  // (2026-11-27) ui_bottom_3ds.cpp lang()
extern volatile int re4_hud_option;      // (2026-11-27) runtime/cheats.cpp: the game's option screen open
extern volatile int re4_ui_setup_live;   // (2026-11-27) ui_bottom_3ds.cpp: the footnote "Applied at once"
extern volatile unsigned char re4_wep_slot[4];   // (2026-11-27) runtime/cheats.cpp: the D-pad weapons
extern volatile unsigned char re4_wep_owned[24];
extern volatile int re4_wep_owned_n, re4_wep_owned_ok;
void re4_option_request(void);                    // runtime/cheats.cpp
void re4_ui_wep_pick(unsigned short* fb, int slot, const unsigned char* ids, int n, int lit, int now);  // ui_bottom_3ds.cpp
int re4_ui_wep_pick_hit(int n, int x, int y);
extern volatile int re4_option_close;  // runtime/cheats.cpp (patch 0385): the option screen closed from here
void re4_add_frame_hook(void (*hook)(unsigned));                // runtime/pad.cpp
void re4_cheats_set(int bits);                                  // runtime/cheats.cpp
int re4_cheats_wanted(void);
void re4_cheats_money_arm(void);                                // runtime/cheats.cpp (R2)
int re4_3ds_icons_progress(void);  // (2026-11-27) icons_3ds.cpp: the item pictures being made (-1 none)
extern volatile int re4_merchant_weapons;                       // (2026-11-27) runtime/cheats.cpp (patch 0386)
void re4_3ds_gyro_set(int gain10);                              // input_3ds.cpp (R2)
void re4_3ds_stick_set(int index);                              // input_3ds.cpp (2026-11-18)
void re4_3ds_gx_fill(int mode);                                 // gx_3ds.cpp (2026-11-19)
void re4_3ds_hud_fps(int on);                                   // settings_3ds.cpp (R2)
void re4_ui_setup(unsigned short* fb, const int* v, int lit, int tab);  // ui_bottom_3ds.cpp (R2; tabs 2026-11-19)
int re4_ui_setup_hit(int tab, int x, int y, int* left);
// (2026-11-21) the button mapping: its page (ui_bottom_3ds.cpp), the mapping (controls_3ds.cpp)
void re4_ui_buttons(unsigned short* fb, int page, int lit, int catching, const unsigned char (*keys)[2]);
int re4_ui_buttons_hit(int page, int x, int y);
void re4_3ds_controls_start(int on);
void re4_3ds_stick_deadzone(int pct);  // input_3ds.cpp (2026-11-21)
void re4_3ds_controls_catch(int action);
int re4_3ds_controls_catching(void);
void re4_3ds_controls_reset(void);
void re4_3ds_controls_binding(int a, unsigned char* k);
int re4_3ds_controls_dirty(void);
void re4_3ds_controls_poll(void);
void re4_log(const char* fmt, ...);
void re4_3ds_gyro_poll(void);  // input_3ds.cpp (2026-11-21): the gyro's zero into gyro.bin
}

namespace {

const char kPath[] = "sdmc:/3ds/pueblo/settings.bin";
const int kRows = 15, kGyroRow = 5, kStickRow = 7;
const int kShopRow = 13, kCaseRow = 14;  // (2026-11-27) CHEATS: all weapons at the merchant; 14 - the case XL, gone
const int kTopHudRow = 11, kUiLangRow = 12, kUiLangSteps = 8;  // (2026-11-27) the SCREEN tab: the top HUD off; the
                                                               // bottom screen's language (0 Auto, 1..7)
const int kDeadRow = 10, kDeadMax = 50, kDeadStep = 5;  // (2026-11-21) the Circle Pad's dead zone, percent  // (2026-11-18: row 7 the circle pad's sensitivity, 0..3)
const int kStickSteps = 4;
const int kPictureRow = 8, kPictureSteps = 3;  // (2026-11-19: row 8 the picture, 0 Fit 1 Stretch 2 Zoom)
const int kButtonsRow = 9;  // (2026-11-21) the CONTROLS tab's link to the button mapping's page (no value of its own)
const int kBtnActions = 24, kBtnReset = 24;  // the page's rows: the actions (controls_map.h), then "Reset to defaults"
const int kBtnPages = 4;
const int kGains[] = {0, 10, 15, 20, 25, 30, 40};
const int kGainCount = (int) (sizeof(kGains) / sizeof(kGains[0]));
const int kDefaults[kRows] = {0, 0, 0, 0, 0, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // (R0: the cheats off, the gyro on 2x, the counter off; the stick 1x; Fit)

struct File {
    char magic[4];  // "PCS1"
    u8 version;     // 1: 16 bytes (the sum after deadzone); (2026-11-27) 2: 32 bytes
    u8 cheats;      // bits 1 2 4 8
    u8 money;       // +1 000 000 pesetas once a launch
    u8 gyro;        // the gain x10 (0 off)
    u8 fps;         // the frame counter
    u8 stick;       // (2026-11-18; padding before: 0) the circle pad's sensitivity 0..3: 1x 1.25x 1.5x 2x
    u8 picture;     // (2026-11-19; padding before: 0) the picture 0..2: Fit, Stretch, Zoom
    u8 deadzone;    // (2026-11-21; padding before: 0) the Circle Pad's dead zone, percent 0..50 by 5
    u8 tophud;      // (2026-11-27, version 2) the life / ammo HUD off the top screen
    u8 uilang;      // (version 2) the bottom screen's language: 0 Auto, 1..7 en fr de es it ja chs
    u8 wep[4];      // (2026-11-27, in version 2's spare bytes) the D-pad weapons: up, down, left, right - item ids, 0 none
    u8 more;        // (2026-11-27, in version 2's spare bytes: 0 before) the cheats 1 all weapons at the merchant, 2 the case XL
    u8 spare[9];    // (version 2: 0)
    u32 sum;        // FNV-1a of the bytes before
};
static_assert(sizeof(File) == 32, "settings.bin is 32 bytes (version 2)");

int gV[kRows] = {0, 0, 0, 0, 0, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // the screen's values: the boxes 0/1, the gyro's gain x10, the steps
volatile int gLit = -1;                  // the row under the finger
volatile int gTab = 2;                   // (2026-11-19) the open tab: 0 CHEATS, 1 CONTROLS, 2 SCREEN (2026-11-27: the
                                         // leftmost - SCREEN - at each launch)
volatile int gSub;                       // (2026-11-21) 1: the button mapping's page instead of the tab
volatile int gPage;                      // its page: 0, 1
bool gOn;                                // the screen (RE4_3DS_SETUP)
volatile bool gInGame;                   // the title left for the gameplay
bool gSawTitle;
volatile bool gChanged = true;           // a new picture due (a value, the finger, the screen switched, the language)
volatile u64 gWriteAt;                   // the file due at this tick (0: none)
bool gMoneyGiven;                        // (once a launch)
int gLastLang = -2;
int gLastOption;                         // (2026-11-27) re4_hud_option as frame() saw it
volatile int gPick = -1;                 // (2026-11-27) the D-pad weapons: the arrow whose weapon is being chosen
int gPickWait;                           // frames the option screen was awaited for it
unsigned char gPickIds[24];              // the list shown (Leon's weapons when it opened)
int gPickN;

// (2026-11-27) the SCREEN tab's rows that act at once: the top HUD, the bottom screen's language
void applyScreen()
{
    re4_top_hud_off = gV[kTopHudRow] ? 1 : 0;
    re4_ui_lang_choice = gV[kUiLangRow];
}

u32 fnv(const u8* p, size_t n)
{
    u32 h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

int gainIndex(int g)
{
    for (int i = 0; i < kGainCount; i++) {
        if (kGains[i] == g) {
            return i;
        }
    }
    return 3;  // (2x)
}

void load()
{
    memcpy(gV, kDefaults, sizeof(gV));
    File f;
    memset(&f, 0, sizeof(f));
    FILE* fp = fopen(kPath, "rb");
    if (!fp) {
        re4_log("3ds: settings: no %s - the defaults", kPath);
        return;
    }
    const size_t n = fread(&f, 1, sizeof(f), fp);
    fclose(fp);
    u32 sum1;  // (2026-11-27) version 1: 16 bytes, its sum at 12
    memcpy(&sum1, (const u8*) &f + 12, 4);
    const bool v1 = n >= 16 && f.version == 1 && sum1 == fnv((const u8*) &f, 12);
    const bool v2 = n == sizeof(f) && f.version == 2 && f.sum == fnv((const u8*) &f, offsetof(File, sum));
    if (memcmp(f.magic, "PCS1", 4) || (!v1 && !v2)) {
        re4_log("3ds: settings: %s not ours or damaged (%u bytes) - the defaults", kPath, (unsigned) n);
        return;
    }
    for (int i = 0; i < 4; i++) {
        gV[i] = (f.cheats >> i) & 1;
    }
    gV[4] = 0;  // (2026-11-27: a button now - f.money no longer gives the pesetas at each launch)
    gV[5] = kGains[gainIndex(f.gyro)];
    gV[6] = f.fps ? 1 : 0;
    gV[kStickRow] = f.stick < kStickSteps ? f.stick : 0;
    gV[kPictureRow] = f.picture < kPictureSteps ? f.picture : 0;
    gV[kDeadRow] = f.deadzone <= kDeadMax && f.deadzone % kDeadStep == 0 ? f.deadzone : 0;
    if (v2) {
        for (int i = 0; i < 4; i++) {
            re4_wep_slot[i] = f.wep[i];
        }
        gV[kTopHudRow] = f.tophud ? 1 : 0;
        gV[kUiLangRow] = f.uilang < kUiLangSteps ? f.uilang : 0;
        gV[kShopRow] = f.more & 1;
        gV[kCaseRow] = 0;  // (a button)
    }
}

bool save()
{
    File f;
    memset(&f, 0, sizeof(f));
    memcpy(f.magic, "PCS1", 4);
    f.version = 2;
    f.cheats = (u8) ((gV[0] ? 1 : 0) | (gV[1] ? 2 : 0) | (gV[2] ? 4 : 0) | (gV[3] ? 8 : 0));
    f.money = 0;  // (2026-11-27: a button)
    f.gyro = (u8) gV[5];
    f.fps = (u8) (gV[6] ? 1 : 0);
    f.stick = (u8) gV[kStickRow];
    f.picture = (u8) gV[kPictureRow];
    f.deadzone = (u8) gV[kDeadRow];
    f.tophud = (u8) (gV[kTopHudRow] ? 1 : 0);
    f.uilang = (u8) gV[kUiLangRow];
    f.more = (u8) (gV[kShopRow] ? 1 : 0);  // (2: the case XL - a button now)
    for (int i = 0; i < 4; i++) {
        f.wep[i] = re4_wep_slot[i];
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

// the game's thread: the title left for the gameplay
void take()
{
    const int bits = (gV[0] ? 1 : 0) | (gV[1] ? 2 : 0) | (gV[2] ? 4 : 0) | (gV[3] ? 8 : 0);
    re4_cheats_set((re4_cheats_wanted() & 16) | bits);  // (16: the extra games kept unlocked - not ours)
    re4_merchant_weapons = gV[kShopRow] ? 1 : 0;  // (2026-11-27; the pesetas and the case XL: buttons, at the touch)
    re4_3ds_gyro_set(gV[5]);
    re4_3ds_hud_fps(gV[6]);
    re4_3ds_stick_set(gV[kStickRow]);
    re4_3ds_gx_fill(gV[kPictureRow]);
    re4_log("3ds: settings taken by the game: cheats %d%s%s, gyro %d.%dx, frame counter %s, stick step %d, picture %d", bits,
            gMoneyGiven ? " (the pesetas given this launch)" : "", gV[kShopRow] ? ", all weapons at the merchant" : "",
            gV[5] / 10, gV[5] % 10, gV[6] ? "on" : "off", gV[kStickRow], gV[kPictureRow]);
}

// the game's thread, each frame (after hudSample: its hook came first)
void frame(unsigned)
{
    static int lastProg = -2;  // (2026-11-27) the pictures' indicator in the foot: drawn anew as it grows
    if (re4_3ds_icons_progress() != lastProg) {
        lastProg = re4_3ds_icons_progress();
        gChanged = true;
    }
    const bool title = re4_hud_title != 0;
    gSawTitle = gSawTitle || title;
    if (gInGame && title) {
        gInGame = false;
        gChanged = true;
    } else if (!gInGame && !title && re4_hud_life >= 0) {
        gInGame = true;
        gChanged = true;
        re4_3ds_controls_catch(-1);  // (2026-11-21: a binding half caught stays as it was)
        if (gSawTitle) {
            take();
        }
    }
    if (re4_hud_lang != gLastLang) {
        gLastLang = re4_hud_lang;
        gChanged = true;
    }
    // (2026-11-27) the game's option screen opened / closed: the settings shown / gone; closed - no catch left, the
    // tab's page back, a settings.bin write due goes out now
    const int option = gInGame && re4_hud_option ? 1 : 0;
    if (gPick >= 0 && !option && ++gPickWait > 4) {  // (2026-11-27: the option screen did not come - a movie, ...)
        gPick = -1;
    }
    if (!option) {
        re4_option_close = 0;  // (a close asked for lives while the option screen does)
    }
    if (gInGame && re4_wep_owned_ok && re4_wep_owned_n > 0) {  // (an arrow's weapon not in Leon's case any more - a
        bool gone = false;                                      // load, a new game, sold: taken off the arrow)
        for (int s = 0; s < 4; s++) {
            bool have = !re4_wep_slot[s];
            for (int i = 0; i < re4_wep_owned_n && i < 24 && !have; i++) {
                have = re4_wep_owned[i] == re4_wep_slot[s];
            }
            if (!have) {
                re4_log("3ds: the D-pad weapons: item 0x%x not in the case - off arrow %d", re4_wep_slot[s], s);
                re4_wep_slot[s] = 0;
                gone = true;
            }
        }
        if (gone) {
            gWriteAt = svcGetSystemTick() + SYSCLOCK_ARM11 / 2;
        }
    }
    if (option != gLastOption) {
        gLastOption = option;
        gChanged = true;
        gLit = -1;
        if (!option) {
            re4_3ds_controls_catch(-1);
            gSub = 0;
            gPick = -1;
            if (gWriteAt) {
                gWriteAt = svcGetSystemTick();
            }
        }
        re4_log("3ds: settings %s the game's option screen", option ? "shown over" : "gone with");
    }
}

}  // namespace

// settings_3ds.cpp re4_3ds_hud_start (before the game, the main thread): the file, the frame hook
extern "C" void re4_3ds_setup_start(void)
{
    // on by default, but not under a test's buttons (a replay, a pad script: the benches, the start in r100)
    const char* v = getenv("RE4_3DS_SETUP");
    const char* replay = getenv("RE4_PAD_REPLAY");
    const char* script = getenv("RE4_PAD_SCRIPT");
    const bool test = (replay && *replay) || (script && *script);
    gOn = v && *v ? *v != '0' : !test;
    re4_3ds_controls_start(gOn);  // (2026-11-21: the button mapping only with the settings screen)
    if (!gOn) {
        re4_log("3ds: settings screen off (%s)", v && *v ? "RE4_3DS_SETUP=0" : "a replay or a pad script");
        return;
    }
    load();
    if (const char* t = getenv("RE4_3DS_SETUPTAB")) {  // (2026-11-19, the stand: a tab open from the start, 0..2)
        gTab = atoi(t) < 0 || atoi(t) > 2 ? 0 : atoi(t);
        if (atoi(t) >= 3 && atoi(t) <= 6) {  // (2026-11-21) the button mapping's page 1, 2, 3 (3, 4, 6); 5: page 1, fire's
            gTab = 1, gSub = 1, gPage = atoi(t) == 4 ? 1 : atoi(t) == 6 ? 2 : 0;  // keys awaited
            if (atoi(t) == 5) {
                re4_3ds_controls_catch(0);  // (forward)
            }
        }
    }
    re4_3ds_gx_fill(gV[kPictureRow]);  // (2026-11-19: the picture at once - the title shows it)
    re4_3ds_stick_deadzone(gV[kDeadRow]);  // (2026-11-21: the dead zone at once, as the picture)
    applyScreen();  // (2026-11-27: the top HUD and the bottom screen's language at once)
    re4_add_frame_hook(frame);
    re4_log("3ds: settings screen until the gameplay: cheats %d%d%d%d, pesetas %d, merchant %d, gyro %d, "
            "frame counter %d, stick step %d, picture %d", gV[0], gV[1], gV[2], gV[3], gV[4], gV[kShopRow],
            gV[5], gV[6], gV[kStickRow], gV[kPictureRow]);
}

// (2026-11-27) the settings screen in use (not under a replay / a pad script): the gear asks for the option screen
extern "C" int re4_3ds_setup_on(void)
{
    return gOn ? 1 : 0;
}

// The panel's thread: the settings screen instead of the game's
extern "C" int re4_3ds_setup_visible(void)
{
    return gOn && (!gInGame || gLastOption);  // (2026-11-27: also over the game's option screen)
}

// The panel's thread: a new picture due since the last call
extern "C" int re4_3ds_setup_dirty(void)
{
    const int mapping = re4_3ds_controls_dirty();  // (2026-11-21: a binding caught, the page's state)
    if (!gChanged && !mapping) {
        return 0;
    }
    gChanged = false;
    return 1;
}

// (2026-11-27) the D-pad weapons' files' room wanted now (main_3ds.cpp at the start, movie_3ds.cpp after a movie):
// RE4_WEP_CACHE KB (3072; 0 never), only with a weapon on an arrow and the settings screen on (not under a replay)
extern "C" unsigned re4_3ds_weapon_room_bytes(void)
{
    const char* e = getenv("RE4_WEP_CACHE");
    const unsigned kb = e && *e ? (unsigned) atoi(e) : 3072u;
    const bool any = re4_wep_slot[0] || re4_wep_slot[1] || re4_wep_slot[2] || re4_wep_slot[3];
    return gOn && any ? kb << 10 : 0;
}

// (2026-11-27) the bottom screen's weapon cell touched (settings_3ds.cpp, the game's thread): the option screen asked
// for, the list of Leon's weapons over it for that arrow
extern "C" void re4_3ds_wep_pick(int slot)
{
    if (!gOn || slot < 0 || slot > 3) {
        return;
    }
    gPickN = re4_wep_owned_n < 24 ? re4_wep_owned_n : 24;
    for (int i = 0; i < gPickN; i++) {
        gPickIds[i] = re4_wep_owned[i];
    }
    gPickWait = 0;
    gPick = slot;
    gLit = -1;
    gChanged = true;
    re4_option_request();
}

extern "C" void re4_3ds_setup_draw(unsigned short* fb)
{
    if (gPick >= 0 && gLastOption) {  // (2026-11-27) the list for an arrow
        re4_ui_wep_pick(fb, gPick, gPickIds, gPickN, gLit, re4_wep_slot[gPick]);
        return;
    }
    if (gSub) {  // (2026-11-21) the button mapping's page
        unsigned char keys[kBtnActions][2];
        for (int a = 0; a < kBtnActions; a++) {
            re4_3ds_controls_binding(a, keys[a]);
        }
        re4_ui_buttons(fb, gPage, gLit, re4_3ds_controls_catching(), keys);
        return;
    }
    int v[kRows];
    memcpy(v, gV, sizeof(v));
    v[4] = gMoneyGiven ? 1 : gInGame ? 0 : 2;  // (2026-11-27) the buttons: 1 "Done", 2 not before the gameplay
    re4_ui_setup_live = gInGame ? 1 : 0;  // (2026-11-27: over the option screen - the changes at once)
    re4_ui_setup(fb, v, gLit, gTab);
}

// The panel's thread, each pass: the file half a second after the last change
extern "C" void re4_3ds_setup_poll(void)
{
    re4_3ds_controls_poll();  // (2026-11-21: controls.bin)
    re4_3ds_gyro_poll();  // (2026-11-21: also with the settings screen off - the panel's thread runs on)
    const u64 at = gWriteAt;
    if (at && svcGetSystemTick() >= at) {
        gWriteAt = 0;
        const bool ok = save();
        re4_log("3ds: settings: %s %s", kPath, ok ? "written" : "NOT written");
    }
}

// The game's thread (settings_3ds.cpp re4_3ds_hud_pad): phase 1 a new touch, 0 held, -1 none. 1: the settings screen
// is up and took it (nothing for the game: the title's pad stays the buttons')
extern "C" int re4_3ds_setup_touch(int phase, int x, int y)
{
    if (!re4_3ds_setup_visible()) {
        if (gLit >= 0) {
            gLit = -1;
        }
        return 0;
    }
    if (phase < 0) {
        if (gLit >= 0) {
            gLit = -1;
            gChanged = true;
        }
        return 1;
    }
    if (phase != 1) {
        return 1;
    }
    if (gPick >= 0) {  // (2026-11-27) the list for an arrow: a weapon, "None", or back
        const int r = re4_ui_wep_pick_hit(gPickN, x, y);
        if (r == -10) {
            gPick = -1;
        } else if (r >= 0 && r <= gPickN) {
            const unsigned char id = r < gPickN ? gPickIds[r] : 0;
            const unsigned char was = re4_wep_slot[gPick];
            for (int i = 0; i < 4; i++) {  // (one weapon on one arrow: the other arrow takes this one's old)
                if (i != gPick && id && re4_wep_slot[i] == id) {
                    re4_wep_slot[i] = was;
                }
            }
            re4_wep_slot[gPick] = id;
            re4_log("3ds: the D-pad weapons: arrow %d -> item 0x%x (up 0x%x, down 0x%x, left 0x%x, right 0x%x)", gPick,
                    id, re4_wep_slot[0], re4_wep_slot[1], re4_wep_slot[2], re4_wep_slot[3]);
            gPick = -1;
            gWriteAt = svcGetSystemTick() + SYSCLOCK_ARM11 / 2;
            re4_option_close = 1;  // (straight back to the game: the user's wish)
        }
        gLit = -1;
        gChanged = true;
        return 1;
    }
    if (gSub) {  // (2026-11-21) the button mapping's page: back, the other page, an action (catch its keys), the reset
        const int r = re4_ui_buttons_hit(gPage, x, y);
        if (r == -10 || r == -11) {
            re4_3ds_controls_catch(-1);
            if (r == -10) {
                gSub = 0;
            } else {
                gPage = (gPage + 1) % kBtnPages;  // (four pages: 2026-11-27, the D-pad weapons)
            }
            gLit = -1;
        } else if (r == kBtnReset) {
            re4_3ds_controls_catch(-1);
            re4_3ds_controls_reset();
            gLit = r;
        } else if (r >= 0 && r < kBtnActions) {
            re4_3ds_controls_catch(re4_3ds_controls_catching() == r ? -1 : r);  // (touched again: as it was)
            gLit = r;
        }
        gChanged = true;
        return 1;
    }
    int left = 0;
    const int row = re4_ui_setup_hit(gTab, x, y, &left);
    if (row == -20) {  // (2026-11-27) RESUME over the game's option screen: back to the game
        if (gInGame && gLastOption) {
            re4_option_close = 1;
            re4_log("3ds: settings: back to the game");
        }
        return 1;
    }
    if (row == kButtonsRow) {  // (2026-11-21) the link: the page opens
        gSub = 1;
        gPage = 0;
        gLit = -1;
        gChanged = true;
        return 1;
    }
    if (row <= -2) {  // (2026-11-19) a tab: opened
        gTab = -2 - row;
        gLit = -1;
        gChanged = true;
        return 1;
    }
    if (row < 0 || row >= kRows) {
        return 1;
    }
    if (row == 4) {  // (2026-11-27) the giving cheat: a button (given when Leon is next free)
        if (!gInGame) {
            return 1;  // (on the title: no Leon to give it to - the button dim)
        }
        if (row == 4 && !gMoneyGiven) {
            gMoneyGiven = true;
            re4_cheats_money_arm();
            re4_log("3ds: settings: +1 000 000 pesetas when Leon is free");
        }
        gLit = row;
        gChanged = true;
        return 1;
    }
    if (row == kGyroRow) {
        int g = gainIndex(gV[kGyroRow]);
        if (left) {
            g = g > 0 ? g - 1 : 0;  // the left arrow
        } else {
            g = (g + 1) % kGainCount;
        }
        gV[kGyroRow] = kGains[g];
    } else if (row == kStickRow) {  // (2026-11-18) the arrows as the gyro's: 2x -> 1x
        const int s = gV[kStickRow];
        gV[kStickRow] = left ? (s > 0 ? s - 1 : 0) : (s + 1) % kStickSteps;
    } else if (row == kPictureRow) {  // (2026-11-19) the same arrows: Zoom -> Fit
        const int s = gV[kPictureRow];
        gV[kPictureRow] = left ? (s > 0 ? s - 1 : 0) : (s + 1) % kPictureSteps;
    } else if (row == kUiLangRow) {  // (2026-11-27) the arrows as the picture's: 中文 -> Auto; at once
        const int s = gV[kUiLangRow];
        gV[kUiLangRow] = left ? (s > 0 ? s - 1 : 0) : (s + 1) % kUiLangSteps;
    } else if (row == kDeadRow) {  // (2026-11-21) the arrows by 5 %: 50 % -> off; at once
        const int s = gV[kDeadRow];
        gV[kDeadRow] = left ? (s >= kDeadStep ? s - kDeadStep : 0) : (s >= kDeadMax ? 0 : s + kDeadStep);
        re4_3ds_stick_deadzone(gV[kDeadRow]);
    } else {
        gV[row] = !gV[row];
    }
    gLit = row;
    gChanged = true;
    gWriteAt = svcGetSystemTick() + SYSCLOCK_ARM11 / 2;
    if (row == kPictureRow) {
        re4_3ds_gx_fill(gV[kPictureRow]);  // (2026-11-19: at once - seen on the title while it is chosen)
    }
    if (row == kTopHudRow || row == kUiLangRow) {
        applyScreen();  // (2026-11-27: at once)
    }
    if (gInGame) {
        take();  // (2026-11-27: over the game's option screen every row at once - the cheats, the gyro, ...)
    }
    re4_log("3ds: settings: row %d -> %d", row, gV[row]);
    return 1;
}
