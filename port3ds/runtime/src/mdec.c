#include "rt.h"
#include <stdlib.h>

/* port of RecompOne.Runtime.Mdec */

static const int ZIGZAG[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static const s16 DEFAULT_SCALE[64] = {
    0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82,
    0x7D8A, 0x6A6D, 0x471C, 0x18F8, (s16)0xE707, (s16)0xB8E3, (s16)0x9592, (s16)0x8275,
    0x7641, 0x30FB, (s16)0xCF04, (s16)0x89BE, (s16)0x89BE, (s16)0xCF04, 0x30FB, 0x7641,
    0x6A6D, (s16)0xE707, (s16)0x8275, (s16)0xB8E3, 0x471C, 0x7D8A, 0x18F8, (s16)0x9592,
    0x5A82, (s16)0xA57D, (s16)0xA57D, 0x5A82, 0x5A82, (s16)0xA57D, (s16)0xA57D, 0x5A82,
    0x471C, (s16)0x8275, 0x18F8, 0x6A6D, (s16)0x9592, (s16)0xE707, 0x7D8A, (s16)0xB8E3,
    0x30FB, (s16)0x89BE, 0x7641, (s16)0xCF04, (s16)0xCF04, 0x7641, (s16)0x89BE, 0x30FB,
    0x18F8, (s16)0xB8E3, 0x6A6D, (s16)0x8275, 0x7D8A, (s16)0x9592, 0x471C, (s16)0xE707,
};

static u8 q_luma[64], q_chroma[64];
static int sc[64];          /* scale table / 8, rebuilt when the game uploads a new one */
static int sc_dirty = 1;
static s16 scale[64];
static int depth, is_signed, bit15, en_in, en_out;
enum { M_IDLE, M_DECODE, M_QUANT, M_SCALE };
static int mode = M_IDLE, remaining = -1, quant_color;

static u16 *in_hw;
static int in_n, in_cap;
static u8 tbl[128];
static int tbl_n;

static u32 *out_q;
static int out_r, out_w, out_cap;
static int read_pos;

static int out_count(void) { return out_w - out_r; }

void mdec_init(void)
{
    memcpy(scale, DEFAULT_SCALE, sizeof scale);
    sc_dirty = 1;
    mode = M_IDLE;
    remaining = -1;
    in_n = 0;
    out_r = out_w = 0;
}

u32 mdec_read_status(void)
{
    u32 st = 0;
    if (out_count() == 0) st |= 1u << 31;
    if (mode != M_IDLE) st |= 1u << 29;
    if (en_in && remaining > 0) st |= 1u << 28;
    if (en_out && out_count() > 0) st |= 1u << 27;
    st |= (u32)(depth & 3) << 25;
    if (is_signed) st |= 1u << 24;
    if (bit15) st |= 1u << 23;
    u32 rem = remaining > 0 ? (u32)(remaining - 1) : 0xFFFFu;
    st |= rem & 0xFFFFu;
    return st;
}

u32 mdec_read_data(void)
{
    if (out_r < out_w) {
        u32 v = out_q[out_r++];
        if (out_r == out_w) out_r = out_w = 0;
        return v;
    }
    return 0;
}

void mdec_write_ctrl(u32 v)
{
    if (v & (1u << 31)) {
        mode = M_IDLE;
        remaining = -1;
        in_n = tbl_n = 0;
        out_r = out_w = 0;
        en_in = en_out = 0;
        return;
    }
    en_in = (v & (1u << 30)) != 0;
    en_out = (v & (1u << 29)) != 0;
}

static u16 next_hw(void) { return read_pos < in_n ? in_hw[read_pos++] : 0xFE00; }

static int s10(u16 n) { int v = n & 0x3FF; return (v & 0x200) ? v - 0x400 : v; }

/* (a + 0xFFF) >> 13 with the magnitude rounded down, i.e. a truncating divide */
static inline int rnd13(s32 a) { return (a + 0xFFF) / 8192; }

/* separable 8x8 IDCT, same integer math as the C# version (exact sums), but
   driven by the list of non-zero coefficients and the mask of non-zero rows */
static void idct(const u8 *pos, const int *val, int nc, int *out)
{
    if (sc_dirty) {
        for (int i = 0; i < 64; i++) sc[i] = scale[i] / 8;
        sc_dirty = 0;
    }
    /* pass 1: acc[y*8+j] = sum_z blk[y+z*8] * sc[z*8+j] */
    s32 acc[64];
    unsigned rows = 0;
    for (int i = 0; i < nc; i++) {
        int p = pos[i], y = p & 7, v = val[i];
        const int *col = sc + (p & ~7);
        s32 *d = acc + y * 8;
        if (!(rows & (1u << y))) {
            rows |= 1u << y;
            d[0] = v * col[0]; d[1] = v * col[1]; d[2] = v * col[2]; d[3] = v * col[3];
            d[4] = v * col[4]; d[5] = v * col[5]; d[6] = v * col[6]; d[7] = v * col[7];
        } else {
            d[0] += v * col[0]; d[1] += v * col[1]; d[2] += v * col[2]; d[3] += v * col[3];
            d[4] += v * col[4]; d[5] += v * col[5]; d[6] += v * col[6]; d[7] += v * col[7];
        }
    }
    int tmp[64];
    unsigned m = rows;
    while (m) {
        int y = __builtin_ctz(m);
        m &= m - 1;
        for (int j = 0; j < 8; j++) tmp[y * 8 + j] = rnd13(acc[y * 8 + j]);
    }
    /* pass 2: out[j*8+i] = sum_y tmp[y*8+j] * sc[y*8+i] */
    for (int j = 0; j < 8; j++) {
        s32 a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0, a7 = 0;
        m = rows;
        while (m) {
            int y = __builtin_ctz(m);
            m &= m - 1;
            int v = tmp[y * 8 + j];
            if (!v) continue;
            const int *col = sc + y * 8;
            a0 += v * col[0]; a1 += v * col[1]; a2 += v * col[2]; a3 += v * col[3];
            a4 += v * col[4]; a5 += v * col[5]; a6 += v * col[6]; a7 += v * col[7];
        }
        int *o = out + j * 8;
        o[0] = rnd13(a0); o[1] = rnd13(a1); o[2] = rnd13(a2); o[3] = rnd13(a3);
        o[4] = rnd13(a4); o[5] = rnd13(a5); o[6] = rnd13(a6); o[7] = rnd13(a7);
    }
}

static int decode_block(const u8 *qt, int *blk)
{
    u8 pos[64];
    int val[64], nc = 0;
    u16 n = next_hw();
    while (n == 0xFE00 && read_pos < in_n) n = next_hw();
    if (n == 0xFE00) return 0;
    int qs = (n >> 10) & 0x3F;
    int v = s10(n) * qt[0];
    int k = 0;
    for (;;) {
        if (qs == 0) v = s10(n) * 2;
        if (v < -0x400) v = -0x400;
        if (v > 0x3FF) v = 0x3FF;
        if (v) {
            /* k strictly increases, so a position is never written twice */
            pos[nc] = (u8)ZIGZAG[k];
            val[nc++] = v;
        }
        n = next_hw();
        if (n == 0xFE00) break;
        k += ((n >> 10) & 0x3F) + 1;
        if (k > 63) break;
        v = (s10(n) * qt[k] * qs + 4) / 8;
    }
    idct(pos, val, nc, blk);
    return 1;
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* signed sample clamped to -128..127, returned biased to 0..255 */
static inline u32 sat8(int v)
{
#if defined(__ARM_ARCH) && __ARM_ARCH >= 6
    u32 r;
    __asm__("usat %0, #8, %1" : "=r"(r) : "r"(v + 128));
    return r;
#else
    v += 128;
    return (u32)(v < 0 ? 0 : v > 255 ? 255 : v);
#endif
}

/* fixed point r=1.402cr, g=-0.3437cb-0.7143cr, b=1.772cb (rounded half away from 0) */
static inline int rnd10(int x) { return (x + (x >= 0 ? 512 : -512)) / 1024; }

static void chroma_terms(const int *cr, const int *cb, int *R, int *G, int *B)
{
    for (int c = 0; c < 64; c++) {
        R[c] = rnd10(cr[c] * 1436);
        G[c] = rnd10(-(cb[c] * 352) - (cr[c] * 731));
        B[c] = rnd10(cb[c] * 1815);
    }
}

/* one 8x8 luma quadrant of a 16x16 macroblock, as packed rgb bytes */
static void yuv_to_rgb(const int *R, const int *G, const int *B, const int *y, int xx, int yy, u8 *dst)
{
    u32 x80 = is_signed ? 0x80 : 0;
    for (int py = 0; py < 8; py++) {
        const int *yr = y + py * 8;
        int cb = ((py + yy) >> 1) * 8 + (xx >> 1);
        u8 *d = dst + ((py + yy) * 16 + xx) * 3;
        for (int px = 0; px < 8; px += 2, d += 6) {
            int c = cb + (px >> 1);
            int r = R[c], g = G[c], b = B[c];
            d[0] = (u8)(sat8(yr[px] + r) ^ x80);
            d[1] = (u8)(sat8(yr[px] + g) ^ x80);
            d[2] = (u8)(sat8(yr[px] + b) ^ x80);
            d[3] = (u8)(sat8(yr[px + 1] + r) ^ x80);
            d[4] = (u8)(sat8(yr[px + 1] + g) ^ x80);
            d[5] = (u8)(sat8(yr[px + 1] + b) ^ x80);
        }
    }
}

/* (v >> 3) clamped to 0..31; equals sat8(v - 128) >> 3 */
static inline u32 sat5s3(int v)
{
#if defined(__ARM_ARCH) && __ARM_ARCH >= 6
    u32 r;
    __asm__("usat %0, #5, %1, asr #3" : "=r"(r) : "r"(v));
    return r;
#else
    v >>= 3;
    return (u32)(v < 0 ? 0 : v > 31 ? 31 : v);
#endif
}

/* same, straight to 15-bit pixels (two per output word); R/G/B are biased by +128 */
static void yuv_to_15(const int *R, const int *G, const int *B, const int *y, int xx, int yy, u32 *dst)
{
    /* signed output flips the top bit of each byte, i.e. bit 4 of each 5-bit field */
    u32 x = (is_signed ? 0x42104210u : 0) | (bit15 ? 0x80008000u : 0);
    for (int py = 0; py < 8; py++) {
        const int *yr = y + py * 8;
        int cb = ((py + yy) >> 1) * 8 + (xx >> 1);
        u32 *d = dst + ((py + yy) * 16 + xx) / 2;
        for (int px = 0; px < 8; px += 2) {
            int c = cb + (px >> 1);
            int r = R[c], g = G[c], b = B[c];
            int y0 = yr[px], y1 = yr[px + 1];
            u32 p0 = sat5s3(y0 + r) | (sat5s3(y0 + g) << 5) | (sat5s3(y0 + b) << 10);
            u32 p1 = sat5s3(y1 + r) | (sat5s3(y1 + g) << 5) | (sat5s3(y1 + b) << 10);
            *d++ = (p0 | (p1 << 16)) ^ x;
        }
    }
}

static u32 *out_reserve(int n)
{
    if (out_w + n > out_cap) {
        if (out_r > 0) {
            memmove(out_q, out_q + out_r, (out_w - out_r) * sizeof(u32));
            out_w -= out_r;
            out_r = 0;
        }
        while (out_w + n > out_cap) {
            out_cap = out_cap ? out_cap * 2 : 16384;
            out_q = realloc(out_q, out_cap * sizeof(u32));
        }
    }
    u32 *p = out_q + out_w;
    out_w += n;
    return p;
}

static void pack_bytes(const u8 *b, int n)
{
    u32 *d = out_reserve(n / 4);
    for (int i = 0; i < n; i += 4) *d++ = b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | ((u32)b[i + 3] << 24);
}

static void decode_all(void)
{
    read_pos = 0;
    int color = depth >= 2;
    int crb[64], cbb[64], yb[64], R[64], G[64], B[64];
    u8 rgb[16 * 16 * 3];
    while (read_pos < in_n) {
        if (color) {
            if (!decode_block(q_chroma, crb)) break;
            decode_block(q_chroma, cbb);
            chroma_terms(crb, cbb, R, G, B);
            if (depth == 3) {
                for (int c = 0; c < 64; c++) { R[c] += 128; G[c] += 128; B[c] += 128; }
                u32 *d = out_reserve(128);
                for (int q = 0; q < 4; q++) {
                    decode_block(q_luma, yb);
                    yuv_to_15(R, G, B, yb, (q & 1) * 8, (q >> 1) * 8, d);
                }
            } else {
                for (int q = 0; q < 4; q++) {
                    decode_block(q_luma, yb);
                    yuv_to_rgb(R, G, B, yb, (q & 1) * 8, (q >> 1) * 8, rgb);
                }
                pack_bytes(rgb, 256 * 3);
            }
        } else {
            if (!decode_block(q_luma, yb)) break;
            u8 bytes[64];
            int nb = depth == 0 ? 32 : 64;
            memset(bytes, 0, sizeof bytes);
            for (int i = 0; i < 64; i++) {
                int v = clampi(yb[i], -128, 127);
                if (!is_signed) v ^= 0x80;
                if (depth == 1) bytes[i] = (u8)v;
                else {
                    int nib = (v & 0xFF) >> 4;
                    if (!(i & 1)) bytes[i / 2] = (u8)nib;
                    else bytes[i / 2] |= (u8)(nib << 4);
                }
            }
            pack_bytes(bytes, nb);
        }
    }
}

/* DMA1 fast path: copy decoded words straight into RAM */
int mdec_read_block(u32 *dst, int n)
{
    if (n > out_w - out_r) return 0;
    memcpy(dst, out_q + out_r, (size_t)n * 4);
    out_r += n;
    if (out_r == out_w) out_r = out_w = 0;
    return 1;
}

/* DMA0 fast path: bulk data words of a decode command (not the last one,
   which triggers the decode through mdec_write0) */
int mdec_write_block(const u32 *src, int n)
{
    if (mode != M_DECODE) return 0;
    if (n > remaining - 1) n = remaining - 1;
    if (n <= 0) return 0;
    while (in_n + 2 * n > in_cap) {
        in_cap = in_cap ? in_cap * 2 : 65536;
        in_hw = realloc(in_hw, in_cap * sizeof(u16));
    }
    memcpy(in_hw + in_n, src, (size_t)n * 4);   /* little endian: low half first */
    in_n += 2 * n;
    remaining -= n;
    return n;
}

void mdec_write0(u32 w)
{
    if (mode == M_IDLE) {
        u32 cmd = (w >> 29) & 7;
        switch (cmd) {
        case 1:
            depth = (int)((w >> 27) & 3);
            is_signed = (w & (1u << 26)) != 0;
            bit15 = (w & (1u << 25)) != 0;
            remaining = (int)(w & 0xFFFF);
            in_n = 0;
            mode = M_DECODE;
            break;
        case 2:
            quant_color = (w & 1) != 0;
            remaining = quant_color ? 32 : 16;
            tbl_n = 0;
            mode = M_QUANT;
            break;
        case 3:
            remaining = 32;
            tbl_n = 0;
            mode = M_SCALE;
            break;
        default:
            depth = (int)((w >> 27) & 3);
            is_signed = (w & (1u << 26)) != 0;
            bit15 = (w & (1u << 25)) != 0;
            break;
        }
        return;
    }
    if (mode == M_DECODE) {
        if (in_n + 2 > in_cap) {
            in_cap = in_cap ? in_cap * 2 : 65536;
            in_hw = realloc(in_hw, in_cap * sizeof(u16));
        }
        in_hw[in_n++] = (u16)w;
        in_hw[in_n++] = (u16)(w >> 16);
    } else if (tbl_n + 4 <= (int)sizeof tbl) {
        tbl[tbl_n++] = (u8)w;
        tbl[tbl_n++] = (u8)(w >> 8);
        tbl[tbl_n++] = (u8)(w >> 16);
        tbl[tbl_n++] = (u8)(w >> 24);
    }
    if (--remaining > 0) return;
    if (mode == M_DECODE) { u64 t0 = host_ticks_us(); decode_all(); g_prof[PROF_MDEC] += host_ticks_us() - t0; }
    else if (mode == M_QUANT) {
        memcpy(q_luma, tbl, 64);
        if (quant_color) memcpy(q_chroma, tbl + 64, 64);
    } else if (mode == M_SCALE) {
        for (int i = 0; i < 64; i++) scale[i] = (s16)(tbl[i * 2] | (tbl[i * 2 + 1] << 8));
        sc_dirty = 1;
    }
    mode = M_IDLE;
    remaining = -1;
}
