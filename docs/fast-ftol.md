# Float-to-int conversion

Option: `fast_ftol` (default 1). Exact: the same result for every input.

## What the game does

Once [holiday_wait](holiday-wait.md) had removed the pause, 98.5% of a holiday's samples were in the game's own code,
and the top function, at 15.7%, was the MSVC6 runtime's `__ftol` (`0x945640`, 1 949 call sites). The compiler calls it
for every C cast from a float to an integer. It converts the top of the x87 stack to a 64-bit integer, truncating:

```
fstcw [ebp-2]              ; save the control word
or    ah,0x0c              ; rounding mode: chop
fldcw [ebp-4]
fistp qword [ebp-12]
fldcw [ebp-2]              ; restore
```

Each `fldcw` serialises the FPU pipeline on modern CPUs, twice per call.

## How cmspeed works

It overwrites the function in place with:

```
sub esp,8 ; fisttp qword [esp] ; mov eax,[esp] ; mov edx,[esp+4] ; add esp,8 ; ret
```

SSE3's `fisttp` always truncates, without touching the control word. It gives the same result as the original for
every input, including the integer indefinite `0x8000000000000000` for NaN and out-of-range values. Like the original,
it pops the stack and changes only EAX and EDX. Without SSE3, cmspeed leaves `__ftol` alone.

## How it was checked

`tests/ftoltest.c` runs under Wine. It copies the original 39 bytes out of the exe and runs both versions on
200 000 000 inputs:

- random 80-bit patterns (denormals, NaNs, infinities, out-of-range values), random doubles and floats, and edge cases;
- under every rounding mode and precision control a caller could have set.

It compares the result, the caller's control word afterwards and the x87 stack depth. Result: **0 differences**. As a
control, a replacement that rounds to nearest instead differs on 12.5% of inputs.

```sh
i686-w64-mingw32-gcc -O2 -Isrc -o ftoltest.exe tests/ftoltest.c
wine ftoltest.exe /path/to/cm0102.exe 200000000
```

## Measured

The same 37 holiday days as in [holiday-wait](holiday-wait.md): 18.6 s → 15.8 s, a mean of 502 → 427 ms per day (−15%).
In a later profile `__ftol` was down to 3.9% of samples; it is still called that often.
