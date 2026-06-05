<div align="center">

# 🟡 BOLO — a static recomp of *Bolo Adventures III*

```
   ___  ___  _    ___    _   ___ _   _____ _  _ _____ _   _ ___ ___ ___   ___ ___ ___
  | _ )/ _ \| |  / _ \  /_\ |   \ \ / / __| \| |_   _| | | | _ \ __/ __| |_ _|_ _|_ _|
  | _ \ (_) | |_| (_) |/ _ \| |) \ V /| _|| .` | | | | |_| |   / _|\__ \  | | | | | |
  |___/\___/|____\___//_/ \_\___/ \_/ |___|_|\_| |_|  \___/|_|_\___|___/ |___|___|___|

         Mr. Bolo is trapped in 15 rooms of lasers, crates and water.
              Let's get him out — on hardware that didn't exist in 1993.
```

**Soleau Software, 1993 → native code, today.**
No DOSBox. No emulator. The original 16-bit DOS binary, taken apart and rebuilt.

</div>

---

## What is this?

*Bolo Adventures III* is a 1993 EGA/VGA shareware puzzle game by William Soleau —
a grid-based logic game in the Sokoban lineage. You push boxes, roll balls into
holes, float crates across water, and block lasers to guide **Mr. Bolo** to the
red stairs through 15 increasingly devious rooms. It rewards thinking, not
reflexes, and a certain kind of kid (👋) lost whole afternoons to it.

It's also wonderfully *small* — a single 80 KB executable and a handful of data
files — which makes it a perfect candidate for a **static recompilation**: not
emulating the game, but reverse-engineering the original 8086 machine code and
lifting it back into C that compiles and runs natively on a modern machine.

This repo is that effort, done with the [pcrecomp toolkit](../tools) — the same
pipeline that resurrected *Civilization* (1991) from 16-bit DOS.

## Why bother?

Because emulation runs old games; recompilation *frees* them. The output is
native code you can read, debug, fix, and extend — widescreen, save states, a
level editor, new puzzle packs — things the original could never do. And because
some games deserve to outlive their floppy disks.

> *"We're not reverse engineers. We're software archaeologists. And the dig site
> is every hard drive from the 90s."*

## How it works (the short version)

```
  BOLO3.EXE (PKLITE-packed 16-bit DOS)
        │  ① unpack         tools/unpklite.py
        ▼
  clean MZ image
        │  ② decode         decode16.py   (8086 → instructions)
        │  ③ analyze        analyze.py    (functions, call graph, strings)
        ▼
  symbol table
        │  ④ lift           lift16.py     (8086 → C, against a DOS/VGA runtime)
        ▼
  C source  ──⑤ shim──▶  EGA→SDL2, DOS INTs→runtime  ──⑥ build──▶  native BOLO
```

The `.OV0`–`.OV4` and `.JFT` files alongside the exe look like code overlays but
are actually the game's **data** — EGA tile/sprite graphics, menu text, and the 15
puzzle layouts. Reversing those formats happens in parallel (see
[`docs/FORMATS.md`](docs/FORMATS.md)).

## Status

🚧 **Early days — Phase 1 (unpack & disassemble).**

| Phase | What | State |
|------:|------|-------|
| 0 | Reconnaissance | ✅ done — [`docs/RECON.md`](docs/RECON.md) |
| 1 | Unpack PKLITE + disassemble | 🔨 in progress |
| 2 | Classify CRT vs game logic | ⬜ |
| 3 | Lift 8086 → C | ⬜ |
| 4 | Shim EGA/DOS → SDL2 | ⬜ |
| 5 | Build & debug to playable | ⬜ |
| 6 | Ship native + extras | ⬜ |

See [`docs/PLAN.md`](docs/PLAN.md) for the full roadmap and milestones.

## Repo layout

```
bolo/
  original/     The original 1993 shareware files (the ground truth)
  docs/         RECON.md · PLAN.md · FORMATS.md
  tools/        Project-specific tools (PKLITE unpacker, asset extractors)
  work/         Scratch / regeneratable analysis output (gitignored)
  src/          Recompiled + hand-written runtime code (later phases)
```

## Building

Nothing to build yet — we're still unpacking the patient. Build instructions land
with Phase 5.

## Credits & legal

Original game © 1993 **William Soleau / Soleau Software**, distributed as
shareware. This is a personal preservation / reverse-engineering project of a
long-abandoned title; all reverse-engineered code here is original work produced
by analyzing the freely-distributable shareware binary. The original game files
are included because they're tiny, freely redistributable, and serve as the
reference for the recompilation. If you enjoyed Bolo, go say thanks to Soleau
Software.

Built with the [pcrecomp toolkit](../tools) and far too much nostalgia.
