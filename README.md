# IPC Client-Server System

A Linux client-server system using POSIX shared memory for inter-process
communication. Implements math and string operations via blocking and
non-blocking IPC calls.

## Design Choices

### Language Strategy

- **C++17** for internal implementation (RAII, `std::thread`, `std::mutex`,
  `std::condition_variable`, `std::queue`).
- **C ABI** (`extern "C"`) at the library boundary so `libipc.so` can be loaded
  via `dlopen`/`dlsym` without name mangling issues.
- **POD types only** in shared memory (`include/ipc_defs.h`).

### IPC Mechanism -- Slot-Based Shared Memory

Shared memory (`/dev/shm/ipc_shm`) contains a fixed array of 16 message slots.
Each slot holds one in-flight request and its corresponding response, transitioning
through states: `FREE -> REQUEST_PENDING -> PROCESSING -> RESPONSE_READY -> FREE`.

This eliminates the need for request/response correlation (no scanning or
ring buffer complexity). Blocking calls wait on a per-slot semaphore; non-blocking
calls return immediately and poll the slot state via `ipc_get_result()`.

To keep naming and validation logic consistent across components, shared helpers
live in `include/ipc_defs.h`:
- `ipc_slot_sem_name(...)` builds slot semaphore names from `IPC_SLOT_SEM_PREFIX`.
- `ipc_validate_string(...)` validates the 1..16 string input constraint.

### Synchronization Strategy

All inter-process synchronization uses **named POSIX semaphores**:

| Semaphore | Purpose |
|---|---|
| `/ipc_mutex` (binary, init=1) | Protects all shared memory access |
| `/ipc_server_notify` (counting, init=0) | Wakes server when a request is posted |
| `/ipc_slot_0` .. `/ipc_slot_15` (binary, init=0) | Per-slot wake-up for blocking calls |

**Locking rules:**
- All shared memory access requires holding `/ipc_mutex`.
- The mutex is held for minimal duration (find slot, read/write, release).
- Never hold the mutex while waiting on another semaphore (deadlock prevention).

### Threading

The server uses `std::thread` which is the C++17 standard wrapper over POSIX
pthreads on Linux, linked explicitly via `-lpthread` (`Threads::Threads` in CMake).

Two separate thread pools (math and string) dispatch work from the main
dispatcher thread. The number of worker threads per pool is **auto-detected**
at startup based on the CPU core count: `(cores - 1) / 2`, reserving one core
for the dispatcher. This can be overridden with the `-t N` command-line flag
(see the ``Running`` section).

### Non-Blocking Demonstration

Multiply and divide operations include an artificial server-side delay of
**2 milliseconds** to keep test runs fast. This still preserves non-blocking
semantics (request returns immediately, result becomes available later).

If you want a visually obvious demo in the interactive clients, temporarily
increase the delay in `src/server.cpp` (inside `process_math`) from
`std::chrono::milliseconds(2)` to a larger value such as
`std::chrono::seconds(10)`, rebuild, and rerun. With a larger delay, concat/search
results will visibly complete before multiply/divide results.

## Build

### Prerequisites

- Linux (tested on Manjaro and Debian13)
- GCC/G++ with C++17 support
- CMake >= 3.14
- Python 3.10 or later + pytest (for tests)
- Doxygen + Sphinx + Breathe (for documentation, optional)

### Build Commands

A wrapper `Makefile` provides shortcuts for the most common commands:

```bash
make              # full pipeline: build + test + docs
make build        # compile only
make all          # full pipeline: build + test + docs
make full         # alias for all
make debug        # Debug build (-g -O0, for GDB)
make release      # Release build (-O3, for production)
make sanitize     # Debug + AddressSanitizer + UBSan
make reldbg       # RelWithDebInfo (-O2 -g, for profiling)
make clean        # remove build artifacts
make clean_all    # remove build + docs + test caches + venv
make rebuild      # clean + rebuild
make rebuild_all  # clean_all + build + test + docs
make test         # run pytest integration tests
make docs         # generate Sphinx + Doxygen documentation
make doxygen      # generate Doxygen documentation only
make cppcheck     # run cppcheck static analysis
make cppcheck-deep # run exhaustive cppcheck analysis (slower)
make venv         # create .venv and install Python dependencies
make deps         # print required/optional dependencies
make help         # show all available targets
```

`make` without arguments runs the default full pipeline and is equivalent to
`make all`.

### Dependency Guide

The `make deps` target prints the authoritative dependency list:

```bash
make deps
```

Expected categories:
- Required (build + run + tests):
  - C++ compiler with C++17 support (`g++`)
  - CMake >= 3.14
  - POSIX runtime libs (`pthread`, `rt`) (usually provided by libc/dev toolchain)
  - Python 3.10+ (tested on 3.10-3.14)
  - `python3-venv`
  - pip package: `pytest` (installed by `make test`)
- Optional (documentation):
  - `doxygen`
  - `graphviz` (`dot`)
  - `sphinx-build`
  - pip packages: `sphinx`, `breathe`, `myst-parser` (installed by `make docs` into `.venv`)
- Optional (static analysis):
  - `cppcheck`

Debian/Ubuntu quick install:

```bash
sudo apt update && sudo apt install -y build-essential cmake python3 python3-venv
sudo apt install -y doxygen graphviz sphinx-doc cppcheck
```

If you prefer not to use the Makefile wrapper, run CMake directly:

```bash
cmake -B build
cmake --build build
cmake --build build --clean-first   # clean rebuild
```

### Build with Sanitizers

```bash
make sanitize
# Or directly:
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
cmake --build build
```

### Build Outputs

All outputs are placed in `build/`:
- `build/libipc.so` -- shared communication library
- `build/server` -- server executable
- `build/client1` -- client 1 (direct link)
- `build/client2` -- client 2 (dlopen/dlsym)

## Running

### Start the Server

```bash
cd build
./server                          # auto-detect threads, drain on shutdown
./server -t 4                     # force 4 threads per pool
./server -t 1                     # single-threaded pools (for GDB debugging)
./server --shutdown=drain         # finish queued tasks before exit (default)
./server --shutdown=immediate     # discard pending tasks, exit fast
./server -t 2 --shutdown=immediate  # combine flags
```

The server creates shared memory and semaphores, then waits for requests.
The startup banner shows the detected core count, threads-per-pool, and
shutdown mode. Press Ctrl+C (SIGINT) or send SIGTERM for shutdown.

**Restart recovery model:** The shared memory contains a `server_generation`
counter that changes on every server startup. Clients detect generation
changes and automatically reconnect their shared-memory/semaphore handles.
If a restart invalidates an in-flight request, library calls return
`IPC_ERR_SERVER_RESTARTED`.

**Shutdown modes:**
- `drain` (default) -- all queued tasks finish before the server exits. On
  shutdown the server reports how many tasks remain.
- `immediate` -- pending queue is discarded; only tasks already being processed
  are allowed to complete. Useful for fast restarts.

**Status report:** Send `SIGUSR1` to get a live status report without stopping
the server:

```bash
kill -USR1 $(pidof server)
```

**Duplicate instance protection:** Only one server can run at a time.
Attempting to start a second instance prints an error and exits immediately.
The protection uses an advisory file lock (`/tmp/ipc_server.lock`) via
`flock()`, which the kernel releases automatically if the server crashes.

### Run Client 1 (add, multiply, concat)

```bash
cd build
./client1
```

Client 1 behavior on restart detection:
- Blocking calls fail fast with a message (user retries manually).
- Pending async requests are re-submitted automatically after reconnect.
- Input constraints:
  - numeric operands are integer-only (`int32_t`) at the CLI,
  - float values (for example `12.3`) are rejected as invalid input.

### Run Client 2 (subtract, divide, search)

```bash
cd build
./client2
```

Client 2 follows the same restart policy as Client 1.
- Input constraints:
  - subtract/divide operands are integer-only (`int32_t`) at the CLI,
  - search substring/string inputs must each be 1..16 characters.

Client 2 runtime library resolution order:
1. `IPC_LIB_PATH` environment variable (if set),
2. `./libipc.so` (current working directory),
3. `libipc.so` via standard dynamic-loader search paths.

Examples:

```bash
# Local build directory usage
cd build && ./client2

# Explicit override (absolute path recommended)
IPC_LIB_PATH=/opt/ipc/lib/libipc.so ./build/client2

# System-installed library path fallback (after install/ldconfig)
unset IPC_LIB_PATH
./build/client2
```

Both clients now share common CLI and restart helpers via `src/client_common.h`
(`PendingRequest`, input parsing helpers, pre-menu restart probe, and pending
re-submit flow), while command-specific result formatting stays in each client.

### Runtime Constraints

- Numeric CLI inputs are integer-only (`int32_t`) in both clients.
  - Floating-point input such as `12.3` is rejected as invalid input.
- String inputs for concat/search must be 1..16 characters each.
  - Concat result capacity is 32 characters plus null terminator.
- Async request retrieval is explicit:
  - non-blocking operations return a request ID,
  - results are fetched with menu command `4` (`Check pending results`).
- IPC capacity is bounded:
  - at most 16 in-flight requests can exist at once (`IPC_MAX_SLOTS`),
  - additional submissions fail with a no-free-slots error.
- Divide-by-zero is reported as an operation status error, not a numeric result.

## Third-Party Client Quickstart

This section is a practical template for building your own client against
`libipc.so`.

### Option A: Direct Link (recommended)

Minimal source (`my_client.cpp`):

```cpp
#include "libipc.h"
#include <cstdio>
#include <cstdint>

int main() {
    if (ipc_init() != 0) {
        std::fprintf(stderr, "ipc_init failed (is server running?)\n");
        return 1;
    }

    int32_t sum = 0;
    int rc = ipc_add(20, 22, &sum);
    if (rc == 0) {
        std::printf("20 + 22 = %d\n", sum);
    } else if (rc == IPC_ERR_SERVER_RESTARTED) {
        std::printf("Server restarted; retry request.\n");
    } else {
        std::printf("ipc_add failed.\n");
    }

    ipc_cleanup();
    return 0;
}
```

Build from project root (using built artifacts in `build/`):

```bash
g++ -std=c++17 -Iinclude -Isrc my_client.cpp -Lbuild -lipc -Wl,-rpath,'$ORIGIN/build' -o my_client
```

### Option B: Runtime Loading (`dlopen`)

Use this when the library path is chosen at runtime.

```cpp
#include "ipc_defs.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstdint>

using ipc_init_fn = int (*)();
using ipc_cleanup_fn = void (*)();
using ipc_add_fn = int (*)(int32_t, int32_t, int32_t *);

int main() {
    void *h = nullptr;
    if (const char *p = std::getenv("IPC_LIB_PATH"); p && *p) {
        h = dlopen(p, RTLD_NOW);          // 1) explicit override
    }
    if (!h) h = dlopen("./libipc.so", RTLD_NOW);       // 2) local path (run from build/)
    if (!h) h = dlopen("libipc.so", RTLD_NOW);         // 3) standard paths
    if (!h) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    auto fn_init = reinterpret_cast<ipc_init_fn>(dlsym(h, "ipc_init"));
    auto fn_cleanup = reinterpret_cast<ipc_cleanup_fn>(dlsym(h, "ipc_cleanup"));
    auto fn_add = reinterpret_cast<ipc_add_fn>(dlsym(h, "ipc_add"));
    if (!fn_init || !fn_cleanup || !fn_add) {
        std::fprintf(stderr, "dlsym failed: %s\n", dlerror());
        dlclose(h);
        return 1;
    }

    if (fn_init() != 0) {
        std::fprintf(stderr, "ipc_init failed\n");
        dlclose(h);
        return 1;
    }

    int32_t out = 0;
    int rc = fn_add(1, 2, &out);
    if (rc == 0) std::printf("1 + 2 = %d\n", out);
    fn_cleanup();
    dlclose(h);
    return 0;
}
```

### Integration Notes

- Call order: `ipc_init()` -> one or more `ipc_*` calls -> `ipc_cleanup()`.
- For `dlopen` clients, use robust lookup order:
  `IPC_LIB_PATH` -> local path -> standard paths.
- On `IPC_ERR_SERVER_RESTARTED`, reconnect is already attempted in `libipc`; retry at app level.
- For async APIs, handle:
  - `0`: result ready,
  - `IPC_NOT_READY`: keep polling,
  - `IPC_ERR_SERVER_RESTARTED`: pending request IDs are invalid; re-submit payload.
- Respect protocol limits (`IPC_MAX_STRING_LEN`, `IPC_MAX_SLOTS`) from `include/ipc_defs.h`.
- Ensure server is running before client startup.

## Testing

### Install pytest

```bash
python3 -m venv .venv
.venv/bin/pip install pytest
```

### Run Tests

```bash
# Via CMake (reconfigure first to detect pytest)
cmake -B build
cmake --build build --target test

# Or directly
.venv/bin/pytest tests -v

# Run isolated suites explicitly
.venv/bin/pytest -v tests/test_client_server.py
.venv/bin/pytest -v tests/test_server_threads.py
```

Important test preconditions:
- Do not run a manual `build/server` while running pytest. The test harness
  fails if an external server process is detected.
- IPC tests use a global pytest lock at `/tmp/ipc_pytest.lock` to prevent
  concurrent test invocations against shared POSIX IPC names.
- If you see a lock conflict, wait for the other pytest run to finish (or
  terminate stale pytest processes) and rerun.
- Startup PID diagnostics are enabled by default. Set `IPC_TEST_DEBUG_PIDS=0`
  to suppress debug lines if you need quieter pytest output.
- Pytest aborts immediately when an external server is detected at session
  start (single clear failure instead of per-test setup errors). You can
  disable this behavior with `IPC_TEST_ABORT_ON_EXTERNAL=0` for debugging only.

`make test` bootstraps `.venv`, installs `pytest`, and re-runs CMake configure
before building the test target, so it works in fresh clones without manual setup.

The test setup uses two lifecycle models:
- `tests/test_client_server.py` uses a session-scoped shared server fixture.
- `tests/test_server_threads.py` self-manages server start/stop per test.

`make test` runs these suites in separate sequential pytest invocations to avoid
cross-suite interference on global POSIX IPC names.

If a prior interrupted run left orphan server processes, recover with:

```bash
pkill -f "/home/$USER/linProj/.*/build/server|/home/$USER/linProj/build/server" || true
rm -f /dev/shm/ipc_shm /dev/shm/sem.ipc_* /tmp/ipc_server.lock
```

## Documentation

### Doxygen

```bash
# Install: sudo pacman -S doxygen graphviz (Arch/Manjaro)
cmake --build build --target docs-doxygen
# Output: build/docs/doxygen/html/index.html
```

### Sphinx + Breathe

```bash
# Install: pip install sphinx breathe myst-parser
cmake --build build --target docs-sphinx
# Output: build/docs/sphinx/index.html
```

`make docs` bootstraps `.venv`, installs `sphinx` + `breathe` + `myst-parser`,
checks for Graphviz `dot`, and re-runs CMake configure before building
`docs-sphinx`.

## Static Analysis

### cppcheck

```bash
# Install: sudo pacman -S cppcheck (Arch/Manjaro)
cmake --build build --target cppcheck
cmake --build build --target cppcheck-deep   # deeper path exploration, slower
```

`cppcheck` is the fast/default profile. `cppcheck-deep` enables
`--check-level=exhaustive` to explore more branch paths and can take
significantly longer on larger code changes.

### Sanitizers (ASan + UBSan)

```bash
cmake -B build -DENABLE_SANITIZERS=ON
cmake --build build
cmake --build build --target test
```

### Findings

- **cppcheck**: No issues found. Only informational messages about analysis
  branch depth limits (not actual defects).
- **Compiler warnings** (`-Wall -Wextra -Wpedantic`): Clean -- zero warnings.
- **ASan/UBSan**: No runtime errors detected during test execution.
- **Restart caveat**: In-flight async requests are not durable. After restart,
  clients can re-submit pending async operations, but work already in progress
  at crash time is not recovered from disk.

## Project Structure

```
linProj/
├── CMakeLists.txt              # Build configuration
├── README.md                   # This file
├── include/
│   └── ipc_defs.h              # Protocol structs, enums, constants (pure C)
├── src/
│   ├── libipc.h                # Public C API header
│   ├── libipc.cpp              # Library implementation
│   ├── server.cpp              # Server with dual thread pools
│   ├── client_common.h         # Shared client helpers (input + restart flow)
│   ├── client1.cpp             # Client 1 (direct link)
│   └── client2.cpp             # Client 2 (dlopen/dlsym)
├── Makefile                    # Convenience wrapper for CMake commands
├── docs/
│   ├── Doxyfile.in                # Doxygen config template
│   └── sphinx/
│       ├── conf.py                # Sphinx + Breathe config
│       ├── index.rst              # Documentation root
│       └── overview.md            # Includes README as overview
└── tests/
    ├── conftest.py                # Pytest fixtures (server lifecycle)
    ├── test_client_server.py      # Integration tests
    └── test_server_threads.py     # Thread-config and -t flag tests
```
