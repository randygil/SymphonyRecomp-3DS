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

static void out_push(u32 v)
{
    if (out_w >= out_cap) {
        if (out_r > 0) {
            memmove(out_q, out_q + out_r, (out_w - out_r) * sizeof(u32));
            out_w -= out_r;
            out_r = 0;
        }
        if (out_w >= out_cap) {
            out_cap = out_cap ? out_cap * 2 : 16384;
            out_q = realloc(out_q, out_cap * sizeof(u32));
        }
    }
    out_q[out_w++] = v;
}

static int out_count(void) { return out_w - out_r; }

void mdec_init(void)
{
    memcpy(scale, DEFAULT_SCALE, sizeof scale);
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

/* separable 8x8 IDCT; zero inputs are skipped (results are identical to the
   dense C# version since only exact integer sums are involved) */
static void idct(int *blk)
{
    static int sc[64];
    static const s16 *sc_src;
    static s16 sc_copy[64];
    if (sc_src != scale || memcmp(sc_copy, scale, sizeof sc_copy)) {
        for (int i = 0; i < 64; i++) sc[i] = scale[i] / 8;
        memcpy(sc_copy, scale, sizeof sc_copy);
        sc_src = scale;
    }
    int tmp[64];
    for (int pass = 0; pass < 2; pass++) {
        s32 acc[64];
        memset(acc, 0, sizeof acc);
        for (int y = 0; y < 8; y++) {
            for (int z = 0; z < 8; z++) {
                int v = blk[y + z * 8];
                if (!v) continue;
                const int *col = sc + z * 8;
                s32 *dst = acc + y * 8;
                dst[0] += v * col[0]; dst[1] += v * col[1]; dst[2] += v * col[2]; dst[3] += v * col[3];
                dst[4] += v * col[4]; dst[5] += v * col[5]; dst[6] += v * col[6]; dst[7] += v * col[7];
            }
        }
        for (int i = 0; i < 64; i++) {
            s32 t = acc[i] + 0xFFF;
            tmp[i] = t >= 0 ? t >> 13 : -((-t) >> 13);
        }
        memcpy(blk, tmp, sizeof tmp);
    }
}

static int decode_block(const u8 *qt, int *blk)
{
    memset(blk, 0, 64 * sizeof(int));
    u16 n = next_hw();
    while (n == 0xFE00 && read_pos < in_n) n = next_hw();
    if (n == 0xFE00) return 0;
    int qs = (n >> 10) & 0x3F;
    int val = s10(n) * qt[0];
    int k = 0;
    for (;;) {
        if (qs == 0) val = s10(n) * 2;
        if (val < -0x400) val = -0x400;
        if (val > 0x3FF) val = 0x3FF;
        blk[ZIGZAG[k]] = val;
        n = next_hw();
        if (n == 0xFE00) break;
        k += ((n >> 10) & 0x3F) + 1;
        if (k > 63) break;
        val = (s10(n) * qt[k] * qs + 4) / 8;
    }
    idct(blk);
    return 1;
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void yuv_to_rgb(const int *cr, const int *cb, const int *y, int xx, int yy, u8 *dst)
{
    for (int py = 0; py < 8; py++)
        for (int px = 0; px < 8; px++) {
            int c = ((px + xx) / 2) + ((py + yy) / 2) * 8;
            /* fixed point version of r=1.402cr, g=-0.3437cb-0.7143cr, b=1.772cb */
            int R = (cr[c] * 1436 + (cr[c] >= 0 ? 512 : -512)) / 1024;
            int G = (-(cb[c] * 352) - (cr[c] * 731));
            G = (G + (G >= 0 ? 512 : -512)) / 1024;
            int B = (cb[c] * 1815 + (cb[c] >= 0 ? 512 : -512)) / 1024;
            int yv = y[px + py * 8];
            int ri = clampi(yv + R, -128, 127), gi = clampi(yv + G, -128, 127), bi = clampi(yv + B, -128, 127);
            if (!is_signed) { ri ^= 0x80; gi ^= 0x80; bi ^= 0x80; }
            int o = ((px + xx) + (py + yy) * 16) * 3;
            dst[o] = (u8)ri;
            dst[o + 1] = (u8)gi;
            dst[o + 2] = (u8)bi;
        }
}

static void pack_bytes(const u8 *b, int n)
{
    for (int i = 0; i < n; i += 4) out_push(b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | ((u32)b[i + 3] << 24));
}

static void decode_all(void)
{
    read_pos = 0;
    int color = depth >= 2;
    int crb[64], cbb[64], yb[64];
    u8 rgb[16 * 16 * 3];
    while (read_pos < in_n) {
        if (color) {
            if (!decode_block(q_chroma, crb)) break;
            decode_block(q_chroma, cbb);
            for (int q = 0; q < 4; q++) {
                decode_block(q_luma, yb);
                yuv_to_rgb(crb, cbb, yb, (q & 1) * 8, (q >> 1) * 8, rgb);
            }
            if (depth == 3) {
                for (int i = 0; i < 256; i += 2) {
                    u32 a = (u32)(rgb[i * 3] >> 3) | ((u32)(rgb[i * 3 + 1] >> 3) << 5) | ((u32)(rgb[i * 3 + 2] >> 3) << 10);
                    u32 b = (u32)(rgb[i * 3 + 3] >> 3) | ((u32)(rgb[i * 3 + 4] >> 3) << 5) | ((u32)(rgb[i * 3 + 5] >> 3) << 10);
                    if (bit15) { a |= 0x8000; b |= 0x8000; }
                    out_push(a | (b << 16));
                }
            } else
                pack_bytes(rgb, 256 * 3);
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
    }
    mode = M_IDLE;
    remaining = -1;
}
