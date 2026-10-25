/* par_matches: play the day's background fixtures in batches, simulated on worker threads (docs/par-matches.md).
 *
 * The day's fixture loop 0x6b4360 plays each background fixture in turn (0x6b4a36-0x6b4dd6): squad setup,
 * match object, simulation steps, recording, release, progress text. With par_matches=K the body at
 * 0x6b4a36 only sets the fixture up and queues it; the queue is drained when it holds K fixtures, and before
 * any other code can see the queued fixtures: at the loop's end (0x6b4e0b), before its break for a match a
 * human must watch (0x6b4df5), and before it handles a click during a holiday (0x6b496b). Draining simulates
 * each queued fixture from the RNG state its setup left (so the order of simulation doesn't matter), then
 * records and releases them in fixture order, as the original body does. The per-fixture progress text is
 * not shown.
 *
 * Differences from stock with K > 1: a fixture is set up before the earlier fixtures of its batch are
 * simulated and recorded, and each simulation starts from its own RNG state. */

#include <stdio.h>
#include <string.h>

#include "cmspeed.h"

#define FN_SETUP_SQUADS 0x90d2f0  /* thiscall(squads; entry + 4, slot, 0) */
#define FN_RELEASE_SQUADS 0x90d6b0 /* thiscall(squads; entry + 4) */
#define SQUADS        (*(void **)0xde2f70)
#define FN_NEW        0x944cf1    /* operator new, cdecl */
#define FN_DELETE     0x944c94    /* operator delete, cdecl */
#define FN_MATCH_CTOR 0x6b7e50    /* thiscall(match) */
#define FN_MATCH_PREP 0x6b66a0    /* cdecl(entry + 4) */
#define FN_MATCH_INIT 0x6b7f20    /* thiscall(match; entry + 4, prep, 0, 0) */
#define FN_SIM_STEP   0x6b98c0    /* thiscall(match; -1) -> state in al */
#define FN_MATCH_DTOR 0x6b97a0    /* thiscall(match) */
#define FN_VEC_CTOR   0x9463af    /* stdcall(array, size, count, ctor, dtor): `vector constructor iterator' */
#define FN_RECORD     0x7293f0    /* thiscall(recorder; match, stats) */
#define RECORDER      (*(void **)0xb6b7c0)
#define MATCH_SIZE 0xf63b
#define STATS_SIZE 0xd7a

#define ENTRY_MATCH(f) (*(void **)((f) + 0x57))
#define ENTRY_STATS(f) (*(void **)((f) + 0x5f))
#define ENTRY_STATE(f) (*(signed char *)((f) + 0x67))
#define LIST_DONE(l)   (*(int *)((l) + 0x25))

#define RNG_POS  (*(volatile DWORD *)tls_ptr(0xde1f60))
#define RNG_OFF  (*(volatile DWORD *)tls_ptr(0xde1f5c))
#define CRT_SEED (*(volatile DWORD *)tls_ptr(0xad6c30))

struct rng { DWORD pos, off, crt; };
/* 0x896290 adds a finished match to statistics shared with other clubs' matches (ecx = *(0xde1d08)); its
 * one caller (0x6ba9a0, at the end of a simulation) ignores the result. During a batch the call is recorded
 * and replayed when the fixture is recorded, in fixture order, so simulations never see each other's. */
#define TABLE_UPDATE_CALL 0x6ba9a0
#define FN_TABLE_UPDATE   0x896290
/* 0x62be00 adds to or merges into a list on the global object *(0xb63d28) (+0x28) and returns the merged
 * values; its one caller is 0x6d09ed. Simulations use it under a lock. */
#define LIST_MERGE_CALL 0x6d09ed
#define FN_LIST_MERGE   0x62be00
#define MAX_DEFERRED 4

struct queued {
    BYTE *entry;
    void *match;
    struct rng rng;
    int finished;
    int ndeferred;
    struct { void *self; int arg; } deferred[MAX_DEFERRED];
};

#define MAX_BATCH 17
static struct queued queue[MAX_BATCH];
static int nqueued, batch_size;

static void save_rng(struct rng *r) { r->pos = RNG_POS, r->off = RNG_OFF, r->crt = CRT_SEED; }
static void load_rng(const struct rng *r) { RNG_POS = r->pos, RNG_OFF = r->off, CRT_SEED = r->crt; }

/* Squad setup fills short squads with generated "virtual" players, which live in fixed person-id ranges
 * per slot (0x90e760): slot s >= 0 (the human managers' slots) at base + 50*s, background fixtures' slot -2 at
 * base + 800. In this game almost every background club plays with them, so each fixture of a batch gets a
 * slot of its own: -2, then the slots of the humans not in the game. Release (0x90d6b0) finds virtual players
 * by id, whatever their slot. */
#define HUMAN_ACTIVE ((volatile short *)0xb77c5a)
static int slots[17], nslots;

static void init_slots(void)
{
    nslots = 0;
    slots[nslots++] = -2;
    for (int i = 0; i < 16; i++)
        if (HUMAN_ACTIVE[i] <= 0)
            slots[nslots++] = i;
}

/* The engine reads uninitialised stack (its results depended on which code last used the stack below the
 * simulation: here, Wine's PeekMessage, so runs weren't reproducible). Each step therefore starts on 64 KB of
 * zeroed stack, on the game thread and on workers alike. alloca probes the pages in order. */
#define ZERO_STACK 0x10000

static void __attribute__((noinline)) zero_stack_below(void)
{
    volatile BYTE *p = __builtin_alloca(ZERO_STACK);
    for (int i = 0; i < ZERO_STACK; i += 4)
        *(volatile DWORD *)(p + i) = 0;
}

static char sim_step(void *match)
{
    zero_stack_below();
    return ((char(__attribute__((thiscall)) *)(void *, int))FN_SIM_STEP)(match, -1);
}

/* The same for the simulation step's other callers (the human's own matches, live scores, ...). */
static char __attribute__((thiscall)) zeroed_sim_step(void *match, int arg)
{
    zero_stack_below();
    return ((char(__attribute__((thiscall)) *)(void *, int))FN_SIM_STEP)(match, arg);
}

int install_zeroed_sim_steps(void)
{
    int n = 0;
    for (BYTE *p = (BYTE *)0x401000; p < (BYTE *)0x967000 - 5; p++)
        if (*p == 0xe8 && (DWORD)p + 5 + *(int *)(p + 1) == FN_SIM_STEP)
            n += patch_call((DWORD)p, FN_SIM_STEP, zeroed_sim_step);
    return n;
}

/* One fixture's simulation steps, on whichever thread: its RNG state goes in and out through this thread's
 * copies of the RNG globals (tls.c). The list's finished count is updated serially, in par_drain. */
static DWORD current_slot; /* TLS: the queued fixture this thread is simulating */
static CRITICAL_SECTION list_lock;

static void __attribute__((thiscall)) deferred_table_update(void *self, int arg)
{
    struct queued *q = TlsGetValue(current_slot);
    if (q && q->ndeferred < MAX_DEFERRED) {
        q->deferred[q->ndeferred].self = self, q->deferred[q->ndeferred].arg = arg;
        q->ndeferred++;
        return;
    }
    if (q)
        logmsg("par: more than %d table updates in one match; applied at once", MAX_DEFERRED);
    ((int(__attribute__((thiscall)) *)(void *, int))FN_TABLE_UPDATE)(self, arg);
}

static void __attribute__((thiscall)) locked_list_merge(void *self, int a, int b, int c)
{
    EnterCriticalSection(&list_lock);
    ((void(__attribute__((thiscall)) *)(void *, int, int, int))FN_LIST_MERGE)(self, a, b, c);
    LeaveCriticalSection(&list_lock);
}

/* The engine also reads memory it allocated without initialising, so what a fresh block holds (left over
 * from earlier use) changes results: on several threads which block an allocation gets depends on timing,
 * and even serially the heap's history depends on how many frames idle screens ran. With batching every
 * CRT allocation is zeroed (HEAP_ZERO_MEMORY). */
#define IAT_HEAPALLOC   0x967154
#define IAT_HEAPREALLOC 0x967160
static void *(WINAPI *real_HeapAlloc)(HANDLE, DWORD, SIZE_T);
static void *(WINAPI *real_HeapReAlloc)(HANDLE, DWORD, void *, SIZE_T);

static void *WINAPI zeroing_HeapAlloc(HANDLE h, DWORD flags, SIZE_T n)
{
    return real_HeapAlloc(h, flags | HEAP_ZERO_MEMORY, n);
}

static void *WINAPI zeroing_HeapReAlloc(HANDLE h, DWORD flags, void *p, SIZE_T n)
{
    return real_HeapReAlloc(h, flags | HEAP_ZERO_MEMORY, p, n);
}

static int hook_slot(DWORD va, void *fn, void **real)
{
    DWORD old;
    if (!VirtualProtect((void *)va, 4, PAGE_READWRITE, &old))
        return 0;
    *real = *(void **)va;
    *(void **)va = fn;
    VirtualProtect((void *)va, 4, old, &old);
    return 1;
}

static void simulate(struct queued *q)
{
    BYTE *f = q->entry;
    TlsSetValue(current_slot, q);
    load_rng(&q->rng);
    signed char state = ENTRY_STATE(f);
    while (state != 2 && state != 8) {
        state = sim_step(q->match);
        ENTRY_STATE(f) = state;
        if (state == 2 || state == 8)
            q->finished = 1;
    }
    save_rng(&q->rng);
    TlsSetValue(current_slot, NULL);
}

/* ---- workers ----
 * par_threads=T: T threads besides the game thread take queued fixtures in turn until none are left. They
 * run with the game thread's x87 control word and MXCSR (the engine uses x87 floats). */
static int nworkers;
static HANDLE workers_go, workers_done;
static volatile LONG next_item, workers_left;
static WORD fpu_cw;
static DWORD mxcsr;

static void take_items(void)
{
    for (LONG i; (i = InterlockedIncrement(&next_item) - 1) < nqueued;)
        simulate(&queue[i]);
}

static DWORD WINAPI worker(void *unused)
{
    for (;;) {
        WaitForSingleObject(workers_go, INFINITE);
        __asm__ volatile("fninit\n\tfldcw %0\n\tldmxcsr %1" : : "m"(fpu_cw), "m"(mxcsr));
        take_items();
        if (!InterlockedDecrement(&workers_left))
            SetEvent(workers_done);
    }
    return 0;
}

/* The CRT's malloc (0x944e84) uses HeapAlloc only in heap mode 1 (__active_heap, 0xde4404). __heap_select
 * (0x9489a3) picks it on NT 5 and later; on older Windows (Wine set to Windows 95) it picks the CRT's own
 * small-block heap (mode 3, 0x948ee4), whose free lists have no lock, so serialising HeapAlloc doesn't
 * protect them and parallel simulation corrupts memory. cmspeed.c sets __MSVCRT_HEAP_SELECT before the CRT
 * starts; this is the check that it worked. */
#define CRT_ACTIVE_HEAP (*(volatile int *)0xde4404)
static int par_threads;

static void start_workers(int n)
{
    if (CRT_ACTIVE_HEAP != 1) {
        logmsg("par_matches: the CRT heap is mode %d, not 1 (HeapAlloc), so it isn't thread-safe; simulating "
               "on the game thread (set __MSVCRT_HEAP_SELECT=__GLOBAL_HEAP_SELECTED,1)", CRT_ACTIVE_HEAP);
        par_threads = 0; /* don't try again */
        return;
    }
    workers_go = CreateSemaphoreA(NULL, 0, 64, NULL);
    workers_done = CreateEventA(NULL, FALSE, FALSE, NULL);
    for (int i = 0; i < n; i++) {
        HANDLE t = CreateThread(NULL, 8 << 20, worker, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!t)
            break;
        CloseHandle(t);
        nworkers++;
    }
    logmsg("par_matches: %d worker threads", nworkers);
}

/* Records and releases one fixture, continuing from the RNG state its simulation left (as stock does,
 * where recording follows the simulation directly). */
static void record(struct queued *q)
{
    BYTE *f = q->entry;
    load_rng(&q->rng);
    void *stats = ((void *(__cdecl *)(size_t))FN_NEW)(STATS_SIZE);
    if (stats)
        ((void(__stdcall *)(void *, int, int, DWORD, DWORD))FN_VEC_CTOR)((BYTE *)stats + 0x11f, 0x625, 2, 0x453c30,
                                                                         0x453c10);
    for (int i = 0; i < q->ndeferred; i++)
        ((int(__attribute__((thiscall)) *)(void *, int))FN_TABLE_UPDATE)(q->deferred[i].self, q->deferred[i].arg);
    ENTRY_STATS(f) = stats;
    if (!stats)
        logmsg("par: out of memory for match stats; fixture %p not recorded", f);
    else
        ((void(__attribute__((thiscall)) *)(void *, void *, void *))FN_RECORD)(RECORDER, q->match, stats);
    ((void(__attribute__((thiscall)) *)(void *))FN_MATCH_DTOR)(q->match);
    ((void(__cdecl *)(void *))FN_DELETE)(q->match);
    ENTRY_MATCH(f) = NULL;
    ((void(__attribute__((thiscall)) *)(void *, int))FN_RELEASE_SQUADS)(SQUADS, (int)(f + 4));
    save_rng(&q->rng);
}

/* Simulates, records and releases the queued fixtures (game thread). */
void __cdecl par_drain(BYTE *list) __asm__("par_drain");
void __cdecl par_drain(BYTE *list)
{
    if (!nqueued)
        return;
    if (par_threads && !nworkers)
        start_workers(par_threads);
    next_item = 0;
    if (nworkers && nqueued > 1) {
        __asm__ volatile("fnstcw %0\n\tstmxcsr %1" : "=m"(fpu_cw), "=m"(mxcsr));
        struct rng own;
        save_rng(&own);
        workers_left = nworkers;
        ReleaseSemaphore(workers_go, nworkers, NULL);
        take_items();
        WaitForSingleObject(workers_done, INFINITE);
        load_rng(&own);
    } else
        take_items();
    for (int i = 0; i < nqueued; i++) {
        if (queue[i].finished)
            LIST_DONE(list)++;
        record(&queue[i]);
    }
    load_rng(&queue[nqueued - 1].rng); /* as after the last fixture's recording */
    nqueued = 0;
}

/* The fixture body: set up and queue. Returns 1 when stock would have shown the progress text
 * (the match object's first byte is 0), for the loop's "anything shown" flag. */
int __cdecl par_queue(BYTE *list, BYTE *f) __asm__("par_queue");
int __cdecl par_queue(BYTE *list, BYTE *f)
{
    if (!nqueued)
        init_slots();
    ((void(__attribute__((thiscall)) *)(void *, int, int, int))FN_SETUP_SQUADS)(SQUADS, (int)(f + 4), slots[nqueued], 0);
    void *m = ((void *(__cdecl *)(size_t))FN_NEW)(MATCH_SIZE);
    if (m)
        m = ((void *(__attribute__((thiscall)) *)(void *))FN_MATCH_CTOR)(m);
    ENTRY_MATCH(f) = m;
    if (!m) {
        logmsg("par: out of memory for a match; fixture %p skipped", f);
        return 0;
    }
    int prep = ((int(__cdecl *)(int))FN_MATCH_PREP)((int)(f + 4));
    ((void(__attribute__((thiscall)) *)(void *, int, int, int, int))FN_MATCH_INIT)(m, (int)(f + 4), prep, 0, 0);
    int shown = *(signed char *)m == 0;
    struct queued *q = &queue[nqueued++];
    q->entry = f, q->match = m, q->finished = 0, q->ndeferred = 0;
    save_rng(&q->rng);
    if (nqueued >= batch_size || nqueued >= nslots)
        par_drain(list);
    return shown;
}

/* Stubs at the hooked instructions. The loop's frame: [esp+0x14] the fixture list, [esp+0x20] "progress
 * shown", esi the fixture entry at 0x6b4a36. */
__asm__(".intel_syntax noprefix\n"
        "par_body_stub:\n"                 /* replaces 0x6b4a36 mov ecx,[0xde2f70] */
        "  pushad\n"
        "  push esi\n"
        "  push dword ptr [esp + 0x14 + 36]\n"
        "  call par_queue\n"
        "  add esp, 8\n"
        "  test eax, eax\n"
        "  jz 1f\n"
        "  mov dword ptr [esp + 0x20 + 32], 1\n"
        "1:\n"
        "  popad\n"
        "  push 0x6b4dd6\n"
        "  ret\n"
        "par_end_stub:\n"                  /* replaces 0x6b4e0b mov edx,[esp+0x28]; push edx */
        "  pushad\n"
        "  push dword ptr [esp + 0x14 + 32]\n"
        "  call par_drain\n"
        "  add esp, 4\n"
        "  popad\n"
        "  mov edx, [esp + 0x28]\n"
        "  push edx\n"
        "  push 0x6b4e10\n"
        "  ret\n"
        "par_break_stub:\n"                /* replaces 0x6b4df5 mov ecx,[esp+0x14]; push 1 */
        "  pushad\n"
        "  push dword ptr [esp + 0x14 + 32]\n"
        "  call par_drain\n"
        "  add esp, 4\n"
        "  popad\n"
        "  mov ecx, [esp + 0x14]\n"
        "  push 1\n"
        "  push 0x6b4dfb\n"
        "  ret\n"
        "par_click_stub:\n"                /* replaces 0x6b496b xor ecx,ecx; xor esi,esi; mov eax,[0xae2364] */
        "  pushad\n"
        "  push dword ptr [esp + 0x14 + 32]\n"
        "  call par_drain\n"
        "  add esp, 4\n"
        "  popad\n"
        "  xor ecx, ecx\n"
        "  xor esi, esi\n"
        "  mov eax, ds:[0xae2364]\n"
        "  push 0x6b4974\n"
        "  ret\n"
        ".att_syntax\n");
void par_body_stub(void) __asm__("par_body_stub");
void par_end_stub(void) __asm__("par_end_stub");
void par_break_stub(void) __asm__("par_break_stub");
void par_click_stub(void) __asm__("par_click_stub");

static int hook(DWORD va, const char *orig, int len, void *stub)
{
    BYTE repl[16];
    memset(repl, 0x90, sizeof repl);
    repl[0] = 0xe9;
    *(DWORD *)(repl + 1) = (DWORD)stub - (va + 5);
    return patch_bytes(va, (const BYTE *)orig, repl, len);
}

int install_par_matches(int k, int threads)
{
    par_threads = threads;
    batch_size = k > MAX_BATCH ? MAX_BATCH : k;
    current_slot = TlsAlloc();
    InitializeCriticalSection(&list_lock);
    return hook_slot(IAT_HEAPALLOC, zeroing_HeapAlloc, (void **)&real_HeapAlloc) &&
           hook_slot(IAT_HEAPREALLOC, zeroing_HeapReAlloc, (void **)&real_HeapReAlloc) &&
           patch_call(TABLE_UPDATE_CALL, FN_TABLE_UPDATE, deferred_table_update) &&
           patch_call(LIST_MERGE_CALL, FN_LIST_MERGE, locked_list_merge) &&
           hook(0x6b4a36, "\x8b\x0d\x70\x2f\xde\x00", 6, par_body_stub) &&
           hook(0x6b4e0b, "\x8b\x54\x24\x28\x52", 5, par_end_stub) &&
           hook(0x6b4df5, "\x8b\x4c\x24\x14\x6a\x01", 6, par_break_stub) &&
           hook(0x6b496b, "\x33\xc9\x33\xf6\xa1\x64\x23\xae\x00", 9, par_click_stub);
}
