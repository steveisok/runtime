// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// In-process crash report writer for Linux/Android/Apple.
//
// Wraps the shared JsonWriter (jsonwriter.h) with a fixed 32 KB buffer
// sink and provides CrashReport_* convenience functions.

#pragma once

#include <signal.h>
#include <stdint.h>
#include "jsonwriter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CRASH_REPORT_BUFFER_SIZE (32 * 1024)

struct CrashReportWriter
{
    char buffer[CRASH_REPORT_BUFFER_SIZE];
    JsonBufferSink sink;
    JsonWriter json;
};

void CrashReport_Init(CrashReportWriter* w);

// Convenience wrappers — delegate to the shared JsonWriter.
static inline void CrashReport_OpenObject(CrashReportWriter* w, const char* key)  { JsonWriter_OpenObject(&w->json, key); }
static inline void CrashReport_CloseObject(CrashReportWriter* w)                  { JsonWriter_CloseObject(&w->json); }
static inline void CrashReport_OpenArray(CrashReportWriter* w, const char* key)    { JsonWriter_OpenArray(&w->json, key); }
static inline void CrashReport_CloseArray(CrashReportWriter* w)                    { JsonWriter_CloseArray(&w->json); }
static inline void CrashReport_WriteString(CrashReportWriter* w, const char* key, const char* value) { JsonWriter_WriteString(&w->json, key, value); }
static inline void CrashReport_WriteInt(CrashReportWriter* w, const char* key, int64_t value)        { JsonWriter_WriteInt(&w->json, key, value); }
static inline void CrashReport_WriteHex(CrashReportWriter* w, const char* key, uint64_t value)       { JsonWriter_WriteHex(&w->json, key, value); }
static inline void CrashReport_WriteBool(CrashReportWriter* w, const char* key, int value)           { JsonWriter_WriteBool(&w->json, key, value); }

static inline int         CrashReport_GetLength(const CrashReportWriter* w) { return w->sink.pos; }
static inline const char* CrashReport_GetBuffer(const CrashReportWriter* w) { return w->buffer; }

// High-level: write the complete crash report JSON envelope.
void CrashReport_Generate(int signal, siginfo_t* siginfo, void* context);

// Initialize crash report settings from environment at startup.
void CrashReport_Initialize(void);

// Returns 1 if crash reporting is enabled, 0 otherwise.
int CrashReport_IsEnabled(void);

// Callback types for VM integration.

typedef void (*CrashReport_ManagedFrameCallback)(
    const char* methodName,
    const char* moduleName,
    uint32_t token,
    uint32_t nativeOffset,
    uint32_t ilOffset,
    uint32_t timeStamp,
    uint32_t imageSize,
    const char* mvid,
    void* context);

typedef void (*CrashReport_GenerateCallback)(
    int signal,
    siginfo_t* siginfo,
    void* signalContext,
    void* managedContext,
    CrashReportWriter* writer,
    CrashReport_ManagedFrameCallback frameCallback);

void CrashReport_SetGenerateCallback(CrashReport_GenerateCallback callback);

#ifdef __cplusplus
}
#endif
