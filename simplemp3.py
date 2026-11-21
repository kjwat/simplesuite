#!/usr/bin/env python3
"""SimpleSuite's terminal YouTube-to-MP3 downloader."""

import argparse
import curses
from dataclasses import dataclass
from collections import deque
from contextlib import ExitStack
import errno
import fcntl
import hashlib
import json
import locale
import os
from pathlib import Path
import platform
from queue import Empty, SimpleQueue
import re
import selectors
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unicodedata
from urllib.parse import parse_qs, quote, urlsplit
from urllib.request import Request, urlopen
from zipfile import ZipFile


VERSION = "1.0.0"
INSTALLED = Path("/usr/local/bin/simplemp3")
APP_REPO = "kjwat/simplesuite"
SUPPORT_URL = "https://github.com/yt-dlp/yt-dlp/wiki/EJS"
RELEASE_NAME = re.compile(r"simplemp3-(\d+\.\d+\.\d+)")
VIDEO_ID = re.compile(r"[\w-]{11}", re.ASCII)
PASTE_START, PASTE_END = "\x1b[200~", "\x1b[201~"
BROWSERS = ("firefox", "chrome", "chromium", "brave", "edge", "safari", "opera", "vivaldi", "whale")


class Cancelled(Exception):
    pass


class Work:
    """Run cancellable subprocesses and send status to the curses thread."""

    def __init__(self, send):
        self.send = send
        self.cancel = threading.Event()

    def check(self):
        if self.cancel.is_set():
            raise Cancelled()

    def log(self, line):
        self.send("log", line)

    def status(self, phase, detail="", fraction=None, metrics=""):
        self.send("status", (phase, detail, fraction, metrics))

    def run(self, command, *, cwd=None, on_line=None, timeout=None):
        self.check()
        deadline = time.monotonic() + timeout if timeout else None
        lines = deque(maxlen=50)

        def read(line):
            line = line.rstrip(b"\r").decode("utf-8", "replace")
            if line and not (on_line and on_line(line)):
                lines.append(line)

        with subprocess.Popen(
            [str(arg) for arg in command], cwd=cwd, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True,
        ) as child, selectors.DefaultSelector() as selector:
            selector.register(child.stdout, selectors.EVENT_READ)
            pending = b""
            try:
                while True:
                    self.check()
                    if deadline and time.monotonic() >= deadline:
                        raise RuntimeError(f"{Path(command[0]).name} timed out")
                    if not selector.select(0.15):
                        continue
                    data = os.read(child.stdout.fileno(), 65536)
                    if not data:
                        if pending:
                            read(pending)
                        break
                    *complete, pending = (pending + data).split(b"\n")
                    for line in complete:
                        read(line)
                while child.poll() is None:
                    self.check()
                    if deadline and time.monotonic() >= deadline:
                        raise RuntimeError(f"{Path(command[0]).name} timed out")
                    self.cancel.wait(0.05)
            except BaseException:
                try:
                    os.killpg(child.pid, signal.SIGTERM)
                    time.sleep(0.15)
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                raise
            if child.returncode:
                context = next((line for line in reversed(lines) if line.startswith("ERROR:")),
                               lines[-1] if lines else "")
                raise RuntimeError(context or f"{Path(command[0]).name} exited with status {child.returncode}")
        return list(lines)


def release(repo, work, tag=None):
    work.check()
    endpoint = "latest" if tag is None else f"tags/{quote(tag, safe='')}"
    url = f"https://api.github.com/repos/{repo}/releases/{endpoint}"
    request = Request(url, headers={"User-Agent": f"SimpleMP3/{VERSION}"})
    with urlopen(request, timeout=6) as response:
        result = json.load(response)
    work.check()
    return result


def release_asset(repo, info, asset, target, work):
    name = asset["name"]
    digest = asset.get("digest") or ""
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
        raise RuntimeError(f"Missing release checksum for {name}")
    tag = quote(info["tag_name"], safe="")
    url = f"https://github.com/{repo}/releases/download/{tag}/{quote(name, safe='')}"
    request = Request(url, headers={"User-Agent": f"SimpleMP3/{VERSION}"})
    checksum = hashlib.sha256()
    deadline = time.monotonic() + 300
    with urlopen(request, timeout=10) as response, target.open("wb") as file:
        received = 0
        size = asset["size"]
        while chunk := response.read1(256 * 1024):
            work.check()
            received += len(chunk)
            if received > size:
                raise RuntimeError(f"Release file is larger than expected for {name}")
            if time.monotonic() > deadline:
                raise RuntimeError(f"Download timed out while receiving {name}")
            checksum.update(chunk)
            file.write(chunk)
            work.status("Updating components", f"Downloading {name}", received / size,
                        f"{received / 1048576:.1f} / {size / 1048576:.1f} MiB")
    if received != size or "sha256:" + checksum.hexdigest() != digest:
        raise RuntimeError(f"Release checksum mismatch for {name}")


def version_number(text):
    match = re.search(r"(?<!\d)\d+\.\d+\.\d+(?:\.\d+)?", text)
    return tuple(map(int, match[0].split("."))) if match else ()


def version_text(path, work):
    if path:
        try:
            lines = work.run([path, "--version"], timeout=4)
            if lines:
                match = re.search(r"(?<!\d)\d+\.\d+\.\d+(?:\.\d+)?", lines[0])
                if match:
                    return match[0]
        except (OSError, RuntimeError):
            pass
    return ""


def update_tool(name, repo, artifact, cache, work, check_updates):
    cached = cache / name
    candidates = [(path, version_text(path, work)) for path in
                  (str(cached), shutil.which(name))]
    current, installed = max(candidates, key=lambda item: version_number(item[1]))
    if check_updates and artifact:
        try:
            info = release(repo, work)
            latest = version_number(info["tag_name"])
            if not latest:
                raise RuntimeError(f"Unrecognized {name} release version")
            if latest > version_number(installed):
                asset = next((a for a in info["assets"] if a["name"] == artifact), None)
                if asset is None:
                    raise RuntimeError(f"Release asset {artifact} is unavailable")
                with tempfile.TemporaryDirectory(prefix=f".{name}-", dir=cache) as directory:
                    staged = Path(directory) / name
                    download = staged.with_suffix(".zip") if artifact.endswith(".zip") else staged
                    release_asset(repo, info, asset, download, work)
                    if download != staged:
                        with ZipFile(download) as archive, archive.open(name) as binary, staged.open("wb") as file:
                            shutil.copyfileobj(binary, file)
                    staged.chmod(0o755)
                    staged_version = version_text(staged, work)
                    if version_number(staged_version) != latest:
                        raise RuntimeError(f"Could not verify the new {name} executable")
                    work.check()
                    staged.replace(cached)
                    current, installed = str(cached), staged_version
                    work.log(f"Updated {name} to {installed}")
        except (OSError, RuntimeError, ValueError, KeyError) as error:
            work.log(f"{name} update: {error}")
            return (current if installed else None), installed, True
    return (current if installed else None), installed, False


def app_asset(info):
    candidates = [asset for asset in info["assets"] if RELEASE_NAME.fullmatch(asset["name"])]
    return max(candidates, key=lambda asset: version_number(asset["name"]), default=None)


def apply_app_release(tag, work):
    if not os.access(INSTALLED.parent, os.W_OK) or Path(__file__).resolve() != INSTALLED:
        raise RuntimeError("The system SimpleMP3 executable is required to install an update")
    info = release(APP_REPO, work, tag)
    asset = app_asset(info)
    if not asset or version_number(asset["name"]) <= version_number(VERSION):
        return
    with tempfile.TemporaryDirectory(prefix=".simplemp3-update-", dir=INSTALLED.parent) as directory:
        staged = Path(directory) / "simplemp3"
        release_asset(APP_REPO, info, asset, staged, work)
        source = staged.read_bytes()
        version = RELEASE_NAME.fullmatch(asset["name"])[1]
        if not source.startswith(b"#!/usr/bin/env python3\n") or \
                not re.search(rb'^VERSION = "' + re.escape(version.encode()) + rb'"$', source, re.M):
            raise RuntimeError("SimpleMP3 release version did not match the executable")
        compile(source, str(INSTALLED), "exec")
        staged.chmod(0o755)
        work.check()
        staged.replace(INSTALLED)


def update_app(work):
    if Path(__file__).resolve() != INSTALLED:
        return ""
    try:
        info = release(APP_REPO, work)
        asset = app_asset(info)
        if asset and version_number(asset["name"]) > version_number(VERSION):
            tag = info["tag_name"]
            if os.access(INSTALLED.parent, os.W_OK):
                apply_app_release(tag, work)
            elif shutil.which("pkexec") and os.environ.get("DBUS_SESSION_BUS_ADDRESS"):
                work.status("Updating SimpleMP3", "Waiting for system installation authorization")
                work.run(["pkexec", "--disable-internal-agent", INSTALLED, "--apply-release", tag],
                         timeout=120)
            else:
                work.log(f"SimpleMP3 update available ({asset['name']}); system installation required")
                return "App update available"
            return "restart"
    except (OSError, RuntimeError, ValueError, KeyError, SyntaxError) as error:
        work.log(f"SimpleMP3 update: {error}")
        return "App update deferred"
    return ""


@dataclass
class Tools:
    downloader: str
    ffmpeg: str
    ffprobe: str
    js: tuple
    summary: str


def startup(work, check_updates, *, prepare=False):
    work.status("Checking components", "Checking yt-dlp, JavaScript and FFmpeg")
    update = update_app(work) if check_updates and not prepare else ""
    if update == "restart":
        return None

    ffmpeg, ffprobe = shutil.which("ffmpeg"), shutil.which("ffprobe")
    if not ffmpeg or not ffprobe:
        raise RuntimeError("Install the FFmpeg package with ffmpeg and ffprobe, then restart.")
    encoders = work.run([ffmpeg, "-hide_banner", "-encoders"], timeout=6,
                        on_line=lambda line: " libmp3lame " not in line)
    if not encoders:
        raise RuntimeError("FFmpeg needs the libmp3lame encoder. Install a full FFmpeg build, then restart.")
    work.run([ffprobe, "-version"], timeout=6)
    first = work.run([ffmpeg, "-version"], timeout=6)[0]
    ff_version = first.split()[2]

    cache = Path(os.environ.get("XDG_CACHE_HOME") or (Path.home() / ".cache")) / "simplemp3"
    cache.mkdir(mode=0o700, parents=True, exist_ok=True)
    cpu = {"amd64": "x86_64", "arm64": "aarch64"}.get(platform.machine(), platform.machine())
    os_target = {"Linux": "unknown-linux-gnu", "Darwin": "apple-darwin"}.get(platform.system())
    node = shutil.which("node")
    node_version = version_text(node, work)
    have_node = version_number(node_version) >= (22,)
    have_deno = (cache / "deno").is_file() or shutil.which("deno")
    # Alpine and Void can supply a native Node binary, including on musl.
    deno_asset = (f"deno-{cpu}-{os_target}.zip"
                  if os_target and cpu in {"x86_64", "aarch64"} and
                  (have_deno or not have_node) else None)

    with (cache / "update.lock").open("a") as lock:
        while True:
            work.check()
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                work.status("Checking components", "Waiting for another SimpleMP3 update")
                work.cancel.wait(0.15)
        downloader, yt_version, yt_error = update_tool(
            "yt-dlp", "yt-dlp/yt-dlp", "yt-dlp", cache, work, check_updates)
        deno, deno_version, deno_error = update_tool(
            "deno", "denoland/deno", deno_asset, cache, work, check_updates)

    if not downloader or version_number(yt_version) < (2025, 11, 12):
        raise RuntimeError("A current yt-dlp is required. Check connectivity to GitHub or update yt-dlp.")
    js, runtime = (), "No JavaScript runtime"
    if deno and version_number(deno_version) >= (2, 3, 0):
        js, runtime = ("--js-runtimes", f"deno:{deno}"), f"Deno {deno_version}"
    elif have_node:
        js, runtime = ("--js-runtimes", f"node:{node}"), f"Node.js {node_version}"
    elif qjs := shutil.which("qjs"):
        js, runtime = ("--js-runtimes", f"quickjs:{qjs}"), "QuickJS"
    else:
        if prepare:
            raise RuntimeError("A supported JavaScript runtime is required. Check connectivity to GitHub or install Node.js 22 or newer.")
        work.log(f"A JavaScript runtime unlocks more YouTube formats. {SUPPORT_URL}")

    summary = f"yt-dlp {yt_version}  ·  {runtime}  ·  FFmpeg {ff_version}"
    if yt_error or deno_error:
        summary += "  ·  Update check incomplete (F4)"
    if update:
        summary += f"  ·  {update} (F4)"
    return Tools(downloader, ffmpeg, ffprobe, js, summary)


def youtube_url(raw, video_only=False):
    value = raw.strip()
    if value.startswith(("www.youtube.com/", "youtube.com/", "m.youtube.com/", "youtu.be/")):
        value = "https://" + value
    try:
        url = urlsplit(value)
        host = url.hostname or ""
        if (len(value) > 8192 or any(char.isspace() or not char.isprintable() for char in value)
                or url.scheme not in {"http", "https"} or url.username is not None or url.port is not None
                or not (host == "youtu.be" or host == "youtube.com" or host.endswith(".youtube.com"))):
            raise ValueError
        query = parse_qs(url.query)
        playlist = query.get("list", [""])[0]
        parts = url.path.strip("/").split("/")
        video = ""
        if host == "youtu.be" and len(parts) == 1:
            video = parts[0]
        elif url.path == "/watch":
            video = query.get("v", [""])[0]
        elif len(parts) == 2 and parts[0] in {"shorts", "live", "embed", "v"}:
            video = parts[1]
        if VIDEO_ID.fullmatch(video):
            if playlist and not video_only:
                return f"https://www.youtube.com/watch?v={video}&list={quote(playlist, safe='')}"
            return f"https://www.youtube.com/watch?v={video}"
        if playlist and (url.path.rstrip("/") in {"/playlist", "/embed/videoseries", "/watch"}):
            if video_only:
                raise RuntimeError("This URL has no video. Press Enter to download the playlist.")
            return f"https://www.youtube.com/playlist?list={quote(playlist, safe='')}"
    except ValueError:
        pass
    raise RuntimeError("Paste a YouTube video or playlist URL.")


def dlp_base(tools, auth):
    return [tools.downloader, "--ignore-config", "--no-colors", "--no-plugin-dirs",
            "--socket-timeout", "15", "--retries", "3", "--fragment-retries", "3",
            "--extractor-retries", "2", *tools.js, "--remote-components", "ejs:github", *auth]


def number(value):
    try:
        return float(value or 0)
    except (ValueError, TypeError):
        return 0


def duration_label(seconds):
    seconds = max(0, int(seconds))
    hours, rest = divmod(seconds, 3600)
    minutes, seconds = divmod(rest, 60)
    return f"{hours}:{minutes:02}:{seconds:02}" if hours else f"{minutes}:{seconds:02}"


def speed_label(bytes_per_second):
    if bytes_per_second >= 1048576:
        return f"{bytes_per_second / 1048576:.1f} MiB/s"
    return f"{bytes_per_second / 1024:.0f} KiB/s"


def probe(url, tools, work, auth):
    work.status("Reading link", "Identifying video or playlist")
    items = []

    def receive(line):
        if line.startswith("{"):
            items.append(json.loads(line))
            return True
        work.log(line)
        return False

    work.run(dlp_base(tools, auth) + ["--flat-playlist", "--dump-single-json",
                                "--yes-playlist", "--", url], on_line=receive, timeout=180)
    if len(items) != 1:
        raise RuntimeError("YouTube returned no video or playlist details.")
    info = items[0]
    if info.get("is_live"):
        raise RuntimeError("This video is live. Use the archived video once the stream ends.")
    return info


def download(url, info, directory, tools, work, auth):
    entries = info.get("entries")
    playlist = entries is not None
    count = len(entries) if playlist else 1
    if not count:
        raise RuntimeError("This playlist has no visible tracks.")
    if playlist and any(not isinstance(entry, dict) or not entry.get("id") for entry in entries):
        raise RuntimeError("YouTube listed an unavailable entry in this playlist.")

    title = info.get("title") or ("YouTube playlist" if playlist else "YouTube video")
    work.send("title", title)
    tracks = []
    current = None
    work.status("Downloading playlist" if playlist else "Downloading audio",
                f"Resolving {'track 1 of ' + str(count) if playlist else 'the audio stream'}", 0)
    phase = "Downloading playlist" if playlist else "Downloading audio"

    def receive(line):
        nonlocal current
        if line.startswith("@track "):
            current = json.loads(line[7:])
            expected = entries[len(tracks)] if playlist and len(tracks) < count else info
            if len(tracks) >= count or current.get("id") != expected.get("id"):
                raise RuntimeError("The playlist changed while downloading. Press Enter to retry.")
            detail = current.get("title") or title
            if playlist:
                detail = f"{len(tracks) + 1}/{count}  ·  {detail}"
            work.status(phase, detail, len(tracks) / count)
            return True
        if line.startswith("@progress ") and current:
            progress = json.loads(line[10:])
            if progress is None:
                return True
            percent = number((progress.get("_percent_str") or "").strip().rstrip("%"))
            rate = number(progress.get("speed"))
            eta = progress.get("eta")
            metrics = [speed_label(rate)] if rate else []
            if isinstance(eta, (int, float)) and eta >= 0:
                metrics.append(f"{'Track ' if playlist else ''}ETA {duration_label(eta)}")
            detail = current.get("title") or title
            if playlist:
                detail = f"{len(tracks) + 1}/{count}  ·  {detail}"
            work.status(phase, detail, (len(tracks) + min(100, percent) / 100) / count,
                        "   ·   ".join(metrics))
            return True
        if line.startswith("@file "):
            path = (directory / json.loads(line[6:])).resolve()
            if current is None or path.parent != directory or not path.is_file() or not path.stat().st_size:
                raise RuntimeError("The downloaded audio file is missing.")
            tracks.append((path, current))
            current = None
            if len(tracks) < count:
                work.status(phase, f"Resolving track {len(tracks) + 1} of {count}", len(tracks) / count)
            return True
        if not line.startswith("@progress "):
            work.log(line)
        return False

    command = dlp_base(tools, auth) + [
        "--yes-playlist" if playlist else "--no-playlist", "--no-simulate",
        "--abort-on-error", "--abort-on-unavailable-fragments", "--no-overwrites",
        "--format", "bestaudio/best", "--match-filter", "!is_live",
        "--newline", "--progress", "--progress-delta", "0.25",
        "--progress-template", "download:@progress %(progress.{_percent_str,speed,eta})j",
        "--print", "before_dl:@track %(.{id,title,artist,album,creator,uploader,upload_date,duration})j",
        "--print", "after_move:@file %(filepath)j",
        "--output", "%(playlist_autonumber,autonumber)06d.%(ext)s",
    ]
    if not playlist:
        command += ["--write-thumbnail", "--output", "thumbnail:art.%(ext)s"]
    work.run(command + ["--", url], cwd=directory, on_line=receive)
    if len(tracks) != count:
        raise RuntimeError(f"Received {len(tracks)} of {count} tracks. The job was incomplete.")
    return tracks


def ffmpeg_base(tools):
    return [tools.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error",
            "-xerror", "-y", "-stats_period", "0.25", "-progress", "pipe:1"]


def encode_progress(work, phase, detail, duration, offset=0, span=1):
    stats = {}

    def receive(line):
        if "=" not in line:
            work.log(line)
            return False
        key, value = line.split("=", 1)
        stats[key] = value
        if key == "progress":
            done = max(0, number(stats.get("out_time_us")) / 1000000)
            fraction = offset + span * min(1, done / duration) if duration else None
            if value == "end":
                fraction = offset + span
            metric = duration_label(done)
            if duration:
                metric += f" / {duration_label(duration)} audio"
            if stats.get("speed") not in (None, "N/A"):
                metric += f"   ·   {stats['speed'].strip()} speed"
            work.status(phase, detail, fraction, metric)
            stats.clear()
        return True

    work.status(phase, detail, offset if duration else None)
    return receive


def ffmetadata_text(value):
    if not isinstance(value, str):
        return ""
    return re.sub(r"([\\=;#])", r"\\\1", " ".join(value.splitlines())).replace("\x00", "")


def file_title(value):
    cleaned = "".join(char if char.isprintable() and char not in '/\\:*?"<>|' else " "
                      for char in value)
    return re.sub(r"\s+", " ", cleaned).strip(" .") or "YouTube audio"


def playlist_inputs(tracks, directory, tools, work):
    chapters = [";FFMETADATA1"]
    concat = ["ffconcat version 1.0"]
    elapsed = 0
    for index, (source, track) in enumerate(tracks, 1):
        target = directory / f"part-{index:06d}.flac"
        phase = "Preparing tracks"
        title = track.get("title") or f"Track {index}"
        detail = f"{index}/{len(tracks)}  ·  {title}"
        callback = encode_progress(work, phase, detail, number(track.get("duration")),
                                   (index - 1) / len(tracks), 1 / len(tracks))
        work.run(ffmpeg_base(tools) + [
            "-i", source, "-map", "0:a:0", "-map_metadata", "-1",
            "-ac", "2", "-ar", "48000", "-sample_fmt", "s32", "-c:a", "flac", target,
        ], on_line=callback)
        samples = work.run([tools.ffprobe, "-v", "error", "-select_streams", "a:0",
                            "-show_entries", "stream=duration_ts", "-of", "csv=p=0", target], timeout=10)
        if not samples or not samples[-1].isdigit() or int(samples[-1]) <= 0:
            raise RuntimeError(f"Could not measure track {index}.")
        length = int(samples[-1])
        concat += [f"file {target.name}", f"duration {length / 48000:.9f}"]
        chapters += ["[CHAPTER]", "TIMEBASE=1/48000", f"START={elapsed}",
                     f"END={elapsed + length}", f"title={ffmetadata_text(title)}"]
        elapsed += length
        source.unlink()
    (directory / "parts.ffconcat").write_text("\n".join(concat) + "\n", encoding="utf-8")
    (directory / "chapters.ffmeta").write_text("\n".join(chapters) + "\n", encoding="utf-8")
    return ["-f", "concat", "-i", directory / "parts.ffconcat",
            "-f", "ffmetadata", "-i", directory / "chapters.ffmeta"], elapsed / 48000


def publish(result, output, title, identity):
    safe_id = file_title(identity)[:40] if identity else ""
    suffix = f" [{safe_id}]" if safe_id else ""
    base = file_title(title).encode("utf-8")[:(220 - len(suffix.encode("utf-8")))]
    stem = base.decode("utf-8", "ignore").rstrip(" .") + suffix
    for index in range(1, 10000):
        extra = f" ({index})" if index > 1 else ""
        target = output / f"{stem}{extra}.mp3"
        try:
            os.link(result, target)
        except FileExistsError:
            continue
        except OSError as error:
            if error.errno not in (errno.EPERM, errno.EOPNOTSUPP, errno.ENOSYS, errno.EXDEV):
                raise
            # Filesystems without hard links can still reserve a name exclusively.
            try:
                with target.open("xb"):
                    pass
            except FileExistsError:
                continue
            try:
                result.replace(target)
            except OSError:
                target.unlink(missing_ok=True)
                raise
        return target
    raise RuntimeError("Too many files already use this name.")


def make_mp3(url, output, tools, work, auth=()):
    with ExitStack() as temporary:
        if auth and auth[0] == "--cookies":
            cookie_dir = Path(temporary.enter_context(tempfile.TemporaryDirectory(prefix="simplemp3-account-")))
            cookie_file = cookie_dir / "cookies.txt"
            shutil.copyfile(auth[1], cookie_file)
            auth = ("--cookies", str(cookie_file))
        info = probe(url, tools, work, auth)
        playlist = info.get("entries") is not None
        output.mkdir(parents=True, exist_ok=True)
        temp = temporary.enter_context(tempfile.TemporaryDirectory(prefix=".simplemp3-", dir=output))
        directory = Path(temp).resolve()
        tracks = download(url, info, directory, tools, work, auth)
        title = info.get("title") or tracks[0][1].get("title") or "YouTube audio"
        mapping = ["-map", "0:a:0"]
        tags = {"title": title}
        if playlist:
            inputs, duration = playlist_inputs(tracks, directory, tools, work)
            mapping += ["-map_chapters", "1", "-map_metadata:g", "-1"]
            tags.update(album=title, artist=info.get("uploader"))
        else:
            source, track = tracks[0]
            inputs, duration = ["-i", source], number(track.get("duration"))
            tags.update(artist=track.get("artist") or track.get("creator") or track.get("uploader"),
                        album=track.get("album"), date=(track.get("upload_date") or "")[:4])
            thumbnail = next(directory.glob("art.*"), None)
            if thumbnail:
                cover = directory / "cover.jpg"
                try:
                    work.run([tools.ffmpeg, "-nostdin", "-v", "error", "-y", "-i", thumbnail,
                              "-frames:v", "1", "-f", "image2", "-update", "1", cover], timeout=15)
                    inputs += ["-i", cover]
                    mapping += ["-map", "1:v:0", "-c:v", "copy", "-disposition:v:0", "attached_pic",
                                "-metadata:s:v:0", "comment=Cover (front)"]
                except (OSError, RuntimeError) as error:
                    work.log(f"Cover image: {error}")
        metadata = []
        for key, value in tags.items():
            if isinstance(value, str) and value:
                metadata += ["-metadata", f"{key}={value.replace(chr(0), '')}"]
        result = directory / "finished.mp3"
        work.run(ffmpeg_base(tools) + inputs + mapping + metadata + [
            "-c:a", "libmp3lame", "-q:a", "0", "-id3v2_version", "3", result,
        ], on_line=encode_progress(work, "Encoding MP3", title, duration))
        work.check()
        if not result.is_file() or not result.stat().st_size:
            raise RuntimeError("FFmpeg did not produce a playable MP3.")
        return publish(result, output, title, info.get("id", ""))


def display_text(value):
    return "".join(char if char.isprintable() else " " for char in str(value))


def fit(value, limit):
    result = []
    used = 0
    for char in display_text(value):
        width = 0 if unicodedata.combining(char) else (
            2 if unicodedata.east_asian_width(char) in "WF" else 1)
        if used + width > limit:
            break
        result.append(char)
        used += width
    return "".join(result), used


def clipboard_url():
    commands = []
    if os.environ.get("WAYLAND_DISPLAY"):
        commands.append(["wl-paste", "--no-newline", "--type", "text"])
    if os.environ.get("DISPLAY"):
        commands += [["xclip", "-selection", "clipboard", "-out"], ["xsel", "-b", "-o"]]
    if platform.system() == "Darwin":
        commands.append(["pbpaste"])
    for command in commands:
        try:
            result = subprocess.run(command, capture_output=True, timeout=0.5, text=True,
                                    errors="replace", check=True)
            youtube_url(result.stdout)
            return result.stdout.strip()
        except (OSError, subprocess.SubprocessError, RuntimeError):
            continue
    return ""


def read_key(screen):
    try:
        key = screen.get_wch()
    except curses.error:
        return None
    if key != "\x1b":
        return key
    screen.timeout(12)
    extra = []
    try:
        for expected in "[20":
            received = screen.get_wch()
            extra.append(received)
            if received != expected:
                return key
        extra.append(screen.get_wch())
        if extra[-1] not in ("0", "1"):
            return key
        extra.append(screen.get_wch())
        sequence = key + "".join(str(char) for char in extra)
        if sequence in (PASTE_START, PASTE_END):
            extra.clear()
            return sequence
        return key
    except curses.error:
        return key
    finally:
        screen.timeout(90)
        for char in reversed(extra):
            curses.unget_wch(char)


class App:
    def __init__(self, args):
        self.args = args
        self.url = args.url or ""
        self.cursor = len(self.url)
        self.output = args.output_dir.expanduser().absolute()
        self.messages = SimpleQueue()
        self.logs = deque(maxlen=100)
        self.tools = None
        self.thread = self.work = self.pending = None
        self.phase, self.detail, self.metrics, self.title = "Ready", "", "", ""
        self.fraction = None
        self.notice = ""
        self.summary = ""
        self.details = self.quit = self.restart = self.exit_requested = False
        self.error = self.success = False
        self.paste = None
        self.account_choice = None
        self.account_options = (
            [("Public access", ())]
            + [(name.capitalize(), ("--cookies-from-browser", name)) for name in BROWSERS
               if name != "safari" or platform.system() == "Darwin"]
        )
        self.auth = ()
        if args.cookies_from_browser:
            self.auth = ("--cookies-from-browser", args.cookies_from_browser)
            self.account_options.append(("Browser profile from command line", self.auth))
        elif args.cookies:
            self.auth = ("--cookies", str(args.cookies.expanduser().absolute()))
            self.account_options.append(("Cookie file from command line", self.auth))

    def begin(self, kind, function, *args):
        work = Work(lambda tag, payload: self.messages.put((tag, payload)))
        self.work = work

        def run():
            try:
                self.messages.put((kind, function(*args, work)))
            except Cancelled:
                self.messages.put(("cancelled", kind))
            except (OSError, RuntimeError, ValueError, KeyError, TypeError) as error:
                self.messages.put(("failed", (kind, str(error))))

        self.thread = threading.Thread(target=run, daemon=True)
        self.thread.start()

    def submit(self, video_only=False):
        try:
            url = youtube_url(self.url, video_only)
        except RuntimeError as error:
            self.notice = str(error)
            return
        if self.tools is None:
            if self.thread is None:
                self.notice = "Restart after resolving the component error."
            else:
                self.pending = url
                self.notice = "Download queued for when the component check finishes."
            return
        if self.thread is not None:
            self.notice = "A download is already running. Esc cancels it."
            return
        self.start_download(url)

    def start_download(self, url):
        self.error = self.success = False
        self.title = self.notice = ""
        self.phase, self.detail = "Reading link", "Identifying video or playlist"
        auth = self.auth
        self.begin("download_done", lambda work: make_mp3(url, self.output, self.tools, work, auth))

    def drain(self):
        while True:
            try:
                tag, data = self.messages.get_nowait()
            except Empty:
                break
            if tag == "status":
                self.phase, self.detail, self.fraction, self.metrics = data
            elif tag == "log":
                self.logs.append(data)
            elif tag == "title":
                self.title = data
            else:
                self.thread.join()
                self.thread = self.work = None
                self.fraction = None
                self.metrics = self.notice = ""
                if tag == "startup_done":
                    self.tools = data
                    if data is None:
                        self.restart = self.quit = True
                        return
                    self.phase, self.detail, self.summary = "Ready", "Paste a link to get started.", data.summary
                    if self.pending and self.account_choice is None:
                        request, self.pending = self.pending, None
                        self.start_download(request)
                elif tag == "download_done":
                    self.phase, self.detail = "Saved MP3", data.name
                    self.fraction, self.success = 1, True
                elif tag == "cancelled":
                    self.phase, self.detail = "Cancelled", "Paste another link or press Enter to retry."
                elif tag == "failed":
                    kind, reason = data
                    self.logs.append(reason)
                    self.phase, self.detail = ("Setup failed" if kind == "startup_done" else "Download failed"), reason
                    if kind == "download_done" and re.search(r"sign in|login required|confirm your age", reason, re.I):
                        self.phase = "YouTube sign-in required"
                        if self.auth:
                            self.detail = "Verify access to this link in your selected browser, then press Enter to retry."
                        else:
                            self.detail = "Press F6 to choose a browser signed in to YouTube, then Enter to retry."
                    self.error = True

    def draw(self, screen, palette):
        screen.erase()
        rows, cols = screen.getmaxyx()
        inset = 2 if cols >= 38 else 1
        width = max(0, cols - 2 * inset)
        accent, muted, green, red, bar, field = palette

        def put(y, x, value, attr=0, fill=0):
            if not (0 <= y < rows and x < cols - 1):
                return
            limit = min(cols - x - 1, fill or width)
            visible, cells = fit(value, limit)
            if fill:
                visible += " " * (limit - cells)
            if visible:
                try:
                    screen.addstr(y, x, visible, attr)
                except curses.error:
                    pass

        put(0, 0, " SimpleMP3 ", accent | curses.A_BOLD, cols - 1)
        if rows < 7 or cols < 24:
            put(2, inset, "Enlarge terminal", curses.A_BOLD)
            screen.refresh()
            return
        if self.account_choice is not None:
            self.draw_accounts(screen, put, rows, cols, inset, accent, muted, bar)
            return
        busy = self.thread is not None
        compact = rows < 15
        field_row = 2 if compact else 3
        if not compact:
            put(2, inset, "YouTube URL", curses.A_BOLD)
        start = max(0, self.cursor - max(1, width - 3))
        offset = 2 if width >= 6 else 0
        field_text = self.url[start:] if self.url else ("Paste a YouTube URL" if compact else "")
        put(field_row, inset, (" " * offset) + field_text,
            field | (curses.A_DIM if not self.url else 0), width)
        if not compact:
            put(4, inset, "Enter  Download    F2  Video only    F3  Paste    F6  Account", muted)
        row = 4 if compact else 6
        color = red if self.error else green if self.success else accent
        put(row, inset, self.phase, color | curses.A_BOLD)
        if self.title and not compact:
            put(row + 1, inset, self.title, curses.A_BOLD)
            row += 1
        footer_rows = 4 if rows >= 17 else 1
        show_bar = not self.error and rows >= 9 and (busy or self.success)
        for line in self.wrap(self.notice or self.detail, width):
            if row + 1 >= rows - footer_rows - (2 if show_bar else 0):
                break
            row += 1
            put(row, inset, line, color if self.error else 0)

        if show_bar:
            row += 1
            size = max(1, min(width - 8, 66))
            if self.fraction is not None:
                fraction = max(0, min(1, self.fraction))
                filled = round(size * fraction)
                percent = round(fraction * 100)
            elif busy:
                filled, percent = min(size, 3), None
            else:
                filled, percent = 0, None
            if percent is None and busy:
                start_mark = int(time.monotonic() * 12) % size
                put(row, inset, " " * size, field, size)
                put(row, inset + start_mark, " " * min(filled, size - start_mark), bar)
            else:
                put(row, inset, " " * size, field, size)
                put(row, inset, " " * filled, bar)
            if percent is not None:
                put(row, inset + size + 1, f"{percent:3}%", color | curses.A_BOLD)
            row += 1
            put(row, inset, self.metrics, muted)

        detail_rows = rows - footer_rows - row - 3
        if self.details and self.logs and detail_rows > 0:
            put(row + 2, inset, "Details", curses.A_BOLD)
            for pos, line in enumerate(list(self.logs)[-detail_rows:], row + 3):
                put(pos, inset, line, muted)

        if rows >= 17:
            put(rows - 4, inset, "Save to  " + str(self.output), muted)
            account = next((label for label, auth in self.account_options if auth == self.auth), "")
            put(rows - 3, inset, (account + "  ·  " if self.auth else "") + self.summary, muted)
        footer = "Esc  Cancel    " if busy else ""
        footer += "F4  Details    F5  Open folder    Ctrl+Q  Quit"
        if width < 62:
            footer = ("Esc Cancel  " if busy else "") + "F6 Account  F4 Log  F5 Folder  ^Q Quit"
        put(rows - 1, 0, " " + footer, accent | curses.A_BOLD, cols - 1)
        try:
            _, cursor_cells = fit(self.url[start:self.cursor], max(0, width - offset - 1))
            screen.move(field_row, inset + offset + cursor_cells)
            curses.curs_set(1)
        except curses.error:
            pass
        screen.refresh()

    def draw_accounts(self, screen, put, rows, cols, inset, accent, muted, bar):
        put(2, inset, "YouTube account", accent | curses.A_BOLD)
        put(3, inset, "Choose a browser already signed in to YouTube.", muted)
        visible = rows - 5
        first = max(0, self.account_choice - visible + 1)
        for index, (label, auth) in enumerate(self.account_options[first:first + visible], first):
            selected = index == self.account_choice
            label = ("› " if selected else "  ") + label
            if auth == self.auth:
                label += "  ✓"
            put(4 + index - first, inset, label, bar | curses.A_BOLD if selected else 0, cols - 2 * inset)
        put(rows - 1, 0, " ↑↓ Choose    Enter Use session    Esc Back", accent | curses.A_BOLD, cols - 1)
        try:
            curses.curs_set(0)
        except curses.error:
            pass
        screen.refresh()

    @staticmethod
    def wrap(text, width):
        text = display_text(text)
        for _ in range(2):
            if not text:
                return
            part, _ = fit(text, width)
            if part != text and " " in part:
                part = part.rsplit(" ", 1)[0]
            if not part:
                return
            yield part
            text = text[len(part):].lstrip()

    def key(self, key):
        if self.account_choice is not None:
            if key == curses.KEY_UP:
                self.account_choice = max(0, self.account_choice - 1)
            elif key == curses.KEY_DOWN:
                self.account_choice = min(len(self.account_options) - 1, self.account_choice + 1)
            elif key in ("\n", "\r", curses.KEY_ENTER):
                label, self.auth = self.account_options[self.account_choice]
                self.account_choice = None
                self.pending = None
                self.notice = f"{label} selected. Press Enter to download."
            elif key in ("\x03", "\x1b", curses.KEY_F6, "\x11"):
                self.account_choice = None
                self.pending = None
                if key == "\x11":
                    self.key(key)
            return
        if key == PASTE_START:
            self.paste = ""
        elif key == PASTE_END:
            if self.paste is not None:
                self.url = self.paste.strip()
                self.cursor = len(self.url)
                self.notice = ""
            self.paste = None
        elif self.paste is not None:
            if isinstance(key, str) and len(self.paste) < 8192:
                self.paste += key
        elif key in ("\n", "\r", curses.KEY_ENTER):
            self.submit()
        elif key == curses.KEY_F2:
            self.submit(video_only=True)
        elif key in ("\x16", curses.KEY_F3):
            pasted = clipboard_url()
            if pasted:
                self.url, self.cursor, self.notice = pasted, len(pasted), ""
            else:
                self.notice = "Clipboard has no YouTube link. Use your terminal’s paste shortcut."
        elif key == curses.KEY_F4:
            self.details = not self.details
        elif key == curses.KEY_F6:
            if not self.thread or self.tools is None:
                self.account_choice = next((i for i, (_, auth) in enumerate(self.account_options)
                                            if auth == self.auth), 0)
            else:
                self.notice = "Finish or cancel the current download before changing accounts."
        elif key == curses.KEY_F5:
            try:
                opener = "open" if platform.system() == "Darwin" else "xdg-open"
                self.output.mkdir(parents=True, exist_ok=True)
                subprocess.Popen([opener, self.output], stdin=subprocess.DEVNULL,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 start_new_session=True)
            except OSError as error:
                self.notice = str(error)
        elif key in ("\x03", "\x1b") and self.work:
            if self.tools is None:
                self.pending = None
                self.notice = "Queued download cleared. Component check continues."
            else:
                self.work.cancel.set()
                self.notice = "Cancelling…"
        elif key == "\x11":
            self.exit_requested = True
            self.pending = None
            if self.work:
                self.work.cancel.set()
        elif key in ("\x01", curses.KEY_HOME):
            self.cursor = 0
        elif key in ("\x05", curses.KEY_END):
            self.cursor = len(self.url)
        elif key == curses.KEY_LEFT:
            self.cursor = max(0, self.cursor - 1)
        elif key == curses.KEY_RIGHT:
            self.cursor = min(len(self.url), self.cursor + 1)
        elif key == "\x15":
            self.url, self.cursor, self.notice = "", 0, ""
        elif key in (curses.KEY_BACKSPACE, "\x7f", "\b") and self.cursor:
            self.url = self.url[:self.cursor - 1] + self.url[self.cursor:]
            self.cursor -= 1
            self.notice = ""
        elif key == curses.KEY_DC and self.cursor < len(self.url):
            self.url = self.url[:self.cursor] + self.url[self.cursor + 1:]
            self.notice = ""
        elif isinstance(key, str) and len(key) == 1 and key.isprintable() and len(self.url) < 8192:
            self.url = self.url[:self.cursor] + key + self.url[self.cursor:]
            self.cursor += 1
            self.notice = ""

    def run(self, screen):
        curses.raw()
        curses.set_escdelay(25)
        screen.timeout(90)
        if curses.has_colors():
            background = -1
            try:
                curses.use_default_colors()
            except curses.error:
                background = curses.COLOR_BLACK
            for index, foreground, bg in [
                (1, curses.COLOR_CYAN, background), (2, curses.COLOR_WHITE, background),
                (3, curses.COLOR_GREEN, background), (4, curses.COLOR_RED, background),
                (5, curses.COLOR_BLACK, curses.COLOR_CYAN),
                (6, curses.COLOR_WHITE, curses.COLOR_BLUE),
            ]:
                curses.init_pair(index, foreground, bg)
            palette = [curses.color_pair(i) for i in range(1, 7)]
        else:
            palette = [curses.A_BOLD, curses.A_DIM, curses.A_BOLD, curses.A_BOLD,
                       curses.A_REVERSE, curses.A_UNDERLINE]
        sys.stdout.write("\x1b[?2004h")
        sys.stdout.flush()
        try:
            self.phase, self.detail = "Checking components", "Checking download and audio tools"
            self.begin("startup_done", lambda work: startup(work, not self.args.no_update))
            if self.args.url:
                self.submit(self.args.video)
            while not self.quit:
                self.drain()
                if self.exit_requested:
                    if self.thread is None:
                        break
                    self.notice = "Finishing cancellation…"
                self.draw(screen, palette)
                key = read_key(screen)
                if not self.exit_requested:
                    self.key(key)
        finally:
            if self.work:
                self.work.cancel.set()
                self.thread.join(0.4)
            sys.stdout.write("\x1b[?2004l")
            sys.stdout.flush()


def main():
    parser = argparse.ArgumentParser(description="Save a YouTube video or playlist as a high-quality MP3.")
    parser.add_argument("url", nargs="?", help="YouTube URL to start with")
    parser.add_argument("--video", action="store_true", help="save only the video from a watch link")
    parser.add_argument("--output-dir", type=Path,
                        default=Path(os.environ.get("YTMP3_DIR") or (Path.home() / "Music/YouTube")),
                        help="output folder (default: YTMP3_DIR or ~/Music/YouTube)")
    parser.add_argument("--no-update", action="store_true", help="use installed or cached components")
    parser.add_argument("--prepare", action="store_true",
                        help="provision and verify downloader dependencies without opening the terminal interface")
    account = parser.add_mutually_exclusive_group()
    account.add_argument("--cookies-from-browser", metavar="BROWSER[:PROFILE]",
                         help="use your signed-in YouTube browser session")
    account.add_argument("--cookies", type=Path, metavar="FILE",
                         help="read a YouTube cookie file in Netscape format")
    parser.add_argument("--version", action="version", version=f"simplemp3 {VERSION}")
    parser.add_argument("--apply-release", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.apply_release:
        try:
            apply_app_release(args.apply_release, Work(lambda *_: None))
        except (OSError, RuntimeError, ValueError, KeyError, SyntaxError) as error:
            parser.exit(1, f"simplemp3: {error}\n")
        return
    if args.prepare:
        downloads = set()

        def report(tag, value):
            if tag == "log":
                print(value, file=sys.stderr, flush=True)
            elif tag == "status" and value[0] == "Updating components" and value[1] not in downloads:
                downloads.add(value[1])
                print(value[1], flush=True)

        work = Work(report)
        try:
            tools = startup(work, not args.no_update, prepare=True)
        except (OSError, RuntimeError, ValueError, KeyError) as error:
            parser.exit(1, f"simplemp3: {error}\n")
        print(f"SimpleMP3 ready · {tools.summary}")
        return
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        parser.exit(2, "simplemp3: an interactive terminal is required\n")
    locale.setlocale(locale.LC_ALL, "")
    app = App(args)
    for sig in (signal.SIGHUP, signal.SIGTERM):
        signal.signal(sig, lambda *_: setattr(app, "quit", True))
    try:
        curses.wrapper(app.run)
    except curses.error as error:
        parser.exit(1, f"simplemp3: could not open the terminal: {error}\n")
    if app.restart:
        os.execv(sys.executable, [sys.executable, *sys.argv])


if __name__ == "__main__":
    main()
