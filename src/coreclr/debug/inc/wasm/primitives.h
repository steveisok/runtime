// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//*****************************************************************************
// File: primitives.h
//

//
// Platform-specific debugger primitives
//
//*****************************************************************************

#ifndef PRIMITIVES_H_
#define PRIMITIVES_H_

inline CORDB_ADDRESS GetPatchEndAddr(CORDB_ADDRESS patchAddr)
{
    PORTABILITY_ASSERT("The function is not implemented on wasm");
    return patchAddr;
}

typedef const BYTE                  CORDB_ADDRESS_TYPE;
typedef DPTR(CORDB_ADDRESS_TYPE)    PTR_CORDB_ADDRESS_TYPE;

//This is an abstraction to keep x86/ia64 patch data separate
#define PRD_TYPE                               USHORT

#define MAX_INSTRUCTION_LENGTH 2 // update once we have codegen

#ifndef STACKWALK_CONTROLPC_ADJUST_OFFSET
#define STACKWALK_CONTROLPC_ADJUST_OFFSET 1
#endif

#define CORDbg_BREAK_INSTRUCTION_SIZE 1
#define CORDbg_BREAK_INSTRUCTION 0 // unreachable intruction

#define InitializePRDToBreakInst(_pPRD) *(_pPRD) = CORDbg_BREAK_INSTRUCTION
#define PRDIsBreakInst(_pPRD) (*(_pPRD) == CORDbg_BREAK_INSTRUCTION)

#define CORDbgGetInstructionEx(_buffer, _requestedAddr, _patchAddr, _dummy1, _dummy2) \
    CORDbgGetInstruction((CORDB_ADDRESS_TYPE *)((_buffer) + ((_patchAddr) - (_requestedAddr))));

#define CORDbgSetInstructionEx(_buffer, _requestedAddr, _patchAddr, _opcode, _dummy2) \
    CORDbgSetInstruction((CORDB_ADDRESS_TYPE *)((_buffer) + ((_patchAddr) - (_requestedAddr))), (_opcode));

#define CORDbgInsertBreakpointEx(_buffer, _requestedAddr, _patchAddr, _dummy1, _dummy2) \
    CORDbgInsertBreakpoint((CORDB_ADDRESS_TYPE *)((_buffer) + ((_patchAddr) - (_requestedAddr))));

inline bool PRDIsEmpty(PRD_TYPE p1)
{
    LIMITED_METHOD_CONTRACT;

    return p1 == 0;
}

inline void InitializePRD(PRD_TYPE *p1)
{
    *p1 = 0;
}

inline BOOL CompareControlRegisters(const DT_CONTEXT * pCtx1, const DT_CONTEXT * pCtx2)
{
    LIMITED_METHOD_CONTRACT;

    return pCtx1->InterpreterIP == pCtx2->InterpreterIP &&
        pCtx1->InterpreterSP == pCtx2->InterpreterSP &&
        pCtx1->InterpreterFP == pCtx2->InterpreterFP &&
        pCtx1->InterpreterWalkFramePointer == pCtx2->InterpreterWalkFramePointer;
}

inline PRD_TYPE CORDbgGetInstruction(UNALIGNED CORDB_ADDRESS_TYPE* address)
{
    LIMITED_METHOD_CONTRACT;

    return *address;
}

inline CorDebugRegister ConvertRegNumToCorDebugRegister(ICorDebugInfo::RegNum reg)
{
    LIMITED_METHOD_CONTRACT;

    if (reg == ICorDebugInfo::REGNUM_SP)
    {
        return REGISTER_STACK_POINTER;
    }

    if (reg == ICorDebugInfo::REGNUM_FP)
    {
        return REGISTER_FRAME_POINTER;
    }

    return REGISTER_INSTRUCTION_POINTER;
}

inline LPVOID CORDbgGetIP(DT_CONTEXT* context)
{
    LIMITED_METHOD_CONTRACT;

    return reinterpret_cast<LPVOID>(static_cast<uintptr_t>(context->InterpreterIP));
}

inline void CORDbgSetIP(DT_CONTEXT* context, LPVOID ip)
{
    LIMITED_METHOD_CONTRACT;

    context->InterpreterIP = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ip));
}

inline LPVOID CORDbgGetSP(const DT_CONTEXT* context)
{
    LIMITED_METHOD_CONTRACT;

    return reinterpret_cast<LPVOID>(static_cast<uintptr_t>(context->InterpreterSP));
}

inline void CORDbgSetSP(DT_CONTEXT* context, LPVOID sp)
{
    LIMITED_METHOD_CONTRACT;

    context->InterpreterSP = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sp));
}

inline LPVOID CORDbgGetFP(DT_CONTEXT* context)
{
    LIMITED_METHOD_CONTRACT;

    return reinterpret_cast<LPVOID>(static_cast<uintptr_t>(context->InterpreterFP));
}

inline void CORDbgSetFP(DT_CONTEXT* context, LPVOID fp)
{
    LIMITED_METHOD_CONTRACT;

    context->InterpreterFP = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fp));
}

inline void CORDbgInsertBreakpoint(UNALIGNED CORDB_ADDRESS_TYPE *address)
{
    LIMITED_METHOD_CONTRACT;

    *((BYTE*)address) = CORDbg_BREAK_INSTRUCTION;
}

inline void CORDbgSetInstruction(CORDB_ADDRESS_TYPE* address, PRD_TYPE instruction)
{
    LIMITED_METHOD_DAC_CONTRACT;

    *((BYTE*)address) = (BYTE)instruction;
}

inline void CORDbgAdjustPCForBreakInstruction(DT_CONTEXT* pContext)
{
    LIMITED_METHOD_CONTRACT;
}

inline bool AddressIsBreakpoint(CORDB_ADDRESS_TYPE *address)
{
    LIMITED_METHOD_CONTRACT;

    return *address == CORDbg_BREAK_INSTRUCTION;
}

inline void SetSSFlag(DT_CONTEXT *pContext)
{
    LIMITED_METHOD_CONTRACT;
}

inline void UnsetSSFlag(DT_CONTEXT *pContext)
{
    LIMITED_METHOD_CONTRACT;
}

#endif
