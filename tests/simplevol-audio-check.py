#!/usr/bin/env python3
"""Measured signal tests for SimpleVol's plugin configuration, with no speakers."""
import math
from pathlib import Path
import runpy
import sys

ROOT = Path(__file__).resolve().parent.parent
backend = runpy.run_path(str(ROOT / "simplevol-audio"))
host = runpy.run_path(str(ROOT / "tests/simplevol-lv2-host.py"))
Plugin, sine, rms, db = (host[k] for k in ("Plugin", "sine", "rms", "db"))


def settings(name, **kwargs):
    config = dict(backend["DEFAULTS"], **kwargs)
    return {k.split(":", 1)[1]: v for k, v in backend["controls"](config).items() if k.startswith(name + ":")}


def compressor():
    controls = settings("comp", compressor=1, threshold=-18, ratio=4, knee=0, attack=1, release=100, lookahead=0)
    with Plugin("compressor_stereo", controls) as plugin:
        signal = sine(.2, .01) + sine(.5, .8)
        left, right = plugin.process(signal)
        fast = db(rms(left[10080:11040]) / rms(signal[10080:11040]))
        steady = db(rms(left[-4800:]) / rms(signal[-4800:]))
        assert fast < -9, f"Compressor too slow: {fast:.2f} dB after 10 ms"
        assert -13 < steady < -10, f"Wrong threshold/ratio: {steady:.2f} dB"
        assert max(abs(a-b) for a,b in zip(left,right)) < 1e-7, "Stereo mismatch"
        print(f"Compressor: {fast:.2f} dB reduction after 10 ms; {steady:.2f} dB settled")
    with Plugin("compressor_stereo", controls) as plugin:
        left, right = plugin.process(sine(.4, .8), sine(.4, .08))
        assert abs(db(rms(left[-4800:]) / rms(right[-4800:])) - 20) < .02, "Stereo image changed"
    with Plugin("compressor_stereo", settings("comp", compressor=0, makeup=12)) as plugin:
        signal = sine(.2, .3)
        left, _ = plugin.process(signal)
        assert abs(db(rms(left[-4800:]) / rms(signal[-4800:]))) < .05, "Disabled compressor adds makeup gain"


def limiter():
    with Plugin("limiter_stereo", settings("limit", limiter=1, ceiling=-1, oversample=2)) as plugin:
        signal = sine(.5, 3) + sine(.5, 1.8, frequency=15000)
        signal[10000] = 8
        left, right = plugin.process(signal)
        peak = max(abs(x) for x in left + right)
        assert all(math.isfinite(x) for x in left + right)
        assert db(peak) <= -.75, f"Limiter exceeds ceiling: {db(peak):.2f} dBFS"
        assert rms(left[4800:12000]) > .1, "Limiter silenced the signal"
        print(f"Limiter: {db(peak):.2f} dBFS peak for a -1 dB ceiling, including a +18 dB impulse")


def leveling():
    with Plugin("autogain_stereo", settings("level", autogain=1, target=-18, level_time=500)) as plugin:
        quiet, _ = plugin.process(sine(4, .04))
        loud, _ = plugin.process(sine(4, .4))
        difference = abs(db(rms(quiet[-24000:])) - db(rms(loud[-24000:])))
        assert difference < 1.0, f"Loudness levels differ by {difference:.2f} dB"
        before = plugin.ports["g_g"].value
        silence, _ = plugin.process([0] * 96000)
        after = plugin.ports["g_g"].value
        assert max(abs(x) for x in silence[-48000:]) < 1e-8
        assert db(after) <= 12.1, "Gain exceeds maximum boost"
        # Let the measurement window drain; then gain must freeze in silence.
        plugin.process([0] * 96000)
        frozen = plugin.ports["g_g"].value
        assert abs(after - frozen) < 1e-4, "Gain keeps rising in silence"
        print(f"Leveling: 20 dB input difference reduced to {difference:.3f} dB; silence freezes gain")
        assert math.isfinite(before)


if __name__ == "__main__":
    compressor()
    limiter()
    leveling()
    print("SimpleVol real-plugin audio checks passed")
