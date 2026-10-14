/* winmm_shim.c - the proxy that gets us loaded, and nothing else.
 *
 * Why winmm and not version:
 *   Windows loads a DLL from the game folder before System32, so naming a DLL
 *   after one the game imports gets it loaded with zero injector. eu4.exe
 *   imports 22 DLLs; of the ones that are not protected KnownDLLs, `version`
 *   is already taken by Mare Liberum, and `imm32` cannot be used at all
 *   because it IS a KnownDLL (Windows always loads those from System32 and
 *   ignores the copy next to the exe). That leaves winmm.
 *
 * Why proxying winmm cannot break your sound:
 *   This DLL exports all 181 of winmm's entry points as PE *forwarders* to
 *   winmm_orig.dll - a verbatim copy of the genuine System32 winmm.dll that
 *   install.ps1 places next to it. The loader resolves those forwarders itself,
 *   so a waveOutWrite call reaches the real waveOutWrite without passing
 *   through a single instruction of ours. There is no wrapper to get wrong.
 *   See gen-winmm-def.ps1.
 *
 * Why the real mod is a separate file:
 *   All this shim does is load cpbm_identity.dll, which is where the whole mod
 *   lives. The hijacked name stays confined to this one small file; the mod
 *   keeps its own name, is the thing you rebuild while iterating, and can be
 *   deleted on its own to disable the mod while leaving the (inert) proxy.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Provided by the linker; the base of this module. Using it rather than the
 * DllMain HINSTANCE keeps shim_dir() usable from anywhere. */
EXTERN_C IMAGE_DOS_HEADER __ImageBase;

static HMODULE g_core;

/* Write the full path of this DLL's own directory, with trailing backslash,
 * then append `leaf`. No CRT: this file links without one. */
static int build_sibling_path(wchar_t *out, DWORD cap, const wchar_t *leaf)
{
    DWORD n = GetModuleFileNameW((HMODULE)&__ImageBase, out, cap);
    DWORD cut = 0, i;
    const wchar_t *s;
    wchar_t *w;

    if (n == 0 || n >= cap) return 0;
    for (i = n; i > 0; i--) {
        if (out[i - 1] == L'\\' || out[i - 1] == L'/') { cut = i; break; }
    }
    if (!cut) return 0;

    w = out + cut;
    for (s = leaf; *s; s++) {
        if ((DWORD)(w - out) >= cap - 1) return 0;
        *w++ = *s;
    }
    *w = 0;
    return 1;
}

/* If the core will not load there is no logger yet, so say so here. This is the
 * only file the shim ever writes, and only on failure. */
static void shim_report_failure(DWORD err)
{
    wchar_t path[MAX_PATH];
    HANDLE  h;
    char    line[256];
    int     n;
    DWORD   wrote;

    if (!build_sibling_path(path, MAX_PATH, L"cpbm-identity.log")) return;

    h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    n = wsprintfA(line,
                  "winmm shim: FAILED to load cpbm_identity.dll (error %lu). "
                  "The proxy is inert; the game runs vanilla.\r\n", err);
    if (n > 0) WriteFile(h, line, (DWORD)n, &wrote, NULL);
    CloseHandle(h);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t path[MAX_PATH];

        /* We never use thread notifications, and declining them keeps this DLL
         * off the path of every thread the game creates. */
        DisableThreadLibraryCalls(inst);

        if (build_sibling_path(path, MAX_PATH, L"cpbm_identity.dll")) {
            /* Loading here runs only the core's DllMain, which is deliberately
             * tiny - it starts a worker thread and returns. Everything that
             * could block (hashing eu4.exe, reading JSON, parsing rules)
             * happens on that thread, off the loader lock. */
            g_core = LoadLibraryW(path);
            if (!g_core) shim_report_failure(GetLastError());
        }
    }
    return TRUE;
}
