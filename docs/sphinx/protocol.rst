IPC Protocol
============

Protocol definitions live in ``include/ipc_defs.h``. All shared-memory payload
types are POD-safe and bounded for predictable IPC behavior.

Highlights:

- Fixed request capacity: ``IPC_MAX_SLOTS = 16``.
- Numeric payload type: ``int32_t`` operands/results.
- Request IDs: ``uint64_t`` monotonic IDs for correlation.
- Command families: blocking math, non-blocking math/string, and result polling.

Status and error model:

- ``IPC_STATUS_OK`` for successful operations.
- ``IPC_STATUS_DIV_BY_ZERO`` for divide-by-zero.
- ``IPC_STATUS_TOO_LONG`` for string length violations.
- ``IPC_STATUS_NOT_FOUND`` for failed substring search.

See API details in :doc:`api`.
