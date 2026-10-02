/* Nintendo 3DS host (old 3DS compatible): libctru, software framebuffer, NDSP audio */
#include "rt.h"
#include <3ds.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>

int rt_main(const char *cue_path);

/* only framebuffers and audio buffers live in linear memory; leave the rest to the regular heap */
u32 __ctru_linear_heap_size = 6 * 1024 * 1024;

#define DATA_DIR "sdmc:/3ds/sotn"
#define CUE_NAME "Castlevania - Symphony of the Night (USA).cue"

static LightLock s_spu_lock;
static volatile int s_audio_run;
static Thread s_audio_thread;
static int s_show_fps = 0;
static u64 s_fps_t0;
static int s_fps_frames, s_fps, s_game_fps;
static PrintConsole s_con;

const char *host_data_dir(void) { return DATA_DIR; }

u64 host_ticks_us(void) { return svcGetSystemTick() / CPU_TICKS_PER_USEC; }
/* bench mode (<data>/bench.txt exists): no frameskip and no 60 Hz pacing, so the
   fps counter shows raw throughput */
static int s_bench;
void host_sleep_us(u32 us) { if (!s_bench) svcSleepThread((s64)us * 1000); }
int host_running(void) { return aptMainLoop(); }
void spu_lock(void)
{
    if (LightLock_TryLock(&s_spu_lock) == 0) return;
    u64 t = host_ticks_us();
    LightLock_Lock(&s_spu_lock);
    g_prof[PROF_SPU_WAIT] += host_ticks_us() - t;
}
void spu_unlock(void) { LightLock_Unlock(&s_spu_lock); }

/* ---------------- video ---------------- */
/* bgr555 -> rgb565, indexed with the mask bit too so no masking is needed */
static u16 s_lut[0x10000];

static void build_lut(void)
{
    for (int p = 0; p < 0x10000; p++) {
        int r = p & 31, g = (p >> 5) & 31, b = (p >> 10) & 31;
        s_lut[p] = (u16)((r << 11) | (((g << 1) | (g >> 4)) << 5) | b);
    }
}

/* two source rows into the column-major framebuffer: pixels (x, y) and (x, y + 1)
   are adjacent (y + 1 first), so each pair is one aligned 32-bit store */
static void present_rows2(u32 *dst, const u16 *r0, const u16 *r1, int n, const u16 *sxt)
{
    const u16 *lut = s_lut;
    if (!sxt)
        for (int i = 0; i < n; i++, dst += 120) *dst = lut[r1[i]] | ((u32)lut[r0[i]] << 16);
    else
        for (int i = 0; i < n; i++, dst += 120) *dst = lut[r1[sxt[i]]] | ((u32)lut[r0[sxt[i]]] << 16);
}

int host_thread_start(void (*fn)(void *), void *arg, int core)
{
    Thread t = threadCreate(fn, arg, 64 * 1024, 0x28, core, true);
    rt_log("[host] thread on core %d: %s\n", core, t ? "ok" : "failed");
    if (!t && core != 0) t = threadCreate(fn, arg, 64 * 1024, 0x31, 0, true);
    return t != NULL;
}

void host_yield(void) { svcSleepThread(100000); }

void *host_mutex_new(void) { LightLock *l = malloc(sizeof *l); LightLock_Init(l); return l; }
void host_mutex_lock(void *m) { LightLock_Lock(m); }
void host_mutex_unlock(void *m) { LightLock_Unlock(m); }
void *host_event_new(void) { LightEvent *e = malloc(sizeof *e); LightEvent_Init(e, RESET_ONESHOT); return e; }
void host_event_signal(void *e) { LightEvent_Signal(e); }
void host_event_wait(void *e) { LightEvent_Wait(e); }

int host_io_thread_start(void (*fn)(void *), void *arg)
{
    /* above the game thread (0x30): it only runs when an SD read completes */
    return threadCreate(fn, arg, 32 * 1024, 0x2C, 0, true) != NULL;
}

void host_present(const struct GpuDisplay *dp)
{
    GpuDisplay d = *dp;
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    int w = d.w, h = d.h;
    if (w <= 0 || h <= 0 || !d.enabled) {
        memset(fb, 0, 400 * 240 * 2);
    } else {
        int ystep = 1;
        if (h > 240) { ystep = 2; h /= 2; }
        if (h > 240) h = 240;
        int outw = w <= 400 ? w : 400;
        int x0 = (400 - outw) / 2, y0 = (240 - h) / 2;
        /* source x for each output column (16.16) */
        u32 sx_step = (u32)(((u64)w << 16) / outw);
        static int last_w = -1, last_h = -1;
        static int clear_frames;
        if (last_w != outw || last_h != h) { clear_frames = 2; last_w = outw; last_h = h; }
        if (clear_frames > 0) { memset(fb, 0, 400 * 240 * 2); clear_frames--; }   /* both buffers */
        static u16 sxtab[400];
        static int sx_w = -1, sx_outw = -1;
        if (sx_w != w || sx_outw != outw) {
            for (int ox = 0; ox < outw; ox++) sxtab[ox] = (u16)((ox * sx_step) >> 16);
            sx_w = w;
            sx_outw = outw;
        }
        int oy = 0;
        /* fast path: pairs of rows, when the pair lands 32-bit aligned */
        if (!d.rgb24 && d.x + w <= VRAM_W && !(y0 & 1)) {
            const u16 *sxt = outw == w ? NULL : sxtab;
            for (; oy + 1 < h; oy += 2) {
                int sy0 = (d.y + oy * ystep) & (VRAM_H - 1), sy1 = (d.y + (oy + 1) * ystep) & (VRAM_H - 1);
                u32 *dst = (u32 *)(fb + x0 * 240 + (239 - y0 - oy - 1));
                present_rows2(dst, g_vram + sy0 * VRAM_W + d.x, g_vram + sy1 * VRAM_W + d.x, outw, sxt);
            }
        }
        for (; oy < h; oy++) {
            /* framebuffer is column major: pixel (x, y) at x * 240 + 239 - y */
            u16 *col = fb + x0 * 240 + (239 - y0 - oy);
            int sy = (d.y + oy * ystep) & (VRAM_H - 1);
            if (d.rgb24) {
                const u8 *row = (const u8 *)(g_vram + sy * VRAM_W);
                for (int ox = 0; ox < outw; ox++) {
                    int o = d.x * 2 + sxtab[ox] * 3;
                    u8 r = row[o & 2047], g = row[(o + 1) & 2047], b = row[(o + 2) & 2047];
                    col[ox * 240] = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                }
            } else if (d.x + w <= VRAM_W) {
                const u16 *row = g_vram + sy * VRAM_W + d.x;
                if (outw == w)
                    for (int ox = 0; ox < outw; ox++) col[ox * 240] = s_lut[row[ox] & 0x7FFF];
                else
                    for (int ox = 0; ox < outw; ox++) col[ox * 240] = s_lut[row[sxtab[ox]] & 0x7FFF];
            } else {
                const u16 *row = g_vram + sy * VRAM_W;
                for (int ox = 0; ox < outw; ox++) col[ox * 240] = s_lut[row[(d.x + sxtab[ox]) & (VRAM_W - 1)] & 0x7FFF];
            }
        }
    }
    gfxFlushBuffers();
    gfxSwapBuffers();

    s_fps_frames++;
    u64 now = host_ticks_us();
    if (now - s_fps_t0 >= 1000000) {
        s_fps = s_fps_frames;
        s_fps_frames = 0;
        s_fps_t0 = now;
        static u32 last_fc;
        s_game_fps = (int)(g_frame_count - last_fc);
        last_fc = g_frame_count;
        if (s_show_fps) printf("\x1b[29;1Hfps %2d (game %2d)   ", s_fps, s_game_fps);
        static int sec;
        if (++sec % 5 == 0) {
            rt_log("[fps] %d game %d  gpu %llu present %llu idle %llu spuwait %llu cd %llu mdec %llu spumix %llu worker %llu sync %llu (ms/s)\n", s_fps, s_game_fps,
                   g_prof[PROF_GPU] / 5000, g_prof[PROF_PRESENT] / 5000, g_prof[PROF_IDLE] / 5000,
                   g_prof[PROF_SPU_WAIT] / 5000, g_prof[PROF_CD] / 5000, g_prof[PROF_MDEC] / 5000, g_prof[PROF_SPU_MIX] / 5000, g_prof[PROF_WORKER] / 5000, g_prof[PROF_SYNC] / 5000);
#ifdef RT_GPU_PROF
            rt_log("      gpu detail: tri %llu (setup %llu) rect %llu fill %llu (ms/s) tris/s %u spans/s %u\n", g_prof[PROF_G_TRI] / 5000,
                   g_prof[PROF_G_SETUP] / 5000, g_prof[PROF_G_RECT] / 5000, g_prof[PROF_G_FILL] / 5000, g_tri_calls / 5, g_tri_spans / 5);
            g_tri_calls = g_tri_spans = 0;
#endif
            memset(g_prof, 0, sizeof g_prof);
        }
    }
}

/* ---------------- input ---------------- */
/* optional test script: <data>/autoinput.txt with "frame:hexmask" pairs (PSX buttons, active high) */
typedef struct { u32 frame; u16 mask; } AutoEv;
static AutoEv s_auto[512];
static int s_auto_n = -1;
static u32 s_input_frame;

static void load_auto(void)
{
    s_auto_n = 0;
    FILE *b = fopen(DATA_DIR "/bench.txt", "r");
    if (b) { fclose(b); s_bench = 1; g_frameskip_max = 0; rt_log("[host] bench mode\n"); }
    FILE *f = fopen(DATA_DIR "/autoinput.txt", "r");
    if (!f) return;
    unsigned fr, m;
    while (s_auto_n < 512 && fscanf(f, " %u:%x ,", &fr, &m) == 2) { s_auto[s_auto_n].frame = fr; s_auto[s_auto_n].mask = (u16)m; s_auto_n++; }
    fclose(f);
    rt_log("[input] autoinput: %d events\n", s_auto_n);
}

void host_poll_input(void)
{
    if (s_auto_n < 0) load_auto();
    s_input_frame++;
    hidScanInput();
    u32 k = hidKeysHeld();
    u16 m = 0;
    if (k & KEY_SELECT) m |= 1 << 0;
    if (k & KEY_START) m |= 1 << 3;
    if (k & KEY_UP) m |= 1 << 4;
    if (k & KEY_RIGHT) m |= 1 << 5;
    if (k & KEY_DOWN) m |= 1 << 6;
    if (k & KEY_LEFT) m |= 1 << 7;
    if (k & KEY_L) m |= 1 << 10;      /* L1 */
    if (k & KEY_R) m |= 1 << 11;      /* R1 */
    if (k & KEY_X) m |= 1 << 12;      /* triangle */
    if (k & KEY_A) m |= 1 << 13;      /* circle */
    if (k & KEY_B) m |= 1 << 14;      /* cross */
    if (k & KEY_Y) m |= 1 << 15;      /* square */
    if (k & KEY_ZL) m |= 1 << 8;
    if (k & KEY_ZR) m |= 1 << 9;
    /* old 3DS has no ZL/ZR: touch the left/right half of the bottom screen for L2/R2 */
    if (k & KEY_TOUCH) {
        touchPosition t;
        hidTouchRead(&t);
        m |= t.px < 160 ? (1 << 8) : (1 << 9);
    }
    for (int i = 0; i < s_auto_n; i++) if (s_auto[i].frame <= s_input_frame) { if (i + 1 == s_auto_n || s_auto[i + 1].frame > s_input_frame) m |= s_auto[i].mask; }
    g_pad_state = (u16)~m;
    u32 down = hidKeysDown();
    if ((k & KEY_SELECT) && (down & KEY_START)) s_show_fps = !s_show_fps;
}

/* ---------------- audio ---------------- */
#define AUD_FRAMES 1024
#define AUD_BUFS 4
static ndspWaveBuf s_wb[AUD_BUFS];
static s16 *s_abuf;

static void audio_thread(void *arg)
{
    (void)arg;
    rt_log("[host] audio thread running on cpu %d\n", svcGetProcessorID());
    while (s_audio_run) {
        int filled = 0;
        for (int i = 0; i < AUD_BUFS; i++) {
            if (s_wb[i].status == NDSP_WBUF_DONE || s_wb[i].status == NDSP_WBUF_FREE) {
                u64 t0 = host_ticks_us();
                spu_mix(s_wb[i].data_pcm16, AUD_FRAMES);
                g_prof[PROF_SPU_MIX] += host_ticks_us() - t0;
                DSP_FlushDataCache(s_wb[i].data_pcm16, AUD_FRAMES * 4);
                ndspChnWaveBufAdd(0, &s_wb[i]);
                filled = 1;
            }
        }
        if (!filled) svcSleepThread(2000000);
    }
}

void host_audio_init(void)
{
    LightLock_Init(&s_spu_lock);
    if (R_FAILED(ndspInit())) { printf("ndsp unavailable (dspfirm.cdc missing?), no sound\n"); return; }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_NONE);
    ndspChnSetRate(0, 44100.0f);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = { 1.0f, 1.0f };
    ndspChnSetMix(0, mix);
    s_abuf = linearAlloc(AUD_FRAMES * 4 * AUD_BUFS);
    memset(s_abuf, 0, AUD_FRAMES * 4 * AUD_BUFS);
    memset(s_wb, 0, sizeof s_wb);
    for (int i = 0; i < AUD_BUFS; i++) {
        s_wb[i].data_pcm16 = s_abuf + i * AUD_FRAMES * 2;
        s_wb[i].nsamples = AUD_FRAMES;
        s_wb[i].status = NDSP_WBUF_FREE;
    }
    s_audio_run = 1;
    s_audio_thread = threadCreate(audio_thread, NULL, 32 * 1024, 0x18, 1, true);
    if (!s_audio_thread) {
        printf("audio on core 0\n");
        s_audio_thread = threadCreate(audio_thread, NULL, 32 * 1024, 0x18, 0, true);
    }
}

/* ---------------- misc ---------------- */
void host_init(void) {}
void host_shutdown(void) {}

void rt_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    s_audio_run = 0;
    rt_log("[FATAL] %s\n", buf);
    printf("\n\x1b[31mERROR:\x1b[0m %s\n\nPress START to exit.\n", buf);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    ndspExit();
    gfxExit();
    exit(1);
}

static char s_cue[300];

static void game_thread(void *arg)
{
    (void)arg;
    rt_main(s_cue);
}

int main(void)
{
    APT_SetAppCpuTimeLimit(80);   /* let the GPU worker and audio use the second core */
    gfxInit(GSP_RGB565_OES, GSP_RGB565_OES, false);
    gfxSetDoubleBuffering(GFX_TOP, true);
    consoleInit(GFX_BOTTOM, &s_con);
    build_lut();
    mkdir("sdmc:/3ds", 0777);
    mkdir(DATA_DIR, 0777);
    printf("SymphonyRecomp 3DS\n");
    printf("B=Cross A=Circle Y=Square X=Triangle\n");
    printf("L/R=L1/R1  touch left/right=L2/R2\n");
    printf("SELECT+START: fps counter\n\n");

    snprintf(s_cue, sizeof s_cue, "%s/%s", DATA_DIR, CUE_NAME);
    FILE *f = fopen(s_cue, "r");
    if (!f) rt_fatal("Disc not found. Copy the cue + both bins to\n%s/", DATA_DIR);
    fclose(f);

    s_fps_t0 = host_ticks_us();
    /* the recompiled code nests native calls deeply: give it a big stack */
    Thread t = threadCreate(game_thread, NULL, 8 * 1024 * 1024, 0x30, 0, false);
    if (!t) rt_fatal("could not create game thread");
    threadJoin(t, U64_MAX);
    threadFree(t);
    s_audio_run = 0;
    ndspExit();
    gfxExit();
    return 0;
}
