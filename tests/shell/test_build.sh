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
check 'portable option parses' parse_options --portable
check 'portable selected' test "$PORTABLE" = 1
check 'next parse returns to native' parse_options
check 'native is the default' test "$PORTABLE" = 0
check 'portable conflicts with uninstall' reject parse_options --uninstall --portable

# CMake arguments: native or portable code, and ccache as launcher only for unwrapped compilers.
has_arg() {
    local wanted="$1" arg
    for arg in "${COMMON_ARGS[@]}"; do [[ "$arg" == "$wanted" ]] && return 0; done
    return 1
}
tools="$SCRATCH/tools"
mkdir -p "$tools/wrappers" || exit 1
printf '#!/bin/sh\n' > "$tools/ccache" && chmod +x "$tools/ccache"
printf '#!/bin/sh\n' > "$tools/clang-99" && chmod +x "$tools/clang-99"
ln -s ../ccache "$tools/wrappers/clang"
saved_path="$PATH"
LINKER_TYPE=DEFAULT FRESH=0
PORTABLE=0 CC_PATH="$tools/clang-99" CXX_PATH="$tools/wrappers/clang"
PATH="$tools:$saved_path" set_common_args
check 'native code by default' has_arg -DORCLI_NATIVE=ON
check 'unwrapped compiler gets ccache' has_arg "-DCMAKE_C_COMPILER_LAUNCHER=$tools/ccache"
check 'ccache wrapper is not wrapped again' has_arg -DCMAKE_CXX_COMPILER_LAUNCHER=
PORTABLE=1
find_tool() { return 1; }
set_common_args
check 'portable turns native off' has_arg -DORCLI_NATIVE=OFF
check 'no ccache leaves the launcher empty' has_arg -DCMAKE_C_COMPILER_LAUNCHER=
check 'fresh is off unless asked' reject has_arg --fresh
unset -f find_tool has_arg
source "$SOURCE_ROOT/scripts/build.sh"

# CET property check against stubbed readelf output.
readelf_output="" readelf_status=0
readelf() { printf '%s\n' "$readelf_output"; return "$readelf_status"; }
quiet_cet() { cet_check > /dev/null; }
readelf_output='      Properties: x86 feature: IBT, SHSTK, x86 ISA needed: x86-64-v4, x86 feature used: x86'
check 'CET property accepted' quiet_cet
readelf_output='      Properties: x86 feature: IBT, x86 ISA needed: x86-64-baseline'
check 'missing SHSTK rejected' reject quiet_cet
readelf_output='      Properties: x86 ISA needed: x86-64-baseline, x86 feature used: x86, IBT, SHSTK'
check 'feature-used list is not the CET property' reject quiet_cet
readelf_output='      Properties: x86 feature: IBT, SHSTK' readelf_status=1
check 'readelf failure rejected' reject quiet_cet
unset -f readelf quiet_cet

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

# Static analysis reads the debug compile database, so it must not run on one a failed configure
# left behind. cmake, ctest and the checks are stubs; nothing is built.
analyzer_marker="$SCRATCH/analyzers-called"
fail_debug_step=""
cmake() {
    if [[ "$1" == --preset && "$2" == debug && "$fail_debug_step" == configure ]]; then return 1; fi
    if [[ "$1" == --build && "$2" == "$DEBUG_DIR" && "$fail_debug_step" == build ]]; then return 1; fi
    return 0
}
ctest() { return 0; }
format_check() { return 0; }
linkage_check() { return 0; }
tidy_check() { printf 'tidy\n' >> "$analyzer_marker"; }
cppcheck_check() { printf 'cppcheck\n' >> "$analyzer_marker"; }
quiet_checks() { run_checks > /dev/null; }
CHECK=1 QUICK=0 JOBS=1 DEBUG_DIR="$SCRATCH/debug" RELEASE_DIR="$SCRATCH/release-dir"
COMMON_ARGS=()
fail_debug_step=configure
check 'failed debug configure fails the checks' reject quiet_checks
check 'failed debug configure skips static analysis' test ! -e "$analyzer_marker"
fail_debug_step=build
check 'failed debug build fails the checks' reject quiet_checks
check 'failed debug build still runs both analyzers' test "$(wc -l < "$analyzer_marker")" -eq 2
rm -f -- "$analyzer_marker"
fail_debug_step=""
check 'passing stub checks pass' quiet_checks
check 'passing checks run both analyzers' test "$(wc -l < "$analyzer_marker")" -eq 2
unset -f cmake ctest format_check linkage_check tidy_check cppcheck_check quiet_checks

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
