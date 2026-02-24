Architecture
============

The system uses slot-based shared memory IPC with two worker pools in the server:
one for math tasks and one for string tasks.

Core components:

- ``build/server``: creates shared memory and semaphore objects, dispatches tasks.
- ``build/libipc.so``: client-facing C API for blocking and non-blocking calls.
- ``build/client1`` and ``build/client2``: sample CLI clients with different
  linking approaches (direct link vs ``dlopen``/``dlsym``).

Shared-memory slot lifecycle:

.. code-block:: text

   FREE -> REQUEST_PENDING -> PROCESSING -> RESPONSE_READY -> FREE

Synchronization model:

- ``/ipc_mutex`` protects shared memory reads/writes.
- ``/ipc_server_notify`` wakes server dispatcher when new work arrives.
- ``/ipc_slot_0`` .. ``/ipc_slot_15`` provide per-slot wake-ups for blocking calls.
