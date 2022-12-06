// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Linq;
using Microsoft.Build.Framework;
using Microsoft.Build.Utilities;

public class AppleLibraryBuilderTask : AppleBuilderTask
{
    /// <summary>
    /// The name of the library being generated
    /// </summary>
    [Required]
    public string Name { get; set; } = ""!;

    public override bool Execute()
    {
        return true;
    }
}
