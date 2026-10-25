/* cmspeed: dsound.dll proxy that speeds up cm0102.exe 3.9.68 (Championship Manager 01/02).
 *
 * The game imports only DirectSoundCreate (ordinal 1) from dsound.dll, so a proxy placed next to the
 * exe is loaded at startup (Wine: WINEDLLOVERRIDES="dsound=n,b"). DirectSoundCreate is forwarded to
 * the system dsound.dll. On load, the DLL checks the exe's PE timestamp and the original bytes at
 * every patch site, and leaves anything it does not recognise untouched.
 *
 * Features (settings in cmspeed.ini next to the exe; how each works in docs/):
 *   - holiday_wait: on holiday, the screen loop 0x7615d0 auto-continues only once
 *     clock() >= <per-manager timestamp> + 2000 ms (0x76224c). The simulation itself takes a
 *     fraction of that, so a day costs ~4 s of mostly waiting. This rewrites the 2000.
 *   - fast_ftol: the CRT's float-to-int helper __ftol (0x945640, 1949 call sites) reloads the x87
 *     control word twice per call and is ~16% of holiday processing. Replaced in place with SSE3
 *     fisttp, which gives the same result for every input (tests/ftoltest.c).
 *   - skip_splash: skip the startup logos.
 *   - mouse_nav: the mouse's back/forward buttons go back/forward through the screens, as the sidebar's
 *     arrows do (nav.c).
 *   - fast_hist: game creation re-sorts the player history list after every insert (~28% of creating a
 *     game with all leagues); insert in place instead, with exactly the same result (hist.c).
 *   - prefetch: prefetch the next candidates' records in the AI shortlist scan 0x83f3a0, which waits on
 *     cache misses (prefetch.c).
 *   - threads: split the AI shortlist scan's large loops over threads (par_scan, scan.c), and play
 *     background fixtures in batches, simulated on threads (par_matches, par.c).
 *
 * Build: mingw-build.sh
 */

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmspeed.h"
#include "ftol_fast.h"

/* cm0102.exe 3.9.68 (image base 0x400000, no relocations). */
#define CM0102_TIMESTAMP 0x3ddcdba5
#define FN_FTOL          0x945640

/* 0x76224c: add esi,0x7d0 -- the holiday auto-continue delay in ms, after
 * FUN_005fdc30 (the human manager's timestamp, +0x9a65) and before clock() (0x90c510). */
#define HOLIDAY_WAIT_AT  0x76224c
static const BYTE holiday_wait_orig[6] = {0x81, 0xc6, 0xd0, 0x07, 0x00, 0x00};

/* 0x5ccd3c: je 0x5cd0b3 -- the main loop 0x5cccd0 skips all startup logos when logo.rgn fails to
 * load. Made unconditional. */
#define SPLASH_AT 0x5ccd3c
static const BYTE splash_orig[6] = {0x0f, 0x84, 0x71, 0x03, 0x00, 0x00};
static const BYTE splash_skip[6] = {0x90, 0xe9, 0x71, 0x03, 0x00, 0x00};

static struct {
    int log;          /* cmspeed.log next to the exe */
    int holiday_wait; /* ms; stock 2000 */
    int fast_ftol;
    int skip_splash;
    int mouse_nav;    /* mouse back/forward buttons navigate the screen history (nav.c) */
    int fast_hist;    /* game creation's player histories: insert instead of re-sorting (hist.c) */
    int prefetch;     /* prefetch the AI shortlist scan's next candidates (prefetch.c) */
    int threads;      /* par_scan and par_matches on this many threads; 0 = both off */
    int par_scan;     /* the AI shortlist scan's big loops on this many threads (scan.c); 0 = serial */
    int par_matches;  /* play background fixtures in batches of this many (par.c); 0 = stock */
    int par_threads;  /* with par_matches: worker threads besides the game thread (0 = simulate serially) */
} cfg = {.log = 0, .fast_ftol = 1, .skip_splash = 1, .mouse_nav = 1, .fast_hist = 1, .prefetch = 1};

/* Shortlist scans with fewer candidates run serially: splitting them costs more than it saves (docs/par-scan.md). */
#define PAR_SCAN_MIN 10000

static FILE *logfile;
char exe_dir[MAX_PATH];
DWORD game_thread_id;

void logmsg(const char *fmt, ...)
{
    if (!logfile)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(logfile, "%8lu ", GetTickCount());
    vfprintf(logfile, fmt, ap);
    va_end(ap);
    fputc('\n', logfile);
    fflush(logfile);
}

int patch_bytes(DWORD va, const BYTE *orig, const BYTE *repl, int len)
{
    if (memcmp((void *)va, orig, len)) {
        logmsg("%#lx: unexpected code; not patching", va);
        return 0;
    }
    DWORD old;
    VirtualProtect((void *)va, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void *)va, repl, len);
    VirtualProtect((void *)va, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)va, len);
    return 1;
}

void *hook_entry(DWORD va, const BYTE *orig, int len, void *fn)
{
    BYTE *t = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    BYTE repl[16];
    if (!t || len < 5 || len > 16)
        return NULL;
    memcpy(t, orig, len); /* the replaced instructions, then jmp back */
    t[len] = 0xe9;
    *(DWORD *)(t + len + 1) = va + len - (DWORD)(t + len + 5);
    memset(repl, 0x90, sizeof repl);
    repl[0] = 0xe9;
    *(DWORD *)(repl + 1) = (DWORD)fn - (va + 5);
    if (!patch_bytes(va, orig, repl, len)) {
        VirtualFree(t, 0, MEM_RELEASE);
        return NULL;
    }
    return t;
}

int patch_call(DWORD va, DWORD orig, void *fn)
{
    BYTE expect[5] = {0xe8}, repl[5] = {0xe8};
    *(DWORD *)(expect + 1) = orig - (va + 5);
    *(DWORD *)(repl + 1) = (DWORD)fn - (va + 5);
    return patch_bytes(va, expect, repl, 5);
}

int after_call(DWORD a)
{
    const BYTE *p = (const BYTE *)a;
    if (a < 0x401000 + 8 || a >= 0x967000)
        return 0;
    if (p[-5] == 0xe8)                                                    /* call rel32 */
        return 1;
    if (p[-2] == 0xff && ((p[-1] >> 3) & 7) == 2 && (p[-1] >> 6) == 3)    /* call reg */
        return 1;
    if (p[-2] == 0xff && (p[-1] & 0xf8) == 0x10 && (p[-1] & 7) != 4 && (p[-1] & 7) != 5)
        return 1;                                                         /* call [reg] */
    if (p[-3] == 0xff && (p[-2] & 0xf8) == 0x50 && (p[-2] & 7) != 4)      /* call [reg+d8] */
        return 1;
    if (p[-3] == 0xff && p[-2] == 0x14)                                   /* call [sib] */
        return 1;
    if (p[-4] == 0xff && p[-3] == 0x54)                                   /* call [sib+d8] */
        return 1;
    if (p[-6] == 0xff && (p[-5] == 0x15 || ((p[-5] & 0xf8) == 0x90 && (p[-5] & 7) != 4)))
        return 1;                                                         /* call [abs] / [reg+d32] */
    if (p[-7] == 0xff && (p[-6] == 0x14 || p[-6] == 0x94))                /* call [sib*s+d32] */
        return 1;
    return 0;
}

/* ---- holiday_wait ---- */

static int install_holiday_wait(void)
{
    BYTE repl[6];
    memcpy(repl, holiday_wait_orig, 2);
    memcpy(repl + 2, &cfg.holiday_wait, 4);
    return patch_bytes(HOLIDAY_WAIT_AT, holiday_wait_orig, repl, sizeof repl);
}

/* ---- fast_ftol ---- */

static int install_fast_ftol(void)
{
    if (!IsProcessorFeaturePresent(PF_SSE3_INSTRUCTIONS_AVAILABLE)) {
        logmsg("fast_ftol: no SSE3; not patching");
        return 0;
    }
    BYTE repl[sizeof ftol_orig];
    memset(repl, 0xcc, sizeof repl);
    memcpy(repl, ftol_fast, sizeof ftol_fast);
    return patch_bytes(FN_FTOL, ftol_orig, repl, sizeof repl);
}

/* ---- crash log ---- */

static HMODULE self;

static int in_module(DWORD a, HMODULE m)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
    return a >= (DWORD)m && a < (DWORD)m + nt->OptionalHeader.SizeOfImage;
}

/* First-chance exceptions in the exe or this DLL (IsBadReadPtr's own faults happen in kernelbase). */
static LONG CALLBACK crash_logger(EXCEPTION_POINTERS *e)
{
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    DWORD ip = (DWORD)r->ExceptionAddress;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || r->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION ||
        r->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO) {
        static volatile LONG others;
        if (in_module(ip, GetModuleHandleA(NULL)) || in_module(ip, self) || InterlockedIncrement(&others) <= 5) {
            CONTEXT *c = e->ContextRecord;
            const BYTE *code = (const BYTE *)ip;
            logmsg("exception %#lx at %#lx%s (thread %lu, address %#lx): eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx "
                   "esi=%08lx edi=%08lx ebp=%08lx esp=%08lx code %02x %02x %02x %02x %02x %02x [esp] %08lx",
                   r->ExceptionCode, ip, in_module(ip, self) ? " (cmspeed)" : "", GetCurrentThreadId(),
                   r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi,
                   c->Edi, c->Ebp, c->Esp, code[0], code[1], code[2], code[3], code[4], code[5], *(DWORD *)c->Esp);
            /* the call chain, from return addresses on the stack: the exe's (after a call instruction) and this DLL's */
            char buf[1024];
            int n = snprintf(buf, sizeof buf, "  %s thread; stack:", GetCurrentThreadId() == game_thread_id ? "game" : "other");
            const DWORD *sp = (const DWORD *)c->Esp;
            MEMORY_BASIC_INFORMATION mi;
            SIZE_T words = VirtualQuery(sp, &mi, sizeof mi) ? ((DWORD)mi.BaseAddress + mi.RegionSize - (DWORD)sp) / 4 : 0;
            for (SIZE_T i = 0, k = 0; i < words && i < 4096 && k < 32 && n < (int)sizeof buf - 32; i++) {
                DWORD a = sp[i];
                if (after_call(a))
                    n += snprintf(buf + n, sizeof buf - n, " %lx", a), k++;
                else if (in_module(a, self))
                    n += snprintf(buf + n, sizeof buf - n, " cmspeed+%lx", a - (DWORD)self), k++;
            }
            logmsg("%s", buf);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- error boxes ----
 * The game reports failed checks in a modal box 0x5e8290 (cdecl: title, text, flags), "v3.9.68 FILE.CPP LINE",
 * and waits for a click. Each one also goes to the log. On a worker thread (par_scan, par_matches) the box isn't
 * shown: the game thread waits for the worker, so nobody could click it. */
#define FN_ERROR_BOX 0x5e8290
static const BYTE error_box_orig[9] = {0x83, 0xec, 0x6c, 0x8b, 0x0d, 0x30, 0x7a, 0xb6, 0x00};
static int(__cdecl *error_box)(const char *, const char *, int);

static int __cdecl logged_error_box(const char *title, const char *text, int flags)
{
    logmsg("error box (thread %lu%s, from %#lx): %s: %s", GetCurrentThreadId(),
           GetCurrentThreadId() == game_thread_id ? ", game" : "", (DWORD)__builtin_return_address(0),
           title ? title : "", text ? text : "");
    if (GetCurrentThreadId() != game_thread_id)
        return 0;
    return error_box(title, text, flags);
}

/* ---- startup ---- */

/* Settings: cmspeed.ini, then CMSPEED_<KEY> environment variables, which override it. */
static void setting(const char *ini, const char *key, const char *def, char *out, int size)
{
    char env[64];
    snprintf(env, sizeof env, "CMSPEED_%s", key);
    for (char *p = env; *p; p++)
        if (*p >= 'a' && *p <= 'z')
            *p -= 'a' - 'A';
    if (!GetEnvironmentVariableA(env, out, size))
        GetPrivateProfileStringA("cmspeed", key, def, out, size, ini);
}

static int setting_int(const char *ini, const char *key, int def)
{
    char buf[32], defs[16];
    snprintf(defs, sizeof defs, "%d", def);
    setting(ini, key, defs, buf, sizeof buf);
    return atoi(buf);
}

/* threads=auto (or missing): the CPU's logical processors, at most 8, which were as fast as 16; at most 16, the
 * most either option takes; below 2, off. par_scan runs that many workers while the game thread waits; par_matches
 * simulates that many fixtures at a time, one of them on the game thread. */
static void set_threads(const char *ini)
{
    char buf[32];
    setting(ini, "threads", "auto", buf, sizeof buf);
    int n = atoi(buf);
    if (!_stricmp(buf, "auto") || !*buf) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        n = si.dwNumberOfProcessors < 8 ? (int)si.dwNumberOfProcessors : 8;
    }
    if (n > 16)
        n = 16;
    cfg.threads = n < 2 ? 0 : n;
    cfg.par_scan = cfg.par_matches = cfg.threads;
    cfg.par_threads = cfg.threads ? cfg.threads - 1 : 0;
}

static char ini_path[MAX_PATH + 16];

static void load_config(void)
{
    char *ini = ini_path;
    snprintf(ini_path, sizeof ini_path, "%s\\cmspeed.ini", exe_dir);
    cfg.log = setting_int(ini, "log", cfg.log);
    cfg.holiday_wait = setting_int(ini, "holiday_wait", cfg.holiday_wait);
    cfg.fast_ftol = setting_int(ini, "fast_ftol", cfg.fast_ftol);
    cfg.skip_splash = setting_int(ini, "skip_splash", cfg.skip_splash);
    cfg.mouse_nav = setting_int(ini, "mouse_nav", cfg.mouse_nav);
    cfg.fast_hist = setting_int(ini, "fast_hist", cfg.fast_hist);
    cfg.prefetch = setting_int(ini, "prefetch", cfg.prefetch);
    set_threads(ini);
    if (cfg.holiday_wait < 0 || cfg.holiday_wait > 2000)
        cfg.holiday_wait = 2000;
}

static void install(void)
{
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, sizeof path);
    strcpy(exe_dir, path);
    char *slash = strrchr(exe_dir, '\\');
    if (slash)
        *slash = 0;
    game_thread_id = GetCurrentThreadId();
    load_config();
    if (cfg.log) {
        char lp[MAX_PATH + 16];
        snprintf(lp, sizeof lp, "%s\\cmspeed.log", exe_dir);
        logfile = fopen(lp, "w");
    }
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    if ((DWORD)base != 0x400000 || nt->FileHeader.TimeDateStamp != CM0102_TIMESTAMP) {
        logmsg("%s: not cm0102.exe 3.9.68 (base %p, timestamp %#lx); doing nothing", path, base,
               nt->FileHeader.TimeDateStamp);
        return;
    }
    logmsg("cmspeed on %s: holiday_wait=%d fast_ftol=%d skip_splash=%d mouse_nav=%d fast_hist=%d prefetch=%d "
           "threads=%d", path, cfg.holiday_wait, cfg.fast_ftol, cfg.skip_splash, cfg.mouse_nav, cfg.fast_hist,
           cfg.prefetch, cfg.threads);
    if (cfg.holiday_wait != 2000 && install_holiday_wait())
        logmsg("holiday auto-continue delay: %d ms (stock 2000)", cfg.holiday_wait);
    if (cfg.fast_ftol && install_fast_ftol())
        logmsg("fast_ftol on");
    if (cfg.skip_splash && patch_bytes(SPLASH_AT, splash_orig, splash_skip, sizeof splash_skip))
        logmsg("skip_splash on");
    if (cfg.mouse_nav && install_mouse_nav())
        logmsg("mouse_nav on");
    if (cfg.par_matches)
        logmsg("zeroed stack for %d simulation-step calls", install_zeroed_sim_steps());
    if ((cfg.par_matches || cfg.par_scan) && !GetEnvironmentVariableA("__MSVCRT_HEAP_SELECT", NULL, 0)) {
        /* On Windows before NT 5 (Wine set to Windows 95) the CRT would use its small-block heap, which
         * isn't thread-safe (par.c, start_workers). This runs before the exe's CRT initialises its heap,
         * and selects HeapAlloc, as on NT. */
        SetEnvironmentVariableA("__MSVCRT_HEAP_SELECT", "__GLOBAL_HEAP_SELECTED,1");
        logmsg("CRT heap: __MSVCRT_HEAP_SELECT=__GLOBAL_HEAP_SELECTED,1 (HeapAlloc on any Windows version)");
    }
    if (cfg.par_matches || cfg.par_scan) {
        /* the CRT heap is created HEAP_NO_SERIALIZE (single-threaded CRT: 0x948aeb xor eax,eax; ...; sete al);
         * without the sete the flags are 0 and the heap locks itself */
        if (patch_bytes(0x948af8, (const BYTE *)"\x0f\x94\xc0", (const BYTE *)"\x90\x90\x90", 3))
            logmsg("CRT heap: serialised");
        if (!install_tls_globals())
            logmsg("tls: not installed");
        logmsg("DATE methods: %s", install_dates() ? "thread-safe versions" : "unexpected code; not replaced");
    }
    if (cfg.par_matches)
        logmsg("par_matches=%d, %d threads: %s", cfg.par_matches, cfg.par_threads,
               install_par_matches(cfg.par_matches, cfg.par_threads) ? "on" : "unexpected code");
    if (cfg.par_scan) { /* its loop-head stub also does the prefetch */
        logmsg("par_scan=%d (loops from %d candidates): %s", cfg.par_scan, PAR_SCAN_MIN,
               install_par_scan(cfg.par_scan, PAR_SCAN_MIN, cfg.prefetch) ? "on" : "unexpected code; off");
        cfg.prefetch = 0;
    }
    if (cfg.prefetch && install_prefetch())
        logmsg("prefetch on");
    if (cfg.fast_hist && install_fast_hist())
        logmsg("fast_hist on");
    if ((logfile || cfg.par_matches || cfg.par_scan) &&
        !(error_box = hook_entry(FN_ERROR_BOX, error_box_orig, sizeof error_box_orig, logged_error_box)))
        logmsg("error boxes: unexpected code; not logged");
    if (logfile)
        AddVectoredExceptionHandler(1, crash_logger);
}

/* ---- dsound.dll forwarding (cm0102.exe imports only ordinal 1) ---- */

static HRESULT (WINAPI *real_DirectSoundCreate)(LPCGUID, void **, IUnknown *);

HRESULT WINAPI DirectSoundCreate(LPCGUID guid, void **ds, IUnknown *outer)
{
    if (!real_DirectSoundCreate) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, sizeof path);
        strcat(path, "\\dsound.dll");
        HMODULE m = LoadLibraryA(path);
        real_DirectSoundCreate = m ? (void *)GetProcAddress(m, "DirectSoundCreate") : NULL;
        logmsg("system dsound %s: %p", path, (void *)real_DirectSoundCreate);
        if (!real_DirectSoundCreate)
            return E_FAIL;
    }
    return real_DirectSoundCreate(guid, ds, outer);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        self = inst;
        install();
    }
    return TRUE;
}
