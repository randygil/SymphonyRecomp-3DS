/* PSX GPU: command processor (port of RecompOne.Runtime.Gpu) with a fast
   fixed point software rasterizer suited to the 3DS ARM11.

   Included twice (see gpu.c). Both copies see the whole command stream:
   GPU_MAIN=1 runs on the game thread, owns all VRAM transfers and answers
   status/VRAM reads; GPU_MAIN=0 runs on the worker thread. Each copy only
   rasterizes the VRAM rows of its band so the two CPU cores share the work. */
#include "rt.h"

#if GPU_MAIN
#define GPUFN(n) gpus_##n
#define XFER 1
#else
#define GPUFN(n) gpuw_##n
#define XFER 0
#endif

/* rows this copy draws: the union of [band_lo[i], band_hi[i]) */
static int band_n = 1, band_lo[2] = { 0, 0 }, band_hi[2] = { VRAM_H, 0 };
static int skip_draw;

void GPUFN(set_skip)(int s) { skip_draw = s; }

void GPUFN(set_band)(int n, const int *lo, const int *hi)
{
    band_n = n;
    for (int i = 0; i < n; i++) { band_lo[i] = lo[i]; band_hi[i] = hi[i]; }
}

void gpu_front_sync(void);

/* ---- state ---- */
static int dl = 0, dt = 0, dr = VRAM_W - 1, db = VRAM_H - 1;  /* draw area (inclusive) */
static int ofx, ofy;
static int tpx, tpy, tdepth, blend, dither_on, tex_disable;
static int twmx, twmy, twox, twoy;
static int set_mask, check_mask;
static int disp_x, disp_y;
static int hr1 = 0x200, hr2 = 0xC00, vr1 = 0x10, vr2 = 0x100;
static int hres, hres368, vres480, pal, disp24, interlace, disp_disabled = 1;
static int dma_dir;
static u32 gpuread;
static int stat_field;

/* ---- fifo ---- */
#define FIFO_MAX 256
static u32 fifo[FIFO_MAX];
static int fifo_n, need;
static int polyline;

/* image upload in progress; load_col/load_row track load_px (no divisions) */
static int load_active, load_x, load_y, load_w, load_h, load_px, load_col, load_row;
static int read_active, read_x, read_y, read_w, read_h, read_px;

/* derived texture-window masks */
static u32 tw_uand = 0xFF, tw_uor, tw_vand = 0xFF, tw_vor;

static void update_tw(void)
{
    tw_uand = (u32)(~(twmx * 8)) & 0xFF;
    tw_uor = (u32)((twox & twmx) * 8);
    tw_vand = (u32)(~(twmy * 8)) & 0xFF;
    tw_vor = (u32)((twoy & twmy) * 8);
}

static const s8 s_dither[4][4] = {
    { -4, 0, -3, 1 },
    { 2, -2, 3, -1 },
    { -3, 1, -4, 0 },
    { 3, -1, 2, -2 },
};

/* 8 bit color + dither -> 5 bit, per dither cell */
static u8 s_dlut[16][256 + 64];
static int s_dlut_ready;

static void build_dlut(void)
{
    for (int c = 0; c < 16; c++) {
        int d = s_dither[c >> 2][c & 3];
        for (int v = 0; v < 256 + 64; v++) {
            int x = v + d;
            if (x < 0) x = 0;
            if (x > 255) x = 255;
            s_dlut[c][v] = (u8)(x >> 3);
        }
    }
    s_dlut_ready = 1;
}

void GPUFN(init)(void)
{
    memset(g_vram, 0, sizeof g_vram);
    update_tw();
    if (!s_dlut_ready) build_dlut();
}

int GPUFN(is_pal)(void) { return pal; }

static int cycles_per_pixel(void)
{
    if (hres368) return 7;
    switch (hres) { case 0: return 10; case 1: return 8; case 2: return 5; default: return 4; }
}

void GPUFN(get_display)(GpuDisplay *d)
{
    int w = ((hr2 - hr1) / cycles_per_pixel() + 2) & ~3;
    if (w < 0) w = 0;
    if (w > VRAM_W) w = VRAM_W;
    int lines = vr2 - vr1;
    if (vres480) lines <<= 1;
    if (lines < 0) lines = 0;
    if (lines > VRAM_H) lines = VRAM_H;
    d->x = disp_x;
    d->y = disp_y;
    d->w = w;
    d->h = lines;
    d->rgb24 = disp24;
    d->enabled = !disp_disabled;
    d->pal = pal;
    d->interlace = interlace;
}

u32 GPUFN(read_stat)(void)
{
    u32 s = 0;
    s |= (u32)((tpx / 64) & 0xF);
    s |= (u32)(((tpy / 256) & 1) << 4);
    s |= (u32)((blend & 3) << 5);
    s |= (u32)((tdepth & 3) << 7);
    if (dither_on) s |= 1u << 9;
    s |= 1u << 10;
    if (set_mask) s |= 1u << 11;
    if (check_mask) s |= 1u << 12;
    s |= 1u << 13;
    if (tex_disable) s |= 1u << 15;
    if (hres368) s |= 1u << 16;
    s |= (u32)((hres & 3) << 17);
    if (vres480) s |= 1u << 19;
    if (pal) s |= 1u << 20;
    if (disp24) s |= 1u << 21;
    if (interlace) s |= 1u << 22;
    if (disp_disabled) s |= 1u << 23;
    s |= 1u << 26;
    s |= 1u << 27;
    s |= 1u << 28;
    s |= (u32)((dma_dir & 3) << 29);
    switch (dma_dir) { case 1: s |= 1u << 25; break; case 2: s |= 1u << 28; break; case 3: s |= 1u << 27; break; }
    stat_field = !stat_field;
    if (stat_field) s |= 1u << 31;
    return s;
}

static u16 read_image_hw(void)
{
    if (!read_active) return 0;
    int x = (read_x + (read_px % read_w)) & (VRAM_W - 1);
    int y = (read_y + (read_px / read_w)) & (VRAM_H - 1);
    u16 v = g_vram[y * VRAM_W + x];
    if (++read_px >= read_w * read_h) read_active = 0;
    return v;
}

u32 GPUFN(read_data)(void)
{
    if (!read_active) return gpuread;
    u32 lo = read_image_hw();
    u32 hi = read_image_hw();
    return lo | (hi << 16);
}

/* ---------------------------------------------------------------- */
/* pixel helpers */

static inline u16 blend_px(u16 bg, int fr, int fg, int fb)
{
    int br = bg & 0x1F, bgn = (bg >> 5) & 0x1F, bb = (bg >> 10) & 0x1F;
    switch (blend) {
    case 0: fr = (br + fr) >> 1; fg = (bgn + fg) >> 1; fb = (bb + fb) >> 1; break;
    case 1: fr += br; fg += bgn; fb += bb; if (fr > 31) fr = 31; if (fg > 31) fg = 31; if (fb > 31) fb = 31; break;
    case 2: fr = br - fr; fg = bgn - fg; fb = bb - fb; if (fr < 0) fr = 0; if (fg < 0) fg = 0; if (fb < 0) fb = 0; break;
    default: fr = br + (fr >> 2); fg = bgn + (fg >> 2); fb = bb + (fb >> 2); if (fr > 31) fr = 31; if (fg > 31) fg = 31; if (fb > 31) fb = 31; break;
    }
    return (u16)(fr | (fg << 5) | (fb << 10));
}

static inline u16 fetch_texel(u32 u, u32 v, const u16 *clut)
{
    u = (u & tw_uand) | tw_uor;
    v = (v & tw_vand) | tw_vor;
    const u16 *row = g_vram + ((tpy + (int)v) & (VRAM_H - 1)) * VRAM_W;
    switch (tdepth) {
    case 0: {
        u16 blk = row[(tpx + (int)(u >> 2)) & (VRAM_W - 1)];
        return clut[(blk >> ((u & 3) * 4)) & 0xF];
    }
    case 1: {
        u16 blk = row[(tpx + (int)(u >> 1)) & (VRAM_W - 1)];
        return clut[(blk >> ((u & 1) * 8)) & 0xFF];
    }
    default:
        return row[(tpx + (int)u) & (VRAM_W - 1)];
    }
}

/* clut pointer; the lookup above never exceeds 256 entries; wrap is ignored
   for cluts placed at the very right edge (they are not used in practice) */
static inline const u16 *clut_ptr(int clut)
{
    int cx = (clut & 0x3F) * 16, cy = (clut >> 6) & 0x1FF;
    if (cx > VRAM_W - 256 && tdepth == 1) cx = VRAM_W - 256;
    return g_vram + cy * VRAM_W + cx;
}

/* ---------------------------------------------------------------- */
/* triangle rasterizer */

typedef struct { int x, y, r, g, b, u, v; } Vtx;

enum { F_TEX = 1, F_GOURAUD = 2, F_SEMI = 4, F_RAW = 8 };

typedef struct {
    s32 drdx, dgdx, dbdx, dudx, dvdx;   /* 16.16 */
    s32 drdy, dgdy, dbdy, dudy, dvdy;
    s32 r0, g0, b0, u0, v0;             /* at (ox, oy) */
    int ox, oy;
    const u16 *clut;
} Grad;

#define SPAN_BODY(TEX, GOUR)                                                              \
    {                                                                                     \
        u16 *dst = g_vram + y * VRAM_W;                                                   \
        s32 dx = x0 - gr->ox, dy = y - gr->oy;                                            \
        s32 r = gr->r0 + gr->drdx * dx + gr->drdy * dy;                                   \
        s32 g = gr->g0 + gr->dgdx * dx + gr->dgdy * dy;                                   \
        s32 b = gr->b0 + gr->dbdx * dx + gr->dbdy * dy;                                   \
        s32 u = 0, v = 0;                                                                 \
        if (TEX) {                                                                        \
            u = gr->u0 + gr->dudx * dx + gr->dudy * dy;                                   \
            v = gr->v0 + gr->dvdx * dx + gr->dvdy * dy;                                   \
        }                                                                                 \
        const u8 *dl0 = s_dlut[(y & 3) << 2];                                             \
        for (int x = x0; x < x1; x++) {                                                   \
            u16 bg = dst[x];                                                              \
            if (check_mask && (bg & 0x8000)) goto next;                                   \
            {                                                                             \
                int cr = GOUR ? (r >> 16) : rr, cg = GOUR ? (g >> 16) : rg,               \
                    cb = GOUR ? (b >> 16) : rb;                                           \
                int fr, fg, fb, stp = 0, semi_px = semi;                                  \
                if (TEX) {                                                                \
                    u16 t = fetch_texel((u32)(u >> 16) & 0xFF, (u32)(v >> 16) & 0xFF,     \
                                        gr->clut);                                        \
                    if (!t) goto next;                                                    \
                    stp = t & 0x8000;                                                     \
                    semi_px = semi && stp;                                                \
                    if (raw) {                                                            \
                        fr = t & 0x1F; fg = (t >> 5) & 0x1F; fb = (t >> 10) & 0x1F;       \
                    } else {                                                              \
                        int tr = (t & 0x1F) * cr, tg = ((t >> 5) & 0x1F) * cg,            \
                            tb = ((t >> 10) & 0x1F) * cb;                                 \
                        if (dith) {                                                       \
                            const u8 *dd = dl0 + 0; (void)dd;                             \
                            const u8 *lut = s_dlut[((y & 3) << 2) | (x & 3)];             \
                            tr >>= 4; tg >>= 4; tb >>= 4;                                 \
                            fr = lut[tr > 319 ? 319 : tr];                                \
                            fg = lut[tg > 319 ? 319 : tg];                                \
                            fb = lut[tb > 319 ? 319 : tb];                                \
                        } else {                                                          \
                            fr = tr >> 7; fg = tg >> 7; fb = tb >> 7;                     \
                            if (fr > 31) fr = 31;                                         \
                            if (fg > 31) fg = 31;                                         \
                            if (fb > 31) fb = 31;                                         \
                        }                                                                 \
                    }                                                                     \
                } else {                                                                  \
                    if (dith) {                                                           \
                        const u8 *lut = s_dlut[((y & 3) << 2) | (x & 3)];                 \
                        fr = lut[cr]; fg = lut[cg]; fb = lut[cb];                         \
                    } else {                                                              \
                        fr = cr >> 3; fg = cg >> 3; fb = cb >> 3;                         \
                    }                                                                     \
                }                                                                         \
                u16 o = semi_px ? blend_px(bg, fr, fg, fb)                                \
                                : (u16)(fr | (fg << 5) | (fb << 10));                     \
                if (set_mask || stp) o |= 0x8000;                                         \
                dst[x] = o;                                                               \
            }                                                                             \
        next:                                                                             \
            if (GOUR) { r += gr->drdx; g += gr->dgdx; b += gr->dbdx; }                    \
            if (TEX) { u += gr->dudx; v += gr->dvdx; }                                    \
        }                                                                                 \
    }

#define DEF_SPAN(NAME, TEX, GOUR)                                                         \
    static void NAME(const Grad *gr, int y, int x0, int x1, int rr, int rg, int rb,       \
                     int semi, int raw, int dith)                                         \
    SPAN_BODY(TEX, GOUR)

DEF_SPAN(span_f, 0, 0)
DEF_SPAN(span_g, 0, 1)
DEF_SPAN(span_tf, 1, 0)
DEF_SPAN(span_tg, 1, 1)

typedef void (*SpanFn)(const Grad *, int, int, int, int, int, int, int, int, int);

static s32 grad_x(s64 a0, s64 a1, s64 a2, const Vtx *v0, const Vtx *v1, const Vtx *v2, s64 den)
{
    s64 n = (a1 - a0) * (v2->y - v0->y) - (a2 - a0) * (v1->y - v0->y);
    return (s32)((n << 16) / den);
}

static s32 grad_y(s64 a0, s64 a1, s64 a2, const Vtx *v0, const Vtx *v1, const Vtx *v2, s64 den)
{
    s64 n = (a2 - a0) * (v1->x - v0->x) - (a1 - a0) * (v2->x - v0->x);
    return (s32)((n << 16) / den);
}

#ifndef RT_FAST_NO_DITHER
#define RT_FAST_NO_DITHER 0
#endif
#include "gpu_fast.inc"

#ifdef RT_GPU_STATS
extern u32 g_gst[8];
extern u32 g_tri_kind[64];
static int tri_area(const Vtx *a, const Vtx *b, const Vtx *c)
{
    int d = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    return (d < 0 ? -d : d) / 2;
}
#endif

static void raster_tri(Vtx a, Vtx b, Vtx c, int flags, int clut)
{
#ifdef RT_GPU_STATS
    g_gst[0]++;
    g_gst[1] += tri_area(&a, &b, &c);
    {
        extern u32 g_tri_kind[64];
        int k = (flags & 15) | ((flags & F_TEX) ? tdepth << 4 : 0);
        g_tri_kind[k] += tri_area(&a, &b, &c);
    }
    if (check_mask || (flags & F_TEX && tdepth == 3)) g_gst[6]++;
#endif
    if (fast_tri(&a, &b, &c, flags, clut)) return;
#ifdef RT_GPU_STATS
    g_gst[7]++;
#endif
    int minx = a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x);
    int maxx = a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x);
    int miny = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
    int maxy = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);
    if (maxx - minx > 1023 || maxy - miny > 511) return;
    if (maxx < dl || minx > dr || maxy < dt || miny > db) return;

    s64 den = (s64)(b.x - a.x) * (c.y - a.y) - (s64)(c.x - a.x) * (b.y - a.y);
    if (den == 0) return;

    int tex = flags & F_TEX, gour = flags & F_GOURAUD;
    Grad gr;
    gr.ox = a.x;
    gr.oy = a.y;
    gr.r0 = (a.r << 16) + 0x8000;
    gr.g0 = (a.g << 16) + 0x8000;
    gr.b0 = (a.b << 16) + 0x8000;
    gr.drdx = gr.dgdx = gr.dbdx = gr.drdy = gr.dgdy = gr.dbdy = 0;
    gr.dudx = gr.dvdx = gr.dudy = gr.dvdy = 0;
    gr.u0 = gr.v0 = 0;
    gr.clut = NULL;
    if (gour) {
        gr.drdx = grad_x(a.r, b.r, c.r, &a, &b, &c, den);
        gr.dgdx = grad_x(a.g, b.g, c.g, &a, &b, &c, den);
        gr.dbdx = grad_x(a.b, b.b, c.b, &a, &b, &c, den);
        gr.drdy = grad_y(a.r, b.r, c.r, &a, &b, &c, den);
        gr.dgdy = grad_y(a.g, b.g, c.g, &a, &b, &c, den);
        gr.dbdy = grad_y(a.b, b.b, c.b, &a, &b, &c, den);
    }
    if (tex) {
        gr.u0 = (a.u << 16) + 0x8000;
        gr.v0 = (a.v << 16) + 0x8000;
        gr.dudx = grad_x(a.u, b.u, c.u, &a, &b, &c, den);
        gr.dvdx = grad_x(a.v, b.v, c.v, &a, &b, &c, den);
        gr.dudy = grad_y(a.u, b.u, c.u, &a, &b, &c, den);
        gr.dvdy = grad_y(a.v, b.v, c.v, &a, &b, &c, den);
        gr.clut = clut_ptr(clut);
    }

    SpanFn fn = tex ? (gour ? span_tg : span_tf) : (gour ? span_g : span_f);
    int semi = (flags & F_SEMI) != 0, raw = (flags & F_RAW) != 0;
    int dith = dither_on && (tex ? !raw : gour);

    /* sort by y */
    const Vtx *v0 = &a, *v1 = &b, *v2 = &c, *t;
    if (v1->y < v0->y) { t = v0; v0 = v1; v1 = t; }
    if (v2->y < v0->y) { t = v0; v0 = v2; v2 = t; }
    if (v2->y < v1->y) { t = v1; v1 = v2; v2 = t; }
    if (v0->y == v2->y) return;

    /* long edge v0->v2, short edges v0->v1, v1->v2 ; x in 16.16 */
    s64 dxl = ((s64)(v2->x - v0->x) << 16) / (v2->y - v0->y);
    int ystart = v0->y < dt ? dt : v0->y;
    int yend = v2->y > db + 1 ? db + 1 : v2->y;

    for (int y = ystart; y < yend; y++) {
        s64 xl = ((s64)v0->x << 16) + dxl * (y - v0->y);
        s64 xs;
        if (y < v1->y) {
            s64 d = ((s64)(v1->x - v0->x) << 16) / (v1->y - v0->y);
            xs = ((s64)v0->x << 16) + d * (y - v0->y);
        } else {
            if (v2->y == v1->y) continue;
            s64 d = ((s64)(v2->x - v1->x) << 16) / (v2->y - v1->y);
            xs = ((s64)v1->x << 16) + d * (y - v1->y);
        }
        s64 lo = xl < xs ? xl : xs, hi = xl < xs ? xs : xl;
        int x0 = (int)((lo + 0xFFFF) >> 16), x1 = (int)((hi + 0xFFFF) >> 16);
        if (x0 < dl) x0 = dl;
        if (x1 > dr + 1) x1 = dr + 1;
        if (x0 >= x1) continue;
        fn(&gr, y, x0, x1, a.r, a.g, a.b, semi, raw, dith);
    }
}

/* ---------------------------------------------------------------- */

static inline int coord_x(u32 w) { int x = (int)(w & 0x7FF); return (x & 0x400) ? x - 0x800 : x; }
static inline int coord_y(u32 w) { int y = (int)((w >> 16) & 0x7FF); return (y & 0x400) ? y - 0x800 : y; }

static void set_texpage(u32 tp)
{
    tpx = (int)(tp & 0xF) * 64;
    tpy = (int)((tp >> 4) & 1) * 256;
    blend = (int)((tp >> 5) & 3);
    tdepth = (int)((tp >> 7) & 3);
    tex_disable = (tp & (1u << 11)) != 0;
}

#ifdef RT_GPU_STATS
#if GPU_MAIN
u32 g_quad_rect[2];   /* textured quads: [0] drawn as rects, [1] total */
#else
extern u32 g_quad_rect[2];
#endif
#endif

/* A textured quad laid out as an axis-aligned rectangle (TL, TR, BL, BR) with
   texels mapped 1:1 is drawn by the sprite code: the two triangles would cover
   exactly [x0,x1) x [y0,y1) with u = u0 + dx, v = v0 + dy (same fill rule and
   rounding as fast_tri), so the result is identical and the per-triangle and
   per-span setup is skipped. Common for particles, flames and HUD pieces. */
static int quad_as_rect(const Vtx *v, int flags, int clut)
{
#ifdef RT_GPU_STATS
    g_quad_rect[1]++;
#endif
    if (check_mask) return 0;
    int raw = (flags & F_RAW) != 0;
    if (flags & F_GOURAUD) {
        if (raw) {}
        else if (v[0].r == v[1].r && v[0].r == v[2].r && v[0].r == v[3].r && v[0].g == v[1].g && v[0].g == v[2].g &&
                 v[0].g == v[3].g && v[0].b == v[1].b && v[0].b == v[2].b && v[0].b == v[3].b) {
            if (v[0].r == 0x80 && v[0].g == 0x80 && v[0].b == 0x80) raw = 1;
        } else return 0;
    }
    /* polygons dither when modulated, sprites never do (the 3DS build has no dithering) */
    if (!RT_FAST_NO_DITHER && dither_on && !raw) return 0;
    int x0 = v[0].x, x1 = v[1].x, y0 = v[0].y, y1 = v[2].y;
    if (v[1].y != y0 || v[3].y != y1 || v[2].x != x0 || v[3].x != x1 || x1 <= x0 || y1 <= y0) return 0;
    int u0 = v[0].u, vv0 = v[0].v;
    if (v[2].u != u0 || v[1].u != v[3].u || v[1].v != vv0 || v[2].v != v[3].v) return 0;
    if (v[1].u - u0 != x1 - x0 || v[2].v - vv0 != y1 - y0) return 0;
    if (x1 - x0 > 1023 || y1 - y0 > 511) return 0;
    int cx0 = x0 < dl ? dl : x0, cx1 = x1 > dr + 1 ? dr + 1 : x1;
    int cy0 = y0 < dt ? dt : y0, cy1 = y1 > db + 1 ? db + 1 : y1;
    if (cx0 >= cx1 || cy0 >= cy1) return 1;
    /* drawing over its own texels (screen-feedback effects) depends on the
       pixel order, which differs from the two-triangle order: leave those */
    {
        int tw = tw_uand != 0xFF || tw_uor || tw_vand != 0xFF || tw_vor;
        int su0 = tw ? 0 : u0, su1 = tw ? 256 : v[1].u, sv0 = tw ? 0 : vv0, sv1 = tw ? 256 : v[2].v;
        int shift = tdepth == 0 ? 2 : tdepth == 1 ? 1 : 0;
        int tx0 = tpx + (su0 >> shift), tx1 = tpx + ((su1 + (1 << shift) - 1) >> shift);
        int ty0 = tpy + sv0, ty1 = tpy + sv1;
        if (tx0 < cx1 && cx0 < tx1 && ty0 < cy1 && cy0 < ty1) return 0;
        if (tdepth < 2) {
            int clx = (clut & 0x3F) * 16, cly = (clut >> 6) & 0x1FF, clw = tdepth == 0 ? 16 : 256;
            if (clx < cx1 && cx0 < clx + clw && cly >= cy0 && cly < cy1) return 0;
        }
    }
#ifdef RT_QUAD_CHECK
    static u16 saved[VRAM_W * VRAM_H], tri_out[VRAM_W * VRAM_H];
    memcpy(saved, g_vram, sizeof saved);
    raster_tri(v[0], v[1], v[2], flags, clut);
    raster_tri(v[1], v[2], v[3], flags, clut);
    memcpy(tri_out, g_vram, sizeof tri_out);
    memcpy(g_vram, saved, sizeof saved);
#endif
    if (!fast_rect(cx0, cx1, cy0, cy1, x0, y0, u0, vv0, clut, v[0].r, v[0].g, v[0].b, 1, (flags & F_SEMI) != 0, raw)) return 0;
#ifdef RT_QUAD_CHECK
    if (memcmp(tri_out, g_vram, sizeof tri_out)) {
        static int nlog;
        int first = -1, cnt = 0;
        for (int i = 0; i < VRAM_W * VRAM_H; i++) if (tri_out[i] != g_vram[i]) { if (first < 0) first = i; cnt++; }
        if (nlog++ < 20)
            rt_log("[quad] mismatch %d px first (%d,%d) tri %04X rect %04X: rect %d,%d-%d,%d uv %d,%d flags %x depth %d tw %d semi-bl %d raw %d col %d,%d,%d clip %d,%d-%d,%d\n",
                   cnt, first % VRAM_W, first / VRAM_W, tri_out[first], g_vram[first], x0, y0, x1, y1, u0, vv0, flags, tdepth,
                   tw_uand != 0xFF || tw_uor || tw_vand != 0xFF || tw_vor, blend, raw, v[0].r, v[0].g, v[0].b, cx0, cy0, cx1, cy1);
    }
#endif
#ifdef RT_GPU_STATS
    g_quad_rect[0]++;
#endif
    return 1;
}

static void draw_polygon(void)
{
    u32 cmd = fifo[0];
    int gour = (cmd & (1u << 28)) != 0, quad = (cmd & (1u << 27)) != 0;
    int tex = (cmd & (1u << 26)) != 0, semi = (cmd & (1u << 25)) != 0, raw = (cmd & (1u << 24)) != 0;
    int n = quad ? 4 : 3;
    Vtx v[4];
    int idx = 1, clut = 0;
    int cr = (int)(cmd & 0xFF), cg = (int)((cmd >> 8) & 0xFF), cb = (int)((cmd >> 16) & 0xFF);
    for (int i = 0; i < n; i++) {
        if (gour && i > 0) {
            u32 cw = fifo[idx++];
            cr = (int)(cw & 0xFF); cg = (int)((cw >> 8) & 0xFF); cb = (int)((cw >> 16) & 0xFF);
        }
        v[i].r = cr; v[i].g = cg; v[i].b = cb;
        u32 vw = fifo[idx++];
        v[i].x = ofx + coord_x(vw);
        v[i].y = ofy + coord_y(vw);
        v[i].u = v[i].v = 0;
        if (tex) {
            u32 uv = fifo[idx++];
            v[i].u = (int)(uv & 0xFF);
            v[i].v = (int)((uv >> 8) & 0xFF);
            if (i == 0) clut = (int)((uv >> 16) & 0xFFFF);
            else if (i == 1) set_texpage((uv >> 16) & 0xFFFF);
        }
    }
    int flags = (tex ? F_TEX : 0) | (gour ? F_GOURAUD : 0) | (semi ? F_SEMI : 0) | (raw ? F_RAW : 0);
    /* textured, unshaded, unmodulated colour 0x80 is the same as raw */
    if (tex && !gour && !raw && cr == 0x80 && cg == 0x80 && cb == 0x80) flags |= F_RAW;
    if (quad && tex && quad_as_rect(v, flags, clut)) return;
    raster_tri(v[0], v[1], v[2], flags, clut);
    if (quad) raster_tri(v[1], v[2], v[3], flags, clut);
}

static void draw_rect(void)
{
    u32 cmd = fifo[0];
    int sz = (int)((cmd >> 27) & 3);
    int tex = (cmd & (1u << 26)) != 0, semi = (cmd & (1u << 25)) != 0, raw = (cmd & (1u << 24)) != 0;
    int cr = (int)(cmd & 0xFF), cg = (int)((cmd >> 8) & 0xFF), cb = (int)((cmd >> 16) & 0xFF);
    int idx = 1;
    u32 vw = fifo[idx++];
    int x = ofx + coord_x(vw), y = ofy + coord_y(vw);
    int u0 = 0, v0 = 0, clut = 0;
    if (tex) {
        u32 uv = fifo[idx++];
        u0 = (int)(uv & 0xFF);
        v0 = (int)((uv >> 8) & 0xFF);
        clut = (int)((uv >> 16) & 0xFFFF);
    }
    int w, h;
    if (sz == 0) { u32 wh = fifo[idx]; w = (int)(wh & 0x3FF); h = (int)((wh >> 16) & 0x1FF); }
    else w = h = sz == 1 ? 1 : sz == 2 ? 8 : 16;

    int x0 = x < dl ? dl : x, x1 = x + w > dr + 1 ? dr + 1 : x + w;
    int y0 = y < dt ? dt : y, y1 = y + h > db + 1 ? db + 1 : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    if (tex && cr == 0x80 && cg == 0x80 && cb == 0x80) raw = 1;
#ifdef RT_GPU_STATS
    g_gst[2]++;
    g_gst[3] += (x1 - x0) * (y1 - y0);
#endif
    if (fast_rect(x0, x1, y0, y1, x, y, u0, v0, clut, cr, cg, cb, tex, semi, raw)) return;
#ifdef RT_GPU_STATS
    g_gst[4]++;
#endif

    const u16 *cl = tex ? clut_ptr(clut) : NULL;
    int dith = 0;
    (void)dith;
    for (int py = y0; py < y1; py++) {
        u16 *dst = g_vram + py * VRAM_W;
        u32 tv = (u32)(v0 + (py - y)) & 0xFF;
        for (int px = x0; px < x1; px++) {
            u16 bg = dst[px];
            if (check_mask && (bg & 0x8000)) continue;
            int fr, fg, fb, stp = 0, sp = semi;
            if (tex) {
                u16 t = fetch_texel((u32)(u0 + (px - x)) & 0xFF, tv, cl);
                if (!t) continue;
                stp = t & 0x8000;
                sp = semi && stp;
                fr = t & 0x1F; fg = (t >> 5) & 0x1F; fb = (t >> 10) & 0x1F;
                if (!raw) {
                    fr = (fr * cr) >> 7; fg = (fg * cg) >> 7; fb = (fb * cb) >> 7;
                    if (fr > 31) fr = 31;
                    if (fg > 31) fg = 31;
                    if (fb > 31) fb = 31;
                }
                if (!sp && !set_mask && !check_mask) {
                    dst[px] = (u16)(fr | (fg << 5) | (fb << 10) | stp);
                    continue;
                }
            } else {
                fr = cr >> 3; fg = cg >> 3; fb = cb >> 3;
            }
            u16 o = sp ? blend_px(bg, fr, fg, fb) : (u16)(fr | (fg << 5) | (fb << 10));
            if (set_mask || stp) o |= 0x8000;
            dst[px] = o;
        }
    }
}

static void plot(int x, int y, int r, int g, int b, int semi, int dith)
{
    if (x < dl || x > dr || y < dt || y > db) return;
    u16 *p = &g_vram[y * VRAM_W + x];
    if (check_mask && (*p & 0x8000)) return;
    int fr, fg, fb;
    if (dith) {
        const u8 *lut = s_dlut[((y & 3) << 2) | (x & 3)];
        fr = lut[r]; fg = lut[g]; fb = lut[b];
    } else { fr = r >> 3; fg = g >> 3; fb = b >> 3; }
    u16 o = semi ? blend_px(*p, fr, fg, fb) : (u16)(fr | (fg << 5) | (fb << 10));
    if (set_mask) o |= 0x8000;
    *p = o;
}

static void line_seg(int x0, int y0, int r0, int g0, int b0, int x1, int y1, int r1, int g1, int b1, int semi, int gour)
{
    x0 += ofx; y0 += ofy; x1 += ofx; y1 += ofy;
    int adx = x1 > x0 ? x1 - x0 : x0 - x1, ady = y1 > y0 ? y1 - y0 : y0 - y1;
    if (adx > 1023 || ady > 511) return;
    int steps = adx > ady ? adx : ady;
    int dith = dither_on && gour;
    if (!steps) { plot(x0, y0, r0, g0, b0, semi, dith); return; }
    s32 fx = (x0 << 16) + 0x8000, fy = (y0 << 16) + 0x8000;
    s32 sx = ((x1 - x0) << 16) / steps, sy = ((y1 - y0) << 16) / steps;
    s32 fr = r0 << 16, fg = g0 << 16, fb = b0 << 16;
    s32 sr = ((r1 - r0) << 16) / steps, sg = ((g1 - g0) << 16) / steps, sb = ((b1 - b0) << 16) / steps;
    for (int i = 0; i <= steps; i++) {
        plot(fx >> 16, fy >> 16, fr >> 16, fg >> 16, fb >> 16, semi, dith);
        fx += sx; fy += sy;
        if (gour) { fr += sr; fg += sg; fb += sb; }
    }
}

static void draw_line(void)
{
    u32 cmd = fifo[0];
    int gour = (cmd & (1u << 28)) != 0, semi = (cmd & (1u << 25)) != 0;
    int idx = 1;
    int r0 = (int)(cmd & 0xFF), g0 = (int)((cmd >> 8) & 0xFF), b0 = (int)((cmd >> 16) & 0xFF);
    u32 a = fifo[idx++];
    int r1 = r0, g1 = g0, b1 = b0;
    if (gour) { u32 cw = fifo[idx++]; r1 = (int)(cw & 0xFF); g1 = (int)((cw >> 8) & 0xFF); b1 = (int)((cw >> 16) & 0xFF); }
    u32 bw = fifo[idx++];
    line_seg(coord_x(a), coord_y(a), r0, g0, b0, coord_x(bw), coord_y(bw), r1, g1, b1, semi, gour);
}

static void exec_polyline(void)
{
    u32 cmd = fifo[0];
    int gour = (cmd & (1u << 28)) != 0, semi = (cmd & (1u << 25)) != 0;
    int idx = 1, first = 1, have = 0;
    int r = (int)(cmd & 0xFF), g = (int)((cmd >> 8) & 0xFF), b = (int)((cmd >> 16) & 0xFF);
    int px = 0, py = 0, pr = 0, pg = 0, pb = 0;
    while (idx < fifo_n) {
        if (gour && !first) { u32 cw = fifo[idx++]; r = (int)(cw & 0xFF); g = (int)((cw >> 8) & 0xFF); b = (int)((cw >> 16) & 0xFF); }
        if (idx >= fifo_n) break;
        u32 vw = fifo[idx++];
        int x = coord_x(vw), y = coord_y(vw);
        if (have) line_seg(px, py, pr, pg, pb, x, y, r, g, b, semi, gour);
        px = x; py = y; pr = r; pg = g; pb = b;
        have = 1;
        first = 0;
    }
}

static void fill_rect(void)
{
    u32 c0 = fifo[0];
    u16 color = (u16)(((c0 & 0xFF) >> 3) | ((((c0 >> 8) & 0xFF) >> 3) << 5) | ((((c0 >> 16) & 0xFF) >> 3) << 10));
    int x = (int)(fifo[1] & 0x3F0), y = (int)((fifo[1] >> 16) & 0x1FF);
    int w = (int)(((fifo[2] & 0x3FF) + 0xF) & ~0xF), h = (int)((fifo[2] >> 16) & 0x1FF);
    for (int dy = 0; dy < h; dy++) {
        u16 *row = g_vram + ((y + dy) & (VRAM_H - 1)) * VRAM_W;
        if (x + w <= VRAM_W) {
            for (int dx = 0; dx < w; dx++) row[x + dx] = color;
        } else {
            for (int dx = 0; dx < w; dx++) row[(x + dx) & (VRAM_W - 1)] = color;
        }
    }
}

static void copy_vram(void)
{
    int sx = (int)(fifo[1] & 0x3FF), sy = (int)((fifo[1] >> 16) & 0x1FF);
    int dx = (int)(fifo[2] & 0x3FF), dy = (int)((fifo[2] >> 16) & 0x1FF);
    int w = (int)(fifo[3] & 0x3FF); if (!w) w = 0x400;
    int h = (int)((fifo[3] >> 16) & 0x1FF); if (!h) h = 0x200;
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++) {
            int s = ((sy + row) & (VRAM_H - 1)) * VRAM_W + ((sx + col) & (VRAM_W - 1));
            int d = ((dy + row) & (VRAM_H - 1)) * VRAM_W + ((dx + col) & (VRAM_W - 1));
            u16 px = g_vram[s];
            if (check_mask && (g_vram[d] & 0x8000)) continue;
            if (set_mask) px |= 0x8000;
            g_vram[d] = px;
        }
}

static void store_image_hw(u16 value)
{
    u16 stored = set_mask ? (u16)(value | 0x8000) : value;
    int x = (load_x + load_col) & (VRAM_W - 1);
    int y = (load_y + load_row) & (VRAM_H - 1);
    u16 *p = &g_vram[y * VRAM_W + x];
    if (XFER && !(check_mask && (*p & 0x8000))) *p = stored;
    if (++load_col >= load_w) { load_col = 0; load_row++; }
    if (++load_px >= load_w * load_h) load_active = 0;
}

/* fast path for whole words while an image upload is active */
static void store_image_word(u32 w)
{
    int col = load_col;
    if (XFER && !set_mask && !check_mask && col + 2 <= load_w && load_x + col + 2 <= VRAM_W) {
        int y = (load_y + load_row) & (VRAM_H - 1);
        u16 *p = &g_vram[y * VRAM_W + load_x + col];
        p[0] = (u16)w;
        p[1] = (u16)(w >> 16);
        if ((load_col += 2) >= load_w) { load_col = 0; load_row++; }
        load_px += 2;
        if (load_px >= load_w * load_h) load_active = 0;
        return;
    }
    store_image_hw((u16)w);
    if (load_active) store_image_hw((u16)(w >> 16));
}

/* bulk words of an image upload (DMA): whole row segments at a time. Returns how
   many words were consumed (stops when the upload ends). The worker copy calls
   it with src == NULL to advance its counters in step with the main copy. */
u32 GPUFN(load_block)(const u32 *src, u32 n)
{
    u32 done = 0;
    while (done < n && load_active) {
        int rem = load_w - load_col;
        int fast = rem >= 2 && (!XFER || (src && !set_mask && !check_mask && load_x + load_col + 2 <= VRAM_W));
        if (!fast) {
            store_image_word(src ? src[done] : 0);
            done++;
            continue;
        }
        u32 k = (u32)(rem / 2);
        if (XFER) {
            u32 lim = (u32)(VRAM_W - (load_x + load_col)) / 2;
            if (lim < k) k = lim;
        }
        if (k > n - done) k = n - done;
        u32 left = (u32)(load_w * load_h - load_px + 1) / 2;   /* words until the upload ends */
        if (k > left) k = left;
        if (XFER) {
            int y = (load_y + load_row) & (VRAM_H - 1);
            memcpy(&g_vram[y * VRAM_W + load_x + load_col], src + done, k * 4);
        }
        load_col += (int)k * 2;
        if (load_col >= load_w) { load_col = 0; load_row++; }
        load_px += (int)k * 2;
        if (load_px >= load_w * load_h) load_active = 0;
        done += k;
    }
    return done;
}

static int cmd_len(u32 w)
{
    u32 op = w >> 24;
    if (op == 0x02) return 3;
    if (op >= 0x20 && op <= 0x3F) {
        int n = (w & (1u << 27)) ? 4 : 3;
        int sh = (w & (1u << 28)) != 0, tx = (w & (1u << 26)) != 0;
        return 1 + n + (sh ? n - 1 : 0) + (tx ? n : 0);
    }
    if (op >= 0x40 && op <= 0x5F) {
        if (w & (1u << 27)) return -1;
        return 1 + 2 + ((w & (1u << 28)) ? 1 : 0);
    }
    if (op >= 0x60 && op <= 0x7F) {
        int sz = (int)((w >> 27) & 3);
        return 1 + 1 + ((w & (1u << 26)) ? 1 : 0) + (sz == 0 ? 1 : 0);
    }
    if (op >= 0x80 && op <= 0x9F) return 4;
    if (op >= 0xA0 && op <= 0xBF) return 3;
    if (op >= 0xC0 && op <= 0xDF) return 3;
    return 1;
}

static void execute(void)
{
    u32 w = fifo[0];
    u32 op = w >> 24;
#ifdef RT_GPU_PROF
    u64 _tx = host_ticks_us();
#define XFER_PROF() (g_prof[PROF_G_FILL] += host_ticks_us() - _tx)
#else
#define XFER_PROF() ((void)0)
#endif
    if (op == 0x02) { if (XFER) { gpu_front_sync(); fill_rect(); XFER_PROF(); } }
    else if (op >= 0x20 && op <= 0x7F) {
        /* draw the primitive once per band, clipped to it */
        int sdt = dt, sdb = db;
        if (skip_draw) {
            /* still track the texpage a textured polygon would load */
            if (op <= 0x3F && (w & (1u << 26))) set_texpage((fifo[(w & (1u << 28)) ? 5 : 4] >> 16) & 0xFFFF);
        }
        else for (int i = 0; i < band_n; i++) {
            dt = sdt > band_lo[i] ? sdt : band_lo[i];
            db = sdb < band_hi[i] - 1 ? sdb : band_hi[i] - 1;
            if (dt > db) continue;
#ifdef RT_GPU_PROF
            u64 _t0 = host_ticks_us();
#endif
            if (op <= 0x3F) draw_polygon();
            else if (op <= 0x5F) draw_line();
            else draw_rect();
#ifdef RT_GPU_PROF
            g_prof[op <= 0x3F ? PROF_G_TRI : PROF_G_RECT] += host_ticks_us() - _t0;
#endif
        }
        dt = sdt;
        db = sdb;
    }
    else if (op >= 0x80 && op <= 0x9F) { if (XFER) { gpu_front_sync(); copy_vram(); XFER_PROF(); } }
    else if (op >= 0xA0 && op <= 0xBF) {
        if (XFER) gpu_front_sync();
        load_x = (int)(fifo[1] & 0x3FF);
        load_y = (int)((fifo[1] >> 16) & 0x1FF);
        load_w = (int)(fifo[2] & 0xFFFF); load_w = load_w == 0 ? 0x400 : (load_w & 0x3FF) ? (load_w & 0x3FF) : 0x400;
        load_h = (int)((fifo[2] >> 16) & 0xFFFF); load_h = load_h == 0 ? 0x200 : (load_h & 0x1FF) ? (load_h & 0x1FF) : 0x200;
        load_px = load_col = load_row = 0;
        load_active = 1;
    } else if (op >= 0xC0 && op <= 0xDF) {
        if (XFER) gpu_front_sync();   /* the worker must finish drawing before VRAM is read */
        read_x = (int)(fifo[1] & 0x3FF);
        read_y = (int)((fifo[1] >> 16) & 0x1FF);
        read_w = (int)(fifo[2] & 0x3FF); if (!read_w) read_w = 0x400;
        read_h = (int)((fifo[2] >> 16) & 0x1FF); if (!read_h) read_h = 0x200;
        read_px = 0;
        read_active = 1;
    } else switch (op) {
    case 0xE1:
        set_texpage(w);
        dither_on = (w & (1u << 9)) != 0;
        break;
    case 0xE2:
        twmx = (int)(w & 0x1F); twmy = (int)((w >> 5) & 0x1F);
        twox = (int)((w >> 10) & 0x1F); twoy = (int)((w >> 15) & 0x1F);
        update_tw();
        break;
    case 0xE3: dl = (int)(w & 0x3FF); dt = (int)((w >> 10) & 0x1FF); break;
    case 0xE4: dr = (int)(w & 0x3FF); db = (int)((w >> 10) & 0x1FF); break;
    case 0xE5: {
        int x = (int)(w & 0x7FF), y = (int)((w >> 11) & 0x7FF);
        ofx = (x & 0x400) ? x - 0x800 : x;
        ofy = (y & 0x400) ? y - 0x800 : y;
        break;
    }
    case 0xE6: set_mask = (w & 1) != 0; check_mask = (w & 2) != 0; break;
    }
}

void GPUFN(write_gp0)(u32 w)
{
    if (load_active) { store_image_word(w); return; }
    if (polyline) {
        if ((w & 0xF000F000u) == 0x50005000u) {
            polyline = 0;
            int sdt = dt, sdb = db;
            for (int i = 0; i < band_n; i++) {
                dt = sdt > band_lo[i] ? sdt : band_lo[i];
                db = sdb < band_hi[i] - 1 ? sdb : band_hi[i] - 1;
                if (dt <= db) exec_polyline();
            }
            dt = sdt;
            db = sdb;
            fifo_n = 0;
        }
        else if (fifo_n < FIFO_MAX) fifo[fifo_n++] = w;
        return;
    }
    fifo[fifo_n++] = w;
    if (fifo_n == 1) {
        need = cmd_len(w);
        if (need < 0) { polyline = 1; return; }
    }
    if (fifo_n >= need) { execute(); fifo_n = 0; }
}

void GPUFN(write_gp1)(u32 w)
{
    u32 op = (w >> 24) & 0xFF, p = w & 0xFFFFFF;
    switch (op) {
    case 0x00:
        fifo_n = 0; polyline = load_active = read_active = 0;
        disp_disabled = 1; dma_dir = 0;
        tpx = tpy = tdepth = blend = 0;
        dither_on = tex_disable = 0;
        twmx = twmy = twox = twoy = 0; update_tw();
        dl = dt = 0; dr = VRAM_W - 1; db = VRAM_H - 1;
        ofx = ofy = 0;
        set_mask = check_mask = 0;
        disp_x = disp_y = 0;
        break;
    case 0x01: fifo_n = 0; polyline = 0; load_active = 0; break;
    case 0x03: disp_disabled = (p & 1) != 0; break;
    case 0x04: dma_dir = (int)(p & 3); break;
    case 0x05: disp_x = (int)(p & 0x3FF); disp_y = (int)((p >> 10) & 0x1FF); break;
    case 0x06: hr1 = (int)(p & 0xFFF); hr2 = (int)((p >> 12) & 0xFFF); break;
    case 0x07: vr1 = (int)(p & 0x3FF); vr2 = (int)((p >> 10) & 0x3FF); break;
    case 0x08:
        hres = (int)(p & 3); hres368 = (p & 0x40) != 0; vres480 = (p & 4) != 0;
        pal = (p & 8) != 0; disp24 = (p & 0x10) != 0; interlace = (p & 0x20) != 0;
        break;
    case 0x10:
        switch (p & 0xFF) {
        case 0x03: gpuread = (u32)(dl | (dt << 10)); break;
        case 0x04: gpuread = (u32)(dr | (db << 10)); break;
        case 0x05: gpuread = (u32)((ofx & 0x7FF) | ((ofy & 0x7FF) << 11)); break;
        default: gpuread = 0; break;
        }
        break;
    }
}
