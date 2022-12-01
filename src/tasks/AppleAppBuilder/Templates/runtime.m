// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#import <Foundation/Foundation.h>
#include <mono/utils/mono-publib.h>
#include <mono/utils/mono-logger.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/class.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/mono-gc.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/object.h>
#include <mono/jit/jit.h>
#include <mono/jit/mono-private-unstable.h>
#include <TargetConditionals.h>
#import <os/log.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <stdio.h>

static char *bundle_path;

#define APPLE_RUNTIME_IDENTIFIER "//%APPLE_RUNTIME_IDENTIFIER%"

#define RUNTIMECONFIG_BIN_FILE "runtimeconfig.bin"

void runtime_one_init (const char *bundle);
int runtime_one_exec (const char *executable, int argi, const char *managed_argv);
void runtime_one_shutdown (void);

void runtime_two_init (const char *bundle);
int runtime_two_exec (const char *executable, int argi, const char *managed_argv);
void runtime_two_shutdown (void);

const char *
get_bundle_path (void)
{
    if (bundle_path)
        return bundle_path;
    NSBundle* main_bundle = [NSBundle mainBundle];
    NSString* path = [main_bundle bundlePath];

#if TARGET_OS_MACCATALYST
    path = [path stringByAppendingString:@"/Contents/Resources"];
#endif

    bundle_path = strdup ([path UTF8String]);

    return bundle_path;
}

void
mono_ios_runtime_init (void)
{
#if INVARIANT_GLOBALIZATION
    setenv ("DOTNET_SYSTEM_GLOBALIZATION_INVARIANT", "1", TRUE);
#endif

#if ENABLE_RUNTIME_LOGGING
    setenv ("MONO_LOG_LEVEL", "debug", TRUE);
    setenv ("MONO_LOG_MASK", "all", TRUE);
#endif

    // build using DiagnosticPorts property in AppleAppBuilder
    // or set DOTNET_DiagnosticPorts env via mlaunch, xharness when undefined.
    // NOTE, using DOTNET_DiagnosticPorts requires app build using AppleAppBuilder and RuntimeComponents=diagnostics_tracing
#ifdef DIAGNOSTIC_PORTS
    setenv ("DOTNET_DiagnosticPorts", DIAGNOSTIC_PORTS, true);
#endif

    id args_array = [[NSProcessInfo processInfo] arguments];
    assert ([args_array count] <= 128);
    const char *managed_argv [128];
    int argi;
    for (argi = 0; argi < [args_array count]; argi++) {
        NSString* arg = [args_array objectAtIndex: argi];
        managed_argv[argi] = [arg UTF8String];
    }

    const char* bundle = get_bundle_path ();
    chdir (bundle);

    const char* executable = "%EntryPointLibName%";
    if (executable [0] == '\0') {
        executable = getenv ("MONO_APPLE_APP_ENTRY_POINT_LIB_NAME");
    }
    if (executable == NULL) {
        executable = "";
    }

    runtime_one_init (bundle);
    runtime_two_init (bundle);

    int res = runtime_one_exec (executable, argi, managed_argv);
    os_log_info (OS_LOG_DEFAULT, "RUNTIME ONE RETURN VALUE '%d'.", res);

    res = runtime_two_exec (executable, argi, managed_argv);
    os_log_info (OS_LOG_DEFAULT, "RUNTIME TWO RETURN VALUE '%d'.", res);

    runtime_one_shutdown ();
    runtime_two_shutdown ();
    
    exit (res);
}
