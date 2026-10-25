/* Shared between cmspeed.c and its feature modules. */
#ifndef CMSPEED_H
#define CMSPEED_H

#include <windows.h>

extern char exe_dir[MAX_PATH];
extern DWORD game_thread_id;

void logmsg(const char *fmt, ...);
int patch_bytes(DWORD va, const BYTE *orig, const BYTE *repl, int len);
/* Points a call rel32 at va to fn, if it currently calls orig. */
int patch_call(DWORD va, DWORD orig, void *fn);
/* Jumps from va to fn, if the len (5..16) bytes there are orig: whole, position-independent instructions.
 * Returns a trampoline that runs them and continues at va + len, i.e. the original function; NULL if not
 * patched. */
void *hook_entry(DWORD va, const BYTE *orig, int len, void *fn);
/* 1 if a is a return address into the exe: right after a call instruction. */
int after_call(DWORD a);

/* par.c: par_matches=K */
int install_par_matches(int k, int threads);
/* every call to the simulation step 0x6b98c0 starts on zeroed stack: returns the number of call sites */
int install_zeroed_sim_steps(void);
/* tls.c: per-thread copies of some dword globals (tls_sites.h) */
int install_tls_globals(void);
DWORD *tls_ptr(DWORD global); /* this thread's copy, or the global itself when not installed */
/* dates.c: thread-safe DATE methods */
int install_dates(void);
/* nav.c: mouse_nav (hooks the game's window procedure and sidebar handler) */
int install_mouse_nav(void);
/* hist.c: fast_hist */
int install_fast_hist(void);
/* scan.c: par_scan=K */
int install_par_scan(int k, int min, int prefetch);
void par_scan_check_heap(void);
/* prefetch.c: prefetch=1 */
int install_prefetch(void);

#endif
