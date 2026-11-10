Script hover tooltips
=====================

Modules of type ``custom/script`` can show a native tooltip with information
independent of their normal ``exec`` output. Hover anywhere over the module's
rendered format, including prefix, suffix, and padding. Leaving the module or
hiding the bar cancels pending work and hides the popup immediately. Modules
without either content option keep their existing behavior.

Configuration
-------------

All options belong to the module section. Distances and dimensions are pixels;
delays are milliseconds.

.. list-table::
   :header-rows: 1
   :widths: 30 20 50

   * - Key
     - Default
     - Meaning
   * - ``tooltip``
     - unset
     - Static plain text; ``\n`` becomes a line break.
   * - ``tooltip-exec``
     - unset
     - Shell command producing tooltip text on stdout. Takes precedence over
       static text, even when explicitly empty.
   * - ``tooltip-delay``
     - 350
     - Pointer dwell before displaying text or starting the command.
   * - ``tooltip-exec-timeout``
     - 2000
     - Time limit for command completion and stdout closure.
   * - ``tooltip-max-width``
     - 400
     - Maximum total popup width, including padding and border.
   * - ``tooltip-max-height``
     - 300
     - Maximum total popup height, including padding and border.
   * - ``tooltip-offset``
     - 8
     - Gap below a top bar or above a bottom bar.
   * - ``tooltip-padding``
     - 10
     - Inner padding on each side.
   * - ``tooltip-background``
     - bar background
     - Solid background color.
   * - ``tooltip-foreground``
     - bar foreground
     - Text color.
   * - ``tooltip-border-size``
     - 1
     - Border thickness; zero disables it.
   * - ``tooltip-border-color``
     - bar foreground
     - Border color.

Empty content produces no popup. Trailing CR/LF characters are removed; internal
line breaks and blank lines are preserved. Text is UTF-8, uses the bar's configured
fonts and fallback fonts, and wraps at character boundaries. Formatting tags in
text are displayed literally. Content beyond the height limit is clipped, without
scrolling. Captured stdout is limited to 64 KiB; exceeding that limit cancels the
command and logs a warning. Static text is truncated to 64 KiB. Invalid UTF-8 bytes
are dropped. Text dimensions are further limited by the monitor work area;
padding/borders shrink when needed. Colors render opaquely, ignoring alpha, so the
popup also works without a compositor.

Commands run once per qualifying hover entry, after the delay, using
``POLYBAR_SHELL`` (default ``/bin/sh``) with ``-c``. They run independently of the
module's regular script, its interval, condition, and tail mode. Stderr remains
in Polybar's diagnostic output. A failed command displays nothing. Timeout,
excess output, leaving the module, hiding the bar, and shutdown kill the command's
process group and discard late results. Scripts that deliberately detach into a
separate session are outside this process group; tooltip commands should not
launch daemons. Moving inside the same module or rerendering it does not rerun the
command or reset the delay. Reentry runs it again.

The popup is centered over the module's current pixel bounds, positioned below
top bars and above bottom bars, then clamped to the intersection of the selected
monitor and the EWMH work area when available. Negative monitor origins and bar
offsets are supported. It is an override-redirect X11 window so tiling window
managers such as i3 do not manage it or reserve space for it. Its empty Shape input
region lets pointer events pass through; it never requests focus. It uses no
notification daemon. Building requires ``xcb-shape`` development headers/library
and the X server must support Shape input regions (Shape 1.1).

Examples
--------

.. code-block:: ini

   [module/updates]
   type = custom/script
   exec = ~/.config/polybar/scripts/updates-count.sh
   interval = 60
   label = Updates: %output%
   tooltip-exec = ~/.config/polybar/scripts/updates-details.sh
   tooltip-delay = 350
   tooltip-exec-timeout = 2000
   tooltip-max-width = 380
   tooltip-max-height = 300
   tooltip-offset = 8
   tooltip-padding = 12
   tooltip-background = #222222
   tooltip-foreground = #ffffff
   tooltip-border-size = 1
   tooltip-border-color = #ff00ff

   [module/status]
   type = custom/script
   exec = echo OK
   interval = 10
   tooltip = All services operational\nLast checked automatically

For a self-contained dynamic example, use:

.. code-block:: ini

   [module/details]
   type = custom/script
   exec = echo Details
   interval = 60
   tooltip-exec = printf 'Line 1\\nLine 2\\n'
