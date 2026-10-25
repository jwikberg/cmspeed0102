/* The game's DATE methods that keep scratch dates in function-local statics (0x54c5b0 and others), replaced by
 * the exact C versions in dates.h, which use locals: safe on several threads (par.c). Each original's first
 * 5 bytes become a jump to a thiscall wrapper with the same arguments, stack cleanup and return value (the
 * out parameter, or the day count). docs/par-matches.md describes how dates.h was checked against the originals. */

#include <string.h>

#include "cmspeed.h"
#include "dates.h"

static game_date *__attribute__((thiscall)) add_days(game_date *self, game_date *out, int n)
{
    game_date r;
    date_add_days(self, &r, n);
    *out = r;
    return out;
}

static game_date *__attribute__((thiscall)) sub_days(game_date *self, game_date *out, int n)
{
    game_date r;
    date_sub_days(self, &r, n);
    *out = r;
    return out;
}

static game_date *__attribute__((thiscall)) add_weeks(game_date *self, game_date *out, DWORD n)
{
    game_date r;
    date_add_weeks(self, &r, (unsigned char)n);
    *out = r;
    return out;
}

static game_date *__attribute__((thiscall)) sub_weeks(game_date *self, game_date *out, DWORD n)
{
    game_date r;
    date_sub_weeks(self, &r, (unsigned char)n);
    *out = r;
    return out;
}

static game_date *__attribute__((thiscall)) diff(game_date *self, game_date *out, game_date other)
{
    game_date r;
    date_diff(self, &r, other);
    *out = r;
    return out;
}

static int __attribute__((thiscall)) days_between(game_date *self, game_date other)
{
    return date_days_between(self, other);
}

static const struct { DWORD addr; BYTE orig[5]; void *fn; } detours[] = {
    {0x54c5b0, {0x83, 0xec, 0x10, 0xa0, 0x69}, add_days},
    {0x54c8e0, {0x83, 0xec, 0x10, 0x56, 0x57}, sub_days},
    {0x54c770, {0x51, 0xa0, 0x30, 0x24, 0xae}, add_weeks},
    {0x54cab0, {0x51, 0x56, 0x8b, 0xf1, 0x8a}, sub_weeks},
    {0x54cc30, {0x6a, 0xff, 0x68, 0x58, 0x7a}, diff},
    {0x54cdb0, {0x83, 0xec, 0x0c, 0xa0, 0x18}, days_between},
};

int install_dates(void)
{
    int n = 0;
    for (unsigned i = 0; i < sizeof detours / sizeof *detours; i++) {
        BYTE repl[5] = {0xe9};
        *(DWORD *)(repl + 1) = (DWORD)detours[i].fn - (detours[i].addr + 5);
        n += patch_bytes(detours[i].addr, detours[i].orig, repl, 5);
    }
    return n == sizeof detours / sizeof *detours;
}
