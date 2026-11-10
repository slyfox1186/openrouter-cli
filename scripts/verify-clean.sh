#!/usr/bin/env bash
# Explicit acceptance run: builds/tests a source-only copy and installs only in scratch paths.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/openrouter clean's acceptance.XXXXXX")"
SOURCE="$SCRATCH/source tree's copy"
VERIFY_HOME="$SCRATCH/home"
LOG_DIR="$ROOT/build/logs"
SUCCESS=0
cleanup() {
    if [[ "$SUCCESS" == 1 ]]; then
        rm -rf -- "$SCRATCH"
    else
        printf 'Acceptance failed; scratch files and logs retained at %s\n' "$SCRATCH" >&2
    fi
}
trap cleanup EXIT
mkdir -p -- "$SOURCE" "$VERIFY_HOME" "$LOG_DIR"
cd "$ROOT"
# git supplies the reviewed source boundary, never ignored credentials, logs, caches or state.
# These named new public files also allow verification before the implementation is committed.
MANIFEST="$SCRATCH/source-manifest"
git ls-files -z > "$MANIFEST"
printf '%s\0' LICENSE THIRD_PARTY_NOTICES.md scripts/verify-clean.sh tests/shell/test_build.sh \
    tests/check_download_headers.cmake >> "$MANIFEST"
sort -zu -o "$MANIFEST" "$MANIFEST"
while IFS= read -r -d '' file; do
    case "$file" in
    .env.example) ;;
    .env|.env.*|.git/*|build/*|state/*|.claude/*|.codex/*|CLAUDE.md|AGENTS.md|JEFF_START_HERE.md)
        printf 'Refusing private/build path in source manifest: %s\n' "$file" >&2; exit 1 ;;
    esac
    [[ -f "$file" && ! -L "$file" ]] || { printf 'Unsupported source entry: %s\n' "$file" >&2; exit 1; }
    mkdir -p -- "$SOURCE/$(dirname "$file")"
    cp -p -- "$file" "$SOURCE/$file"
done < "$MANIFEST"
cd "$SOURCE"
run_build() {
    env -u OPENROUTER_CLI_HOME -u OPENROUTER_API_KEY -u OPENROUTER_MANAGEMENT_KEY \
        HOME="$VERIFY_HOME" XDG_CONFIG_HOME="$VERIFY_HOME/.config" \
        XDG_STATE_HOME="$VERIFY_HOME/.local/state" bash scripts/build.sh "$@"
}
env -u OPENROUTER_CLI_HOME -u OPENROUTER_API_KEY -u OPENROUTER_MANAGEMENT_KEY \
    HOME="$VERIFY_HOME" XDG_CONFIG_HOME="$VERIFY_HOME/.config" \
    XDG_STATE_HOME="$VERIFY_HOME/.local/state" \
    cmake "-DORCLI_SOURCE_DIR=$SOURCE" -P tests/check_download_headers.cmake
run_build --jobs "${1:-2}" --with-skills --bin-dir "$SCRATCH/bin"
[[ -f "$SCRATCH/bin/openrouter-cli" && -L "$VERIFY_HOME/.agents/skills/openrouter-cli" && -L "$VERIFY_HOME/.claude/skills/openrouter-cli" ]]
run_build --check --no-install --jobs "${1:-2}"
run_build --uninstall --bin-dir "$SCRATCH/bin"
[[ ! -e "$SCRATCH/bin/openrouter-cli" && ! -L "$VERIFY_HOME/.agents/skills/openrouter-cli" && ! -L "$VERIFY_HOME/.claude/skills/openrouter-cli" ]]
# Preserve build evidence outside the disposable copy before cleaning it.
for log in build/logs/build-*.log; do
    cp -- "$log" "$LOG_DIR/clean-${log##*/}"
done
SUCCESS=1
printf 'Clean acceptance passed; logs retained in %s\n' "$LOG_DIR"
