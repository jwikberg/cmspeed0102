/* prefetch.c: prefetch=1 touches the next candidates' data in the AI shortlist scan (game creation's
 * "Creating Shortlists" step, and the same scan later in the game).
 *
 * 0x83f3a0 walks a list of 12-byte candidate entries (list in edx, index in ecx, count at [esp+0x8c] at
 * the loop head 0x83fa25). Each entry's first dword is a person id; the loop reads the person record
 * (0xae23bc + id * 0x6e) and then that person's entry in the table at 0xae2d0c (stride 0x4f, byte +0xb),
 * one dependent cache miss after the other. With all leagues selected, the instructions waiting on those
 * loads take ~15% of creating a game. The loop head jumps to a stub that prefetches the person record
 * 8 entries ahead and the 0xae2d0c entry 3 entries ahead (that one needs the person's id field, which
 * the earlier prefetch has brought in), runs the two replaced instructions and jumps back.
 * Prefetches change no data, so the game's results are the same.
 */

#include <windows.h>
#include <string.h>

#include "cmspeed.h"

#define SCAN_HEAD 0x83fa25
static const BYTE scan_head_orig[11] = {0x8d, 0x04, 0x49,                            /* lea eax,[ecx+ecx*2] */
                                        0xc7, 0x44, 0x24, 0x34, 0, 0, 0, 0};         /* mov [esp+0x34],0 */
DWORD scan_body = 0x83fa30; /* where the stub continues (an indirect jmp: the DLL may be rebased) */

/* Stub entered by jmp from SCAN_HEAD with the game's registers; eax is free (the next instruction sets it).
 * Constants: person table 0xae23bc (pointer), its count 0xae2364, table 0xae2d0c (pointer). */
__attribute__((naked)) static void scan_head_stub(void)
{
    __asm__ volatile(".intel_syntax noprefix\n"
                     "lea eax, [ecx+8]\n"                /* person record, 8 entries ahead */
                     "cmp eax, [esp+0x8c]\n"
                     "jge 1f\n"
                     "lea eax, [eax+eax*2]\n"
                     "mov eax, [edx+eax*4]\n"
                     "cmp eax, ds:0xae2364\n"
                     "jae 1f\n"                          /* also catches -1 */
                     "imul eax, eax, 0x6e\n"
                     "add eax, ds:0xae23bc\n"
                     "prefetcht0 [eax]\n"
                     "prefetcht0 [eax+0x6d]\n"
                     "1:\n"
                     "lea eax, [ecx+3]\n"                /* its staff entry, 3 entries ahead */
                     "cmp eax, [esp+0x8c]\n"
                     "jge 2f\n"
                     "lea eax, [eax+eax*2]\n"
                     "mov eax, [edx+eax*4]\n"
                     "cmp eax, ds:0xae2364\n"
                     "jae 2f\n"
                     "imul eax, eax, 0x6e\n"
                     "add eax, ds:0xae23bc\n"
                     "mov eax, [eax]\n"                  /* the person's id, as 0x83fb44 reads it */
                     "cmp eax, ds:0xae2364\n"
                     "jae 2f\n"
                     "imul eax, eax, 0x4f\n"
                     "add eax, ds:0xae2d0c\n"
                     "prefetcht0 [eax+0xb]\n"
                     "2:\n"
                     "lea eax, [ecx+ecx*2]\n"            /* the replaced instructions */
                     "mov dword ptr [esp+0x34], 0\n"
                     "jmp dword ptr [_scan_body]\n"
                     ".att_syntax prefix\n");
}

int install_prefetch(void)
{
    BYTE repl[11];
    memset(repl, 0x90, sizeof repl);
    repl[0] = 0xe9;
    *(DWORD *)(repl + 1) = (DWORD)scan_head_stub - (SCAN_HEAD + 5);
    return patch_bytes(SCAN_HEAD, scan_head_orig, repl, sizeof repl);
}
