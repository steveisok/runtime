// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Shared JSON writer with pluggable output sink.
//
// Used by both the in-process crash reporter (buffer sink, signal handler)
// and the out-of-process createdump tool (file descriptor sink).

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Output sink: called to emit bytes. Returns 0 on success, -1 on error.
typedef int (*JsonEmitFn)(void* ctx, const char* data, int len);

// Policy flags for format variations between consumers.
enum JsonWriterFlags
{
    JsonWriter_Compact      = 0,       // No whitespace (default)
    JsonWriter_PrettyPrint  = 0x1,     // Indent with spaces, newlines between elements
    JsonWriter_QuotedBools  = 0x2,     // Emit bools as "true"/"false" (quoted strings)
    JsonWriter_SpaceAfterColon = 0x4,  // Emit " : " instead of ":" after keys
};

typedef struct
{
    JsonEmitFn emit;
    void* emitCtx;
    int depth;
    int commaNeeded;
    int indentSize;     // Spaces per indent level (used when PrettyPrint is set)
    int flags;
    int failed;         // Latched error — once set, all operations become no-ops
} JsonWriter;

void JsonWriter_Init(JsonWriter* w, JsonEmitFn emit, void* ctx, int indentSize, int flags);
void JsonWriter_OpenObject(JsonWriter* w, const char* key);
void JsonWriter_CloseObject(JsonWriter* w);
void JsonWriter_OpenArray(JsonWriter* w, const char* key);
void JsonWriter_CloseArray(JsonWriter* w);
void JsonWriter_WriteString(JsonWriter* w, const char* key, const char* value);
void JsonWriter_WriteInt(JsonWriter* w, const char* key, int64_t value);
void JsonWriter_WriteHex(JsonWriter* w, const char* key, uint64_t value);
void JsonWriter_WriteBool(JsonWriter* w, const char* key, int value);

// Buffer-based output sink for in-process crash reporting.
typedef struct
{
    char* buffer;
    int capacity;
    int pos;
} JsonBufferSink;

void JsonBufferSink_Init(JsonBufferSink* sink, char* buffer, int capacity);
int  JsonBufferSink_Emit(void* ctx, const char* data, int len);

// File-descriptor output sink for createdump.
typedef struct
{
    int fd;
    int error;
} JsonFileSink;

void JsonFileSink_Init(JsonFileSink* sink, int fd);
int  JsonFileSink_Emit(void* ctx, const char* data, int len);

#ifdef __cplusplus
}
#endif
