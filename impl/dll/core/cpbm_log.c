/* cpbm_log.c - the log is the mod's only user interface, so it is written to
 * be readable rather than complete: one line per decision the mod makes, in the
 * order it makes them, saying what it saw and what it did about it.
 */

#include "cpbm_log.h"
#include "cpbm_env.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static HANDLE           g_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_lock;
static int              g_ready;

void cpbm_log_open(void)
{
    wchar_t path[MAX_PATH];

    if (g_ready) return;
    InitializeCriticalSection(&g_lock);
    g_ready = 1;

    if (!cpbm_game_path(path, MAX_PATH, L"cpbm-identity.log")) return;

    /* CREATE_ALWAYS: one launch, one log. Chasing a stale line from three
     * sessions ago is a waste of an evening. */
    g_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

void cpbm_log(const char *fmt, ...)
{
    char    line[2048];
    int     n;
    DWORD   wrote;
    va_list ap;

    if (!g_ready || g_file == INVALID_HANDLE_VALUE) return;

    va_start(ap, fmt);
    n = _vsnprintf_s(line, sizeof(line) - 3, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(line);

    line[n++] = '\r';
    line[n++] = '\n';

    EnterCriticalSection(&g_lock);
    WriteFile(g_file, line, (DWORD)n, &wrote, NULL);
    FlushFileBuffers(g_file);   /* a crash must not cost us the line that explains it */
    LeaveCriticalSection(&g_lock);
}

void cpbm_log_close(void)
{
    if (!g_ready) return;
    EnterCriticalSection(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) { CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE; }
    LeaveCriticalSection(&g_lock);
    DeleteCriticalSection(&g_lock);
    g_ready = 0;
}
