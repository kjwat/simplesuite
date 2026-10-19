#!/usr/bin/env python3
"""Real media/GUI checks. No downloads or writes outside the selected build dir."""
from pathlib import Path
import http.server
import os
import re
import shutil
import socketserver
import subprocess
import sys
import tempfile
import threading
import time


def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def fixtures(directory):
    directory.mkdir(parents=True, exist_ok=True)
    sub = directory / "captions.srt"
    sub.write_text("1\n00:00:00,250 --> 00:00:03,500\nSimplePlay subtitle check\n\n2\n00:00:04,000 --> 00:00:05,900\nThe last caption\n")
    meta = directory / "chapters.txt"
    meta.write_text(";FFMETADATA1\ntitle=SimplePlay test film\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=3000\ntitle=Opening\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=3000\nEND=6000\ntitle=Finale\n")
    base = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y"]
    run(base + ["-f", "lavfi", "-i", "testsrc2=size=320x180:rate=24:duration=6",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=6",
        "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000:duration=6",
        "-i", sub, "-i", meta, "-map", "0:v", "-map", "1:a", "-map", "2:a", "-map", "3:s",
        "-map_metadata", "4", "-map_chapters", "4", "-c:v", "libx264", "-preset", "veryfast", "-g", "48", "-bf", "3",
        "-c:a:0", "flac", "-c:a:1", "libopus", "-c:s", "srt", "-disposition:s:0", "default",
        "-metadata:s:a:0", "title=Main audio", "-metadata:s:a:1", "title=Alternative audio",
        directory / "film.mkv"])
    run(base + ["-f", "lavfi", "-i", "sine=frequency=330:duration=4", "-metadata", "title=Evening light",
        "-metadata", "artist=SimplePlay", "-metadata", "album=Test sessions", directory / "song.flac"])
    run(base + ["-f", "lavfi", "-i", "testsrc2=size=160x90:rate=25:duration=1", "-c:v", "libx264",
        "-preset", "veryfast", "-an", directory / "silent.mp4"])
    run(base + ["-f", "lavfi", "-i", "testsrc2=size=160x90:rate=25:duration=2",
        "-f", "lavfi", "-i", "sine=frequency=440:duration=0.4", "-c:v", "libx264", "-preset", "veryfast",
        "-c:a", "flac", directory / "trailing-video.mkv"])
    run(base + ["-f", "lavfi", "-i", "testsrc2=size=160x90:rate=25:duration=1.5",
        "-itsoffset", "0.75", "-f", "lavfi", "-i", "sine=frequency=440:duration=0.5",
        "-c:v", "libx264", "-preset", "veryfast", "-c:a", "flac", directory / "delayed-audio.mkv"])
    album = directory / "album"
    album.mkdir(exist_ok=True)
    shutil.copyfile(directory / "song.flac", album / "2. song.flac")
    shutil.copyfile(directory / "film.mkv", album / "10. film.mkv")
    shutil.copyfile(directory / "song.flac", album / "20. extensionless")
    (album / "notes.txt").write_text("Not media\n")
    (directory / "album.m3u8").write_text("#EXTM3U\nalbum/2. song.flac\nalbum/10. film.mkv\n")


def gui(build, directory):
    env = os.environ.copy()
    env.update(SDL_AUDIODRIVER="dummy", GDK_BACKEND="x11", NO_AT_BRIDGE="1", GTK_USE_PORTAL="0", GIO_USE_VFS="local",
               XDG_CONFIG_HOME=str(directory / "gui-config"), XDG_CACHE_HOME=str(directory / "gui-cache"))
    config = directory / "gui-config"
    config.mkdir(exist_ok=True)
    (config / "user-dirs.dirs").write_text(f'XDG_PICTURES_DIR="{directory}/Pictures"\n')
    log = directory / "gui.log"
    process = subprocess.Popen([str(build / "simpleplay")], env=env, stdout=log.open("w"), stderr=subprocess.STDOUT)

    def bus(method, *args):
        return subprocess.check_output(["gdbus", "call", "--session", "--dest", "org.mpris.MediaPlayer2.simpleplay",
            "--object-path", "/org/mpris/MediaPlayer2", "--method", method, *args], env=env, text=True, stderr=subprocess.DEVNULL).strip()

    def prop(name):
        return bus("org.freedesktop.DBus.Properties.Get", "org.mpris.MediaPlayer2.Player", name)

    def wait_for(predicate, message, seconds=6):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise AssertionError("GUI exited unexpectedly:\n" + log.read_text())
            try:
                if predicate():
                    return
            except subprocess.CalledProcessError:
                pass
            time.sleep(0.05)
        raise AssertionError(message + "\n" + log.read_text())

    def key(value):
        # A modal dialog owns keyboard events even without a window manager.
        # Address it directly instead of sending Escape to its parent window.
        dialog = subprocess.run(["xdotool", "search", "--onlyvisible",
            "--name", "^(Media information|Format support)$"],
            env=env, capture_output=True, text=True)
        target = dialog.stdout.splitlines()[-1] if dialog.returncode == 0 else window
        run(["xdotool", "windowfocus", "--sync", target, "key", "--clearmodifiers", value],
            env=env, stdout=subprocess.DEVNULL)
        time.sleep(0.18)

    def close_dialog(title):
        dialog = subprocess.check_output(["xdotool", "search", "--onlyvisible", "--name", "^" + title + "$"],
                                         env=env, text=True).splitlines()[-1]
        geometry = dict(line.split("=", 1) for line in subprocess.check_output(
            ["xdotool", "getwindowgeometry", "--shell", dialog], env=env, text=True).splitlines())
        run(["xdotool", "mousemove", "--window", dialog, str(int(geometry["WIDTH"]) - 50),
             str(int(geometry["HEIGHT"]) - 25), "click", "1"], env=env)
        wait_for(lambda: subprocess.run(["xdotool", "search", "--onlyvisible", "--name", "^" + title + "$"],
                                       env=env, capture_output=True).returncode != 0, title + " did not close")

    try:
        wait_for(lambda: "Stopped" in prop("PlaybackStatus"), "MPRIS did not become ready")
        window = subprocess.check_output(["xdotool", "search", "--onlyvisible", "--class", "simpleplay"], env=env, text=True).splitlines()[0]
        time.sleep(0.25)
        if shutil.which("import"):
            run(["import", "-window", window, directory / "simpleplay-empty.png"], env=env)
        run([build / "simpleplay", directory / "film.mkv"], env=env)
        wait_for(lambda: "Playing" in prop("PlaybackStatus"), "CLI file open did not play")
        wait_for(lambda: "SimplePlay test film" in prop("Metadata"), "Metadata did not reach the desktop")
        bus("org.mpris.MediaPlayer2.Player.Pause")
        wait_for(lambda: "Paused" in prop("PlaybackStatus"), "Pause failed")
        position = prop("Position")
        time.sleep(0.15)
        assert position == prop("Position"), "Paused GUI position moved"
        key("ctrl+e")
        key("f")
        key("Escape")
        run([build / "simpleplay", "--enqueue", directory / "song.flac"], env=env)
        wait_for(lambda: "true" in prop("CanGoNext"), "Enqueue did not update the queue")
        assert "SimplePlay test film" in prop("Metadata"), "Enqueue replaced the current file"
        bus("org.mpris.MediaPlayer2.Player.Next")
        wait_for(lambda: "Evening light" in prop("Metadata"), "Queue next failed")
        bus("org.mpris.MediaPlayer2.Player.Pause")
        wait_for(lambda: "Paused" in prop("PlaybackStatus"), "Audio pause failed")
        if shutil.which("import"):
            run(["import", "-window", window, directory / "simpleplay-audio.png"], env=env)
        bus("org.mpris.MediaPlayer2.Player.Previous")
        wait_for(lambda: "SimplePlay test film" in prop("Metadata"), "Queue previous failed")
        bus("org.mpris.MediaPlayer2.Player.Pause")
        key("ctrl+i")
        close_dialog("Media information")
        key("ctrl+shift+i")
        key("s")
        key("i")
        key("d")
        if shutil.which("import"):
            run(["import", "-window", "root", directory / "simpleplay-formats.png"], env=env)
        close_dialog("Format support")
        # Inspect the actual options popover and take a video screenshot.
        run(["xdotool", "mousemove", "--window", window, "880", "27", "click", "1"], env=env)
        time.sleep(0.25)
        if shutil.which("import"):
            run(["import", "-window", "root", directory / "simpleplay-options.png"], env=env)
        key("Escape")
        if shutil.which("import"):
            run(["import", "-window", window, directory / "simpleplay-video.png"], env=env)
        previous_frames = set((directory / "Pictures/SimplePlay").glob("*.png"))
        key("ctrl+s")
        wait_for(lambda: bool(set((directory / "Pictures/SimplePlay").glob("*.png")) - previous_frames), "Frame was not saved in the configured Pictures directory")
        run([build / "simpleplay", directory / "album"], env=env)
        wait_for(lambda: "Evening light" in prop("Metadata"), "Folder discovery/natural sorting failed")
        bus("org.mpris.MediaPlayer2.Player.Next")
        wait_for(lambda: "SimplePlay test film" in prop("Metadata"), "Folder queue failed")
        bus("org.mpris.MediaPlayer2.Player.Next")
        wait_for(lambda: "Evening light" in prop("Metadata"), "Extensionless media was skipped during folder discovery")
        run([build / "simpleplay", directory / "album.m3u8"], env=env)
        wait_for(lambda: "2.%20song.flac" in prop("Metadata"), "Relative playlist entries failed")
        key("ctrl+shift+i")
        key("s")
        bus("org.mpris.MediaPlayer2.Quit")
        process.wait(timeout=5)
        assert process.returncode == 0, log.read_text()
        output = log.read_text()
        assert not re.search(r"(CRITICAL|ERROR|WARNING).*", output), output
        print("SimplePlay GUI checks passed: file open, MPRIS, pause, enqueue, navigation, fullscreen, screenshots")
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)


class Stalled(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        time.sleep(4)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    build = Path(sys.argv[1]).resolve()
    directory = build / "simpleplay-fixtures"
    if len(sys.argv) > 2 and sys.argv[2] == "--gui":
        gui(build, directory)
    else:
        fixtures(directory)
        server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Stalled)
        server.daemon_threads = True
        threading.Thread(target=server.serve_forever, daemon=True).start()
        env = os.environ.copy()
        env["SDL_AUDIODRIVER"] = "dummy"
        ape = os.environ.get("SIMPLEPLAY_APE_SAMPLE", "")
        run([build / "simpleplay-engine-check", directory / "film.mkv", directory / "song.flac",
             directory / "captions.srt", directory / "silent.mp4", ape,
             f"http://127.0.0.1:{server.server_address[1]}/stalled",
             directory / "trailing-video.mkv", directory / "delayed-audio.mkv"], env=env, timeout=45)
        server.shutdown()
        if all(shutil.which(tool) for tool in ("xvfb-run", "dbus-run-session", "gdbus", "xdotool")):
            run(["dbus-run-session", "--", "xvfb-run", "-a", "-s", "-screen 0 1440x1000x24", sys.executable,
                 Path(__file__).resolve(), build, "--gui"], timeout=40)
        else:
            print("GUI checks skipped: install Xvfb, xdotool and dbus tools to run them")
