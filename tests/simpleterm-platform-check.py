#!/usr/bin/env python3
"""Exercise the native binary and staged install without needing a display."""
import os
from pathlib import Path
import plistlib
import platform
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent


def run(*arguments, **kwargs):
    return subprocess.run(arguments, check=True, text=True, **kwargs)


def check_cli(binary):
    assert run(str(binary), "--version", capture_output=True).stdout.startswith("simpleterm ")
    assert "Ctrl+Shift+C/V" in run(str(binary), "--help", capture_output=True).stdout
    for arguments in (("--zoom", "nan"), ("--zoom", "0"), ("--geometry", "80x"),
                      ("--geometry", "0x24"), ("--bad-option",), ("--tab",), ("--",), ("-e",)):
        result = subprocess.run([str(binary), *arguments], capture_output=True, text=True, timeout=10)
        assert result.returncode == 2, (arguments, result)
    print("OK CLI validation without a display", flush=True)


def check_staging(binary, stage):
    environment = dict(os.environ, DESTDIR=str(stage), SIMPLETERM_BUILD_DIR=str(binary.parent))
    run(str(ROOT / "install-simpleterm.sh"), "--no-deps", env=environment, timeout=60)
    installed = stage / "usr/local/bin/simpleterm"
    assert installed.read_bytes() == binary.read_bytes()
    assert installed.stat().st_mode & 0o777 == 0o755
    assert (stage / "usr/local/share/simplesuite/simpleterm/SIMPLETERM.md").is_file()
    application = stage / "Applications/Simpleterm.app"
    bundle = stage / "usr/local/share/simplesuite/simpleterm/Simpleterm.app"
    if platform.system() == "Darwin":
        assert application.is_symlink()
        assert os.readlink(application) == "/usr/local/share/simplesuite/simpleterm/Simpleterm.app"
        info = plistlib.loads((bundle / "Contents/Info.plist").read_bytes())
        assert info["CFBundleIdentifier"] == "org.simplesuite.Simpleterm"
        assert (bundle / "Contents/MacOS" / info["CFBundleExecutable"]).stat().st_mode & 0o777 == 0o755
    else:
        assert (stage / "usr/local/share/applications/org.simplesuite.Simpleterm.desktop").is_file()
    unrelated = stage / "usr/local/bin/unrelated"
    unrelated.write_text("keep me")
    run(str(ROOT / "install-simpleterm.sh"), "--uninstall", env=environment, timeout=10)
    assert not installed.exists()
    assert not application.is_symlink()
    assert not bundle.exists()
    assert unrelated.read_text() == "keep me"
    print("OK native staged install, launcher, verification, and uninstall", flush=True)


if __name__ == "__main__":
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/simpleterm").resolve()
    check_cli(binary)
    with tempfile.TemporaryDirectory(prefix="simpleterm-native-") as temporary:
        check_staging(binary, Path(temporary) / "stage")
