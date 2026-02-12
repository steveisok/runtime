# Plan: Add regression test for SampleProfiler SampleType fix (PR #124019)

## Problem

PR [dotnet/runtime#124019](https://github.com/dotnet/runtime/pull/124019) fixed a bug where the `SampleType` field in the `ThreadSample` event from the EventPipe SampleProfiler always reported `EXTERNAL (1)` even when threads were executing managed code. The correct value should be `MANAGED (2)`. There is currently no test to prevent this from regressing again.

## Approach

Add a new EventPipe test under `src/tests/tracing/eventpipe/eventsvalidation/` that:

1. Enables the `Microsoft-DotNETCore-SampleProfiler` EventPipe provider
2. Runs a CPU-intensive managed code loop during sampling (ensuring the main thread spends time in cooperative/managed mode)
3. Collects events and inspects the raw payload of events from the SampleProfiler provider
4. Validates that at least one event reports `SampleType == MANAGED (2)`, not all `EXTERNAL (1)`

The test follows the existing `IpcTraceTest.RunAndValidateEventCounts` pattern used by `GCEvents.cs`, `ExceptionThrown_V1.cs`, and `providervalidation.cs`.

### Key technical details

- The SampleProfiler event (eventID 0) has a single `uint32_t` payload field (SampleType) with no metadata schema, so raw `EventData()` bytes must be read
- SampleType values: `ERROR = 0`, `EXTERNAL = 1`, `MANAGED = 2`
- The provider name is `Microsoft-DotNETCore-SampleProfiler`
- The event level is `Informational`

## Files to create

1. **`SampleProfilerSampleType.cs`** — Test that validates SampleType field values
2. **`SampleProfilerSampleType.csproj`** — Project file (matches pattern of `GCEvents.csproj`)

## Workplan

- [ ] Create `SampleProfilerSampleType.csproj` following the `GCEvents.csproj` pattern
- [ ] Create `SampleProfilerSampleType.cs` test that:
  - Enables the SampleProfiler provider at Verbose level
  - Runs a managed computation loop as the event-generating action
  - Uses `_optionalTraceValidator` to inspect events from `Microsoft-DotNETCore-SampleProfiler`
  - Reads the raw 4-byte payload to extract the SampleType enum value
  - Asserts at least one event has `SampleType == 2` (MANAGED)
- [ ] Run baseline build (`./build.sh clr+libs -rc release`)
- [ ] Build the test project with `dotnet build`
- [ ] **Validate test catches the bug:** Revert the PR #124019 fix in `ep-rt-coreclr.cpp` (swap `HasThreadState(TS_SuspensionTrapped)` back to the broken logic) and confirm the test **fails**
- [ ] **Validate test passes with fix:** Re-apply the PR #124019 fix and confirm the test **passes**
- [ ] Code review the changes

## Notes

- The test should be marked with the same stress-incompatible attributes as other tracing tests
- The event-generating action needs to run long enough for the SampleProfiler (1ms default sampling rate) to capture multiple samples of managed code execution
- Since this is a CoreCLR-specific fix (uses `TS_SuspensionTrapped` thread state in `threadsuspend.cpp`), the test is only relevant on CoreCLR, not Mono
- This test is inherently probabilistic — it verifies that *at least one* managed sample is captured, not an exact count
