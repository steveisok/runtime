// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Runtime.InteropServices;
using System.Threading;

public class Program
{
    static ManualResetEventSlim s_ready = new ManualResetEventSlim(false);

    public static int Main(string[] args)
    {
        string scenario = Environment.GetEnvironmentVariable("CRASH_SCENARIO") ?? "abort";
        Console.WriteLine($"[CrashDumpTest] Scenario: {scenario}");

        // Spin up background threads doing realistic work
        var dbThread = new Thread(() => DatabaseWorker()) { Name = "DatabaseWorker", IsBackground = true };
        var httpThread = new Thread(() => HttpRequestProcessor()) { Name = "HttpProcessor", IsBackground = true };
        var timerThread = new Thread(() => TimerCallback()) { Name = "TimerCallback", IsBackground = true };
        dbThread.Start();
        httpThread.Start();
        timerThread.Start();

        // Wait for threads to be in their work methods
        Thread.Sleep(200);
        s_ready.Set();

        switch (scenario)
        {
            case "abort": Level1(() => abort()); break;
            case "sigsegv": Level1(() => memset(IntPtr.Zero, 0, 1)); break;
            case "sigsegv_raise": Level1(() => raise(11)); break;
            case "failfast": Level1(() => Environment.FailFast("Test FailFast")); break;
            case "unhandled": Level1(() => throw new InvalidOperationException("Test unhandled")); break;
            case "stackoverflow": LevelN(); break;
        }

        return 42;
    }

    static void Level1(Action a) { Level2(a); }
    static void Level2(Action a) { Level3(a); }
    static void Level3(Action a) { a(); }
    static void LevelN() { LevelN(); }

    // Simulated background workers — their stacks reveal what was happening at crash time
    static void DatabaseWorker()
    {
        s_ready.Wait();
        ExecuteQuery("SELECT * FROM Users WHERE active = 1");
    }

    static void ExecuteQuery(string sql)
    {
        PrepareStatement(sql);
    }

    static void PrepareStatement(string sql)
    {
        Thread.Sleep(Timeout.Infinite);
    }

    static void HttpRequestProcessor()
    {
        s_ready.Wait();
        ProcessRequest("/api/users", "GET");
    }

    static void ProcessRequest(string path, string method)
    {
        ValidateAuth(path);
    }

    static void ValidateAuth(string path)
    {
        Thread.Sleep(Timeout.Infinite);
    }

    static void TimerCallback()
    {
        s_ready.Wait();
        OnTimerElapsed();
    }

    static void OnTimerElapsed()
    {
        FlushMetrics();
    }

    static void FlushMetrics()
    {
        Thread.Sleep(Timeout.Infinite);
    }

    [DllImport("libc")]
    static extern void abort();

    [DllImport("libc")]
    static extern void memset(IntPtr dest, int c, int n);

    [DllImport("libc")]
    static extern int raise(int sig);
}
