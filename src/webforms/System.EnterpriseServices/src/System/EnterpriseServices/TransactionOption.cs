// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

namespace System.EnterpriseServices
{
    [Serializable]
    public enum TransactionOption
    {
        Disabled = 0,
        NotSupported = 1,
        Supported = 2,
        Required = 3,
        RequiresNew = 4,
    }
}
