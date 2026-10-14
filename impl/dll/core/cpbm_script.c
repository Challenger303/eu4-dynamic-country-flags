/* cpbm_script.c - let EU4 decide, and listen for what it decided.
 *
 * WHY
 * The conditions this mod needs to answer - has_reform, government, owns
 * province, does a country exist, what year is it - are the whole EU4 trigger
 * vocabulary. Reading them out of engine memory means a separate reverse
 * engineering job per predicate, and the simplest of them (the date) already
 * resisted several attempts.
 *
 * EU4 will evaluate them for us. The script effect `log = "..."` writes a line
 * to logs/game.log, so a hidden event with any trigger you like can announce
 * its own result:
 *
 *     immediate = { log = "dynamic_country_flag FRA FRA_white" }
 *
 * We tail that file. The engine does the thinking in its own scripting
 * language, which is both the thing the mod author already writes and a stable
 * documented interface - unlike struct offsets, which move on every patch.
 *
 * LINE FORMAT
 *     dynamic_country_flag <TAG> <flagname>     show flagname on that country
 *     dynamic_country_flag <TAG> -              restore that country's own flag
 *
 * `cpbm_flag` is accepted in place of `dynamic_country_flag`: it is what every
 * script written before 0.3.0 says, and players receive mod updates and this
 * DLL at different times.
 *
 * Anything else on the line is ignored, so the usual EU4 log prefix and any
 * interpolated text around it are harmless.
 */

#include "cpbm_env.h"
#include "cpbm_log.h"
#include "cpbm_d3d9.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *word; size_t len; } k_keywords[] = {
    { "dynamic_country_flag", sizeof("dynamic_country_flag") - 1 },
    { "cpbm_flag",            sizeof("cpbm_flag") - 1 },
};

/* ---------------------------------------------------- tag -> atlas slot -- */
/* Straight from the atlas manifest, which lists every flag name in slot order.
 * That is the authoritative mapping - deriving it from the tag declaration
 * files would only be a reconstruction of what this file already states.
 *
 * EU4 can rewrite the manifest AFTER we first read it, in the same launch (seen
 * right after mods that add tags are switched on). Every tag past the first
 * change then maps one or more slots off, and flags paint onto a neighbouring
 * country while every log line says OK. So the file's write time is kept and
 * checked before each instruction, and a rewrite triggers a re-read. */

#define MAX_FLAGNAMES 8192
typedef struct { char name[32]; int slot; } flagslot;
static flagslot *g_slots;
static int       g_nslots;
static FILETIME  g_slots_time;

static int manifest_path(wchar_t *path)
{
    return cpbm_docs_path(path, MAX_PATH, L"gfx\\flags\\flagfiles.txt");
}

static int manifest_time(FILETIME *ft)
{
    wchar_t path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!manifest_path(path) || !GetFileAttributesExW(path, GetFileExInfoStandard, &a)) return 0;
    *ft = a.ftLastWriteTime;
    return 1;
}

static int slot_of(const char *name)
{
    int i;
    for (i = 0; i < g_nslots; i++)
        if (_stricmp(g_slots[i].name, name) == 0) return g_slots[i].slot;
    return -1;
}

int cpbm_script_load_slots(void)
{
    wchar_t  path[MAX_PATH];
    HANDLE   h;
    DWORD    size, got = 0;
    char    *buf, *p;
    char     hash[40];
    int      slot = -1, n = 0;           /* first token is the set hash */
    FILETIME ft;

    hash[0] = '?'; hash[1] = 0;
    if (!manifest_path(path)) return 0;
    if (!manifest_time(&ft)) memset(&ft, 0, sizeof ft);

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        cpbm_log("script: no atlas manifest at %S - launch the game once", path);
        return 0;
    }
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > (4u << 20)) { CloseHandle(h); return 0; }

    buf = (char *)HeapAlloc(GetProcessHeap(), 0, size + 1);
    if (!buf) { CloseHandle(h); return 0; }
    if (!ReadFile(h, buf, size, &got, NULL)) { HeapFree(GetProcessHeap(), 0, buf); CloseHandle(h); return 0; }
    buf[got] = 0;
    CloseHandle(h);

    if (!g_slots) {
        g_slots = (flagslot *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                        sizeof(flagslot) * MAX_FLAGNAMES);
        if (!g_slots) { HeapFree(GetProcessHeap(), 0, buf); return 0; }
    }

    for (p = strtok(buf, " \t\r\n"); p && n < MAX_FLAGNAMES; p = strtok(NULL, " \t\r\n")) {
        if (slot < 0) {
            strncpy(hash, p, sizeof(hash) - 1);
            hash[sizeof(hash) - 1] = 0;
        } else {
            char *dot = strstr(p, ".tga");
            if (dot) *dot = 0;
            if (strlen(p) < sizeof(g_slots[0].name)) {
                strcpy(g_slots[n].name, p);
                g_slots[n].slot = slot;
                n++;
            }
        }
        slot++;
    }
    HeapFree(GetProcessHeap(), 0, buf);

    /* Remember the time even for an empty read: a file caught mid-write gets a
     * new time when EU4 finishes, and that is what triggers the next attempt. */
    g_slots_time = ft;
    if (n == 0) {
        cpbm_log("script: the atlas manifest had no entries (mid-write?) - keeping the "
                 "previous slot map");
        return g_nslots > 0;
    }
    g_nslots = n;
    cpbm_log("script: %d flag slots mapped from the atlas manifest (set-hash %s)", g_nslots, hash);
    return 1;
}

/* --------------------------------------------------------- what is shown -- */
/* Remember what each country is currently displaying, so a line repeated every
 * pulse costs nothing and only real changes reach the renderer. The slot is
 * kept too, so a manifest re-read can tell which paints landed in a slot that
 * turned out to belong to someone else. */

#define MAX_SHOWN 8192
static struct { char tag[4]; char flag[32]; int slot; } g_shown[MAX_SHOWN];
static int g_nshown;

static int already_showing(const char *tag, const char *flag, int slot)
{
    int i;
    for (i = 0; i < g_nshown; i++) {
        if (_stricmp(g_shown[i].tag, tag) != 0) continue;
        g_shown[i].slot = slot;
        if (_stricmp(g_shown[i].flag, flag) == 0) return 1;
        strncpy(g_shown[i].flag, flag, sizeof(g_shown[i].flag) - 1);
        g_shown[i].flag[sizeof(g_shown[i].flag) - 1] = 0;
        return 0;
    }
    if (g_nshown < MAX_SHOWN) {
        strncpy(g_shown[g_nshown].tag, tag, 3);
        g_shown[g_nshown].tag[3] = 0;
        strncpy(g_shown[g_nshown].flag, flag, sizeof(g_shown[0].flag) - 1);
        g_shown[g_nshown].flag[sizeof(g_shown[0].flag) - 1] = 0;
        g_shown[g_nshown].slot = slot;
        g_nshown++;
    }
    return 0;
}

/* The manifest was rewritten since we read it: re-map, put back any slot we
 * painted that is no longer that country's, and forget the cache so every
 * country is painted again at its real slot on the next line about it. */
static void check_manifest(void)
{
    FILETIME ft;
    int i, moved = 0;

    if (!manifest_time(&ft) || CompareFileTime(&ft, &g_slots_time) == 0) return;

    cpbm_log("script: flagfiles.txt was rewritten after it was read (EU4 rebuilt its tag "
             "list) - re-mapping tags to atlas slots");
    if (!cpbm_script_load_slots()) return;

    for (i = 0; i < g_nshown; i++) {
        int now_slot = slot_of(g_shown[i].tag);
        if (now_slot == g_shown[i].slot) continue;
        moved++;
        if (g_shown[i].slot >= 0 && strcmp(g_shown[i].flag, "-") != 0) {
            cpbm_log("script: %s had been painted into slot %d, but its slot is %d - "
                     "putting slot %d back", g_shown[i].tag, g_shown[i].slot, now_slot,
                     g_shown[i].slot);
            cpbm_d3d9_queue(g_shown[i].slot, NULL);
        }
    }
    cpbm_log("script: %d tag(s) changed slot; forgetting %d cached flag(s) so they are "
             "re-applied", moved, g_nshown);
    g_nshown = 0;
}

/* Loading a save makes EU4 build a brand-new atlas. The renderer repaints what it
 * knows about, but the cache here would still swallow the next pulse's lines, so
 * forget it and let the script state everything again. */
static int g_gen_seen;

static void check_generation(void)
{
    int gen = cpbm_d3d9_generation();
    if (gen <= g_gen_seen) return;
    if (g_gen_seen >= 1) {
        cpbm_log("script: atlas generation %d -> %d (a save was loaded); forgetting %d "
                 "cached flag(s) so the next pulse re-applies them", g_gen_seen, gen, g_nshown);
        g_nshown = 0;
    }
    g_gen_seen = gen;
}

/* One `dynamic_country_flag <TAG> <name>` instruction. */
static void handle(const char *tag, const char *flag)
{
    int slot;

    check_manifest();
    check_generation();

    slot = slot_of(tag);
    if (slot < 0) { cpbm_log("script: unknown tag '%s' - no atlas slot", tag); return; }
    if (already_showing(tag, flag, slot)) return;

    if (strcmp(flag, "-") == 0) {
        cpbm_log("script: %s -> restore its own flag (slot %d)", tag, slot);
        cpbm_d3d9_queue(slot, NULL);
    } else {
        cpbm_log("script: %s -> %s (slot %d)", tag, flag, slot);
        cpbm_d3d9_queue(slot, flag);
    }
}

static void scan_line(const char *line)
{
    const char *start = NULL, *p;
    size_t klen = 0, i;
    char tag[8], flag[32];
    int n;

    for (i = 0; i < sizeof(k_keywords) / sizeof(k_keywords[0]); i++) {
        const char *q = strstr(line, k_keywords[i].word);
        if (q && (!start || q < start)) { start = q; klen = k_keywords[i].len; }
    }
    if (!start) return;
    p = start + klen;
    while (*p == ' ' || *p == '\t' || *p == '=' || *p == '"') p++;

    n = 0; while (*p && *p != ' ' && *p != '\t' && n < 7) tag[n++] = *p++;
    tag[n] = 0;
    while (*p == ' ' || *p == '\t') p++;
    n = 0; while (*p && *p != ' ' && *p != '\t' && *p != '"' && *p != '\r' && *p != '\n'
                  && n < 31) flag[n++] = *p++;
    flag[n] = 0;

    if (tag[0] && flag[0]) handle(tag, flag);
}

/* ------------------------------------------------------------- the tail -- */

static DWORD WINAPI watcher(LPVOID unused)
{
    wchar_t path[MAX_PATH];
    LARGE_INTEGER pos;
    char line[1024];
    int  len = 0;
    (void)unused;

    if (!cpbm_docs_path(path, MAX_PATH, L"logs\\game.log")) return 0;
    pos.QuadPart = 0;

    cpbm_log("script: watching %S for dynamic_country_flag lines (cpbm_flag also accepted)", path);

    for (;;) {
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size;
            if (GetFileSizeEx(h, &size)) {
                /* The game truncates game.log at startup; if it shrank, start over. */
                if (size.QuadPart < pos.QuadPart) { pos.QuadPart = 0; len = 0; }

                if (size.QuadPart > pos.QuadPart) {
                    char buf[8192];
                    DWORD got = 0;
                    SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
                    while (ReadFile(h, buf, sizeof(buf), &got, NULL) && got) {
                        DWORD i;
                        for (i = 0; i < got; i++) {
                            if (buf[i] == '\n' || len >= (int)sizeof(line) - 1) {
                                line[len] = 0;
                                if (len) scan_line(line);
                                len = 0;
                            } else if (buf[i] != '\r') {
                                line[len++] = buf[i];
                            }
                        }
                        pos.QuadPart += got;
                    }
                }
            }
            CloseHandle(h);
        }
        Sleep(250);          /* a quarter second is far finer than any game pulse */
    }
}

void cpbm_script_start(void)
{
    HANDLE t;
    if (!cpbm_script_load_slots()) {
        cpbm_log("script: no slot map, so log instructions cannot be applied");
        return;
    }
    t = CreateThread(NULL, 0, watcher, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else cpbm_log("script: could not start the log watcher");
}
