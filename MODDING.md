# Adding flags, day to day

**Nothing here needs a compiler, and nothing here needs me.** Conditions are
written in ordinary EU4 script and evaluated by EU4 itself. The DLL only reads
one line out of `game.log` and swaps a texture.

The DLL gets rebuilt only when the engine hooks change — a once-per-game-patch
event, not a per-flag one.

---

## The loop

```
1. drop  FRA_white.tga  into  mod\cpbm_identity\gfx\flags\
2. add an if/else block to
   mod\cpbm_identity\common\scripted_effects\zz_cpbm_identity_effects.txt
3. launch
```

That's it. No registration, no country tag, no atlas bookkeeping.

---

## How it actually works

EU4's `log = "..."` script effect writes a line to `logs\game.log`. The DLL
tails that file and acts on lines of one shape:

```
dynamic_country_flag <TAG> <flagname>     show <flagname>.tga on that country
dynamic_country_flag <TAG> -              put that country's own flag back
```

**The DLL never sees your conditions.** It reads that line and nothing else. So
any trigger EU4 supports works, with no support needed on this side:

```
if = {
    limit = { tag = FRA }
    if = {
        limit = {
            has_reform = communist_dictatorship
            owns_core_province = 183
            NOT = { exists = BUR }
            OR = { dynasty = "Bonaparte" is_year = 1789 }
        }
        log = "dynamic_country_flag FRA FRA_communist"
    }
    else = { log = "dynamic_country_flag FRA -" }
}
```

Re-checked every month via `on_monthly_pulse`, and once at `on_startup`.

---

## Two rules that matter

**State the whole truth every pulse.** Always pair the `if` with an `else` that
logs `-`. The DLL ignores a line repeating what is already shown, so restating
it monthly is free.

> **Do not guard rules with country flags to "only log on change."** That was
> the first design and it is broken: country flags are saved with the game, so
> after a reload the guard is already set, the line never fires again, and the
> DLL — which starts each launch knowing nothing — leaves the country on the
> wrong flag. Stating it unconditionally is simpler *and* reload-proof.

**Order decides ties.** If two rules could apply to one country, the last line
logged in a pulse wins. Put the most specific case last.

---

## Flag art

A normal EU4 flag `.tga`: 128×128, 24- or 32-bit, RLE or uncompressed — copying
an existing one from `CPBM\gfx\flags` and repainting it always works.

**Naming is yours.** `FRA_communist`, `FRA_angevin`, `FRA_savoyunion`. The name
is just the filename and the string in your `log` line. It does not have to be a
tag, and it must not collide with one.

The art never enters EU4's flag atlas on disk. The DLL loads the `.tga` at
runtime and writes its pixels into the country's own atlas slot. That is why no
registration is needed — and why the earlier rebel-type trick, which corrupted
every flag after the one it added, is gone.

---

## Checking your work

`<game folder>\cpbm-identity.log` records what happened:

```
script: 3788 flag slots mapped from the atlas manifest
script: watching ...\logs\game.log for dynamic_country_flag lines
script: FRA -> FRA_white (slot 121)
d3d9: slot 121 <- FRA_white : OK
```

- **No `script:` lines at all** — the `cpbm.D3D9` marker is missing from the
  game folder. That marker is the on/off switch for the whole graphics side.
- **`unknown tag 'XXX'`** — that tag has no atlas slot; check the spelling.
- **`FRA_white.tga would not load`** — wrong path, wrong size, or a TGA variant
  the loader does not handle.
- **Nothing logged for your rule** — EU4 never reached your `log`. Check the
  trigger in game first; the mod is not involved yet at that point.

---

## Turning it off

Delete `cpbm.D3D9` from the game folder. The DLL stays loaded, reports itself
dormant for the graphics side, and touches no texture. Disabling the mod in the
launcher switches off everything.
