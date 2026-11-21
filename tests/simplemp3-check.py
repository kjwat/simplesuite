#!/usr/bin/env python3
"""Exercise the installed-style app in a terminal with real FFmpeg output."""

import fcntl
import json
import os
from pathlib import Path
import pty
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import termios
import time


APP = Path(sys.argv[1]).resolve()
SINGLE = "https://www.youtube.com/watch?v=AAAAAAAAAAA&pp=tracking"
PLAYLIST = "https://www.youtube.com/watch?v=AAAAAAAAAAA&list=PLfixture&t=15"
RESTRICTED = "https://www.youtube.com/watch?v=CCCCCCCCCCC&pp=tracking"
SLOW = "https://youtu.be/SSSSSSSSSSS"
BROKEN = "https://www.youtube.com/playlist?list=PLbroken"


FIXTURE = r'''
from pathlib import Path
import json
import os
import shutil
import sys
import time
from urllib.parse import parse_qs, urlsplit

args = sys.argv[1:]
if args == ["--version"]:
    print("2026.10.26")
    sys.exit(0)

query = parse_qs(urlsplit(args[-1]).query)
assert not query.keys() - {"v", "list"}, query
video = query.get("v", ["AAAAAAAAAAA"])[0]
first = dict(id="AAAAAAAAAAA", title="First", duration=1.2, uploader="Test Publisher")
second = dict(id="BBBBBBBBBBB", title=r"Second = # ; \ Movement", duration=0.9)
info = dict(first, id=video) if video != "BBBBBBBBBBB" else second
playlist = "list" in query
broken = query.get("list") == ["PLbroken"]
restricted = video == "CCCCCCCCCCC"
if restricted:
    if "--cookies-from-browser" not in args:
        print("ERROR: [youtube] Sign in to confirm your age.", flush=True)
        sys.exit(1)
    assert args[args.index("--cookies-from-browser") + 1] == "firefox"
    info = dict(info, title="Signed-in test")
if video == "SSSSSSSSSSS":
    info = dict(info, title="Slow download")

tracks = [first, second, first] if playlist else [info]
if "--dump-single-json" in args:
    print(json.dumps(dict(id="PLfixture", title="Test Playlist", entries=tracks) if playlist else info))
    sys.exit(0)

assert args[args.index("--format") + 1] == "bestaudio/best"
assert "--abort-on-error" in args and "--abort-on-unavailable-fragments" in args
assert ("--yes-playlist" in args) == playlist
for index, info in enumerate(tracks, 1):
    print("@track " + json.dumps(info), flush=True)
    print('@progress {"_percent_str": "50.0%", "speed": 2097152, "eta": 20}', flush=True)
    root = Path(os.environ["SIMPLEMP3_TEST_AUDIO"])
    source = root / ("second.m4a" if info["id"] == "BBBBBBBBBBB" else "first.wav")
    target = Path(f"{index:06d}{source.suffix}")
    shutil.copy(source, target)
    if broken and index == 2:
        print("ERROR: The second track is unavailable.", flush=True)
        sys.exit(1)
    time.sleep(30 if video == "SSSSSSSSSSS" else 0.3)
    if not playlist:
        Path("art.ppm").write_bytes(b"P6\n8 8\n255\n" + b"\xdf\xa3\x36" * 64)
    print("@file " + json.dumps(str(target)), flush=True)
'''


class Terminal:
    def __init__(self, env, output):
        self.master, tty = pty.openpty()
        fcntl.ioctl(tty, termios.TIOCSWINSZ, struct.pack("HHHH", 26, 100, 0, 0))
        try:
            self.child = subprocess.Popen(
                [APP, "--no-update", "--output-dir", output], env=env,
                stdin=tty, stdout=tty, stderr=tty, start_new_session=True,
            )
        finally:
            os.close(tty)
        self.buffer = b""

    def send(self, text):
        os.write(self.master, text.encode())

    def paste(self, url):
        self.send("\x1b[200~" + url + "\n\x1b[201~")

    def expect(self, text):
        expected = text.encode()
        deadline = time.monotonic() + 12
        while (found := self.buffer.find(expected)) < 0:
            if time.monotonic() > deadline or self.child.poll() is not None:
                raise AssertionError(f"Missing {text!r} from terminal display: {self.buffer[-3500:]!r}")
            if select.select([self.master], [], [], 0.1)[0]:
                self.buffer += os.read(self.master, 65536)
        self.buffer = self.buffer[found + len(expected):]

    def close(self):
        try:
            if self.child.poll() is None:
                self.send("\x11")
                self.child.wait(timeout=8)
            assert self.child.returncode == 0, self.child.returncode
        finally:
            if self.child.poll() is None:
                self.child.kill()
                self.child.wait()
            os.close(self.master)


def synthesize(ffmpeg, path, frequency, rate, channels, duration):
    subprocess.run([ffmpeg, "-nostdin", "-v", "error", "-f", "lavfi",
                    "-i", f"sine=frequency={frequency}:sample_rate={rate}:duration={duration}",
                    "-ac", str(channels), path], check=True, timeout=10)


def inspect(ffprobe, path):
    return json.loads(subprocess.check_output(
        [ffprobe, "-v", "error", "-show_streams", "-show_format", "-show_chapters", "-of", "json", path],
        text=True, timeout=10))


def tone(ffmpeg, path, start):
    audio = subprocess.check_output(
        [ffmpeg, "-v", "error", "-i", path, "-ss", str(start), "-t", "0.3", "-map", "0:a:0",
         "-ar", "8000", "-ac", "1", "-c:a", "pcm_s16le", "-f", "s16le", "-"], timeout=10)
    samples = [sample for (sample,) in struct.iter_unpack("<h", audio)]
    return sum(a < 0 <= b for a, b in zip(samples, samples[1:])) / 0.3


def main():
    ffmpeg, ffprobe = shutil.which("ffmpeg"), shutil.which("ffprobe")
    if not ffmpeg or not ffprobe:
        print("SKIP simplemp3 audio check (FFmpeg and ffprobe unavailable)")
        return
    with tempfile.TemporaryDirectory(prefix="simplemp3-check-") as temp:
        root = Path(temp)
        synthesize(ffmpeg, root / "first.wav", 440, 44100, 1, 1.2)
        synthesize(ffmpeg, root / "second.m4a", 880, 48000, 2, 0.9)
        command_dir = root / "bin"
        command_dir.mkdir()
        for name, path in (("python3", sys.executable), ("ffmpeg", ffmpeg), ("ffprobe", ffprobe)):
            (command_dir / name).symlink_to(path)
        stub = command_dir / "yt-dlp"
        stub.write_text(f"#!{sys.executable}\n" + FIXTURE)
        stub.chmod(0o755)
        output = root / "out"
        env = dict(os.environ, PATH=str(command_dir),
                   TERM="xterm-256color", XDG_CACHE_HOME=str(root / "cache"),
                   SIMPLEMP3_TEST_AUDIO=str(root), LC_ALL="en_US.UTF-8")
        missing_js = subprocess.run([APP, "--prepare", "--no-update"], env=env,
                                    capture_output=True, text=True, timeout=10)
        assert missing_js.returncode == 1 and "JavaScript runtime is required" in missing_js.stderr
        node = command_dir / "node"
        node.write_text("#!/bin/sh\nprintf 'v22.1.0\\n'\n")
        node.chmod(0o755)
        prepared = subprocess.check_output([APP, "--prepare", "--no-update"], env=env, text=True, timeout=10)
        assert "Node.js 22.1.0" in prepared
        terminal = Terminal(env, output)
        try:
            terminal.expect("Ready")
            terminal.paste(SINGLE)
            terminal.expect(SINGLE)
            assert not output.exists(), "A newline in pasted text submitted the job"
            terminal.send("\n")
            terminal.expect("50%")
            terminal.expect("2.0 MiB/s")
            terminal.expect("Saved MP3")
            first = output / "First [AAAAAAAAAAA].mp3"
            single = inspect(ffprobe, first)
            assert [stream["codec_name"] for stream in single["streams"]] == ["mp3", "mjpeg"]
            assert single["format"]["tags"]["artist"] == "Test Publisher"
            assert single["streams"][0]["sample_rate"] == "44100"
            before = first.read_bytes()

            terminal.paste(PLAYLIST)
            terminal.send("\n")
            terminal.expect("Downloading playlist")
            terminal.expect("Track ETA")
            terminal.expect("Saved MP3")
            merged = output / "Test Playlist [PLfixture].mp3"
            listing = inspect(ffprobe, merged)
            chapter_titles = [chapter["tags"]["title"] for chapter in listing["chapters"]]
            assert chapter_titles == ["First", r"Second = # ; \ Movement", "First"], chapter_titles
            durations = [(float(c["start_time"]), float(c["end_time"])) for c in listing["chapters"]]
            assert abs(durations[0][1] - 1.2) < 0.01
            assert abs(durations[1][1] - 2.1) < 0.03 and durations[1][1] == durations[2][0]
            assert abs(float(listing["format"]["duration"]) - 3.3) < 0.08
            frequencies = [tone(ffmpeg, merged, at) for at in (0.2, 1.4, 2.4)]
            assert all(abs(actual - expected) < 15
                       for actual, expected in zip(frequencies, (440, 880, 440))), frequencies

            terminal.send("\x1bOQ")  # F2 downloads only the video from the watch URL.
            terminal.expect("Saved MP3")
            assert first.read_bytes() == before
            assert (output / "First [AAAAAAAAAAA] (2).mp3").is_file()

            terminal.paste(RESTRICTED)
            terminal.send("\n")
            terminal.expect("YouTube sign-in required")
            terminal.send("\x1b[17~")  # F6
            terminal.expect("YouTube account")
            terminal.send("\x1bOB\n")  # Select Firefox, then retry the same URL.
            terminal.expect("Firefox selected")
            terminal.send("\n")
            terminal.expect("Saved MP3")
            assert (output / "Signed-in test [CCCCCCCCCCC].mp3").is_file()

            originals = set(output.iterdir())
            terminal.paste(BROKEN)
            terminal.send("\n")
            terminal.expect("Download failed")
            assert set(output.iterdir()) == originals

            terminal.paste(SLOW)
            terminal.send("\n")
            terminal.expect("50%")
            terminal.send("\x1b")
            terminal.expect("Cancelled")
            assert set(output.iterdir()) == originals
        finally:
            terminal.close()
    print("OK SimpleMP3 terminal paste, live progress, playlist order, metadata, auth, failure and cancel")


if __name__ == "__main__":
    main()
