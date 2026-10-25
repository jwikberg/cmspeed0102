/* The game's __ftol (0x945640) and its replacement, shared by cmspeed.c and ftoltest.c.
 *
 * __ftol converts ST0 to a 64-bit integer in EDX:EAX, truncating, and pops ST0. MSVC6's version saves
 * the control word, sets rounding to chop, does fistp and restores the control word: two fldcw, which
 * serialise the x87 pipeline on modern CPUs. SSE3 fisttp always truncates without touching the control
 * word, with the same result for every input (out-of-range and NaN give the integer indefinite
 * 0x8000000000000000 in both), and also leaves only EAX and EDX changed. */

static const unsigned char ftol_orig[39] = {
    0x55,                   /* push ebp */
    0x8b, 0xec,             /* mov ebp,esp */
    0x83, 0xc4, 0xf4,       /* add esp,-12 */
    0x9b, 0xd9, 0x7d, 0xfe, /* fstcw [ebp-2] */
    0x9b,                   /* fwait */
    0x66, 0x8b, 0x45, 0xfe, /* mov ax,[ebp-2] */
    0x80, 0xcc, 0x0c,       /* or ah,0x0c */
    0x66, 0x89, 0x45, 0xfc, /* mov [ebp-4],ax */
    0xd9, 0x6d, 0xfc,       /* fldcw [ebp-4] */
    0xdf, 0x7d, 0xf4,       /* fistp qword [ebp-12] */
    0xd9, 0x6d, 0xfe,       /* fldcw [ebp-2] */
    0x8b, 0x45, 0xf4,       /* mov eax,[ebp-12] */
    0x8b, 0x55, 0xf8,       /* mov edx,[ebp-8] */
    0xc9,                   /* leave */
    0xc3,                   /* ret */
};

static const unsigned char ftol_fast[17] = {
    0x83, 0xec, 0x08,       /* sub esp,8 */
    0xdd, 0x0c, 0x24,       /* fisttp qword [esp] */
    0x8b, 0x04, 0x24,       /* mov eax,[esp] */
    0x8b, 0x54, 0x24, 0x04, /* mov edx,[esp+4] */
    0x83, 0xc4, 0x08,       /* add esp,8 */
    0xc3,                   /* ret */
};
