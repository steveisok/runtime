// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;

public static class Program
{
    public static int Main(string[] args)
    {
        var ctx = AppContext.GetData("EMBEDDED_RUNTIME_ID");
        string id = "NO-ID";

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
            Console.WriteLine("EXCEPTION: " + e.ToString());
        }

        Console.WriteLine("GC COLLECT FROM " + id);
        GC.Collect();

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
