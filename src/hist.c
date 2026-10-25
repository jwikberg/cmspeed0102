/* hist.c: fast_hist, an exact replacement for the re-sort after each insert into the player history list.
 *
 * Game creation's "Player histories" step (0x7bfd50; progress text 0x9b5084) builds one array of 6-byte
 * records at manager +0x3c (count +0x44): dword id, signed byte, value byte (<= 10). For each player it
 * looks a record up with bsearch (0x946464) and, when that misses, appends one with 0x7c6a30(.., sort=1),
 * which re-sorts the whole array with qsort (0x945333, call at 0x7c6bda) every time. With all leagues
 * selected that is ~28% of game creation (the comparator 0x7c6d40 alone is 20%).
 *
 * The comparator 0x7c6d40 orders by id, then byte, except that a byte of -1 matches any byte. Without -1
 * bytes it is a strict order, and when the records before the new one are already strictly increasing and
 * none equals the new one, the sorted order is unique: whatever qsort does, it produces the array with the
 * new record moved into place. So that case is done with a binary search and a memmove. Anything else (a
 * -1 byte, an unsorted prefix after the sort=0 appends, an equal record) calls the original qsort.
 */

#include <windows.h>
#include <string.h>

#include "cmspeed.h"

#define HIST_SORT_CALL 0x7c6bda
#define FN_QSORT       0x945333
#define FN_HIST_CMP    0x7c6d40
#define REC 6

typedef void(__cdecl *qsort_fn)(void *, int, int, void *);

static BYTE *last_base; /* the array as this file last left it: sorted, strictly increasing, no -1 bytes */
static int last_n;

/* 0x7c6d40 for records without -1 bytes */
static int cmp(const BYTE *a, const BYTE *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    if (x != y)
        return x - y;
    return (signed char)a[4] - (signed char)b[4];
}

static int strictly_sorted(const BYTE *base, int n)
{
    for (int i = 0; i < n; i++)
        if ((signed char)base[i * REC + 4] == -1 || (i && cmp(base + (i - 1) * REC, base + i * REC) >= 0))
            return 0;
    return 1;
}

static void slow_sort(BYTE *base, int n, int size, void *f)
{
    ((qsort_fn)FN_QSORT)(base, n, size, f);
    if (strictly_sorted(base, n))
        last_base = base, last_n = n;
    else
        last_base = NULL;
}

static void __cdecl hist_sort(BYTE *base, int n, int size, void *f)
{
    if (size != REC || f != (void *)FN_HIST_CMP || n < 2)
        return ((qsort_fn)FN_QSORT)(base, n, size, f);
    BYTE e[REC];
    memcpy(e, base + (n - 1) * REC, REC);
    if ((signed char)e[4] == -1)
        return slow_sort(base, n, size, f);
    if (!((base == last_base && n == last_n + 1) || strictly_sorted(base, n - 1)))
        return slow_sort(base, n, size, f);
    int lo = 0, hi = n - 1; /* the first record greater than or equal to e */
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (cmp(base + mid * REC, e) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < n - 1 && cmp(base + lo * REC, e) == 0)
        return slow_sort(base, n, size, f);
    memmove(base + (lo + 1) * REC, base + lo * REC, (n - 1 - lo) * REC);
    memcpy(base + lo * REC, e, REC);
    last_base = base, last_n = n;
}

int install_fast_hist(void)
{
    return patch_call(HIST_SORT_CALL, FN_QSORT, hist_sort);
}
