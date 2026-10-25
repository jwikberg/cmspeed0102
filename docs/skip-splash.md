# The startup logos

Option: `skip_splash` (default 1).

## What the game does

The main loop `0x5cccd0` plays the startup logos with their sounds before the main menu. If `logo.rgn` fails to load, a
branch at `0x5ccd3c` skips the whole sequence.

## How cmspeed works

It makes the "`logo.rgn` failed to load" branch at `0x5ccd3c` unconditional (`je` becomes `nop; jmp`).

## How it was researched

The decompile of the main loop showed the logo sequence and its failure branch. Headless runs confirmed that the game
reaches the main menu without the logos.

## Limits

On a modified exe that has already changed this branch, cmspeed skips it, and `cmspeed.log` says so.
