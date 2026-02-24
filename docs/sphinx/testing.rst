Testing Guide
=============

Integration tests use ``pytest`` and run against real binaries:

- Client behavior tests in ``tests/test_client_server.py``.
- IPC/load/pathological tests in ``tests/test_server_threads.py``.

Execution:

.. code-block:: bash

   make test

Target model:

- ``make build`` compiles binaries only.
- ``make test`` runs tests only.
- ``make docs`` builds docs only.
- ``make clean_all`` removes generated build/test/docs artifacts.
- ``make all`` / ``make full`` run build + test + docs.

Important preconditions:

- Do not run a manual ``./server`` while tests run.
- Run one pytest session at a time to avoid shared-resource conflicts.
- Respect test harness preflight checks for external server detection.

Debug aids:

- PID diagnostics can print detected server instances at session start.
- Tests abort early when external server processes are detected.
