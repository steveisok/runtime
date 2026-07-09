// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Runtime.InteropServices;

public static class Program
{
    public static int Main(string[] args)
    {
        const string message = "Triggering fail-fast to validate in-proc crash report minidump generation.";
        Console.WriteLine(message);

        Environment.FailFast(message);
        return 1;
    }
}
