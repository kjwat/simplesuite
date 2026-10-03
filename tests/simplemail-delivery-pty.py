#!/usr/bin/env python3
"""Check automatic delivery and reading against the real terminal event loop."""

import errno
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time


def run(binary):
    with tempfile.TemporaryDirectory(prefix="simplemail-delivery-pty-") as temporary:
        root = Path(temporary)
        mail = root / "mail"
        for role in ("Inbox", "Sent", "Drafts", "Archive", "Trash"):
            for sub in ("cur", "new", "tmp"):
                (mail / role / sub).mkdir(parents=True, mode=0o700)
        (mail / "Inbox/new/original.eml").write_text(
            "From: sender@example.test\nSubject: Original reader message\n"
            "Message-ID: <old@example.test>\nDate: 01 Oct 2026 10:00:00 +0000\n\n"
            "Original message body is being read.\n")
        counter = root / "fetch-count"
        script = root / "fetch-fixture.py"
        script.write_text(
            "from pathlib import Path\nimport os, sys\n"
            "root=Path(sys.argv[1])\ncount=root/'fetch-count'\n"
            "n=int(count.read_text())+1 if count.exists() else 1\n"
            "if n==2:\n"
            " (root/'mail/Inbox/new/arrival.eml').write_text('From: sender@example.test\\nSubject: New inbox arrival\\nMessage-ID: <new@example.test>\\nDate: 02 Oct 2026 10:00:00 +0000\\n\\nIncoming body.\\n')\n"
            " for sub in ('cur','new','tmp'): (root/'mail/Spam'/sub).mkdir(parents=True, exist_ok=True)\n"
            "staged=root/'fetch-count.tmp'\nstaged.write_text(str(n))\nos.replace(staged,count)\n"
            "if n==1:\n"
            " print('simplemail-fetch: Mail server disconnected during the check.', file=sys.stderr)\n"
            " sys.exit(1)\n")
        config_dir = root / "config/simplemail"
        config_dir.mkdir(parents=True)
        config_dir.joinpath("config").write_text(
            f"maildir={mail}\nsync_cmd=exec '{sys.executable}' '{script}' '{root}'\n"
            "fetch_on_start=1\ncheck_interval=1\nsend_cmd=false\n")
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith("SIMPLEMAIL_")}
        environment.update(HOME=str(root), XDG_CONFIG_HOME=str(root / "config"),
                           XDG_STATE_HOME=str(root / "state"), TERM="xterm-256color")
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 120, 0, 0))
        process = subprocess.Popen([binary], stdin=slave, stdout=slave, stderr=slave,
                                   env=environment, start_new_session=True)
        os.close(slave)
        output = bytearray()

        def drain():
            while select.select([master], [], [], 0)[0]:
                try:
                    data = os.read(master, 65536)
                except OSError as error:
                    if error.errno == errno.EIO:
                        return
                    raise
                if not data:
                    return
                output.extend(data)

        def wait_until(condition, description, seconds=10):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                drain()
                if condition():
                    return
                if process.poll() is not None:
                    raise AssertionError(f"SimpleMail exited during {description}, code {process.returncode}")
                select.select([master], [], [], 0.05)
            raise AssertionError(f"Timed out during {description}")

        def fetch_count():
            return int(counter.read_text()) if counter.exists() else 0

        try:
            wait_until(lambda: fetch_count() >= 1, "startup download")
            wait_until(lambda: b"Mail server disconnected during the check." in output,
                       "showing the actual failure reason")
            assert b"Retrying automatically" in output
            os.write(master, b"\n")
            wait_until(lambda: b"Original message body is being read." in output, "opening the reader")
            output.clear()
            wait_until(lambda: fetch_count() >= 3, "periodic downloads while reading")
            wait_until(lambda: b"complete." in output or b"checked." in output,
                       "clearing the failure notice after recovery")
            drain()
            assert b"New inbox arrival" not in output, "Download switched the open reader to another message"
            error_log = root / "state/simplemail/pull-error.log"
            assert "Mail server disconnected" in error_log.read_text(), "A successful check erased the failure details"
            os.write(master, b"\x7f")
            wait_until(lambda: b"New inbox arrival" in output, "refreshing the inbox after reading")
            output.clear()
            os.write(master, b"m")
            wait_until(lambda: b"Spam" in output, "showing the delivered Spam folder")
            os.write(master, b"qy")
            deadline = time.monotonic() + 5
            while process.poll() is None and time.monotonic() < deadline:
                drain()
                select.select([master], [], [], 0.05)
            assert process.poll() == 0, "SimpleMail failed to exit cleanly"
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            os.close(master)
    print("SimpleMail terminal delivery checks passed.")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: simplemail-delivery-pty.py /path/to/simplemail")
    run(str(Path(sys.argv[1]).resolve()))
