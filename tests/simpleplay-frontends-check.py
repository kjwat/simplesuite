#!/usr/bin/env python3
"""Real engine playback through all five frontends, without speaker output."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parent.parent
build = Path(sys.argv[1]).resolve()
core = Path(os.environ.get('SIMPLEPLAY_TEST_CORE', build / 'simpleplay-core')).resolve()

def run(command, **kwargs):
    return subprocess.run([str(arg) for arg in command], check=True, timeout=60, **kwargs)

with tempfile.TemporaryDirectory(prefix='simpleplay-frontends-', dir=build) as directory:
    directory = Path(directory)
    media = directory / 'song.flac'
    (directory / 'home').mkdir()
    run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
         'sine=frequency=440:duration=4', media])
    env = dict(os.environ, SIMPLESUITE_AUDIO_PLAYER=str(core), SDL_AUDIODRIVER='dummy',
               SIMPLEPLAY_TEST_MEDIA=str(media), SIMPLESUITE_DATADIR=str(root / 'assets'),
               HOME=str(directory / 'home'))
    for key in ('SIMPLECAL_ALARM_PLAYER', 'SIMPLECLOCK_ALARM_PLAYER', 'DISPLAY', 'WAYLAND_DISPLAY',
                'DBUS_SESSION_BUS_ADDRESS'):
        env.pop(key, None)
    if os.environ.get('SIMPLEPLAY_TEST_DEFAULT') == '1':
        env.pop('SIMPLESUITE_AUDIO_PLAYER', None)
    run([os.environ.get('MAKE', 'make'), 'test-simpleflac-player', 'test-simpleradio-ipc',
         'test-simplepod-ipc'], cwd=root, env=env)
    compiler = shlex.split(os.environ.get('CC', 'cc'))
    flags = shlex.split(subprocess.check_output(
        [os.environ.get('PKG_CONFIG', 'pkg-config'), '--cflags', '--libs', 'ncursesw', 'libcurl'], text=True))
    for program in ('simplecal', 'simpleclock'):
        binary = directory / program
        options = ['-DTEST_SIMPLECLOCK'] if program == 'simpleclock' else []
        run([*compiler, '-O2', *options, root / 'tests/simpleplay-alarm-check.c',
             *flags, '-o', binary], cwd=root)
        home = directory / (program + '-home')
        home.mkdir()
        run([binary], env=env | {'HOME': str(home)})
    print('SimplePlay frontend checks passed: FLAC, radio, podcasts, calendar and clock alarms')
