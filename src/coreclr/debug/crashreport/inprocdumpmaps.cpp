// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// /proc/self/maps and /proc/self/auxv parser for in-process dump generation.
// All code is async-signal-safe.

#if defined(__linux__)

#include "inprocdump.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Minimal hex/decimal parsing — async-signal-safe (no strtoul dependency)
// ---------------------------------------------------------------------------

static uint64_t ParseHex(const char* s, const char** endp)
{
    uint64_t val = 0;
    while (*s)
    {
        char c = *s;
        if (c >= '0' && c <= '9')
            val = (val << 4) | (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            val = (val << 4) | (uint64_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            val = (val << 4) | (uint64_t)(c - 'A' + 10);
        else
            break;
        s++;
    }
    if (endp) *endp = s;
    return val;
}

static void SkipSpaces(const char** pp)
{
    while (**pp == ' ' || **pp == '\t')
        (*pp)++;
}

static void SkipNonSpaces(const char** pp)
{
    while (**pp != '\0' && **pp != ' ' && **pp != '\t' && **pp != '\n')
        (*pp)++;
}

// ---------------------------------------------------------------------------
// Parse a single /proc/self/maps line:
//   "7f1234000000-7f1234021000 r-xp 00000000 08:02 135522 /usr/lib64/ld.so"
// ---------------------------------------------------------------------------

static int ParseMapsLine(const char* line,
                         uint64_t* start, uint64_t* end,
                         uint32_t* flags, uint64_t* offset,
                         char* name, int nameMax)
{
    const char* p = line;

    // start address
    *start = ParseHex(p, &p);
    if (*p != '-') return 0;
    p++;

    // end address
    *end = ParseHex(p, &p);
    SkipSpaces(&p);

    // permissions: r/w/x/s/p
    *flags = 0;
    if (*p == 'r') *flags |= PF_R;
    p++;
    if (*p == 'w') *flags |= PF_W;
    p++;
    if (*p == 'x') *flags |= PF_X;
    p++;
    p++; // skip 's' or 'p'
    SkipSpaces(&p);

    // offset
    *offset = ParseHex(p, &p);
    SkipSpaces(&p);

    // dev (skip)
    SkipNonSpaces(&p);
    SkipSpaces(&p);

    // inode (skip)
    SkipNonSpaces(&p);
    SkipSpaces(&p);

    // pathname (rest of line, trimmed)
    name[0] = '\0';
    if (*p != '\0' && *p != '\n')
    {
        int i = 0;
        while (*p != '\0' && *p != '\n' && i < nameMax - 1)
        {
            name[i++] = *p++;
        }
        name[i] = '\0';
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Read /proc/self/maps into state->regions[] and state->modules[]
// ---------------------------------------------------------------------------

int InProcDumpMaps_Collect(struct InProcDumpState* state)
{
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return -1;

    // Read line-by-line using a static buffer.
    // /proc/self/maps lines are typically <200 chars.
    char buf[4096];
    char line[1024];
    int linePos = 0;
    int bufPos = 0;
    int bufLen = 0;

    state->regionCount = 0;
    state->moduleCount = 0;
    state->truncatedRegions = 0;
    state->truncatedModules = 0;

    for (;;)
    {
        // Refill buffer if needed
        if (bufPos >= bufLen)
        {
            bufLen = (int)read(fd, buf, sizeof(buf));
            if (bufLen <= 0) break;
            bufPos = 0;
        }

        char c = buf[bufPos++];
        if (c == '\n' || linePos >= (int)sizeof(line) - 1)
        {
            line[linePos] = '\0';
            linePos = 0;

            if (line[0] == '\0') continue;

            uint64_t start, end, offset;
            uint32_t flags;
            char name[INPROC_MAX_MODULE_NAME];

            if (!ParseMapsLine(line, &start, &end, &flags, &offset, name, INPROC_MAX_MODULE_NAME))
                continue;

            // Add to regions array
            if (state->regionCount < INPROC_MAX_MEMORY_REGIONS)
            {
                struct InProcMemoryRegion* r = &state->regions[state->regionCount];
                r->start = start;
                r->end = end;
                r->flags = flags;
                r->offset = offset;
                // Copy name safely
                int i = 0;
                while (name[i] && i < INPROC_MAX_MODULE_NAME - 1) { r->name[i] = name[i]; i++; }
                r->name[i] = '\0';
                state->regionCount++;
            }
            else
            {
                state->truncatedRegions = 1;
            }

            // If named and not a pseudo-path, add to modules
            if (name[0] != '\0' && name[0] != '[')
            {
                // Check if this is a new module (different name from last)
                int isNew = 1;
                if (state->moduleCount > 0)
                {
                    struct InProcModuleInfo* prev = &state->modules[state->moduleCount - 1];
                    // Same module if same name — extend the range
                    int same = 1;
                    for (int i = 0; name[i] || prev->name[i]; i++)
                    {
                        if (name[i] != prev->name[i]) { same = 0; break; }
                    }
                    if (same)
                    {
                        // Extend existing module's range
                        if (end > prev->end) prev->end = end;
                        isNew = 0;
                    }
                }

                if (isNew)
                {
                    if (state->moduleCount < INPROC_MAX_MODULES)
                    {
                        struct InProcModuleInfo* m = &state->modules[state->moduleCount];
                        m->start = start;
                        m->end = end;
                        m->offset = offset;
                        int i = 0;
                        while (name[i] && i < INPROC_MAX_MODULE_NAME - 1) { m->name[i] = name[i]; i++; }
                        m->name[i] = '\0';
                        state->moduleCount++;
                    }
                    else
                    {
                        state->truncatedModules = 1;
                    }
                }
            }
        }
        else
        {
            line[linePos++] = c;
        }
    }

    close(fd);
    return 0;
}

// ---------------------------------------------------------------------------
// Read /proc/self/auxv into state->auxv[]
// ---------------------------------------------------------------------------

int InProcDumpMaps_CollectAuxv(struct InProcDumpState* state)
{
    int fd = open("/proc/self/auxv", O_RDONLY);
    if (fd < 0)
    {
        state->auxvSize = 0;
        return -1;
    }

    int total = 0;
    while (total < (int)sizeof(state->auxv))
    {
        int n = (int)read(fd, state->auxv + total, sizeof(state->auxv) - total);
        if (n <= 0) break;
        total += n;
    }
    state->auxvSize = total;
    close(fd);
    return 0;
}

#endif // __linux__
