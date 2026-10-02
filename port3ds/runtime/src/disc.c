#include "rt.h"
#include <stdlib.h>
#include <ctype.h>

/* cue/bin image + ISO9660 (port of CueBinImage / DiscFs) */

typedef struct {
    char bin[512];
    int number;
    int audio;
    int sector_size;
    int data_offset;
    long long file_offset;
    int start_lba;
} Track;

static Track s_tracks[99];
static int s_ntracks;
static FILE *s_data;          /* data track file */
static int s_data_track = -1;
static int s_data_sectors;
static int s_leadout;
static u8 s_raw[2352];

static long long file_size(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long long n = ftell(f);
    fclose(f);
    return n;
}

static long long msf_to_sectors(const char *s)
{
    int m = 0, sec = 0, fr = 0;
    sscanf(s, "%d:%d:%d", &m, &sec, &fr);
    return (long long)m * 60 * 75 + sec * 75 + fr;
}

int disc_open(const char *cue_path)
{
    FILE *f = fopen(cue_path, "r");
    if (!f) return 0;
    char dir[512];
    snprintf(dir, sizeof dir, "%s", cue_path);
    char *sl = strrchr(dir, '/');
    char *bs = strrchr(dir, '\\');
    if (bs && (!sl || bs > sl)) sl = bs;
    if (sl) sl[1] = 0; else dir[0] = 0;

    char line[1024], cur[512] = "";
    int tnum = 0;
    char mode[32] = "MODE2/2352";
    long long base = 0;
    s_ntracks = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncasecmp(p, "FILE ", 5)) {
            char *a = strchr(p, '"'), *b = a ? strrchr(p, '"') : NULL;
            if (cur[0]) base += file_size(cur) / 2352;
            if (a && b > a) { *b = 0; snprintf(cur, sizeof cur, "%s%s", dir, a + 1); }
        } else if (!strncasecmp(p, "TRACK ", 6)) {
            sscanf(p + 6, "%d %31s", &tnum, mode);
        } else if (!strncasecmp(p, "INDEX 01 ", 9)) {
            if (s_ntracks >= 99) break;
            Track *t = &s_tracks[s_ntracks++];
            long long within = msf_to_sectors(p + 9);
            snprintf(t->bin, sizeof t->bin, "%s", cur);
            t->number = tnum;
            t->audio = !strcasecmp(mode, "AUDIO");
            t->sector_size = !strcasecmp(mode, "MODE1/2048") ? 2048 : !strcasecmp(mode, "MODE2/2336") ? 2336 : 2352;
            t->data_offset = !strcasecmp(mode, "MODE1/2352") ? 16 : !strcasecmp(mode, "MODE2/2352") ? 24 : !strcasecmp(mode, "MODE2/2336") ? 8 : 0;
            t->file_offset = within * t->sector_size;
            t->start_lba = (int)(base + within);
        }
    }
    fclose(f);
    if (cur[0]) base += file_size(cur) / 2352;
    s_leadout = (int)base;

    s_data_track = -1;
    for (int i = 0; i < s_ntracks; i++) if (!s_tracks[i].audio) { s_data_track = i; break; }
    if (s_data_track < 0) return 0;
    Track *d = &s_tracks[s_data_track];
    s_data = fopen(d->bin, "rb");
    if (!s_data) return 0;
    setvbuf(s_data, NULL, _IONBF, 0);
    int next = 0x7FFFFFFF;
    for (int i = 0; i < s_ntracks; i++)
        if (s_tracks[i].start_lba > d->start_lba && s_tracks[i].start_lba < next) next = s_tracks[i].start_lba;
    if (next != 0x7FFFFFFF) s_data_sectors = next - d->start_lba;
    else s_data_sectors = (int)((file_size(d->bin) - d->file_offset) / d->sector_size);
    rt_log("[disc] %d tracks, data sectors %d\n", s_ntracks, s_data_sectors);
    return 1;
}

int disc_data_sectors(void) { return s_data_sectors; }
int disc_leadout(void) { return s_leadout; }
int disc_has_tracks(void) { return s_ntracks > 0; }

int disc_first_track(void)
{
    int m = 99;
    for (int i = 0; i < s_ntracks; i++) if (s_tracks[i].number < m) m = s_tracks[i].number;
    return s_ntracks ? m : 1;
}

int disc_last_track(void)
{
    int m = 0;
    for (int i = 0; i < s_ntracks; i++) if (s_tracks[i].number > m) m = s_tracks[i].number;
    return s_ntracks ? m : 1;
}

int disc_track_start(int track, int *lba)
{
    for (int i = 0; i < s_ntracks; i++) if (s_tracks[i].number == track) { *lba = s_tracks[i].start_lba; return 1; }
    *lba = 0;
    return 0;
}

/* read-ahead cache: sectors are fetched RA_N at a time to keep the number of
   SD card requests low (XA music streaming touches every sector) */
#define RA_N 32
#define RA_SETS 4
static int ra_base[RA_SETS] = { -1, -1, -1, -1 };
static int ra_count[RA_SETS];
static u8 ra_data[RA_SETS][RA_N * 2352];
static int ra_next;

static const u8 *raw_sector(int lba)
{
    for (int i = 0; i < RA_SETS; i++)
        if (ra_base[i] >= 0 && lba >= ra_base[i] && lba < ra_base[i] + ra_count[i])
            return ra_data[i] + (lba - ra_base[i]) * 2352;
    Track *t = &s_tracks[s_data_track];
    int set = ra_next;
    ra_next = (ra_next + 1) % RA_SETS;
    int n = RA_N;
    if (lba + n > s_data_sectors) n = s_data_sectors - lba;
    long long pos = t->file_offset + (long long)lba * t->sector_size;
    fseek(s_data, (long)pos, SEEK_SET);
    if (t->sector_size == 2352) {
        size_t got = fread(ra_data[set], 2352, n, s_data);
        if ((int)got < n) memset(ra_data[set] + got * 2352, 0, (n - got) * 2352);
    } else {
        for (int i = 0; i < n; i++) {
            memset(ra_data[set] + i * 2352, 0, 2352);
            fread(ra_data[set] + i * 2352, 1, t->sector_size, s_data);
        }
    }
    ra_base[set] = lba;
    ra_count[set] = n;
    return ra_data[set];
}

static int disc_read_impl(int lba, int size, u8 *out);

u32 g_disc_reads, g_disc_last_lba;
int disc_read(int lba, int size, u8 *out)
{
    g_disc_reads++;
    g_disc_last_lba = (u32)lba;
    u64 t0 = host_ticks_us();
    int r = disc_read_impl(lba, size, out);
    g_prof[PROF_CD] += host_ticks_us() - t0;
    return r;
}

static int disc_read_impl(int lba, int size, u8 *out)
{
    memset(out, 0, size);
    if (s_data_track < 0 || lba < 0 || lba >= s_data_sectors) return 0;
    Track *t = &s_tracks[s_data_track];
    int off = t->sector_size == 2352 ? (size >= 2340 ? 12 : size >= 2329 ? 16 : 24) : t->data_offset;
    int want = size;
    if (want > t->sector_size - off) want = t->sector_size - off;
    const u8 *raw = raw_sector(lba);
    memcpy(out, raw + off, want);
    (void)s_raw;
    return want;
}

/* ---------------- ISO9660 ---------------- */
typedef struct { int lba; u32 size; int dir; char name[64]; } Entry;

static int parse_entry(const u8 *d, Entry *e)
{
    int len = d[0];
    if (!len) return 0;
    e->lba = d[2] | (d[3] << 8) | (d[4] << 16) | (d[5] << 24);
    e->size = d[10] | (d[11] << 8) | (d[12] << 16) | ((u32)d[13] << 24);
    e->dir = (d[25] & 2) != 0;
    int nl = d[32];
    if (nl > 63) nl = 63;
    memcpy(e->name, d + 33, nl);
    e->name[nl] = 0;
    char *semi = strchr(e->name, ';');
    if (semi) *semi = 0;
    return len;
}

static int root_entry(Entry *e)
{
    u8 sec[2048];
    disc_read(16, 2048, sec);
    return parse_entry(sec + 156, e);
}

/* iterate a directory; cb returns nonzero to stop */
typedef int (*DirCb)(const Entry *e, void *ud);

static int iter_dir(const Entry *dir, DirCb cb, void *ud)
{
    u32 done = 0;
    int lba = dir->lba;
    u8 sec[2048];
    while (done < dir->size) {
        disc_read(lba++, 2048, sec);
        int i = 0;
        while (i < 2048) {
            if (!sec[i]) break;
            Entry e;
            int len = parse_entry(sec + i, &e);
            if (!len) break;
            if (!(e.name[0] == 0 || e.name[0] == 1) && cb(&e, ud)) return 1;
            i += len;
        }
        done += 2048;
    }
    return 0;
}

typedef struct { const char *name; int want_dir; Entry out; int found; } FindCtx;

static int find_cb(const Entry *e, void *ud)
{
    FindCtx *f = ud;
    if (e->dir == f->want_dir && !strcasecmp(e->name, f->name)) { f->out = *e; f->found = 1; return 1; }
    return 0;
}

static int find_in(const Entry *dir, const char *name, int want_dir, Entry *out)
{
    FindCtx f = { name, want_dir, { 0 }, 0 };
    iter_dir(dir, find_cb, &f);
    if (f.found) *out = f.out;
    return f.found;
}

static void strip_version(char *s) { char *p = strchr(s, ';'); if (p) *p = 0; }

static int locate_path(const char *path, Entry *out)
{
    while (*path == '/' || *path == '\\') path++;
    Entry cur;
    if (!root_entry(&cur)) return 0;
    char part[128];
    const char *p = path;
    for (;;) {
        const char *s = p;
        while (*s && *s != '/' && *s != '\\') s++;
        int n = (int)(s - p);
        if (n > 127) n = 127;
        memcpy(part, p, n);
        part[n] = 0;
        strip_version(part);
        if (!*s) return find_in(&cur, part, 0, out);
        if (!find_in(&cur, part, 1, &cur)) return 0;
        p = s + 1;
    }
}

typedef struct { const char *name; char *path; int plen; Entry out; int found; char base[256]; } SearchCtx;

static int search_rec(const Entry *dir, SearchCtx *sc);

static int search_cb(const Entry *e, void *ud)
{
    SearchCtx *sc = ud;
    if (e->dir) {
        char saved[256];
        snprintf(saved, sizeof saved, "%s", sc->base);
        if (sc->base[0]) { strncat(sc->base, "/", sizeof sc->base - strlen(sc->base) - 1); }
        strncat(sc->base, e->name, sizeof sc->base - strlen(sc->base) - 1);
        int r = search_rec(e, sc);
        if (r) return 1;
        snprintf(sc->base, sizeof sc->base, "%s", saved);
        return 0;
    }
    if (!strcasecmp(e->name, sc->name)) {
        sc->out = *e;
        sc->found = 1;
        if (sc->path) {
            if (sc->base[0]) snprintf(sc->path, sc->plen, "%s/%s", sc->base, e->name);
            else snprintf(sc->path, sc->plen, "%s", e->name);
        }
        return 1;
    }
    return 0;
}

static int search_rec(const Entry *dir, SearchCtx *sc) { return iter_dir(dir, search_cb, sc); }

static int search_name(const char *name, Entry *out, char *path, int plen)
{
    Entry root;
    if (!root_entry(&root)) return 0;
    SearchCtx sc;
    memset(&sc, 0, sizeof sc);
    sc.name = name;
    sc.path = path;
    sc.plen = plen;
    search_rec(&root, &sc);
    if (sc.found && out) *out = sc.out;
    return sc.found;
}

int iso_locate(const char *path, int *lba, u32 *size)
{
    Entry e;
    if (!locate_path(path, &e)) {
        const char *b = strrchr(path, '/');
        const char *b2 = strrchr(path, '\\');
        if (b2 && (!b || b2 > b)) b = b2;
        char base[64];
        snprintf(base, sizeof base, "%s", b ? b + 1 : path);
        strip_version(base);
        if (!search_name(base, &e, NULL, 0)) return 0;
    }
    *lba = e.lba;
    *size = e.size;
    return 1;
}

int iso_find_file(const char *name, char *out_path, int out_len)
{
    return search_name(name, NULL, out_path, out_len);
}

u8 *iso_read_file(const char *path, u32 *size)
{
    Entry e;
    if (!locate_path(path, &e)) return NULL;
    u8 *buf = malloc(e.size ? e.size : 1);
    if (!buf) return NULL;
    u32 done = 0;
    int lba = e.lba;
    u8 sec[2048];
    while (done < e.size) {
        disc_read(lba++, 2048, sec);
        u32 n = e.size - done < 2048 ? e.size - done : 2048;
        memcpy(buf + done, sec, n);
        done += n;
    }
    *size = e.size;
    return buf;
}
