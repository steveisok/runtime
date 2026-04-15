// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// VM-side crash report helper for Android.
//
// Walks managed threads' stacks and exception state, feeding frame data
// back into the PAL crash report writer. The crashing thread is written
// first, then all other live managed threads from the ThreadStore.
//
// This is best-effort code that uses live VM inspection — a secondary
// crash is possible but there is no secondary-fault guard
// (sigsetjmp/siglongjmp). If the VM state is corrupted, the crash
// handler itself will fault and the process terminates.

#include "common.h"

#if defined(TARGET_LINUX) || defined(TARGET_ANDROID) || defined(TARGET_APPLE)

#include "crashreporthelper.h"
#include "threads.h"
#include "stackwalk.h"
#include "method.hpp"
#include "class.h"
#include "assembly.hpp"
#include "peassembly.h"
#include <minipal/guid.h>

#ifdef TARGET_APPLE
#include <pthread.h>
#include <mach/mach.h>
#endif

#include "crashreportwriter.h"

// Context passed through the stack walk callback.
struct CrashFrameContext
{
    CrashReportWriter* writer;
    int frameCount;
};

static StackWalkAction CrashReportStackWalkCallback(CrawlFrame* pCF, VOID* pData)
{
    WRAPPER_NO_CONTRACT;

    CrashFrameContext* ctx = (CrashFrameContext*)pData;

    MethodDesc* pMD = pCF->GetFunction();
    if (pMD == nullptr)
        return SWA_CONTINUE;

    CrashReport_OpenObject(ctx->writer, NULL);
    CrashReport_WriteBool(ctx->writer, "is_managed", 1);

    // Method and class name extraction. These may allocate or throw,
    // so protect with EX_TRY to avoid aborting the entire walk.
    EX_TRY
    {
        LPCUTF8 methodName = pMD->GetName();
        if (methodName != nullptr)
        {
            MethodTable* pMT = pMD->GetMethodTable();
            if (pMT != nullptr)
            {
                DefineFullyQualifiedNameForClass();
                LPCUTF8 className = GetFullyQualifiedNameForClass(pMT);
                if (className != nullptr)
                {
                    char fullName[256];
                    snprintf(fullName, sizeof(fullName), "%s.%s", className, methodName);
                    CrashReport_WriteString(ctx->writer, "method_name", fullName);
                }
                else
                {
                    CrashReport_WriteString(ctx->writer, "method_name", methodName);
                }
            }
            else
            {
                CrashReport_WriteString(ctx->writer, "method_name", methodName);
            }
        }
    }
    EX_CATCH
    {
        // If name resolution fails, emit what we can
        CrashReport_WriteString(ctx->writer, "method_name", "<unknown>");
    }
    EX_END_CATCH

    // Token
    mdMethodDef token = pMD->GetMemberDef();
    CrashReport_WriteHex(ctx->writer, "token", (uint64_t)token);

    // Native offset — FUNCTIONSONLY ensures this is a jitted frame with CodeInfo.
    DWORD nativeOffset = pCF->GetCodeInfo()->GetRelOffset();
    CrashReport_WriteHex(ctx->writer, "native_offset", (uint64_t)nativeOffset);

    // IL offset — requires debug info, emit 0 for v1
    CrashReport_WriteHex(ctx->writer, "il_offset", 0);

    // Module info — also protected since metadata access can throw
    EX_TRY
    {
        Module* pModule = pMD->GetModule();
        if (pModule != nullptr)
        {
            Assembly* pAssembly = pModule->GetAssembly();
            if (pAssembly != nullptr)
            {
                LPCUTF8 simpleName = pAssembly->GetSimpleName();
                if (simpleName != nullptr)
                {
                    char dllName[128];
                    snprintf(dllName, sizeof(dllName), "%s.dll", simpleName);
                    CrashReport_WriteString(ctx->writer, "filename", dllName);
                }
            }

            // PE timestamp
            PEAssembly* pPEAssembly = pModule->GetPEAssembly();
            if (pPEAssembly != nullptr)
            {
                ULONG timeStamp = pPEAssembly->GetPEImageTimeDateStamp();
                CrashReport_WriteHex(ctx->writer, "timestamp", (uint64_t)timeStamp);
            }

            // MVID
            GUID mvid;
            if (SUCCEEDED(pModule->GetMDImport()->GetScopeProps(nullptr, &mvid)))
            {
                char mvidStr[MINIPAL_GUID_BUFFER_LEN];
                minipal_guid_as_string(mvid, mvidStr, sizeof(mvidStr));
                CrashReport_WriteString(ctx->writer, "guid", mvidStr);
            }
        }
    }
    EX_CATCH
    {
        // Module info unavailable — partial frame is still useful
    }
    EX_END_CATCH

    CrashReport_CloseObject(ctx->writer);
    ctx->frameCount++;

    return SWA_CONTINUE;
}

// Write exception info for a thread, if available.
static void WriteExceptionInfo(CrashReportWriter* writer, Thread* pThread)
{
    WRAPPER_NO_CONTRACT;

    EX_TRY
    {
        ThreadExceptionState* pExState = pThread->GetExceptionState();
        if (pExState != nullptr)
        {
            OBJECTREF throwable = pExState->GetThrowable();
            if (throwable != NULL)
            {
                MethodTable* pMT = throwable->GetMethodTable();
                if (pMT != nullptr)
                {
                    DefineFullyQualifiedNameForClass();
                    LPCUTF8 exTypeName = GetFullyQualifiedNameForClass(pMT);
                    if (exTypeName != nullptr)
                    {
                        CrashReport_WriteString(writer, "managed_exception_type", exTypeName);
                    }
                }
                CrashReport_WriteHex(writer, "managed_exception_object", (uint64_t)OBJECTREFToObject(throwable));
            }
        }
    }
    EX_CATCH
    {
    }
    EX_END_CATCH
}

// Walk managed frames for a single thread and write them as a JSON array.
static void WalkAndWriteFrames(CrashReportWriter* writer, Thread* pThread, T_CONTEXT* filterCtx)
{
    WRAPPER_NO_CONTRACT;

    CrashReport_OpenArray(writer, "stack_frames");

    T_CONTEXT* savedFilterContext = pThread->GetFilterContext();
    if (filterCtx != nullptr)
    {
        pThread->SetFilterContext(filterCtx);
    }

    CrashFrameContext frameCtx;
    frameCtx.writer = writer;
    frameCtx.frameCount = 0;

    EX_TRY
    {
        pThread->StackWalkFrames(
            &CrashReportStackWalkCallback,
            &frameCtx,
            QUICKUNWIND | FUNCTIONSONLY | ALLOW_ASYNC_STACK_WALK);
    }
    EX_CATCH
    {
    }
    EX_END_CATCH

    pThread->SetFilterContext(savedFilterContext);

    CrashReport_CloseArray(writer);
}

// Write a single thread entry to the JSON threads array.
// For the crashing thread, signalContext and managedContext carry the
// register state at the crash site. For other threads these are null.
static void WriteThreadEntry(
    CrashReportWriter* writer,
    Thread* pThread,
    bool isCrashed,
    void* signalContext,
    T_CONTEXT* managedContext)
{
    WRAPPER_NO_CONTRACT;

    CrashReport_OpenObject(writer, NULL);
    CrashReport_WriteBool(writer, "crashed", isCrashed ? 1 : 0);
    CrashReport_WriteHex(writer, "native_thread_id", (uint64_t)pThread->GetOSThreadId());
    CrashReport_WriteBool(writer, "is_managed", 1);

    WriteExceptionInfo(writer, pThread);

    if (signalContext != nullptr)
    {
        CrashReport_OpenObject(writer, "ctx");
        ucontext_t* uctx = (ucontext_t*)signalContext;
#if defined(__APPLE__) && defined(__x86_64__)
        CrashReport_WriteHex(writer, "IP", uctx->uc_mcontext->__ss.__rip);
        CrashReport_WriteHex(writer, "SP", uctx->uc_mcontext->__ss.__rsp);
        CrashReport_WriteHex(writer, "BP", uctx->uc_mcontext->__ss.__rbp);
#elif defined(__APPLE__) && defined(__aarch64__)
        CrashReport_WriteHex(writer, "IP", arm_thread_state64_get_pc(uctx->uc_mcontext->__ss));
        CrashReport_WriteHex(writer, "SP", arm_thread_state64_get_sp(uctx->uc_mcontext->__ss));
        CrashReport_WriteHex(writer, "BP", arm_thread_state64_get_fp(uctx->uc_mcontext->__ss));
#elif defined(__x86_64__)
        CrashReport_WriteHex(writer, "IP", uctx->uc_mcontext.gregs[REG_RIP]);
        CrashReport_WriteHex(writer, "SP", uctx->uc_mcontext.gregs[REG_RSP]);
        CrashReport_WriteHex(writer, "BP", uctx->uc_mcontext.gregs[REG_RBP]);
#elif defined(__aarch64__)
        CrashReport_WriteHex(writer, "IP", uctx->uc_mcontext.pc);
        CrashReport_WriteHex(writer, "SP", uctx->uc_mcontext.sp);
        CrashReport_WriteHex(writer, "BP", uctx->uc_mcontext.regs[29]);
#elif defined(__arm__)
        CrashReport_WriteHex(writer, "IP", uctx->uc_mcontext.arm_pc);
        CrashReport_WriteHex(writer, "SP", uctx->uc_mcontext.arm_sp);
        CrashReport_WriteHex(writer, "BP", uctx->uc_mcontext.arm_fp);
#endif
        CrashReport_CloseObject(writer);
    }

    WalkAndWriteFrames(writer, pThread, managedContext);

    CrashReport_CloseObject(writer);
}

// The callback invoked from PROCCreateCrashDumpIfEnabled via the PAL.
// Writes the crashing thread first, then enumerates all other managed
// threads in the ThreadStore.
static void GenerateCrashReportCallback(
    int signal,
    siginfo_t* siginfo,
    void* signalContext,
    void* managedContext,
    CrashReportWriter* writer,
    CrashReport_ManagedFrameCallback /*unused*/)
{
    WRAPPER_NO_CONTRACT;

    Thread* pCrashThread = GetThreadNULLOk();

    if (pCrashThread != nullptr)
    {
        // Write the crashing thread with its signal context and register state.
        WriteThreadEntry(writer, pCrashThread, true, signalContext,
                         static_cast<T_CONTEXT*>(managedContext));
    }
    else
    {
        // Crashing thread is not managed — emit a minimal entry.
        CrashReport_OpenObject(writer, NULL);
        CrashReport_WriteBool(writer, "crashed", 1);
#ifdef TARGET_APPLE
        uint64_t tid64;
        pthread_threadid_np(NULL, &tid64);
        CrashReport_WriteHex(writer, "native_thread_id", tid64);
#else
        CrashReport_WriteHex(writer, "native_thread_id", (uint64_t)gettid());
#endif
        CrashReport_WriteBool(writer, "is_managed", 0);
        if (signalContext != nullptr)
        {
            CrashReport_OpenObject(writer, "ctx");
            ucontext_t* uctx = (ucontext_t*)signalContext;
#if defined(__APPLE__) && defined(__x86_64__)
            CrashReport_WriteHex(writer, "IP", uctx->uc_mcontext->__ss.__rip);
            CrashReport_WriteHex(writer, "SP", uctx->uc_mcontext->__ss.__rsp);
            CrashReport_WriteHex(writer, "BP", uctx->uc_mcontext->__ss.__rbp);
#elif defined(__APPLE__) && defined(__aarch64__)
            CrashReport_WriteHex(writer, "IP", arm_thread_state64_get_pc(uctx->uc_mcontext->__ss));
            CrashReport_WriteHex(writer, "SP", arm_thread_state64_get_sp(uctx->uc_mcontext->__ss));
            CrashReport_WriteHex(writer, "BP", arm_thread_state64_get_fp(uctx->uc_mcontext->__ss));
#elif defined(__x86_64__)
            CrashReport_WriteHex(writer, "IP", uctx->uc_mcontext.gregs[REG_RIP]);
            CrashReport_WriteHex(writer, "SP", uctx->uc_mcontext.gregs[REG_RSP]);
            CrashReport_WriteHex(writer, "BP", uctx->uc_mcontext.gregs[REG_RBP]);
#elif defined(__aarch64__)
            CrashReport_WriteHex(writer, "IP", uctx->uc_mcontext.pc);
            CrashReport_WriteHex(writer, "SP", uctx->uc_mcontext.sp);
            CrashReport_WriteHex(writer, "BP", uctx->uc_mcontext.regs[29]);
#elif defined(__arm__)
            CrashReport_WriteHex(writer, "IP", uctx->uc_mcontext.arm_pc);
            CrashReport_WriteHex(writer, "SP", uctx->uc_mcontext.arm_sp);
            CrashReport_WriteHex(writer, "BP", uctx->uc_mcontext.arm_fp);
#endif
            CrashReport_CloseObject(writer);
        }
        CrashReport_OpenArray(writer, "stack_frames");
        CrashReport_CloseArray(writer);
        CrashReport_CloseObject(writer);
    }

    // Enumerate other managed threads. We use GetThreadListNoLock which
    // walks the ThreadStore's linked list without asserting the lock —
    // we cannot acquire locks from a signal handler. This is safe here
    // because CAS serialization in CrashReport_Generate guarantees only
    // one thread runs crash report generation.
    Thread* pOther = ThreadStore::GetThreadListNoLock(nullptr);
    while (pOther != nullptr)
    {
        if (pOther != pCrashThread)
        {
            EX_TRY
            {
                WriteThreadEntry(writer, pOther, false, nullptr, nullptr);
            }
            EX_CATCH
            {
            }
            EX_END_CATCH
        }

        pOther = ThreadStore::GetThreadListNoLock(pOther);
    }
}

void CrashReport_RegisterVMCallback()
{
    CrashReport_SetGenerateCallback(GenerateCrashReportCallback);
}

#endif // TARGET_LINUX || TARGET_ANDROID || TARGET_APPLE
