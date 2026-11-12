#!/usr/bin/env python3
"""Isolated X11 tooltip regression tests. Requires Xvfb and python-xlib.
Run: python3 tests/integration/tooltip.py --polybar build/bin/polybar
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

from Xlib import X, display
from Xlib.ext import shape, xtest


def wait_for(predicate, timeout=3):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.02)
    raise AssertionError("Timed out waiting for X11 state")


class TooltipTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="polybar-tooltip-")
        cls.path = Path(cls.directory.name)
        cls.server_log = (cls.path / "xvfb.log").open("w+")
        # -displayfd allocates an unused display and signals when it is ready.
        read_fd, write_fd = os.pipe()
        cls.server = subprocess.Popen([ARGS.xvfb, "-displayfd", str(write_fd), "-screen", "0", "1000x700x24",
                                       "-nolisten", "tcp"], pass_fds=(write_fd,), stdout=cls.server_log,
                                      stderr=subprocess.STDOUT)
        os.close(write_fd)
        with os.fdopen(read_fd) as stream:
            number = stream.readline().strip()
        if not number:
            cls.server_log.seek(0)
            raise RuntimeError(cls.server_log.read())
        cls.env = dict(os.environ, DISPLAY=":" + number, XDG_RUNTIME_DIR=str(cls.path))
        cls.env.pop("I3SOCK", None)
        cls.env.pop("I3_CONFIG_PATH", None)
        cls.x = display.Display(cls.env["DISPLAY"])
        cls.root = cls.x.screen().root
        # Run under i3 when available, without using any live-session config.
        cls.wm = None
        if shutil.which("i3"):
            config = cls.path / "i3.conf"
            config.write_text(f"# i3 config file (v4)\nfont pango:monospace 10\nfocus_follows_mouse no\n"
                              f"ipc-socket {cls.path / 'i3.sock'}\nbindsym Mod4+Return exec true\n")
            cls.wm = subprocess.Popen(["i3", "-c", str(config)], env=cls.env,
                                      stdout=cls.server_log, stderr=subprocess.STDOUT)
            try:
                wait_for(lambda: cls.root.get_full_property(cls.x.intern_atom("_NET_SUPPORTING_WM_CHECK"), X.AnyPropertyType))
            except Exception:
                cls.server_log.flush()
                cls.server_log.seek(0)
                print(cls.server_log.read())
                cls.wm.terminate()
                cls.wm.wait(timeout=5)
                cls.server.terminate()
                cls.server.wait(timeout=5)
                raise

    @classmethod
    def tearDownClass(cls):
        cls.x.close()
        if cls.wm:
            cls.wm.terminate()
            cls.wm.wait(timeout=5)
        cls.server.terminate()
        cls.server.wait(timeout=5)
        cls.server_log.close()
        cls.directory.cleanup()

    def setUp(self):
        self.process = None
        self.root.warp_pointer(500, 400)
        self.x.sync()
        for name in ("runs", "child", "label", "click"):
            (self.path / name).write_text("OK\n" if name == "label" else "")

    def tearDown(self):
        if any(test is self for test, _ in self._outcome.result.failures + self._outcome.result.errors):
            print(self.logs())
        if self.process:
            self.process.terminate()
            self.process.wait(timeout=5)
            self.assertEqual(0, self.process.returncode, self.logs())
            wait_for(lambda: not self.popup())
            self.log.close()

    def logs(self):
        self.log.flush()
        return (self.path / "polybar.log").read_text()

    def start(self, options="tooltip = Hello\\nGrüße → UTF-8", bottom=False, tail=False, monitor="", width=800):
        options = "\n".join(line.replace("\\", "\\\\") if line.startswith("tooltip-exec =") else line
                            for line in options.splitlines())
        conf = self.path / "config.ini"
        conf.write_text(f"""[bar/test]
width = {width}
monitor = {monitor}
height = 32
offset-x = 70
offset-y = 10
bottom = {'true' if bottom else 'false'}
font-0 = monospace:size=10
background = #222222
foreground = #ffffff
modules-left = subject
modules-center = plain
modules-right = other
module-margin = 0
padding = 0
enable-ipc = true
wm-name = polybar-tooltip-test
[module/subject]
type = custom/script
exec = {'tail -n +1 -f' if tail else 'cat'} {self.path / 'label'}
tail = {'true' if tail else 'false'}
interval = 0.1
label = %output%
format-prefix = PRE
format-suffix = POST
format-padding = 2
click-left = echo clicked >> {self.path / 'click'}
scroll-up = echo scrolled >> {self.path / 'click'}
{options}
[module/plain]
type = custom/script
exec = echo PLAIN
interval = 60
[module/other]
type = custom/script
exec = echo OTHER
interval = 60
tooltip = Second module
""")
        self.log = (self.path / "polybar.log").open("w")
        self.process = subprocess.Popen([ARGS.polybar, "-l", "trace", "-c", str(conf), "test"], env=self.env,
                                        stdout=self.log, stderr=subprocess.STDOUT)
        try:
            self.bar = wait_for(lambda: self.find("polybar-tooltip-test"))
        except AssertionError as error:
            raise AssertionError(self.logs()) from error
        time.sleep(0.2)
        self.assertIsNone(self.popup())

    def find(self, name):
        def visit(parent, depth=0):
            for window in parent.query_tree().children:
                try:
                    if window.get_wm_name() == name and window.get_attributes().map_state == X.IsViewable:
                        return window
                    if depth < 4:
                        found = visit(window, depth + 1)
                        if found:
                            return found
                except Exception:
                    pass
            return None
        return visit(self.root)

    def popup(self):
        return self.find("polybar-tooltip")

    def move(self, x, y=15):
        self.bar.warp_pointer(x, y)
        self.x.sync()

    def exit(self):
        self.root.warp_pointer(500, 400)
        self.x.sync()
        wait_for(lambda: not self.popup())

    def test_visible_text_has_balanced_vertical_padding(self):
        self.start("tooltip = HELLO\\nGrüße → gyp\\nLast line")
        self.move(10)
        window = wait_for(self.popup)
        rect = window.get_geometry()
        image = window.get_image(0, 0, rect.width, rect.height, X.ZPixmap, 0xffffffff)
        # The isolated server uses 24-bit TrueColor with four bytes per pixel.
        # Ignore the white border and find the actual antialiased text pixels.
        pixels = memoryview(image.data)
        stride = len(pixels) // rect.height
        rows = [y for y in range(1, rect.height - 1)
                if any(max(pixels[y * stride + x * 4:y * stride + x * 4 + 3]) > 85
                       for x in range(1, rect.width - 1))]
        self.assertTrue(rows)
        top = rows[0] - 1
        bottom = rect.height - 2 - rows[-1]
        self.assertLessEqual(abs(top - bottom), 1, f"Text padding: top={top}, bottom={bottom}")
        self.assertGreaterEqual(min(top, bottom), 10)

    def test_static_delay_input_and_transitions(self):
        self.start()
        focus = self.x.get_input_focus().focus
        self.move(2)  # Format padding, before prefix or label.
        time.sleep(0.1)
        self.assertIsNone(self.popup())
        window = wait_for(self.popup)
        geometry = window.get_geometry()
        bar_geometry = self.bar.get_geometry()
        self.assertGreaterEqual(geometry.y, self.root.translate_coords(self.bar, 0, 0).y + bar_geometry.height)
        self.assertGreaterEqual(geometry.x, 0)
        self.assertLessEqual(geometry.x + geometry.width, 1000)
        self.assertGreater(geometry.height, 35)  # Two rendered lines.
        self.assertTrue(window.get_attributes().override_redirect)
        self.assertEqual(0, len(window.shape_get_rectangles(shape.SK.Input).rectangles))
        self.assertEqual(focus, self.x.get_input_focus().focus)
        xtest.fake_input(self.x, X.ButtonPress, 1)
        xtest.fake_input(self.x, X.ButtonRelease, 1)
        xtest.fake_input(self.x, X.ButtonPress, 4)
        xtest.fake_input(self.x, X.ButtonRelease, 4)
        self.x.sync()
        wait_for(lambda: len((self.path / "click").read_text().splitlines()) == 2)
        self.assertEqual({"clicked", "scrolled"}, set((self.path / "click").read_text().splitlines()))
        self.move(30)
        time.sleep(0.15)
        self.assertEqual(window.id, self.popup().id)
        # Move to unconfigured module in another alignment group.
        self.move(400)
        wait_for(lambda: not self.popup())
        time.sleep(0.4)
        self.assertIsNone(self.popup())
        self.move(785)
        second = wait_for(self.popup)
        self.assertNotEqual(window.id, second.id)
        self.exit()

    def dynamic(self, command, timeout=2000):
        self.start(f"tooltip = unused\ntooltip-exec = {command}\ntooltip-delay = 80\n"
                   f"tooltip-exec-timeout = {timeout}")

    def test_command_once_multiline_and_reentry(self):
        self.dynamic(f"echo run >> {self.path / 'runs'}; printf 'Line 1\\nLine 2\\n'")
        self.move(10)
        window = wait_for(self.popup)
        self.assertGreater(window.get_geometry().height, 35)
        for x in (20, 25, 30):
            self.move(x)
            time.sleep(0.08)
        self.assertEqual(["run"], (self.path / "runs").read_text().splitlines())
        self.exit()
        self.move(10)
        wait_for(self.popup)
        self.assertEqual(2, len((self.path / "runs").read_text().splitlines()))

    def test_delay_cancel_and_slow_cancel(self):
        self.dynamic(f"echo run >> {self.path / 'runs'}; echo $$ > {self.path / 'child'}; sleep 1; echo late")
        self.move(10)
        time.sleep(0.02)
        self.exit()
        time.sleep(0.15)
        self.assertEqual("", (self.path / "runs").read_text())
        self.move(10)
        wait_for(lambda: (self.path / "child").read_text())
        pid = int((self.path / "child").read_text())
        self.exit()
        time.sleep(1.1)
        self.assertIsNone(self.popup())
        self.assertFalse(Path(f"/proc/{pid}").exists())
        self.assertEqual(1, len((self.path / "runs").read_text().splitlines()))

    def test_timeout_does_not_retry_on_motion(self):
        self.dynamic(f"echo run >> {self.path / 'runs'}; trap '' TERM; sleep 5; echo late", timeout=120)
        self.move(10)
        wait_for(lambda: "timed out" in self.logs())
        self.move(20)
        time.sleep(0.3)
        self.assertIsNone(self.popup())
        self.assertEqual(["run"], (self.path / "runs").read_text().splitlines())
        self.assertIsNone(self.process.poll())

    def test_command_failure_discards_output(self):
        self.dynamic("printf stale; exit 7")
        self.move(10)
        wait_for(lambda: "tooltip-exec failed" in self.logs())
        self.assertIsNone(self.popup())

    def test_output_limit(self):
        self.dynamic("yes flood")
        self.move(10)
        wait_for(lambda: "output limit" in self.logs())
        self.assertIsNone(self.popup())
        self.assertIsNone(self.process.poll())

    def test_resize_hidden_and_bottom_placement(self):
        self.start(bottom=True)
        self.move(10)
        window = wait_for(self.popup)
        rect = window.get_geometry()
        bar_rect = self.bar.get_geometry()
        self.assertLessEqual(rect.y + rect.height, self.root.translate_coords(self.bar, 0, 0).y - 8)
        self.assertGreaterEqual(rect.y, 0)
        self.exit()
        (self.path / "label").write_text("W" * 30)
        time.sleep(0.25)
        self.move(200)
        wait_for(self.popup)
        (self.path / "label").write_text("OK")
        wait_for(lambda: not self.popup())  # Pointer now over empty bar space.
        self.move(10)
        wait_for(self.popup)
        msg = str(Path(ARGS.polybar).with_name("polybar-msg"))
        if Path(msg).exists():
            subprocess.run([msg, "-p", str(self.process.pid), "cmd", "hide"], env=self.env, check=True,
                           stdout=subprocess.DEVNULL)
            wait_for(lambda: not self.popup())

    def test_tail_and_popup_cleanup_on_shutdown(self):
        self.start(tail=True)
        self.move(10)
        wait_for(self.popup)
        self.process.terminate()
        self.process.wait(timeout=5)
        wait_for(lambda: not self.popup())

    def test_slow_command_keeps_bar_responsive(self):
        self.dynamic("sleep 0.5; printf 'Completed\\n'")
        self.move(10)
        time.sleep(0.15)
        redraws = self.logs().count("Redrawing bar window")
        (self.path / "label").write_text("W" * 30 + "\n")
        wait_for(lambda: self.logs().count("Redrawing bar window") > redraws)
        self.move(200)  # Newly expanded portion must remain the same hover entry.
        wait_for(self.popup)
        self.assertIsNone(self.process.poll())

    def test_empty_command_precedence(self):
        self.dynamic("")
        self.move(10)
        time.sleep(0.4)
        self.assertIsNone(self.popup())

    def test_missing_executable(self):
        self.dynamic("/definitely/nonexistent/polybar-tooltip-command")
        self.move(10)
        wait_for(lambda: "tooltip-exec failed" in self.logs())
        self.assertIsNone(self.popup())

    def test_virtual_nonprimary_monitor(self):
        if not shutil.which("xrandr"):
            self.skipTest("xrandr is unavailable")
        subprocess.run(["xrandr", "--setmonitor", "tooltip-right", "500/132x600/159+500+100", "none"],
                       env=self.env, check=True, stdout=subprocess.DEVNULL)
        try:
            monitors = subprocess.check_output(["xrandr", "--listmonitors"], env=self.env, text=True)
            if "tooltip-right" not in monitors:
                self.skipTest("X server does not expose virtual RandR monitors")
            supported = subprocess.check_output([ARGS.polybar, "--list-all-monitors"], env=self.env, text=True)
            if "tooltip-right" not in supported:
                self.skipTest("Polybar was built without RandR monitor support")
            self.start(monitor="tooltip-right", width=400)
            self.move(10)
            rect = wait_for(self.popup).get_geometry()
            self.assertGreaterEqual(rect.x, 500)
            self.assertGreaterEqual(rect.y, 100)
            self.assertLessEqual(rect.x + rect.width, 1000)
            self.assertLessEqual(rect.y + rect.height, 700)
            self.exit()
            self.move(390)
            rect = wait_for(self.popup).get_geometry()
            self.assertGreaterEqual(rect.x, 500)
            self.assertLessEqual(rect.x + rect.width, 1000)
        finally:
            subprocess.run(["xrandr", "--delmonitor", "tooltip-right"], env=self.env, check=True,
                           stdout=subprocess.DEVNULL)

    def test_module_visibility_hides_popup(self):
        self.start()
        self.move(10)
        wait_for(self.popup)
        msg = str(Path(ARGS.polybar).with_name("polybar-msg"))
        if not Path(msg).exists():
            self.skipTest("polybar-msg was not built")
        subprocess.run([msg, "-p", str(self.process.pid), "action", "#subject.module_hide"],
                       env=self.env, check=True, stdout=subprocess.DEVNULL)
        wait_for(lambda: not self.popup())

    def test_shutdown_cancels_child(self):
        self.dynamic(f"echo $$ > {self.path / 'child'}; trap '' TERM; sleep 10; echo late")
        self.move(10)
        pid = int(wait_for(lambda: (self.path / "child").read_text()))
        self.process.terminate()
        self.process.wait(timeout=5)
        wait_for(lambda: not Path(f"/proc/{pid}").exists())
        self.assertIsNone(self.popup())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--polybar", default="build/bin/polybar")
    parser.add_argument("--xvfb", default=shutil.which("Xvfb"))
    ARGS, remaining = parser.parse_known_args()
    if not ARGS.xvfb:
        parser.error("Xvfb is required (or pass --xvfb /path/to/Xvfb)")
    ARGS.polybar = str(Path(ARGS.polybar).resolve())
    unittest.main(argv=[__file__, *remaining])
