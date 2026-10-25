# Research methods

This page describes the tools and test setup behind the other docs. All addresses are virtual addresses in
`cm0102.exe` 3.9.68. The exe has no relocation table and always loads at `0x400000`, so the addresses never change.

## The target

| | |
|---|---|
| File | `cm0102.exe` 3.9.68, 7 192 576 bytes |
| SHA-256 | `0c78e7da6d0bcb4603fa179449bbe6d1b6b5e1c627a22c445d14d489bd9bff86` |
| Build | MSVC6, statically linked single-threaded C runtime, PE timestamp `0x3ddcdba5` (2002-11-21) |
| Rendering | DirectDraw at 800×600, with a hardware mouse cursor. `cm0102_GDI.exe` draws with GDI instead, has different addresses, and cmspeed leaves it alone |
| Threads | One. The whole game runs on its main thread |

cmspeed checks the image base and the PE timestamp before it patches anything, and the original bytes at every patch
site.

## Static analysis

- **Decompiles.** Headless Ghidra analysed the exe once into a saved project, which later queries opened read-only:
  decompile a function, list references to an address, list a function's callers.
- **Disassembly.** GNU `objdump` gave the full disassembly. `tools/tls_sites.py` uses it to find every instruction that
  addresses the globals made per-thread.
- **Names.** The exe keeps few symbols, but its assert messages name source files and lines (`comp_stats.cpp 2465`),
  and progress texts name the steps of new-game creation.
- **Reachability.** Walking direct calls from the match simulation and from the shortlist scan listed the functions
  each reaches, and their writes to fixed addresses.

## Runtime tools

These tools lived in a research build of cmspeed. The public DLL doesn't include them.

- **A sampling profiler.** A thread suspends the game thread up to 1 000 times a second and records `eip` plus every
  return address into the exe it finds on the stack. MSVC6 omits frame pointers, so the profiler scans the stack and
  counts a value only if a `call` instruction ends right before it. It also records the game thread's CPU time and game
  dates, and markers such as the steps of new-game creation, so a profile can be split by phase.
  Linux `perf`, attached to the Wine process with user-space call chains, gave the same ranking.
- **Call timers.** A stub around every direct call to chosen functions logged their inclusive time.
- **A write watch.** For the length of one match simulation or one shortlist scan, the watch made the exe's data and all
  heap memory read-only. It logged every write outside the watched code's own allocations, with the writing
  instruction. An option recorded every write instead of only the first per page.
- **A refresh probe.** During shortlist scans it recorded each daily refresh with its person and the candidate being
  scored, so slicing could be simulated offline.
- **A list probe.** It recorded, per split scan, the game lists that several slices advanced.
- **A scan verify mode.** Each split scan also ran serially on the game thread with the same random streams, and the
  picks were compared.
- **Match fingerprints and pinned seeds.** One line per background match, with its score and the random generators'
  state. With the generators reseeded per fixture and a virtual clock, two runs gave identical fingerprints, so a change
  could be checked match by match.
- **Heap padding.** Adding N bytes to every allocation moves every heap block without changing contents. It showed that
  match results depend on addresses, and its spread across several paddings is the noise floor for comparing runs.
- **An autopilot.** It loads a save with the exe's own `-load` switch, goes on holiday until a date, then returns and
  quits; or it creates a new game with chosen leagues through the exe's own automatic setup. That made the benchmarks
  repeatable with no input.

## Exactness tests

- `fast_ftol`: `tests/ftoltest.c` copies the original function out of the exe and compares it with the replacement on
  200 million inputs under every rounding mode, with a deliberately wrong control (see [fast-ftol](fast-ftol.md)).
- The date methods: an in-process test compared the C versions with the game's own on 1 000 000 random cases each.
- `fast_hist`: a verify mode sorted a copy with the original `qsort` on every insert.
- `par_scan`: the verify mode above.

## Test setup

- Automated runs used Proton-CachyOS (wine-11.0) on Xvfb, with cnc-ddraw's OpenGL renderer on Mesa llvmpipe, and
  again with Wine's own DirectDraw. Real input, where needed, went through XTest.
- The host was an AMD Ryzen AI 9 365, 20 threads.
- The main save is a 16 MB save from 14 July 2001. Holiday benchmarks ran from there, for 37 days, three months or a
  year.
- A second install used a modified 3.9.68 exe that loads every player in the database, which makes the AI's shortlist
  scans several times larger. The large-database numbers in these docs come from it.

Single runs vary by a few percent, because the game seeds its randomness from the clock and its results depend on heap
addresses.
