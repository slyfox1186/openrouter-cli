#!/usr/bin/env bash
# Makes the verified release build available to every Claude Code and Codex session:
#   ~/.local/bin/openrouter-cli        -> build/release/openrouter-cli
#   ~/.agents/skills/openrouter-cli    -> skill/openrouter-cli   (Codex user skills)
#   ~/.claude/skills/openrouter-cli    -> ../../.agents/skills/openrouter-cli (Claude Code)
# Run scripts/check.sh first; this script never builds. It refuses to replace a real file or
# directory and only updates links that already point into this project.

set -Euo pipefail

clear

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="$ROOT/build/release/openrouter-cli"
SKILL="$ROOT/skill/openrouter-cli"

if [[ ! -x "$BINARY" ]]; then
    echo "missing $BINARY; run scripts/check.sh first" >&2
    exit 1
fi
if [[ ! -f "$SKILL/SKILL.md" ]]; then
    echo "missing $SKILL/SKILL.md" >&2
    exit 1
fi

# link TARGET LINK: creates or refreshes LINK -> TARGET without touching anything else.
link() {
    local target="$1"
    local link_path="$2"
    if [[ -L "$link_path" ]]; then
        if [[ "$(readlink "$link_path")" == "$target" ]]; then
            echo "ok       $link_path -> $target"
            return 0
        fi
        echo "refusing to change $link_path: it points to $(readlink "$link_path")" >&2
        return 1
    fi
    if [[ -e "$link_path" ]]; then
        echo "refusing to replace $link_path: it is a real file or directory" >&2
        return 1
    fi
    mkdir -p "$(dirname "$link_path")"
    ln -s "$target" "$link_path"
    echo "created  $link_path -> $target"
}

STATUS=0
link "$BINARY" "$HOME/.local/bin/openrouter-cli" || STATUS=1
link "$SKILL" "$HOME/.agents/skills/openrouter-cli" || STATUS=1
link "../../.agents/skills/openrouter-cli" "$HOME/.claude/skills/openrouter-cli" || STATUS=1

if ! command -v openrouter-cli >/dev/null 2>&1; then
    echo "note: ~/.local/bin is not on PATH in this shell" >&2
fi
exit "$STATUS"
