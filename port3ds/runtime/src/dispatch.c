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
/* overlays read from CD but not yet verified in RAM, at most one per range */
#define MAX_PENDING 8
static int s_pend[MAX_PENDING];
static int s_pend_n;
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

u32 g_watch_lo = 0x80000000u;   /* start of the most recent pending range (bulk writes) */

static int overlaps(const OverlayDesc *a, const OverlayDesc *b)
{
    u32 as = a->base & 0x1FFFFFFFu, bs = b->base & 0x1FFFFFFFu;
    return as < bs + b->size && bs < as + a->size;
}

static void drop_pending(int k)
{
    memmove(&s_pend[k], &s_pend[k + 1], (s_pend_n - k - 1) * sizeof(int));
    s_pend_n--;
    g_watch_lo = s_pend_n ? (g_overlays[s_pend[s_pend_n - 1]].base & 0x1FFFFFFFu & RAM_MASK) : 0x80000000u;
}

/* i < 0 clears every pending overlay */
static void set_pending(int i)
{
    if (i < 0) { s_pend_n = 0; g_watch_lo = 0x80000000u; return; }
    for (int k = 0; k < s_pend_n;) {
        if (s_pend[k] == i || overlaps(&g_overlays[s_pend[k]], &g_overlays[i])) drop_pending(k);
        else k++;
    }
    if (s_pend_n == MAX_PENDING) drop_pending(0);
    if (getenv("RT_DISPATCH_DEBUG")) rt_log("[dispatch] pending %s\n", g_overlays[i].name);
    s_pend[s_pend_n++] = i;
    g_watch_lo = g_overlays[i].base & 0x1FFFFFFFu & RAM_MASK;
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

/* bulk writes (DMA, runtime copies) over the start of a pending overlay load it */
void dispatch_notify_write(u32 phys, u32 len)
{
    for (int k = 0; k < s_pend_n; k++) {
        u32 start = g_overlays[s_pend[k]].base & 0x1FFFFFFFu & RAM_MASK;
        if (phys + len <= start || phys >= start + 0x800u) continue;
        int idx = s_pend[k];
        drop_pending(k);
        load_idx(idx);
        return;
    }
}

void dispatch_clear_pending(void) { set_pending(-1); }

static u32 sig_hash(u32 base, u32 n)
{
    u32 h = 2166136261u, o = base & RAM_MASK;
    for (u32 i = 0; i < n; i++) { h ^= g_ram[(o + i) & RAM_MASK]; h *= 16777619u; }
    return h;
}

static int sig_ok(const OverlayDesc *o) { return !o->sig_len || sig_hash(o->base, o->sig_len) == o->sig; }

/* does overlay o define a function at addr? (funcs are sorted by address) */
static int has_func(const OverlayDesc *o, u32 addr)
{
    int lo = 0, hi = o->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        u32 m = o->funcs[mid].addr;
        if (m == addr) return 1;
        if (m < addr) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* A pending overlay (read from CD) takes over its range when code in that range
   is first called, unless the active overlay that defines the called function
   is demonstrably still in RAM (its signature matches), i.e. the new one hasn't
   been copied into place yet. A matching signature of the pending overlay
   settles it directly; it often doesn't match because games patch headers. */
static void check_pending(u32 addr)
{
    for (int k = 0; k < s_pend_n; k++) {
        const OverlayDesc *p = &g_overlays[s_pend[k]];
        if (addr - p->base >= p->size) continue;
        int idx = s_pend[k];
        if (!sig_ok(p)) {
            for (int i = 0; i < s_active_n; i++) {
                const OverlayDesc *x = &g_overlays[s_active[i]];
                if (x->sig_len && has_func(x, addr) && sig_ok(x)) return;   /* old code still there */
            }
        }
        drop_pending(k);
        load_idx(idx);
        return;
    }
}

void dispatch_call(Cpu *restrict c, u32 addr)
{
    if (bios_try_dispatch(c, addr)) return;
    if (UNLIKELY(s_pend_n)) check_pending(addr);
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
