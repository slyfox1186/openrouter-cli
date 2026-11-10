#!/usr/bin/env bash
# Offline installer regressions; all mutations remain inside the disposable directory.
set -uo pipefail
if [[ "${1:-}" != --isolated ]]; then
    TEST_SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/orcli install's tests.XXXXXX")" || exit 1
    trap 'rm -rf -- "$TEST_SCRATCH"' EXIT
    env HOME="$TEST_SCRATCH/home" bash "$0" --isolated "$TEST_SCRATCH"
    exit "$?"
fi
SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)" || exit 1
source "$SOURCE_ROOT/scripts/build.sh"
SCRATCH="$2"
ROOT="$SCRATCH/project"
mkdir -p "$ROOT/skill/openrouter-cli" "$HOME" || exit 1
printf 'skill\n' > "$ROOT/skill/openrouter-cli/SKILL.md"
BINARY="$SCRATCH/release"
printf 'release one\n' > "$BINARY"
failures=0

check() {
    local label="$1"
    shift
    if "$@"; then
        printf 'PASS %s\n' "$label"
    else
        printf 'FAIL %s\n' "$label" >&2
        failures=$((failures + 1))
    fi
}
reject() { if "$@"; then return 1; else return 0; fi; }

check 'reject missing bin-dir' reject parse_options --bin-dir
check 'reject option as bin-dir' reject parse_options --bin-dir --force
check 'reject empty bin-dir' reject parse_options --bin-dir=
check 'quick and check conflict' reject parse_options --quick --check
check 'no-skill only applies to uninstall' reject parse_options --no-skill
check 'default options parse' parse_options
check 'skills opt in' test "$INSTALL_SKILL" = 0
check 'relative binary directory parses' parse_options --bin-dir caller-bin
check 'relative binary directory uses caller cwd' test "$BIN_DIR" = "$PWD/caller-bin"
check 'quick parses' parse_options --quick
check 'quick never installs' test "$INSTALL" = 0
check 'uninstall accepts force' parse_options --uninstall --force --no-skill
check 'uninstall binary only' test "$REMOVE_SKILL" = 0
check 'skills option parses' parse_options --with-skills
check 'skills enabled explicitly' test "$INSTALL_SKILL" = 1

parse_options --bin-dir "$SCRATCH/bin"
check 'first binary install' install_binary
check 'owned binary recognized' owned_binary "$BIN_DIR/openrouter-cli"
printf 'release two\n' > "$BINARY"
check 'owned binary upgrade' install_binary
check 'new binary is installed' cmp -s -- "$BINARY" "$BIN_DIR/openrouter-cli"
printf 'unrelated\n' > "$BIN_DIR/openrouter-cli"
check 'different binary protected' reject install_binary
check 'unrecognized uninstall protected' reject uninstall
FORCE=1
check 'force permits regular binary replacement' install_binary
check 'known binary uninstall' uninstall
check 'binary was removed' test ! -e "$BIN_DIR/openrouter-cli"
check 'receipt was removed' test ! -e "$BIN_DIR/.openrouter-cli.sha256"

mkfifo "$BIN_DIR/openrouter-cli"
check 'FIFO refused even with force' reject install_binary
check 'FIFO uninstall refused even with force' reject uninstall
rm -- "$BIN_DIR/openrouter-cli"
mkdir "$BIN_DIR/openrouter-cli"
check 'directory refused even with force' reject install_binary
check 'directory uninstall refused even with force' reject uninstall
rmdir "$BIN_DIR/openrouter-cli"
ln -s /dev/null "$BIN_DIR/openrouter-cli"
FORCE=0
check 'unrelated link protected' reject install_binary
check 'unrelated link uninstall protected' reject uninstall
rm -- "$BIN_DIR/openrouter-cli"
ln -s /dev/null "$BIN_DIR/.openrouter-cli.sha256"
FORCE=1
check 'unsafe receipt refused' reject install_binary
rm -- "$BIN_DIR/.openrouter-cli.sha256"

FORCE=0
check 'optional skills install' install_skill
check 'owned skills uninstall' uninstall
mkdir -p "$HOME/.agents/skills/openrouter-cli"
printf 'real skill\n' > "$HOME/.agents/skills/openrouter-cli/SKILL.md"
FORCE=1
check 'real skill directory protected on install' reject install_skill
check 'failed primary skill link stops secondary link' test ! -L "$HOME/.claude/skills/openrouter-cli"
check 'real skill directory protected on uninstall' reject uninstall
check 'real skill contents retained' test -f "$HOME/.agents/skills/openrouter-cli/SKILL.md"

LOG=/dev/full
check 'logging failure reported' reject run_logged printf 'cannot log this\n'
LOG="$SCRATCH/check.log"
check 'failed command remains failed through logging' reject run_logged false
check 'successful command and log succeed' run_logged printf 'saved\n'

# Exercise the production check-to-install boundary without invoking builds.
install_marker="$SCRATCH/install-called"
run_install() { printf 'installed\n' > "$install_marker"; }
run_checks() { printf 'checks passed\n'; }
INSTALL=1
LOG=/dev/full
check 'failed check logging blocks installation' reject complete_run
check 'install was not called' test ! -e "$install_marker"
LOG="$SCRATCH/check.log"
run_checks() { printf 'checks failed\n'; return 1; }
check 'failed checks block installation' reject complete_run
check 'install still was not called' test ! -e "$install_marker"

run_checks() { printf 'checks passed\n'; }
check 'passing checks reach installation' complete_run
check 'installation observation works' test -f "$install_marker"

exit "$((failures > 0))"
