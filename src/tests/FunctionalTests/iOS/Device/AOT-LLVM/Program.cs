// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Threading;
using System.Threading.Tasks;
using System.Runtime.InteropServices;

public static class Program
{
    [DllImport("__Internal")]
    public static extern void mono_ios_set_summary (string value);

    public static int Main(string[] args)
    {
        mono_ios_set_summary($"Starting functional test");
        var ctx = AppContext.GetData("EMBEDDED_RUNTIME_ID");
        string id = "NO-ID";
        string ex = null;

        if (ctx != null)
        {
            id = ctx.ToString();
        }

        Console.WriteLine("HELLO FROM " + id);

        try
        {
            ThrowMe(id);
        }
        catch(Exception e)
        {
            Console.WriteLine($"EXCEPTION in {id}: " + e.ToString());
        }

        Console.WriteLine("GC COLLECT FROM " + id);
        GC.Collect();

        if (ex == null && id != "DOTNET-RUNTIME-ONE")
            ex = ex.ToString();

        //await Task.Delay(1000);
        return 42;
    }

    public static void ThrowMe(string id)
    {
        if (id != "NO-ID")
        {
            throw new Exception("THROWN FROM " + id);
        }
    }
}
