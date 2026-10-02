#include "rt.h"

u8 g_ram[RAM_SIZE] __attribute__((aligned(4096)));
u8 g_scratch[0x400] __attribute__((aligned(16)));
#define s_scratch g_scratch
static u8 s_hw[0x2000] __attribute__((aligned(16)));
static u8 s_bios[0x80000] __attribute__((aligned(16)));

#define SCRATCH_BASE 0x1F800000u
#define HW_BASE 0x1F801000u
#define BIOS_BASE 0x1FC00000u

extern const u8 g_font1[];
extern const u32 g_font1_size;
extern const u8 g_font2[];
extern const u32 g_font2_size;

/* keep the compiler from folding "array - 0x1F800000" into one literal: the
   3dsx loader rejects relocations whose value has the top nibble set */
static inline u32 opaque(u32 v) { __asm__("" : "+r"(v)); return v; }

u8 *mem_scratchpad(void) { return s_scratch; }

void mem_init(void)
{
    memset(g_ram, 0, sizeof g_ram);
    memset(s_scratch, 0, sizeof s_scratch);
    memset(s_hw, 0, sizeof s_hw);
    memset(s_bios, 0, sizeof s_bios);
    memcpy(s_bios + 0x66000, g_font1, g_font1_size);
    memcpy(s_bios + 0x69D68, g_font2, g_font2_size);
}

static u8 *resolve(u32 a, int size)
{
    u32 p = a & 0x1FFFFFFFu;
    if (p < 0x00800000u) return g_ram + (p & RAM_MASK);
    if (p >= SCRATCH_BASE && p + size <= SCRATCH_BASE + 0x400u) return s_scratch + opaque(p - SCRATCH_BASE);
    if (p >= HW_BASE && p + size <= HW_BASE + 0x2000u) return s_hw + opaque(p - HW_BASE);
    if (p >= BIOS_BASE && p + size <= BIOS_BASE + 0x80000u) return s_bios + opaque(p - BIOS_BASE);
    return NULL;
}

static u32 s_unmapped_reads;

static int is_cd(u32 p) { return p >= 0x1F801800u && p <= 0x1F801803u; }
static int is_spu(u32 p) { return p >= 0x1F801C00u && p < 0x1F801E80u; }
static int is_dma_chcr(u32 p) { return p >= 0x1F801080u && p < 0x1F8010F0u && (p & 0xFu) == 8u; }

static u32 hw32(u32 p) { u32 v; memcpy(&v, s_hw + opaque(p - HW_BASE), 4); return v; }
static void hw32w(u32 p, u32 v) { memcpy(s_hw + opaque(p - HW_BASE), &v, 4); }

static void unmapped(u32 a, int write)
{
    if (s_unmapped_reads++ < 16) rt_log("[mem] unmapped %s 0x%08X\n", write ? "write" : "read", a);
}

u32 mem_rd8_slow(u32 a)
{
    if (IS_SCRATCH(a)) return g_scratch[a & 0x3FF];
    u32 p = a & 0x1FFFFFFFu;
    if (is_cd(p)) return cdc_read(p);
    u8 *s = resolve(a, 1);
    if (!s) { unmapped(a, 0); return 0; }
    return *s;
}

u32 mem_rd16_slow(u32 a)
{
    if (IS_SCRATCH(a) && !(a & 1)) return *(const u16 *)(g_scratch + (a & 0x3FF));
    u32 p = a & 0x1FFFFFFFu;
    if (is_cd(p)) return cdc_read(p);
    if (is_spu(p)) return spu_read16(p);
    u32 tv;
    if (timers_read(p, &tv)) return tv & 0xFFFF;
    u8 *s = resolve(a, 2);
    if (!s) { unmapped(a, 0); return 0; }
    return s[0] | (s[1] << 8);
}

u32 mem_rd32_slow(u32 a)
{
    if (IS_SCRATCH(a) && !(a & 3)) return *(const u32 *)(g_scratch + (a & 0x3FF));
    u32 p = a & 0x1FFFFFFFu;
    switch (p) {
    case 0x1F801810u: return gpu_read_data();
    case 0x1F801814u: return gpu_read_stat();
    case 0x1F801820u: return mdec_read_data();
    case 0x1F801824u: return mdec_read_status();
    case 0x1F8010F4u: return dma_read_dicr();
    }
    if (is_cd(p)) return cdc_read(p);
    if (is_spu(p)) return spu_read16(p) | ((u32)spu_read16(p + 2) << 16);
    u32 tv;
    if (timers_read(p, &tv)) return tv;
    u8 *s = resolve(a, 4);
    if (!s) { unmapped(a, 0); return 0; }
    return s[0] | (s[1] << 8) | (s[2] << 16) | ((u32)s[3] << 24);
}

void mem_wr8_slow(u32 a, u32 v)
{
    if (IS_SCRATCH(a)) { g_scratch[a & 0x3FF] = (u8)v; return; }
    u32 p = a & 0x1FFFFFFFu;
    if (is_cd(p)) { cdc_write(p, (u8)v); return; }
    u8 *s = resolve(a, 1);
    if (!s) { unmapped(a, 1); return; }
    *s = (u8)v;
}

void mem_wr16_slow(u32 a, u32 v)
{
    if (IS_SCRATCH(a) && !(a & 1)) { *(u16 *)(g_scratch + (a & 0x3FF)) = (u16)v; return; }
    u32 p = a & 0x1FFFFFFFu;
    if (is_cd(p)) { cdc_write(p, (u8)v); return; }
    if (is_spu(p)) { spu_write16(p, (u16)v); return; }
    if (timers_write(p, v)) return;
    u8 *s = resolve(a, 2);
    if (!s) { unmapped(a, 1); return; }
    s[0] = (u8)v;
    s[1] = (u8)(v >> 8);
}

void mem_wr32_slow(u32 a, u32 v)
{
    if (IS_SCRATCH(a) && !(a & 3)) { *(u32 *)(g_scratch + (a & 0x3FF)) = v; return; }
    u32 p = a & 0x1FFFFFFFu;
    switch (p) {
    case 0x1F801810u: gpu_write_gp0(v); return;
    case 0x1F801814u: gpu_write_gp1(v); return;
    case 0x1F801820u: mdec_write0(v); return;
    case 0x1F801824u: mdec_write_ctrl(v); return;
    case 0x1F8010F4u: dma_write_dicr(v); return;
    }
    if (is_dma_chcr(p) && (v & 0x01000000u)) {
        hw32w(p, v & ~0x01000000u);
        dma_run((int)((p - 0x1F801080u) / 0x10u), hw32(p - 8u), hw32(p - 4u), v);
        return;
    }
    if (is_cd(p)) { cdc_write(p, (u8)v); return; }
    if (is_spu(p)) { spu_write16(p, (u16)v); spu_write16(p + 2, (u16)(v >> 16)); return; }
    if (timers_write(p, v)) return;
    u8 *s = resolve(a, 4);
    if (!s) { unmapped(a, 1); return; }
    s[0] = (u8)v;
    s[1] = (u8)(v >> 8);
    s[2] = (u8)(v >> 16);
    s[3] = (u8)(v >> 24);
}

u32 mem_lwl(u32 cur, u32 a)
{
    int shift = (int)((a & 3) * 8);
    u32 word = RD32(a & ~3u);
    return (cur & (0x00FFFFFFu >> shift)) | (word << (24 - shift));
}

u32 mem_lwr(u32 cur, u32 a)
{
    int shift = (int)((a & 3) * 8);
    u32 word = RD32(a & ~3u);
    return (cur & ~(0xFFFFFFFFu >> shift)) | (word >> shift);
}

void mem_swl(u32 a, u32 v)
{
    u32 al = a & ~3u;
    int shift = (int)((a & 3) * 8);
    u32 m = RD32(al);
    u32 keep = shift == 24 ? 0u : (0xFFFFFF00u << shift);
    WR32(al, (m & keep) | (v >> (24 - shift)));
}

void mem_swr(u32 a, u32 v)
{
    u32 al = a & ~3u;
    int shift = (int)((a & 3) * 8);
    u32 m = RD32(al);
    u32 keep = shift == 0 ? 0u : (0x00FFFFFFu >> (24 - shift));
    WR32(al, (m & keep) | (v << shift));
}

void mem_write_block(u32 addr, const u8 *src, u32 len)
{
    if (!len) return;
    u32 p = addr & 0x1FFFFFFFu;
    if (p < 0x00800000u && (p & RAM_MASK) + len <= RAM_SIZE) {
        memcpy(g_ram + (p & RAM_MASK), src, len);
        dispatch_notify_write(p & RAM_MASK, len);
        return;
    }
    for (u32 i = 0; i < len; i++) WR8(addr + i, src[i]);
    if (p < 0x00800000u) dispatch_notify_write(p & RAM_MASK, len);
}

void mem_read_block(u32 addr, u8 *dst, u32 len)
{
    u32 p = addr & 0x1FFFFFFFu;
    if (p < 0x00800000u && (p & RAM_MASK) + len <= RAM_SIZE) {
        memcpy(dst, g_ram + (p & RAM_MASK), len);
        return;
    }
    for (u32 i = 0; i < len; i++) dst[i] = (u8)RD8(addr + i);
}

void mem_zero(u32 addr, u32 len)
{
    u32 p = addr & 0x1FFFFFFFu;
    if (p < 0x00800000u && (p & RAM_MASK) + len <= RAM_SIZE) {
        memset(g_ram + (p & RAM_MASK), 0, len);
        return;
    }
    for (u32 i = 0; i < len; i++) WR8(addr + i, 0);
}
