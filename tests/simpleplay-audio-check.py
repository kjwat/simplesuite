#!/usr/bin/env python3
"""Capture the real engine's PCM: native rate, unity transparency, gain/headroom.
No speaker output and no mpv dependency. The SDL interposer is test-only.
"""
from pathlib import Path
import array
import os
import shlex
import subprocess
import sys
import tempfile

build = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parent.parent
if not sys.platform.startswith("linux"):
    print("SDL PCM capture check currently requires Linux LD_PRELOAD")
    sys.exit(0)

def run(args, **kwargs):
    return subprocess.run(args, check=True, capture_output=True, timeout=15, **kwargs)

flags = shlex.split(run([os.environ.get("PKG_CONFIG", "pkg-config"), "--cflags", "sdl2", "libavutil"], text=True).stdout)
capture = build / "simpleplay-audio-capture.so"
run(shlex.split(os.environ.get("CC", "cc")) + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
    *flags, str(root / "tests/simpleplay-audio-capture.c"), "-ldl", "-o", str(capture)])
with tempfile.TemporaryDirectory(prefix="simpleplay-pcm-", dir=build) as directory:
    directory = Path(directory)
    for hz in (44100, 48000, 96000):
        source, reference = directory / "source.wav", directory / "reference.pcm"
        run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
             f"aevalsrc=1.4*sin(2*PI*997*t)|0.6*sin(2*PI*7133*t):s={hz}:d=0.3",
             "-c:a", "pcm_f32le", str(source)])
        run(["ffmpeg", "-v", "error", "-y", "-i", str(source), "-f", "f32le", str(reference)])
        for volume in (100, 80):
            pcm, spec = directory / "output.pcm", directory / "output.spec"
            pcm.unlink(missing_ok=True); spec.unlink(missing_ok=True)
            env = dict(os.environ, SDL_AUDIODRIVER="dummy", LD_PRELOAD=str(capture),
                SIMPLEPLAY_TEST_AUDIO_PCM=str(pcm), SIMPLEPLAY_TEST_AUDIO_SPEC=str(spec))
            args = [] if volume == 80 else ["--volume=100"]
            run([str(build / "simpleplay-core"), *args, str(source)], env=env)
            assert list(map(int, spec.read_text().split()[:2])) == [hz, 2], spec.read_text()
            expected = array.array("f"); expected.frombytes(reference.read_bytes())
            expected = array.array("f", (sample * (volume / 100) for sample in expected))
            assert pcm.read_bytes() == expected.tobytes(), f"PCM changed at {hz} Hz / {volume}%"
    print("SimplePlay audio checks passed: 44.1/48/96 kHz, exact unity PCM, 80% default, float headroom")
