IPC Client-Server System
========================

A Linux client-server system using POSIX shared memory for inter-process
communication. The system consists of a shared library (``libipc.so``),
a multi-threaded server, and two client applications.

Architecture
------------

The system uses a **slot-based shared memory** design. A fixed array of 16
message slots resides in POSIX shared memory (``/dev/shm/ipc_shm``). Each slot
holds one in-flight request and its corresponding response.

**Slot state machine:**

.. code-block:: text

   FREE -> REQUEST_PENDING -> PROCESSING -> RESPONSE_READY -> FREE

**Synchronization** uses named POSIX semaphores:

- ``/ipc_mutex`` -- protects all shared memory reads/writes.
- ``/ipc_server_notify`` -- counting semaphore to wake the server dispatcher.
- ``/ipc_slot_0`` through ``/ipc_slot_15`` -- per-slot semaphores for blocking call wake-up.

The server uses **dual thread pools** (one for math, one for string operations)
backed by ``std::thread`` (which wraps POSIX pthreads on Linux).

IPC Protocol
------------

Defined in ``include/ipc_defs.h``. All types are POD and safe for shared memory.

API Reference
-------------

libipc.h
^^^^^^^^

.. doxygenfile:: libipc.h
   :project: ipc

ipc_defs.h
^^^^^^^^^^

.. doxygenfile:: ipc_defs.h
   :project: ipc
