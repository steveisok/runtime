// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// In-process core dump orchestrator.
// Collects thread registers, memory regions, and delegates to the
// platform-specific writer (ELF on Linux/Android, Mach-O on Apple).
//
// All code is async-signal-safe.

// The PAL static library is linked into both libcoreclr.so and libclrjit.so.
// Guard the implementation to avoid pulling a multi-MB static buffer into the JIT.
#ifdef JIT_STANDALONE_BUILD
// Intentionally empty — in-process dump is not used in the JIT.
#elif !defined(__linux__) && !defined(__APPLE__)
// Platform not supported.
#else

#include "inprocdump.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>

#if defined(__linux__)
#include <sys/types.h>
#include <sys/syscall.h>
#endif

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/thread_act.h>
#include <mach/vm_map.h>
#endif

// ---------------------------------------------------------------------------
// Static state — initialized at startup, used in signal handler
// ---------------------------------------------------------------------------

static enum InProcDumpType s_dumpType = InProcDumpType_None;
static char s_dumpPath[1024];
static int s_initialized = 0;

// CAS guard: ensures only one thread generates the dump.
static volatile int s_dumpInProgress = 0;

#if defined(__linux__)
// Pre-opened fd for /proc/self/mem — stored separately so memset of s_state
// doesn't lose it.
static int s_fdMem = -1;
#endif

// The dump state is large (~multi-MB) and must be static (no malloc in handler).
// Only one crash can be in progress at a time (ensured by CAS above).
static struct InProcDumpState s_state;

// ---------------------------------------------------------------------------
// Initialization — called at startup, not from signal handler
// ---------------------------------------------------------------------------

void InProcDump_Initialize(void)
{
    if (s_initialized) return;
    s_initialized = 1;

    // Check if in-process crash report is enabled
    const char* enable = getenv("DOTNET_EnableInProcessCrashReport");
    if (enable == NULL || enable[0] != '1')
    {
        s_dumpType = InProcDumpType_None;
        return;
    }

    // Check dump type (default: mini)
    const char* typeStr = getenv("DOTNET_InProcessDumpType");
    if (typeStr != NULL)
    {
        if (typeStr[0] == 'f' || typeStr[0] == 'F' || typeStr[0] == '2')
            s_dumpType = InProcDumpType_Full;
        else
            s_dumpType = InProcDumpType_Mini;
    }
    else
    {
        s_dumpType = InProcDumpType_Mini;
    }

    // Resolve output path at init time, not in handler
    const char* pathEnv = getenv("DOTNET_DbgMiniDumpName");
    if (pathEnv != NULL && pathEnv[0] != '\0')
    {
        // Copy the path, replacing %d with pid and %t with timestamp would be
        // complex in signal handler. For now, use as-is.
        int i = 0;
        while (pathEnv[i] && i < (int)sizeof(s_dumpPath) - 1)
        {
            s_dumpPath[i] = pathEnv[i];
            i++;
        }
        s_dumpPath[i] = '\0';
    }
    else
    {
        // If no explicit path, try TEST_RESULTS_DIR (set by xharness on Android/iOS).
        // This ensures the dump lands in the directory xharness pulls back to the host.
        const char* resultsDir = getenv("TEST_RESULTS_DIR");
        if (resultsDir != NULL && resultsDir[0] != '\0')
        {
            int i = 0;
            while (resultsDir[i] && i < (int)sizeof(s_dumpPath) - 32)
            {
                s_dumpPath[i] = resultsDir[i];
                i++;
            }
            const char* suffix = "/crash.coredump";
            int j = 0;
            while (suffix[j] && i < (int)sizeof(s_dumpPath) - 1)
                s_dumpPath[i++] = suffix[j++];
            s_dumpPath[i] = '\0';
        }
        else
        {
            s_dumpPath[0] = '\0';
        }
    }

#if defined(__linux__)
    // Open /proc/self/mem now — may not be openable in some signal states.
    // Store in separate static so memset of s_state doesn't lose it.
    s_fdMem = open("/proc/self/mem", O_RDONLY);
#endif
}

enum InProcDumpType InProcDump_GetDumpType(void)
{
    return s_dumpType;
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

int InProcDump_WriteAll(int fd, const void* buf, size_t len)
{
    const char* p = (const char*)buf;
    while (len > 0)
    {
        ssize_t n = write(fd, p, len);
        if (n < 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int InProcDump_FindRegion(const struct InProcDumpState* state, uint64_t addr)
{
    for (int i = 0; i < state->regionCount; i++)
    {
        if (addr >= state->regions[i].start && addr < state->regions[i].end)
            return i;
    }
    return -1;
}

void InProcDump_ClipStackRegion(const struct InProcMemoryRegion* region,
                                uint64_t sp,
                                uint64_t* dumpStart, size_t* dumpSize)
{
    size_t fullSize = region->end - region->start;
    if (fullSize <= INPROC_MAX_STACK_SIZE)
    {
        // Whole region fits — dump it all
        *dumpStart = region->start;
        *dumpSize = fullSize;
        return;
    }

    // Region is larger than max. Anchor the window so SP is included.
    // On downward-growing stacks, active frames are between SP and region.end.
    // Include everything from (SP - some margin) to region.end.
    uint64_t margin = INPROC_MAX_STACK_SIZE / 4;  // 2MB below SP for locals
    uint64_t windowStart = (sp > margin) ? (sp - margin) : 0;
    if (windowStart < region->start)
        windowStart = region->start;

    size_t windowSize = region->end - windowStart;
    if (windowSize > INPROC_MAX_STACK_SIZE)
    {
        windowStart = region->end - INPROC_MAX_STACK_SIZE;
        if (windowStart < region->start)
            windowStart = region->start;
        windowSize = region->end - windowStart;
    }

    *dumpStart = windowStart;
    *dumpSize = windowSize;
}

// ---------------------------------------------------------------------------
// Async-signal-safe integer-to-string for building the dump path
// ---------------------------------------------------------------------------

static int IntToStr(int val, char* buf, int bufSize)
{
    if (bufSize < 2) return 0;

    char tmp[32];
    int i = 0;
    int neg = 0;

    if (val < 0) { neg = 1; val = -val; }

    do
    {
        tmp[i++] = '0' + (val % 10);
        val /= 10;
    } while (val > 0 && i < (int)sizeof(tmp));

    int pos = 0;
    if (neg && pos < bufSize - 1) buf[pos++] = '-';
    while (i > 0 && pos < bufSize - 1)
        buf[pos++] = tmp[--i];
    buf[pos] = '\0';
    return pos;
}

// ---------------------------------------------------------------------------
// Async-signal-safe stderr output
// ---------------------------------------------------------------------------

static void WriteStderr(const char* msg)
{
    if (msg == NULL) return;
    int len = 0;
    while (msg[len]) len++;
    write(STDERR_FILENO, msg, len);
}

// ---------------------------------------------------------------------------
// Build the dump file path
// ---------------------------------------------------------------------------

static void BuildDumpPath(char* path, int pathSize, int pid)
{
    if (s_dumpPath[0] != '\0')
    {
        // Use user-specified path
        int i = 0;
        while (s_dumpPath[i] && i < pathSize - 1)
        {
            path[i] = s_dumpPath[i];
            i++;
        }
        path[i] = '\0';
        return;
    }

    // Build default path: /tmp/dotnet_crash_<pid>.coredump
    const char* tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL || tmpdir[0] == '\0')
        tmpdir = "/tmp";

    int pos = 0;
    int i = 0;
    while (tmpdir[i] && pos < pathSize - 1)
        path[pos++] = tmpdir[i++];

    const char* prefix = "/dotnet_crash_";
    i = 0;
    while (prefix[i] && pos < pathSize - 1)
        path[pos++] = prefix[i++];

    char pidStr[16];
    IntToStr(pid, pidStr, sizeof(pidStr));
    i = 0;
    while (pidStr[i] && pos < pathSize - 1)
        path[pos++] = pidStr[i++];

    const char* suffix = ".coredump";
    i = 0;
    while (suffix[i] && pos < pathSize - 1)
        path[pos++] = suffix[i++];

    path[pos] = '\0';
}

// ---------------------------------------------------------------------------
// Linux: Collect crashing thread registers from ucontext
// ---------------------------------------------------------------------------

#if defined(__linux__)

static void CollectCrashThread(struct InProcDumpState* state, int signal, siginfo_t* siginfo, void* context)
{
    state->signal = signal;
    if (siginfo != NULL)
        memcpy(&state->siginfo, siginfo, sizeof(siginfo_t));

    state->pid = (int)getpid();
    state->ppid = (int)getppid();
    state->tgid = state->pid;

    // Read process name from /proc/self/comm
    {
        int fd = open("/proc/self/comm", O_RDONLY);
        if (fd >= 0)
        {
            int n = (int)read(fd, state->processName, sizeof(state->processName) - 1);
            if (n > 0)
            {
                // Strip trailing newline
                if (state->processName[n - 1] == '\n') n--;
                state->processName[n] = '\0';
            }
            close(fd);
        }
    }

    // Extract registers from ucontext_t
    struct InProcThreadInfo* crash = &state->threads[0];
    crash->tid = (uint64_t)syscall(SYS_gettid);
    crash->isCrashThread = 1;

    if (context != NULL)
    {
        ucontext_t* uc = (ucontext_t*)context;

#if defined(__aarch64__)
        // ARM64 GP registers
        memcpy(&crash->gpRegs, &uc->uc_mcontext.regs[0], sizeof(crash->gpRegs));
        crash->hasGPRegs = 1;

        // ARM64 FP registers (FPSIMD context in __reserved area)
        // Note: fpsimd_context layout is {head, fpsr, fpcr, vregs[32]} but
        // user_fpregs_struct layout is {vregs[32], fpsr, fpcr} — must copy fields individually.
        struct _aarch64_ctx* head = (struct _aarch64_ctx*)&uc->uc_mcontext.__reserved[0];
        while (head->magic != 0)
        {
            if (head->magic == FPSIMD_MAGIC)
            {
                struct fpsimd_context* fpctx = (struct fpsimd_context*)head;
                memcpy(&crash->fpRegs.vregs, &fpctx->vregs, sizeof(fpctx->vregs));
                crash->fpRegs.fpsr = fpctx->fpsr;
                crash->fpRegs.fpcr = fpctx->fpcr;
                crash->hasFPRegs = 1;
                break;
            }
            head = (struct _aarch64_ctx*)((char*)head + head->size);
        }

#elif defined(__x86_64__)
        // x86_64 GP registers: fill user_regs_struct from ucontext
        crash->gpRegs.r15 = uc->uc_mcontext.gregs[REG_R15];
        crash->gpRegs.r14 = uc->uc_mcontext.gregs[REG_R14];
        crash->gpRegs.r13 = uc->uc_mcontext.gregs[REG_R13];
        crash->gpRegs.r12 = uc->uc_mcontext.gregs[REG_R12];
        crash->gpRegs.rbp = uc->uc_mcontext.gregs[REG_RBP];
        crash->gpRegs.rbx = uc->uc_mcontext.gregs[REG_RBX];
        crash->gpRegs.r11 = uc->uc_mcontext.gregs[REG_R11];
        crash->gpRegs.r10 = uc->uc_mcontext.gregs[REG_R10];
        crash->gpRegs.r9 = uc->uc_mcontext.gregs[REG_R9];
        crash->gpRegs.r8 = uc->uc_mcontext.gregs[REG_R8];
        crash->gpRegs.rax = uc->uc_mcontext.gregs[REG_RAX];
        crash->gpRegs.rcx = uc->uc_mcontext.gregs[REG_RCX];
        crash->gpRegs.rdx = uc->uc_mcontext.gregs[REG_RDX];
        crash->gpRegs.rsi = uc->uc_mcontext.gregs[REG_RSI];
        crash->gpRegs.rdi = uc->uc_mcontext.gregs[REG_RDI];
        crash->gpRegs.orig_rax = -1;
        crash->gpRegs.rip = uc->uc_mcontext.gregs[REG_RIP];
        crash->gpRegs.cs = uc->uc_mcontext.gregs[REG_CSGSFS] & 0xFFFF;
        crash->gpRegs.eflags = uc->uc_mcontext.gregs[REG_EFL];
        crash->gpRegs.rsp = uc->uc_mcontext.gregs[REG_RSP];
        crash->gpRegs.ss = (uc->uc_mcontext.gregs[REG_CSGSFS] >> 48) & 0xFFFF;
        crash->hasGPRegs = 1;

        // x86_64 FP registers from fpregs
        if (uc->uc_mcontext.fpregs != NULL)
        {
            memcpy(&crash->fpRegs, uc->uc_mcontext.fpregs, sizeof(crash->fpRegs));
            crash->hasFPRegs = 1;
        }
#endif
    }

    state->threadCount = 1;
    state->crashThreadIndex = 0;
}

#endif // __linux__

// ---------------------------------------------------------------------------
// Apple: Collect all threads using Mach APIs
// ---------------------------------------------------------------------------

#if defined(__APPLE__)

static void CollectCrashThread_Apple(struct InProcDumpState* state, int signal, siginfo_t* siginfo, void* context)
{
    state->signal = signal;
    if (siginfo != NULL)
        memcpy(&state->siginfo, siginfo, sizeof(siginfo_t));

    state->pid = (int)getpid();
    state->ppid = (int)getppid();
    state->tgid = state->pid;

    // Process name — getprogname() is async-signal-safe on Apple
    {
        const char* name = getprogname();
        if (name != NULL)
        {
            int i = 0;
            while (name[i] && i < INPROC_MAX_PROCESS_NAME - 1)
            {
                state->processName[i] = name[i];
                i++;
            }
            state->processName[i] = '\0';
        }
    }

    // Get the crashing thread's Mach port
    mach_port_t crashThread = mach_thread_self();
    thread_act_array_t threads = NULL;
    mach_msg_type_number_t threadCount = 0;
    state->threadCount = 0;
    state->crashThreadIndex = -1;
    state->truncatedThreads = 0;

    kern_return_t kr = task_threads(mach_task_self(), &threads, &threadCount);
    if (kr != KERN_SUCCESS)
    {
        // Fallback: just use the crash thread from ucontext
        struct InProcThreadInfo* crash = &state->threads[0];
        crash->tid = (uint64_t)crashThread;  // mach_thread_self() — Mach trap, signal-safe
        crash->isCrashThread = 1;

        if (context != NULL)
        {
            ucontext_t* uc = (ucontext_t*)context;
#if defined(__aarch64__)
            memcpy(&crash->gpRegs, &uc->uc_mcontext->__ss, sizeof(crash->gpRegs));
            crash->hasGPRegs = 1;
            memcpy(&crash->fpRegs, &uc->uc_mcontext->__ns, sizeof(crash->fpRegs));
            crash->hasFPRegs = 1;
#elif defined(__x86_64__)
            memcpy(&crash->gpRegs, &uc->uc_mcontext->__ss, sizeof(crash->gpRegs));
            crash->hasGPRegs = 1;
            memcpy(&crash->fpRegs, &uc->uc_mcontext->__fs, sizeof(crash->fpRegs));
            crash->hasFPRegs = 1;
#endif
        }

        state->threadCount = 1;
        state->crashThreadIndex = 0;
        return;
    }

    // Suspend all other threads for a consistent snapshot
    for (mach_msg_type_number_t i = 0; i < threadCount; i++)
    {
        if (threads[i] != crashThread)
            thread_suspend(threads[i]);
    }

    // Collect registers for each thread
    for (mach_msg_type_number_t i = 0; i < threadCount && state->threadCount < INPROC_MAX_THREADS; i++)
    {
        struct InProcThreadInfo* info = &state->threads[state->threadCount];
        memset(info, 0, sizeof(*info));
        info->tid = (uint64_t)threads[i];

        if (threads[i] == crashThread)
        {
            info->isCrashThread = 1;
            state->crashThreadIndex = state->threadCount;

            // Use ucontext for crash thread (most accurate — Mach API might return handler context)
            if (context != NULL)
            {
                ucontext_t* uc = (ucontext_t*)context;
#if defined(__aarch64__)
                memcpy(&info->gpRegs, &uc->uc_mcontext->__ss, sizeof(info->gpRegs));
                info->hasGPRegs = 1;
                memcpy(&info->fpRegs, &uc->uc_mcontext->__ns, sizeof(info->fpRegs));
                info->hasFPRegs = 1;
#elif defined(__x86_64__)
                memcpy(&info->gpRegs, &uc->uc_mcontext->__ss, sizeof(info->gpRegs));
                info->hasGPRegs = 1;
                memcpy(&info->fpRegs, &uc->uc_mcontext->__fs, sizeof(info->fpRegs));
                info->hasFPRegs = 1;
#endif
            }
        }
        else
        {
            // Use Mach API for other threads
#if defined(__aarch64__)
            mach_msg_type_number_t gpCount = ARM_THREAD_STATE64_COUNT;
            kr = thread_get_state(threads[i], ARM_THREAD_STATE64,
                                  (thread_state_t)&info->gpRegs, &gpCount);
            info->hasGPRegs = (kr == KERN_SUCCESS) ? 1 : 0;

            mach_msg_type_number_t fpCount = ARM_NEON_STATE64_COUNT;
            kr = thread_get_state(threads[i], ARM_NEON_STATE64,
                                  (thread_state_t)&info->fpRegs, &fpCount);
            info->hasFPRegs = (kr == KERN_SUCCESS) ? 1 : 0;
#elif defined(__x86_64__)
            mach_msg_type_number_t gpCount = x86_THREAD_STATE64_COUNT;
            kr = thread_get_state(threads[i], x86_THREAD_STATE64,
                                  (thread_state_t)&info->gpRegs, &gpCount);
            info->hasGPRegs = (kr == KERN_SUCCESS) ? 1 : 0;

            mach_msg_type_number_t fpCount = x86_FLOAT_STATE64_COUNT;
            kr = thread_get_state(threads[i], x86_FLOAT_STATE64,
                                  (thread_state_t)&info->fpRegs, &fpCount);
            info->hasFPRegs = (kr == KERN_SUCCESS) ? 1 : 0;
#endif
        }

        state->threadCount++;
    }

    if ((int)threadCount > INPROC_MAX_THREADS)
        state->truncatedThreads = 1;

    // Resume threads
    for (mach_msg_type_number_t i = 0; i < threadCount; i++)
    {
        if (threads[i] != crashThread)
            thread_resume(threads[i]);
    }

    // Deallocate thread array (vm_deallocate is Mach kernel call, should be safe)
    vm_deallocate(mach_task_self(), (vm_address_t)threads,
                  threadCount * sizeof(thread_act_t));
}

// ---------------------------------------------------------------------------
// Apple: Collect memory regions using mach_vm_region_recurse
// ---------------------------------------------------------------------------

static void CollectRegions_Apple(struct InProcDumpState* state)
{
    state->regionCount = 0;
    state->moduleCount = 0;
    state->truncatedRegions = 0;
    state->truncatedModules = 0;

    // Follow the proven traversal pattern from createdump/crashinfomac.cpp:
    // start at address 1, keep depth persistent, only advance address for non-submaps.
    mach_vm_address_t addr = 1;
    mach_vm_size_t size = 0;
    vm_region_submap_info_data_64_t info;
    uint32_t depth = 0;

    while (addr > 0 && addr < MACH_VM_MAX_ADDRESS && state->regionCount < INPROC_MAX_MEMORY_REGIONS)
    {
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;

        kern_return_t kr = mach_vm_region_recurse(
            mach_task_self(),
            &addr,
            &size,
            &depth,
            (vm_region_recurse_info_t)&info,
            &count);

        if (kr != KERN_SUCCESS) break;

        if (info.is_submap)
        {
            depth++;
            continue;
        }

        // Skip empty/no-permission regions (matching createdump behavior)
        if (info.share_mode != SM_EMPTY && (info.protection & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)) != 0)
        {
            struct InProcMemoryRegion* r = &state->regions[state->regionCount];
            r->start = (uint64_t)addr;
            r->end = (uint64_t)(addr + size);
            r->flags = (uint32_t)info.protection;
            r->offset = (uint64_t)info.offset;
            r->name[0] = '\0';  // Mach APIs don't directly give us file names
            state->regionCount++;
        }

        addr += size;
    }

    if (state->regionCount >= INPROC_MAX_MEMORY_REGIONS)
        state->truncatedRegions = 1;
}

#endif // __APPLE__

// ---------------------------------------------------------------------------
// Generate the core dump
// ---------------------------------------------------------------------------

void InProcDump_Generate(int signal, siginfo_t* siginfo, void* context)
{
    if (s_dumpType == InProcDumpType_None)
        return;

    // One-thread-wins serialization — prevent concurrent dump generation.
    int expected = 0;
    if (!__atomic_compare_exchange_n(&s_dumpInProgress, &expected, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return;

    // Clear the state
    memset(&s_state, 0, sizeof(s_state));
    s_state.dumpType = s_dumpType;
    s_state.stackRegionIndex = -1;

#if defined(__linux__)
    // Restore pre-opened fd (memset cleared s_state.fdMem).
    // Fall back to opening in handler if pre-open failed.
    s_state.fdMem = s_fdMem;
    if (s_state.fdMem < 0)
        s_state.fdMem = open("/proc/self/mem", O_RDONLY);

    // Collect crashing thread registers
    CollectCrashThread(&s_state, signal, siginfo, context);

    // Collect memory regions from /proc/self/maps
    InProcDumpMaps_Collect(&s_state);

    // Collect auxiliary vector
    InProcDumpMaps_CollectAuxv(&s_state);

    // Find stack region for crashing thread
    if (s_state.threadCount > 0 && s_state.threads[0].hasGPRegs)
    {
#if defined(__aarch64__)
        uint64_t sp = s_state.threads[0].gpRegs.sp;
#elif defined(__x86_64__)
        uint64_t sp = s_state.threads[0].gpRegs.rsp;
#else
        uint64_t sp = 0;
#endif
        s_state.stackRegionIndex = InProcDump_FindRegion(&s_state, sp);
    }

    // Build output path
    char dumpPath[1024];
    BuildDumpPath(dumpPath, sizeof(dumpPath), s_state.pid);

    // Write the dump
    WriteStderr("Writing core dump to: ");
    WriteStderr(dumpPath);
    WriteStderr("\n");
    int dumpResult = InProcDumpElf_Write(&s_state, dumpPath);
    if (dumpResult < 0)
    {
        WriteStderr("Failed to write core dump to: ");
        WriteStderr(dumpPath);
        char errBuf[16];
        WriteStderr(" (errno=");
        IntToStr(errno, errBuf, sizeof(errBuf));
        WriteStderr(errBuf);
        WriteStderr(")\n");
    }
    else
    {
        WriteStderr("Core dump written successfully\n");
    }

    // Don't close s_fdMem — the process is about to exit anyway.

#elif defined(__APPLE__)

    // Collect all threads using Mach APIs
    CollectCrashThread_Apple(&s_state, signal, siginfo, context);

    // Collect memory regions
    CollectRegions_Apple(&s_state);

    // Find stack regions for threads
    if (s_state.crashThreadIndex >= 0)
    {
        uint64_t sp = 0;
#if defined(__aarch64__)
        if (s_state.threads[s_state.crashThreadIndex].hasGPRegs)
            sp = arm_thread_state64_get_sp(s_state.threads[s_state.crashThreadIndex].gpRegs);
#elif defined(__x86_64__)
        if (s_state.threads[s_state.crashThreadIndex].hasGPRegs)
            sp = s_state.threads[s_state.crashThreadIndex].gpRegs.__rsp;
#endif
        s_state.stackRegionIndex = InProcDump_FindRegion(&s_state, sp);
    }

    // Build output path
    char dumpPath[1024];
    BuildDumpPath(dumpPath, sizeof(dumpPath), s_state.pid);

    // Write the dump
    WriteStderr("Writing core dump to: ");
    WriteStderr(dumpPath);
    WriteStderr("\n");
    int dumpResult = InProcDumpMachO_Write(&s_state, dumpPath);
    if (dumpResult < 0)
    {
        WriteStderr("Failed to write core dump to: ");
        WriteStderr(dumpPath);
        char errBuf[16];
        WriteStderr(" (errno=");
        IntToStr(errno, errBuf, sizeof(errBuf));
        WriteStderr(errBuf);
        WriteStderr(")\n");
    }
    else
    {
        WriteStderr("Core dump written successfully\n");
    }

#endif
}

#endif // !JIT_STANDALONE_BUILD && (__linux__ || __APPLE__)
