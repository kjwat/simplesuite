#!/usr/bin/env python3
"""Pixel comparisons for color tags, range, HD fallbacks and chroma positioning."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

build = Path(sys.argv[1]).resolve()
env = dict(os.environ, SDL_AUDIODRIVER="dummy")
base = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y"]


def run(args):
    subprocess.run([str(a) for a in args], env=env, check=True, timeout=12)


with tempfile.TemporaryDirectory(prefix="simpleplay-video-", dir=build) as temporary:
    directory = Path(temporary)
    cases = [
        ("untagged-sd", 160, 96, None, "tv", "bt601", "yuv420p", "left"),
        ("untagged-hd", 1280, 720, None, "tv", "bt709", "yuv420p", "left"),
        ("tagged-601-hd", 1280, 720, "smpte170m", "tv", "bt601", "yuv420p", "left"),
        ("tagged-709-sd", 160, 96, "bt709", "tv", "bt709", "yuv420p", "left"),
        ("full-range", 160, 96, "bt709", "pc", "bt709", "yuv420p", "left"),
        ("ten-bit", 160, 96, "bt709", "tv", "bt709", "yuv420p10le", "left"),
        ("chroma-center", 160, 96, "bt709", "tv", "bt709", "yuv420p", "center"),
        ("full-rgb", 160, 96, "rgb", "pc", "bt601", "bgr0", "left"),
        ("native-4k", 3840, 2160, "bt709", "tv", "bt709", "yuv420p", "left"),
    ]
    for name, width, height, tag, color_range, matrix, pixel_format, chroma in cases:
        source, reference = directory / (name + ".mkv"), directory / "reference.bgra"
        duration = 0.04 if width > 2560 else 0.24
        args = base + ["-f", "lavfi", "-i", f"testsrc2=size={width}x{height}:rate=25:duration={duration}"]
        if color_range == "pc" and pixel_format != "bgr0":
            args += ["-vf", "scale=in_range=tv:out_range=pc"]
        args += (["-c:v", "libx264", "-qp", "0", "-x264-params", "chromaloc=1"] if chroma == "center" else ["-c:v", "ffv1"])
        args += ["-pix_fmt", pixel_format, "-color_range", color_range,
                 "-chroma_sample_location", chroma]
        if tag:
            args += ["-colorspace", tag]
        run(args + [source])
        filters = f"scale=flags=lanczos+full_chroma_int+accurate_rnd:in_color_matrix={matrix}:out_color_matrix={matrix}:in_range={color_range}:out_range=full:in_h_chr_pos={128 if chroma == 'center' else 0}:in_v_chr_pos=128,format=bgra"
        run(base + ["-i", source, "-vf", filters, "-fps_mode", "passthrough", "-f", "rawvideo", reference])
        print(name, flush=True)
        for state in (0, 1):
            run([build / "simpleplay-engine-check", "--deinterlace-check", source, reference,
                 state, 1, 25, width, height])
print("SimplePlay video checks passed: SD/HD color fallback, explicit matrices, full/limited range, RGB, 10-bit and chroma location")
