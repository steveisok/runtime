# Browser CoreCLR debugging with universal DAC and DBI

This document describes the prototype architecture used to debug CoreCLR running
as WebAssembly in a browser. It covers:

- loading the universal DAC and DBI;
- connecting Concord to BrowserDebugProxy;
- reading and writing WebAssembly linear memory;
- transporting CoreCLR debugger IPC messages in both directions;
- coordinating browser pauses, managed debugger events, continue requests, and
  function evaluation.

The implementation spans three repositories:

- **dotnet/runtime**
  - universal DAC and DBI;
  - the CoreCLR left-side debugger;
  - the cooperative WebAssembly debugger transport;
  - the JavaScript byte queues exposed by the browser runtime.
- **dotnet-diagnostics-internal-components**
  - `Microsoft.Diagnostics.BrowserDebugProxy`;
  - the bridge between the local Concord connection and Chrome DevTools
    Protocol (CDP).
- **Concord**
  - the browser debug channel;
  - universal DAC/DBI loading;
  - the `ICorDebug` wrapper used by the Visual Studio debug engine.

## Architecture

There are two nested protocols:

1. The **browser debug channel protocol** is a small request/response protocol
   between Concord and BrowserDebugProxy. It carries target information, memory
   operations, process control, and opaque debugger transport bytes.
2. The **CoreCLR debugger transport protocol** is the existing DBI-to-runtime
   protocol implemented by `DbgTransportSession`. Its framed messages and
   `DebuggerIPCEvent` payloads are carried unchanged inside browser debug channel
   commands 7 and 8.

Keeping these protocols separate is important. BrowserDebugProxy understands
browser pause state and WebAssembly memory, but it does not decode the CoreCLR
debugger transport. DBI and the runtime continue to own that protocol.

Visual Studio communicates with universal DBI through `ICorDebug`. Universal
DBI uses `CBrowserDebugChannel` for both target-memory access and its
byte-oriented `IDebugChannel`. Concord sends browser-channel requests over
localhost TCP to BrowserDebugProxy. The proxy uses CDP to evaluate JavaScript,
access WebAssembly linear memory, and control physical browser pauses.

Commands 7 and 8 do not define new debugger events. They carry portions of the
existing `DbgTransportSession` byte stream inside the browser debug channel.
BrowserDebugProxy treats those bytes as opaque.

## Starting the browser connection

For a browser CoreCLR target, Concord takes a browser-specific branch in
`CDbgShimLiveProcLoader`.

1. Concord creates `CBrowserDebugChannel`.
2. The channel opens a localhost TCP listener on the configured ICorDebug port.
3. BrowserDebugProxy starts `CoreClrProxy.RunDebugChannelAsync` and connects to
   that listener.
4. Concord waits for the connection and sends `GetTargetInfo`.
5. BrowserDebugProxy waits until:
   - the CoreCLR browser runtime has begun initialization;
   - WebAssembly linear memory is available;
   - the address of `DotNetRuntimeContractDescriptor` is available; and
   - the browser target is paused.
6. BrowserDebugProxy returns the runtime identity and contract information that
   Concord needs to create the virtual process.

The current browser target information contains:

| Field | Current value or source |
|---|---|
| Runtime base address | Nonzero virtual-process identity |
| CLR instance ID | Nonzero virtual-process identity |
| Contract descriptor address | Address exported by the CoreCLR browser runtime |
| Pointer size | 4 bytes |
| Platform | Browser/WASM `CorDebugPlatform` value |

The runtime base and CLR instance ID identify the single browser runtime. Target
memory access is based on WebAssembly linear-memory offsets rather than an
operating-system process address space.

Local snapshots of heap references must also use the target pointer width.
Universal DBI stores a target object address in a checked 32-bit or 64-bit
integer before passing its byte view to reference initialization; a host
`void*` is not a target-width snapshot. The storage stays alive through
synchronous initialization. `CORDB_ADDRESS` uses zero-extension, so an address
that cannot fit a 32-bit target is rejected rather than truncated. This local
snapshot is not the little-endian wire representation of debugger IPC.

## Loading the universal DBI and DAC

After receiving valid target information, Concord resolves both universal
components from its browser assets directory. Windows uses
`mscordbi_universal.dll` and `mscordaccore_universal.dll`; macOS uses
`libmscordbi_universal.dylib` and `libmscordaccore_universal.dylib`; Linux uses
`libmscordbi_universal.so` and `libmscordaccore_universal.so`. These are
host-native diagnostic libraries, not WebAssembly modules. "Universal" refers
to the diagnostic implementation, not a fat Mach-O binary or a library that can
be loaded on every host.

Concord loads the host-native universal DBI and resolves:

- `OpenVirtualProcessImpl4`;
- `RegisterVirtualProcessImpl`.

It calls `OpenVirtualProcessImpl4` with:

- the browser CLR instance ID;
- `CBrowserDebugChannel` as an `ICorDebugMutableDataTarget`;
- the path to the host-native universal DAC;
- the target contract descriptor address;
- the requested `ICorDebugProcess` interface.

`OpenVirtualProcessImpl4` performs two related operations:

1. It creates an `ICorDebug`/`Cordb` instance in universal DBI.
2. It loads the specified universal DAC and creates a virtual
   `ICorDebugProcess` backed by the supplied data target.

Universal DBI passes the data target and contract descriptor address to the
universal DAC. The universal DAC export `DacDbiInterfaceInstance` converts the
COM `ICorDebugDataTarget` into a managed data target and creates a
`ContractDescriptorTarget`.

DBI and DAC data requests flow through `ICorDebugDataTarget` or
`ICorDebugMutableDataTarget`, `CBrowserDebugChannel`, BrowserDebugProxy, and
finally WebAssembly linear memory.

The explicit contract descriptor address is necessary because the browser data
target is virtual and does not discover the descriptor through normal native
module or process APIs.

### Registering the virtual process

Concord wraps the returned `ICorDebug` in `CBrowserCorDebug`.

When Visual Studio calls `DebugActiveProcess`, the wrapper calls
`RegisterVirtualProcessImpl` once. This registers the virtual
`ICorDebugProcess` with the `Cordb` instance returned by
`OpenVirtualProcessImpl4`.

The browser was already running before DBI attached, so there are no natural
startup callbacks for the existing state. Concord synthesizes the initial
callback sequence by enumerating the virtual process and replaying:

- `CreateProcess`;
- `CreateAppDomain`;
- `LoadAssembly`;
- `LoadModule`;
- `CreateThread`.

Continue requests issued while this initial callback sequence is being replayed
are deferred until replay finishes.

During subsequent runtime callbacks, universal DBI balances nested `Stop` requests
as their matching `Continue` requests arrive. Only the final target continue is
deferred until callback dispatch finishes. This keeps symbol-loading stop/continue
scopes balanced without resuming the target inside a callback or bypassing queued
debugger events.

The supplied-channel DBI publishes runtime debug callbacks only after receiving
the ordered `DB_IPCE_SYNC_COMPLETE` marker for their stop batch. For example, both
exception callbacks must be received before either can continue the target. The
transport message IDs associate callbacks with the completed batch; synchronous
request replies remain immediately available on the old-style event channel.
Closing the supplied channel wakes batch and reply consumers. Orderly teardown
uses the existing virtual-process termination signaling; an unexpected closure
is reported through `DebuggerError`, without fabricating an `ExitProcess` callback.

## Browser debug channel protocol

The Concord-to-proxy connection is a serialized request/response protocol over
localhost TCP.

All integer fields are little-endian. Every message starts with a 16-byte
header:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Magic: `0x43444257` |
| 4 | 2 | Protocol version: `1` |
| 6 | 2 | Command |
| 8 | 4 | Request ID |
| 12 | 4 | Payload size |

A response uses the same command with bit `0x8000` set. Payloads are limited to
64 MiB. Concord serializes requests with a critical section because the
connection carries synchronous request/response pairs.

### Commands

| ID | Command | Purpose |
|---:|---|---|
| 1 | `GetTargetInfo` | Returns runtime identity, contract descriptor, pointer size, and platform |
| 2 | `ReadMemory` | Reads a range from WebAssembly linear memory |
| 3 | `WriteMemory` | Writes a range to WebAssembly linear memory |
| 4 | `ContinueProcess` | Resumes the currently recorded browser pause |
| 5 | `StopProcess` | Requests a CDP `Debugger.pause` |
| 6 | `GetPauseState` | Returns the current browser pause epoch, or zero when running |
| 7 | `WriteDebuggerTransport` | Sends opaque DBI transport bytes to CoreCLR |
| 8 | `ReadDebuggerTransport` | Receives opaque DBI transport bytes from CoreCLR |
| 9 | `FuncEvalResultConsumed` | Releases a function-evaluation completion pause and waits for restoration of the original managed pause |

Most responses begin with a 32-bit `HRESULT`. Memory and control responses also
contain counts or pause epochs as appropriate.

### Commands 7 and 8

The command names are from Concord's point of view:

- **Command 7, `WriteDebuggerTransport`, writes bytes from DBI to CoreCLR.**
- **Command 8, `ReadDebuggerTransport`, reads bytes produced by CoreCLR for
  DBI.**

They form the full-duplex `IDebugChannel` used by `DbgTransportSession`.

#### Sending an IPC event from the debugger to CoreCLR

```mermaid
sequenceDiagram
    participant DBI as Universal DBI
    participant C as CBrowserDebugChannel
    participant P as BrowserDebugProxy
    participant JS as JavaScript incoming queue
    participant RT as CoreCLR transport pump
    participant CLR as CoreCLR debugger

    DBI->>C: IDebugChannel::Write(transport bytes)
    C->>P: Command 7 + transport bytes
    P->>JS: enqueueIncoming(Base64(bytes))
    P->>RT: CoreClrWasmDebuggerTransportPump()
    RT->>RT: ProcessIncomingBytes()
    RT->>CLR: HandleIPCEvent(DebuggerIPCEvent)
    CLR-->>RT: S_OK or continue requested
    RT-->>P: pump HRESULT
    P-->>C: HRESULT
    C-->>DBI: Write completed
```

The command 7 payload is exactly the byte array passed to
`CBrowserDebugChannel::Write`. BrowserDebugProxy does not parse its transport
headers or IPC event. It Base64-encodes the bytes only to place them in the page
through `Runtime.evaluate`.

After enqueueing the bytes, BrowserDebugProxy immediately invokes the runtime
pump. The runtime's `DbgTransportSession` can then assemble complete messages
and deliver the contained event to `Debugger::HandleIPCEvent`.

If event handling continues the runtime, the pump returns `S_FALSE`.
BrowserDebugProxy uses that return value to resume the corresponding physical
CDP pause.

Command 7 does not pass a `DebuggerIPCEvent` directly to the debugger. It may
deliver any fragment of the transport byte stream. The runtime performs framing,
unmarshalling, queueing, and dispatch inside CoreCLR. `ProcessIncomingBytes`
accumulates partial messages. Complete `MT_Event` messages are unmarshalled by
`QueueEvent` into a locked circular event buffer. The WASM pump calls
`HasPendingEvent`, removes each event with `GetNextEvent`, and passes it to
`HandleIPCEventWrapper`.

If the dequeued event is a `DB_IPCE_CONTINUE` received while the runtime is
already running, the pump resets the debugger's running state and returns
`S_FALSE`. Otherwise it acquires the debugger lock when required and processes
the event normally. Returning `S_FALSE` tells BrowserDebugProxy that the
physical browser pause must also be resumed.

#### Receiving an IPC event after a user break

```mermaid
sequenceDiagram
    participant CLR as CoreCLR debugger
    participant RT as CoreCLR DbgTransportSession
    participant JS as JavaScript outgoing queue
    participant P as BrowserDebugProxy
    participant C as CBrowserDebugChannel
    participant DBI as Universal DBI
    participant VS as Concord / Visual Studio

    CLR->>CLR: Managed Debugger.Break / breakpoint
    CLR->>RT: Send breakpoint DebuggerIPCEvent
    RT->>JS: WasmDebuggerChannel::Write(bytes)
    JS->>JS: enqueueOutgoing(Base64(bytes))
    CLR->>P: emscripten_debugger / CDP Debugger.paused
    P->>P: Record browser pause epoch
    DBI->>C: IDebugChannel::Read(required count)
    C->>P: Command 8 + requested count
    P->>JS: dequeueOutgoing(requested count)
    JS-->>P: Base64(bytes), possibly empty
    P-->>C: HRESULT + byte count + bytes
    alt bytes are available
        C-->>DBI: copy bytes into DBI buffer
    else queue is temporarily empty
        C->>C: wait briefly and retry command 8
    end
    DBI->>DBI: decode transport framing
    DBI->>VS: Dispatch ICorDebug breakpoint callback
    VS->>VS: Publish stopped managed state
```

The command 8 request payload is a 32-bit requested byte count:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Maximum bytes requested |

Its response is:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `HRESULT` |
| 4 | 4 | Bytes returned |
| 8 | N | Opaque CoreCLR debugger transport bytes |

An empty successful response means that CoreCLR has not produced more bytes
yet. It is not end-of-stream. Concord waits briefly and sends another command 8
until it has filled the exact read requested by DBI.

The outgoing queue can contain part of one transport message, exactly one
message, or bytes from multiple messages. Message boundaries are reconstructed
only by DBI's `DbgTransportSession`.

## Reading target memory

The universal DAC and DBI call `CBrowserDebugChannel::ReadVirtual` through
`ICorDebugDataTarget`.

```mermaid
sequenceDiagram
    participant DAC as Universal DAC / DBI
    participant C as CBrowserDebugChannel
    participant P as BrowserDebugProxy
    participant CDP as CDP Runtime.evaluate
    participant MEM as WebAssembly.Memory

    DAC->>C: ReadVirtual(address, count)
    C->>P: Command 2 + address + count
    P->>P: Wait for paused target and validate range
    P->>CDP: Evaluate Uint8Array read
    CDP->>MEM: Read linear-memory bytes
    MEM-->>CDP: Bytes
    CDP-->>P: Base64(bytes)
    P-->>C: HRESULT + count + bytes
    C-->>DAC: Copy bytes and return
```

The request payload is:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | Target address |
| 8 | 4 | Requested byte count |

BrowserDebugProxy:

1. waits for the target to be paused;
2. validates that the requested range is within the current WebAssembly memory;
3. refreshes the memory length if memory may have grown;
4. evaluates JavaScript in the page to create a `Uint8Array` over the requested
   range;
5. converts the bytes to Base64 for transport through CDP;
6. decodes the result in BrowserDebugProxy and returns the raw bytes to
   Concord.

The response contains:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `HRESULT` |
| 4 | 4 | Bytes read |
| 8 | N | Data |

Concord requires a complete read. A short result is reported as
`ERROR_PARTIAL_COPY`.

The memory address is a WebAssembly linear-memory offset. BrowserDebugProxy uses
the runtime's active `WebAssembly.Memory` object, not an operating-system
`ReadProcessMemory` operation.

## Writing target memory

The universal DAC and DBI call `CBrowserDebugChannel::WriteVirtual` through
`ICorDebugMutableDataTarget`.

```mermaid
sequenceDiagram
    participant DAC as Universal DAC / DBI
    participant C as CBrowserDebugChannel
    participant P as BrowserDebugProxy
    participant CDP as CDP Runtime.evaluate
    participant MEM as WebAssembly.Memory

    DAC->>C: WriteVirtual(address, bytes)
    C->>P: Command 3 + address + count + bytes
    P->>P: Wait for paused target and validate range
    P->>CDP: Evaluate Uint8Array write
    CDP->>MEM: Copy bytes into linear memory
    MEM-->>CDP: Number of bytes written
    CDP-->>P: Written count
    P-->>C: HRESULT + written count
    C-->>DAC: Return success or partial-copy failure
```

The request payload is:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | Target address |
| 8 | 4 | Byte count |
| 12 | N | Data |

BrowserDebugProxy validates the range, Base64-encodes the bytes for a CDP
evaluation, and runs JavaScript that copies them into a `Uint8Array` over
WebAssembly linear memory.

The response contains an `HRESULT` and the number of bytes written. Concord
requires the returned count to equal the requested count.

Memory operations are serialized in BrowserDebugProxy by a memory gate. They are
performed only while the target is paused so that DAC and DBI observe a stable
managed process state.

## CoreCLR debugger transport in the browser

The regular CoreCLR debugger protocol assumes a byte-oriented `IDebugChannel`.
The browser implementation preserves that abstraction on both sides:

- **DBI side:** `CBrowserDebugChannel` implements `IDebugChannel`.
- **Runtime side:** `WasmDebuggerChannel` implements `IDebugChannel`.

Each side owns a `DbgTransportSession`. The browser channel and JavaScript queues
replace the operating-system socket or pipe normally used between them.

### JavaScript transport queues

The runtime loader creates:

```js
globalThis.__coreclrWasmDebuggerTransport
```

It contains two byte queues:

- **incoming:** DBI to CoreCLR;
- **outgoing:** CoreCLR to DBI.

The object exposes:

- `enqueueIncoming`;
- `dequeueIncoming`;
- `incomingByteCount`;
- `enqueueOutgoing`;
- `dequeueOutgoing`;
- `outgoingByteCount`;
- `pump`.

Queue API payloads are Base64 strings because CDP evaluations return
JavaScript-compatible values. The bytes inside the queues remain the original
opaque debugger transport stream.

`pump` calls the exported WebAssembly function
`CoreClrWasmDebuggerTransportPump`.

## Sending IPC from DBI to CoreCLR

Detailed flow:

1. Universal DBI serializes a normal debugger transport message.
2. DBI writes the bytes through `IDebugChannel::Write`.
3. Concord sends command 7 with those bytes as its payload.
4. BrowserDebugProxy Base64-encodes the payload and evaluates JavaScript that
   appends it to the incoming queue.
5. BrowserDebugProxy invokes `pump`.
6. `CoreClrWasmDebuggerTransportPump` removes available bytes from the incoming
   queue and passes them to `DbgTransportSession::ProcessIncomingBytes`.
7. `DbgTransportSession` performs its normal framing and queues complete
   debugger events.
8. The pump dequeues pending events and calls the normal CoreCLR
   `HandleIPCEvent` path.

The browser is single-threaded, so this processing is cooperative rather than
being performed by a dedicated debugger transport thread. When the pump
re-enters CoreCLR on a managed thread, it temporarily enters cooperative GC mode
before handling the debugger event.

If handling an event continues the runtime, the pump returns `S_FALSE`.
BrowserDebugProxy uses that result to coordinate the corresponding CDP
`Debugger.resume`.

## Sending IPC from CoreCLR to DBI

Detailed flow:

1. CoreCLR fills the single-threaded `DebuggerIPCEvent` buffer.
2. `SendSingleThreadedDebuggerIPCEvent` sends it through the runtime
   `DbgTransportSession`.
3. `WasmDebuggerChannel::Write` copies the serialized transport bytes to the
   JavaScript outgoing queue.
4. Universal DBI eventually calls `CBrowserDebugChannel::Read` for an exact
   number of bytes.
5. Concord sends command 8 with the requested byte count.
6. BrowserDebugProxy evaluates JavaScript that removes up to that count from the
   outgoing queue.
7. BrowserDebugProxy returns the bytes to Concord.
8. Concord continues polling when zero bytes are currently available and
   returns only after satisfying DBI's requested read size.
9. Universal DBI decodes the normal transport framing and dispatches the
   corresponding `ICorDebugManagedCallback`.

`DB_IPCE_SYNC_COMPLETE` uses the old-style event path. Other debugger events are
sent as debug events, matching the existing `DbgTransportSession` distinction.

## Browser pauses and managed debugger events

A browser pause and a managed debugger stop are related but not identical:

- CDP owns whether JavaScript/WebAssembly execution is physically paused.
- CoreCLR and DBI own managed debugger state and `DebuggerIPCEvent` delivery.
- BrowserDebugProxy records the relationship between the two.

BrowserDebugProxy assigns a monotonically increasing **pause epoch** to every
accepted `Debugger.paused` event. Concord can query the current epoch with
`GetPauseState`.

The epoch prevents a continue intended for one stop from accidentally resuming
a later stop.

### Managed breakpoint flow

A typical managed breakpoint or `Debugger.Break` flow is:

1. CoreCLR creates and sends the managed breakpoint IPC event.
2. CoreCLR enters `emscripten_debugger()`, causing CDP to report
   `Debugger.paused`.
3. BrowserDebugProxy records the browser session and increments the pause epoch.
4. Universal DBI reads the event through command 8.
5. DBI dispatches the managed callback through Concord.
6. Visual Studio displays the stopped managed state.
7. When the debugger continues, Concord first records whether a physical
   browser pause exists.
8. After DBI completes its continue transition, Concord sends command 4.
9. BrowserDebugProxy sends CDP `Debugger.resume` for the recorded pause epoch.

The managed event is the authoritative source of the managed callback.
Concord does not synthesize a second user-break callback merely because CDP
reported a browser pause.

### Explicit stop

For an explicit debugger stop, command 5 increments a nested stop count and
requests CDP `Debugger.pause` if the browser is not already paused. Once the
pause is observed, Concord dispatches the corresponding browser async-break
callback.

Nested stop/continue operations retain the physical pause until the stop count
returns to zero.

## Function evaluation

Function evaluation requires additional pause coordination because a single
evaluation temporarily changes the meaning of the browser pause.

The implemented sequence is:

1. The original managed stop leaves CoreCLR with a saved managed context.
2. DBI sends the function-evaluation setup IPC request.
3. CoreCLR queues the evaluation and installs the saved context while processing
   pending evaluations.
4. Concord continues the process.
5. BrowserDebugProxy resumes the original CDP pause so managed evaluation code
   can run.
6. CoreCLR executes the evaluation and sends `DB_IPCE_FUNC_EVAL_COMPLETE`.
7. CoreCLR pauses in the browser again.
8. DBI receives the completion event and VIL retrieves the result.
9. `CordbEval::GetResult` notifies the private process-control interface that
   the result was consumed.
10. Concord sends command 9, `FuncEvalResultConsumed`.
11. BrowserDebugProxy resumes the function-evaluation completion pause.
12. Before the saved managed context holder is destroyed, CoreCLR creates a new
    runtime-owned browser pause.
13. BrowserDebugProxy waits for this later pause epoch before completing command
    9.

The final pause restores the original managed inspection state at a known safe
point. BrowserDebugProxy must not replace this with an arbitrary
`Debugger.pause`, because an arbitrary instruction may be unsafe for the next
function-evaluation setup.

## Process-control interface

`CBrowserDebugChannel` also implements a private `ICorDebugProcessControl`
interface used by universal DBI:

- `ContinueProcess`;
- `ContinueCompleted`;
- `StopProcess`;
- `FuncEvalResultConsumed`.

This interface connects DBI state transitions to physical browser pause
transitions without making BrowserDebugProxy understand DBI internals.

## Current platform assumptions and limitations

- The target is currently WASM32, with a four-byte target pointer size.
- WebAssembly addresses are represented as 64-bit protocol fields but must
  resolve to valid offsets in the current linear memory.
- Memory may grow, so BrowserDebugProxy refreshes the memory length when a range
  falls outside the cached size.
- `GetThreadContext`, `SetThreadContext`, and `ContinueStatusChanged` are not
  implemented by the browser data target. The debugger uses interpreter-managed
  contexts for the supported paths.
- The browser runtime has no normal debugger helper thread. IPC handling is
  cooperatively pumped on the browser thread.
- The localhost browser debug channel currently has one in-flight serialized
  request at a time.
- Empty debugger transport reads are normal. Concord waits briefly and retries
  until it has returned the exact number of bytes requested by DBI.
- DAC and DBI binaries must match the contract and interfaces expected by the
  runtime and Concord deployment.

## Reproducing the runtime build

This section covers runtime source and artifacts only. Cross-repository debugger
deployment, browser launch, application entry selection, and measurement runners
belong to their respective repositories. Do not copy their implementation code or
host-specific configuration into this runtime document.

### Source, prerequisites, and host selection

The tested prototype starts from
[`a2016b8794108a3650876bfd91a7a2fd31054d2d`](https://github.com/thaystg/runtime/commit/a2016b8794108a3650876bfd91a7a2fd31054d2d)
on `thaystg/runtime`, branch `dev/thays/wasm_coreclr_debugger_cdac`, plus the
runtime corrections below. Fetch that fork explicitly; neither the current
`dotnet/runtime` default branch nor a moving prototype branch tip identifies this
source snapshot. A replay patch is local source provenance, not an additional
remote commit. Apply it to the exact base before building. The optional sample
`Debugger.Break()` patch is a workload entry, not an engine prerequisite.

Follow the [platform requirements](/docs/workflow/README.md#introduction) and
[CoreCLR build guide](/docs/workflow/building/coreclr/README.md). The runtime
checkout's `global.json` pins SDK `11.0.100-rc.1.26420.103` and Arcade/shared
framework SDK `12.0.0-beta.26471.108`. Use `dotnet.sh` or `dotnet.cmd`, not an
arbitrary globally selected SDK. The package branding is `12.0.0-dev`, while
this source builds `net11.0` framework assets; the package version is not the
target framework version. Emscripten `6.0.2` is pinned by
`src/mono/browser/emscripten-version.txt` and provisioned by the browser subset.
Native CMake/compiler/linker tools and the NativeAOT prerequisites are needed
for the host universal DAC. Restore uses the public package sources in
`NuGet.config`, including the .NET engineering/tools and .NET 11/12 feeds; do
not add credentials or personal feed configuration to a source snapshot.

The successful macOS arm64 environment used CMake 3.29.0, Ninja 1.13.2, and
Apple Clang 21.0.0. These are observed versions, not new minimum requirements.
SDK provisioning and tool feeds remain external dependencies; a source patch is
not an offline toolchain bundle.

The browser target is always `browser`/`wasm`. Build the diagnostic libraries
separately for the OS and architecture of the native debugger process, for
example `osx`/`arm64`, not for `browser`/`wasm`. Keep source, configuration,
package versions, SDK tasks, and protocol expectations consistent across the
two builds.

| Native debugger host | Artifact names | Verification status |
|---|---|---|
| macOS arm64 | `libmscordbi_universal.dylib`, `libmscordaccore_universal.dylib` | Built, loaded, exports resolved, and real browser attach/stack/Locals/normal stepping exercised |
| macOS x64 | Same `.dylib` names | Repository build parameterization exists; this experiment did not build or validate the x64 deployment |
| Windows | `mscordbi_universal.dll`, `mscordaccore_universal.dll` | Windows build/export branches exist in source; no Windows end-to-end validation in this experiment |
| Linux | `libmscordbi_universal.so`, `libmscordaccore_universal.so` | Unix build/export branches exist in source; no Linux end-to-end validation in this experiment |

On Unix, select `HOST_OS` and `HOST_ARCH` for the native debugger host and build
from the runtime checkout root. These exact values were exercised on macOS:

```bash
HOST_OS=osx
HOST_ARCH=arm64
export DOTNET_WASM_TOOL_CACHE_DIR="$PWD/.dotnet/wasm-tools"
./build.sh clr+libs -os browser -a wasm -c Debug
./build.sh clr.runtime+tools.cdac -os "$HOST_OS" -a "$HOST_ARCH" -c Debug
```

Use `linux` and its supported native architecture only as a source-supported,
not experimentally validated, alternative. The repository's Windows entrypoint
accepts the corresponding subset/OS/architecture parameters:

```powershell
$HostArch = "x64"
$env:DOTNET_WASM_TOOL_CACHE_DIR = Join-Path $PWD ".dotnet/wasm-tools"
.\build.cmd clr+libs -os browser -a wasm -c Debug
.\build.cmd clr.runtime+tools.cdac -os windows -a $HostArch -c Debug
```

These Windows commands are entrypoint-supported equivalents, not recorded
successful builds. Follow the Windows toolchain requirements before using them.
The explicit WASM tool cache override avoids the default shared main-checkout
cache when working in an isolated checkout.

Host-native installed libraries are under
`artifacts/bin/coreclr/<host-os>.<host-arch>.Debug/`. The universal DBI exports
`OpenVirtualProcessImpl4` and `RegisterVirtualProcessImpl`; the DAC exports
`DacDbiInterfaceInstance`, `CLRDataCreateInstance`, and its cDAC initialization
entrypoints. Verify native loading and required exports on the deployment host
before attaching to a browser. On macOS, matching `.dylib.dwarf` files may be
kept beside the libraries; they are symbols, not additional runtime libraries.

### Incremental native rebuilds and interpreted packages

Only after a successful baseline has produced the native object trees, the
following targets were used for the two-field target-descriptor correction:

```bash
cmake --build artifacts/obj/coreclr/browser.wasm.Debug --target coreclr_static --parallel 6
cmake --install artifacts/obj/coreclr/browser.wasm.Debug --component runtime
cmake --build artifacts/obj/browser-wasm.Debug --target browserhost --parallel 6
cmake --install artifacts/obj/browser-wasm.Debug --component runtime
```

A host-only DBI correction can be rebuilt independently:

```bash
cmake --build "artifacts/obj/coreclr/$HOST_OS.$HOST_ARCH.Debug" --target mscordbi_universal --parallel 6
```

The resulting DBI and its symbols are in that object tree's
`dlls/mscordbi_universal/` directory. A targeted build is not an installation:
stage those exact outputs explicitly, rather than assuming a previous installed
copy was updated. CMake target names and object-tree paths are from this
prototype; do not treat them as a replacement for the baseline scripts.

The validated debugger profile is interpreted, untrimmed Debug with plain DLLs:
`PublishReadyToRun=false`, `RunAOTCompilation=false`, `WasmEnableWebcil=false`,
and `WasmPreloadAssets=false`. This is not a claim of mixed/R2R or default
application parity. Create the interpreted shipping runtime package explicitly:

```bash
./dotnet.sh build src/installer/pkg/sfx/Microsoft.NETCore.App/Microsoft.NETCore.App.Runtime.CoreCLR.sfxproj \
  -c Debug -p:TargetOS=browser -p:TargetArchitecture=wasm \
  -p:RuntimeConfiguration=Debug -p:LibrariesConfiguration=Debug \
  -p:RuntimeFlavor=CoreCLR -p:PublishReadyToRun=false
```

The runtime and matching `Microsoft.NET.Sdk.WebAssembly.Pack.12.0.0-dev.nupkg`
are under `artifacts/packages/Debug/Shipping/`. The direct SFX project defaults
to framework R2R packaging; its `PublishReadyToRun=false` override is necessary
for this plain-DLL debugger cohort. Otherwise the shipping `RuntimeList.xml`
can declare raw framework `native/r2r/*.wasm` assets that this profile's boot
generation rejects.

Do not confuse the shipping package with the existing in-tree test layout at
`artifacts/bin/microsoft.netcore.app.runtime.browser-wasm/Debug/`. A successful
SFX build does not establish that every file and manifest in that directory was
refreshed. Extract the package's complete `data/` and `runtimes/` trees to a
separate clean consumer layout, and retain matching symbols from the same build
if needed. Do not overlay a shipping layout onto the library-output directory
used as input to another SFX build: a relocated duplicate CoreLib can cause
duplicate package entries.

Publish with the matching SDK pack/tasks, runtime pack, and targeting pack.
Use a fresh isolated package cache when replacing a locally built package with
the same version; otherwise NuGet may reuse the prior extraction. Preserve the
previous complete pack and application publication before restaging. Do not
clear a shared user cache or silently combine differently built same-version
packages.

The existing runtime-owned sample is
`src/mono/sample/wasm/browser/Wasm.Browser.Sample.csproj`. Its checked source
defaults to the non-R2R profile. The following successful in-tree publication
also disabled trimming, retained portable PDBs, and disabled fingerprinting
only for this reference sample:

```bash
./dotnet.sh msbuild src/mono/sample/wasm/browser/Wasm.Browser.Sample.csproj \
  -t:BuildSampleInTree -p:Configuration=Debug -p:RuntimeFlavor=CoreCLR \
  -p:PublishTrimmed=false -p:WasmEnableWebcil=false -p:WasmPreloadAssets=false \
  '-p:BuildAdditionalArgs=-p:PublishTrimmed=false -p:DebugType=portable -p:CopyOutputSymbolsToPublishDirectory=true -p:WasmEnableWebcil=false -p:WasmFingerprintAssets=false -p:WasmPreloadAssets=false -p:WasmBuildOnlyAfterPublish=true'
```

`BuildSampleInTree` clears the sample's output before its nested publish.
Afterward stage the selected host-native DBI/DAC beside the framework assets
for the consuming debugger's deployment contract; do not accidentally recopy
an older host DBI from a previous publication. Other applications can retain
fingerprinting, but must regenerate boot metadata and integrity for their own
complete publication. Never replace only `dotnet.native.wasm` inside an old
boot/fingerprinted cohort. Check each declared asset hash against its actual
file; JS-module entries can be name-only.

### Required corrections, invariants, and validation

The replay source includes the Unix private-interface IID, balanced nested
shimless continues, supplied-channel `SYNC_COMPLETE` batch publication and
closure handling, target-width heap-reference snapshots, and the two delegate
descriptor annotations. Closure handling preserves genuine error reporting
and does not synthesize process-exit events. The optional sample break is
separate from these engine requirements.

`Delegate.MethodPtr` and `MethodPtrAux` are VM `PCODE` fields and must be
described as `CodePointer`, not a data `pointer`. The production cDAC typed
reader and contract already require this. Correct the target descriptor and
typed mocks; do not weaken the host reader. On WASM32 the embedded descriptor
has `Delegate` size 24, method fields at offsets 12 and 16, and `CodePointer`
size 4. The correction changes type annotations, not field layout.

The host-reference invariant is independent: a cached heap reference passed
to `GetObjectAddress` has exactly the target's pointer width, despite a
64-bit native debugger. `CORDB_ADDRESS` is zero-extended; unrepresentable
32-bit addresses are errors, not values to truncate. Keep the strict consumer
assertion and local typed-storage lifetime through synchronous initialization.

The focused managed regression command was:

```bash
./dotnet.sh test src/native/managed/cdac/tests/UnitTests/Microsoft.Diagnostics.DataContractReader.Tests.csproj \
  -c Debug --filter 'FullyQualifiedName~ObjectTests'
```

All 60 Object tests passed, including existing closed/open delegate theories
on 32/64-bit little/big-endian mocks with explicit code-pointer metadata.
The original pointer metadata failed eight focused cases. Host-native builds
also compile 13 assertions exercising the actual heap-reference serialization
helper, plus 27 nested-continue/batch/closure policy assertions. The original
host-sized serialization policy failed nine of its 13 cases. An auxiliary
native check exercised 15 actual helper/`MemoryRange` byte-view cases under
ASan/UBSan on arm64; it is source-derived validation tooling, not a runtime
build dependency.

Historical accepted macOS Debug artifacts had SHA-256 digests
`1929c85160b4d4ed25c7c0e294a811e8b5e681d0865aa7baa255bc96e7c37109`
(browser WASM), `3e9088884a3a8d1eb1502995362537fd3651467f0ab44e6b2f1363715dd25bdb`
(host DBI), and `f437b00eb548b36032500a4373cfd7fe05210afd8bd4a55ee25c7c29dcc1afe6`
(host DAC). Earlier host DBI cohorts predated the heap-reference fix. These
digests identify local experiment outputs, not downloadable remote artifacts
or deterministic rebuild requirements: build paths, tool versions, and debug
information can change binary hashes. Record fresh source/patch and artifact
digests for each new complete deployment.

Real macOS arm64 reference-sample stepping and a synchronous application
workload exercised automatic stack/Locals/source UI and successive normal
steps with default JMC. The filtering workload's later step after implicit
UI evaluation remains unresolved and deferred; it is not established as an
async-await limitation. Windows/Linux/x64 deployment and comprehensive
evaluation are unvalidated. Native close-path `E_FAIL`/broken-pipe reporting
and bounded helper termination remain separate limitations; this source does
not establish complete graceful teardown.

## Main implementation files

### dotnet/runtime

- `src/coreclr/debug/di/process.cpp`
  - `OpenVirtualProcessImpl4`;
  - `RegisterVirtualProcessImpl`;
  - browser process-control integration.
- `src/coreclr/debug/shared/dbgtransportsession.cpp`
  - CoreCLR debugger transport framing and event queues.
- `src/coreclr/debug/ee/wks/wasm/wasmdebugger.cpp`
  - runtime `IDebugChannel`;
  - cooperative transport pump;
  - browser pause support.
- `src/native/libs/Common/JavaScript/loader/run.ts`
  - `globalThis.__coreclrWasmDebuggerTransport`;
  - incoming and outgoing JavaScript byte queues.
- `src/native/managed/cdac/mscordaccore_universal/Entrypoints.cs`
  - universal DAC exports;
  - creation of `ContractDescriptorTarget` from
    `ICorDebugDataTarget`.

### dotnet-diagnostics-internal-components

- `src/WasmDebugger/Microsoft.Diagnostics.BrowserDebugProxy/CoreClrProxy.cs`
  - browser debug channel server peer;
  - CDP pause/resume coordination;
  - memory access;
  - debugger transport queue bridge.

### Concord

- `src/impl/ManagedDM/V2/DbgShimLiveProcLoader.cpp`
  - browser attach branch;
  - universal DAC/DBI loading;
  - virtual-process creation.
- `src/impl/ManagedDM/V2/BrowserDebugChannel.h`
  - browser protocol definitions;
  - `IDebugChannel`, data-target, and process-control interfaces.
- `src/impl/ManagedDM/V2/BrowserDebugChannel.cpp`
  - protocol implementation;
  - memory and debugger transport operations;
  - `CBrowserCorDebug` and initial callback replay.
