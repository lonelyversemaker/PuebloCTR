// (2026-11-27) The case's item pictures from the disc's ss_pzzl.dat (item_icons.cpp).
#pragma once
#include <stddef.h>

struct Re4IconItem {
    unsigned short id;           // the item
    unsigned char sx, sy;        // its size in cells (the game's piece_info)
};

// The pictures of `count` items, front and back, `ppc` pixels a cell, `ss` x `ss` samples a pixel: a malloc'ed blob
// ("PIC1", item_icons.cpp's head) and its size; 0 when the data are not a case archive.
// `step` (may be null): called after each item with the items done.
unsigned char* re4_icons_render(const unsigned char* pzzl, size_t n, const Re4IconItem* items, int count, int ppc,
                                int ss, size_t* outSize, void (*step)(int done, void* ctx) = 0, void* ctx = 0);
