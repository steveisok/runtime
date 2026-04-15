// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Shared JSON writer implementation.
// See jsonwriter.h for API documentation.

#include "jsonwriter.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void Emit(JsonWriter* w, const char* data, int len)
{
    if (w->failed)
        return;
    if (w->emit(w->emitCtx, data, len) != 0)
        w->failed = 1;
}

static void EmitStr(JsonWriter* w, const char* s)
{
    Emit(w, s, (int)strlen(s));
}

static void EmitChar(JsonWriter* w, char c)
{
    Emit(w, &c, 1);
}

static void EmitIndent(JsonWriter* w)
{
    if (!(w->flags & JsonWriter_PrettyPrint))
        return;

    EmitChar(w, '\n');
    int spaces = w->depth * w->indentSize;
    // Emit in chunks to avoid per-byte calls
    static const char blanks[] = "                ";
    while (spaces > 0)
    {
        int n = spaces > 16 ? 16 : spaces;
        Emit(w, blanks, n);
        spaces -= n;
    }
}

static void EmitSeparator(JsonWriter* w)
{
    if (w->commaNeeded)
        EmitChar(w, ',');
    EmitIndent(w);
}

static void EmitKey(JsonWriter* w, const char* key)
{
    EmitSeparator(w);
    if (key != NULL)
    {
        EmitChar(w, '"');
        EmitStr(w, key);
        EmitChar(w, '"');
        if (w->flags & JsonWriter_SpaceAfterColon)
            EmitStr(w, " : ");
        else
            EmitChar(w, ':');
    }
}

static void EmitEscapedString(JsonWriter* w, const char* value)
{
    EmitChar(w, '"');
    if (value != NULL)
    {
        for (const char* p = value; *p != '\0'; p++)
        {
            switch (*p)
            {
                case '"':  EmitStr(w, "\\\""); break;
                case '\\': EmitStr(w, "\\\\"); break;
                case '\n': EmitStr(w, "\\n"); break;
                case '\r': EmitStr(w, "\\r"); break;
                case '\t': EmitStr(w, "\\t"); break;
                case '\b': EmitStr(w, "\\b"); break;
                case '\f': EmitStr(w, "\\f"); break;
                default:
                    if ((unsigned char)*p < 0x20)
                    {
                        char esc[8];
                        snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)*p);
                        EmitStr(w, esc);
                    }
                    else
                    {
                        EmitChar(w, *p);
                    }
                    break;
            }
        }
    }
    EmitChar(w, '"');
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void JsonWriter_Init(JsonWriter* w, JsonEmitFn emit, void* ctx, int indentSize, int flags)
{
    w->emit = emit;
    w->emitCtx = ctx;
    w->depth = 0;
    w->commaNeeded = 0;
    w->indentSize = indentSize;
    w->flags = flags;
    w->failed = 0;
}

void JsonWriter_OpenObject(JsonWriter* w, const char* key)
{
    if (w->failed) return;
    EmitKey(w, key);
    EmitChar(w, '{');
    w->depth++;
    w->commaNeeded = 0;
}

void JsonWriter_CloseObject(JsonWriter* w)
{
    if (w->failed) return;
    w->depth--;
    if (w->flags & JsonWriter_PrettyPrint)
        EmitIndent(w);
    EmitChar(w, '}');
    w->commaNeeded = 1;
}

void JsonWriter_OpenArray(JsonWriter* w, const char* key)
{
    if (w->failed) return;
    EmitKey(w, key);
    EmitChar(w, '[');
    w->depth++;
    w->commaNeeded = 0;
}

void JsonWriter_CloseArray(JsonWriter* w)
{
    if (w->failed) return;
    w->depth--;
    if (w->flags & JsonWriter_PrettyPrint)
        EmitIndent(w);
    EmitChar(w, ']');
    w->commaNeeded = 1;
}

void JsonWriter_WriteString(JsonWriter* w, const char* key, const char* value)
{
    if (w->failed) return;
    EmitKey(w, key);
    EmitEscapedString(w, value);
    w->commaNeeded = 1;
}

void JsonWriter_WriteInt(JsonWriter* w, const char* key, int64_t value)
{
    if (w->failed) return;
    EmitKey(w, key);
    char buf[24];
    snprintf(buf, sizeof(buf), "%lld", (long long)value);
    EmitStr(w, buf);
    w->commaNeeded = 1;
}

void JsonWriter_WriteHex(JsonWriter* w, const char* key, uint64_t value)
{
    if (w->failed) return;
    EmitKey(w, key);
    char buf[24];
    snprintf(buf, sizeof(buf), "\"0x%" PRIx64 "\"", value);
    EmitStr(w, buf);
    w->commaNeeded = 1;
}

void JsonWriter_WriteBool(JsonWriter* w, const char* key, int value)
{
    if (w->failed) return;
    EmitKey(w, key);
    if (w->flags & JsonWriter_QuotedBools)
    {
        // Legacy createdump format: bools as quoted strings
        EmitStr(w, value ? "\"true\"" : "\"false\"");
    }
    else
    {
        EmitStr(w, value ? "true" : "false");
    }
    w->commaNeeded = 1;
}

// ---------------------------------------------------------------------------
// Buffer sink — for in-process crash reporter (signal handler safe)
// ---------------------------------------------------------------------------

void JsonBufferSink_Init(JsonBufferSink* sink, char* buffer, int capacity)
{
    sink->buffer = buffer;
    sink->capacity = capacity;
    sink->pos = 0;
    if (capacity > 0)
        buffer[0] = '\0';
}

int JsonBufferSink_Emit(void* ctx, const char* data, int len)
{
    JsonBufferSink* sink = (JsonBufferSink*)ctx;
    int remaining = sink->capacity - sink->pos - 1;
    if (remaining <= 0)
        return 0; // Silently truncate — best effort
    if (len > remaining)
        len = remaining;
    memcpy(sink->buffer + sink->pos, data, len);
    sink->pos += len;
    sink->buffer[sink->pos] = '\0';
    return 0;
}

// ---------------------------------------------------------------------------
// File descriptor sink — for createdump (out-of-process)
// ---------------------------------------------------------------------------

void JsonFileSink_Init(JsonFileSink* sink, int fd)
{
    sink->fd = fd;
    sink->error = 0;
}

int JsonFileSink_Emit(void* ctx, const char* data, int len)
{
    JsonFileSink* sink = (JsonFileSink*)ctx;
    if (sink->error)
        return -1;

    const char* p = data;
    int remaining = len;
    while (remaining > 0)
    {
        ssize_t written;
        do {
            written = write(sink->fd, p, remaining);
        } while (written == -1 && errno == EINTR);

        if (written < 1)
        {
            sink->error = errno;
            return -1;
        }
        p += written;
        remaining -= (int)written;
    }
    return 0;
}
