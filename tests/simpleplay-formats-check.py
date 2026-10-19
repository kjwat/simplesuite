#!/usr/bin/env python3
"""Generate a diverse, redistributable codec corpus and decode it with the engine."""
from pathlib import Path
import json
import math
import os
import struct
import subprocess
import sys

build = Path(sys.argv[1]).resolve()
directory = build / "simpleplay-fixtures/formats"
directory.mkdir(parents=True, exist_ok=True)
base = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y"]
manifest = []


def generated(kind, codec, extension, extra=()):
    path = directory / (codec + "." + extension)
    source = "sine=frequency=523.25:sample_rate=48000:duration=0.6" if kind == "audio" else "testsrc2=size=320x180:rate=10:duration=0.6"
    args = base + ["-f", "lavfi", "-i", source, "-c:a" if kind == "audio" else "-c:v", codec, *extra, str(path)]
    result = subprocess.run(args, capture_output=True)
    if result.returncode:
        raise RuntimeError(f"Cannot generate {codec}: {result.stderr.decode()}")
    info = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-show_streams", "-of", "json", path]))
    name = next(s["codec_name"] for s in info["streams"] if s["codec_type"] == kind)
    manifest.append(f"{kind}|{name}|{path}")


for codec, extension, extra in [
    ("alac", "m4a", []), ("wavpack", "wv", []), ("tta", "tta", []), ("libvorbis", "ogg", []),
    ("libopus", "opus", []), ("aac", "aac", []), ("ac3", "ac3", []), ("eac3", "eac3", []),
    ("dca", "dts", ["-strict", "-2"]), ("mlp", "mlp", ["-strict", "-2"]),
    ("truehd", "thd", ["-strict", "-2"]), ("wmav2", "wma", []), ("pcm_s24be", "aiff", []),
    ("pcm_mulaw", "wav", []), ("adpcm_ima_wav", "wav", []), ("libspeex", "spx", ["-ar", "16000"]),
]:
    generated("audio", codec, extension, extra)
for codec, extension, extra in [
    ("ffv1", "mkv", []), ("huffyuv", "avi", ["-pix_fmt", "yuv422p"]), ("mjpeg", "avi", []),
    ("mpeg4", "avi", []), ("mpeg2video", "mpg", []), ("libtheora", "ogv", []),
    ("libvpx", "webm", ["-deadline", "realtime", "-cpu-used", "8"]),
    ("libvpx-vp9", "webm", ["-deadline", "realtime", "-cpu-used", "8"]),
    ("libx265", "mkv", ["-preset", "ultrafast", "-x265-params", "log-level=error:pools=1:frame-threads=1"]),
    ("libaom-av1", "mkv", ["-cpu-used", "8", "-crf", "50"]), ("wmv2", "wmv", []),
    ("flv", "flv", []), ("rv10", "rm", ["-vf", "scale=320:240"]), ("prores_ks", "mov", ["-profile:v", "1", "-pix_fmt", "yuv422p10le"]),
    ("dnxhd", "mov", ["-profile:v", "dnxhr_lb", "-pix_fmt", "yuv422p"]),
]:
    generated("video", codec, extension, extra)

# An original two-note MIDI, with a tempo change, requires no external assets.
track = bytes.fromhex("00 ff 03 0c") + b"Simple MIDI!" + bytes.fromhex(
    "00 ff 51 03 07 a1 20 00 c0 00 00 90 3c 64 83 60 80 3c 00 "
    "00 ff 51 03 0b 71 b0 00 90 40 64 83 60 80 40 00 00 ff 2f 00")
midi = directory / "music.mid"
midi.write_bytes(b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480) + b"MTrk" + struct.pack(">I", len(track)) + track)
manifest.append(f"audio|midi|{midi}")

# An original C64 tone: init writes the SID registers; the play routine returns.
header = bytearray(124)
header[:4] = b"PSID"
struct.pack_into(">7H", header, 4, 2, 124, 0x1000, 0x1000, 0x1030, 2, 1)
header[22:22 + 15] = b"Simple SID tone"
header[54:54 + 10] = b"SimplePlay"
code = bytes.fromhex("a9 0f 8d 18 d4 a9 11 8d 04 d4 a9 00 8d 05 d4 a9 f0 8d 06 d4 a9 4c 8d 00 d4 a9 1d 8d 01 d4 60")
sid = directory / "music.sid"
sid.write_bytes(header + code + b"\xea" * (48 - len(code)) + b"\x60")
manifest.append(f"audio|sid|{sid}")

# Original ProTracker module, one looping sine sample and a single pattern.
mod = bytearray(1084 + 1024 + 32)
mod[:15] = b"SimplePlay MOD!"
struct.pack_into(">H", mod, 42, 16)
mod[45] = 48
struct.pack_into(">H", mod, 48, 16)
mod[950] = 1
mod[1080:1084] = b"M.K."
mod[1084:1088] = bytes([1, 0xac, 0x10, 0])
for n in range(32):
    mod[2108 + n] = int(80 * math.sin(n * math.tau / 32)) & 255
module = directory / "music.mod"
module.write_bytes(mod)
manifest.append(f"audio|pcm_f32le|{module}")

# Original NES APU tone, exercised through FFmpeg's Game Music Emu demuxer.
nsf = bytearray(128)
nsf[:8] = b"NESM\x1a\x01\x01\x01"
struct.pack_into("<HHH", nsf, 8, 0x8000, 0x8000, 0x8040)
nsf[14:14 + 14] = b"SimplePlay NSF"
struct.pack_into("<H", nsf, 110, 16639)
struct.pack_into("<H", nsf, 120, 19997)
nes = bytes.fromhex("a9 01 8d 15 40 a9 bf 8d 00 40 a9 40 8d 02 40 a9 01 8d 03 40 60")
game = directory / "music.nsf"
game.write_bytes(nsf + nes + b"\xea" * (64 - len(nes)) + b"\x60")
manifest.append(f"audio|pcm_s16le|{game}")
manifest_path = directory / "corpus.txt"
manifest_path.write_text("\n".join(manifest) + "\n")
env = dict(os.environ, SDL_AUDIODRIVER="dummy")
subprocess.run([build / "simpleplay-engine-check", "--decode-only", manifest_path], env=env, check=True, timeout=65)
subprocess.run([build / "simpleplay-core", "--check-formats"], env=env, check=True)
print(f"Decoded {len(manifest)} additional codec families, including MIDI, SID, MOD and NSF")
