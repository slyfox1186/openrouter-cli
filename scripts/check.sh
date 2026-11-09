#!/usr/bin/env bash
# Builds, tests and lints openrouter-cli, then builds the release binary.
#
#   scripts/check.sh          format check, debug build (ASan+UBSan) and tests, clang-tidy,
#                             cppcheck, release build, static-link check
#   scripts/check.sh --gcc    also builds and tests with GCC (portability check)
#
# Every step runs even if an earlier one fails (tests are skipped when their build fails), so
# one run reports everything. Full output is saved to build/logs/check-<UTC time>.log.

set -Euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1

LOG_DIR="$ROOT/build/logs"
mkdir -p "$LOG_DIR"
LOG="$LOG_DIR/check-$(date -u +%Y-%m-%dT%H%M%SZ).log"
ln -sfn "$(basename "$LOG")" "$LOG_DIR/latest.log"
exec > >(tee "$LOG") 2>&1

WITH_GCC=0
if [[ "${1:-}" == "--gcc" ]]; then
    WITH_GCC=1
fi

FAILED=()

begin() {
    printf '\n==== %s ====\n' "$1"
}

# finish NAME STATUS: records the outcome of the step that begin NAME started.
finish() {
    if [[ "$2" == 0 ]]; then
        printf -- '---- %s: ok\n' "$1"
        return 0
    fi
    printf -- '---- %s: FAILED\n' "$1"
    FAILED+=("$1")
    return 1
}

step() {
    local name="$1"
    shift
    begin "$name"
    "$@"
    finish "$name" "$?"
}

find_tool() {
    local tool
    for tool in "$@"; do
        if command -v "$tool" >/dev/null 2>&1; then
            command -v "$tool"
            return 0
        fi
    done
    return 1
}

CLANG_FORMAT="$(find_tool clang-format-23 clang-format || true)"
CLANG_TIDY="$(find_tool clang-tidy-23 clang-tidy || true)"
CPPCHECK="$(find_tool cppcheck || true)"

mapfile -t FIRST_PARTY < <(find src tests -name '*.cpp' -o -name '*.hpp' | sort)
mapfile -t SOURCES < <(find src -name '*.cpp' | sort)

printf 'openrouter-cli check on %s, %s\n' "$(hostname)" "$(date -u +%FT%TZ)"
printf 'compilers: %s | %s\n' "$(clang++ --version | head -n 1)" "$(g++ --version | head -n 1)"

format_check() {
    [[ -n "$CLANG_FORMAT" ]] || { echo "clang-format not found"; return 1; }
    "$CLANG_FORMAT" --dry-run --Werror "${FIRST_PARTY[@]}"
}
begin "format check"
format_check
finish "format check" "$?"

DEBUG_BUILT=0
if step "configure debug" cmake --preset debug --fresh; then
    if step "build debug" cmake --build --preset debug; then
        DEBUG_BUILT=1
    fi
fi
if [[ "$DEBUG_BUILT" == 1 ]]; then
    step "tests (debug, ASan+UBSan)" ctest --preset debug
else
    printf '\n==== tests skipped: debug build failed ====\n'
    FAILED+=("tests (not run)")
fi

tidy_check() {
    [[ -n "$CLANG_TIDY" ]] || { echo "clang-tidy not found"; return 1; }
    [[ -f build/debug/compile_commands.json ]] || { echo "no compile_commands.json"; return 1; }
    "$CLANG_TIDY" -p build/debug --quiet "${SOURCES[@]}"
}
begin "clang-tidy (src)"
tidy_check
finish "clang-tidy (src)" "$?"

cppcheck_check() {
    [[ -n "$CPPCHECK" ]] || { echo "cppcheck not found"; return 1; }
    [[ -f build/debug/compile_commands.json ]] || { echo "no compile_commands.json"; return 1; }
    "$CPPCHECK" --project=build/debug/compile_commands.json --file-filter="$ROOT/src/*" \
        --enable=warning,performance,portability --error-exitcode=1 --quiet \
        -j "$(nproc)"
}
begin "cppcheck (src)"
cppcheck_check
finish "cppcheck (src)" "$?"

RELEASE_BUILT=0
if step "configure release" cmake --preset release --fresh; then
    if step "build release" cmake --build --preset release; then
        RELEASE_BUILT=1
    fi
fi

static_check() {
    local binary="build/release/openrouter-cli"
    echo "binary: $binary ($(stat -c %s "$binary") bytes)"
    echo "shared libraries it needs:"
    ldd "$binary"
    if ldd "$binary" | grep -E -v 'linux-vdso|libc\.so|libm\.so|ld-linux' | grep -q '=>'; then
        echo "unexpected shared library dependency"
        return 1
    fi
    "$binary" --version
}
if [[ "$RELEASE_BUILT" == 1 ]]; then
    begin "static link check"
    static_check
    finish "static link check" "$?"
fi

if [[ "$WITH_GCC" == 1 ]]; then
    if step "configure gcc" cmake --preset gcc --fresh; then
        if step "build gcc" cmake --build --preset gcc; then
            step "tests (gcc)" ctest --preset gcc
        fi
    fi
fi

printf '\n==== summary ====\n'
if [[ ${#FAILED[@]} -eq 0 ]]; then
    echo "all steps passed"
    echo "log: $LOG"
    exit 0
fi
printf 'failed: %s\n' "${FAILED[@]}"
echo "log: $LOG"
exit 1
