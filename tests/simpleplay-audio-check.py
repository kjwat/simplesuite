#!/usr/bin/env python3
"""Measure actual PCM: transparency, SRC, safe downmixing and gain changes.
No speaker output. The SDL interposer is test-only.
"""
from pathlib import Path
import array
import math
import os
import shlex
import socket
import subprocess
import sys
import tempfile
import time

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
    serial = 0

    def read_pcm(path):
        values = array.array("f")
        values.frombytes(path.read_bytes())
        return values

    def capture_media(source, volume=100, output_rate=None, controls=None):
        global serial
        serial += 1
        pcm, spec = directory / f"output-{serial}.pcm", directory / f"output-{serial}.spec"
        env = dict(os.environ, SDL_AUDIODRIVER="dummy", LD_PRELOAD=str(capture),
            SIMPLEPLAY_TEST_AUDIO_PCM=str(pcm), SIMPLEPLAY_TEST_AUDIO_SPEC=str(spec))
        env.pop("SIMPLEPLAY_TEST_OUTPUT_RATE", None)
        if output_rate:
            env["SIMPLEPLAY_TEST_OUTPUT_RATE"] = str(output_rate)
        args = [] if volume is None else [f"--volume={volume}"]
        if controls:
            path = directory / f"ipc-{serial}.sock"
            process = subprocess.Popen([str(build / "simpleplay-core"), *args, "--pause",
                f"--input-ipc-server={path}", str(source)], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            client = None
            try:
                deadline = time.monotonic() + 4
                while not path.exists() and time.monotonic() < deadline:
                    assert process.poll() is None
                    time.sleep(0.01)
                client = socket.socket(socket.AF_UNIX)
                client.settimeout(4)
                client.connect(str(path))
                response = client.makefile("rb")
                request = 0

                def command(*args, allow_unavailable=False):
                    import json
                    nonlocal request
                    request += 1
                    client.sendall((json.dumps({"command": args, "request_id": request}) + "\n").encode())
                    while True:
                        line = response.readline()
                        assert line, "IPC closed before a response"
                        value = json.loads(line)
                        if value.get("request_id") == request:
                            if allow_unavailable and value.get("error") == "property unavailable":
                                return None
                            assert value.get("error") == "success", value
                            return value.get("data")

                controls(command, pcm)
                stdout, stderr = process.communicate(timeout=8)
                assert process.returncode == 0, stderr
                response.close()
            finally:
                if client:
                    client.close()
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=4)
        else:
            run([str(build / "simpleplay-core"), *args, str(source)], env=env)
        return read_pcm(pcm), spec.read_text().splitlines()

    for hz in (44100, 48000, 96000):
        source, reference = directory / "source.wav", directory / "reference.pcm"
        run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
             f"aevalsrc=1.4*sin(2*PI*997*t)|0.6*sin(2*PI*7133*t):s={hz}:d=0.3",
             "-c:a", "pcm_f32le", str(source)])
        run(["ffmpeg", "-v", "error", "-y", "-i", str(source), "-f", "f32le", str(reference)])
        for volume in (100, 80):
            actual, specs = capture_media(source, None if volume == 80 else 100)
            assert list(map(int, specs[-1].split()[:2])) == [hz, 2], specs
            expected = array.array("f"); expected.frombytes(reference.read_bytes())
            expected = array.array("f", (sample * (volume / 100) for sample in expected))
            assert actual.tobytes() == expected.tobytes(), f"PCM changed at {hz} Hz / {volume}%"

    # Integer and lossless inputs must reach the float output without extra DSP.
    for codec, suffix in (("pcm_s16le", "wav"), ("pcm_s24le", "wav"), ("flac", "flac")):
        source = directory / f"integer-{codec}.{suffix}"
        run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
             "aevalsrc=0.9*sin(2*PI*997*t)|0.1*sin(2*PI*7133*t):s=48000:d=0.3",
             "-c:a", codec, str(source)])
        run(["ffmpeg", "-v", "error", "-y", "-i", str(source), "-f", "f32le", str(reference)])
        actual, _ = capture_media(source)
        assert actual.tobytes() == reference.read_bytes(), codec

    # Float output otherwise defaults to an unnormalized surround matrix.
    for channels, layout in ((6, "5.1"), (8, "7.1")):
        source = directory / (layout + ".wav")
        expression = "|".join(["0.9*sin(2*PI*997*t)"] * channels)
        run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
             f"aevalsrc={expression}:s=48000:d=0.3:c={layout}", "-c:a", "pcm_f32le", str(source)])
        actual, _ = capture_media(source)
        assert max(map(abs, actual)) <= 0.900001, f"{layout} downmix exceeds its input peak"

    for source_rate, frequency in ((44100, 997), (96000, 20000), (96000, 30000)):
        source = directory / f"resample-{source_rate}-{frequency}.wav"
        run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
             f"aevalsrc=0.8*sin(2*PI*{frequency}*t)|0.8*sin(2*PI*{frequency}*t):s={source_rate}:d=0.3",
             "-c:a", "pcm_f32le", str(source)])
        actual, specs = capture_media(source, output_rate=48000)
        assert list(map(int, specs[-1].split()[:2])) == [48000, 2]
        run(["ffmpeg", "-v", "error", "-y", "-i", str(source), "-af",
             "aresample=48000:filter_size=64:cutoff=0.97,aformat=sample_fmts=flt:channel_layouts=stereo",
             "-f", "f32le", str(reference)])
        assert actual.tobytes() == reference.read_bytes(), "Sample-rate conversion differs from FFmpeg"
        if frequency == 30000:
            interior = actual[4096:-4096]
            rms = math.sqrt(sum(sample * sample for sample in interior) / len(interior))
            assert rms < 0.00001, f"Insufficient rejection above the output Nyquist rate: {rms}"

    # Invalid float media must not send NaNs or infinities into the audio server.
    raw, source = directory / "invalid.pcm", directory / "invalid.wav"
    values = array.array("f", [0.2] * 48000)
    values[1000], values[2000], values[3000] = math.nan, math.inf, -math.inf
    raw.write_bytes(values.tobytes())
    run(["ffmpeg", "-v", "error", "-y", "-f", "f32le", "-ar", "48000", "-ac", "2",
         "-i", str(raw), "-c:a", "pcm_f32le", str(source)])
    actual, _ = capture_media(source)
    assert all(map(math.isfinite, actual))
    assert all(actual[index] == 0 for index in (1000, 2000, 3000))

    source = directory / "gain.wav"
    run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
         "aevalsrc=0.5|0.5:s=48000:d=2", "-c:a", "pcm_f32le", str(source)])

    def volume_changes(command, pcm):
        command("set_property", "pause", False)
        for volume in (80, 0, 100):
            target = (pcm.stat().st_size if pcm.exists() else 0) + 48000
            deadline = time.monotonic() + 3
            while (not pcm.exists() or pcm.stat().st_size < target) and time.monotonic() < deadline:
                time.sleep(0.005)
            assert pcm.exists() and pcm.stat().st_size >= target
            command("set_property", "volume", volume)

    actual, _ = capture_media(source, volume=20, controls=volume_changes)
    left = actual[::2]
    assert abs(left[0] - 0.1) < 0.000001 and left[-1] == 0.5
    assert min(left) == 0
    assert max(abs(b - a) for a, b in zip(left, left[1:])) < 0.003, "Volume changes produce a discontinuity"

    source = directory / "tempo.wav"
    run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
         "aevalsrc=0.5*sin(2*PI*997*t)|0.5*sin(2*PI*997*t):s=48000:d=0.6",
         "-c:a", "pcm_f32le", str(source)])
    for speed in (0.5, 1.5, 2):
        def set_speed(command, pcm):
            deadline = time.monotonic() + 3
            while (command("get_property", "duration", allow_unavailable=True) or 0) <= 0 and time.monotonic() < deadline:
                time.sleep(0.01)
            command("set_property", "speed", speed)
            deadline = time.monotonic() + 3
            while command("get_property", "speed") != speed and time.monotonic() < deadline:
                time.sleep(0.01)
            assert command("get_property", "speed") == speed
            command("set_property", "pause", False)

        actual, _ = capture_media(source, controls=set_speed)
        run(["ffmpeg", "-v", "error", "-y", "-i", str(source), "-af",
             f"aformat=sample_fmts=flt:channel_layouts=stereo,atempo={speed}",
             "-f", "f32le", str(reference)])
        assert actual.tobytes() == reference.read_bytes(), f"Tempo {speed} differs from FFmpeg"
        assert abs(len(actual) / 2 / 48000 - 0.6 / speed) < 0.05
        assert max(map(abs, actual)) < 0.55
    print("SimplePlay audio checks passed: exact PCM/gain, 44.1/48/96 kHz, SRC rejection, safe 5.1/7.1 downmix, finite output, smooth gain/mute, tempo")
