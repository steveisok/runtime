// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

namespace System
{
	static class LocalAppContextSwitches
    {
        public static readonly bool IgnoreEmptyKeySequences = false;
        public static readonly bool DontThrowOnInvalidSurrogatePairs = false;
        public static readonly bool IgnoreKindInUtcTimeSerialization = false;
        public static readonly bool EnableTimeSpanSerialization = false;
        public static readonly bool LimitXPathComplexity = false;
        public static readonly bool AllowDefaultResolver = false;

        public static readonly string Test = System.Xml.Serialization.CodeIdentifier.GetCSharpName("test");
	}
}