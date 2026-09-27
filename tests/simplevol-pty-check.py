#!/usr/bin/env python3
"""Real-terminal interaction test with a fake audio controller."""
import errno
import fcntl
import os
from pathlib import Path
import pty
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time


with tempfile.TemporaryDirectory(prefix="simplevol-pty-") as tmp:
    directory = Path(tmp)
    binary = directory / "simplevol"
    shutil.copy2(sys.argv[1], binary)
    helper = directory / "simplevol-audio"
    log = directory / "commands"
    helper.write_text('''import sys,json,time
from pathlib import Path
presets = ("Flat", "Rock", "Classical", "Jazz", "Pop", "Bass", "Voice")
preset = "Flat"
def snapshot():
    for line in snapshot_lines():
        print(line, flush=True)
def snapshot_lines():
    yield "BEGIN"
    yield "INFO\\t0\\t0\\t" + preset + "\\tSpeakers\\tTest audio server"
    yield "FX\\tcompressor\\tCompressor\\t0\\t0\\t1\\t1\\tbool\\tFast linked compression"
    yield "FX\\tthreshold\\tThreshold\\t-18\\t-60\\t0\\t1\\tdB\\tCompression threshold"
    yield "FX\\tlowpass\\tLow-pass filter\\t0\\t0\\t1\\t1\\tbool\\tSoften harsh highs"
    yield "FX\\tlowpass_cutoff\\tLow-pass cutoff\\t6000\\t1000\\t20000\\t250\\tHz\\tLower cuts more treble; higher lets more through."
    for name in presets:
        yield "PRESET\\t" + name + "\\tEQ"
    yield "PRESET\\tStudio\\tSaved chain"
    yield "ROW\\tplayback\\t42\\tmpv\\tMusic Player\\tSpeakers\\t80\\t0\\t0\\t80,40\\tfront-left,front-right"
    yield "ROW\\toutputs\\t10\\tSpeakers\\tStudio Speakers\\tAnalog output\\t75\\t0\\t1\\t75,75\\tfront-left,front-right"
    yield "ROW\\toutputs\\t11\\tHeadphones\\tUSB Headphones\\tUSB\\t50\\t0\\t0\\t50,50\\tfront-left,front-right"
    yield "OPTION\\toutputs\\t10\\tport\\tanalog\\tAnalog output\\t1\\t1"
    yield "END"
snapshot()
for line in sys.stdin:
    with (Path(__file__).parent / "commands").open("a") as f: f.write(line)
    if line.startswith("preset\\t"):
        preset = line.strip().split("\\t")[1]
        snapshot()
    print("MESSAGE\\tok\\tUpdated", flush=True)
''')
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 38, 120, 0, 0))
    process = subprocess.Popen([str(binary)], stdin=slave, stdout=slave, stderr=slave,
                               env={**os.environ, "TERM": "xterm-256color"}, start_new_session=True)
    os.close(slave)
    output = bytearray()

    def drain(seconds=.2):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            readable, _, _ = select.select([master], [], [], max(0, deadline - time.monotonic()))
            if readable:
                try:
                    chunk = os.read(master, 65536)
                    if not chunk:
                        break
                    output.extend(chunk)
                except OSError as exc:
                    if exc.errno == errno.EIO:
                        break
                    raise

    def keys(text):
        os.write(master, text)
        drain()

    try:
        drain(.7)
        assert b"Music Player" in output, output[-2000:]
        keys(b"l")
        keys(b"rj\n")
        keys(b"6")
        assert b"EQ preset [Enter]" in output
        keys(b"\n")
        for name in (b"Flat", b"Rock", b"Classical", b"Jazz", b"Pop", b"Bass", b"Voice"):
            assert name in output, name
        keys(b"jj\n")
        keys(b" j\n")  # Space reopens the selector at Classical; next is Jazz.
        keys(b"lj\n")  # Right opens it at Jazz; next is Pop.
        keys(b"j ")
        keys(b"j\n-24\n")
        assert b"Low-pass filter" in output
        keys(b"j ")
        keys(b"jhl\n4500\n")
        keys(b"Pkkk\n")  # The existing shortcut still includes all presets.
        keys(b"SStudio\n")
        keys(b"2c\nj\x1b")
        keys(b"?")
        assert b"SimpleVol controls" in output
        keys(b"\x1b")
        fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 12, 40, 0, 0))
        os.kill(process.pid, signal.SIGWINCH)
        drain(.3)
        assert b"needs at least" in output
        keys(b"q")
        assert process.wait(timeout=3) == 0
        commands = log.read_text().splitlines()
        assert "adjust\tplayback\t42\t2" in commands, commands
        assert "route\tplayback\t42\tHeadphones" in commands, commands
        assert "set\tcompressor\t1" in commands, commands
        assert "set\tthreshold\t-24" in commands, commands
        assert "set\tlowpass\t1" in commands, commands
        assert "step\tlowpass_cutoff\t-1" in commands, commands
        assert "step\tlowpass_cutoff\t1" in commands, commands
        assert "set\tlowpass_cutoff\t4500" in commands, commands
        for preset in ("Classical", "Jazz", "Pop"):
            assert "preset\t" + preset in commands, commands
        assert "preset\tRock" in commands, commands
        assert "save\tStudio" in commands, commands
        print("SimpleVol PTY: navigation, routing, EQ selector, Classical/Jazz/Pop, exact values, saved chains, help, resize, clean exit passed")
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=3)
        os.close(master)
