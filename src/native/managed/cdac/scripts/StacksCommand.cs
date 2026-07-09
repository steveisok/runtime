// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.CommandLine;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using Microsoft.Diagnostics.DataContractReader;
using Microsoft.Diagnostics.DataContractReader.Contracts;
using Microsoft.Diagnostics.Runtime;

using Exception = System.Exception;

namespace Microsoft.DotNet.Diagnostics.CdacDumpInspect;

internal sealed class StacksCommand : Command
{
    private readonly Argument<string> _dumpPath = new("dump-path") { Description = "Path to a .NET crash dump" };

    public StacksCommand() : base("stacks", "Print managed stack traces for all threads")
    {
        Add(_dumpPath);
        SetAction(Run);
    }

    private int Run(ParseResult parse)
    {
        string dumpPath = parse.GetValue(_dumpPath)!;
        if (!File.Exists(dumpPath))
        {
            Console.Error.WriteLine($"Dump not found: {dumpPath}");
            return 1;
        }

        try
        {
            Execute(dumpPath);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex.ToString());
            return 1;
        }

        return 0;
    }

    private static void Execute(string dumpPath)
    {
        using DataTarget dt = DataTarget.LoadDump(dumpPath);
        var cdac = DumpHelpers.CreateCdacTarget(dt);

        Console.WriteLine($"Dump: {dumpPath}\n");

        IThread threadContract = cdac.Contracts.GetContract<IThread>();
        IStackWalk stackWalk = cdac.Contracts.GetContract<IStackWalk>();
        IRuntimeTypeSystem rts = cdac.Contracts.GetContract<IRuntimeTypeSystem>();
        ThreadStoreData storeData = threadContract.GetThreadStoreData();

        int idx = 0;
        HashSet<ulong> visited = [];
        TargetPointer threadAddr = storeData.FirstThread;
        while (threadAddr != TargetPointer.Null)
        {
            if (!visited.Add(threadAddr.Value))
            {
                Console.WriteLine($"Cycle detected in thread list at {threadAddr}");
                break;
            }

            ThreadData td;
            try
            {
                td = threadContract.GetThreadData(threadAddr);
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Thread {idx} ({threadAddr}): Error - {ex.Message}\n");
                break;
            }

            Console.WriteLine($"Thread {idx} (OS ID: 0x{td.OSId.Value:x}):");

            try
            {
                foreach (IStackDataFrameHandle frame in stackWalk.CreateStackWalk(td))
                {
                    try
                    {
                        TargetCodePointer ip = stackWalk.GetInstructionPointer(frame);
                        TargetPointer mdPtr = stackWalk.GetMethodDescPtr(frame);
                        string frameName;

                        if (mdPtr != TargetPointer.Null)
                        {
                            try
                            {
                                MethodDescHandle mdHandle = rts.GetMethodDescHandle(mdPtr);
                                frameName = ResolveMethodName(cdac, rts, mdHandle)
                                    ?? $"MD@0x{mdPtr.Value:x}";
                            }
                            catch (Exception)
                            {
                                frameName = $"MethodDesc@0x{mdPtr.Value:x}";
                            }
                        }
                        else
                        {
                            TargetPointer frameAddr = stackWalk.GetFrameAddress(frame);
                            if (frameAddr != TargetPointer.Null)
                            {
                                try { frameName = $"[{stackWalk.GetFrameName(frameAddr)}]"; }
                                catch { frameName = $"[InternalFrame@0x{frameAddr.Value:x}]"; }
                            }
                            else
                            {
                                frameName = "[Native Frame]";
                            }
                        }

                        Console.WriteLine($"  0x{ip.Value:x16} {frameName}");
                    }
                    catch (Exception ex)
                    {
                        Console.WriteLine($"  <frame error: {ex.Message}>");
                    }
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"  Stack walk failed: {ex.Message}");
            }

            Console.WriteLine();
            threadAddr = td.NextThread;
            idx++;
        }
    }

    // Resolves a MethodDesc to a fully-qualified "Namespace.Type.Method" name via ECMA metadata.
    // Returns null if the name cannot be resolved (e.g. metadata not captured in the dump).
    private static string? ResolveMethodName(ContractDescriptorTarget cdac, IRuntimeTypeSystem rts, MethodDescHandle mdHandle)
    {
        if (rts.IsNoMetadataMethod(mdHandle, out string dynamicName))
            return dynamicName;

        uint token = rts.GetMethodToken(mdHandle);
        TargetPointer mt = rts.GetMethodTable(mdHandle);
        TargetPointer modulePtr = rts.GetModule(rts.GetTypeHandle(mt));

        ILoader loader = cdac.Contracts.GetContract<ILoader>();
        Microsoft.Diagnostics.DataContractReader.Contracts.ModuleHandle moduleHandle = loader.GetModuleHandleFromModulePtr(modulePtr);

        IEcmaMetadata ecmaMetadata = cdac.Contracts.GetContract<IEcmaMetadata>();
        MetadataReader? reader = ecmaMetadata.GetMetadata(moduleHandle);
        if (reader is null)
            return null;

        var methodDef = MetadataTokens.MethodDefinitionHandle((int)(token & 0x00FFFFFF));
        MethodDefinition method = reader.GetMethodDefinition(methodDef);
        string methodName = reader.GetString(method.Name);

        TypeDefinition declaringType = reader.GetTypeDefinition(method.GetDeclaringType());
        string typeName = reader.GetString(declaringType.Name);
        string ns = reader.GetString(declaringType.Namespace);
        string fullTypeName = string.IsNullOrEmpty(ns) ? typeName : $"{ns}.{typeName}";

        return $"{fullTypeName}.{methodName}";
    }
}
