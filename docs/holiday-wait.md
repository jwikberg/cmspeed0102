# The holiday pause

Option: `holiday_wait` (default 0; stock 2000). Changes what you see, not what the game computes.

## What the game does

The stock game takes about 4.1 s per holiday day, measured by reading the in-game date from memory. The simulation
itself used only 15–20% of that time. The rest went to the UI loop: 72% of the game's own samples were under the UI
frame `0x55f620`, and Wine system calls from polling `clock()` and `PeekMessage` added about 23% of all samples.

The pacing is in the screen handler `0x7615d0`, which 104 screens share:

```
0x762231  call 0x5fd210        ; the human manager's holiday flag (+0x9a01 in its record)
0x762245  call 0x5fdc30        ; the manager's timestamp (+0x9a65)
0x76224c  add  esi,0x7d0       ; +2000 ms
0x762252  call 0x90c510        ; clock()
0x762259  jl   0x762389        ; not yet: run another UI frame
```

So after processing a day, the game shows it until 2 seconds have passed since the manager's timestamp, then continues.

## How cmspeed works

It rewrites the `0x7d0` immediate at `0x76224c` with `holiday_wait`. Values outside 0–2000 fall back to 2000.

## How it was researched

- A `perf` profile of a stock holiday, attached to the Wine process, showed the time in the UI loop rather than in the
  simulation (see [research methods](research-methods.md)).
- The decompiled screen handler showed the clock comparison above.
- A second gate in the same function, `time() >= [0xb74310] + 5` at `0x7622fd`, looked like another pacer. It is
  guarded by `0xdd7118`, which `0x81b890` sets once a game has started, not by the holiday flag. Patching it to 0 didn't
  change the holiday rate, so cmspeed leaves it alone.

## Measured

Same save, the same 37 holiday days (13 Jul – 19 Aug 2001):

| | Total | Mean per day | Median per day |
|---|---|---|---|
| Stock | about 152 s | about 4 100 ms | |
| `holiday_wait=0` | 18.6 s | 502 ms | 378 ms |
| … plus [fast_ftol](fast-ftol.md) | 15.8 s | 427 ms | 341 ms |

The game seeds its randomness from the clock, so two runs never process exactly the same events. Expect a few percent
of noise between runs.
