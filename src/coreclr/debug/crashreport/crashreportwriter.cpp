// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// In-process crash report generator for Linux/Android/Apple.
//
// Uses the shared JsonWriter (jsonwriter.h) with a buffer sink for
// zero-allocation JSON output. The high-level CrashReport_Generate
// function builds the complete crash report JSON envelope.
//
// This is best-effort code — it is NOT async-signal-safe.

// This file is compiled as part of the PAL static library which is linked
// into both libcoreclr.so and libclrjit.so. The crash reporter is only needed
// in coreclr, so guard the entire implementation to avoid pulling in a 32 KB
// static buffer and unnecessary code into the JIT.
#ifdef JIT_STANDALONE_BUILD
// Intentionally empty — crash report writer is not used in the JIT.
#else

#include "pal/palinternal.h"
#include "crashreportwriter.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <minipal/log.h>
#include "pal/context.h"

#ifdef __APPLE__
#include <pthread.h>
#endif

// ---------------------------------------------------------------------------
// CrashReportWriter initialization
// ---------------------------------------------------------------------------

void CrashReport_Init(CrashReportWriter* w)
{
    JsonBufferSink_Init(&w->sink, w->buffer, CRASH_REPORT_BUFFER_SIZE);
    JsonWriter_Init(&w->json, JsonBufferSink_Emit, &w->sink, 0, JsonWriter_Compact);
}

// ---------------------------------------------------------------------------
// Startup configuration
// ---------------------------------------------------------------------------

static volatile int g_crashReportEnabled = 0;
static char g_crashReportPath[256];
static char g_crashReportDefaultDir[256];
static volatile CrashReport_GenerateCallback g_generateCallback = NULL;

void CrashReport_Initialize(void)
{
    // Check DOTNET_EnableInProcessCrashReport — distinct from DOTNET_EnableCrashReport
    // which controls createdump's JSON crash report alongside the core dump.
    const char* enabledStr = getenv("DOTNET_EnableInProcessCrashReport");
    if (enabledStr != NULL && enabledStr[0] == '1')
    {
        g_crashReportEnabled = 1;
    }

    // Optional: custom output path
    const char* dumpName = getenv("DOTNET_DbgMiniDumpName");
    if (dumpName != NULL)
    {
        snprintf(g_crashReportPath, sizeof(g_crashReportPath), "%s", dumpName);
    }

    // Optional: default directory for crash reports
    const char* defaultDir = getenv("DOTNET_CrashReportDirectory");
    if (defaultDir != NULL)
    {
        snprintf(g_crashReportDefaultDir, sizeof(g_crashReportDefaultDir), "%s", defaultDir);
    }
}

int CrashReport_IsEnabled(void)
{
    return g_crashReportEnabled;
}

void CrashReport_SetGenerateCallback(CrashReport_GenerateCallback callback)
{
    g_generateCallback = callback;
}

// ---------------------------------------------------------------------------
// Signal → exception type code mapping (matches Mitch's schema)
// ---------------------------------------------------------------------------

static const char* GetExceptionTypeCode(int signal)
{
    switch (signal)
    {
        case SIGSEGV: return "0x20000000";
        case SIGABRT: return "0x30000000";
        case SIGBUS:  return "0x60000000";
        case SIGILL:  return "0x50000000";
        case SIGFPE:  return "0x70000000";
        case SIGTRAP: return "0x03000000";
        case SIGTERM: return "0x02000000";
        default:      return "0x00000000";
    }
}

// ---------------------------------------------------------------------------
// Register context extraction from ucontext_t
// ---------------------------------------------------------------------------

static void WriteRegistersToJson(CrashReportWriter* w, void* context)
{
    ucontext_t* uctx = (ucontext_t*)context;
    if (uctx == NULL)
        return;

    CrashReport_OpenObject(w, "ctx");
#if defined(__APPLE__) && defined(__x86_64__)
    CrashReport_WriteHex(w, "IP", uctx->uc_mcontext->__ss.__rip);
    CrashReport_WriteHex(w, "SP", uctx->uc_mcontext->__ss.__rsp);
    CrashReport_WriteHex(w, "BP", uctx->uc_mcontext->__ss.__rbp);
#elif defined(__APPLE__) && defined(__aarch64__)
    CrashReport_WriteHex(w, "IP", arm_thread_state64_get_pc(uctx->uc_mcontext->__ss));
    CrashReport_WriteHex(w, "SP", arm_thread_state64_get_sp(uctx->uc_mcontext->__ss));
    CrashReport_WriteHex(w, "BP", arm_thread_state64_get_fp(uctx->uc_mcontext->__ss));
#elif defined(__x86_64__)
    CrashReport_WriteHex(w, "IP", uctx->uc_mcontext.gregs[REG_RIP]);
    CrashReport_WriteHex(w, "SP", uctx->uc_mcontext.gregs[REG_RSP]);
    CrashReport_WriteHex(w, "BP", uctx->uc_mcontext.gregs[REG_RBP]);
#elif defined(__aarch64__)
    CrashReport_WriteHex(w, "IP", uctx->uc_mcontext.pc);
    CrashReport_WriteHex(w, "SP", uctx->uc_mcontext.sp);
    CrashReport_WriteHex(w, "BP", uctx->uc_mcontext.regs[29]);
#elif defined(__arm__)
    CrashReport_WriteHex(w, "IP", uctx->uc_mcontext.arm_pc);
    CrashReport_WriteHex(w, "SP", uctx->uc_mcontext.arm_sp);
    CrashReport_WriteHex(w, "BP", uctx->uc_mcontext.arm_fp);
#endif
    CrashReport_CloseObject(w);
}

// ---------------------------------------------------------------------------
// File output
// ---------------------------------------------------------------------------

static void WriteReportToFile(const char* json, int jsonLen)
{
    char reportPath[256];

    if (g_crashReportPath[0] != '\0')
    {
        snprintf(reportPath, sizeof(reportPath), "%s.crashreport.json", g_crashReportPath);
    }
    else
    {
        // TMPDIR is set by Android to the app's cache directory (e.g.,
        // /data/user/0/<package>/cache), which is always writable.
        // /data/local/tmp is only accessible to debuggable apps via adb.
        const char* directory = g_crashReportDefaultDir[0] != '\0' ? g_crashReportDefaultDir : getenv("TMPDIR");
        if (directory == NULL || directory[0] == '\0')
            directory = "/data/local/tmp";
        snprintf(reportPath, sizeof(reportPath), "%s/dotnet_crash_%d.crashreport.json", directory, (int)getpid());
    }

    int fd = open(reportPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd != -1)
    {
        (void)write(fd, json, jsonLen);
        (void)write(fd, "\n", 1);
        close(fd);

        char logMsg[300];
        snprintf(logMsg, sizeof(logMsg), "Crash report written to: %s", reportPath);
        minipal_log_write_info(logMsg);
    }
    else
    {
        char logMsg[300];
        snprintf(logMsg, sizeof(logMsg), "Failed to write crash report to: %s (errno=%d)", reportPath, errno);
        minipal_log_write_error(logMsg);
    }
}

// ---------------------------------------------------------------------------
// High-level crash report generation
// ---------------------------------------------------------------------------

// Serialization: only one thread generates the report.
static volatile pid_t g_crashReportThreadId = 0;

void CrashReport_Generate(int signal, siginfo_t* siginfo, void* context)
{
    // One-thread-wins serialization via CAS
#ifdef __APPLE__
    uint64_t tid64;
    pthread_threadid_np(NULL, &tid64);
    pid_t currentTid = (pid_t)tid64;
#else
    pid_t currentTid = gettid();
#endif
    pid_t expected = 0;
    if (!__atomic_compare_exchange_n(&g_crashReportThreadId, &expected, currentTid, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    {
        return;
    }

    // Static writer — safe because the CAS above guarantees single-threaded access.
    static CrashReportWriter writer;
    CrashReport_Init(&writer);

    // Root object
    CrashReport_OpenObject(&writer, NULL);

    // payload
    CrashReport_OpenObject(&writer, "payload");
    CrashReport_WriteString(&writer, "protocol_version", "1.0.0");

    // configuration
    CrashReport_OpenObject(&writer, "configuration");
#if defined(__x86_64__)
    CrashReport_WriteString(&writer, "architecture", "amd64");
#elif defined(__aarch64__)
    CrashReport_WriteString(&writer, "architecture", "arm64");
#elif defined(__arm__)
    CrashReport_WriteString(&writer, "architecture", "arm");
#else
    CrashReport_WriteString(&writer, "architecture", "unknown");
#endif
    CrashReport_CloseObject(&writer); // configuration

    // threads array — the VM callback writes all thread entries (crashing
    // thread first, then other managed threads from the ThreadStore).
    CrashReport_OpenArray(&writer, "threads");

    if (g_generateCallback != NULL)
    {
        // Convert the native signal context to a CONTEXT structure that the
        // VM can use directly (e.g. to seed the managed stack walker).
        CONTEXT managedContext;
        memset(&managedContext, 0, sizeof(managedContext));
        if (context != NULL)
        {
            managedContext.ContextFlags = CONTEXT_FULL;
            CONTEXTFromNativeContext((native_context_t*)context, &managedContext, CONTEXT_FULL);
        }

        // VM callback writes all thread entries with managed frames + exception info
        g_generateCallback(signal, siginfo, context, &managedContext, &writer, NULL);
    }
    else
    {
        // No VM callback — minimal: single native thread with registers only
        CrashReport_OpenObject(&writer, NULL);
        CrashReport_WriteBool(&writer, "crashed", 1);
        CrashReport_WriteHex(&writer, "native_thread_id", (uint64_t)currentTid);
        CrashReport_WriteBool(&writer, "is_managed", 0);
        WriteRegistersToJson(&writer, context);
        CrashReport_OpenArray(&writer, "stack_frames");
        CrashReport_CloseArray(&writer);
        CrashReport_CloseObject(&writer);
    }

    CrashReport_CloseArray(&writer);  // threads
    CrashReport_CloseObject(&writer); // payload

    // parameters
    CrashReport_OpenObject(&writer, "parameters");
    CrashReport_WriteString(&writer, "ExceptionType", GetExceptionTypeCode(signal));
    CrashReport_CloseObject(&writer);

    CrashReport_CloseObject(&writer); // root

    // Emit to logcat
    const char* json = CrashReport_GetBuffer(&writer);
    int jsonLen = CrashReport_GetLength(&writer);
    minipal_log_write_fatal(json);

    // Write to file
    WriteReportToFile(json, jsonLen);
}

#endif // !JIT_STANDALONE_BUILD
