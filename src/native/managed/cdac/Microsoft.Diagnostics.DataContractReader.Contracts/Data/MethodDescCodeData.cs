// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

namespace Microsoft.Diagnostics.DataContractReader.Data;

[CdacType(nameof(DataType.MethodDescCodeData))]
internal sealed partial class MethodDescCodeData : IData<MethodDescCodeData>
{
    [Field] public TargetCodePointer TemporaryEntryPoint { get; }
    // Descriptor-optional: only emitted when the runtime is built with FEATURE_CODE_VERSIONING
    // (tiered compilation). Absent on runtimes without it (e.g. mobile/iOS).
    [Field] public TargetPointer? VersioningState { get; }
    [Field] public uint? OptimizationTier { get; }
}
