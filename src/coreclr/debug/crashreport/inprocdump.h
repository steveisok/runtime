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

// ---------------------------------------------------------------------------
// Dump type
// ---------------------------------------------------------------------------

enum InProcDumpType
{
    InProcDumpType_None = 0,
    InProcDumpType_Mini = 1,    // Thread stacks + registers + modules
    InProcDumpType_Full = 2,    // All readable memory
};

// ---------------------------------------------------------------------------
// Per-thread info — registers collected from ucontext (crash thread)
//                   or ptrace/Mach APIs (other threads)
// ---------------------------------------------------------------------------

struct InProcThreadInfo
{
    uint64_t tid;
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

#if defined(__linux__)
    // Auxiliary vector
    uint8_t auxv[4096];
    int     auxvSize;

    // File descriptor for /proc/self/mem (opened at init)
    int     fdMem;
#endif

    // Dump type
    enum InProcDumpType dumpType;

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
