// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//*****************************************************************************
// thread.cpp
//
// Implementation of the Thread stack-region walk declared in thread.h.
//*****************************************************************************

#include "thread.h"
#include "runtimetypes.h"

#include <set>

namespace cdac
{
namespace contracts
{
    namespace
    {
        // Global pointing at the ThreadStore*: &ThreadStore::s_pThreadStore.
        const char* const GlobalThreadStore = "ThreadStore";

        const int MaxThreads = 100000; // guard against corrupt thread lists

        // The reader's Data.Thread eagerly reads the Thread's RuntimeThreadLocals (which embeds
        // EEAllocContext -> GCAllocContext). Emit a blob at that pointer covering the embedded
        // alloc-context chain (the descriptor field-span logic under-covers embedded structs).
        const uint32_t RuntimeThreadLocalsEmit = 512;
        const uint32_t ThreadEmit = 4096;
        const uint32_t ExceptionInfoEmit = 512;

        const char* const ThreadObjectHandleFields[] =
        {
            "ExposedObject",
            "LastThrownObject",
            "CurrentCustomDebuggerNotification",
        };

        bool EmitThreadPointerCell(const Target& target, RegionCallback sink, void* sinkContext, uint64_t threadAddr, const char* fieldName, const char* kind)
        {
            uint64_t cellAddr = 0;
            uint64_t value = 0;
            if (target.TryGetFieldAddress(threadAddr, "Thread", fieldName, cellAddr) &&
                cellAddr != 0 &&
                target.TryReadPointer(cellAddr, value))
            {
                sink(sinkContext, kind, cellAddr, target.PointerSize());
                return true;
            }
            return false;
        }
    }

    int EnumerateThreadRegions(const Target& target, RegionCallback sink, void* sinkContext)
    {
        // ThreadStore global is a pointer-to-pointer: deref once to get the ThreadStore.
        uint64_t threadStoreAddr = 0;
        if (!target.TryReadGlobalPointer(GlobalThreadStore, threadStoreAddr) || threadStoreAddr == 0)
        {
            return -1;
        }

        data::ThreadStore threadStore;
        if (!target.TryRead(threadStoreAddr, threadStore))
        {
            return -1;
        }

        std::set<uint64_t> visited;
        int count = 0;
        uint64_t threadAddr = threadStore.FirstThreadLink; // head Thread*

        for (int i = 0; threadAddr != 0 && i < MaxThreads; i++)
        {
            if (!visited.insert(threadAddr).second)
            {
                break; // cycle
            }

            data::Thread thread;
            if (!target.TryRead(threadAddr, thread))
            {
                break;
            }
            sink(sinkContext, "thread", threadAddr, ThreadEmit);

            // Data.Thread exposes several raw pointer cells and pseudo-handles. The managed
            // reader may dereference values that are not modeled as normal descriptor fields
            // (for example FieldAddress-backed exception tracker state). Capture readable
            // pointer-sized cells referenced from the Thread page so GetThreadData can make
            // best-effort progress without pulling in the whole handle table.
            uint8_t threadBytes[ThreadEmit];
            if (target.ReadBuffer(threadAddr, threadBytes, ThreadEmit))
            {
                const uint64_t ptrSize = target.PointerSize();
                for (uint32_t off = 0; off + ptrSize <= ThreadEmit; off += (uint32_t)ptrSize)
                {
                    uint64_t value = 0;
                    memcpy(&value, threadBytes + off, sizeof(uint64_t));
                    if (value != 0)
                    {
                        uint64_t ignored = 0;
                        if (target.TryReadPointer(value, ignored))
                        {
                            sink(sinkContext, "thread-pointer-cell", value, ptrSize);
                        }
                    }
                }
            }

            // The reader's Data.Thread ctor dereferences RuntimeThreadLocals; emit it so
            // GetThreadData succeeds from the dump without the legacy DAC.
            uint64_t rtlPtr = 0;
            if (target.TryReadFieldPointer(threadAddr, "Thread", "RuntimeThreadLocals", rtlPtr) && rtlPtr != 0)
            {
                sink(sinkContext, "thread-locals", rtlPtr, RuntimeThreadLocalsEmit);
            }

            uint64_t exceptionTrackerAddr = 0;
            uint64_t exceptionInfoAddr = 0;
            if (EmitThreadPointerCell(target, sink, sinkContext, threadAddr, "ExceptionTracker", "thread-exception-tracker") &&
                target.TryGetFieldAddress(threadAddr, "Thread", "ExceptionTracker", exceptionTrackerAddr) &&
                target.TryReadPointer(exceptionTrackerAddr, exceptionInfoAddr))
            {
                if (exceptionInfoAddr != 0)
                {
                    sink(sinkContext, "thread-exception-tracker-target", exceptionInfoAddr, target.PointerSize());
                    sink(sinkContext, "thread-exception-info", exceptionInfoAddr, ExceptionInfoEmit);
                }
            }

            for (const char* fieldName : ThreadObjectHandleFields)
            {
                uint64_t handleCellAddr = 0;
                uint64_t handleAddr = 0;
                if (target.TryGetFieldAddress(threadAddr, "Thread", fieldName, handleCellAddr) &&
                    handleCellAddr != 0 &&
                    target.TryReadPointer(handleCellAddr, handleAddr))
                {
                    sink(sinkContext, "thread-handle-cell", handleCellAddr, target.PointerSize());
                    if (handleAddr != 0)
                    {
                        sink(sinkContext, "thread-handle", handleAddr, target.PointerSize());
                    }
                }
            }

            // Stack grows down: CachedStackLimit is the low address, CachedStackBase
            // the high address. Report the committed stack range.
            if (thread.CachedStackBase > thread.CachedStackLimit && thread.CachedStackLimit != 0)
            {
                sink(sinkContext, "thread-stack", thread.CachedStackLimit,
                     thread.CachedStackBase - thread.CachedStackLimit);
                count++;
            }

            threadAddr = thread.LinkNext;
        }

        return count;
    }
}
} // namespace contracts
