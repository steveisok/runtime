// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

class SuppliedEventChannel final : public IEventChannel
{
public:
    SuppliedEventChannel(DebuggerIPCControlBlock * pDCB, DbgTransportSession * pTransport)
        : m_pDCB(pDCB),
          m_pTransport(pTransport)
    {
    }

    HRESULT Init(HANDLE) override
    {
        return S_OK;
    }

    void Detach() override
    {
    }

    void Delete() override
    {
        delete m_pDCB;
        m_pTransport->Shutdown();
        delete this;
    }

    HRESULT UpdateLeftSideDCBField(void *, SIZE_T) override
    {
        return m_pTransport->SetDCB(m_pDCB);
    }

    HRESULT UpdateRightSideDCB() override
    {
        return m_pTransport->GetDCB(m_pDCB);
    }

    DebuggerIPCControlBlock * GetDCB() override
    {
        return m_pDCB;
    }

    BOOL NeedToWaitForAck(DebuggerIPCEvent * pEvent) override
    {
        return !pEvent->asyncSend && pEvent->replyRequired;
    }

    WaitEvent * GetRightSideEventAckHandle() override
    {
        return m_pTransport->GetIPCEventReadyEvent();
    }

    WaitEvent * GetLeftSideEventAvailableHandle() override
    {
        return m_pTransport->GetDebugEventReadyEvent();
    }

    bool HasPendingEventFromLeftSide() override
    {
        return m_pTransport->HasPendingEvent(IPCET_DebugEvent);
    }

    void ClearEventForLeftSide() override
    {
    }

    HRESULT SendEventToLeftSide(DebuggerIPCEvent * pEvent, SIZE_T eventSize) override
    {
        _ASSERTE(eventSize <= CorDBIPC_BUFFER_SIZE);
        return m_pTransport->SendEvent(pEvent);
    }

    HRESULT GetReplyFromLeftSide(
        DebuggerIPCEvent * pReplyEvent,
        SIZE_T eventSize,
        DWORD timeout) override
    {
        while (true)
        {
            m_pTransport->GetNextEvent(
                pReplyEvent,
                static_cast<DWORD>(eventSize),
                IPCET_OldStyle);
            if ((pReplyEvent->type & DB_IPCE_TYPE_MASK) != DB_IPCE_SYNC_COMPLETE)
            {
                return S_OK;
            }

            int32_t waitResult = WaitHandle::Wait(
                *m_pTransport->GetIPCEventReadyEvent(),
                timeout);
            if (waitResult == WaitHandle::Timeout)
            {
                return ErrWrapper(CORDBG_E_TIMEOUT);
            }
            if (waitResult != 0)
            {
                return E_FAIL;
            }
        }
    }

    HRESULT SaveEventFromLeftSide(DebuggerIPCEvent *) override
    {
        return E_NOTIMPL;
    }

    HRESULT GetEventFromLeftSide(DebuggerIPCEvent * pLocalManagedEvent) override
    {
        m_pTransport->GetNextEvent(
            pLocalManagedEvent,
            CorDBIPC_BUFFER_SIZE,
            IPCET_DebugEvent);
        return S_OK;
    }

private:
    DebuggerIPCControlBlock * m_pDCB;
    DbgTransportSession * m_pTransport;
};

HRESULT NewEventChannelForThisPlatform(IDebugChannel * pChannel,
                                       IEventChannel ** ppEventChannel)
{
    if (pChannel == NULL || ppEventChannel == NULL)
    {
        return E_INVALIDARG;
    }
    *ppEventChannel = NULL;

    WaitEvent processExited(false);
    if (!processExited.IsValid())
    {
        return E_OUTOFMEMORY;
    }

    DbgTransportSession * pTransport = new (nothrow) DbgTransportSession();
    if (pTransport == NULL)
    {
        return E_OUTOFMEMORY;
    }

    ProcessDescriptor processDescriptor = ProcessDescriptor::CreateUninitialized();
    HRESULT hr = pTransport->Init(processDescriptor, processExited, pChannel);
    if (FAILED(hr))
    {
        delete pTransport;
        return hr;
    }

    if (!pTransport->WaitForSessionToOpen(300000))
    {
        pTransport->Shutdown();
        return CORDBG_E_TIMEOUT;
    }

    DebuggerIPCControlBlock * pDCB = new (nothrow) DebuggerIPCControlBlock;
    if (pDCB == NULL)
    {
        pTransport->Shutdown();
        return E_OUTOFMEMORY;
    }

    SuppliedEventChannel * pEventChannel = new (nothrow) SuppliedEventChannel(pDCB, pTransport);
    if (pEventChannel == NULL)
    {
        delete pDCB;
        pTransport->Shutdown();
        return E_OUTOFMEMORY;
    }

    *ppEventChannel = pEventChannel;
    return S_OK;
}
