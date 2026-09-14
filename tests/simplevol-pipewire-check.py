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
        data = array.array("f", [v for i in range(48000 * 16) for v in
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
