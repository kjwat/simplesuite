#!/usr/bin/env python3
"""Check automatic delivery and reading against the real terminal event loop."""

import errno
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import shlex
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


def run_live(binary):
    with tempfile.TemporaryDirectory(prefix="simplemail-live-pty-") as temporary:
        root = Path(temporary)
        mail = root / "mail"
        for role in ("Inbox", "Sent", "Drafts", "Archive", "Trash"):
            for sub in ("cur", "new", "tmp"):
                (mail / role / sub).mkdir(parents=True)
        script = root / "live-fixture.py"
        script.write_text(
            "from pathlib import Path\nimport os, select, sys, time\n"
            "root=Path(sys.argv[1])\n"
            "count=root/'launch-count'\ncount.write_text(str(int(count.read_text())+1 if count.exists() else 1))\n"
            "(root/'receiver-pid').write_text(str(os.getpid()))\n"
            "print('SIMPLEMAIL READY', flush=True)\n"
            "delivered=set(); released=False\n"
            "while True:\n"
            " if select.select([sys.stdin], [], [], 0.01)[0]:\n"
            "  command=os.read(sys.stdin.fileno(), 4096)\n"
            "  if not command or b'q' in command: break\n"
            "  if b'c' in command:\n"
            "   (root/'manual-check').write_text('done')\n"
            "   print('SIMPLEMAIL CHECKED', flush=True)\n"
            " for n, subject, body in [(1, 'Instant live arrival', 'Instant body stays open.'), (2, 'Second live arrival', 'Second message body.')]:\n"
            "  if n not in delivered and (root/('inject-'+str(n))).exists():\n"
            "   raw='From: sender@example.test\\nSubject: '+subject+'\\nMessage-ID: <live-'+str(n)+'@example.test>\\nDate: Fri, 2 Oct 2026 10:00:0'+str(n)+' +0000\\nContent-Type: text/plain\\n\\n'+body+'\\n'\n"
            "   (root/('mail/Inbox/new/live-'+str(n)+'.eml')).write_text(raw)\n"
            "   delivered.add(n)\n"
            "   (root/('published-'+str(n))).write_text(str(time.monotonic()))\n"
            "   print('SIMPLEMAIL MAIL', flush=True)\n"
            " if not released and (root/'release-cleanup').exists():\n"
            "  released=True\n  (root/'cleanup-finished').write_text('done')\n"
            "  print('SIMPLEMAIL CHECKED', flush=True)\n")
        config = root / "config/simplemail"
        config.mkdir(parents=True)
        command = " ".join(shlex.quote(str(value)) for value in (sys.executable, script, root))
        config.joinpath("config").write_text(
            f"maildir={mail}\nwatch_cmd={command}\n"
            f"sync_cmd=touch {shlex.quote(str(root / 'legacy-check'))}\n"
            "fetch_on_start=1\ncheck_interval=1\nsend_cmd=false\n")
        environment = {key: value for key, value in os.environ.items() if not key.startswith("SIMPLEMAIL_")}
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

        def wait_until(condition, description, seconds=5):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                drain()
                if condition():
                    return
                if process.poll() is not None:
                    raise AssertionError(f"SimpleMail exited during {description}: {process.returncode}")
                select.select([master], [], [], 0.01)
            raise AssertionError(f"Timed out during {description}")

        try:
            wait_until(lambda: b"Mail connected." in output, "starting live delivery")
            output.clear()
            started = time.monotonic()
            (root / "inject-1").touch()
            wait_until(lambda: b"Instant live arrival" in output, "displaying live arrival before cleanup")
            latency = time.monotonic() - started
            assert latency < 0.75, f"Live UI waited {latency:.3f}s for a delivered message"
            assert not (root / "cleanup-finished").exists(), "UI waited for server cleanup"
            assert not (root / "legacy-check").exists(), "Live mode started another polling command"
            os.write(master, b"\n")
            wait_until(lambda: b"Instant body stays open." in output, "opening a live message")
            output.clear()
            (root / "release-cleanup").touch()
            (root / "inject-2").touch()
            wait_until(lambda: (root / "published-2").exists(), "delivering while reading")
            time.sleep(0.2)
            drain()
            assert b"Second live arrival" not in output, "Arrival replaced the open reader"
            os.write(master, b"\x7f")
            wait_until(lambda: b"Second live arrival" in output, "showing the new Inbox after reading")
            os.write(master, b"p")
            wait_until(lambda: (root / "manual-check").exists(), "checking over the existing connection")
            assert not (root / "legacy-check").exists()
            first_pid = int((root / "receiver-pid").read_text())
            os.kill(first_pid, signal.SIGTERM)
            wait_until(lambda: (root / "launch-count").read_text().strip() == "2" and
                       (root / "receiver-pid").read_text().strip() not in ("", str(first_pid)),
                       "restarting an exited receiver")
            receiver_pid = int((root / "receiver-pid").read_text())
            os.write(master, b"qy")
            wait_until(lambda: process.poll() is not None, "quitting live mail")
            assert process.returncode == 0
            try:
                os.kill(receiver_pid, 0)
            except ProcessLookupError:
                pass
            else:
                raise AssertionError("Receiver stayed running after SimpleMail quit")
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            if (root / "receiver-pid").exists():
                try:
                    os.kill(int((root / "receiver-pid").read_text()), signal.SIGKILL)
                except ProcessLookupError:
                    pass
            os.close(master)
    print(f"SimpleMail live terminal checks passed; fixture arrival displayed in {latency:.3f}s before cleanup.")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: simplemail-delivery-pty.py /path/to/simplemail")
    run(str(Path(sys.argv[1]).resolve()))
    run_live(str(Path(sys.argv[1]).resolve()))
