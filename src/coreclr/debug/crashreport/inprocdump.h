// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// In-process core dump writer for Linux/Android (ELF) and Apple mobile (Mach-O).
//
// All code must be async-signal-safe: no malloc, no C++ STL, static buffers only.
// Called from the crash signal handler alongside the JSON crash reporter.

#pragma once

#include <stdint.h>
#include <signal.h>

#if defined(__linux__)
#include <elf.h>
#include <sys/user.h>

// Android/Bionic doesn't define user_fpregs_struct in <sys/user.h>.
// Provide the same aliases that createdump/threadinfo.h uses.
#if defined(__arm__)
#define user_regs_struct  user_regs
#define user_fpregs_struct user_fpregs
#elif defined(__aarch64__)
#define user_fpregs_struct user_fpsimd_struct
#elif defined(__riscv)
struct user_fpregs_struct
{
    unsigned long long fpregs[32];
    unsigned long      fcsr;
} __attribute__((__packed__));
#elif defined(__loongarch64)
#include <asm/sigcontext.h>
#define user_fpregs_struct lasx_context
#endif

#include <sys/procfs.h>

#if defined(__ANDROID__)
// Android's <sys/procfs.h> provides elf_siginfo, elf_gregset_t, and
// ELF_PRARGSZ but does NOT define elf_prstatus/prstatus_t or
// elf_prpsinfo/prpsinfo_t.  Define them here — these are ABI-stable
// ELF core note structures consumed only by debuggers.
#include <sys/time.h>

struct elf_prstatus
{
    struct elf_siginfo pr_info;
    short              pr_cursig;
    unsigned long      pr_sigpend;
    unsigned long      pr_sighold;
    pid_t              pr_pid;
    pid_t              pr_ppid;
    pid_t              pr_pgrp;
    pid_t              pr_sid;
    struct timeval     pr_utime;
    struct timeval     pr_stime;
    struct timeval     pr_cutime;
    struct timeval     pr_cstime;
    elf_gregset_t      pr_reg;
    int                pr_fpvalid;
};
typedef struct elf_prstatus prstatus_t;

struct elf_prpsinfo
{
    char               pr_state;
    char               pr_sname;
    char               pr_zomb;
    char               pr_nice;
    unsigned long      pr_flag;
#if defined(__LP64__)
    unsigned int       pr_uid;
    unsigned int       pr_gid;
#else
    unsigned short     pr_uid;
    unsigned short     pr_gid;
#endif
    pid_t              pr_pid;
    pid_t              pr_ppid;
    pid_t              pr_pgrp;
    pid_t              pr_sid;
    char               pr_fname[16];
    char               pr_psargs[ELF_PRARGSZ];
};
typedef struct elf_prpsinfo prpsinfo_t;
#endif // __ANDROID__

#elif defined(__APPLE__)
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/thread_status.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Static limits — truncation-safe, never overflow
// ---------------------------------------------------------------------------

#define INPROC_MAX_THREADS          256
#define INPROC_MAX_MEMORY_REGIONS   8192
#define INPROC_MAX_MODULES          512
#define INPROC_MAX_MODULE_NAME      512
#define INPROC_MAX_PROCESS_NAME     16
// I/O buffer for reading memory and writing to disk
#define INPROC_IO_BUFFER_SIZE       (16 * 1024)
// Maximum stack region to include in mini dump
#define INPROC_MAX_STACK_SIZE       (8 * 1024 * 1024)
// Maximum dyld memory ranges to include in mini dump for SOS module enumeration.
// We include specific address ranges (dyld __TEXT, symtab, strtab, image info)
// rather than full memory regions because dyld is in the shared cache and its
// __LINKEDIT region is ~600 MB.
#define INPROC_MAX_DYLD_RANGES      8

// Maximum number of module memory ranges to include in mini dump.
// Includes header pages (clrmd throws if unreadable) and path string pages
// (SOS identifies the runtime by module name). With ~500 modules × 2 ranges each,
// deduplication typically reduces the count well below this limit.
#define INPROC_MAX_MODULE_HEADERS   1536

// Maximum number of managed debug memory pages for Tier 2 (clrstack support).
// Covers: ThreadStore, Thread objects, code heaps, nibble maps, RangeSectionMap
// levels/fragments, RangeSections, MethodDescs, MethodDescChunks, MethodTables,
// Modules, and PE metadata sections. With dedup, typically ~1000-3000 entries.
#define INPROC_MAX_MANAGED_DEBUG_PAGES  4096

// Maximum return addresses to scan per thread's stack for IP resolution.
#define INPROC_MAX_STACK_IPS        64

// ---------------------------------------------------------------------------
// SpecialDiagInfo — synthetic dump segment for SOS/dotnet-dump discovery
// ---------------------------------------------------------------------------
// Must match: https://github.com/dotnet/diagnostics/blob/main/src/SOS/inc/specialdiaginfo.h
// and src/coreclr/debug/createdump/specialdiaginfo.h in this repo.

#define SPECIAL_DIAGINFO_SIGNATURE  "DIAGINFOHEADER"
#define SPECIAL_DIAGINFO_VERSION    2
#define SPECIAL_DIAGINFO_SIZE       0x1000

#ifdef __APPLE__
#define SPECIAL_DIAGINFO_ADDRESS    0x7fffffff10000000ULL
#elif defined(__LP64__)
#define SPECIAL_DIAGINFO_ADDRESS    0x00007ffffff10000ULL
#else
#define SPECIAL_DIAGINFO_ADDRESS    0x7fff1000ULL
#endif

struct InProcSpecialDiagInfoHeader
{
    char     Signature[16];
    int32_t  Version;
    uint64_t ExceptionRecordAddress;
    uint64_t RuntimeBaseAddress;
};

// ---------------------------------------------------------------------------
// SpecialThreadInfo — synthetic dump segment mapping thread IDs to SPs
// ---------------------------------------------------------------------------
// Must match: https://github.com/dotnet/diagnostics/blob/main/src/SOS/inc/specialthreadinfo.h
// and src/coreclr/debug/createdump/specialthreadinfo.h in this repo.
// Mach-O LC_THREAD commands have no thread ID field, so clrmd/SOS uses this
// segment to map dump thread indices to OS thread IDs for register lookups.

#define SPECIAL_THREADINFO_SIGNATURE  "THREADINFO"
#define SPECIAL_THREADINFO_ADDRESS    0x7fffffff00000000ULL

struct InProcSpecialThreadInfoHeader
{
    char     signature[16];
    uint32_t pid;
    uint32_t numThreads;
};

struct InProcSpecialThreadInfoEntry
{
    uint32_t tid;
    uint64_t sp;
};

// ---------------------------------------------------------------------------
// Dyld memory range — a specific address range to include in mini dumps
// ---------------------------------------------------------------------------

struct InProcDyldRange
{
    uint64_t addr;
    uint64_t size;
};

enum InProcDumpType
{
    InProcDumpType_None = 0,
    InProcDumpType_Mini = 1,    // Thread stacks + registers + modules
    InProcDumpType_Full = 2,    // All readable memory
};

// ---------------------------------------------------------------------------
// VM-provided offsets and addresses for managed debug page capture (Tier 2).
// Populated at VM startup by InProcDump_InitManagedDebug(), consumed at crash
// time to capture memory pages needed for clrthreads/clrstack in the dump.
// ---------------------------------------------------------------------------

struct InProcManagedDebugInfo
{
    int initialized;

    // Global root addresses (in runtime data segment, already captured)
    uint64_t threadStoreAddr;       // Address of s_pThreadStore pointer
    uint64_t eeJitManagerAddr;      // Address of m_pEEJitManager pointer
    uint64_t rangeSectionMapAddr;   // Address of g_codeRangeMap data

    // ThreadStore → Thread linked list offsets
    uint32_t threadStore_FirstThreadLink;
    uint32_t threadStore_ThreadCount;
    uint32_t thread_Link;
    uint32_t thread_OSId;
    uint32_t thread_State;
    uint32_t thread_RuntimeThreadLocals;
    uint32_t thread_ExposedObject;
    uint32_t thread_LastThrownObject;

    // EEJitManager → HeapList chain offsets
    uint32_t eeJitManager_AllCodeHeaps;
    uint32_t heapList_Next;
    uint32_t heapList_StartAddress;
    uint32_t heapList_EndAddress;
    uint32_t heapList_MapBase;
    uint32_t heapList_HeaderMap;

    // RangeSectionMap offsets
    uint32_t rangeSectionMap_TopLevelData;

    // RangeSectionFragment offsets
    uint32_t fragment_RangeBegin;
    uint32_t fragment_RangeEndOpen;
    uint32_t fragment_RangeSection;
    uint32_t fragment_Next;

    // RangeSection offsets
    uint32_t rangeSection_RangeBegin;
    uint32_t rangeSection_RangeEndOpen;
    uint32_t rangeSection_Flags;
    uint32_t rangeSection_HeapList;
    uint32_t rangeSection_R2RModule;

    // RealCodeHeader offset
    uint32_t realCodeHeader_MethodDesc;

    // MethodDesc → MethodDescChunk computation
    uint32_t methodDesc_ChunkIndex;
    uint32_t methodDesc_Alignment;
    uint32_t methodDescChunk_Size;
    uint32_t methodDescChunk_MethodTable;

    // MethodTable → Module
    uint32_t methodTable_Module;

    // Module → PE base
    uint32_t module_Base;
};

// ---------------------------------------------------------------------------
// Per-thread info — registers collected from ucontext (crash thread)
//                   or ptrace/Mach APIs (other threads)
// ---------------------------------------------------------------------------

struct InProcThreadInfo
{
    uint64_t tid;           // Mach port (Apple) or TID (Linux)
    uint64_t pthreadId;     // pthread-level thread ID (Apple only, for SpecialThreadInfo)
    int      isCrashThread;
    int      hasGPRegs;
    int      hasFPRegs;

#if defined(__linux__)
    struct user_regs_struct gpRegs;
    struct user_fpregs_struct fpRegs;
#elif defined(__APPLE__) && defined(__aarch64__)
    arm_thread_state64_t gpRegs;
    arm_neon_state64_t   fpRegs;
#elif defined(__APPLE__) && defined(__x86_64__)
    x86_thread_state64_t gpRegs;
    x86_float_state64_t  fpRegs;
#endif
};

// ---------------------------------------------------------------------------
// Memory region from /proc/self/maps (Linux) or mach_vm_region (Apple)
// ---------------------------------------------------------------------------

struct InProcMemoryRegion
{
    uint64_t start;
    uint64_t end;
    uint32_t flags;     // PF_R | PF_W | PF_X (Linux) or VM_PROT_* (Apple)
    uint64_t offset;    // File offset (Linux /proc/self/maps)
    char     name[INPROC_MAX_MODULE_NAME];
};

// ---------------------------------------------------------------------------
// Module info — subset of memory regions that are named (shared libraries, etc.)
// ---------------------------------------------------------------------------

struct InProcModuleInfo
{
    uint64_t start;
    uint64_t end;
    uint64_t offset;
    char     name[INPROC_MAX_MODULE_NAME];
};

// ---------------------------------------------------------------------------
// Collected dump state — passed to the platform-specific writer
// ---------------------------------------------------------------------------

struct InProcDumpState
{
    // Process info
    int      pid;
    int      ppid;
    int      tgid;
    char     processName[INPROC_MAX_PROCESS_NAME];

    // Signal info (crash context)
    int      signal;
    siginfo_t siginfo;

    // Threads
    struct InProcThreadInfo threads[INPROC_MAX_THREADS];
    int threadCount;
    int crashThreadIndex;    // Index into threads[] for the crashing thread

    // Memory regions (all from /proc/self/maps or mach_vm_region)
    struct InProcMemoryRegion regions[INPROC_MAX_MEMORY_REGIONS];
    int regionCount;

    // Modules (named regions — for NT_FILE / informational)
    struct InProcModuleInfo modules[INPROC_MAX_MODULES];
    int moduleCount;

    // Stack region for the crashing thread (found from regions[])
    int stackRegionIndex;    // Index into regions[], or -1

    // Region containing the runtime module (for mini dumps so SOS can validate the base)
    int runtimeRegionIndex;  // Index into regions[], or -1

    // Dyld memory ranges for mini dumps (SOS/clrmd module enumeration).
    // These are specific address ranges (not region indices) because dyld's
    // segments live in the shared cache where full regions are hundreds of MB.
    struct InProcDyldRange dyldRanges[INPROC_MAX_DYLD_RANGES];
    int dyldRangeCount;

    // Module header pages for mini dumps.
    // clrmd throws when a module's Mach-O header can't be read, aborting enumeration.
    // We include one 4 KB page per loaded module so all headers are available.
    struct InProcDyldRange moduleHeaders[INPROC_MAX_MODULE_HEADERS];
    int moduleHeaderCount;

    // Managed debug pages for Tier 2 (clrstack/clrthreads support).
    // Captured at crash time by walking runtime data structures (ThreadStore,
    // code heaps, RangeSectionMap, MethodDescs, etc.) using compile-time offsets
    // provided by the VM at startup.
    struct InProcDyldRange managedDebugPages[INPROC_MAX_MANAGED_DEBUG_PAGES];
    int managedDebugPageCount;

#if defined(__linux__)
    // Auxiliary vector
    uint8_t auxv[4096];
    int     auxvSize;

    // File descriptor for /proc/self/mem (opened at init)
    int     fdMem;
#endif

    // Dump type
    enum InProcDumpType dumpType;

    // Runtime base address for SpecialDiagInfo (SOS discovery)
    uint64_t runtimeBaseAddress;

    // Truncation flags
    int truncatedThreads;    // 1 if we hit MAX_THREADS
    int truncatedRegions;    // 1 if we hit MAX_MEMORY_REGIONS
    int truncatedModules;    // 1 if we hit MAX_MODULES
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Initialize at startup — reads env vars, opens /proc/self/mem.
// Called from PROCAbortInitialize().
void InProcDump_Initialize(void);

// Provide VM struct offsets and global addresses for managed debug capture.
// Called from VM startup (EEStartupHelper) after runtime data structures are initialized.
void InProcDump_InitManagedDebug(const struct InProcManagedDebugInfo* info);

// Returns the configured dump type, or InProcDumpType_None if disabled.
enum InProcDumpType InProcDump_GetDumpType(void);

// Generate a core dump from the signal handler.
// Called from PROCCreateCrashDumpIfEnabled() when in-process reporting is enabled.
void InProcDump_Generate(int signal, siginfo_t* siginfo, void* context);

// ---------------------------------------------------------------------------
// Platform-specific writers (called by InProcDump_Generate)
// ---------------------------------------------------------------------------

#if defined(__linux__)
// Write an ELF core dump from the collected state.
int InProcDumpElf_Write(struct InProcDumpState* state, const char* path);
#endif

#if defined(__APPLE__)
// Write a Mach-O core dump from the collected state.
int InProcDumpMachO_Write(struct InProcDumpState* state, const char* path);
#endif

// ---------------------------------------------------------------------------
// Platform-specific collection helpers (called by InProcDump_Generate)
// ---------------------------------------------------------------------------

#if defined(__linux__)
// Parse /proc/self/maps into state->regions[] and state->modules[].
int InProcDumpMaps_Collect(struct InProcDumpState* state);

// Read /proc/self/auxv into state->auxv[].
int InProcDumpMaps_CollectAuxv(struct InProcDumpState* state);
#endif

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

// Async-signal-safe write: loops on short writes, returns 0 on success.
int InProcDump_WriteAll(int fd, const void* buf, size_t len);

// Find the memory region containing the given address.
// Returns the index into state->regions[], or -1 if not found.
int InProcDump_FindRegion(const struct InProcDumpState* state, uint64_t addr);

// Compute the dump window for a stack region anchored around SP.
// On downward-growing stacks, active frames are near SP (high addresses),
// so we keep the window containing SP rather than the start of the VMA.
// Sets *dumpStart and *dumpSize. Always within [region.start, region.end].
void InProcDump_ClipStackRegion(const struct InProcMemoryRegion* region,
                                uint64_t sp,
                                uint64_t* dumpStart, size_t* dumpSize);

#ifdef __cplusplus
}
#endif
