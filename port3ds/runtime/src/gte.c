#include "rt.h"

/* port of RecompOne.Runtime.Gte */

static s16 V[9];
static u8 RGBC_R, RGBC_G, RGBC_B, RGBC_CODE;
static u16 OTZ;
static s32 IR0, IR1, IR2, IR3;
static s16 SX[3], SY[3];
static u16 SZ[4];
static u32 RGB[3];
static u32 RES1;
static s32 MAC0, MAC1, MAC2, MAC3;
static u32 LZCS, LZCR;

static s16 RT[9], LLM[9], LCM[9];
static s32 TR[3], BK[3], FC[3];
static s32 OFX, OFY;
static u16 H;
static s16 DQA;
static s32 DQB;
static s16 ZSF3, ZSF4;
static u32 FLAG;

static u8 s_unr[0x101];
static int s_unr_ready;

static void build_unr(void)
{
    for (int i = 0; i < 0x101; i++) {
        int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        s_unr[i] = (u8)(v < 0 ? 0 : v > 0xFF ? 0xFF : v);
    }
    s_unr_ready = 1;
}

#define FL(b) (FLAG |= 1u << (b))

static inline s32 sat_ir(int n, s32 v, int lm)
{
    s32 mn = lm ? 0 : -0x8000;
    if (v < mn) { v = mn; FL(25 - n); }
    else if (v > 0x7FFF) { v = 0x7FFF; FL(25 - n); }
    return v;
}

static inline s32 sat_ir0(s32 v)
{
    if (v < 0) { FL(12); return 0; }
    if (v > 0x1000) { FL(12); return 0x1000; }
    return v;
}

static inline s32 sat_color(int n, s32 v)
{
    if (v < 0) { FL(21 - n); return 0; }
    if (v > 0xFF) { FL(21 - n); return 0xFF; }
    return v;
}

static inline s32 sat_sz(s32 v)
{
    if (v < 0) { FL(18); return 0; }
    if (v > 0xFFFF) { FL(18); return 0xFFFF; }
    return v;
}

static inline s32 sat_x(s32 v)
{
    if (v < -0x400) { FL(14); return -0x400; }
    if (v > 0x3FF) { FL(14); return 0x3FF; }
    return v;
}

static inline s32 sat_y(s32 v)
{
    if (v < -0x400) { FL(13); return -0x400; }
    if (v > 0x3FF) { FL(13); return 0x3FF; }
    return v;
}

static inline s64 check_mac0(s64 v)
{
    if (v > 0x7FFFFFFFLL) FL(16);
    else if (v < -0x80000000LL) FL(15);
    return v;
}

static inline void check_mac(int n, s64 v)
{
    if (v >= (1LL << 43)) FL(31 - n);
    else if (v < -(1LL << 43)) FL(28 - n);
}

static inline void set_mac(int n, s64 v, int sf, int lm)
{
    check_mac(n, v);
    s32 m = (s32)(v >> sf);
    if (n == 1) { MAC1 = m; IR1 = sat_ir(1, m, lm); }
    else if (n == 2) { MAC2 = m; IR2 = sat_ir(2, m, lm); }
    else { MAC3 = m; IR3 = sat_ir(3, m, lm); }
}

static void mat_vec(const s16 *mx, s32 t0, s32 t1, s32 t2, s32 vx, s32 vy, s32 vz, int sf, int lm)
{
    set_mac(1, ((s64)t0 << 12) + (s64)mx[0] * vx + (s64)mx[1] * vy + (s64)mx[2] * vz, sf, lm);
    set_mac(2, ((s64)t1 << 12) + (s64)mx[3] * vx + (s64)mx[4] * vy + (s64)mx[5] * vz, sf, lm);
    set_mac(3, ((s64)t2 << 12) + (s64)mx[6] * vx + (s64)mx[7] * vy + (s64)mx[8] * vz, sf, lm);
}

static void push_color(void)
{
    s32 r = sat_color(0, MAC1 >> 4), g = sat_color(1, MAC2 >> 4), b = sat_color(2, MAC3 >> 4);
    RGB[0] = RGB[1];
    RGB[1] = RGB[2];
    RGB[2] = (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)RGBC_CODE << 24);
}

static void interp(s64 in1, s64 in2, s64 in3, int sf, int lm)
{
    IR1 = sat_ir(1, (s32)((((s64)FC[0] << 12) - in1) >> sf), 0);
    IR2 = sat_ir(2, (s32)((((s64)FC[1] << 12) - in2) >> sf), 0);
    IR3 = sat_ir(3, (s32)((((s64)FC[2] << 12) - in3) >> sf), 0);
    set_mac(1, (s64)IR1 * IR0 + in1, sf, lm);
    set_mac(2, (s64)IR2 * IR0 + in2, sf, lm);
    set_mac(3, (s64)IR3 * IR0 + in3, sf, lm);
    push_color();
}

static void modulate(int sf, int lm)
{
    set_mac(1, ((s64)RGBC_R * IR1) << 4, sf, lm);
    set_mac(2, ((s64)RGBC_G * IR2) << 4, sf, lm);
    set_mac(3, ((s64)RGBC_B * IR3) << 4, sf, lm);
    push_color();
}

/* UNR division. Past the overflow check sz3 >= 1 and h < 2 * sz3, so after
   normalizing d is in [0x8000, 0xFFFF] and n < 2^17: everything but the final
   product fits in 32 bits (same results as the 64-bit reference). */
static u32 divide(u32 h, u32 sz3)
{
    if (h >= sz3 * 2) { FL(17); return 0x1FFFF; }
    int z = __builtin_clz(sz3) - 16;
    u32 n = h << z;
    u32 d = sz3 << z;
    u32 idx = (d - 0x7FC0) >> 7;
    u32 u = (u32)s_unr[idx] + 0x101;
    d = (0x2000080u - d * u) >> 8;
    d = (0x0000080u + d * u) >> 8;
    u64 res = ((u64)n * d + 0x8000) >> 16;
    return res > 0x1FFFF ? 0x1FFFFu : (u32)res;
}

static void rtp(s32 vx, s32 vy, s32 vz, int sf, int lm, int last)
{
    s64 m1 = ((s64)TR[0] << 12) + (s64)RT[0] * vx + (s64)RT[1] * vy + (s64)RT[2] * vz;
    s64 m2 = ((s64)TR[1] << 12) + (s64)RT[3] * vx + (s64)RT[4] * vy + (s64)RT[5] * vz;
    s64 m3 = ((s64)TR[2] << 12) + (s64)RT[6] * vx + (s64)RT[7] * vy + (s64)RT[8] * vz;
    check_mac(1, m1); check_mac(2, m2); check_mac(3, m3);
    MAC1 = (s32)(m1 >> sf); MAC2 = (s32)(m2 >> sf); MAC3 = (s32)(m3 >> sf);
    IR1 = sat_ir(1, MAC1, lm);
    IR2 = sat_ir(2, MAC2, lm);
    s32 ir3f = (s32)(m3 >> 12);
    if (ir3f < -0x8000 || ir3f > 0x7FFF) FL(22);
    s32 mn = lm ? 0 : -0x8000;
    IR3 = MAC3 < mn ? mn : MAC3 > 0x7FFF ? 0x7FFF : MAC3;

    s32 sz = sat_sz((s32)(m3 >> 12));
    SZ[0] = SZ[1]; SZ[1] = SZ[2]; SZ[2] = SZ[3]; SZ[3] = (u16)sz;

    u32 dv = divide(H, SZ[3]);
    s64 sx = check_mac0((s64)dv * IR1 + OFX); MAC0 = (s32)sx;
    s64 sy = check_mac0((s64)dv * IR2 + OFY); MAC0 = (s32)sy;
    s32 nx = sat_x((s32)(sx >> 16));
    s32 ny = sat_y((s32)(sy >> 16));
    SX[0] = SX[1]; SX[1] = SX[2]; SX[2] = (s16)nx;
    SY[0] = SY[1]; SY[1] = SY[2]; SY[2] = (s16)ny;

    if (last) {
        s64 dp = check_mac0((s64)dv * DQA + DQB);
        MAC0 = (s32)dp;
        IR0 = sat_ir0((s32)(dp >> 12));
    }
}

static void ncs(int vec, int sf, int lm)
{
    mat_vec(LLM, 0, 0, 0, V[vec * 3], V[vec * 3 + 1], V[vec * 3 + 2], sf, lm);
    mat_vec(LCM, BK[0], BK[1], BK[2], IR1, IR2, IR3, sf, lm);
    push_color();
}

static void ncds(int vec, int sf, int lm)
{
    mat_vec(LLM, 0, 0, 0, V[vec * 3], V[vec * 3 + 1], V[vec * 3 + 2], sf, lm);
    mat_vec(LCM, BK[0], BK[1], BK[2], IR1, IR2, IR3, sf, lm);
    interp(((s64)RGBC_R * IR1) << 4, ((s64)RGBC_G * IR2) << 4, ((s64)RGBC_B * IR3) << 4, sf, lm);
}

static void nccs(int vec, int sf, int lm)
{
    mat_vec(LLM, 0, 0, 0, V[vec * 3], V[vec * 3 + 1], V[vec * 3 + 2], sf, lm);
    mat_vec(LCM, BK[0], BK[1], BK[2], IR1, IR2, IR3, sf, lm);
    modulate(sf, lm);
}

static void mvmva(int sf, int lm, int mx, int vn, int cv)
{
    const s16 *mat = mx == 0 ? RT : mx == 1 ? LLM : mx == 2 ? LCM : RT;
    s32 vx, vy, vz;
    if (vn < 3) { vx = V[vn * 3]; vy = V[vn * 3 + 1]; vz = V[vn * 3 + 2]; }
    else { vx = IR1; vy = IR2; vz = IR3; }
    if (cv == 2) {
        sat_ir(1, (s32)((((s64)FC[0] << 12) + (s64)mat[0] * vx) >> sf), lm);
        sat_ir(2, (s32)((((s64)FC[1] << 12) + (s64)mat[3] * vx) >> sf), lm);
        sat_ir(3, (s32)((((s64)FC[2] << 12) + (s64)mat[6] * vx) >> sf), lm);
        set_mac(1, (s64)mat[1] * vy + (s64)mat[2] * vz, sf, lm);
        set_mac(2, (s64)mat[4] * vy + (s64)mat[5] * vz, sf, lm);
        set_mac(3, (s64)mat[7] * vy + (s64)mat[8] * vz, sf, lm);
        return;
    }
    s32 t0 = 0, t1 = 0, t2 = 0;
    if (cv == 0) { t0 = TR[0]; t1 = TR[1]; t2 = TR[2]; }
    else if (cv == 1) { t0 = BK[0]; t1 = BK[1]; t2 = BK[2]; }
    mat_vec(mat, t0, t1, t2, vx, vy, vz, sf, lm);
}

void gte_execute(u32 cmd)
{
    if (UNLIKELY(!s_unr_ready)) build_unr();
    FLAG = 0;
    int sf = (cmd & (1u << 19)) ? 12 : 0;
    int lm = (cmd & (1u << 10)) != 0;
    int mx = (int)((cmd >> 17) & 3), vn = (int)((cmd >> 15) & 3), cv = (int)((cmd >> 13) & 3);

    switch (cmd & 0x3F) {
    case 0x01: rtp(V[0], V[1], V[2], sf, lm, 1); break;
    case 0x30:
        rtp(V[0], V[1], V[2], sf, lm, 0);
        rtp(V[3], V[4], V[5], sf, lm, 0);
        rtp(V[6], V[7], V[8], sf, lm, 1);
        break;
    case 0x06:
        MAC0 = (s32)check_mac0((s64)SX[0] * (SY[1] - SY[2]) + (s64)SX[1] * (SY[2] - SY[0]) + (s64)SX[2] * (SY[0] - SY[1]));
        break;
    case 0x2D:
        MAC0 = (s32)check_mac0((s64)ZSF3 * (SZ[1] + SZ[2] + SZ[3]));
        OTZ = (u16)sat_sz(MAC0 >> 12);
        break;
    case 0x2E:
        MAC0 = (s32)check_mac0((s64)ZSF4 * (SZ[0] + SZ[1] + SZ[2] + SZ[3]));
        OTZ = (u16)sat_sz(MAC0 >> 12);
        break;
    case 0x12: mvmva(sf, lm, mx, vn, cv); break;
    case 0x28:
        set_mac(1, (s64)IR1 * IR1, sf, lm);
        set_mac(2, (s64)IR2 * IR2, sf, lm);
        set_mac(3, (s64)IR3 * IR3, sf, lm);
        break;
    case 0x0C: {
        s32 a = IR1, b = IR2, d = IR3;
        set_mac(1, (s64)RT[4] * d - (s64)RT[8] * b, sf, lm);
        set_mac(2, (s64)RT[8] * a - (s64)RT[0] * d, sf, lm);
        set_mac(3, (s64)RT[0] * b - (s64)RT[4] * a, sf, lm);
        break;
    }
    case 0x3D:
        set_mac(1, (s64)IR0 * IR1, sf, lm);
        set_mac(2, (s64)IR0 * IR2, sf, lm);
        set_mac(3, (s64)IR0 * IR3, sf, lm);
        push_color();
        break;
    case 0x3E:
        set_mac(1, ((s64)MAC1 << sf) + (s64)IR0 * IR1, sf, lm);
        set_mac(2, ((s64)MAC2 << sf) + (s64)IR0 * IR2, sf, lm);
        set_mac(3, ((s64)MAC3 << sf) + (s64)IR0 * IR3, sf, lm);
        push_color();
        break;
    case 0x10: interp((s64)RGBC_R << 16, (s64)RGBC_G << 16, (s64)RGBC_B << 16, sf, lm); break;
    case 0x2A:
        for (int i = 0; i < 3; i++)
            interp((s64)(RGB[0] & 0xFF) << 16, (s64)((RGB[0] >> 8) & 0xFF) << 16, (s64)((RGB[0] >> 16) & 0xFF) << 16, sf, lm);
        break;
    case 0x11: interp((s64)IR1 << 12, (s64)IR2 << 12, (s64)IR3 << 12, sf, lm); break;
    case 0x29: interp(((s64)RGBC_R * IR1) << 4, ((s64)RGBC_G * IR2) << 4, ((s64)RGBC_B * IR3) << 4, sf, lm); break;
    case 0x1E: ncs(0, sf, lm); break;
    case 0x20: ncs(0, sf, lm); ncs(1, sf, lm); ncs(2, sf, lm); break;
    case 0x13: ncds(0, sf, lm); break;
    case 0x16: ncds(0, sf, lm); ncds(1, sf, lm); ncds(2, sf, lm); break;
    case 0x1B: nccs(0, sf, lm); break;
    case 0x3F: nccs(0, sf, lm); nccs(1, sf, lm); nccs(2, sf, lm); break;
    case 0x1C:
        mat_vec(LCM, BK[0], BK[1], BK[2], IR1, IR2, IR3, sf, lm);
        modulate(sf, lm);
        break;
    case 0x14:
        mat_vec(LCM, BK[0], BK[1], BK[2], IR1, IR2, IR3, sf, lm);
        interp(((s64)RGBC_R * IR1) << 4, ((s64)RGBC_G * IR2) << 4, ((s64)RGBC_B * IR3) << 4, sf, lm);
        break;
    }
    if (FLAG & 0x7F87E000u) FLAG |= 0x80000000u;
}

static u32 pack(s16 a, s16 b) { return (u32)(u16)a | ((u32)(u16)b << 16); }

u32 gte_read(int reg)
{
    switch (reg) {
    case 0: return pack(V[0], V[1]);
    case 1: return (u32)(s32)V[2];
    case 2: return pack(V[3], V[4]);
    case 3: return (u32)(s32)V[5];
    case 4: return pack(V[6], V[7]);
    case 5: return (u32)(s32)V[8];
    case 6: return RGBC_R | (RGBC_G << 8) | (RGBC_B << 16) | ((u32)RGBC_CODE << 24);
    case 7: return OTZ;
    case 8: return (u32)IR0;
    case 9: return (u32)IR1;
    case 10: return (u32)IR2;
    case 11: return (u32)IR3;
    case 12: return pack(SX[0], SY[0]);
    case 13: return pack(SX[1], SY[1]);
    case 14: case 15: return pack(SX[2], SY[2]);
    case 16: return SZ[0];
    case 17: return SZ[1];
    case 18: return SZ[2];
    case 19: return SZ[3];
    case 20: return RGB[0];
    case 21: return RGB[1];
    case 22: return RGB[2];
    case 23: return RES1;
    case 24: return (u32)MAC0;
    case 25: return (u32)MAC1;
    case 26: return (u32)MAC2;
    case 27: return (u32)MAC3;
    case 28: case 29: {
        s32 r = IR1 >> 7, g = IR2 >> 7, b = IR3 >> 7;
        r = r < 0 ? 0 : r > 0x1F ? 0x1F : r;
        g = g < 0 ? 0 : g > 0x1F ? 0x1F : g;
        b = b < 0 ? 0 : b > 0x1F ? 0x1F : b;
        return (u32)(r | (g << 5) | (b << 10));
    }
    case 30: return LZCS;
    case 31: return LZCR;
    }
    return 0;
}

void gte_write(int reg, u32 v)
{
    switch (reg) {
    case 0: V[0] = (s16)v; V[1] = (s16)(v >> 16); break;
    case 1: V[2] = (s16)v; break;
    case 2: V[3] = (s16)v; V[4] = (s16)(v >> 16); break;
    case 3: V[5] = (s16)v; break;
    case 4: V[6] = (s16)v; V[7] = (s16)(v >> 16); break;
    case 5: V[8] = (s16)v; break;
    case 6: RGBC_R = (u8)v; RGBC_G = (u8)(v >> 8); RGBC_B = (u8)(v >> 16); RGBC_CODE = (u8)(v >> 24); break;
    case 7: OTZ = (u16)v; break;
    case 8: IR0 = (s16)v; break;
    case 9: IR1 = (s16)v; break;
    case 10: IR2 = (s16)v; break;
    case 11: IR3 = (s16)v; break;
    case 12: SX[0] = (s16)v; SY[0] = (s16)(v >> 16); break;
    case 13: SX[1] = (s16)v; SY[1] = (s16)(v >> 16); break;
    case 14: SX[2] = (s16)v; SY[2] = (s16)(v >> 16); break;
    case 15:
        SX[0] = SX[1]; SY[0] = SY[1]; SX[1] = SX[2]; SY[1] = SY[2];
        SX[2] = (s16)v; SY[2] = (s16)(v >> 16);
        break;
    case 16: SZ[0] = (u16)v; break;
    case 17: SZ[1] = (u16)v; break;
    case 18: SZ[2] = (u16)v; break;
    case 19: SZ[3] = (u16)v; break;
    case 20: RGB[0] = v; break;
    case 21: RGB[1] = v; break;
    case 22: RGB[2] = v; break;
    case 23: RES1 = v; break;
    case 24: MAC0 = (s32)v; break;
    case 25: MAC1 = (s32)v; break;
    case 26: MAC2 = (s32)v; break;
    case 27: MAC3 = (s32)v; break;
    case 28:
        IR1 = (s32)((v & 0x1F) << 7);
        IR2 = (s32)(((v >> 5) & 0x1F) << 7);
        IR3 = (s32)(((v >> 10) & 0x1F) << 7);
        break;
    case 30: {
        LZCS = v;
        u32 t = (v & 0x80000000u) ? ~v : v;
        LZCR = t == 0 ? 32 : (u32)__builtin_clz(t);
        break;
    }
    default: break;
    }
}

u32 gte_read_ctrl(int reg)
{
    switch (reg) {
    case 0: return pack(RT[0], RT[1]);
    case 1: return pack(RT[2], RT[3]);
    case 2: return pack(RT[4], RT[5]);
    case 3: return pack(RT[6], RT[7]);
    case 4: return (u32)(s32)RT[8];
    case 5: return (u32)TR[0];
    case 6: return (u32)TR[1];
    case 7: return (u32)TR[2];
    case 8: return pack(LLM[0], LLM[1]);
    case 9: return pack(LLM[2], LLM[3]);
    case 10: return pack(LLM[4], LLM[5]);
    case 11: return pack(LLM[6], LLM[7]);
    case 12: return (u32)(s32)LLM[8];
    case 13: return (u32)BK[0];
    case 14: return (u32)BK[1];
    case 15: return (u32)BK[2];
    case 16: return pack(LCM[0], LCM[1]);
    case 17: return pack(LCM[2], LCM[3]);
    case 18: return pack(LCM[4], LCM[5]);
    case 19: return pack(LCM[6], LCM[7]);
    case 20: return (u32)(s32)LCM[8];
    case 21: return (u32)FC[0];
    case 22: return (u32)FC[1];
    case 23: return (u32)FC[2];
    case 24: return (u32)OFX;
    case 25: return (u32)OFY;
    case 26: return (u32)(s32)(s16)H;
    case 27: return (u32)(s32)DQA;
    case 28: return (u32)DQB;
    case 29: return (u32)(s32)ZSF3;
    case 30: return (u32)(s32)ZSF4;
    case 31: return FLAG;
    }
    return 0;
}

void gte_write_ctrl(int reg, u32 v)
{
    switch (reg) {
    case 0: RT[0] = (s16)v; RT[1] = (s16)(v >> 16); break;
    case 1: RT[2] = (s16)v; RT[3] = (s16)(v >> 16); break;
    case 2: RT[4] = (s16)v; RT[5] = (s16)(v >> 16); break;
    case 3: RT[6] = (s16)v; RT[7] = (s16)(v >> 16); break;
    case 4: RT[8] = (s16)v; break;
    case 5: TR[0] = (s32)v; break;
    case 6: TR[1] = (s32)v; break;
    case 7: TR[2] = (s32)v; break;
    case 8: LLM[0] = (s16)v; LLM[1] = (s16)(v >> 16); break;
    case 9: LLM[2] = (s16)v; LLM[3] = (s16)(v >> 16); break;
    case 10: LLM[4] = (s16)v; LLM[5] = (s16)(v >> 16); break;
    case 11: LLM[6] = (s16)v; LLM[7] = (s16)(v >> 16); break;
    case 12: LLM[8] = (s16)v; break;
    case 13: BK[0] = (s32)v; break;
    case 14: BK[1] = (s32)v; break;
    case 15: BK[2] = (s32)v; break;
    case 16: LCM[0] = (s16)v; LCM[1] = (s16)(v >> 16); break;
    case 17: LCM[2] = (s16)v; LCM[3] = (s16)(v >> 16); break;
    case 18: LCM[4] = (s16)v; LCM[5] = (s16)(v >> 16); break;
    case 19: LCM[6] = (s16)v; LCM[7] = (s16)(v >> 16); break;
    case 20: LCM[8] = (s16)v; break;
    case 21: FC[0] = (s32)v; break;
    case 22: FC[1] = (s32)v; break;
    case 23: FC[2] = (s32)v; break;
    case 24: OFX = (s32)v; break;
    case 25: OFY = (s32)v; break;
    case 26: H = (u16)v; break;
    case 27: DQA = (s16)v; break;
    case 28: DQB = (s32)v; break;
    case 29: ZSF3 = (s16)v; break;
    case 30: ZSF4 = (s16)v; break;
    case 31: FLAG = v & 0x7FFFF000u; if (FLAG & 0x7F87E000u) FLAG |= 0x80000000u; break;
    }
}
