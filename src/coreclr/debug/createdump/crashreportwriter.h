// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#pragma once

#include "jsonwriter.h"

#define JSON_INDENT_VALUE 1

class CrashReportWriter
{
private:
    JsonFileSink m_sink;
    JsonWriter m_json;
    CrashInfo& m_crashInfo;

    // no public copy constructor
    CrashReportWriter(const CrashReportWriter&) = delete;
    void operator=(const CrashReportWriter&) = delete;

public:
    CrashReportWriter(CrashInfo& crashInfo);
    virtual ~CrashReportWriter();
    void WriteCrashReport(const std::string& dumpFileName);

private:
    void WriteCrashReport();
#ifdef __APPLE__
    void WriteSysctl(const char* sysctlname, const char* valueName);
#endif
    void WriteStackFrame(const StackFrame& frame);
    bool OpenWriter(const char* fileName);
    void CloseWriter();

    // Convenience wrappers for the shared JsonWriter
    void OpenObject(const char* key = nullptr)            { JsonWriter_OpenObject(&m_json, key); }
    void CloseObject()                                     { JsonWriter_CloseObject(&m_json); }
    void OpenArray(const char* key)                        { JsonWriter_OpenArray(&m_json, key); }
    void CloseArray()                                      { JsonWriter_CloseArray(&m_json); }
    void WriteValue(const char* key, const char* value)    { JsonWriter_WriteString(&m_json, key, value); }
    void WriteValueBool(const char* key, bool value)       { JsonWriter_WriteBool(&m_json, key, value ? 1 : 0); }
    void WriteValue32(const char* key, uint32_t value)     { JsonWriter_WriteHex(&m_json, key, (uint64_t)value); }
    void WriteValue64(const char* key, uint64_t value)     { JsonWriter_WriteHex(&m_json, key, value); }
};
