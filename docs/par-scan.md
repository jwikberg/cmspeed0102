# The AI shortlist scan on threads

Option: `threads` (default `auto`: the logical processors, at most 8; `0` = off). The scan is split over `threads`
worker threads while the game thread waits. Equivalent, not identical: about 1 AI shortlist decision in 2 000 comes
out differently.

## What the game does

The AI's per-club candidate scan `0x83f3a0` (see [prefetch](prefetch.md)) scores candidates for a club's needs. It loops
over sublists, and within each over a list of candidates (loop head `0x83fa25`, exit `0x840a99`). It never exits early.
After the loop, the pick goes into the club's shortlist (`0x8423f0`).

How much it matters depends on the database:

- creating a new game with every league: 52% of samples;
- a holiday with the stock database: about 7%. Scans there have about 2 400 candidates;
- a holiday with a modified exe that loads every player in the database: 44%. Median 2 705 candidates per scan, 90th
  percentile 19 344, maximum 96 629.

## How cmspeed works

**Splitting.** On the game thread's first arrival at the loop head of a sublist with at least 10 000 candidates,
cmspeed splits that loop into K contiguous slices. Each worker copies the 0x114-byte stack frame below the loop head
onto its own stack, with the same alignment, sets its start index (`+0x40`) and end (`+0x8c`), loads the game thread's
registers and jumps into the loop head. A stub at the loop exit sends the worker back. The game thread waits meanwhile,
and arguments are read through `ebp` from its frame.

**Merging.** Across candidates the loop carries only the pick (`+0x88`) and the running best value (`+0x58`). A candidate
becomes the pick only if its value, after `random(17)+92` percent noise, is strictly greater than the best, so the first
maximum wins. The merge goes through the slices in order and takes a slice's pick when its best is strictly higher than
the best so far.

**Random numbers.** During a split each candidate draws from its own stream, seeded at its loop head from the scan's
seed (one draw of the game's generator), its sublist and its index. So its draws don't depend on how the loop was sliced.

**Shared state**, from a write watch around the scan (see [research methods](research-methods.md)):

| Shared state | Treatment |
|---|---|
| The daily per-person refresh `0x62c680`: the first lookup of a person each game day catches up its entry with random draws, sometimes with news and file I/O | One lock around the whole call (about 350 calls per scan). It draws from a stream of (person, date) and restores the thread's generator afterwards, so the result doesn't depend on which candidate triggers it |
| Lazily sorted lookup arrays in `0x4a4850`: byte `+0x18` is the array's sort key, set before a `qsort` that reads it | A reader–writer lock while workers run: exclusive for a call that will re-sort |
| The transfer manager's embedded list iterator, reset and walked by `0x8ce990` | A per-thread copy |
| Two list searches that use the list's own cursor (`0x69fc60`, `0x4cca30`); two threads cut each other's walk short | Replaced by the same searches with a local cursor |
| The "club is watching your player" news `0x587220` | Recorded on workers; the merge posts the ones a serial scan would have posted |
| The random number generators, scratch dates, lookup caches and the C runtime heap | Per thread or serialised, shared with [par_matches](par-matches.md) |

On a worker, the game's error message boxes are logged and not shown, since the game thread is waiting and nobody could
click them.

## Why the results differ

A candidate's value depends on the best found before it: the evaluation `0x840d80` passes a value derived from the
running best into the player valuation `0x6e6340`. A slice starts from a lower best, so a few values come out higher than
in a serial scan. Making that exact would mean re-evaluating each slice's interim picks with the true serial best on the
game thread. A different pick follows from the game's own scoring, always for a candidate it rated at least as highly,
so cmspeed accepts it.

## How it was researched and checked

- **Write watches** around the first 217 scans, then around 40 scans recording every write, listed everything the
  scan's 37 callees write outside the scan's own memory. That gave the table above.
- **A probe** recorded every refresh call with its person and candidate over 3 000 scans. In 8 slices, 35 870 persons
  would have been refreshed by more than one slice, in 7% of scans: hence the lock and the per-person stream.
- **A list probe** recorded, per split, the lists that several slices advanced. It found the two list searches; after
  their replacement, 0 shared lists in 4 100 splits.
- **A verify mode** reran each split loop serially on the game thread with the same streams and compared. Over 4 100
  split loops (80 million candidates) with no error boxes, 2 picks and 1 best value differed, for the reason above.

## Measured

The measurements below were taken when the scan (`par_scan`) and the matches (`par_matches`, with `par_threads`
workers) were separate settings. `threads=8` gives `par_scan=8`, `par_matches=8` and `par_threads=7`.

New-game creation, every league, maximum database, with [fast_hist](fast-hist.md) and [prefetch](prefetch.md):

| | Creating Shortlists | Whole creation |
|---|---|---|
| `par_scan=0` | 57.0 s | 87.7 s |
| `par_scan=8` | 26.6 s | 57.0 s (−35%) |

With the modified exe that loads every player: 185.0 s → 117.8 s (−36%).

95-day holiday with the modified exe, both runs with `par_matches=8`, `par_threads=7`:

| | Wall | Game-thread CPU |
|---|---|---|
| `par_scan=0` | 150–151 s | 144 s |
| `par_scan=8` | 112–114 s (−25%) | 71 s |

37 000 loops were split there (704 million candidates), with no faults or error boxes.

With the stock database, one-year holidays took 52–53 s either way (with `par_matches=8`). Most stock scans are below 10 000 candidates and run as
before.

Two costs were found and removed. The loop-head stub first called into C for every candidate; now it does so only at a
loop's first candidate or during a split, and otherwise prefetches inline. Splitting loops from 2 000 candidates made the
game thread wait on workers for little work; the threshold is now 10 000.
