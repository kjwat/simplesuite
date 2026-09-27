#!/usr/bin/env python3
"""Opt-in real PipeWire integration using silent, explicitly targeted devices.

The real desktop default is never changed. An Engine subclass scopes default
routing to a dedicated null sink while exercising real stream moves, graph
controls, signal flow, cleanup, and recovery.
"""
import array
import copy
import json
import math
import os
from pathlib import Path
import runpy
import select
import signal
import subprocess
import tempfile
import time
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
M = runpy.run_path(str(ROOT / "simplevol-audio"))
G = M["run"].__globals__
real_run = M["run"]
real_info = M["pulse_info"]
before = real_info()["default_sink_name"]
suffix = str(os.getpid())
input_name, output_name = "simplevol_test_in_" + suffix, "simplevol_test_out_" + suffix
modules = []
children = []
engine = None


def wait_for(predicate, seconds=5):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        time.sleep(.05)
    raise AssertionError("Timed out waiting for PipeWire")


def check_lowpass(directory, engine, original_stream):
    # Mute only our original test tone; retain its stream to check restoration.
    real_run("pactl", "set-sink-input-mute", str(original_stream), "1")
    frequencies = (1000, 3000, 6000, 12000)
    frame = array.array("f")
    for i in range(48000):
        value = sum(.1 * math.sin(2 * math.pi * f * i / 48000) for f in frequencies)
        frame.extend((value, value / 2))
    raw = directory / "lowpass.f32"
    raw.write_bytes(frame.tobytes() * 20)
    with raw.open("rb") as source:
        player = subprocess.Popen(["pacat", "--playback", "--raw", "--format=float32le", "--rate=48000",
                                   "--channels=2", "--latency-msec=20", "--client-name=SimpleVolLowPassTest",
                                   "--device=" + input_name], stdin=source, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        children.append(player)
        stream = wait_for(lambda: next((s for s in M["pulse_list"]("sink-inputs") if
                                       s.get("properties", {}).get("application.name") == "SimpleVolLowPassTest"), None))
        real_run("pactl", "set-sink-input-mute", str(stream["index"]), "0")
        real_run("pactl", "set-sink-input-volume", str(stream["index"]), "100%")
        real_run("pactl", "move-sink-input", str(stream["index"]), M["SINK"])

        def measure(**settings):
            config = dict(M["DEFAULTS"], output=output_name, limiter=0, **settings)
            engine.request({"command": "configure", "config": config})
            recorder = subprocess.Popen(["parec", "--raw", "--format=float32le", "--rate=48000", "--channels=2",
                                         "--latency-msec=20", "--device=" + output_name + ".monitor"],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            children.append(recorder)
            try:
                captured = bytearray()
                end = time.monotonic() + 5
                while len(captured) < 48000 * 8:
                    remaining = end - time.monotonic()
                    assert remaining > 0 and select.select([recorder.stdout], [], [], remaining)[0], "Capture timed out"
                    block = os.read(recorder.stdout.fileno(), 48000 * 8 - len(captured))
                    assert block, "Capture ended unexpectedly"
                    captured.extend(block)
            finally:
                recorder.terminate()
                recorder.wait(timeout=3)
            samples = array.array("f"); samples.frombytes(captured)
            levels = []
            for channel in (0, 1):
                signal = samples[-24000 + channel::2]
                levels.append([2 * abs(sum(v * complex(math.cos(2 * math.pi * f * i / 48000),
                                                       -math.sin(2 * math.pi * f * i / 48000))
                                          for i, v in enumerate(signal))) / len(signal) for f in frequencies])
            assert all(abs(left / right - 2) < .01 for left, right in zip(*levels)), "Low-pass filter changed stereo balance"
            return levels[0]

        dry = measure(lowpass=0)
        assert all(abs(v - .1) < .002 for v in dry), dry
        normal = measure(lowpass=1)
        lower = measure(lowpass=1, lowpass_cutoff=3000)
        higher = measure(lowpass=1, lowpass_cutoff=12000)
        bypass = measure(lowpass=1, lowpass_cutoff=1000, bypass=1)
        off = measure(lowpass=0, lowpass_cutoff=1000)
        reduction = [20 * math.log10(wet / original) for wet, original in zip(normal, dry)]
        assert abs(reduction[0]) < .15, reduction
        # Retain the script's Q=0.707. PipeWire 1.0 interprets low-pass Q as
        # resonance in dB, so allow its cutoff gain as well as standard Q.
        assert -3.2 < reduction[2] < -.6, reduction
        assert reduction[3] < -14, reduction
        assert lower[2] < normal[2] < higher[2], "Live cutoff does not change treble attenuation"
        assert abs(20 * math.log10(lower[1] / dry[1]) - reduction[2]) < .2
        assert abs(20 * math.log10(higher[3] / dry[3]) - reduction[2]) < .2
        for restored in (bypass, off):
            assert all(abs(20 * math.log10(v / ref)) < .05 for v, ref in zip(restored, dry)), "Bypass still filters audio"
        assert engine.child.poll() is None
        print(f"Low-pass filter: {reduction[2]:.2f} dB at 6 kHz, {reduction[3]:.2f} dB at 12 kHz; live cutoff, stereo, off and bypass passed")
        player.terminate()
        player.wait(timeout=3)
    real_run("pactl", "set-sink-input-mute", str(original_stream), "0")


try:
    if any(s["name"] == M["SINK"] for s in M["pulse_list"]("sinks")):
        raise RuntimeError("Stop SimpleVol effects before this isolated integration test")
    for name in (input_name, output_name):
        modules.append(real_run("pactl", "load-module", "module-null-sink", "sink_name=" + name,
                                "sink_properties=device.description=SimpleVol_Test", "rate=48000", "channels=2").strip())
    with tempfile.TemporaryDirectory(prefix="simplevol-live-") as tmp:
        directory = Path(tmp)
        config_home = directory / "config"
        settings = config_home / "simplevol"
        settings.mkdir(parents=True)
        config = dict(M["DEFAULTS"], output=output_name, compressor=1, threshold=-18, attack=1, lookahead=0, knee=0, limiter=0)
        (settings / "config.json").write_text(json.dumps(config))
        # A synthetic source first plays to the test's unprocessed input sink.
        data = array.array("f", [v for i in range(48000 * 45) for v in
                               (.8 * math.sin(2 * math.pi * 1000 * i / 48000),) * 2])
        raw = directory / "tone.f32"
        with raw.open("wb") as handle:
            data.tofile(handle)
        source_file = raw.open("rb")
        player = subprocess.Popen(["pacat", "--playback", "--raw", "--format=float32le", "--rate=48000", "--channels=2",
                                   "--latency-msec=20", "--client-name=SimpleVolIntegration", "--device=" + input_name],
                                  stdin=source_file, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        children.append(player)
        stream = wait_for(lambda: next((s for s in M["pulse_list"]("sink-inputs") if
                                       s.get("properties", {}).get("application.name") == "SimpleVolIntegration"), None))
        # WirePlumber can remember the test stream's mute state after a failed run.
        real_run("pactl", "set-sink-input-mute", str(stream["index"]), "0")
        real_run("pactl", "set-sink-input-volume", str(stream["index"]), "100%")
        input_index = next(s["index"] for s in M["pulse_list"]("sinks") if s["name"] == input_name)
        virtual_default = [input_name]

        def scoped_info():
            info = real_info()
            info["default_sink_name"] = virtual_default[0]
            return info

        def scoped_run(*args, **kwargs):
            if args[:2] == ("pactl", "set-default-sink"):
                virtual_default[0] = args[2]
                return ""
            return real_run(*args, **kwargs)

        with patch.dict(os.environ, {"XDG_CONFIG_HOME": str(config_home)}), patch.dict(G, run=scoped_run, pulse_info=scoped_info):
            engine = M["Engine"]()
            engine.start()
            effect_index = next(s["index"] for s in M["pulse_list"]("sinks") if s["name"] == M["SINK"])
            routed = next(s for s in M["pulse_list"]("sink-inputs") if s["index"] == stream["index"])
            assert routed["sink"] == effect_index, "Existing test stream was not moved into effects"
            recorder = subprocess.Popen(["parec", "--raw", "--format=float32le", "--rate=48000", "--channels=2",
                                         "--latency-msec=20", "--device=" + output_name + ".monitor"],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            children.append(recorder)
            # Drain a second of captured output, which includes startup latency.
            captured = recorder.stdout.read(48000 * 8)
            samples = array.array("f"); samples.frombytes(captured)
            steady = samples[-24000:]
            level = math.sqrt(sum(v*v for v in steady) / len(steady))
            reduction = 20 * math.log10(max(1e-9, level) / (.8 / math.sqrt(2)))
            assert -14 < reduction < -9, f"Live compression is wrong: {reduction:.2f} dB"
            meters = engine.status()["meters"]
            assert meters["compression"] > 9, meters
            print(f"PipeWire signal path: {reduction:.2f} dB measured compression, {meters['compression']:.2f} dB meter")
            next_config = dict(engine.config, eq5=6, bypass=1)
            engine.request({"command": "configure", "config": next_config})
            node_controls = M["meter_values"](engine.node)
            assert node_controls["comp:enabled"] == 0
            assert node_controls["eq5_l:Gain"] == 0
            next_config.update(bypass=0, compressor=0)
            engine.request({"command": "configure", "config": next_config})
            node_controls = M["meter_values"](engine.node)
            assert node_controls["eq5_l:Gain"] == 6
            assert node_controls["eq5_r:Gain"] == 6
            assert engine.child.poll() is None
            recorder.terminate()
            recorder.wait(timeout=3)
            check_lowpass(directory, engine, stream["index"])
            pid = engine.child.pid
            engine.close()
            engine = None
            restored = next(s for s in M["pulse_list"]("sink-inputs") if s["index"] == stream["index"])
            assert restored["sink"] == input_index, "Original test stream route was not restored"
            assert virtual_default[0] == input_name
            assert not any(s["name"] == M["SINK"] for s in M["pulse_list"]("sinks"))
            assert not Path(f"/proc/{pid}").exists()
            print("PipeWire lifecycle: live EQ/bypass, exact stream restoration, and graph cleanup passed")
        source_file.close()
finally:
    if engine is not None:
        engine.close()
    for child in children:
        if child.poll() is None:
            child.terminate()
        child.wait(timeout=5)
    for module in reversed(modules):
        real_run("pactl", "unload-module", module, check=False)
    assert real_info()["default_sink_name"] == before, "Desktop output changed during the test"
print("Desktop audio default remained unchanged")
