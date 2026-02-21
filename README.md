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
dispatcher thread. Each pool has 2 worker threads, enabling concurrent processing
of multiple requests within each category.

### Non-Blocking Demonstration

Multiply and divide operations include an artificial 2-second server-side delay
to demonstrate that non-blocking calls return immediately. Concat results arrive
before multiply results, showing out-of-order completion.

## Build

### Prerequisites

- Linux (tested on Manjaro/Arch)
- GCC/G++ with C++17 support
- CMake >= 3.14
- Python 3 + pytest (for tests)
- Doxygen + Sphinx + Breathe (for documentation, optional)

### Build Commands

```bash
# Configure
cmake -B build

# Incremental build
cmake --build build

# Clean rebuild (equivalent of make clean all)
cmake --build build --clean-first
# Or from the build directory:
cd build && make clean all
```

### Build with Sanitizers

```bash
cmake -B build -DENABLE_SANITIZERS=ON
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
./server
```

The server creates shared memory and semaphores, then waits for requests.
Press Ctrl+C (SIGINT) for graceful shutdown.

### Run Client 1 (add, multiply, concat)

```bash
cd build
./client1
```

### Run Client 2 (subtract, divide, search)

```bash
cd build
./client2
```

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
```

The test suite automatically starts and stops the server process.

## Documentation

### Doxygen

```bash
# Install: sudo pacman -S doxygen (Arch/Manjaro)
cmake --build build --target docs-doxygen
# Output: build/docs/doxygen/html/index.html
```

### Sphinx + Breathe

```bash
# Install: pip install sphinx breathe
cmake --build build --target docs-sphinx
# Output: build/docs/sphinx/index.html
```

## Static Analysis

### cppcheck

```bash
# Install: sudo pacman -S cppcheck (Arch/Manjaro)
cmake --build build --target cppcheck
```

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
│   ├── client1.cpp             # Client 1 (direct link)
│   └── client2.cpp             # Client 2 (dlopen/dlsym)
├── docs/
│   ├── Doxyfile.in             # Doxygen config template
│   └── sphinx/
│       ├── conf.py             # Sphinx + Breathe config
│       └── index.rst           # Documentation root
└── tests/
    ├── conftest.py             # Pytest fixtures (server lifecycle)
    └── test_client_server.py   # Integration tests
```
