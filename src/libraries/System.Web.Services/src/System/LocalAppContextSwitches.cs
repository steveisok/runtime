// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

namespace System
{
    internal static class LocalAppContextSwitches
    {
        public const bool IgnoreEmptyKeySequences = false;
        public const bool DontThrowOnInvalidSurrogatePairs = false;
        public const bool IgnoreKindInUtcTimeSerialization = false;
        public const bool EnableTimeSpanSerialization = false;
        public const bool LimitXPathComplexity = false;
        public const bool AllowDefaultResolver = false;
        public static string Test = System.Xml.Serialization.CodeIdentifier.GetCSharpName("test");
    }
}