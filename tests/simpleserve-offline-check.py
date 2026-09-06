#!/usr/bin/env python3
"""Exercise real client/daemon IPC without contacting or mounting a real share."""
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time

repo = Path(__file__).resolve().parent.parent
build = Path(os.environ.get("SIMPLESERVE_BUILD_DIR", repo / "build")).resolve()
client, daemon = str(build / "simpleserve"), str(build / "simpleserved")

with tempfile.TemporaryDirectory(prefix="simpleserve-offline.") as directory:
    root = Path(directory)
    endpoint = root / "control.sock"
    env = os.environ | {
        "SIMPLESERVE_TEST_PLATFORM": "Linux",
        "SIMPLESERVE_SOCKET": str(endpoint),
    }

    def cli(*args, success=True):
        command = [client, *args]
        if shutil.which("timeout"):
            command = ["timeout", "5s", *command]
        started = time.monotonic()
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=5.5)
        elapsed = time.monotonic() - started
        assert result.returncode != 124, "outer timeout fired"
        assert (result.returncode == 0) == success, result.stderr
        return result, elapsed

    # Reproduce an old daemon accepting the local socket but never replying.
    with socket.socket(socket.AF_UNIX) as blackhole:
        blackhole.bind(str(endpoint))
        blackhole.listen()
        result, elapsed = cli("status", success=False)
        assert "timed out" in result.stderr.lower(), result.stderr
        assert 2.5 <= elapsed < 4.5, elapsed
        print(f"OK timeout 5s simpleserve status: silent daemon failed in {elapsed:.2f}s")
    endpoint.unlink()

    (root / "home").mkdir()
    (root / "role").write_text("client\n")
    (root / "mounts").write_text("")
    (root / "reachable").write_text("0\n")
    remembered = (
        f"[mounts]\nversion=1\n\n[mount]\nuid={os.getuid()}\ngid={os.getgid()}\n"
        "server=offline-peer\nshare=Library\nfilesystem_id=test-disk\n"
        "hostname=offline-peer.local\nlan_address=192.0.2.50\n"
        "export_path=/exports/Library\naccess=read-write\n"
    )
    (root / "state").write_text(remembered)
    fifo = root / "tailscale-ip"
    os.mkfifo(fifo)
    env |= {
        "SIMPLESERVE_TEST_MODE": "1",
        "SIMPLESERVE_TEST_NO_NETWORK": "1",
        "SIMPLESERVE_TEST_HOME": str(root / "home"),
        "SIMPLESERVE_TEST_MOUNTS": str(root / "mounts"),
        "SIMPLESERVE_TEST_TAILSCALE_IP_FILE": str(fifo),
        "SIMPLESERVE_TEST_TAILSCALE_INSTALLED": "1",
        "SIMPLESERVE_TEST_TAILSCALE_STATE": "stopped",
        "SIMPLESERVE_TEST_LAN_REACHABLE_FILE": str(root / "reachable"),
        "SIMPLESERVE_TEST_TAILSCALE_REACHABLE": "0",
        "SIMPLESERVE_TEST_COMMAND_LOG": str(root / "commands"),
        "SIMPLESERVE_ROLE": str(root / "role"),
        "SIMPLESERVE_CONFIG": str(root / "config"),
        "SIMPLESERVE_STATE": str(root / "state"),
        "SIMPLESERVE_EXPORTS": str(root / "exports"),
        "SIMPLESERVE_FSTAB": str(root / "fstab"),
        "SIMPLESERVE_SMB_CONF": str(root / "smb.conf"),
        "SIMPLESERVE_SAMBA": str(root / "samba.conf"),
    }
    with (root / "daemon.log").open("w+") as log:
        process = subprocess.Popen([daemon], env=env, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 4
            while not endpoint.exists():
                assert process.poll() is None, "daemon exited during startup"
                assert time.monotonic() < deadline, "daemon did not expose its socket"
                time.sleep(0.02)
            # Worker is blocked opening the FIFO. It cannot publish a refresh.
            time.sleep(3.1)
            result, elapsed = cli("status")
            assert elapsed < 1, elapsed
            assert "server unavailable, remembered" in result.stdout, result.stdout
            assert "Background operation pending" in result.stdout, result.stdout
            assert (root / "state").read_text() == remembered
            _, control_elapsed = cli("refresh", success=False)
            assert control_elapsed < 1, control_elapsed
            print(f"OK blocked background I/O: status returned in {elapsed:.2f}s; control reported busy")

            # Release the blocked operation and restore an ordinary input file.
            writer = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
            fifo.unlink()
            fifo.write_text("")
            os.close(writer)
            deadline = time.monotonic() + 4
            while True:
                result, _ = cli("status")
                if "Background operation pending" not in result.stdout:
                    break
                assert time.monotonic() < deadline, "worker failed to resume"
                time.sleep(0.05)
            result, _ = cli("mount", "offline-peer:Library", success=False)
            assert "server=offline-peer" in (root / "state").read_text()
            (root / "reachable").write_text("1\n")
            deadline = time.monotonic() + 20
            while True:
                result, elapsed = cli("status")
                assert elapsed < 1, elapsed
                if "mounted, remembered, route: LAN" in result.stdout:
                    break
                assert time.monotonic() < deadline, result.stdout
                time.sleep(0.1)
            assert "address: 192.0.2.50" in result.stdout, result.stdout
            assert "server=offline-peer" in (root / "state").read_text()
            print("OK remembered share survived outage and automatically mounted after recovery")
        finally:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
            log.seek(0)
            if process.returncode:
                print(log.read())

    # Discovery startup failures must reach the supervisor, which can restart
    # the daemon. A live socket with a dead worker would stay busy forever.
    failed_env = env | {
        "SIMPLESERVE_TEST_NO_NETWORK": "0",
        "SIMPLESERVE_TEST_INIT": "systemd",
        "SIMPLESERVE_TEST_COMMAND_FAIL": "avahi-daemon.service",
    }
    result = subprocess.run([daemon], env=failed_env, capture_output=True, text=True, timeout=3)
    assert result.returncode == 1, result.stderr
    assert "discovery startup failed" in result.stderr, result.stderr
    assert not endpoint.exists(), "failed startup left its control socket behind"
    print("OK discovery startup failure exited for supervisor recovery")
