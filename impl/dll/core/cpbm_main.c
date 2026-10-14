/* cpbm_main.c - startup sequence for the identity mod.
 *
 * Order matters and is deliberate:
 *   1. open the log        - so every later step can explain itself
 *   2. hook D3D9           - from DllMain, because the hooks must be in place
 *                            before the game builds its renderer
 *   3. am I switched on?   - if not, touch nothing at all and say so
 *   4. is this the right exe? - if not, refuse
 *   5. watch game.log      - EU4 evaluates the conditions and logs its verdict;
 *                            we read that and swap the texture
 *
 * There is no rules engine here any more. An earlier design parsed a DSL of
 * trigger conditions and evaluated them against country state read out of
 * engine memory. That state could never be read - see RE-NOTES.md - and the
 * whole layer was replaced by letting EU4 evaluate its own script and tell us
 * the answer through `log`. The DSL, its tests and its converter are still in
 * the repo, but nothing calls them.
 */

#include "cpbm_env.h"
#include "cpbm_log.h"
#include "cpbm_d3d9.h"
#include "cpbm_script.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define CPBM_VERSION "0.3.0"

/* The graphics side is ON by default: it is the entire point of the mod, and
 * making people create a marker file by hand to get any behaviour would be a
 * poor way to ship. cpbm.NOD3D9 turns it off for bisection. */
static int graphics_enabled(void)
{
    if (cpbm_switch(L"cpbm.NOD3D9")) {
        cpbm_log("graphics: disabled by cpbm.NOD3D9 - no texture is touched");
        return 0;
    }
    return 1;
}

static DWORD WINAPI worker(LPVOID unused)
{
    char hex[65];
    cpbm_gate_result gate;

    (void)unused;

    cpbm_log_open();
    cpbm_log("cpbm-identity " CPBM_VERSION " attached via winmm proxy shim");

    /* No "is my mod enabled" gate any more.
     *
     * There used to be one, keyed on cpbm_identity.mod appearing in
     * dlc_load.json. That breaks the moment the script is folded into CPBM
     * itself and the companion mod stops existing - the DLL would go dormant
     * and there would be no flags and no visible reason why.
     *
     * It is also unnecessary. Nothing happens unless some mod writes a
     * `dynamic_country_flag` line to game.log, so the script's presence IS the
     * switch: no script, no lines, no writes. That is self-enforcing, works for
     * any number of mods, and cannot fall out of step with where the files
     * live. cpbm.NOD3D9 remains as a hard off. */
    cpbm_log("active: waiting for dynamic_country_flag lines in game.log "
             "(no script writing them means nothing is touched)");

    gate = cpbm_build_gate(hex);
    switch (gate) {
    case CPBM_GATE_PASS:
        cpbm_log("build gate PASS: %s (%s)", hex, cpbm_pinned_build());
        break;
    case CPBM_GATE_MISMATCH:
        /* Players read this line, not developers, so it says what to do. */
        cpbm_log("build gate FAIL - THIS IS WHY NOTHING HAPPENS, and it is deliberate.");
        cpbm_log("  your eu4.exe : %s", hex);
        cpbm_log("  expected     : %s", cpbm_pinned_hash());
        cpbm_log("  which is     : EU4 %s", cpbm_pinned_build());
        cpbm_log("  Your EU4 is a different build from the one this was built");
        cpbm_log("  against, so the mod is refusing to attach rather than risk");
        cpbm_log("  your game. Nothing has been changed and nothing is damaged.");
        cpbm_log("  Fix: use EU4 %s (Steam: right-click EU4 -> Properties ->", cpbm_pinned_build());
        cpbm_log("  Betas), or ask for a build made against your version.");
        cpbm_log_close();
        return 0;
    case CPBM_GATE_UNREADABLE:
        cpbm_log("build gate FAIL: could not read eu4.exe next to this DLL.");
        cpbm_log("REFUSING to attach.");
        cpbm_log_close();
        return 0;
    }

    if (graphics_enabled()) cpbm_script_start();

    cpbm_log("startup complete");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE t;
        DisableThreadLibraryCalls(inst);

        /* The D3D9 hook has to be installed before the game creates its
         * renderer, and the worker thread is far too late - it spends a second
         * or two hashing eu4.exe first. Patching import-table pointers is a few
         * memory writes with no loader involvement, so it is safe here. */
        cpbm_log_open();
        cpbm_d3d9_init();

        /* Everything else runs off the loader lock. */
        t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (t) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        cpbm_log_close();
    }
    return TRUE;
}
