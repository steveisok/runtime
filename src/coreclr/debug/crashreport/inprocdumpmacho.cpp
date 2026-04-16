// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Mach-O core dump writer for in-process dump generation (Apple mobile).
// All code is async-signal-safe.
//
// Format:
//   mach_header_64   (MH_CORE)
//   LC_THREAD × T    — per-thread GP + FP registers
//   LC_SEGMENT_64 × N — per included memory region
//   [padding]        — to page boundary
//   Memory blocks    — one per LC_SEGMENT_64

#if defined(__APPLE__)

#include "inprocdump.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/vm_map.h>

#define PAGE_SIZE_DUMP 0x1000

// ---------------------------------------------------------------------------
// Thread command structure — GP + FP register flavors
// ---------------------------------------------------------------------------

struct ThreadCommand
{
    uint32_t cmd;       // LC_THREAD
    uint32_t cmdsize;

#if defined(__aarch64__)
    uint32_t gpFlavor;
    uint32_t gpCount;
    arm_thread_state64_t gpRegs;

    uint32_t fpFlavor;
    uint32_t fpCount;
    arm_neon_state64_t fpRegs;
#elif defined(__x86_64__)
    uint32_t gpFlavor;
    uint32_t gpCount;
    x86_thread_state64_t gpRegs;

    uint32_t fpFlavor;
    uint32_t fpCount;
    x86_float_state64_t fpRegs;
#endif
};

// ---------------------------------------------------------------------------
// Write helpers
// ---------------------------------------------------------------------------

static int WriteData(int fd, const void* buf, size_t len)
{
    return InProcDump_WriteAll(fd, buf, len);
}

static int WriteZeros(int fd, size_t len)
{
    char zeros[256];
    memset(zeros, 0, sizeof(zeros));
    while (len > 0)
    {
        size_t chunk = len < sizeof(zeros) ? len : sizeof(zeros);
        if (WriteData(fd, zeros, chunk) != 0) return -1;
        len -= chunk;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Count included memory regions
// ---------------------------------------------------------------------------

static int CountIncludedRegions(const struct InProcDumpState* state, int dumpType)
{
    if (dumpType == InProcDumpType_Full)
    {
        int count = 0;
        for (int i = 0; i < state->regionCount; i++)
        {
            if (state->regions[i].flags != 0)
                count++;
        }
        return count;
    }

    // Mini: stack regions for each collected thread
    int count = 0;
    for (int t = 0; t < state->threadCount; t++)
    {
        uint64_t sp = 0;

#if defined(__aarch64__)
        if (state->threads[t].hasGPRegs)
            sp = arm_thread_state64_get_sp(state->threads[t].gpRegs);
#elif defined(__x86_64__)
        if (state->threads[t].hasGPRegs)
            sp = state->threads[t].gpRegs.__rsp;
#endif

        if (sp != 0 && InProcDump_FindRegion(state, sp) >= 0)
            count++;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Write the Mach-O core dump
// ---------------------------------------------------------------------------

int InProcDumpMachO_Write(struct InProcDumpState* state, const char* path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    int result = -1;

    int segCount = CountIncludedRegions(state, state->dumpType);
    uint32_t ncmds = (uint32_t)state->threadCount + (uint32_t)segCount;

    // Calculate load command sizes
    size_t threadCmdSize = sizeof(struct ThreadCommand) * (size_t)state->threadCount;
    size_t segCmdSize = sizeof(struct segment_command_64) * (size_t)segCount;
    size_t totalCmdsSize = threadCmdSize + segCmdSize;
    size_t headerAndCmds = sizeof(struct mach_header_64) + totalCmdsSize;

    // Align data start to page boundary
    size_t dataOffset = (headerAndCmds + PAGE_SIZE_DUMP - 1) & ~((size_t)PAGE_SIZE_DUMP - 1);

    // --- Write Mach-O header ---
    struct mach_header_64 mh;
    memset(&mh, 0, sizeof(mh));
    mh.magic = MH_MAGIC_64;
#if defined(__aarch64__)
    mh.cputype = CPU_TYPE_ARM64;
    mh.cpusubtype = CPU_SUBTYPE_ARM64_ALL;
#elif defined(__x86_64__)
    mh.cputype = CPU_TYPE_X86_64;
    mh.cpusubtype = CPU_SUBTYPE_X86_64_ALL;
#endif
    mh.filetype = MH_CORE;
    mh.ncmds = ncmds;
    mh.sizeofcmds = (uint32_t)totalCmdsSize;
    mh.flags = 0;

    if (WriteData(fd, &mh, sizeof(mh)) != 0) goto done;

    // --- Write LC_THREAD commands ---
    for (int t = 0; t < state->threadCount; t++)
    {
        const struct InProcThreadInfo* thread = &state->threads[t];
        struct ThreadCommand tc;
        memset(&tc, 0, sizeof(tc));

        tc.cmd = LC_THREAD;
        tc.cmdsize = sizeof(struct ThreadCommand);

#if defined(__aarch64__)
        tc.gpFlavor = ARM_THREAD_STATE64;
        tc.gpCount = ARM_THREAD_STATE64_COUNT;
        if (thread->hasGPRegs)
            memcpy(&tc.gpRegs, &thread->gpRegs, sizeof(tc.gpRegs));

        tc.fpFlavor = ARM_NEON_STATE64;
        tc.fpCount = ARM_NEON_STATE64_COUNT;
        if (thread->hasFPRegs)
            memcpy(&tc.fpRegs, &thread->fpRegs, sizeof(tc.fpRegs));
#elif defined(__x86_64__)
        tc.gpFlavor = x86_THREAD_STATE64;
        tc.gpCount = x86_THREAD_STATE64_COUNT;
        if (thread->hasGPRegs)
            memcpy(&tc.gpRegs, &thread->gpRegs, sizeof(tc.gpRegs));

        tc.fpFlavor = x86_FLOAT_STATE64;
        tc.fpCount = x86_FLOAT_STATE64_COUNT;
        if (thread->hasFPRegs)
            memcpy(&tc.fpRegs, &thread->fpRegs, sizeof(tc.fpRegs));
#endif

        if (WriteData(fd, &tc, sizeof(tc)) != 0) goto done;
    }

    // --- Write LC_SEGMENT_64 commands ---
    {
        size_t curFileOff = dataOffset;

        if (state->dumpType == InProcDumpType_Full)
        {
            for (int i = 0; i < state->regionCount; i++)
            {
                if (state->regions[i].flags == 0) continue;

                size_t regionSize = state->regions[i].end - state->regions[i].start;
                struct segment_command_64 seg;
                memset(&seg, 0, sizeof(seg));
                seg.cmd = LC_SEGMENT_64;
                seg.cmdsize = sizeof(struct segment_command_64);
                seg.vmaddr = state->regions[i].start;
                seg.vmsize = regionSize;
                seg.fileoff = curFileOff;
                seg.filesize = regionSize;
                seg.maxprot = (vm_prot_t)state->regions[i].flags;
                seg.initprot = (vm_prot_t)state->regions[i].flags;

                if (WriteData(fd, &seg, sizeof(seg)) != 0) goto done;
                curFileOff += regionSize;
            }
        }
        else
        {
            // Mini: stack regions for each thread
            for (int t = 0; t < state->threadCount; t++)
            {
                uint64_t sp = 0;
#if defined(__aarch64__)
                if (state->threads[t].hasGPRegs)
                    sp = arm_thread_state64_get_sp(state->threads[t].gpRegs);
#elif defined(__x86_64__)
                if (state->threads[t].hasGPRegs)
                    sp = state->threads[t].gpRegs.__rsp;
#endif
                int ri = InProcDump_FindRegion(state, sp);
                if (sp == 0 || ri < 0) continue;

                const struct InProcMemoryRegion* r = &state->regions[ri];
                uint64_t dumpStart;
                size_t dumpSize;
                InProcDump_ClipStackRegion(r, sp, &dumpStart, &dumpSize);

                struct segment_command_64 seg;
                memset(&seg, 0, sizeof(seg));
                seg.cmd = LC_SEGMENT_64;
                seg.cmdsize = sizeof(struct segment_command_64);
                seg.vmaddr = dumpStart;
                seg.vmsize = dumpSize;
                seg.fileoff = curFileOff;
                seg.filesize = dumpSize;
                seg.maxprot = (vm_prot_t)r->flags;
                seg.initprot = (vm_prot_t)r->flags;

                if (WriteData(fd, &seg, sizeof(seg)) != 0) goto done;
                curFileOff += dumpSize;
            }
        }
    }

    // --- Pad to data offset ---
    {
        off_t cur = lseek(fd, 0, SEEK_CUR);
        if (cur < 0) goto done;
        if ((size_t)cur < dataOffset)
        {
            if (WriteZeros(fd, dataOffset - (size_t)cur) != 0) goto done;
        }
    }

    // --- Write memory segments ---
    {
        char ioBuf[INPROC_IO_BUFFER_SIZE];

        auto writeMemory = [&](uint64_t startAddr, size_t writeSize) -> int
        {
            vm_address_t addr = (vm_address_t)startAddr;
            size_t remaining = writeSize;

            while (remaining > 0)
            {
                size_t chunk = remaining < sizeof(ioBuf) ? remaining : sizeof(ioBuf);
                vm_size_t bytesRead = 0;
                kern_return_t kr = vm_read_overwrite(
                    mach_task_self(),
                    (vm_address_t)addr,
                    (vm_size_t)chunk,
                    (vm_address_t)ioBuf,
                    &bytesRead);

                if (kr != KERN_SUCCESS || bytesRead == 0)
                {
                    memset(ioBuf, 0, chunk);
                    bytesRead = (vm_size_t)chunk;
                }
                if (WriteData(fd, ioBuf, (size_t)bytesRead) != 0) return -1;
                addr += bytesRead;
                remaining -= (size_t)bytesRead;
            }
            return 0;
        };

        if (state->dumpType == InProcDumpType_Full)
        {
            for (int i = 0; i < state->regionCount; i++)
            {
                if (state->regions[i].flags == 0) continue;
                size_t regionSize = state->regions[i].end - state->regions[i].start;
                if (writeMemory(state->regions[i].start, regionSize) != 0) goto done;
            }
        }
        else
        {
            for (int t = 0; t < state->threadCount; t++)
            {
                uint64_t sp = 0;
#if defined(__aarch64__)
                if (state->threads[t].hasGPRegs)
                    sp = arm_thread_state64_get_sp(state->threads[t].gpRegs);
#elif defined(__x86_64__)
                if (state->threads[t].hasGPRegs)
                    sp = state->threads[t].gpRegs.__rsp;
#endif
                int ri = InProcDump_FindRegion(state, sp);
                if (sp == 0 || ri < 0) continue;

                const struct InProcMemoryRegion* r = &state->regions[ri];
                uint64_t dumpStart;
                size_t dumpSize;
                InProcDump_ClipStackRegion(r, sp, &dumpStart, &dumpSize);
                if (writeMemory(dumpStart, dumpSize) != 0) goto done;
            }
        }
    }

    result = 0;

done:
    close(fd);
    return result;
}

#endif // __APPLE__
