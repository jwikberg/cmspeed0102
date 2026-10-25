```
DISCLAIMER: This project was researched and developed through Claude Code.
```

# cmspeed

cmspeed is a drop-in `dsound.dll` for Championship Manager 01/02, version 3.9.68. It adds:

- **Faster holidays.** A holiday day takes well under half a second instead of about 4 s. Most of the stock time was a fixed
  2-second pause per day, plus a slow float-to-int conversion in the match engine.
- **Faster new games.** Creating a game with every league selected takes about 57 s instead of about 157 s.
- **Extras.** The startup logos are gone, and the mouse's back/forward buttons work.

It changes nothing on disk. The game's exe and saves stay as they are, and deleting `dsound.dll` restores the stock
game.

## Requirements

- **Championship Manager 01/02 patched to 3.9.68**, the DirectDraw `cm0102.exe`. cmspeed checks the exe when the game
  starts. With any other version, or with `cm0102_GDI.exe`, it only passes sound through and changes nothing.
  Modified 3.9.68 exes usually work: cmspeed compares the original bytes at every place it patches and skips any that
  differ.
- **Windows 10 or later, or Wine/Proton.** The DLL uses the Universal C Runtime, which Windows 10 includes. Windows 7
  and 8 need the UCRT update.
- **A CPU with SSE3** for `fast_ftol`. Every x86 CPU from the last 15 years has it; without it cmspeed leaves that
  option off. `threads` needs at least 2 logical processors to do anything.

cmspeed was developed and tested under Wine/Proton on Linux. Nobody has tested it on Windows yet.

## Install

1. Copy `dsound.dll` and `cmspeed.ini` into the game folder, next to `cm0102.exe`.
2. On Wine or Proton, tell Wine to load the game's copy of `dsound.dll` instead of its own:
   - Steam, with the game added as a non-Steam game: set the launch options to
     `WINEDLLOVERRIDES="dsound=n,b" %command%`.
   - Lutris, Faugus, Bottles and similar launchers: add the environment variable `WINEDLLOVERRIDES=dsound=n,b`.
   - Plain Wine: run `WINEDLLOVERRIDES="dsound=n,b" wine cm0102.exe`.

   If you also use cnc-ddraw (a `ddraw.dll` in the game folder), combine them: `WINEDLLOVERRIDES="ddraw=n,b;dsound=n,b"`.
3. Start the game.

To uninstall, delete `dsound.dll` and `cmspeed.ini`.

## Options

Every option goes in `cmspeed.ini`, under `[cmspeed]`, and you can turn each one off on its own. The defaults below
apply when the ini is missing or leaves an option out. An environment variable `CMSPEED_<OPTION>`, such as
`CMSPEED_THREADS=0`, overrides the ini.

### Holiday

| Option | Default | What it does | Docs |
|---|---|---|---|
| `holiday_wait` | `0` | Milliseconds each holiday day stays on screen before the game continues. Stock is 2000, which is most of a holiday's time. Holidays become about 8× faster | [holiday-wait](docs/holiday-wait.md) |

### Exact speed-ups

These options give the same results as the stock game. They replace slow code with faster code that computes the same
thing, or they change only what the CPU does while it waits.

| Option | Default | What it does | Docs |
|---|---|---|---|
| `fast_ftol` | `1` | The game's float-to-int conversion uses one SSE3 instruction instead of reprogramming the FPU twice per call. Holidays take about 15% less time | [fast-ftol](docs/fast-ftol.md) |
| `fast_hist` | `1` | Creating a new game inserts each player history record in place instead of re-sorting the whole list. That step drops from 47 s to 3 s with every league | [fast-hist](docs/fast-hist.md) |
| `prefetch` | `1` | Loads the next candidates' data early in the AI's shortlist scan. New-game creation's "Creating Shortlists" step takes about 28% less time | [prefetch](docs/prefetch.md) |

### Parallel processing

Results with these options are equivalent to the stock game's, not identical. The game already gives different results
from run to run, because it seeds its random numbers from the clock and its match results depend on memory addresses.

| Option | Default | What it does | Docs |
|---|---|---|---|
| `threads` | `auto` | How many threads cmspeed uses for the two jobs below. `auto` uses as many as the CPU has logical processors, up to 8. Set a number, up to 16, to choose yourself. `0` turns both off | [par-scan](docs/par-scan.md), [par-matches](docs/par-matches.md) |

The two jobs run at different moments, never at once, so one thread count suits both:

- **The AI's shortlist scan** is split over `threads` threads. New games with every league take about 35% less time.
  Holidays with a much larger database took 25% less; with the stock database most scans are too small to split.
- **Background matches** are simulated `threads` at a time. A one-year holiday takes about 33% less time with 8.

More threads than 8 gained nothing in testing. On a CPU with fewer logical processors, `auto` already uses them all;
set a lower number if you want to leave some free for other programs.

The scan makes the AI pick a different transfer target in about 1 decision in 2 000, always one the game's own scoring
rated at least as highly. Background matches use the stock engine, but cmspeed sets up a whole batch of fixtures before
it records the first. Parallel matches are newer and haven't been tested through a whole season of real play yet; see
[par-matches](docs/par-matches.md). If you'd rather keep the stock behaviour, set `threads=0`.

### Convenience

| Option | Default | What it does | Docs |
|---|---|---|---|
| `skip_splash` | `1` | Skips the startup logos | [skip-splash](docs/skip-splash.md) |
| `mouse_nav` | `1` | The mouse's back/forward side buttons and the keyboard's browser keys go back and forward through screens, like the ← → arrows in the sidebar | [mouse-nav](docs/mouse-nav.md) |

### Troubleshooting

| Option | Default | What it does |
|---|---|---|
| `log` | `0` | Writes `cmspeed.log` next to the exe, new each time the game starts: the options in effect, which patches were applied or skipped, the game's error messages, and the address of any crash. Attach it to bug reports |

## Measured speed-ups

These come from Proton on a 20-thread AMD Ryzen AI 9 365, with one or a few runs each. Holidays vary by a few percent
between runs.

| Benchmark | Stock | cmspeed defaults |
|---|---|---|
| Holiday from mid-July 2001, per day | about 4.1 s | 0.43 s (−90%) |
| One-year holiday from July 2001 | | 58 s; about 86 s with `threads=0` |
| Creating a new game with every league, maximum database | 157 s | 57 s (−64%) |
| 95-day holiday with a much larger database (every player loaded) | | 112 s; (151 s with the scan on one thread) |

## Building

The build needs mingw-w64 (`i686-w64-mingw32-gcc` and `i686-w64-mingw32-windres`):

```sh
./mingw-build.sh  # writes dsound.dll next to the script
```

### Versions and releases

Releases are git tags named `vX.Y.Z`. The build stamps the version into the DLL, where Explorer shows it under
Properties > Details. `tools/version.sh` works it out from the tags:

- A build of a release tag gets the release version, such as `1.2.0`.
- Any other build gets a prerelease of the next patch version, such as `1.2.1-dev.5+g1a2b3c4`: 5 commits after
  `v1.2.0`, at commit `1a2b3c4`.

`CMSPEED_VERSION=1.3.0 ./mingw-build.sh` sets the version by hand.

GitHub Actions builds every push to main and every pull request. Each run's page offers `cmspeed-<version>.zip`, with
the DLL, the ini, this README and the license.

To make a release, run the **Release** workflow from the Actions tab on main. Pick `patch`, `minor` or `major`, or
type an exact version. The workflow builds the zip and creates a draft release with generated notes. Review the draft
on the Releases page and publish it. Publishing creates the `vX.Y.Z` tag, and later builds of main count from it.

### Generated code and tests

`src/tls_sites.h` lists the 63 game instructions that address the globals the parallel scan and matches give each thread
its own copy of. `tools/tls_sites.py` generates it from the 3.9.68 exe with GNU `objdump`:

```sh
python3 -I tools/tls_sites.py cm0102.exe > src/tls_sites.h
```

`tests/ftoltest.c` checks `fast_ftol` against the game's own conversion (see [fast-ftol](docs/fast-ftol.md)):

```sh
i686-w64-mingw32-gcc -O2 -Isrc -o ftoltest.exe tests/ftoltest.c
wine ftoltest.exe /path/to/cm0102.exe 200000000
```

## How it works

The game imports one function from `dsound.dll`. Windows looks for `dsound.dll` in the game folder before the system
folder, so the game loads cmspeed at startup. cmspeed passes sound calls on to the real DirectSound. If the exe is
3.9.68, it also patches the game in memory: a handful of instructions get new constants or a jump to a faster
equivalent in the DLL, or a small stub that runs extra code first. cmspeed compares the original bytes before each patch
and skips the patch if they differ.

Each doc covers one option: what the game does, how cmspeed changes it, how that was found and how it was checked.

- [holiday-wait](docs/holiday-wait.md): the 2-second pause per holiday day
- [fast-ftol](docs/fast-ftol.md): the C runtime's float-to-int conversion
- [fast-hist](docs/fast-hist.md): the player history re-sorts in new-game creation
- [prefetch](docs/prefetch.md): the cache misses in the AI's shortlist scan
- [par-scan](docs/par-scan.md): the AI's shortlist scan on threads
- [par-matches](docs/par-matches.md): background matches on threads
- [skip-splash](docs/skip-splash.md): the startup logos
- [mouse-nav](docs/mouse-nav.md): back and forward through the screen history
- [research methods](docs/research-methods.md): the tools and test setup behind all of the above

## Known limitations

- `holiday_wait` changes what you see: on holiday, each day's screen no longer stays up for 2 seconds. Set it to 2000
  for the stock pace.
- Parallel matches were checked on one-year holidays and on a 17-month holiday through the end of a season. Your own
  matches, cup ties, and saving and loading in the middle of a holiday haven't had a dedicated test.
- The parallel scan was checked with a verify mode over 4 100 split scans and run through holidays and new-game
  creation without faults.
- If the game crashes or stops during processing, set `threads=0` and attach `cmspeed.log` to a bug report.
- On a modified exe, a patch site the modification changed is skipped, and `cmspeed.log` says which.

## Disclaimer

cmspeed is an unofficial fan project, not affiliated with or endorsed by Sports Interactive or Eidos. It contains no
game data and changes no files. You need your own copy of the game.

## License

MIT
