/* cpbm_d3d9.c - find the flag atlas as a live Direct3D texture.
 *
 * WHY THIS INSTEAD OF A FLAG-INDEX HOOK
 * The original plan was to intercept "which atlas slot does this country draw"
 * and substitute a different slot. Four systematic attempts failed to locate
 * that lookup (see RE-NOTES.md), because the draw path references no strings
 * and the slot is not stored beside the tag in memory.
 *
 * But we do not actually need it. Slot 121 belongs to FRA and to nothing else,
 * so rewriting the PIXELS of slot 121 changes France's flag and only France's.
 * That turns an open-ended reverse-engineering problem into a documented API
 * call - and one that should survive game patches, unlike a hardcoded offset.
 *
 * HOW WE GET THERE, WITHOUT ANY OFFSETS
 * eu4.exe imports exactly two functions from d3d9.dll, both by name:
 * Direct3DCreate9 and Direct3DCreate9Ex. So we patch those two pointers in the
 * import table - no code patching, no instruction-length decoding, nothing
 * pinned to a build. From the returned IDirect3D9 we hook CreateDevice, and
 * from the device we hook CreateTexture. Vtable slots are located with
 * offsetof() on the SDK's own Vtbl structs rather than hardcoded indices.
 *
 * WHAT IT DOES
 * Records the atlas pages as the game creates them, matching the page size read
 * off the cached flagfiles_N.tga rather than assuming one - page size follows the
 * graphics settings and differs between machines. Then writes 128x128 tiles.
 * DEFAULT cannot be locked, so writes go through UpdateSurface from a SYSTEMMEM
 * scratch surface - the supported route - and always on the render thread, via
 * EndScene, because the device is not thread-safe.
 *
 * On by default. cpbm.NOD3D9 next to the DLL turns it off.
 */

#include "cpbm_env.h"
#include "cpbm_log.h"

#define WIN32_LEAN_AND_MEAN
#define CINTERFACE                  /* C, not C++: gives us the Vtbl structs */
#define COBJMACROS
#include <windows.h>
#include <strsafe.h>
#include <d3d9.h>
#include <d3d11.h>
#include <stddef.h>

/* ------------------------------------------------------------ patching -- */

/* Overwrite one pointer-sized slot, restoring page protection afterwards. */
static int poke_ptr(void **slot, void *value, void **old)
{
    DWORD prot;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &prot)) return 0;
    if (old) *old = *slot;
    *slot = value;
    VirtualProtect(slot, sizeof(void *), prot, &prot);
    return 1;
}

/* Remember every slot we patch, so we can check later whether our pointer is
 * still there. The Steam overlay also IAT-hooks the game, and whoever writes
 * last wins - a hook that installs cleanly and never fires looks identical to
 * a game that never makes the call, so the two must be told apart. */
#define MAX_SLOTS 8
static struct {
    void **slot;        /* the IAT entry itself                      */
    void  *ours;        /* the function we want it to point at       */
    void **realp;       /* where we keep the pointer we call through */
    const char *name;
} g_slots[MAX_SLOTS];
static int g_nslots;

static void remember_slot(void **slot, void *ours, void **realp, const char *name)
{
    if (g_nslots < MAX_SLOTS) {
        g_slots[g_nslots].slot  = slot;
        g_slots[g_nslots].ours  = ours;
        g_slots[g_nslots].realp = realp;
        g_slots[g_nslots].name  = name;
        g_nslots++;
    }
}

/* Epic's overlay (EOSOVH-Win64-Shipping.dll) hooks the same imports we do and
 * installs after us: measured, all three of our pointers were replaced by
 * theirs. Re-applying ours ONCE puts us back in front, and because we adopt
 * whatever pointer is currently there as the one we call through, Epic's
 * overlay stays in the chain and keeps working. This is how two hooks are meant
 * to coexist - it is what EOS did to us.
 *
 * Deliberately one-shot. An earlier draft re-applied continuously, which is a
 * hook war: if EOS ever reinstalls, two pieces of software fight forever and
 * neither behaves predictably.
 *
 * Timing is the whole problem. Waiting for d3d11.dll was tried and is wrong:
 * measured, it is already present 30 ms in, long before Epic hooks anything,
 * so the reclaim fired with nothing to reclaim and Epic overwrote us after.
 *
 * The correct trigger is the overwrite itself. We watch our own slots, and the
 * moment one stops pointing at us we know Epic has installed. A short settle
 * pause lets it finish all three, then we reclaim once and stop watching. */
static DWORD WINAPI rehook_once(LPVOID unused)
{
    int i, taken = 0, waited, seen = 0;
    (void)unused;

    for (waited = 0; waited < 60000; waited++) {          /* up to 60s at 1ms */
        for (i = 0; i < g_nslots; i++) {
            void *now = *g_slots[i].slot;
            if (now && now != g_slots[i].ours) { seen = 1; break; }
        }
        if (seen) break;
        Sleep(1);
    }

    if (!seen) {
        cpbm_log("gfx: nobody took our hooks in 60s - leaving them as they are");
        return 0;
    }
    /* Let Epic finish installing the rest before we take them back, or we
     * would reclaim the first slot while it is still patching the third. */
    Sleep(100);
    cpbm_log("gfx: hooks were taken at ~%d ms - reclaiming once", waited);

    for (i = 0; i < g_nslots; i++) {
        void *now = *g_slots[i].slot;
        if (now == g_slots[i].ours || now == NULL) continue;

        *g_slots[i].realp = now;              /* chain through whoever is there */
        if (poke_ptr(g_slots[i].slot, g_slots[i].ours, NULL)) {
            taken++;
            cpbm_log("gfx: re-took %s, now chaining through %p", g_slots[i].name, now);
        }
    }
    cpbm_log("gfx: one-shot re-hook done, %d slot(s) reclaimed", taken);
    return 0;
}

/* Replace a named import of `dll` in the main module's import table. */
static int hook_iat(const char *dll, const char *func, void *repl, void **orig)
{
    HMODULE base = GetModuleHandleW(NULL);
    unsigned char *b = (unsigned char *)base;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)b;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    DWORD rva;

    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS *)(b + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return 0;

    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(b + rva); imp->Name; imp++) {
        const char *name = (const char *)(b + imp->Name);
        IMAGE_THUNK_DATA *oft, *ft;
        if (_stricmp(name, dll) != 0) continue;

        oft = (IMAGE_THUNK_DATA *)(b + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk
                                                                : imp->FirstThunk));
        ft  = (IMAGE_THUNK_DATA *)(b + imp->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            IMAGE_IMPORT_BY_NAME *ibn;
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(b + oft->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, func) != 0) continue;
            if (!poke_ptr((void **)&ft->u1.Function, repl, orig)) return 0;
            remember_slot((void **)&ft->u1.Function, repl, orig, func);
            return 1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------- hooks -- */

typedef IDirect3D9 *(WINAPI *pfn_create9)(UINT);
typedef HRESULT (WINAPI *pfn_create9ex)(UINT, IDirect3D9Ex **);

static pfn_create9   real_create9;
static pfn_create9ex real_create9ex;

static HRESULT (WINAPI *real_CreateDevice)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                           D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
/* EU4 creates MORE THAN ONE DEVICE, and only the first was ever hooked.
 *
 * On one machine those devices shared a vtable, so patching the first happened
 * to cover the second and everything worked. On another they did not, so the
 * hook sat on a device the game never rendered with: CreateTexture was reported
 * hooked and then never fired once, no atlas page was captured, and every write
 * was skipped while every step reported success.
 *
 * So: patch every device's vtable, and keep each vtable's ORIGINAL pointers
 * beside it. One shared `real_` pointer is not enough - and patching the same
 * vtable twice would make our own function its own original, which is an
 * infinite recursion, not a bug that announces itself politely. */
typedef HRESULT (WINAPI *pfn_CreateTexture)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD,
                                            D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);
typedef HRESULT (WINAPI *pfn_EndScene)(IDirect3DDevice9 *);

#define MAX_VT 8
static struct {
    void            **vt;
    pfn_CreateTexture real_CreateTexture;
    pfn_EndScene      real_EndScene;
    void            **ct_slot;   /* the vtable entries themselves, so a  */
    void            **es_slot;   /* watchdog can see if someone re-took  */
    pfn_EndScene      first_EndScene; /* genuine d3d9 one, before anybody  */
    int               dead;      /* memory recycled - never write here   */
} g_vt[MAX_VT];
static int g_nvt;
static volatile LONG g_endscene_calls;

static int vt_index(void **vt)
{
    int i;
    for (i = 0; i < g_nvt; i++) if (g_vt[i].vt == vt) return i;
    return -1;
}

static IDirect3DDevice9 *g_device;
static int g_atlas_seen;
static int g_textures_seen;

static const char *pool_name(D3DPOOL p)
{
    switch (p) {
    case D3DPOOL_DEFAULT:   return "DEFAULT (not lockable; needs UpdateSurface)";
    case D3DPOOL_MANAGED:   return "MANAGED (lockable - ideal)";
    case D3DPOOL_SYSTEMMEM: return "SYSTEMMEM (lockable)";
    case D3DPOOL_SCRATCH:   return "SCRATCH";
    default:                return "unknown";
    }
}

/* Atlas geometry is MEASURED, not assumed.
 *
 * It was hardcoded to 2048x2048 / 15 pages / 256 tiles, taken from one machine.
 * Page size follows the graphics settings, so it is read from the atlas cache
 * the game actually built.
 *
 * PAGES COME IN THREE SHAPES, depending on which D3D9 path the machine takes:
 *
 *   A  D3DPOOL_MANAGED, one texture per page (plain D3D9)
 *      -> LockRect it; the runtime uploads
 *   B  D3DPOOL_DEFAULT plus a D3DPOOL_SYSTEMMEM twin, created back to back
 *      (D3D9Ex, which forbids MANAGED, so the engine emulates it by hand)
 *      -> lock the twin, UpdateSurface into the DEFAULT one
 *   C  D3DPOOL_DEFAULT alone
 *      -> own scratch surface, UpdateSurface
 *
 * The old filter ignored pool entirely, so layout B produced THIRTY entries for
 * a fifteen-page atlas - entry 2k the real page, 2k+1 its staging twin - and
 * slot/tiles_per_page then addressed the wrong half of the table. That is why
 * a second test machine wrote pixels nobody ever sampled.
 *
 * In A and B the surface written IS the copy the runtime or the engine restores
 * from, so both survive a device reset for free. C does not. */
#define MAX_ATLAS_PAGES 64
#define TILE_PX 128

typedef struct {
    IDirect3DTexture9 *vram;      /* MANAGED or DEFAULT - what the GPU samples */
    IDirect3DTexture9 *staging;   /* SYSTEMMEM twin, layout B only, else NULL  */
    D3DPOOL            pool;      /* pool of vram; decides the write path      */
} atlas_page;

static atlas_page        g_pages[MAX_ATLAS_PAGES];
static int               g_npages;

/* The device that OWNS the atlas - not necessarily the first one created.
 * UpdateSurface demands the owning device, and a queue drained against any
 * other does precisely nothing, silently. */
static IDirect3DDevice9 *g_atlas_device;

static int g_page_w, g_page_h;      /* pixels, from flagfiles_0.tga        */
static int g_tiles_x, g_per_page;   /* derived                             */
static int g_expected_pages;        /* how many flagfiles_N.tga exist      */

/* The atlas format is remembered rather than demanded. X8R8G8B8 was what one
 * machine used; A8R8G8B8 is equally plausible elsewhere and has the same BGRA
 * byte layout, so the same tile data works for both. UpdateSurface requires the
 * scratch surface to match the destination, so every surface we make uses this
 * rather than a constant. */
static D3DFORMAT g_page_fmt = D3DFMT_X8R8G8B8;

/* ------------------------------------------------------ atlas rebuilds -- *
 * EU4 throws the whole atlas away and builds a fresh one when a save is loaded:
 * a new set of atlas-shaped textures, same device, no reset. Holding on to the
 * old pages meant every later write went into textures nobody draws any more,
 * so loading a save reverted every flag for good.
 *
 * So the atlas is versioned. Atlas-shaped textures arriving after the atlas is
 * complete are collected as a PENDING set; when that set is complete it is
 * adopted on the render thread, the old pages are released, and every override
 * currently shown is painted again a moment later. A pending set that stalls is
 * discarded - textures that merely share the size must never replace the atlas.
 *
 * CreateTexture can run off the render thread (EU4's devices are MULTITHREADED),
 * so the pending set is guarded by its own lock. */
static int               g_generation;          /* 0 until complete, +1 per rebuild  */
static atlas_page        g_pending[MAX_ATLAS_PAGES];
static int               g_npending;
static IDirect3DDevice9 *g_pending_device;
static ULONGLONG         g_pending_last_ms;     /* when the newest pending page came */
static volatile LONG     g_adopt_ready;
static CRITICAL_SECTION  g_pend_lock;
static int               g_pend_ready;
static int               g_repaint_in;          /* atlas-device frames to repaint; 0 idle */
#define PENDING_STALL_MS 10000
#define REPAINT_FRAMES   60

/* What each overridden slot is showing, so a rebuilt atlas can be repainted.
 * Only touched on the render thread. */
#define MAX_ACTIVE 8192
typedef struct { int slot; wchar_t name[32]; } active_flag;
static active_flag g_active[MAX_ACTIVE];
static int         g_nactive;

static void set_active(int slot, const wchar_t *name)
{
    int i;
    for (i = 0; i < g_nactive; i++)
        if (g_active[i].slot == slot) break;
    if (i == g_nactive) {
        if (g_nactive >= MAX_ACTIVE) return;
        g_nactive++;
    }
    g_active[i].slot = slot;
    StringCchCopyW(g_active[i].name, 32, name);
}

static void clear_active(int slot)
{
    int i;
    for (i = 0; i < g_nactive; i++)
        if (g_active[i].slot == slot) { g_active[i] = g_active[--g_nactive]; return; }
}

int cpbm_d3d9_generation(void) { return g_generation; }

/* Complete = every page present and, on machines that pair each page with a
 * SYSTEMMEM twin (layout B), the last page's twin too. Caller holds the lock. */
static void pending_check_complete(void)
{
    if (g_npending < g_expected_pages) return;
    if (g_npages > 0 && g_pages[0].staging && !g_pending[g_npending - 1].staging) return;
    InterlockedExchange(&g_adopt_ready, 1);
}

static void pending_release_all(void)
{
    int i;
    for (i = 0; i < g_npending; i++) {
        if (g_pending[i].vram)    IDirect3DTexture9_Release(g_pending[i].vram);
        if (g_pending[i].staging) IDirect3DTexture9_Release(g_pending[i].staging);
        g_pending[i].vram = g_pending[i].staging = NULL;
    }
    g_npending = 0;
}

/* How many flags the atlas manifest lists. Its first token is the set hash, so
 * the answer is one less than the token count. 0 if it cannot be read. */
static int manifest_flag_count(void)
{
    wchar_t path[MAX_PATH];
    HANDLE  h;
    DWORD   size, got = 0;
    char   *buf, *p;
    int     tokens = 0;

    if (!cpbm_docs_path(path, MAX_PATH, L"gfx\\flags\\flagfiles.txt")) return 0;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > (4u << 20)) { CloseHandle(h); return 0; }
    buf = (char *)HeapAlloc(GetProcessHeap(), 0, size + 1);
    if (!buf) { CloseHandle(h); return 0; }
    if (!ReadFile(h, buf, size, &got, NULL)) got = 0;
    buf[got] = 0;
    CloseHandle(h);

    /* Counted by hand rather than with strtok, which is not safe to call from
     * two threads and would also chew up the buffer the script side parses. */
    for (p = buf; *p; ) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        tokens++;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    }
    HeapFree(GetProcessHeap(), 0, buf);
    return tokens > 1 ? tokens - 1 : 0;
}

/* Read the atlas dimensions and page count off disk. */
static void probe_atlas_geometry(void)
{
    wchar_t path[MAX_PATH];
    HANDLE  h;
    unsigned char hd[18];
    DWORD   got = 0;
    int     n;

    if (!cpbm_docs_path(path, MAX_PATH, L"gfx\\flags\\flagfiles_0.tga")) return;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        cpbm_log("d3d9: no atlas cache at %S - cannot learn the page size", path);
        return;
    }
    if (ReadFile(h, hd, 18, &got, NULL) && got == 18) {
        g_page_w = hd[12] | (hd[13] << 8);
        g_page_h = hd[14] | (hd[15] << 8);
    }
    CloseHandle(h);

    if (g_page_w < TILE_PX || g_page_h < TILE_PX) { g_page_w = g_page_h = 0; return; }

    g_tiles_x  = g_page_w / TILE_PX;
    g_per_page = g_tiles_x * (g_page_h / TILE_PX);

    for (n = 0; n < MAX_ATLAS_PAGES; n++) {
        wchar_t leaf[64];
        if (FAILED(StringCchPrintfW(leaf, 64, L"gfx\\flags\\flagfiles_%d.tga", n))) break;
        if (!cpbm_docs_path(path, MAX_PATH, leaf)) break;
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) break;
    }
    g_expected_pages = n;

    /* The page files on disk are a cache, and EU4 leaves surplus ones behind
     * when a mod set shrinks: nine flagfiles_N.tga for an atlas that now needs
     * six. Waiting for pages the game never creates left every write queued,
     * with the log repeating "BLOCKED" forever and no flag ever painted. How
     * many flags exist is the authoritative number, so derive the page count
     * from the manifest and treat the file count as a fallback only. */
    {
        int flags = manifest_flag_count();
        if (flags > 0 && g_per_page > 0) {
            int need = (flags + g_per_page - 1) / g_per_page;
            if (need > MAX_ATLAS_PAGES) need = MAX_ATLAS_PAGES;
            if (need != g_expected_pages)
                cpbm_log("d3d9: %d flagfiles_N.tga on disk, but the manifest lists %d flag(s) "
                         "= %d page(s). Trusting the manifest; the surplus files are a stale "
                         "cache from a larger mod set.", g_expected_pages, flags, need);
            g_expected_pages = need;
        }
    }

    cpbm_log("d3d9: atlas is %dx%d, %d tiles per page, %d page(s)",
             g_page_w, g_page_h, g_per_page, g_expected_pages);
}

static IDirect3DDevice9 *g_seen_present;
static int g_announced_owner;
static volatile LONG g_swap_pending;   /* set from elsewhere, done in EndScene */

static HRESULT WINAPI my_CreateTexture(IDirect3DDevice9 *dev, UINT w, UINT h, UINT levels,
                                       DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                       IDirect3DTexture9 **out, HANDLE *shared)
{
    int vi = vt_index(*(void ***)dev);
    HRESULT hr;

    if (vi < 0) return E_FAIL;            /* cannot happen: we only get called
                                             through a vtable we patched */
    hr = g_vt[vi].real_CreateTexture(dev, w, h, levels, usage, fmt, pool, out, shared);

    /* Match the atlas we actually measured. Log any large texture either way,
     * so a mismatch is visible rather than silent - that silence is exactly
     * what made this fail on a second machine with no clue why. */
    /* Count everything, so "our hook never fired" and "it fired but nothing
     * matched" are distinguishable. On the machine where this failed, the
     * distinction was the whole answer and the log could not tell them apart. */
    g_textures_seen++;
    if (g_textures_seen <= 3 || (w >= 1024 && h >= 1024))
        cpbm_log("d3d9: CreateTexture #%d: %ux%u levels=%u fmt=%d pool=%d",
                 g_textures_seen, w, h, levels, (int)fmt, (int)pool);

    /* Not atlas-shaped: nothing to do. */
    if (FAILED(hr) || !g_page_w || (int)w != g_page_w || (int)h != g_page_h ||
        levels != 1 || !out || !*out)
        return hr;
    if (fmt != D3DFMT_X8R8G8B8 && fmt != D3DFMT_A8R8G8B8) return hr;

    g_atlas_seen++;
    g_page_fmt = fmt;

    /* AddRef everything we keep, or the pointer dangles the moment the game
     * drops the texture - which crashed the game once already, an access
     * violation in GetSurfaceLevel on a freed texture. */
    if (pool == D3DPOOL_MANAGED || pool == D3DPOOL_DEFAULT) {
        if (g_expected_pages && g_npages >= g_expected_pages && g_pend_ready) {
            /* The atlas is already complete, so this is part of a rebuild (or a
             * stray). Collect it; EndScene decides. */
            EnterCriticalSection(&g_pend_lock);
            if (g_adopt_ready || g_npending >= g_expected_pages || g_npending >= MAX_ATLAS_PAGES) {
                cpbm_log("d3d9: atlas-shaped texture %p beyond a full pending set - IGNORED",
                         (void *)*out);
            } else {
                if (g_npending == 0) g_pending_device = dev;
                IDirect3DTexture9_AddRef(*out);
                g_pending[g_npending].vram    = *out;
                g_pending[g_npending].staging = NULL;
                g_pending[g_npending].pool    = pool;
                cpbm_log("d3d9: atlas (pending) page %d  vram=%p pool=%s (dev %p)",
                         g_npending, (void *)*out, pool_name(pool), (void *)dev);
                g_npending++;
                g_pending_last_ms = GetTickCount64();
                pending_check_complete();
            }
            LeaveCriticalSection(&g_pend_lock);
            return hr;
        }
        if (g_npages >= g_expected_pages || g_npages >= MAX_ATLAS_PAGES) {
            cpbm_log("d3d9: atlas-shaped texture %p past page %d - IGNORED",
                     (void *)*out, g_expected_pages);
            return hr;
        }
        IDirect3DTexture9_AddRef(*out);
        g_pages[g_npages].vram    = *out;
        g_pages[g_npages].staging = NULL;
        g_pages[g_npages].pool    = pool;

        if (!g_atlas_device) {
            g_atlas_device = dev;
            IDirect3DDevice9_AddRef(dev);
        }
        cpbm_log("d3d9: atlas page %d  vram=%p pool=%s (dev %p)",
                 g_npages, (void *)*out, pool_name(pool), (void *)dev);
        g_npages++;
        if (g_npages == g_expected_pages) {
            g_generation = 1;
            cpbm_log("d3d9: ATLAS COMPLETE - %d page(s), generation 1", g_npages);
        }
        return hr;
    }

    if (pool == D3DPOOL_SYSTEMMEM) {
        /* The twin follows its partner immediately. It is the master copy the
         * engine re-uploads from after a device reset, so writing it is what
         * makes a change survive alt-tab. It never gets a page of its own. */
        int k;
        if (g_pend_ready) {
            int handled = 0;
            EnterCriticalSection(&g_pend_lock);
            if (g_npending > 0 && !g_adopt_ready) {
                int pk = g_npending - 1;
                if (g_pending[pk].vram && !g_pending[pk].staging &&
                    g_pending[pk].pool == D3DPOOL_DEFAULT) {
                    IDirect3DTexture9_AddRef(*out);
                    g_pending[pk].staging = *out;
                    g_pending_last_ms = GetTickCount64();
                    cpbm_log("d3d9: atlas (pending) page %d  staging=%p", pk, (void *)*out);
                    pending_check_complete();
                }
                handled = 1;
            }
            LeaveCriticalSection(&g_pend_lock);
            if (handled) return hr;
        }
        k = g_npages - 1;
        if (k >= 0 && g_pages[k].vram && !g_pages[k].staging &&
            g_pages[k].pool == D3DPOOL_DEFAULT) {
            IDirect3DTexture9_AddRef(*out);
            g_pages[k].staging = *out;
            cpbm_log("d3d9: atlas page %d  staging=%p", k, (void *)*out);
        }
        return hr;
    }
    return hr;
}

/* Write one 128x128 tile into an atlas page.
 *
 * The pages are D3DPOOL_DEFAULT, which cannot be locked. UpdateSurface is
 * exactly the supported route for this: source in SYSTEMMEM, destination in
 * DEFAULT, same format, no mips. We build a scratch surface, fill it, and blit
 * it into place.
 *
 * Must run on the render thread - the device is not thread-safe unless it was
 * created D3DCREATE_MULTITHREADED, and we do not control that. Hence EndScene. */
static int g_write_disabled;    /* latched after any fault: never crash twice */

/* `dev` is passed in rather than taken from g_device. The game creates a device
 * through CreateDevice and ALSO calls Direct3DCreate9Ex, so the device we
 * captured is not necessarily the one that owns the atlas textures - and a
 * cross-device UpdateSurface returns D3DERR_INVALIDCALL, which is exactly what
 * we got (0x8876086C). The device handed to EndScene is by definition the live
 * rendering device, so use that. */
static void copy_tile(D3DLOCKED_RECT *lr, const unsigned char *bgra)
{
    int y;
    for (y = 0; y < TILE_PX; y++)
        memcpy((unsigned char *)lr->pBits + (size_t)y * lr->Pitch,
               bgra + (size_t)y * TILE_PX * 4, TILE_PX * 4);
}

static int write_tile_inner(IDirect3DDevice9 *dev, int slot, const unsigned char *bgra)
{
    int page, cell, row, col, tx, ty;
    atlas_page *pg;
    IDirect3DSurface9 *scratch = NULL, *dest = NULL, *srcsurf = NULL;
    D3DLOCKED_RECT lr;
    HRESULT hr;
    POINT at;
    RECT r;
    int ok = 0;

    if (!g_per_page || !g_tiles_x) {
        cpbm_log("d3d9: slot %d - atlas geometry not measured, SKIPPED", slot);
        return 0;
    }
    page = slot / g_per_page; cell = slot % g_per_page;
    row  = cell / g_tiles_x;  col  = cell % g_tiles_x;
    tx   = col * TILE_PX;     ty   = row * TILE_PX;

    if (page < 0 || page >= g_npages || !g_pages[page].vram) {
        cpbm_log("d3d9: slot %d -> page %d, but only %d page(s) captured - SKIPPED",
                 slot, page, g_npages);
        return 0;
    }
    pg = &g_pages[page];

    r.left = tx; r.top = ty; r.right = tx + TILE_PX; r.bottom = ty + TILE_PX;
    at.x = tx;   at.y = ty;

    /* ---- layout A: MANAGED, lock it directly ------------------------- */
    if (pg->pool == D3DPOOL_MANAGED) {
        hr = IDirect3DTexture9_LockRect(pg->vram, 0, &lr, &r, 0);
        if (FAILED(hr)) {
            cpbm_log("d3d9: LockRect(managed) 0x%08lX", (unsigned long)hr);
            return 0;
        }
        copy_tile(&lr, bgra);
        IDirect3DTexture9_UnlockRect(pg->vram, 0);
        cpbm_log("d3d9: slot %d written via MANAGED lock", slot);
        return 1;
    }

    /* ---- layout B: DEFAULT with a SYSTEMMEM twin ---------------------- */
    /* Preferred: the twin is the copy the engine restores from, so this also
     * survives a device reset. */
    if (pg->staging) {
        hr = IDirect3DTexture9_LockRect(pg->staging, 0, &lr, &r, 0);
        if (FAILED(hr)) {
            cpbm_log("d3d9: LockRect(staging) 0x%08lX", (unsigned long)hr);
            return 0;
        }
        copy_tile(&lr, bgra);
        IDirect3DTexture9_UnlockRect(pg->staging, 0);

        if (SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(pg->staging, 0, &srcsurf)) &&
            SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(pg->vram, 0, &dest))) {
            hr = IDirect3DDevice9_UpdateSurface(dev, srcsurf, &r, dest, &at);
            if (SUCCEEDED(hr)) { ok = 1; cpbm_log("d3d9: slot %d written via SYSTEMMEM twin", slot); }
            else cpbm_log("d3d9: UpdateSurface(twin) 0x%08lX", (unsigned long)hr);
        }
        if (dest)    IDirect3DSurface9_Release(dest);
        if (srcsurf) IDirect3DSurface9_Release(srcsurf);
        return ok;
    }

    /* ---- layout C: DEFAULT alone, own scratch surface ----------------- */
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, TILE_PX, TILE_PX,
             g_page_fmt, D3DPOOL_SYSTEMMEM, &scratch, NULL);
    if (FAILED(hr)) {
        cpbm_log("d3d9: scratch surface failed 0x%08lX", (unsigned long)hr);
        return 0;
    }
    if (SUCCEEDED(IDirect3DSurface9_LockRect(scratch, &lr, NULL, 0))) {
        RECT whole; whole.left = 0; whole.top = 0;
        whole.right = TILE_PX; whole.bottom = TILE_PX;
        copy_tile(&lr, bgra);
        IDirect3DSurface9_UnlockRect(scratch);

        if (SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(pg->vram, 0, &dest))) {
            hr = IDirect3DDevice9_UpdateSurface(dev, scratch, &whole, dest, &at);
            if (SUCCEEDED(hr)) {
                ok = 1;
                cpbm_log("d3d9: slot %d written via scratch surface "
                         "(does NOT survive a device reset)", slot);
            } else {
                cpbm_log("d3d9: UpdateSurface(scratch) 0x%08lX", (unsigned long)hr);
            }
            IDirect3DSurface9_Release(dest);
        }
    }
    IDirect3DSurface9_Release(scratch);
    return ok;
}

/* A fault in here must never take the game down. Any access violation is
 * caught, reported, and latched off - a mod that draws the wrong flag is a
 * nuisance, a mod that crashes someone's campaign is not shippable. */
static int write_tile(IDirect3DDevice9 *dev, int slot, const unsigned char *bgra)
{
    int ok = 0;
    if (g_write_disabled) return 0;
    __try {
        ok = write_tile_inner(dev, slot, bgra);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_write_disabled = 1;
        cpbm_log("d3d9: FAULT during tile write (code 0x%08lX) - texture writes "
                 "disabled for this session. The game continues.",
                 (unsigned long)GetExceptionCode());
        ok = 0;
    }
    return ok;
}

/* Locate <name>.tga in any mod's gfx\flags folder, falling back to the game's
 * own. Returns 1 and fills `out` with the full path.
 *
 * Deliberately not tied to a particular mod: the art can sit in a companion
 * mod, or be folded into CPBM, and either works. The alternative - a fixed
 * folder name - fails silently and invisibly the day someone reorganises. */
/* Read the `path=` line out of a .mod file. That is where the mod's files
 * really are, and for a Steam Workshop subscription it points right out of the
 * Documents tree into steamapps\workshop\content\236850\<id>. Scanning folders
 * under Documents\mod would miss every Workshop mod, CPBM included. */
static int mod_path_from(const wchar_t *modfile, wchar_t *out, DWORD cap)
{
    HANDLE h;
    DWORD  size, got = 0;
    char  *buf, *p, *q;
    int    ok = 0;

    h = CreateFileW(modfile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > (1u << 16)) { CloseHandle(h); return 0; }

    buf = (char *)HeapAlloc(GetProcessHeap(), 0, size + 1);
    if (!buf) { CloseHandle(h); return 0; }
    if (ReadFile(h, buf, size, &got, NULL)) {
        buf[got] = 0;
        /* The key must be `path` at the start of a line. A plain substring search
         * matched `replace_path="customizable_localization"` in Extended
         * Timeline's .mod and looked for art there: any mod with a replace_path
         * line above its path= line silently lost every flag. */
        p = buf;
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
            (unsigned char)p[2] == 0xBF) p += 3;
        while (p && *p) {
            char *line = p, *nl = strchr(p, '\n');
            while (*line == ' ' || *line == '\t') line++;
            if (strncmp(line, "path", 4) == 0) {
                q = line + 4;
                while (*q == ' ' || *q == '\t') q++;
                if (*q == '=') {
                    q++;
                    while (*q == ' ' || *q == '\t' || *q == '"') q++;
                    line = q;
                    while (*q && *q != '"' && *q != '\r' && *q != '\n') q++;
                    *q = 0;
                    if (*line) ok = MultiByteToWideChar(CP_UTF8, 0, line, -1, out, (int)cap) > 0;
                    break;
                }
            }
            p = nl ? nl + 1 : NULL;
        }
    }
    HeapFree(GetProcessHeap(), 0, buf);
    CloseHandle(h);
    return ok;
}

/* Mods ticked in the launcher, in load order, read once from dlc_load.json.
 * Art is searched only in these - a disabled mod's file must never be painted -
 * and the LAST-loaded first, because a later mod in the playset overrides an
 * earlier one. If the file cannot be read at all, every .mod is searched. */
#define MAX_ENABLED 1024
static wchar_t *g_enabled[MAX_ENABLED];
static int      g_nenabled = -1;       /* -1 = not read yet */
static int      g_enabled_known;       /* dlc_load.json read and had enabled_mods */

static void load_enabled_mods(void)
{
    wchar_t path[MAX_PATH];
    HANDLE  h;
    DWORD   size, got = 0;
    char   *buf, *p;

    g_nenabled = 0;
    if (!cpbm_docs_path(path, MAX_PATH, L"dlc_load.json")) return;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        cpbm_log("d3d9: dlc_load.json not readable - searching every mod for flag art");
        return;
    }
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > (1u << 20)) { CloseHandle(h); return; }
    buf = (char *)HeapAlloc(GetProcessHeap(), 0, size + 1);
    if (!buf) { CloseHandle(h); return; }
    if (!ReadFile(h, buf, size, &got, NULL)) got = 0;
    buf[got] = 0;
    CloseHandle(h);

    p = strstr(buf, "\"enabled_mods\"");
    if (p) p = strchr(p, '[');
    if (p) {
        g_enabled_known = 1;
        p++;
        while (*p && g_nenabled < MAX_ENABLED) {
            char    entry[MAX_PATH];
            wchar_t w[MAX_PATH];
            wchar_t *copy;
            int     n = 0;

            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
            if (*p != '"') break;                      /* ']' ends the list */
            p++;
            while (*p && *p != '"' && n < MAX_PATH - 1) {
                if (*p == '\\' && p[1]) p++;           /* JSON escape: keep the next char */
                entry[n++] = *p++;
            }
            entry[n] = 0;
            if (*p == '"') p++;
            if (!n || !MultiByteToWideChar(CP_UTF8, 0, entry, -1, w, MAX_PATH)) continue;

            copy = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, sizeof(wchar_t) * ((size_t)lstrlenW(w) + 1));
            if (!copy) break;
            StringCchCopyW(copy, (size_t)lstrlenW(w) + 1, w);
            g_enabled[g_nenabled++] = copy;
        }
        cpbm_log("d3d9: flag art is searched in the %d enabled mod(s) listed in dlc_load.json, "
                 "last-loaded first", g_nenabled);
    } else {
        cpbm_log("d3d9: dlc_load.json has no enabled_mods list - searching every mod for flag art");
    }
    HeapFree(GetProcessHeap(), 0, buf);
}

/* <name>.tga inside the mod a .mod file points at. path= may be absolute
 * (Workshop, and CPBM here) or relative to the Documents folder. */
static int flag_in_mod(const wchar_t *modfile, const wchar_t *name, wchar_t *out, DWORD cap)
{
    wchar_t root[MAX_PATH], rel[MAX_PATH];

    if (!mod_path_from(modfile, root, MAX_PATH)) return 0;
    if (SUCCEEDED(StringCchPrintfW(out, cap, L"%s\\gfx\\flags\\%s.tga", root, name)) &&
        GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return 1;
    if (SUCCEEDED(StringCchPrintfW(rel, MAX_PATH, L"%s\\gfx\\flags\\%s.tga", root, name)) &&
        cpbm_docs_path(out, cap, rel) &&
        GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return 1;
    return 0;
}

static int find_flag_file(const wchar_t *name, wchar_t *out, DWORD cap)
{
    wchar_t moddir[MAX_PATH], glob[MAX_PATH], modfile[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int i;

    if (g_nenabled < 0) load_enabled_mods();

    /* 1. mods, resolved through their own .mod files - which covers local mods
     *    and Workshop subscriptions alike. */
    if (g_enabled_known) {
        for (i = g_nenabled - 1; i >= 0; i--) {
            const wchar_t *e = g_enabled[i];
            int ok = (e[0] && e[1] == L':')
                   ? SUCCEEDED(StringCchCopyW(modfile, MAX_PATH, e))
                   : cpbm_docs_path(modfile, MAX_PATH, e);
            if (ok && flag_in_mod(modfile, name, out, cap)) return 1;
        }
    } else if (cpbm_docs_path(moddir, MAX_PATH, L"mod\\") &&
               SUCCEEDED(StringCchPrintfW(glob, MAX_PATH, L"%s*.mod", moddir))) {
        h = FindFirstFileW(glob, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (SUCCEEDED(StringCchPrintfW(modfile, MAX_PATH, L"%s%s", moddir, fd.cFileName)) &&
                    flag_in_mod(modfile, name, out, cap)) { FindClose(h); return 1; }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    /* 2. the game's own gfx\flags, next to the DLL */
    if (SUCCEEDED(StringCchPrintfW(glob, MAX_PATH, L"gfx\\flags\\%s.tga", name)) &&
        cpbm_game_path(out, cap, glob) &&
        GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
        return 1;

    return 0;
}

/* Load a flag .tga from the mod folder into 128x128 BGRA, ready for the atlas.
 * Handles type 2 (uncompressed) and type 10 (RLE), 24 or 32 bpp, either origin
 * - CPBM's art is RLE 24-bit, vanilla's is uncompressed, so both are needed.
 * Returns 1 on success, filling `out` (128*128*4 bytes). */
static int load_flag_tga(const wchar_t *name, unsigned char *out)
{
    wchar_t path[MAX_PATH], leaf[MAX_PATH];
    HANDLE  fh;
    DWORD   size, got = 0;
    unsigned char *raw, *px;
    int w, h, bpp, type, bytes, topleft, i, n, ok = 0;

    /* Search every mod folder, not one fixed name. The art may live in this
     * mod, or folded into CPBM itself, or anywhere else the user keeps it -
     * hardcoding a folder would silently stop finding it the moment they
     * reorganise, with no error to explain why. */
    if (!find_flag_file(name, path, MAX_PATH)) {
        cpbm_log("d3d9: no %S.tga in any enabled mod's gfx\\flags, nor in the game folder", name);
        return 0;
    }
    (void)leaf;

    fh = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                     FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) { cpbm_log("d3d9: cannot open %S", path); return 0; }
    size = GetFileSize(fh, NULL);
    if (size == INVALID_FILE_SIZE || size < 18 || size > (8u << 20)) { CloseHandle(fh); return 0; }

    raw = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, size);
    if (!raw) { CloseHandle(fh); return 0; }
    if (!ReadFile(fh, raw, size, &got, NULL) || got < 18) {
        HeapFree(GetProcessHeap(), 0, raw); CloseHandle(fh); return 0;
    }
    CloseHandle(fh);

    type    = raw[2];
    w       = raw[12] | (raw[13] << 8);
    h       = raw[14] | (raw[15] << 8);
    bpp     = raw[16];
    topleft = (raw[17] & 0x20) != 0;
    bytes   = bpp / 8;

    if (w != 128 || h != 128 || (type != 2 && type != 10) || (bpp != 24 && bpp != 32)) {
        cpbm_log("d3d9: %S is %dx%d type %d %dbpp - need 128x128, type 2 or 10, 24/32bpp",
                 name, w, h, type, bpp);
        HeapFree(GetProcessHeap(), 0, raw);
        return 0;
    }

    px = raw + 18 + raw[0];
    n  = w * h;

    if (type == 2) {
        for (i = 0; i < n && (DWORD)(px - raw) + (DWORD)bytes <= got; i++, px += bytes) {
            out[i*4+0] = px[0]; out[i*4+1] = px[1]; out[i*4+2] = px[2]; out[i*4+3] = 0xFF;
        }
        ok = (i == n);
    } else {
        i = 0;
        while (i < n && (DWORD)(px - raw) < got) {
            int c = *px++, count = (c & 0x7F) + 1, k;
            if (c & 0x80) {
                if ((DWORD)(px - raw) + (DWORD)bytes > got) break;
                for (k = 0; k < count && i < n; k++, i++) {
                    out[i*4+0] = px[0]; out[i*4+1] = px[1]; out[i*4+2] = px[2]; out[i*4+3] = 0xFF;
                }
                px += bytes;
            } else {
                for (k = 0; k < count && i < n; k++, i++, px += bytes) {
                    if ((DWORD)(px - raw) + (DWORD)bytes > got) { i = n; break; }
                    out[i*4+0] = px[0]; out[i*4+1] = px[1]; out[i*4+2] = px[2]; out[i*4+3] = 0xFF;
                }
            }
        }
        ok = (i == n);
    }

    if (ok && !topleft) {                    /* TGA defaults to bottom-up */
        unsigned char tmp[128 * 4];
        int y;
        for (y = 0; y < 64; y++) {
            memcpy(tmp, out + (size_t)y * 128 * 4, 128 * 4);
            memcpy(out + (size_t)y * 128 * 4, out + (size_t)(127 - y) * 128 * 4, 128 * 4);
            memcpy(out + (size_t)(127 - y) * 128 * 4, tmp, 128 * 4);
        }
    }
    HeapFree(GetProcessHeap(), 0, raw);
    if (!ok) cpbm_log("d3d9: %S decoded short", name);
    return ok;
}

/* ------------------------------------------------------- originals ----- *
 * A country with no matching rule must show the flag it already had, and rules
 * stop matching as often as they start. So before overwriting a slot for the
 * first time we take a copy of what was there.
 *
 * Reading back a D3DPOOL_DEFAULT texture takes a detour: it cannot be locked,
 * and GetRenderTargetData only works on render targets. So we StretchRect the
 * tile into a scratch render target, pull that into SYSTEMMEM, and lock it.
 *
 * Capturing rather than reloading TAG.tga from disk is deliberate: which file
 * wins across vanilla + CPBM + Extended Timeline is EU4's business, not ours,
 * and guessing it wrong would restore the wrong flag. What is in the atlas is
 * by definition correct. */
/* One entry per slot this session has changed, allocated as needed. A fixed
 * table of 64 ran out long before every country with a rule had changed, and
 * past it a country could be painted but never given its own flag back. */
typedef struct {
    int            slot;
    int            valid;
    unsigned char *px;           /* 128*128*4 bytes, heap */
} orig_tile;
static orig_tile *g_orig;
static int        g_norig, g_orig_cap;

static int read_tile(IDirect3DDevice9 *dev, int slot, unsigned char *out)
{
    int page, cell, row, col;

    if (!g_per_page || !g_tiles_x) return 0;   /* geometry not measured yet */
    page = slot / g_per_page; cell = slot % g_per_page;
    row  = cell / g_tiles_x; col  = cell % g_tiles_x;
    IDirect3DSurface9 *atlas = NULL, *rt = NULL, *sys = NULL;
    D3DLOCKED_RECT lr;
    RECT src;
    int y, ok = 0;

    if (!dev || page < 0 || page >= g_npages || !g_pages[page].vram) return 0;

    /* Layout B: the engine's own SYSTEMMEM twin holds the same pixels and IS
     * lockable, so read the original straight out of it.
     *
     * The render-target detour below exists only for layout C, where no twin
     * exists. It is a full GPU round trip - CreateRenderTarget, StretchRect,
     * GetRenderTargetData, LockRect - and on one test machine the render thread went
     * into it and never came back: the queue drained, but "captured original of
     * slot N" was never printed and EndScene stopped heartbeating. Never take
     * that route when a twin is available. */
    if (g_pages[page].staging) {
        src.left = col * 128; src.top = row * 128;
        src.right = src.left + 128; src.bottom = src.top + 128;
        if (SUCCEEDED(IDirect3DTexture9_LockRect(g_pages[page].staging, 0, &lr,
                                                 &src, D3DLOCK_READONLY))) {
            for (y = 0; y < 128; y++)
                memcpy(out + (size_t)y * 128 * 4,
                       (unsigned char *)lr.pBits + (size_t)y * lr.Pitch, 128 * 4);
            IDirect3DTexture9_UnlockRect(g_pages[page].staging, 0);
            return 1;
        }
        cpbm_log("d3d9: slot %d - LockRect on the staging twin failed; "
                 "falling back to the render-target readback", slot);
    }
    if (FAILED(IDirect3DTexture9_GetSurfaceLevel(g_pages[page].vram, 0, &atlas))) return 0;

    src.left = col * 128; src.top = row * 128;
    src.right = src.left + 128; src.bottom = src.top + 128;

    if (SUCCEEDED(IDirect3DDevice9_CreateRenderTarget(dev, 128, 128, g_page_fmt,
            D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL)) &&
        SUCCEEDED(IDirect3DDevice9_StretchRect(dev, atlas, &src, rt, NULL, D3DTEXF_NONE)) &&
        SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 128, 128,
            g_page_fmt, D3DPOOL_SYSTEMMEM, &sys, NULL)) &&
        SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, rt, sys)) &&
        SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {

        for (y = 0; y < 128; y++)
            memcpy(out + (size_t)y * 128 * 4,
                   (unsigned char *)lr.pBits + (size_t)y * lr.Pitch, 128 * 4);
        IDirect3DSurface9_UnlockRect(sys);
        ok = 1;
    }
    if (sys)   IDirect3DSurface9_Release(sys);
    if (rt)    IDirect3DSurface9_Release(rt);
    if (atlas) IDirect3DSurface9_Release(atlas);
    return ok;
}

/* The stored original for a slot, capturing it on first use. NULL if we could
 * not read it - in which case we must not overwrite the slot at all, or the
 * flag could never be restored. */
static const unsigned char *original_for(IDirect3DDevice9 *dev, int slot)
{
    int i;
    for (i = 0; i < g_norig; i++)
        if (g_orig[i].slot == slot) return g_orig[i].valid ? g_orig[i].px : NULL;

    if (g_norig == g_orig_cap) {
        int cap = g_orig_cap ? g_orig_cap * 2 : 64;
        orig_tile *grown = g_orig
            ? (orig_tile *)HeapReAlloc(GetProcessHeap(), 0, g_orig, sizeof(orig_tile) * (size_t)cap)
            : (orig_tile *)HeapAlloc(GetProcessHeap(), 0, sizeof(orig_tile) * (size_t)cap);
        if (!grown) return NULL;
        g_orig = grown;
        g_orig_cap = cap;
    }
    g_orig[g_norig].slot  = slot;
    g_orig[g_norig].px    = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, 128 * 128 * 4);
    if (!g_orig[g_norig].px) return NULL;
    cpbm_log("d3d9: capturing original of slot %d ...", slot);
    g_orig[g_norig].valid = read_tile(dev, slot, g_orig[g_norig].px);
    cpbm_log("d3d9: captured original of slot %d -> %s",
             slot, g_orig[g_norig].valid ? "ok" : "FAILED (slot will be left alone)");
    g_norig++;
    return g_orig[g_norig - 1].valid ? g_orig[g_norig - 1].px : NULL;
}

/* Defined below, used by EndScene above them. */
static void drain_queue(IDirect3DDevice9 *dev);
static int  queue_depth(void);
static void repaint_active(IDirect3DDevice9 *dev);
int cpbm_d3d9_apply_flag(IDirect3DDevice9 *dev, int slot, const wchar_t *name);

/* Top of every EndScene: adopt a finished rebuild, or drop a pending set that
 * stalled. Releasing our references to the old pages frees them; the game has
 * already let go of them by the time it builds the new set. */
static void service_rebuild(void)
{
    int i;
    if (!g_pend_ready || (!g_npending && !g_adopt_ready)) return;

    EnterCriticalSection(&g_pend_lock);
    if (g_adopt_ready) {
        for (i = 0; i < g_npages; i++) {
            if (g_pages[i].vram)    IDirect3DTexture9_Release(g_pages[i].vram);
            if (g_pages[i].staging) IDirect3DTexture9_Release(g_pages[i].staging);
        }
        for (i = 0; i < g_npending; i++) {
            g_pages[i] = g_pending[i];
            g_pending[i].vram = g_pending[i].staging = NULL;
        }
        g_npages   = g_npending;
        g_npending = 0;
        InterlockedExchange(&g_adopt_ready, 0);

        if (g_pending_device && g_pending_device != g_atlas_device) {
            IDirect3DDevice9_AddRef(g_pending_device);
            if (g_atlas_device) IDirect3DDevice9_Release(g_atlas_device);
            g_atlas_device    = g_pending_device;
            g_announced_owner = 0;
            cpbm_log("d3d9: atlas device changed to %p", (void *)g_atlas_device);
        }
        g_generation++;
        g_repaint_in = REPAINT_FRAMES;
        cpbm_log("d3d9: ATLAS REBUILT - generation %d adopted, %d page(s)",
                 g_generation, g_npages);
        cpbm_log("d3d9:   (EU4 rebuilds the atlas when a save is loaded; the old pages "
                 "have been released)");
    } else if (g_npending && GetTickCount64() - g_pending_last_ms > PENDING_STALL_MS) {
        cpbm_log("d3d9: discarding %d stray atlas-shaped texture(s) - too far apart to be "
                 "a rebuild", g_npending);
        pending_release_all();
    }
    LeaveCriticalSection(&g_pend_lock);
}

static __declspec(thread) int t_in_endscene;

static HRESULT WINAPI my_EndScene(IDirect3DDevice9 *dev)
{
    InterlockedIncrement(&g_endscene_calls);

    /* We re-hook a slot that somebody else took, chaining through whatever they
     * installed. If their chain ever comes back to us, recursing would blow the
     * stack, so fall through to the genuine d3d9 EndScene captured before
     * anyone touched the slot. */
    if (t_in_endscene) {
        int vi0 = vt_index(*(void ***)dev);
        return (vi0 >= 0 && g_vt[vi0].first_EndScene)
                 ? g_vt[vi0].first_EndScene(dev) : S_OK;
    }
    t_in_endscene = 1;
    service_rebuild();

    /* Only attempt once the full atlas is present. A partial set means we have
     * misidentified something, and writing on a guess is how the last crash
     * happened. */
    /* Announce each presenting device once. If no "owns the atlas" line ever
     * appears, the atlas device never presents and the queue can never drain -
     * which is then the whole bug, and visible instead of silent. */
    if (!g_seen_present || g_seen_present != dev) {
        g_seen_present = dev;
        cpbm_log("d3d9: EndScene from device %p%s", (void *)dev,
                 dev == g_atlas_device ? "  <- owns the atlas" : "");
    }
    /* Say it again once the owner is actually known. Presenting starts before
     * the atlas is built, so the first announcements are all made while
     * g_atlas_device is still NULL - which reads as "no device owns the atlas"
     * when in fact we simply asked too early. That is the exact line a second machine
     * is relying on to diagnose, so it must not be misleading. */
    if (g_atlas_device && !g_announced_owner) {
        g_announced_owner = 1;
        cpbm_log("d3d9: atlas is owned by device %p; this EndScene is from %p%s",
                 (void *)g_atlas_device, (void *)dev,
                 dev == g_atlas_device ? "  <- owns the atlas, queue can drain"
                                       : "  <- NOT the owner; waiting for its EndScene");
    }

    /* Per-frame while anything is waiting: says on the very next frame whether
     * the drain condition holds, so a blocked drain names its own reason. */
    {
        static unsigned long dc_frames;
        int qd = queue_depth();
        if (qd > 0 && !(g_expected_pages && g_npages >= g_expected_pages &&
                        dev == g_atlas_device) && (dc_frames++ % 300) == 0)
            cpbm_log("d3d9: [drain-check] queued=%d dev=%p atlas=%p pages=%d/%d -> %s",
                     qd, (void *)dev, (void *)g_atlas_device,
                     g_npages, g_expected_pages,
                     (g_expected_pages && g_npages >= g_expected_pages &&
                      dev == g_atlas_device) ? "draining now" : "BLOCKED");
    }

    /* DIAGNOSTIC heartbeat. An earlier log from another machine could not tell "EndScene
     * stopped being called" apart from "the queue was empty every time it
     * drained" - both look like total silence. This separates them. */
    {
        static unsigned long hb_frames;
        if ((hb_frames++ % 600) == 0)
            cpbm_log("d3d9: [hb] EndScene frame %lu dev=%p atlas_dev=%p "
                     "pages=%d/%d queued=%d",
                     hb_frames, (void *)dev, (void *)g_atlas_device,
                     g_npages, g_expected_pages, queue_depth());
    }

    if (g_expected_pages && g_npages >= g_expected_pages && dev == g_atlas_device) {
        if (g_repaint_in > 0 && --g_repaint_in == 0) repaint_active(dev);
        drain_queue(dev);

        /* 1 = show the override, 2 = put the original back. Exercising both
         * directions matters: a rule that stops matching has to restore, and an
         * override that cannot be undone is worse than no override. */
        if (InterlockedCompareExchange(&g_swap_pending, 0, 1) == 1)
            cpbm_log("d3d9: apply FRA_white to slot 121 -> %s",
                     cpbm_d3d9_apply_flag(dev, 121, L"FRA_white") ? "OK" : "FAILED");
        else if (InterlockedCompareExchange(&g_swap_pending, 0, 2) == 2)
            cpbm_log("d3d9: restore original of slot 121 -> %s",
                     cpbm_d3d9_apply_flag(dev, 121, NULL) ? "OK" : "FAILED");
    }
    {
        int vi = vt_index(*(void ***)dev);
        HRESULT chained = (vi >= 0) ? g_vt[vi].real_EndScene(dev) : S_OK;
        t_in_endscene = 0;
        return chained;
    }
}

/* Show `name` in a country's slot, or restore the original when name is NULL.
 * This is the operation the rules engine drives: a matching rule names a flag,
 * no match means the country keeps its own. Idempotent per slot - it only
 * writes when what should be shown has actually changed. */
int cpbm_d3d9_apply_flag(IDirect3DDevice9 *dev, int slot, const wchar_t *name)
{
    static unsigned char tile[128 * 128 * 4];
    const unsigned char *orig;

    if (g_write_disabled) return 0;
    if (!g_expected_pages || g_npages < g_expected_pages) {
        cpbm_log("d3d9: only %d of %d atlas page(s) captured - not writing",
                 g_npages, g_expected_pages);
        return 0;
    }

    /* Capture before the first overwrite, or the original is lost forever. */
    orig = original_for(dev, slot);
    if (!orig) {
        /* An unreadable original used to abort the write silently. Restoring is
         * impossible without it, but showing the override is not - and a flag
         * that appears and cannot be undone beats one that never appears with
         * no explanation. */
        if (!name) {
            cpbm_log("d3d9: slot %d - no original captured, cannot restore", slot);
            return 0;
        }
        cpbm_log("d3d9: slot %d - original could not be captured; applying anyway "
                 "(this slot will not be restorable this session)", slot);
    }

    if (!name) {
        int ok = write_tile(dev, slot, orig);
        if (ok) clear_active(slot);
        return ok;
    }

    if (!load_flag_tga(name, tile)) {
        cpbm_log("d3d9: %S.tga would not load; leaving slot %d alone", name, slot);
        return 0;
    }
    {
        int ok = write_tile(dev, slot, tile);
        if (ok) set_active(slot, name);
        return ok;
    }
}

/* Paint every current override into a freshly adopted atlas. Copied first,
 * because applying updates the table it walks. */
static void repaint_active(IDirect3DDevice9 *dev)
{
    static active_flag work[MAX_ACTIVE];
    int i, n = g_nactive;

    if (!n) return;
    for (i = 0; i < n; i++) work[i] = g_active[i];
    cpbm_log("d3d9: repainting %d slot(s) into atlas generation %d", n, g_generation);
    for (i = 0; i < n; i++) {
        int ok = cpbm_d3d9_apply_flag(dev, work[i].slot, work[i].name);
        cpbm_log("d3d9:   slot %d <- %S : %s", work[i].slot, work[i].name, ok ? "OK" : "FAILED");
    }
}

/* ------------------------------------------------------------- queue ---- *
 * The log watcher runs on its own thread, but D3D work must happen on the
 * render thread. Requests are parked here and drained in EndScene. */

/* Grows as needed and holds at most one request per slot: a newer request for a
 * slot replaces the one still waiting, because only the final state matters. A
 * fixed table of 64 silently dropped the rest of the startup burst, and those
 * countries kept the wrong flag until their rule next changed.
 *
 * Named, so the drain can copy entries out wholesale - two anonymous struct
 * types are not assignment-compatible in C. */
typedef struct { int slot; int restore; wchar_t name[32]; } flagreq;
static flagreq         *g_queue;
static int              g_nqueue, g_queue_cap;
/* Writes per frame. Startup and save loads announce every country at once;
 * spreading them out keeps the whole burst from landing in one frame. */
#define DRAIN_PER_FRAME 16
static CRITICAL_SECTION g_qlock;
static int              g_qready;

/* DIAGNOSTIC: queue depth for the EndScene heartbeat. -1 = never armed. */
static int queue_depth(void)
{
    int n;
    if (!g_qready) return -1;
    EnterCriticalSection(&g_qlock);
    n = g_nqueue;
    LeaveCriticalSection(&g_qlock);
    return n;
}

void cpbm_d3d9_queue(int slot, const char *name)
{
    int depth = -1;
    if (!g_qready) {
        cpbm_log("d3d9: [q] slot %d DROPPED - queue never armed", slot);
        return;
    }
    EnterCriticalSection(&g_qlock);
    {
        int i, at = -1;
        for (i = 0; i < g_nqueue; i++)
            if (g_queue[i].slot == slot) { at = i; break; }
        if (at < 0) {
            if (g_nqueue == g_queue_cap) {
                int cap = g_queue_cap ? g_queue_cap * 2 : 128;
                flagreq *grown = g_queue
                    ? (flagreq *)HeapReAlloc(GetProcessHeap(), 0, g_queue, sizeof(flagreq) * (size_t)cap)
                    : (flagreq *)HeapAlloc(GetProcessHeap(), 0, sizeof(flagreq) * (size_t)cap);
                if (grown) { g_queue = grown; g_queue_cap = cap; }
            }
            if (g_nqueue < g_queue_cap) at = g_nqueue++;
        }
        if (at >= 0) {
            g_queue[at].slot    = slot;
            g_queue[at].restore = (name == NULL);
            if (name)
                MultiByteToWideChar(CP_UTF8, 0, name, -1, g_queue[at].name, 32);
            else
                g_queue[at].name[0] = 0;
        }
    }
    depth = g_nqueue;
    LeaveCriticalSection(&g_qlock);
    cpbm_log("d3d9: [q] slot %d <- %s queued, depth %d", slot,
             name ? name : "(original)", depth);
}

/* Drain on the render thread. Copies out under the lock, then does the D3D work
 * outside it - a texture write must never be done holding a lock the watcher
 * thread might want. */
static void drain_queue(IDirect3DDevice9 *dev)
{
    flagreq work[DRAIN_PER_FRAME];
    int n, i, left;

    if (!g_qready) return;
    EnterCriticalSection(&g_qlock);
    n = g_nqueue < DRAIN_PER_FRAME ? g_nqueue : DRAIN_PER_FRAME;
    for (i = 0; i < n; i++) work[i] = g_queue[i];
    if (n) memmove(g_queue, g_queue + n, sizeof(flagreq) * (size_t)(g_nqueue - n));
    g_nqueue -= n;
    left = g_nqueue;
    LeaveCriticalSection(&g_qlock);

    if (n) cpbm_log("d3d9: draining %d queued write(s), %d still waiting", n, left);
    for (i = 0; i < n; i++) {
        int ok = cpbm_d3d9_apply_flag(dev, work[i].slot,
                                      work[i].restore ? NULL : work[i].name);
        cpbm_log("d3d9: slot %d <- %S : %s", work[i].slot,
                 work[i].restore ? L"(original)" : work[i].name, ok ? "OK" : "FAILED");
    }
}

void cpbm_d3d9_request_test_swap(void) { InterlockedExchange(&g_swap_pending, 1); }

/* Patch CreateTexture and EndScene on whatever device we are handed, whichever
 * of the two creation paths produced it. The inherited methods sit at the same
 * vtable indices in both classes, so the same offsets serve. */
static void hook_device_vtable(IDirect3DDevice9 *dev, const char *how)
{
    void **vt;
    size_t idx;
    int    slot;

    if (!dev) return;

    /* Keep the first device for reference, but never let that stop us hooking
     * later ones - that guard is exactly what broke this. */
    if (!g_device) {
        g_device = dev;
        IDirect3DDevice9_AddRef(g_device);
    }

    vt = *(void ***)dev;
    if (vt_index(vt) >= 0) {
        cpbm_log("d3d9: device via %s (%p) shares an already-hooked vtable", how, (void *)dev);
        return;
    }
    if (g_nvt >= MAX_VT) { cpbm_log("d3d9: too many device vtables to hook"); return; }

    slot = g_nvt++;
    g_vt[slot].vt = vt;
    cpbm_log("d3d9: device via %s (%p), new vtable %p -> hooking it", how, (void *)dev, (void *)vt);

    idx = offsetof(IDirect3DDevice9Vtbl, CreateTexture) / sizeof(void *);
    g_vt[slot].ct_slot = &vt[idx];
    if (poke_ptr(&vt[idx], (void *)my_CreateTexture, (void **)&g_vt[slot].real_CreateTexture))
        cpbm_log("d3d9:   CreateTexture hooked at vtable[%u]", (unsigned)idx);
    else
        cpbm_log("d3d9:   FAILED to hook CreateTexture");

    /* Texture writes must happen on the render thread; EndScene is where we
     * get to run on it. */
    idx = offsetof(IDirect3DDevice9Vtbl, EndScene) / sizeof(void *);
    g_vt[slot].es_slot = &vt[idx];
    if (poke_ptr(&vt[idx], (void *)my_EndScene, (void **)&g_vt[slot].real_EndScene)) {
        g_vt[slot].first_EndScene = g_vt[slot].real_EndScene;
        cpbm_log("d3d9:   EndScene hooked at vtable[%u]", (unsigned)idx);
    }
    else
        cpbm_log("d3d9:   FAILED to hook EndScene");
}

static HRESULT WINAPI my_CreateDevice(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type,
                                      HWND focus, DWORD flags,
                                      D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    HRESULT hr = real_CreateDevice(d3d, adapter, type, focus, flags, pp, out);

    if (SUCCEEDED(hr) && out && *out) {
        /* MULTITHREADED would let texture writes happen off the render thread,
         * making the whole EndScene dependency - and every overlay fight over
         * that hook - unnecessary. Worth knowing which one we are on. */
        cpbm_log("d3d9: CreateDevice flags=0x%08lX%s", (unsigned long)flags,
                 (flags & D3DCREATE_MULTITHREADED) ? "  MULTITHREADED" : "");
        hook_device_vtable(*out, "CreateDevice");
    }
    return hr;
}

/* IDirect3D9Ex::CreateDeviceEx returns an IDirect3DDevice9Ex - a DIFFERENT
 * class with its own vtable. Patching IDirect3DDevice9's does nothing to it.
 * Hooking only CreateDevice worked on one machine and on another left
 * CreateTexture never firing once: no atlas page captured, every write skipped,
 * and nothing in the log to say why. */
static HRESULT (WINAPI *real_CreateDeviceEx)(IDirect3D9Ex *, UINT, D3DDEVTYPE, HWND, DWORD,
                                             D3DPRESENT_PARAMETERS *, D3DDISPLAYMODEEX *,
                                             IDirect3DDevice9Ex **);

static HRESULT WINAPI my_CreateDeviceEx(IDirect3D9Ex *d3d, UINT adapter, D3DDEVTYPE type,
                                        HWND focus, DWORD flags, D3DPRESENT_PARAMETERS *pp,
                                        D3DDISPLAYMODEEX *mode, IDirect3DDevice9Ex **out)
{
    HRESULT hr = real_CreateDeviceEx(d3d, adapter, type, focus, flags, pp, mode, out);
    if (SUCCEEDED(hr) && out && *out) {
        cpbm_log("d3d9: CreateDeviceEx flags=0x%08lX%s", (unsigned long)flags,
                 (flags & D3DCREATE_MULTITHREADED) ? "  MULTITHREADED" : "");
        hook_device_vtable((IDirect3DDevice9 *)*out, "CreateDeviceEx");
    }
    return hr;
}

static void hook_d3d9_object(IDirect3D9 *d3d)
{
    void **vt;
    size_t idx;
    if (!d3d || real_CreateDevice) return;

    vt  = *(void ***)d3d;
    idx = offsetof(IDirect3D9Vtbl, CreateDevice) / sizeof(void *);
    if (poke_ptr(&vt[idx], (void *)my_CreateDevice, (void **)&real_CreateDevice))
        cpbm_log("d3d9: CreateDevice hooked at vtable[%u]", (unsigned)idx);
    else
        cpbm_log("d3d9: FAILED to hook CreateDevice");
}

/* The Ex object exposes CreateDeviceEx further down its own vtable. */
static void hook_d3d9ex_object(IDirect3D9Ex *d3d)
{
    void **vt;
    size_t idx;
    if (!d3d || real_CreateDeviceEx) return;

    vt  = *(void ***)d3d;
    idx = offsetof(IDirect3D9ExVtbl, CreateDeviceEx) / sizeof(void *);
    if (poke_ptr(&vt[idx], (void *)my_CreateDeviceEx, (void **)&real_CreateDeviceEx))
        cpbm_log("d3d9: CreateDeviceEx hooked at vtable[%u]", (unsigned)idx);
    else
        cpbm_log("d3d9: FAILED to hook CreateDeviceEx");
}

static IDirect3D9 *WINAPI my_Direct3DCreate9(UINT sdk)
{
    IDirect3D9 *d3d = real_create9(sdk);
    cpbm_log("d3d9: Direct3DCreate9(%u) -> %p", sdk, (void *)d3d);
    hook_d3d9_object(d3d);
    return d3d;
}

static HRESULT WINAPI my_Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex **out)
{
    HRESULT hr = real_create9ex(sdk, out);
    cpbm_log("d3d9: Direct3DCreate9Ex(%u) -> hr=0x%08lX", sdk, (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out) {
        hook_d3d9_object((IDirect3D9 *)*out);   /* in case it uses CreateDevice */
        hook_d3d9ex_object(*out);               /* and the Ex path it may prefer */
    }
    return hr;
}

/* ---------------------------------------------------------------- D3D11 --
 *
 * Measured: the D3D9 hooks above install fine and are never called. EU4 does
 * not use D3D9 - it loads d3d11.dll at runtime and resolves D3D11CreateDevice
 * through GetProcAddress, which is why neither name appears in its import
 * table while "d3d11.dll" and "D3D11CreateDevice" appear as plain strings.
 *
 * So the way in is GetProcAddress itself, which IS imported by name. We hand
 * back our own D3D11CreateDevice, and from the device we hook CreateTexture2D.
 *
 * Intercepting creation matters for more than watching: a texture created
 * IMMUTABLE can never be written to afterwards. Because we see the descriptor
 * before the device does, we can widen it at creation time - which is the
 * difference between this approach working and not. This build only reports
 * what it sees; it changes nothing. */

typedef HRESULT (WINAPI *pfn_d3d11create)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
                                          const D3D_FEATURE_LEVEL *, UINT, UINT,
                                          ID3D11Device **, D3D_FEATURE_LEVEL *,
                                          ID3D11DeviceContext **);

static pfn_d3d11create real_d3d11create;
static FARPROC (WINAPI *real_GetProcAddress)(HMODULE, LPCSTR);

static HRESULT (WINAPI *real_CreateTexture2D)(ID3D11Device *, const D3D11_TEXTURE2D_DESC *,
                                              const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D **);
static ID3D11Device *g_dev11;

static const char *usage_name(D3D11_USAGE u)
{
    switch (u) {
    case D3D11_USAGE_DEFAULT:   return "DEFAULT (UpdateSubresource works)";
    case D3D11_USAGE_IMMUTABLE: return "IMMUTABLE (cannot be written - must widen at creation)";
    case D3D11_USAGE_DYNAMIC:   return "DYNAMIC (Map/discard only, no partial update)";
    case D3D11_USAGE_STAGING:   return "STAGING";
    default:                    return "unknown";
    }
}

static HRESULT WINAPI my_CreateTexture2D(ID3D11Device *dev, const D3D11_TEXTURE2D_DESC *desc,
                                         const D3D11_SUBRESOURCE_DATA *init,
                                         ID3D11Texture2D **out)
{
    HRESULT hr = real_CreateTexture2D(dev, desc, init, out);

    if (SUCCEEDED(hr) && desc && desc->Width >= 2048 && desc->Height >= 2048) {
        g_atlas_seen++;
        cpbm_log("d3d11: texture %ux%u mips=%u arr=%u fmt=%d usage=%s bind=0x%X cpu=0x%X tex=%p",
                 desc->Width, desc->Height, desc->MipLevels, desc->ArraySize,
                 (int)desc->Format, usage_name(desc->Usage),
                 desc->BindFlags, desc->CPUAccessFlags,
                 (void *)(out ? *out : NULL));
    }
    return hr;
}

static HRESULT WINAPI my_D3D11CreateDevice(IDXGIAdapter *ad, D3D_DRIVER_TYPE dt, HMODULE sw,
                                           UINT flags, const D3D_FEATURE_LEVEL *levels,
                                           UINT nlevels, UINT sdk, ID3D11Device **dev,
                                           D3D_FEATURE_LEVEL *got, ID3D11DeviceContext **ctx)
{
    HRESULT hr = real_d3d11create(ad, dt, sw, flags, levels, nlevels, sdk, dev, got, ctx);

    cpbm_log("d3d11: D3D11CreateDevice -> hr=0x%08lX dev=%p",
             (unsigned long)hr, (void *)(dev ? *dev : NULL));

    if (SUCCEEDED(hr) && dev && *dev && !g_dev11) {
        void **vt = *(void ***)*dev;
        size_t idx = offsetof(ID3D11DeviceVtbl, CreateTexture2D) / sizeof(void *);
        g_dev11 = *dev;
        if (poke_ptr(&vt[idx], (void *)my_CreateTexture2D, (void **)&real_CreateTexture2D))
            cpbm_log("d3d11: CreateTexture2D hooked at vtable[%u]", (unsigned)idx);
        else
            cpbm_log("d3d11: FAILED to hook CreateTexture2D");
    }
    return hr;
}

static FARPROC WINAPI my_GetProcAddress(HMODULE mod, LPCSTR name)
{
    FARPROC p = real_GetProcAddress(mod, name);

    /* Ordinal lookups arrive as small integers, not pointers - do not deref. */
    if (p && name && ((ULONG_PTR)name >> 16) != 0) {
        if (strcmp(name, "D3D11CreateDevice") == 0) {
            if (!real_d3d11create) {
                real_d3d11create = (pfn_d3d11create)p;
                cpbm_log("d3d11: intercepted GetProcAddress(\"D3D11CreateDevice\")");
            }
            return (FARPROC)my_D3D11CreateDevice;
        }
    }
    return p;
}

/* -------------------------------------------------------------- public -- */

/* Name whatever module a pointer lives in, so "somebody re-took our hook" can
 * say WHO. An overlay is fixable; recycled heap is not. */
static void describe_ptr(void *p, char *out, size_t n)
{
    HMODULE m = NULL;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)p, &m) && m &&
        GetModuleFileNameA(m, path, MAX_PATH)) {
        const char *base = strrchr(path, '\\');
        _snprintf(out, n, "%s+0x%llX", base ? base + 1 : path,
                  (unsigned long long)((char *)p - (char *)m));
    } else {
        _snprintf(out, n, "no module (not code)");
    }
    out[n - 1] = 0;
}

/* Only ever re-hook a slot whose current value is executable code inside a
 * loaded module. One machine's vtables came back holding the ASCII
 * "eu4_dlc0" - that device had been freed and its memory reused, and writing
 * a pointer back into it would corrupt whatever owns it now. */
static int is_code_ptr(void *p)
{
    MEMORY_BASIC_INFORMATION mbi;
    HMODULE m = NULL;
    if (!p) return 0;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)p, &m) || !m) return 0;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

/* DIAGNOSTIC watchdog.
 *
 * It reports from its own thread, so it can describe the one state the logging
 * inside EndScene cannot: EndScene not being called at all. On one test machine the
 * queue fills, nothing drains, and the game keeps rendering perfectly - which
 * means either the game stopped calling our EndScene, or our function is no
 * longer the one in the vtable. This tells those apart. */
static DWORD WINAPI cpbm_d3d9_watchdog(LPVOID unused)
{
    LONG last = 0;
    (void)unused;
    for (;;) {
        LONG now;
        int  i, intact = 0, replaced = 0;

        Sleep(2000);
        now = g_endscene_calls;

        for (i = 0; i < g_nvt; i++) {
            void *cur;
            char  who[192];

            if (!g_vt[i].es_slot || g_vt[i].dead) continue;
            cur = *g_vt[i].es_slot;
            if (cur == (void *)my_EndScene) { intact++; continue; }

            replaced++;
            describe_ptr(cur, who, sizeof who);

            /* Recycled memory, not a hook. Writing here would corrupt whatever
             * owns that heap block now. */
            if (!is_code_ptr(cur)) {
                g_vt[i].dead = 1;
                cpbm_log("d3d9: [wd] vtable %p is GONE - slot holds %p (%s). That "
                         "device was destroyed and its memory reused; never "
                         "touching it again.", (void *)g_vt[i].vt, cur, who);
                continue;
            }

            cpbm_log("d3d9: [wd] vtable %p EndScene was taken by %s (%p) - "
                     "re-hooking, chaining through it",
                     (void *)g_vt[i].vt, who, cur);
            if (!poke_ptr(g_vt[i].es_slot, (void *)my_EndScene,
                          (void **)&g_vt[i].real_EndScene))
                cpbm_log("d3d9: [wd] FAILED to re-hook EndScene on vtable %p",
                         (void *)g_vt[i].vt);

            cur = g_vt[i].ct_slot ? *g_vt[i].ct_slot : NULL;
            if (cur && cur != (void *)my_CreateTexture && is_code_ptr(cur)) {
                describe_ptr(cur, who, sizeof who);
                if (poke_ptr(g_vt[i].ct_slot, (void *)my_CreateTexture,
                             (void **)&g_vt[i].real_CreateTexture))
                    cpbm_log("d3d9: [wd] CreateTexture re-hooked on vtable %p "
                             "(had been taken by %s)", (void *)g_vt[i].vt, who);
            }
        }

        cpbm_log("d3d9: [wd] EndScene total=%ld (+%ld in 2s)  textures=%d  "
                 "pages=%d/%d  queued=%d  vtables ok=%d retaken=%d",
                 now, now - last, g_textures_seen, g_npages, g_expected_pages,
                 queue_depth(), intact, replaced);
        last = now;
    }
}

void cpbm_d3d9_init(void)
{
    int a, b, c;

    if (cpbm_switch(L"cpbm.NOD3D9")) {
        cpbm_log("d3d9: disabled by cpbm.NOD3D9 - no hooks, no texture writes");
        return;
    }

    /* D3D9 kept as a diagnostic: it costs nothing, and its silence is what
     * established that the game is not using D3D9 at all. */
    InitializeCriticalSection(&g_qlock);
    g_qready = 1;
    InitializeCriticalSection(&g_pend_lock);
    g_pend_ready = 1;

    probe_atlas_geometry();

    a = hook_iat("d3d9.dll", "Direct3DCreate9",   (void *)my_Direct3DCreate9,   (void **)&real_create9);
    b = hook_iat("d3d9.dll", "Direct3DCreate9Ex", (void *)my_Direct3DCreate9Ex, (void **)&real_create9ex);

    /* The real route. D3D11 is resolved dynamically, so GetProcAddress is the
     * chokepoint - and it is imported by name, so this needs no code patching. */
    c = hook_iat("kernel32.dll", "GetProcAddress",
                 (void *)my_GetProcAddress, (void **)&real_GetProcAddress);

    cpbm_log("gfx: IAT hooks - Direct3DCreate9=%s Direct3DCreate9Ex=%s GetProcAddress=%s",
             a ? "ok" : "FAILED", b ? "ok" : "FAILED", c ? "ok" : "FAILED");
    if (!c)
        cpbm_log("gfx: GetProcAddress could not be patched; the D3D11 device "
                 "cannot be reached this way.");

    /* Epic's overlay will overwrite the above shortly. Re-take the slots once,
     * timed off d3d11.dll appearing, chaining through whatever Epic installed. */
    {
        HANDLE t = CreateThread(NULL, 0, rehook_once, NULL, 0, NULL);
        HANDLE w = CreateThread(NULL, 0, cpbm_d3d9_watchdog, NULL, 0, NULL);
        if (t) CloseHandle(t);
        else cpbm_log("gfx: could not start the re-hook thread");
        if (w) CloseHandle(w);
        else cpbm_log("gfx: could not start the watchdog thread");
    }
}

int cpbm_d3d9_atlas_count(void) { return g_atlas_seen; }

/* Called well after startup, once the renderer is definitely up. Distinguishes
 * "the game never made the call" from "somebody replaced our hook". */
void cpbm_d3d9_verify(void)
{
    int i, intact = 0, stolen = 0;

    if (!g_nslots) { cpbm_log("gfx: no IAT slots were patched"); return; }

    for (i = 0; i < g_nslots; i++) {
        void *now = *g_slots[i].slot;
        if (now == g_slots[i].ours) {
            intact++;
        } else {
            stolen++;
            cpbm_log("gfx: OUR HOOK ON %s WAS REPLACED - slot now points at %p "
                     "(ours was %p)", g_slots[i].name, now, g_slots[i].ours);
        }
    }
    cpbm_log("gfx: %d of %d IAT hook(s) still ours; %d overwritten",
             intact, g_nslots, stolen);

    cpbm_log("gfx: d3d11.dll loaded=%d dxgi.dll loaded=%d opengl32 ICD active=%d",
             GetModuleHandleW(L"d3d11.dll") != NULL,
             GetModuleHandleW(L"dxgi.dll") != NULL,
             GetModuleHandleW(L"nvoglv64.dll") != NULL);

    if (stolen == 0 && g_atlas_seen == 0)
        cpbm_log("gfx: hooks intact but nothing came through them - the device "
                 "is being created by some path we are not on.");
}
