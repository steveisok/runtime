# Handoff: issue #126081 — DAC AMD64 unwinder null-pointer dereference

**Issue:** https://github.com/dotnet/runtime/issues/126081
**Status of this branch:** investigation + line-mapped native repro harness. No product fix yet.
**Purpose of this doc:** let a fresh session (intended: **vs-debug-mcp on Windows**, where native
debugging is supported) pick this up cold and continue.

---

## 1. TL;DR root cause

The crash is **inside the DAC's AMD64 stack unwinder**, running on vsdbg's `DBI-Callback` thread —
not in the target app. During a stack unwind, a `UWOP_PUSH_NONVOL` step reads memory at
`ContextRecord->Rsp`. When that address resolves to **0**, a DAC "preserve special pointer values"
fast-path returns a **NULL host pointer without throwing**, and the unwinder **dereferences it**,
producing `segfault at 0 ... in libmscordaccore.so`.

The DAC read path is *designed* to throw a catchable `HRESULT` on a bad target read (so the
stackwalk aborts cleanly). The `addr == 0` fast-path silently bypasses that contract. That
asymmetry is the bug.

---

## 2. Evidence (how the root cause was pinned down)

Reproduced the symbolication from the exact shipped binary the reporter hit:

- Runtime pack: `Microsoft.NETCore.App.Runtime.linux-x64` **9.0.3**
- `libmscordaccore.so` **build-id** `87a6382167fb641b8871d87d4336c8a9b456efd7`
- The reporter's `Code:` bytes match **byte-for-byte** at file offset `0x2057b5`
  (`.text` VMA = file offset + 0x1000, so faulting VMA = `0x2067b5`).
- Symbols pulled from msdl (`elf-buildid-sym-<build-id>`) and symbolized:

| Address (VMA) | Symbol | Source |
|---|---|---|
| `0x2067b5` (fault) | `OOPStackUnwinderAMD64::UnwindPrologue`, inlining `MemoryRead64` | `src/coreclr/unwinder/amd64/unwinder.cpp:748` / `:30` |
| `0x2067b0` | `__DPtrBase<ULONG64>::operator*` | `src/coreclr/inc/daccess.h:1017` (impl at `:997`) |
| `0x12fe20` (callee) | `DacInstantiateTypeByAddress` → `...Helper` | `src/coreclr/debug/daccess/dacfn.cpp:498` / `:281` |

Faulting instruction decode of the reporter's bytes:
`... call <DacInstantiateTypeByAddress> ; 48 8b 00 = mov rax,[rax] (rax==0) ; ...`
The `call` **returns normally** (no C++ exception unwind), then the deref faults — only the
`addr==0` fast-path returns NULL without throwing.

### The exact code path

```
unwinder.cpp:747   IntegerAddress = (PULONG64)ContextRecord->Rsp;      // Rsp == 0
unwinder.cpp:748   IntegerRegister[OpInfo] = MemoryRead64(IntegerAddress);
unwinder.cpp:30      return *dac_cast<PTR_ULONG64>((TADDR)addr);        // DPtr::operator*
daccess.h:997          return *(type*)DacInstantiateTypeByAddress(m_addr, sizeof(type), true);
dacfn.cpp:289-292        if (!addr || addr == (TADDR)-1) return (PVOID)addr;  // addr==0 -> NULL, NO throw
                       // *(type*)NULL  ==>  segfault at 0
```

### Why it matches every reported symptom

- **x64-only, arm64 fine:** the arm64 unwinder validates stack addresses
  (`VALIDATE_STACK_ADDRESS_EX`, `src/coreclr/unwinder/arm64/unwinder.cpp`) before each read;
  the amd64 unwinder has 14+ **unchecked** `MemoryRead64/128` sites and no validation.
- **`lldb` workaround works:** lldb uses its own DWARF CFI unwinder and never enters this DAC path.
- **Deterministic, identical offset across sessions:** it's a fixed code path, not a race.
- **`justMyCode` / step-filtering / minidump settings have no effect:** none touch the unwinder.
- **Hard to minimally repro / "metadata-specific":** it requires producing an unwind step whose
  read address resolves to 0 — reachable while walking the many native `.so` frames from the
  third-party SDK, not a function of assembly count.

---

## 3. The repro harness

`repro.cpp` is a **self-contained, line-mapped model** (not the real DAC binary — that needs the
reporter's SDK). It reproduces the exact control-flow shape and the exact null-return-then-deref
mechanism, with every modeled function annotated with the real `file:line` it mirrors. It is meant
to be single-stepped in a native debugger and to A/B a candidate fix.

Build:

```
# Windows (Developer prompt or vs-debug-mcp environment)
build.cmd            # -> repro.exe (+ repro.pdb, /Zi /Od for clean line mapping)

# macOS / Linux sanity check
./build.sh           # -> ./repro
```

Run (verified locally on macOS):

| Command | Meaning | Result |
|---|---|---|
| `repro` | `Rsp == 0`, no fix | **crash / access violation** (reproduces the bug) |
| `repro --unmapped` | `Rsp` non-zero but unreadable | **graceful `DacError` throw** — the *intended* behavior |
| `repro --fixed` | `Rsp == 0`, candidate fix on | **graceful throw** instead of crash |

The `--unmapped` vs default contrast is the whole point: a bad-but-nonzero address throws and the
stackwalk aborts cleanly; a zero address crashes. Same read, opposite outcome.

---

## 4. Driving it with vs-debug-mcp on Windows

1. `build.cmd` to produce `repro.exe` + `repro.pdb`.
2. `debugger_launch` on `repro.exe` (no args) with an initial breakpoint at the `switch (UnwindOp)`
   in `UnwindPrologue` (repro.cpp) — or at the `case UWOP_PUSH_NONVOL` line.
3. Step in and watch: `OpInfo`, `ContextRecord->Rsp` (== 0), then step **into** `MemoryRead64` ->
   `DPtr_ULONG64_deref` -> `DacInstantiateTypeByAddress`. Observe it take the
   `if (!addr || addr == (TADDR)-1)` branch and **return NULL** while `throwEx == true`.
4. Step to `return *host;` — the access violation fires (the real `mov rax,[rax]`).
5. Relaunch with `--fixed` to watch the same path throw `HRException` instead and unwind cleanly.

This lets us prototype and validate a fix natively before touching the product build.

> To debug the **real** DAC (not the model), you need a native repro input; the reporter's SDK is
> required for that. A candidate product fix can still be validated by building coreclr + DAC on
> Windows and running against any test that walks a frame with a zero read address.

---

## 5. Candidate fixes (to design/validate next)

Roughly in order of surgical-ness:

1. **Validate the read address in the amd64 unwinder** (mirror arm64's `VALIDATE_STACK_ADDRESS`):
   reject `Rsp == 0` / obviously-bad addresses before `MemoryRead64`, returning a failure HRESULT
   so the stackwalk aborts cleanly. Most localized to the actual divergence from arm64.
2. **Make `MemoryRead64/128` honor the throw contract** for the null/`-1` case (throw `DacError`
   when the instantiate returns NULL under `throwEx`). Fixes every unchecked read site at once.
3. **Change the `dacfn.cpp` fast-path** so `addr == 0` throws when `throwEx` is set. Highest blast
   radius — the `addr==0 -> return addr` behavior is relied upon elsewhere (e.g. `IsValid`,
   optional pointers), so this needs careful auditing and is likely **not** the right layer.

Recommendation: prototype **(1)** and/or **(2)** in the harness first (the `g_applyFix` hook already
models option 2), then port the chosen shape to `unwinder.cpp` / the `MemoryRead*` helpers and add a
DAC unit/stackwalk test that exercises a zero read address.

Open questions worth a look while debugging:
- **Why does `Rsp` become 0 in the first place?** Is an earlier unwind step producing a bogus
  context from a native/non-managed frame? There may be a second, upstream bug beyond "read
  defensively." Walking the frame chain in the real repro (Windows) is the way to answer this.

---

## 6. Files in this folder

- `repro.cpp`   — the line-mapped harness (see header comment for the full annotated root cause).
- `build.cmd`   — Windows build (`/Zi /Od`).
- `build.sh`    — macOS/Linux build (`-g -O0`).
- `HANDOFF.md`  — this document.
