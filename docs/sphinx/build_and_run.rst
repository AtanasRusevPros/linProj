Build and Run
=============

Build flow:

.. code-block:: bash

   make all

Primary binaries:

- ``build/server``
- ``build/client1``
- ``build/client2``
- ``build/libipc.so``

Useful make targets:

- ``make help``: print all targets.
- ``make deps``: print dependency guide.
- ``make test``: run pytest integration suites.
- ``make docs``: generate Doxygen and Sphinx docs.

Documentation output:

- Doxygen HTML: ``build/docs/doxygen/html/index.html``
- Sphinx HTML: ``build/docs/sphinx/index.html``
