#!/usr/bin/env python3
"""Check the Bash prompt integration in isolated homes without a display."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
rc = root / 'assets/simpleterm-bashrc'
bash = shutil.which('bash')
if not bash:
    print('SKIP Bash prompt checks: Bash is not installed')
    raise SystemExit(0)

with tempfile.TemporaryDirectory(prefix='simpleterm-prompt-') as temporary:
    home = Path(temporary) / 'Writer’s home with spaces'
    home.mkdir()
    config = home / 'config with spaces'
    color = config / 'prompt-color/color'
    color.parent.mkdir(parents=True)
    original = r'\[\e]0;\u@\h: \w\a\]\[\033[01;32m\]\u@\h\[\033[00m\]:\[\033[01;34m\]\w\[\033[00m\]\$ '
    bashrc = home / '.bashrc'
    personal = "export PERSONAL_SETTING=kept\nPS1='" + original + "'\n"
    bashrc.write_text(personal)
    env = dict(os.environ, HOME=str(home), XDG_CONFIG_HOME=str(config), TERM='xterm-256color')
    def run(script='printf "%s\\0%s" "$PS1" "$PERSONAL_SETTING"'):
        result = subprocess.run([bash, '--noprofile', '--norc', '-c',
                                 'source "$1"; ' + script, 'test', str(rc)],
                                env=env, check=True, capture_output=True, text=True)
        assert bashrc.read_text() == personal
        return result.stdout.split('\0')
    assert run() == [original, 'kept']
    for code in ('01;32', '01;38;2;14;246;90', '01;38;2;142;255;56', '01;38;5;120', '01;38;2;0;255;9'):
        color.write_text(code + '\n')
        expected = original.replace(r'\[\033[01;32m\]\u@\h', '\\[\\033[' + code + 'm\\]\\u@\\h')
        assert run() == [expected, 'kept'], code
    marker = home / 'must-not-exist'
    for code in ('01;38;2;256;0;0', '01;38;2;-1;0;0', '01;38;2;1;2', '01;31',
                 '01;38;2;1;2;3;4', '$(touch "' + str(marker) + '")', '\x1b[31m'):
        color.write_text(code + '\n')
        assert run() == [original, 'kept'], repr(code)
        assert not marker.exists()
    color.write_text('01;38;2;14;246;90\n')
    # Existing hooks may reconstruct PS1 each time; the handle update runs last.
    for hook in ('PROMPT_COMMAND="PS1=\\\"personal \\\\u@\\\\h> \\\""\n',
                 "PROMPT_COMMAND=('PS1=\"personal \\u@\\h> \"' 'PERSONAL_HOOK=ran')\n"):
        personal = "export PERSONAL_SETTING=kept\nPS1='" + original + "'\n" + hook
        bashrc.write_text(personal)
        script = '''
if [[ $(declare -p PROMPT_COMMAND) == 'declare -a '* ]]; then
    for entry in "${PROMPT_COMMAND[@]}"; do eval "$entry"; done
else
    eval "$PROMPT_COMMAND"
fi
printf '%s' "$PS1"
'''
        assert run(script) == [r'personal \[\033[01;38;2;14;246;90m\]\u@\h\[\033[00m\]> ']
    personal = "export PERSONAL_SETTING=kept\nPS1='" + original + "'\n"
    bashrc.write_text(personal)
    result = run('''
printf '%s\\0' "$PS1"
printf '01;38;2;103;232;249\\n' > "$XDG_CONFIG_HOME/prompt-color/color"
_simpleterm_prompt_color_update
printf '%s\\0' "$PS1"
printf '01;32\\n' > "$XDG_CONFIG_HOME/prompt-color/color"
_simpleterm_prompt_color_update
printf '%s\\0' "$PS1"
false
_simpleterm_prompt_color_update
printf '%s' "$?"
''')
    assert result[0] == original.replace('01;32', '01;38;2;14;246;90')
    assert result[1] == original.replace('01;32', '01;38;2;103;232;249')
    assert result[2:] == [original, '1']
print('OK standard Bash prompts, exact greens, custom RGB, shared picker format, live changes, personal startup/hooks, exit status and invalid input')
