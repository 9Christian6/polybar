Testing
=======

Polybar uses `googletest <https://google.github.io/googletest/>`_ as its
testing and mocking framework.
Tests live in the ``tests/`` directory; they can be enabled during cmake with
``-DBUILD_TESTS=ON`` and compiled with ``make all_unit_tests``.

Each test gets its own executable in ``build/tests``, which can be executed to run
a specific test.

Running all tests is preferably done with the following command:

.. code-block:: shell

  make check

This runs all available tests and prints the output in color for failed tests
only.

Adding New Tests
----------------

All new tests need to be added to the ``tests/CMakeLists.txt`` file. Have a look
at the other unit tests in ``tests/unit_tests`` to see how to write tests for your
code.

Script tooltip integration tests
--------------------------------

``tests/integration/tooltip.py`` runs an isolated Xvfb server and, when available,
a separate i3 instance with its own configuration and IPC socket. It requires
``Xvfb`` and the Python ``python-xlib`` package. It does not change the running
desktop session. Build ``polybar`` and ``polybar-msg`` first, then run:

.. code-block:: shell

   python3 tests/integration/tooltip.py --polybar build/bin/polybar -v

Pass ``--xvfb /path/to/Xvfb`` to use a server outside ``PATH``. Tests cover delay
cancellation, hover transitions, focus and input transparency, click/scroll
regression, UTF-8 and multiple lines, command completion/failure/timeout/output
limits, once-per-entry execution, responsive updates during execution, dynamic
module widths, bottom placement, tail mode, bar hiding, and shutdown cleanup.
When ``xrandr`` and RandR monitor support are available, a virtual monitor with a
nonzero origin also checks monitor clamping. Numerical unit tests additionally
cover negative monitor origins, oversized content, defaults, invalid options,
content precedence, and independent click/hover geometry.

Physical dual-monitor setups, unusual window-manager work-area policies, and
visual appearance with a compositor still merit manual verification.
