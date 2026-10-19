#!/usr/bin/env python3
"""Exercise the real display-free daemon and its public mpv-style IPC API."""
from pathlib import Path
import http.server
import json
import os
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time

build = Path(sys.argv[1]).resolve()
core = build / "simpleplay-core"
media = build / "simpleplay-fixtures/song.flac"
film = build / "simpleplay-fixtures/film.mkv"
env = dict(os.environ, SDL_AUDIODRIVER="dummy")
for key in ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS"):
    env.pop(key, None)


def wait_for(predicate, message, seconds=4):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except AssertionError:
            pass
        time.sleep(0.02)
    raise AssertionError(message)


class Client:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(3)
        self.socket.connect(str(path))
        self.file = self.socket.makefile("rb")
        self.serial = 0
        self.events = []

    def receive(self, request_id):
        while True:
            line = self.file.readline()
            assert line, "IPC connection unexpectedly closed"
            response = json.loads(line)
            if "event" in response:
                self.events.append(response)
            elif response.get("request_id") == request_id:
                return response

    def command(self, *command, success=True):
        self.serial += 1
        self.socket.sendall((json.dumps({"command": command, "request_id": self.serial}) + "\n").encode())
        response = self.receive(self.serial)
        assert (response["error"] == "success") == success, response
        return response.get("data")

    def close(self):
        self.file.close()
        self.socket.close()


with tempfile.TemporaryDirectory(prefix="simpleplay-core-", dir=build) as temporary:
    directory = Path(temporary)
    path = directory / "player.sock"
    log = (directory / "daemon.log").open("w+")
    process = subprocess.Popen([core, "--idle=yes", "--no-video", "--no-audio-display", "--force-window=no",
        "--no-terminal", "--cache=yes", "--network-timeout=15", "--volume=72", f"--input-ipc-server={path}"],
        env=env, stdout=subprocess.PIPE, stderr=log)
    try:
        wait_for(lambda: path.exists() or process.poll() is not None, "IPC socket did not appear")
        assert process.poll() is None
        assert stat.S_IMODE(path.stat().st_mode) == 0o600
        client = Client(path)
        assert client.command("get_property", "idle-active") is True
        assert client.command("get_property", "volume") == 72
        client.command("loadfile", str(film))
        wait_for(lambda: client.command("get_property", "duration", success=True) > 6, "Media duration unavailable")
        client.command("set_property", "pause", True)
        wait_for(lambda: client.command("get_property", "pause") is True, "Pause failed")
        first = client.command("get_property", "time-pos")
        time.sleep(0.1)
        assert client.command("get_property", "time-pos") == first
        client.command("seek", 1.25, "absolute+exact")
        wait_for(lambda: abs(client.command("get_property", "time-pos") - 1.25) < 0.05, "Paused seek failed")
        client.command("set_property", "volume", 130)
        assert client.command("get_property", "volume") == 130
        client.command("cycle", "mute")
        assert client.command("get_property", "mute") is True
        client.command("cycle", "mute")
        assert client.command("get_property", "volume") == 130
        client.command("observe_property", 7, "time-pos")
        client.command("cycle", "pause")
        time.sleep(0.2)
        assert client.command("get_property", "time-pos") > 1.3
        assert any(e.get("event") == "property-change" and e.get("id") == 7 for e in client.events)
        client.command("unobserve_property", 7)
        client.command("loadfile", str(media), "append")
        assert client.command("get_property", "playlist-count") == 2
        client.command("playlist-next")
        wait_for(lambda: client.command("get_property", "media-title") == "Evening light", "Playlist next failed")
        assert client.command("get_property", "metadata")["artist"] == "SimplePlay"
        assert len(client.command("get_property", "track-list")) == 1
        client.command("get_property", "does-not-exist", success=False)
        client.command("set_property", "volume", {"invalid": True}, success=False)
        client.command("seek", [], success=False)
        client.command("not-a-command", success=False)
        # Multiple requests in one write, exactly like SimpleRadio's metadata poll.
        requests = [json.dumps({"command": ["get_property", name], "request_id": 900 + n})
                    for n, name in enumerate(("metadata/icy-title", "media-title", "playback-time", "time-pos", "paused-for-cache"))]
        client.socket.sendall(("\n".join(requests) + "\n").encode())
        replies = [client.receive(900 + n) for n in range(5)]
        assert replies[1]["data"] == "Evening light" and isinstance(replies[-1]["data"], bool)
        # A fragmented command must wait for its terminating newline.
        client.socket.sendall(b'{"command":["get_property",')
        client.socket.sendall(b'"volume"],"request_id":999}\n')
        assert client.receive(999)["error"] == "success"
        client.socket.sendall(b'not json\n')
        assert json.loads(client.file.readline())["error"] == "invalid JSON"
        client.command("stop")
        assert client.command("get_property", "idle-active") is True
        # A hostile/buggy client cannot fill unbounded memory or kill playback.
        oversized = Client(path)
        oversized.socket.sendall(b"x" * 70000)
        oversized.close()
        assert client.command("get_property", "volume") == 130
        client.command("quit")
        client.close()
        process.wait(timeout=4)
        assert process.returncode == 0 and not path.exists()
        log.seek(0)
        assert not log.read(), "Daemon emitted warnings/errors"
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=4)
        log.close()
    # Display-free GUI entry point and CUE-style start/end boundaries.
    started = time.monotonic()
    result = subprocess.run([build / "simpleplay", "--headless", "--start=0.5", "--end=0.8", media], env=env, capture_output=True, timeout=4)
    assert result.returncode == 0, result.stderr
    assert 0.15 < time.monotonic() - started < 2
    raw = directory / "headerless.pcm"
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", media, "-t", "0.3", "-ar", "32000", "-ac", "1",
        "-f", "s24be", raw], check=True)
    result = subprocess.run([core, "--input-format=s24be", "--sample-rate=32000", "--channels=1", raw], env=env, capture_output=True, timeout=4)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([core, "--vo=null", "--ao=null", build / "simpleplay-fixtures/silent.mp4"], env=env, capture_output=True, timeout=4)
    assert result.returncode == 0, result.stderr
    # Existing files at the socket path are never removed/replaced.
    path.write_text("unrelated file")
    result = subprocess.run([core, "--idle=yes", f"--input-ipc-server={path}"], env=env, capture_output=True, timeout=4)
    assert result.returncode != 0 and path.read_text() == "unrelated file"
    result = subprocess.run([core, directory / "missing.flac"], env=env, capture_output=True, timeout=4)
    assert result.returncode == 1 and b"Cannot open media" in result.stderr
    # Signals exit cleanly and remove the owned socket.
    path.unlink()
    process = subprocess.Popen([core, "--idle", f"--input-ipc-server={path}"], env=env)
    wait_for(path.exists, "Idle socket did not appear")
    process.terminate()
    process.wait(timeout=3)
    assert process.returncode == 0 and not path.exists()
    # A real HTTP/ICY stream provides live metadata for SimpleRadio.
    radio = directory / "radio.mp3"
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", media, "-c:a", "libmp3lame", radio], check=True)
    audio = radio.read_bytes()
    metadata = b"StreamTitle='SimplePlay live title';"
    metadata = metadata.ljust((len(metadata) + 15) // 16 * 16, b"\0")
    body = bytearray()
    for n in range(0, len(audio), 1024):
        part = audio[n:n + 1024]
        body.extend(part)
        if len(part) == 1024:
            body.extend(bytes([len(metadata) // 16]) + metadata)

    class Radio(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "audio/mpeg")
            self.send_header("icy-metaint", "1024")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except BrokenPipeError:
                pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Radio)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    process = subprocess.Popen([core, "--idle=yes", f"--input-ipc-server={path}",
        f"http://127.0.0.1:{server.server_port}/radio.mp3"], env=env, stderr=subprocess.PIPE)
    try:
        wait_for(path.exists, "Radio socket did not appear")
        client = Client(path)
        wait_for(lambda: client.command("get_property", "metadata/icy-title") == "SimplePlay live title", "ICY title was not exposed")
        assert client.command("get_property", "media-title") == "SimplePlay live title"
        client.command("quit")
        client.close()
        process.wait(timeout=4)
        assert process.returncode == 0
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=4)
        server.shutdown()
        server.server_close()
print("SimplePlay headless checks passed: no display/bus, JSON IPC, observers, batch/partial requests, pause/seek/volume, queue, CUE boundaries, signals, socket safety")
