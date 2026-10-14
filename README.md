# EU4 Dynamic Country Flags

Change a country's **flag** at runtime in Europa Universalis IV, from ordinary
EU4 script conditions, without changing its tag. France stays `FRA` and keeps
every mission, idea, event and core it has. Only the picture changes.

Names and colours already work in plain script (`override_country_name`,
`change_country_color`). This adds the flag, which the engine otherwise only
does for the Revolution Target.

Built for [Challenger's Flavour Bonanza Mod][cpbm], where it drives the flags of
26 nations, and split out here so other modders can use and audit it.

[cpbm]: https://steamcommunity.com/sharedfiles/filedetails/?id=438503893

---

## How it works

Three moving parts, and only the third is unusual:

1. **Your script decides.** A scripted effect, run from `on_startup` and
   `on_monthly_pulse`, evaluates whatever triggers you like and writes one line:

   ```
   log = "dynamic_country_flag FRA FRA_bonaparte"   # show FRA_bonaparte.tga
   log = "dynamic_country_flag FRA -"               # restore its own flag
   ```

   EU4 writes that to `logs/game.log`. The engine does all the condition work,
   in its own language, so no trigger has to be reimplemented here.

2. **The DLL reads that line.** `cpbm_identity.dll` tails `game.log`, maps the
   tag to its atlas slot using the game's own flag manifest, and queues a write.

3. **The flag is repainted in the live atlas.** EU4 keeps every country flag in
   a Direct3D texture atlas; a country's slot is simply its index in the
   declared tag list. Writing 128x128 pixels into slot N changes exactly one
   country's flag. The write happens on the render thread, through
   `UpdateSurface`, and the original tile is captured first so `-` can restore
   it.

The DLL knows nothing about countries, conditions or mods. It reads one line
shape and paints one tile, so **any number of mods can use it at once**.

### What it does not do

- It does not modify `eu4.exe`, which is never written to, only hashed.
- It does not use any hardcoded game addresses or struct offsets. The only
  build-specific thing is a SHA-256 of `eu4.exe`, and on a mismatch the mod
  refuses to attach rather than risk anything.
- It does not redistribute any Paradox or Microsoft file.

---

## Repo layout

```
impl/dll/core/     the mod itself - log watcher, atlas writer, build gate
impl/dll/shim/     the winmm proxy that gets it loaded (80 lines)
impl/dll/          build-dll.cmd, gen-winmm-def.ps1
installer/         install.bat, what players actually run
example-mod/       a minimal mod: rules, on_actions, localisation
MODDING.md         writing flag rules, day to day
RE-NOTES.md        approaches that were tried and rejected, and why
```

---

## Building

Needs Visual Studio 2022 with the C++ desktop workload. Nothing else.

```
impl\dll\build-dll.cmd
```

That produces `dist\cpbm_identity.dll` (the mod) and `dist\winmm.dll` (the
proxy). The proxy's forwarder table is regenerated from the building machine's
own `System32\winmm.dll` on every build, so it cannot drift from the real
export list.

## Installing

Run `installer\install.bat`. It finds the game folder, copies both DLLs in, and
makes `winmm_orig.dll` from the player's own Windows copy, which is why that
file is never shipped. To uninstall, delete the three DLLs.

---

## Why a proxy DLL

Windows loads a DLL from the game folder before System32, so naming one after a
DLL the game imports gets it loaded with no injector.

`eu4.exe` imports 22 DLLs. Most are KnownDLLs, which Windows always loads from
System32, so hijacking them is impossible rather than merely unwise. `version`
is free but already used by another EU4 mod, and only one file can have a name.
That leaves `winmm`.

The shim exports all ~180 of winmm's entry points as PE **forwarders** to
`winmm_orig.dll`, a verbatim copy of the real one. The loader resolves those
itself, so a `waveOutWrite` call never passes through a single instruction of
this code. There is no wrapper to get wrong, and sound cannot break.

---

## Compatibility

- EU4 **1.37.5.0** on Windows x64. The build gate pins that exact executable;
  re-pin and rebuild after a game patch.
- Single player. Multiplayer and Ironman are untested.
- Works alongside other mods, including other flag mods, as long as two mods do
  not both manage the same country.

---

## Licence

MIT. See [LICENSE](LICENSE).

Note for Paradox modders: do not copy the licence file into a mod you upload to
the Workshop. Paradox's mod rules forbid shipping a licence or copyright claim
with a mod. The source is licensed here; the mod is just a mod.
