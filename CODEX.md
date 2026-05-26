# ARM64 Jak 1 Boot Baseline

This branch is the Apple Silicon ARM64 experimental port baseline.

Current target state:

- Jak 1 boots natively on Apple Silicon without Rosetta.
- Sony Presents screen appears.
- Naughty Dog intro and title/start-screen audio continue after Sony.
- The process must not crash immediately after Sony.
- Post-Sony video may still be black or broken. That renderer bug is separate.

Known bad state:

- Commit `7b226be70` regressed post-Sony boot on fresh clones.
- It added late graphics, trampoline, MIPS2C, and renderer experiments.
- Fresh clones of that commit may crash directly after Sony.

Baseline restore:

- The tested stable behavior matches the tree state before `7b226be70`,
  around `d4bf68543`.
- The restore commit on `experimental` reverts those late experiments so all
  Macs cloning or pulling `experimental` start from the same boot baseline.

## Required Host

Use native ARM64 tools:

```sh
rtk uname -m
rtk file build/game/gk
```

Expected:

```text
arm64
build/game/gk: Mach-O 64-bit executable arm64
```

If this says `x86_64`, the build/runtime is wrong for this branch.

## ISO / Data Setup

For Jak 1 PAL / Europe:

```sh
rtk task set-game-jak1
rtk task set-decomp-pal
```

The game data must be extracted into:

```text
iso_data/jak1_pal/
```

A `.iso` file alone is not enough for `-fakeiso` boot. The extracted disc
contents must exist in `iso_data/jak1_pal`.

Then extract/build project data:

```sh
rtk task extract
```

## C++ Build

Use Ninja on Apple Silicon:

```sh
rtk cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
rtk cmake --build build --target goalc gk -j 8
```

## GOAL Build

Start the compiler:

```sh
rtk ./build/goalc/goalc --game jak1
```

In the GOAL REPL, build everything:

```lisp
(mi)
```

Equivalent explicit form:

```lisp
(build-kernel)
(build-game)
```

## Boot

Use `-fakeiso` for this baseline:

```sh
rtk ./build/game/gk -v --game jak1 -- -boot -fakeiso -debug
```

Do not use `--no-display` for the visual/audio baseline check.

## Expected Log Markers

Good baseline markers:

```text
Got correct kernel version 2.0
begin load title-vis [tit.DGO]
GAMEPLAY: enter title
Load music village1
Kernel dispatch #100
Kernel dispatch #1000
```

The compiled version should be the current restore commit on `experimental`,
or a local baseline checkout such as `d4bf68543`. It must not be the known bad
`7b226be70` unless the restore patch is applied locally.

## SIGBUS Note

`[SIGBUS] count=...` is not automatically a crash in this port. The current
runtime can recover from many SIGBUS faults and continue dispatching frames.

Real regression:

- process exits or aborts directly after Sony Presents
- no Naughty Dog / title audio continues
- no `Kernel dispatch #100` or later markers appear

Known remaining bug:

- Post-Sony renderer output can be black or distorted.
- Fix that separately after preserving this boot/audio baseline.
