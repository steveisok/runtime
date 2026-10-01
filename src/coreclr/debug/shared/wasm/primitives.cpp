// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "stdafx.h"

void CORDbgCopyThreadContext(DT_CONTEXT* pDst, const DT_CONTEXT* pSrc)
{
    LIMITED_METHOD_DAC_CONTRACT;

    DWORD dstFlags = pDst->ContextFlags;
    DWORD srcFlags = pSrc->ContextFlags;

    if ((dstFlags & srcFlags & DT_CONTEXT_CONTROL) == DT_CONTEXT_CONTROL)
    {
        CopyContextChunk(
            &pDst->InterpreterWalkFramePointer,
            &pSrc->InterpreterWalkFramePointer,
            &pDst->InterpreterIP + 1,
            DT_CONTEXT_CONTROL);
    }
}
