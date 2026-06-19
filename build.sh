#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

make -C "$script_dir" all "$@"

case ":$PATH:" in
    *":$HOME/.local/bin:"*) ;;
    *)
        echo
        echo "Warning: ~/.local/bin is not in PATH."
        echo
        echo "If commands like simplewords are not found:"
        echo
        echo "Bash:"
        echo '  echo '\''export PATH="$HOME/.local/bin:$PATH"'\'' >> ~/.bashrc'
        echo '  source ~/.bashrc'
        echo
        echo "Zsh:"
        echo '  echo '\''export PATH="$HOME/.local/bin:$PATH"'\'' >> ~/.zshrc'
        echo '  source ~/.zshrc'
        ;;
esac