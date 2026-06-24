#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

make -C "$script_dir" all "$@"

mkdir -p "$HOME/.config/simplenews"

if [ ! -f "$HOME/.config/simplenews/urls.example" ]; then
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

config_example="$HOME/.config/simplenews/config.example"
if [ ! -f "$config_example" ] || ! grep -q '^feed_timeout=' "$config_example"; then
    cat > "$config_example" <<'EOF'
# SimpleNews config example
# browser: %u is replaced with the article URL.
# timeout: seconds for one network attempt.
# feed_timeout: total seconds before one stuck feed is abandoned.
browser=links %u
timeout=8
feed_timeout=18
max_articles=200
EOF
fi

echo
echo "SimpleNews:"
echo "  feeds:  ~/.config/simplenews/urls"
echo "  config: ~/.config/simplenews/config"
echo
echo "Examples installed/updated:"
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
