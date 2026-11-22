/*
 * Sidequest - whole-system suspend slots for PSP-2000/3000 on ARK-5.
 * Kernel plugin. Load it in GAME context from SEPLUGINS/PLUGINS.TXT.
 * MIT licensed, see LICENSE.
 */
#include <pspkernel.h>
#include <pspinit.h>
#include <pspsysmem_kernel.h>
#include <string.h>
#include <systemctrl_se.h>
#include <systemctrl.h>
#include "common.h"
#include "engine.h"
#include "staging.h"

PSP_MODULE_INFO("Sidequest", 0x1007, 0, 1);

int  ui_thread(SceSize args, void *argp);

int module_start(SceSize args, void *argp)
{
    SceIoStat st;
    sceIoMkdir(SQ_DIR, 0777);
    /* Keep the log short enough to read on the PSP: once it passes 3KB the
     * old one is set aside as sidequest.old and a new one is started. */
    if (sceIoGetstat(SQ_LOG_PATH, &st) >= 0 && st.st_size > 3 * 1024) {
        sceIoRemove(SQ_DIR "/sidequest.old");
        if (sceIoRename(SQ_LOG_PATH, SQ_DIR "/sidequest.old") < 0) sceIoRemove(SQ_LOG_PATH);
    }
    config_load();
    sq_log("Sidequest %s (built " SQ_BUILD_STAMP ") start: model=%d keyconfig=%03X", SQ_VERSION_STR,
           sceKernelGetModel(), sceKernelInitKeyConfig());

    /* Games only, and only on models this was written for. Anywhere else
     * the plugin leaves without touching anything. */
    if (sceKernelInitKeyConfig() != PSP_INIT_KEYCONFIG_GAME ||
        !staging_supported_model(sceKernelGetModel())) {
        sq_log("not a game on a PSP-2000/3000: unloading");
        return 1;
    }

    /* Only in real games: an ISO, an EBOOT.PBP or a disc. Never in ARK's
     * custom launcher or other loaders: loading a slot from there would cut
     * the launcher off without letting it exit, and it then starts in its
     * "safe start" mode next time. */
    {
        const char *iso = sctrlSEGetUmdFile();
        const char *exe = sceKernelInitFileName();
        int (*is_launcher)(void) = (void *)sctrlHENFindFunction("SystemControl", "ArkCtrl", 0x65C8280F);
        int has_iso = iso && iso[0];
        int n = exe ? (int)strlen(exe) : 0;
        int is_eboot = n >= 9 && !strcmp(exe + n - 9, "EBOOT.PBP");
        int is_disc = n >= 6 && !strncmp(exe, "disc0:", 6);
        int launcher = (n && is_launcher && is_launcher()) ||
                       (n && (strstr(exe, "VBOOT.PBP") || strstr(exe, "/ARK_01234/")));
        if (launcher || !(has_iso || is_eboot || is_disc)) {
            sq_log("not a game (%s): unloading", launcher ? "launcher" : n ? exe : "no path");
            return 1;
        }
    }

    engine_init();

    SceUID th = sceKernelCreateThread("SidequestUI", ui_thread, 0x10, 0x4000, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    return 0;
}

/* The kernel calls this just before it restarts the system: on game exit
 * and when another game is launched. Nothing to give back any more. */
int module_reboot_before(void)
{
    staging_release_all();
    return 0;
}

int module_stop(SceSize args, void *argp)
{
    return 0;
}
