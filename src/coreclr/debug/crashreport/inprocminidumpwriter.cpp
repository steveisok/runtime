// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "inprocminidumpwriter.h"

#include "pal.h"

#include <algorithm>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#if defined(TARGET_APPLE)
#include <mach-o/loader.h>
#include <mach/machine.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#else
#include <elf.h>
#if TARGET_64BIT
#define TARGET_WORDSIZE 64
#else
#define TARGET_WORDSIZE 32
#endif
#ifndef ElfW
#define ElfW(type)      _ElfW(Elf, TARGET_WORDSIZE, type)
#define _ElfW(e,w,t)    _ElfW_1(e, w, _##t)
#define _ElfW_1(e,w,t)  e##w##t
#endif
#endif

static constexpr size_t InProcMiniDumpWriteChunkSize = 64 * 1024;
static constexpr ULONG32 MiniDumpWithPrivateReadWriteMemoryFlag = 0x200;
static constexpr uint64_t SpecialDiagInfoAddress =
#if defined(TARGET_APPLE)
    0x7fffffff10000000;
#elif defined(TARGET_64BIT)
    0x00007ffffff10000;
#else
    0x7fff1000;
#endif
static constexpr uint64_t SpecialDiagInfoSize = 0x1000;

STDAPI CLRDataCreateInstance(REFIID iid, ICLRDataTarget* target, void** iface);

struct SpecialDiagInfoHeader
{
    char signature[16];
    int32_t version;
    uint64_t exceptionRecordAddress;
    uint64_t runtimeBaseAddress;
};

static void InProcMiniDumpRuntimeBaseAnchor()
{
}

static bool WriteAll(int fd, const void* buffer, size_t length)
{
    const uint8_t* current = reinterpret_cast<const uint8_t*>(buffer);
    size_t remaining = length;
    while (remaining != 0)
    {
        ssize_t written = write(fd, current, remaining);
        if (written > 0)
        {
            current += written;
            remaining -= static_cast<size_t>(written);
            continue;
        }

        if (written == -1 && errno == EINTR)
        {
            continue;
        }

        return false;
    }

    return true;
}

static bool IsReadableMemoryRange(uint64_t address, uint64_t size)
{
    if (size == 0)
    {
        return true;
    }

    uint64_t end = address + size;
    if (end < address)
    {
        return false;
    }

#if defined(TARGET_APPLE)
    // PAL VirtualQuery misreports some valid runtime regions on Apple (e.g. loader-heap
    // Module objects), which caused capture to drop memory the reader needs for the managed
    // stack walk. Probe the real VM map with mach_vm_read_overwrite instead: it reads iff the
    // page is mapped/readable and returns an error rather than faulting otherwise. Probe one
    // byte per page across the requested range.
    uint64_t pageSize = static_cast<uint64_t>(vm_page_size);
    if (pageSize == 0)
    {
        pageSize = 0x4000;
    }

    uint8_t scratch[8];
    uint64_t probe = address;
    for (;;)
    {
        vm_size_t outSize = 0;
        kern_return_t kr = vm_read_overwrite(
            mach_task_self(),
            static_cast<vm_address_t>(probe),
            1,
            reinterpret_cast<vm_address_t>(scratch),
            &outSize);
        if (kr != KERN_SUCCESS || outSize != 1)
        {
            return false;
        }

        uint64_t nextPage = (probe & ~(pageSize - 1)) + pageSize;
        if (nextPage >= end)
        {
            break;
        }
        probe = nextPage;
    }

    return true;
#else
    uint64_t current = address;
    while (current < end)
    {
        MEMORY_BASIC_INFORMATION memoryInfo;
        if (VirtualQuery(reinterpret_cast<LPCVOID>(current), &memoryInfo, sizeof(memoryInfo)) != sizeof(memoryInfo))
        {
            return false;
        }

        uint64_t regionStart = reinterpret_cast<uint64_t>(memoryInfo.BaseAddress);
        uint64_t regionEnd = regionStart + memoryInfo.RegionSize;
        if (regionEnd <= current || current < regionStart)
        {
            return false;
        }

        DWORD protection = memoryInfo.Protect & 0xff;
        bool isReadable =
            protection == PAGE_READONLY ||
            protection == PAGE_READWRITE ||
            protection == PAGE_WRITECOPY ||
            protection == PAGE_EXECUTE_READ ||
            protection == PAGE_EXECUTE_READWRITE ||
            protection == PAGE_EXECUTE_WRITECOPY;

        if (memoryInfo.State != MEM_COMMIT || !isReadable)
        {
            return false;
        }

        current = regionEnd < end ? regionEnd : end;
    }

    return true;
#endif
}

static bool WriteTargetMemory(int fd, uint64_t address, uint64_t size)
{
    while (size != 0)
    {
        size_t chunkSize = static_cast<size_t>(std::min<uint64_t>(size, InProcMiniDumpWriteChunkSize));
        if (!IsReadableMemoryRange(address, chunkSize))
        {
            return false;
        }

        if (!WriteAll(fd, reinterpret_cast<const void*>(address), chunkSize))
        {
            return false;
        }

        address += chunkSize;
        size -= chunkSize;
    }

    return true;
}

InProcCrashReportDataTarget::InProcCrashReportDataTarget()
    : m_ref(1),
      m_runtimeBase(0)
{
}

bool
InProcCrashReportDataTarget::Initialize()
{
    m_runtimeBase = reinterpret_cast<CLRDATA_ADDRESS>(
        PAL_GetSymbolModuleBase(reinterpret_cast<PVOID>(&InProcMiniDumpRuntimeBaseAnchor)));
    return m_runtimeBase != 0;
}

HRESULT
InProcCrashReportDataTarget::QueryInterface(REFIID riid, void** ppvObject)
{
    if (ppvObject == nullptr)
    {
        return E_POINTER;
    }

    if (riid == IID_IUnknown || riid == __uuidof(ICLRDataTarget))
    {
        *ppvObject = static_cast<ICLRDataTarget*>(this);
        AddRef();
        return S_OK;
    }

    if (riid == __uuidof(ICLRRuntimeLocator))
    {
        *ppvObject = static_cast<ICLRRuntimeLocator*>(this);
        AddRef();
        return S_OK;
    }

    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG
InProcCrashReportDataTarget::AddRef()
{
    return InterlockedIncrement(&m_ref);
}

ULONG
InProcCrashReportDataTarget::Release()
{
    // Member-lifetime object; never delete through COM Release.
    return InterlockedDecrement(&m_ref);
}

HRESULT
InProcCrashReportDataTarget::GetMachineType(ULONG32* machine)
{
    if (machine == nullptr)
    {
        return E_POINTER;
    }

#if defined(__x86_64__)
    *machine = IMAGE_FILE_MACHINE_AMD64;
#elif defined(__aarch64__)
    *machine = IMAGE_FILE_MACHINE_ARM64;
#elif defined(__arm__)
    *machine = IMAGE_FILE_MACHINE_ARMNT;
#elif defined(__i386__)
    *machine = IMAGE_FILE_MACHINE_I386;
#else
    return E_NOTIMPL;
#endif
    return S_OK;
}

HRESULT
InProcCrashReportDataTarget::GetPointerSize(ULONG32* size)
{
    if (size == nullptr)
    {
        return E_POINTER;
    }

    *size = sizeof(void*);
    return S_OK;
}

HRESULT
InProcCrashReportDataTarget::GetImageBase(LPCWSTR moduleName, CLRDATA_ADDRESS* baseAddress)
{
    (void)moduleName;

    if (baseAddress == nullptr)
    {
        return E_POINTER;
    }

    *baseAddress = m_runtimeBase;
    return m_runtimeBase != 0 ? S_OK : E_FAIL;
}

HRESULT
InProcCrashReportDataTarget::ReadVirtual(CLRDATA_ADDRESS address, PBYTE buffer, ULONG32 size, ULONG32* done)
{
    if (buffer == nullptr)
    {
        if (done != nullptr)
        {
            *done = 0;
        }
        return E_POINTER;
    }

    if (!IsReadableMemoryRange(static_cast<uint64_t>(address), size))
    {
        if (done != nullptr)
        {
            *done = 0;
        }
        return E_FAIL;
    }

    memcpy(buffer, reinterpret_cast<const void*>(address), size);
    if (done != nullptr)
    {
        *done = size;
    }

    return S_OK;
}

HRESULT InProcCrashReportDataTarget::WriteVirtual(CLRDATA_ADDRESS, PBYTE, ULONG32, ULONG32*) { return E_NOTIMPL; }
HRESULT InProcCrashReportDataTarget::GetTLSValue(ULONG32, ULONG32, CLRDATA_ADDRESS*) { return E_NOTIMPL; }
HRESULT InProcCrashReportDataTarget::SetTLSValue(ULONG32, ULONG32, CLRDATA_ADDRESS) { return E_NOTIMPL; }
HRESULT InProcCrashReportDataTarget::GetCurrentThreadID(ULONG32*) { return E_NOTIMPL; }
HRESULT InProcCrashReportDataTarget::GetThreadContext(ULONG32, ULONG32, ULONG32, PBYTE) { return E_NOTIMPL; }
HRESULT InProcCrashReportDataTarget::SetThreadContext(ULONG32, ULONG32, PBYTE) { return E_NOTIMPL; }
HRESULT InProcCrashReportDataTarget::Request(ULONG32, ULONG32, BYTE*, ULONG32, BYTE*) { return E_NOTIMPL; }

HRESULT
InProcCrashReportDataTarget::GetRuntimeBase(CLRDATA_ADDRESS* baseAddress)
{
    if (baseAddress == nullptr)
    {
        return E_POINTER;
    }

    *baseAddress = m_runtimeBase;
    return m_runtimeBase != 0 ? S_OK : E_FAIL;
}

InProcMiniDumpWriter::InProcMiniDumpWriter()
    : m_ref(1),
      m_enumMemoryRegions(nullptr),
      m_dumpType(InProcCrashReportMiniDumpType::Mini),
      m_regionCount(0),
      m_droppedRegionCount(0),
      m_failureReason("not initialized")
{
}

InProcMiniDumpWriter::~InProcMiniDumpWriter()
{
    if (m_enumMemoryRegions != nullptr)
    {
        m_enumMemoryRegions->Release();
        m_enumMemoryRegions = nullptr;
    }
}

bool
InProcMiniDumpWriter::Initialize(InProcCrashReportMiniDumpType dumpType)
{
    m_dumpType = dumpType;
    if (!m_dataTarget.Initialize())
    {
        SetFailureReason("runtime base unavailable");
        return false;
    }

    ICLRDataEnumMemoryRegions* enumMemoryRegions = nullptr;
    HRESULT hr = CLRDataCreateInstance(__uuidof(ICLRDataEnumMemoryRegions), &m_dataTarget, reinterpret_cast<void**>(&enumMemoryRegions));
    if (FAILED(hr) || enumMemoryRegions == nullptr)
    {
        SetFailureReason("cdaclite initialization failed");
        return false;
    }

    m_enumMemoryRegions = enumMemoryRegions;
    SetFailureReason("");
    return true;
}

void
InProcMiniDumpWriter::ResetRegions()
{
    m_regionCount = 0;
    m_droppedRegionCount = 0;
}

void
InProcMiniDumpWriter::NormalizeRegions()
{
    std::sort(m_regions, m_regions + m_regionCount, [](const Region& left, const Region& right)
    {
        return left.start < right.start ||
            (left.start == right.start && left.size < right.size);
    });

    uint32_t writeIndex = 0;
    for (uint32_t readIndex = 0; readIndex < m_regionCount; readIndex++)
    {
        Region current = m_regions[readIndex];
        if (current.size == 0)
        {
            continue;
        }

        if (writeIndex == 0)
        {
            if (IsReadableMemoryRange(current.start, current.size))
            {
                m_regions[writeIndex++] = current;
            }
            else
            {
                m_droppedRegionCount++;
            }
            continue;
        }

        Region& previous = m_regions[writeIndex - 1];
        uint64_t previousEnd = previous.start + previous.size;
        uint64_t currentEnd = current.start + current.size;
        if (previousEnd >= current.start)
        {
            if (currentEnd > previousEnd)
            {
                previous.size = currentEnd - previous.start;
            }
        }
        else
        {
            if (IsReadableMemoryRange(current.start, current.size))
            {
                m_regions[writeIndex++] = current;
            }
            else
            {
                m_droppedRegionCount++;
            }
        }
    }

    m_regionCount = writeIndex;
}

bool
InProcMiniDumpWriter::WriteDump(int fd)
{
    if (fd < 0)
    {
        SetFailureReason("invalid dump fd");
        return false;
    }

    if (m_enumMemoryRegions == nullptr)
    {
        SetFailureReason("cdaclite unavailable");
        return false;
    }

    ResetRegions();
    ULONG32 miniDumpFlags = m_dumpType == InProcCrashReportMiniDumpType::WithHeap
        ? MiniDumpWithPrivateReadWriteMemoryFlag
        : 0;
    HRESULT hr = m_enumMemoryRegions->EnumMemoryRegions(this, miniDumpFlags, CLRDATA_ENUM_MEM_DEFAULT);
    if (FAILED(hr))
    {
        SetFailureReason("cdaclite memory enumeration failed");
        return false;
    }

    if (m_regionCount == 0)
    {
        SetFailureReason("cdaclite returned no memory regions");
        return false;
    }

    NormalizeRegions();
    return WritePlatformDump(fd);
}

HRESULT
InProcMiniDumpWriter::QueryInterface(REFIID riid, void** ppvObject)
{
    if (ppvObject == nullptr)
    {
        return E_POINTER;
    }

    if (riid == IID_IUnknown || riid == __uuidof(ICLRDataEnumMemoryRegionsCallback))
    {
        *ppvObject = static_cast<ICLRDataEnumMemoryRegionsCallback*>(this);
        AddRef();
        return S_OK;
    }

    if (riid == __uuidof(ICLRDataLoggingCallback))
    {
        *ppvObject = static_cast<ICLRDataLoggingCallback*>(this);
        AddRef();
        return S_OK;
    }

    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG
InProcMiniDumpWriter::AddRef()
{
    return InterlockedIncrement(&m_ref);
}

ULONG
InProcMiniDumpWriter::Release()
{
    // Member-lifetime object; never delete through COM Release.
    return InterlockedDecrement(&m_ref);
}

HRESULT
InProcMiniDumpWriter::EnumMemoryRegion(CLRDATA_ADDRESS address, ULONG32 size)
{
    if (address == 0 || size == 0)
    {
        return S_OK;
    }

    if (m_regionCount == MaxRegions)
    {
        m_droppedRegionCount++;
        return S_OK;
    }

    m_regions[m_regionCount].start = static_cast<uint64_t>(address);
    m_regions[m_regionCount].size = static_cast<uint64_t>(size);
    m_regionCount++;
    return S_OK;
}

HRESULT
InProcMiniDumpWriter::LogMessage(LPCSTR)
{
    return S_OK;
}

bool
InProcMiniDumpWriter::WriteSpecialDiagInfo(int fd)
{
    SpecialDiagInfoHeader header = {
        { "DIAGINFOHEADER" },
        2,
        0,
        m_dataTarget.RuntimeBase()
    };

    if (!WriteAll(fd, &header, sizeof(header)))
    {
        return false;
    }

    uint8_t zeroBuffer[256] = {};
    uint64_t remaining = SpecialDiagInfoSize - sizeof(header);
    while (remaining != 0)
    {
        size_t toWrite = static_cast<size_t>(std::min<uint64_t>(remaining, sizeof(zeroBuffer)));
        if (!WriteAll(fd, zeroBuffer, toWrite))
        {
            return false;
        }
        remaining -= toWrite;
    }

    return true;
}

#if defined(TARGET_APPLE)

bool
InProcMiniDumpWriter::WritePlatformDump(int fd)
{
    mach_header_64 header;
    memset(&header, 0, sizeof(header));
    header.magic = MH_MAGIC_64;
#if defined(__x86_64__)
    header.cputype = CPU_TYPE_X86_64;
    header.cpusubtype = CPU_SUBTYPE_X86_64_ALL;
#elif defined(__aarch64__)
    header.cputype = CPU_TYPE_ARM64;
    header.cpusubtype = CPU_SUBTYPE_ARM64_ALL;
#else
    SetFailureReason("unsupported Mach-O architecture");
    return false;
#endif
    header.filetype = MH_CORE;
    uint32_t segmentCount = m_regionCount + 1;
    header.ncmds = segmentCount;
    header.sizeofcmds = segmentCount * sizeof(segment_command_64);

    if (!WriteAll(fd, &header, sizeof(header)))
    {
        SetFailureReason("failed writing Mach-O header");
        return false;
    }

    uint64_t fileOffset = sizeof(header) + header.sizeofcmds;
    for (uint32_t i = 0; i < m_regionCount; i++)
    {
        segment_command_64 segment;
        memset(&segment, 0, sizeof(segment));
        segment.cmd = LC_SEGMENT_64;
        segment.cmdsize = sizeof(segment);
        segment.vmaddr = m_regions[i].start;
        segment.vmsize = m_regions[i].size;
        segment.fileoff = fileOffset;
        segment.filesize = m_regions[i].size;
        segment.maxprot = VM_PROT_READ;
        segment.initprot = VM_PROT_READ;
        fileOffset += m_regions[i].size;

        if (!WriteAll(fd, &segment, sizeof(segment)))
        {
            SetFailureReason("failed writing Mach-O segment command");
            return false;
        }
    }

    segment_command_64 diagSegment;
    memset(&diagSegment, 0, sizeof(diagSegment));
    diagSegment.cmd = LC_SEGMENT_64;
    diagSegment.cmdsize = sizeof(diagSegment);
    diagSegment.vmaddr = SpecialDiagInfoAddress;
    diagSegment.vmsize = SpecialDiagInfoSize;
    diagSegment.fileoff = fileOffset;
    diagSegment.filesize = SpecialDiagInfoSize;
    diagSegment.maxprot = VM_PROT_READ;
    diagSegment.initprot = VM_PROT_READ;
    fileOffset += SpecialDiagInfoSize;

    if (!WriteAll(fd, &diagSegment, sizeof(diagSegment)))
    {
        SetFailureReason("failed writing Mach-O special diag segment command");
        return false;
    }

    for (uint32_t i = 0; i < m_regionCount; i++)
    {
        if (!WriteTargetMemory(fd, m_regions[i].start, m_regions[i].size))
        {
            SetFailureReason("failed writing Mach-O memory segment");
            return false;
        }
    }

    if (!WriteSpecialDiagInfo(fd))
    {
        SetFailureReason("failed writing Mach-O special diag info");
        return false;
    }

    SetFailureReason("");
    return true;
}

#else // TARGET_APPLE

bool
InProcMiniDumpWriter::WritePlatformDump(int fd)
{
    uint32_t segmentCount = m_regionCount + 1;
    if (segmentCount >= PN_XNUM)
    {
        SetFailureReason("too many ELF program headers");
        return false;
    }

    ElfW(Ehdr) header;
    memset(&header, 0, sizeof(header));
    header.e_ident[EI_MAG0] = ELFMAG0;
    header.e_ident[EI_MAG1] = ELFMAG1;
    header.e_ident[EI_MAG2] = ELFMAG2;
    header.e_ident[EI_MAG3] = ELFMAG3;
#if defined(HOST_64BIT)
    header.e_ident[EI_CLASS] = ELFCLASS64;
#else
    header.e_ident[EI_CLASS] = ELFCLASS32;
#endif
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_ident[EI_OSABI] = ELFOSABI_LINUX;
    header.e_type = ET_CORE;
#if defined(__x86_64__)
    header.e_machine = EM_X86_64;
#elif defined(__i386__)
    header.e_machine = EM_386;
#elif defined(__aarch64__)
    header.e_machine = EM_AARCH64;
#elif defined(__arm__)
    header.e_machine = EM_ARM;
#else
    SetFailureReason("unsupported ELF architecture");
    return false;
#endif
    header.e_version = EV_CURRENT;
    header.e_phoff = sizeof(header);
    header.e_ehsize = sizeof(header);
    header.e_phentsize = sizeof(ElfW(Phdr));
    header.e_phnum = static_cast<uint16_t>(segmentCount);

    if (!WriteAll(fd, &header, sizeof(header)))
    {
        SetFailureReason("failed writing ELF header");
        return false;
    }

    uint64_t fileOffset = sizeof(header) + (static_cast<uint64_t>(segmentCount) * sizeof(ElfW(Phdr)));
    for (uint32_t i = 0; i < m_regionCount; i++)
    {
        ElfW(Phdr) phdr;
        memset(&phdr, 0, sizeof(phdr));
        phdr.p_type = PT_LOAD;
        phdr.p_flags = PF_R;
        phdr.p_offset = fileOffset;
        phdr.p_vaddr = m_regions[i].start;
        phdr.p_filesz = m_regions[i].size;
        phdr.p_memsz = m_regions[i].size;
        phdr.p_align = 1;
        fileOffset += m_regions[i].size;

        if (!WriteAll(fd, &phdr, sizeof(phdr)))
        {
            SetFailureReason("failed writing ELF program header");
            return false;
        }
    }

    ElfW(Phdr) diagPhdr;
    memset(&diagPhdr, 0, sizeof(diagPhdr));
    diagPhdr.p_type = PT_LOAD;
    diagPhdr.p_flags = PF_R;
    diagPhdr.p_offset = fileOffset;
    diagPhdr.p_vaddr = SpecialDiagInfoAddress;
    diagPhdr.p_filesz = SpecialDiagInfoSize;
    diagPhdr.p_memsz = SpecialDiagInfoSize;
    diagPhdr.p_align = 1;
    fileOffset += SpecialDiagInfoSize;

    if (!WriteAll(fd, &diagPhdr, sizeof(diagPhdr)))
    {
        SetFailureReason("failed writing ELF special diag program header");
        return false;
    }

    for (uint32_t i = 0; i < m_regionCount; i++)
    {
        if (!WriteTargetMemory(fd, m_regions[i].start, m_regions[i].size))
        {
            SetFailureReason("failed writing ELF memory segment");
            return false;
        }
    }

    if (!WriteSpecialDiagInfo(fd))
    {
        SetFailureReason("failed writing ELF special diag info");
        return false;
    }

    SetFailureReason("");
    return true;
}

#endif // TARGET_APPLE
