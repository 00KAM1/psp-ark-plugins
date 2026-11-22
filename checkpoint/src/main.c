/*
 * Checkpoint - automatic history of game saves for PSP on ARK-5.
 * Kernel plugin for the GAME context.
 */
#include <pspkernel.h>
#include <pspinit.h>
#include <string.h>
#include <systemctrl.h>
#include <systemctrl_se.h>
#include "common.h"

PSP_MODULE_INFO("Checkpoint", 0x1007, 1, 0);

int ui_thread(SceSize args, void *argp);

int module_start(SceSize args, void *argp)
{
    SceIoStat st;
    sceIoMkdir(BT_DIR, 0777);
    if (sceIoGetstat(BT_LOG_PATH, &st) >= 0 && st.st_size > 8 * 1024) {
        sceIoRemove(BT_DIR "/checkpoint.old");
        if (sceIoRename(BT_LOG_PATH, BT_DIR "/checkpoint.old") < 0) sceIoRemove(BT_LOG_PATH);
    }
    config_load();

    /* PSP games only: not the XMB, not PS1 games, not ARK's launcher. */
    if (sceKernelInitKeyConfig() != PSP_INIT_KEYCONFIG_GAME) return 1;
    {
        const char *iso = sctrlSEGetUmdFile();
        const char *exe = sceKernelInitFileName();
        int (*is_launcher)(void) = (void *)sctrlHENFindFunction("SystemControl", "ArkCtrl", 0x65C8280F);
        int n = exe ? (int)strlen(exe) : 0;
        int has_iso = iso && iso[0];
        int is_eboot = n >= 9 && !strcmp(exe + n - 9, "EBOOT.PBP");
        int is_disc = n >= 6 && !strncmp(exe, "disc0:", 6);
        int launcher = (n && is_launcher && is_launcher()) ||
                       (n && (strstr(exe, "VBOOT.PBP") || strstr(exe, "/ARK_01234/")));
        if (launcher || !(has_iso || is_eboot || is_disc)) return 1;
    }

    int hooked = hook_install();
    sq_log("Checkpoint %s (built " BT_BUILD_STAMP ") start: hooks %s, keep %d, min %d min", BT_VERSION_STR,
           hooked == 0 ? "ok" : "NOT installed", g_cfg.keep, g_cfg.min_minutes);

    SceUID th = sceKernelCreateThread("CheckpointUI", ui_thread, 0x18, 0x4000, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    return 0;
}

int module_stop(SceSize args, void *argp) { return 0; }
