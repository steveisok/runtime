// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Runtime.CompilerServices;
using Xunit;

namespace Runtime_130999;

public static class Runtime_130999
{
    private static int Expected(float[] input)
    {
        int acc = 0;

        foreach (float f in input)
        {
            if (f < 0)
            {
                acc++;
            }

            acc += BitConverter.SingleToInt32Bits(f);
        }

        return acc;
    }

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
            // X64-FULL-LINE-NEXT: add {{[a-z0-9]+}}, [[REG]]
            // X64-NOT: movss dword ptr [rsp
            acc += BitConverter.SingleToInt32Bits(f);
        }

        // X64-FULL-LINE: ret
        return acc;
    }

    [Fact]
    public static void Test()
    {
        float[] empty = Array.Empty<float>();
        float[] single = new[] { -2.5f };
        float[] mixed = new[] { 1.25f, -2.5f, 3.75f, 4.125f };
        float[] edge = new[] { float.NaN, float.PositiveInfinity, float.NegativeInfinity, float.Epsilon, -float.Epsilon };

        Assert.Equal(Expected(empty), SumBits(empty));
        Assert.Equal(Expected(single), SumBits(single));
        Assert.Equal(Expected(mixed), SumBits(mixed));
        Assert.Equal(Expected(edge), SumBits(edge));
    }
}
