#!/usr/bin/env python3
"""Exercise b/c and live wallpaper changes in the actual terminal program."""
import fcntl
import importlib.util
import os
from pathlib import Path
import pty
import re
import select
import shlex
import struct
import subprocess
import sys
import tempfile
import termios
import time
import zlib

binary = Path(sys.argv[1]).resolve()
spec = importlib.util.spec_from_file_location(
    "simplevis_terminal_screen", Path(__file__).with_name("simplesearch-check.py"))
terminal_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(terminal_module)


def png(path, color):
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(
            ">I", zlib.crc32(kind + payload) & 0xffffffff)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" +
                     chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(b"\0" + bytes(color))) +
                     chunk(b"IEND", b""))


with tempfile.TemporaryDirectory(prefix="simplevis-wallpaper-pty-") as temporary:
    directory = Path(temporary)
    schemas = directory / "schemas"
    schemas.mkdir()
    (schemas / "test.gschema.xml").write_text("""<schemalist>
<schema id="org.gnome.desktop.background" path="/org/gnome/desktop/background/">
 <key name="picture-uri" type="s"><default>''</default></key>
 <key name="picture-uri-dark" type="s"><default>''</default></key>
 <key name="picture-options" type="s"><default>'zoom'</default></key>
 <key name="primary-color" type="s"><default>''</default></key>
</schema>
<schema id="org.gnome.desktop.interface" path="/org/gnome/desktop/interface/">
 <key name="color-scheme" type="s"><default>'default'</default></key>
</schema></schemalist>""")
    subprocess.run(["glib-compile-schemas", str(schemas)], check=True)
    red, blue = directory / "red.png", directory / "blue.png"
    png(red, (255, 0, 0))
    png(blue, (0, 0, 255))
    env = dict(os.environ, HOME=str(directory), XDG_CONFIG_HOME=str(directory / "config"),
               XDG_CURRENT_DESKTOP="GNOME", DESKTOP_SESSION="GNOME",
               GSETTINGS_SCHEMA_DIR=str(schemas), GSETTINGS_BACKEND="keyfile",
               GIO_USE_VFS="local", TERM="xterm-256color", COLORTERM="truecolor")
    env.pop("DBUS_SESSION_BUS_ADDRESS", None)
    env.pop("SIMPLEVIS_CMD", None)
    env.pop("SIMPLEVIS_SOURCE", None)

    def wallpaper(path):
        subprocess.run(["gsettings", "set", "org.gnome.desktop.background",
                        "picture-uri", path.as_uri()], env=env, check=True)

    wallpaper(red)
    audio = shlex.join([sys.executable, "-c",
        "import math,struct,sys,time\n"
        "pcm=b''.join(struct.pack('<h',int(9000*math.sin(2*math.pi*440*n/44100))) "
        "for n in range(2048))\n"
        "while True:\n sys.stdout.buffer.write(pcm);sys.stdout.buffer.flush();time.sleep(2048/44100)\n"])
    def launch():
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 120, 0, 0))
        process = subprocess.Popen([str(binary), "-c", audio], stdin=slave, stdout=slave,
                                   stderr=slave, env=env, start_new_session=True)
        os.close(slave)
        return process, master

    process, master = launch()
    output = bytearray()

    def read():
        if select.select([master], [], [], 0.05)[0]:
            try:
                output.extend(os.read(master, 65536))
            except OSError:
                pass

    def wait_for(pattern, start=0, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            read()
            if pattern.startswith(rb"\x1b"):
                matched = re.search(pattern, output[start:], re.I)
            else:
                # ncurses sends differential updates, so a visible word may
                # span several writes and cursor movements. Reuse the suite's
                # screen decoder after removing palette-control OSC sequences.
                visible = re.sub(rb"\x1b\].*?(?:\x07|\x1b\\)", b"",
                                 output, flags=re.S)
                if b"\x1b]" in visible:
                    visible = visible[:visible.rfind(b"\x1b]")]
                screen = terminal_module.Screen(30, 120)
                screen.feed(visible)
                matched = re.search(pattern.decode(), screen.text(), re.I)
            if matched:
                return
            assert process.poll() is None, bytes(output[-1500:])
        raise AssertionError((pattern, bytes(output[-1800:])))

    def key(value, pattern):
        start = len(output)
        os.write(master, value)
        wait_for(pattern, start)

    try:
        wait_for(rb"\x1b\]4;17;rgb:ff[0-9a-f]*/0+/0+")
        key(b"i", rb"color:accent")
        assert not re.search(rb"\x1b\[(?:\d+;)*7(?:;\d+)*m", output), \
            "Wallpaper startup painted terminal-color bars before the accent"
        key(b"b", rb"color:white")
        key(b"b", rb"color:accent")
        start = len(output)
        wallpaper(blue)
        wait_for(rb"\x1b\]4;17;rgb:0+/0+/ff[0-9a-f]*", start)
        key(b"c", rb"color:spectrum")
        key(b"b", rb"color:accent")
        key(b"b", rb"color:white")
        os.write(master, b"q")
        assert process.wait(timeout=3) == 0
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=3)
        os.close(master)
    # Each selection must survive a complete process exit/relaunch.
    for expected in ("white", "accent", "spectrum"):
        process, master = launch()
        output.clear()
        try:
            key(b"i", ("color:" + expected).encode())
            if expected == "white":
                key(b"b", rb"color:accent")
            elif expected == "accent":
                assert not re.search(rb"\x1b\[(?:\d+;)*7(?:;\d+)*m", output), \
                    "Restored wallpaper mode flashed terminal-color bars"
                key(b"c", rb"color:spectrum")
            else:
                key(b"c", rb"color:white")
            os.write(master, b"q")
            assert process.wait(timeout=3) == 0
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=3)
            os.close(master)
print("OK real PTY: b/c modes persist across restarts, wallpaper paints first, and live color updates work")
