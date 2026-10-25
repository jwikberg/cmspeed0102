# Player histories in new-game creation

Option: `fast_hist` (default 1). Exact: the same array as the stock sort, record for record.

## What the game does

Creating a game advances a progress bar (`0x802f80`) once per step. Timing each step with every league selected showed
step 13, "player histories" (`0x7bfd50`), at 44–47 s of about 155 s.

That step builds an array of 6-byte records: a dword person id, a signed byte and a value byte. For each player it looks
a record up with `bsearch` (`0x946464`). On a miss it appends one with `0x7c6a30(…, sort=1)`, which then re-sorts the
whole array with `qsort` (`0x945333`, called at `0x7c6bda`). With every league that is 9 452 full sorts of a growing
array, and the comparator `0x7c6d40` alone was 20% of all samples.

## How cmspeed works

The comparator orders by id, then by the signed byte, except that a byte of −1 equals any byte. Without −1 bytes it is
a strict order. When the records before the new one are strictly increasing and none equals it, the sorted result is
unique: whatever `qsort` does, it must produce the array with the new record moved into place.

cmspeed points the call at `0x7c6bda` at its own function. In that case it finds the place with a binary search and
moves the tail up with `memmove`. In every other case (a −1 byte, an unsorted prefix, an equal record, another
comparator) it calls the original `qsort`.

## How it was checked

- A verify build sorted a copy with the original `qsort` on every fast insert and compared the arrays: no differences,
  with every league and with England alone.
- Counters showed that all 9 452 inserts (every league) and all 5 863 (England) took the fast path.

## Measured

Every league, maximum database, one run each:

| | Player histories | Whole creation |
|---|---|---|
| `fast_hist=0` | 46.9 s | 157.3 s |
| `fast_hist=1` | 3.2 s | 111.7 s |
