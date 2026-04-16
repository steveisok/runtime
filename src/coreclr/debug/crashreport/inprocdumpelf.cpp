// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// ELF core dump writer for in-process dump generation (Linux/Android).
// All code is async-signal-safe.
//
// Format:
//   Ehdr
//   [Shdr]             — only if phnum >= 0xFFFF
//   Phdr (PT_NOTE)
//   Phdr (PT_LOAD) × N — one per included memory region
//   Notes:
//     NT_PRPSINFO      — process info
//     NT_AUXV          — auxiliary vector
//     NT_FILE          — module mappings
//     NT_PRSTATUS × T  — per-thread GP registers
//     NT_FPREGSET × T  — per-thread FP registers
//     NT_SIGINFO       — signal info (crash thread only)
//     [padding]        — to page boundary
//   Memory blocks      — one per PT_LOAD

#if defined(__linux__)

#include "inprocdump.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <elf.h>
#include <minipal/log.h>

// ---------------------------------------------------------------------------
// ELF type aliases
// ---------------------------------------------------------------------------

#ifdef __LP64__
#define Ehdr   Elf64_Ehdr
#define Phdr   Elf64_Phdr
#define Shdr   Elf64_Shdr
#define Nhdr   Elf64_Nhdr
#else
#define Ehdr   Elf32_Ehdr
#define Phdr   Elf32_Phdr
#define Shdr   Elf32_Shdr
#define Nhdr   Elf32_Nhdr
#endif

#if defined(__x86_64__)
#define ELF_ARCH  EM_X86_64
#elif defined(__aarch64__)
#define ELF_ARCH  EM_AARCH64
#elif defined(__arm__)
#define ELF_ARCH  EM_ARM
#elif defined(__i386__)
#define ELF_ARCH  EM_386
#endif

#ifdef __LP64__
#define ELF_CLASS ELFCLASS64
#else
#define ELF_CLASS ELFCLASS32
#endif

#ifndef NT_FILE
#define NT_FILE 0x46494c45
#endif

#ifndef NT_SIGINFO
#define NT_SIGINFO 0x53494749
#endif

#define PH_HDR_CANARY 0xFFFF
#define PAGE_SIZE_DUMP 4096

static uint64_t GetCrashThreadSP(const struct InProcDumpState* state)
{
    if (state->crashThreadIndex < 0 || !state->threads[state->crashThreadIndex].hasGPRegs)
        return 0;
#if defined(__aarch64__)
    return state->threads[state->crashThreadIndex].gpRegs.sp;
#elif defined(__x86_64__)
    return state->threads[state->crashThreadIndex].gpRegs.rsp;
#else
    return 0;
#endif
}

struct NTFileEntry
{
    unsigned long startAddress;
    unsigned long endAddress;
    unsigned long offset;
};

// ---------------------------------------------------------------------------
// Size calculations — mirror createdump's approach
// ---------------------------------------------------------------------------

static size_t GetNoteNamePaddedSize(int namesz)
{
    // Note name is padded to 4-byte boundary; we always use 8 bytes
    // (CORE\0XXX pattern from createdump).
    (void)namesz;
    return 8;
}

static size_t GetProcessInfoSize(void)
{
    return sizeof(Nhdr) + 8 + sizeof(prpsinfo_t);
}

static size_t GetAuxvSize(const struct InProcDumpState* state)
{
    if (state->auxvSize <= 0) return 0;
    return sizeof(Nhdr) + 8 + (size_t)state->auxvSize;
}

static size_t GetNTFileSize(const struct InProcDumpState* state)
{
    if (state->moduleCount <= 0) return 0;

    // Header + entry count + page size
    size_t size = sizeof(Nhdr) + 8 + sizeof(unsigned long) + sizeof(unsigned long);
    // Per-module entries
    size += (size_t)state->moduleCount * sizeof(struct NTFileEntry);
    // File names + null terminators
    for (int i = 0; i < state->moduleCount; i++)
    {
        int len = 0;
        while (state->modules[i].name[len]) len++;
        size += (size_t)len + 1;
    }
    // Pad to 4-byte boundary
    size_t pad = (4 - (size % 4)) % 4;
    size += pad;
    return size;
}

static size_t GetThreadNotesSize(const struct InProcDumpState* state)
{
    size_t perThread = (sizeof(Nhdr) + 8 + sizeof(prstatus_t))
                     + (sizeof(Nhdr) + 8 + sizeof(user_fpregs_struct));
    size_t total = (size_t)state->threadCount * perThread;

    // NT_SIGINFO for crash thread
    if (state->signal != 0)
        total += sizeof(Nhdr) + 8 + sizeof(siginfo_t);

    return total;
}

// ---------------------------------------------------------------------------
// Count included memory regions based on dump type
// ---------------------------------------------------------------------------

static int CountIncludedRegions(const struct InProcDumpState* state, int dumpType)
{
    if (dumpType == InProcDumpType_Full)
    {
        // All regions with any permissions
        int count = 0;
        for (int i = 0; i < state->regionCount; i++)
        {
            if (state->regions[i].flags != 0)
                count++;
        }
        return count;
    }

    // Mini: only the stack region(s) for collected threads
    int count = 0;
    if (state->stackRegionIndex >= 0)
        count = 1;

    // Include the runtime module region (for SOS module base validation)
    if (state->runtimeRegionIndex >= 0)
        count++;

    return count;
}

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
// Write the ELF core dump
// ---------------------------------------------------------------------------

int InProcDumpElf_Write(struct InProcDumpState* state, const char* path)
{
    minipal_log_write_info("ELF: opening file\n");
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        minipal_log_write_error("ELF: open failed\n");
        return -1;
    }

    int result = -1;

    // Count PT_LOAD segments
    int hasDiagInfo = (state->runtimeBaseAddress != 0) ? 1 : 0;
    int loadCount = CountIncludedRegions(state, state->dumpType) + hasDiagInfo;
    uint64_t phnum = 1 + (uint64_t)loadCount;  // PT_NOTE + PT_LOADs
    int needShdr = (phnum >= PH_HDR_CANARY) ? 1 : 0;

    // Calculate note sizes
    size_t noteSize = GetProcessInfoSize()
                    + GetAuxvSize(state)
                    + GetNTFileSize(state)
                    + GetThreadNotesSize(state);

    // Calculate offsets
    size_t ehdrSize = sizeof(Ehdr);
    size_t shdrSize = needShdr ? sizeof(Shdr) : 0;
    size_t phdrOffset = ehdrSize + shdrSize;
    size_t allPhdrsSize = phnum * sizeof(Phdr);
    size_t noteOffset = phdrOffset + allPhdrsSize;

    // Align note end to page boundary for memory segments
    size_t noteEndRaw = noteOffset + noteSize;
    size_t noteAlignment = (PAGE_SIZE_DUMP - (noteEndRaw % PAGE_SIZE_DUMP)) % PAGE_SIZE_DUMP;
    size_t dataOffset = noteEndRaw + noteAlignment;

    // --- Write ELF header ---
    Ehdr ehdr;
    memset(&ehdr, 0, sizeof(ehdr));
    ehdr.e_ident[0] = ELFMAG0;
    ehdr.e_ident[1] = ELFMAG1;
    ehdr.e_ident[2] = ELFMAG2;
    ehdr.e_ident[3] = ELFMAG3;
    ehdr.e_ident[EI_CLASS] = ELF_CLASS;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_ident[EI_OSABI] = ELFOSABI_LINUX;
    ehdr.e_type = ET_CORE;
    ehdr.e_machine = ELF_ARCH;
    ehdr.e_version = EV_CURRENT;
    ehdr.e_phoff = phdrOffset;
    ehdr.e_ehsize = sizeof(Ehdr);
    ehdr.e_phentsize = sizeof(Phdr);

    if (!needShdr)
    {
        ehdr.e_phnum = (uint16_t)phnum;
    }
    else
    {
        ehdr.e_phnum = PH_HDR_CANARY;
        ehdr.e_shoff = ehdrSize;
        ehdr.e_shnum = 1;
        ehdr.e_shentsize = sizeof(Shdr);
    }

    minipal_log_write_info("ELF: writing ELF header\n");
    if (WriteData(fd, &ehdr, sizeof(ehdr)) != 0) goto done;

    // --- Write section header (for large phnum) ---
    if (needShdr)
    {
        Shdr shdr;
        memset(&shdr, 0, sizeof(shdr));
        shdr.sh_info = (uint32_t)phnum;
        shdr.sh_size = 1;
        if (WriteData(fd, &shdr, sizeof(shdr)) != 0) goto done;
    }

    // --- Write program headers ---
    {
        // PT_NOTE
        Phdr phdr;
        memset(&phdr, 0, sizeof(phdr));
        phdr.p_type = PT_NOTE;
        phdr.p_offset = noteOffset;
        phdr.p_filesz = noteSize;
        if (WriteData(fd, &phdr, sizeof(phdr)) != 0) goto done;

        // PT_LOAD segments
        size_t curDataOffset = dataOffset;
        phdr.p_type = PT_LOAD;
        phdr.p_align = PAGE_SIZE_DUMP;

        if (state->dumpType == InProcDumpType_Full)
        {
            for (int i = 0; i < state->regionCount; i++)
            {
                if (state->regions[i].flags == 0) continue;

                size_t regionSize = state->regions[i].end - state->regions[i].start;
                phdr.p_flags = state->regions[i].flags;
                phdr.p_vaddr = state->regions[i].start;
                phdr.p_memsz = regionSize;
                phdr.p_filesz = regionSize;
                phdr.p_offset = curDataOffset;
                if (WriteData(fd, &phdr, sizeof(phdr)) != 0) goto done;
                curDataOffset += regionSize;
            }
        }
        else
        {
            // Mini: just the stack region, anchored around SP
            if (state->stackRegionIndex >= 0)
            {
                const struct InProcMemoryRegion* r = &state->regions[state->stackRegionIndex];
                uint64_t dumpStart;
                size_t dumpSize;
                InProcDump_ClipStackRegion(r, GetCrashThreadSP(state), &dumpStart, &dumpSize);
                phdr.p_flags = r->flags;
                phdr.p_vaddr = dumpStart;
                phdr.p_memsz = dumpSize;
                phdr.p_filesz = dumpSize;
                phdr.p_offset = curDataOffset;
                if (WriteData(fd, &phdr, sizeof(phdr)) != 0) goto done;
                curDataOffset += dumpSize;
            }

            // Runtime module region (for SOS module base validation)
            if (state->runtimeRegionIndex >= 0)
            {
                const struct InProcMemoryRegion* r = &state->regions[state->runtimeRegionIndex];
                size_t regionSize = r->end - r->start;
                phdr.p_flags = r->flags;
                phdr.p_vaddr = r->start;
                phdr.p_memsz = regionSize;
                phdr.p_filesz = regionSize;
                phdr.p_offset = curDataOffset;
                if (WriteData(fd, &phdr, sizeof(phdr)) != 0) goto done;
                curDataOffset += regionSize;
            }
        }

        // SpecialDiagInfo synthetic segment (for SOS/dotnet-dump runtime discovery)
        if (hasDiagInfo)
        {
            phdr.p_flags = PF_R;
            phdr.p_vaddr = SPECIAL_DIAGINFO_ADDRESS;
            phdr.p_memsz = SPECIAL_DIAGINFO_SIZE;
            phdr.p_filesz = SPECIAL_DIAGINFO_SIZE;
            phdr.p_offset = curDataOffset;
            if (WriteData(fd, &phdr, sizeof(phdr)) != 0) goto done;
        }
    }

    // --- Write NT_PRPSINFO ---
    minipal_log_write_info("ELF: writing NT_PRPSINFO\n");
    {
        prpsinfo_t psinfo;
        memset(&psinfo, 0, sizeof(psinfo));
        psinfo.pr_sname = 'R';
        psinfo.pr_pid = state->pid;
        psinfo.pr_ppid = state->ppid;
        psinfo.pr_pgrp = state->tgid;
        for (int i = 0; i < (int)sizeof(psinfo.pr_fname) - 1 && state->processName[i]; i++)
            psinfo.pr_fname[i] = state->processName[i];

        Nhdr nhdr;
        memset(&nhdr, 0, sizeof(nhdr));
        nhdr.n_namesz = 5;
        nhdr.n_descsz = sizeof(prpsinfo_t);
        nhdr.n_type = NT_PRPSINFO;
        if (WriteData(fd, &nhdr, sizeof(nhdr)) != 0) goto done;
        if (WriteData(fd, "CORE\0PRP", 8) != 0) goto done;
        if (WriteData(fd, &psinfo, sizeof(psinfo)) != 0) goto done;
    }

    // --- Write NT_AUXV ---
    if (state->auxvSize > 0)
    {
        Nhdr nhdr;
        memset(&nhdr, 0, sizeof(nhdr));
        nhdr.n_namesz = 5;
        nhdr.n_descsz = (uint32_t)state->auxvSize;
        nhdr.n_type = NT_AUXV;
        if (WriteData(fd, &nhdr, sizeof(nhdr)) != 0) goto done;
        if (WriteData(fd, "CORE\0AUX", 8) != 0) goto done;
        if (WriteData(fd, state->auxv, (size_t)state->auxvSize) != 0) goto done;
    }

    // --- Write NT_FILE ---
    if (state->moduleCount > 0)
    {
        // Calculate descriptor size (everything after CORE\0FIL)
        size_t descSize = sizeof(unsigned long) + sizeof(unsigned long);  // count + pageSize
        descSize += (size_t)state->moduleCount * sizeof(struct NTFileEntry);
        for (int i = 0; i < state->moduleCount; i++)
        {
            int len = 0;
            while (state->modules[i].name[len]) len++;
            descSize += (size_t)len + 1;
        }

        Nhdr nhdr;
        memset(&nhdr, 0, sizeof(nhdr));
        nhdr.n_namesz = 5;
        nhdr.n_descsz = (uint32_t)descSize;
        nhdr.n_type = NT_FILE;
        if (WriteData(fd, &nhdr, sizeof(nhdr)) != 0) goto done;
        if (WriteData(fd, "CORE\0FIL", 8) != 0) goto done;

        unsigned long count = (unsigned long)state->moduleCount;
        unsigned long pageSize = PAGE_SIZE_DUMP;
        if (WriteData(fd, &count, sizeof(count)) != 0) goto done;
        if (WriteData(fd, &pageSize, sizeof(pageSize)) != 0) goto done;

        // Write entries
        for (int i = 0; i < state->moduleCount; i++)
        {
            struct NTFileEntry entry;
            entry.startAddress = state->modules[i].start;
            entry.endAddress = state->modules[i].end;
            entry.offset = state->modules[i].offset / PAGE_SIZE_DUMP;
            if (WriteData(fd, &entry, sizeof(entry)) != 0) goto done;
        }

        // Write file names
        for (int i = 0; i < state->moduleCount; i++)
        {
            int len = 0;
            while (state->modules[i].name[len]) len++;
            if (WriteData(fd, state->modules[i].name, (size_t)len + 1) != 0) goto done;
        }

        // Pad NT_FILE to 4-byte boundary
        size_t totalNTFile = sizeof(Nhdr) + 8 + descSize;
        size_t pad = (4 - (totalNTFile % 4)) % 4;
        if (pad > 0)
        {
            if (WriteZeros(fd, pad) != 0) goto done;
        }
    }

    // --- Write per-thread notes (NT_PRSTATUS + NT_FPREGSET) ---
    minipal_log_write_info("ELF: writing thread notes\n");
    for (int t = 0; t < state->threadCount; t++)
    {
        const struct InProcThreadInfo* thread = &state->threads[t];

        // NT_PRSTATUS
        {
            prstatus_t pr;
            memset(&pr, 0, sizeof(pr));

            if (thread->isCrashThread && state->signal != 0)
            {
                pr.pr_info.si_signo = state->siginfo.si_signo;
                pr.pr_info.si_code = state->siginfo.si_code;
                pr.pr_info.si_errno = state->siginfo.si_errno;
                pr.pr_cursig = state->siginfo.si_signo;
            }
            pr.pr_pid = (pid_t)thread->tid;
            pr.pr_ppid = (pid_t)state->ppid;
            pr.pr_pgrp = (pid_t)state->tgid;

            if (thread->hasGPRegs)
                memcpy(&pr.pr_reg, &thread->gpRegs, sizeof(thread->gpRegs));

            Nhdr nhdr;
            memset(&nhdr, 0, sizeof(nhdr));
            nhdr.n_namesz = 5;
            nhdr.n_descsz = sizeof(prstatus_t);
            nhdr.n_type = NT_PRSTATUS;
            if (WriteData(fd, &nhdr, sizeof(nhdr)) != 0) goto done;
            if (WriteData(fd, "CORE\0THR", 8) != 0) goto done;
            if (WriteData(fd, &pr, sizeof(pr)) != 0) goto done;
        }

        // NT_FPREGSET
        {
            Nhdr nhdr;
            memset(&nhdr, 0, sizeof(nhdr));
            nhdr.n_namesz = 5;
            nhdr.n_descsz = sizeof(user_fpregs_struct);
            nhdr.n_type = NT_FPREGSET;
            if (WriteData(fd, &nhdr, sizeof(nhdr)) != 0) goto done;
            if (WriteData(fd, "CORE\0FLT", 8) != 0) goto done;

            if (thread->hasFPRegs)
            {
                if (WriteData(fd, &thread->fpRegs, sizeof(thread->fpRegs)) != 0) goto done;
            }
            else
            {
                if (WriteZeros(fd, sizeof(user_fpregs_struct)) != 0) goto done;
            }
        }
    }

    // --- Write NT_SIGINFO (crash thread only) ---
    if (state->signal != 0)
    {
        Nhdr nhdr;
        memset(&nhdr, 0, sizeof(nhdr));
        nhdr.n_namesz = 5;
        nhdr.n_descsz = sizeof(siginfo_t);
        nhdr.n_type = NT_SIGINFO;
        if (WriteData(fd, &nhdr, sizeof(nhdr)) != 0) goto done;
        if (WriteData(fd, "CORE\0SIG", 8) != 0) goto done;
        if (WriteData(fd, &state->siginfo, sizeof(siginfo_t)) != 0) goto done;
    }

    // --- Pad to page boundary ---
    if (noteAlignment > 0)
    {
        if (WriteZeros(fd, noteAlignment) != 0) goto done;
    }

    // --- Write memory segments ---
    minipal_log_write_info("ELF: writing memory segments\n");
    {
        char ioBuf[INPROC_IO_BUFFER_SIZE];

        auto writeMemory = [&](uint64_t startAddr, size_t writeSize) -> int
        {
            uint64_t addr = startAddr;
            size_t remaining = writeSize;

#ifdef __aarch64__
            // ARM64 TBI: strip top-byte tags for pread on /proc/self/mem
            #define TBI_MASK 0x00FFFFFFFFFFFFFFULL
#else
            #define TBI_MASK 0xFFFFFFFFFFFFFFFFULL
#endif

            while (remaining > 0)
            {
                size_t chunk = remaining < sizeof(ioBuf) ? remaining : sizeof(ioBuf);
                ssize_t rd = pread(state->fdMem, ioBuf, chunk, (off_t)(addr & TBI_MASK));
                if (rd <= 0)
                {
                    // Page unreadable — write zeros instead
                    memset(ioBuf, 0, chunk);
                    rd = (ssize_t)chunk;
                }
                if (WriteData(fd, ioBuf, (size_t)rd) != 0) return -1;
                addr += (uint64_t)rd;
                remaining -= (size_t)rd;
            }

#undef TBI_MASK
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
            // Mini: stack region anchored around SP
            if (state->stackRegionIndex >= 0)
            {
                const struct InProcMemoryRegion* r = &state->regions[state->stackRegionIndex];
                uint64_t dumpStart;
                size_t dumpSize;
                InProcDump_ClipStackRegion(r, GetCrashThreadSP(state), &dumpStart, &dumpSize);
                if (writeMemory(dumpStart, dumpSize) != 0) goto done;
            }

            // Write runtime module region data
            if (state->runtimeRegionIndex >= 0)
            {
                const struct InProcMemoryRegion* r = &state->regions[state->runtimeRegionIndex];
                size_t regionSize = r->end - r->start;
                if (writeMemory(r->start, regionSize) != 0) goto done;
            }
        }
        if (hasDiagInfo)
        {
            struct InProcSpecialDiagInfoHeader diagHeader;
            memset(&diagHeader, 0, sizeof(diagHeader));
            memcpy(diagHeader.Signature, SPECIAL_DIAGINFO_SIGNATURE, sizeof(SPECIAL_DIAGINFO_SIGNATURE));
            diagHeader.Version = SPECIAL_DIAGINFO_VERSION;
            diagHeader.ExceptionRecordAddress = 0;
            diagHeader.RuntimeBaseAddress = state->runtimeBaseAddress;

            if (WriteData(fd, &diagHeader, sizeof(diagHeader)) != 0) goto done;
            if (WriteZeros(fd, SPECIAL_DIAGINFO_SIZE - sizeof(diagHeader)) != 0) goto done;
        }
    }

    result = 0;
    minipal_log_write_info("ELF: write complete\n");

done:
    close(fd);
    return result;
}

#endif // __linux__
