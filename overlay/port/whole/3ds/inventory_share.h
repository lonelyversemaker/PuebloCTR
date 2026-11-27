// (2026-11-27) The bottom screen's inventory: Leon's case as runtime/cheats.cpp samples it on the game's thread (every
// 4th frame, invSample) and 3ds/ui_bottom_3ds.cpp draws it on the panel's thread (re4_inv_copy).
#pragma once

struct Re4InvItem {
    unsigned short id;        // the item
    unsigned char slot;       // its place in ItemMgr (the touch names the place, the id checks it)
    unsigned char x0, y0;     // its rectangle in cells (left, top)
    unsigned char w, h;       //   the size as it lies (turned)
    unsigned char orient;     // the case's orientation 0..7 (4..7: its back, mirrored)
    unsigned char flags;      // 1 equipped, 2 a weapon (equipped by a touch), 4 heals (used by a touch)
    short count;              // the number shown (a weapon: its loaded rounds; a stack: its count); -1 none
    unsigned char type;       // (2026-11-27, the edits) itemType: 1 weapon, 2 ammo, 9 a weapon's part, ...
    unsigned char pad;
    unsigned short ammo;      //   a weapon's ammo now (its rounds onto it load it), else 0
};

// (2026-11-27) flags: 8 - may be discarded (not a file, not the weapon in the hands)
//   16 - on the spare board (the tray: x0 / y0 mean nothing)

struct Re4Inv {
    int n;                    // items (0: no case - Ashley, ...)
    int cols, rows;           // the case's cells (its size)
    unsigned gen;             // changes when anything shown changes
    Re4InvItem it[64];
};

extern "C" int re4_inv_copy(Re4Inv* out);     // runtime/cheats.cpp: 0 before the first sample
extern "C" volatile int re4_inv_tap;          // runtime/cheats.cpp: (slot + 1) << 16 | id - the game acts at its next frame
extern "C" volatile unsigned re4_inv_gen;     // runtime/cheats.cpp: Re4Inv::gen of the latest sample

// (2026-11-27) the inventory's edits: the touch (3ds/ui_bottom_3ds.cpp, on the game's thread as the touch) asks,
// runtime/cheats.cpp does it at the game's next frame; a = the item ((slot + 1) << 16 | id), b = the one it is dropped on
enum { RE4_INV_MOVE = 1, RE4_INV_ROTATE, RE4_INV_COMBINE, RE4_INV_DISCARD };
struct Re4InvOp {
    int kind, a, b;
    int x0, y0, orient;       // MOVE: the rectangle's top left and the orientation
};
extern "C" Re4InvOp re4_inv_op_req;
extern "C" volatile int re4_inv_op_seq, re4_inv_op_done, re4_inv_op_ok;  // asked / done (the same number), done well
extern "C" int re4_inv_table_combine(int a, int b);  // the game's combination table has the pair (no item touched)
enum { RE4_INV_TRAY = 5 };  // (2026-11-27) aside: onto the spare board (MOVE lays it back in the case)
