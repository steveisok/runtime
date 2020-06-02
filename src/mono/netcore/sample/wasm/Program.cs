// -*- indent-tabs-mode: nil -*-
// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
using System;

public class Test
{
    public static void Main (String[] args) {
        Console.WriteLine ("Hello, World!");
    }
}

public class Sample {
    public static void Go() {
        var g = Guid.NewGuid();
        Console.WriteLine("ID: " + g.ToString());
    }
}
