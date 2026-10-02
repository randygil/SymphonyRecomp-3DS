#include "rt.h"
#include <stdlib.h>

/* address -> function map of the currently loaded overlays (open addressing) */
#define MAP_BITS 16
#define MAP_SIZE (1u << MAP_BITS)
#define MAX_OVERLAYS 256

static u32 s_keys[MAP_SIZE];
static FuncPtr s_vals[MAP_SIZE];
static u32 s_used;

static int s_active[MAX_OVERLAYS];
static int s_active_n;
static int s_pending = -1;
static void set_pending(int i);

static inline u32 hash(u32 a) { return ((a >> 2) * 2654435761u) >> (32 - MAP_BITS); }

static void map_clear(void)
{
    memset(s_keys, 0, sizeof s_keys);
    memset(s_vals, 0, sizeof s_vals);
    s_used = 0;
}

static void map_put(u32 addr, FuncPtr fn)
{
    u32 h = hash(addr);
    for (;;) {
        if (s_keys[h] == addr) { s_vals[h] = fn; return; }
        if (s_keys[h] == 0) {
            if (s_used >= MAP_SIZE - 1) rt_fatal("dispatch map full");
            s_keys[h] = addr;
            s_vals[h] = fn;
            s_used++;
            return;
        }
        h = (h + 1) & (MAP_SIZE - 1);
    }
}

FuncPtr dispatch_lookup(u32 addr)
{
    u32 h = hash(addr);
    for (;;) {
        u32 k = s_keys[h];
        if (k == addr) return s_vals[h];
        if (k == 0) return NULL;
        h = (h + 1) & (MAP_SIZE - 1);
    }
}

static int find_overlay(const char *name)
{
    for (int i = 0; i < g_overlay_count; i++) {
        const char *a = g_overlays[i].name, *b = name;
        while (*a && *b && ((*a | 0x20) == (*b | 0x20))) { a++; b++; }
        if (!*a && !*b) return i;
    }
    return -1;
}

static void add_funcs(int idx)
{
    const OverlayDesc *o = &g_overlays[idx];
    for (int i = 0; i < o->count; i++) map_put(o->funcs[i].addr, o->funcs[i].fn);
}

static void rebuild(void)
{
    map_clear();
    for (int i = 0; i < s_active_n; i++) add_funcs(s_active[i]);
}

static int remove_active(int idx)
{
    for (int i = 0; i < s_active_n; i++) {
        if (s_active[i] == idx) {
            memmove(&s_active[i], &s_active[i + 1], (s_active_n - i - 1) * sizeof(int));
            s_active_n--;
            return 1;
        }
    }
    return 0;
}

void dispatch_init(void)
{
    s_active_n = 0;
    set_pending(-1);
    map_clear();
}

void dispatch_reset(void) { dispatch_init(); }

static void load_idx(int idx)
{
    const OverlayDesc *o = &g_overlays[idx];
    int already = remove_active(idx);
    if (!already && o->base != 0 && o->size != 0) {
        u32 ns = o->base & 0x1FFFFFFFu, ne = ns + o->size;
        int removed = 0;
        for (int i = 0; i < s_active_n;) {
            const OverlayDesc *x = &g_overlays[s_active[i]];
            if (x->base != 0 && x->size != 0) {
                u32 s = x->base & 0x1FFFFFFFu, e = s + x->size;
                if (s >= ns && e <= ne) {
                    rt_log("[dispatch] overlay %s overwritten by %s\n", x->name, o->name);
                    memmove(&s_active[i], &s_active[i + 1], (s_active_n - i - 1) * sizeof(int));
                    s_active_n--;
                    removed = 1;
                    continue;
                }
            }
            i++;
        }
        if (removed) rebuild();
    }
    if (s_active_n >= MAX_OVERLAYS) rt_fatal("too many overlays");
    s_active[s_active_n++] = idx;
    add_funcs(idx);
    if (!already) rt_log("[dispatch] loaded overlay: %s\n", o->name);
}

void dispatch_load(const char *name)
{
    int idx = find_overlay(name);
    if (idx < 0) rt_fatal("overlay not registered: %s", name);
    load_idx(idx);
}

int dispatch_try_load(const char *name)
{
    int idx = find_overlay(name);
    if (idx < 0) return 0;
    load_idx(idx);
    return 1;
}

u32 g_watch_lo = 0x80000000u;

static void set_pending(int i)
{
    s_pending = i;
    g_watch_lo = i < 0 ? 0x80000000u : (g_overlays[i].base & 0x1FFFFFFFu & RAM_MASK);
}

void dispatch_load_by_lba(int lba)
{
    for (int i = 0; i < g_overlay_count; i++) {
        if (g_overlays[i].lba >= 0 && g_overlays[i].lba == lba) {
            if (g_overlays[i].base == 0) load_idx(i);
            else set_pending(i);
            return;
        }
    }
}

void dispatch_watch_hit(u32 off)
{
    (void)off;
    int p = s_pending;
    set_pending(-1);
    if (p >= 0) load_idx(p);
}

void dispatch_notify_write(u32 phys, u32 len)
{
    if (s_pending < 0) return;
    u32 start = g_watch_lo;
    if (phys + len <= start || phys >= start + 0x800u) return;
    dispatch_watch_hit(phys);
}

void dispatch_clear_pending(void) { set_pending(-1); }

void dispatch_call(Cpu *restrict c, u32 addr)
{
    if (bios_try_dispatch(c, addr)) return;
    FuncPtr fn = dispatch_lookup(addr);
    if (UNLIKELY(!fn)) {
        char act[512];
        int n = 0;
        act[0] = 0;
        for (int i = 0; i < s_active_n && n < 480; i++)
            n += snprintf(act + n, sizeof act - n, "%s ", g_overlays[s_active[i]].name);
        rt_fatal("unmapped call: 0x%08X (ra=0x%08X)\nactive: %s", addr, c->r[31], act);
    }
    fn(c);
}
