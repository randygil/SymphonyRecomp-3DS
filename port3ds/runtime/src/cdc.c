#include "rt.h"
#include <stdlib.h>

/* CD-ROM controller registers (port of RecompOne.Runtime.Cdrom.CdController) */

static u8 s_index;
static u8 s_param[16];
static int s_param_n;
static u8 s_resp[16];
static int s_resp_r, s_resp_n;

typedef struct { u8 type; u8 n; u8 b[8]; } PendIrq;
#define PQ 16
static PendIrq s_pq[PQ];
static int s_pq_r, s_pq_n;

static u8 s_irq_flags, s_last_irq;
static int s_seek_lba, s_last_read_lba;
static u8 s_data[2048];
static int s_data_pos, s_data_ready = 0;
static int s_reading, s_stream_pending, s_playing, s_seeking;
static u8 s_mode, s_filter_file, s_filter_chan;

void cdc_init(void)
{
    s_index = 0;
    s_param_n = 0;
    s_resp_n = s_resp_r = 0;
    s_pq_r = s_pq_n = 0;
    s_irq_flags = s_last_irq = 0;
    s_seek_lba = s_last_read_lba = 0;
    s_data_pos = 0;
    s_data_ready = 0;
    s_reading = s_stream_pending = s_playing = s_seeking = 0;
    s_mode = s_filter_file = s_filter_chan = 0;
}

static u8 drive_status(void)
{
    u8 s = 0x02;
    if (s_reading) s |= 0x20;
    if (s_seeking) s |= 0x40;
    if (s_playing) s |= 0x80;
    return s;
}

u8 cdc_drive_status(void) { return drive_status(); }

static void set_in_interrupt(u16 v) { if (g_intr_env_addr) WR16(g_intr_env_addr, v); }

static void deliver(u8 type, const u8 *b, int n)
{
    memcpy(s_resp, b, n);
    s_resp_n = n;
    s_resp_r = 0;
    s_irq_flags = type;
    s_last_irq = type;
    set_in_interrupt(1);
}

static void queue_irq(u8 type, const u8 *b, int n)
{
    if (s_irq_flags == 0 && s_pq_n == 0) { deliver(type, b, n); return; }
    if (s_pq_n >= PQ) return;
    PendIrq *p = &s_pq[(s_pq_r + s_pq_n) % PQ];
    p->type = type;
    p->n = (u8)n;
    memcpy(p->b, b, n);
    s_pq_n++;
}

#define Q1(t, a) do { u8 _b[1] = { (a) }; queue_irq((t), _b, 1); } while (0)

static void after_ack(void)
{
    if (s_pq_n > 0) {
        PendIrq *p = &s_pq[s_pq_r];
        s_pq_r = (s_pq_r + 1) % PQ;
        s_pq_n--;
        deliver(p->type, p->b, p->n);
        return;
    }
    if (s_reading && s_last_irq == 1) s_stream_pending = 1;
    set_in_interrupt(0);
}

static void read_next_sector(void)
{
    disc_read(s_seek_lba, 2048, s_data);
    s_last_read_lba = s_seek_lba;
    s_seek_lba++;
}

static int bcd2int(u8 b) { return (b >> 4) * 10 + (b & 0xF); }
static u8 int2bcd(int n) { return (u8)(((n / 10) << 4) | (n % 10)); }
static int bcd_to_lba(u8 mm, u8 ss, u8 ff) { return (bcd2int(mm) * 60 + bcd2int(ss)) * 75 + bcd2int(ff) - 150; }

static void lba_to_msf(int lba, u8 *mm, u8 *ss, u8 *ff)
{
    if (lba < 0) lba = 0;
    *ff = int2bcd(lba % 75);
    *ss = int2bcd(lba / 75 % 60);
    *mm = int2bcd(lba / 75 / 60);
}

static int is_audio(int lba) { return lba >= disc_data_sectors(); }

static void exec_cmd(u8 cmd)
{
    u8 p[16];
    int pn = s_param_n;
    memcpy(p, s_param, pn);
    s_param_n = 0;
    u8 st;
    switch (cmd) {
    case 0x01: Q1(3, drive_status()); break;
    case 0x02:
        if (pn >= 3) s_seek_lba = bcd_to_lba(p[0], p[1], p[2]);
        Q1(3, drive_status());
        break;
    case 0x03: {
        s_reading = 0;
        s_playing = 1;
        int tl;
        if (pn == 1 && p[0] && disc_track_start(bcd2int(p[0]), &tl)) s_seek_lba = tl - 150;
        Q1(3, drive_status());
        break;
    }
    case 0x04: case 0x05: case 0x0B: case 0x0C: Q1(3, drive_status()); break;
    case 0x06: case 0x1B:
        if (is_audio(s_seek_lba) && !(s_mode & 1)) {
            s_reading = 0;
            u8 b[2] = { (u8)(drive_status() | 1), 0x40 };
            queue_irq(5, b, 2);
            break;
        }
        s_reading = 1;
        s_playing = 0;
        read_next_sector();
        Q1(3, drive_status());
        Q1(1, drive_status());
        break;
    case 0x07: Q1(3, drive_status()); Q1(2, drive_status()); break;
    case 0x08: case 0x09:
        s_reading = s_playing = s_stream_pending = 0;
        Q1(3, drive_status());
        Q1(2, drive_status());
        break;
    case 0x0A:
        s_mode = 0;
        s_reading = s_playing = s_stream_pending = 0;
        Q1(3, drive_status());
        Q1(2, drive_status());
        break;
    case 0x0D:
        if (pn >= 2) { s_filter_file = p[0]; s_filter_chan = p[1]; }
        Q1(3, drive_status());
        break;
    case 0x0E:
        if (pn >= 1) s_mode = p[0];
        Q1(3, drive_status());
        break;
    case 0x0F: { u8 b[5] = { drive_status(), s_mode, 0, s_filter_file, s_filter_chan }; queue_irq(3, b, 5); break; }
    case 0x10: {
        u8 b[8];
        lba_to_msf(s_last_read_lba + 150, &b[0], &b[1], &b[2]);
        b[3] = s_mode; b[4] = s_filter_file; b[5] = s_filter_chan; b[6] = b[7] = 0;
        queue_irq(3, b, 8);
        break;
    }
    case 0x11: {
        int abs = s_seek_lba + 150, track = 1, rel = s_seek_lba;
        u8 b[8];
        lba_to_msf(abs, &b[5], &b[6], &b[7]);
        for (int t = disc_first_track(); t <= disc_last_track(); t++) {
            int tl;
            if (disc_track_start(t, &tl) && abs >= tl) { track = t; rel = abs - tl; }
        }
        b[0] = int2bcd(track);
        b[1] = 1;
        lba_to_msf(rel, &b[2], &b[3], &b[4]);
        queue_irq(3, b, 8);
        break;
    }
    case 0x13: { u8 b[3] = { drive_status(), int2bcd(disc_first_track()), int2bcd(disc_last_track()) }; queue_irq(3, b, 3); break; }
    case 0x14: {
        int track = pn >= 1 ? bcd2int(p[0]) : 0, tl;
        int lba = (track == 0 || !disc_track_start(track, &tl)) ? disc_leadout() : tl;
        u8 b[3], ff;
        b[0] = drive_status();
        lba_to_msf(lba, &b[1], &b[2], &ff);
        queue_irq(3, b, 3);
        break;
    }
    case 0x15:
        s_seeking = 1;
        Q1(3, drive_status());
        s_seeking = 0;
        if (is_audio(s_seek_lba)) {
            s_reading = s_playing = 0;
            u8 b[2] = { (u8)(drive_status() | 4), 4 };
            queue_irq(5, b, 2);
        } else Q1(2, drive_status());
        break;
    case 0x16:
        s_seeking = 1;
        Q1(3, drive_status());
        s_seeking = 0;
        Q1(2, drive_status());
        break;
    case 0x19:
        if (pn >= 1 && p[0] == 0x20) { u8 b[4] = { 0x94, 0x09, 0x19, 0xC0 }; queue_irq(3, b, 4); }
        else Q1(3, drive_status());
        break;
    case 0x1A: {
        Q1(3, drive_status());
        u8 b[8] = { 0x02, 0x00, 0x20, 0x00, 0x53, 0x43, 0x45, 0x41 };
        queue_irq(2, b, 8);
        break;
    }
    case 0x1E: Q1(3, drive_status()); Q1(2, drive_status()); break;
    default: {
        st = drive_status();
        u8 b[2] = { st, 0x40 };
        rt_log("[cd] unknown command 0x%02X\n", cmd);
        queue_irq(5, b, 2);
        break;
    }
    }
}

u8 cdc_read(u32 phys)
{
    switch (phys & 3) {
    case 0: return (u8)((s_index & 3) | (s_param_n == 0 ? 0x08 : 0) | 0x10 | (s_resp_r < s_resp_n ? 0x20 : 0) | (s_data_ready ? 0x40 : 0));
    case 1: return s_resp_r < s_resp_n ? s_resp[s_resp_r++] : 0;
    case 2:
        if (!s_data_ready || s_data_pos >= 2048) { s_data_ready = 0; return 0; }
        {
            u8 b = s_data[s_data_pos++];
            if (s_data_pos >= 2048) s_data_ready = 0;
            return b;
        }
    default: return s_index == 1 ? s_irq_flags : 0;
    }
}

void cdc_write(u32 phys, u8 v)
{
    switch (phys & 3) {
    case 0: s_index = v & 3; break;
    case 1: if (s_index == 0) exec_cmd(v); break;
    case 2:
        if (s_index == 0) { if (s_param_n < 16) s_param[s_param_n++] = v; }
        else if (s_index == 1) s_param_n = 0;
        break;
    case 3:
        if (s_index == 0) {
            if (v & 0x80) { s_data_pos = 0; s_data_ready = 1; }
            else s_data_ready = 0;
        } else if (s_index == 1) {
            s_irq_flags &= (u8)~v;
            if (!s_irq_flags) after_ack();
        }
        break;
    }
}

void cdc_dma_read(u32 addr, u32 bytes)
{
    u8 tmp[2048];
    u32 done = 0;
    while (done < bytes) {
        u32 n = bytes - done;
        if (n > sizeof tmp) n = sizeof tmp;
        for (u32 i = 0; i < n; i++) tmp[i] = s_data_pos < 2048 ? s_data[s_data_pos++] : 0;
        mem_write_block(addr + done, tmp, n);
        done += n;
    }
    if (s_data_pos >= 2048) s_data_ready = 0;
}

void cdc_read_sector_data(int lba, u8 *out)
{
    s_seek_lba = lba;
    read_next_sector();
    memcpy(out, s_data, 2048);
}

void cdc_load_to_memory(const char *path, u32 address, int offset, int length)
{
    u32 size;
    u8 *data = iso_read_file(path, &size);
    if (!data) rt_fatal("boot file not found: %s", path);
    int count = length < 0 ? (int)size - offset : length;
    if (offset + count > (int)size) count = (int)size - offset;
    mem_write_block(address, data + offset, (u32)count);
    free(data);
    rt_log("[cd] %s -> 0x%08X (%d bytes)\n", path, address, count);
    char name[64];
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    int n = 0;
    for (; *b && *b != '.' && *b != ';' && n < 63; b++) name[n++] = (char)(*b | 0x20);
    name[n] = 0;
    dispatch_try_load(name);
}

void cdc_async_seekl(u8 mm, u8 ss, u8 ff)
{
    s_seek_lba = bcd_to_lba(mm, ss, ff);
    Q1(3, drive_status());
    Q1(2, drive_status());
}

void cdc_async_get_status(void) { Q1(3, drive_status()); }
void cdc_async_set_mode(u8 mode) { (void)mode; Q1(3, drive_status()); }

void cdc_async_read_sector(u32 count, u32 dst, u32 mode)
{
    (void)mode;
    for (u32 i = 0; i < count; i++) {
        read_next_sector();
        mem_write_block(dst + i * 2048u, s_data, 2048);
        s_seek_lba++;
    }
    Q1(3, drive_status());
    Q1(1, drive_status());
    Q1(2, drive_status());
}
