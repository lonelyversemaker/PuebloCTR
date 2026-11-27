// Stage 9 (2026-11-08, the project owner's wish: "the bottom screen looks like Minecraft - it's a horror game"; their choice of
// the mockups: "B - the dark"): the game's bottom screen drawn from pictures made on the Mac (3ds/ui_assets.h by
// tools/ui_assets.py): the background by one copy, the text as coverage blended in its colour (every string
// in en fr de es it ja - Noto Serif JP, one face for all; ru until R0 2026-11-12), the numbers by glyphs, the heartbeats coloured by the life.
// By the panel's thread (settings_3ds.cpp drawGame -> re4_ui_game; the CPU, no GPU), into its picture of the screen.
//
// The language: RE4_3DS_LANG=en|fr|de|es|it|ja, else the game's own (pSys->language, runtime/cheats.cpp hudSample:
// 0 ja, 1/2/7 en, 3 de, 4 fr, 5 es, 6 it). RE4_3DS_UI=0 - the look of before (stage 8).
// Also the minimap's text (3ds/minimap_3ds.cpp): re4_ui_text, re4_ui_tpl, within re4_ui_clip.
// R2 (2026-11-12): the settings before the gameplay (3ds/setup_3ds.cpp) - re4_ui_setup.
#include <3ds.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_assets.h"
#include "inventory_share.h"  // (2026-11-27) the inventory
#include "hud_font.h"     // the panel's 8x8 font: the version
#include "re4_version.h"  // build3ds.py: RE4_3DS_VERSION

extern "C" {
// runtime/cheats.cpp hudSample (the game's thread, each frame)
extern volatile int re4_hud_life, re4_hud_life_max, re4_hud_ashley, re4_hud_ashley_max, re4_hud_ammo, re4_hud_reserve,
    re4_hud_peseta, re4_hud_lang;
int re4_3ds_minimap_draw(unsigned short* fb, int x, int y, int w, int h);
int re4_3ds_minimap_shot_due(void);  // (the stand: RE4_3DS_MAP_SHOT)
}

namespace {

constexpr u16 rgb(int r, int g, int b)
{
    return (u16) ((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

const u16 kBone = rgb(212, 204, 192), kBoneLit = rgb(240, 234, 224), kDim = rgb(122, 112, 104),
          kBtnText = rgb(160, 150, 140), kGold = rgb(196, 160, 90), kRed = rgb(200, 36, 28);

int gClip[4] = {0, 0, 320, 240};
u32 gCode[16];  // the codepoints of UI_DIGITS
int gCodes;

inline u16 blend(u16 d, u16 c, u32 a)
{
    if (a >= 250) {
        return c;
    }
    const u32 b = 256 - a;
    a += 1;
    const u32 r = ((d >> 11) * b + (c >> 11) * a) >> 8;
    const u32 g = (((d >> 5) & 63) * b + ((c >> 5) & 63) * a) >> 8;
    const u32 bl = ((d & 31) * b + (c & 31) * a) >> 8;
    return (u16) (r << 11 | g << 5 | bl);
}

// a piece's coverage in `c` with its baseline's left end at (x, base); the columns from `from` to `to` (of the piece) only
void blit(u16* fb, int piece, int x, int base, u16 c, int from = 0, int to = 1 << 16)
{
    const UiPiece& p = kUiPiece[piece];
    const int x0 = x + p.ox, y0 = base + p.oy;
    const u8* cov = kUiCov + p.off;
    for (int i = from < 0 ? 0 : from; i < p.w && i < to; i++) {
        const int sx = x0 + i;
        if (sx < gClip[0] || sx >= gClip[2]) {
            continue;
        }
        u16* col = fb + sx * 240 + 239;
        for (int j = 0; j < p.h; j++) {
            const u32 a = cov[j * p.w + i];
            const int sy = y0 + j;
            if (a && sy >= gClip[1] && sy < gClip[3]) {
                col[-sy] = blend(col[-sy], c, a);
            }
        }
    }
}

void fill(u16* fb, int x, int y, int w, int h, u16 c)
{
    for (int i = x; i < x + w; i++) {
        if (i < 0 || i >= 320) {
            continue;
        }
        for (int j = y; j < y + h; j++) {
            if (j >= 0 && j < 240) {
                fb[i * 240 + 239 - j] = c;
            }
        }
    }
}

}  // namespace
// (2026-11-27) the settings' choice of the bottom screen's language: 0 Auto (below), 1..UI_LANGS - that language
extern "C" {
volatile int re4_ui_lang_choice;
volatile int re4_ui_setup_live;  // (2026-11-27) the settings over the game's option screen: "Applied at once"
}
namespace {

int lang()
{
    if (re4_ui_lang_choice >= 1 && re4_ui_lang_choice <= UI_LANGS) {
        return re4_ui_lang_choice - 1;  // (ahead of RE4_3DS_LANG: the player's choice)
    }
    static int env = -2;
    if (env == -2) {
        env = -1;
        static const char* const kCodes[UI_LANGS] = {"en", "fr", "de", "es", "it", "ja", "chs"};  // (chs: the port's own, never pSys->language)
        if (const char* v = getenv("RE4_3DS_LANG")) {
            for (int i = 0; i < UI_LANGS; i++) {
                if (!strncmp(v, kCodes[i], 2)) {
                    env = i;
                }
            }
        }
    }
    if (env >= 0) {
        return env;
    }
    switch (re4_hud_lang) {  // (pSys->language)
    case 0:
        return UI_LANG_JA;
    case 3:
        return UI_LANG_DE;
    case 4:
        return UI_LANG_FR;
    case 5:
        return UI_LANG_ES;
    case 6:
        return UI_LANG_IT;
    }
    return UI_LANG_EN;
}

int glyph(int style, u32 code)
{
    if (!gCodes) {
        const unsigned char* s = (const unsigned char*) UI_DIGITS;
        while (*s && gCodes < 16) {
            u32 v = *s++;
            if ((v & 0xE0) == 0xC0) {
                v = (v & 0x1F) << 6 | (*s++ & 0x3F);
            } else if ((v & 0xF0) == 0xE0) {
                v = (v & 0x0F) << 12 | (s[0] & 0x3F) << 6 | (s[1] & 0x3F);
                s += 2;
            }
            gCode[gCodes++] = v;
        }
    }
    for (int i = 0; i < gCodes; i++) {
        if (gCode[i] == code) {
            return kUiDigit[style][i];
        }
    }
    return -1;
}

int numWidth(int style, const char* s)
{
    int w = 0;
    for (; *s; s++) {
        const int g = glyph(style, (u8) *s);
        w += g >= 0 ? kUiPiece[g].adv : 0;
    }
    return w;
}

// a number (ASCII digits, '/', ' ', '-') in a style; align 0 left, 1 centre, 2 right; its width
int number(u16* fb, int style, const char* s, int x, int base, int align, u16 c, bool shadow)
{
    const int w = numWidth(style, s);
    x -= align == 2 ? w : align == 1 ? w / 2 : 0;
    for (; *s; s++) {
        const int g = glyph(style, (u8) *s);
        if (g < 0) {
            continue;
        }
        if (shadow) {
            blit(fb, g, x + 1, base + 1, 0);
        }
        blit(fb, g, x, base, c);
        x += kUiPiece[g].adv;
    }
    return w;
}

int pieceText(u16* fb, int piece, int x, int base, int align, u16 c, bool shadow)
{
    const int w = kUiPiece[piece].adv;
    x -= align == 2 ? w : align == 1 ? w / 2 : 0;
    if (shadow) {
        blit(fb, piece, x + 1, base + 1, 0);
    }
    blit(fb, piece, x, base, c);
    return w;
}

// a heartbeat (UI_PULSE_*) from x, its columns from `skip` on (narrowed by a long label), bright up to the life's share
void heartbeat(u16* fb, int piece, int x, int mid, int skip, int life, int max)
{
    const int w = kUiPiece[piece].w;
    const float share = max > 0 ? (life <= 0 ? 0.0f : life >= max ? 1.0f : (float) life / max) : 0.0f;
    const u16 c = share > 0.5f ? rgb(70, 200, 90) : share > 0.25f ? rgb(220, 190, 60) : rgb(220, 40, 30);
    const u16 dimmed = share > 0.5f ? rgb(22, 52, 28) : share > 0.25f ? rgb(56, 48, 18) : rgb(60, 14, 12);
    const int cut = skip + (int) ((w - skip) * share + 0.5f);
    blit(fb, piece, x - skip, mid, c, skip, cut);  // (the piece's box: its middle row on the baseline)
    blit(fb, piece, x - skip, mid, dimmed, cut, w);
}

void groupThousands(int v, char* out)
{
    char t[16];
    const int n = snprintf(t, sizeof(t), "%d", v < 0 ? 0 : v);
    int o = 0;
    for (int i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0) {
            out[o++] = ' ';
        }
        out[o++] = t[i];
    }
    out[o] = 0;
}

}  // namespace

// --- for the minimap (3ds/minimap_3ds.cpp; the panel's thread) ----------------------------------------------------------

extern "C" void re4_ui_clip(int x0, int y0, int x1, int y1)
{
    gClip[0] = x0, gClip[1] = y0, gClip[2] = x1, gClip[3] = y1;
}

// a string of the screen (UI_* of ui_assets.h) in the language; align 0 left, 1 centre, 2 right; its width
extern "C" int re4_ui_text(unsigned short* fb, int id, int x, int base, int align, unsigned short c, int shadow)
{
    if (id < 0 || id >= UI_STRINGS) {
        return 0;
    }
    return pieceText(fb, kUiStr[lang()][id], x, base, align, c, shadow != 0);
}

// a template (UI_T_*) with its two numbers (in UI_DS_SMALL)
extern "C" int re4_ui_tpl(unsigned short* fb, int tpl, int a0, int a1, int x, int base, int align, unsigned short c,
                          int shadow)
{
    if (tpl < 0 || tpl >= UI_TEMPLATES) {
        return 0;
    }
    const int16_t* t = kUiTpl[lang()][tpl];
    char num[2][12];
    snprintf(num[0], sizeof(num[0]), "%d", a0);
    snprintf(num[1], sizeof(num[1]), "%d", a1);
    int w = 0;
    for (int k = 1; k <= t[0]; k++) {
        w += t[k] >= 0 ? kUiPiece[t[k]].adv : numWidth(UI_DS_SMALL, num[-t[k] - 1]);
    }
    x -= align == 2 ? w : align == 1 ? w / 2 : 0;
    for (int k = 1; k <= t[0]; k++) {
        if (t[k] >= 0) {
            x += pieceText(fb, t[k], x, base, 0, c, shadow != 0);
        } else {
            x += number(fb, UI_DS_SMALL, num[-t[k] - 1], x, base, 0, c, shadow != 0);
        }
    }
    return w;
}

// --- (2026-11-27) the D-pad weapons ---------------------------------------------------------------------------------------

extern "C" {
extern volatile unsigned char re4_wep_slot[4];  // runtime/cheats.cpp
extern volatile unsigned char re4_wep_owned[24];
extern volatile int re4_wep_owned_n, re4_wep_equipped;
}

// A case item's short name (UI_WN_*): the variants (a silencer, a stock, a scope) under their base's; the ids by
// game/item.cpp wep_info (the weapon numbers) and the weapon modules' own names
int re4_ui_wep_name(int id)
{
    switch (id) {
    case 0x23: case 0x24: return UI_WN_HANDGUN;
    case 0x25: case 0x26: return UI_WN_RED9;
    case 0x21: case 0x22: case 0x40: return UI_WN_PUNISHER;
    case 0x27: case 0x28: return UI_WN_BLACKTAIL;
    case 0x03: return UI_WN_MATILDA;
    case 0x29: return UI_WN_BUTTERFLY;
    case 0x2A: case 0x2B: return UI_WN_KILLER7;
    case 0x2C: return UI_WN_SHOTGUN;
    case 0x2D: return UI_WN_STRIKER;
    case 0x94: return UI_WN_RIOT;
    case 0x2E: case 0x6B: case 0x99: return UI_WN_RIFLE;
    case 0x2F: case 0x6C: case 0x51: return UI_WN_SEMI;
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x3E: return UI_WN_TMP;
    case 0x34: return UI_WN_CHICAGO;
    case 0x35: case 0x17: case 0x52: return UI_WN_ROCKET;
    case 0x6D: return UI_WN_ROCKETINF;  // (2026-11-27: 0x6D the endless one)
    case 0x36: case 0xAB: return UI_WN_MINE;
    case 0x37: return UI_WN_HANDCANNON;
    case 0x01: return UI_WN_GRENADE;
    case 0x02: return UI_WN_INCEND;
    case 0x0E: return UI_WN_FLASH;
    }
    return UI_WN_OTHER;
}

// --- (2026-11-27) the inventory: Leon's case instead of the map (3ds/inventory_share.h) ----------------------------------

extern "C" const unsigned char* re4_3ds_icon(int ppc, int id, int side, int* w, int* h);  // 3ds/icons_3ds.cpp
extern "C" int re4_3ds_icons_progress(void);
extern "C" volatile int re4_inv_view;     // the case button switches the map and it (settings_3ds.cpp)
extern "C" volatile int re4_inv_pressed;  // the item under the finger (its index), -1 none
volatile int re4_inv_view, re4_inv_pressed = -1;
// (the user: the whole screen above the strip) the lives' band 0..26, the case below it up to 205: a cell as large as
// the case allows - the sizes of icons_3ds.cpp's files
const int kInvTop = 27, kInvBottom = 205;

static int invCell(const Re4Inv& v)
{
    return v.cols >= 15 ? 20 : v.cols >= 12 ? 22 : v.cols >= 11 ? 25 : 29;
}

static void invOrigin(const Re4Inv& v, int* x0, int* y0)
{
    const int c = invCell(v);
    *x0 = 160 - v.cols * c / 2;
    *y0 = kInvTop + (kInvBottom - kInvTop - v.rows * c) / 2;
}

// What is under (x, y): the index of the item there (-1 none), its place and id
extern "C" int re4_ui_inv_hit(int x, int y, int* slot, int* id)
{
    static Re4Inv v;
    if (!re4_inv_copy(&v)) {
        return -1;
    }
    int ox, oy;
    invOrigin(v, &ox, &oy);
    const int c = invCell(v);
    if (x < ox || y < oy) {
        return -1;
    }
    const int cx = (x - ox) / c, cy = (y - oy) / c;
    for (int i = 0; i < v.n; i++) {
        const Re4InvItem& e = v.it[i];
        if (!(e.flags & 16) && cx >= e.x0 && cx < e.x0 + e.w && cy >= e.y0 && cy < e.y0 + e.h) {
            *slot = e.slot, *id = e.id;
            return i;
        }
    }
    return -1;
}

// a picture w x h (RGB565, then alpha) turned `rot` quarter turns clockwise, its top-left at (dx, dy)
static void invBlit(u16* fb, const unsigned char* pic, int sw, int sh, int rot, int dx, int dy)
{
    const unsigned char* al = pic + sw * sh * 2;
    const int dw = rot & 1 ? sh : sw, dh = rot & 1 ? sw : sh;
    for (int y = 0; y < dh; y++) {
        const int py = dy + y;
        if (py < 0 || py >= 240) {
            continue;
        }
        for (int x = 0; x < dw; x++) {
            const int px = dx + x;
            if (px < 0 || px >= 320) {
                continue;
            }
            int sx, sy;
            switch (rot & 3) {
            case 0: sx = x, sy = y; break;
            case 1: sx = y, sy = sh - 1 - x; break;
            case 2: sx = sw - 1 - x, sy = sh - 1 - y; break;
            default: sx = sw - 1 - y, sy = x; break;
            }
            const int a = al[sy * sw + sx];
            if (!a) {
                continue;
            }
            const u16 s = (u16) (pic[2 * (sy * sw + sx)] | pic[2 * (sy * sw + sx) + 1] << 8);
            u16& d = fb[px * 240 + 239 - py];
            if (a >= 250) {
                d = s;
                continue;
            }
            const int r = ((s >> 11) * a + (d >> 11) * (255 - a)) / 255;
            const int g = (((s >> 5) & 63) * a + ((d >> 5) & 63) * (255 - a)) / 255;
            const int b = ((s & 31) * a + (d & 31) * (255 - a)) / 255;
            d = (u16) (r << 11 | g << 5 | b);
        }
    }
}

// a picture w x h (RGB565, then alpha) scaled into dw x dh at (dx, dy): each pixel the mean of its source box, the
// colour weighted by the coverage (2026-11-27: the D-pad weapons' list)
static void invBlitScaled(u16* fb, const unsigned char* pic, int sw, int sh, int dx, int dy, int dw, int dh)
{
    const unsigned char* al = pic + sw * sh * 2;
    for (int y = 0; y < dh; y++) {
        const int sy0 = y * sh / dh, sy1 = (y + 1) * sh / dh > sy0 ? (y + 1) * sh / dh : sy0 + 1;
        for (int x = 0; x < dw; x++) {
            const int sx0 = x * sw / dw, sx1 = (x + 1) * sw / dw > sx0 ? (x + 1) * sw / dw : sx0 + 1;
            int ar = 0, ag = 0, ab = 0, aa = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                for (int sx = sx0; sx < sx1; sx++, n++) {
                    const int a = al[sy * sw + sx];
                    const u16 s = (u16) (pic[2 * (sy * sw + sx)] | pic[2 * (sy * sw + sx) + 1] << 8);
                    ar += (s >> 11) * a, ag += ((s >> 5) & 63) * a, ab += (s & 31) * a, aa += a;
                }
            }
            if (!aa) {
                continue;
            }
            const int px = dx + x, py = dy + y;
            if (px < 0 || px >= 320 || py < 0 || py >= 240) {
                continue;
            }
            const int a = aa / n;
            u16& d = fb[px * 240 + 239 - py];
            const int r = (ar / aa * a + (d >> 11) * (255 - a)) / 255;
            const int g = (ag / aa * a + ((d >> 5) & 63) * (255 - a)) / 255;
            const int b = (ab / aa * a + (d & 31) * (255 - a)) / 255;
            d = (u16) (r << 11 | g << 5 | b);
        }
    }
}

// a life as a thin bar (its share of the most, red under a quarter) and its number
static void invLife(u16* fb, int y, int life, int most, u16 colour)
{
    const int x0 = 74, w = 150;  // (2026-11-27: EDIT at their right)
    const int f = most > 0 ? (life * w / most > w ? w : life * w / most < 0 ? 0 : life * w / most) : 0;
    fill(fb, x0, y, w, 5, rgb(40, 30, 30));
    fill(fb, x0, y, f, 5, life * 4 < most ? rgb(200, 36, 28) : colour);
    char t[12];
    snprintf(t, sizeof(t), "%d", life);
    number(fb, UI_DS_SMALL, t, 256, y + 6, 2, kDim, false);
}

static void invEditDraw(u16* fb, const Re4Inv& v, int ox, int oy, int c);  // (2026-11-27: below)

static void invDraw(u16* fb)
{
    fill(fb, 0, 0, 320, kInvBottom + 1, rgb(16, 12, 12));
    const int lg = lang();
    if (re4_hud_life >= 0) {  // the band: LEON, his bar; Ashley's under it when she is there
        const bool two = re4_hud_ashley >= 0;  // (the user: both named, both green)
        pieceText(fb, kUiStr[lg][UI_LEON], 12, two ? 11 : 17, 0, kBone, true);
        invLife(fb, two ? 5 : 9, re4_hud_life, re4_hud_life_max, rgb(70, 170, 70));
        if (two) {
            pieceText(fb, kUiStr[lg][UI_ASHLEY], 12, 23, 0, kBone, true);
            invLife(fb, 17, re4_hud_ashley, re4_hud_ashley_max, rgb(70, 170, 70));
        }
    }
    static Re4Inv v;
    if (!re4_inv_copy(&v) || v.n == 0) {
        pieceText(fb, kUiStr[lg][UI_W_EMPTY], 160, (kInvTop + kInvBottom) / 2 + 4, 1, kDim, false);
        return;
    }
    int ox, oy;
    invOrigin(v, &ox, &oy);
    const int c = invCell(v), W = v.cols * c, H = v.rows * c;
    fill(fb, ox - 2, oy - 2, W + 4, H + 4, rgb(52, 40, 36));
    fill(fb, ox, oy, W, H, rgb(10, 8, 8));
    for (int k = 1; k < v.cols; k++) {
        fill(fb, ox + k * c, oy, 1, H, rgb(30, 26, 25));
    }
    for (int k = 1; k < v.rows; k++) {
        fill(fb, ox, oy + k * c, W, 1, rgb(30, 26, 25));
    }
    bool missing = false;
    for (int i = 0; i < v.n; i++) {
        const Re4InvItem& e = v.it[i];
        if (e.flags & 16) {
            continue;  // (2026-11-27: on the spare board - the edit mode's tray)
        }
        const int x = ox + e.x0 * c, y = oy + e.y0 * c, w = e.w * c, h = e.h * c;
        if (re4_inv_pressed == i) {
            fill(fb, x, y, w, h, rgb(70, 46, 42));
        }
        int pw = 0, ph = 0;
        const unsigned char* pic = re4_3ds_icon(c, e.id, e.orient > 3 ? 1 : 0, &pw, &ph);
        const int rot = e.orient & 3;
        if (pic && (rot & 1 ? ph : pw) == w && (rot & 1 ? pw : ph) == h) {
            invBlit(fb, pic, pw, ph, rot, x, y);
        } else {  // (no picture yet: a box)
            fill(fb, x + 2, y + 2, w - 4, h - 4, rgb(48, 40, 38));
            missing = true;
        }
        if (e.flags & 1) {  // the equipped weapon
            const u16 r = rgb(200, 40, 30);
            fill(fb, x, y, w, 1, r), fill(fb, x, y + h - 1, w, 1, r), fill(fb, x, y, 1, h, r), fill(fb, x + w - 1, y, 1, h, r);
        }
        if (e.count >= 0) {
            char t[8];
            snprintf(t, sizeof(t), "%d", e.count);
            const int ds = c >= 25 ? UI_DS_MID : UI_DS_SMALL, tw = numWidth(ds, t), th = c >= 25 ? 10 : 8;
            fill(fb, x + w - 3 - tw, y + h - 2 - th, tw + 3, th + 2, 0);  // (the user: on black, as the game's case)
            number(fb, ds, t, x + w - 2, y + h - 2, 2,
                   e.count == 0 && (e.flags & 2) ? kRed : kBoneLit, true);
        }
    }
    invEditDraw(fb, v, ox, oy, c);  // (2026-11-27: the edits)
    const int prog = re4_3ds_icons_progress();
    if (missing && prog >= 0) {  // (the first start: the pictures being made)
        char t[8];
        snprintf(t, sizeof(t), "%d%%", prog);
        const int tw = kUiPiece[kUiStr[lg][UI_I_PREP]].adv;
        fill(fb, 160 - tw / 2 - 30, kInvBottom - 18, tw + 60, 16, rgb(16, 12, 12));
        pieceText(fb, kUiStr[lg][UI_I_PREP], 160 - 20, kInvBottom - 6, 1, kBtnText, false);
        number(fb, UI_DS_SMALL, t, 160 + tw / 2 + 4, kInvBottom - 6, 0, kGold, false);
    }
}

// --- (2026-11-27) the inventory's edits: EDIT in the band; select, drag, drop onto an item; ROTATE / DISCARD in the strip

extern "C" volatile int re4_inv_edit;      // the edit mode
extern "C" volatile int re4_inv_sel;       // the selected item ((slot + 1) << 16 | id), 0 none
extern "C" volatile int re4_inv_ask;       // DISCARD asked: YES / NO in the strip
extern "C" volatile int re4_inv_drag;      // the dragged item (its key), 0 none
extern "C" volatile int re4_inv_ghost;     // where it would go: 1 << 31 | status << 24 | orient << 16 | (y + 32) << 8 | (x + 32)
extern "C" volatile int re4_inv_target;    // the item it combines with (its key)
extern "C" volatile int re4_inv_refused;   // the item whose edit the game refused (a red line until the next touch)
extern "C" volatile int re4_inv_trayturn;  // the tray's selected item: quarter turns asked by ROTATE (its drop's pose)
volatile int re4_inv_edit, re4_inv_sel, re4_inv_ask, re4_inv_trayturn;  // (the other four: runtime/cheats.cpp - the game's library defines what both use)
enum { kGhostFits = 1, kGhostCombine = 2, kGhostNo = 3, kGhostTray = 4 };
const int kEditX = 262, kEditY = 3, kEditW = 52, kEditH = 21;              // the band's button
const int kTrayX = 6, kTrayY = 2, kTrayW = 250, kTrayH = 23;                // (the user) the tray: the game's spare board
const int kStripL = 8, kStripW = 114, kStripR = 124, kStripEnd = 238;       // the strip: its two buttons
const int kAskYes = 104, kAskNo = 172, kAskW = 64;                          //   the question's buttons

static int invKey(const Re4InvItem& e)
{
    return (e.slot + 1) << 16 | e.id;
}

static int invFind(const Re4Inv& v, int key)
{
    for (int i = 0; key && i < v.n; i++) {
        if (invKey(v.it[i]) == key) {
            return i;
        }
    }
    return -1;
}

static int floorDiv(int a, int c)
{
    return a >= 0 ? a / c : -((c - 1 - a) / c);
}

static int invNextOrient(int o)
{
    return o <= 3 ? (o + 1) & 3 : 4 + ((o - 3) & 3);  // (a quarter clockwise, as the game's pzlPiece::rotate(0))
}

// (the user: a long weapon set aside lies upright on the 6 cells wide spare board) the pose a tray's item is dragged back
// with: lying (wider than high) unless ROTATE turned it; *w / *h its cells in that pose
static int trayOrient(const Re4InvItem& e, int* w, int* h)
{
    int o = e.orient & 7, ww = e.w, hh = e.h;
    // (upright: three quarters clockwise = one back - the spare board took it a quarter clockwise; one forward turned it
    // upside down, the user's shot of invedit4)
    int turns = (e.h > e.w ? 3 : 0) + ((((e.slot + 1) << 16 | e.id) == re4_inv_sel) ? re4_inv_trayturn : 0);
    for (int k = 0; k < (turns & 3); k++) {
        o = invNextOrient(o);
        const int t = ww;
        ww = hh, hh = t;
    }
    *w = ww, *h = hh;
    return o;
}

// invBlitScaled with the picture turned `rot` quarter turns clockwise (as invBlit) into dw x dh
static void invBlitScaledRot(u16* fb, const unsigned char* pic, int sw, int sh, int rot, int dx, int dy, int dw, int dh)
{
    const unsigned char* al = pic + sw * sh * 2;
    const int rw = rot & 1 ? sh : sw, rh = rot & 1 ? sw : sh;
    for (int y = 0; y < dh; y++) {
        const int ry0 = y * rh / dh, ry1 = (y + 1) * rh / dh > ry0 ? (y + 1) * rh / dh : ry0 + 1;
        for (int x = 0; x < dw; x++) {
            const int rx0 = x * rw / dw, rx1 = (x + 1) * rw / dw > rx0 ? (x + 1) * rw / dw : rx0 + 1;
            int ar = 0, ag = 0, ab = 0, aa = 0, n = 0;
            for (int ry = ry0; ry < ry1; ry++) {
                for (int rx = rx0; rx < rx1; rx++, n++) {
                    int sx, sy;
                    switch (rot & 3) {
                    case 0: sx = rx, sy = ry; break;
                    case 1: sx = ry, sy = sh - 1 - rx; break;
                    case 2: sx = sw - 1 - rx, sy = sh - 1 - ry; break;
                    default: sx = sw - 1 - ry, sy = rx; break;
                    }
                    const int a = al[sy * sw + sx];
                    const u16 c = (u16) (pic[2 * (sy * sw + sx)] | pic[2 * (sy * sw + sx) + 1] << 8);
                    ar += (c >> 11) * a, ag += ((c >> 5) & 63) * a, ab += (c & 31) * a, aa += a;
                }
            }
            const int px = dx + x, py = dy + y;
            if (!aa || px < 0 || px >= 320 || py < 0 || py >= 240) {
                continue;
            }
            const int a = aa / n;
            u16& d = fb[px * 240 + 239 - py];
            const int r = (ar / aa * a + (d >> 11) * (255 - a)) / 255;
            const int g = (ag / aa * a + ((d >> 5) & 63) * (255 - a)) / 255;
            const int b = (ab / aa * a + (d & 31) * (255 - a)) / 255;
            d = (u16) (r << 11 | g << 5 | b);
        }
    }
}

// the tray's thumbnails (the items on the spare board, flag 16) in the pose they are dragged back with, at most 19 px
// high and 56 wide, left to right; their indices and boxes, the count shown
static int trayLayout(const Re4Inv& v, int* idx, int (*box)[4], int max)
{
    int n = 0, x = kTrayX + 4;
    for (int i = 0; i < v.n && n < max; i++) {
        const Re4InvItem& e = v.it[i];
        if (!(e.flags & 16)) {
            continue;
        }
        int tw, th;
        trayOrient(e, &tw, &th);
        const int pw = tw * 20, ph = th * 20;
        int dh = 19, dw = pw * 19 / ph;
        if (dw > 56) {
            dw = 56, dh = ph * 56 / pw;
        }
        if (x + dw > kTrayX + kTrayW - 2) {
            break;
        }
        idx[n] = i, box[n][0] = x, box[n][1] = kTrayY + (kTrayH - dh) / 2, box[n][2] = dw, box[n][3] = dh;
        n++, x += dw + 5;
    }
    return n;
}

static bool invCombines(const Re4InvItem& d, const Re4InvItem& t)
{
    if ((d.type == 2 && t.type == 1 && t.ammo == d.id) || (t.type == 2 && d.type == 1 && d.ammo == t.id)) {
        return true;  // (the user: ammo onto its weapon loads it)
    }
    if (d.id == t.id && (d.type == 2 || d.type == 3)) {
        return true;  // (a stack onto its like)
    }
    return re4_inv_table_combine(d.id, t.id) != 0;
}

// where the dragged item `d` would lie at (gx, gy): fits / combines with one item (*target) / not there
static int invGhostStatus(const Re4Inv& v, int d, int gx, int gy, int gw, int gh, int* target)
{
    const Re4InvItem& e = v.it[d];
    *target = 0;
    if (gx < 0 || gy < 0 || gx + gw > v.cols || gy + gh > v.rows) {
        return kGhostNo;
    }
    int over = -1, overs = 0;
    for (int i = 0; i < v.n; i++) {
        const Re4InvItem& o = v.it[i];
        if (i == d || (o.flags & 16) || gx >= o.x0 + o.w || o.x0 >= gx + gw || gy >= o.y0 + o.h || o.y0 >= gy + gh) {
            continue;
        }
        overs += over != i, over = i;
    }
    if (overs == 0) {
        return kGhostFits;
    }
    if (overs == 1 && invCombines(e, v.it[over])) {
        *target = invKey(v.it[over]);
        return kGhostCombine;
    }
    return kGhostNo;
}

static void invAsk(int kind, int a, int b, int x0, int y0, int orient)
{
    re4_inv_op_req.kind = kind, re4_inv_op_req.a = a, re4_inv_op_req.b = b;
    re4_inv_op_req.x0 = x0, re4_inv_op_req.y0 = y0, re4_inv_op_req.orient = orient;
    re4_inv_op_seq = re4_inv_op_seq + 1;
}

static void invEditReset()
{
    re4_inv_edit = 0, re4_inv_sel = 0, re4_inv_ask = 0, re4_inv_drag = 0, re4_inv_ghost = 0, re4_inv_target = 0;
    re4_inv_refused = 0;
}

static int ghostPack(int st, int orient, int gx, int gy)
{
    if (gx < -32 || gx > 31 || gy < -32 || gy > 31) {
        return st == kGhostTray ? (int) (1u << 31 | (unsigned) st << 24) : 0;
    }
    return (int) (1u << 31 | (unsigned) st << 24 | (unsigned) orient << 16 | (unsigned) (gy + 32) << 8 | (unsigned) (gx + 32));
}

// A touch while the inventory is shown (settings_3ds.cpp re4_3ds_hud_pad, the game's thread); 1 - taken
extern "C" int re4_ui_inv_edit_touch(int phase, int x, int y)
{
    static Re4Inv v;
    static bool down, dragging, fromTray;
    static int downKey, downCx, downCy, downX, downY;
    if (!re4_inv_view) {
        if (re4_inv_edit) {
            invEditReset();
        }
        down = dragging = false;
        return 0;
    }
    if (phase == 1) {
        re4_inv_refused = 0;
        if (x >= kEditX && x < kEditX + kEditW && y < kInvTop) {  // EDIT / DONE
            const bool on = !re4_inv_edit;
            invEditReset();
            re4_inv_edit = on;
            down = dragging = false;
            return 1;
        }
        if (!re4_inv_edit) {
            return 0;
        }
        if (y >= UI_BTN_Y && x < kStripEnd) {  // the strip
            if (re4_inv_ask) {
                if (x >= kAskYes && x < kAskYes + kAskW) {
                    invAsk(RE4_INV_DISCARD, re4_inv_sel, 0, 0, 0, 0);
                    re4_inv_ask = 0, re4_inv_sel = 0;
                } else if (x >= kAskNo) {
                    re4_inv_ask = 0;
                }
            } else if (re4_inv_sel && re4_inv_copy(&v)) {
                const int i = invFind(v, re4_inv_sel);
                if (i >= 0 && x < kStripL + kStripW && (v.it[i].flags & 16)) {
                    re4_inv_trayturn = (re4_inv_trayturn + 1) & 3;  // (the tray: the pose of its drop, the game untouched)
                } else if (i >= 0 && x < kStripL + kStripW) {
                    invAsk(RE4_INV_ROTATE, re4_inv_sel, 0, 0, 0, 0);
                } else if (i >= 0 && x >= kStripR && (v.it[i].flags & 8)) {
                    re4_inv_ask = 1;
                }
            }
            return 1;
        }
        if (y >= kInvBottom) {
            return 0;  // (the case's button and the gear as ever)
        }
        re4_inv_ask = 0;
        down = dragging = fromTray = false;
        if (!re4_inv_copy(&v)) {
            return 1;
        }
        if (y < kInvTop) {  // the tray
            int idx[16], box[16][4];
            const int n = trayLayout(v, idx, box, 16);
            for (int k = 0; k < n; k++) {
                if (x >= box[k][0] - 2 && x < box[k][0] + box[k][2] + 2) {
                    down = fromTray = true, downKey = invKey(v.it[idx[k]]), downX = x, downY = y;
                    break;
                }
            }
        } else {
            int ox, oy;
            invOrigin(v, &ox, &oy);
            const int c = invCell(v), cx = floorDiv(x - ox, c), cy = floorDiv(y - oy, c);
            for (int i = 0; i < v.n; i++) {
                const Re4InvItem& e = v.it[i];
                if (!(e.flags & 16) && cx >= e.x0 && cx < e.x0 + e.w && cy >= e.y0 && cy < e.y0 + e.h) {
                    down = true, downKey = invKey(e), downCx = cx, downCy = cy, downX = x, downY = y;
                    break;
                }
            }
        }
        if (!down) {
            re4_inv_sel = 0;
        }
        return 1;
    }
    if (!re4_inv_edit || !down) {
        return 0;
    }
    if (phase == 0) {  // held: a drag past 6 px
        if (!dragging && abs(x - downX) + abs(y - downY) > 6) {
            dragging = true;
            if (re4_inv_sel != downKey) {
                re4_inv_trayturn = 0;
            }
            re4_inv_drag = downKey, re4_inv_sel = downKey;
        }
        if (dragging) {
            re4_inv_copy(&v);
            const int d = invFind(v, downKey);
            if (d < 0) {
                return 1;
            }
            const Re4InvItem& e = v.it[d];
            int target = 0, st, gx = 0, gy = 0, gw = e.w, gh = e.h, go = e.orient;
            if (y < kInvTop) {  // over the tray: aside (a case's item), nothing (the tray's own)
                st = fromTray ? 0 : kGhostTray;
            } else {
                int ox, oy;
                invOrigin(v, &ox, &oy);
                const int c = invCell(v);
                if (fromTray) {  // (its middle under the finger, in the pose of trayOrient)
                    go = trayOrient(e, &gw, &gh);
                    gx = floorDiv(x - ox - gw * c / 2 + c / 2, c), gy = floorDiv(y - oy - gh * c / 2 + c / 2, c);
                    st = invGhostStatus(v, d, gx, gy, gw, gh, &target);
                } else {
                    gx = e.x0 + floorDiv(x - ox, c) - downCx, gy = e.y0 + floorDiv(y - oy, c) - downCy;
                    st = gx == e.x0 && gy == e.y0 ? 0 : invGhostStatus(v, d, gx, gy, gw, gh, &target);
                }
            }
            re4_inv_ghost = ghostPack(st, go, gx, gy), re4_inv_target = target;
        }
        return 1;
    }
    // released: a drop, or a tap's selection
    down = false;
    if (!dragging) {
        re4_inv_sel = re4_inv_sel == downKey ? 0 : downKey;
        re4_inv_trayturn = 0;
        return 1;
    }
    dragging = false;
    const int g = re4_inv_ghost, st = (g >> 24) & 7;
    if (g && st == kGhostFits) {
        invAsk(RE4_INV_MOVE, downKey, 0, (g & 0xFF) - 32, ((g >> 8) & 0xFF) - 32, (g >> 16) & 7);
    } else if (g && st == kGhostCombine) {
        invAsk(RE4_INV_COMBINE, downKey, re4_inv_target, 0, 0, 0);
    } else if (g && st == kGhostTray) {
        invAsk(RE4_INV_TRAY, downKey, 0, 0, 0, 0);
    } else {
        re4_inv_ghost = 0, re4_inv_drag = 0, re4_inv_target = 0;
        if (g && st == kGhostNo) {
            re4_inv_refused = downKey;
        }
    }
    return 1;
}

static void shade(u16* fb, int x, int y, int w, int h)
{
    for (int i = x; i < x + w; i++) {
        for (int j = y; j < y + h; j++) {
            if (i >= 0 && i < 320 && j >= 0 && j < 240) {
                u16& d = fb[i * 240 + 239 - j];
                d = (u16) ((d >> 1) & 0x7BEF);
            }
        }
    }
}

static void frame(u16* fb, int x, int y, int w, int h, int t, u16 c)
{
    fill(fb, x, y, w, t, c), fill(fb, x, y + h - t, w, t, c), fill(fb, x, y, t, h, c), fill(fb, x + w - t, y, t, h, c);
}

static void slab(u16* fb, int x, int w, bool lit)
{
    const int y = UI_BTN_Y, h = UI_BTN_H;
    fill(fb, x, y, w, h, lit ? rgb(40, 30, 30) : rgb(22, 20, 20));
    frame(fb, x, y, w, h, 1, rgb(48, 44, 42));
    fill(fb, x + 1, y + h - 3, w - 2, 2, lit ? rgb(170, 22, 18) : rgb(90, 10, 10));
}

// the band's EDIT / DONE (a gold count of the tray's items on it), in the edit mode the tray instead of the lives, the
// selection, the dragged item's ghost and the refused one (after the items, the case's origin (ox, oy) and its cell c)
static void invEditDraw(u16* fb, const Re4Inv& v, int ox, int oy, int c)
{
    const int lg = lang();
    const bool on = re4_inv_edit;
    int idx[16], box[16][4], trays = 0;
    for (int i = 0; i < v.n; i++) {
        trays += (v.it[i].flags & 16) ? 1 : 0;
    }
    fill(fb, kEditX, kEditY, kEditW, kEditH, on ? rgb(40, 30, 30) : rgb(22, 20, 20));
    frame(fb, kEditX, kEditY, kEditW, kEditH, 1, rgb(48, 44, 42));
    fill(fb, kEditX + 1, kEditY + kEditH - 3, kEditW - 2, 2, on ? rgb(170, 22, 18) : rgb(90, 10, 10));
    pieceText(fb, kUiStr[lg][on ? UI_I_DONE : UI_I_EDIT], kEditX + kEditW / 2, kEditY + 14, 1, on ? kBoneLit : kBtnText, false);
    if (!on && trays) {
        char t[4];
        snprintf(t, sizeof(t), "%d", trays > 9 ? 9 : trays);
        number(fb, UI_DS_SMALL, t, kEditX + kEditW - 2, kEditY + 8, 2, kGold, true);
    }
    const int g = re4_inv_ghost, st = g ? (g >> 24) & 7 : 0;
    const int sel = invFind(v, re4_inv_sel), drag = invFind(v, re4_inv_drag), tgt = invFind(v, re4_inv_target);
    const int ref = invFind(v, re4_inv_refused);
    if (on) {  // the tray
        fill(fb, 0, 0, kEditX - 2, kInvTop, rgb(16, 12, 12));
        fill(fb, kTrayX, kTrayY, kTrayW, kTrayH, rgb(10, 8, 8));
        frame(fb, kTrayX - 1, kTrayY - 1, kTrayW + 2, kTrayH + 2, 1, st == kGhostTray ? rgb(70, 200, 90) : rgb(52, 40, 36));
        const int n = trayLayout(v, idx, box, 16);
        for (int k = 0; k < n; k++) {
            const Re4InvItem& e = v.it[idx[k]];
            int pw = 0, ph = 0, tw, th;
            const int to = trayOrient(e, &tw, &th);
            // (the case's own size: another size's file would be opened anew at every picture - read from the SD each
            // time, the game waiting on it)
            const unsigned char* pic = re4_3ds_icon(c, e.id, to > 3 ? 1 : 0, &pw, &ph);
            if (pic) {
                invBlitScaledRot(fb, pic, pw, ph, to & 3, box[k][0], box[k][1], box[k][2], box[k][3]);
            } else {
                fill(fb, box[k][0], box[k][1], box[k][2], box[k][3], rgb(48, 40, 38));
            }
            if (idx[k] == drag && g) {
                shade(fb, box[k][0], box[k][1], box[k][2], box[k][3]);
            } else if (idx[k] == sel || idx[k] == ref) {
                frame(fb, box[k][0] - 2, box[k][1] - 2, box[k][2] + 4, box[k][3] + 4, 1,
                      idx[k] == ref ? kRed : re4_inv_ask ? kRed : kGold);
            }
        }
    }
    if (ref >= 0 && !(v.it[ref].flags & 16)) {
        const Re4InvItem& e = v.it[ref];
        frame(fb, ox + e.x0 * c, oy + e.y0 * c, e.w * c, e.h * c, 2, kRed);
    }
    if (!on) {
        return;
    }
    if (sel >= 0 && sel != drag && !(v.it[sel].flags & 16)) {
        const Re4InvItem& e = v.it[sel];
        frame(fb, ox + e.x0 * c, oy + e.y0 * c, e.w * c, e.h * c, 2, re4_inv_ask ? kRed : kGold);
    }
    if (drag < 0 || !g) {
        return;
    }
    const Re4InvItem& e = v.it[drag];
    if (!(e.flags & 16)) {
        shade(fb, ox + e.x0 * c, oy + e.y0 * c, e.w * c, e.h * c);
    }
    if (st == kGhostTray || st == 0 && (e.flags & 16)) {
        return;
    }
    const int gx = ox + ((g & 0xFF) - 32) * c, gy = oy + (((g >> 8) & 0xFF) - 32) * c;
    if (tgt >= 0) {
        const Re4InvItem& t = v.it[tgt];
        frame(fb, ox + t.x0 * c, oy + t.y0 * c, t.w * c, t.h * c, 2, kGold);
    }
    const int go = (g >> 16) & 7, same = (go & 1) == (e.orient & 1);
    int pw = 0, ph = 0;
    const unsigned char* pic = re4_3ds_icon(c, e.id, go > 3 ? 1 : 0, &pw, &ph);
    const int rot = go & 3, w = (same ? e.w : e.h) * c, h = (same ? e.h : e.w) * c;
    if (pic && (rot & 1 ? ph : pw) == w && (rot & 1 ? pw : ph) == h) {
        invBlit(fb, pic, pw, ph, rot, gx, gy);
    }
    const u16 col = st == kGhostFits ? rgb(70, 200, 90) : st == kGhostCombine ? kGold : st == kGhostNo ? kRed : kBtnText;
    frame(fb, gx, gy, w, h, 2, col);
}

// the strip in the edit mode: the hint, or ROTATE / DISCARD for the selected item, or the discard's question
static void invEditStrip(u16* fb, int lg)
{
    for (int px = kStripL - 2; px < kStripEnd + 2; px++) {  // (the cells' slabs: the column's colour above the strip)
        const u16 bg = kUiBg[px * 240 + 239 - (UI_BTN_Y - 2)];
        for (int py = UI_BTN_Y - 1; py <= UI_BTN_Y + UI_BTN_H; py++) {
            fb[px * 240 + 239 - py] = bg;
        }
    }
    static Re4Inv v;
    const int sel = re4_inv_copy(&v) ? invFind(v, re4_inv_sel) : -1;
    const int mid = UI_BTN_Y + (UI_BTN_H - 3) / 2;
    if (sel < 0) {
        pieceText(fb, kUiStr[lg][UI_I_HINT], (kStripL + kStripEnd) / 2, mid + 4, 1, kDim, false);
        return;
    }
    if (re4_inv_ask) {
        pieceText(fb, kUiStr[lg][UI_I_DISCARD], (kStripL + kAskYes) / 2, mid + 4, 1, kBone, false);
        slab(fb, kAskYes, kAskW, true);
        pieceText(fb, kUiStr[lg][UI_I_YES], kAskYes + kAskW / 2, mid + 5, 1, kBoneLit, false);
        slab(fb, kAskNo, kStripEnd - kAskNo, false);
        pieceText(fb, kUiStr[lg][UI_I_NO], (kAskNo + kStripEnd) / 2, mid + 5, 1, kBtnText, false);
        return;
    }
    const u16 off = rgb(60, 54, 50);
    slab(fb, kStripL, kStripW, false);
    blit(fb, UI_SHAPE_ROTATE, kStripL + kStripW / 2, mid, kBone);  // (a tray's item: the pose of its drop)
    slab(fb, kStripR, kStripEnd - kStripR, false);
    blit(fb, UI_SHAPE_TRASH, (kStripR + kStripEnd) / 2, mid, (v.it[sel].flags & 8) ? kBone : off);
}

// --- the game's screen ---------------------------------------------------------------------------------------------------

// The whole bottom screen of the game into `fb` (320x240, the framebuffer's order); `pressed` the button under the finger
// (0 the case, 1 the map, -1 none). 0: the look of before asked for (RE4_3DS_UI=0) - nothing drawn
extern "C" int re4_ui_game(unsigned short* fb, int pressed)
{
    static int on = -1;
    if (on < 0) {
        const char* v = getenv("RE4_3DS_UI");
        on = v && *v == '0' ? 0 : 1;
    }
    if (!on) {
        return 0;
    }
    memcpy(fb, kUiBg, sizeof(kUiBg));
    re4_ui_clip(0, 0, 320, 240);
    const int lg = lang();
    const int life = re4_inv_view ? -1 : re4_hud_life;  // (2026-11-27: the inventory draws its own band)
    static int invStart = -1;  // (2026-11-27) RE4_3DS_INV=1: the inventory from the start (the A/B under a replay)
    if (invStart < 0) {
        const char* e = getenv("RE4_3DS_INV");
        invStart = e && *e == '1' ? 1 : 0;
        re4_inv_view = invStart;
    }
    if (life >= 0) {
        // the lives: the label, the heartbeat (after the longer label), the number
        const int ashley = re4_hud_ashley;
        int labelEnd = UI_LABEL_X + kUiPiece[kUiStr[lg][UI_LEON]].adv;
        if (ashley >= 0) {
            const int e = UI_LABEL_X + kUiPiece[kUiStr[lg][UI_ASHLEY]].adv;
            labelEnd = e > labelEnd ? e : labelEnd;
        }
        const int px = labelEnd + 8 > UI_PULSE_X ? labelEnd + 8 : UI_PULSE_X;
        const int skip = px - UI_PULSE_X;
        char t[24];
        pieceText(fb, kUiStr[lg][UI_LEON], UI_LABEL_X, UI_LEON_BASE, 0, kBone, true);
        heartbeat(fb, UI_PULSE_LEON, px, UI_PULSE_LEON_Y, skip, life, re4_hud_life_max);
        snprintf(t, sizeof(t), "%d", life);
        number(fb, UI_DS_SMALL, t, UI_NUM_R, UI_LEON_BASE, 2, kDim, false);
        if (ashley >= 0) {
            pieceText(fb, kUiStr[lg][UI_ASHLEY], UI_LABEL_X, UI_ASHLEY_BASE, 0, kBone, true);
            heartbeat(fb, UI_PULSE_ASHLEY, px, UI_PULSE_ASHLEY_Y, skip, ashley, re4_hud_ashley_max);
            snprintf(t, sizeof(t), "%d", ashley);
            number(fb, UI_DS_SMALL, t, UI_NUM_R, UI_ASHLEY_BASE, 2, kDim, false);
        }
        if (!re4_inv_view) {  // (2026-11-27: the inventory takes their rows - the weapon's rounds on it)
        // the ammo: loaded large, the reserve after it; "—" without a counter
        pieceText(fb, kUiStr[lg][UI_AMMO], UI_LABEL_X, UI_LABEL2_BASE, 0, kDim, false);
        const int ammo = re4_hud_ammo, reserve = re4_hud_reserve;
        if (ammo >= 0) {
            snprintf(t, sizeof(t), "%d", ammo);
            const int w = number(fb, UI_DS_BIG, t, UI_LABEL_X, UI_NUM_BASE, 0, ammo == 0 ? kRed : kBone, true);
            if (reserve >= 0) {
                snprintf(t, sizeof(t), "/ %d", reserve);
                number(fb, UI_DS_MID, t, UI_LABEL_X + w + 4, UI_NUM_BASE, 0, kDim, true);
            }
        } else {
            const int g = glyph(UI_DS_BIG, 0x2014);
            if (g >= 0) {
                blit(fb, g, UI_LABEL_X, UI_NUM_BASE, kDim);
            }
        }
        // the pesetas (grouped by thousands)
        pieceText(fb, kUiStr[lg][UI_PTAS], UI_NUM_R, UI_LABEL2_BASE, 2, kDim, false);
        groupThousands(re4_hud_peseta, t);
        number(fb, UI_DS_PTAS, t, UI_NUM_R, UI_NUM_BASE, 2, kGold, true);
        }
    }
    // the map, or why not; (2026-11-27) or the inventory
    if (re4_inv_view) {
        invDraw(fb);
    } else if (!re4_3ds_minimap_draw(fb, UI_MAP_X, UI_MAP_Y, UI_MAP_W, UI_MAP_H)) {
        fill(fb, UI_MAP_X, UI_MAP_Y, UI_MAP_W, UI_MAP_H, 0x0000);
        re4_ui_clip(0, 0, 320, 240);
        pieceText(fb, kUiStr[lg][UI_NOMAP], UI_MAP_X + UI_MAP_W / 2, UI_MAP_Y + UI_MAP_H / 2 + 4, 1, kDim, false);
    }
    re4_ui_clip(0, 0, 320, 240);
    // (2026-11-27) the strip: the D-pad weapons' four cells (up, left, right, down: an arrow, the weapon's short name;
    // the equipped one lit, one not in the case dim, none: a dash), the case's button and the gear (icons); lit while
    // the finger is on them (the inside lighter, the line beneath red); `pressed`: 0..3 the cells, 4 the case, 5 the gear
    {
        static const int kCellSlot[4] = {0, 2, 3, 1};
        static const int kTri[4] = {UI_SHAPE_TRI_U, UI_SHAPE_TRI_L, UI_SHAPE_TRI_R, UI_SHAPE_TRI_D};
        for (int c = 0; c < 4; c++) {
            const int x0 = UI_WEP_X + c * (UI_WEP_W + UI_WEP_GAP), id = re4_wep_slot[kCellSlot[c]];
            bool owned = false;
            for (int i = 0; i < re4_wep_owned_n && i < 24 && !owned; i++) {
                owned = re4_wep_owned[i] == id;
            }
            const bool eq = id && id == re4_wep_equipped, lit = pressed == c;
            if (lit || eq) {
                fill(fb, x0 + 1, UI_BTN_Y + 1, UI_WEP_W - 2, UI_BTN_H - 5, rgb(40, 30, 30));
                fill(fb, x0 + 1, UI_BTN_Y + UI_BTN_H - 3, UI_WEP_W - 2, 2, rgb(170, 22, 18));
            }
            const u16 c1 = lit || eq ? kBoneLit : kBtnText;
            blit(fb, kTri[c], x0 + 6, UI_BTN_Y + 6, c1);
            const int name = id ? re4_ui_wep_name(id) : UI_W_EMPTY;
            pieceText(fb, kUiStr[lg][name], x0 + UI_WEP_W / 2, UI_BTN_Y + 19, 1, id && !owned ? kDim : c1, false);
        }
        for (int b = 0; b < 2; b++) {
            const int x0 = b ? UI_GEAR_X : UI_CASE_X, w = b ? UI_GEAR_W : UI_CASE_W;
            const bool lit = pressed == 4 + b || (b == 0 && re4_inv_view);  // (2026-11-27: the inventory shown)
            if (lit) {
                fill(fb, x0 + 1, UI_BTN_Y + 1, w - 2, UI_BTN_H - 5, rgb(40, 30, 30));
                fill(fb, x0 + 1, UI_BTN_Y + UI_BTN_H - 3, w - 2, 2, rgb(170, 22, 18));
            }
            blit(fb, b ? UI_SHAPE_GEAR : UI_SHAPE_CASE, x0 + w / 2, UI_BTN_Y + (UI_BTN_H - 3) / 2,
                 lit ? kBoneLit : kBtnText);
        }
    }
    if (re4_inv_view && re4_inv_edit) {  // (2026-11-27: the inventory's edits)
        invEditStrip(fb, lg);
    }
    if (re4_3ds_minimap_shot_due()) {  // (the stand: the whole screen, the format of RE4_3DS_HUD_SHOT)
        if (FILE* f = fopen("sdmc:/3ds/pueblo/hud.fb", "wb")) {
            fwrite(fb, 2, 320 * 240, f);
            fclose(f);
        }
    }
    return 1;
}

// --- R2: the settings before the gameplay ---------------------------------------------------------------------------------

namespace {

// the game screen's gradient without its lines, slabs and frame: the formula and the dither of stage9_ui_assets.py
// background() (made here, not a second picture: 150 KB less in the image; drawn only when the settings change)
void gradient(u16* fb)
{
    static float vig[257];  // the vignette by the distance 0..1.6 in 256 steps
    static u8 bayer[8][8];
    static bool made;
    if (!made) {
        for (int i = 0; i <= 256; i++) {
            float d = i * (1.6f / 256.0f) - 0.45f;
            d = d < 0.0f ? 0.0f : d > 1.0f ? 1.0f : d;
            vig[i] = 1.0f - 0.45f * powf(d, 1.3f);
        }
        static const u8 b2[2][2] = {{0, 2}, {3, 1}};
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                bayer[y][x] = (u8) (16 * b2[y & 1][x & 1] + 4 * b2[(y >> 1) & 1][(x >> 1) & 1] + b2[y >> 2][x >> 2]);
            }
        }
        made = true;
    }
    const float stepRB = 255.0f / 31.0f, stepG = 255.0f / 63.0f;
    for (int y = 0; y < 240; y++) {
        const float f = powf(y / 239.0f, 0.8f);
        const float r0 = 34.0f + (7.0f - 34.0f) * f, g0 = 17.0f + (6.0f - 17.0f) * f, b0 = 16.0f + (6.0f - 16.0f) * f;
        const float dy = (y - 96.0f) / 180.0f;
        for (int x = 0; x < 320; x++) {
            const float dx = (x - 160.0f) / 160.0f;
            int i = (int) (sqrtf(dx * dx + dy * dy) * (256.0f / 1.6f));
            const float v = vig[i > 256 ? 256 : i];
            const float t = (bayer[y & 7][x & 7] + 0.5f) / 64.0f - 0.5f;
            int r = (int) floorf((r0 * v + t * stepRB) / stepRB + 0.5f);
            int g = (int) floorf((g0 * v + t * stepG) / stepG + 0.5f);
            int b = (int) floorf((b0 * v + t * stepRB) / stepRB + 0.5f);
            r = r < 0 ? 0 : r > 31 ? 31 : r;
            g = g < 0 ? 0 : g > 63 ? 63 : g;
            b = b < 0 ? 0 : b > 31 ? 31 : b;
            fb[x * 240 + 239 - y] = (u16) (r << 11 | g << 5 | b);
        }
    }
}

void outline(u16* fb, int x, int y, int w, int h, u16 c)
{
    fill(fb, x, y, w, 1, c);
    fill(fb, x, y + h - 1, w, 1, c);
    fill(fb, x, y, 1, h, c);
    fill(fb, x + w - 1, y, 1, h, c);
}

}  // namespace

// (2026-11-19) The settings screen's tabs (more rows than a page holds): CHEATS, CONTROLS, SCREEN - each its rows, as
// the indices of setup_3ds.cpp's values (0..4 the cheats and the money, 5 the gyro, 6 the frame counter, 7 the circle
// pad's sensitivity, 8 the picture); -1 ends a tab
// (2026-11-21: 9 the button mapping's row on CONTROLS - a link to its page, re4_ui_buttons; 10 the Circle Pad's dead zone)
// (2026-11-27: 11 the top HUD off, 12 the bottom screen's language - both on SCREEN)
// (2026-11-27: 13 all weapons at the merchant, 14 the attaché case XL - CHEATS of seven rows, 22 px as the button
// mapping's page; the other tabs 24 px)
const int kTabSlots = 7;
const int kTabRows[3][kTabSlots] = {{0, 1, 2, 3, 13, 4, -1}, {5, 7, 10, 9, -1, -1, -1}, {8, 6, 11, 12, -1, -1, -1}};

// the height of a tab's rows: 22 px when it has more than six
static int tabRowH(int tab)
{
    return kTabRows[tab][6] >= 0 ? 22 : UI_S_ROW_H;
}

// (2026-11-27, the user) the tabs' order on the screen: SCREEN, CONTROLS, CHEATS - the tab at each place
const int kTabAt[3] = {2, 1, 0};

// The top of a tab's `slot`th row - also for the touch (setup_3ds.cpp)
extern "C" int re4_ui_setup_row_y(int slot)
{
    return UI_S_ROW1_Y + slot * UI_S_ROW_H;
}

static int tabRowY(int tab, int slot)
{
    return UI_S_ROW1_Y + slot * tabRowH(tab);
}

// What is under (x, y) with the tab `tab` open: a value's row (0..8), -2 - t the tab t, -1 nothing; *left: on an arrow
// row's left arrow (from halfway to the label's end up to halfway to the value)
extern "C" int re4_ui_setup_hit(int tab, int x, int y, int* left)
{
    *left = 0;
    if (x < 8 || x >= 312) {
        return -1;
    }
    if (y >= UI_S_TAB_Y && y < UI_S_TAB_Y + UI_S_TAB_H) {
        return -2 - kTabAt[(x - 8) * 3 / 304];  // (2026-11-27: the tabs' order on the screen)
    }
    if (re4_ui_setup_live && y >= UI_S_LINE2_Y && x >= 200) {
        return -20;  // (2026-11-27) RESUME
    }
    tab = tab < 0 || tab > 2 ? 0 : tab;
    for (int s = 0; s < kTabSlots && kTabRows[tab][s] >= 0; s++) {
        const int y0 = tabRowY(tab, s);
        if (y >= y0 && y < y0 + tabRowH(tab)) {
            const int i = kTabRows[tab][s];
            *left = (i == 5 || i == 7 || i == 8 || i == 10 || i == 12) && x >= UI_S_ARROW_L - 28 &&
                    x < (UI_S_ARROW_L + UI_S_GAIN_X) / 2;  // (2026-11-21: 10 the dead zone)
            return i;
        }
    }
    return -1;
}

// The build's version (RE4_3DS_VERSION, ASCII) in the panel's 8x8 font, ending at `right`, on the baseline `base`;
// its left end never left of x 168 (the titles' room): a longer text loses its first characters
void versionText(u16* fb, int right, int base, u16 c)
{
    const char* s = RE4_3DS_VERSION;
    int n = (int) strlen(s);
    const int room = (right - 168) / 8;
    if (n > room) {
        s += n - room;
        n = room;
    }
    const int x0 = right - n * 8, y0 = base - 7;
    for (int i = 0; i < n; i++) {
        const unsigned char ch = (unsigned char) s[i];
        const unsigned char* glyph = &kHudFont[(ch >= 0x20 && ch < 0x7F ? ch : '?') * 8];
        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 8; col++) {
                if (glyph[row] & (0x80 >> col)) {
                    fill(fb, x0 + i * 8 + col, y0 + row, 1, 1, c);
                }
            }
        }
    }
}

// The whole settings screen into `fb` with the tab `tab` open: v[0..4] the cheats and the money (0 / 1), v[5] the gyro's
// gain x10 (0 off, 10..40), v[6] the frame counter, v[7] the circle pad's step, v[8] the picture; `lit` the value's row
// under the finger (-1 none)
extern "C" void re4_ui_setup(unsigned short* fb, const int* v, int lit, int tab)
{
    gradient(fb);
    re4_ui_clip(0, 0, 320, 240);
    const int lg = lang();
    const u16 blood = rgb(90, 10, 10), bloodLit = rgb(170, 22, 18), slab = rgb(40, 30, 30);
    tab = tab < 0 || tab > 2 ? 0 : tab;
    fill(fb, 8, UI_S_LINE1_Y, 305, 1, blood);
    fill(fb, 8, UI_S_LINE2_Y, 305, 1, blood);
    pieceText(fb, kUiStr[lg][UI_S_TITLE], 12, UI_S_TITLE_BASE, 0, kBone, true);
    versionText(fb, 308, UI_S_TITLE_BASE, kDim);  // (R7: the build's version; was UI_S_VERSION)
    // (2026-11-19) the tabs: the open one on a slab with a blood-red underline, the others dim over a hairline
    static const int kTab[3] = {UI_S_TAB_CHEATS, UI_S_TAB_CONTROLS, UI_S_TAB_SCREEN};
    for (int at = 0; at < 3; at++) {
        const int t = kTabAt[at];
        const int x0 = 8 + at * 304 / 3, x1 = 8 + (at + 1) * 304 / 3;
        if (t == tab) {
            fill(fb, x0 + 1, UI_S_TAB_Y, x1 - x0 - 2, UI_S_TAB_H, slab);
            fill(fb, x0 + 1, UI_S_TAB_Y + UI_S_TAB_H - 2, x1 - x0 - 2, 2, bloodLit);
        } else {
            fill(fb, x0 + 1, UI_S_TAB_Y + UI_S_TAB_H - 1, x1 - x0 - 2, 1, blood);
        }
        pieceText(fb, kUiStr[lg][kTab[t]], (x0 + x1) / 2, UI_S_TAB_BASE, 1, t == tab ? kBoneLit : kDim, t == tab);
    }
    static const int kLabel[15] = {UI_S_HEALTH, UI_S_AMMO, UI_S_ONEHIT, UI_S_QTE,     UI_S_MONEY,
                                   UI_S_GYRO,   UI_S_FPS,  UI_S_STICK,  UI_S_PICTURE, 0,
                                   0,           UI_S_TOPHUD, UI_S_UILANG, UI_S_SHOPWEP, 0};  // (9, 10: their own branches)
    static const int kLangName[8] = {UI_S_L_AUTO, UI_S_L_EN, UI_S_L_FR, UI_S_L_DE,
                                     UI_S_L_ES,   UI_S_L_IT, UI_S_L_JA, UI_S_L_CHS};  // (2026-11-27)
    static const int kStick[4] = {UI_S_G10, UI_S_G12, UI_S_G15, UI_S_G20};
    static const int kPicture[3] = {UI_S_FIT, UI_S_STRETCH, UI_S_ZOOM};
    const int rowH = tabRowH(tab);
    const int tb = rowH / 2 + 4;  // (the text's baseline in a row: 16 of 24, 15 of 22)
    for (int s = 0; s < kTabSlots && kTabRows[tab][s] >= 0; s++) {
        const int i = kTabRows[tab][s];
        const int y0 = tabRowY(tab, s);
        if (lit == i) {
            fill(fb, 8, y0 + 1, 304, rowH - 3, slab);
            fill(fb, 8, y0 + rowH - 2, 304, 1, bloodLit);
        }
        if (i == 9) {  // (2026-11-21) the button mapping: a link (no value of its own), an arrow to its page
            pieceText(fb, kUiStr[lg][UI_S_BUTTONS], 16, y0 + tb, 0, kBone, true);
            blit(fb, UI_SHAPE_ARROW_R, UI_S_ARROW_R, y0 + rowH / 2, kBone);
            continue;
        }
        if (i == 10) {  // (2026-11-21) the Circle Pad's dead zone: OFF (dim, as the gyro's) or the percent
            const int dz = v[10];
            pieceText(fb, kUiStr[lg][UI_S_DEADZONE], 16, y0 + tb, 0, lit == i ? kBoneLit : kBone, true);
            blit(fb, UI_SHAPE_ARROW_L, UI_S_ARROW_L, y0 + rowH / 2, dz ? kBtnText : kDim);
            blit(fb, UI_SHAPE_ARROW_R, UI_S_ARROW_R, y0 + rowH / 2, dz ? kBtnText : kDim);
            if (dz) {
                char t[8];
                snprintf(t, sizeof(t), "%d%%", dz);
                number(fb, UI_DS_MID, t, UI_S_GAIN_X, y0 + tb, 1, kBoneLit, true);
            } else {
                pieceText(fb, kUiStr[lg][UI_S_OFF], UI_S_GAIN_X, y0 + tb, 1, kDim, false);
            }
            continue;
        }
        if (i == 4 || i == 14) {  // (2026-11-27) the giving cheats: a button GET, once given "Done" (dim); 2: dim
            const bool done = v[i] == 1, off = v[i] == 2;  // (before the gameplay)
            pieceText(fb, kUiStr[lg][kLabel[i]], 16, y0 + tb, 0, lit == i && !off ? kBoneLit : done || off ? kBtnText : kBone, true);
            const int id = done ? UI_S_DONE : UI_S_GET;
            const int w = kUiPiece[kUiStr[lg][id]].adv;
            if (off) {
                outline(fb, 304 - w - 8, y0 + 3, w + 12, rowH - 6, rgb(60, 50, 48));
                pieceText(fb, kUiStr[lg][id], 304 - w - 2, y0 + tb, 0, kDim, false);
                continue;
            }
            if (!done) {
                const int bx = 304 - w - 8, bw = w + 12;
                fill(fb, bx, y0 + 3, bw, rowH - 6, lit == i ? rgb(110, 20, 16) : rgb(60, 22, 20));
                outline(fb, bx, y0 + 3, bw, rowH - 6, lit == i ? rgb(200, 60, 50) : rgb(120, 40, 34));
            }
            pieceText(fb, kUiStr[lg][id], done ? 304 : 304 - w - 2, y0 + tb, done ? 2 : 0, done ? kDim : kBoneLit, !done);
            continue;
        }
        const bool on = v[i] != 0;
        // (the owner: a default value is no "off") dim: an unticked box and the gyro off; the stick's 1x, the picture's
        // Fit as bright as any other value
        const bool dim = i == 5 ? v[5] == 0 : i == 7 || i == 8 || i == 12 ? false : !on;
        pieceText(fb, kUiStr[lg][kLabel[i]], 16, y0 + tb, 0, lit == i ? kBoneLit : dim ? kBtnText : kBone, true);
        if (i == 5 || i == 7 || i == 8 || i == 12) {  // the arrows and the value between them
            int id;
            u16 c, arrow = kBtnText;
            bool bright;
            if (i == 5) {  // the gyro: off dim
                const int g = v[5];
                id = g >= 40 ? UI_S_G40 : g >= 30 ? UI_S_G30 : g >= 25 ? UI_S_G25 : g >= 20 ? UI_S_G20 : g >= 15 ? UI_S_G15
                   : g > 0 ? UI_S_G10 : UI_S_OFF;
                c = g ? kBoneLit : kDim, bright = g != 0, arrow = g ? kBtnText : kDim;
            } else if (i == 7) {  // the circle pad's sensitivity: 1x (as it was) plain
                const int st = v[7] < 0 || v[7] > 3 ? 0 : v[7];
                id = kStick[st], c = st ? kBoneLit : kBone, bright = st != 0;
            } else if (i == 12) {  // (2026-11-27) the bottom screen's language: Auto (as it was) plain
                const int lc = v[12] < 0 || v[12] > 7 ? 0 : v[12];
                id = kLangName[lc], c = lc ? kBoneLit : kBone, bright = lc != 0;
            } else {  // the picture: Fit (as it was) plain
                const int pm = v[8] < 0 || v[8] > 2 ? 0 : v[8];
                id = kPicture[pm], c = pm ? kBoneLit : kBone, bright = pm != 0;
            }
            blit(fb, UI_SHAPE_ARROW_L, UI_S_ARROW_L, y0 + rowH / 2, arrow);
            blit(fb, UI_SHAPE_ARROW_R, UI_S_ARROW_R, y0 + rowH / 2, arrow);
            pieceText(fb, kUiStr[lg][id], UI_S_GAIN_X, y0 + tb, 1, c, bright);
            continue;
        }
        const int by = y0 + (rowH - UI_S_BOX) / 2;
        if (on) {
            fill(fb, UI_S_BOX_X, by, UI_S_BOX, UI_S_BOX, bloodLit);
            outline(fb, UI_S_BOX_X, by, UI_S_BOX, UI_S_BOX, rgb(200, 60, 50));
            blit(fb, UI_SHAPE_CHECK, UI_S_BOX_X, by, kBoneLit);
        } else {
            fill(fb, UI_S_BOX_X, by, UI_S_BOX, UI_S_BOX, rgb(18, 14, 14));
            outline(fb, UI_S_BOX_X, by, UI_S_BOX, UI_S_BOX, rgb(78, 68, 64));
        }
    }
    if (re4_ui_setup_live) {  // (2026-11-27) over the game's option screen: "Applied at once", RESUME (back to the game)
        pieceText(fb, kUiStr[lg][UI_S_FOOT_LIVE], 12, UI_S_FOOT_BASE, 0, kDim, false);
        const bool on = lit == -20;
        fill(fb, 200, UI_S_LINE2_Y + 3, 112, 240 - UI_S_LINE2_Y - 5, on ? rgb(60, 40, 38) : rgb(40, 30, 30));
        fill(fb, 201, 238, 110, 1, on ? rgb(170, 22, 18) : rgb(90, 10, 10));
        pieceText(fb, kUiStr[lg][UI_S_RESUME], 256, (UI_S_LINE2_Y + 238) / 2 + 7, 1, on ? kBoneLit : kBone, true);
    } else if (re4_3ds_icons_progress() >= 0) {  // (2026-11-27, the user) the item pictures being made: their share
        const int prog = re4_3ds_icons_progress();
        char t[8];
        snprintf(t, sizeof(t), "%d%%", prog);
        pieceText(fb, kUiStr[lg][UI_I_PREP], 12, UI_S_FOOT_BASE, 0, kBtnText, false);
        fill(fb, 196, UI_S_FOOT_BASE - 7, 80, 6, rgb(40, 30, 30));
        fill(fb, 196, UI_S_FOOT_BASE - 7, prog * 80 / 100, 6, rgb(170, 22, 18));
        number(fb, UI_DS_SMALL, t, 308, UI_S_FOOT_BASE, 2, kGold, false);
    } else {
        pieceText(fb, kUiStr[lg][UI_S_FOOT], 160, UI_S_FOOT_BASE, 1, kDim, false);
    }
}

// --- (2026-11-21) the button mapping's page (3ds/setup_3ds.cpp, 3ds/controls_3ds.cpp) ---------------------------------

// its rows: the actions of controls_map.h (0..19), 20 "Reset to defaults"; three pages of seven rows of 22 px (-1: none)
const int kBtnPages = 4;  // (2026-11-27: the fourth - the D-pad weapons' actions, the reset)
const int kBtnRows[kBtnPages][7] = {{0, 1, 2, 3, 4, 5, 6}, {7, 8, 9, 10, 11, 12, 13}, {14, 15, 16, 17, 18, 19, -1},
                                    {20, 21, 22, 23, 24, -1, -1}};
const int kBtnRowH = 22;
const int kBtnLabel[25] = {UI_A_UP,     UI_A_DOWN,  UI_A_LEFT,  UI_A_RIGHT,  UI_A_LUP,   UI_A_LDOWN, UI_A_LLEFT,
                           UI_A_LRIGHT, UI_A_ACTION, UI_A_RUN,  UI_A_TURN,   UI_A_AIM,   UI_A_FIRE,  UI_A_RELOAD,
                           UI_A_KNIFE,  UI_A_SLASH, UI_A_ASHLEY, UI_A_CASE,  UI_A_MAP,   UI_A_OPTIONS, UI_A_WUP,
                           UI_A_WDOWN,  UI_A_WLEFT, UI_A_WRIGHT, UI_S_RESET};
const int kKeyName[21] = {UI_K_A,     UI_K_B,     UI_K_X,     UI_K_Y,      UI_K_L,     UI_K_R,     UI_K_ZL,
                          UI_K_ZR,    UI_K_UP,    UI_K_DOWN,  UI_K_LEFT,   UI_K_RIGHT, UI_K_START, UI_K_PUP,
                          UI_K_PDOWN, UI_K_PLEFT, UI_K_PRIGHT, UI_K_CUP,   UI_K_CDOWN, UI_K_CLEFT, UI_K_CRIGHT};

// What is under (x, y): a row (0..20), -10 back to the tabs (the band's left half), -11 the other page (its right half),
// -1 nothing
extern "C" int re4_ui_buttons_hit(int page, int x, int y)
{
    if (x < 8 || x >= 312) {
        return -1;
    }
    if (y >= UI_S_TAB_Y && y < UI_S_TAB_Y + UI_S_TAB_H) {
        return x < 160 ? -10 : -11;
    }
    page = page < 0 || page >= kBtnPages ? 0 : page;
    for (int s = 0; s < 7 && kBtnRows[page][s] >= 0; s++) {
        const int y0 = UI_S_ROW1_Y + s * kBtnRowH;
        if (y >= y0 && y < y0 + kBtnRowH) {
            return kBtnRows[page][s];
        }
    }
    return -1;
}

// The page into `fb`: `keys` each action's keys (controls_map.h's ctl::Key, 0xFF none), `catching` the action whose
// keys are awaited (-1 none), `lit` the row under the finger
extern "C" void re4_ui_buttons(unsigned short* fb, int page, int lit, int catching, const unsigned char (*keys)[2])
{
    gradient(fb);
    re4_ui_clip(0, 0, 320, 240);
    const int lg = lang();
    const u16 blood = rgb(90, 10, 10), bloodLit = rgb(170, 22, 18), slab = rgb(40, 30, 30);
    page = page < 0 || page >= kBtnPages ? 0 : page;
    fill(fb, 8, UI_S_LINE1_Y, 305, 1, blood);
    fill(fb, 8, UI_S_LINE2_Y, 305, 1, blood);
    pieceText(fb, kUiStr[lg][UI_S_BTN_TITLE], 12, UI_S_TITLE_BASE, 0, kBone, true);
    versionText(fb, 308, UI_S_TITLE_BASE, kDim);
    // the band of the tabs: back to CONTROLS (left), the page and its arrows (right)
    const int mid = UI_S_TAB_Y + UI_S_TAB_H / 2;
    fill(fb, 9, UI_S_TAB_Y + UI_S_TAB_H - 1, 302, 1, blood);
    blit(fb, UI_SHAPE_ARROW_L, 14, mid, kBone);
    pieceText(fb, kUiStr[lg][UI_S_TAB_CONTROLS], 28, UI_S_TAB_BASE, 0, kBone, false);
    blit(fb, UI_SHAPE_ARROW_L, 250, mid, kBtnText);
    char pages[8];
    snprintf(pages, sizeof(pages), "%d/%d", page + 1, kBtnPages);
    number(fb, UI_DS_SMALL, pages, 275, UI_S_TAB_BASE, 1, kBone, false);
    blit(fb, UI_SHAPE_ARROW_R, 296, mid, kBtnText);
    const int tb = kBtnRowH / 2 + 4;
    for (int s = 0; s < 7 && kBtnRows[page][s] >= 0; s++) {
        const int i = kBtnRows[page][s];
        const int y0 = UI_S_ROW1_Y + s * kBtnRowH;
        const bool wait = i == catching;
        if (lit == i || wait) {
            fill(fb, 8, y0 + 1, 304, kBtnRowH - 3, slab);
            fill(fb, 8, y0 + kBtnRowH - 2, 304, 1, bloodLit);
        }
        pieceText(fb, kUiStr[lg][kBtnLabel[i]], 16, y0 + tb, 0, lit == i || wait ? kBoneLit : kBone, true);
        if (i >= 24) {
            continue;  // (the reset: no value)
        }
        if (wait) {
            pieceText(fb, kUiStr[lg][UI_S_PRESS], 304, y0 + tb, 2, kGold, false);
            continue;
        }
        const unsigned char* k = keys[i];
        const int n = k[0] >= 21 ? 0 : k[1] >= 21 ? 1 : 2;
        if (!n) {
            pieceText(fb, kUiStr[lg][UI_K_NONE], 304, y0 + tb, 2, kDim, false);
            continue;
        }
        int w = kUiPiece[kUiStr[lg][kKeyName[k[0]]]].adv;
        if (n == 2) {
            w += kUiPiece[kUiStr[lg][UI_K_PLUS]].adv + kUiPiece[kUiStr[lg][kKeyName[k[1]]]].adv;
        }
        int x = 304 - w;
        x += pieceText(fb, kUiStr[lg][kKeyName[k[0]]], x, y0 + tb, 0, kBoneLit, true);
        if (n == 2) {
            x += pieceText(fb, kUiStr[lg][UI_K_PLUS], x, y0 + tb, 0, kBtnText, false);
            pieceText(fb, kUiStr[lg][kKeyName[k[1]]], x, y0 + tb, 0, kBoneLit, true);
        }
    }
    pieceText(fb, kUiStr[lg][UI_S_BTN_FOOT], 160, UI_S_FOOT_BASE, 1, kDim, false);
}

// the list's layout: tiles of 4 x 4 (the user's wish: easy to touch), Leon's weapons in the case's order, then "None"
const int kTileGap = 4;

// (2026-11-27) the tiles for `n` weapons and "None": 4 x 4 of 73 x 35, more than 16 - 5 x 5 of 57 x 28
struct Tiles {
    int cols, rows, w, h;
};

static Tiles tilesFor(int n)
{
    return n + 1 <= 16 ? Tiles{4, 4, 73, 35} : Tiles{5, 5, 57, 28};
}

// What is under (x, y) in the list of `n` weapons: 0..n-1 a weapon, n "None", -10 back (the band), -1 nothing
extern "C" int re4_ui_wep_pick_hit(int n, int x, int y)
{
    if (x < 8 || x >= 312) {
        return -1;
    }
    if (y >= UI_S_TAB_Y && y < UI_S_TAB_Y + UI_S_TAB_H) {
        return -10;
    }
    if (y < UI_S_ROW1_Y) {
        return -1;
    }
    const Tiles t = tilesFor(n);
    const int col = (x - 8) / (t.w + kTileGap), row = (y - UI_S_ROW1_Y) / (t.h + kTileGap);
    if (col >= t.cols || row >= t.rows) {
        return -1;
    }
    const int i = row * t.cols + col, last = n < t.cols * t.rows - 1 ? n : t.cols * t.rows - 1;
    return i <= last ? (i == last ? n : i) : -1;
}

// The list over the game's option screen: the arrow `slot` (0 up, 1 down, 2 left, 3 right), Leon's weapons `ids` as
// tiles, the one on the arrow now `now` (marked), `lit` the tile under the finger
extern "C" void re4_ui_wep_pick(unsigned short* fb, int slot, const unsigned char* ids, int n, int lit, int now)
{
    gradient(fb);
    re4_ui_clip(0, 0, 320, 240);
    const int lg = lang();
    const u16 blood = rgb(90, 10, 10), bloodLit = rgb(170, 22, 18);
    static const int kTri[4] = {UI_SHAPE_TRI_U, UI_SHAPE_TRI_D, UI_SHAPE_TRI_L, UI_SHAPE_TRI_R};
    fill(fb, 8, UI_S_LINE1_Y, 305, 1, blood);
    fill(fb, 8, UI_S_LINE2_Y, 305, 1, blood);
    const int tw = pieceText(fb, kUiStr[lg][UI_W_PICK], 12, UI_S_TITLE_BASE, 0, kBone, true);
    blit(fb, kTri[slot & 3], 12 + tw + 10, UI_S_TITLE_BASE - 5, kBoneLit);
    versionText(fb, 308, UI_S_TITLE_BASE, kDim);
    const int mid = UI_S_TAB_Y + UI_S_TAB_H / 2;
    fill(fb, 9, UI_S_TAB_Y + UI_S_TAB_H - 1, 302, 1, blood);
    blit(fb, UI_SHAPE_ARROW_L, 14, mid, kBone);
    pieceText(fb, kUiStr[lg][UI_S_TITLE], 28, UI_S_TAB_BASE, 0, kBone, false);
    const Tiles t = tilesFor(n);
    const int kTileW = t.w, kTileH = t.h;
    n = n < t.cols * t.rows - 1 ? n : t.cols * t.rows - 1;
    for (int i = 0; i <= n; i++) {
        const int x0 = 8 + (i % t.cols) * (kTileW + kTileGap), y0 = UI_S_ROW1_Y + (i / t.cols) * (kTileH + kTileGap);
        const int id = i < n ? ids[i] : 0;
        const bool mark = i < n ? id == now : !now, on = lit == i;
        fill(fb, x0, y0, kTileW, kTileH, on ? rgb(60, 40, 38) : mark ? rgb(40, 30, 30) : rgb(22, 20, 20));
        outline(fb, x0, y0, kTileW, kTileH, rgb(48, 44, 42));
        fill(fb, x0 + 1, y0 + kTileH - 3, kTileW - 2, 2, on || mark ? bloodLit : blood);
        const int name = i < n ? re4_ui_wep_name(id) : UI_W_NONE;
        int pw = 0, ph = 0;  // (2026-11-27, the user) the weapon's picture, its name over it at the foot
        const unsigned char* pic = i < n ? re4_3ds_icon(20, id, 0, &pw, &ph) : nullptr;
        if (pic) {
            const int bw = kTileW - 4, bh = kTileH - 5;
            int dw = bw, dh = ph * bw / pw;
            if (dh > bh) {
                dh = bh, dw = pw * bh / ph;
            }
            invBlitScaled(fb, pic, pw, ph, x0 + (kTileW - dw) / 2, y0 + 2 + (bh - dh) / 2, dw, dh);
            pieceText(fb, kUiStr[lg][name], x0 + kTileW / 2, y0 + kTileH - 5, 1, on || mark ? kBoneLit : kBone, true);
        } else {
            pieceText(fb, kUiStr[lg][name], x0 + kTileW / 2, y0 + kTileH / 2 + 3, 1, on || mark ? kBoneLit : kBone, i < n);
        }
    }
    pieceText(fb, kUiStr[lg][UI_W_HINT], 160, UI_S_FOOT_BASE, 1, kDim, false);
}

// --- R3: the checks at the start ------------------------------------------------------------------------------------------

// The whole screen into `fb` (3ds/startcheck_3ds.cpp; English only, R3): `title` and `foot` strings (UI_C_T_*, UI_C_F_*),
// `lines` the problems' strings: a heading with RE4_UI_HEAD (UI_C_H_*) and its lines under it
extern "C" void re4_ui_check(unsigned short* fb, int title, const int* lines, int n, int foot)
{
    gradient(fb);
    re4_ui_clip(0, 0, 320, 240);
    const int lg = UI_LANG_EN;
    const u16 blood = rgb(90, 10, 10);
    fill(fb, 8, UI_S_LINE1_Y, 305, 1, blood);
    fill(fb, 8, UI_S_LINE2_Y, 305, 1, blood);
    pieceText(fb, kUiStr[lg][title], 12, UI_S_TITLE_BASE, 0, title == UI_C_T_STOP ? kRed : kBone, true);
    versionText(fb, 308, UI_S_TITLE_BASE, kDim);  // (R7: the build's version; was UI_S_VERSION)
    int y = 50;
    for (int i = 0; i < n; i++) {
        const int id = lines[i] & 0xFFF;
        if (id < 0 || id >= UI_STRINGS) {
            continue;
        }
        if (lines[i] & 0x1000) {  // (RE4_UI_HEAD) a heading: a gap above it but the first
            y += i ? 10 : 0;
            pieceText(fb, kUiStr[lg][id], 14, y, 0, kBoneLit, true);
            y += 18;
        } else {
            pieceText(fb, kUiStr[lg][id], 20, y, 0, kBtnText, false);
            y += 15;
        }
    }
    pieceText(fb, kUiStr[lg][foot], 160, UI_S_FOOT_BASE, 1, kBone, false);
}

// (2026-11-27) The item pictures' making at the first start (3ds/icons_3ds.cpp): the title, "Preparing item pictures",
// a bar and the percent - the player waits once and sees the progress (the user)
extern "C" void re4_ui_prep(unsigned short* fb, int percent)
{
    gradient(fb);
    re4_ui_clip(0, 0, 320, 240);
    const int lg = lang();
    const u16 blood = rgb(90, 10, 10);
    fill(fb, 8, UI_S_LINE1_Y, 305, 1, blood);
    fill(fb, 8, UI_S_LINE2_Y, 305, 1, blood);
    pieceText(fb, kUiStr[lg][UI_S_TITLE], 12, UI_S_TITLE_BASE, 0, kBone, true);
    versionText(fb, 308, UI_S_TITLE_BASE, kDim);
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    pieceText(fb, kUiStr[lg][UI_I_PREP], 160, 112, 1, kBone, true);
    fill(fb, 40, 124, 240, 8, rgb(40, 30, 30));
    fill(fb, 40, 124, percent * 240 / 100, 8, rgb(170, 22, 18));
    char t[8];
    snprintf(t, sizeof(t), "%d%%", percent);
    number(fb, UI_DS_SMALL, t, 160, 150, 1, kGold, false);
}
