using System;
using System.Threading;

Console.WriteLine($"Target app running. PID: {Environment.ProcessId}");

using var cts = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) => { e.Cancel = true; cts.Cancel(); };

while (!cts.Token.IsCancellationRequested)
{
    Thread.Sleep(1000);
}

Console.WriteLine("Target app exiting.");
