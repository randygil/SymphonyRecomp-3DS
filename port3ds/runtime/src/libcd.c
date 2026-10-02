#include "rt.h"
#include <stdlib.h>

/* port of RecompOne.Runtime.Sdk.LibCd / LibCdStream. The C# background
   threads (XA pump, stream reader) run here once per frame instead. */

enum {
    CdlNop = 0x01, CdlSetloc = 0x02, CdlPlay = 0x03, CdlForward = 0x04, CdlBackward = 0x05,
    CdlReadN = 0x06, CdlStandby = 0x07, CdlStop = 0x08, CdlPause = 0x09, CdlInit = 0x0A,
    CdlMute = 0x0B, CdlDemute = 0x0C, CdlSetfilter = 0x0D, CdlSetmode = 0x0E, CdlGetparam = 0x0F,
    CdlGetlocL = 0x10, CdlGetlocP = 0x11, CdlGetTN = 0x13, CdlGetTD = 0x14, CdlSeekL = 0x15,
    CdlSeekP = 0x16, CdlReadS = 0x1B
};
#define COMPLETE 2
#define DATAREADY 1
#define DISKERROR 5
#define ST_MOTOR 0x02
#define ST_READ 0x20
#define ST_PLAY 0x80

static u8 s_status = ST_MOTOR, s_mode, s_com;
static u8 s_pos[4];
static u8 s_res[8];
static int s_last_intr = COMPLETE;
static u32 s_cb_sync, s_cb_ready, s_cb_data;
static int s_read_active, s_xa_active;
static u8 s_ffile, s_fchan;
static int s_pumping;
static u32 s_xa_stat[4], s_xa_frames;

static int bcd(u8 b) { return (b >> 4) * 10 + (b & 0xF); }
static u8 tobcd(int n) { return (u8)(((n / 10) << 4) + (n % 10)); }
static int pos_to_int(const u8 *p) { return (bcd(p[0]) * 60 + bcd(p[1])) * 75 + bcd(p[2]) - 150; }
static void int_to_pos(int i, u8 *mm, u8 *ss, u8 *ff)
{
    i += 150;
    *ff = tobcd(i % 75);
    *ss = tobcd(i / 75 % 60);
    *mm = tobcd(i / 75 / 60);
}

int libcd_current_lba(void) { return pos_to_int(s_pos); }
double libcd_sectors_per_second(void) { return (s_mode & 0x80) ? 150.0 : 75.0; }
static void advance(int n) { int_to_pos(pos_to_int(s_pos) + n, &s_pos[0], &s_pos[1], &s_pos[2]); }
static int is_audio(int lba) { return lba >= disc_data_sectors(); }

static void write_result(u32 addr) { for (int i = 0; i < 8; i++) WR8(addr + i, s_res[i]); }

static void set_error(u8 err, u8 extra)
{
    s_last_intr = DISKERROR;
    s_res[0] = (u8)(s_status | extra);
    s_res[1] = err;
    memset(s_res + 2, 0, 6);
}

static void reset_state(void)
{
    libcdstream_on_stop();
    s_status = ST_MOTOR;
    s_mode = s_com = 0;
    s_last_intr = COMPLETE;
    s_cb_sync = s_cb_ready = s_cb_data = 0;
    s_read_active = s_xa_active = 0;
    s_ffile = s_fchan = 0;
    memset(s_pos, 0, sizeof s_pos);
    memset(s_res, 0, sizeof s_res);
    spu_set_cd_mix(0x80, 0, 0x80, 0);
    dispatch_clear_pending();
}

void libcd_reset(void) { reset_state(); }

static void pump_ready(int max)
{
    if (s_pumping || !s_read_active || !s_cb_ready || s_cb_data) return;
    Cpu *c = &g_cpu;
    s_pumping = 1;
    Cpu snap = *c;
    for (int i = 0; i < max && s_read_active && s_cb_ready; i++) {
        s_last_intr = DATAREADY;
        c->r[4] = DATAREADY;
        c->r[5] = 0;
        dispatch_call(c, s_cb_ready);
        advance(1);
        dispatch_load_by_lba(libcd_current_lba());
    }
    memcpy(c->r, snap.r, sizeof snap.r);
    c->hi = snap.hi;
    c->lo = snap.lo;
    s_pumping = 0;
}

void libcd_tick(void)
{
    int xa_mode = (s_mode & 0x40) != 0;
    /* XA audio pump */
    if (s_read_active && xa_mode) {
        u8 sec[2336];
        int scanned = 0, use_filter = (s_mode & 0x08) != 0;
        while (s_read_active && xa_buffered() < 4096 && scanned < 32) {
            int lba = libcd_current_lba();
            if (lba < 0) break;
            disc_read(lba, 2336, sec);
            advance(1);
            scanned++;
            if (!(sec[2] & 0x04)) { s_xa_stat[1]++; continue; }
            if (use_filter && (sec[0] != s_ffile || sec[1] != s_fchan)) { s_xa_stat[2]++; continue; }
            s_xa_stat[3]++;
            xa_decode_sector(sec, 8, sec[3]);
        }
        s_xa_stat[0] += scanned;
        if (getenv("RT_XA_DEBUG") && ++s_xa_frames % 120 == 0) {
            rt_log("[xa] lba %d scanned %u nonaudio %u filtered %u accepted %u buffered %d filter %u/%u mode %02X\n",
                   libcd_current_lba(), s_xa_stat[0], s_xa_stat[1], s_xa_stat[2], s_xa_stat[3], xa_buffered(),
                   s_ffile, s_fchan, s_mode);
            memset(s_xa_stat, 0, sizeof s_xa_stat);
        }
        return;
    }
    if (!s_read_active || (!s_cb_data && !s_cb_ready)) return;
    Cpu *c = &g_cpu;
    Cpu snap = *c;
    if (s_cb_data) {
        for (int guard = 0; s_cb_data && guard < 4096; guard++) {
            s_last_intr = DATAREADY;
            if (s_cb_ready) { c->r[4] = DATAREADY; c->r[5] = 0; dispatch_call(c, s_cb_ready); }
            advance(1);
            dispatch_load_by_lba(libcd_current_lba());
            if (s_cb_data) { c->r[4] = DATAREADY; c->r[5] = 0; dispatch_call(c, s_cb_data); }
        }
    } else {
        memcpy(c->r, snap.r, sizeof snap.r);
        c->hi = snap.hi;
        c->lo = snap.lo;
        pump_ready(400000);
        return;
    }
    memcpy(c->r, snap.r, sizeof snap.r);
    c->hi = snap.hi;
    c->lo = snap.lo;
}

static int exec_command(u8 com, u32 param, u32 result)
{
    s_com = com;
    s_last_intr = COMPLETE;
    switch (com) {
    case CdlSetloc: if (param) for (int i = 0; i < 4; i++) s_pos[i] = (u8)RD8(param + i); break;
    case CdlSetmode: if (param) s_mode = (u8)RD8(param); break;
    case CdlSetfilter: if (param) { s_ffile = (u8)RD8(param); s_fchan = (u8)RD8(param + 1); } break;
    case CdlReadN:
        if (is_audio(libcd_current_lba()) && !(s_mode & 1)) {
            s_read_active = 0;
            set_error(0x40, 1);
            if (result) write_result(result);
            return DISKERROR;
        }
        s_read_active = 1;
        s_status = ST_MOTOR | ST_READ;
        dispatch_load_by_lba(libcd_current_lba());
        break;
    case CdlReadS:
        if (is_audio(libcd_current_lba()) && !(s_mode & 1)) {
            s_xa_active = s_read_active = 0;
            set_error(0x40, 1);
            if (result) write_result(result);
            return DISKERROR;
        }
        s_xa_active = 1;
        s_read_active = 0;
        s_status = ST_MOTOR | ST_READ;
        libcdstream_on_read(libcd_current_lba());
        break;
    case CdlPlay:
        s_read_active = 0;
        s_status = ST_MOTOR | ST_PLAY;
        break;
    case CdlGetparam:
        s_res[0] = s_status; s_res[1] = s_mode; s_res[2] = 0; s_res[3] = s_ffile; s_res[4] = s_fchan;
        s_res[5] = s_res[6] = s_res[7] = 0;
        if (result) write_result(result);
        return 0;
    case CdlGetlocL:
        s_res[0] = s_pos[0]; s_res[1] = s_pos[1]; s_res[2] = s_pos[2];
        s_res[3] = s_mode; s_res[4] = s_ffile; s_res[5] = s_fchan; s_res[6] = s_res[7] = 0;
        if (result) write_result(result);
        return 0;
    case CdlGetlocP: {
        int abs = libcd_current_lba() + 150, track = 1, rel = abs - 150;
        for (int t = disc_first_track(); t <= disc_last_track(); t++) {
            int tl;
            if (disc_track_start(t, &tl) && abs >= tl) { track = t; rel = abs - tl; }
        }
        u8 mm, ss, ff;
        int_to_pos(rel, &mm, &ss, &ff);
        s_res[0] = tobcd(track); s_res[1] = 1; s_res[2] = mm; s_res[3] = ss; s_res[4] = ff;
        s_res[5] = s_pos[0]; s_res[6] = s_pos[1]; s_res[7] = s_pos[2];
        if (result) write_result(result);
        return 0;
    }
    case CdlGetTN:
        memset(s_res, 0, 8);
        s_res[0] = s_status; s_res[1] = tobcd(disc_first_track()); s_res[2] = tobcd(disc_last_track());
        if (result) write_result(result);
        return 0;
    case CdlGetTD: {
        int track = param ? bcd((u8)RD8(param)) : 0, tl;
        int lba = (track == 0 || !disc_track_start(track, &tl)) ? disc_leadout() : tl;
        if (lba < 0) lba = 0;
        memset(s_res, 0, 8);
        s_res[0] = s_status; s_res[1] = tobcd(lba / 75 / 60); s_res[2] = tobcd(lba / 75 % 60);
        if (result) write_result(result);
        return 0;
    }
    case CdlPause: case CdlStop: case CdlInit:
        libcdstream_on_stop();
        s_read_active = s_xa_active = 0;
        s_status = ST_MOTOR;
        /* the overlay just read stays pending: the game copies it into place
           after stopping the drive, and the dispatcher verifies it on first call */
        break;
    case CdlSeekL:
        if (is_audio(libcd_current_lba())) {
            set_error(0x04, 0x04);
            if (result) write_result(result);
            return 0;
        }
        break;
    default: break;
    }
    s_res[0] = s_status;
    memset(s_res + 1, 0, 7);
    if (result) write_result(result);
    return 0;
}

static int needs_loc(u8 com) { return com == CdlPlay || com == CdlReadN || com == CdlSeekL || com == CdlSeekP || com == CdlReadS; }

static int command_wait(u8 com, u32 param, u32 result)
{
    if (param && needs_loc(com)) exec_command(CdlSetloc, param, 0);
    return exec_command(com, param, result);
}

static int sync_result(u32 result) { if (result) write_result(result); return s_last_intr; }

void sdk_LibCd_CdInit(Cpu *restrict c) { reset_state(); s_last_intr = COMPLETE; s_res[0] = s_status; c->r[2] = 0; }
void sdk_LibCd_CdReset(Cpu *restrict c) { reset_state(); s_last_intr = COMPLETE; s_res[0] = s_status; c->r[2] = 1; }
void sdk_LibCd_CdControl(Cpu *restrict c) { c->r[2] = command_wait((u8)c->r[4], c->r[5], c->r[6]) == 0 ? 1u : 0u; }
void sdk_LibCd_CdControlF(Cpu *restrict c) { c->r[2] = command_wait((u8)c->r[4], c->r[5], 0) == 0 ? 1u : 0u; }

void sdk_LibCd_CdControlB(Cpu *restrict c)
{
    u32 result = c->r[6];
    if (command_wait((u8)c->r[4], c->r[5], c->r[6]) != 0) { c->r[2] = 0; return; }
    pump_ready(1);
    c->r[2] = sync_result(result) == COMPLETE ? 1u : 0u;
}

void sdk_LibCd_CdSync(Cpu *restrict c)
{
    u32 result = c->r[5];
    pump_ready(1);
    c->r[2] = (u32)sync_result(result);
}

void sdk_LibCd_CdReady(Cpu *restrict c)
{
    u32 result = c->r[5];
    pump_ready(1);
    if (result) write_result(result);
    c->r[2] = (u32)s_last_intr;
}

void sdk_LibCd_CdRead(Cpu *restrict c)
{
    int sectors = (int)c->r[4];
    u32 buf = c->r[5];
    s_mode = (u8)c->r[6];
    int lba = libcd_current_lba();
    if (is_audio(lba) && !(s_mode & 1)) { set_error(0x40, 1); c->r[2] = 1; return; }
    int size = (s_mode & 0x20) ? 2340 : (s_mode & 0x10) ? 2328 : 2048;
    u8 sec[2352];
    dispatch_load_by_lba(lba);
    for (int i = 0; i < sectors; i++) {
        dispatch_load_by_lba(lba + i);
        disc_read(lba + i, size, sec);
        mem_write_block(buf + (u32)(i * size), sec, (u32)size);
    }
    s_last_intr = COMPLETE;
    c->r[2] = 1;
}

void sdk_LibCd_CdReadSync(Cpu *restrict c)
{
    if (c->r[5]) write_result(c->r[5]);
    c->r[2] = s_last_intr == DISKERROR ? 0xFFFFFFFFu : 0u;
}

void sdk_LibCd_CdGetSector(Cpu *restrict c)
{
    u32 madr = c->r[4];
    u32 words = c->r[5];
    u8 sec[2048];
    cdc_read_sector_data(libcd_current_lba(), sec);
    u32 bytes = words * 4 < 2048 ? words * 4 : 2048;
    mem_write_block(madr, sec, bytes);
    c->r[2] = 1;
}

void sdk_LibCd_CdDataSync(Cpu *restrict c) { c->r[2] = 0; }

void sdk_LibCd_CdSearchFile(Cpu *restrict c)
{
    u32 fp = c->r[4];
    char name[128];
    int i = 0;
    for (; i < 127; i++) { u8 b = (u8)RD8(c->r[5] + i); if (!b) break; name[i] = (char)b; }
    name[i] = 0;
    int lba;
    u32 size;
    if (!iso_locate(name, &lba, &size)) {
        rt_log("[libcd] CdSearchFile '%s' not found\n", name);
        c->r[2] = 0;
        return;
    }
    u8 mm, ss, ff;
    int_to_pos(lba, &mm, &ss, &ff);
    WR8(fp + 0, mm);
    WR8(fp + 1, ss);
    WR8(fp + 2, ff);
    WR8(fp + 3, 0);
    WR32(fp + 4, size);
    const char *b = strrchr(name, '/');
    const char *b2 = strrchr(name, '\\');
    if (b2 && (!b || b2 > b)) b = b2;
    b = b ? b + 1 : name;
    int bl = (int)strlen(b);
    for (int k = 0; k < 16; k++) WR8(fp + 8 + k, k < bl ? (u8)b[k] : 0);
    c->r[2] = fp;
}

void sdk_LibCd_CdSyncCallback(Cpu *restrict c) { c->r[2] = s_cb_sync; s_cb_sync = c->r[4]; }
void sdk_LibCd_CdReadyCallback(Cpu *restrict c) { c->r[2] = s_cb_ready; s_cb_ready = c->r[4]; }
void sdk_LibCd_CdReadCallback(Cpu *restrict c) { c->r[2] = s_cb_data; s_cb_data = c->r[4]; }
void sdk_LibCd_CdDataCallback(Cpu *restrict c) { c->r[2] = s_cb_data; s_cb_data = c->r[4]; }
void sdk_LibCd_CdStatus(Cpu *restrict c) { pump_ready(1); c->r[2] = s_status; }
void sdk_LibCd_CdMode(Cpu *restrict c) { c->r[2] = s_mode; }
void sdk_LibCd_CdLastCom(Cpu *restrict c) { c->r[2] = s_com; }

void sdk_LibCd_CdMix(Cpu *restrict c)
{
    u32 a = c->r[4];
    if (a) spu_set_cd_mix((int)RD8(a), (int)RD8(a + 1), (int)RD8(a + 2), (int)RD8(a + 3));
    c->r[2] = 1;
}

/* ---------------- LibCdStream ---------------- */
#define HDR_SIZE 32
#define SLOT_DATA 2016
#define VIDEO_MAGIC 0x0160
#define PRIME_FRAMES 2
#define MAX_SLOTS 64

static int st_in_use, st_active, st_reading;
static u32 st_status_base, st_data_base;
static int st_slots;
static int st_pending_lba = -1, st_lba = -1, st_start_lba;
static u64 st_clock_us;
static int st_write_idx, st_primed;
static u8 st_busy[MAX_SLOTS];
static int st_ready_start[MAX_SLOTS], st_ready_n[MAX_SLOTS];
static int st_ready_r, st_ready_c;
static int st_prev_start = -1, st_prev_n;

static void st_reset_ring(void)
{
    st_primed = 0;
    st_write_idx = 0;
    st_prev_start = -1;
    st_prev_n = 0;
    st_ready_r = st_ready_c = 0;
    memset(st_busy, 0, sizeof st_busy);
    for (int i = 0; i < st_slots; i++) WR16(st_status_base + (u32)(i * HDR_SIZE), 0);
}

void libcdstream_reset(void)
{
    st_in_use = st_active = st_reading = 0;
    st_status_base = st_data_base = 0;
    st_slots = 0;
    st_pending_lba = st_lba = -1;
    st_start_lba = 0;
    st_write_idx = 0;
    st_prev_start = -1;
    st_prev_n = 0;
    st_ready_r = st_ready_c = 0;
}

void sdk_LibCdStream_StSetRing(Cpu *restrict c)
{
    st_in_use = 1;
    st_status_base = c->r[4];
    st_slots = (int)c->r[5];
    if (st_slots > MAX_SLOTS) st_slots = MAX_SLOTS;
    st_data_base = st_status_base + (u32)(st_slots * HDR_SIZE);
    st_reset_ring();
}

void sdk_LibCdStream_StClearRing(Cpu *restrict c) { st_reset_ring(); c->r[2] = 0; }
void sdk_LibCdStream_StUnSetRing(Cpu *restrict c) { (void)c; st_active = 0; st_reading = 0; }

void sdk_LibCdStream_StSetStream(Cpu *restrict c)
{
    (void)c;
    st_lba = -1;
    st_reset_ring();
    xa_reset();
    st_active = 1;
}

void sdk_LibCdStream_StSetMask(Cpu *restrict c) { c->r[2] = 0; }

void sdk_LibCdStream_StGetNext(Cpu *restrict c)
{
    if (!st_active) { c->r[2] = 1; return; }
    if (st_prev_start >= 0) {
        for (int i = 0; i < st_prev_n; i++) st_busy[st_prev_start + i] = 0;
        st_prev_start = -1;
    }
    if (!st_ready_c) {
        libcdstream_pump();
        if (!st_ready_c) {
            extern u64 g_test_spin_us;
            if (g_test_vclock) g_test_spin_us += 500;
            c->r[2] = 1;
            return;
        }
    }
    int start = st_ready_start[st_ready_r], n = st_ready_n[st_ready_r];
    st_ready_r = (st_ready_r + 1) % MAX_SLOTS;
    st_ready_c--;
    WR32(c->r[4], st_data_base + (u32)(start * SLOT_DATA));
    WR32(c->r[5], st_status_base + (u32)(start * HDR_SIZE));
    st_prev_start = start;
    st_prev_n = n;
    c->r[2] = 0;
}

void sdk_LibCdStream_StFreeRing(Cpu *restrict c) { c->r[2] = 0; }
void sdk_LibCdStream_StGetBackloc(Cpu *restrict c) { c->r[2] = 0xFFFFFFFFu; }

void libcdstream_on_read(int lba)
{
    if (!st_in_use) return;
    st_pending_lba = lba;
    st_reading = 1;
}

void libcdstream_on_stop(void) { st_reading = 0; }

static u16 rd16(const u8 *b, int o) { return (u16)(b[o] | (b[o + 1] << 8)); }

static int collect_frame(int start, int n)
{
    int collected = 0, lba = st_lba;
    u8 sec[2336];
    while (collected < n) {
        disc_read(lba, 2336, sec);
        lba++;
        if (sec[2] & 0x04) { xa_decode_sector(sec, 8, sec[3]); continue; }
        if (rd16(sec, 8) != VIDEO_MAGIC) continue;
        u32 hdr = st_status_base + (u32)((start + collected) * HDR_SIZE);
        u32 dat = st_data_base + (u32)((start + collected) * SLOT_DATA);
        mem_write_block(hdr, sec + 8, HDR_SIZE);
        mem_write_block(dat, sec + 8 + HDR_SIZE, SLOT_DATA);
        collected++;
        if (lba - st_lba > 4096) return 0;
    }
    st_lba = lba;
    return 1;
}

void libcdstream_pump(void)
{
    u8 sec[2336];
    for (int iter = 0; iter < 256; iter++) {
        if (!st_active || !st_reading || st_slots <= 0) return;
        if (st_lba < 0) {
            st_lba = st_pending_lba >= 0 ? st_pending_lba : libcd_current_lba();
            st_start_lba = st_lba;
            st_clock_us = rt_stream_clock_us();
        }
        disc_read(st_lba, 2336, sec);
        if (sec[2] & 0x04) { xa_decode_sector(sec, 8, sec[3]); st_lba++; continue; }
        if (rd16(sec, 8) != VIDEO_MAGIC || rd16(sec, 12) != 0) { st_lba++; if (st_lba >= disc_data_sectors()) { st_reading = 0; return; } continue; }
        int n = rd16(sec, 14);
        if (n <= 0 || n > st_slots) { st_lba++; continue; }
        if (st_primed) {
            double delivered = (double)(rt_stream_clock_us() - st_clock_us) / 1e6 * libcd_sectors_per_second();
            if ((st_lba - st_start_lba) + n > delivered) return;
        }
        if (st_write_idx + n > st_slots) st_write_idx = 0;
        int start = st_write_idx;
        for (int i = 0; i < n; i++) if (st_busy[start + i]) return;
        if (st_ready_c >= MAX_SLOTS) return;
        if (!collect_frame(start, n)) { st_lba++; continue; }
        for (int i = 0; i < n; i++) st_busy[start + i] = 1;
        int q = (st_ready_r + st_ready_c) % MAX_SLOTS;
        st_ready_start[q] = start;
        st_ready_n[q] = n;
        st_ready_c++;
        st_write_idx = start + n;
        if (!st_primed && st_ready_c >= PRIME_FRAMES) {
            st_primed = 1;
            st_start_lba = st_lba;
            st_clock_us = rt_stream_clock_us();
        }
    }
}
