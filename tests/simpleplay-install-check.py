#!/usr/bin/env python3
"""Exercise the real standalone installer in isolated staging prefixes."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
installer = root / "install-simpleplay.sh"
binary = root / "build" / "simpleplay"


def run(*args, env=None, success=True):
    result = subprocess.run([str(installer), *args], cwd=root, env=env, text=True, capture_output=True)
    assert (result.returncode == 0) == success, result.stdout + result.stderr
    return result


assert binary.is_file(), "Build SimplePlay first (make simpleplay)"
run("--help")
run("--unknown", success=False)
run("--build-only", "--uninstall", success=False)
with tempfile.TemporaryDirectory(prefix="simpleplay-install-") as temporary:
    directory = Path(temporary)
    stage = directory / "stage"
    env = os.environ.copy()
    env.update(DESTDIR=str(stage), SIMPLEPLAY_BUILD_DIR=str(root / "build"))
    run("--copy-built", env=env)
    installed = stage / "usr/local/bin/simpleplay"
    desktop = stage / "usr/local/share/applications/org.simplesuite.Simpleplay.desktop"
    compat = stage / "usr/share/applications/org.simplesuite.Simpleplay.desktop"
    assert installed.read_bytes() == binary.read_bytes()
    assert (installed.parent / "simpleplay-core").read_bytes() == (root / "build/simpleplay-core").read_bytes()
    assert os.access(installed, os.X_OK)
    assert desktop.is_file() and compat.resolve() == desktop.resolve()
    assert (stage / "usr/local/share/simplesuite/simpleplay/simpleplay.svg").is_file()
    # Reinstallation must also work while a previous binary/launcher exists.
    run("--copy-built", env=env)
    unrelated = installed.parent / "personal-tool"
    unrelated.write_text("preserve me")
    config = stage / "home/user/.config/simpleplay/settings.ini"
    config.parent.mkdir(parents=True)
    config.write_text("preserve settings")
    run("--uninstall", env=env)
    assert not installed.exists() and not desktop.exists() and not compat.is_symlink()
    assert not (installed.parent / "simpleplay-core").exists()
    assert unrelated.read_text() == "preserve me" and config.read_text() == "preserve settings"
    run("--uninstall", env=env)
    # A staged symlink must never redirect copying/deletion onto the host.
    escaped = directory / "escaped"
    escaped.mkdir()
    bad = directory / "bad"
    bad.mkdir()
    (bad / "usr").symlink_to(escaped, target_is_directory=True)
    bad_env = dict(env, DESTDIR=str(bad))
    run("--copy-built", env=bad_env, success=False)
    run("--uninstall", env=bad_env, success=False)
    assert not list(escaped.iterdir())
    run("--copy-built", env=dict(env, DESTDIR="/"), success=False)
    run("--copy-built", env=dict(env, DESTDIR="relative"), success=False)
    # Server installs need no GUI files or desktop directories.
    headless = directory / "headless"
    run("--copy-built", "--headless-only", env=dict(env, DESTDIR=str(headless)))
    assert (headless / "usr/local/bin/simpleplay-core").is_file()
    assert not (headless / "usr/local/bin/simpleplay").exists()
    assert not (headless / "usr/local/share/applications").exists()
    run("--uninstall", env=dict(env, DESTDIR=str(headless)))
print("SimplePlay installer checks passed: staging, repeat install, uninstall, preservation, symlink boundaries")
