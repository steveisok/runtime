// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// VM-side crash report helper for Linux/Android.
//
// Walks the crashing thread's managed stack and exception state, feeding
// frame data back into the PAL crash report writer. This is best-effort
// code that uses live VM inspection — a secondary crash is possible and
// is guarded with sigsetjmp at the call site.

#pragma once

#if defined(TARGET_LINUX) || defined(TARGET_ANDROID) || defined(TARGET_APPLE)
// Register the VM crash report callback with the PAL.
// Called during EE startup on Linux/Android/Apple.
void CrashReport_RegisterVMCallback();
#endif
