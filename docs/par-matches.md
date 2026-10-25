# Background matches on threads

Option: `threads` (default `auto`: the logical processors, at most 8; `0` = off). Fixtures are batched `threads` at a
time and simulated on `threads − 1` worker threads plus the game thread. Equivalent results, not identical.

## What the game does

Match days are about 69% of a stock holiday's game-thread time. The day's fixture loop `0x6b4360` plays each background
fixture in turn:

1. squad setup `0x90d2f0`;
2. a new 0xf63b-byte match object, set up by `0x6b7e50`, `0x6b66a0` and `0x6b7f20` (which includes team selection);
3. the simulation: `0x6b98c0` called until the state is 2 or 8;
4. recording into a new stats object, `0x7293f0`;
5. delete the match, then release the squads with `0x90d6b0`.

The database is updated from the stats objects only after the whole list is done.

## How cmspeed works

**Batching.** The fixture loop's body (`0x6b4a36`) only sets a fixture up (steps 1–2) and queues it. The queue is
drained when it holds `threads` fixtures, and before any other code can see queued fixtures: at the loop's end
(`0x6b4e0b`), before its break for a match a human must watch (`0x6b4df5`), and before it handles a click on holiday
(`0x6b496b`). Draining simulates the queued fixtures on the worker threads and the game thread, each from the random
generator state its own setup left. Then, in fixture order, it records each one, deletes its match and releases its
squads. The per-fixture progress text isn't shown.

**Thread safety**, from write watches around simulations and a static scan of the exe:

| Shared state | Treatment |
|---|---|
| Squad setup puts generated "virtual" players for short squads in fixed person-id ranges per slot (`0x90e760`) | Each fixture in a batch uses its own slot: −2, then the human slots not in the game (so at most 16 per batch) |
| The random generators' state, a scratch dword, lookup caches and a sort context: 9 globals | Per thread: the 63 instructions that address them jump to thunks that run the same instruction with an `fs:` override onto a thread-local slot (`src/tls.c`, sites from `tools/tls_sites.py`) |
| The game's date methods keep their results in static scratch dates | Replaced by exact C versions that use locals (`src/dates.h`). An in-process test compared them with the originals in 1 000 000 random cases each: 0 differences |
| The C runtime heap, created `HEAP_NO_SERIALIZE` by the single-threaded runtime | Serialised by removing the instruction that sets the flag (`0x948af8`) |
| Competition statistics updated at the end of a match (`0x896290`) | Deferred, and replayed in fixture order when the fixture is recorded |
| The injury list (`0x62be00`) | Locked |
| Uninitialised stack and heap reads in the engine | Each simulation step starts on 64 KB of zeroed stack, and every runtime allocation is zeroed (`HEAP_ZERO_MEMORY`) |

Workers get the game thread's x87 control word and MXCSR, and 8 MB stacks.

**Windows 95 mode.** Under Windows versions before NT 5, including Wine set to Windows 95, the runtime's `malloc`
picks its own small-block heap, which has no lock. Worker threads corrupted it. cmspeed sets the runtime's own
override `__MSVCRT_HEAP_SELECT=__GLOBAL_HEAP_SELECTED,1` before the exe starts, which selects the Windows heap on any
version. If the heap still isn't the Windows heap, batches run on the game thread only. This applies to
[par_scan](par-scan.md) too.

## Why the results differ

With the exe's `-seed N` switch the game is deterministic, and with the random generators reseeded per fixture
(research build), two runs gave identical results for all 503 matches of 30 days. That made it possible to compare
modes match by match.

Batched and serial runs still differed, and the cause turned out to be the stock game's own: **its results depend on
heap addresses.** Adding 16 bytes to every allocation, which moves blocks without changing any contents, changed 474 of
503 results in a plain serial run. Three ways the engine turns addresses into results were traced:

- uninitialised stack, where the heap functions leave address-dependent values;
- the order of lineup slots in team selection, which depends on addresses and feeds a `random(n)` range;
- referee assignment.

Any difference in heap history changes results, including which screens you visited before a holiday. Simulating a
batch changes the heap's history. With one particular layout, serial, reversed-order and parallel runs gave identical
results for all 503 fixtures, so batching and threads add no dependency of their own.

## Measured

The measurements below were taken when the scan (`par_scan`) and the matches (`par_matches`, with `par_threads`
workers) were separate settings. `threads=8` gives `par_scan=8`, `par_matches=8` and `par_threads=7`.

One-year holidays from July 2001, stock database:

| | Wall | Game-thread CPU | Goals per match | Home / draw / away |
|---|---|---|---|---|
| `par_matches=0`, three runs | 85.6 s | 75.0–75.5 s | 2.388–2.421 | 43.6–44.3% / 25.2–25.5% / 30.5–31.2% |
| `par_matches=8`, `par_threads=7` | 57.5 s (−33%) | 43.6 s (−42%) | 2.424 | 44.9% / 25.9% / 29.2% |

No crashes in any run (4 800+ matches each). Across four heap layouts the home-win share moved by 1.7 points in stock
runs alone, and the parallel runs fell inside or next to that range, once above and once below the stock mean: noise,
not a bias. A larger batch didn't help: 16 was no faster than 8.

With a modified exe that loads every player, a 95-day holiday took 164 s without and 150 s with `par_matches=8` (−9%),
with about 13 500 batched fixtures and no faults. There the AI's shortlist scan dominates instead; see
[par_scan](par-scan.md).

An automatic setup that creates an England game and goes straight on holiday ran 17 months (to December 2002, through
the end of a season) in about two minutes with `par_matches=8` and `par_scan=8`, with no faults or error messages.
With Wine's own DirectDraw instead of cnc-ddraw, the same setup reached April 2003 in the same time, also without
faults.

## Limits

- Your own matches, cup ties, saving and loading in the middle of a holiday haven't had a dedicated test. If the game
  crashes during processing, set `threads=0`.
- Team selection and match init still run on the game thread, one fixture at a time. Match init writes shared
  competition data, so it isn't safe on threads as it is.
