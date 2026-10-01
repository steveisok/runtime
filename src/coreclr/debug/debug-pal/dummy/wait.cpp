// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include <errno.h>
#include <new>
#include <stdint.h>

#include "debugwait.h"

namespace
{
    constexpr uint32_t MaxWaitHandles = 64;

    struct Waitable
    {
        int32_t RefCount;
        bool ResetOnWait;
        bool Signaled;
    };

    Waitable* CreateWaitable(bool initialState, bool resetOnWait)
    {
        return new (std::nothrow) Waitable { 1, resetOnWait, initialState };
    }

    Waitable* DuplicateWaitable(void* handle)
    {
        if (handle == nullptr)
        {
            errno = EINVAL;
            return nullptr;
        }

        Waitable* waitable = static_cast<Waitable*>(handle);
        waitable->RefCount++;
        return waitable;
    }

    void ReleaseWaitable(void* handle)
    {
        Waitable* waitable = static_cast<Waitable*>(handle);
        if (--waitable->RefCount == 0)
        {
            delete waitable;
        }
    }
}

WaitHandle::WaitHandle(void* handle)
    : m_handle(handle)
{
}

WaitHandle::WaitHandle(const WaitHandle& handle)
    : WaitHandle(DuplicateWaitable(handle.m_handle))
{
}

WaitHandle::~WaitHandle()
{
    if (m_handle != nullptr)
    {
        ReleaseWaitable(m_handle);
    }
}

WaitEvent::WaitEvent(bool initialState)
    : WaitHandle(CreateWaitable(initialState, true))
{
}

bool WaitEvent::Set()
{
    if (!IsValid())
    {
        errno = EINVAL;
        return false;
    }

    static_cast<Waitable*>(GetWaitable())->Signaled = true;
    return true;
}

bool WaitEvent::Reset()
{
    if (!IsValid())
    {
        errno = EINVAL;
        return false;
    }

    static_cast<Waitable*>(GetWaitable())->Signaled = false;
    return true;
}

WaitLatch::WaitLatch()
    : WaitHandle(CreateWaitable(false, false))
{
}

bool WaitLatch::Set()
{
    if (!IsValid())
    {
        errno = EINVAL;
        return false;
    }

    static_cast<Waitable*>(GetWaitable())->Signaled = true;
    return true;
}

int32_t WaitHandle::Wait(
    const WaitHandle* const* handles,
    uint32_t count,
    uint32_t timeout)
{
    if (handles == nullptr || count == 0 || count > MaxWaitHandles)
    {
        errno = EINVAL;
        return Failed;
    }

    for (uint32_t index = 0; index < count; index++)
    {
        if (handles[index] == nullptr || !handles[index]->IsValid())
        {
            errno = EINVAL;
            return Failed;
        }

        Waitable* waitable = static_cast<Waitable*>(handles[index]->m_handle);
        if (waitable->Signaled)
        {
            if (waitable->ResetOnWait)
            {
                waitable->Signaled = false;
            }
            return static_cast<int32_t>(index);
        }
    }

    return Timeout;
}
