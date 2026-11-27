// Stage 6.6 (a wish of the project owner, 2026-11-07): the settings before the game and the panel on the bottom
// screen, in Russian (the console's own font is CP437: hud_font.h).
//
// The menu: L held at the start (the top screen asks for it for kAskMs), or RE4_3DS_MENU=1 in pueblo.cfg: a short list
// of the settings worth a player's hand (kItems), written back into sdmc:/3ds/pueblo/pueblo.cfg (its other lines kept as
// they are) before readSettings (main_3ds.cpp) reads it; so the game starts with them, no restart.
//
// The panel (RE4_3DS_HUD: 2 the panel, by default; 1 the old line of frames per second, system_3ds.cpp showFps; 0
// nothing): the frame rate and the frame's time, the worst frame and the share above 33.3 / 50 ms, how long the game
// waited for the GX thread, the GX thread's and the GPU's busy time per frame, the free VRAM and linear memory, the
// room; redrawn every kHudMs by a thread on core 1 (the log writer's priority). Below, three switches by touch (left half
// less, right half more), for this run only: the draw distance (RE4_DRAWDIST, runtime/part_cull.cpp), the effects
// (RE4_POTATO, runtime/potato.cpp), the cheats (RE4_CHEATS, runtime/cheats.cpp).
//
// Test hooks (the emulator): RE4_3DS_MENU_KEYS=D,D,R,A — the menu's keys, one a frame, instead of the buttons;
// RE4_3DS_MENU_SHOT=1 — each picture of the menu to sdmc:/3ds/pueblo/menu-NN-{top,bot}.fb; RE4_3DS_HUD_SHOT=N — the
// panel's Nth picture to sdmc:/3ds/pueblo/hud.fb (RGB565, the screens' own column order).
#include <3ds.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>  // after stdlib.h: newlib's calloc there is noexcept, here not

#include <string>
#include <vector>

#include "hud_font.h"
#include "re4_core.h"
#include "re4_runtime.h"

extern "C" float re4_draw_dist(void);           // runtime/part_cull.cpp
extern "C" void re4_draw_dist_set(int percent);
extern "C" int re4_potato_mask(void);           // runtime/potato.cpp
extern "C" int re4_room_now(void);
extern "C" int re4_cheats_wanted(void);         // runtime/cheats.cpp
// the GX thread's and the GPU's counters, from the start (gx_thread_3ds.cpp frameFn, gx_3ds.cpp busySampler)
extern "C" {
volatile u64 re4_hud_gx_busy, re4_hud_game_wait;
// (8.3) 3ds/minimap_3ds.cpp: the area's map into a box of the bottom screen (0: no map)
int re4_3ds_minimap_draw(unsigned short* fb, int x, int y, int w, int h);
int re4_3ds_minimap_dirty(void);
int re4_3ds_minimap_touch(int phase, int x, int y);  // (8.3e) the map: scrolled by the finger, its zoom buttons
int re4_ui_game(unsigned short* fb, int pressed);  // (stage 9) 3ds/ui_bottom_3ds.cpp: the whole screen (0: the old look)
// (R2) 3ds/setup_3ds.cpp: the settings before the gameplay
void re4_3ds_setup_start(void);
int re4_3ds_setup_visible(void);
int re4_3ds_setup_dirty(void);
void re4_3ds_setup_draw(unsigned short* fb);
void re4_3ds_setup_poll(void);
int re4_3ds_setup_touch(int phase, int x, int y);
// (8.4) runtime/cheats.cpp hudSample: the game's numbers, each game frame
extern volatile int re4_hud_life, re4_hud_life_max, re4_hud_ashley, re4_hud_ashley_max, re4_hud_ammo, re4_hud_reserve,
    re4_hud_peseta;
extern volatile unsigned char re4_wep_slot[4];  // (2026-11-27) runtime/cheats.cpp: the D-pad weapons
extern volatile int re4_wep_equipped, re4_wep_owned_n;
volatile u32 re4_hud_vram_kb, re4_hud_linear_kb, re4_hud_gpu_busy, re4_hud_gpu_all;
}

namespace {

// --- the font and the text ----------------------------------------------------------------------------------------

ConsoleFont gFont = {(u8*) kHudFont, 0, 256};
u32 gExtra[64];  // kHudExtra's code points (from 0xC2 on)
int gExtraCount = -1;

u32 nextCode(const char*& s)
{
    const u8 c = (u8) *s++;
    if (c < 0x80) {
        return c;
    }
    int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    u32 v = c & (0x3F >> more);
    while (more-- && (*s & 0xC0) == 0x80) {
        v = v << 6 | ((u8) *s++ & 0x3F);
    }
    return v;
}

// UTF-8 into the font's codes (hud_font.h); the escape sequences pass
void hudText(const char* s, char* out, size_t size)
{
    if (gExtraCount < 0) {
        gExtraCount = 0;
        for (const char* e = kHudExtra; *e && gExtraCount < 64;) {
            gExtra[gExtraCount++] = nextCode(e);
        }
    }
    size_t n = 0;
    while (*s && n + 1 < size) {
        const u32 v = nextCode(s);
        char c = '?';
        if (v < 0x80) {
            c = (char) v;
        } else if (v >= 0x410 && v <= 0x44F) {
            c = (char) (0x80 + v - 0x410);
        } else if (v == 0x401) {
            c = (char) 0xC0;
        } else if (v == 0x451) {
            c = (char) 0xC1;
        } else {
            for (int k = 0; k < gExtraCount; k++) {
                if (gExtra[k] == v) {
                    c = (char) (0xC2 + k);
                    break;
                }
            }
        }
        out[n++] = c;
    }
    out[n] = 0;
}

int visible(const char* s)
{
    int n = 0;
    while (*s) {
        if (*s == 0x1B) {
            while (*s && !((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z'))) {
                s++;
            }
            s += *s != 0;
            continue;
        }
        n++;
        s++;
    }
    return n;
}

// Row y (1-based) of the console from its first column, as wide as `width` (spaces after; the last column left free:
// a character there moves the cursor on, at the bottom row the console would scroll)
void rowText(int y, int width, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
void rowText(int y, int width, const char* fmt, ...)
{
    char utf[320], text[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(utf, sizeof(utf), fmt, ap);
    va_end(ap);
    hudText(utf, text, sizeof(text));
    const int pad = width - 1 - visible(text);
    printf("\x1b[%d;1H%s\x1b[0m%*s", y, text, pad > 0 ? pad : 0, "");
}

// --- the settings -------------------------------------------------------------------------------------------------

struct Option {
    const char* value;  // nullptr: the line taken out of the file (the game's default)
    const char* label;
};
struct Item {
    const char* key;
    const char* name;
    const char* help;
    int def;  // the option the game takes without the line
    Option options[8];
};

const Item kItems[] = {
    {"RE4_CHEATS", "Читы",
     "Бессмертие Леона и Эшли (кроме захватов и ловушек), бесконечные патроны, QTE нажимаются сами, враги с одного "
     "удара. Меняется и в игре, на нижнем экране.",
     0,
     {{"0", "выкл"}, {"1", "бессмертие"}, {"3", "бессм.+патроны"}, {"11", "бессм.+патр.+QTE"},
      {"7", "бессм.+патр.+удар"}, {"15", "всё"}}},
    {"RE4_TEST_CHEATS", "Деньги",
     "+1 000 000 песет один раз за запуск, через секунду после того, как Леон впервые свободен.", 0,
     {{nullptr, "нет"}, {"free+60,3", "+1 млн за запуск"}}},
    {"RE4_GYRO", "Гироскоп",
     "Прицел наклоном и поворотом приставки, пока зажата R, и в оптике винтовки и в бинокль; стик работает как раньше. Множитель: при 2x поворот приставки на 10 градусов двигает прицел на 20. При записи и повторе ввода выключен.", 3,
     {{"0", "выкл"}, {"10", "1x"}, {"15", "1,5x"}, {"20", "2x"}, {"25", "2,5x"}, {"30", "3x"}, {"40", "4x"}}},
    {"RE4_DRAWDIST", "Дальность",
     "Туман и дальняя граница ближе: вдали рисуется меньше, быстрее. Работает в комнатах с туманом (почти везде). "
     "Меняется и в игре, на нижнем экране.",
     0,
     {{"100", "100% (как в игре)"}, {"90", "90%"}, {"80", "80%"}, {"70", "70%"}, {"60", "60%"}, {"50", "50%"}}},
    {"RE4_POTATO", "Эффекты",
     "Меньше эффектов - быстрее (r118 до -4 мс). Погода и пар: дождь, снег, туман-частицы, искажения. Частицы: меньше - каждая вторая кровь, пыль, дым; минимум - огонь и взрывы наполовину. Тени - и в роликах, с бликами. Попадания остаются. Меняется и на нижнем экране.",
     0,
     {{"0", "все"}, {"3", "без погоды и пара"}, {"4", "частиц меньше"}, {"8", "частиц минимум"},
      {"16", "без теней, бликов"}, {"20", "меньше, без теней"}, {"24", "минимум, без теней"}, {"31", "всё урезано"}}},
    {"RE4_3DS_FOG", "Туман",
     "Как рендерер 3DS рисует туман игры. Упрощённый — по таблице глубины; выкл — без тумана (вид другой, "
     "дальность выше не действует на вид).",
     0,
     {{"2", "как в игре"}, {"1", "упрощённый"}, {"0", "выкл"}}},
    {"RE4_3DS_FARLIGHT", "Дальний свет",
     "Источники света дальше этого расстояния считаются направленными: дешевле для GPU, свет вдали чуть грубее.", 1,
     {{"0", "всё точно"}, {"50000", "стандарт"}, {"20000", "быстрее"}, {"10000", "ещё быстрее"}}},
    {"RE4_3DS_LIGHTS", "Освещение",
     "Выкл — модели без освещения вовсе. Только для опытов со скоростью.", 0, {{"1", "вкл"}, {"0", "выкл (опыт)"}}},
    {"RE4_3DS_WATER", "Вода", "Поверхность воды (копия кадра). Выкл — немного быстрее, но вода пропадает.", 0,
     {{"1", "вкл"}, {"0", "выкл"}}},
    {"RE4_3DS_MIPMAP", "Мип-текстуры",
     "Уменьшенные копии текстур для дальних поверхностей. Выкл — рябь вдали; для опытов.", 0,
     {{"1", "вкл"}, {"0", "выкл (опыт)"}}},
    {"RE4_3DS_ASPECT", "Экран",
     "Широкий — картинка во всю ширину верхнего экрана (как на ТВ 16:9 с полосами игры); 4:3 — с чёрным по бокам; "
     "растянуть — весь кадр GameCube на экран.",
     0,
     {{"2", "широкий"}, {"1", "4:3"}, {"0", "растянуть"}}},
    {"RE4_3DS_HUD", "Нижний экран",
     "Игра: кнопки кейса и карты касанием (дальше — карта, здоровье, патроны). Панель скорости — для замеров, "
     "с переключателями касанием. Игра + FPS — то же, что «игра», и мелко кадры в секунду в правом верхнем углу. "
     "Только FPS — одна строка.", 1,
     {{"3", "игра"}, {"4", "игра + FPS"}, {"2", "панель скорости"}, {"1", "только FPS"}, {"0", "пусто"}}},
    {"RE4_3DS_LOG", "Лог на SD", "Лог прогона в sdmc:/3ds/pueblo/pueblo.log — по нему я разбираю ошибки и скорость.", 0,
     {{"1", "вкл"}, {"0", "выкл"}}},
    {"RE4_3DS_TEXMISS", "Промахи текстур",
     "Текстуры, которых нет в пакете, записываются в texmiss.dump — из них я собираю следующий пакет.", 0,
     {{"0", "выкл"}, {"1", "записывать"}}},
    {"RE4_3DS_MENU", "Это меню", "Когда показывать это меню: по кнопке L при запуске или каждый раз.", 0,
     {{"0", "по кнопке L"}, {"1", "всегда"}}},
};
const int kItemCount = (int) (sizeof(kItems) / sizeof(kItems[0]));

int optionCount(const Item& it)
{
    int n = 0;
    while (n < 8 && it.options[n].label) {
        n++;
    }
    return n;
}

int itemIndex(const char* key)
{
    for (int i = 0; i < kItemCount; i++) {
        if (!strcmp(kItems[i].key, key)) {
            return i;
        }
    }
    return -1;
}

// The option of `value` (nullptr: no line), or -1
int optionOf(const Item& it, const char* value)
{
    const int n = optionCount(it);
    for (int k = 0; k < n; k++) {
        const char* v = it.options[k].value;
        if ((!v && (!value || !*value)) || (v && value && !strcmp(v, value))) {
            return k;
        }
    }
    return value ? -1 : it.def;
}

// --- the menu before the game -------------------------------------------------------------------------------------

const char kPath[] = "sdmc:/3ds/pueblo/pueblo.cfg";
const u64 kAskMs = 700;

struct Menu {
    std::vector<std::string> lines;  // the file
    int choice[sizeof(kItems) / sizeof(kItems[0])];
    std::string custom[sizeof(kItems) / sizeof(kItems[0])];  // a value of the file that is no option (choice -1)
    int sel = 0;
    std::vector<std::string> script;  // RE4_3DS_MENU_KEYS
    size_t step = 0;
    bool shot = false;
    int shots = 0;
};

// The value of `key` in the file's lines (nullptr: none); the last line wins, as readSettings has it
const char* lineValue(const std::vector<std::string>& lines, const char* key, int* at = nullptr)
{
    const size_t len = strlen(key);
    const char* v = nullptr;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string& l = lines[i];
        if (l.size() > len && l[len] == '=' && !l.compare(0, len, key)) {
            v = l.c_str() + len + 1;
            if (at) {
                *at = (int) i;
            }
        }
    }
    return v;
}

void readLines(std::vector<std::string>& lines)
{
    FILE* f = fopen(kPath, "r");
    if (!f) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        lines.push_back(line);
    }
    fclose(f);
}

PrintConsole gTop, gBottom;

void dumpScreen(gfxScreen_t screen, const char* path)
{
    u16 w, h;
    u8* fb = gfxGetFramebuffer(screen, GFX_LEFT, &w, &h);
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(fb, 2, (size_t) w * h, f);
        fclose(f);
    }
}

void wrap(int y0, int rows, int width, const char* utf)
{
    char text[640];
    hudText(utf, text, sizeof(text));
    const char* p = text;
    for (int r = 0; r < rows; r++) {
        while (*p == ' ') {
            p++;
        }
        int n = (int) strlen(p);
        if (n > width - 1) {
            n = width - 1;
            while (n > 0 && p[n] != ' ') {
                n--;
            }
            if (n == 0) {
                n = width - 1;
            }
        }
        printf("\x1b[%d;1H%.*s%*s", y0 + r, n, p, width - 1 - n, "");
        p += n;
    }
}

void drawMenu(Menu& m)
{
    consoleSelect(&gBottom);
    rowText(1, 40, "\x1b[1;36m  НАСТРОЙКИ ЗАПУСКА");
    for (int i = 0; i < kItemCount; i++) {
        const Item& it = kItems[i];
        const bool sel = i == m.sel;
        std::string label = m.choice[i] >= 0 ? it.options[m.choice[i]].label : ("своё: " + m.custom[i].substr(0, 10));
        const char* color = sel ? "\x1b[1;33m" : m.choice[i] != it.def ? "\x1b[1;32m" : "";
        // the name in 16 columns (Cyrillic: two bytes a letter in UTF-8, padded by hand)
        char name[64];
        hudText(it.name, name, sizeof(name));
        const int pad = 16 - visible(name);
        rowText(3 + i, 40, "%s%s%s%*s%s%s%s", color, sel ? "►" : " ", it.name, pad > 0 ? pad : 0, "", sel ? "◄ " : "  ",
                label.c_str(), sel ? " ►" : "");
    }
    rowText(4 + kItemCount, 40, "");
    rowText(5 + kItemCount, 40, "\x1b[1;36m A/START — играть   Y — по умолчанию");
    rowText(6 + kItemCount, 40, "\x1b[1;36m B — играть без изменений");
    consoleSelect(&gTop);
    rowText(1, 50, "\x1b[1;36m RE4 3DS — настройки запуска");
    rowText(2, 50, "");
    const Item& it = kItems[m.sel];
    rowText(3, 50, "\x1b[1;33m %s", it.name);
    wrap(4, 6, 50, it.help);
    rowText(11, 50, "");
    rowText(12, 50, " ↑↓ — пункт, ←→ — значение");
    rowText(13, 50, " A или START — сохранить и играть");
    rowText(14, 50, " B — играть, ничего не меняя");
    rowText(15, 50, " Y — всё как в игре по умолчанию");
    rowText(17, 50, " Зелёным — не как по умолчанию.");
    rowText(18, 50, " Сохраняется в sdmc:/3ds/pueblo/pueblo.cfg (прочие");
    rowText(19, 50, " строки файла остаются как были).");
    rowText(21, 50, " Дальность, эффекты и читы меняются и в игре —");
    rowText(22, 50, " касанием нижнего экрана (до выхода из игры).");
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
    if (m.shot) {
        char path[64];
        snprintf(path, sizeof(path), "sdmc:/3ds/pueblo/menu-%02d-top.fb", m.shots);
        dumpScreen(GFX_TOP, path);
        snprintf(path, sizeof(path), "sdmc:/3ds/pueblo/menu-%02d-bot.fb", m.shots);
        dumpScreen(GFX_BOTTOM, path);
        m.shots++;
    }
}

// The file with the menu's choices: a line changed in its place, a new one at the end, a default with no value
// taken out. false: not written
bool save(Menu& m)
{
    std::vector<std::string> lines = m.lines;
    for (int i = 0; i < kItemCount; i++) {
        if (m.choice[i] < 0) {
            continue;  // its own value, kept
        }
        const Item& it = kItems[i];
        // the game's default: no line (the defaults here are the game's own; a later change of them follows)
        const char* v = m.choice[i] == it.def ? nullptr : it.options[m.choice[i]].value;
        const size_t len = strlen(it.key);
        bool done = false;
        for (size_t k = 0; k < lines.size();) {
            std::string& l = lines[k];
            if (l.size() > len && l[len] == '=' && !l.compare(0, len, it.key)) {
                if (!v || done) {
                    lines.erase(lines.begin() + (long) k);
                    continue;
                }
                l = std::string(it.key) + "=" + v;
                done = true;
            }
            k++;
        }
        if (v && !done) {
            lines.push_back(std::string(it.key) + "=" + v);
        }
    }
    // the test hooks do not stay
    for (size_t k = 0; k < lines.size();) {
        if (!lines[k].compare(0, 18, "RE4_3DS_MENU_KEYS=") || !lines[k].compare(0, 18, "RE4_3DS_MENU_SHOT=")) {
            lines.erase(lines.begin() + (long) k);
        } else {
            k++;
        }
    }
    if (lines == m.lines) {
        return true;
    }
    FILE* f = fopen(kPath, "w");
    if (!f) {
        return false;
    }
    for (const std::string& l : lines) {
        fprintf(f, "%s\n", l.c_str());
    }
    fclose(f);
    return true;
}

u32 scriptKey(Menu& m)
{
    if (m.step >= m.script.size()) {
        return KEY_A;  // the script over: play
    }
    const std::string& k = m.script[m.step++];
    return k == "U" ? KEY_DUP : k == "D" ? KEY_DDOWN : k == "L" ? KEY_DLEFT : k == "R" ? KEY_DRIGHT : k == "A" ? KEY_A
         : k == "B" ? KEY_B : k == "Y" ? KEY_Y : 0;
}

}  // namespace

// Before readSettings reads the file (main_3ds.cpp, the constructor of priority 101: the SD card and the buttons are
// there by then, __appInit). The screens are taken for the menu and given back (main's gfxInitDefault takes them again).
extern "C" void re4_3ds_settings_menu(void)
{
    Menu m;
    readLines(m.lines);
    const char* always = lineValue(m.lines, "RE4_3DS_MENU");
    if (const char* keys = lineValue(m.lines, "RE4_3DS_MENU_KEYS")) {
        std::string s = keys;
        size_t p = 0;
        while (p <= s.size()) {
            size_t c = s.find(',', p);
            if (c == std::string::npos) {
                c = s.size();
            }
            if (c > p) {
                m.script.push_back(s.substr(p, c - p));
            }
            p = c + 1;
        }
    }
    const char* shot = lineValue(m.lines, "RE4_3DS_MENU_SHOT");
    m.shot = shot && *shot == '1';
    gfxInitDefault();
    consoleInit(GFX_TOP, &gTop);
    consoleInit(GFX_BOTTOM, &gBottom);
    consoleSetFont(&gTop, &gFont);
    consoleSetFont(&gBottom, &gFont);
    bool open = (always && *always == '1') || !m.script.empty();
    if (!open) {
        consoleSelect(&gTop);
        rowText(13, 50, "\x1b[1;36m                 RE4 3DS");
        rowText(15, 50, "         Держи L — настройки запуска");
        const u64 from = osGetTime();
        while (!open && osGetTime() - from < kAskMs && aptMainLoop()) {
            hidScanInput();
            open = (hidKeysHeld() & KEY_L) != 0;
            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();
        }
    }
    if (!open) {
        gfxExit();
        return;
    }
    for (int i = 0; i < kItemCount; i++) {
        const char* v = lineValue(m.lines, kItems[i].key);
        m.choice[i] = optionOf(kItems[i], v);
        if (m.choice[i] < 0) {
            m.custom[i] = v;
        }
    }
    hidSetRepeatParameters(20, 6);
    consoleSelect(&gTop);
    consoleClear();
    consoleSelect(&gBottom);
    consoleClear();
    bool play = false, keep = false;
    while (!play) {
        drawMenu(m);
        if (!aptMainLoop()) {
            gfxExit();
            exit(0);
        }
        u32 down;
        if (!m.script.empty()) {
            down = scriptKey(m);
        } else {
            hidScanInput();
            down = hidKeysDownRepeat();
        }
        const Item& it = kItems[m.sel];
        const int n = optionCount(it);
        int& c = m.choice[m.sel];
        if (down & KEY_DUP) {
            m.sel = (m.sel + kItemCount - 1) % kItemCount;
        } else if (down & KEY_DDOWN) {
            m.sel = (m.sel + 1) % kItemCount;
        } else if (down & KEY_DLEFT) {
            c = c < 0 ? it.def : (c + n - 1) % n;
        } else if (down & KEY_DRIGHT) {
            c = c < 0 ? it.def : (c + 1) % n;
        } else if (down & KEY_Y) {
            for (int i = 0; i < kItemCount; i++) {
                m.choice[i] = kItems[i].def;
            }
        } else if (down & (KEY_A | KEY_START)) {
            play = true;
        } else if (down & KEY_B) {
            play = keep = true;
        }
    }
    bool saved = keep || save(m);
    consoleSelect(&gTop);
    consoleClear();
    rowText(14, 50, saved ? (keep ? "  Без изменений. Запуск…" : "  Сохранено. Запуск…")
                          : "\x1b[1;31m  Не удалось записать pueblo.cfg. Запуск…");
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
    if (m.shot) {
        dumpScreen(GFX_TOP, "sdmc:/3ds/pueblo/menu-end-top.fb");
    }
    gfxExit();
}

// --- the panel on the bottom screen -------------------------------------------------------------------------------

namespace {

int gHudMode = -1;
volatile bool gHudOwns;  // the panel draws the console (system_3ds.cpp: its own lines go to the log only)
volatile bool gHudStop;
volatile bool gHudPause, gHudPaused;  // re4_3ds_hud_pause (HOME, stage 6.2c)
volatile bool gHudDirty;
const u64 kHudMs = 500;
volatile u32 gHudFrames;
// (stage 8.2) the bottom screen: 0 the game's screen (RE4_3DS_HUD=3), 1 the speed panel (2); the button under the
// finger (-1 none); the game's screen drawn whole once (its background)
int gPage = 0;
volatile int gPressed = -1;
bool gGameDrawn;
volatile float gHudWorst;
volatile u32 gHudOver33, gHudOver50;
u32 gHudShot;

int liveItem(int row)  // the switches: rows 16, 19, 22 (1-based), a row above and below theirs
{
    static const int kRows[3] = {16, 19, 22};
    for (int i = 0; i < 3; i++) {
        if (row >= kRows[i] - 1 && row <= kRows[i] + 1) {
            return i;
        }
    }
    return -1;
}

const Item& liveDef(int i)
{
    static const int kIndex[3] = {itemIndex("RE4_DRAWDIST"), itemIndex("RE4_POTATO"), itemIndex("RE4_CHEATS")};
    return kItems[kIndex[i]];
}

int liveValue(int i)
{
    return i == 0 ? (int) (re4_draw_dist() * 100.0f + 0.5f) : i == 1 ? re4_potato_mask() : re4_cheats_wanted();
}

int liveChoice(int i)
{
    char v[16];
    snprintf(v, sizeof(v), "%d", liveValue(i));
    return optionOf(liveDef(i), v);
}

void drawHud(double ms, u32 frames, double worst, u32 over33, u32 over50, double gxBusy, double gameWait,
             double gpuBusy)
{
    const int room = re4_room_now();
    if (room >= 0) {
        rowText(1, 40, "\x1b[1;36m RE4 3DS        комната r%03x", (unsigned) room);
    } else {
        rowText(1, 40, "\x1b[1;36m RE4 3DS");
    }
    rowText(2, 40, "\x1b[1;36m───────────────────────────────────────");
    if (!frames) {
        rowText(3, 40, "\x1b[1;33m  загрузка / ожидание…");
        rowText(4, 40, "");
        rowText(5, 40, "");
    } else {
        const double avg = ms / frames, fps = frames * 1000.0 / ms;
        const char* color = avg <= 34.0 ? "\x1b[1;32m" : avg <= 42.0 ? "\x1b[1;33m" : "\x1b[1;31m";
        rowText(3, 40, "%s  FPS %5.1f      кадр %5.1f мс", color, fps, avg);
        rowText(4, 40, "  худший %5.1f мс  >33 %3u%%  >50 %3u%%", worst, over33 * 100 / frames,
                over50 * 100 / frames);
        // the frame's time: 0..50 ms in 30 cells, the mark at 33.3 ms (cell 20)
        char bar[160] = "";
        const int cells = avg >= 50.0 ? 30 : (int) (avg * 30.0 / 50.0 + 0.5);
        for (int k = 0; k < 30; k++) {
            strcat(bar, k == 20 ? (k < cells ? "▓" : "│") : k < cells ? "█" : "░");
        }
        rowText(5, 40, "%s  %s", color, bar);
    }
    rowText(6, 40, "");
    rowText(7, 40, "  Игра ждала GX/GPU  %6.1f мс/кадр", gameWait);
    rowText(8, 40, "  Поток GX занят     %6.1f мс/кадр", gxBusy);
    if (gpuBusy >= 0.0) {
        rowText(9, 40, "  GPU занят          %6.1f мс/кадр", gpuBusy);
    } else {
        rowText(9, 40, "  GPU занят               —");
    }
    struct mallinfo mi = mallinfo();
    rowText(10, 40, "  Своб.: VRAM %4u КБ, линейн. %5u КБ", (unsigned) re4_hud_vram_kb, (unsigned) re4_hud_linear_kb);
    rowText(11, 40, "  Куча C занята %5.1f МБ", mi.uordblks / 1048576.0);
    rowText(12, 40, "");
    rowText(13, 40, "\x1b[1;36m── на лету: касание: слева -, справа + ──");
    rowText(14, 40, "");
    for (int i = 0; i < 3; i++) {
        const Item& it = liveDef(i);
        const int c = liveChoice(i);
        char own[24];
        snprintf(own, sizeof(own), "%d", liveValue(i));
        char name[64];
        hudText(it.name, name, sizeof(name));
        const int pad = 12 - visible(name);
        const char* color = c != it.def ? "\x1b[1;32m" : "";
        rowText(16 + i * 3, 40, "  %s◄  %s%*s%s%s", color, it.name, pad > 0 ? pad : 0, "", c >= 0 ? it.options[c].label : own,
                "");
        // the right arrow at the row's end
        printf("\x1b[%d;37H%s\x1b[0m", 16 + i * 3, c != it.def ? "\x1b[1;32m\xC6" : "\xC6");
        rowText(17 + i * 3, 40, "");
        if (i < 2) {
            rowText(18 + i * 3, 40, "");
        }
    }
    rowText(24, 40, "");
    rowText(25, 40, "  Изменения на лету — до выхода из игры.");
    rowText(26, 40, "  Все настройки — держи L при запуске.");
    rowText(27, 40, "");
    rowText(28, 40, "  START+SELECT 1 с — выход");
}

// --- (stage 8.2) the game's page: pixels straight into the bottom screen's framebuffer ------------------------------

const u16 kBg = 0x0841, kBar = 0x10A2, kText = 0xE71C, kDim = 0x8410, kAccent = 0xFE60;

struct Button {
    int x, y, w, h;
    const char* label;
    u16 padBit;  // GameCube pad bit while held
    u16 fill, lit;
};
// GameCube pad bits: Y 0x0800 (the case), Z 0x0010 (the map)
// (2026-11-27) the strip of ui_bottom_3ds.cpp: the D-pad weapons' four cells (up, left, right, down: their list - no
// pad bit), the case (Y while held), the settings' gear (a request); the big CASE / MAP buttons gone (the 3DS's own)
const Button kButtons[] = {
    {6, 206, 60, 32, "^", 0, 0x4208, 0x9CD3},   {64, 206, 60, 32, "<", 0, 0x4208, 0x9CD3},
    {122, 206, 60, 32, ">", 0, 0x4208, 0x9CD3}, {180, 206, 60, 32, "v", 0, 0x4208, 0x9CD3},
    {240, 206, 38, 32, "[]", 0, 0x4208, 0x9CD3},  // (2026-11-27: the map / the inventory; the case on Y)
    {278, 206, 40, 32, "*", 0, 0x4208, 0x9CD3},
};
const int kCellSlot[4] = {0, 2, 3, 1};  // (the cells' arrows: up, left, right, down)
const int kButtonCount = (int) (sizeof(kButtons) / sizeof(kButtons[0]));

u16* bottomFb()
{
    return (u16*) gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr);
}

// the game's screen drawn here first, then into the framebuffer at once (drawn there piece by piece it flickered)
u16 gBack[320 * 240];

void fillRect(u16* fb, int x, int y, int w, int h, u16 c)
{
    for (int px = x < 0 ? 0 : x; px < x + w && px < 320; px++) {
        u16* col = fb + px * 240;
        for (int py = y < 0 ? 0 : y; py < y + h && py < 240; py++) {
            col[239 - py] = c;
        }
    }
}

// UTF-8 text in the panel's font (hudText's codes), each dot `scale` pixels, its top left at (x, y); its width back
int textAt(u16* fb, int x, int y, int scale, const char* utf, u16 c)
{
    char codes[128];
    hudText(utf, codes, sizeof(codes));
    int cx = x;
    for (const char* s = codes; *s; s++) {
        const u8* glyph = &kHudFont[(u8) *s * 8];
        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 8; col++) {
                if (glyph[row] & (0x80 >> col)) {
                    fillRect(fb, cx + col * scale, y + row * scale, scale, scale, c);
                }
            }
        }
        cx += 8 * scale;
    }
    return cx - x;
}

int textWidth(const char* utf, int scale)
{
    char codes[128];
    hudText(utf, codes, sizeof(codes));
    return (int) strlen(codes) * 8 * scale;
}

void drawButton(u16* fb, int i, bool lit)
{
    const Button& b = kButtons[i];
    fillRect(fb, b.x, b.y, b.w, b.h, kBg);
    fillRect(fb, b.x + 1, b.y + 1, b.w - 2, b.h - 2, lit ? b.lit : b.fill);
    fillRect(fb, b.x + 1, b.y + 1, b.w - 2, 2, lit ? kAccent : 0x6B4D);  // a light edge on top
    const int scale = 2, tw = textWidth(b.label, scale);
    textAt(fb, b.x + (b.w - tw) / 2, b.y + (b.h - 8 * scale) / 2, scale, b.label, lit ? 0xFFFF : kText);
}

// (8.4) a bar of life: its label, the share full (green, yellow below a half, red below a quarter) and the numbers
void lifeBar(u16* fb, int y, const char* label, int life, int max)
{
    fillRect(fb, 0, y, 320, 14, kBg);
    textAt(fb, 8, y + 3, 1, label, kText);
    const int x = 56, w = 168;
    fillRect(fb, x, y + 1, w, 12, 0x2104);
    const int full = max > 0 ? (life <= 0 ? 0 : life >= max ? w - 2 : (w - 2) * life / max) : 0;
    const u16 c = max > 0 && life * 4 < max ? 0xE8E4 : max > 0 && life * 2 < max ? 0xEEA0 : 0x2EA6;
    fillRect(fb, x + 1, y + 2, full, 10, c);
    char n[32];
    snprintf(n, sizeof(n), "%d/%d", life < 0 ? 0 : life, max);
    textAt(fb, 314 - textWidth(n, 1), y + 3, 1, n, kText);
}

// (8.4) the health, the ammo, the pesetas: rows 22..83 (empty outside the game)
void drawStatus(u16* fb)
{
    const int life = re4_hud_life;
    fillRect(fb, 0, 20, 320, 66, kBg);
    if (life < 0) {
        return;
    }
    lifeBar(fb, 22, "ЛЕОН", life, re4_hud_life_max);
    if (re4_hud_ashley >= 0) {
        lifeBar(fb, 38, "ЭШЛИ", re4_hud_ashley, re4_hud_ashley_max);
    }
    char t[48];
    const int ammo = re4_hud_ammo, reserve = re4_hud_reserve;
    textAt(fb, 8, 56, 1, "ПАТРОНЫ", kDim);  // (the labels above the numbers: 8 digits of pesetas reached the ammo)
    if (ammo >= 0) {
        if (reserve >= 0) {
            snprintf(t, sizeof(t), "%d / %d", ammo, reserve);
        } else {
            snprintf(t, sizeof(t), "%d", ammo);
        }
        textAt(fb, 8, 66, 2, t, ammo == 0 ? 0xE8E4 : 0xFFFF);
    } else {
        textAt(fb, 8, 66, 2, "—", kDim);
    }
    snprintf(t, sizeof(t), "%d", re4_hud_peseta);
    const int tw = textWidth(t, 2);
    textAt(fb, 314 - tw, 66, 2, t, kAccent);
    textAt(fb, 314 - textWidth("ПЕСЕТЫ", 1), 56, 1, "ПЕСЕТЫ", kDim);
}

// (8.4) the numbers since the last picture (the panel's thread looks every 50 ms)
extern "C" volatile int re4_inv_view, re4_inv_pressed;  // (2026-11-27) ui_bottom_3ds.cpp: the inventory
extern "C" volatile unsigned re4_inv_gen;              //   runtime/cheats.cpp
extern "C" volatile int re4_inv_tap;
extern "C" int re4_ui_inv_hit(int x, int y, int* slot, int* id);
extern "C" int re4_ui_inv_edit_touch(int phase, int x, int y);  // (2026-11-27) the inventory's edits
extern "C" volatile int re4_inv_edit, re4_inv_sel, re4_inv_ask, re4_inv_ghost, re4_inv_refused, re4_inv_op_done,
    re4_inv_trayturn;

bool statusChanged()
{
    static int last[19] = {-2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2};
    const int slots = re4_wep_slot[0] | re4_wep_slot[1] << 8 | re4_wep_slot[2] << 16 | re4_wep_slot[3] << 24;
    const int now[19] = {re4_hud_life, re4_hud_life_max, re4_hud_ashley, re4_hud_ammo, re4_hud_reserve, re4_hud_peseta,
                         re4_wep_equipped, slots, re4_wep_owned_n,  // (2026-11-27: the D-pad weapons' cells)
                         re4_inv_view, re4_inv_view ? (int) re4_inv_gen : 0, re4_inv_pressed,  // (the inventory)
                         re4_inv_edit, re4_inv_sel, re4_inv_ask, re4_inv_ghost, re4_inv_refused, re4_inv_op_done,  // (its edits)
                         re4_inv_trayturn};
    bool changed = false;
    for (int i = 0; i < 19; i++) {
        changed = changed || now[i] != last[i];
        last[i] = now[i];
    }
    return changed;
}

void drawGame(double ms, u32 frames)
{
    u16* fb = gBack;
    if (re4_ui_game(fb, gPressed)) {  // (stage 9) the look "B - the dark" (3ds/ui_bottom_3ds.cpp; RE4_3DS_UI=0: this one)
        if (gHudMode == 4 && frames) {  // (2026-11-09) "the game + FPS" (the default): the frame rate small in the top right corner, above the life
            char fps[16];
            snprintf(fps, sizeof(fps), "%.1f", frames * 1000.0 / ms);
            const int w = textWidth(fps, 1);
            fillRect(fb, 316 - w - 2, 0, w + 4, 9, 0x0000);
            textAt(fb, 316 - w, 1, 1, fps, ms / frames <= 34.0 ? 0x87F0 : kAccent);
        }
        memcpy(bottomFb(), gBack, sizeof(gBack));
        return;
    }
    if (!gGameDrawn) {
        fillRect(fb, 0, 0, 320, 240, kBg);
        gGameDrawn = true;
    }
    // the top line: the room and the frame rate
    fillRect(fb, 0, 0, 320, 18, kBar);
    char line[96];
    const int room = re4_room_now();
    if (room >= 0) {
        snprintf(line, sizeof(line), "RE4 3DS · r%03x", (unsigned) room);
    } else {
        snprintf(line, sizeof(line), "RE4 3DS");
    }
    textAt(fb, 6, 5, 1, line, kText);
    if (frames) {
        snprintf(line, sizeof(line), "%4.1f к/с", frames * 1000.0 / ms);
        textAt(fb, 314 - textWidth(line, 1), 5, 1, line, ms / frames <= 34.0 ? 0x87F0 : kAccent);
    }
    drawStatus(fb);  // (8.4)
    if (!re4_3ds_minimap_draw(fb, 2, 88, 316, 116)) {  // (8.3) the map, or why not
        fillRect(fb, 2, 88, 316, 116, 0x0000);
        const char* none = "карта недоступна";
        textAt(fb, 160 - textWidth(none, 1) / 2, 142, 1, none, kDim);
    }
    for (int i = 0; i < kButtonCount; i++) {
        drawButton(fb, i, gPressed == i);
    }
    memcpy(bottomFb(), gBack, sizeof(gBack));  // (at once: no flicker)
}

// (R2) the settings screen: made anew in gBack when it changed (a value, the finger, the language) or gBack held
// the game's screen since; copied to the screen each time (the start's console lines, the HOME menu: over it)
bool gSetupShown, gSetupNew;

void drawSetup()
{
    if (!gSetupShown || gSetupNew) {
        gSetupNew = false;
        gSetupShown = true;
        re4_3ds_setup_draw(gBack);
    }
    memcpy(bottomFb(), gBack, sizeof(gBack));
}

int buttonAt(int x, int y)
{
    for (int i = 0; i < kButtonCount; i++) {
        const Button& b = kButtons[i];
        if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) {
            return i;
        }
    }
    return -1;
}

void hudThread(void*)
{
    // the first game frame (counted from the room's main loop on): the start's lines stay until then; (R2) the
    // settings screen at once (the title has no counted frames)
    while (!gHudFrames && !gHudStop && !re4_3ds_setup_visible()) {
        svcSleepThread(100000000LL);
    }
    consoleClear();
    gHudOwns = true;
    u64 last = svcGetSystemTick(), gxLast = re4_hud_gx_busy, waitLast = re4_hud_game_wait;
    u32 framesLast = gHudFrames, busyLast = re4_hud_gpu_busy, allLast = re4_hud_gpu_all;
    double ms = 0.0, worst = 0.0, gx = 0.0, wait = 0.0, gpu = -1.0;
    u32 frames = 0, over33 = 0, over50 = 0;
    bool first = true;
    while (!gHudStop) {
        if (gHudPause) {  // (HOME: the screens are the HOME menu's)
            gHudPaused = true;
            svcSleepThread(20000000LL);
            continue;
        }
        gHudPaused = false;
        const u64 now = svcGetSystemTick();
        const double since = (double) (now - last) / CPU_TICKS_PER_MSEC;
        if (since >= kHudMs || first) {
            const u32 f = gHudFrames, gpuB = re4_hud_gpu_busy, gpuA = re4_hud_gpu_all;
            const u64 gxNow = re4_hud_gx_busy, waitNow = re4_hud_game_wait;
            frames = f - framesLast;
            ms = since;
            worst = gHudWorst;
            gHudWorst = 0.0f;
            over33 = __atomic_exchange_n(&gHudOver33, 0, __ATOMIC_SEQ_CST);
            over50 = __atomic_exchange_n(&gHudOver50, 0, __ATOMIC_SEQ_CST);
            const double perFrame = frames ? (double) CPU_TICKS_PER_MSEC * frames : 0.0;
            gx = perFrame > 0.0 ? (double) (gxNow - gxLast) / perFrame : 0.0;
            wait = perFrame > 0.0 ? (double) (waitNow - waitLast) / perFrame : 0.0;
            gpu = gpuA != allLast && frames ? (double) (gpuB - busyLast) / (gpuA - allLast) * since / frames : -1.0;
            framesLast = f;
            gxLast = gxNow;
            waitLast = waitNow;
            busyLast = gpuB;
            allLast = gpuA;
            last = now;
            first = false;
            gHudDirty = true;
        }
        if (re4_3ds_setup_dirty()) {  // (R2) the settings screen
            gSetupNew = true;
            gHudDirty = true;
        }
        if (gPage == 0 && (statusChanged() || re4_3ds_minimap_dirty())) {  // (8.4) the health / ammo, (8.3) the map
            gHudDirty = true;
        }
        if (gHudDirty) {
            gHudDirty = false;
            if (gPage == 0 && re4_3ds_setup_visible()) {  // (R2) the settings before the gameplay
                drawSetup();
            } else if (gPage == 0) {  // (8.2) the game's screen
                gSetupShown = false;
                drawGame(ms, frames);
            } else {
                drawHud(ms, frames, worst, over33, over50, gx, wait, gpu);
            }
            u16 w, h;
            u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
            GSPGPU_FlushDataCache(fb, (u32) w * h * 2);
            if (gHudShot && --gHudShot == 0) {
                dumpScreen(GFX_BOTTOM, "sdmc:/3ds/pueblo/hud.fb");
            }
        }
        re4_3ds_setup_poll();  // (R2) settings.bin after a change
        svcSleepThread(50000000LL);
    }
}

}  // namespace

// RE4_3DS_HUD (2 the panel; RE4_3DS_FPS=0 of before: 0)
extern "C" int re4_3ds_hud_mode(void)
{
    if (gHudMode < 0) {
        const char* v = getenv("RE4_3DS_HUD");
        const char* fps = getenv("RE4_3DS_FPS");
        gHudMode = v ? atoi(v) : fps && *fps == '0' ? 0 : 4;  // (8.2: 3 the game's screen; 2026-11-09: 4 with the frame rate, the default)
#ifdef RE4_3DS_RELEASE
        if (gHudMode != 3 && gHudMode != 4) {  // (R1) the release: the game's screen only (no speed panel, R0)
            gHudMode = 3;
        }
#endif
        gPage = gHudMode == 2 ? 1 : 0;
    }
    return gHudMode;
}

extern "C" int re4_3ds_hud_owns_console(void)
{
    return gHudOwns;
}

extern "C" void re4_3ds_console_font(PrintConsole* console)
{
    consoleSetFont(console, &gFont);
}

// main_3ds.cpp, the renderer started: the panel's thread (core 1, below everything; else core 0)
extern "C" void re4_3ds_hud_start(void)
{
    if (re4_3ds_hud_mode() != 2 && re4_3ds_hud_mode() != 3 && re4_3ds_hud_mode() != 4) {  // (8.2: 3 the game's screen; 4 with the frame rate)
        return;
    }
    if (const char* shot = getenv("RE4_3DS_HUD_SHOT")) {
        gHudShot = (u32) atoi(shot);
    }
    if (re4_3ds_hud_mode() != 2) {
        re4_3ds_setup_start();  // (R2) the settings screen until the gameplay
    }
    Thread t = threadCreate(hudThread, nullptr, 16 * 1024, 0x30, 1, true);  // (as the log's writer: 0x3F starved)
    int core = 1;
    if (!t) {
        core = 0;
        t = threadCreate(hudThread, nullptr, 16 * 1024, 0x3F, 0, true);
    }
    re4_log("3ds: the panel on the bottom screen (RE4_3DS_HUD=2): %s%d", t ? "a thread on core " : "no thread ", core);
}

// HOME (input_3ds.cpp, stage 6.2c): the panel's thread stands (no drawing, no cache flush through GSP) while the
// HOME menu has the screens; on: waits until it stands (at most 200 ms); off: drawn anew
extern "C" void re4_3ds_hud_pause(int on)
{
    if (!gHudOwns) {
        return;
    }
    if (on) {
        gHudPause = true;
        for (int i = 0; i < 20 && !gHudPaused; i++) {
            svcSleepThread(10000000LL);
        }
    } else {
        gHudPause = false;
        gHudDirty = true;
    }
}

// re4_3ds_finish (system_3ds.cpp): the console back to the end's lines
extern "C" void re4_3ds_hud_stop(void)
{
    if (gHudOwns) {
        gHudStop = true;
        svcSleepThread(120000000LL);
        gHudOwns = false;
        consoleClear();
    }
}

// Each game frame (system_3ds.cpp re4_3ds_frame): its time
extern "C" void re4_3ds_hud_frame(double ms)
{
    gHudFrames++;
    if (ms > gHudWorst) {
        gHudWorst = (float) ms;
    }
    if (ms > 1000.0 / 30.0 + 0.2) {
        __atomic_add_fetch(&gHudOver33, 1, __ATOMIC_SEQ_CST);
    }
    if (ms > 50.0) {
        __atomic_add_fetch(&gHudOver50, 1, __ATOMIC_SEQ_CST);
    }
}

// (stage 8.2) The touch screen each game frame (input_3ds.cpp, the game's thread): phase 1 a new touch, 0 held, -1 none.
// The GameCube pad bits to hold for the game (КЕЙС / КАРТА under the finger); on the speed panel (RE4_3DS_HUD=2) a new
// touch goes to its switches (re4_3ds_hud_touch).
extern "C" void re4_3ds_hud_touch(int x, int y);
extern "C" void re4_option_request(void);  // (2026-11-27) runtime/cheats.cpp
extern "C" int re4_3ds_setup_on(void);     // (2026-11-27) setup_3ds.cpp: the settings screen in use
extern "C" void re4_3ds_wep_pick(int slot);  // (2026-11-27) setup_3ds.cpp: the D-pad weapons' list
extern "C" unsigned re4_3ds_hud_pad(int phase, int x, int y)
{
    if (!gHudOwns) {
        return 0;
    }
    if (gPage == 0 && re4_3ds_setup_touch(phase, x, y)) {  // (R2) the settings screen took the touch
        if (gPressed != -1) {
            gPressed = -1;
        }
        return 0;
    }
    if (gPage == 1) {
        if (phase == 1) {
            re4_3ds_hud_touch(x, y);
        }
        return 0;
    }
    if (re4_ui_inv_edit_touch(phase, x, y)) {  // (2026-11-27) the inventory's edits took it
        re4_inv_pressed = -1;
        if (gPressed != -1) {
            gPressed = -1;
            gHudDirty = true;
        }
        return 0;
    }
    if (re4_inv_view && (phase < 0 || y < 205)) {  // (2026-11-27) the inventory: an item's touch
        int slot = 0, id = 0;
        const int hit = phase < 0 ? -1 : re4_ui_inv_hit(x, y, &slot, &id);
        if (phase == 1 && hit >= 0) {
            re4_inv_tap = (slot + 1) << 16 | id;
            re4_log("3ds: the inventory: item 0x%02x touched", id);
        }
        re4_inv_pressed = phase == 1 || (phase == 0 && re4_inv_pressed >= 0) ? hit : -1;
        if (gPressed != -1) {
            gPressed = -1;
            gHudDirty = true;
        }
        if (phase >= 0) {
            return 0;
        }
    }
    if (!re4_inv_view && re4_3ds_minimap_touch(phase, x, y)) {  // (8.3e) a touch begun in the map's box: its scroll or its buttons
        if (gPressed != -1) {
            gPressed = -1;
            gHudDirty = true;
        }
        return 0;
    }
    const int i = phase < 0 ? -1 : buttonAt(x, y);
    if (i != gPressed) {
        gPressed = i;
        gHudDirty = true;
    }
    if (i < 0) {
        return 0;
    }
    if (phase == 1) {
        re4_log("3ds: the bottom screen: %s", i < 4 ? "a weapon's cell" : i == 4 ? "the case" : "the settings");
        if (i == 5 && re4_3ds_setup_on()) {
            re4_option_request();  // (2026-11-27) the game's option screen, as by START (patch 0384)
        }
        if (i == 4) {  // (2026-11-27) the map / the inventory
            re4_inv_view = !re4_inv_view;
            gHudDirty = true;
        }
        if (i < 4 && re4_3ds_setup_on()) {
            re4_3ds_wep_pick(kCellSlot[i]);  // (2026-11-27) the option screen and the list for that arrow
        }
    }
    return kButtons[i].padBit;
}

// A touch of the bottom screen (input_3ds.cpp, the game's thread): a switch one step on (right half) or back
extern "C" void re4_3ds_hud_touch(int x, int y)
{
    if (!gHudOwns) {
        return;
    }
    const int i = liveItem(y / 8 + 1);
    if (i < 0) {
        return;
    }
    const Item& it = liveDef(i);
    const int n = optionCount(it);
    int c = liveChoice(i);
    if (c < 0) {
        c = it.def;
    } else if (x >= 160) {
        c = c + 1 < n ? c + 1 : c;
    } else {
        c = c > 0 ? c - 1 : c;
    }
    const int v = atoi(it.options[c].value ? it.options[c].value : "0");
    if (i == 0) {
        re4_draw_dist_set(v);
    } else if (i == 1) {
        re4_core_set_potato(v);
    } else {
        re4_core_set_cheats(v);
    }
    re4_log("3ds: the panel: %s %s", it.key, it.options[c].value ? it.options[c].value : "0");
    gHudDirty = true;
}

// (R2) setup_3ds.cpp, the gameplay starts: the frame counter in the game screen's corner by the settings
// (RE4_3DS_HUD 4 with it, 3 without; the speed panel and the rest left alone)
extern "C" void re4_3ds_hud_fps(int on)
{
    if (gHudMode == 3 || gHudMode == 4) {
        gHudMode = on ? 4 : 3;
        gHudDirty = true;
    }
}
