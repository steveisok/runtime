// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <windows.h>
#include <cor.h>
#include <corhdr.h>
#include <clrdata.h>
#include <xclrdata.h>

#include "inproccrashreporter.h"

class InProcCrashReportDataTarget final : public ICLRDataTarget, public ICLRRuntimeLocator
{
public:
    InProcCrashReportDataTarget();

    InProcCrashReportDataTarget(const InProcCrashReportDataTarget&) = delete;
    InProcCrashReportDataTarget& operator=(const InProcCrashReportDataTarget&) = delete;

    bool Initialize();
    uint64_t RuntimeBase() const { return static_cast<uint64_t>(m_runtimeBase); }

    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObject) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    STDMETHOD(GetMachineType)(ULONG32* machine) override;
    STDMETHOD(GetPointerSize)(ULONG32* size) override;
    STDMETHOD(GetImageBase)(LPCWSTR moduleName, CLRDATA_ADDRESS* baseAddress) override;
    STDMETHOD(ReadVirtual)(CLRDATA_ADDRESS address, PBYTE buffer, ULONG32 size, ULONG32* done) override;
    STDMETHOD(WriteVirtual)(CLRDATA_ADDRESS address, PBYTE buffer, ULONG32 size, ULONG32* done) override;
    STDMETHOD(GetTLSValue)(ULONG32 threadID, ULONG32 index, CLRDATA_ADDRESS* value) override;
    STDMETHOD(SetTLSValue)(ULONG32 threadID, ULONG32 index, CLRDATA_ADDRESS value) override;
    STDMETHOD(GetCurrentThreadID)(ULONG32* threadID) override;
    STDMETHOD(GetThreadContext)(ULONG32 threadID, ULONG32 contextFlags, ULONG32 contextSize, PBYTE context) override;
    STDMETHOD(SetThreadContext)(ULONG32 threadID, ULONG32 contextSize, PBYTE context) override;
    STDMETHOD(Request)(ULONG32 reqCode, ULONG32 inBufferSize, BYTE* inBuffer, ULONG32 outBufferSize, BYTE* outBuffer) override;

    STDMETHOD(GetRuntimeBase)(CLRDATA_ADDRESS* baseAddress) override;

private:
    LONG m_ref;
    CLRDATA_ADDRESS m_runtimeBase;
};

class InProcMiniDumpWriter final : public ICLRDataEnumMemoryRegionsCallback, public ICLRDataLoggingCallback
{
public:
    InProcMiniDumpWriter();
    ~InProcMiniDumpWriter();

    InProcMiniDumpWriter(const InProcMiniDumpWriter&) = delete;
    InProcMiniDumpWriter& operator=(const InProcMiniDumpWriter&) = delete;

    bool Initialize(InProcCrashReportMiniDumpType dumpType);
    bool IsEnabled() const { return m_enumMemoryRegions != nullptr; }
    bool WriteDump(int fd);

    const char* FailureReason() const { return m_failureReason; }
    uint32_t RegionCount() const { return m_regionCount; }
    uint32_t DroppedRegionCount() const { return m_droppedRegionCount; }

    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObject) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(EnumMemoryRegion)(CLRDATA_ADDRESS address, ULONG32 size) override;
    STDMETHOD(LogMessage)(LPCSTR message) override;

private:
    struct Region
    {
        uint64_t start;
        uint64_t size;
    };

    static constexpr uint32_t MaxRegions = 4096;

    void ResetRegions();
    void NormalizeRegions();
    bool WriteSpecialDiagInfo(int fd);
    bool WritePlatformDump(int fd);
    void SetFailureReason(const char* reason) { m_failureReason = reason; }

    LONG m_ref;
    InProcCrashReportDataTarget m_dataTarget;
    ICLRDataEnumMemoryRegions* m_enumMemoryRegions;
    InProcCrashReportMiniDumpType m_dumpType;
    Region m_regions[MaxRegions];
    uint32_t m_regionCount;
    uint32_t m_droppedRegionCount;
    const char* m_failureReason;
};
