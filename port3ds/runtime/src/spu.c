#include "rt.h"

/* port of RecompOne.Runtime.Spu + XaAudio */

#define SPU_RAM 0x80000u
static u8 s_ram[SPU_RAM];

static const int K0[5] = { 0, 60, 115, 98, 122 };
static const int K1[5] = { 0, 0, -52, -55, -60 };
static const int STEP_DOWN[4] = { -8, -7, -6, -5 };
static const int STEP_UP[4] = { 7, 6, 5, 4 };

static const s16 GAUSS[512] = {
    -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001,
    -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001,
    0x0001, 0x0001, 0x0001, 0x0002, 0x0002, 0x0002, 0x0003, 0x0003,
    0x0003, 0x0004, 0x0004, 0x0005, 0x0005, 0x0006, 0x0007, 0x0007,
    0x0008, 0x0009, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E,
    0x000F, 0x0010, 0x0011, 0x0012, 0x0013, 0x0015, 0x0016, 0x0018,
    0x0019, 0x001B, 0x001C, 0x001E, 0x0020, 0x0021, 0x0023, 0x0025,
    0x0027, 0x0029, 0x002C, 0x002E, 0x0030, 0x0033, 0x0035, 0x0038,
    0x003A, 0x003D, 0x0040, 0x0043, 0x0046, 0x0049, 0x004D, 0x0050,
    0x0054, 0x0057, 0x005B, 0x005F, 0x0063, 0x0067, 0x006B, 0x006F,
    0x0074, 0x0078, 0x007D, 0x0082, 0x0087, 0x008C, 0x0091, 0x0096,
    0x009C, 0x00A1, 0x00A7, 0x00AD, 0x00B3, 0x00BA, 0x00C0, 0x00C7,
    0x00CD, 0x00D4, 0x00DB, 0x00E3, 0x00EA, 0x00F2, 0x00FA, 0x0101,
    0x010A, 0x0112, 0x011B, 0x0123, 0x012C, 0x0135, 0x013F, 0x0148,
    0x0152, 0x015C, 0x0166, 0x0171, 0x017B, 0x0186, 0x0191, 0x019C,
    0x01A8, 0x01B4, 0x01C0, 0x01CC, 0x01D9, 0x01E5, 0x01F2, 0x0200,
    0x020D, 0x021B, 0x0229, 0x0237, 0x0246, 0x0255, 0x0264, 0x0273,
    0x0283, 0x0293, 0x02A3, 0x02B4, 0x02C4, 0x02D6, 0x02E7, 0x02F9,
    0x030B, 0x031D, 0x0330, 0x0343, 0x0356, 0x036A, 0x037E, 0x0392,
    0x03A7, 0x03BC, 0x03D1, 0x03E7, 0x03FC, 0x0413, 0x042A, 0x0441,
    0x0458, 0x0470, 0x0488, 0x04A0, 0x04B9, 0x04D2, 0x04EC, 0x0506,
    0x0520, 0x053B, 0x0556, 0x0572, 0x058E, 0x05AA, 0x05C7, 0x05E4,
    0x0601, 0x061F, 0x063E, 0x065C, 0x067C, 0x069B, 0x06BB, 0x06DC,
    0x06FD, 0x071E, 0x0740, 0x0762, 0x0784, 0x07A7, 0x07CB, 0x07EF,
    0x0813, 0x0838, 0x085D, 0x0883, 0x08A9, 0x08D0, 0x08F7, 0x091E,
    0x0946, 0x096F, 0x0998, 0x09C1, 0x09EB, 0x0A16, 0x0A40, 0x0A6C,
    0x0A98, 0x0AC4, 0x0AF1, 0x0B1E, 0x0B4C, 0x0B7A, 0x0BA9, 0x0BD8,
    0x0C07, 0x0C38, 0x0C68, 0x0C99, 0x0CCB, 0x0CFD, 0x0D30, 0x0D63,
    0x0D97, 0x0DCB, 0x0E00, 0x0E35, 0x0E6B, 0x0EA1, 0x0ED7, 0x0F0F,
    0x0F46, 0x0F7F, 0x0FB7, 0x0FF1, 0x102A, 0x1065, 0x109F, 0x10DB,
    0x1116, 0x1153, 0x118F, 0x11CD, 0x120B, 0x1249, 0x1288, 0x12C7,
    0x1307, 0x1347, 0x1388, 0x13C9, 0x140B, 0x144D, 0x1490, 0x14D4,
    0x1517, 0x155C, 0x15A0, 0x15E6, 0x162C, 0x1672, 0x16B9, 0x1700,
    0x1747, 0x1790, 0x17D8, 0x1821, 0x186B, 0x18B5, 0x1900, 0x194B,
    0x1996, 0x19E2, 0x1A2E, 0x1A7B, 0x1AC8, 0x1B16, 0x1B64, 0x1BB3,
    0x1C02, 0x1C51, 0x1CA1, 0x1CF1, 0x1D42, 0x1D93, 0x1DE5, 0x1E37,
    0x1E89, 0x1EDC, 0x1F2F, 0x1F82, 0x1FD6, 0x202A, 0x207F, 0x20D4,
    0x2129, 0x217F, 0x21D5, 0x222C, 0x2282, 0x22DA, 0x2331, 0x2389,
    0x23E1, 0x2439, 0x2492, 0x24EB, 0x2545, 0x259E, 0x25F8, 0x2653,
    0x26AD, 0x2708, 0x2763, 0x27BE, 0x281A, 0x2876, 0x28D2, 0x292E,
    0x298B, 0x29E7, 0x2A44, 0x2AA1, 0x2AFF, 0x2B5C, 0x2BBA, 0x2C18,
    0x2C76, 0x2CD4, 0x2D33, 0x2D91, 0x2DF0, 0x2E4F, 0x2EAE, 0x2F0D,
    0x2F6C, 0x2FCC, 0x302B, 0x308B, 0x30EA, 0x314A, 0x31AA, 0x3209,
    0x3269, 0x32C9, 0x3329, 0x3389, 0x33E9, 0x3449, 0x34A9, 0x3509,
    0x3569, 0x35C9, 0x3629, 0x3689, 0x36E8, 0x3748, 0x37A8, 0x3807,
    0x3867, 0x38C6, 0x3926, 0x3985, 0x39E4, 0x3A43, 0x3AA2, 0x3B00,
    0x3B5F, 0x3BBD, 0x3C1B, 0x3C79, 0x3CD7, 0x3D35, 0x3D92, 0x3DEF,
    0x3E4C, 0x3EA9, 0x3F05, 0x3F62, 0x3FBD, 0x4019, 0x4074, 0x40D0,
    0x412A, 0x4185, 0x41DF, 0x4239, 0x4292, 0x42EB, 0x4344, 0x439C,
    0x43F4, 0x444C, 0x44A3, 0x44FA, 0x4550, 0x45A6, 0x45FC, 0x4651,
    0x46A6, 0x46FA, 0x474E, 0x47A1, 0x47F4, 0x4846, 0x4898, 0x48E9,
    0x493A, 0x498A, 0x49D9, 0x4A29, 0x4A77, 0x4AC5, 0x4B13, 0x4B5F,
    0x4BAC, 0x4BF7, 0x4C42, 0x4C8D, 0x4CD7, 0x4D20, 0x4D68, 0x4DB0,
    0x4DF7, 0x4E3E, 0x4E84, 0x4EC9, 0x4F0E, 0x4F52, 0x4F95, 0x4FD7,
    0x5019, 0x505A, 0x509A, 0x50DA, 0x5118, 0x5156, 0x5194, 0x51D0,
    0x520C, 0x5247, 0x5281, 0x52BA, 0x52F3, 0x532A, 0x5361, 0x5397,
    0x53CC, 0x5401, 0x5434, 0x5467, 0x5499, 0x54CA, 0x54FA, 0x5529,
    0x5558, 0x5585, 0x55B2, 0x55DE, 0x5609, 0x5632, 0x565B, 0x5684,
    0x56AB, 0x56D1, 0x56F6, 0x571B, 0x573E, 0x5761, 0x5782, 0x57A3,
    0x57C3, 0x57E2, 0x57FF, 0x581C, 0x5838, 0x5853, 0x586D, 0x5886,
    0x589E, 0x58B5, 0x58CB, 0x58E0, 0x58F4, 0x5907, 0x5919, 0x592A,
    0x593A, 0x5949, 0x5958, 0x5965, 0x5971, 0x597C, 0x5986, 0x598F,
    0x5997, 0x599E, 0x59A4, 0x59A9, 0x59AD, 0x59B0, 0x59B2, 0x59B3,
};

enum { PH_OFF, PH_ATTACK, PH_DECAY, PH_SUSTAIN, PH_RELEASE };

typedef struct {
    u16 vol_l, vol_r, pitch, start, repeat, adsr_lo, adsr_hi;
    s16 adsr_vol;
    u32 cur, counter;
    int phase, adsr_cyc;
    int endx, ignore_loop, has_block;
    int old, older;
    s16 cur_l, cur_r;
    int cyc_l, cyc_r;
    s16 buf[31];
} Voice;

static Voice v_[24];
static u16 main_l, main_r;
static s16 main_cur_l, main_cur_r;
static int main_cyc_l, main_cyc_r;
static u16 rev_l, rev_r, kon_lo, kon_hi, koff_lo, koff_hi, pmon_lo, pmon_hi, non_lo, non_hi, eon_lo, eon_hi;
static u32 endx;
static u16 spucnt, xfer_addr, xfer_ctrl = 4, cd_l, cd_r, ext_l, ext_r, rev_start;
static int mix_ll = 0x80, mix_lr, mix_rl, mix_rr = 0x80;
static u32 kon_pend, koff_pend;
static int noise_level = 1, noise_timer;

void spu_init(void)
{
    memset(s_ram, 0, sizeof s_ram);
    memset(v_, 0, sizeof v_);
    xfer_ctrl = 4;
    mix_ll = 0x80; mix_lr = 0; mix_rl = 0; mix_rr = 0x80;
}

void spu_set_cd_mix(int ll, int lr, int rr, int rl)
{
    spu_lock();
    mix_ll = ll; mix_lr = lr; mix_rr = rr; mix_rl = rl;
    spu_unlock();
}

u16 spu_read16(u32 phys)
{
    u32 off = phys - 0x1F801C00u;
    u16 r = 0;
    spu_lock();
    if (off < 0x180u) {
        int n = (int)(off >> 4);
        Voice *v = &v_[n];
        switch (off & 0xF) {
        case 0x0: r = v->vol_l; break;
        case 0x2: r = v->vol_r; break;
        case 0x4: r = v->pitch; break;
        case 0x6: r = v->start; break;
        case 0x8: r = v->adsr_lo; break;
        case 0xA: r = v->adsr_hi; break;
        case 0xC: r = ((kon_pend & (1u << n)) || (v->phase == PH_ATTACK && v->adsr_vol == 0)) ? 1 : (u16)v->adsr_vol; break;
        case 0xE: r = v->repeat; break;
        }
    } else {
        switch (off) {
        case 0x180: r = main_l; break;
        case 0x182: r = main_r; break;
        case 0x184: r = rev_l; break;
        case 0x186: r = rev_r; break;
        case 0x188: r = kon_lo; break;
        case 0x18A: r = kon_hi; break;
        case 0x18C: r = koff_lo; break;
        case 0x18E: r = koff_hi; break;
        case 0x190: r = pmon_lo; break;
        case 0x192: r = pmon_hi; break;
        case 0x194: r = non_lo; break;
        case 0x196: r = non_hi; break;
        case 0x198: r = eon_lo; break;
        case 0x19A: r = eon_hi; break;
        case 0x19C: r = (u16)endx; break;
        case 0x19E: r = (u16)(endx >> 16); break;
        case 0x1A2: r = rev_start; break;
        case 0x1A6: r = xfer_addr; break;
        case 0x1AA: r = spucnt; break;
        case 0x1AC: r = xfer_ctrl; break;
        case 0x1AE: r = spucnt & 0x3F; break;
        case 0x1B0: r = cd_l; break;
        case 0x1B2: r = cd_r; break;
        case 0x1B4: r = ext_l; break;
        case 0x1B6: r = ext_r; break;
        }
    }
    spu_unlock();
    return r;
}

static void key_on(u16 mask, int hi)
{
    u32 bits = (u32)mask << (hi ? 16 : 0);
    kon_pend |= bits;
    koff_pend &= ~bits;
    endx &= ~bits;
}

static void key_off(u16 mask, int hi)
{
    u32 bits = (u32)mask << (hi ? 16 : 0);
    koff_pend |= bits & ~kon_pend;
}

void spu_write16(u32 phys, u16 val)
{
    u32 off = phys - 0x1F801C00u;
    spu_lock();
    if (off < 0x180u) {
        Voice *v = &v_[off >> 4];
        switch (off & 0xF) {
        case 0x0: v->vol_l = val; break;
        case 0x2: v->vol_r = val; break;
        case 0x4: v->pitch = val; break;
        case 0x6: v->start = val; break;
        case 0x8: v->adsr_lo = val; break;
        case 0xA: v->adsr_hi = val; break;
        case 0xC: v->adsr_vol = (s16)val; break;
        case 0xE: v->repeat = val; v->ignore_loop = 1; break;
        }
    } else {
        switch (off) {
        case 0x180: main_l = val; break;
        case 0x182: main_r = val; break;
        case 0x184: rev_l = val; break;
        case 0x186: rev_r = val; break;
        case 0x188: key_on(val, 0); kon_lo = val; break;
        case 0x18A: key_on(val, 1); kon_hi = val; break;
        case 0x18C: key_off(val, 0); koff_lo = val; break;
        case 0x18E: key_off(val, 1); koff_hi = val; break;
        case 0x190: pmon_lo = val; break;
        case 0x192: pmon_hi = val; break;
        case 0x194: non_lo = val; break;
        case 0x196: non_hi = val; break;
        case 0x198: eon_lo = val; break;
        case 0x19A: eon_hi = val; break;
        case 0x1A2: rev_start = val; break;
        case 0x1A6: xfer_addr = val; break;
        case 0x1AA: spucnt = val; break;
        case 0x1AC: xfer_ctrl = val; break;
        case 0x1B0: cd_l = val; break;
        case 0x1B2: cd_r = val; break;
        case 0x1B4: ext_l = val; break;
        case 0x1B6: ext_r = val; break;
        }
    }
    spu_unlock();
}

void spu_dma_write(const u8 *data, u32 len)
{
    spu_lock();
    u32 a = (u32)xfer_addr << 3;
    for (u32 i = 0; i < len; i++) s_ram[(a + i) & (SPU_RAM - 1)] = data[i];
    spu_unlock();
}

void spu_dma_read(u8 *data, u32 len)
{
    spu_lock();
    u32 a = (u32)xfer_addr << 3;
    for (u32 i = 0; i < len; i++) data[i] = s_ram[(a + i) & (SPU_RAM - 1)];
    spu_unlock();
}

static void resolve_keys(void)
{
    if (!kon_pend && !koff_pend) return;
    for (int i = 0; i < 24; i++) {
        u32 bit = 1u << i;
        Voice *v = &v_[i];
        if (kon_pend & bit) {
            v->phase = PH_ATTACK;
            v->adsr_vol = 0;
            v->adsr_cyc = 0;
            v->cur = (u32)v->start << 3;
            v->ignore_loop = 0;
            v->counter = 0;
            v->old = v->older = 0;
            v->has_block = 0;
            v->endx = 0;
            endx &= ~bit;
        } else if (koff_pend & bit) {
            if (v->phase != PH_OFF) v->phase = PH_RELEASE;
        }
    }
    kon_pend = koff_pend = 0;
}

static inline void env_tick(s16 *level, int *cyc, int shift, int step_idx, int exp, int dec, int neg)
{
    int mag = neg ? -*level : *level;
    int cycles = 1 << (shift - 11 > 0 ? shift - 11 : 0);
    int step = (dec ? STEP_DOWN : STEP_UP)[step_idx] << (11 - shift > 0 ? 11 - shift : 0);
    if (exp && !dec && mag > 0x6000) cycles *= 4;
    if (exp && dec) { step = step * mag / 0x8000; if (!step) step = -1; }
    if (++*cyc < cycles) return;
    *cyc = 0;
    int next = mag + step;
    if (next < 0) next = 0;
    if (next > 0x7FFF) next = 0x7FFF;
    *level = (s16)(neg ? -next : next);
}

static inline void sweep_tick(u16 reg, s16 *level, int *cyc)
{
    if (!(reg & 0x8000)) { *level = (s16)(reg << 1); return; }
    env_tick(level, cyc, (reg >> 2) & 0x1F, reg & 3, (reg & 0x4000) != 0, (reg & 0x2000) != 0, (reg & 0x1000) != 0);
}

static void decode_block(Voice *v, int index)
{
    v->buf[0] = v->buf[28];
    v->buf[1] = v->buf[29];
    v->buf[2] = v->buf[30];
    u32 addr = v->cur & (SPU_RAM - 1);
    u8 hdr = s_ram[addr], flags = s_ram[(addr + 1) & (SPU_RAM - 1)];
    int shift = hdr & 0xF;
    if (shift > 12) shift = 9;
    int filter = (hdr >> 4) & 7;
    if (filter > 4) filter = 4;
    int k0 = K0[filter], k1 = K1[filter];
    int old = v->old, older = v->older;
    for (int i = 0; i < 14; i++) {
        u8 b = s_ram[(addr + 2 + i) & (SPU_RAM - 1)];
        for (int h = 0; h < 2; h++) {
            int nib = h ? b >> 4 : b & 0xF;
            int s = (s32)((u32)nib << 28) >> 28;
            s = (s << 12) >> shift;
            s += (old * k0 + older * k1) >> 6;
            if (s < -32768) s = -32768;
            if (s > 32767) s = 32767;
            older = old;
            old = s;
            v->buf[3 + i * 2 + h] = (s16)s;
        }
    }
    v->old = old;
    v->older = older;
    if ((flags & 4) && !v->ignore_loop) v->repeat = (u16)(addr >> 3);
    v->cur += 16;
    if (flags & 1) {
        v->endx = 1;
        endx |= 1u << index;
        v->cur = (u32)v->repeat << 3;
        if (!(flags & 2)) { v->adsr_vol = 0; v->phase = PH_RELEASE; }
    }
}

static void tick_adsr(Voice *v)
{
    int lo = v->adsr_lo, hi = v->adsr_hi;
    int atk_exp = (lo >> 15) & 1, atk_shift = (lo >> 10) & 0x1F, atk_step = (lo >> 8) & 3;
    int dec_shift = (lo >> 4) & 0xF, sus_lvl = ((lo & 0xF) + 1) << 11;
    int sus_exp = (hi >> 15) & 1, sus_dec = (hi >> 14) & 1, sus_shift = (hi >> 8) & 0x1F, sus_step = (hi >> 6) & 3;
    int rel_exp = (hi >> 5) & 1, rel_shift = hi & 0x1F;

    if (v->phase == PH_ATTACK && v->adsr_vol >= 0x7FFF) { v->phase = PH_DECAY; v->adsr_cyc = 0; }
    if (v->phase == PH_DECAY && v->adsr_vol <= sus_lvl) { v->phase = PH_SUSTAIN; v->adsr_cyc = 0; }
    switch (v->phase) {
    case PH_ATTACK: env_tick(&v->adsr_vol, &v->adsr_cyc, atk_shift, atk_step, atk_exp, 0, 0); break;
    case PH_DECAY: env_tick(&v->adsr_vol, &v->adsr_cyc, dec_shift, 0, 1, 1, 0); break;
    case PH_SUSTAIN: env_tick(&v->adsr_vol, &v->adsr_cyc, sus_shift, sus_step, sus_exp, sus_dec, 0); break;
    case PH_RELEASE: env_tick(&v->adsr_vol, &v->adsr_cyc, rel_shift, 0, rel_exp, 1, 0); break;
    }
    if (v->phase == PH_RELEASE && v->adsr_vol == 0) v->phase = PH_OFF;
}

static void tick_noise(void)
{
    int shift = (spucnt >> 10) & 0xF, step = ((spucnt >> 8) & 3) + 4;
    noise_timer -= step;
    int parity = ((noise_level >> 15) ^ (noise_level >> 12) ^ (noise_level >> 11) ^ (noise_level >> 10) ^ 1) & 1;
    if (noise_timer < 0) {
        noise_level = ((noise_level << 1) | parity) & 0xFFFF;
        noise_timer += 0x20000 >> shift;
        if (noise_timer < 0) noise_timer += 0x20000 >> shift;
    }
}

static void tick(int *outl, int *outr)
{
    resolve_keys();
    tick_noise();
    int sl = 0, sr = 0, prev = 0;
    u32 nonm = non_lo | ((u32)non_hi << 16), pmonm = pmon_lo | ((u32)pmon_hi << 16);
    for (int i = 0; i < 24; i++) {
        Voice *v = &v_[i];
        if (v->phase == PH_OFF) { prev = 0; continue; }
        tick_adsr(v);
        sweep_tick(v->vol_l, &v->cur_l, &v->cyc_l);
        sweep_tick(v->vol_r, &v->cur_r, &v->cyc_r);
        if (!v->has_block) {
            memset(v->buf, 0, sizeof v->buf);
            decode_block(v, i);
            v->has_block = 1;
        }
        int sample;
        if (nonm & (1u << i)) sample = (s16)noise_level;
        else {
            int idx = (int)(v->counter >> 12), fi = (int)((v->counter >> 4) & 0xFF);
            sample = ((GAUSS[0x0FF - fi] * v->buf[idx]) >> 15) + ((GAUSS[0x1FF - fi] * v->buf[idx + 1]) >> 15) +
                     ((GAUSS[0x100 + fi] * v->buf[idx + 2]) >> 15) + ((GAUSS[fi] * v->buf[idx + 3]) >> 15);
        }
        int amp = (sample * v->adsr_vol) >> 15;
        int step = v->pitch;
        if (i > 0 && (pmonm & (1u << i))) {
            int f = prev < -0x8000 ? -0x8000 : prev > 0x7FFF ? 0x7FFF : prev;
            f += 0x8000;
            step = (((s16)(u16)step * f) >> 15) & 0xFFFF;
        }
        if (step > 0x3FFF) step = 0x4000;
        v->counter += (u32)step;
        if ((v->counter >> 12) >= 28) {
            v->counter -= 28u << 12;
            decode_block(v, i);
        }
        prev = amp;
        sl += (amp * v->cur_l) >> 15;
        sr += (amp * v->cur_r) >> 15;
    }
    *outl = sl < -32768 ? -32768 : sl > 32767 ? 32767 : sl;
    *outr = sr < -32768 ? -32768 : sr > 32767 ? 32767 : sr;
}

static inline int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

/* ADSR with the per-phase parameters decoded once (identical to tick_adsr +
   env_tick, which decode the registers on every sample) */
typedef struct { int cycles, step, exp, dec, phase; } EnvP;

static void env_params(const Voice *v, EnvP *e)
{
    int lo = v->adsr_lo, hi = v->adsr_hi, shift, step_idx;
    e->phase = v->phase;
    switch (v->phase) {
    case PH_ATTACK: shift = (lo >> 10) & 0x1F; step_idx = (lo >> 8) & 3; e->exp = (lo >> 15) & 1; e->dec = 0; break;
    case PH_DECAY: shift = (lo >> 4) & 0xF; step_idx = 0; e->exp = 1; e->dec = 1; break;
    case PH_SUSTAIN: shift = (hi >> 8) & 0x1F; step_idx = (hi >> 6) & 3; e->exp = (hi >> 15) & 1; e->dec = (hi >> 14) & 1; break;
    default: shift = hi & 0x1F; step_idx = 0; e->exp = (hi >> 5) & 1; e->dec = 1; break;
    }
    e->cycles = 1 << (shift - 11 > 0 ? shift - 11 : 0);
    e->step = (e->dec ? STEP_DOWN : STEP_UP)[step_idx] << (11 - shift > 0 ? 11 - shift : 0);
}

static inline void adsr_fast(Voice *v, EnvP *e)
{
    if (v->phase == PH_ATTACK && v->adsr_vol >= 0x7FFF) { v->phase = PH_DECAY; v->adsr_cyc = 0; }
    if (v->phase == PH_DECAY && v->adsr_vol <= (((v->adsr_lo & 0xF) + 1) << 11)) { v->phase = PH_SUSTAIN; v->adsr_cyc = 0; }
    if (v->phase != e->phase) env_params(v, e);
    if (v->phase != PH_OFF) {
        int mag = v->adsr_vol;   /* envelopes are never negative */
        int cycles = e->cycles;
        if (e->exp && !e->dec && mag > 0x6000) cycles *= 4;
        if (++v->adsr_cyc >= cycles) {
            v->adsr_cyc = 0;
            int step = e->step;
            if (e->exp && e->dec) { step = step * mag / 0x8000; if (!step) step = -1; }
            int next = mag + step;
            if (next < 0) next = 0;
            if (next > 0x7FFF) next = 0x7FFF;
            v->adsr_vol = (s16)next;
        }
    }
    if (v->phase == PH_RELEASE && v->adsr_vol == 0) v->phase = PH_OFF;
}

#define MIX_BLOCK 256

/* one voice over a block of samples (no pitch modulation involved).
   The envelope only moves every `cycles` samples; in between, a tick just
   counts (adsr_cyc++). Those stretches run in a tight loop with the volume
   held, which gives the same output as ticking every sample. */
static void mix_voice(Voice *v, int i, int frames, const s16 *noise, int *accl, int *accr)
{
    EnvP e;
    env_params(v, &e);
    int is_noise = ((non_lo | ((u32)non_hi << 16)) >> i) & 1;
    int fixl = !(v->vol_l & 0x8000), fixr = !(v->vol_r & 0x8000);
    int step = v->pitch;
    if (step > 0x3FFF) step = 0x4000;
    int n = 0;
    while (n < frames) {
        if (v->phase == PH_OFF) break;
        adsr_fast(v, &e);
        if (fixl) v->cur_l = (s16)(v->vol_l << 1); else sweep_tick(v->vol_l, &v->cur_l, &v->cyc_l);
        if (fixr) v->cur_r = (s16)(v->vol_r << 1); else sweep_tick(v->vol_r, &v->cur_r, &v->cyc_r);
        if (!v->has_block) {
            memset(v->buf, 0, sizeof v->buf);
            decode_block(v, i);
            v->has_block = 1;
        }
        int sample;
        if (is_noise) sample = noise[n];
        else {
            int idx = (int)(v->counter >> 12), fi = (int)((v->counter >> 4) & 0xFF);
            const s16 *bp = v->buf + idx;
            sample = ((GAUSS[0x0FF - fi] * bp[0]) >> 15) + ((GAUSS[0x1FF - fi] * bp[1]) >> 15) +
                     ((GAUSS[0x100 + fi] * bp[2]) >> 15) + ((GAUSS[fi] * bp[3]) >> 15);
        }
        int amp = (sample * v->adsr_vol) >> 15;
        v->counter += (u32)step;
        if ((v->counter >> 12) >= 28) {
            v->counter -= 28u << 12;
            decode_block(v, i);
        }
        accl[n] += (amp * v->cur_l) >> 15;
        accr[n] += (amp * v->cur_r) >> 15;
        n++;

        /* quiet stretch: the next ticks only count if no phase change is due */
        if (!fixl || !fixr || is_noise || v->phase == PH_OFF || v->phase != e.phase) continue;
        int vol = v->adsr_vol;
        if (v->phase == PH_ATTACK && vol >= 0x7FFF) continue;
        if (v->phase == PH_DECAY && vol <= (((v->adsr_lo & 0xF) + 1) << 11)) continue;
        if (v->phase == PH_RELEASE && vol == 0) continue;
        int cycles = e.cycles;
        if (e.exp && !e.dec && vol > 0x6000) cycles *= 4;
        int k = cycles - 1 - v->adsr_cyc;
        if (k <= 0) continue;
        if (k > frames - n) k = frames - n;
        int phase = v->phase, cl = v->cur_l, cr = v->cur_r;
        u32 counter = v->counter;
        v->adsr_cyc += k;   /* undone below for the samples not run */
        int j = 0;
        while (j < k) {
            int idx = (int)(counter >> 12), fi = (int)((counter >> 4) & 0xFF);
            const s16 *bp = v->buf + idx;
            int smp = ((GAUSS[0x0FF - fi] * bp[0]) >> 15) + ((GAUSS[0x1FF - fi] * bp[1]) >> 15) +
                      ((GAUSS[0x100 + fi] * bp[2]) >> 15) + ((GAUSS[fi] * bp[3]) >> 15);
            int am = (smp * vol) >> 15;
            counter += (u32)step;
            accl[n] += (am * cl) >> 15;
            accr[n] += (am * cr) >> 15;
            n++;
            j++;
            if ((counter >> 12) >= 28) {
                counter -= 28u << 12;
                v->counter = counter;
                decode_block(v, i);
                counter = v->counter;
                /* an end-of-sample block may stop the voice: leave the stretch */
                if (v->phase != phase || v->adsr_vol != vol) break;
            }
        }
        v->adsr_cyc -= k - j;
        v->counter = counter;
    }
}

static void mix_block(s16 *dst, int frames)
{
    int accl[MIX_BLOCK], accr[MIX_BLOCK];
    s16 noise[MIX_BLOCK];
    u32 pmonm = pmon_lo | ((u32)pmon_hi << 16), on = 0;
    /* the game can't touch the registers while we hold the lock, so the keys
       resolved at the first sample hold for the whole block */
    resolve_keys();
    for (int i = 0; i < 24; i++) if (v_[i].phase != PH_OFF) on |= 1u << i;
    if (pmonm & on & ~1u) {
        /* pitch modulation chains voices sample by sample: use the reference path */
        for (int n = 0; n < frames; n++) {
            sweep_tick(main_l, &main_cur_l, &main_cyc_l);
            sweep_tick(main_r, &main_cur_r, &main_cyc_r);
            tick(&accl[n], &accr[n]);
            int l = accl[n], r = accr[n];
            s16 xl, xr;
            if (xa_next(&xl, &xr)) {
                int al = clamp16((xl * mix_ll + xr * mix_rl) >> 7);
                int ar = clamp16((xl * mix_lr + xr * mix_rr) >> 7);
                l += (al * (s16)cd_l) >> 15;
                r += (ar * (s16)cd_r) >> 15;
            }
            dst[n * 2] = (s16)((clamp16(l) * main_cur_l) >> 15);
            dst[n * 2 + 1] = (s16)((clamp16(r) * main_cur_r) >> 15);
        }
        return;
    }
    for (int n = 0; n < frames; n++) { tick_noise(); noise[n] = (s16)noise_level; }
    memset(accl, 0, frames * sizeof(int));
    memset(accr, 0, frames * sizeof(int));
    for (int i = 0; i < 24; i++)
        if (on & (1u << i)) mix_voice(&v_[i], i, frames, noise, accl, accr);
    for (int n = 0; n < frames; n++) {
        sweep_tick(main_l, &main_cur_l, &main_cyc_l);
        sweep_tick(main_r, &main_cur_r, &main_cyc_r);
        int l = clamp16(accl[n]), r = clamp16(accr[n]);
        s16 xl, xr;
        if (xa_next(&xl, &xr)) {
            int al = clamp16((xl * mix_ll + xr * mix_rl) >> 7);
            int ar = clamp16((xl * mix_lr + xr * mix_rr) >> 7);
            l += (al * (s16)cd_l) >> 15;
            r += (ar * (s16)cd_r) >> 15;
        }
        dst[n * 2] = (s16)((clamp16(l) * main_cur_l) >> 15);
        dst[n * 2 + 1] = (s16)((clamp16(r) * main_cur_r) >> 15);
    }
}

void spu_mix(s16 *dst, int frames)
{
    spu_lock();
    for (int n = 0; n < frames; n += MIX_BLOCK)
        mix_block(dst + n * 2, frames - n < MIX_BLOCK ? frames - n : MIX_BLOCK);
    spu_unlock();
}

/* ---------------- XA ---------------- */
#define XA_CAP (1 << 16)
static u32 xa_ring[XA_CAP];
static int xa_w, xa_r, xa_count;
static int xa_old_l, xa_older_l, xa_old_r, xa_older_r;
static int xa_rate = 37800, xa_playing;
static u32 xa_step = (u32)((37800ull << 16) / 44100);   /* xa_rate in 16.16 output samples */
static u32 xa_pos;   /* 16.16 */
static s16 xa_s0l, xa_s0r, xa_s1l, xa_s1r;
static int xa_underrun;

void xa_reset(void)
{
    spu_lock();
    xa_old_l = xa_older_l = xa_old_r = xa_older_r = 0;
    xa_w = xa_r = xa_count = 0;
    xa_playing = 0;
    xa_pos = 0;
    xa_s0l = xa_s0r = xa_s1l = xa_s1r = 0;
    xa_underrun = 0;
    spu_unlock();
}

static const int XPOS[4] = { 0, 60, 115, 98 };
static const int XNEG[4] = { 0, 0, -52, -55 };

static void xa_block(const u8 *sec, int b, int blk, int *old, int *older, int *dst)
{
    u8 hdr = sec[b + 4 + blk];
    int sv = hdr & 0xF;
    if (sv > 12) sv = 9;
    int filter = (hdr >> 4) & 3;
    int f0 = XPOS[filter], f1 = XNEG[filter];
    int col = blk >> 1, nshift = (blk & 1) * 4;
    for (int j = 0; j < 28; j++) {
        int nib = (sec[b + 16 + 4 * j + col] >> nshift) & 0xF;
        int t = nib >= 8 ? nib - 16 : nib;
        int s = clamp16(((t << 12) >> sv) + ((*old * f0 + *older * f1 + 32) >> 6));
        *older = *old;
        *old = s;
        dst[j] = s;
    }
}

void xa_decode_sector(const u8 *sec, int off, u8 coding)
{
    int stereo = coding & 1;
    int rate = (coding & 4) ? 18900 : 37800;
    static u32 frames[4032];
    int l[28], r[28], n = 0;
    for (int p = 0; p < 18; p++) {
        int b = off + p * 128;
        if (stereo) {
            for (int tb = 0; tb < 4; tb++) {
                xa_block(sec, b, tb * 2, &xa_old_l, &xa_older_l, l);
                xa_block(sec, b, tb * 2 + 1, &xa_old_r, &xa_older_r, r);
                for (int j = 0; j < 28; j++) frames[n++] = (u16)l[j] | ((u32)(u16)r[j] << 16);
            }
        } else {
            for (int blk = 0; blk < 8; blk++) {
                xa_block(sec, b, blk, &xa_old_l, &xa_older_l, l);
                for (int j = 0; j < 28; j++) frames[n++] = (u16)l[j] | ((u32)(u16)l[j] << 16);
            }
        }
    }
    spu_lock();
    xa_rate = rate;
    xa_step = (u32)(((u64)rate << 16) / 44100);
    for (int i = 0; i < n; i++) {
        xa_ring[xa_w] = frames[i];
        xa_w = (xa_w + 1) & (XA_CAP - 1);
        if (xa_count < XA_CAP) xa_count++;
        else xa_r = (xa_r + 1) & (XA_CAP - 1);
    }
    if (!xa_playing && xa_count >= 1024) xa_playing = 1;
    spu_unlock();
}

int xa_buffered(void)
{
    spu_lock();
    int n = xa_count;
    spu_unlock();
    return n;
}

/* called with the spu lock held (from spu_mix) */
int xa_next(s16 *left, s16 *right)
{
    if (!xa_playing) { *left = *right = 0; return 0; }
    while (xa_pos >= 0x10000) {
        xa_s0l = xa_s1l;
        xa_s0r = xa_s1r;
        if (xa_count > 0) {
            u32 p = xa_ring[xa_r];
            xa_r = (xa_r + 1) & (XA_CAP - 1);
            xa_count--;
            xa_s1l = (s16)(p & 0xFFFF);
            xa_s1r = (s16)(p >> 16);
            xa_underrun = 0;
        } else {
            if (++xa_underrun > 8192) { xa_playing = 0; *left = *right = 0; return 0; }
            xa_s1l = (s16)(xa_s1l * 31 / 32);
            xa_s1r = (s16)(xa_s1r * 31 / 32);
        }
        xa_pos -= 0x10000;
    }
    int f = (int)(xa_pos >> 4);   /* 12 bit fraction */
    *left = (s16)(xa_s0l + (((xa_s1l - xa_s0l) * f) >> 12));
    *right = (s16)(xa_s0r + (((xa_s1r - xa_s0r) * f) >> 12));
    xa_pos += xa_step;
    return 1;
}
