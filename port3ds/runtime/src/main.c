#include "rt.h"

/* shared boot sequence (port of the generated Entry.Run) */

int rt_main(const char *cue_path)
{
    rt_log("[rt] SymphonyRecomp 3DS runtime\n");
    mem_init();
    gpu_init();
    spu_init();
    xa_reset();
    mdec_init();
    cdc_init();
    bios_init();
    dispatch_init();
    libcd_reset();
    libcdstream_reset();

    if (!disc_open(cue_path)) rt_fatal("could not open disc image:\n%s", cue_path);

    char path[300];
    snprintf(path, sizeof path, "%s/carda.sav", host_data_dir());
    mc_load(&g_card[0], path);
    snprintf(path, sizeof path, "%s/cardb.sav", host_data_dir());
    mc_load(&g_card[1], path);
    g_card[1].enabled = 0;

    host_audio_init();

    cdc_load_to_memory(g_boot_exe, g_boot_dest, 0x800, (int)g_boot_text_size);
    dispatch_load("main");

    memset(&g_cpu, 0, sizeof g_cpu);
    g_cpu.r[28] = g_boot_gp;
    g_cpu.r[29] = g_boot_sp;
    g_cpu.r[30] = g_boot_sp;
    g_cpu.r[31] = 0;
    rt_log("[rt] boot %s pc=0x%08X\n", g_boot_exe, g_boot_pc);
    if (g_boot_main) g_boot_main(&g_cpu);
    else dispatch_call(&g_cpu, g_boot_pc);
    rt_log("[rt] game returned\n");
    return 0;
}
