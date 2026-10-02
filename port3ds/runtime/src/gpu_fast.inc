/* fast paths of the software rasterizer, included by gpu.c.
   Used whenever mask checking (and dithering, unless RT_FAST_NO_DITHER) is off,
   which is the common case; everything else goes through the reference code.

   The span/sprite loops are templated on texture depth, colour handling (MOD),
   texture window (TW) and blend mode (BL: 0 = opaque, 1..4 = semi-transparency
   mode 0..3), and copy the GPU state they need into locals: the build uses
   -fno-strict-aliasing, so otherwise every VRAM store would force the globals
   to be reloaded for the next pixel.

   MOD: 0 raw texels, 1 texels modulated per pixel through a ModLut,
        2 palette entries modulated up front into a Pal32 (4/8 bit textures). */

/* constants that aren't ARM immediates: hide them from GCC so it loads them
   into a register instead of expanding each mask into a chain of bic/shifts */
static inline u32 kreg(u32 k)
{
#ifdef __arm__
    __asm__("" : "+r"(k));
#endif
    return k;
}

/* f never has bit 15 set (callers pass a 15-bit colour) */
static inline u16 pk_blend0(u16 b, u16 f)
{
    u32 a = (u32)b & ~0x8000u, c = f;
    return (u16)((a + c - (((u32)b ^ c) & kreg(0x0421u))) >> 1);
}
static inline u16 pk_blend1(u16 b, u16 f)
{
    u32 a = (u32)b & ~0x8000u, c = f, sum = a + c;
    u32 carry = (sum - (((u32)b ^ c) & kreg(0x0421u))) & kreg(0x8420u);
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

/* BL is a compile-time constant in every caller, so this folds to one blend */
#define PK_BLEND(BL, b, f) ((BL) == 1 ? pk_blend0(b, f) : (BL) == 2 ? pk_blend1(b, f) : (BL) == 3 ? pk_blend2(b, f) : pk_blend3(b, f))

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

/* palette already modulated; bit 16 marks the transparent (all zero) texel */
#define PAL_SKIP 0x10000u
static void build_pal(u32 *pal, const u16 *clut, int n, const ModLut *m)
{
    for (int i = 0; i < n; i++) {
        u32 t = clut[i];
        pal[i] = t ? (u32)MODULATE(m, t) | (t & 0x8000) : PAL_SKIP;
    }
}

/* texture page origin; tex_fast_ok() guarantees no texel address can wrap
   around VRAM, so the fast paths index from here without masking */
#define FAST_LOCALS                                                                         \
    const u16 *l_tex = g_vram + tpy * VRAM_W + tpx;                                         \
    const u32 l_ua = tw_uand, l_uo = tw_uor, l_va = tw_vand, l_vo = tw_vor;                 \
    (void)l_tex; (void)l_ua; (void)l_uo; (void)l_va; (void)l_vo;

/* v <= 255 and tpy is 0 or 256, so rows never wrap; columns only can for
   8 bit pages at x 960 and 16 bit pages past x 768 (left to the reference code) */
static inline int tex_fast_ok(void)
{
    return tdepth == 0 || (tdepth == 1 && tpx + 128 <= VRAM_W) || (tdepth == 2 && tpx + 256 <= VRAM_W);
}

/* palette index (4/8 bit) or the texel itself (16 bit) at texture column u of row */
#define FETCH(DEPTH, row, u)                                                                \
    ((DEPTH) == 0 ? ((row)[(u) >> 2] >> (((u) & 3) << 2)) & 0xF :                           \
     (DEPTH) == 1 ? ((row)[(u) >> 1] >> (((u) & 1) << 3)) & 0xFF :                           \
                    (u32)(row)[u])

/* fetched value -> entry: final colour plus bit 15, or PAL_SKIP */
#define ENTRY(DEPTH, MOD, x, pal, m)                                                        \
    ({                                                                                      \
        u32 _e;                                                                             \
        if ((MOD) == 2) _e = ((const u32 *)(pal))[x];                                       \
        else {                                                                              \
            u32 _t = (DEPTH) < 2 ? ((const u16 *)(pal))[x] : (x);                           \
            _e = !_t ? PAL_SKIP : (MOD) ? (u32)MODULATE(m, _t) | (_t & 0x8000) : _t;        \
        }                                                                                   \
        _e;                                                                                 \
    })

/* one textured output pixel: blend, store */
#define PUT(BL, d, e, mor)                                                                  \
    do {                                                                                    \
        if (!((e) & PAL_SKIP)) {                                                            \
            u16 _o = (u16)((e) & 0x7FFF);                                                   \
            if ((BL) && ((e) & 0x8000)) _o = PK_BLEND(BL, *(d), _o);                        \
            *(d) = _o | (u16)((e) & 0x8000) | (mor);                                        \
        }                                                                                   \
    } while (0)

#define TROW(TW, v) (l_tex + ((int)((TW) ? (((v) & l_va) | l_vo) : (v)) << 10))

/* one sprite row of a 4/8 bit texture without texture window: an unaligned
   head, then one texture word per 4 (or 2) texels, then the tail.
   Always inlined with constant DEPTH/MOD/BL so it specializes fully. */
static inline __attribute__((always_inline)) void
frect_row_pal(const int DEPTH, const int MOD, const int BL, u16 *dst, const u16 *row, int px, int x1, u32 u,
              const void *pal, const ModLut *m, u16 mor)
{
    const u32 per = DEPTH == 0 ? 4 : 2, bits = DEPTH == 0 ? 4 : 8, msk = DEPTH == 0 ? 0xF : 0xFF;
    u16 *d = dst + px;
    for (; px < x1 && (u & (per - 1)); px++, d++, u = (u + 1) & 0xFF) {
        u32 e = ENTRY(DEPTH, MOD, FETCH(DEPTH, row, u), pal, m);
        PUT(BL, d, e, mor);
    }
    for (; x1 - px >= (int)per; px += per, d += per, u = (u + per) & 0xFF) {
        u32 w = row[u / per];
        u32 e0 = ENTRY(DEPTH, MOD, w & msk, pal, m);
        u32 e1 = ENTRY(DEPTH, MOD, (w >> bits) & msk, pal, m);
        PUT(BL, d, e0, mor);
        PUT(BL, d + 1, e1, mor);
        if (DEPTH == 0) {
            u32 e2 = ENTRY(DEPTH, MOD, (w >> 8) & msk, pal, m);
            u32 e3 = ENTRY(DEPTH, MOD, (w >> 12) & msk, pal, m);
            PUT(BL, d + 2, e2, mor);
            PUT(BL, d + 3, e3, mor);
        }
    }
    for (; px < x1; px++, d++, u = (u + 1) & 0xFF) {
        u32 e = ENTRY(DEPTH, MOD, FETCH(DEPTH, row, u), pal, m);
        PUT(BL, d, e, mor);
    }
}

/* ---- textured sprites (and triangle spans that map 1:1 onto a texture row) ---- */
#define DEF_FRECT(NAME, DEPTH, MOD, BL, TW)                                                 \
    static void NAME(int x0, int x1, int y0, int y1, int ubase, int vbase, const void *pal, \
                     const ModLut *m, u16 mor)                                              \
    {                                                                                       \
        FAST_LOCALS                                                                         \
        for (int py = y0; py < y1; py++) {                                                  \
            u16 *dst = g_vram + py * VRAM_W;                                                \
            const u16 *row = TROW(TW, (u32)(vbase + py) & 0xFF);                            \
            int px = x0;                                                                    \
            if ((DEPTH) < 2 && !(TW)) {                                                     \
                frect_row_pal(DEPTH, MOD, BL, dst, row, px, x1, (u32)(ubase + px) & 0xFF, pal, m, mor); \
                continue;                                                                   \
            }                                                                               \
            for (; px < x1; px++) {                                                         \
                u32 u = (u32)(ubase + px) & 0xFF;                                           \
                if (TW) u = (u & l_ua) | l_uo;                                              \
                u32 e = ENTRY(DEPTH, MOD, FETCH(DEPTH, row, u), pal, m);                    \
                PUT(BL, dst + px, e, mor);                                                  \
            }                                                                               \
        }                                                                                   \
    }

/* expands X(suffix, MOD, BL, TW) for every variant of one depth */
#define VARIANTS(X)                                                                         \
    X(r0, 0, 0, 0) X(r1, 0, 1, 0) X(r2, 0, 2, 0) X(r3, 0, 3, 0) X(r4, 0, 4, 0)              \
    X(m0, 1, 0, 0) X(m1, 1, 1, 0) X(m2, 1, 2, 0) X(m3, 1, 3, 0) X(m4, 1, 4, 0)              \
    X(p0, 2, 0, 0) X(p1, 2, 1, 0) X(p2, 2, 2, 0) X(p3, 2, 3, 0) X(p4, 2, 4, 0)              \
    X(r0w, 0, 0, 1) X(r1w, 0, 1, 1) X(r2w, 0, 2, 1) X(r3w, 0, 3, 1) X(r4w, 0, 4, 1)         \
    X(m0w, 1, 0, 1) X(m1w, 1, 1, 1) X(m2w, 1, 2, 1) X(m3w, 1, 3, 1) X(m4w, 1, 4, 1)         \
    X(p0w, 2, 0, 1) X(p1w, 2, 1, 1) X(p2w, 2, 2, 1) X(p3w, 2, 3, 1) X(p4w, 2, 4, 1)
/* 16 bit textures have no palette: their MOD 2 slot is never selected */
#define VARIANTS16(X)                                                                       \
    X(r0, 0, 0, 0) X(r1, 0, 1, 0) X(r2, 0, 2, 0) X(r3, 0, 3, 0) X(r4, 0, 4, 0)              \
    X(m0, 1, 0, 0) X(m1, 1, 1, 0) X(m2, 1, 2, 0) X(m3, 1, 3, 0) X(m4, 1, 4, 0)              \
    X(r0w, 0, 0, 1) X(r1w, 0, 1, 1) X(r2w, 0, 2, 1) X(r3w, 0, 3, 1) X(r4w, 0, 4, 1)         \
    X(m0w, 1, 0, 1) X(m1w, 1, 1, 1) X(m2w, 1, 2, 1) X(m3w, 1, 3, 1) X(m4w, 1, 4, 1)
/* table layout: [tw][mod][bl] */
#define VTABLE(P)                                                                           \
    { { { P##r0, P##r1, P##r2, P##r3, P##r4 }, { P##m0, P##m1, P##m2, P##m3, P##m4 },      \
        { P##p0, P##p1, P##p2, P##p3, P##p4 } },                                            \
      { { P##r0w, P##r1w, P##r2w, P##r3w, P##r4w }, { P##m0w, P##m1w, P##m2w, P##m3w, P##m4w }, \
        { P##p0w, P##p1w, P##p2w, P##p3w, P##p4w } } }
#define VTABLE16(P)                                                                         \
    { { { P##r0, P##r1, P##r2, P##r3, P##r4 }, { P##m0, P##m1, P##m2, P##m3, P##m4 } },    \
      { { P##r0w, P##r1w, P##r2w, P##r3w, P##r4w }, { P##m0w, P##m1w, P##m2w, P##m3w, P##m4w } } }

#define FR4(S, MOD, BL, TW) DEF_FRECT(frect4_##S, 0, MOD, BL, TW)
#define FR8(S, MOD, BL, TW) DEF_FRECT(frect8_##S, 1, MOD, BL, TW)
#define FR16(S, MOD, BL, TW) DEF_FRECT(frect16_##S, 2, MOD, BL, TW)
VARIANTS(FR4)
VARIANTS(FR8)
VARIANTS16(FR16)

typedef void (*FRectFn)(int, int, int, int, int, int, const void *, const ModLut *, u16);
static const FRectFn s_frect[3][2][3][5] = { VTABLE(frect4_), VTABLE(frect8_), VTABLE16(frect16_) };

/* blend selector for the tables: 0 opaque, else mode + 1 */
static inline int bl_index(int semi) { return semi ? blend + 1 : 0; }

static void fill_span(u16 *dst, int n, u16 c, int semi, u16 mor)
{
    switch (bl_index(semi)) {
    case 0: for (int i = 0; i < n; i++) dst[i] = c | mor; break;
    case 1: for (int i = 0; i < n; i++) dst[i] = PK_BLEND(1, dst[i], c) | mor; break;
    case 2: for (int i = 0; i < n; i++) dst[i] = PK_BLEND(2, dst[i], c) | mor; break;
    case 3: for (int i = 0; i < n; i++) dst[i] = PK_BLEND(3, dst[i], c) | mor; break;
    default: for (int i = 0; i < n; i++) dst[i] = PK_BLEND(4, dst[i], c) | mor; break;
    }
}

/* how the texels of a primitive get their colour; fills m / pal as needed.
   Returns the MOD index and the palette pointer to hand to the span code. */
static int setup_colour(int raw, int cr, int cg, int cb, int clut, int area, ModLut *m, u32 *pal, const void **out)
{
    const u16 *cl = tdepth < 2 ? clut_ptr(clut) : NULL;
    if (!raw && cr == 0x80 && cg == 0x80 && cb == 0x80) raw = 1;   /* (t * 128) >> 7 == t */
    *out = cl;
    if (raw) return 0;
    build_mod(m, cr, cg, cb);
    int n = tdepth == 0 ? 16 : 256;
    if (tdepth < 2 && area >= n) {
        build_pal(pal, cl, n, m);
        *out = pal;
        return 2;
    }
    return 1;
}

/* returns 1 if handled */
static int fast_rect(int x0, int x1, int y0, int y1, int x, int y, int u0, int v0, int clut,
                     int cr, int cg, int cb, int tex, int semi, int raw)
{
    if (check_mask) return 0;
    if (!tex) {
        u16 c = (u16)((cr >> 3) | ((cg >> 3) << 5) | ((cb >> 3) << 10));
        u16 mor = set_mask ? 0x8000 : 0;
        for (int py = y0; py < y1; py++) fill_span(g_vram + py * VRAM_W + x0, x1 - x0, c, semi, mor);
        return 1;
    }
    if (tdepth == 3 || !tex_fast_ok()) return 0;
    ModLut m;
    u32 pal[256];
    const void *p;
    int mod = setup_colour(raw, cr, cg, cb, clut, (x1 - x0) * (y1 - y0), &m, pal, &p);
    int tw = tw_uand != 0xFF || tw_uor || tw_vand != 0xFF || tw_vor;
    s_frect[tdepth][tw][mod][bl_index(semi)](x0, x1, y0, y1, u0 - x, v0 - y, p, &m, set_mask ? 0x8000 : 0);
    return 1;
}

/* ---- triangles ---- */
typedef struct { s32 dudx, dvdx, drdx, dgdx, dbdx; } Steps;

#define DEF_FSPAN_T(NAME, DEPTH, MOD, BL, TW)                                               \
    static void NAME(u16 *dst, int n, s32 u, s32 v, const Steps *st, const void *pal,       \
                     const ModLut *m, u16 mor)                                              \
    {                                                                                       \
        FAST_LOCALS                                                                         \
        s32 du = st->dudx, dv = st->dvdx;                                                   \
        if (dv == 0) {                                                                      \
            const u16 *row = TROW(TW, (u32)(v >> 16) & 0xFF);                               \
            for (int i = 0; i < n; i++, u += du) {                                          \
                u32 tu = (u32)(u >> 16) & 0xFF;                                             \
                if (TW) tu = (tu & l_ua) | l_uo;                                            \
                u32 e = ENTRY(DEPTH, MOD, FETCH(DEPTH, row, tu), pal, m);                   \
                PUT(BL, dst + i, e, mor);                                                   \
            }                                                                               \
            return;                                                                         \
        }                                                                                   \
        for (int i = 0; i < n; i++, u += du, v += dv) {                                     \
            u32 tu = (u32)(u >> 16) & 0xFF;                                                 \
            if (TW) tu = (tu & l_ua) | l_uo;                                                \
            const u16 *row = TROW(TW, (u32)(v >> 16) & 0xFF);                               \
            u32 e = ENTRY(DEPTH, MOD, FETCH(DEPTH, row, tu), pal, m);                       \
            PUT(BL, dst + i, e, mor);                                                       \
        }                                                                                   \
    }

#define FS4(S, MOD, BL, TW) DEF_FSPAN_T(fspan4_##S, 0, MOD, BL, TW)
#define FS8(S, MOD, BL, TW) DEF_FSPAN_T(fspan8_##S, 1, MOD, BL, TW)
#define FS16(S, MOD, BL, TW) DEF_FSPAN_T(fspan16_##S, 2, MOD, BL, TW)
VARIANTS(FS4)
VARIANTS(FS8)
VARIANTS16(FS16)

/* gouraud textured: per pixel modulation. Done in two passes over the span
   (texels into a buffer, then shade + blend) so that each loop's state fits in
   the ARM11 registers; the one-pass version spilled five steps per pixel. */
#define TG_CHUNK 64
#define DEF_FSPAN_TG(NAME, DEPTH, BL, TW)                                                   \
    static void NAME(u16 *dst, int n, s32 u, s32 v, s32 r, s32 g, s32 b, const Steps *st,   \
                     const u16 *clut, u16 mor)                                              \
    {                                                                                       \
        FAST_LOCALS                                                                         \
        u16 tb[TG_CHUNK];                                                                   \
        while (n > 0) {                                                                     \
            int c = n < TG_CHUNK ? n : TG_CHUNK;                                            \
            {                                                                               \
                s32 du = st->dudx, dv = st->dvdx;                                           \
                for (int i = 0; i < c; i++, u += du, v += dv) {                             \
                    u32 tu = (u32)(u >> 16) & 0xFF;                                         \
                    if (TW) tu = (tu & l_ua) | l_uo;                                        \
                    const u16 *row = TROW(TW, (u32)(v >> 16) & 0xFF);                       \
                    u32 x = FETCH(DEPTH, row, tu);                                          \
                    tb[i] = (DEPTH) < 2 ? clut[x] : (u16)x;                                 \
                }                                                                           \
            }                                                                               \
            {                                                                               \
                s32 dr = st->drdx, dg = st->dgdx, db_ = st->dbdx;                           \
                for (int i = 0; i < c; i++, r += dr, g += dg, b += db_) {                   \
                    u32 t = tb[i];                                                          \
                    if (!t) continue;                                                       \
                    u32 fr = ((t & 31) * (u32)(r >> 16)) >> 7;                              \
                    u32 fg = (((t >> 5) & 31) * (u32)(g >> 16)) >> 7;                       \
                    u32 fb = (((t >> 10) & 31) * (u32)(b >> 16)) >> 7;                      \
                    if (fr > 31) fr = 31;                                                   \
                    if (fg > 31) fg = 31;                                                   \
                    if (fb > 31) fb = 31;                                                   \
                    u16 o = (u16)(fr | (fg << 5) | (fb << 10));                             \
                    if ((BL) && (t & 0x8000)) o = PK_BLEND(BL, dst[i], o);                  \
                    dst[i] = o | (u16)(t & 0x8000) | mor;                                   \
                }                                                                           \
            }                                                                               \
            dst += c;                                                                       \
            n -= c;                                                                         \
        }                                                                                   \
    }

/* gouraud variants only exist modulated; MOD is ignored */
#define FG4(S, MOD, BL, TW) DEF_FSPAN_TG(fspan4g_##S, 0, BL, TW)
#define FG8(S, MOD, BL, TW) DEF_FSPAN_TG(fspan8g_##S, 1, BL, TW)
#define FG16(S, MOD, BL, TW) DEF_FSPAN_TG(fspan16g_##S, 2, BL, TW)
#define GVARIANTS(X)                                                                        \
    X(0, 1, 0, 0) X(1, 1, 1, 0) X(2, 1, 2, 0) X(3, 1, 3, 0) X(4, 1, 4, 0)                   \
    X(0w, 1, 0, 1) X(1w, 1, 1, 1) X(2w, 1, 2, 1) X(3w, 1, 3, 1) X(4w, 1, 4, 1)
GVARIANTS(FG4)
GVARIANTS(FG8)
GVARIANTS(FG16)
#define GTABLE(P) { { P##0, P##1, P##2, P##3, P##4 }, { P##0w, P##1w, P##2w, P##3w, P##4w } }

typedef void (*FSpanT)(u16 *, int, s32, s32, const Steps *, const void *, const ModLut *, u16);
static const FSpanT s_fspan[3][2][3][5] = { VTABLE(fspan4_), VTABLE(fspan8_), VTABLE16(fspan16_) };
typedef void (*FSpanTG)(u16 *, int, s32, s32, s32, s32, s32, const Steps *, const u16 *, u16);
static const FSpanTG s_fspang[3][2][5] = { GTABLE(fspan4g_), GTABLE(fspan8g_), GTABLE(fspan16g_) };

/* untextured gouraud span */
#define DEF_GSPAN(NAME, BL)                                                                 \
    static void NAME(u16 *dst, int n, s32 r, s32 g, s32 b, const Steps *st, u16 mor)        \
    {                                                                                       \
        s32 dr = st->drdx, dg = st->dgdx, db_ = st->dbdx;                                   \
        for (int i = 0; i < n; i++, r += dr, g += dg, b += db_) {                           \
            u16 o = (u16)(((r >> 19) & 31) | (((g >> 19) & 31) << 5) | (((b >> 19) & 31) << 10)); \
            dst[i] = ((BL) ? PK_BLEND(BL, dst[i], o) : o) | mor;                            \
        }                                                                                   \
    }
DEF_GSPAN(gspan0, 0) DEF_GSPAN(gspan1, 1) DEF_GSPAN(gspan2, 2) DEF_GSPAN(gspan3, 3) DEF_GSPAN(gspan4, 4)
typedef void (*GSpanFn)(u16 *, int, s32, s32, s32, const Steps *, u16);
static const GSpanFn s_gspan[5] = { gspan0, gspan1, gspan2, gspan3, gspan4 };

/* 4 bit palette shaded by one colour, exactly as DEF_FSPAN_TG would shade it */
static void build_pal16_shaded(u32 *pal, const u16 *clut, u32 cr, u32 cg, u32 cb)
{
    for (int i = 0; i < 16; i++) {
        u32 t = clut[i];
        if (!t) { pal[i] = PAL_SKIP; continue; }
        u32 fr = ((t & 31) * cr) >> 7, fg = (((t >> 5) & 31) * cg) >> 7, fb = (((t >> 10) & 31) * cb) >> 7;
        if (fr > 31) fr = 31;
        if (fg > 31) fg = 31;
        if (fb > 31) fb = 31;
        pal[i] = fr | (fg << 5) | (fb << 10) | (t & 0x8000);
    }
}

/* returns 1 if handled */
/* gradients are rounded: truncation turns exact 1:1 mappings into 0xFFFF steps */
static inline s32 rnd16(double x) { return (s32)(x >= 0 ? x + 0.5 : x - 0.5); }

#if defined(RT_GPU_PROF) && GPU_MAIN
u32 g_tri_calls, g_tri_spans;
#endif
static int fast_tri(const Vtx *a, const Vtx *b, const Vtx *c, int flags, int clut)
{
#ifdef RT_GPU_PROF
    u64 _ts = host_ticks_us();
    g_tri_calls++;
#endif
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
    if (tex && (tdepth == 3 || !tex_fast_ok())) return 0;

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

    int bl = bl_index(semi);
    ModLut m;
    u32 pal[256];
    const void *p = NULL;
    const u16 *cl = NULL;
    FSpanT ft = NULL;
    FSpanTG ftg = NULL;
    FRectFn rowfn = NULL;
    /* 4 bit gouraud with a purely vertical colour gradient: each span has a single
       colour, so shade the palette once per colour and use the flat span code */
    int gflat = 0;
    u32 gkey = 0xFFFFFFFFu;
    if (tex) {
        int tw = tw_uand != 0xFF || tw_uor || tw_vand != 0xFF || tw_vor;
        if (gour) {
            cl = clut_ptr(clut);
            ftg = s_fspang[tdepth][tw][bl];
            if (tdepth == 0 && st.drdx == 0 && st.dgdx == 0 && st.dbdx == 0) {
                gflat = 1;
                ft = s_fspan[0][tw][2][bl];
                if (st.dvdx == 0 && st.dudx == 0x10000) rowfn = s_frect[0][tw][2][bl];
                p = pal;
            }
        } else {
            int area = (den < 0 ? -den : den) / 2;
            int mod = setup_colour(raw, a->r, a->g, a->b, clut, area, &m, pal, &p);
            ft = s_fspan[tdepth][tw][mod][bl];
            if (st.dvdx == 0 && st.dudx == 0x10000) rowfn = s_frect[tdepth][tw][mod][bl];
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
    /* the clip rectangle is copied too: span stores could alias the globals */
    const int cdl = dl, cdr = dr;
    int ystart = p0->y < dt ? dt : p0->y;
    int yend = p2->y > db + 1 ? db + 1 : p2->y;

#ifdef RT_GPU_PROF
    g_prof[PROF_G_SETUP] += host_ticks_us() - _ts;
    g_tri_spans += yend > ystart ? yend - ystart : 0;
#endif
    for (int y = ystart; y < yend; y++) {
        s32 xl = (p0->x << 16) + dxl * (y - p0->y);
        s32 xs = y < p1->y ? (p0->x << 16) + dxa * (y - p0->y) : (p1->x << 16) + dxb * (y - p1->y);
        s32 lo = xl < xs ? xl : xs, hi = xl < xs ? xs : xl;
        int x0 = (lo + 0xFFFF) >> 16, x1 = (hi + 0xFFFF) >> 16;
        if (x0 < cdl) x0 = cdl;
        if (x1 > cdr + 1) x1 = cdr + 1;
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
                if (gflat) {
                    u32 cr = (u32)(r >> 16), cg = (u32)(g >> 16), cb = (u32)(bb >> 16);
                    if ((cr | cg | cb) <= 255) {   /* in range: the palette math is identical */
                        u32 key = cr | (cg << 8) | (cb << 16);
                        if (key != gkey) { gkey = key; build_pal16_shaded(pal, cl, cr, cg, cb); }
                        if (rowfn) rowfn(x0, x1, y, y + 1, (u >> 16) - x0, (v >> 16) - y, p, &m, mor);
                        else ft(dst, n, u, v, &st, p, &m, mor);
                        continue;
                    }
                }
                ftg(dst, n, u, v, r, g, bb, &st, cl, mor);
            } else if (rowfn) {
                /* affine-free span: v constant, u advancing one texel per pixel */
                rowfn(x0, x1, y, y + 1, (u >> 16) - x0, (v >> 16) - y, p, &m, mor);
            } else
                ft(dst, n, u, v, &st, p, &m, mor);
        } else if (gour) {
            s32 r = r0 + st.drdx * dx + drdy * dy, g = g0 + st.dgdx * dx + dgdy * dy, bb = b0 + st.dbdx * dx + dbdy * dy;
            s_gspan[bl](dst, n, r, g, bb, &st, mor);
        } else
            fill_span(dst, n, flat, semi, mor);
    }
    return 1;
}
