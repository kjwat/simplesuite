#!/usr/bin/env python3
"""Private-display integration tests; never type into the user's desktop."""
import os
from pathlib import Path
import select
import resource
import shutil
import subprocess
import sys
import tempfile


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, **kwargs)


resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
binary = Path(sys.argv[1]).resolve()
harness = binary.with_name("simpleterm-check")
repo = Path(__file__).resolve().parent.parent
assert run(str(binary), "--version", capture_output=True).stdout.startswith("simpleterm ")
assert "Ctrl+Shift+C/V" in run(str(binary), "--help", capture_output=True).stdout
for args in [("--zoom", "nan"), ("--zoom", "0"), ("--geometry", "80x"),
             ("--geometry", "0x24"), ("--bad-option",), ("--",), ("-e",)]:
    result = subprocess.run([str(binary), *args], capture_output=True, text=True)
    assert result.returncode == 2, (args, result)
print("OK CLI validation without a display", flush=True)

for tool in ("Xvfb", "xdotool", "dbus-run-session"):
    if not shutil.which(tool):
        sys.exit(f"Install {tool} to run Simpleterm integration tests.")

with tempfile.TemporaryDirectory(prefix="simpleterm-tests-") as temporary:
    temp = Path(temporary)
    bus_config = temp / "session.conf"
    bus_config.write_text('''<busconfig>
<type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth>
<policy context="default"><allow own="*"/><allow send_destination="*"/>
<allow receive_sender="*"/></policy></busconfig>''')
    bus = ["dbus-run-session", f"--config-file={bus_config}", "--"]
    read_fd, write_fd = os.pipe()
    log = (temp / "xvfb.log").open("w+")
    xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1280x900x24",
                             "-nolisten", "tcp"], pass_fds=(write_fd,), stdout=log, stderr=log)
    os.close(write_fd)
    try:
        ready, _, _ = select.select([read_fd], [], [], 15)
        if not ready:
            log.seek(0)
            sys.exit("Xvfb did not start: " + log.read())
        display = os.read(read_fd, 64).decode().strip()
        assert display.isdigit(), "Xvfb did not return a display number"
        runtime = temp / "runtime"
        runtime.mkdir(mode=0o700)
        env = dict(os.environ, DISPLAY=f":{display}", GDK_BACKEND="x11", NO_AT_BRIDGE="1",
                   G_DEBUG="fatal-warnings", LC_ALL="C.UTF-8", GTK_USE_PORTAL="0",
                   GIO_USE_VFS="local", XDG_CURRENT_DESKTOP="", XDG_RUNTIME_DIR=str(runtime),
                   GSETTINGS_BACKEND="memory", XDG_CONFIG_HOME=str(temp / "config"))
        env.pop("WAYLAND_DISPLAY", None)
        env.pop("DBUS_SESSION_BUS_ADDRESS", None)
        # Verify the shipped executable's real command-line entry point.
        output = temp / "command.txt"
        run(*bus, str(binary), "--working-directory", temporary,
            "--", "/bin/sh", "-c", 'printf "%s:%s" "$PWD" "$TERM" > "$1"', "sh", str(output),
            env=env, timeout=15)
        assert output.read_text() == f"{temporary}:xterm-256color"
        print("OK application command launch and working directory", flush=True)
        forwarding = temp / "forwarding.py"
        forwarding.write_text('''import os, subprocess, sys, time
from pathlib import Path
binary, directory = sys.argv[1:]
root = Path(directory)
ready, output = root / "ready", root / "forwarded"
other = root / "directory with spaces"
other.mkdir()
primary = subprocess.Popen([binary, "--title", "--help", "--", "/bin/sh", "-c",
    'printf ready > "$1"; exec sleep 30', "sh", str(ready)])
try:
    for _ in range(100):
        if ready.exists(): break
        assert primary.poll() is None
        time.sleep(0.03)
    assert ready.exists()
    env = dict(os.environ, SIMPLETERM_FORWARDED="secondary-environment")
    subprocess.run([binary, "--tab", "--working-directory", str(other), "--", "/bin/sh", "-c",
        'printf "%s:%s" "$PWD" "$SIMPLETERM_FORWARDED" > "$1"', "sh", str(output)],
        env=env, check=True, timeout=10)
    for _ in range(100):
        if output.exists(): break
        time.sleep(0.03)
    assert output.read_text() == str(other) + ":secondary-environment"
    assert primary.poll() is None
finally:
    primary.terminate()
    primary.wait(timeout=10)
''')
        run(*bus, sys.executable, str(forwarding), str(binary), temporary, env=env, timeout=20)
        print("OK existing-instance command, argument, environment, and directory forwarding", flush=True)
        harness_command = [str(harness)]
        if os.environ.get("SIMPLETERM_DEBUG"):
            harness_command = ["gdb", "-batch", "-ex", "run", "-ex", "bt", "--args", str(harness)]
        run(*bus, *harness_command, env=env, timeout=90)
        config = temp / "config/simpleterm/settings.ini"
        config.write_text("[Text]\ncolumns=100\nrows=30\n[General]\nshow-menubar=false\n"
                          "[Scrolling]\nshow-scrollbar=false\n")
        geometry = temp / "geometry.txt"
        for arguments, expected in [((), "30 100"), (("--geometry", "90x28"), "28 90")]:
            run(*bus, str(binary), *arguments, "--", "/bin/sh", "-c", 'stty size > "$1"',
                "sh", str(geometry), env=env, timeout=15)
            assert geometry.read_text().strip() == expected
        print("OK saved defaults across launches and command-line geometry override", flush=True)
    finally:
        os.close(read_fd)
        xvfb.terminate()
        xvfb.wait(timeout=10)
        log.close()

    stage = temp / "stage"
    stage_env = dict(os.environ, DESTDIR=str(stage), SIMPLETERM_BUILD_DIR=str(binary.parent))
    run(str(repo / "install-simpleterm.sh"), "--no-deps", env=stage_env, timeout=45)
    installed = stage / "usr/local/bin/simpleterm"
    assert installed.read_bytes() == binary.read_bytes()
    assert (stage / "usr/local/share/applications/org.simplesuite.Simpleterm.desktop").is_file()
    assert (stage / "usr/local/share/simplesuite/simpleterm/SIMPLETERM.md").is_file()
    unrelated = stage / "usr/local/bin/unrelated"
    unrelated.write_text("keep me")
    run(str(repo / "install-simpleterm.sh"), "--uninstall", env=stage_env, timeout=10)
    assert not installed.exists()
    assert unrelated.read_text() == "keep me"
    print("OK staged standalone install, launcher, verification, and uninstall", flush=True)

    # Exercise dependency provisioning without touching the host package manager.
    mock = temp / "mock-bin"
    mock.mkdir()
    package_log = temp / "packages.log"
    provisioned = temp / "provisioned"
    scripts = {
        "id": '#!/bin/sh\nprintf "0\\n"\n',
        "apt-get": '#!/bin/sh\nprintf "%s\\n" "$*" >> "$SIMPLETERM_TEST_PACKAGES"\ntouch "$SIMPLETERM_TEST_PROVISIONED"\n',
        "pkg-config": '''#!/bin/sh
if [ "${1-}" = --exists ] && [ ! -e "$SIMPLETERM_TEST_PROVISIONED" ]; then exit 1; fi
exec "$SIMPLETERM_TEST_PKG_CONFIG" "$@"
''',
    }
    for name, contents in scripts.items():
        script = mock / name
        script.write_text(contents)
        script.chmod(0o755)
    mocked_env = dict(os.environ, PATH=str(mock) + os.pathsep + os.environ["PATH"],
                      SIMPLETERM_TEST_PACKAGES=str(package_log),
                      SIMPLETERM_TEST_PROVISIONED=str(provisioned),
                      SIMPLETERM_TEST_PKG_CONFIG=shutil.which("pkg-config"),
                      SIMPLETERM_BUILD_DIR=str(binary.parent))
    mocked_env.pop("DESTDIR", None)
    run(str(repo / "install-simpleterm.sh"), "--build-only", env=mocked_env, timeout=45)
    packages = package_log.read_text()
    assert "update" in packages
    for package in ("libgtk-3-dev", "libvte-2.91-dev", "libpcre2-dev", "fonts-dejavu-core", "xdg-utils"):
        assert package in packages
    print("OK standalone dependency provisioning (mock package manager)", flush=True)

# Default suite dependencies and program manifest must stay independent of GTK.
manifest = run("sh", "-c", '. ./program-manifest.sh; simplesuite_programs Linux 1',
               cwd=repo, capture_output=True).stdout
assert "simpleterm" not in manifest
make_env = dict(os.environ)
for name in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEOVERRIDES", "BUILD_DIR", "CFLAGS", "LDFLAGS"):
    make_env.pop(name, None)
dry_run = run("make", "-n", "all", cwd=repo, env=make_env, capture_output=True).stdout
assert "simpleterm.c" not in dry_run and "vte-2.91" not in dry_run
print("OK default suite build and Scriptorium manifest exclude Simpleterm", flush=True)
