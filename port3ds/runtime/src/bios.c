#include "rt.h"
#include <stdlib.h>
#include <ctype.h>

/* High level BIOS, port of RecompOne.Runtime.Bios */

u32 g_intr_env_addr;

/* ---------------- helpers ---------------- */
static void read_str(u32 addr, char *out, int max)
{
    int i = 0;
    for (; i < max - 1; i++) {
        u8 b = (u8)RD8(addr + i);
        if (!b) break;
        out[i] = (char)b;
    }
    out[i] = 0;
}

static void extract_file_name(const char *raw, char *out, int max)
{
    const char *p = strchr(raw, ':');
    p = p ? p + 1 : raw;
    char tmp[256];
    int n = 0;
    for (; *p && *p != ';' && n < 255; p++) tmp[n++] = *p == '\\' ? '/' : *p;
    tmp[n] = 0;
    const char *s = strrchr(tmp, '/');
    snprintf(out, max, "%s", s ? s + 1 : tmp);
}

static void overlay_name(const char *file, char *out, int max)
{
    int n = 0;
    for (const char *p = file; *p && *p != '.' && n < max - 1; p++) out[n++] = (char)tolower((u8)*p);
    out[n] = 0;
}

/* ---------------- events / threads ---------------- */
typedef struct { u32 status, cls, spec, mode, func; } EvCB;
#define MAX_EVENTS 64
static EvCB s_ev[MAX_EVENTS];
static int s_tcb_used[4];
static u32 s_int_chain[4];

static u32 s_pad_buf;
static u32 s_padcard_buf1, s_padcard_buf2;
static int s_padcard_started;

void bios_deliver_event(u32 cls, u32 spec)
{
    for (int i = 0; i < MAX_EVENTS; i++)
        if (s_ev[i].status == 2u && s_ev[i].cls == cls && s_ev[i].spec == spec)
            s_ev[i].status = 4u;
}

static void deliver_event_intr(Cpu *c, u32 cls, u32 spec)
{
    for (int i = 0; i < MAX_EVENTS; i++) {
        if (s_ev[i].status != 2u || s_ev[i].cls != cls || s_ev[i].spec != spec) continue;
        if ((s_ev[i].mode & 0x1000u) && s_ev[i].func) {
            Cpu snap = *c;
            dispatch_call(c, s_ev[i].func);
            u32 sr = c->sr;
            *c = snap;
            c->sr = sr;
        } else
            s_ev[i].status = 4u;
    }
}

static void card_complete(Cpu *c, u32 port)
{
    MemCard *card = &g_card[(port & 0x10u) ? 1 : 0];
    u32 spec = card->enabled ? 0x0004u : 0x0100u;
    deliver_event_intr(c, 0xF4000001u, spec);
    deliver_event_intr(c, 0xF0000011u, spec);
}

static u32 get_free_ev_slot(void)
{
    for (int i = 0; i < MAX_EVENTS; i++) if (s_ev[i].status == 0) return (u32)i;
    return 0xFFFFFFFFu;
}

static u32 open_event(u32 cls, u32 spec, u32 mode, u32 func)
{
    for (int i = 0; i < MAX_EVENTS; i++) {
        if (s_ev[i].status == 0) {
            s_ev[i] = (EvCB){ 1u, cls, spec, mode, func };
            return 0xF0000000u | (u32)i;
        }
    }
    return 0xFFFFFFFFu;
}

static int ev_slot(u32 ev) { int i = (int)(ev & 0xFFu); return i < MAX_EVENTS ? i : -1; }

/* ---------------- pad ---------------- */
static u16 swap16(u16 v) { return (u16)((v >> 8) | (v << 8)); }

static void pad_read(void)
{
    if (!s_pad_buf) return;
    u16 s = swap16(g_pad_state);
    WR32(s_pad_buf, (0xFFFFu << 16) | s);
    WR8(s_pad_buf + 4, 0x80);
    WR8(s_pad_buf + 5, 0x80);
    WR8(s_pad_buf + 6, 0x80);
    WR8(s_pad_buf + 7, 0x80);
}

static void write_pad_slot(u32 buf, int connected, u16 buttons)
{
    if (!buf) return;
    if (!connected) { WR8(buf, 0xFF); WR8(buf + 1, 0); return; }
    WR8(buf, 0);
    WR8(buf + 1, 0x41);
    WR8(buf + 2, (u8)buttons);
    WR8(buf + 3, (u8)(buttons >> 8));
    WR8(buf + 4, 0x80);
    WR8(buf + 5, 0x80);
    WR8(buf + 6, 0x80);
    WR8(buf + 7, 0x80);
}

void bios_refresh_pad(void)
{
    pad_read();
    if (s_padcard_started) {
        write_pad_slot(s_padcard_buf1, 1, g_pad_state);
        write_pad_slot(s_padcard_buf2, 0, 0xFFFF);
    }
}

/* ---------------- heap (first fit, same policy as the C# runtime) ---------------- */
#define MAX_BLOCKS 2048
typedef struct { u32 start, end; } FreeBlk;
typedef struct { u32 addr, size; } BusyBlk;
static FreeBlk s_free[MAX_BLOCKS];
static int s_free_n;
static BusyBlk s_busy[MAX_BLOCKS];
static int s_busy_n;

static void free_insert(u32 start, u32 end)
{
    int i = 0;
    while (i < s_free_n && s_free[i].start < start) i++;
    if (i < s_free_n && s_free[i].start == start) { s_free[i].end = end; return; }
    if (s_free_n >= MAX_BLOCKS) return;
    memmove(&s_free[i + 1], &s_free[i], (s_free_n - i) * sizeof(FreeBlk));
    s_free[i] = (FreeBlk){ start, end };
    s_free_n++;
}

static void init_heap(u32 base, u32 size)
{
    s_free_n = 0;
    s_busy_n = 0;
    free_insert(base, base + size);
}

static u32 bmalloc(u32 size)
{
    if (!size) size = 4;
    size = (size + 3) & ~3u;
    for (int i = 0; i < s_free_n; i++) {
        if (s_free[i].end - s_free[i].start >= size) {
            u32 addr = s_free[i].start, end = s_free[i].end;
            memmove(&s_free[i], &s_free[i + 1], (s_free_n - i - 1) * sizeof(FreeBlk));
            s_free_n--;
            if (end - addr > size) free_insert(addr + size, end);
            if (s_busy_n < MAX_BLOCKS) s_busy[s_busy_n++] = (BusyBlk){ addr, size };
            return addr;
        }
    }
    return 0;
}

static int busy_find(u32 addr)
{
    for (int i = 0; i < s_busy_n; i++) if (s_busy[i].addr == addr) return i;
    return -1;
}

static void bfree(u32 addr)
{
    int b = addr ? busy_find(addr) : -1;
    if (b < 0) return;
    u32 size = s_busy[b].size;
    s_busy[b] = s_busy[--s_busy_n];
    u32 end = addr + size;
    for (int i = 0; i < s_free_n; i++) {
        if (s_free[i].start == end) {
            end = s_free[i].end;
            memmove(&s_free[i], &s_free[i + 1], (s_free_n - i - 1) * sizeof(FreeBlk));
            s_free_n--;
            break;
        }
    }
    free_insert(addr, end);
}

/* ---------------- string helpers on guest memory ---------------- */
static u32 b_strlen(u32 a) { u32 n = 0; while (RD8(a++)) n++; return n; }

static u32 b_strcmp(u32 a, u32 b)
{
    for (;;) {
        u8 x = (u8)RD8(a++), y = (u8)RD8(b++);
        if (x != y) return x < y ? 0xFFFFFFFFu : 1u;
        if (!x) return 0;
    }
}

static u32 b_strncmp(u32 a, u32 b, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        u8 x = (u8)RD8(a++), y = (u8)RD8(b++);
        if (x != y) return x < y ? 0xFFFFFFFFu : 1u;
        if (!x) return 0;
    }
    return 0;
}

static u32 b_strcpy(u32 dst, u32 src)
{
    u32 d = dst;
    u8 b;
    do { b = (u8)RD8(src++); WR8(d++, b); } while (b);
    return dst;
}

static u32 b_strncpy(u32 dst, u32 src, u32 n)
{
    u32 d = dst;
    for (u32 i = 0; i < n; i++) {
        u8 b = (u8)RD8(src++);
        WR8(d++, b);
        if (!b) { while (++i < n) WR8(d++, 0); break; }
    }
    return dst;
}

static u32 b_strcat(u32 dst, u32 src)
{
    u32 d = dst;
    while (RD8(d)) d++;
    u8 b;
    do { b = (u8)RD8(src++); WR8(d++, b); } while (b);
    return dst;
}

static u32 b_strncat(u32 dst, u32 src, u32 n)
{
    u32 d = dst;
    while (RD8(d)) d++;
    for (u32 i = 0; i < n; i++) {
        u8 b = (u8)RD8(src++);
        if (!b) break;
        WR8(d++, b);
    }
    WR8(d, 0);
    return dst;
}

static u32 b_strchr(u32 s, u32 ch)
{
    u8 t = (u8)ch;
    for (;; s++) {
        u8 b = (u8)RD8(s);
        if (b == t) return s;
        if (!b) return 0;
    }
}

static u32 b_strrchr(u32 s, u32 ch)
{
    u8 t = (u8)ch;
    u32 last = 0;
    for (;; s++) {
        u8 b = (u8)RD8(s);
        if (b == t) last = s;
        if (!b) return last;
    }
}

static u32 b_strpbrk(u32 s, u32 acc)
{
    for (;; s++) {
        u8 b = (u8)RD8(s);
        if (!b) return 0;
        if (b_strchr(acc, b)) return s;
    }
}

static u32 b_strspn(u32 s, u32 acc)
{
    u32 n = 0;
    for (;; n++) {
        u8 b = (u8)RD8(s + n);
        if (!b || !b_strchr(acc, b)) return n;
    }
}

static u32 b_strcspn(u32 s, u32 rej)
{
    u32 n = 0;
    for (;; n++) {
        u8 b = (u8)RD8(s + n);
        if (!b || b_strchr(rej, b)) return n;
    }
}

static u32 b_strstr(u32 s, u32 sub)
{
    u32 sl = b_strlen(sub);
    if (!sl) return s;
    u32 len = b_strlen(s);
    for (u32 i = 0; i + sl <= len; i++)
        if (!b_strncmp(s + i, sub, sl)) return s + i;
    return 0;
}

static u32 s_strtok;
static u32 b_strtok(u32 s, u32 delim)
{
    if (s) s_strtok = s;
    while (s_strtok && RD8(s_strtok) && b_strchr(delim, RD8(s_strtok))) s_strtok++;
    if (!RD8(s_strtok)) return 0;
    u32 start = s_strtok;
    while (RD8(s_strtok) && !b_strchr(delim, RD8(s_strtok))) s_strtok++;
    if (RD8(s_strtok)) { WR8(s_strtok, 0); s_strtok++; }
    return start;
}

static u32 b_memcpy(u32 d, u32 s, u32 n) { for (u32 i = 0; i < n; i++) WR8(d + i, RD8(s + i)); return d; }
static u32 b_memmove(u32 d, u32 s, u32 n)
{
    if (d <= s || d >= s + n) return b_memcpy(d, s, n);
    for (u32 i = n; i > 0; i--) WR8(d + i - 1, RD8(s + i - 1));
    return d;
}
static u32 b_memset(u32 p, u8 v, u32 n) { for (u32 i = 0; i < n; i++) WR8(p + i, v); return p; }
static u32 b_memcmp(u32 a, u32 b, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        u8 x = (u8)RD8(a + i), y = (u8)RD8(b + i);
        if (x != y) return x < y ? 0xFFFFFFFFu : 1u;
    }
    return 0;
}
static u32 b_memchr(u32 p, u8 v, u32 n) { for (u32 i = 0; i < n; i++) if (RD8(p + i) == v) return p + i; return 0; }

static int b_atoi(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    int sign = 1, r = 0;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (isdigit((u8)*s)) r = r * 10 + (*s++ - '0');
    return sign * r;
}

static u32 b_strtoul(u32 str, u32 endp, u32 base)
{
    char s[256];
    read_str(str, s, sizeof s);
    int lead = 0;
    while (s[lead] == ' ' || s[lead] == '\t' || s[lead] == '\n' || s[lead] == '\r') lead++;
    const char *p = s + lead;
    int i = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && (base == 0 || base == 16)) { i = 2; base = 16; }
    else if (base == 0) base = p[0] == '0' ? 8u : 10u;
    u32 r = 0;
    for (; p[i]; i++) {
        int d = p[i] >= '0' && p[i] <= '9' ? p[i] - '0' : p[i] >= 'a' && p[i] <= 'f' ? p[i] - 'a' + 10 : p[i] >= 'A' && p[i] <= 'F' ? p[i] - 'A' + 10 : -1;
        if (d < 0 || d >= (int)base) break;
        r = r * base + (u32)d;
    }
    /* the C# runtime reports the offset relative to the trimmed string */
    if (endp) WR32(endp, str + (u32)i);
    return r;
}

static u32 b_rand_seed = 1;
static u32 b_rand(void) { b_rand_seed = b_rand_seed * 1103515245u + 12345u; return (b_rand_seed >> 16) & 0x7FFFu; }

static void format_string(Cpu *c, const char *fmt, char *out, int max)
{
    int ai = 0, o = 0;
    for (int i = 0; fmt[i] && o < max - 32;) {
        if (fmt[i] != '%') { out[o++] = fmt[i++]; continue; }
        if (!fmt[++i]) break;
        if (fmt[i] == '%') { out[o++] = '%'; i++; continue; }
        if (fmt[i] == '-') i++;
        int zero = fmt[i] == '0';
        if (zero) i++;
        int width = 0;
        while (isdigit((u8)fmt[i])) width = width * 10 + (fmt[i++] - '0');
        if (fmt[i] == '.') { i++; while (isdigit((u8)fmt[i])) i++; }
        if (!fmt[i]) break;
        char conv = fmt[i++];
        int idx = ai++;
        u32 arg = idx == 0 ? c->r[5] : idx == 1 ? c->r[6] : idx == 2 ? c->r[7] : RD32(c->r[29] + 16u + (u32)((idx - 3) * 4));
        char tmp[128];
        switch (conv) {
        case 'd': case 'i': snprintf(tmp, sizeof tmp, "%d", (int)arg); break;
        case 'u': snprintf(tmp, sizeof tmp, "%u", arg); break;
        case 'x': snprintf(tmp, sizeof tmp, "%x", arg); break;
        case 'X': snprintf(tmp, sizeof tmp, "%X", arg); break;
        case 'o': snprintf(tmp, sizeof tmp, "%o", arg); break;
        case 'c': tmp[0] = (char)arg; tmp[1] = 0; break;
        case 's': if (arg) read_str(arg, tmp, sizeof tmp); else strcpy(tmp, "(null)"); break;
        default: snprintf(tmp, sizeof tmp, "%%%c", conv); ai--; break;
        }
        int len = (int)strlen(tmp);
        while (width > len && o < max - 32) { out[o++] = zero ? '0' : ' '; width--; }
        for (int k = 0; k < len && o < max - 1; k++) out[o++] = tmp[k];
    }
    out[o] = 0;
}

/* ---------------- files ---------------- */
#define MAX_FILES 16
typedef struct {
    int used;
    int card;          /* -1 disc, 0/1 memory card */
    char name[32];     /* overlay name for disc files */
    u8 *data;
    u32 size;
    u32 pos;
    int chain[16];
    int chain_n;
} OpenFile;
static OpenFile s_files[MAX_FILES];
static u32 s_errno;

static int card_for(const char *path)
{
    if (!strncasecmp(path, "bu00:", 5)) return g_card[0].enabled ? 0 : -1;
    if (!strncasecmp(path, "bu10:", 5)) return g_card[1].enabled ? 1 : -1;
    return -1;
}
static const char *card_name(const char *path) { const char *p = strchr(path, ':'); return p ? p + 1 : path; }

static u32 alloc_fd(void)
{
    for (int i = 3; i < MAX_FILES; i++) if (!s_files[i].used) { memset(&s_files[i], 0, sizeof s_files[i]); s_files[i].used = 1; return (u32)i; }
    return 0xFFFFFFFFu;
}

static OpenFile *get_fd(u32 fd) { return fd < MAX_FILES && s_files[fd].used ? &s_files[fd] : NULL; }

static char s_ff_names[15][21];
static int s_ff_sizes[15];
static int s_ff_n, s_ff_idx;

static void write_dir_entry(u32 ptr, const char *name, int size)
{
    int len = (int)strlen(name);
    for (int i = 0; i < 20; i++) WR8(ptr + i, i < len ? (u8)name[i] : 0);
    WR32(ptr + 0x14, 0x50);
    WR32(ptr + 0x18, (u32)size);
    WR32(ptr + 0x1C, 0);
    WR32(ptr + 0x20, 0);
    WR32(ptr + 0x24, 0);
}

static u32 next_file(u32 dir)
{
    if (s_ff_idx >= s_ff_n) return 0;
    write_dir_entry(dir, s_ff_names[s_ff_idx], s_ff_sizes[s_ff_idx]);
    s_ff_idx++;
    return dir;
}

static u32 first_file(u32 wild_ptr, u32 dir)
{
    char wild[64];
    read_str(wild_ptr, wild, sizeof wild);
    int card = card_for(wild);
    if (card < 0) return 0;
    s_ff_n = mc_match(&g_card[card], card_name(wild), s_ff_names, s_ff_sizes, 15);
    s_ff_idx = 0;
    return next_file(dir);
}

static void file_open(Cpu *c)
{
    char raw[128];
    read_str(c->r[4], raw, sizeof raw);
    int card = card_for(raw);
    if (card >= 0) {
        MemCard *mc = &g_card[card];
        const char *cn = card_name(raw);
        int first = mc_find(mc, cn);
        if (!first && (c->r[5] & 0x200u)) first = mc_create(mc, cn, (int)(c->r[5] >> 16));
        if (!first) { c->r[2] = 0xFFFFFFFFu; s_errno = 2; return; }
        u32 fd = alloc_fd();
        if (fd == 0xFFFFFFFFu) { c->r[2] = fd; s_errno = 24; return; }
        OpenFile *f = &s_files[fd];
        f->card = card;
        f->chain_n = mc_chain(mc, first, f->chain, 16);
        f->size = (u32)mc_file_size(mc, first);
        c->r[2] = fd;
        s_errno = 0;
        return;
    }
    if (!strncasecmp(raw, "sim:", 4)) { c->r[2] = 0xFFFFFFFFu; s_errno = 2; return; }
    char fname[64], found[256];
    extract_file_name(raw, fname, sizeof fname);
    if (!iso_find_file(fname, found, sizeof found)) { c->r[2] = 0xFFFFFFFFu; s_errno = 2; return; }
    u32 size;
    u8 *data = iso_read_file(found, &size);
    if (!data) { c->r[2] = 0xFFFFFFFFu; s_errno = 16; return; }
    u32 fd = alloc_fd();
    if (fd == 0xFFFFFFFFu) { free(data); c->r[2] = fd; s_errno = 24; return; }
    OpenFile *f = &s_files[fd];
    f->card = -1;
    f->data = data;
    f->size = size;
    overlay_name(fname, f->name, sizeof f->name);
    c->r[2] = fd;
    s_errno = 0;
}

/* ---------------- exec ---------------- */
#define EXEC_HDR_SCRATCH 0x1F800340u

static u32 do_load(u32 name_ptr, u32 hdr, int load_body)
{
    char raw[128], fname[64], found[256];
    read_str(name_ptr, raw, sizeof raw);
    extract_file_name(raw, fname, sizeof fname);
    if (!iso_find_file(fname, found, sizeof found)) return 0;
    u32 size;
    u8 *data = iso_read_file(found, &size);
    if (!data) return 0;
    if (size < 0x800 || data[0] != 'P' || data[1] != 'S') { free(data); return 0; }
    for (u32 i = 0; i < 40; i++) WR8(hdr + i, data[0x10 + i]);
    for (u32 i = 40; i < 60; i++) WR8(hdr + i, 0);
    if (load_body) {
        u32 taddr, tsize;
        memcpy(&taddr, data + 0x18, 4);
        memcpy(&tsize, data + 0x1C, 4);
        if (0x800 + tsize > size) tsize = size - 0x800;
        mem_write_block(taddr, data + 0x800, tsize);
        char on[32];
        overlay_name(fname, on, sizeof on);
        dispatch_try_load(on);
    }
    free(data);
    return hdr;
}

static u32 do_exec(Cpu *c, u32 hdr, u32 argc, u32 argv)
{
    u32 pc0 = RD32(hdr + 0x00);
    if (!pc0) return 0;
    u32 gp0 = RD32(hdr + 0x04);
    u32 baddr = RD32(hdr + 0x18), bsize = RD32(hdr + 0x1C);
    u32 saddr = RD32(hdr + 0x20), ssize = RD32(hdr + 0x24);
    mem_zero(baddr, bsize);
    u32 sv_sp = c->r[29], sv_fp = c->r[30], sv_gp = c->r[28], sv_ra = c->r[31];
    u32 sv_a0 = c->r[4], sv_a1 = c->r[5], sv_a2 = c->r[6], sv_a3 = c->r[7];
    WR32(hdr + 0x28, c->r[29]);
    WR32(hdr + 0x2C, c->r[30]);
    WR32(hdr + 0x30, c->r[28]);
    WR32(hdr + 0x34, c->r[31]);
    c->r[28] = gp0;
    if (saddr) { c->r[29] = saddr + ssize; c->r[30] = c->r[29]; }
    c->r[4] = argc;
    c->r[5] = argv;
    dispatch_call(c, pc0);
    c->r[29] = sv_sp; c->r[30] = sv_fp; c->r[28] = sv_gp; c->r[31] = sv_ra;
    c->r[4] = sv_a0; c->r[5] = sv_a1; c->r[6] = sv_a2; c->r[7] = sv_a3;
    return 1;
}

/* ---------------- kanji font ---------------- */
static const u16 s_krom[][2] = {
    {0x8140, 0x0000}, {0x8180, 0x003f}, {0x81ad, 0x006d}, {0x81b8, 0x006c},
    {0x81c0, 0x0080}, {0x81c8, 0x0074}, {0x81cf, 0x008f}, {0x81da, 0x007b},
    {0x81e9, 0x00a9}, {0x81f0, 0x008a}, {0x81f8, 0x00b8}, {0x81fc, 0x0092},
    {0x81fd, 0x00bd}, {0x824f, 0x0093}, {0x8259, 0x0119}, {0x8260, 0x009d},
    {0x827a, 0x013a}, {0x8281, 0x00b7}, {0x829b, 0x015b}, {0x829f, 0x00d1},
    {0x82f2, 0x01b2}, {0x8340, 0x0124}, {0x837f, 0x023f}, {0x8380, 0x0163},
    {0x8397, 0x0257}, {0x839f, 0x017a}, {0x83b7, 0x0277}, {0x83bf, 0x0192},
    {0x83d7, 0x0297}, {0x8440, 0x01aa}, {0x8461, 0x0321}, {0x8470, 0x01cb},
    {0x847f, 0x033f}, {0x8480, 0x01da}, {0x8492, 0x0352}, {0x849f, 0x01ec},
    {0x889f, 0x0000}, {0x8900, 0x001e}, {0x897f, 0x009c}, {0x8a00, 0x00da},
    {0x8a7f, 0x0158}, {0x8b00, 0x0196}, {0x8b7f, 0x0214}, {0x8c00, 0x0252},
    {0x8c7f, 0x02d0}, {0x8d00, 0x030e}, {0x8d7f, 0x038c}, {0x8e00, 0x03ca},
    {0x8e7f, 0x0448}, {0x8f00, 0x0486}, {0x8f7f, 0x0504}, {0x9000, 0x0542},
    {0x907f, 0x05c0}, {0x9100, 0x05fe}, {0x917f, 0x067c}, {0x9200, 0x06ba},
    {0x927f, 0x0738}, {0x9300, 0x0776}, {0x937f, 0x07f4}, {0x9400, 0x0832},
    {0x947f, 0x08b0}, {0x9500, 0x08ee}, {0x957f, 0x096c}, {0x9600, 0x09aa},
    {0x967f, 0x0a28}, {0x9700, 0x0a66}, {0x977f, 0x0ae4}, {0x9800, 0x0b22},
    {0xffff, 0x0000},
};

static u16 krom2_offset(u32 code)
{
    u16 c = (u16)code;
    if (c < 0x8140 || c > 0x9872) return 0;
    int idx = 1;
    while (s_krom[idx][0] <= c) idx++;
    idx--;
    return (u16)(c - s_krom[idx][0] + s_krom[idx][1]);
}

static u32 krom2_raw_add(u32 code)
{
    u16 c = (u16)code;
    if (c >= 0x8140 && c <= 0x84BE) return 0xBFC66000u + (u32)krom2_offset(code) * 0x1Eu;
    if (c >= 0x889F && c <= 0x9872) return 0xBFC69D68u + (u32)krom2_offset(code) * 0x1Eu;
    return 0xFFFFFFFFu;
}

/* ---------------- A table ---------------- */
static u32 s_conf_ev = 16, s_conf_tcb = 4, s_conf_stack;

static void bios_a(Cpu *c, u32 fn)
{
    char buf[512];
    switch (fn) {
    case 0x00: file_open(c); break;
    case 0x01: {
        OpenFile *f = get_fd(c->r[4]);
        if (!f) { c->r[2] = 0xFFFFFFFFu; s_errno = 9; break; }
        s32 off = (s32)c->r[5];
        s32 no = c->r[6] == 0 ? off : c->r[6] == 1 ? (s32)f->pos + off : c->r[6] == 2 ? (s32)f->size + off : (s32)f->pos;
        if (no < 0) no = 0;
        if ((u32)no > f->size) no = (s32)f->size;
        f->pos = (u32)no;
        c->r[2] = (u32)no;
        s_errno = 0;
        break;
    }
    case 0x02: {
        OpenFile *f = get_fd(c->r[4]);
        if (!f) { c->r[2] = 0xFFFFFFFFu; s_errno = 9; break; }
        u32 n = c->r[6];
        if (n > f->size - f->pos) n = f->size - f->pos;
        if (f->card >= 0) {
            for (u32 i = 0; i < n; i++) WR8(c->r[5] + i, mc_read_byte(&g_card[f->card], f->chain, f->chain_n, (int)(f->pos + i)));
            f->pos += n;
            bios_deliver_event(0xF4000001u, 0x0004u);
        } else {
            mem_write_block(c->r[5], f->data + f->pos, n);
            f->pos += n;
            if (f->pos >= f->size) dispatch_try_load(f->name);
        }
        c->r[2] = n;
        s_errno = 0;
        break;
    }
    case 0x03: {
        u32 fd = c->r[4];
        OpenFile *f = get_fd(fd);
        if (fd <= 2 && !f) {
            u32 n = c->r[6] < sizeof buf - 1 ? c->r[6] : sizeof buf - 1;
            for (u32 i = 0; i < n; i++) buf[i] = (char)RD8(c->r[5] + i);
            buf[n] = 0;
            rt_log("%s", buf);
            c->r[2] = c->r[6];
            s_errno = 0;
            break;
        }
        if (f && f->card >= 0) {
            u32 n = c->r[6];
            if (n > f->size - f->pos) n = f->size - f->pos;
            for (u32 i = 0; i < n; i++) mc_write_byte(&g_card[f->card], f->chain, f->chain_n, (int)(f->pos + i), (u8)RD8(c->r[5] + i));
            mc_flush(&g_card[f->card]);
            f->pos += n;
            bios_deliver_event(0xF4000001u, 0x0004u);
            c->r[2] = n;
            s_errno = 0;
            break;
        }
        c->r[2] = 0xFFFFFFFFu;
        s_errno = 9;
        break;
    }
    case 0x04: {
        OpenFile *f = get_fd(c->r[4]);
        if (f) { free(f->data); f->used = 0; }
        c->r[2] = c->r[4];
        s_errno = 0;
        break;
    }
    case 0x05: c->r[2] = 0xFFFFFFFFu; break;
    case 0x06: rt_fatal("game called exit(%d)", (int)c->r[4]);
    case 0x07: c->r[2] = c->r[4] <= 2 ? 2u : 0u; break;
    case 0x08: c->r[2] = 0xFFFFFFFFu; break;
    case 0x09: rt_log("%c", (char)c->r[4]); c->r[2] = c->r[4]; break;
    case 0x0A: { u32 ch = c->r[4] & 0xFF; c->r[2] = isdigit((int)ch) ? ch - '0' : 0xFFFFFFFFu; break; }
    case 0x0B: c->r[2] = 0; break;
    case 0x0C: c->r[2] = b_strtoul(c->r[4], c->r[5], c->r[6]); break;
    case 0x0D: {
        char s[256];
        read_str(c->r[4], s, sizeof s);
        int i = 0, sign = 1;
        while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') i++;
        if (s[i] == '-') { sign = -1; i++; } else if (s[i] == '+') i++;
        c->r[2] = (u32)(sign * (s32)b_strtoul(c->r[4] + (u32)i, c->r[5], c->r[6]));
        break;
    }
    case 0x0E: case 0x0F: { s32 v = (s32)c->r[4]; c->r[2] = (u32)(v < 0 ? -v : v); break; }
    case 0x10: case 0x11: read_str(c->r[4], buf, sizeof buf); c->r[2] = (u32)b_atoi(buf); break;
    case 0x12: {
        read_str(c->r[4], buf, sizeof buf);
        const char *s = buf;
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
        int sign = 1, r = 0, ok = 0;
        if (*s == '-') { sign = -1; s++; }
        while (isdigit((u8)*s)) { r = r * 10 + (*s++ - '0'); ok = 1; }
        if (!ok) { c->r[2] = 0; break; }
        WR32(c->r[5], (u32)(sign * r));
        c->r[2] = 1;
        break;
    }
    case 0x13: {
        u32 b = c->r[4];
        WR32(b + 0x00, c->r[31]); WR32(b + 0x04, c->r[29]);
        WR32(b + 0x08, c->r[30]); WR32(b + 0x0C, c->r[28]);
        for (int i = 0; i < 8; i++) WR32(b + 0x10 + i * 4, c->r[16 + i]);
        c->r[2] = 0;
        break;
    }
    case 0x14: {
        u32 b = c->r[4];
        c->r[31] = RD32(b + 0x00); c->r[29] = RD32(b + 0x04);
        c->r[30] = RD32(b + 0x08); c->r[28] = RD32(b + 0x0C);
        for (int i = 0; i < 8; i++) c->r[16 + i] = RD32(b + 0x10 + i * 4);
        c->r[2] = c->r[5] ? c->r[5] : 1u;
        break;
    }
    case 0x15: c->r[2] = b_strcat(c->r[4], c->r[5]); break;
    case 0x16: c->r[2] = b_strncat(c->r[4], c->r[5], c->r[6]); break;
    case 0x17: c->r[2] = b_strcmp(c->r[4], c->r[5]); break;
    case 0x18: c->r[2] = b_strncmp(c->r[4], c->r[5], c->r[6]); break;
    case 0x19: c->r[2] = b_strcpy(c->r[4], c->r[5]); break;
    case 0x1A: c->r[2] = b_strncpy(c->r[4], c->r[5], c->r[6]); break;
    case 0x1B: c->r[2] = b_strlen(c->r[4]); break;
    case 0x1C: case 0x1E: c->r[2] = b_strchr(c->r[4], c->r[5]); break;
    case 0x1D: case 0x1F: c->r[2] = b_strrchr(c->r[4], c->r[5]); break;
    case 0x20: c->r[2] = b_strpbrk(c->r[4], c->r[5]); break;
    case 0x21: c->r[2] = b_strspn(c->r[4], c->r[5]); break;
    case 0x22: c->r[2] = b_strcspn(c->r[4], c->r[5]); break;
    case 0x23: c->r[2] = b_strtok(c->r[4], c->r[5]); break;
    case 0x24: c->r[2] = b_strstr(c->r[4], c->r[5]); break;
    case 0x25: c->r[2] = (u32)toupper((int)(c->r[4] & 0xFF)) & 0xFF; break;
    case 0x26: c->r[2] = (u32)tolower((int)(c->r[4] & 0xFF)) & 0xFF; break;
    case 0x27: b_memcpy(c->r[5], c->r[4], c->r[6]); c->r[2] = c->r[5]; break;
    case 0x28: b_memset(c->r[4], 0, c->r[5]); break;
    case 0x29: case 0x2D: c->r[2] = b_memcmp(c->r[4], c->r[5], c->r[6]); break;
    case 0x2A: c->r[2] = b_memcpy(c->r[4], c->r[5], c->r[6]); break;
    case 0x2B: c->r[2] = b_memset(c->r[4], (u8)c->r[5], c->r[6]); break;
    case 0x2C: c->r[2] = b_memmove(c->r[4], c->r[5], c->r[6]); break;
    case 0x2E: c->r[2] = b_memchr(c->r[4], (u8)c->r[5], c->r[6]); break;
    case 0x2F: c->r[2] = b_rand(); break;
    case 0x30: b_rand_seed = c->r[4]; break;
    case 0x31: {
        u32 qb = c->r[4], qn = c->r[5], qs = c->r[6], qc = c->r[7];
        for (u32 i = 1; i < qn; i++) {
            u32 j = i;
            while (j > 0) {
                u32 pa = qb + (j - 1) * qs, pb = qb + j * qs;
                c->r[4] = pa; c->r[5] = pb;
                dispatch_call(c, qc);
                if ((s32)c->r[2] <= 0) break;
                for (u32 k = 0; k < qs; k++) { u8 t = (u8)RD8(pa + k); WR8(pa + k, RD8(pb + k)); WR8(pb + k, t); }
                j--;
            }
        }
        break;
    }
    case 0x32: c->r[2] = 0; break;
    case 0x33: c->r[2] = bmalloc(c->r[4]); break;
    case 0x34: bfree(c->r[4]); break;
    case 0x35: {
        u32 key = c->r[4], ub = c->r[5], nel = c->r[6], w = c->r[7];
        u32 cmp = RD32(c->r[29] + 0x10);
        c->r[2] = 0;
        u32 res = 0;
        for (u32 i = 0; i < nel; i++) {
            u32 e = ub + i * w;
            c->r[4] = key; c->r[5] = e;
            dispatch_call(c, cmp);
            if (c->r[2] == 0) { res = e; break; }
        }
        if (!res) { res = ub + nel * w; b_memcpy(res, key, w); }
        c->r[2] = res;
        break;
    }
    case 0x36: {
        u32 key = c->r[4], ub = c->r[5], nel = c->r[6], w = c->r[7];
        u32 cmp = RD32(c->r[29] + 0x10);
        int lo = 0, hi = (int)nel - 1;
        u32 res = 0;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            u32 e = ub + (u32)mid * w;
            c->r[4] = key; c->r[5] = e;
            dispatch_call(c, cmp);
            s32 d = (s32)c->r[2];
            if (!d) { res = e; break; }
            if (d < 0) hi = mid - 1; else lo = mid + 1;
        }
        c->r[2] = res;
        break;
    }
    case 0x37: { u32 t = c->r[4] * c->r[5]; u32 p = bmalloc(t); if (p) b_memset(p, 0, t); c->r[2] = p; break; }
    case 0x38: {
        u32 ptr = c->r[4], size = c->r[5];
        if (!ptr) { c->r[2] = bmalloc(size); break; }
        if (!size) { bfree(ptr); c->r[2] = 0; break; }
        int b = busy_find(ptr);
        u32 old = b >= 0 ? s_busy[b].size : 0;
        u32 np = bmalloc(size);
        if (!np) { c->r[2] = 0; break; }
        b_memcpy(np, ptr, old < size ? old : size);
        bfree(ptr);
        c->r[2] = np;
        break;
    }
    case 0x39: init_heap(c->r[4], c->r[5]); break;
    case 0x3A: rt_fatal("game called _exit");
    case 0x3B: c->r[2] = 0xFFFFFFFFu; break;
    case 0x3C: rt_log("%c", (char)c->r[4]); c->r[2] = c->r[4]; break;
    case 0x3D: c->r[2] = 0; break;
    case 0x3E: read_str(c->r[4], buf, sizeof buf); rt_log("%s", buf); c->r[2] = c->r[4]; break;
    case 0x3F: {
        char fmt[256];
        read_str(c->r[4], fmt, sizeof fmt);
        format_string(c, fmt, buf, sizeof buf);
        rt_log("%s", buf);
        c->r[2] = 0;
        break;
    }
    case 0x40: rt_fatal("BIOS A(40h) SystemErrorUnresolvedException");
    case 0x41: c->r[2] = do_load(c->r[4], c->r[5], 0); break;
    case 0x42: c->r[2] = do_load(c->r[4], c->r[5], 1); break;
    case 0x43: c->r[2] = do_exec(c, c->r[4], c->r[5], c->r[6]); break;
    case 0x4D: c->r[2] = 0; break;
    case 0x51: {
        if (!do_load(c->r[4], EXEC_HDR_SCRATCH, 1)) { c->r[2] = 0; break; }
        if (c->r[5]) { WR32(EXEC_HDR_SCRATCH + 0x20, c->r[5]); WR32(EXEC_HDR_SCRATCH + 0x24, c->r[6]); }
        c->r[2] = do_exec(c, EXEC_HDR_SCRATCH, 0, 0);
        break;
    }
    case 0x78: cdc_async_seekl((u8)RD8(c->r[4]), (u8)RD8(c->r[4] + 1), (u8)RD8(c->r[4] + 2)); c->r[2] = 1; break;
    case 0x7C: cdc_async_get_status(); c->r[2] = 1; break;
    case 0x7E: cdc_async_read_sector(c->r[4], c->r[5], c->r[6]); c->r[2] = 1; break;
    case 0x81: cdc_async_set_mode((u8)c->r[4]); c->r[2] = 1; break;
    case 0x9C: s_conf_ev = c->r[4]; s_conf_tcb = c->r[5]; s_conf_stack = c->r[6]; break;
    case 0x9D:
        if (c->r[4]) WR32(c->r[4], s_conf_ev);
        if (c->r[5]) WR32(c->r[5], s_conf_tcb);
        if (c->r[6]) WR32(c->r[6], s_conf_stack);
        break;
    case 0xA4: c->r[2] = 0xFFFFFFFFu; break;
    case 0xA5: {
        u32 count = c->r[4], lba = c->r[5], b = c->r[6];
        u8 sec[2048];
        for (u32 i = 0; i < count; i++) {
            cdc_read_sector_data((int)(lba + i), sec);
            mem_write_block(b + i * 2048u, sec, 2048);
        }
        c->r[2] = count;
        break;
    }
    case 0xA6: c->r[2] = cdc_drive_status(); break;
    case 0xAB: case 0xAC: card_complete(c, c->r[4]); c->r[2] = 1; break;
    case 0xB4: c->r[2] = c->r[4] == 0 ? 0xFFFFFFFFu : (c->r[4] == 1 || c->r[4] == 2) ? 1u : 0u; break;
    default: break;
    }
}

/* ---------------- B table ---------------- */
static void bios_b(Cpu *c, u32 fn)
{
    char buf[256];
    switch (fn) {
    case 0x00: case 0x02: case 0x03: c->r[2] = 0; break;
    case 0x07: bios_deliver_event(c->r[4], c->r[5]); break;
    case 0x08: c->r[2] = open_event(c->r[4], c->r[5], c->r[6], c->r[7]); break;
    case 0x09: { int s = ev_slot(c->r[4]); if (s >= 0) memset(&s_ev[s], 0, sizeof s_ev[s]); c->r[2] = 1; break; }
    case 0x0A: { int s = ev_slot(c->r[4]); if (s >= 0 && s_ev[s].status == 4u) s_ev[s].status = 2u; c->r[2] = 1; break; }
    case 0x0B: {
        int s = ev_slot(c->r[4]);
        if (s >= 0 && s_ev[s].status == 4u) { s_ev[s].status = 2u; c->r[2] = 1; }
        else c->r[2] = 0;
        break;
    }
    case 0x0C: { int s = ev_slot(c->r[4]); if (s >= 0) s_ev[s].status = 2u; c->r[2] = 1; break; }
    case 0x0D: { int s = ev_slot(c->r[4]); if (s >= 0 && s_ev[s].status) s_ev[s].status = 1u; c->r[2] = 1; break; }
    case 0x0E: {
        c->r[2] = 0xFFFFFFFFu;
        for (int i = 0; i < 4; i++) if (!s_tcb_used[i]) { s_tcb_used[i] = 1; c->r[2] = 0xFF000000u | (u32)i; break; }
        break;
    }
    case 0x0F: { int i = (int)(c->r[4] & 0xFF); if (i < 4) s_tcb_used[i] = 0; c->r[2] = 1; break; }
    case 0x12: {
        s_padcard_buf1 = c->r[4];
        s_padcard_buf2 = c->r[6];
        mem_zero(c->r[4], c->r[5]);
        mem_zero(c->r[6], c->r[7]);
        break;
    }
    case 0x13: s_padcard_started = 1; break;
    case 0x14: s_padcard_started = 0; break;
    case 0x15: s_pad_buf = c->r[5]; break;
    case 0x16: pad_read(); break;
    case 0x18: g_intr_env_addr = 0; break;
    case 0x19: g_intr_env_addr = c->r[4] ? c->r[4] - 0x36u : 0; break;
    case 0x20: {
        for (int i = 0; i < MAX_EVENTS; i++)
            if (s_ev[i].status == 4u && s_ev[i].cls == c->r[4] && s_ev[i].spec == c->r[5]) s_ev[i].status = 2u;
        break;
    }
    case 0x2F: case 0x30: case 0x31: c->r[2] = 0; break;
    case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38:
        bios_a(c, fn - 0x32);
        break;
    case 0x39: c->r[2] = c->r[4] <= 2 ? 2u : 0u; break;
    case 0x3A: case 0x3C: c->r[2] = 0xFFFFFFFFu; break;
    case 0x3B: case 0x3D: rt_log("%c", (char)c->r[4]); c->r[2] = c->r[4]; break;
    case 0x3E: c->r[2] = 0; break;
    case 0x3F: read_str(c->r[4], buf, sizeof buf); rt_log("%s", buf); c->r[2] = c->r[4]; break;
    case 0x40: c->r[2] = 1; break;
    case 0x41: {
        read_str(c->r[4], buf, sizeof buf);
        int card = card_for(buf);
        if (card < 0) { bios_deliver_event(0xF0000011u, 0x8000u); c->r[2] = 0; break; }
        mc_format(&g_card[card]);
        bios_deliver_event(0xF0000011u, 0x0004u);
        c->r[2] = 1;
        break;
    }
    case 0x42: c->r[2] = first_file(c->r[4], c->r[5]); break;
    case 0x43: c->r[2] = next_file(c->r[4]); break;
    case 0x44: case 0x46: c->r[2] = 0; break;
    case 0x45: {
        read_str(c->r[4], buf, sizeof buf);
        int card = card_for(buf);
        if (card < 0) { c->r[2] = 0; break; }
        mc_delete(&g_card[card], card_name(buf));
        c->r[2] = 1;
        break;
    }
    case 0x47: c->r[2] = get_free_ev_slot(); break;
    case 0x48: c->r[2] = 0xFFFFFFFFu; break;
    case 0x4A: case 0x4B: case 0x4C: c->r[2] = 1; break;
    case 0x4E: case 0x4F: {
        MemCard *card = &g_card[(c->r[4] & 0x10u) ? 1 : 0];
        if (card->enabled && c->r[6]) {
            u8 f[0x80];
            if (fn == 0x4E) {
                mem_read_block(c->r[6], f, 0x80);
                mc_frame_write(card, (int)(c->r[5] & 0x3FFu), f);
            } else {
                mc_frame_read(card, (int)(c->r[5] & 0x3FFu), f);
                mem_write_block(c->r[6], f, 0x80);
            }
        }
        card_complete(c, c->r[4]);
        c->r[2] = 1;
        break;
    }
    case 0x51: c->r[2] = krom2_raw_add(c->r[4]); break;
    case 0x53: c->r[2] = krom2_offset(c->r[4]); break;
    case 0x54: c->r[2] = s_errno; break;
    case 0x55: case 0x56: case 0x57: case 0x5B: case 0x5C: c->r[2] = 0; break;
    case 0x59: read_str(c->r[4], buf, sizeof buf); c->r[2] = card_for(buf) >= 0 ? 1u : 0u; break;
    default: break;
    }
}

/* ---------------- C table ---------------- */
static void bios_c(Cpu *c, u32 fn)
{
    switch (fn) {
    case 0x02: {
        u32 pri = c->r[4] & 3u, st = c->r[5];
        c->r[2] = s_int_chain[pri];
        WR32(st, s_int_chain[pri]);
        s_int_chain[pri] = st;
        break;
    }
    case 0x03: {
        u32 pri = c->r[4] & 3u, st = c->r[5];
        if (s_int_chain[pri] == st) { s_int_chain[pri] = RD32(st); c->r[2] = 1; break; }
        u32 cur = s_int_chain[pri];
        c->r[2] = 0;
        while (cur) {
            u32 next = RD32(cur);
            if (next == st) { WR32(cur, RD32(st)); c->r[2] = 1; break; }
            cur = next;
        }
        break;
    }
    case 0x04: c->r[2] = get_free_ev_slot(); break;
    case 0x05: case 0x11: case 0x18: case 0x19: case 0x1D: c->r[2] = 0; break;
    default: break;
    }
}

int bios_try_dispatch(Cpu *restrict c, u32 addr)
{
    switch (addr) {
    case 0xA0u: bios_a(c, c->r[9]); return 1;
    case 0xB0u: bios_b(c, c->r[9]); return 1;
    case 0xC0u: bios_c(c, c->r[9]); return 1;
    }
    return (addr & 0xFFF00000u) == 0xBFC00000u;
}

void bios_init(void)
{
    memset(s_ev, 0, sizeof s_ev);
    memset(s_tcb_used, 0, sizeof s_tcb_used);
    memset(s_int_chain, 0, sizeof s_int_chain);
    for (int i = 0; i < MAX_FILES; i++) { free(s_files[i].data); s_files[i].used = 0; s_files[i].data = NULL; }
    g_intr_env_addr = 0;
    s_pad_buf = 0;
    s_padcard_buf1 = s_padcard_buf2 = 0;
    s_padcard_started = 0;
    s_free_n = s_busy_n = 0;
}
