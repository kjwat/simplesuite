#!/usr/bin/env python3
"""Check visible controls while the audio controller withholds replies."""
import errno
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import signal
import shutil
import struct
import subprocess
import sys
import tempfile
import termios
import time


class Screen:
    def __init__(self, rows, cols):
        self.rows, self.cols = rows, cols
        self.cells = [[" "] * cols for _ in range(rows)]
        self.y = self.x = 0
        self.pending, self.last = "", " "

    def feed(self, data):
        text = self.pending + data.decode("ascii", errors="replace")
        i = 0
        while i < len(text):
            char = text[i]
            if char == "\x1b":
                if i + 1 == len(text):
                    break
                if text[i + 1] == "[":
                    match = re.match(r"\x1b\[([0-?]*)([ -/]*)([@-~])", text[i:])
                    if not match:
                        break
                    args, _, command = match.groups()
                    values = [int(v) if v else 0 for v in args.lstrip("?").split(";")]
                    n = values[0] or 1
                    if command in ("H", "f"):
                        self.y = (values[0] or 1) - 1
                        self.x = ((values[1] if len(values) > 1 else 1) or 1) - 1
                    elif command == "d":
                        self.y = n - 1
                    elif command in ("G", "`"):
                        self.x = n - 1
                    elif command == "A":
                        self.y -= n
                    elif command in ("B", "e"):
                        self.y += n
                    elif command in ("C", "a"):
                        self.x += n
                    elif command == "D":
                        self.x -= n
                    elif command == "J" and values[0] in (2, 3):
                        self.cells = [[" "] * self.cols for _ in range(self.rows)]
                    elif command == "K":
                        start = 0 if values[0] in (1, 2) else self.x
                        end = self.x + 1 if values[0] == 1 else self.cols
                        self.cells[self.y][start:end] = [" "] * (end - start)
                    elif command == "b":
                        for _ in range(n):
                            self.put(self.last)
                    self.y = max(0, min(self.rows - 1, self.y))
                    self.x = max(0, min(self.cols - 1, self.x))
                    i += match.end()
                    continue
                if text[i + 1] in "()":
                    if i + 2 == len(text):
                        break
                    i += 3
                    continue
                i += 2
                continue
            if char == "\r":
                self.x = 0
            elif char == "\n":
                self.y = min(self.rows - 1, self.y + 1)
            elif char == "\b":
                self.x = max(0, self.x - 1)
            elif char >= " ":
                self.put(char)
            i += 1
        self.pending = text[i:]

    def put(self, char):
        self.cells[self.y][self.x] = char
        self.last = char
        self.x = min(self.cols - 1, self.x + 1)

    def lines(self):
        return ["".join(row).rstrip() for row in self.cells]


HELPER = '''import os,sys,time
from pathlib import Path
root = Path(__file__).parent
(root / "helper-pid").write_text(str(os.getpid()))
sequence = 0
rows = {
    ("playback", "system-sounds"): [80., 40., False, "System Sounds"],
    ("playback", "42"): [60., 30., False, "Music Player"],
    ("outputs", "10"): [75., 37.5, False, "Speakers"],
    ("inputs", "11"): [50., 25., False, "Microphone"],
    ("recording", "12"): [30., 15., False, "Recorder"],
}
preamp = 0.
def snapshot():
    print("BEGIN\\t" + str(sequence), flush=True)
    print("INFO\\t0\\t0\\tFlat\\tSpeakers\\tDelayed audio server", flush=True)
    for (section, identity), (left,right,mute,title) in rows.items():
        print("\\t".join(map(str,("ROW",section,identity,identity,title,"Test route",max(left,right),int(mute),0,
                                   f"{left:.3f},{right:.3f}","front-left,front-right"))), flush=True)
    print(f"FX\\tpreamp\\tInput gain\\t{preamp}\\t-24\\t12\\t0.5\\tdB\\tInput gain", flush=True)
    print("END", flush=True)
snapshot()
for line in sys.stdin:
    fields = line.rstrip("\\n").split("\\t")
    (root / "received").write_text(line)
    while not (root / "release").exists():
        snapshot()  # A stale snapshot must not undo newer keyboard edits.
        time.sleep(.03)
    sequence += 1
    if (root / "reject").exists():
        (root / "reject").unlink()
        print(f"MESSAGE\\terror\\tTest volume rejected\\t{sequence}", flush=True)
    else:
        action = fields[0]
        if action == "step":
            preamp = max(-24,min(12,preamp + float(fields[2])*.5))
        else:
            row = rows[(fields[1],fields[2])]
            if action == "mute":
                row[2] = not row[2]
            elif action in ("volume", "adjust"):
                old = max(row[:2])
                target = max(0,min(150,float(fields[3]) + (old if action == "adjust" else 0)))
                if (root / "limit").exists():
                    target = min(65,target)
                row[:2] = [target*v/old if old else target for v in row[:2]]
            elif action == "channel":
                channel, value = fields[3].split(":")
                row[int(channel)] = float(value)
        print(f"MESSAGE\\tok\\tUpdated\\t{sequence}", flush=True)
    snapshot()
    (root / "applied").write_text(str(sequence))
'''


def run(binary):
    with tempfile.TemporaryDirectory(prefix="simplevol-responsive-") as temporary:
        root = Path(temporary)
        shutil.copy2(binary, root / "simplevol")
        (root / "simplevol-audio").write_text(HELPER)
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 32, 110, 0, 0))
        child = subprocess.Popen([str(root / "simplevol")], stdin=slave, stdout=slave, stderr=slave,
                                 env={**os.environ, "TERM": "xterm-256color"}, start_new_session=True)
        os.close(slave)
        screen = Screen(32, 110)

        def drain(seconds=0):
            deadline = time.monotonic() + seconds
            while select.select([master], [], [], max(0, deadline - time.monotonic()))[0]:
                try:
                    data = os.read(master, 65536)
                except OSError as error:
                    if error.errno == errno.EIO:
                        break
                    raise
                if not data:
                    break
                screen.feed(data)
                if time.monotonic() >= deadline:
                    break

        def until(predicate, seconds=2):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                drain(.005)
                if predicate():
                    return
                assert child.poll() is None, screen.lines()
            raise AssertionError(screen.lines())

        def keys(text):
            os.write(master, text)

        def volume(value, row=5):
            return f"{value:.1f}%" in screen.lines()[row]

        try:
            until(lambda: volume(80))
            old_bar = screen.lines()[7]
            started = time.monotonic()
            keys(b"l")
            until(lambda: volume(82) and screen.lines()[7] != old_bar, .2)
            latency = time.monotonic() - started
            until(lambda: (root / "received").exists())
            drain(.15)
            assert volume(82), "Old refresh reversed the pending slider"
            keys(b"lllhl")
            until(lambda: volume(88), .2)
            keys(b"jl")
            until(lambda: volume(62, 8), .2)
            keys(b"2h")
            until(lambda: volume(73), .2)
            keys(b"c")
            until(lambda: "front-right  36.5%" in "\n".join(screen.lines()), .2)
            keys(b"\x1b3l")
            until(lambda: volume(52), .2)
            keys(b"4h")
            until(lambda: volume(28), .2)
            keys(b"6jl")
            until(lambda: "0.5 dB" in screen.lines()[6], .2)
            keys(b"1k \n70\n")
            until(lambda: volume(70) and "MUTED" in screen.lines()[5], .2)
            # The helper has not applied any command yet, throughout the
            # repeated keys, navigation and exact/mute edits above.
            assert not (root / "applied").exists()
            (root / "release").touch()
            until(lambda: (root / "applied").exists() and int((root / "applied").read_text()) == 13)
            drain(.1)
            assert volume(70) and "MUTED" in screen.lines()[5], screen.lines()
            (root / "reject").touch()
            keys(b"l")
            until(lambda: "Test volume rejected" in "\n".join(screen.lines()))
            assert volume(70), "Rejected change stayed on the slider"
            (root / "release").unlink()
            (root / "limit").touch()
            keys(b"l")
            until(lambda: volume(72), .2)
            drain(.1)
            assert volume(72), "Older confirmed value undid the new keypress"
            (root / "release").touch()
            until(lambda: volume(65))
            os.kill(int((root / "helper-pid").read_text()), signal.SIGTERM)
            until(lambda: "Audio controller exited" in "\n".join(screen.lines()))
            keys(b"l")
            until(lambda: "busy or disconnected" in "\n".join(screen.lines()))
            assert volume(65), "A failed pipe write moved the slider"
            keys(b"q")
            assert child.wait(timeout=3) == 0
            print(f"SimpleVol visual response: {latency * 1000:.1f} ms; held replies, repeated keys, stale snapshots, pages, balance, effects, reconciliation, errors, disconnect passed")
        finally:
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=3)
            os.close(master)


if __name__ == "__main__":
    run(os.path.abspath(sys.argv[1]))
