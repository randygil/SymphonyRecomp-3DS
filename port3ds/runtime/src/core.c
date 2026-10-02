#include "rt.h"
#include <stdarg.h>
#include <stdlib.h>

Cpu g_cpu;
u16 g_pad_state = 0xFFFF;
u32 g_frame_count;
u64 g_prof[PROF_N];

/* ---------------- interrupts ---------------- */
static int s_in_handler;
static int s_irq_pending[16];

static void irq_dispatch(int irq)
{
    u32 env = g_intr_env_addr;
    if (!env) return;
    u32 handler = RD32(env + 2u + (u32)irq * 4u);
    if (!handler) return;
    Cpu snap = g_cpu;
    WR16(env, 1);
    dispatch_call(&g_cpu, handler);
    WR16(env, 0);
    memcpy(g_cpu.r, snap.r, sizeof snap.r);
    g_cpu.hi = snap.hi;
    g_cpu.lo = snap.lo;
}

void irq_deliver(int irq)
{
    if ((unsigned)irq >= 16) return;
    if (s_in_handler) { s_irq_pending[irq] = 1; return; }
    s_in_handler = 1;
    irq_dispatch(irq);
    for (int again = 1; again;) {
        again = 0;
        for (int i = 0; i < 16; i++) {
            if (!s_irq_pending[i]) continue;
            s_irq_pending[i] = 0;
            irq_dispatch(i);
            again = 1;
        }
    }
    s_in_handler = 0;
}

/* ---------------- frame pacing / VSync ---------------- */
static u64 s_next_frame_us;

int g_frameskip_max = 2;   /* 0 disables automatic frameskip */
int g_test_onehit, g_test_vclock;

u64 rt_stream_clock_us(void) { return g_test_vclock ? (u64)g_frame_count * 16683u : host_ticks_us(); }

/* entity table 0x800733D8, stride 0xBC, hit points at +0x3E; enemies live in
   slots 64..255 and 0x7FFF marks invulnerable ones */
static void test_onehit(void)
{
    for (u32 i = 64; i < 256; i++) {
        u32 o = (0x000733D8u + i * 0xBCu + 0x3Eu) & RAM_MASK;
        s16 hp = *(s16 *)(g_ram + o);
        if (hp > 1 && hp != 0x7FFF) *(s16 *)(g_ram + o) = 1;
    }
}
static int s_skipped;

/* paces the game to 60 Hz; returns how far behind schedule we are (us) */
static u32 throttle(void)
{
    const u64 frame_us = 16683;
    u64 now = host_ticks_us();
    if (!s_next_frame_us) s_next_frame_us = now;
    s_next_frame_us += frame_us;
    if (now > s_next_frame_us + 100000 || now + 100000 < s_next_frame_us - frame_us) {
        s_next_frame_us = now;
        return 0;
    }
    if (s_next_frame_us > now) {
        host_sleep_us((u32)(s_next_frame_us - now));
        return 0;
    }
    return (u32)(now - s_next_frame_us);
}

void rt_present_frame(void)
{
    if (!host_running()) rt_fatal("quit");
    static u64 s_draw_mark, s_draw_cost;
    u64 t0 = host_ticks_us();
    if (!gpu_skipping()) {
        gpu_present();
        /* what rasterizing the frame cost: skipping only helps when this is high
           (e.g. during FMVs the time goes to MDEC, so frames are never dropped) */
        u64 m = g_prof[PROF_GPU] + g_prof[PROF_SYNC];
        s_draw_cost = m - s_draw_mark;
    }
    s_draw_mark = g_prof[PROF_GPU] + g_prof[PROF_SYNC];
    u64 t1 = host_ticks_us();
    host_poll_input();
    u32 late = throttle();
    /* frameskip: keep running the game at full speed, draw fewer frames.
       A few ms of lag are tolerated so the drawn/skipped ratio settles near
       what the frame budget allows instead of dropping two frames each time. */
    if (late > 4000 && s_draw_cost > 2000 && s_skipped < g_frameskip_max) { s_skipped++; gpu_set_skip(1); }
    else { s_skipped = 0; gpu_set_skip(0); }
    u64 t2 = host_ticks_us();
    g_prof[PROF_PRESENT] += t1 - t0;
    g_prof[PROF_IDLE] += t2 - t1;
    if (g_test_onehit) test_onehit();
    libcd_tick();
    libcdstream_pump();
    bios_refresh_pad();
    libpad_refresh();
    irq_deliver(0);
    g_frame_count++;
    {
        extern u32 g_disc_reads, g_disc_last_lba;
        static int dbg = -1;
        if (dbg < 0) dbg = getenv("RT_XA_DEBUG") != NULL;
        if (dbg && g_frame_count % 120 == 0) {
            rt_log("[cd] %u sector reads in 120 frames, last lba %u\n", g_disc_reads, g_disc_last_lba);
            g_disc_reads = 0;
        }
    }
}

static int s_vcount;

void sdk_LibEtc_VSync(Cpu *restrict c)
{
    s32 mode = (s32)c->r[4];
    if (mode < 0) { c->r[2] = (u32)s_vcount; return; }
    rt_present_frame();
    s_vcount++;
    c->r[2] = 0;
}

/* ---------------- timers (approximation driven by the host clock) ---------------- */
static u64 s_timer_reset[3];
static u16 s_timer_mode[3], s_timer_target[3];

static double timer_rate(int t)
{
    switch (t) {
    case 0: return (s_timer_mode[0] & 0x100u) ? 5322240.0 : 33868800.0;
    case 1: return (s_timer_mode[1] & 0x100u) ? 15780.0 : 33868800.0;
    default: return (s_timer_mode[2] & 0x200u) ? 33868800.0 / 8.0 : 33868800.0;
    }
}

int timers_read(u32 p, u32 *out)
{
    if (p < 0x1F801100u || p >= 0x1F801130u) return 0;
    int t = (int)((p - 0x1F801100u) / 0x10u);
    switch ((p - 0x1F801100u) & 0xFu) {
    case 0x0: {
        double el = (double)(host_ticks_us() - s_timer_reset[t]) / 1e6;
        *out = (u32)((u64)(el * timer_rate(t)) & 0xFFFF);
        break;
    }
    case 0x4: *out = s_timer_mode[t]; break;
    case 0x8: *out = s_timer_target[t]; break;
    default: *out = 0; break;
    }
    return 1;
}

int timers_write(u32 p, u32 v)
{
    if (p < 0x1F801100u || p >= 0x1F801130u) return 0;
    int t = (int)((p - 0x1F801100u) / 0x10u);
    switch ((p - 0x1F801100u) & 0xFu) {
    case 0x0: s_timer_reset[t] = host_ticks_us(); break;
    case 0x4: s_timer_mode[t] = (u16)v; s_timer_reset[t] = host_ticks_us(); break;
    case 0x8: s_timer_target[t] = (u16)v; break;
    }
    return 1;
}

/* ---------------- DMA ---------------- */
static u32 s_dicr;

u32 dma_read_dicr(void) { return s_dicr; }

void dma_write_dicr(u32 v)
{
    u32 flags = (s_dicr >> 24) & 0x7Fu;
    flags &= ~((v >> 24) & 0x7Fu);
    s_dicr = (v & 0x00FFFFFFu) | (flags << 24);
    if ((s_dicr & 0x8000u) || (((s_dicr >> 23) & 1u) && flags)) s_dicr |= 0x80000000u;
}

static u32 word_count(u32 bcr)
{
    u32 size = bcr & 0xFFFFu, blocks = (bcr >> 16) & 0xFFFFu;
    u32 total = blocks == 0 ? size : size * blocks;
    return total == 0 ? 0x10000u : total;
}

static void gpu_linked_list(u32 addr)
{
    addr &= RAM_MASK & ~3u;
    for (int guard = 0; guard < 0x100000; guard++) {
        u32 header = *(u32 *)(g_ram + addr);
        u32 count = header >> 24;
        for (u32 i = 0; i < count; i++) gpu_write_gp0(*(u32 *)(g_ram + ((addr + 4u + i * 4u) & RAM_MASK)));
        u32 next = header & 0xFFFFFFu;
        if (next == 0xFFFFFFu || (next & 0x800000u)) break;
        addr = next & RAM_MASK & ~3u;
    }
}

void dma_run(int ch, u32 madr, u32 bcr, u32 chcr)
{
    switch (ch) {
    case 0: {
        u32 n = word_count(bcr), o = madr & RAM_MASK;
        int fast = IS_RAM(madr) && !(o & 3) && o + n * 4 <= RAM_SIZE;
        for (u32 i = 0; i < n;) {
            int k = fast ? mdec_write_block((const u32 *)(g_ram + o + i * 4u), (int)(n - i)) : 0;
            if (k) { i += (u32)k; continue; }
            mdec_write0(RD32(madr + i * 4u));
            i++;
        }
        break;
    }
    case 1: {
        u32 n = word_count(bcr), o = madr & RAM_MASK;
        if (IS_RAM(madr) && !(o & 3) && o + n * 4 <= RAM_SIZE &&
            (o + n * 4 <= g_watch_lo || o >= g_watch_lo + 0x800u) &&
            mdec_read_block((u32 *)(g_ram + o), (int)n)) break;
        for (u32 i = 0; i < n; i++) WR32(madr + i * 4u, mdec_read_data());
        break;
    }
    case 2: {
        u32 sync = (chcr >> 9) & 3u;
        if (sync == 2) { u64 t = host_ticks_us(); gpu_linked_list(madr); g_prof[PROF_GPU] += host_ticks_us() - t; }
        else if (chcr & 1u) {
            u32 n = word_count(bcr), o = madr & RAM_MASK;
            if (IS_RAM(madr) && !(o & 3) && o + n * 4 <= RAM_SIZE) gpu_write_gp0_block((const u32 *)(g_ram + o), n);
            else for (u32 i = 0; i < n; i++) gpu_write_gp0(RD32(madr + i * 4u));
        }
        else { u32 n = word_count(bcr); for (u32 i = 0; i < n; i++) WR32(madr + i * 4u, gpu_read_data()); }
        break;
    }
    case 3: cdc_dma_read(madr, word_count(bcr) * 4u); break;
    case 4: {
        if (!(chcr & 1u)) break;
        u32 bytes = word_count(bcr) * 4u;
        u8 *buf = malloc(bytes);
        if (!buf) break;
        mem_read_block(madr, buf, bytes);
        spu_dma_write(buf, bytes);
        free(buf);
        break;
    }
    case 6: {
        u32 count = bcr & 0xFFFFu;
        if (!count) break;
        u32 a = madr;
        for (u32 i = 0; i < count - 1; i++) { WR32(a, (a - 4u) & 0x00FFFFFFu); a -= 4u; }
        WR32(a, 0x00FFFFFFu);
        break;
    }
    default: return;
    }
    int master = (s_dicr & (1u << 23)) != 0;
    int enabled = (s_dicr & (1u << (16 + ch))) != 0;
    if (!master || !enabled) return;
    s_dicr |= 1u << (24 + ch);
    irq_deliver(3);
}

/* ---------------- libgpu ---------------- */
void sdk_LibGpu_DrawOTag(Cpu *restrict c) { u64 t = host_ticks_us(); gpu_linked_list(c->r[4]); g_prof[PROF_GPU] += host_ticks_us() - t; }
void sdk_LibGpu_DrawSync(Cpu *restrict c) { c->r[2] = 0; }

static s16 S16(u32 a) { return (s16)RD16(a); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void sdk_LibGpu_PutDrawEnv(Cpu *restrict c)
{
    u32 env = c->r[4];
    s16 cx = S16(env + 0), cy = S16(env + 2), cw = S16(env + 4), ch = S16(env + 6);
    s16 ox = S16(env + 8), oy = S16(env + 10);
    s16 twx = S16(env + 12), twy = S16(env + 14), tww = S16(env + 16), twh = S16(env + 18);
    u16 tpage = (u16)RD16(env + 0x14);
    u8 dtd = (u8)RD8(env + 0x16), dfe = (u8)RD8(env + 0x17), isbg = (u8)RD8(env + 0x18);
    u8 r0 = (u8)RD8(env + 0x19), g0 = (u8)RD8(env + 0x1A), b0 = (u8)RD8(env + 0x1B);

    int x0 = clampi(cx, 0, VRAM_W - 1), y0 = clampi(cy, 0, VRAM_H - 1);
    int x1 = clampi(cx + cw - 1, 0, VRAM_W - 1), y1 = clampi(cy + ch - 1, 0, VRAM_H - 1);
    gpu_write_gp0(0xE3000000u | (((u32)y0 & 0x3FF) << 10) | ((u32)x0 & 0x3FF));
    gpu_write_gp0(0xE4000000u | (((u32)y1 & 0x3FF) << 10) | ((u32)x1 & 0x3FF));
    gpu_write_gp0(0xE5000000u | (((u32)oy & 0x7FF) << 11) | ((u32)ox & 0x7FF));
    gpu_write_gp0((dtd ? 0xE1000200u : 0xE1000000u) | (dfe ? 0x400u : 0u) | ((u32)tpage & 0x9FF));
    {
        u32 a = ((u32)twx & 0xFF) >> 3, b = ((u32)twy & 0xFF) >> 3;
        u32 w = ((u32)(-tww) & 0xFF) >> 3, h = ((u32)(-twh) & 0xFF) >> 3;
        gpu_write_gp0(0xE2000000u | (b << 15) | (a << 10) | (h << 5) | w);
    }
    gpu_write_gp0(0xE6000000u);
    if (isbg) {
        int w = clampi(cw, 0, VRAM_W - 1), h = clampi(ch, 0, VRAM_H - 1);
        int x = cx - ox, y = cy - oy;
        gpu_write_gp0(0x60000000u | ((u32)b0 << 16) | ((u32)g0 << 8) | r0);
        gpu_write_gp0(((u32)(u16)y << 16) | (u16)x);
        gpu_write_gp0(((u32)(u16)h << 16) | (u16)w);
    }
    c->r[2] = c->r[4];
}

void sdk_LibGpu_PutDispEnv(Cpu *restrict c)
{
    u32 env = c->r[4];
    s16 dx = S16(env + 0), dy = S16(env + 2), dw = S16(env + 4), dh = S16(env + 6);
    s16 sx = S16(env + 8), sy = S16(env + 10), sw = S16(env + 12), sh = S16(env + 14);
    u8 isinter = (u8)RD8(env + 0x10), isrgb24 = (u8)RD8(env + 0x11);
    int pal = gpu_is_pal();
    gpu_write_gp1(0x05000000u | (((u32)dy & 0x3FF) << 10) | ((u32)dx & 0x3FF));
    int hs = sx * 10 + 0x260, vs = sy + (pal ? 0x13 : 0x10);
    int he = hs + (sw ? sw * 10 : 2560), ve = vs + (sh ? sh : 240);
    hs = clampi(hs, 500, 3290);
    he = clampi(he, hs + 0x50, 3290);
    vs = clampi(vs, 0x10, pal ? 310 : 256);
    ve = clampi(ve, vs + 2, pal ? 312 : 258);
    gpu_write_gp1(0x06000000u | (((u32)he & 0xFFF) << 12) | ((u32)hs & 0xFFF));
    gpu_write_gp1(0x07000000u | (((u32)ve & 0x3FF) << 10) | ((u32)vs & 0x3FF));
    u32 mode = 0x08000000u;
    if (pal) mode |= 0x8;
    if (isrgb24) mode |= 0x10;
    if (isinter) mode |= 0x20;
    if (dw <= 280) {}
    else if (dw <= 352) mode |= 1;
    else if (dw <= 400) mode |= 0x40;
    else if (dw <= 560) mode |= 2;
    else mode |= 3;
    if (dh > (pal ? 288 : 256)) mode |= 0x24;
    gpu_write_gp1(mode);
    c->r[2] = c->r[4];
}

/* ---------------- libpad ---------------- */
static u32 s_pad1, s_pad2;

static void write_pad(u32 buf, u16 buttons, int present)
{
    WR8(buf + 0, present ? 0x00 : 0xFF);
    WR8(buf + 1, present ? 0x41 : 0xFF);
    WR8(buf + 2, (u8)buttons);
    WR8(buf + 3, (u8)(buttons >> 8));
    WR8(buf + 4, 0x80);
    WR8(buf + 5, 0x80);
    WR8(buf + 6, 0x80);
    WR8(buf + 7, 0x80);
}

void libpad_refresh(void)
{
    if (s_pad1) write_pad(s_pad1, g_pad_state, 1);
    if (s_pad2) write_pad(s_pad2, 0xFFFF, 0);
}

void sdk_LibPad_PadInitDirect(Cpu *restrict c) { s_pad1 = c->r[4]; s_pad2 = c->r[5]; c->r[2] = 0; }
void sdk_LibPad_PadStartCom(Cpu *restrict c) { libpad_refresh(); c->r[2] = 0; }
void sdk_LibPad_PadStopCom(Cpu *restrict c) { c->r[2] = 0; }
void sdk_LibPad_PadEnableCom(Cpu *restrict c) { c->r[2] = 0; }
void sdk_LibPad_PadChkVsync(Cpu *restrict c) { c->r[2] = 1; }
void sdk_LibPad_PadChkMtap(Cpu *restrict c) { c->r[2] = 0; }
void sdk_LibPad_PadGetState(Cpu *restrict c) { c->r[2] = (c->r[4] & 0x10u) == 0 ? 6u : 0u; }
void sdk_LibPad_PadInfoMode(Cpu *restrict c) { c->r[2] = 0; }
void sdk_LibPad_PadInfoComb(Cpu *restrict c) { c->r[2] = 0; }
void sdk_LibPad_PadInfoAct(Cpu *restrict c) { c->r[2] = (s32)c->r[6] < 0 ? 2u : 1u; }
void sdk_LibPad_PadSetMainMode(Cpu *restrict c) { c->r[2] = 0; }
void sdk_LibPad_PadSetActAlign(Cpu *restrict c) { c->r[2] = (c->r[4] & 0x10u) ? 1u : (c->r[5] == 0 || c->r[6] < 2) ? 0u : 1u; }
void sdk_LibPad_PadSetAct(Cpu *restrict c) { c->r[2] = (c->r[4] & 0x10u) ? 1u : (c->r[5] == 0 || c->r[6] == 0) ? 0u : 1u; }

/* ---------------- memory card ---------------- */
MemCard g_card[2];

#define MC_FR 0x80
#define MC_BLK 0x2000
#define MC_DIR 15

static void mc_fix(MemCard *mc, int o)
{
    u8 x = 0;
    for (int i = 0; i < 0x7F; i++) x ^= mc->d[o + i];
    mc->d[o + 0x7F] = x;
}

void mc_flush(MemCard *mc)
{
    FILE *f = fopen(mc->path, "wb");
    if (!f) { rt_log("[mc] cannot write %s\n", mc->path); return; }
    fwrite(mc->d, 1, sizeof mc->d, f);
    fclose(f);
}

void mc_format(MemCard *mc)
{
    memset(mc->d, 0, sizeof mc->d);
    mc->d[0] = 0x4D; mc->d[1] = 0x43; mc_fix(mc, 0);
    for (int i = 1; i <= MC_DIR; i++) { int o = i * MC_FR; mc->d[o] = 0xA0; mc->d[o + 8] = 0xFF; mc->d[o + 9] = 0xFF; mc_fix(mc, o); }
    for (int i = 16; i <= 35; i++) { int o = i * MC_FR; mc->d[o] = mc->d[o + 1] = mc->d[o + 2] = mc->d[o + 3] = 0xFF; mc->d[o + 8] = 0xFF; mc->d[o + 9] = 0xFF; mc_fix(mc, o); }
    mc_flush(mc);
}

void mc_load(MemCard *mc, const char *path)
{
    snprintf(mc->path, sizeof mc->path, "%s", path);
    mc->enabled = 1;
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(mc->d, 1, sizeof mc->d, f);
        fclose(f);
        if (n == sizeof mc->d) return;
    }
    mc_format(mc);
}

static void mc_name(MemCard *mc, int b, char *out)
{
    int o = b * MC_FR + 0x0A, n = 0;
    while (n < 20 && mc->d[o + n]) { out[n] = (char)mc->d[o + n]; n++; }
    out[n] = 0;
}

int mc_file_size(MemCard *mc, int b) { int v; memcpy(&v, mc->d + b * MC_FR + 4, 4); return v; }

int mc_find(MemCard *mc, const char *name)
{
    char n[21];
    for (int b = 1; b <= MC_DIR; b++) {
        if (mc->d[b * MC_FR] != 0x51) continue;
        mc_name(mc, b, n);
        if (!strcmp(n, name)) return b;
    }
    return 0;
}

int mc_chain(MemCard *mc, int first, int *out, int max)
{
    int n = 0, b = first, guard = 0;
    while (b >= 1 && b <= MC_DIR && guard++ < MC_DIR && n < max) {
        out[n++] = b;
        int next = mc->d[b * MC_FR + 8] | (mc->d[b * MC_FR + 9] << 8);
        if (next == 0xFFFF) break;
        b = next + 1;
    }
    return n;
}

int mc_create(MemCard *mc, const char *name, int blocks)
{
    if (blocks < 1) blocks = 1;
    if (mc_find(mc, name)) return 0;
    int fr[MC_DIR], nf = 0;
    for (int b = 1; b <= MC_DIR && nf < blocks; b++) if (mc->d[b * MC_FR] == 0xA0) fr[nf++] = b;
    if (nf < blocks) return 0;
    for (int i = 0; i < blocks; i++) {
        int b = fr[i], o = b * MC_FR;
        memset(mc->d + o, 0, MC_FR);
        mc->d[o] = (u8)(i == 0 ? 0x51 : i == blocks - 1 ? 0x53 : 0x52);
        int next = i == blocks - 1 ? 0xFFFF : fr[i + 1] - 1;
        mc->d[o + 8] = (u8)next; mc->d[o + 9] = (u8)(next >> 8);
        if (i == 0) {
            int sz = blocks * MC_BLK;
            memcpy(mc->d + o + 4, &sz, 4);
            int len = (int)strlen(name);
            for (int k = 0; k < len && k < 20; k++) mc->d[o + 0x0A + k] = (u8)name[k];
        }
        mc_fix(mc, o);
        memset(mc->d + b * MC_BLK, 0, MC_BLK);
    }
    mc_flush(mc);
    return fr[0];
}

void mc_frame_read(MemCard *mc, int frame, u8 *dst)
{
    if ((unsigned)frame < sizeof mc->d / MC_FR) memcpy(dst, mc->d + frame * MC_FR, MC_FR);
}

void mc_frame_write(MemCard *mc, int frame, const u8 *src)
{
    if ((unsigned)frame >= sizeof mc->d / MC_FR) return;
    memcpy(mc->d + frame * MC_FR, src, MC_FR);
    mc_flush(mc);
}

u8 mc_read_byte(MemCard *mc, const int *chain, int n, int pos)
{
    int bi = pos / MC_BLK, off = pos % MC_BLK;
    return bi < n ? mc->d[chain[bi] * MC_BLK + off] : 0;
}

void mc_write_byte(MemCard *mc, const int *chain, int n, int pos, u8 v)
{
    int bi = pos / MC_BLK, off = pos % MC_BLK;
    if (bi < n) mc->d[chain[bi] * MC_BLK + off] = v;
}

void mc_delete(MemCard *mc, const char *name)
{
    int first = mc_find(mc, name);
    if (!first) return;
    int ch[MC_DIR];
    int n = mc_chain(mc, first, ch, MC_DIR);
    for (int i = 0; i < n; i++) { mc->d[ch[i] * MC_FR] = 0xA0; mc_fix(mc, ch[i] * MC_FR); }
    mc_flush(mc);
}

static int mc_glob(const char *pat, const char *name)
{
    if (!*pat) return 1;
    for (; *pat; pat++, name++) {
        if (*pat == '*') return 1;
        if (!*name) return 0;
        if (*pat != '?' && *pat != *name) return 0;
    }
    return *name == 0;
}

int mc_match(MemCard *mc, const char *pattern, char names[][21], int *sizes, int max)
{
    int n = 0;
    for (int b = 1; b <= MC_DIR && n < max; b++) {
        if (mc->d[b * MC_FR] != 0x51) continue;
        mc_name(mc, b, names[n]);
        if (!mc_glob(pattern, names[n])) continue;
        sizes[n] = mc_file_size(mc, b);
        n++;
    }
    return n;
}

/* ---------------- bug fixes from patches/qol/FunctionFixes.cs ---------------- */
int fix_scylla_door(Cpu *restrict c)
{
    (void)c;
    if (RD8(0x80180c66) == 1 && RD32(0x8003CA3C) > 0) WR8(0x80180c66, 0);
    return 1;
}

int fix_olrox_explosion(Cpu *restrict c)
{
    (void)c;
    if (RD16(0x80077c64) > 0x7000 && RD32(0x8003CA2C) > 0) WR16(0x80077c64, 0x60);
    return 1;
}

int fix_clock_collision(Cpu *restrict c)
{
    (void)c;
    WR16(0x80182476, 0x80);
    return 1;
}

int fix_minotaur_werewolf(Cpu *restrict c)
{
    (void)c;
    u32 step = RD8(0x80077A84), pstep = RD16(0x80073404), px = RD16(0x800733DA);
    if (step < 4 && px > 0x40 && (pstep == 5 || pstep == 0x18 || pstep == 0x19)) {
        WR16(0x80073404, 0);
        WR16(0x80073406, 0);
        WR16(0x800733EE, 0x8100);
        WR16(0x800733F0, 0);
    }
    return 1;
}

/* ---------------- logging ---------------- */
static FILE *s_logf;

void rt_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    if (!s_logf) {
        char p[300];
        snprintf(p, sizeof p, "%s/log.txt", host_data_dir());
        s_logf = fopen(p, "w");
    }
    if (s_logf) { fputs(buf, s_logf); fflush(s_logf); }
}
