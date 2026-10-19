#!/usr/bin/env python3
"""Run pointer checks on a private display with isolated player settings."""
from pathlib import Path
import os
import resource
import shutil
import subprocess
import sys
import tempfile

build = Path(sys.argv[1]).resolve()
for tool in ("xvfb-run", "dbus-run-session", "xdotool"):
    if not shutil.which(tool):
        sys.exit(f"Install {tool} to run SimplePlay menu checks.")

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
with tempfile.TemporaryDirectory(prefix="simpleplay-menus-", dir=build) as temporary:
    bus_config = Path(temporary) / "session.conf"
    bus_config.write_text('''<busconfig>
<type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth>
<policy context="default"><allow own="*"/><allow send_destination="*"/>
<allow receive_sender="*"/></policy></busconfig>''')
    env = dict(os.environ, GDK_BACKEND="x11", SDL_AUDIODRIVER="dummy", NO_AT_BRIDGE="1",
               GTK_USE_PORTAL="0", GIO_USE_VFS="local", G_DEBUG="fatal-warnings", GSETTINGS_BACKEND="memory",
               XDG_CONFIG_HOME=temporary + "/config", XDG_CACHE_HOME=temporary + "/cache")
    subprocess.run(["dbus-run-session", f"--config-file={bus_config}", "--", "xvfb-run", "-a", "-s", "-screen 0 1440x1000x24",
                    str(build / "simpleplay-menu-check")], env=env, check=True, timeout=30)
