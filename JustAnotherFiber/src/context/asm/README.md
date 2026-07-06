# Vendored Boost.Context assembly

This folder holds the hand-written context-switch routines from
[Boost.Context](https://github.com/boostorg/context), vendored so we don't take
a dependency on all of Boost. Only the **Windows x64 / MSVC (MASM)** variants are
needed for the current target.

## Files to place here

Download these three files (exact names) from
<https://github.com/boostorg/context/tree/develop/src/asm>:

| File | Routine |
|------|---------|
| `make_x86_64_ms_pe_masm.asm`  | `make_fcontext` — set up a new context on a stack |
| `jump_x86_64_ms_pe_masm.asm`  | `jump_fcontext` — switch between contexts |
| `ontop_x86_64_ms_pe_masm.asm` | `ontop_fcontext` — resume a context running a function on top (used later) |

Raw URLs:

```
https://raw.githubusercontent.com/boostorg/context/develop/src/asm/make_x86_64_ms_pe_masm.asm
https://raw.githubusercontent.com/boostorg/context/develop/src/asm/jump_x86_64_ms_pe_masm.asm
https://raw.githubusercontent.com/boostorg/context/develop/src/asm/ontop_x86_64_ms_pe_masm.asm
```

These are the **`ms_pe` (Windows PE / MASM)** variants — they save and restore the
Windows TIB stack fields (stack base/limit, dealloc stack, fiber-local storage).
Do **not** substitute the `sysv` (Linux/macOS) files; the calling convention for
`transfer_t` differs and they will crash.

`ontop_fcontext` is not exercised in Milestone 1 but is included now so we don't
have to re-fetch when the scheduler needs it.

## Local modifications

These files are **no longer byte-for-byte upstream.** `jump_x86_64_ms_pe_masm.asm`
and `ontop_x86_64_ms_pe_masm.asm` have been edited for AVX/AVX2 hosts:

- The XMM6–XMM15 save/restore uses **VEX-encoded `vmovaps`** instead of legacy
  `movaps`. Mixing legacy SSE into a switch path that runs between AVX2 code
  incurs the AVX↔SSE transition penalty on every switch; VEX encoding avoids it.
- A **`vzeroupper`** is emitted on the restore path before control is handed to
  the resumed context, so it starts in a clean upper-YMM state.

These changes do **not** alter the saved-frame layout (the win64 ABI makes the
upper 128 bits of YMM6–YMM15 volatile, so only the lower 128 bits need saving —
exactly what upstream stores). `make_x86_64_ms_pe_masm.asm` has no XMM path and
is untouched. If you ever re-fetch from upstream, re-apply these two edits.

## License

Boost.Context is distributed under the **Boost Software License 1.0**. Keep the
license header at the top of each `.asm` file intact, and place the full license
text in `../LICENSE` (i.e. `src/context/LICENSE`).
