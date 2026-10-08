// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "stdafx.h"
#include "../../controller.h"
#include "../../debugger.h"
#include "dbgtransportsession.h"
#include "interpexec.h"
#include <emscripten.h>

EXTERN_C const IID IID_IDebugChannel;

EM_JS(int, coreclr_wasm_debugger_transport_available, (), {
    return globalThis.__coreclrWasmDebuggerTransport ? 1 : 0;
});

EM_JS(int, coreclr_wasm_debugger_incoming_byte_count, (), {
    return globalThis.__coreclrWasmDebuggerTransport?.incomingByteCount() ?? -1;
});

EM_JS(int, coreclr_wasm_debugger_read_incoming, (BYTE* buffer, int size), {
    const transport = globalThis.__coreclrWasmDebuggerTransport;
    if (!transport || size < 0 || transport.incomingByteCount() < size) {
        return 0;
    }

    const decoded = atob(transport.dequeueIncoming(size));
    if (decoded.length !== size) {
        return 0;
    }

    for (let i = 0; i < size; i++) {
        HEAPU8[buffer + i] = decoded.charCodeAt(i);
    }
    return 1;
});

EM_JS(int, coreclr_wasm_debugger_write_outgoing, (const BYTE* data, int size), {
    const transport = globalThis.__coreclrWasmDebuggerTransport;
    if (!transport || size < 0) {
        return 0;
    }

    let binary = "";
    const chunkSize = 0x8000;
    for (let offset = 0; offset < size; offset += chunkSize) {
        binary += String.fromCharCode(...HEAPU8.subarray(data + offset, data + Math.min(offset + chunkSize, size)));
    }
    return transport.enqueueOutgoing(btoa(binary)) ? 1 : 0;
});

class WasmDebuggerChannel final : public IDebugChannel
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (object == nullptr)
        {
            return E_POINTER;
        }

        if (riid == IID_IUnknown || riid == IID_IDebugChannel)
        {
            *object = static_cast<IDebugChannel*>(this);
            AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG refCount = InterlockedDecrement(&m_refCount);
        if (refCount == 0)
        {
            delete this;
        }
        return static_cast<ULONG>(refCount);
    }

    HRESULT STDMETHODCALLTYPE WaitForConnection(
        DWORD timeoutMilliseconds,
        BOOL* isConnected) override
    {
        if (isConnected == nullptr)
        {
            return E_POINTER;
        }

        *isConnected = coreclr_wasm_debugger_transport_available() ? TRUE : FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CloseConnection() override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Read(BYTE* buffer, DWORD size) override
    {
        if (size != 0 && buffer == nullptr)
        {
            return E_POINTER;
        }
        if (size > INT_MAX)
        {
            return E_INVALIDARG;
        }

        return coreclr_wasm_debugger_read_incoming(buffer, static_cast<int>(size)) ? S_OK : E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE Write(BYTE* data, DWORD size) override
    {
        if (size != 0 && data == nullptr)
        {
            return E_POINTER;
        }
        if (size > INT_MAX)
        {
            return E_INVALIDARG;
        }

        return coreclr_wasm_debugger_write_outgoing(data, static_cast<int>(size)) ? S_OK : E_FAIL;
    }

private:
    LONG m_refCount = 1;
};

static DebuggerIPCEvent s_singleThreadedDebuggerIPCEvent;
static DebuggerController* s_singleThreadedDebuggerEventController;
static CONTEXT* s_singleThreadedDebuggerContext;

bool IsSingleThreadedDebuggerAttached()
{
    return g_pDbgTransport != nullptr && g_pDbgTransport->IsOpen();
}

HRESULT InitializeSingleThreadedDebuggerTransport(DebuggerRCThread* rcThread)
{
    WasmDebuggerChannel* channel = new (nothrow) WasmDebuggerChannel();
    if (channel == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    g_pDbgTransport = new (nothrow) DbgTransportSession();
    if (g_pDbgTransport == nullptr)
    {
        channel->Release();
        return E_OUTOFMEMORY;
    }

    HRESULT hr = g_pDbgTransport->InitCooperative(rcThread->GetDCB(), channel);
    channel->Release();
    return hr;
}

DebuggerIPCEvent* GetSingleThreadedDebuggerIPCEventBuffer()
{
    return &s_singleThreadedDebuggerIPCEvent;
}

HRESULT SendSingleThreadedDebuggerIPCEvent(DebuggerIPCEvent* event)
{
    _ASSERTE(event == &s_singleThreadedDebuggerIPCEvent);
    if (g_pDbgTransport == nullptr)
    {
        return E_UNEXPECTED;
    }

    if (event->type == DB_IPCE_SYNC_COMPLETE)
    {
        return g_pDbgTransport->SendEvent(event);
    }

    return g_pDbgTransport->SendDebugEvent(event);
}

void PauseSingleThreadedDebugger()
{
    emscripten_debugger();
}

extern "C" EMSCRIPTEN_KEEPALIVE int CoreClrWasmDebuggerTransportPump()
{
    if (g_pDbgTransport == nullptr || g_pRCThread == nullptr || g_pDebugger == nullptr)
    {
        return E_UNEXPECTED;
    }

    BYTE buffer[4096];
    int available = coreclr_wasm_debugger_incoming_byte_count();
    while (available > 0)
    {
        int readSize = min(available, static_cast<int>(sizeof(buffer)));
        if (!coreclr_wasm_debugger_read_incoming(buffer, readSize))
        {
            return E_FAIL;
        }

        HRESULT hr = g_pDbgTransport->ProcessIncomingBytes(buffer, static_cast<DWORD>(readSize));
        if (FAILED(hr))
        {
            return hr;
        }
        available = coreclr_wasm_debugger_incoming_byte_count();
    }

    if (available < 0)
    {
        return E_UNEXPECTED;
    }

    while (g_pDbgTransport->HasPendingEvent())
    {
        DebuggerIPCEvent* event = g_pRCThread->GetIPCEventReceiveBuffer();
        g_pDbgTransport->GetNextEvent(event, CorDBIPC_BUFFER_SIZE);
        Debugger::DebuggerLockHolder debuggerLock(g_pDebugger, FALSE);
        DebuggerIPCEventType eventType =
            static_cast<DebuggerIPCEventType>(event->type & DB_IPCE_TYPE_MASK);
        if (!g_pDebugger->IsStopped() && eventType == DB_IPCE_CONTINUE)
        {
            g_pDebugger->ResumeThreads(event->vmAppDomain.GetRawPtr());
            return S_FALSE;
        }
        if (g_pDebugger->IsStopped() &&
            eventType != DB_IPCE_ASYNC_BREAK &&
            eventType != DB_IPCE_ATTACHING &&
            !g_pDebugger->ThreadHoldsLock())
        {
            debuggerLock.Acquire();
        }
        bool wasContinue;
        Thread* currentThread = GetThreadNULLOk();
        if (currentThread != nullptr)
        {
            // Browser debugger evaluation re-enters the runtime on the stopped
            // managed thread, but HandleIPCEvent requires cooperative mode.
            GCX_COOP_THREAD_EXISTS(currentThread);
            wasContinue = HandleIPCEventWrapper(g_pDebugger, event);
        }
        else
        {
            wasContinue = HandleIPCEventWrapper(g_pDebugger, event);
        }
        if (wasContinue)
        {
            return S_FALSE;
        }
    }

    return S_OK;
}

void PrepareSingleThreadedDebuggerPatch(Thread* thread, CONTEXT* context, CORDB_ADDRESS_TYPE* address)
{
    s_singleThreadedDebuggerEventController = nullptr;
    s_singleThreadedDebuggerContext = context;

    InterpMethodContextFrame* frame =
        reinterpret_cast<InterpMethodContextFrame*>(GetSP(context));
    if (thread == nullptr ||
        frame == nullptr ||
        frame->startIp == nullptr ||
        frame->startIp->Method == nullptr ||
        frame->startIp->Method->methodHnd == nullptr)
    {
        return;
    }

    DebuggerPatchTable* patches = DebuggerController::GetPatchTable();
    if (patches == nullptr)
    {
        return;
    }

    DebuggerControllerPatch* patch = patches->GetPatch(
        reinterpret_cast<PTR_CORDB_ADDRESS_TYPE>(address));
    if (patch != nullptr)
    {
        s_singleThreadedDebuggerEventController = patch->controller;
    }
}

bool ShouldTriggerSingleThreadedDebuggerBreakpoint(DebuggerBreakpoint* breakpoint)
{
    return breakpoint == s_singleThreadedDebuggerEventController;
}

bool SendSingleThreadedDebuggerBreakpointEvent(Thread* thread, DebuggerBreakpoint* breakpoint)
{
    g_pDebugger->SendBreakpoint(thread, s_singleThreadedDebuggerContext, breakpoint);
    s_singleThreadedDebuggerEventController = nullptr;
    s_singleThreadedDebuggerContext = nullptr;
    return true;
}

bool SendSingleThreadedDebuggerStepperEvent(Thread* thread, DebuggerStepper* stepper)
{
    g_pDebugger->SendStep(
        thread,
        s_singleThreadedDebuggerContext,
        stepper,
        stepper->GetStepReason());
    stepper->Delete();
    s_singleThreadedDebuggerEventController = nullptr;
    s_singleThreadedDebuggerContext = nullptr;
    return true;
}
