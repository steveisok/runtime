// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "stdafx.h"
#include "../../shared/wasm/primitives.cpp"

void CopyREGDISPLAY(REGDISPLAY* pDst, REGDISPLAY* pSrc)
{
    memcpy(pDst, pSrc, sizeof(REGDISPLAY));

    pDst->pContext = pSrc->pContext;

    if (pSrc->pCurrentContextPointers == &pSrc->ctxPtrsOne)
    {
        pDst->pCurrentContextPointers = &pDst->ctxPtrsOne;
        pDst->pCallerContextPointers = &pDst->ctxPtrsTwo;
    }
    else
    {
        pDst->pCurrentContextPointers = &pDst->ctxPtrsTwo;
        pDst->pCallerContextPointers = &pDst->ctxPtrsOne;
    }

    if (pSrc->pCurrentContext == &pSrc->ctxOne)
    {
        pDst->pCurrentContext = &pDst->ctxOne;
        pDst->pCallerContext = &pDst->ctxTwo;
    }
    else
    {
        pDst->pCurrentContext = &pDst->ctxTwo;
        pDst->pCallerContext = &pDst->ctxOne;
    }
}
