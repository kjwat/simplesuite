#!/usr/bin/env python3
"""Test saved sound roles against a private PipeWire PulseAudio server.

The temporary runtime directory, explicit Pulse server and session manager
with hardware monitoring disabled keep playback away from desktop audio.
"""
import contextlib
import copy
import json
import os
from pathlib import Path
import runpy
import select
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
M = runpy.run_path(str(ROOT / "simplevol-audio"))


def wait_for(predicate):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(.02)
    raise AssertionError("Private audio server did not reach the expected state")


def check():
    if any(not shutil.which(tool) for tool in ("pipewire", "pipewire-pulse", "wireplumber", "dbus-daemon", "pw-dump", "pactl", "pacat")):
        print("SimpleVol System Sounds integration: skipped (needs PipeWire, WirePlumber and PulseAudio client tools)")
        return
    version = subprocess.run(["wireplumber", "--version"], capture_output=True, text=True, check=True).stdout
    if "libwireplumber 0.4." not in version:
        print("SimpleVol System Sounds integration: skipped (private fixture uses WirePlumber 0.4 Lua configuration)")
        return
    with tempfile.TemporaryDirectory(prefix="simplevol-sounds-") as tmp:
        directory = Path(tmp)
        runtime = directory / "run"
        runtime.mkdir(mode=0o700)
        environment = {**os.environ, "XDG_RUNTIME_DIR": str(runtime), "PIPEWIRE_RUNTIME_DIR": str(runtime),
                       "XDG_CONFIG_HOME": str(directory / "config"), "XDG_STATE_HOME": str(directory / "state"),
                       "PULSE_RUNTIME_PATH": str(runtime / "pulse"), "PULSE_SERVER": "unix:" + str(runtime / "pulse/native"),
                       "PIPEWIRE_REMOTE": "pipewire-0", "GIO_USE_VFS": "local"}
        processes = []
        stream_processes = []
        log = (directory / "server.log").open("w+")
        original_environment = dict(os.environ)
        os.environ.clear()
        os.environ.update(environment)
        try:
            # Disable every hardware monitor before WirePlumber's enable-all
            # configuration runs. It supplies only policy for our null sink.
            for fragment, text in (
                    ("main.lua.d/85-sounds-test.lua", "alsa_monitor.enabled = false\nv4l2_monitor.enabled = false\nlibcamera_monitor.enabled = false\n"),
                    ("bluetooth.lua.d/85-sounds-test.lua", "bluez_monitor.enabled = false\n")):
                path = directory / "config/wireplumber" / fragment
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            state = directory / "state/wireplumber/restore-stream"
            state.parent.mkdir(parents=True)
            state.write_text("[restore-stream]\nOutput/Audio:media.role:Notification:volume=1.0\n")
            bus = subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--print-address=1"],
                                   stdout=subprocess.PIPE, stderr=log, env=environment)
            processes.append(bus)
            assert select.select([bus.stdout], [], [], 2)[0], "Private D-Bus did not start"
            environment["DBUS_SESSION_BUS_ADDRESS"] = bus.stdout.readline().decode().strip()
            os.environ["DBUS_SESSION_BUS_ADDRESS"] = environment["DBUS_SESSION_BUS_ADDRESS"]
            for executable in ("pipewire", "pipewire-pulse"):
                processes.append(subprocess.Popen([executable], env=environment, stdout=log, stderr=log))
            wait_for(lambda: (runtime / "pipewire-0").exists())
            processes.append(subprocess.Popen(["wireplumber"], env=environment, stdout=log, stderr=log))
            wait_for(lambda: (runtime / "pulse/native").exists())

            def command(*args):
                result = subprocess.run(args, env=environment, capture_output=True, text=True, timeout=3)
                assert result.returncode == 0, (args, result.stderr)
                return result.stdout

            wait_for(lambda: {"default", "route-settings"}.issubset(
                {row.get("props", {}).get("metadata.name") for row in json.loads(command("pw-dump"))}))
            command("pactl", "load-module", "module-null-sink", "sink_name=sounds_test", "channels=2")
            wait_for(lambda: any(row["name"] == "sounds_test" for row in json.loads(command("pactl", "-f", "json", "list", "sinks"))))
            command("pactl", "set-default-sink", "sounds_test")

            def entries():
                with M["PulseStreamRestore"]() as restore:
                    return restore.read()

            before = entries()
            event = M["event_sound_entry"]({})
            event.update(volumes=[65536, 32768], channels=[1, 2], channel_names=["front-left", "front-right"],
                         device=None)
            music = copy.deepcopy(event)
            music.update(name="sink-input-by-media-role:music", volumes=[40000, 30000], mute=True)
            with M["PulseStreamRestore"]() as restore:
                restore.write(music)
            with M["PulseStreamRestore"]() as restore:
                restore.write(event)
            saved = entries()
            assert saved[event["name"]] == event, saved
            assert saved[music["name"]] == music, saved
            for name, entry in before.items():
                if name not in (event["name"], music["name"]):
                    assert saved[name] == entry

            def start_stream(role):
                process = subprocess.Popen(["pacat", "--playback", "--raw", "--format=s16le", "--rate=48000",
                                            "--channels=2", "--device=sounds_test", "--property=media.role=" + role],
                                           stdin=subprocess.PIPE, stdout=log, stderr=log, env=environment)
                processes.append(process)
                stream_processes.append(process)
                process.stdin.write(bytes(4096))
                process.stdin.flush()
                return wait_for(lambda: next((row for row in json.loads(command("pactl", "-f", "json", "list", "sink-inputs"))
                                             if row.get("properties", {}).get("media.role") == role and
                                             row.get("sink") != 4294967295), None))

            event_stream = start_stream("event")
            music_stream = start_stream("music")
            M["mixer_action"]("volume", "playback", "system-sounds", "40")
            M["mixer_action"]("mute", "playback", "system-sounds")
            updated = entries()
            assert updated[event["name"]]["volumes"] == [26214, 13107], updated
            assert updated[event["name"]]["mute"]
            assert updated[event["name"]]["device"] is None
            assert updated[music["name"]] == music, updated
            def event_updated():
                live = json.loads(command("pactl", "-f", "json", "list", "sink-inputs"))
                row = next(stream for stream in live if stream["index"] == event_stream["index"])
                return live if row["mute"] and list(row["volume"].values())[0]["value"] == 26214 else None

            live = wait_for(event_updated)
            current_event = next(row for row in live if row["index"] == event_stream["index"])
            current_music = next(row for row in live if row["index"] == music_stream["index"])
            assert list(current_event["volume"].values())[0]["value"] == 26214, current_event
            assert current_event["mute"], current_event
            assert current_music["volume"] == music_stream["volume"] and current_music["mute"] == music_stream["mute"], current_music
            for process in stream_processes:
                process.terminate()
                process.wait(timeout=2)
            wait_for(lambda: not json.loads(command("pactl", "-f", "json", "list", "sink-inputs")))
            assert M["system_sounds"]()["volumes"] == [26214, 13107]
            assert M["system_sounds"]()["mute"]
            # A new notification must inherit the saved control without any
            # application stream having existed while the mixer was opened.
            new_event = start_stream("event")
            assert list(new_event["volume"].values())[0]["value"] == 26214, new_event
            assert new_event["mute"], new_event
            for process in processes:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=2)
            started = time.monotonic()
            assert M["system_sounds"]() is None
            assert time.monotonic() - started < 1.5
            print("SimpleVol System Sounds integration: saved volume/mute, live and future notifications, unrelated roles, server unavailable passed")
        except BaseException:
            log.flush()
            log.seek(0)
            print(log.read()[-4000:])
            raise
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    with contextlib.suppress(subprocess.TimeoutExpired):
                        process.wait(timeout=2)
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=2)
                if process.stdin:
                    process.stdin.close()
                if process.stdout:
                    process.stdout.close()
            os.environ.clear()
            os.environ.update(original_environment)
            log.close()


if __name__ == "__main__":
    check()
