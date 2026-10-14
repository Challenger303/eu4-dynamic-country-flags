#ifndef CPBM_D3D9_H
#define CPBM_D3D9_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct IDirect3DDevice9;

/* Direct3D 9 hook: writes flag art directly into the live atlas.
 *
 * This is the only way to change a country's flag at runtime. The atlas slot IS
 * the country's index in the declared tag list, so there is no per-country flag
 * field to patch - but slot 121 belongs to FRA and nothing else, so rewriting
 * its pixels changes France's flag alone.
 *
 * On by default; a cpbm.NOD3D9 marker file next to the DLL turns it off. */
void cpbm_d3d9_init(void);
void cpbm_d3d9_verify(void);
void cpbm_d3d9_request_test_swap(void);
void cpbm_d3d9_request_test_restore(void);
int  cpbm_d3d9_atlas_count(void);

/* 0 until the first complete atlas is captured, then 1, and +1 every time EU4
 * builds a new atlas (it does on every save load). */
int  cpbm_d3d9_generation(void);

/* Show `name` in a slot, or restore what was there when name is NULL. The
 * original is captured automatically before the first overwrite. */
int  cpbm_d3d9_apply_flag(struct IDirect3DDevice9 *dev, int slot, const wchar_t *name);

/* Request a change from any thread; applied on the render thread in EndScene.
 * name == NULL restores the country's own flag. */
void cpbm_d3d9_queue(int slot, const char *name);

#endif
