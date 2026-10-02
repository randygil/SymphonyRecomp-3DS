/* internal runtime interfaces */
#ifndef RT_H
#define RT_H

#include "recomp.h"
#include <stdio.h>
#include <string.h>

extern Cpu g_cpu;

void rt_log(const char *fmt, ...);
void rt_fatal(const char *fmt, ...) __attribute__((noreturn));

/* ---- host layer (host_3ds.c / host_pc.c) ---- */
void host_init(void);
int host_running(void);
struct GpuDisplay;
void host_present(const struct GpuDisplay *d);   /* show a display area (may run on the GPU thread) */
int host_thread_start(void (*fn)(void *), void *arg, int core);
void host_yield(void);
void host_poll_input(void);
u64 host_ticks_us(void);
void host_sleep_us(u32 us);
const char *host_data_dir(void);    /* where disc + saves live */
void host_audio_init(void);
void host_shutdown(void);
/* minimal sync for the disc I/O thread; events are auto-reset */
void *host_mutex_new(void);
void host_mutex_lock(void *m);
void host_mutex_unlock(void *m);
void *host_event_new(void);
void host_event_signal(void *e);
void host_event_wait(void *e);
int host_io_thread_start(void (*fn)(void *), void *arg);   /* same core as the game, higher priority */

/* PSX pad state, active low like the hardware */
extern u16 g_pad_state;

/* ---- GPU ---- */
#define VRAM_W 1024
#define VRAM_H 512
extern u16 g_vram[VRAM_W * VRAM_H];

typedef struct GpuDisplay {
    int x, y, w, h;
    int rgb24, enabled, pal, interlace;
} GpuDisplay;

void gpu_init(void);
void gpu_write_gp0(u32 w);
void gpu_write_gp0_block(const u32 *src, u32 n);
void gpu_write_gp1(u32 w);
u32 gpu_read_data(void);
u32 gpu_read_stat(void);
void gpu_get_display(GpuDisplay *d);
int gpu_is_pal(void);
void gpu_present(void);
void gpu_set_skip(int skip);
int gpu_skipping(void);
extern int g_frameskip_max;
/* test aids, set by the hosts: g_test_onehit leaves enemies at 1 HP each frame;
   g_test_vclock paces CD streaming by frames instead of the wall clock, so
   scripted input runs the same way on PC and 3DS */
extern int g_test_onehit, g_test_vclock;
u64 rt_stream_clock_us(void);

/* ---- SPU / XA ---- */
void spu_init(void);
u16 spu_read16(u32 phys);
void spu_write16(u32 phys, u16 v);
void spu_dma_write(const u8 *data, u32 len);
void spu_dma_read(u8 *data, u32 len);
void spu_set_cd_mix(int ll, int lr, int rr, int rl);
void spu_mix(s16 *dst, int frames);   /* stereo interleaved, 44100 Hz */
void spu_lock(void);
void spu_unlock(void);

void xa_reset(void);
void xa_decode_sector(const u8 *sec, int off, u8 coding);
int xa_buffered(void);
int xa_next(s16 *l, s16 *r);

/* ---- MDEC ---- */
void mdec_init(void);
u32 mdec_read_data(void);
u32 mdec_read_status(void);
void mdec_write0(u32 v);
void mdec_write_ctrl(u32 v);
int mdec_read_block(u32 *dst, int n);
int mdec_write_block(const u32 *src, int n);

/* ---- timers ---- */
int timers_read(u32 phys, u32 *out);
int timers_write(u32 phys, u32 v);

/* ---- DMA ---- */
u32 dma_read_dicr(void);
void dma_write_dicr(u32 v);
void dma_run(int ch, u32 madr, u32 bcr, u32 chcr);

/* ---- disc ---- */
int disc_open(const char *cue_path);
int disc_read(int lba, int size, u8 *out);   /* returns bytes read, zero-fills */
int disc_data_sectors(void);
int disc_first_track(void);
int disc_last_track(void);
int disc_has_tracks(void);
int disc_track_start(int track, int *lba);
int disc_leadout(void);

int iso_locate(const char *path, int *lba, u32 *size);   /* full path or basename search */
int iso_find_file(const char *name, char *out_path, int out_len);
u8 *iso_read_file(const char *path, u32 *size);             /* malloc'd */

/* ---- CD controller (hardware registers + BIOS async) ---- */
void cdc_init(void);
u8 cdc_read(u32 phys);
void cdc_write(u32 phys, u8 v);
void cdc_dma_read(u32 addr, u32 bytes);
void cdc_load_to_memory(const char *path, u32 address, int offset, int length);
u8 cdc_drive_status(void);
void cdc_read_sector_data(int lba, u8 *out2048);
void cdc_async_seekl(u8 mm, u8 ss, u8 ff);
void cdc_async_get_status(void);
void cdc_async_set_mode(u8 mode);
void cdc_async_read_sector(u32 count, u32 dst, u32 mode);

/* ---- libcd / stream ---- */
void libcd_reset(void);
void libcd_tick(void);
int libcd_current_lba(void);
double libcd_sectors_per_second(void);
void libcdstream_reset(void);
void libcdstream_on_read(int lba);
void libcdstream_on_stop(void);
void libcdstream_pump(void);
void libpad_refresh(void);

/* ---- BIOS ---- */
void bios_init(void);
int bios_try_dispatch(Cpu *restrict c, u32 addr);
void bios_refresh_pad(void);
void bios_deliver_event(u32 cls, u32 spec);
extern u32 g_intr_env_addr;

/* ---- interrupts ---- */
void irq_deliver(int irq);

/* ---- memory card ---- */
typedef struct MemCard {
    u8 d[0x20000];
    char path[256];
    int enabled;
} MemCard;
extern MemCard g_card[2];
void mc_load(MemCard *mc, const char *path);
void mc_flush(MemCard *mc);
void mc_format(MemCard *mc);
int mc_find(MemCard *mc, const char *name);
int mc_chain(MemCard *mc, int first, int *out, int max);
int mc_create(MemCard *mc, const char *name, int blocks);
int mc_file_size(MemCard *mc, int b);
void mc_delete(MemCard *mc, const char *name);
void mc_frame_read(MemCard *mc, int frame, u8 *dst);
void mc_frame_write(MemCard *mc, int frame, const u8 *src);
u8 mc_read_byte(MemCard *mc, const int *chain, int n, int pos);
void mc_write_byte(MemCard *mc, const int *chain, int n, int pos, u8 v);
int mc_match(MemCard *mc, const char *pattern, char names[][21], int *sizes, int max);

/* ---- frame ---- */
void rt_present_frame(void);
extern u32 g_frame_count;
enum { PROF_GPU, PROF_PRESENT, PROF_IDLE, PROF_SPU_WAIT, PROF_CD, PROF_MDEC, PROF_SPU_MIX, PROF_WORKER, PROF_SYNC, PROF_G_TRI, PROF_G_RECT, PROF_G_FILL, PROF_G_SETUP, PROF_N };
extern u32 g_tri_calls, g_tri_spans;
extern u64 g_prof[PROF_N];

#endif
