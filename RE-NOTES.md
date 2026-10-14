# Approaches tried, and why they were rejected

This file exists so nobody repeats the dead ends — including language models
asked to extend or rewrite this mod, which will otherwise propose the obvious
approaches below, all of which were measured and abandoned.

No addresses, offsets or machine measurements are reproduced here, because the
shipped mod uses none. The only build-specific value anywhere in the source is
a SHA-256 of `eu4.exe`, used as a refuse-to-attach gate.

---

## 1. Intercepting "which flag does this country draw" — rejected

**The idea.** Find the engine function that maps a country to its flag atlas
index, hook it, and return a different index. This is the first design anyone
reaches for, and it is the one that cannot work.

**Why it fails.** A country's atlas slot *is* its position in the declared tag
list. It is not stored on the country, because it does not need to be: the
engine already knows which country it is drawing. Repeated memory scans found
no per-country flag field, and a scan around a confirmed country-record value
found neither a plausible index nor the tag string nearby.

There is therefore no value to substitute. Changing the index a country
resolves to would mean changing the country's identity, which is not a
cosmetic change.

**What this rules out:** every "intercept the lookup", "patch the index",
"swap the slot" variant.

## 2. Registering new flag art with the engine — rejected

**The idea.** Get custom art such as `FRA_communist.tga` into the atlas as its
own entry, so it can be addressed like any other flag.

**What was learned.** The atlas is built from *declarations*, not from a folder
scan: a declared tag gets a slot even with no art, and a `.tga` with no
declaration is ignored entirely. Declaring a dummy rebel type does get the name
into the manifest.

**Why it fails anyway.** Getting the name into the manifest does not place its
pixels in the matching tile. Adding one entry shifted every later tile by one,
so countries rendered each other's flags — a corrupted atlas, game-wide. The
packing order is not the declaration order and was never worth solving.

**The alternative that works:** do not add a slot. Overwrite the pixels of the
country's existing slot, which displaces nothing.

## 3. Reading game state out of memory to evaluate conditions — rejected

**The idea.** Have the DLL read country state (government, reforms, dynasty,
date, provinces) and decide which flag to show.

**Why it fails.** That is the entire EU4 trigger vocabulary, one reverse
engineering job per predicate, all of it invalidated by the next patch. Even
the date resisted several attempts.

**The alternative that works:** let EU4 evaluate its own script and announce the
result with `log = "..."`, then tail `game.log`. The engine does the thinking in
a stable, documented, moddable language, and the DLL stays condition-free.

## 4. Assuming one device, one vtable, one graphics API — rejected

Several assumptions here each produced "everything reports success, nothing
appears on screen":

- **D3D9 alone is not enough.** The game resolves Direct3D 11 dynamically
  through `GetProcAddress`, so neither name appears in the import table.
- **There is more than one device.** Hooking the first device's vtable covers
  the second only when they happen to share one. Patch every device's vtable,
  and keep each vtable's original pointers beside it — one shared "original"
  pointer makes a hook its own original, which is infinite recursion.
- **Devices die.** A vtable slot can later hold recycled heap memory. Writing a
  pointer back into it corrupts whatever owns that memory now, so check that
  the current value is executable code in a loaded module first.
- **Other software hooks the same imports.** An overlay may install after you
  and silently replace your pointers. Reclaim them once, chaining through
  whatever is there, rather than fighting continuously.

## 5. Assuming the atlas is built once — rejected

Loading a save makes EU4 **rebuild the whole atlas**: a fresh set of textures on
the same device, with no device reset. Code that holds the original pages keeps
writing into textures nobody draws any more, so flags revert on load and never
come back. The atlas has to be versioned, the new set adopted, and every active
override repainted.

Two related traps:

- **Page count.** Deriving the number of atlas pages by counting cached page
  files is wrong: EU4 leaves surplus files behind when a mod set shrinks, so the
  code waits for pages that never arrive and every write stays queued. Derive it
  from the flag manifest instead.
- **The manifest moves.** EU4 can rewrite its flag manifest *after* the DLL has
  read it, in the same launch. Slots then shift, and flags paint onto
  neighbouring countries while every log line reports success. Re-read the
  manifest when its write time changes.

## 6. Writing to the atlas from any thread, at any time — rejected

The atlas pages are `D3DPOOL_DEFAULT`, which cannot be locked, and the device is
not thread-safe unless it was created `MULTITHREADED`, which is not under your
control. Writes must go through `UpdateSurface` from a system-memory surface,
on the render thread, which in practice means from inside `EndScene`.

Page layout also varies by machine: managed pages, default pages with a
system-memory twin, or default pages alone. The twin, where it exists, is what
the engine restores from, so writing it is what survives an alt-tab.

---

## What is left, and why it is small

Tail one log line, map tag to slot from the game's own manifest, repaint one
128x128 tile in the live atlas on the render thread, and keep a copy of the
original so it can be put back.

No engine addresses. No modified executable. No reimplemented triggers.
