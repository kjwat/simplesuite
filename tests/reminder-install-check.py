#!/usr/bin/env python3
"""Exercise installed reminder commands with private HOME and mocked activation."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import uuid


def run(argv, env):
    result = subprocess.run(argv, env=env, text=True, capture_output=True, timeout=20)
    assert result.returncode == 0, (argv, result.returncode, result.stdout, result.stderr)
    return result


def cron_command(line):
    command = line.split(None, 5)[5]
    # cron consumes a backslash before %, before passing the command to sh.
    result = []
    i = 0
    while i < len(command):
        if command[i] == "\\" and i + 1 < len(command):
            if command[i + 1] != "%":
                result.append("\\")
            result.append(command[i + 1])
            i += 2
        else:
            assert command[i] != "%", "Unescaped cron input delimiter"
            result.append(command[i])
            i += 1
    return "".join(result)


def execute_systemd_command(command, root, env):
    # Load the unit text so the real manager applies both % and $ expansion.
    # Transient ExecStart properties bypass the unit file's % processing.
    name = 'suite-reminder-check-' + uuid.uuid4().hex + '.service'
    unit = root / name
    environment = root / (name + '.env')
    values = []
    for key in ('HOME', 'XDG_CONFIG_HOME', 'XDG_STATE_HOME',
                'XDG_DATA_HOME', 'XDG_CACHE_HOME', 'PATH'):
        value = env[key].replace('\\', '\\\\').replace('"', '\\"')
        values.append(f'{key}="{value}"\n')
    environment.write_text(''.join(values))
    unit.write_text('[Service]\nType=oneshot\nTimeoutStartSec=10s\n'
                    f'EnvironmentFile={environment}\nExecStart={command}\n')
    systemctl = shutil.which('systemctl')
    try:
        run([systemctl, '--user', '--runtime', 'link', str(unit)], os.environ)
        run([systemctl, '--user', 'start', name], os.environ)
        status = run([systemctl, '--user', 'show', name,
                      '-p', 'ExecMainStatus', '--value'], os.environ)
        assert status.stdout.strip() == '0'
    finally:
        run([systemctl, '--user', 'stop', name], os.environ)
        run([systemctl, '--user', '--runtime', 'disable', name], os.environ)


def check_binary(source, root, prefix_name, systemd_exec):
    program = source.name
    alias = "cal" if program == "simplecal" else "clock"
    prefix = root / prefix_name
    prefix.mkdir()
    binary = prefix / program
    shutil.copy2(source, binary)
    (prefix / alias).symlink_to(program)
    test_home = root / (prefix_name + "-home")
    test_home.mkdir()
    mocks = root / (prefix_name + "-mocks")
    mocks.mkdir()
    (mocks / "systemctl").write_text(
        '#!/bin/sh\n[ "${REMINDER_TEST_BACKEND}" = systemd ]\n')
    (mocks / "crontab").write_text(
        '#!/bin/sh\nif [ "$1" = -l ]; then cat "$REMINDER_TEST_CRONTAB"; '
        'else cp "$1" "$REMINDER_TEST_CRONTAB"; fi\n')
    for path in mocks.iterdir():
        path.chmod(0o755)
    env = dict(os.environ, HOME=str(test_home), XDG_CONFIG_HOME=str(test_home / '.config'),
               XDG_STATE_HOME=str(test_home / '.local/state'),
               XDG_DATA_HOME=str(test_home / '.local/share'),
               XDG_CACHE_HOME=str(test_home / '.cache'),
               PATH=str(mocks) + ':/usr/bin:/bin', REMINDER_TEST_BACKEND='systemd',
               REMINDER_TEST_CRONTAB=str(root / (prefix_name + '.crontab')))
    run([str(prefix / alias), '--install-reminders'], env)
    service = test_home / f'.config/systemd/user/{program}-reminders.service'
    text = service.read_text()
    assert '.local/bin/' not in text
    assert '%%n' in text if '%n' in str(prefix) else str(binary) in text
    assert 'Environment=XDG_RUNTIME_DIR=%t\n' in text
    if shutil.which('systemd-analyze'):
        # This parses and checks the executable without starting a service.
        run(['systemd-analyze', '--user', 'verify', '--man=no', str(service)], env)
    if systemd_exec:
        # Exercise the parsed command in a disposable oneshot, with empty data.
        # Never activate the generated reminder service or timer for this test.
        command = next(line.split('=', 1)[1] for line in text.splitlines()
                       if line.startswith('ExecStart='))
        command = command.rsplit(' ', 1)[0] + ' --check-reminders'
        execute_systemd_command(command, root, env)

    env['REMINDER_TEST_BACKEND'] = 'cron'
    crontab = Path(env['REMINDER_TEST_CRONTAB'])
    personal = '0 0 * * * echo personal-job\n'
    crontab.write_text(personal + f'* * * * * ~/.local/bin/{program} --check-reminders\n')
    run([str(prefix / alias), '--install-reminders'], env)
    first = crontab.read_text()
    run([str(prefix / alias), '--install-reminders'], env)
    assert crontab.read_text() == first, 'Repeated install duplicated the cron job'
    assert first.startswith(personal)
    assert len(first.splitlines()) == 2
    assert '.local/bin/' not in first
    run(['/bin/sh', '-c', cron_command(first.splitlines()[1])], env)
    assert not (test_home / '.local/bin').exists()

    # The uninstaller must remove both current quoted and old reminder commands.
    fragment = Path(__file__).resolve().parents[1] / 'uninstall.sh'
    code = fragment.read_text().split('cleanup_cron_hooks() {', 1)[1].split('\n}\n', 1)[0]
    crontab.write_text(first + f'* * * * * ~/.local/bin/{program} --check-reminders\n')
    run(['/bin/sh', '-c', 'dry_run=0\ncleanup_cron_hooks() {' + code + '\n}\ncleanup_cron_hooks'], env)
    assert crontab.read_text() == personal
    print(f'PASS {program}: {prefix_name}; systemd parsing, cron execution, reinstall, uninstall')


def main():
    if not sys.platform.startswith(('linux', 'freebsd')):
        print('SKIP systemd/cron reminder install checks on this platform')
        return
    arguments = sys.argv[1:]
    systemd_exec = '--systemd-exec' in arguments
    if systemd_exec:
        arguments.remove('--systemd-exec')
    with tempfile.TemporaryDirectory(prefix='suite-reminder-install-') as directory:
        for argument in arguments:
            source = Path(argument).resolve()
            root = Path(directory) / source.name
            root.mkdir()
            check_binary(source, root, 'system-bin', systemd_exec)
            check_binary(source, root, 'custom space %n $HOME "quote" \\path', systemd_exec)
            check_binary(source, root, "equals=value `false` ${HOME} \\%n 'quote'", systemd_exec)


if __name__ == '__main__':
    main()
