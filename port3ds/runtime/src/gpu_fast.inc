/* fast paths of the software rasterizer, included by gpu.c.
   Used whenever mask checking (and dithering, unless RT_FAST_NO_DITHER) is off,
   which is the common case; everything else goes through the reference code. */

static inline u16 pk_blend0(u16 b, u16 f) { return (u16)((((b & 0x7BDE) + (f & 0x7BDE)) >> 1) + (b & f & 0x0421)); }
static inline u16 pk_blend1(u16 b, u16 f)
{
    u32 a = b & 0x7FFF, c = f & 0x7FFF, sum = a + c;
    u32 carry = (sum - ((a ^ c) & 0x0421)) & 0x8420;
    return (u16)((sum - carry) | (carry - (carry >> 5)));
}
static inline u16 pk_blend2(u16 b, u16 f)
{
    int r = (b & 31) - (f & 31), g = ((b >> 5) & 31) - ((f >> 5) & 31), bl = ((b >> 10) & 31) - ((f >> 10) & 31);
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (bl < 0) bl = 0;
    return (u16)(r | (g << 5) | (bl << 10));
}
static inline u16 pk_blend3(u16 b, u16 f) { return pk_blend1(b, (u16)((f >> 2) & 0x1CE7)); }

static inline u16 pk_blend(u16 b, u16 f)
{
    switch (blend) {
    case 0: return pk_blend0(b, f);
    case 1: return pk_blend1(b, f);
    case 2: return pk_blend2(b, f);
    default: return pk_blend3(b, f);
    }
}

/* flat colour modulation tables: texel channel (5 bit) -> modulated, pre-shifted */
typedef struct { u16 r[32], g[32], b[32]; } ModLut;

static void build_mod(ModLut *m, int cr, int cg, int cb)
{
    for (int i = 0; i < 32; i++) {
        int r = (i * cr) >> 7, g = (i * cg) >> 7, b = (i * cb) >> 7;
        m->r[i] = (u16)(r > 31 ? 31 : r);
        m->g[i] = (u16)((g > 31 ? 31 : g) << 5);
        m->b[i] = (u16)((b > 31 ? 31 : b) << 10);
    }
}

#define MODULATE(m, t) ((m)->r[(t) & 31] | (m)->g[((t) >> 5) & 31] | (m)->b[((t) >> 10) & 31])

#define TEXEL(DEPTH, u, v, clut) TEXELW(DEPTH, 1, u, v, clut)
#define TEXELW(DEPTH, TW, u, v, clut)                                                       \
    ({                                                                                      \
        u32 _u = TW ? (((u) & tw_uand) | tw_uor) : (u), _v = TW ? (((v) & tw_vand) | tw_vor) : (v); \
        const u16 *_row = g_vram + (((tpy + (int)_v) & (VRAM_H - 1)) << 10);                \
        u16 _t;                                                                             \
        if ((DEPTH) == 0) _t = (clut)[(_row[(tpx + (int)(_u >> 2)) & (VRAM_W - 1)] >> ((_u & 3) << 2)) & 0xF]; \
        else if ((DEPTH) == 1) _t = (clut)[(_row[(tpx + (int)(_u >> 1)) & (VRAM_W - 1)] >> ((_u & 1) << 3)) & 0xFF]; \
        else _t = _row[(tpx + (int)_u) & (VRAM_W - 1)];                                     \
        _t;                                                                                 \
    })

/* ---- textured sprites ---- */
#define DEF_FRECT(NAME, DEPTH, MOD, SEMI, TW)                                               \
    static void NAME(int x0, int x1, int y0, int y1, int ubase, int vbase, const u16 *clut, \
                     const ModLut *m, u16 mor)                                              \
    {                                                                                       \
        for (int py = y0; py < y1; py++) {                                                  \
            u16 *dst = g_vram + py * VRAM_W;                                                \
            u32 tv = (u32)(vbase + py) & 0xFF;                                              \
            if (TW) tv = (tv & tw_vand) | tw_vor;                                           \
            const u16 *row = g_vram + (((tpy + (int)tv) & (VRAM_H - 1)) << 10);             \
            for (int px = x0; px < x1; px++) {                                              \
                u32 u = (u32)(ubase + px) & 0xFF;                                           \
                if (TW) u = (u & tw_uand) | tw_uor;                                         \
                u16 t;                                                                      \
                if ((DEPTH) == 0) t = clut[(row[tpx + (int)(u >> 2)] >> ((u & 3) << 2)) & 0xF]; \
                else if ((DEPTH) == 1) t = clut[(row[(tpx + (int)(u >> 1)) & (VRAM_W - 1)] >> ((u & 1) << 3)) & 0xFF]; \
                else t = row[(tpx + (int)u) & (VRAM_W - 1)];                                \
                if (!t) continue;                                                           \
                u16 o = MOD ? (u16)MODULATE(m, t) : (u16)(t & 0x7FFF);                      \
                if (SEMI && (t & 0x8000)) o = pk_blend(dst[px], o);                         \
                dst[px] = o | (t & 0x8000) | mor;                                           \
            }                                                                               \
        }                                                                                   \
    }

#define DEF_FRECT_ALL(D, DN)                                                                \
    DEF_FRECT(frect##DN##_r0, D, 0, 0, 0) DEF_FRECT(frect##DN##_r1, D, 0, 1, 0)            \
    DEF_FRECT(frect##DN##_m0, D, 1, 0, 0) DEF_FRECT(frect##DN##_m1, D, 1, 1, 0)            \
    DEF_FRECT(frect##DN##_r0w, D, 0, 0, 1) DEF_FRECT(frect##DN##_r1w, D, 0, 1, 1)          \
    DEF_FRECT(frect##DN##_m0w, D, 1, 0, 1) DEF_FRECT(frect##DN##_m1w, D, 1, 1, 1)
DEF_FRECT_ALL(0, 4)
DEF_FRECT_ALL(1, 8)
DEF_FRECT_ALL(2, 16)

typedef void (*FRectFn)(int, int, int, int, int, int, const u16 *, const ModLut *, u16);
#define FR_ROW(DN) { { { frect##DN##_r0, frect##DN##_r1 }, { frect##DN##_m0, frect##DN##_m1 } }, { { frect##DN##_r0w, frect##DN##_r1w }, { frect##DN##_m0w, frect##DN##_m1w } } }
static const FRectFn s_frect[3][2][2][2] = { FR_ROW(4), FR_ROW(8), FR_ROW(16) };

/* returns 1 if handled */
static int fast_rect(int x0, int x1, int y0, int y1, int x, int y, int u0, int v0, int clut,
                     int cr, int cg, int cb, int tex, int semi, int raw)
{
    if (check_mask) return 0;
    if (!tex) {
        u16 c = (u16)((cr >> 3) | ((cg >> 3) << 5) | ((cb >> 3) << 10));
        u16 mor = set_mask ? 0x8000 : 0;
        for (int py = y0; py < y1; py++) {
            u16 *dst = g_vram + py * VRAM_W;
            if (semi) for (int px = x0; px < x1; px++) dst[px] = pk_blend(dst[px], c) | mor;
            else for (int px = x0; px < x1; px++) dst[px] = c | mor;
        }
        return 1;
    }
    if (tdepth == 3) return 0;
    ModLut m;
    int mod = !raw;
    if (mod) build_mod(&m, cr, cg, cb);
    int tw = tw_uand != 0xFF || tw_uor || tw_vand != 0xFF || tw_vor;
    s_frect[tdepth][tw][mod][semi != 0](x0, x1, y0, y1, u0 - x, v0 - y, clut_ptr(clut), &m, set_mask ? 0x8000 : 0);
    return 1;
}

/* ---- triangles ---- */
typedef struct { s32 dudx, dvdx, drdx, dgdx, dbdx; } Steps;

#define DEF_FSPAN_T(NAME, DEPTH, MOD, SEMI, TW)                                             \
    static void NAME(u16 *dst, int n, s32 u, s32 v, const Steps *st, const u16 *clut,       \
                     const ModLut *m, u16 mor)                                              \
    {                                                                                       \
        s32 du = st->dudx, dv = st->dvdx;                                                   \
        for (int i = 0; i < n; i++, u += du, v += dv) {                                     \
            u16 t = TEXELW(DEPTH, TW, (u32)(u >> 16) & 0xFF, (u32)(v >> 16) & 0xFF, clut); \
            if (!t) continue;                                                               \
            u16 o = MOD ? (u16)MODULATE(m, t) : (u16)(t & 0x7FFF);                          \
            if (SEMI && (t & 0x8000)) o = pk_blend(dst[i], o);                              \
            dst[i] = o | (t & 0x8000) | mor;                                                \
        }                                                                                   \
    }

#define DEF_FSPAN_ALL(D, DN)                                                                \
    DEF_FSPAN_T(fspan##DN##_r0, D, 0, 0, 0) DEF_FSPAN_T(fspan##DN##_r1, D, 0, 1, 0)        \
    DEF_FSPAN_T(fspan##DN##_m0, D, 1, 0, 0) DEF_FSPAN_T(fspan##DN##_m1, D, 1, 1, 0)        \
    DEF_FSPAN_T(fspan##DN##_r0w, D, 0, 0, 1) DEF_FSPAN_T(fspan##DN##_r1w, D, 0, 1, 1)      \
    DEF_FSPAN_T(fspan##DN##_m0w, D, 1, 0, 1) DEF_FSPAN_T(fspan##DN##_m1w, D, 1, 1, 1)
DEF_FSPAN_ALL(0, 4)
DEF_FSPAN_ALL(1, 8)
DEF_FSPAN_ALL(2, 16)

/* gouraud textured: per pixel modulation */
#define DEF_FSPAN_TG(NAME, DEPTH, SEMI, TW)                                                 \
    static void NAME(u16 *dst, int n, s32 u, s32 v, s32 r, s32 g, s32 b, const Steps *st,   \
                     const u16 *clut, u16 mor)                                              \
    {                                                                                       \
        s32 du = st->dudx, dv = st->dvdx, dr = st->drdx, dg = st->dgdx, db_ = st->dbdx;     \
        for (int i = 0; i < n; i++, u += du, v += dv, r += dr, g += dg, b += db_) {         \
            u16 t = TEXELW(DEPTH, TW, (u32)(u >> 16) & 0xFF, (u32)(v >> 16) & 0xFF, clut); \
            if (!t) continue;                                                               \
            u32 fr = ((t & 31) * (u32)(r >> 16)) >> 7;                                      \
            u32 fg = (((t >> 5) & 31) * (u32)(g >> 16)) >> 7;                               \
            u32 fb = (((t >> 10) & 31) * (u32)(b >> 16)) >> 7;                              \
            if (fr > 31) fr = 31;                                                           \
            if (fg > 31) fg = 31;                                                           \
            if (fb > 31) fb = 31;                                                           \
            u16 o = (u16)(fr | (fg << 5) | (fb << 10));                                     \
            if (SEMI && (t & 0x8000)) o = pk_blend(dst[i], o);                              \
            dst[i] = o | (t & 0x8000) | mor;                                                \
        }                                                                                   \
    }

#define DEF_FSPAN_TG_ALL(D, DN)                                                             \
    DEF_FSPAN_TG(fspan##DN##g_0, D, 0, 0) DEF_FSPAN_TG(fspan##DN##g_1, D, 1, 0)            \
    DEF_FSPAN_TG(fspan##DN##g_0w, D, 0, 1) DEF_FSPAN_TG(fspan##DN##g_1w, D, 1, 1)
DEF_FSPAN_TG_ALL(0, 4)
DEF_FSPAN_TG_ALL(1, 8)
DEF_FSPAN_TG_ALL(2, 16)

typedef void (*FSpanT)(u16 *, int, s32, s32, const Steps *, const u16 *, const ModLut *, u16);
#define FS_ROW(DN) { { { fspan##DN##_r0, fspan##DN##_r1 }, { fspan##DN##_m0, fspan##DN##_m1 } }, { { fspan##DN##_r0w, fspan##DN##_r1w }, { fspan##DN##_m0w, fspan##DN##_m1w } } }
static const FSpanT s_fspan[3][2][2][2] = { FS_ROW(4), FS_ROW(8), FS_ROW(16) };
typedef void (*FSpanTG)(u16 *, int, s32, s32, s32, s32, s32, const Steps *, const u16 *, u16);
static const FSpanTG s_fspang[3][2][2] = {
    { { fspan4g_0, fspan4g_1 }, { fspan4g_0w, fspan4g_1w } },
    { { fspan8g_0, fspan8g_1 }, { fspan8g_0w, fspan8g_1w } },
    { { fspan16g_0, fspan16g_1 }, { fspan16g_0w, fspan16g_1w } } };

/* returns 1 if handled */
/* gradients are rounded: truncation turns exact 1:1 mappings into 0xFFFF steps */
static inline s32 rnd16(double x) { return (s32)(x >= 0 ? x + 0.5 : x - 0.5); }

static int fast_tri(const Vtx *a, const Vtx *b, const Vtx *c, int flags, int clut)
{
    int tex = flags & F_TEX, gour = flags & F_GOURAUD;
    int semi = (flags & F_SEMI) != 0, raw = (flags & F_RAW) != 0;
    if (check_mask) return 0;
    if (tex && gour) {
        /* raw textures ignore the vertex colours; equal colours need no interpolation */
        if (raw) gour = 0;
        else if (a->r == b->r && a->r == c->r && a->g == b->g && a->g == c->g && a->b == b->b && a->b == c->b) {
            gour = 0;
            if (a->r == 0x80 && a->g == 0x80 && a->b == 0x80) raw = 1;
        }
    }
    if (!RT_FAST_NO_DITHER && dither_on && (tex ? !raw : gour)) return 0;
    if (tex && tdepth == 3) return 0;

    int minx = a->x < b->x ? (a->x < c->x ? a->x : c->x) : (b->x < c->x ? b->x : c->x);
    int maxx = a->x > b->x ? (a->x > c->x ? a->x : c->x) : (b->x > c->x ? b->x : c->x);
    int miny = a->y < b->y ? (a->y < c->y ? a->y : c->y) : (b->y < c->y ? b->y : c->y);
    int maxy = a->y > b->y ? (a->y > c->y ? a->y : c->y) : (b->y > c->y ? b->y : c->y);
    if (maxx - minx > 1023 || maxy - miny > 511) return 1;
    if (maxx < dl || minx > dr || maxy < dt || miny > db) return 1;

    s32 e1x = b->x - a->x, e1y = b->y - a->y, e2x = c->x - a->x, e2y = c->y - a->y;
    s32 den = e1x * e2y - e2x * e1y;
    if (den == 0) return 1;
    double inv = 65536.0 / (double)den;
#define GX(A) rnd16((double)((b->A - a->A) * e2y - (c->A - a->A) * e1y) * inv)
#define GY(A) rnd16((double)((c->A - a->A) * e1x - (b->A - a->A) * e2x) * inv)
    Steps st = { 0, 0, 0, 0, 0 };
    s32 dudy = 0, dvdy = 0, drdy = 0, dgdy = 0, dbdy = 0;
    if (tex) { st.dudx = GX(u); st.dvdx = GX(v); dudy = GY(u); dvdy = GY(v); }
    if (gour) { st.drdx = GX(r); st.dgdx = GX(g); st.dbdx = GX(b); drdy = GY(r); dgdy = GY(g); dbdy = GY(b); }
#undef GX
#undef GY
    s32 u0 = (a->u << 16) + 0x8000, v0 = (a->v << 16) + 0x8000;
    s32 r0 = (a->r << 16) + 0x8000, g0 = (a->g << 16) + 0x8000, b0 = (a->b << 16) + 0x8000;

    ModLut m;
    const u16 *cl = NULL;
    FSpanT ft = NULL;
    FSpanTG ftg = NULL;
    FRectFn rowfn = NULL;
    if (tex) {
        cl = clut_ptr(clut);
        int tw = tw_uand != 0xFF || tw_uor || tw_vand != 0xFF || tw_vor;
        if (gour) {
            ftg = s_fspang[tdepth][tw][semi];
        } else {
            if (!raw) build_mod(&m, a->r, a->g, a->b);
            ft = s_fspan[tdepth][tw][!raw][semi];
            if (st.dvdx == 0 && st.dudx == 0x10000) rowfn = s_frect[tdepth][tw][!raw][semi];
#ifdef RT_GPU_STATS
            else {
                static int dbg;
                if (dbg++ < 40)
                    rt_log("[tri] dudx %d dvdx %d dudy %d dvdy %d  (%d,%d)-(%d,%d)-(%d,%d) uv (%d,%d)(%d,%d)(%d,%d)\n",
                           st.dudx, st.dvdx, dudy, dvdy, a->x, a->y, b->x, b->y, c->x, c->y, a->u, a->v, b->u, b->v, c->u, c->v);
            }
#endif
        }
    }
    u16 mor = set_mask ? 0x8000 : 0;
    u16 flat = (u16)((a->r >> 3) | ((a->g >> 3) << 5) | ((a->b >> 3) << 10));

    const Vtx *p0 = a, *p1 = b, *p2 = c, *t;
    if (p1->y < p0->y) { t = p0; p0 = p1; p1 = t; }
    if (p2->y < p0->y) { t = p0; p0 = p2; p2 = t; }
    if (p2->y < p1->y) { t = p1; p1 = p2; p2 = t; }
    if (p0->y == p2->y) return 1;

    s32 dxl = (s32)((double)(p2->x - p0->x) * 65536.0 / (p2->y - p0->y));
    s32 dxa = p1->y != p0->y ? (s32)((double)(p1->x - p0->x) * 65536.0 / (p1->y - p0->y)) : 0;
    s32 dxb = p2->y != p1->y ? (s32)((double)(p2->x - p1->x) * 65536.0 / (p2->y - p1->y)) : 0;
    int ystart = p0->y < dt ? dt : p0->y;
    int yend = p2->y > db + 1 ? db + 1 : p2->y;

    for (int y = ystart; y < yend; y++) {
        s32 xl = (p0->x << 16) + dxl * (y - p0->y);
        s32 xs = y < p1->y ? (p0->x << 16) + dxa * (y - p0->y) : (p1->x << 16) + dxb * (y - p1->y);
        s32 lo = xl < xs ? xl : xs, hi = xl < xs ? xs : xl;
        int x0 = (lo + 0xFFFF) >> 16, x1 = (hi + 0xFFFF) >> 16;
        if (x0 < dl) x0 = dl;
        if (x1 > dr + 1) x1 = dr + 1;
        if (x0 >= x1) continue;
        u16 *dst = g_vram + y * VRAM_W + x0;
        int n = x1 - x0;
#ifdef RT_GPU_STATS
        {
            extern u32 g_path_px[8];
            g_path_px[!tex ? 0 : gour ? 1 + semi : rowfn ? 3 + semi : 5 + semi] += n;
        }
#endif
        s32 dx = x0 - a->x, dy = y - a->y;
        if (tex) {
            s32 u = u0 + st.dudx * dx + dudy * dy, v = v0 + st.dvdx * dx + dvdy * dy;
            if (gour) {
                s32 r = r0 + st.drdx * dx + drdy * dy, g = g0 + st.dgdx * dx + dgdy * dy, bb = b0 + st.dbdx * dx + dbdy * dy;
                ftg(dst, n, u, v, r, g, bb, &st, cl, mor);
            } else if (rowfn) {
                /* affine-free span: v constant, u advancing one texel per pixel */
                rowfn(x0, x1, y, y + 1, (u >> 16) - x0, (v >> 16) - y, cl, &m, mor);
            } else
                ft(dst, n, u, v, &st, cl, &m, mor);
        } else if (gour) {
            s32 r = r0 + st.drdx * dx + drdy * dy, g = g0 + st.dgdx * dx + dgdy * dy, bb = b0 + st.dbdx * dx + dbdy * dy;
            for (int i = 0; i < n; i++, r += st.drdx, g += st.dgdx, bb += st.dbdx) {
                u16 o = (u16)(((r >> 19) & 31) | (((g >> 19) & 31) << 5) | (((bb >> 19) & 31) << 10));
                dst[i] = (semi ? pk_blend(dst[i], o) : o) | mor;
            }
        } else {
            if (semi) for (int i = 0; i < n; i++) dst[i] = pk_blend(dst[i], flat) | mor;
            else for (int i = 0; i < n; i++) dst[i] = flat | mor;
        }
    }
    return 1;
}
