# The AI shortlist scan's cache misses

Option: `prefetch` (default 1). Exact: it only reads memory.

## What the game does

`0x83f3a0` is the per-club candidate scan behind the AI's transfer shortlists. It runs throughout the game, and it is
the "Creating Shortlists" step of new-game creation, which took 77–80 s of about 155 s with every league. There it is 52%
of samples, inclusive.

The scan walks a list of 12-byte candidate entries. Each starts with a person id. For each candidate it loads the person
record (`0xae23bc + id × 0x6e`) and then, through the person's own id field, its entry in a table at `0xae2d0c` (stride
0x4f, byte `+0xb`): one cache miss, then another that depends on it. The samples piled up on the instructions right
after those two loads (`0x83fa5d` and `0x83fb57`).

## How cmspeed works

The loop head `0x83fa25` jumps to a stub. The stub prefetches the person record 8 candidates ahead and the table entry
3 candidates ahead, by which time the earlier prefetch has brought in the person's id. Both are bounds-checked against
the person count `0xae2364`. The stub then runs the two instructions it replaced and jumps back.

With [par_scan](par-scan.md) on, its own loop-head stub does the same prefetches, so each thread prefetches its own
slice.

## How it was researched

The stack-scanning profiler (see [research methods](research-methods.md)) gave the scan's share. Its per-instruction
samples showed the stalls on the two dependent loads.

## Measured

New-game creation, every league, maximum database, with [fast_hist](fast-hist.md):

| | Creating Shortlists | Whole creation |
|---|---|---|
| `prefetch=0` | 79.1 s | 111.7 s |
| `prefetch=1` | 57.0 s (−28%) | 87.7 s |

On holidays it makes no measurable difference. In four pairs of one-year holidays with the stock database, the means
were 79.0 s and 78.3 s of game-thread CPU, well inside the 4.5 s spread between runs. The scan is only about 7% of a
stock holiday.
