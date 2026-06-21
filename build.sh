#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

make -C "$script_dir" all "$@"

if [ -f "$script_dir/simplenews.c" ]; then
    cc "$script_dir/simplenews.c" -Wall -Wextra -O2 -pthread -lncursesw -lcurl -o "$HOME/.local/bin/simplenews"
fi

mkdir -p "$HOME/.config/simplenews"

if [ ! -f "$HOME/.config/simplenews/urls" ]; then
    cat > "$HOME/.config/simplenews/urls.example" <<'EOF'
# SimpleNews feeds go here:
# One feed per line.
#
# Format:
#   URL
#   URL TAG
#   Title | URL
#
# Examples:
# https://www.newyorker.com/feed/everything
# https://lithub.com/feed/ Literary Hub
# The Paris Review | https://www.theparisreview.org/blog/feed/
EOF
fi

if [ ! -f "$HOME/.config/simplenews/config" ]; then
    cat > "$HOME/.config/simplenews/config.example" <<'EOF'
# SimpleNews config
browser=links
timeout=20
max_articles=200
EOF
fi

echo
echo "SimpleNews:"
echo "  feeds:  ~/.config/simplenews/urls"
echo "  config: ~/.config/simplenews/config"
echo
echo "Examples created:"
echo "  ~/.config/simplenews/urls.example"
echo "  ~/.config/simplenews/config.example"
echo

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