// (2026-11-27) The bottom screen's inventory: the case's item pictures, drawn once by the processor from the disc's
// SS/<lang>/ss_pzzl.dat (the models the case shows, docs/reviews/2026-11-27-inventory-icons.md) - an orthographic view
// from the front (+Z to the viewer), 100 units a cell, z-buffer, no culling, one light and an ambient, the alpha texture
// blended; supersampled. Each item twice: its front and, for the case's mirrored orientations 4..7, its back (the case
// turns the model half round about X there; seen from the front the back's x runs the other way).
// Plain C++ without the platform: 3ds/icons_3ds.cpp runs it at the first start and keeps the result on the SD card.
//
// The result (re4_icons_render): "PIC1", u16 version 1, u16 pixels a cell, u32 count, then count entries of
// {u16 item, u8 side (0 front, 1 back), u8 0, u16 w, u16 h, u32 offset} and the pictures: w*h RGB565 (little endian)
// then w*h alpha bytes, row by row.
#include "item_icons.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace {

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

struct Data {
    const u8* p;
    size_t n;
    bool ok;
    u32 w32(size_t o)
    {
        if (o + 4 > n) {
            ok = false;
            return 0;
        }
        return (u32) p[o] << 24 | (u32) p[o + 1] << 16 | (u32) p[o + 2] << 8 | p[o + 3];
    }
    u16 w16(size_t o)
    {
        if (o + 2 > n) {
            ok = false;
            return 0;
        }
        return (u16) (p[o] << 8 | p[o + 1]);
    }
    u8 b(size_t o)
    {
        if (o >= n) {
            ok = false;
            return 0;
        }
        return p[o];
    }
};

// the case's model and texture entries of an item (the aliases the case's table uses: several items share a model)
int modelOf(int id)
{
    if (id == 0x40) return 0x21;
    if (id == 0x17 || id == 0x35 || id == 0x6D) return 0x35;
    if ((id >= 0x12 && id <= 0x16) || id == 0xA8) return 0x12;
    if (id == 1 || id == 2 || id == 0xE) return 1;
    if (id >= 8 && id <= 0xA) return 8;
    if (id == 6 || id == 0x19 || id == 0x1C) return 6;
    if (id == 0 || id == 4 || id == 7 || id == 0x18 || id == 0x1A || id == 0x20 || id == 0x6A || id == 0xA0) return 0;
    return id;
}

int textureOf(int id)
{
    if (id == 0x40) return 0x21;
    if (id == 0x95 || id == 0x97) return 0x95;
    if (id == 0x6D) return 0x35;
    return id;
}

struct Tex {
    int w, h;
    u8* px;  // RGBA
};

void rgb565(u16 v, u8* c)
{
    c[0] = (u8) ((v >> 11) * 255 / 31);
    c[1] = (u8) (((v >> 5) & 63) * 255 / 63);
    c[2] = (u8) ((v & 31) * 255 / 31);
    c[3] = 255;
}

// level 0 of a GameCube texture: CMPR, I4, IA8 (the case's pictures use no other); false for another format
bool decode(Data& d, size_t o, int w, int h, int fmt, Tex* t)
{
    t->w = w, t->h = h;
    t->px = (u8*) calloc((size_t) w * h, 4);
    if (!t->px) {
        return false;
    }
    if (fmt == 14) {  // CMPR: tiles of 8 x 8 in four blocks of 4 x 4, two colours and 2 bits a texel
        for (int ty = 0; ty < h; ty += 8) {
            for (int tx = 0; tx < w; tx += 8) {
                for (int sb = 0; sb < 4; sb++, o += 8) {
                    const u16 c0 = d.w16(o), c1 = d.w16(o + 2);
                    const u32 bits = d.w32(o + 4);
                    u8 pal[4][4];
                    rgb565(c0, pal[0]);
                    rgb565(c1, pal[1]);
                    for (int k = 0; k < 3; k++) {
                        if (c0 > c1) {
                            pal[2][k] = (u8) ((2 * pal[0][k] + pal[1][k]) / 3);
                            pal[3][k] = (u8) ((pal[0][k] + 2 * pal[1][k]) / 3);
                        } else {
                            pal[2][k] = (u8) ((pal[0][k] + pal[1][k]) / 2);
                            pal[3][k] = 0;
                        }
                    }
                    pal[2][3] = 255;
                    pal[3][3] = c0 > c1 ? 255 : 0;
                    const int bx = tx + (sb & 1) * 4, by = ty + (sb >> 1) * 4;
                    for (int i = 0; i < 16; i++) {
                        const int x = bx + (i & 3), y = by + (i >> 2);
                        if (x < w && y < h) {
                            memcpy(t->px + 4 * (y * w + x), pal[(bits >> (30 - 2 * i)) & 3], 4);
                        }
                    }
                }
            }
        }
    } else if (fmt == 0) {  // I4: tiles of 8 x 8, 4 bits a texel
        for (int ty = 0; ty < h; ty += 8) {
            for (int tx = 0; tx < w; tx += 8, o += 32) {
                for (int i = 0; i < 64; i++) {
                    const int x = tx + (i & 7), y = ty + (i >> 3);
                    const u8 v = (u8) (((d.b(o + i / 2) >> (i & 1 ? 0 : 4)) & 15) * 17);
                    if (x < w && y < h) {
                        u8* c = t->px + 4 * (y * w + x);
                        c[0] = c[1] = c[2] = c[3] = v;
                    }
                }
            }
        }
    } else if (fmt == 3) {  // IA8: tiles of 4 x 4, alpha then intensity
        for (int ty = 0; ty < h; ty += 4) {
            for (int tx = 0; tx < w; tx += 4) {
                for (int i = 0; i < 16; i++, o += 2) {
                    const int x = tx + (i & 3), y = ty + (i >> 2);
                    if (x < w && y < h) {
                        u8* c = t->px + 4 * (y * w + x);
                        c[0] = c[1] = c[2] = d.b(o + 1);
                        c[3] = d.b(o);
                    }
                }
            }
        }
    } else {
        return false;
    }
    return d.ok;
}

const u8* sample(const Tex* t, float u, float v)
{
    int x = (int) floorf(u * t->w) % t->w, y = (int) floorf(v * t->h) % t->h;  // (repeat)
    x += x < 0 ? t->w : 0;
    y += y < 0 ? t->h : 0;
    return t->px + 4 * (y * t->w + x);
}

struct Vert {
    float x, y, z, nx, ny, nz, u, v;
};

struct Buf {
    int w, h;
    u8* rgba;  // the colour and the coverage as blended (bytes: the console's C heap is short)
    float* z;
};

void raster(Buf& b, const Vert* tri, const Tex* tex, const Tex* alpha)
{
    static const float L[3] = {0.3446f, 0.4431f, 0.8274f};  // (from the upper front: normalised 0.35, 0.45, 0.82)
    const Vert &a = tri[0], &c1 = tri[1], &c2 = tri[2];
    const float area = (c1.x - a.x) * (c2.y - a.y) - (c2.x - a.x) * (c1.y - a.y);
    if (area == 0) {
        return;
    }
    int x0 = (int) floorf(fminf(a.x, fminf(c1.x, c2.x))), x1 = (int) ceilf(fmaxf(a.x, fmaxf(c1.x, c2.x)));
    int y0 = (int) floorf(fminf(a.y, fminf(c1.y, c2.y))), y1 = (int) ceilf(fmaxf(a.y, fmaxf(c1.y, c2.y)));
    x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > b.w ? b.w : x1, y1 = y1 > b.h ? b.h : y1;
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            const float cx = px + 0.5f, cy = py + 0.5f;
            const float w0 = ((c1.x - cx) * (c2.y - cy) - (c2.x - cx) * (c1.y - cy)) / area;
            const float w1 = ((c2.x - cx) * (a.y - cy) - (a.x - cx) * (c2.y - cy)) / area;
            const float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) {
                continue;
            }
            const float z = w0 * a.z + w1 * c1.z + w2 * c2.z;
            const int i = py * b.w + px;
            if (z < b.z[i]) {
                continue;
            }
            const float u = w0 * a.u + w1 * c1.u + w2 * c2.u, v = w0 * a.v + w1 * c1.v + w2 * c2.v;
            float n[3] = {w0 * a.nx + w1 * c1.nx + w2 * c2.nx, w0 * a.ny + w1 * c1.ny + w2 * c2.ny,
                          w0 * a.nz + w1 * c1.nz + w2 * c2.nz};
            float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            nl = nl > 0 ? nl : 1;
            float d = (n[0] * L[0] + n[1] * L[1] + n[2] * L[2]) / nl;
            float lit = 0.25f + 0.95f * (d > 0 ? d : 0);
            lit = lit > 1 ? 1 : lit;
            const u8* c = tex ? sample(tex, u, v) : 0;
            const float al = alpha ? sample(alpha, u, v)[0] / 255.0f : 1.0f;
            u8* o = b.rgba + 4 * i;
            for (int k = 0; k < 3; k++) {
                const float col = fminf(255.0f, (c ? c[k] : 200) * lit);
                o[k] = (u8) (al >= 0.999f ? col : col * al + o[k] * (1 - al));
            }
            o[3] = (u8) (al >= 0.999f ? 255.0f : 255.0f * al + o[3] * (1 - al));
            if (al >= 0.999f) {
                b.z[i] = z;
            }
        }
    }
}

struct Out {
    u8* p;
    size_t n, cap;
    bool put(const void* s, size_t k)
    {
        if (n + k > cap) {
            size_t c = cap ? cap * 2 : 65536;
            while (c < n + k) {
                c *= 2;
            }
            u8* q = (u8*) realloc(p, c);
            if (!q) {
                return false;
            }
            p = q, cap = c;
        }
        memcpy(p + n, s, k);
        n += k;
        return true;
    }
};

// one item's picture into `out` (w = sx * ppc, h = sy * ppc); false: the data not as expected
bool renderOne(Data& d, int id, int sx, int sy, int ppc, int ss, bool back, u8* rgb565out, u8* aout)
{
    const size_t mo = d.w32(4 * (modelOf(id) * 2 + 4)), to = d.w32(4 * (textureOf(id) * 2 + 5));
    if (!d.ok || d.w32(to) != 0x0020AF30) {
        return false;
    }
    // the textures (level 0)
    Tex tex[16];
    int nt = (int) d.w32(to + 4);
    nt = nt > 16 ? 16 : nt;
    const size_t desc = to + d.w32(to + 8);
    memset(tex, 0, sizeof(tex));
    bool good = true;
    for (int i = 0; i < nt && good; i++) {
        const size_t th = to + d.w32(desc + 8 * i);
        good = decode(d, to + d.w32(th + 8), d.w16(th + 2), d.w16(th), (int) d.w32(th + 4), &tex[i]);
    }
    // the model: the rigid path (one part, no bones), POS / NRM / CLR0 / TEX0 by 16-bit indices
    const u32 flags = d.w32(mo + 0x20);
    const bool clr = (flags & 0x80000000u) != 0, nrm8 = (flags & 0x20000000u) != 0;
    const int nidx = clr ? 4 : 3;
    const float pscale = 1.0f / (float) (1 << d.b(mo + 0x28));
    const size_t vtx = mo + d.w32(mo + 0x30), nrm = mo + d.w32(mo + 0x34), uvs = mo + d.w32(mo + 0x10);
    const int ndl = d.w16(mo + 0x1A);
    Buf b;
    b.w = sx * ppc * ss, b.h = sy * ppc * ss;
    b.rgba = (u8*) calloc((size_t) b.w * b.h, 4);
    b.z = (float*) malloc((size_t) b.w * b.h * sizeof(float));
    good = good && b.rgba && b.z && d.ok;
    for (int i = 0; good && i < b.w * b.h; i++) {
        b.z[i] = -1e9f;
    }
    const float scale = (float) (ppc * ss) / 100.0f;  // (100 units a cell)
    for (int pass = 0; pass < 2 && good; pass++) {  // the parts without an alpha texture first, then those with one
        size_t p = mo + d.w32(mo + 0x1C);
        for (int k = 0; k < ndl && good; k++) {
            const u8 flag = d.b(p + 0xB), mtex = d.b(p + 0xC), atex = d.b(p + 0xE);
            const u32 size = d.w32(p + 0x18), npoly = d.w32(p + 0x1C);
            size_t q = p + 0x20;
            const size_t end = q + size;
            p = end;
            // (the flat plate of every model: two triangles, an alpha texture of zeros - unseen in the case)
            if ((pass == 1) != ((flag & 4) != 0) || (npoly == 2 && flag == 4 && mtex == 0)) {
                continue;
            }
            const Tex* t = mtex < nt ? &tex[mtex] : 0;
            const Tex* at = (flag & 4) && atex < nt ? &tex[atex] : 0;
            while (q < end && good) {
                const u8 op = d.b(q);
                if (op == 0) {
                    q++;
                    continue;
                }
                const int kind = op & 0xF8, cnt = d.w16(q + 1);
                q += 3;
                if (kind != 0x80 && kind != 0x90 && kind != 0x98 && kind != 0xA0) {
                    good = false;
                    break;
                }
                Vert* vs = (Vert*) malloc(sizeof(Vert) * (cnt > 0 ? cnt : 1));
                if (!vs) {
                    good = false;
                    break;
                }
                for (int v = 0; v < cnt; v++, q += 2 * nidx) {
                    const int pi = d.w16(q), ni = d.w16(q + 2), ti = d.w16(q + 2 * (nidx - 1));
                    float x = (short) d.w16(vtx + 8 * pi) * pscale, y = (short) d.w16(vtx + 8 * pi + 2) * pscale;
                    float z = (short) d.w16(vtx + 8 * pi + 4) * pscale;
                    float nx, ny, nz;
                    if (nrm8) {
                        nx = (signed char) d.b(nrm + 4 * ni) / 64.0f, ny = (signed char) d.b(nrm + 4 * ni + 1) / 64.0f;
                        nz = (signed char) d.b(nrm + 4 * ni + 2) / 64.0f;
                    } else {
                        nx = (short) d.w16(nrm + 8 * ni) / 16384.0f, ny = (short) d.w16(nrm + 8 * ni + 2) / 16384.0f;
                        nz = (short) d.w16(nrm + 8 * ni + 4) / 16384.0f;
                    }
                    float u, vv;
                    if (clr) {
                        u = (short) d.w16(uvs + 4 * ti) / 256.0f, vv = (short) d.w16(uvs + 4 * ti + 2) / 256.0f;
                    } else {
                        u = d.w16(uvs + 4 * ti) / 32768.0f, vv = d.w16(uvs + 4 * ti + 2) / 32768.0f;
                    }
                    if (back) {  // (half round about Y as seen: the back's x the other way, z away)
                        x = -x, z = -z, nx = -nx, nz = -nz;
                    }
                    vs[v] = Vert{x * scale + b.w * 0.5f, -y * scale + b.h * 0.5f, z, nx, ny, nz, u, vv};
                }
                Vert tri[3];
                if (kind == 0x90) {
                    for (int i = 0; i + 2 < cnt; i += 3) {
                        raster(b, &vs[i], t, at);
                    }
                } else if (kind == 0x98) {
                    for (int i = 0; i + 2 < cnt; i++) {
                        tri[0] = vs[i + (i & 1)], tri[1] = vs[i + 1 - (i & 1)], tri[2] = vs[i + 2];
                        raster(b, tri, t, at);
                    }
                } else if (kind == 0xA0) {
                    for (int i = 1; i + 1 < cnt; i++) {
                        tri[0] = vs[0], tri[1] = vs[i], tri[2] = vs[i + 1];
                        raster(b, tri, t, at);
                    }
                } else {
                    for (int i = 0; i + 3 < cnt; i += 4) {
                        tri[0] = vs[i], tri[1] = vs[i + 1], tri[2] = vs[i + 2];
                        raster(b, tri, t, at);
                        tri[1] = vs[i + 2], tri[2] = vs[i + 3];
                        raster(b, tri, t, at);
                    }
                }
                free(vs);
            }
        }
    }
    // down to ppc a cell: the colour weighted by its alpha, the alpha averaged
    const int w = sx * ppc, h = sy * ppc;
    for (int y = 0; good && y < h; y++) {
        for (int x = 0; x < w; x++) {
            float acc[4] = {0, 0, 0, 0};
            for (int yy = 0; yy < ss; yy++) {
                for (int xx = 0; xx < ss; xx++) {
                    const u8* c = b.rgba + 4 * ((y * ss + yy) * b.w + x * ss + xx);
                    for (int k = 0; k < 3; k++) {
                        acc[k] += c[k] * (c[3] / 255.0f);
                    }
                    acc[3] += c[3] / 255.0f;
                }
            }
            u8 rgb[3];
            for (int k = 0; k < 3; k++) {
                const float v = acc[3] > 0 ? acc[k] / acc[3] : 0;
                rgb[k] = (u8) (v > 255 ? 255 : v);
            }
            const u16 c = (u16) ((rgb[0] >> 3) << 11 | (rgb[1] >> 2) << 5 | rgb[2] >> 3);
            rgb565out[2 * (y * w + x)] = (u8) c;
            rgb565out[2 * (y * w + x) + 1] = (u8) (c >> 8);
            const float a = acc[3] / (ss * ss) * 255.0f + 0.5f;
            aout[y * w + x] = (u8) (a > 255 ? 255 : a);
        }
    }
    free(b.rgba);
    free(b.z);
    for (int i = 0; i < nt; i++) {
        free(tex[i].px);
    }
    return good && d.ok;
}

}  // namespace

unsigned char* re4_icons_render(const unsigned char* pzzl, size_t n, const Re4IconItem* items, int count, int ppc,
                                int ss, size_t* outSize, void (*step)(int done, void* ctx), void* ctx)
{
    Data d = {pzzl, n, true};
    Out o = {0, 0, 0};
    const u16 head[2] = {1, (u16) ppc};
    const u32 total = (u32) count * 2;
    bool good = o.put("PIC1", 4) && o.put(head, 4) && o.put(&total, 4);
    const size_t table = o.n;
    for (u32 i = 0; i < total && good; i++) {
        u8 e[12] = {0};
        good = o.put(e, 12);
    }
    for (int i = 0; i < count && good; i++) {
        const int w = items[i].sx * ppc, h = items[i].sy * ppc;
        for (int side = 0; side < 2 && good; side++) {
            u8* rgb = (u8*) malloc((size_t) w * h * 2);
            u8* a = (u8*) malloc((size_t) w * h);
            if (!rgb || !a || !renderOne(d, items[i].id, items[i].sx, items[i].sy, ppc, ss, side == 1, rgb, a)) {
                if (rgb && a) {  // (a model not as expected: an empty picture - the screen draws its own then)
                    memset(rgb, 0, (size_t) w * h * 2);
                    memset(a, 0, (size_t) w * h);
                } else {
                    good = false;
                }
            }
            if (good) {
                u8* e = o.p + table + 12 * (2 * i + side);
                const u32 off = (u32) o.n;
                e[0] = (u8) items[i].id, e[1] = (u8) (items[i].id >> 8), e[2] = (u8) side, e[3] = 0;
                e[4] = (u8) w, e[5] = (u8) (w >> 8), e[6] = (u8) h, e[7] = (u8) (h >> 8);
                memcpy(e + 8, &off, 4);
                good = o.put(rgb, (size_t) w * h * 2) && o.put(a, (size_t) w * h);
            }
            free(rgb);
            free(a);
        }
        if (step) {
            step(i + 1, ctx);
        }
    }
    if (!good) {
        free(o.p);
        return 0;
    }
    *outSize = o.n;
    return o.p;
}
