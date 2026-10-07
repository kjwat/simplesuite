#!/usr/bin/env python3
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parent.parent
MOCK = r'''
import json
import os
from pathlib import Path
import sys

name = Path(sys.argv[0]).name
arguments = sys.argv[1:]
state = Path(os.environ["TEST_STATE"])
with (state / "commands.jsonl").open("a") as log:
    log.write(json.dumps({"tool": name, "args": arguments,
                          "uid": os.environ.get("TEST_UID", "1000"),
                          "frontend": os.environ.get("DEBIAN_FRONTEND")}) + "\n")

if name == "uname":
    print(os.environ.get("TEST_OS", "Linux"))
elif name == "id":
    assert arguments == ["-u"], arguments
    print(os.environ.get("TEST_UID", "1000"))
elif name in ("sudo", "doas", "pkexec"):
    if os.environ.get("TEST_AUTH_FAIL") == "1":
        sys.exit(1)
    while arguments and arguments[0] in ("-n", "--"):
        arguments.pop(0)
    if "--copy-built" in arguments or "--uninstall" in arguments:
        (state / "root-copy").touch()
    else:
        os.environ["TEST_UID"] = "0"
        os.execvpe(arguments[0], arguments, os.environ)
elif name in ("apt-get", "dnf", "zypper", "pacman", "xbps-install", "apk", "pkg"):
    assert os.environ.get("TEST_UID") == "0"
    if os.environ.get("TEST_PACKAGE_FAIL") == "1":
        sys.exit(1)
    if arguments != ["update"]:
        (state / "dependencies-installed").touch()
elif name in ("pkg-config", "pkgconf", "custom-pkg-config"):
    ready = os.environ.get("TEST_LIBRARIES", "ready") == "ready"
    if (state / "dependencies-installed").exists():
        ready = os.environ.get("TEST_AFTER_PACKAGES", "ready") == "ready"
    if not ready:
        if "--print-errors" in arguments:
            print("Requested vte-2.91 >= 0.76; found 0.70", file=sys.stderr)
        sys.exit(1)
elif name in ("make", "gmake", "custom-make"):
    if arguments == ["--version"]:
        print("BSD make" if name == "make" and os.environ.get("TEST_BSD_MAKE") == "1" else "GNU Make 4.4")
    else:
        if os.environ.get("TEST_BUILD_FAIL") == "1":
            sys.exit(1)
        source = Path(arguments[arguments.index("-C") + 1])
        build = next(argument.split("=", 1)[1] for argument in arguments if argument.startswith("BUILD_DIR="))
        output = (source / build).resolve()
        assert output.is_relative_to(Path(os.environ["TEST_TEMP"])), output
        output.mkdir(parents=True, exist_ok=True)
        binary = output / "simpleterm"
        binary.write_text("#!/bin/sh\nprintf 'simpleterm test-build\\n'\n")
        binary.chmod(0o755)
elif name == "fc-match":
    if os.environ.get("TEST_NO_FONTS") != "1" or (state / "dependencies-installed").exists():
        print(state / "font.ttf", end="")
elif name == "cmp":
    if "/usr/local/bin/simpleterm" in arguments:
        assert (state / "root-copy").exists()
        if "-s" in arguments:
            sys.exit(1)
    else:
        os.execv(os.environ["TEST_REAL_CMP"], ["cmp", *arguments])
elif name == "install":
    if os.environ.get("TEST_COPY_FAIL") == "1" and arguments[-2].endswith("/simpleterm"):
        Path(arguments[-1]).write_text("incomplete copy")
        sys.exit(1)
    os.execv(os.environ["TEST_REAL_INSTALL"], ["install", *arguments])
elif name == "update-desktop-database":
    sys.exit(int(os.environ.get("TEST_CACHE_FAIL", "0")))
elif name == "desktop-file-validate":
    sys.exit(int(os.environ.get("TEST_DESKTOP_FAIL", "0")))
elif name not in ("cc", "clang", "xdg-open"):
    raise AssertionError(name)
'''


class InstallerChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="simpleterm-install-")
        self.addCleanup(self.temporary.cleanup)
        self.temp = Path(self.temporary.name)
        self.source = self.temp / "checkout with spaces"
        self.tools = self.temp / "tools"
        self.state = self.temp / "state"
        self.stage = self.temp / "staged files"
        self.home = self.temp / "home"
        for directory in (self.source, self.tools, self.state, self.home):
            directory.mkdir()
        for filename in ("install-simpleterm.sh", "simpleterm.c", "simpleterm-settings.c",
                         "simpleterm-settings.h", "Makefile", "SIMPLETERM.md",
                         "assets/org.simplesuite.Simpleterm.desktop"):
            destination = self.source / filename
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / filename, destination)
        for command in ("dirname", "mkdir", "rmdir", "rm", "mv", "mktemp", "readlink",
                        "cat", "env", "true"):
            executable = shutil.which(command)
            self.assertIsNotNone(executable, command)
            (self.tools / command).symlink_to(executable)
        for command in ("uname", "id", "make", "pkg-config", "cc", "fc-match", "xdg-open",
                        "update-desktop-database", "desktop-file-validate", "cmp", "install"):
            self.mock_tool(command)
        (self.state / "font.ttf").touch()
        self.environment = {
            "PATH": str(self.tools), "HOME": str(self.home), "LC_ALL": "C",
            "TEST_TEMP": str(self.temp), "TEST_STATE": str(self.state),
            "TEST_REAL_CMP": shutil.which("cmp"),
            "TEST_REAL_INSTALL": shutil.which("install"),
        }

    def mock_tool(self, name):
        target = self.tools / name
        target.write_text(f"#!{sys.executable}\n{MOCK}")
        target.chmod(0o755)

    def run_installer(self, *arguments, success=True, **environment):
        result = subprocess.run(
            ["/bin/sh", str(self.source / "install-simpleterm.sh"), *arguments],
            env={**self.environment, **environment}, cwd=self.temp,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=30,
        )
        if success:
            self.assertEqual(result.returncode, 0, result.stdout)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result.stdout

    def commands(self, name=None):
        path = self.state / "commands.jsonl"
        commands = [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []
        return [command for command in commands if name is None or command["tool"] == name]

    def builds(self, name="make"):
        return [command for command in self.commands(name) if command["args"] != ["--version"]]

    def test_help_needs_no_build_dependencies(self):
        for command in ("uname", "make", "pkg-config", "cc"):
            (self.tools / command).unlink()
        self.assertIn("Usage:", self.run_installer("--help"))

    def test_invalid_and_conflicting_options(self):
        for arguments in (("--unknown",), ("--uninstall", "--build-only"),
                          ("--build-only", "--uninstall")):
            with self.subTest(arguments=arguments):
                self.run_installer(*arguments, success=False)
        self.assertFalse(self.builds())

    def test_unsupported_hosts_fail_before_installing_packages(self):
        self.mock_tool("apt-get")
        for host in ("Darwin", "OpenBSD", "MINGW64_NT", "CYGWIN_NT"):
            with self.subTest(host=host):
                output = self.run_installer("--build-only", success=False,
                                            TEST_OS=host, TEST_LIBRARIES="missing")
                self.assertIn("Unsupported host", output)
        self.assertFalse(self.commands("apt-get"))

    def test_staging_install_update_and_uninstall(self):
        self.mock_tool("sudo")
        self.mock_tool("apt-get")
        personal = self.home / ".config/simpleterm/settings.ini"
        personal.parent.mkdir(parents=True)
        personal.write_text("preserve settings")
        for iteration in range(2):
            with self.subTest(iteration=iteration):
                self.run_installer(DESTDIR=str(self.stage))
                installed = self.stage / "usr/local/bin/simpleterm"
                self.assertEqual(installed.read_bytes(), (self.source / "build/simpleterm").read_bytes())
                self.assertEqual(installed.stat().st_mode & 0o777, 0o755)
                self.assertTrue((self.stage / "usr/local/share/applications/org.simplesuite.Simpleterm.desktop").is_file())
                self.assertTrue((self.stage / "usr/local/share/simplesuite/simpleterm/SIMPLETERM.md").is_file())
                self.assertFalse(list(self.stage.rglob(".simpleterm.*")))
        unrelated = self.stage / "usr/local/bin/personal-tool"
        unrelated.touch()
        self.run_installer("--uninstall", DESTDIR=str(self.stage), SIMPLETERM_BUILD_DIR="missing")
        self.assertFalse(installed.exists())
        self.assertTrue(unrelated.exists())
        self.assertEqual(personal.read_text(), "preserve settings")
        self.assertFalse(self.commands("sudo"))
        self.assertFalse(self.commands("apt-get"))
        self.assertFalse(self.commands("update-desktop-database"))

    def test_empty_staged_uninstall_does_not_create_directories(self):
        self.run_installer("--uninstall", DESTDIR=str(self.stage))
        self.assertFalse(self.stage.exists())

    def test_rejects_host_root_and_relative_staging(self):
        root_link = self.temp / "root-link"
        root_link.symlink_to("/", target_is_directory=True)
        for stage in ("relative", "/", "//", str(self.temp / "../.."), str(root_link)):
            with self.subTest(stage=stage):
                self.run_installer("--uninstall", success=False, DESTDIR=stage)
        self.assertFalse(self.builds())

    def test_staged_symlinks_cannot_escape_to_host(self):
        self.stage.mkdir()
        outside = self.temp / "outside"
        outside.mkdir()
        (self.stage / "usr").symlink_to(outside, target_is_directory=True)
        for arguments in ((), ("--uninstall",)):
            with self.subTest(arguments=arguments):
                output = self.run_installer(*arguments, success=False, DESTDIR=str(self.stage))
                self.assertIn("escapes DESTDIR", output)
        self.assertEqual(list(outside.iterdir()), [])

    def test_rejects_source_root_build_aliases(self):
        alias = self.temp / "source-link"
        alias.symlink_to(self.source, target_is_directory=True)
        for build in (str(self.source), str(self.source / "."), str(alias)):
            with self.subTest(build=build):
                output = self.run_installer("--build-only", success=False, SIMPLETERM_BUILD_DIR=build)
                self.assertIn("source root", output)
        self.assertFalse(self.builds())

    def test_unsupported_build_directory_is_rejected_clearly(self):
        output = self.run_installer("--build-only", success=False, SIMPLETERM_BUILD_DIR="build output")
        self.assertIn("Make metacharacters", output)

    def test_custom_tools_flags_and_relative_build_directory(self):
        for command in ("custom-make", "custom-pkg-config", "clang"):
            self.mock_tool(command)
        (self.tools / "cc").unlink()
        self.run_installer("--build-only", MAKE="custom-make", PKG_CONFIG="custom-pkg-config",
                           CC="clang", CFLAGS="-O1 -g", SIMPLETERM_BUILD_DIR="build/custom")
        arguments = self.builds("custom-make")[0]["args"]
        self.assertIn("BUILD_DIR=build/custom", arguments)
        self.assertIn("PKG_CONFIG=custom-pkg-config", arguments)
        self.assertIn("CFLAGS=-O1 -g", arguments)
        self.assertIn("-B", arguments)
        self.assertFalse(self.builds())

    def test_default_flags_do_not_turn_new_library_warnings_into_errors(self):
        self.run_installer("--build-only")
        self.assertIn("CFLAGS=-O2 -Wall -Wextra", self.builds()[0]["args"])

    def test_freebsd_selects_gnu_make(self):
        self.mock_tool("gmake")
        self.run_installer("--build-only", TEST_OS="FreeBSD", TEST_BSD_MAKE="1")
        self.assertEqual(len(self.builds("gmake")), 1)
        self.assertFalse(self.builds())

    def test_pkgconf_fallback(self):
        self.mock_tool("pkgconf")
        (self.tools / "pkg-config").unlink()
        self.run_installer("--build-only")
        self.assertIn("PKG_CONFIG=pkgconf", self.builds()[0]["args"])

    def test_compiler_command_can_include_flags(self):
        self.mock_tool("clang")
        (self.tools / "cc").unlink()
        self.run_installer("--build-only", CC="clang -std=c17")
        self.assertEqual(len(self.builds()), 1)

    def test_freebsd_does_not_accidentally_use_linux_package_managers(self):
        self.mock_tool("apt-get")
        output = self.run_installer("--build-only", success=False, TEST_OS="FreeBSD",
                                   TEST_BSD_MAKE="1", TEST_LIBRARIES="missing")
        self.assertIn("FreeBSD pkg tool", output)
        self.assertFalse(self.commands("apt-get"))

    def test_missing_package_manager_provides_manual_instructions(self):
        output = self.run_installer("--build-only", success=False, TEST_LIBRARIES="missing")
        self.assertIn("rerun with --no-deps", output)
        self.assertFalse(self.builds())

    def test_dependency_install_does_not_elevate_the_build(self):
        self.mock_tool("sudo")
        self.mock_tool("apt-get")
        self.run_installer("--build-only", SIMPLETERM_NONINTERACTIVE="1", TEST_LIBRARIES="missing")
        self.assertEqual(self.commands("apt-get")[-1]["uid"], "0")
        self.assertEqual(self.builds()[0]["uid"], "1000")

    def test_old_libraries_report_required_and_found_versions(self):
        output = self.run_installer("--no-deps", "--build-only", success=False, TEST_LIBRARIES="old")
        self.assertIn("found 0.70", output)
        self.assertIn("VTE 0.76+", output)
        self.assertFalse(self.builds())

    def test_missing_compiler_is_reported_before_make(self):
        (self.tools / "cc").unlink()
        output = self.run_installer("--no-deps", "--build-only", success=False)
        self.assertIn("C compiler is required", output)
        self.assertFalse(self.builds())

    def test_bsd_make_is_not_mistaken_for_gnu_make(self):
        output = self.run_installer("--no-deps", "--build-only", success=False, TEST_BSD_MAKE="1")
        self.assertIn("GNU make is required", output)

    def test_missing_source_stops_before_packages(self):
        self.mock_tool("apt-get")
        (self.source / "simpleterm-settings.c").unlink()
        output = self.run_installer("--build-only", success=False, TEST_LIBRARIES="missing")
        self.assertIn("Missing source file", output)
        self.assertFalse(self.commands("apt-get"))

    def test_package_manager_matrix(self):
        managers = {"apt-get": "libvte-2.91-dev", "dnf": "vte291-devel", "zypper": "vte-devel",
                    "pacman": "vte3", "xbps-install": "vte3-devel", "apk": "vte3-dev", "pkg": "vte3"}
        for manager, package in managers.items():
            with self.subTest(manager=manager):
                (self.state / "dependencies-installed").unlink(missing_ok=True)
                self.mock_tool(manager)
                if manager == "pkg":
                    self.mock_tool("gmake")
                self.run_installer("--build-only", TEST_LIBRARIES="missing", TEST_UID="0",
                                   TEST_OS="FreeBSD" if manager == "pkg" else "Linux",
                                   TEST_BSD_MAKE="1" if manager == "pkg" else "0")
                self.assertIn(package, self.commands(manager)[-1]["args"])
                (self.tools / manager).unlink()
        self.assertEqual(self.commands("apt-get")[-1]["frontend"], "noninteractive")
        self.assertIn("--non-interactive", self.commands("zypper")[-1]["args"])

    def test_missing_fonts_trigger_dependency_install(self):
        self.mock_tool("apt-get")
        self.run_installer("--build-only", TEST_UID="0", TEST_NO_FONTS="1")
        self.assertIn("fonts-dejavu-core", self.commands("apt-get")[-1]["args"])

    def test_package_failure_stops_before_build(self):
        self.mock_tool("apt-get")
        self.run_installer("--build-only", success=False, TEST_UID="0",
                           TEST_LIBRARIES="missing", TEST_PACKAGE_FAIL="1")
        self.assertFalse(self.builds())

    def test_old_repositories_still_fail_after_package_install(self):
        self.mock_tool("apt-get")
        output = self.run_installer("--build-only", success=False, TEST_UID="0",
                                   TEST_LIBRARIES="old", TEST_AFTER_PACKAGES="old")
        self.assertIn("found 0.70", output)
        self.assertFalse(self.builds())

    def test_noninteractive_privilege_methods(self):
        for method in ("sudo", "doas"):
            with self.subTest(method=method):
                self.mock_tool(method)
                self.run_installer(SIMPLETERM_NONINTERACTIVE="1")
                arguments = self.commands(method)[-1]["args"]
                self.assertEqual(arguments[0], "-n")
                self.assertIn("--copy-built", arguments)
                self.assertIn("DESTDIR=", arguments)
                self.assertTrue(all(command["uid"] == "1000" for command in self.builds()))
                (self.tools / method).unlink()

    def test_noninteractive_never_uses_pkexec(self):
        self.mock_tool("pkexec")
        output = self.run_installer(success=False, SIMPLETERM_NONINTERACTIVE="1")
        self.assertIn("Noninteractive installation requires", output)
        self.assertFalse(self.commands("pkexec"))

    def test_desktop_can_use_pkexec_without_a_terminal(self):
        self.mock_tool("pkexec")
        self.run_installer()
        self.assertIn("--copy-built", self.commands("pkexec")[-1]["args"])

    def test_privilege_failure_does_not_report_success(self):
        self.mock_tool("sudo")
        output = self.run_installer(success=False, SIMPLETERM_NONINTERACTIVE="1", TEST_AUTH_FAIL="1")
        self.assertNotIn("Installed Simpleterm", output)
        self.assertFalse((self.state / "root-copy").exists())

    def test_build_failure_does_not_request_copy_privileges(self):
        self.mock_tool("sudo")
        self.run_installer(success=False, TEST_BUILD_FAIL="1", SIMPLETERM_NONINTERACTIVE="1")
        self.assertFalse(self.commands("sudo"))

    def test_validation_failure_preserves_installed_files(self):
        self.run_installer(DESTDIR=str(self.stage))
        installed = self.stage / "usr/local/bin/simpleterm"
        original = installed.read_bytes()
        self.run_installer(success=False, DESTDIR=str(self.stage), TEST_DESKTOP_FAIL="1")
        self.assertEqual(installed.read_bytes(), original)

    def test_failed_atomic_copy_preserves_old_binary_and_cleans_temporary_file(self):
        self.run_installer(DESTDIR=str(self.stage))
        installed = self.stage / "usr/local/bin/simpleterm"
        installed.write_text("previous binary")
        self.run_installer(success=False, DESTDIR=str(self.stage), TEST_COPY_FAIL="1")
        self.assertEqual(installed.read_text(), "previous binary")
        self.assertFalse(list(self.stage.rglob(".simpleterm.*")))

    def test_predictable_old_temporary_symlink_is_not_followed(self):
        target = self.temp / "personal-file"
        target.write_text("preserve")
        bindir = self.stage / "usr/local/bin"
        bindir.mkdir(parents=True)
        (bindir / ".simpleterm.new").symlink_to(target)
        self.run_installer(DESTDIR=str(self.stage))
        self.assertEqual(target.read_text(), "preserve")

    def test_directory_at_binary_destination_is_not_overwritten(self):
        destination = self.stage / "usr/local/bin/simpleterm"
        destination.mkdir(parents=True)
        output = self.run_installer(success=False, DESTDIR=str(self.stage))
        self.assertIn("Refusing to replace a directory", output)
        self.assertEqual(list(destination.iterdir()), [])

    def test_uninstall_does_not_require_compiler_or_libraries(self):
        self.mock_tool("sudo")
        for command in ("cc", "make", "pkg-config"):
            (self.tools / command).unlink()
        self.run_installer("--uninstall", SIMPLETERM_NONINTERACTIVE="1")
        self.assertIn("--uninstall", self.commands("sudo")[-1]["args"])


if __name__ == "__main__":
    unittest.main()
