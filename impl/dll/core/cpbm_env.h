#ifndef CPBM_ENV_H
#define CPBM_ENV_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* <game folder>\<leaf>. The game folder is wherever this DLL sits, which is by
 * construction next to eu4.exe - that is the only reason the proxy loads. */
int cpbm_game_path(wchar_t *out, DWORD cap, const wchar_t *leaf);

/* <Documents>\Paradox Interactive\Europa Universalis IV\<leaf>. Uses the shell's
 * known-folder API rather than %USERPROFILE%\Documents so that a Documents
 * folder redirected into OneDrive - which is the common case
 * for many installs - still resolves correctly. */
int cpbm_docs_path(wchar_t *out, DWORD cap, const wchar_t *leaf);

/* Is our companion mod ticked in the launcher's playset? Reads the same
 * dlc_load.json the game itself reads. This is the whole on/off switch. */
int cpbm_mod_enabled(void);

typedef enum {
    CPBM_GATE_PASS = 0,     /* eu4.exe is the build we were built against  */
    CPBM_GATE_MISMATCH,     /* a different build: refuse, do nothing       */
    CPBM_GATE_UNREADABLE    /* could not hash it: refuse, do nothing       */
} cpbm_gate_result;

/* Hashes eu4.exe and compares against the pinned build. `hex_out` receives the
 * hash actually found (65 bytes) so the log can name it on a mismatch. */
cpbm_gate_result cpbm_build_gate(char hex_out[65]);

const char *cpbm_pinned_hash(void);
const char *cpbm_pinned_build(void);

/* An empty file of this name in the game folder turns a feature off; bisecting
 * with these is how a bad hook gets found. */
int cpbm_switch(const wchar_t *name);

#endif
