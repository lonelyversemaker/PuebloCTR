// The button mapping (2026-11-21, the project owner's wish: every action of the game on any button of the 3DS, or on
// two of them - the first held, the second pressed). The logic alone, without libctru: 3ds/controls_3ds.cpp feeds it
// the keys held and hands the GameCube buttons to the game; the host test (a Linux test program of this file)
// checks it. The idea as CubeEvil4's ActionMap.java (the Android app's "Button Mapping"): the game's actions, not the
// GameCube's buttons; in the game's menus (the title, the sub screens) the 3DS's usual layout, whatever the mapping.
//
// An action is on one key or on two (k[0] held, k[1] pressed), or on none. Its GameCube button (kOut) is pressed:
// - one key: while it is held; fire, reload and the knife's slash (kHold: they need the aim or the knife readied) -
//   only while their hold is given (the aim's or the knife's own keys held): "A while aiming", as the GameCube;
// - two keys: while both are held; for fire, reload and the slash the first key also readies their hold.
// The second key of a combination that acts gives only the combination (not its own single action as well), so does a
// key of fire / reload / slash while their hold is given. Keys no action uses: the D-pad's are the GameCube's D-pad (the
// game's few uses of it stay), the others nothing.
// assign() keeps the mapping clear: a binding another action had moves to that one's old place (they swap); the aim or
// the knife moved: the combinations holding it follow; a key that would be a plain action's and the first of a
// combination too (both would act at once) - the older one loses it (none).
#pragma once

#include <stdint.h>
#include <string.h>

namespace ctl {

// the 3DS's keys a mapping can use (SELECT is the port's: START + SELECT ends the game); kKeyPad* / kKeyC*: the Circle
// Pad's and the C stick's four directions - analog for the stick directions bound to them, a button past half way
// otherwise
enum Key { kKeyA, kKeyB, kKeyX, kKeyY, kKeyL, kKeyR, kKeyZL, kKeyZR, kKeyUp, kKeyDown, kKeyLeft, kKeyRight, kKeyStart,
           kKeyPadUp, kKeyPadDown, kKeyPadLeft, kKeyPadRight, kKeyCUp, kKeyCDown, kKeyCLeft, kKeyCRight, kKeys,
           kNone = 0xFF };
// the game's actions, in the order of the page
// (kUp..kRight: the control stick's directions - forward / back / turning, the aim's up / down / left / right;
// kLookUp..kLookRight: the C stick's - the camera, the rifle scope's zoom; kTurn: the quick turn - the port gives the
// stick back, then B: game/pl_class.cpp "back + B"; none of them a GameCube button)
enum Action { kUp, kDown, kLeft, kRight, kLookUp, kLookDown, kLookLeft, kLookRight, kAction, kRun, kTurn, kAim, kFire,
              kReload, kKnife, kSlash, kAshley, kCase, kMap, kOptions,
              // (2026-11-27) the D-pad weapons: the weapon of that arrow (the bottom screen assigns them; no GameCube
              // button: controls_3ds.cpp asks the game - patch 0385)
              kWepUp, kWepDown, kWepLeft, kWepRight, kActions };

// GameCube pad bits (PAD_BUTTON_*)
const uint16_t kGcLeft = 0x0001, kGcRight = 0x0002, kGcDown = 0x0004, kGcUp = 0x0008, kGcZ = 0x0010, kGcR = 0x0020,
               kGcL = 0x0040, kGcA = 0x0100, kGcB = 0x0200, kGcX = 0x0400, kGcY = 0x0800, kGcStart = 0x1000;

const uint16_t kOut[kActions] = {0, 0, 0, 0, 0, 0, 0, 0, kGcA, kGcB, 0, kGcR, kGcA, kGcB, kGcL, kGcA, kGcX, kGcY, kGcZ,
                                 kGcStart, 0, 0, 0, 0};
const int8_t kHold[kActions] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, kAim, kAim, -1, kKnife, -1, -1, -1, -1,
                               -1, -1, -1, -1};
// the usual layout (the menus; a key's GameCube button): A B X Y L R ZL(none) ZR=Z, the D-pad, START
const uint16_t kUsual[kKeys] = {kGcA, kGcB, kGcX, kGcY, kGcL, kGcR, 0, kGcZ, kGcUp, kGcDown, kGcLeft, kGcRight, kGcStart,
                                0, 0, 0, 0, 0, 0, 0, 0};

struct Binding {
    uint8_t k[2];  // kNone: unused; one key: k[1] == kNone
    int keys() const { return k[0] == kNone ? 0 : k[1] == kNone ? 1 : 2; }
    bool operator==(const Binding& o) const
    {
        return keys() == o.keys() && (keys() < 2 ? k[0] == o.k[0] : (k[0] == o.k[0] && k[1] == o.k[1]) ||
                                                                       (k[0] == o.k[1] && k[1] == o.k[0]));
    }
};

struct Map {
    Binding b[kActions];
};

inline Map defaults()
{
    Map m;
    const uint8_t d[kActions][2] = {{kKeyPadUp, kNone}, {kKeyPadDown, kNone}, {kKeyPadLeft, kNone}, {kKeyPadRight, kNone},
                                    {kKeyCUp, kNone}, {kKeyCDown, kNone}, {kKeyCLeft, kNone}, {kKeyCRight, kNone},
                                    {kKeyA, kNone}, {kKeyB, kNone}, {kNone, kNone}, {kKeyR, kNone}, {kKeyR, kKeyA}, {kKeyR, kKeyB},
                                    {kKeyL, kNone}, {kKeyL, kKeyA}, {kKeyX, kNone}, {kKeyY, kNone}, {kKeyZR, kNone},
                                    {kKeyStart, kNone}, {kKeyUp, kNone}, {kKeyDown, kNone}, {kKeyLeft, kNone},
                                    {kKeyRight, kNone}};
    for (int a = 0; a < kActions; a++) {
        m.b[a].k[0] = d[a][0];
        m.b[a].k[1] = d[a][1];
    }
    return m;
}

inline bool same(const Map& x, const Map& y)
{
    for (int a = 0; a < kActions; a++) {
        if (!(x.b[a] == y.b[a])) {
            return false;
        }
    }
    return true;
}

inline bool held(uint32_t keys, uint8_t k) { return k < kKeys && (keys >> k & 1); }
// (2026-11-27) action a's binding held whole: its key, or both of its two
inline bool acting(const Map& m, uint32_t keys, int a)
{
    const Binding& b = m.b[a];
    return b.keys() && held(keys, b.k[0]) && (b.keys() == 1 || held(keys, b.k[1]));
}
inline bool isHold(int a) { return a == kAim || a == kKnife; }

// whether action b's key k0 is the first of action c's combination with the two acting at once (not a hold of it)
inline bool clash(const Map& m, int single, int combo)
{
    const Binding& s = m.b[single];
    const Binding& c = m.b[combo];
    return single != combo && s.keys() == 1 && c.keys() == 2 && s.k[0] == c.k[0] && kHold[combo] != single &&
           kHold[single] < 0;
}

// The map with `action` on `nb` (keys < kKeys, two different ones or one; keys() 0: none). False: not taken.
inline bool assign(Map& m, int action, Binding nb)
{
    if (action < 0 || action >= kActions) {
        return false;
    }
    const int n = nb.keys();
    for (int i = 0; i < n; i++) {
        if (nb.k[i] >= kKeys) {
            return false;
        }
    }
    if (n == 2 && nb.k[0] == nb.k[1]) {
        return false;
    }
    if (n == 0) {
        nb.k[0] = nb.k[1] = kNone;
    } else if (n == 1) {
        nb.k[1] = kNone;
    }
    const Map before = m;
    const Binding old = m.b[action];
    // the same binding of another action of the same kind (a plain single, a "while held" single, a combination): swap
    for (int a = 0; a < kActions && n; a++) {
        if (a != action && m.b[a] == nb && (n == 2 || (kHold[a] < 0) == (kHold[action] < 0))) {
            m.b[a] = old;
        }
    }
    m.b[action] = nb;
    // the aim or the knife moved to another single key - set so or by a swap (the knife's L given to Ashley: the knife
    // on Ashley's X): the combinations that hold it follow (the slash L + A -> X + A)
    for (int h = 0; h < kActions; h++) {
        if (!isHold(h) || before.b[h].keys() != 1 || m.b[h].keys() != 1 || before.b[h].k[0] == m.b[h].k[0]) {
            continue;
        }
        for (int a = 0; a < kActions; a++) {
            Binding& c = m.b[a];
            if (kHold[a] == h && c.keys() == 2 && c.k[0] == before.b[h].k[0] && c.k[1] != m.b[h].k[0]) {
                c.k[0] = m.b[h].k[0];
            }
        }
    }
    // a key both a plain action's and the first of a combination (both acting at once): the one not just set loses it
    // (of two others - after a swap - the plain action)
    for (int s = 0; s < kActions; s++) {
        for (int c = 0; c < kActions; c++) {
            if (clash(m, s, c)) {
                const int lose = s == action ? c : s;
                m.b[lose].k[0] = m.b[lose].k[1] = kNone;
            }
        }
    }
    return true;
}

// The keys some action uses (alone or in a combination)
inline uint32_t usedKeys(const Map& m)
{
    uint32_t used = 0;
    for (int a = 0; a < kActions; a++) {
        for (int i = 0; i < m.b[a].keys(); i++) {
            used |= 1u << m.b[a].k[i];
        }
    }
    return used;
}

// The GameCube buttons of the keys held (a bit a Key) under the mapping; *turn: the quick turn's binding acts
inline uint16_t buttons(const Map& m, uint32_t keys, bool* turn = nullptr)
{
    uint16_t out = 0;
    bool turning = false;  // (the quick turn: its binding acts - no button here, controls_3ds.cpp gives back + B)
    uint32_t taken = 0;  // keys acting as the second of a combination or as fire / reload / slash: their plain action quiet
    // 1. the combinations
    for (int a = 0; a < kActions; a++) {
        const Binding& b = m.b[a];
        if (b.keys() != 2 || !held(keys, b.k[0])) {
            continue;
        }
        if (kHold[a] >= 0) {
            out |= kOut[kHold[a]];
        }
        if (held(keys, b.k[1])) {
            out |= kOut[a];
            turning |= a == kTurn;
            taken |= 1u << b.k[1];
        }
    }
    // 2. the holds' own single keys (the aim, the knife)
    for (int a = 0; a < kActions; a++) {
        const Binding& b = m.b[a];
        if (isHold(a) && b.keys() == 1 && held(keys, b.k[0]) && !(taken >> b.k[0] & 1)) {
            out |= kOut[a];
        }
    }
    // 3. fire / reload / slash on one key: while their hold is given
    for (int a = 0; a < kActions; a++) {
        const Binding& b = m.b[a];
        if (kHold[a] >= 0 && b.keys() == 1 && held(keys, b.k[0]) && (out & kOut[kHold[a]])) {
            out |= kOut[a];
            taken |= 1u << b.k[0];
        }
    }
    // 4. the other single actions
    const uint32_t used = usedKeys(m);
    for (int a = 0; a < kActions; a++) {
        const Binding& b = m.b[a];
        if (!isHold(a) && kHold[a] < 0 && b.keys() == 1 && held(keys, b.k[0]) && !(taken >> b.k[0] & 1)) {
            out |= kOut[a];
            turning |= a == kTurn;
        }
    }
    // 5. a D-pad key no action uses: the GameCube's D-pad (the game moves Leon with it too)
    for (int k = kKeyUp; k <= kKeyRight; k++) {
        if (!(used >> k & 1) && held(keys, (uint8_t) k)) {
            out |= kUsual[k];
        }
    }
    if (turn) {
        *turn = turning;
    }
    return out;
}

inline bool isPad(uint8_t k) { return k >= kKeyPadUp && k <= kKeyCRight; }  // (a direction of either stick)

// A stick of the game under the mapping: `first` its four direction actions (kUp: the control stick, kLookUp: the C
// stick); the Circle Pad's (px, py) and the C stick's (cx, cy) positions (right, up; within -rim..rim, the dead zone
// taken off), the keys held -> the stick (*ox, *oy, same units). A direction on a stick's direction: that part of that
// stick (analog; on the opposite one - mirrored; the other stick's - it moves this one); on keys: the whole way while
// they act; on none, or a stick direction no action has: nothing (switched off there). Two whole ways (a diagonal)
// shortened to the rim.
inline void stick(const Map& m, uint32_t keys, int first, int px, int py, int cx, int cy, int rim, int* ox, int* oy)
{
    const int part[8] = {py > 0 ? py : 0, py < 0 ? -py : 0, px < 0 ? -px : 0, px > 0 ? px : 0,
                         cy > 0 ? cy : 0, cy < 0 ? -cy : 0, cx < 0 ? -cx : 0, cx > 0 ? cx : 0};
    int v[4];
    for (int d = 0; d < 4; d++) {
        const Binding& b = m.b[first + d];
        v[d] = 0;
        if (b.keys() == 1 && isPad(b.k[0])) {
            v[d] = part[b.k[0] - kKeyPadUp];
        } else if (b.keys() == 1 ? held(keys, b.k[0]) : b.keys() == 2 && held(keys, b.k[0]) && held(keys, b.k[1])) {
            v[d] = rim;
        }
    }
    int x = v[3] - v[2], y = v[0] - v[1];
    const long long mag2 = (long long) x * x + (long long) y * y;
    if (mag2 > (long long) rim * rim) {
        // (as the pad's own circle: a diagonal of two whole ways shortened to the rim)
        const double f = (double) rim / __builtin_sqrt((double) mag2);
        x = (int) (x * f + (x > 0 ? 0.5 : -0.5));
        y = (int) (y * f + (y > 0 ? 0.5 : -0.5));
    }
    *ox = x;
    *oy = y;
}

// The usual layout (the menus)
inline uint16_t usual(uint32_t keys)
{
    uint16_t out = 0;
    for (int k = 0; k < kKeys; k++) {
        if (keys >> k & 1) {
            out |= kUsual[k];
        }
    }
    return out;
}

// The switch between the menus' usual layout and the mapping, frame by frame: a GameCube button that a key held over
// the switch gives only in the new layout waits until it is let go (a key held as the case opened is no new press
// there); back in the game the held run, aim, knife and fire come at once (CubeEvil4's testers: Leon did not run on
// until the button was pressed again).
struct Switcher {
    bool menu = true;   // the layout of the last frame
    uint16_t last = 0;  // its buttons
    uint16_t wait = 0;  // the buttons waiting for their keys to be let go
    uint16_t step(const Map& m, uint32_t keys, bool menuNow, bool* turn = nullptr)
    {
        bool t = false;
        uint16_t out = menuNow ? usual(keys) : buttons(m, keys, &t);
        if (turn) {
            *turn = t;
        }
        if (menuNow != menu) {
            const uint16_t carried = menuNow ? 0 : (uint16_t) (kGcA | kGcB | kGcR | kGcL);
            wait = (uint16_t) (out & ~last & ~carried);
            menu = menuNow;
        }
        wait &= out;  // (let go: no longer waiting)
        out &= (uint16_t) ~wait;
        last = out;
        return out;
    }
};

// Catching a binding on the settings screen: all keys let go first, then the first key pressed; a second pressed while
// it is held - the two (a combination); the first let go alone - the one; SELECT - none. Then all keys let go again
// before the game gets the buttons back (the title would take the A as its own).
struct Catch {
    enum { kIdle, kRelease, kWaitFirst, kHaveFirst, kDone } phase = kIdle;
    uint8_t first = kNone;
    Binding got{{kNone, kNone}};
    bool result = false;  // a binding caught (else: cancelled)
    void start() { phase = kRelease, first = kNone, result = false, got.k[0] = got.k[1] = kNone; }
    // each frame while active(): the keys held and SELECT; true when a binding was caught this frame
    bool step(uint32_t keys, bool select)
    {
        switch (phase) {
        case kRelease:
            if (!keys && !select) {
                phase = kWaitFirst;
            }
            return false;
        case kWaitFirst:
            if (select) {
                got.k[0] = got.k[1] = kNone, result = true, phase = kDone;
                return true;
            }
            for (int k = 0; k < kKeys; k++) {
                if (keys >> k & 1) {
                    first = (uint8_t) k, phase = kHaveFirst;
                    break;
                }
            }
            return false;
        case kHaveFirst:
            if (!held(keys, first)) {
                got.k[0] = first, got.k[1] = kNone, result = true, phase = kDone;
                return true;
            }
            for (int k = 0; k < kKeys; k++) {
                if (k != first && (keys >> k & 1)) {
                    got.k[0] = first, got.k[1] = (uint8_t) k, result = true, phase = kDone;
                    return true;
                }
            }
            return false;
        case kDone:
            if (!keys && !select) {
                phase = kIdle;
            }
            return false;
        default:
            return false;
        }
    }
    void cancel() { phase = phase == kIdle ? kIdle : kDone, result = false; }
    bool active() const { return phase != kIdle; }        // the game gets no buttons
    bool waiting() const { return phase == kRelease || phase == kWaitFirst || phase == kHaveFirst; }
};

}  // namespace ctl
