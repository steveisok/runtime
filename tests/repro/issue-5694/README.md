# Repro: dotnet-trace collect-linux cross-container failure

Reproduces [dotnet/diagnostics#5694](https://github.com/dotnet/diagnostics/issues/5694).

## Prerequisites

- Docker and Docker Compose (Docker Desktop on macOS/Windows, or native Docker on Linux/WSL)
- Host kernel with tracefs support (`/sys/kernel/tracing` mounted)

## Steps

```bash
# 1. Start both containers
docker compose up -d --build

# 2. Get the host-namespace PID of the target .NET process
docker inspect --format '{{.State.Pid}}' target-app

# 3. Attach to the tracer container
docker exec -it tracer bash

# 4. Install dotnet-trace (inside tracer container)
dotnet tool install -g dotnet-trace
export PATH="$PATH:/root/.dotnet/tools"

# 5. Reproduce the bug — replace <PID> with the value from step 2
dotnet-trace collect-linux -p <PID>
# Expected: ServerNotAvailableException
```

## Expected result (bug)

```
[ERROR] Microsoft.Diagnostics.NETCore.Client.ServerNotAvailableException:
Unable to connect to Process <PID>...
```

## Expected result (after fix)

`collect-linux` should either proceed with tracing or give a clear,
non-exception error message.

## Cleanup

```bash
docker compose down
```
