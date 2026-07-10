// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//
// Self-contained, line-mapped repro harness for dotnet/runtime issue #126081:
//   "libmscordaccore.so null pointer dereference in DBI-Callback thread on x64 Linux".
//
// This is NOT the real DAC binary. It is a faithful *model* that reproduces the exact
// control-flow shape and the exact null-return-then-dereference mechanism observed in
// the shipped 9.0.x libmscordaccore, so it can be single-stepped in a native debugger
// (e.g. vs-debug-mcp on Windows) and used to validate a candidate fix. Every modeled
// function is annotated with the real source file:line it mirrors.
//
// ------------------------------------------------------------------------------------
// ROOT CAUSE (confirmed by symbolizing the shipped 9.0.3 linux-x64 DAC,
//             build-id 87a6382167fb641b8871d87d4336c8a9b456efd7):
//
//   Fault site : OOPStackUnwinderAMD64::UnwindPrologue
//                src/coreclr/unwinder/amd64/unwinder.cpp:748  (case UWOP_PUSH_NONVOL)
//                inlining MemoryRead64 -> src/coreclr/unwinder/amd64/unwinder.cpp:30
//                which is  *dac_cast<PTR_ULONG64>(addr)  ==  DPtr::operator*
//                src/coreclr/inc/daccess.h:997
//   Callee     : DacInstantiateTypeByAddress -> ...Helper
//                src/coreclr/debug/daccess/dacfn.cpp:281
//
//   The read path is DESIGNED to throw (DacError -> EX_THROW(HRException)) on a bad
//   target read, so a corrupt unwind context aborts the stackwalk with an HRESULT.
//   BUT the helper's first check "preserve special pointer values":
//
//        if (!addr || addr == (TADDR)-1) return (PVOID)addr;   // dacfn.cpp:289-292
//
//   returns a NULL *host* pointer for a target address of 0 WITHOUT throwing, even
//   though throwEx == true. operator* then unconditionally dereferences it:
//
//        return *(type*)DacInstantiateTypeByAddress(m_addr, sizeof(type), true); // daccess.h:997
//
//   => when ContextRecord->Rsp resolves to 0 during a UWOP_PUSH_NONVOL unwind step,
//      the "throw on failure" contract is silently bypassed and the DAC dereferences
//      NULL on its own DBI-Callback thread: "segfault at 0 ... in libmscordaccore.so".
//
//   Why arm64/lldb don't repro:
//     * The arm64 unwinder validates stack addresses (VALIDATE_STACK_ADDRESS_EX,
//       src/coreclr/unwinder/arm64/unwinder.cpp) before every read.
//     * lldb uses its own DWARF CFI unwinder and never enters this DAC path.
// ------------------------------------------------------------------------------------
//
// Usage:
//   repro                 Rsp == 0  and no fix  -> reproduces the null deref (crash / AV).
//   repro --unmapped      Rsp == non-zero unreadable -> the INTENDED behavior: a DacError
//                         throw, i.e. the stackwalk aborts gracefully. Demonstrates the
//                         asymmetry that IS the bug.
//   repro --fixed         Rsp == 0 but with the candidate fix applied -> graceful throw
//                         instead of a crash. A/B this against the default run.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

typedef uint64_t ULONG64;
typedef uint32_t ULONG;
typedef uint16_t USHORT;
typedef uint8_t  UCHAR;
typedef uintptr_t TADDR;

// ---- Unwind structures: copied verbatim from src/coreclr/inc/win64unwind.h ----------

enum _UNWIND_OP_CODES {
    UWOP_PUSH_NONVOL = 0,
    UWOP_ALLOC_LARGE,
    UWOP_ALLOC_SMALL,
    UWOP_SET_FPREG,
    UWOP_SAVE_NONVOL,
    UWOP_SAVE_NONVOL_FAR,
    UWOP_EPILOG,
    UWOP_SPARE_CODE,
    UWOP_SAVE_XMM128,
    UWOP_SAVE_XMM128_FAR,
    UWOP_PUSH_MACHFRAME,
    UWOP_SET_FPREG_LARGE,
};

typedef union _UNWIND_CODE {
    struct {
        UCHAR CodeOffset;
        UCHAR UnwindOp : 4;
        UCHAR OpInfo : 4;
    };
    USHORT FrameOffset;
} UNWIND_CODE;

typedef struct _UNWIND_INFO {
    UCHAR Version : 3;
    UCHAR Flags : 5;
    UCHAR SizeOfProlog;
    UCHAR CountOfUnwindCodes;
    UCHAR FrameRegister : 4;
    UCHAR FrameOffset : 4;
    UNWIND_CODE UnwindCode[8];
} UNWIND_INFO;

// Minimal stand-in for the parts of _CONTEXT that UnwindPrologue touches.
struct Context {
    ULONG64 Rip;
    ULONG64 Rsp;
    ULONG64 Reg[16]; // integer register file; IntegerRegister points at Reg[0]
};

// ---- Harness-controlled state --------------------------------------------------------

static std::unordered_map<TADDR, ULONG64> g_targetMem; // simulated readable target memory
static bool g_applyFix = false;

// ---- DAC model -----------------------------------------------------------------------
// Mirrors src/coreclr/debug/daccess/dacfn.cpp : DacError -> EX_THROW(HRException).
struct HRException { long hr; };
[[noreturn]] static void DacError(long hr) { throw HRException{ hr }; }

// Mirrors src/coreclr/debug/daccess/dacfn.cpp:281 DacInstantiateTypeByAddressHelper
// (called via DacInstantiateTypeByAddress, dacfn.cpp:400, throwEx == true from operator*).
static void* DacInstantiateTypeByAddress(TADDR addr, uint32_t size, bool throwEx)
{
    (void)size;
    // dacfn.cpp:289-292  "Preserve special pointer values."
    // *** THE BUG ***: for addr==0 this returns a NULL host pointer and does NOT throw,
    // regardless of throwEx.
    if (!addr || addr == (TADDR)-1)
    {
        return (void*)addr;
    }

    // Model of DacReadAll (dacfn.cpp:369): is the target address actually readable?
    auto it = g_targetMem.find(addr);
    if (it == g_targetMem.end())
    {
        // dacfn.cpp:371-376  DacReadAll FAILED -> DacError(status) when throwEx.
        if (throwEx)
        {
            DacError(0x80131c40 /* CORDBG_E_READVIRTUAL_FAILURE (illustrative) */);
        }
        return nullptr;
    }

    // Return a host-side copy of the target value (leaked; fine for a repro).
    return new ULONG64(it->second);
}

// Mirrors src/coreclr/inc/daccess.h:997  __DPtrBase<ULONG64>::operator*()
//   return *(type*)DacInstantiateTypeByAddress(m_addr, sizeof(type), true);
static ULONG64 DPtr_ULONG64_deref(TADDR addr)
{
    ULONG64* host = (ULONG64*)DacInstantiateTypeByAddress(addr, sizeof(ULONG64), /*throwEx*/ true);

    // ---- CANDIDATE FIX (one option) -------------------------------------------------
    // Make the read honor its throw-on-failure contract even for the addr==0 fast-path.
    // Equivalent alternatives: (a) validate Rsp in UnwindPrologue like arm64 does, or
    // (b) make the dacfn.cpp addr==0 fast-path throw when throwEx is set.
    if (g_applyFix && host == nullptr)
    {
        DacError(0x80131c40);
    }
    // ---------------------------------------------------------------------------------

    return *host; // <-- src/coreclr/unwinder/amd64/unwinder.cpp:30 MemoryRead64 body.
                  //     Faults here (mov rax,[rax] with rax==0) when host == NULL.
}

// Mirrors src/coreclr/unwinder/amd64/unwinder.cpp:28  MemoryRead64
static ULONG64 MemoryRead64(ULONG64* addr)
{
    return DPtr_ULONG64_deref((TADDR)addr);
}

// ---- Trimmed mirror of OOPStackUnwinderAMD64::UnwindPrologue -------------------------
// (src/coreclr/unwinder/amd64/unwinder.cpp, the UWOP_PUSH_NONVOL arm at :736-755)
static long UnwindPrologue(Context* ContextRecord, UNWIND_INFO* UnwindInfo)
{
    ULONG64* IntegerRegister = ContextRecord->Reg;
    ULONG    Index = 0;
    ULONG    UnwindOp = UnwindInfo->UnwindCode[Index].UnwindOp;
    ULONG    OpInfo   = UnwindInfo->UnwindCode[Index].OpInfo;

    switch (UnwindOp)                                             // unwinder.cpp:736
    {
    case UWOP_PUSH_NONVOL:                                        // unwinder.cpp:746
    {
        ULONG64* IntegerAddress = (ULONG64*)ContextRecord->Rsp;  // unwinder.cpp:747
        IntegerRegister[OpInfo] = MemoryRead64(IntegerAddress);  // unwinder.cpp:748  <-- crash origin
        ContextRecord->Rsp += 8;                                 // unwinder.cpp:754
        break;
    }
    default:
        break;
    }
    return 0; // S_OK
}

int main(int argc, char** argv)
{
    bool unmapped = false;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--fixed")         g_applyFix = true;
        else if (a == "--unmapped") unmapped = true;
        else { printf("unknown arg: %s\n", argv[i]); return 2; }
    }

    Context ctx{};
    // The observed crash condition: Rsp resolves to 0 during the unwind step.
    // --unmapped instead uses a non-zero, unreadable target address to show the
    // INTENDED graceful-throw behavior (the contrast that pinpoints the bug).
    ctx.Rsp = unmapped ? (ULONG64)0xdeadbeef0000ULL : (ULONG64)0;

    UNWIND_INFO ui{};
    ui.Version = 1;
    ui.CountOfUnwindCodes = 1;
    ui.UnwindCode[0].CodeOffset = 0;
    ui.UnwindCode[0].UnwindOp   = UWOP_PUSH_NONVOL;
    ui.UnwindCode[0].OpInfo     = 0; // RAX

    printf("scenario: Rsp=0x%llx  fix=%s\n",
           (unsigned long long)ctx.Rsp, g_applyFix ? "on" : "off");
    printf("driving UnwindPrologue / UWOP_PUSH_NONVOL ...\n");
    fflush(stdout);

    try
    {
        UnwindPrologue(&ctx, &ui);
        printf("=> UnwindPrologue returned S_OK, RAX=0x%llx (no crash)\n",
               (unsigned long long)ctx.Reg[0]);
    }
    catch (const HRException& e)
    {
        printf("=> caught DacError HRESULT=0x%lx : stackwalk would abort GRACEFULLY\n", e.hr);
        return 0;
    }
    return 0;
}
