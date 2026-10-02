/* RecompOne C runtime (3DS port of SymphonyRecomp) */
#ifndef RECOMP_H
#define RECOMP_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef struct Cpu {
    u32 r[32];
    u32 hi, lo;
    u32 sr, cause, epc, badvaddr, prid;
} Cpu;

typedef void (*FuncPtr)(Cpu *restrict c);

typedef struct FuncEntry {
    u32 addr;
    FuncPtr fn;
} FuncEntry;

typedef struct OverlayDesc {
    const char *name;
    int lba;
    u32 base;
    u32 size;
    const FuncEntry *funcs;
    int count;
    u32 sig, sig_len;   /* FNV-1a of the first sig_len bytes of the overlay file */
} OverlayDesc;

/* generated tables */
extern const OverlayDesc g_overlays[];
extern const int g_overlay_count;
extern const char g_boot_exe[];
extern const u32 g_boot_dest, g_boot_text_size, g_boot_pc, g_boot_gp, g_boot_sp;
extern const FuncPtr g_boot_main;

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* ---- memory ---- */
#define RAM_SIZE 0x200000u
#define RAM_MASK (RAM_SIZE - 1u)

extern u8 g_ram[RAM_SIZE];

u32 mem_rd8_slow(u32 a);
u32 mem_rd16_slow(u32 a);
u32 mem_rd32_slow(u32 a);
void mem_wr8_slow(u32 a, u32 v);
void mem_wr16_slow(u32 a, u32 v);
void mem_wr32_slow(u32 a, u32 v);

/* any address whose physical form is below 8MB is main RAM (mirrored) */
#define IS_RAM(a) (((a) & 0x1F800000u) == 0)
/* the 1KB scratchpad at 0x1F800000 (KUSEG) / 0x9F800000 (KSEG0) */
extern u8 g_scratch[0x400];
#define IS_SCRATCH(a) (((a) & 0x7FFFFC00u) == 0x1F800000u)
#define MEM_INLINE static inline __attribute__((always_inline))

/* the inline part is kept minimal: it is expanded at every load/store of ~14k
   recompiled functions, and code size is what limits the old 3DS (64MB) */
/* g_ram as an opaque register: GCC may otherwise drop the mask for addresses it
   can bound and fold "g_ram - 0x80000000" into a literal, which a 3dsx
   relocation can't represent (top nibble set). The asm is pure, so it is CSE'd. */
MEM_INLINE u8 *ram_base(void)
{
    u8 *p = g_ram;
#ifdef __arm__
    __asm__("" : "+r"(p));
#endif
    return p;
}
#define g_ram (ram_base())

MEM_INLINE u32 RD8(u32 a) {
    if (LIKELY(IS_RAM(a))) return g_ram[a & RAM_MASK];
    return mem_rd8_slow(a);
}
MEM_INLINE u32 RD16(u32 a) {
    if (LIKELY(IS_RAM(a))) return *(const u16 *)(g_ram + (a & RAM_MASK));
    return mem_rd16_slow(a);
}
MEM_INLINE u32 RD32(u32 a) {
    if (LIKELY(IS_RAM(a))) return *(const u32 *)(g_ram + (a & RAM_MASK));
    return mem_rd32_slow(a);
}
/* overlay loads are recognized lazily by the dispatcher (see dispatch.c);
   g_watch_lo is only consulted by the bulk helpers */
extern u32 g_watch_lo;

MEM_INLINE void WR8(u32 a, u32 v) {
    if (LIKELY(IS_RAM(a))) g_ram[a & RAM_MASK] = (u8)v;
    else mem_wr8_slow(a, v);
}
MEM_INLINE void WR16(u32 a, u32 v) {
    if (LIKELY(IS_RAM(a))) *(u16 *)(g_ram + (a & RAM_MASK)) = (u16)v;
    else mem_wr16_slow(a, v);
}
MEM_INLINE void WR32(u32 a, u32 v) {
    if (LIKELY(IS_RAM(a))) *(u32 *)(g_ram + (a & RAM_MASK)) = v;
    else mem_wr32_slow(a, v);
}

#undef g_ram

u32 mem_lwl(u32 cur, u32 a);
u32 mem_lwr(u32 cur, u32 a);
void mem_swl(u32 a, u32 v);
void mem_swr(u32 a, u32 v);

/* bulk helpers used by the runtime (they notify the overlay dispatcher) */
void mem_write_block(u32 addr, const u8 *src, u32 len);
void mem_read_block(u32 addr, u8 *dst, u32 len);
void mem_zero(u32 addr, u32 len);
void mem_init(void);
u8 *mem_scratchpad(void);

/* ---- dispatcher ---- */
void dispatch_call(Cpu *restrict c, u32 addr);
void dispatch_init(void);
void dispatch_reset(void);
void dispatch_load(const char *name);
int dispatch_try_load(const char *name);
void dispatch_load_by_lba(int lba);
void dispatch_notify_write(u32 phys, u32 len);
void dispatch_clear_pending(void);
FuncPtr dispatch_lookup(u32 addr);

/* ---- GTE ---- */
void gte_execute(u32 cmd);
u32 gte_read(int reg);
void gte_write(int reg, u32 v);
u32 gte_read_ctrl(int reg);
void gte_write_ctrl(int reg, u32 v);
static inline int gte_condition(void) { return 0; }

/* ---- SDK reimplementations (called from generated code) ---- */
#define SDK(n) void sdk_##n(Cpu *restrict c);
SDK(LibCd_CdInit) SDK(LibCd_CdReset) SDK(LibCd_CdControl) SDK(LibCd_CdControlF) SDK(LibCd_CdControlB)
SDK(LibCd_CdSync) SDK(LibCd_CdReady) SDK(LibCd_CdRead) SDK(LibCd_CdReadSync) SDK(LibCd_CdGetSector)
SDK(LibCd_CdDataSync) SDK(LibCd_CdSearchFile) SDK(LibCd_CdSyncCallback) SDK(LibCd_CdReadyCallback)
SDK(LibCd_CdReadCallback) SDK(LibCd_CdDataCallback) SDK(LibCd_CdStatus) SDK(LibCd_CdMode)
SDK(LibCd_CdLastCom) SDK(LibCd_CdMix)
SDK(LibEtc_VSync)
SDK(LibGpu_DrawOTag) SDK(LibGpu_DrawSync) SDK(LibGpu_PutDrawEnv) SDK(LibGpu_PutDispEnv)
SDK(LibCdStream_StSetRing) SDK(LibCdStream_StClearRing) SDK(LibCdStream_StUnSetRing) SDK(LibCdStream_StSetStream)
SDK(LibCdStream_StSetMask) SDK(LibCdStream_StGetNext) SDK(LibCdStream_StFreeRing) SDK(LibCdStream_StGetBackloc)
SDK(LibPad_PadInitDirect) SDK(LibPad_PadStartCom) SDK(LibPad_PadStopCom) SDK(LibPad_PadEnableCom)
SDK(LibPad_PadChkVsync) SDK(LibPad_PadChkMtap) SDK(LibPad_PadGetState) SDK(LibPad_PadInfoMode)
SDK(LibPad_PadInfoAct) SDK(LibPad_PadInfoComb) SDK(LibPad_PadSetMainMode) SDK(LibPad_PadSetActAlign)
SDK(LibPad_PadSetAct)
#undef SDK

/* ---- game fixes (pre hooks, return nonzero to continue) ---- */
int fix_scylla_door(Cpu *restrict c);
int fix_olrox_explosion(Cpu *restrict c);
int fix_clock_collision(Cpu *restrict c);
int fix_minotaur_werewolf(Cpu *restrict c);

#endif
