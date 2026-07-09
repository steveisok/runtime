// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//*****************************************************************************
// loader.h
//
// Loader contract: walks the AppDomain's assembly list and reports each module's
// loaded PE image (code + metadata) -- the file-backed memory that a heap dump
// needs but that a private-read-write memory capture does not include. Modeled on
// the managed cDAC Loader contract (Contracts/Loader_1.cs).
//*****************************************************************************

#ifndef CDACLITE_CONTRACTS_LOADER_H
#define CDACLITE_CONTRACTS_LOADER_H

#include <stdint.h>

#include "contracts.h"
#include "target.h"

namespace cdac
{
namespace contracts
{
    // Invoked with each Module* discovered by ForEachModule.
    typedef void (*ModuleCallback)(void* context, uint64_t moduleAddr);

    // Enumerates every Module in the (default) AppDomain's assembly list, invoking
    // 'callback' with each Module address. Returns the module count, or -1 if the
    // AppDomain could not be located.
    int ForEachModule(const Target& target, ModuleCallback callback, void* context);

    // Emits the memory the managed reader touches for a single Module: the Module struct
    // itself, its metadata-locator chain (PEAssembly -> PEImage -> PEImageLayout + PE headers),
    // any in-memory symbol stream, and its ReadyToRun locator structs. Returns false (a no-op)
    // if the Module cannot be read. Used both by the whole-AppDomain module enumeration and by
    // the stack scan, which reaches an R2R frame's module directly via its RangeSection.
    bool EmitModuleImageRegions(const Target& target, uint64_t moduleAddr);

    // Walks AppDomain -> AssemblyList -> Assembly -> Module -> PEImageLayout and
    // reports each module's loaded image range ([Base, Base+Size)) with kind
    // "module-image". Returns the number of regions reported, or -1 if the
    // AppDomain could not be located.
    int EnumerateModuleRegions(const Target& target, RegionCallback sink, void* sinkContext);
}
} // namespace contracts

#endif // CDACLITE_CONTRACTS_LOADER_H
