# Build Caching for dotnet/runtime: Findings and Recommendations

## Executive Summary

We investigated build performance for dotnet/runtime on macOS ARM64 (12 cores),
focusing on caching strategies that survive clean rebuilds — the scenario CI faces
on every run. We implemented three caching layers:

| Cache | Target | Savings (exclusive time) | Hit Rate |
|-------|--------|--------------------------|----------|
| **sccache** (native C/C++) | CMake compilation | 99% hit rate, near-zero compile time on warm cache | 2829/2856 |
| **ILLink cache** (managed trimming) | ILLink assembly trimming | 165s → 26s (**-139s**) | 107/107 |
| **ApiCompat cache** (API validation) | ApiCompat validation | 28s → 9s (**-17s**) | 56/56 |

On a full clean rebuild with caches warm, the binlog duration dropped from
**576s → 452s (22% reduction)**. On incremental builds (one library changed),
the improvement was **461s → 180s (61% reduction)**.

## Background

dotnet/runtime builds roughly 2,800 native C/C++ compilation units and 730 managed
C# projects. On a clean build (artifacts wiped), MSBuild's built-in incrementality
— which is timestamp-based — provides no help because all sources are "newer" than
all (missing) outputs.

The build system already uses MSBuild's `/m` (parallel) flag across 12 cores, and
Roslyn's shared compilation server. The question was: what else can we cache?

## What We Tried

### 1. sccache for Native Builds ✅

[sccache](https://github.com/mozilla/sccache) is a compilation cache for C/C++
(similar to ccache). It hashes preprocessed source + compiler flags + compiler binary
to produce a content-addressed cache key.

**What we did:**

- Auto-detect sccache in PATH and configure it as `CMAKE_C_COMPILER_LAUNCHER` /
  `CMAKE_CXX_COMPILER_LAUNCHER` centrally in `eng/native/build-commons.sh`
- Created a macOS wrapper script (`eng/native/sccache-xarch-wrapper.sh`) to handle
  CMake's `-Xarch_<arch>` flags that sccache drops
- Fixed sccache server lifecycle management in `eng/build.sh`

**Results:** 99.05% cache hit rate (2829/2856). The 27 misses are expected
(first-time compiles after cache purge or source changes).

**Limitation:** sccache cannot cache CMake configure (~36s), linking (~40s), or
precompiled header generation (~15s). These remain fixed costs on every build.

### 2. Per-Compilation Csc Cache ❌ (Abandoned)

We attempted to build a content-addressed cache around each Csc (C# compiler)
invocation, hashing source files, references, and compiler options.

**Why it failed:** The 731 Csc invocations average only 75ms each (55s total).
The overhead of hashing all inputs — source files, reference assemblies, analyzers —
was 118s, more than double the actual compilation cost. Per-compilation caching
is a net loss when the compiler is this fast.

**Lesson learned:** Caching is only worthwhile when the cached operation is
significantly more expensive than computing the cache key.

### 3. ILLink Trimming Cache ✅

ILLink trims unused code from 107 assemblies during the build, taking ~165s total.
Unlike Csc, each ILLink invocation averages 1.4s with significant I/O (reading
assemblies, descriptor XMLs, reference assemblies).

**Design:**

- **Cache key:** SHA256 of pre-trim assembly content + PDB content + ILLink arguments
  + descriptor XML contents + reference assembly DLL contents. Using file
  *content* (not timestamps) is critical for correctness across clean rebuilds.
- **Cache storage:** `~/.illinkcache/<hash-prefix>/<hash>/` containing the trimmed
  DLL and PDB.
- **Integration:** Two MSBuild targets in `eng/illink.targets` — `_ILLinkCacheCheck`
  (before ILLink) and `_ILLinkCachePopulate` (after ILLink). On cache hit, the
  cached outputs are copied into place and ILLink is skipped via a `Condition`.
- **Key pass-through:** The cache key computed during the check phase is passed
  to the populate phase as an MSBuild property. This avoids a subtle bug where
  recomputing the key after ILLink runs produces a different hash (because ILLink
  modifies the output assembly).

**Results:** 107/107 cache hits on second build. ILLink dropped from 165s → 26s
(the remaining 4 invocations are special shared-framework trims that use different
code paths).

### 4. ApiCompat Validation Cache ✅

ApiCompat validates that implementation assemblies are compatible with their
reference assembly contracts. It runs for 56 libraries, taking ~28s total.

**Design:**

- **Cache key:** SHA256 of left assemblies (contracts) + right assemblies
  (implementations) + suppression files.
- **Cache storage:** `~/.apicompat-cache/<hash-prefix>/<hash>.ok` marker files.
- **Integration:** Leverages MSBuild's own `Inputs`/`Outputs` incrementality on
  the `ApiCompatValidateAssembliesCore` target. On cache hit, the task writes the
  semaphore file that MSBuild checks, causing it to skip the target entirely.
- **Same key pass-through pattern** as ILLink cache to avoid MISS/STORE key mismatch.

**Results:** 56/56 cache hits on second build. ApiCompat dropped from 28s → 9s.

## Measured Results

### Full Clean Rebuild (artifacts wiped)

| Metric | Without Caches | With Caches | Improvement |
|--------|---------------|-------------|-------------|
| **Binlog duration** | 576s | 452s | **-22%** |
| **ILLink** | 111 exec, 165s | 4 exec, 26s | **-139s** |
| **ApiCompat** | 59 exec, 28s | 3 exec, 9s | **-17s** |
| **Csc** | 731 exec, 192s | 731 exec, 184s | ~same |
| **Exec (native)** | 210s | 183s | ~same (variance) |

### Incremental Build (one file changed)

| Metric | Full Build | Incremental | Improvement |
|--------|-----------|-------------|-------------|
| **Binlog duration** | 461s | 180s | **-61%** |
| **Csc** | 731 projects | 106 projects | MSBuild skipped unchanged |
| **ILLink** | 107 cached | 106 HIT, 1 MISS | Only changed library re-trimmed |
| **ApiCompat** | 56 cached | All HIT | Skipped entirely |

### Where Time Goes Now (with caches warm)

| Task | Exclusive Time | Notes |
|------|---------------|-------|
| Csc (compilation) | 184s | 731 projects, long tail |
| Exec (native build) | 183s | CMake configure, linking, PCH |
| Restore | 35s | NuGet package resolution |
| ILLink (uncached) | 26s | 4 shared-framework trims |
| ApiCompat (uncached) | 9s | 3 special projects |

## Why MSBuild Makes This Harder Than It Should Be

### The Fundamental Problem

MSBuild's incrementality is **timestamp-based**. The `Inputs`/`Outputs` mechanism
on targets compares `LastWriteTime` of input files against output files. This works
for local iterative development but completely breaks on:

- **CI builds** where the repo is cloned fresh (all timestamps are "now")
- **Clean builds** where `artifacts/` is deleted
- **Branch switching** where git updates timestamps on checkout

In all three cases, MSBuild sees every input as "newer" than every (missing) output
and rebuilds everything.

### What We Had to Build by Hand

For each cacheable build step (ILLink, ApiCompat), we had to:

1. **Identify all inputs** — assemblies, PDBs, config files, command-line arguments.
   MSBuild doesn't track these at the task level; we had to read the `.targets` files
   and figure out what flows into each task.

2. **Compute content hashes** — SHA256 of file contents, not timestamps. We wrote
   inline MSBuild tasks using `RoslynCodeTaskFactory` to hash inputs at build time.

3. **Manage a cache store** — directory structure, hash-based lookup, cache
   population, file copying. All custom code.

4. **Wire into the build** — `BeforeTargets`/`AfterTargets` hooks, MSBuild property
   flow between check and populate phases, conditions to skip targets on cache hits.

5. **Debug subtle correctness issues** — the "key mismatch" bug (where the populate
   step recomputes the hash after the build step has modified outputs, producing a
   different key) hit us in both ILLink and ApiCompat. This is a fundamental hazard
   of hand-rolled caching.

This is ~500 lines of inline C# and MSBuild XML per cached step, and every new
cacheable target requires repeating this process.

### Why MSBuildCache Doesn't Help

[Microsoft.MSBuildCache](https://github.com/microsoft/MSBuildCache) is the official
project-level caching solution for MSBuild. It requires `/reportfileaccesses`, which
uses **Microsoft Detours** to intercept Win32 API calls and discover what files
each project reads and writes.

Detours is Windows-only. There is no cross-platform equivalent:

- **ptrace** (Linux): 2-5x performance overhead, conflicts with debuggers
- **LD_PRELOAD / DYLD_INSERT_LIBRARIES** (Linux/Mac): Missed by static binaries,
  Go/Rust direct syscalls, and macOS SIP restrictions
- **FUSE** (Linux/Mac): Latency on every file operation, requires special setup
- **dtrace** (Mac): Requires SIP disabled on modern macOS

MSBuildCache is therefore a Windows-CI-only solution.

## How MSBuild Could Help

### 1. Content-Hash Mode for Inputs/Outputs 🟢 (Most Feasible)

**Today:**
```xml
<Target Name="CoreCompile"
        Inputs="@(Compile);@(ReferencePath)"
        Outputs="$(OutputAssembly)">
```
This compares `LastWriteTime`. After wiping artifacts, everything rebuilds.

**Proposed:**
```xml
<Target Name="CoreCompile"
        Inputs="@(Compile);@(ReferencePath)"
        Outputs="$(OutputAssembly)"
        IncrementalMode="ContentHash">
```
MSBuild computes SHA256 of input file contents. If the hash matches a previous
build's hash, the target is skipped — even on a clean rebuild, even in CI.

**Implementation scope:** Modify `TargetUpToDateChecker` in the MSBuild engine to
support content hashing alongside timestamps. Store hash→output mappings in a
local cache directory. A few hundred lines of engine code plus a new attribute.

**Impact:** This would eliminate the need for hand-rolled caches like our ILLink
and ApiCompat implementations. Any target with correct `Inputs`/`Outputs`
declarations would get content-addressed caching for free.

**Limitation:** Only works for targets that correctly declare all their inputs.
Many third-party tasks and `Exec`-based targets don't.

### 2. First-Class Input/Output Declarations on Tasks 🟡 (Hard)

Today, MSBuild tasks are C# classes that can read/write any file. The engine has
no way to know what files a task actually touches without OS-level interception.

Adding `[DeclaredInput]` / `[DeclaredOutput]` attributes to task parameters would
let MSBuild compute cache keys per-task, not just per-target. This would require
updating all built-in tasks and getting the ecosystem to adopt, but would make
task-level caching reliable and automatic.

### 3. Native Cache Protocol 🟢 (Feasible)

A built-in `GET hash → tarball / PUT hash ← tarball` protocol with pluggable
backends (local disk, HTTP, cloud storage) would make project-level caching
accessible without the MSBuildCache plugin complexity or the Detours dependency.

MSBuild already has the `ProjectCachePluginBase` abstraction. Shipping a simple
built-in implementation that works cross-platform with declared inputs would
democratize caching for all MSBuild users.

### 4. Deterministic Output Enforcement 🟡 (Incremental)

Non-deterministic outputs (timestamps in PEs, varying MVIDs, embedded paths)
cause false cache misses. Roslyn supports `/deterministic`, but the surrounding
toolchain (resource generators, assembly info tasks, satellite assemblies) still
introduces variance. Auditing and fixing these tools one by one would improve
cache hit rates across the ecosystem.

### 5. Hermetic Task Execution 🔴 (Very Hard)

Running each task in a filesystem sandbox where it can only see declared inputs
would guarantee cache correctness by construction. This is what Bazel does, but
retrofitting it onto MSBuild's 20-year-old architecture — where tasks assume full
filesystem access — would be a multi-year effort requiring a fundamentally new
execution model.

## Recommendations

1. **Short term:** The ILLink and ApiCompat caches demonstrated here are
   production-ready for dotnet/runtime. They save ~155s of exclusive build time
   on clean rebuilds and work cross-platform with no special OS support.

2. **Medium term:** MSBuild should add content-hash mode for `Inputs`/`Outputs`.
   This is the highest-impact, lowest-cost improvement. It would make hand-rolled
   caches unnecessary for any target that correctly declares its dependencies.

3. **Long term:** A cross-platform native cache protocol in MSBuild would enable
   project-level caching without the Detours dependency, making build caching
   accessible to the entire .NET ecosystem — not just Windows CI.

## Environment

- **Machine:** macOS ARM64, Apple Silicon, 12 cores
- **SDK:** .NET 11.0.100-preview.1
- **MSBuild:** 18.4.0
- **Build command:** `./build.sh clr+libs+host -rc checked`
- **sccache:** 0.13.0, local disk cache (10 GiB max)
