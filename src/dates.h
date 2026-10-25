/* Exact C versions of the game's DATE methods that keep their scratch in function-local statics, so they
 * aren't safe on several threads (0x54c5b0 and the others below). Used by dates.c; docs/par-matches.md describes
 * how they were checked against the original code.
 *
 * DATE (8 bytes): short day of the year (0-based), ushort year, int flag (1 in a leap year). All methods are
 * thiscall on the date; results go to an 8-byte out parameter, except days_between, which returns them. */
#ifndef DATES_H
#define DATES_H

typedef struct { short day; unsigned short year; int leap; } game_date;

/* The game's test, kept as it is written: (y & 0x80000001) == 1 for an odd year (y is a ushort). */
static int date_leap(unsigned y)
{
    return !((y & 0x80000001) == 1 || (y & 3) != 0 || (y % 100 == 0 && y % 400 != 0));
}

static void date_forward(game_date *out, short d, unsigned short y)
{
    for (;;) {
        for (; !date_leap(y); y++) {
            if (d < 365)
                goto done;
            d -= 365;
        }
        if (d < 366)
            break;
        d -= 366;
        y++;
    }
done:
    out->day = d, out->year = y, out->leap = date_leap(y);
}

static void date_backward(game_date *out, short d, unsigned short y)
{
    for (;;) {
        unsigned short y1;
        for (;;) {
            y1 = y - 1;
            if (date_leap(y1))
                break;
            if (d >= 0)
                goto done;
            d += 365;
            y = y1;
        }
        if (d >= 0)
            break;
        d += 366;
        y = y1;
    }
done:
    out->day = d, out->year = y, out->leap = date_leap(y);
}

static void date_add_days(const game_date *self, game_date *out, int n);

/* 0x54c8e0 */
static void date_sub_days(const game_date *self, game_date *out, int n)
{
    if ((short)n < 0) {
        date_add_days(self, out, -n);
        return;
    }
    date_backward(out, (short)(self->day - (short)n), self->year);
}

/* 0x54c5b0 */
static void date_add_days(const game_date *self, game_date *out, int n)
{
    if ((short)n < 0) {
        date_sub_days(self, out, -n);
        return;
    }
    date_forward(out, (short)(self->day + (short)n), self->year);
}

/* 0x54c770 */
static void date_add_weeks(const game_date *self, game_date *out, unsigned char n)
{
    date_forward(out, (short)((unsigned short)n * 7 + self->day), self->year);
}

/* 0x54cab0 */
static void date_sub_weeks(const game_date *self, game_date *out, unsigned char n)
{
    date_backward(out, (short)(self->day - (unsigned short)n * 7), self->year);
}

/* 0x54cc30: the difference as a DATE (days, years); the flag is always 1 (its static's initial value) */
static void date_diff(const game_date *self, game_date *out, game_date other)
{
    if (self->year < other.year || (self->year <= other.year && self->day < other.day)) {
        date_diff(&other, out, *self);
        return;
    }
    short d = self->day, y;
    if ((int)self->day - (int)other.day < 0) {
        d = (short)(self->day - other.day + (date_leap(self->year) ? 366 : 365));
        y = (short)(self->year - other.year - 1);
    } else {
        d = (short)(self->day - other.day);
        y = (short)(self->year - other.year);
    }
    out->day = d, out->year = (unsigned short)y, out->leap = 1;
}

/* 0x54cdb0: days from other to self */
static int date_days_between(const game_date *self, game_date other)
{
    unsigned short y = self->year;
    int acc = 0;
    if (other.year <= y) {
        if (other.year < y || other.day <= self->day) {
            if (y != other.year)
                while (other.year < y) {
                    y--;
                    acc += date_leap(y) ? 366 : 365;
                }
            goto done;
        }
        if (y == other.year)
            goto done;
    }
    for (; y < other.year; y++)
        acc += date_leap((unsigned short)(y - 1)) ? -366 : -365;
done:
    return acc + ((int)self->day - (int)other.day);
}

#endif
