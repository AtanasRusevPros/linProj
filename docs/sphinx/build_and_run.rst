Build and Run
=============

Build flow:

.. code-block:: bash

   make
   make build

Primary binaries:

- ``build/server``
- ``build/client1``
- ``build/client2``
- ``build/libipc.so``

Useful make targets:

- ``make``: default action (same as ``make all``).
- ``make build``: compile only.
- ``make all`` / ``make full``: run build + test + docs.
- ``make clean_all``: remove build + docs + test caches + venv.
- ``make rebuild_all``: clean_all + build + test + docs.
- ``make help``: print all targets.
- ``make deps``: print dependency guide.
- ``make test``: run pytest integration suites.
- ``make docs``: generate Doxygen and Sphinx docs.

Documentation output:

- Doxygen HTML: ``build/docs/doxygen/html/index.html``
- Sphinx HTML: ``build/docs/sphinx/index.html``

Documentation prerequisites:

- ``doxygen``
- ``graphviz`` (provides ``dot`` for Doxygen diagrams)
- ``sphinx-build``
