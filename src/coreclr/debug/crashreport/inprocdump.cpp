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
#include <signal.h>
#include <sys/stat.h>
#include <stddef.h>
#include <dlfcn.h>
#include <minipal/log.h>

#if defined(__linux__)
#include <sys/types.h>
#include <sys/syscall.h>
#endif

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/vm_map.h>
#include <mach/task_info.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/dyld_images.h>
#endif

// Verify SpecialDiagInfo ABI compatibility with createdump/diagnostics repo.
static_assert(sizeof(struct InProcSpecialDiagInfoHeader) == 40,
    "InProcSpecialDiagInfoHeader size must match SpecialDiagInfoHeader");
static_assert(offsetof(struct InProcSpecialDiagInfoHeader, RuntimeBaseAddress) == 32,
    "RuntimeBaseAddress offset must match SpecialDiagInfoHeader");

// ---------------------------------------------------------------------------
// Static state — initialized at startup, used in signal handler
// ---------------------------------------------------------------------------

static enum InProcDumpType s_dumpType = InProcDumpType_None;
static char s_dumpPath[1024];
static int s_initialized = 0;
static uint64_t s_runtimeBaseAddress = 0;

// CAS guard: ensures only one thread generates the dump.
static volatile int s_dumpInProgress = 0;

#if defined(__APPLE__)
// Dyld memory ranges captured at init time for SOS/clrmd module enumeration.
// clrmd discovers modules by finding dyld in the dump (MH_DYLINKER header in
// __TEXT), parsing its symbol table (symtab/strtab from __LINKEDIT) to locate
// dyld_all_image_infos, then reading the image info array.
//
// On modern Apple, dyld lives in the shared cache. Its __LINKEDIT is ~600 MB
// (shared with all cached dylibs). We include only the specific pages needed:
//   1. dyld __TEXT segment (~680 KB) — contains MH_DYLINKER header + load cmds
//   2. symtab from __LINKEDIT — nlist_64 array for symbol lookup
//   3. strtab from __LINKEDIT — symbol name strings
//   4. dyld_all_image_infos struct — module list metadata
//   5. image info array — all loaded module base addresses and paths
static struct InProcDyldRange s_dyldRanges[INPROC_MAX_DYLD_RANGES];
static int s_dyldRangeCount = 0;

// Module header pages captured at init time.
// clrmd's MachOModule constructor throws (not skips) when a module's Mach-O header
// can't be read, aborting the entire module enumeration. We capture the first page
// of every loaded module so all headers are present in the dump.
static struct InProcDyldRange s_moduleHeaders[INPROC_MAX_MODULE_HEADERS];
static int s_moduleHeaderCount = 0;

// Saved Mach thread list for deferred resume. CollectCrashThread_Apple suspends
// threads but does not resume them — we defer resume until after Tier 2 capture.
static thread_act_array_t s_frozenThreads = NULL;
static mach_msg_type_number_t s_frozenThreadCount = 0;
static mach_port_t s_crashMachThread = MACH_PORT_NULL;
#endif

#if defined(__linux__)
// Pre-opened fd for /proc/self/mem — stored separately so memset of s_state
// doesn't lose it.
static int s_fdMem = -1;
#endif

// Managed debug info — VM-provided offsets and global addresses for Tier 2.
// Populated by InProcDump_InitManagedDebug() during VM startup.
static struct InProcManagedDebugInfo s_managedDebugInfo;

// Managed debug pages captured at crash time for clrstack/clrthreads support.
// Stored separately from s_state to avoid inflating InProcDumpState further
// than necessary. Copied into s_state at dump generation time.
static struct InProcDyldRange s_managedDebugPages[INPROC_MAX_MANAGED_DEBUG_PAGES];
static int s_managedDebugPageCount = 0;

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
        minipal_log_write_info("InProcDump: disabled (DOTNET_EnableInProcessCrashReport not set)\n");
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

    char logMsg[1200];
    snprintf(logMsg, sizeof(logMsg), "InProcDump: initialized, type=%s, path=%s\n",
             s_dumpType == InProcDumpType_Full ? "full" : "mini",
             s_dumpPath[0] ? s_dumpPath : "(deferred to crash time)");
    minipal_log_write_info(logMsg);

    // Ensure the parent directory exists (mkdir is safe at init time).
    if (s_dumpPath[0] != '\0')
    {
        char parentDir[1024];
        int lastSlash = -1;
        for (int k = 0; s_dumpPath[k]; k++)
        {
            if (s_dumpPath[k] == '/') lastSlash = k;
        }
        if (lastSlash > 0)
        {
            for (int k = 0; k < lastSlash && k < (int)sizeof(parentDir) - 1; k++)
                parentDir[k] = s_dumpPath[k];
            parentDir[lastSlash] = '\0';
            mkdir(parentDir, 0755);
        }
    }

#if defined(__linux__)
    // Open /proc/self/mem now — may not be openable in some signal states.
    // Store in separate static so memset of s_state doesn't lose it.
    s_fdMem = open("/proc/self/mem", O_RDONLY);
#endif

    // Capture the runtime base address for SpecialDiagInfo.
    // dladdr is safe at init time (not in signal handler). Use a symbol
    // known to reside in libcoreclr to resolve its load address.
    Dl_info dlInfo;
    if (dladdr((void*)&InProcDump_Initialize, &dlInfo) != 0 && dlInfo.dli_fbase != NULL)
    {
        s_runtimeBaseAddress = (uint64_t)(uintptr_t)dlInfo.dli_fbase;
    }

#if defined(__APPLE__)
    // Capture dyld memory ranges for SOS/clrmd module enumeration.
    // task_info and memory reads are safe at init time (not async-signal-safe).
    //
    // clrmd finds modules by: (1) locating dyld via its MH_DYLINKER header,
    // (2) parsing dyld's LC_SYMTAB to find dyld_all_image_infos in the symbol table,
    // (3) reading the image info array. We compute the exact virtual addresses
    // of each piece at init time so dump-time just writes byte ranges.
    {
        struct task_dyld_info dyldTaskInfo;
        mach_msg_type_number_t diCount = TASK_DYLD_INFO_COUNT;
        kern_return_t kr = task_info(mach_task_self(), TASK_DYLD_INFO,
                                     (task_info_t)&dyldTaskInfo, &diCount);
        if (kr == KERN_SUCCESS)
        {
            uint64_t allImageInfoAddr = dyldTaskInfo.all_image_info_addr;

            const struct dyld_all_image_infos* infos =
                (const struct dyld_all_image_infos*)allImageInfoAddr;
            uint64_t dyldBase = (uint64_t)infos->dyldImageLoadAddress;

            uint64_t imageInfoArrayAddr = (uint64_t)infos->infoArray;
            uint64_t imageInfoArraySize = (uint64_t)infos->infoArrayCount *
                                           sizeof(struct dyld_image_info);

            // Parse dyld's Mach-O header to find __TEXT, __LINKEDIT, and LC_SYMTAB.
            const struct mach_header_64* dyldHeader =
                (const struct mach_header_64*)dyldBase;
            if (dyldHeader->magic == MH_MAGIC_64)
            {
                uint64_t textVmaddr = 0, textVmsize = 0;
                uint64_t linkeditVmaddr = 0, linkeditFileoff = 0;
                uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
                int foundText = 0, foundLinkedit = 0, foundSymtab = 0;

                const uint8_t* cmd = (const uint8_t*)(dyldHeader + 1);
                for (uint32_t i = 0; i < dyldHeader->ncmds; i++)
                {
                    const struct load_command* lc = (const struct load_command*)cmd;
                    if (lc->cmd == LC_SEGMENT_64)
                    {
                        const struct segment_command_64* seg =
                            (const struct segment_command_64*)cmd;
                        if (strcmp(seg->segname, SEG_TEXT) == 0)
                        {
                            textVmaddr = seg->vmaddr;
                            textVmsize = seg->vmsize;
                            foundText = 1;
                        }
                        else if (strcmp(seg->segname, SEG_LINKEDIT) == 0)
                        {
                            linkeditVmaddr = seg->vmaddr;
                            linkeditFileoff = seg->fileoff;
                            foundLinkedit = 1;
                        }
                    }
                    else if (lc->cmd == LC_SYMTAB)
                    {
                        const struct symtab_command* sym =
                            (const struct symtab_command*)cmd;
                        symoff = sym->symoff;
                        nsyms = sym->nsyms;
                        stroff = sym->stroff;
                        strsize = sym->strsize;
                        foundSymtab = 1;
                    }
                    cmd += lc->cmdsize;
                }

                if (foundText && foundLinkedit && foundSymtab)
                {
                    // loadBias accounts for ASLR / shared cache slide.
                    // For standard Mach-O layout, __TEXT starts at the header.
                    uint64_t loadBias = dyldBase - textVmaddr;
                    uint64_t linkeditActual = linkeditVmaddr + loadBias;

                    // Range 0: dyld __TEXT (MH_DYLINKER header + load commands)
                    s_dyldRanges[0].addr = dyldBase;
                    s_dyldRanges[0].size = textVmsize;

                    // Range 1: symbol table (nlist_64 array from __LINKEDIT)
                    s_dyldRanges[1].addr = linkeditActual + (symoff - linkeditFileoff);
                    s_dyldRanges[1].size = (uint64_t)nsyms * sizeof(struct nlist_64);

                    // Range 2: string table (symbol names from __LINKEDIT)
                    s_dyldRanges[2].addr = linkeditActual + (stroff - linkeditFileoff);
                    s_dyldRanges[2].size = strsize;

                    // Range 3: dyld_all_image_infos struct
                    s_dyldRanges[3].addr = allImageInfoAddr;
                    s_dyldRanges[3].size = sizeof(struct dyld_all_image_infos);

                    // Range 4: image info array (all loaded module base addresses)
                    s_dyldRanges[4].addr = imageInfoArrayAddr;
                    s_dyldRanges[4].size = imageInfoArraySize;

                    s_dyldRangeCount = 5;
                }
            }

            // Capture header + load commands for each loaded module.
            // clrmd's MachOModule constructor reads the full Mach-O header and all load
            // commands. If any load command read returns zeros (unmapped memory), the
            // zero cmd.Size triggers InvalidDataException. We read each module's
            // sizeofcmds to compute the exact size needed.
            uint32_t moduleCount = infos->infoArrayCount;
            const struct dyld_image_info* imageInfos = infos->infoArray;
            for (uint32_t i = 0; i < moduleCount && s_moduleHeaderCount < INPROC_MAX_MODULE_HEADERS; i++)
            {
                uint64_t loadAddr = (uint64_t)imageInfos[i].imageLoadAddress;
                if (loadAddr == 0)
                    continue;

                // Read the header to determine how much to include
                const struct mach_header_64* hdr = (const struct mach_header_64*)loadAddr;
                if (hdr->magic != MH_MAGIC_64)
                    continue;

                uint64_t neededSize = sizeof(struct mach_header_64) + hdr->sizeofcmds;

                // Page-align base address
                uint64_t pageAddr = loadAddr & ~(uint64_t)0xFFF;
                // Account for sub-page offset of loadAddr within its page
                uint64_t offsetInPage = loadAddr - pageAddr;
                uint64_t totalSize = (neededSize + offsetInPage + 0xFFF) & ~(uint64_t)0xFFF;

                // Helper: add a range with deduplication
                auto addRange = [](uint64_t addr, uint64_t size)
                {
                    for (int j = 0; j < s_moduleHeaderCount; j++)
                    {
                        if (s_moduleHeaders[j].addr == addr && s_moduleHeaders[j].size >= size)
                            return;
                    }
                    if (s_moduleHeaderCount < INPROC_MAX_MODULE_HEADERS)
                    {
                        s_moduleHeaders[s_moduleHeaderCount].addr = addr;
                        s_moduleHeaders[s_moduleHeaderCount].size = size;
                        s_moduleHeaderCount++;
                    }
                };

                // Add module header + load commands
                addRange(pageAddr, totalSize);

                // Add module path string page (needed for SOS to identify modules by name)
                const char* path = imageInfos[i].imageFilePath;
                if (path != NULL)
                {
                    uint64_t pathPageAddr = (uint64_t)path & ~(uint64_t)0xFFF;
                    addRange(pathPageAddr, 0x1000);
                }
            }
        }
    }

    // Capture the runtime module's segments for SOS/DAC initialization.
    // The DAC needs:
    //   - symtab/strtab (from __LINKEDIT) to resolve exports like g_dacTable
    //   - __DATA, __DATA_CONST, __AUTH, etc. to read runtime globals the DAC uses
    // We skip __LINKEDIT itself (only need symtab/strtab ranges) and __TEXT
    // (already captured in module headers). All other segments are added.
    if (s_runtimeBaseAddress != 0)
    {
        const struct mach_header_64* rtHeader =
            (const struct mach_header_64*)s_runtimeBaseAddress;
        if (rtHeader->magic == MH_MAGIC_64)
        {
            uint64_t textVmaddr = 0;
            uint64_t linkeditVmaddr = 0, linkeditFileoff = 0;
            uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
            int foundText = 0, foundLinkedit = 0, foundSymtab = 0;

            // First pass: find __TEXT, __LINKEDIT, and LC_SYMTAB
            const uint8_t* cmd = (const uint8_t*)(rtHeader + 1);
            for (uint32_t i = 0; i < rtHeader->ncmds; i++)
            {
                const struct load_command* lc = (const struct load_command*)cmd;
                if (lc->cmd == LC_SEGMENT_64)
                {
                    const struct segment_command_64* seg =
                        (const struct segment_command_64*)cmd;
                    if (strcmp(seg->segname, SEG_TEXT) == 0)
                    {
                        textVmaddr = seg->vmaddr;
                        foundText = 1;
                    }
                    else if (strcmp(seg->segname, SEG_LINKEDIT) == 0)
                    {
                        linkeditVmaddr = seg->vmaddr;
                        linkeditFileoff = seg->fileoff;
                        foundLinkedit = 1;
                    }
                }
                else if (lc->cmd == LC_SYMTAB)
                {
                    const struct symtab_command* sym =
                        (const struct symtab_command*)cmd;
                    symoff = sym->symoff;
                    nsyms = sym->nsyms;
                    stroff = sym->stroff;
                    strsize = sym->strsize;
                    foundSymtab = 1;
                }
                cmd += lc->cmdsize;
            }

            if (foundText)
            {
                uint64_t loadBias = s_runtimeBaseAddress - textVmaddr;

                // Add symtab/strtab from __LINKEDIT
                if (foundLinkedit && foundSymtab &&
                    s_dyldRangeCount + 2 <= INPROC_MAX_DYLD_RANGES)
                {
                    uint64_t linkeditActual = linkeditVmaddr + loadBias;

                    s_dyldRanges[s_dyldRangeCount].addr =
                        linkeditActual + (symoff - linkeditFileoff);
                    s_dyldRanges[s_dyldRangeCount].size =
                        (uint64_t)nsyms * sizeof(struct nlist_64);
                    s_dyldRangeCount++;

                    s_dyldRanges[s_dyldRangeCount].addr =
                        linkeditActual + (stroff - linkeditFileoff);
                    s_dyldRanges[s_dyldRangeCount].size = strsize;
                    s_dyldRangeCount++;
                }

                // Second pass: add all non-TEXT/non-LINKEDIT segments
                // (DATA, DATA_CONST, AUTH, OBJC, etc.) for DAC initialization
                cmd = (const uint8_t*)(rtHeader + 1);
                for (uint32_t i = 0; i < rtHeader->ncmds; i++)
                {
                    const struct load_command* lc = (const struct load_command*)cmd;
                    if (lc->cmd == LC_SEGMENT_64)
                    {
                        const struct segment_command_64* seg =
                            (const struct segment_command_64*)cmd;
                        if (seg->vmsize > 0 &&
                            strcmp(seg->segname, SEG_TEXT) != 0 &&
                            strcmp(seg->segname, SEG_LINKEDIT) != 0)
                        {
                            uint64_t segAddr = seg->vmaddr + loadBias;
                            if (s_moduleHeaderCount < INPROC_MAX_MODULE_HEADERS)
                            {
                                s_moduleHeaders[s_moduleHeaderCount].addr = segAddr;
                                s_moduleHeaders[s_moduleHeaderCount].size = seg->vmsize;
                                s_moduleHeaderCount++;
                            }
                        }
                    }
                    cmd += lc->cmdsize;
                }
            }
        }
    }
#endif
}

enum InProcDumpType InProcDump_GetDumpType(void)
{
    return s_dumpType;
}

// Forward declaration — defined below after BuildDumpPath
static void WriteStderr(const char* msg);

void InProcDump_InitManagedDebug(const struct InProcManagedDebugInfo* info)
{
    if (info == NULL || !info->initialized)
        return;

    memcpy(&s_managedDebugInfo, info, sizeof(s_managedDebugInfo));
    WriteStderr("InProcDump: Managed debug offsets initialized (Tier 2 enabled)\n");
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
// Diagnostic logging (uses minipal_log for Android logcat visibility)
// ---------------------------------------------------------------------------

static void WriteStderr(const char* msg)
{
    if (msg == NULL) return;
    // On Android, write(STDERR_FILENO, ...) doesn't appear in logcat.
    // Use minipal_log_write which routes to __android_log_write on Android
    // and write(STDERR_FILENO, ...) on other platforms.
    minipal_log_write_info(msg);
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
// Tier 2: Managed debug page capture
//
// Walks runtime data structures to capture memory pages needed for
// clrthreads and clrstack in the dump. All code is async-signal-safe.
//
// The approach:
//   1. Walk ThreadStore → Thread linked list (enables clrthreads)
//   2. Walk EEJitManager → HeapList linked list (code heaps + nibble maps)
//   3. Walk RangeSectionMap for IPs found on thread stacks
//   4. For each resolved IP: RealCodeHeader → MethodDesc → MethodDescChunk
//      → MethodTable → Module (enables clrstack method names)
// ---------------------------------------------------------------------------

// Safe pointer read: returns the value at `addr` if it falls within a known
// readable memory region, or 0 if not. This prevents faulting on corrupted or
// unmapped pointers during the crash handler.
static uint64_t ReadPointerSafe(const struct InProcDumpState* state, uint64_t addr)
{
    if (addr == 0)
        return 0;

    int idx = InProcDump_FindRegion(state, addr);
    if (idx < 0)
        return 0;

    // Ensure the full pointer fits within the region
    if (addr + sizeof(uint64_t) > state->regions[idx].end)
        return 0;

    // Use memcpy to avoid alignment issues (code heap pointers may be 4-byte aligned)
    uint64_t value = 0;
    memcpy(&value, (const void*)addr, sizeof(uint64_t));
    return value;
}

// Safe byte read
static uint8_t ReadByteSafe(const struct InProcDumpState* state, uint64_t addr)
{
    if (addr == 0)
        return 0;

    int idx = InProcDump_FindRegion(state, addr);
    if (idx < 0)
        return 0;

    return *(const uint8_t*)addr;
}

// Safe uint32 read
static uint32_t ReadU32Safe(const struct InProcDumpState* state, uint64_t addr)
{
    if (addr == 0 || (addr & 3) != 0)
        return 0;

    int idx = InProcDump_FindRegion(state, addr);
    if (idx < 0)
        return 0;

    if (addr + sizeof(uint32_t) > state->regions[idx].end)
        return 0;

    return *(const uint32_t*)addr;
}

// Add a page-aligned memory range to the managed debug pages array with dedup.
static void AddManagedPage(uint64_t addr, uint64_t size)
{
    if (addr == 0 || size == 0)
        return;

    // Page-align
    uint64_t pageAddr = addr & ~(uint64_t)0xFFF;
    uint64_t pageEnd = (addr + size + 0xFFF) & ~(uint64_t)0xFFF;
    uint64_t pageSize = pageEnd - pageAddr;

    // Dedup: check if this page is already captured
    for (int i = 0; i < s_managedDebugPageCount; i++)
    {
        if (s_managedDebugPages[i].addr == pageAddr &&
            s_managedDebugPages[i].size >= pageSize)
            return;
    }

    if (s_managedDebugPageCount >= INPROC_MAX_MANAGED_DEBUG_PAGES)
        return;

    s_managedDebugPages[s_managedDebugPageCount].addr = pageAddr;
    s_managedDebugPages[s_managedDebugPageCount].size = pageSize;
    s_managedDebugPageCount++;
}

// Capture the page containing a given address.
static void CapturePage(const struct InProcDumpState* state, uint64_t addr)
{
    if (addr == 0)
        return;

    // Verify the address is in a readable region before capturing
    if (InProcDump_FindRegion(state, addr) < 0)
        return;

    AddManagedPage(addr, 1);
}

// Walk the ThreadStore → Thread linked list and capture each Thread object's page.
// This enables clrthreads to enumerate managed threads.
static void CaptureThreadObjects(const struct InProcDumpState* state,
                                 const struct InProcManagedDebugInfo* dbg)
{
    // Read s_pThreadStore (pointer-to-pointer: global contains address of the pointer)
    uint64_t threadStorePtr = ReadPointerSafe(state, dbg->threadStoreAddr);
    if (threadStorePtr == 0)
        return;

    // Capture the ThreadStore object itself
    CapturePage(state, threadStorePtr);

    // Read thread count for bounds checking
    uint32_t threadCount = ReadU32Safe(state, threadStorePtr + dbg->threadStore_ThreadCount);
    if (threadCount == 0 || threadCount > 1024)
        return;

    // Read first thread link: ThreadStore + FirstThreadLink offset gives us
    // the address of the SLink that points to the first Thread's m_Link field
    uint64_t linkAddr = ReadPointerSafe(state, threadStorePtr + dbg->threadStore_FirstThreadLink);
    if (linkAddr == 0)
        return;

    // Walk the linked list. Each link points to the next Thread's m_Link field.
    // The Thread object starts at (linkAddr - thread_Link offset).
    int walked = 0;
    while (linkAddr != 0 && walked < (int)threadCount + 10)
    {
        // Compute Thread object address from its Link field address
        uint64_t threadAddr = linkAddr - dbg->thread_Link;
        CapturePage(state, threadAddr);

        // Also capture RuntimeThreadLocals if present
        uint64_t rtlPtr = ReadPointerSafe(state, threadAddr + dbg->thread_RuntimeThreadLocals);
        CapturePage(state, rtlPtr);

        // Follow the linked list: Thread.m_Link contains an SLink with a
        // single pointer field (_next) at offset 0.
        linkAddr = ReadPointerSafe(state, linkAddr);
        walked++;
    }
}

// Walk the EEJitManager → HeapList linked list and capture each HeapList node
// plus its nibble map. This provides the code heap infrastructure that the cDAC
// needs for IP → code block resolution.
static void CaptureCodeHeaps(const struct InProcDumpState* state,
                             const struct InProcManagedDebugInfo* dbg)
{
    // Read m_pEEJitManager (pointer-to-pointer)
    uint64_t jitMgrPtr = ReadPointerSafe(state, dbg->eeJitManagerAddr);
    if (jitMgrPtr == 0)
        return;

    // Capture EEJitManager object
    CapturePage(state, jitMgrPtr);

    // Read AllCodeHeaps (first HeapList node)
    uint64_t heapListPtr = ReadPointerSafe(state, jitMgrPtr + dbg->eeJitManager_AllCodeHeaps);

    int heapCount = 0;
    while (heapListPtr != 0 && heapCount < 256)
    {
        // Capture HeapList node
        CapturePage(state, heapListPtr);

        // Read nibble map pointer and capture it
        uint64_t mapBase = ReadPointerSafe(state, heapListPtr + dbg->heapList_MapBase);
        uint64_t pHdrMap = ReadPointerSafe(state, heapListPtr + dbg->heapList_HeaderMap);
        uint64_t startAddr = ReadPointerSafe(state, heapListPtr + dbg->heapList_StartAddress);
        uint64_t endAddr = ReadPointerSafe(state, heapListPtr + dbg->heapList_EndAddress);

        if (pHdrMap != 0 && mapBase != 0 && endAddr > startAddr)
        {
            // Nibble map size: each 32-byte bucket uses 1 nibble (4 bits).
            // 8 nibbles per uint32_t, so each DWORD covers 256 bytes.
            // mapEntries = (heapSize + 31) / 32 buckets
            // mapBytes = ((mapEntries + 7) / 8) * 4 bytes (round up to DWORD)
            uint64_t heapSize = endAddr - mapBase;
            uint64_t mapEntries = (heapSize + 31) / 32;
            uint64_t mapBytes = ((mapEntries + 7) / 8) * 4;
            if (mapBytes > 0 && mapBytes < 16 * 1024 * 1024)
            {
                AddManagedPage(pHdrMap, mapBytes);
            }
        }

        // Follow linked list
        heapListPtr = ReadPointerSafe(state, heapListPtr + dbg->heapList_Next);
        heapCount++;
    }
}

// Walk the RangeSectionMap for a single IP address to capture the path from
// L5 → L4 → L3 → L2 → L1 → Fragment → RangeSection.
static uint64_t CaptureRangeSectionForIP(const struct InProcDumpState* state,
                                          const struct InProcManagedDebugInfo* dbg,
                                          uint64_t ip)
{
    // The RangeSectionMap is a 5-level trie on 64-bit.
    // Each level has 256 entries (8 bits of address per level).
    // maxSetBit=56, bitsPerLevel=8, mapLevels=5, bitsAtLastLevel=17
    //
    // EffectiveBitsForLevel(addr, level):
    //   addressBitsUsedInMap = addr >> (56 + 1 - 5*8) = addr >> 17
    //   L5: (addr >> 17) >> 32 = addr >> 49, & 0xFF  → bits [56:49]
    //   L4: (addr >> 17) >> 24 = addr >> 41, & 0xFF  → bits [48:41]
    //   L3: (addr >> 17) >> 16 = addr >> 33, & 0xFF  → bits [40:33]
    //   L2: (addr >> 17) >> 8  = addr >> 25, & 0xFF  → bits [32:25]
    //   L1: (addr >> 17) >> 0  = addr >> 17, & 0xFF  → bits [24:17]

    uint64_t mapAddr = dbg->rangeSectionMapAddr + dbg->rangeSectionMap_TopLevelData;

    // L5: index from bits [56:49]
    uint64_t l5Idx = (ip >> 49) & 0xFF;
    uint64_t l4Ptr = ReadPointerSafe(state, mapAddr + l5Idx * 8);
    // Mask off collectible tag bit
    l4Ptr &= ~(uint64_t)1;
    if (l4Ptr == 0) return 0;
    CapturePage(state, l4Ptr);

    // L4: index from bits [48:41]
    uint64_t l4Idx = (ip >> 41) & 0xFF;
    uint64_t l3Ptr = ReadPointerSafe(state, l4Ptr + l4Idx * 8);
    l3Ptr &= ~(uint64_t)1;
    if (l3Ptr == 0) return 0;
    CapturePage(state, l3Ptr);

    // L3: index from bits [40:33]
    uint64_t l3Idx = (ip >> 33) & 0xFF;
    uint64_t l2Ptr = ReadPointerSafe(state, l3Ptr + l3Idx * 8);
    l2Ptr &= ~(uint64_t)1;
    if (l2Ptr == 0) return 0;
    CapturePage(state, l2Ptr);

    // L2: index from bits [32:25]
    uint64_t l2Idx = (ip >> 25) & 0xFF;
    uint64_t l1Ptr = ReadPointerSafe(state, l2Ptr + l2Idx * 8);
    l1Ptr &= ~(uint64_t)1;
    if (l1Ptr == 0) return 0;
    CapturePage(state, l1Ptr);

    // L1: index from bits [24:17]. Each entry is a RangeSectionFragmentPointer.
    uint64_t l1Idx = (ip >> 17) & 0xFF;
    uint64_t fragPtr = ReadPointerSafe(state, l1Ptr + l1Idx * 8);
    // Mask off collectible tag
    fragPtr &= ~(uint64_t)1;
    if (fragPtr == 0) return 0;

    // Walk fragment linked list to find the one containing our IP
    int fragCount = 0;
    while (fragPtr != 0 && fragCount < 100)
    {
        CapturePage(state, fragPtr);

        uint64_t rangeBegin = ReadPointerSafe(state, fragPtr + dbg->fragment_RangeBegin);
        uint64_t rangeEnd = ReadPointerSafe(state, fragPtr + dbg->fragment_RangeEndOpen);

        if (ip >= rangeBegin && ip < rangeEnd)
        {
            // Found the fragment — capture its RangeSection
            uint64_t rangeSectionPtr = ReadPointerSafe(state, fragPtr + dbg->fragment_RangeSection);
            if (rangeSectionPtr != 0)
            {
                CapturePage(state, rangeSectionPtr);
                return rangeSectionPtr;
            }
        }

        // Follow next pointer (mask collectible tag)
        fragPtr = ReadPointerSafe(state, fragPtr + dbg->fragment_Next);
        fragPtr &= ~(uint64_t)1;
        fragCount++;
    }

    return 0;
}

// For a given RangeSection, capture the HeapList → NibbleMap → RealCodeHeader →
// MethodDesc → MethodDescChunk → MethodTable → Module chain.
static void CaptureMethodChainForIP(const struct InProcDumpState* state,
                                     const struct InProcManagedDebugInfo* dbg,
                                     uint64_t rangeSectionPtr,
                                     uint64_t ip)
{
    // Read RangeSection flags to check if this is a code heap
    uint32_t flags = ReadU32Safe(state, rangeSectionPtr + dbg->rangeSection_Flags);

    // Check for RANGE_SECTION_CODEHEAP (0x2) — JIT'd code
    if (flags & 0x2)
    {
        // Read HeapList pointer from RangeSection
        uint64_t heapListPtr = ReadPointerSafe(state, rangeSectionPtr + dbg->rangeSection_HeapList);
        if (heapListPtr == 0) return;
        CapturePage(state, heapListPtr);

        // Read HeapList fields for nibble map lookup
        uint64_t mapBase = ReadPointerSafe(state, heapListPtr + dbg->heapList_MapBase);
        uint64_t pHdrMap = ReadPointerSafe(state, heapListPtr + dbg->heapList_HeaderMap);
        if (mapBase == 0 || pHdrMap == 0 || ip < mapBase) return;

        // NibbleMap lookup (constant-time algorithm matching CoreCLR's nibblemapmacros.h).
        //
        // Constants: BYTES_PER_BUCKET = 32, CODE_ALIGN = 4, NIBBLES_PER_DWORD = 8
        // Each nibble covers a 32-byte bucket.
        // Each uint32_t covers 8 buckets = 256 bytes.
        // Nibble values: 0 = empty, 1-8 = offset within bucket (in DWORDs), 9-12 = pointer encoding.
        //
        // ADDR2POS(x) = x >> 5  (which 32-byte bucket)
        // ADDR2OFFS(x) = ((x & 31) >> 2) + 1  (DWORD offset within bucket, 1-indexed)

        uint64_t relAddr = ip - mapBase;
        uint64_t bucketIdx = relAddr >> 5;  // ADDR2POS: which 32-byte bucket
        uint64_t bucketByteIndex = ((relAddr & 31) >> 2) + 1;  // ADDR2OFFS within bucket
        uint64_t mapUnitIdx = bucketIdx >> 3;  // which DWORD (8 nibbles per DWORD)
        uint64_t nibbleIdx = bucketIdx & 7;   // which nibble within DWORD (0 = highest)
        uint64_t mapUnitAddr = pHdrMap + mapUnitIdx * 4;

        // Capture the nibble map page
        CapturePage(state, mapUnitAddr);

        uint32_t mapUnit = ReadU32Safe(state, mapUnitAddr);
        uint64_t codeStart = 0;

        // Check if this DWORD uses pointer encoding (nibble value 9-12 in lowest nibble)
        if ((mapUnit & 0xF) > 8)
        {
            // Pointer encoding: decode directly
            // DecodePointer: (dword & ~0xF) + (((dword & 0xF) - 9) << 2)
            codeStart = mapBase + (uint64_t)((mapUnit & ~0xFu) + (((mapUnit & 0xFu) - 9) << 2));
        }
        else
        {
            // Standard nibble lookup: find the highest non-zero nibble at or before our position.
            // Nibbles are stored MSB-first: nibble 0 is at bits [31:28], nibble 7 is at bits [3:0].
            // Shift so our nibble is in the lowest position, then scan forward.
            uint32_t shifted = mapUnit >> (4 * (7 - nibbleIdx));
            uint32_t nibble = shifted & 0xF;

            if (nibble != 0 && nibble <= bucketByteIndex)
            {
                // Found code start in our bucket
                // POSOFF2ADDR: (pos << 5) + ((nibble - 1) << 2)
                codeStart = mapBase + (bucketIdx << 5) + (((uint64_t)nibble - 1) << 2);
            }
            else
            {
                // Search backwards through remaining nibbles in this DWORD
                int searchIdx = (int)nibbleIdx;
                int found = 0;

                if (nibble != 0 && nibble > bucketByteIndex)
                {
                    // Current nibble has a higher offset — skip it, go to previous bucket
                    searchIdx--;
                }

                // Scan remaining nibbles backwards (shift right to move to previous nibbles)
                shifted >>= 4;
                searchIdx--;

                while (searchIdx >= 0 && !found)
                {
                    nibble = shifted & 0xF;
                    if (nibble != 0)
                    {
                        uint64_t pos = mapUnitIdx * 8 + (uint64_t)searchIdx;
                        codeStart = mapBase + (pos << 5) + (((uint64_t)nibble - 1) << 2);
                        found = 1;
                    }
                    shifted >>= 4;
                    searchIdx--;
                }

                if (!found && mapUnitIdx > 0)
                {
                    // Check previous DWORD
                    uint64_t prevMapUnitAddr = pHdrMap + (mapUnitIdx - 1) * 4;
                    CapturePage(state, prevMapUnitAddr);
                    uint32_t prevUnit = ReadU32Safe(state, prevMapUnitAddr);

                    if (prevUnit != 0)
                    {
                        if ((prevUnit & 0xF) > 8)
                        {
                            // Previous DWORD is pointer encoded
                            codeStart = mapBase + (uint64_t)((prevUnit & ~0xFu) + (((prevUnit & 0xFu) - 9) << 2));
                        }
                        else
                        {
                            // Find highest non-zero nibble in previous DWORD
                            for (int n = 7; n >= 0; n--)
                            {
                                uint32_t nib = (prevUnit >> (4 * (7 - n))) & 0xF;
                                if (nib != 0)
                                {
                                    uint64_t pos = (mapUnitIdx - 1) * 8 + (uint64_t)n;
                                    codeStart = mapBase + (pos << 5) + (((uint64_t)nib - 1) << 2);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }

        if (codeStart == 0) return;

        // RealCodeHeader is pointed to by [codeStart - 8] (pointer-sized)
        uint64_t codeHeaderIndirect = codeStart - 8;
        uint64_t codeHeaderPtr = ReadPointerSafe(state, codeHeaderIndirect);
        if (codeHeaderPtr == 0) return;
        CapturePage(state, codeHeaderPtr);

        // Read MethodDesc from RealCodeHeader
        uint64_t methodDescPtr = ReadPointerSafe(state, codeHeaderPtr + dbg->realCodeHeader_MethodDesc);
        if (methodDescPtr == 0) return;
        CapturePage(state, methodDescPtr);

        // Compute MethodDescChunk address:
        // chunkAddr = methodDescPtr - sizeof(MethodDescChunk) - (ChunkIndex * alignment)
        uint8_t chunkIndex = ReadByteSafe(state, methodDescPtr + dbg->methodDesc_ChunkIndex);
        uint64_t chunkAddr = methodDescPtr - dbg->methodDescChunk_Size
                           - ((uint64_t)chunkIndex * dbg->methodDesc_Alignment);
        if (chunkAddr == 0) return;
        CapturePage(state, chunkAddr);

        // Read MethodTable from MethodDescChunk
        uint64_t methodTablePtr = ReadPointerSafe(state, chunkAddr + dbg->methodDescChunk_MethodTable);
        if (methodTablePtr == 0) return;
        CapturePage(state, methodTablePtr);

        // Read Module from MethodTable
        uint64_t modulePtr = ReadPointerSafe(state, methodTablePtr + dbg->methodTable_Module);
        if (modulePtr == 0) return;
        CapturePage(state, modulePtr);

        // Read PE base from Module (for metadata resolution)
        uint64_t peBase = ReadPointerSafe(state, modulePtr + dbg->module_Base);
        if (peBase != 0)
        {
            // Capture the PE header page (metadata section discovery)
            CapturePage(state, peBase);
        }
    }
    else if (!(flags & 0x4))  // Not RANGE_SECTION_RANGELIST
    {
        // R2R module — capture the Module pointer directly from RangeSection
        uint64_t modulePtr = ReadPointerSafe(state, rangeSectionPtr + dbg->rangeSection_R2RModule);
        if (modulePtr != 0)
        {
            CapturePage(state, modulePtr);
            uint64_t peBase = ReadPointerSafe(state, modulePtr + dbg->module_Base);
            CapturePage(state, peBase);
        }
    }
}

// Scan a thread's stack for potential return addresses (code pointers).
// We use frame-pointer chain walking on ARM64, with fallback to stack scanning.
static void CollectStackIPs(const struct InProcDumpState* state,
                            const struct InProcThreadInfo* thread,
                            uint64_t* ips, int* ipCount, int maxIPs)
{
    *ipCount = 0;

    if (!thread->hasGPRegs)
        return;

    uint64_t ip = 0;
    uint64_t fp = 0;
    uint64_t sp = 0;

#if defined(__APPLE__) && defined(__aarch64__)
    ip = arm_thread_state64_get_pc(thread->gpRegs);
    fp = arm_thread_state64_get_fp(thread->gpRegs);
    sp = arm_thread_state64_get_sp(thread->gpRegs);
    // Also capture LR as a potential return address
    uint64_t lr = arm_thread_state64_get_lr(thread->gpRegs);
    if (lr != 0 && *ipCount < maxIPs)
        ips[(*ipCount)++] = lr;
#elif defined(__linux__) && defined(__aarch64__)
    ip = thread->gpRegs.pc;
    fp = thread->gpRegs.regs[29]; // x29 = FP
    sp = thread->gpRegs.sp;
    uint64_t lr = thread->gpRegs.regs[30]; // x30 = LR
    if (lr != 0 && *ipCount < maxIPs)
        ips[(*ipCount)++] = lr;
#elif defined(__x86_64__)
#if defined(__APPLE__)
    ip = thread->gpRegs.__rip;
    fp = thread->gpRegs.__rbp;
    sp = thread->gpRegs.__rsp;
#else
    ip = thread->gpRegs.rip;
    fp = thread->gpRegs.rbp;
    sp = thread->gpRegs.rsp;
#endif
#endif

    // Add current IP
    if (ip != 0 && *ipCount < maxIPs)
        ips[(*ipCount)++] = ip;

    // Walk frame pointer chain to collect return addresses
    // ARM64: FP points to [saved_FP, saved_LR] pair
    // x86_64: RBP points to [saved_RBP, return_address] pair
    uint64_t currentFP = fp;
    int frameCount = 0;

    while (currentFP != 0 && frameCount < maxIPs && *ipCount < maxIPs)
    {
        // Validate FP is in a readable region and properly aligned
        int fpRegion = InProcDump_FindRegion(state, currentFP);
        if (fpRegion < 0)
            break;

        // Ensure we can read two pointers at FP
        if (currentFP + 16 > state->regions[fpRegion].end)
            break;

        // Ensure FP is 16-byte aligned (ARM64 requirement)
        if (currentFP & 0xF)
            break;

        uint64_t savedFP = *(const uint64_t*)currentFP;
        uint64_t retAddr = *(const uint64_t*)(currentFP + 8);

        if (retAddr != 0)
            ips[(*ipCount)++] = retAddr;

        // Ensure we're moving up the stack (FP should increase)
        if (savedFP <= currentFP)
            break;

        currentFP = savedFP;
        frameCount++;
    }
}

// Main Tier 2 capture function. Called from InProcDump_Generate while threads
// are frozen (Apple) or for the crash thread only (Linux).
static void CollectManagedDebugPages(struct InProcDumpState* state)
{
    if (!s_managedDebugInfo.initialized)
        return;

    const struct InProcManagedDebugInfo* dbg = &s_managedDebugInfo;
    s_managedDebugPageCount = 0;

    WriteStderr("InProcDump: Capturing managed debug pages (Tier 2)...\n");

    // Phase 1: Capture ThreadStore + Thread objects
    CaptureThreadObjects(state, dbg);

    // Phase 2: Capture code heaps + nibble maps
    CaptureCodeHeaps(state, dbg);

    // Phase 3: For each thread, scan stack for IPs and resolve through RangeSectionMap
    uint64_t ips[INPROC_MAX_STACK_IPS];
    int ipCount = 0;

    for (int t = 0; t < state->threadCount; t++)
    {
        CollectStackIPs(state, &state->threads[t], ips, &ipCount, INPROC_MAX_STACK_IPS);

        for (int i = 0; i < ipCount; i++)
        {
            uint64_t rangeSectionPtr = CaptureRangeSectionForIP(state, dbg, ips[i]);
            if (rangeSectionPtr != 0)
            {
                CaptureMethodChainForIP(state, dbg, rangeSectionPtr, ips[i]);
            }
        }
    }

    // Copy results into state
    state->managedDebugPageCount = s_managedDebugPageCount;
    for (int i = 0; i < s_managedDebugPageCount; i++)
        state->managedDebugPages[i] = s_managedDebugPages[i];

    char countBuf[16];
    IntToStr(s_managedDebugPageCount, countBuf, sizeof(countBuf));
    WriteStderr("InProcDump: Captured ");
    WriteStderr(countBuf);
    WriteStderr(" managed debug pages\n");
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

    // Save thread array for deferred resume — threads remain frozen until
    // after Tier 2 managed debug page capture completes.
    s_frozenThreads = threads;
    s_frozenThreadCount = threadCount;
    s_crashMachThread = crashThread;
}

// Resume all frozen threads and deallocate the Mach thread array.
// Called after managed debug page capture is complete.
static void ResumeAndCleanupThreads_Apple(void)
{
    if (s_frozenThreads == NULL)
        return;

    for (mach_msg_type_number_t i = 0; i < s_frozenThreadCount; i++)
    {
        if (s_frozenThreads[i] != s_crashMachThread)
            thread_resume(s_frozenThreads[i]);
    }

    vm_deallocate(mach_task_self(), (vm_address_t)s_frozenThreads,
                  s_frozenThreadCount * sizeof(thread_act_t));
    s_frozenThreads = NULL;
    s_frozenThreadCount = 0;
    s_crashMachThread = MACH_PORT_NULL;
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
    // Use vm_* APIs (not mach_vm_*) for iOS/tvOS/MacCatalyst compatibility.
    vm_address_t addr = 1;
    vm_size_t size = 0;
    vm_region_submap_info_data_64_t info;
    uint32_t depth = 0;

    while (addr > 0 && state->regionCount < INPROC_MAX_MEMORY_REGIONS)
    {
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;

        kern_return_t kr = vm_region_recurse_64(
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
    {
        minipal_log_write_info("InProcDump_Generate: skipped (dumpType=None)\n");
        return;
    }

    // One-thread-wins serialization — prevent concurrent dump generation.
    int expected = 0;
    if (!__atomic_compare_exchange_n(&s_dumpInProgress, &expected, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    {
        minipal_log_write_info("InProcDump_Generate: skipped (already in progress)\n");
        return;
    }

    minipal_log_write_info("InProcDump_Generate: starting core dump generation\n");

    // Block all signals during dump generation to prevent re-entrancy from
    // secondary faults (e.g., SIGSEGV during /proc reads or memory scanning).
    // sigprocmask is async-signal-safe per POSIX.
    sigset_t blockAll, oldMask;
    sigfillset(&blockAll);
    sigprocmask(SIG_SETMASK, &blockAll, &oldMask);

    // Clear the state
    memset(&s_state, 0, sizeof(s_state));
    s_state.dumpType = s_dumpType;
    s_state.stackRegionIndex = -1;
    s_state.runtimeRegionIndex = -1;
    s_state.runtimeBaseAddress = s_runtimeBaseAddress;

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

    // Find runtime region for mini dumps (so SOS can validate the module base)
    if (s_state.runtimeBaseAddress != 0)
        s_state.runtimeRegionIndex = InProcDump_FindRegion(&s_state, s_state.runtimeBaseAddress);

    // Tier 2: Capture managed debug pages for clrstack/clrthreads
    CollectManagedDebugPages(&s_state);

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

    // Find runtime region for mini dumps (so SOS can validate the module base)
    if (s_state.runtimeBaseAddress != 0)
        s_state.runtimeRegionIndex = InProcDump_FindRegion(&s_state, s_state.runtimeBaseAddress);

    // Copy pre-computed dyld memory ranges for mini dumps (SOS/clrmd module enumeration).
    s_state.dyldRangeCount = s_dyldRangeCount;
    for (int i = 0; i < s_dyldRangeCount; i++)
        s_state.dyldRanges[i] = s_dyldRanges[i];

    // Copy module header pages for mini dumps (SOS/clrmd header validation).
    s_state.moduleHeaderCount = s_moduleHeaderCount;
    for (int i = 0; i < s_moduleHeaderCount; i++)
        s_state.moduleHeaders[i] = s_moduleHeaders[i];

    // Tier 2: Capture managed debug pages while threads are still frozen.
    // This is the critical window — threads suspended by CollectCrashThread_Apple
    // haven't been resumed yet, so heap data is consistent.
    CollectManagedDebugPages(&s_state);

    // Build output path
    char dumpPath[1024];
    BuildDumpPath(dumpPath, sizeof(dumpPath), s_state.pid);

    // Write the dump while threads are still frozen for memory consistency.
    // The writer uses vm_read_overwrite() which reads directly from the
    // frozen process state.
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

    // Resume all frozen threads now that the dump is written.
    ResumeAndCleanupThreads_Apple();

#endif

    // Restore original signal mask.
    sigprocmask(SIG_SETMASK, &oldMask, NULL);
}

#endif // !JIT_STANDALONE_BUILD && (__linux__ || __APPLE__)
