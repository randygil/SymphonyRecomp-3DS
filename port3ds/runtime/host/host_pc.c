/* PC host (SDL2) used to debug the runtime quickly; not part of the 3DS build.
   RT_HEADLESS=1          no window, frames dumped to <data>/frames/NNNNN.bmp every RT_DUMP frames
   RT_FRAMES=n            quit after n frames
   RT_INPUT=f:mask,...    scripted pad (mask = PSX buttons active-high, hex) from frame f
   RT_VCLOCK=1            clock advances exactly one frame per vsync (deterministic runs,
                          profiling numbers become meaningless)
   RT_PCM=path            headless: mix 735 stereo samples per frame into a raw s16 file
   RT_CHEAT_ONEHIT=1      enemies die in one hit (to script past bosses in tests)
*/
#include "rt.h"
#include <SDL2/SDL.h>
#include <stdarg.h>
#include <stdlib.h>
#include <windows.h>

int rt_main(const char *cue_path);

static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture *s_tex;
static SDL_mutex *s_spu_mutex;
static int s_vclock;
static int s_headless, s_dump_every, s_max_frames, s_running = 1;
static u32 s_frame;
static u32 s_pixels[640 * 480];
static char s_data_dir[260] = ".";

typedef struct { u32 frame; u16 mask; } InputEv;
static InputEv s_script[8192];
static int s_script_n;

const char *host_data_dir(void) { return s_data_dir; }
u64 host_ticks_us(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (s_vclock) return (u64)g_frame_count * 16683u;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (u64)(c.QuadPart * 1000000 / f.QuadPart);
}

void host_sleep_us(u32 us)
{
    if (s_headless) return;
    if (us > 1500) SDL_Delay((us - 1000) / 1000);
}

int host_running(void) { return s_running; }
void spu_lock(void) { if (s_spu_mutex) SDL_LockMutex(s_spu_mutex); }
void spu_unlock(void) { if (s_spu_mutex) SDL_UnlockMutex(s_spu_mutex); }

static void audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    spu_mix((s16 *)stream, len / 4);
}

void host_audio_init(void)
{
    s_spu_mutex = SDL_CreateMutex();
    if (s_headless) return;
    SDL_AudioSpec want = { 0 }, have;
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_cb;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev) SDL_PauseAudioDevice(dev, 0);
}

static void convert_display(const GpuDisplay *dp, int *ow, int *oh)
{
    GpuDisplay d = *dp;
    int w = d.w, h = d.h;
    if (w <= 0 || h <= 0) { w = 320; h = 240; }
    if (w > 640) w = 640;
    if (h > 480) h = 480;
    for (int y = 0; y < h; y++) {
        const u16 *row = g_vram + ((d.y + y) & (VRAM_H - 1)) * VRAM_W;
        u32 *dst = s_pixels + y * w;
        if (!d.enabled) { memset(dst, 0, w * 4); continue; }
        if (d.rgb24) {
            const u8 *b = (const u8 *)row;
            for (int x = 0; x < w; x++) {
                int o = d.x * 2 + x * 3;
                dst[x] = 0xFF000000u | ((u32)b[(o) & 2047] << 16) | ((u32)b[(o + 1) & 2047] << 8) | b[(o + 2) & 2047];
            }
        } else {
            for (int x = 0; x < w; x++) {
                u16 p = row[(d.x + x) & (VRAM_W - 1)];
                u32 r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
                dst[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
    }
    *ow = w;
    *oh = h;
}

static void dump_bmp(const char *path, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    u32 rowb = (u32)w * 3, pad = (4 - rowb % 4) % 4, size = 54 + (rowb + pad) * h;
    u8 hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &size, 4);
    u32 off = 54, ih = 40, planes_bpp = 1 | (24 << 16);
    memcpy(hdr + 10, &off, 4);
    memcpy(hdr + 14, &ih, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    memcpy(hdr + 26, &planes_bpp, 4);
    fwrite(hdr, 1, 54, f);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            u32 p = s_pixels[y * w + x];
            u8 px[3] = { (u8)p, (u8)(p >> 8), (u8)(p >> 16) };
            fwrite(px, 1, 3, f);
        }
        u8 z[3] = { 0 };
        fwrite(z, 1, pad, f);
    }
    fclose(f);
}

static void dump_vram(const char *path)
{
    for (int y = 0; y < 512; y++)
        for (int x = 0; x < 1024; x++) {
            u16 p = g_vram[y * 1024 + x];
            (void)p;
        }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(g_vram, 2, 1024 * 512, f);
    fclose(f);
}

#ifdef RT_GPU_STATS
extern u32 g_gst[8];
#endif
static int thread_tramp(void *p)
{
    void **a = p;
    ((void (*)(void *))a[0])(a[1]);
    return 0;
}

int host_thread_start(void (*fn)(void *), void *arg, int core)
{
    (void)core;
    static void *args[2];
    args[0] = (void *)fn;
    args[1] = arg;
    return SDL_CreateThread(thread_tramp, "gpu", args) != NULL;
}

void host_yield(void) { SDL_Delay(0); }

int host_test_flag(const char *name)
{
    char v[64] = "RT_";
    for (int i = 0; name[i] && i < 56; i++) { v[3 + i] = (char)(name[i] & ~0x20); v[4 + i] = 0; }
    return getenv(v) != NULL;
}

void *host_mutex_new(void) { return SDL_CreateMutex(); }
void host_mutex_lock(void *m) { SDL_LockMutex(m); }
void host_mutex_unlock(void *m) { SDL_UnlockMutex(m); }
void *host_event_new(void) { return SDL_CreateSemaphore(0); }
void host_event_signal(void *e) { SDL_SemPost(e); }
void host_event_wait(void *e) { SDL_SemWait(e); }

static int io_tramp(void *p)
{
    void **a = p;
    ((void (*)(void *))a[0])(a[1]);
    return 0;
}

int host_io_thread_start(void (*fn)(void *), void *arg)
{
    static void *args[2];
    args[0] = (void *)fn;
    args[1] = arg;
    return SDL_CreateThread(io_tramp, "io", args) != NULL;
}

static FILE *s_pcm;
static u32 s_onehit_until;   /* RT_CHEAT_UNTIL=n: the one-hit cheat stops at frame n */

void host_present(const struct GpuDisplay *dp)
{
    s_frame++;
    if (s_onehit_until && g_frame_count >= s_onehit_until) g_test_onehit = 0;
    if (s_pcm) {
        static s16 buf[735 * 2];
        spu_mix(buf, 735);
        fwrite(buf, sizeof buf, 1, s_pcm);
    }
#ifdef RT_GPU_STATS
    if (s_frame % 300 == 0) {
        rt_log("[gpu] frame %u: tris %u (px %u, slow %u) rects %u (px %u, slow %u)\n", s_frame,
               g_gst[0], g_gst[1], g_gst[7], g_gst[2], g_gst[3], g_gst[4]);
    }
    {
        extern u32 g_tri_kind[64];
        if (s_frame % 300 == 0)
            for (int k = 0; k < 64; k++)
                if (g_tri_kind[k]) rt_log("   kind tex%d gour%d semi%d raw%d depth%d: %u px\n", k & 1, (k >> 1) & 1, (k >> 2) & 1, (k >> 3) & 1, k >> 4, g_tri_kind[k]);
        memset(g_tri_kind, 0, sizeof g_tri_kind);
        extern u32 g_path_px[8];
        if (s_frame % 300 == 0)
            rt_log("   paths: untex %u gour %u/%u(semi) row %u/%u affine %u/%u\n", g_path_px[0], g_path_px[1], g_path_px[2],
                   g_path_px[3], g_path_px[4], g_path_px[5], g_path_px[6]);
        memset(g_path_px, 0, sizeof g_path_px);
        if (s_frame % 300 == 0) {
            rt_log("   time/300f: tri %llu us rect %llu us fill %llu us\n", g_prof[PROF_G_TRI], g_prof[PROF_G_RECT], g_prof[PROF_G_FILL]);
            g_prof[PROF_G_TRI] = g_prof[PROF_G_RECT] = g_prof[PROF_G_FILL] = 0;
        }
    }
    memset(g_gst, 0, sizeof g_gst);
#endif
    int w, h;
    if (s_headless) {
        if (s_dump_every && g_frame_count % s_dump_every == 0) {
            convert_display(dp, &w, &h);
            char p[300];
            snprintf(p, sizeof p, "%s/frames/%05u.bmp", s_data_dir, g_frame_count);
            dump_bmp(p, w, h);
        }
        if (s_max_frames && (int)g_frame_count >= s_max_frames) {
            char p[300];
            snprintf(p, sizeof p, "%s/frames/vram.bin", s_data_dir);
            dump_vram(p);
            rt_log("[host] frame limit reached\n");
            exit(0);
        }
        return;
    }
    convert_display(dp, &w, &h);
    SDL_UpdateTexture(s_tex, NULL, s_pixels, w * 4);
    SDL_Rect src = { 0, 0, w, h };
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, &src, NULL);
    SDL_RenderPresent(s_ren);
}

void host_poll_input(void)
{
    if (s_headless) {
        u16 mask = 0;
        for (int i = 0; i < s_script_n; i++) if (s_script[i].frame <= g_frame_count) mask = s_script[i].mask;
        g_pad_state = (u16)~mask;
        return;
    }
    SDL_Event e;
    while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) s_running = 0;
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    u16 m = 0;
    if (k[SDL_SCANCODE_BACKSPACE]) m |= 1 << 0;
    if (k[SDL_SCANCODE_RETURN]) m |= 1 << 3;
    if (k[SDL_SCANCODE_UP]) m |= 1 << 4;
    if (k[SDL_SCANCODE_RIGHT]) m |= 1 << 5;
    if (k[SDL_SCANCODE_DOWN]) m |= 1 << 6;
    if (k[SDL_SCANCODE_LEFT]) m |= 1 << 7;
    if (k[SDL_SCANCODE_Q]) m |= 1 << 8;
    if (k[SDL_SCANCODE_E]) m |= 1 << 9;
    if (k[SDL_SCANCODE_A]) m |= 1 << 10;
    if (k[SDL_SCANCODE_D]) m |= 1 << 11;
    if (k[SDL_SCANCODE_W]) m |= 1 << 12;   /* triangle */
    if (k[SDL_SCANCODE_X]) m |= 1 << 13;   /* circle */
    if (k[SDL_SCANCODE_Z]) m |= 1 << 14;   /* cross */
    if (k[SDL_SCANCODE_S]) m |= 1 << 15;   /* square */
    g_pad_state = (u16)~m;
}

void host_init(void) {}
void host_shutdown(void) {}

void rt_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("[FATAL] %s\n", buf);
    if (s_headless) {
        char p[300];
        snprintf(p, sizeof p, "%s/frames/vram.bin", s_data_dir);
        dump_vram(p);
    }
    exit(1);
}

int main(int argc, char **argv)
{
    const char *cue = argc > 1 ? argv[1] : "disc/Castlevania - Symphony of the Night (USA).cue";
    if (argc > 2) snprintf(s_data_dir, sizeof s_data_dir, "%s", argv[2]);
    s_headless = getenv("RT_HEADLESS") != NULL;
    s_vclock = getenv("RT_VCLOCK") != NULL;
    g_test_vclock = s_vclock;
    g_test_onehit = getenv("RT_CHEAT_ONEHIT") != NULL;
    g_test_force_skip = getenv("RT_FORCE_SKIP") ? atoi(getenv("RT_FORCE_SKIP")) : 0;
    if (getenv("RT_PRESENT_LAG")) g_present_lag = atoi(getenv("RT_PRESENT_LAG"));
    s_onehit_until = getenv("RT_CHEAT_UNTIL") ? (u32)atoi(getenv("RT_CHEAT_UNTIL")) : 0;
    if (getenv("RT_PCM") && s_headless) s_pcm = fopen(getenv("RT_PCM"), "wb");
    if (!s_headless) _putenv("RT_NO_GPU_THREAD=1");   /* the SDL renderer must stay on this thread */
    s_dump_every = getenv("RT_DUMP") ? atoi(getenv("RT_DUMP")) : 0;
    s_max_frames = getenv("RT_FRAMES") ? atoi(getenv("RT_FRAMES")) : 0;
    if (getenv("RT_INPUT")) {
        char *s = strdup(getenv("RT_INPUT"));
        for (char *t = strtok(s, ","); t && s_script_n < 8192; t = strtok(NULL, ",")) {
            unsigned f, m;
            if (sscanf(t, "%u:%x", &f, &m) == 2) { s_script[s_script_n].frame = f; s_script[s_script_n].mask = (u16)m; s_script_n++; }
        }
    }
    char fr[300];
    snprintf(fr, sizeof fr, "%s/frames", s_data_dir);
    CreateDirectoryA(fr, NULL);
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    if (!s_headless) {
        s_win = SDL_CreateWindow("SymphonyRecomp (3DS runtime, PC test host)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 720, SDL_WINDOW_RESIZABLE);
        s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_PRESENTVSYNC);
        s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 640, 480);
    }
    return rt_main(cue);
}
