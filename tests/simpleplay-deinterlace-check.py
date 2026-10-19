#!/usr/bin/env python3
"""Check the real engine's pictures, field cadence and EOF against FFmpeg."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

build = Path(sys.argv[1]).resolve()
base = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y"]
env = dict(os.environ, SDL_AUDIODRIVER="dummy")


def run(args):
    subprocess.run([str(a) for a in args], env=env, check=True, timeout=12)


with tempfile.TemporaryDirectory(prefix="simpleplay-deinterlace-", dir=build) as directory:
    directory = Path(directory)
    for kind, rate, fields in (("progressive", "25", None), ("tff", "50", "interleave_top"),
                               ("bff", "50", "interleave_bottom"), ("ntsc", "60000/1001", "interleave_top")):
        source = directory / (kind + ".mkv")
        args = base + ["-f", "lavfi", "-i", f"testsrc2=size=160x96:rate={rate}:duration=1.2"]
        if kind == "tff":
            args += ["-f", "lavfi", "-i", "sine=sample_rate=48000:duration=1.2", "-c:a", "flac"]
        if fields:
            args += ["-vf", "tinterlace=mode=" + fields]
        run(args + ["-colorspace", "bt709", "-c:v", "ffv1", source])
        input_rate = 25 if kind != "ntsc" else 30000 / 1001
        for state, double_rate in ((0, True), (1, True), (2, False), (2, True)):
            reference = directory / "reference.bgra"
            active = state == 2 or (state == 1 and fields is not None)
            filters = []
            if active:
                filters.append(f"yadif=mode={'send_field' if double_rate else 'send_frame'}:parity=auto:deint={'interlaced' if state == 1 else 'all'}")
            filters += ["scale=flags=lanczos+full_chroma_int+accurate_rnd:in_color_matrix=bt709:out_color_matrix=bt709:out_range=full:in_h_chr_pos=0:in_v_chr_pos=128", "format=bgra"]
            run(base + ["-i", source, "-vf", ",".join(filters), "-fps_mode", "passthrough", "-f", "rawvideo", reference])
            output_rate = input_rate * (2 if active and double_rate else 1)
            print(f"{kind}: state={state}, double_rate={double_rate}", flush=True)
            run([build / "simpleplay-engine-check", "--deinterlace-check", source, reference,
                 state, int(double_rate), output_rate, 160, 96])
        if kind == "tff":
            run([build / "simpleplay-engine-check", "--deinterlace-switch", source])
print("SimplePlay deinterlacing checks passed: Off/Automatic/On, both field orders, NTSC, Yadif/2×, unchanged progressive pictures, BT.709 and EOF")
