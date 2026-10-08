#!/usr/bin/env python3
"""Control-plane safety and persistence tests; no live audio is changed."""
import contextlib
import copy
import io
import json
import math
import os
import queue
from pathlib import Path
import runpy
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
M = runpy.run_path(str(ROOT / "simplevol-audio"))
G = M["load_config"].__globals__


class Controls(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="simplevol-check-")
        self.env = patch.dict(os.environ, {"XDG_CONFIG_HOME": self.tmp.name + "/config",
                                          "XDG_RUNTIME_DIR": self.tmp.name + "/runtime"})
        self.env.start()

    def tearDown(self):
        self.env.stop()
        self.tmp.cleanup()

    def test_validate_bounds_and_nonfinite(self):
        for data in ({"attack": 0}, {"threshold": 4}, {"ratio": math.nan}, {"ceiling": math.inf},
                     {"limiter": .5}, {"oversample": 1.5}, {"lowpass": .5},
                     {"lowpass_cutoff": 999}, {"lowpass_cutoff": 20001}, {"lowpass_cutoff": math.nan},
                     {"surprise": 1}, {"output": "x\ny"}, []):
            with self.subTest(data=data), self.assertRaises(M["AudioError"]):
                M["validate_config"](data)

    def test_eq_preset_and_portable_saved_chain(self):
        with patch.dict(G, running=lambda: None):
            M["preset"]("Rock")
            config = M["load_config"]()
            self.assertEqual(config["eq0"], 4)
            self.assertEqual(config["preamp"], -4)
            config.update(output="speakers", compressor=1, threshold=-25, lowpass=1, lowpass_cutoff=4500)
            M["set_config"](config)
            M["preset"]("Classical")
            self.assertEqual(M["load_config"]()["lowpass_cutoff"], 4500)
            self.assertEqual(M["load_config"]()["lowpass"], 1)
            M["preset"]("My mix", save=True)
            file = M["paths"]()[0] / "presets/My mix.json"
            self.assertNotIn("output", json.loads(file.read_text()))
            M["set_config"](dict(config, output="headphones", threshold=-10, lowpass=0, lowpass_cutoff=10000))
            M["preset"]("My mix")
            config = M["load_config"]()
            self.assertEqual(config["output"], "headphones")
            self.assertEqual(config["threshold"], -25)
            self.assertEqual(config["lowpass"], 1)
            self.assertEqual(config["lowpass_cutoff"], 4500)
            self.assertEqual(file.stat().st_mode & 0o777, 0o600)
            for name in ("../outside", "a/b", "a\nb", ""):
                with self.assertRaises(M["AudioError"]):
                    M["preset"](name, save=True)

    def test_bad_config_is_preserved(self):
        directory = M["paths"]()[0]
        directory.mkdir(parents=True)
        file = directory / "config.json"
        file.write_text('{"ratio": "bad"}')
        with self.assertRaises(M["AudioError"]):
            M["load_config"]()
        self.assertEqual(file.read_text(), '{"ratio": "bad"}')

    def test_lowpass_legacy_settings_and_adjustment(self):
        legacy = {k: v for k, v in M["DEFAULTS"].items() if not k.startswith("lowpass")}
        legacy.update(compressor=1, ceiling=-4)
        M["atomic_json"](M["paths"]()[0] / "config.json", legacy)
        config = M["load_config"]()
        self.assertEqual((config["lowpass"], config["lowpass_cutoff"]), (0, 6000))
        self.assertEqual((config["compressor"], config["ceiling"]), (1, -4))
        with patch.dict(G, running=lambda: None):
            M["bridge_action"](["set", "lowpass", "1"])
            M["bridge_action"](["step", "lowpass_cutoff", "-1"])
            self.assertEqual(M["load_config"]()["lowpass_cutoff"], 5750)
            M["bridge_action"](["step", "lowpass_cutoff", "1"])
            self.assertEqual(M["load_config"]()["lowpass_cutoff"], 6000)
            M["bridge_action"](["set", "lowpass_cutoff", "4321"])
            self.assertEqual(M["load_config"]()["lowpass_cutoff"], 4321)
            M["bridge_action"](["step", "lowpass_cutoff", "-1000"])
            self.assertEqual(M["load_config"]()["lowpass_cutoff"], 1000)
            M["bridge_action"](["step", "lowpass_cutoff", "1000"])
            self.assertEqual(M["load_config"]()["lowpass_cutoff"], 20000)
        self.assertEqual(M["load_config"]()["eq_preset"], "Flat")

    def test_lowpass_toggle_and_global_bypass(self):
        for enabled, bypass in ((0, 0), (1, 0), (1, 1)):
            controls = M["controls"](dict(M["DEFAULTS"], lowpass=enabled, bypass=bypass, lowpass_cutoff=4500))
            for side in ("l", "r"):
                self.assertEqual(controls[f"lowpass_{side}:Freq"], 4500)
                wet = int(enabled and not bypass)
                self.assertEqual(controls[f"lowpass_mix_{side}:Gain 1"], 1 - wet)
                self.assertEqual(controls[f"lowpass_mix_{side}:Gain 2"], wet)

    def test_live_updates_fit_older_pipewire_and_send_changed_controls(self):
        calls = []
        before = dict(M["DEFAULTS"], lowpass=1)
        after = dict(before, lowpass_cutoff=4321)
        with patch.dict(G, run=lambda *a: calls.append(a)):
            M["update_controls"](42, after, before)
            self.assertEqual(len(calls), 1)
            params = json.loads(calls[0][-1])["params"]
            self.assertEqual(dict(zip(params[::2], params[1::2])),
                             {"lowpass_l:Freq": 4321, "lowpass_r:Freq": 4321})
            calls.clear()
            M["update_controls"](42, after)
        all_controls = {}
        for call in calls:
            params = json.loads(call[-1])["params"]
            # Object + property + struct headers, then aligned strings and POD values.
            size = 32 + sum(8 + ((len(key.encode()) + 1 + 7) // 8) * 8 + 16 for key in params[::2])
            self.assertLessEqual(size, 1024)
            all_controls.update(zip(params[::2], params[1::2]))
        self.assertEqual(all_controls, M["controls"](after))

    def test_failed_atomic_write_preserves_config(self):
        path = M["paths"]()[0] / "config.json"
        M["atomic_json"](path, {"test": "old"})
        with patch("os.replace", side_effect=OSError("full disk")):
            with self.assertRaises(OSError):
                M["atomic_json"](path, {"test": "new"})
        self.assertEqual(json.loads(path.read_text()), {"test": "old"})
        self.assertEqual(list(path.parent.glob(".simplevol-*")), [])

    def test_graph_has_protective_settings(self):
        config = dict(M["DEFAULTS"], compressor=1, autogain=1)
        controls = M["controls"](config)
        self.assertEqual(controls["comp:scm"], 0)
        self.assertEqual(controls["comp:scr"], 0)
        self.assertEqual(controls["comp:scs"], 5)
        self.assertEqual(controls["limit:boost"], 0)
        self.assertEqual(controls["level:max_on"], 1)
        self.assertEqual(controls["level:qamp"], 0)
        self.assertAlmostEqual(controls["limit:th"], 10 ** (-1 / 20))
        bypass = M["controls"](dict(config, bypass=1, eq0=12, preamp=6))
        for name in ("comp:enabled", "level:enabled", "limit:enabled", "eq0_l:Gain"):
            self.assertEqual(bypass[name], 0)
        self.assertEqual(bypass["pre_l:Mult"], 1)
        text = M["pipewire_config"](config)
        self.assertTrue(text.startswith("context.properties ="))
        graph = M["graph"](config)["context.modules"][-1]["args"]["filter.graph"]
        self.assertEqual(graph["outputs"], ["meters:Out L", "meters:Out R"])

    def test_mixer_keeps_channel_balance_and_caps_volume(self):
        calls = []
        row = {"index": 7, "volume": {"front-left": {"value": 65536}, "front-right": {"value": 32768}}}
        with patch.dict(G, pulse_list=lambda _: [row], run=lambda *a, **k: calls.append(a)):
            M["mixer_action"]("adjust", "outputs", "7", "100")
        self.assertEqual(calls[-1], ("pactl", "set-sink-volume", "7", "150.0000%", "75.0000%"))

    def test_system_sounds_controls_preserve_other_saved_settings(self):
        event = {"name": M["EVENT_ROLE"], "volumes": [65536, 32768], "channels": [1, 2],
                 "channel_names": ["front-left", "front-right"], "device": "notifications", "mute": False}
        music = {"name": "sink-input-by-media-role:music", "volumes": [22222],
                 "channels": [0], "channel_names": ["mono"], "device": "headphones", "mute": True}
        entries = {event["name"]: event, music["name"]: music}
        writes = []

        class Restore:
            def __enter__(self):
                return self

            def __exit__(self, *_):
                pass

            def read(self):
                return entries

            def write(self, entry):
                writes.append(entry)
                entries[entry["name"]] = entry

        with patch.dict(G, PulseStreamRestore=Restore, pulse_list=lambda kind: [] if kind == "sink-inputs" else self.fail("Must use saved event role")):
            self.assertEqual(M["system_sounds"](), event)
            self.assertFalse(writes, "Opening the mixer must not create or change saved settings")
            M["mixer_action"]("adjust", "playback", "system-sounds", "100")
            self.assertEqual(writes[-1]["volumes"], [98304, 49152])
            M["mixer_action"]("mute", "playback", "system-sounds")
            self.assertTrue(writes[-1]["mute"])
            M["mixer_action"]("volume", "playback", "system-sounds", "40")
            self.assertEqual(writes[-1]["volumes"], [26214, 13107])
            M["mixer_action"]("channel", "playback", "system-sounds", "1:60")
            self.assertEqual(writes[-1]["volumes"], [26214, 39322])
            self.assertEqual(writes[-1]["device"], "notifications")
            self.assertEqual(entries[music["name"]], music)
            for action, value in (("volume", "nan"), ("adjust", "inf"), ("channel", "3:40"),
                                  ("channel", "0:-1"), ("channel", "0:nan"), ("route", "speakers")):
                with self.assertRaises(M["AudioError"]):
                    M["mixer_action"](action, "playback", "system-sounds", value)
            self.assertEqual(len(writes), 4)

    def test_system_sounds_updates_only_live_notification_streams(self):
        entry = M["event_sound_entry"]({})
        entry.update(volumes=[65536, 32768], channels=[1, 2], channel_names=["front-left", "front-right"])
        saved, calls = [], []

        class Restore:
            def __enter__(self):
                return self

            def __exit__(self, *_):
                pass

            def read(self):
                return {entry["name"]: entry}

            def write(self, value):
                saved.append(value)

        rows = [{"index": 1, "properties": {"media.role": "event"}, "volume": {"front-left": {}, "front-right": {}}},
                {"index": 2, "properties": {"media.role": "music"}, "volume": {"mono": {}}},
                {"index": 3, "properties": {"module-stream-restore.id": M["EVENT_ROLE"]}, "volume": {"mono": {}}},
                {"index": 4, "properties": {"media.role": "event", "module-stream-restore.id": "custom"}, "volume": {"mono": {}}}]
        with patch.dict(G, PulseStreamRestore=Restore, pulse_list=lambda _: rows, run=lambda *args, **_: calls.append(args)):
            M["system_sounds_action"]("volume", "40")
        self.assertEqual(calls, [("pactl", "set-sink-input-volume", "1", "39.9994%", "19.9997%"),
                                 ("pactl", "set-sink-input-mute", "1", "0"),
                                 ("pactl", "set-sink-input-volume", "3", "29.9995%"),
                                 ("pactl", "set-sink-input-mute", "3", "0")])
        self.assertEqual(len(saved), 1)

    def test_system_sounds_without_an_event_entry_or_extension(self):
        default = M["event_sound_entry"]({})
        self.assertEqual(default["volumes"], [65536])
        self.assertFalse(default["mute"])
        with patch.dict(G, PulseStreamRestore=lambda: (_ for _ in ()).throw(M["AudioError"]("Unsupported"))):
            self.assertIsNone(M["system_sounds"]())

    def test_system_sounds_row_remains_without_active_playback(self):
        sounds = M["event_sound_entry"]({})
        snap = {"effects": {"running": False, "config": M["DEFAULTS"], "meters": {}}, "autostart": False,
                "info": {}, "data": {key: [] for key in M["KINDS"]}, "system_sounds": sounds}
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            M["emit_snapshot"](snap)
        rows = [line.split("\t") for line in output.getvalue().splitlines() if line.startswith("ROW\t")]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][1:7], ["playback", "system-sounds", "system-sounds", "System Sounds",
                                       "Notification and event sounds", "100.0"])
        snap["system_sounds"] = None
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            M["emit_snapshot"](snap)
        self.assertNotIn("ROW\t", output.getvalue())

    def test_bridge_checkpoints_commands_and_reconciles_rejections(self):
        events = iter(("tick", ["adjust", "playback", "42", "2"], ["mute", "playback", "42"],
                       ["refresh"], "tick", None))
        level = 80

        class Commands:
            def get(self, **_):
                event = next(events)
                if event == "tick":
                    raise queue.Empty
                return event

            def empty(self):
                return True

        class Reader:
            def __init__(self, **_):
                pass

            def start(self):
                pass

        def action(fields):
            nonlocal level
            if fields[0] == "adjust":
                level += float(fields[3])
            elif fields[0] == "mute":
                raise M["AudioError"]("Denied")

        def snapshot():
            data = {name: [] for name in M["KINDS"]}
            data["playback"] = [{"index": 42, "volume": {"mono": {"value": level / 100 * 65536}}}]
            return {"info": {}, "data": data, "effects": {"running": False, "config": M["DEFAULTS"], "meters": {}},
                    "autostart": False, "system_sounds": None}

        output = io.StringIO()
        with patch.dict(G, queue=SimpleNamespace(Queue=lambda **_: Commands(), Empty=queue.Empty),
                        threading=SimpleNamespace(Thread=Reader), bridge_action=action, snapshot=snapshot):
            with contextlib.redirect_stdout(output):
                M["bridge"]()
        lines = [line.split("\t") for line in output.getvalue().splitlines()]
        self.assertEqual([line for line in lines if line[0] == "BEGIN"],
                         [["BEGIN", "0"], ["BEGIN", "1"], ["BEGIN", "2"], ["BEGIN", "3"], ["BEGIN", "3"]])
        self.assertIn(["MESSAGE", "error", "Denied", "2"], lines)
        self.assertEqual([float(line[6]) for line in lines if line[0] == "ROW"], [80, 82, 82, 82, 82])

    def test_mixer_arguments_never_become_shell_text(self):
        calls = []
        row = {"index": 7}
        value = 'speakers; $(touch /tmp/nope)'
        with patch.dict(G, pulse_list=lambda _: [row], run=lambda *a, **k: calls.append(a)):
            M["mixer_action"]("route", "playback", "7", value)
        self.assertEqual(calls[-1], ("pactl", "move-sink-input", "7", value))

    def test_restore_only_routes_owned_by_effects(self):
        calls = []
        sinks = [{"name": "speakers", "index": 1}, {"name": "headphones", "index": 2},
                 {"name": M["SINK"], "index": 3}]
        streams = [
            {"index": 10, "sink": 3, "properties": {"object.serial": "10"}},
            {"index": 11, "sink": 3, "properties": {"object.serial": "NEW"}},
            {"index": 12, "sink": 2, "properties": {}},
        ]
        state = {"previous_default": "speakers", "routes": {
            "10": {"sink": "headphones", "serial": "10"},
            "11": {"sink": "headphones", "serial": "OLD"},
        }}
        with patch.dict(G, pulse_list=lambda kind: sinks if kind == "sinks" else streams,
                        pulse_info=lambda: {"default_sink_name": M["SINK"]},
                        run=lambda *a, **k: calls.append(a)):
            M["restore_routes"](state)
        self.assertEqual(calls, [("pactl", "set-default-sink", "speakers"),
                                 ("pactl", "move-sink-input", "10", "headphones"),
                                 ("pactl", "move-sink-input", "11", "speakers")])

    def test_restore_does_not_override_new_default(self):
        calls = []
        with patch.dict(G, pulse_list=lambda kind: [{"index": 1, "name": "speakers"}] if kind == "sinks" else [],
                        pulse_info=lambda: {"default_sink_name": "speakers"}, run=lambda *a, **k: calls.append(a)):
            M["restore_routes"]({"previous_default": "gone"})
        self.assertEqual(calls, [])

    def test_protocol_escapes_control_characters(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            M["emit"]("ROW", "bad\tname\n\x1b[31m%")
        self.assertEqual(out.getvalue().count("\n"), 1)
        self.assertNotIn("\x1b", out.getvalue())
        self.assertIn("%09", out.getvalue())


if __name__ == "__main__":
    unittest.main()
