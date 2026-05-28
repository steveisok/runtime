// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Runtime.CompilerServices;
using Xunit;

namespace Runtime_130999;

public static class Runtime_130999
{
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int SumBits(Span<float> input)
    {
        int acc = 0;

        foreach (float f in input)
        {
            if (f < 0)
            {
                acc++;
            }

            // X64-FULL-LINE: vmovd [[REG:[a-z0-9]+]], xmm{{[0-9]+}}
            // X64-NOT: movss dword ptr [rsp
            acc += BitConverter.SingleToInt32Bits(f);
        }

        return acc;
    }

    [Fact]
    public static void TestEntryPoint()
    {
        float[] input = new[] { 1.25f, -2.5f, 3.75f, 4.125f };
        Assert.Equal(11796481, SumBits(input));
    }
}
