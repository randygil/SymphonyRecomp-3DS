#include "rt.h"
#include <stdlib.h>
#ifdef __3DS__
#include <3ds.h>
#endif

/* GPU front end. The game thread runs a state-only "shadow" copy of the GPU
   (gpu_shadow.c) so status/display reads stay synchronous, and queues every
   GP0/GP1 word for the worker copy (gpu_worker.c), which rasterizes on the
   second CPU core and presents finished frames. VRAM reads drain the queue. */

enum { Q_GP0, Q_GP1, Q_PRESENT, Q_BAND, Q_SKIP, Q_LOAD };

u16 g_vram[VRAM_W * VRAM_H] __attribute__((aligned(64)));

void gpus_init(void);
int gpus_is_pal(void);
void gpus_get_display(GpuDisplay *d);
u32 gpus_read_stat(void);
u32 gpus_read_data(void);
void gpus_write_gp0(u32 w);
void gpus_write_gp1(u32 w);
void gpuw_init(void);
void gpuw_write_gp0(u32 w);
void gpuw_write_gp1(u32 w);
void gpus_set_band(int n, const int *lo, const int *hi);
void gpus_set_skip(int s);
u32 gpus_load_block(const u32 *src, u32 n);
u32 gpuw_load_block(const u32 *src, u32 n);
void gpuw_set_skip(int s);
void gpuw_set_band(int n, const int *lo, const int *hi);

/* rows (modulo 256, so both PSX display buffers are split) drawn by the game thread;
   the worker draws the rest. Adjusted each frame from the measured load. */
static int s_split = 112;

static void worker_band(int split)
{
    int lo[2] = { split, 256 + split }, hi[2] = { 256, 512 };
    gpuw_set_band(2, lo, hi);
}

static inline void push(u8 kind, u32 d);

/* the change travels through the queue so both copies switch at the same command */
static void apply_split(void)
{
    int lo[2] = { 0, 256 }, hi[2] = { s_split, 256 + s_split };
    gpus_set_band(2, lo, hi);
    push(Q_BAND, (u32)s_split);
}

#define QN (1u << 19)
#define QM (QN - 1u)

static u32 *q_data;
static u8 *q_kind;
static volatile u32 q_w, q_r;
static volatile u32 s_presents_queued, s_presents_done;
static GpuDisplay s_disp_ring[4];
static int s_threaded;
static volatile int s_worker_run;
static volatile int s_worker_cpu = -1;

static inline void barrier(void) { __sync_synchronize(); }

static void worker(void *arg)
{
    (void)arg;
#ifdef __3DS__
    s_worker_cpu = svcGetProcessorID();
    /* sharing the game's core only adds overhead (happens in emulators that
       keep homebrew threads on core 0) */
    if (s_worker_cpu != 1) return;
#else
    s_worker_cpu = 1;
#endif
    while (s_worker_run) {
        u32 w = q_w;
        barrier();
        if (q_r == w) { host_yield(); continue; }
        u64 t0 = host_ticks_us();
        while (q_r != w) {
            u32 i = q_r & QM;
            u32 d = q_data[i];
            switch (q_kind[i]) {
            case Q_GP0: gpuw_write_gp0(d); break;
            case Q_GP1: gpuw_write_gp1(d); break;
            case Q_BAND: worker_band((int)d); break;
            case Q_SKIP: gpuw_set_skip((int)d); break;
            case Q_LOAD: gpuw_load_block(NULL, d); break;
            case Q_PRESENT:
                host_present(&s_disp_ring[d & 3]);
                barrier();
                s_presents_done++;
                break;
            }
            barrier();
            q_r++;
        }
        g_prof[PROF_WORKER] += host_ticks_us() - t0;
    }
}

static inline void push(u8 kind, u32 d)
{
    while (q_w - q_r >= QN - 1) host_yield();
    u32 i = q_w & QM;
    q_data[i] = d;
    q_kind[i] = kind;
    barrier();
    q_w++;
}

void gpu_front_sync(void)
{
    if (!s_threaded) return;
    while (q_r != q_w) host_yield();
    barrier();
}

void gpu_init(void)
{
    memset(g_vram, 0, sizeof g_vram);
    gpus_init();
    gpuw_init();
    if (!q_data && !getenv("RT_NO_GPU_THREAD")) {
        q_data = malloc(QN * sizeof(u32));
        q_kind = malloc(QN);
        if (q_data && q_kind) {
            s_worker_run = 1;
            if (host_thread_start(worker, NULL, 1)) {
                u64 t0 = host_ticks_us();
                while (s_worker_cpu < 0 && host_ticks_us() - t0 < 500000) host_yield();
                /* on emulators (Azahar) the "core 1" thread shares core 0 and its
                   spin-waits would starve the game, so it only runs on a real core 1 */
                s_threaded = s_worker_cpu == 1;
                rt_log("[gpu] worker cpu %d\n", s_worker_cpu);
            }
            if (s_threaded) apply_split();
            else s_worker_run = 0;
        }
        rt_log("[gpu] worker thread: %s\n", s_threaded ? "yes" : "no");
    }
}

int gpu_is_pal(void) { return gpus_is_pal(); }
void gpu_get_display(GpuDisplay *d) { gpus_get_display(d); }
u32 gpu_read_stat(void) { return gpus_read_stat(); }
u32 gpu_read_data(void) { return gpus_read_data(); }

void gpu_write_gp0(u32 w)
{
    if (s_threaded) push(Q_GP0, w);
    gpus_write_gp0(w);
}

/* DMA to GP0: image data goes straight to VRAM in row segments */
void gpu_write_gp0_block(const u32 *src, u32 n)
{
    while (n) {
        u32 k = gpus_load_block(src, n);
        if (k) {
            if (s_threaded) push(Q_LOAD, k);
            src += k;
            n -= k;
            continue;
        }
        gpu_write_gp0(*src++);
        n--;
    }
}

void gpu_write_gp1(u32 w)
{
    if (s_threaded) push(Q_GP1, w);
    gpus_write_gp1(w);
}

/* called once per VSync on the game thread: wait for the worker's half of the
   frame, then present (both halves are complete at this point) */
void gpu_present(void)
{
    GpuDisplay d;
    gpus_get_display(&d);
    if (s_threaded) {
        u64 t0 = host_ticks_us();
        gpu_front_sync();
        u64 waited = host_ticks_us() - t0;
        g_prof[PROF_SYNC] += waited;
        /* balance the bands: if we had to wait for the worker, draw more rows here */
        if (waited > 1000 && s_split < 232) { s_split += 4; apply_split(); }
        else if (waited < 150 && s_split > 16) { s_split -= 4; apply_split(); }
    }
    host_present(&d);
}

#ifdef RT_GPU_STATS
u32 g_gst[8];
u32 g_tri_kind[64];
u32 g_path_px[8];
#endif

static int s_skip;

void gpu_set_skip(int skip)
{
    if (skip == s_skip) return;
    s_skip = skip;
    gpus_set_skip(skip);
    if (s_threaded) push(Q_SKIP, (u32)skip);
}

int gpu_skipping(void) { return s_skip; }
