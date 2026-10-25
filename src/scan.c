/* scan.c: par_scan=K runs the AI's candidate scan 0x83f3a0 on K worker threads (docs/par-scan.md).
 *
 * The scan loops over sublists, and in each over a candidate list (loop head 0x83fa25, exit 0x840a99). Within one
 * sublist's loop the only state carried from one candidate to the next is the index [esp+0x40], the running best
 * value [esp+0x58] and the pick [esp+0x88]; the pick changes only for a value strictly above the best, so the first
 * maximum wins. On the game thread's first arrival at the loop head of a sublist with at least PAR_SCAN_MIN
 * candidates, the loop is split into K contiguous slices: each worker copies the 0x114-byte frame below the loop
 * head's esp onto its own stack (same alignment mod 16), sets its index and end ([esp+0x8c]), loads the game
 * thread's registers and enters the loop head. The stub at the loop exit sends a worker back when esp is its
 * slice's frame. The merge goes through the slices in order and takes a slice's pick when its best is strictly
 * higher than the best so far; the game thread then continues at the exit. Arguments are read through ebp from the
 * game thread's frame, which waits meanwhile.
 *
 * Random draws: during a split, each candidate draws from its own stream, seeded at its loop head from the scan's
 * seed (one draw of the game's RNG) and its sublist and index, so its draws don't depend on slicing. The daily
 * per-person refresh 0x62c680 draws from a stream of (person, date) and restores the thread's state, so it gives
 * the same result whichever candidate triggers it. The draws therefore don't depend on slicing, but they aren't the
 * stock draws: results are equivalent to a serial scan's, not identical (docs/par-scan.md).
 *
 * Shared state (found with a write watch around the scan):
 *   - the refresh 0x62c680 (its memo 0xb63d08-0xb63d30, file I/O, news): one lock around the whole call;
 *   - 0x4a4850's lookup arrays (0x76-byte entries): it bsearches by id, first re-sorting by id when the array's
 *     sort key +0x18 isn't 0xff (other code sorts them by stat fields; the comparator reads the key, so the key
 *     must be set before the qsort, as stock does). While workers run, a call that will re-sort takes a
 *     reader-writer lock exclusively and every other call takes it shared;
 *   - 0x8ce990's list iterator embedded in the transfer manager (mgr+0x92c): each thread walks its own copy;
 *   - 0x69fc60 (is the key in this object's list?) walked the list with the list's own cursor, saved and restored
 *     around the walk, so two threads asking at once cut each other's walk short (its "list ends early" check,
 *     manager_manager.cpp 14116). It is replaced by the same test with a local cursor, which leaves the list
 *     untouched. The only other list walk shared between slices was 0x4cca30 (find the item with this id and
 *     state 2 in the list at +0x34; no caller goes on walking that list), replaced the same way;
 *   - the "club is watching your player" news 0x587220 (call at 0x840a79), posted for a human-managed club's
 *     player who becomes the running pick: on a worker the call is recorded, and the merge replays the ones whose
 *     value beats the best of the earlier slices, i.e. the ones a serial scan would have posted;
 *   - RNG, the lookup context 0xadae10, scratch dates and the CRT heap: per thread or serialised, as for par_matches.
 */

#define _WIN32_WINNT 0x0600 /* SRW locks */
#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cmspeed.h"

#define SCAN_HEAD 0x83fa25
static const BYTE head_orig[11] = {0x8d, 0x04, 0x49, 0xc7, 0x44, 0x24, 0x34, 0, 0, 0, 0}; /* lea; mov [esp+0x34],0 */
#define SCAN_EXIT 0x840a99
static const BYTE exit_orig[8] = {0x8b, 0x74, 0x24, 0x78, 0x8a, 0x5c, 0x24, 0x0f}; /* mov esi,[esp+0x78]; mov bl,[esp+0xf] */
#define FRAME   0x114 /* the scan's frame below the loop head's esp */
#define F_INDEX 0x40
#define F_BEST  0x58
#define F_PICK  0x88
#define F_COUNT 0x8c
#define F_SUBLIST 0xf
#define MAX_K 16
#define MAX_EVENTS 64

#define FN_RANDOM  0x90cfc0 /* cdecl int random(n) */
#define FN_REFRESH 0x62c680 /* thiscall (cache, person) */
static const BYTE refresh_orig[7] = {0x6a, 0xff, 0x68, 0xf8, 0xb0, 0x95, 0x00};
#define FN_LOOKUP 0x4a4850 /* thiscall int (array object, key) */
static const BYTE lookup_orig[7] = {0x6a, 0xff, 0x68, 0x88, 0x67, 0x95, 0x00};
#define ITER_LEA        0x8ceab3 /* lea esi,[edi+0x92c] */
static const BYTE iter_orig[6] = {0x8d, 0xb7, 0x2c, 0x09, 0x00, 0x00};
#define NEWS_CALL 0x840a79
#define FN_NEWS   0x587220 /* cdecl (person, club) */
#define RNG_TABLE 0xaa2454
#define RNG_TABLE_LEN 51000
#define GAME_DATE (*(volatile DWORD *)0xae2c90)
#define CRT_ACTIVE_HEAP (*(volatile int *)0xde4404)

struct event { void *person, *club; int best; };

/* Per thread. The job layout up to frame is used by the assembly below. */
struct scan_job {
    DWORD driver_esp;             /* 0: the worker's own esp while it runs a slice */
    DWORD edi, esi, ebp, entry;   /* 4, 8, 12, 16 */
    DWORD ebx, edx, ecx, eax;     /* 20, 24, 28, 32 */
    DWORD align;                  /* 36: the game thread's esp mod 16 at the loop head */
    BYTE frame[FRAME];            /* 40: in: the frame to start from; out: the frame at the slice's end */
    DWORD base;                   /* 0x13c: the slice's frame base on this thread (esp at the loop head) */
    int worker, mode;             /* mode: candidate streams on (a split slice) */
    DWORD seed;
    BYTE iter[16];                /* this thread's copy of the transfer manager's iterator */
    struct event events[MAX_EVENTS];
    int nevents;
    HANDLE go, done, thread;
    WORD fpucw;
    DWORD mxcsr;
};
_Static_assert(offsetof(struct scan_job, frame) == 40, "job layout");
_Static_assert(offsetof(struct scan_job, base) == 0x13c, "job layout");

/* tls.c's per-thread globals besides random(n)'s: a worker starts each slice with the game thread's values */
static const DWORD shared_globals[] = {0xb67a64, 0xadae10, 0xadae14, 0xadae18, 0xb679fc, 0xb67a00};
#define NSHARED (sizeof shared_globals / sizeof *shared_globals)
static DWORD shared_values[NSHARED];

static struct scan_job jobs[MAX_K], game_job, other_job; /* other_job: threads that aren't ours (mode 0) */
static DWORD scan_tls;
/* warmup: the first large loops of a session run serially */
static int k_threads, scan_min = 10000, warmup = 20, active, prefetch;
int scan_prefetch __asm__("scan_prefetch"); /* = prefetch, for the head stub's fast path */
static long n_split;
static CRITICAL_SECTION refresh_lock;
static SRWLOCK lookup_lock = SRWLOCK_INIT;
static volatile int splitting; /* workers are running a split loop (the game thread waits) */
volatile int scan_streams __asm__("scan_streams"); /* a split is on: every loop head goes to scan_head */
DWORD scan_cont[3] __asm__("scan_cont") = {SCAN_HEAD + 11, SCAN_EXIT, SCAN_EXIT + 8};

static struct scan_job *self(void)
{
    struct scan_job *j = TlsGetValue(scan_tls);
    return j ? j : GetCurrentThreadId() == game_thread_id ? &game_job : &other_job;
}

static DWORD mix(DWORD h, DWORD v)
{
    h ^= v + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= h >> 15, h *= 0x2c1b3c6d, h ^= h >> 12, h *= 0x297a2d39, h ^= h >> 15;
    return h;
}

/* random(n)'s state for this thread: the table position, its offset, and the CRT rand seed it reseeds from */
#define RNG_POS  (*(volatile DWORD *)tls_ptr(0xde1f60))
#define RNG_OFF  (*(volatile DWORD *)tls_ptr(0xde1f5c))
#define CRT_SEED (*(volatile DWORD *)tls_ptr(0xad6c30))
static void seed_rng(DWORD h)
{
    RNG_POS = RNG_TABLE + 4 * (h % (RNG_TABLE_LEN - 1));
    RNG_OFF = mix(h, 1) & 0xffff;
    CRT_SEED = mix(h, 2);
}

/* ---- the loop head and exit ---- */

/* Worker: run the job's frame from the loop head on this thread's stack; returns when the exit stub finds this
 * slice's frame (scan_leave_slice). */
void __cdecl scan_enter_slice(struct scan_job *job) __asm__("scan_enter_slice");
__asm__("scan_enter_slice:\n"
        "push %ebp\npush %ebx\npush %esi\npush %edi\n"
        "mov 20(%esp),%eax\n"
        "mov %esp,0(%eax)\n"
        "sub $0x400,%esp\nand $-16,%esp\nor 36(%eax),%esp\n" /* the game thread's alignment */
        "mov %esp,%edi\nlea 40(%eax),%esi\nmov $0x45,%ecx\ncld\nrep movsl\n" /* FRAME / 4 */
        "mov %esp,0x13c(%eax)\n"
        "pushl 16(%eax)\n"                                                    /* the loop head */
        "mov 4(%eax),%edi\nmov 8(%eax),%esi\nmov 12(%eax),%ebp\nmov 20(%eax),%ebx\nmov 24(%eax),%edx\n"
        "mov 28(%eax),%ecx\nmov 32(%eax),%eax\n"
        "ret\n");

void scan_leave_slice(void) __asm__("scan_leave_slice");
__asm__("scan_leave_slice:\n"
        "mov 0(%eax),%esp\npop %edi\npop %esi\npop %ebx\npop %ebp\nret\n");

static DWORD WINAPI scan_worker(void *arg)
{
    struct scan_job *job = arg;
    TlsSetValue(scan_tls, job);
    for (;;) {
        WaitForSingleObject(job->go, INFINITE);
        for (unsigned i = 0; i < NSHARED; i++)
            *tls_ptr(shared_globals[i]) = shared_values[i];
        __asm__ volatile("fninit\n\tfldcw %0\n\tldmxcsr %1" : : "m"(job->fpucw), "m"(job->mxcsr));
        scan_enter_slice(job);
        job->base = 0, job->mode = 0;
        SetEvent(job->done);
    }
    return 0;
}

/* Game thread, first candidate of a big sublist: run its loop on the workers. 1 = done, continue at the exit. */
static int split(DWORD *esp)
{
    BYTE *frame = (BYTE *)esp;
    int count = esp[F_COUNT / 4], k = k_threads;
    if (k > count / 256)
        k = count / 256;
    if (k < 2)
        return 0;
    DWORD seed = mix(((int (*)(int))FN_RANDOM)(0x40000000), (DWORD)n_split);
    WORD cw;
    DWORD mx;
    __asm__ volatile("fnstcw %0\n\tstmxcsr %1" : "=m"(cw), "=m"(mx));
    for (unsigned i = 0; i < NSHARED; i++)
        shared_values[i] = *tls_ptr(shared_globals[i]);
    for (int s = 0; s < k; s++) {
        struct scan_job *j = &jobs[s];
        int lo = (int)((long long)count * s / k), hi = (int)((long long)count * (s + 1) / k);
        memcpy(j->frame, frame, FRAME);
        *(int *)(j->frame + F_INDEX) = lo;
        *(int *)(j->frame + F_COUNT) = hi;
        j->edi = esp[-8], j->esi = esp[-7], j->ebp = esp[-6], j->ebx = esp[-4], j->edx = esp[-3];
        j->ecx = lo, j->eax = esp[-1];
        j->entry = SCAN_HEAD, j->align = (DWORD)esp & 0xf;
        j->mode = 1, j->seed = seed, j->nevents = 0;
        j->fpucw = cw, j->mxcsr = mx;
    }
    splitting = 1, scan_streams = 1;
    for (int s = 0; s < k; s++)
        SetEvent(jobs[s].go);
    for (int s = 0; s < k; s++)
        WaitForSingleObject(jobs[s].done, INFINITE);
    splitting = 0, scan_streams = 0;
    /* the merge, in candidate order */
    int best = *(int *)(frame + F_BEST);
    DWORD pick = *(DWORD *)(frame + F_PICK);
    for (int s = 0; s < k; s++) {
        struct scan_job *j = &jobs[s];
        for (int e = 0; e < j->nevents; e++)
            if (j->events[e].best > best)
                ((void (*)(void *, void *))FN_NEWS)(j->events[e].person, j->events[e].club);
        int b = *(int *)(j->frame + F_BEST);
        if (b > best)
            best = b, pick = *(DWORD *)(j->frame + F_PICK);
    }
    n_split++;
    *(int *)(frame + F_BEST) = best;
    *(DWORD *)(frame + F_PICK) = pick;
    *(int *)(frame + F_INDEX) = count;
    return 1;
}

/* prefetch.c's prefetch, for the loop head this stub takes over: the person record 8 candidates ahead and its
 * 0xae2d0c entry 3 ahead (the list is edx, 12-byte entries, person id first) */
static void prefetch_ahead(const DWORD *esp, int index)
{
    const int *list = (const int *)esp[-3], count = esp[F_COUNT / 4], persons = *(int *)0xae2364;
    if (index + 8 < count && (unsigned)list[(index + 8) * 3] < (unsigned)persons) {
        BYTE *p = *(BYTE **)0xae23bc + list[(index + 8) * 3] * 0x6e;
        __builtin_prefetch(p), __builtin_prefetch(p + 0x6d);
    }
    if (index + 3 < count && (unsigned)list[(index + 3) * 3] < (unsigned)persons) {
        int id = *(int *)(*(BYTE **)0xae23bc + list[(index + 3) * 3] * 0x6e);
        if ((unsigned)id < (unsigned)persons)
            __builtin_prefetch(*(BYTE **)0xae2d0c + id * 0x4f + 0xb);
    }
}

int __cdecl scan_head(DWORD *esp) __asm__("scan_head");
int __cdecl scan_head(DWORD *esp)
{
    struct scan_job *t = self();
    int index = esp[F_INDEX / 4];
    if (prefetch)
        prefetch_ahead(esp, index);
    if (t->mode && t->base == (DWORD)esp) /* this candidate's own stream */
        seed_rng(mix(mix(t->seed, ((BYTE *)esp)[F_SUBLIST]), index));
    if (t != &game_job || t->base || index || !active)
        return 0;
    static int heap_checked;
    if (!heap_checked++)
        par_scan_check_heap();
    if (!active)
        return 0;
    if ((int)esp[F_COUNT / 4] < scan_min || warmup > 0 || GetCurrentThreadId() != game_thread_id) {
        if ((int)esp[F_COUNT / 4] >= scan_min && warmup > 0)
            warmup--;
        return 0;
    }
    return split(esp);
}

DWORD __cdecl scan_exit(DWORD *esp) __asm__("scan_exit");
DWORD __cdecl scan_exit(DWORD *esp)
{
    struct scan_job *t = self();
    if (!t->worker || t->base != (DWORD)esp)
        return 0;
    memcpy(t->frame, esp, FRAME);
    return (DWORD)t;
}

/* stubs: pushal; pushfl; scan_head/scan_exit(esp before); then replay the hooked instructions and go on, or go to
 * the exit (split done), or leave the slice (worker) */
void scan_head_stub(void) __asm__("scan_head_stub");
__asm__("scan_head_stub:\n"
        /* fast path, for nearly every candidate: not the first one of a loop, and no split running. eax is
         * free here (the replayed instructions set it); only lea/mov/cmp follow, and the flags are dead at the loop head */
        "test %ecx,%ecx\njz 2f\ncmpl $0,scan_streams\njnz 2f\ncmpl $0,scan_prefetch\njz 3f\n"
        "lea 8(%ecx),%eax\ncmp 0x8c(%esp),%eax\njge 4f\nlea (%eax,%eax,2),%eax\nmov (%edx,%eax,4),%eax\n"
        "cmp 0xae2364,%eax\njae 4f\nimul $0x6e,%eax,%eax\nadd 0xae23bc,%eax\nprefetcht0 (%eax)\nprefetcht0 0x6d(%eax)\n"
        "4:\nlea 3(%ecx),%eax\ncmp 0x8c(%esp),%eax\njge 3f\nlea (%eax,%eax,2),%eax\nmov (%edx,%eax,4),%eax\n"
        "cmp 0xae2364,%eax\njae 3f\nimul $0x6e,%eax,%eax\nadd 0xae23bc,%eax\nmov (%eax),%eax\ncmp 0xae2364,%eax\njae 3f\n"
        "imul $0x4f,%eax,%eax\nadd 0xae2d0c,%eax\nprefetcht0 0xb(%eax)\n"
        "3:\nlea (%ecx,%ecx,2),%eax\nmovl $0,0x34(%esp)\njmp *scan_cont\n"
        "2:\npushal\npushfl\nlea 0x24(%esp),%eax\npush %eax\ncall scan_head\nadd $4,%esp\ntest %eax,%eax\njnz 1f\n"
        "popfl\npopal\nlea (%ecx,%ecx,2),%eax\nmovl $0,0x34(%esp)\njmp *scan_cont\n"
        "1:\npopfl\npopal\njmp *scan_cont+4\n");
void scan_exit_stub(void) __asm__("scan_exit_stub");
__asm__("scan_exit_stub:\n"
        "pushal\npushfl\nlea 0x24(%esp),%eax\npush %eax\ncall scan_exit\nadd $4,%esp\ntest %eax,%eax\njnz scan_leave_slice\n"
        "popfl\npopal\nmov 0x78(%esp),%esi\nmov 0xf(%esp),%bl\njmp *scan_cont+8\n");

/* ---- shared state ---- */

typedef void(__attribute__((thiscall)) * refresh_fn)(void *, int *);
static refresh_fn refresh;
static void __attribute__((thiscall)) locked_refresh(void *cache, int *person)
{
    struct scan_job *t = self();
    EnterCriticalSection(&refresh_lock);
    if (t->mode && person) { /* the same draws whichever candidate gets here first */
        DWORD pos = RNG_POS, off = RNG_OFF, crt = CRT_SEED;
        seed_rng(mix(mix(t->seed, *person), GAME_DATE));
        refresh(cache, person);
        RNG_POS = pos, RNG_OFF = off, CRT_SEED = crt;
    } else
        refresh(cache, person);
    LeaveCriticalSection(&refresh_lock);
}

/* 0x69fc60: thiscall char (object, int *key): is *key the id (+6) of an item of the list *(object+8)? (count at
 * +0, first node at +2; a node is {item, ?, next at +6}) */
#define FN_IN_LIST 0x69fc60
static const BYTE in_list_orig[6] = {0x81, 0xec, 0x00, 0x02, 0x00, 0x00}; /* sub esp,0x200 */
static DWORD __attribute__((thiscall)) in_list(BYTE *object, int *key)
{
    BYTE *list = *(BYTE **)(object + 8), *node = *(BYTE **)(list + 2);
    for (int i = 0, n = *(short *)list; i < n && node && *(BYTE **)node; i++, node = *(BYTE **)(node + 6))
        if (*(int *)(*(BYTE **)node + 6) == *key)
            return 1;
    return 0; /* stock returns al = 0 here too, after its error box if the list ends before its count */
}

/* 0x4cca30: thiscall int *(object, id): the item of the list *(object+0x34) with id (+0) and byte +0x50 == 2 */
#define FN_FIND_STATE2 0x4cca30
static const BYTE find_state2_orig[7] = {0x56, 0x8b, 0xf1, 0x57, 0x8b, 0x46, 0x34};
static int *__attribute__((thiscall)) find_state2(BYTE *object, int id)
{
    BYTE *list = *(BYTE **)(object + 0x34);
    if (!list)
        return NULL;
    for (BYTE *node = *(BYTE **)(list + 2); node; node = *(BYTE **)(node + 6)) {
        int *item = *(int **)node;
        if (!item) /* 0x68d1b0 returns the item; a null one ends the walk */
            break;
        if (*item == id && ((BYTE *)item)[0x50] == 2)
            return item;
    }
    return NULL;
}

/* 0x4a4850 */
typedef int(__attribute__((thiscall)) * lookup_fn)(BYTE *, void *);
static lookup_fn lookup;
static int __attribute__((thiscall)) locked_lookup(BYTE *obj, void *key)
{
    if (!splitting)
        return lookup(obj, key);
    int sorts = obj && !*(int *)(obj + 0x566) && *(int *)(obj + 0xc) && obj[0x18] != 0xff, r;
    if (sorts) {
        AcquireSRWLockExclusive(&lookup_lock);
        r = lookup(obj, key);
        ReleaseSRWLockExclusive(&lookup_lock);
    } else {
        AcquireSRWLockShared(&lookup_lock);
        r = lookup(obj, key);
        ReleaseSRWLockShared(&lookup_lock);
    }
    return r;
}

/* 0x8ce990: esi = this thread's copy of the transfer manager's iterator at edi+0x92c */
BYTE *__cdecl scan_iter(BYTE *mgr) __asm__("scan_iter");
BYTE *__cdecl scan_iter(BYTE *mgr)
{
    struct scan_job *t = self();
    if (!t->mode)
        return mgr + 0x92c;
    memcpy(t->iter, mgr + 0x92c, sizeof t->iter);
    return t->iter;
}
DWORD scan_iter_cont __asm__("scan_iter_cont") = ITER_LEA + 6;
void scan_iter_stub(void) __asm__("scan_iter_stub");
__asm__("scan_iter_stub:\n"
        "pushal\npush %edi\ncall scan_iter\nadd $4,%esp\nmov %eax,4(%esp)\npopal\njmp *scan_iter_cont\n");

static void __cdecl deferred_news(void *person, void *club)
{
    struct scan_job *t = self();
    if (!t->worker) {
        ((void (*)(void *, void *))FN_NEWS)(person, club);
        return;
    }
    if (t->nevents < MAX_EVENTS)
        t->events[t->nevents++] = (struct event){person, club, *(int *)((BYTE *)t->base + F_BEST)};
}

static int jmp_patch(DWORD va, const BYTE *orig, int len, void *stub)
{
    BYTE repl[16];
    memset(repl, 0x90, sizeof repl);
    repl[0] = 0xe9;
    *(DWORD *)(repl + 1) = (DWORD)stub - (va + 5);
    return patch_bytes(va, orig, repl, len);
}

int install_par_scan(int k, int min, int with_prefetch)
{
    if (k > MAX_K)
        k = MAX_K;
    if (k < 2)
        return 0;
    if (CRT_ACTIVE_HEAP != 0 && CRT_ACTIVE_HEAP != 1)
        logmsg("par_scan: CRT heap mode %d", CRT_ACTIVE_HEAP);
    k_threads = k, scan_min = min, prefetch = with_prefetch;
    scan_prefetch = with_prefetch;
    scan_tls = TlsAlloc();
    InitializeCriticalSection(&refresh_lock);
    if (!jmp_patch(SCAN_HEAD, head_orig, sizeof head_orig, scan_head_stub) ||
        !jmp_patch(SCAN_EXIT, exit_orig, sizeof exit_orig, scan_exit_stub) ||
        !(refresh = hook_entry(FN_REFRESH, refresh_orig, sizeof refresh_orig, locked_refresh)) ||
        !(lookup = hook_entry(FN_LOOKUP, lookup_orig, sizeof lookup_orig, locked_lookup)) ||
        !jmp_patch(ITER_LEA, iter_orig, sizeof iter_orig, scan_iter_stub) ||
        !patch_call(NEWS_CALL, FN_NEWS, deferred_news) || !jmp_patch(FN_IN_LIST, in_list_orig, sizeof in_list_orig, in_list) ||
        !jmp_patch(FN_FIND_STATE2, find_state2_orig, sizeof find_state2_orig, find_state2))
        return 0;
    for (int i = 0; i < k; i++) {
        struct scan_job *j = &jobs[i];
        j->worker = 1;
        j->go = CreateEventA(NULL, FALSE, FALSE, NULL);
        j->done = CreateEventA(NULL, FALSE, FALSE, NULL);
        j->thread = CreateThread(NULL, 8 << 20, scan_worker, j, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!j->thread)
            return 0;
    }
    active = 1;
    return 1;
}

/* once the CRT has chosen its heap (par.c's check): workers only on a thread-safe heap */
void par_scan_check_heap(void)
{
    if (active && CRT_ACTIVE_HEAP != 1) {
        logmsg("par_scan: the CRT heap is mode %d, not 1 (HeapAlloc), so it isn't thread-safe; scans run serially",
               CRT_ACTIVE_HEAP);
        active = 0;
    }
}
