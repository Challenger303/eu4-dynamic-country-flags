/* cpbm_env.c - where things are, whether we are switched on, and whether this
 * is the executable we were built against.
 */

#include "cpbm_env.h"
#include "cpbm_sha256.h"

#include <shlobj.h>
#include <string.h>

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

/* The build this DLL's offsets were discovered against.
 *
 * Measured on the target install: EU4 v1.37.5.0 Inca (491d), Steam, Windows
 * x64. Any other executable moves every address we would write to, so a
 * mismatch is refused rather than risked. Re-pin this after every game patch,
 * and expect to redo the hook discovery with it. */
static const char k_pinned_hash[]  =
    "9ad3efe1af169f40ee577f9dae5debbc87af6fb8b5450fb345ebf110dc4d771a";
static const char k_pinned_build[] = "1.37.5.0 Inca (491d)";

const char *cpbm_pinned_hash(void)  { return k_pinned_hash; }
const char *cpbm_pinned_build(void) { return k_pinned_build; }

/* ------------------------------------------------------------- paths --- */

static int append(wchar_t *out, DWORD cap, DWORD at, const wchar_t *s)
{
    DWORD i = at;
    for (; *s; s++) {
        if (i >= cap - 1) return 0;
        out[i++] = *s;
    }
    out[i] = 0;
    return 1;
}

int cpbm_game_path(wchar_t *out, DWORD cap, const wchar_t *leaf)
{
    DWORD n = GetModuleFileNameW((HMODULE)&__ImageBase, out, cap);
    DWORD i, cut = 0;

    if (n == 0 || n >= cap) return 0;
    for (i = n; i > 0; i--) {
        if (out[i - 1] == L'\\' || out[i - 1] == L'/') { cut = i; break; }
    }
    if (!cut) return 0;
    out[cut] = 0;
    return leaf ? append(out, cap, cut, leaf) : 1;
}

int cpbm_docs_path(wchar_t *out, DWORD cap, const wchar_t *leaf)
{
    PWSTR docs = NULL;
    DWORD at = 0;
    int   ok;

    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Documents, 0, NULL, &docs)) || !docs)
        return 0;

    out[0] = 0;
    ok = append(out, cap, 0, docs);
    CoTaskMemFree(docs);
    if (!ok) return 0;

    while (out[at]) at++;
    if (at && (out[at - 1] == L'\\' || out[at - 1] == L'/')) { at--; out[at] = 0; }

    if (!append(out, cap, at, L"\\Paradox Interactive\\Europa Universalis IV\\")) return 0;
    while (out[at]) at++;

    return leaf ? append(out, cap, at, leaf) : 1;
}

/* -------------------------------------------------------- on/off switch - */

/* Read a whole small file into a NUL-terminated heap buffer. */
static char *slurp(const wchar_t *path, DWORD *size_out)
{
    HANDLE h;
    DWORD  size, got;
    char  *buf;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > (8u << 20)) { CloseHandle(h); return NULL; }

    buf = (char *)HeapAlloc(GetProcessHeap(), 0, size + 1);
    if (!buf) { CloseHandle(h); return NULL; }

    if (!ReadFile(h, buf, size, &got, NULL)) {
        HeapFree(GetProcessHeap(), 0, buf);
        CloseHandle(h);
        return NULL;
    }
    buf[got] = 0;
    if (size_out) *size_out = got;
    CloseHandle(h);
    return buf;
}

int cpbm_mod_enabled(void)
{
    wchar_t path[MAX_PATH];
    char   *json;
    int     found = 0;

    if (cpbm_switch(L"cpbm.FORCEDLL")) return 1;   /* probe sessions without the data mod */

    if (!cpbm_docs_path(path, MAX_PATH, L"dlc_load.json")) return 0;

    json = slurp(path, NULL);
    if (!json) return 0;

    /* dlc_load.json lists only *enabled* mods (its other key is disabled_dlcs),
     * so presence of our .mod filename anywhere in it means we are switched on.
     * Deliberately a substring test: the launcher has changed this file's exact
     * shape between patches, and a real JSON parser would be a bigger thing to
     * get wrong than the question deserves. */
    found = (strstr(json, "cpbm_identity.mod") != NULL);

    HeapFree(GetProcessHeap(), 0, json);
    return found;
}

int cpbm_switch(const wchar_t *name)
{
    wchar_t path[MAX_PATH];
    DWORD   attr;

    if (!cpbm_game_path(path, MAX_PATH, name)) return 0;
    attr = GetFileAttributesW(path);
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

/* ---------------------------------------------------------- build gate -- */

cpbm_gate_result cpbm_build_gate(char hex_out[65])
{
    wchar_t       path[MAX_PATH];
    unsigned char digest[32];

    hex_out[0] = 0;

    if (!cpbm_game_path(path, MAX_PATH, L"eu4.exe")) return CPBM_GATE_UNREADABLE;
    if (!cpbm_sha256_file(path, digest))             return CPBM_GATE_UNREADABLE;

    cpbm_sha256_hex(digest, hex_out);
    return strcmp(hex_out, k_pinned_hash) == 0 ? CPBM_GATE_PASS : CPBM_GATE_MISMATCH;
}
