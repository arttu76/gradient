# Copper Gradient utility — Amiga side

![Gradient editor on Workbench, showing the gradient on the desktop behind it](screenshot.png)

A Workbench-friendly utility that drives one or more WB color registers from
a copperlist, painting smooth vertical gradients across the desktop. Built
for Kickstart/Workbench 3.x on Amiga 600 (ECS, 68000); should work on any
3.x Amiga.

The program registers itself with **Commodities Exchange** as the standard
Amiga way to live as a background utility, so you can show/hide its window,
disable/enable its effect, or remove it cleanly through the Exchange app.

---

## 1. Files

| File                              | What it is                              |
| --------------------------------- | --------------------------------------- |
| `Gradient`                        | The executable. Hunk format.            |
| `Gradient.info`                   | Workbench icon, ships with `DONOTWAIT` and `BACKGROUND` tooltypes already set. |
| `ENV:Gradient.prefs`              | Live settings (read at every launch). ~290 bytes binary. |
| `ENVARC:Gradient.prefs`           | Persistent copy. The system copies `ENVARC:` → `ENV:` early in boot, before `User-Startup` runs. |
| Public broker `"Gradient"`        | Created at runtime, listed in Exchange. |

You don't have to create the prefs files; they're written automatically
when you click **Save** in the editor.

---

## 2. Install

Drop both files into `SYS:WBStartup/`:

```
copy Gradient SYS:WBStartup/
copy Gradient.info SYS:WBStartup/
```

(Workbench equivalent: drag the `Gradient` icon onto `WBStartup/`.)

That's the whole install. The icon already has the two tooltypes that
matter:

- **`DONOTWAIT`** — required for anything in `WBStartup/`; tells
  Workbench not to wait for the program to exit before continuing boot.
- **`BACKGROUND`** — Gradient starts silently, without popping the
  editor window. (Without this, the editor would open at every boot.)

Reboot once. Gradient is now running in the background on every boot.

### Configure your gradient

1. Open `SYS:WBStartup/` on Workbench.
2. **Double-click `Gradient`.** Because Gradient is already running,
   the second launch is intercepted via Commodities `UNIQUE` semantics
   and pops the editor window of the running instance — `BACKGROUND`
   is ignored on this path, so you get the GUI even though the icon
   has the tooltype set.
3. Pick a color register, edit stops, etc.
4. Click **Save**. Settings are written to `ENVARC:Gradient.prefs` and
   survive reboot.

To later disable autostart, drag the icon out of `SYS:WBStartup/`. To
remove the running instance without rebooting, use Exchange → Remove
(see [§4](#4-control-via-commodities-exchange)).

### 2.1 Optional: install elsewhere

`WBStartup/` is just a directory — there's no requirement to use it,
and no requirement to keep `Gradient` in `SYS:Tools/Commodities/`
either. If you want the executable on `C:` and autostart driven from
`S:User-Startup`, see [§4.2](#42-cli-alternative-to-wbstartup).

---

## 3. Run the editor (interactive)

From CLI / Shell:

```
Gradient
```

(or double-click an icon if you've made one). The window opens with the
current saved settings — or a default 3-stop Gradient on Color 3 if none
exist yet.

If Gradient is already running in the background and you launch it again,
the *running* instance simply pops its window to the front (Commodities
`UNIQUE` semantics) — you never get two instances fighting over the
copperlist.

### Controls

- **Color**: which Workbench color register (0..3) you're editing. (See
  [§5](#5-color-depth--the-4-register-limit) for why only 0..3.)
- **Enabled**: turn the copper override on/off for this register.
- **Dither**: ordered vertical dithering; hides 4-bit-per-channel
  step boundaries. On by default.
- **Reverse**: flip the Gradient (Y=100% paints at the top).
- **Stop buttons** (`R/G/B Y%`): one per Gradient stop. The currently
  selected one is prefixed `*`. Up to 8 stops.
- **Y / R / G / B sliders**: edit the selected stop's position and color.
  Updates the copperlist live as you drag.
- **Add before / Add after / Delete this**: stop-list management. Disabled
  when not applicable.
- **Save / Use / Cancel** (the standard Amiga prefs trio):
  - **Save** — write `ENV:Gradient.prefs` *and* `ENVARC:Gradient.prefs`,
    close the editor window. The daemon keeps running with the new
    settings.
  - **Use** — write `ENV:` only (live, lost on reboot), close the editor.
  - **Cancel** — discard your edits, revert the copperlist to what it
    was when the editor opened, close the editor. Window close gadget
    is equivalent to Cancel.

In all three cases the *background broker* keeps running. Closing the
editor doesn't quit the program — see the next section for that.

---

## 4. Control via Commodities Exchange

Run `Tools/Commodities/Exchange` (or wherever your WB has the Exchange
program). You'll see `Gradient` listed alongside any other commodities
you have running (Blanker, AutoPoint, etc.). The standard buttons:

| Button   | What it does                                                |
| -------- | ----------------------------------------------------------- |
| **Show** | Pop the editor window so you can change the Gradient.       |
| **Hide** | Close the editor window (without saving — like Cancel).     |
| **Enable** | Re-attach the copperlist; Gradient appears on Workbench.  |
| **Disable** | Detach the copperlist; Workbench's normal palette returns (broker stays). |
| **Remove** | Quit Gradient entirely. The broker is removed from Exchange. |

### 4.1 Custom prefs file

```
Gradient LOAD=DH0:my-presets/blue-Gradient.prefs
Gradient BACKGROUND LOAD=DH0:my-presets/blue-Gradient.prefs
```

Useful for keeping multiple presets and switching by command line. As a
Tool Type on a `WBStartup/Gradient` icon, the same `LOAD=…` line picks
the preset at boot.

### 4.2 CLI alternative to WBStartup

If you'd rather start it from `S:User-Startup` than `WBStartup/`, add:

```
Run >NIL: C:Gradient BACKGROUND
```

`Run >NIL:` detaches it from the Shell; `BACKGROUND` skips opening the
editor window. Adjust the path if `Gradient` lives somewhere other than
`C:`.

---

## 5. Color depth & the 4-register limit

Gradient always animates Workbench color registers **0..3**, regardless of
how many colors your Workbench screen is set to.

Why those four: on every standard Workbench, registers 0..3 are the
chrome — background, text, dark window border, light window edge — and
nothing else owns them. Registers 4+ on a deeper Workbench belong to app
icon palettes and GUI toolkits (MUI, ReAction skins); driving them from a
copperlist would smear unrelated UI mid-screen.

What happens at each depth:

- **2-color WB**: only registers 0..1 are displayed. Any gradient you
  configure on registers 2..3 is still emitted by the copper but isn't
  visible. Harmless.
- **4-color WB**: the design target — all four registers are visible.
- **8 / 16 / 32-color WB, or AGA 256**: the same four chrome registers
  are animated; the rest stay flat at their normal Workbench colors.

Hardware footnote: the Copper itself can write all 32 ECS color registers
(or 256 on AGA). The 4-register cap is a deliberate UX choice, not a
copperlist limitation. RGB precision is 4-bit-per-channel (ECS 12-bit
`$0RGB` format, see `pack_dither()` in `gradient.c`) on every platform —
AGA users won't see extra color depth in gradients.

---

## 6. Coexistence with games and demos (WHDLoad etc.)

Short answer: **fine — the broker is fully passive while a game runs and
your Gradient resumes automatically when the game exits.**

Long answer:
- WHDLoad slaves and most demos take over the chipset directly
  (`LoadView(NULL)`, write COP1LC, etc.). While they run, AmigaOS
  graphics is dormant — it isn't running our copperlist; the game's
  copper drives the display. Nothing to conflict with.
- When the game exits cleanly it restores the OS view via
  `LoadView(GfxBase->ActiView)` + `RethinkDisplay()`; graphics.library
  re-merges its copperlists, sees our `UCopList` still attached, and
  the Gradient is back. We do nothing.
- If the game does a hard reboot (some old slaves do), the daemon dies
  with everything else — `WBStartup/` re-launches it on the next boot.
- Our task runs at priority **−1**, so even when the broker is awake it
  never competes with foreground apps for CPU.

---

## 7. Build (host side, Linux + vbcc)

If the cross-toolchain is already installed under `./toolchain/`:

```
. ./env.sh
make Gradient
```

Produces `Gradient` (Hunk executable). `file Gradient` should report
`AmigaOS loadseg()ble executable/binary`. Copy it to the Amiga.

If you're starting from a fresh clone (`toolchain/` is gitignored), see
[BUILDING.md](BUILDING.md) for the one-time toolchain install
(vbcc + vasm + vlink + NDK 3.2 — about 5 minutes, ~250 MB on disk).

---

## 8. Troubleshooting

- **The editor window doesn't appear when I run `Gradient` from Shell.** —
  Look at Workbench. The Gradient should be visible. The window may have
  opened behind your Shell window — drag the Shell aside, or use
  `LeftAmiga + M` to bring Workbench to front.
- **Exchange shows Gradient but Show does nothing.** — On some 3.x
  systems Exchange sends `APPEAR` only when the broker has `COF_SHOW_HIDE`
  set; we set it. If the window still doesn't appear, check that the
  Workbench public screen is unlocked (no full-screen app holding it).
- **`Version Gradient` says "no version info available".** — Try
  `Version FULL Gradient` — some Versions are picky about the path.
- **The window doesn't fit on screen.** — The editor needs roughly 250
  lines of Workbench height. If you're in NTSC (200 lines) the bottom
  may be clipped. Try Prefs → ScreenMode → PAL or enable overscan.
