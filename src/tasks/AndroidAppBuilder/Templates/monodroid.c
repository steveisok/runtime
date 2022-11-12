// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <jni.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <sys/mman.h>
#include <assert.h>
#include <unistd.h>

static char *bundle_path;
static char *executable;

#define LOG_INFO(fmt, ...) __android_log_print(ANDROID_LOG_DEBUG, "DOTNET-MONODROID", fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, "DOTNET-MONODROID", fmt, ##__VA_ARGS__)

#if defined(__arm__)
#define ANDROID_RUNTIME_IDENTIFIER "android-arm"
#elif defined(__aarch64__)
#define ANDROID_RUNTIME_IDENTIFIER "android-arm64"
#elif defined(__i386__)
#define ANDROID_RUNTIME_IDENTIFIER "android-x86"
#elif defined(__x86_64__)
#define ANDROID_RUNTIME_IDENTIFIER "android-x64"
#else
#error Unknown architecture
#endif

#define RUNTIMECONFIG_BIN_FILE "runtimeconfig.bin"

static void
strncpy_str (JNIEnv *env, char *buff, jstring str, int nbuff)
{
    jboolean isCopy = 0;
    const char *copy_buff = (*env)->GetStringUTFChars (env, str, &isCopy);
    strncpy (buff, copy_buff, nbuff);
    if (isCopy)
        (*env)->ReleaseStringUTFChars (env, str, copy_buff);
}

void
Java_net_dot_MonoRunner_setEnv (JNIEnv* env, jobject thiz, jstring j_key, jstring j_value)
{
    const char *key = (*env)->GetStringUTFChars(env, j_key, 0);
    const char *val = (*env)->GetStringUTFChars(env, j_value, 0);
    setenv (key, val, true);
    (*env)->ReleaseStringUTFChars(env, j_key, key);
    (*env)->ReleaseStringUTFChars(env, j_value, val);
}

int
Java_net_dot_MonoRunner_initRuntime (JNIEnv* env, jobject thiz, jstring j_files_dir, jstring j_cache_dir, jstring j_testresults_dir, jstring j_entryPointLibName, jobjectArray j_args, long current_local_time)
{
    char file_dir[2048];
    char cache_dir[2048];
    char testresults_dir[2048];
    char entryPointLibName[2048];
    strncpy_str (env, file_dir, j_files_dir, sizeof(file_dir));
    strncpy_str (env, cache_dir, j_cache_dir, sizeof(cache_dir));
    strncpy_str (env, testresults_dir, j_testresults_dir, sizeof(testresults_dir));
    strncpy_str (env, entryPointLibName, j_entryPointLibName, sizeof(entryPointLibName));

    bundle_path = file_dir;
    executable = entryPointLibName;

    setenv ("HOME", bundle_path, true);
    setenv ("TMPDIR", cache_dir, true);
    setenv ("TEST_RESULTS_DIR", testresults_dir, true);

    //setenv ("MONO_LOG_LEVEL", "debug", true);
    //setenv ("MONO_LOG_MASK", "all", true);

    int args_len = (*env)->GetArrayLength(env, j_args);
    int managed_argc = args_len + 1;
    char** managed_argv = (char**)malloc(managed_argc * sizeof(char*));

    managed_argv[0] = bundle_path;
    for (int i = 0; i < args_len; ++i)
    {
        jstring j_arg = (*env)->GetObjectArrayElement(env, j_args, i);
        managed_argv[i + 1] = (*env)->GetStringUTFChars(env, j_arg, NULL);
    }

    runtime_one_init (bundle_path, current_local_time);
    runtime_two_init (bundle_path, current_local_time);

    int res = runtime_one_exec (executable, managed_argc, managed_argv);

    LOG_INFO ("Runtime ONE returned %d \n", res);

    res = runtime_two_exec (executable, managed_argc, managed_argv);

    LOG_INFO ("Runtime TWO returned %d \n", res);

    runtime_one_shutdown ();
    runtime_two_shutdown ();

    for (int i = 0; i < args_len; ++i)
    {
        jstring j_arg = (*env)->GetObjectArrayElement(env, j_args, i);
        (*env)->ReleaseStringUTFChars(env, j_arg, managed_argv[i + 1]);
    }

    free(managed_argv);
    return res;
}
