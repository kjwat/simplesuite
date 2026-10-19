#!/usr/bin/env python3
"""Hold the picture through real seeks served by a slow local range server."""
from pathlib import Path
import http.server
import os
import resource
import shutil
import subprocess
import sys
import tempfile
import threading
import time

build = Path(sys.argv[1]).resolve()
for tool in ("ffmpeg", "xvfb-run", "dbus-run-session"):
    if not shutil.which(tool):
        sys.exit(f"Install {tool} to run SimplePlay presentation checks.")
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

with tempfile.TemporaryDirectory(prefix="simpleplay-presentation-", dir=build) as temporary:
    directory = Path(temporary)
    media, audio, armed = directory / "video.avi", directory / "audio.wav", directory / "delay"
    ffmpeg = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y"]
    subprocess.run(ffmpeg + ["-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=24",
        "-f", "lavfi", "-i", "sine=frequency=440:duration=24", "-c:v", "mjpeg", "-q:v", "2",
        "-pix_fmt", "yuvj420p", "-c:a", "pcm_s16le", str(media)], check=True, timeout=15)
    subprocess.run(ffmpeg + ["-f", "lavfi", "-i", "sine=duration=1", str(audio)], check=True, timeout=10)
    delayed_ranges = []

    class SlowShare(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            size = media.stat().st_size
            header = self.headers.get("Range")
            start, end = 0, size - 1
            if header:
                bounds = header.removeprefix("bytes=").split("-", 1)
                start = int(bounds[0])
                if bounds[1]:
                    end = min(int(bounds[1]), end)
            self.send_response(206 if header else 200)
            self.send_header("Content-Type", "video/x-msvideo")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(end - start + 1))
            self.send_header("Connection", "close")
            if header:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()
            if armed.exists():
                delayed_ranges.append(start)
                time.sleep(0.8)
            try:
                with media.open("rb") as source:
                    source.seek(start)
                    remaining = end - start + 1
                    while remaining:
                        block = source.read(min(65536, remaining))
                        if not block:
                            break
                        self.wfile.write(block)
                        remaining -= len(block)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def log_message(self, *args):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), SlowShare)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    bus_config = directory / "session.conf"
    bus_config.write_text('''<busconfig>
<type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth>
<policy context="default"><allow own="*"/><allow send_destination="*"/>
<allow receive_sender="*"/></policy></busconfig>''')
    env = dict(os.environ, GDK_BACKEND="x11", SDL_AUDIODRIVER="dummy", NO_AT_BRIDGE="1",
               GTK_USE_PORTAL="0", GIO_USE_VFS="local", G_DEBUG="fatal-warnings", GSETTINGS_BACKEND="memory",
               XDG_CONFIG_HOME=str(directory / "config"), XDG_CACHE_HOME=str(directory / "cache"))
    try:
        subprocess.run(["dbus-run-session", f"--config-file={bus_config}", "--", "xvfb-run", "-a",
            str(build / "simpleplay-presentation-check"), f"http://127.0.0.1:{server.server_port}/video.avi",
            str(audio), str(armed)], env=env, check=True, timeout=30)
        assert len(delayed_ranges) >= 3, "Seeks did not exercise delayed network reads"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
