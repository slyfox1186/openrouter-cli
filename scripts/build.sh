#!/usr/bin/env bash
# Checks, builds and installs openrouter-cli in one run.
#
# Uses the newest Clang on PATH. A default run: format check, debug build (ASan+UBSan) and
# tests, clang-tidy, cppcheck, release build, static-link check, then install:
#   ~/.local/bin/openrouter-cli        a copy of the release binary (static except glibc)
#   ~/.agents/skills/openrouter-cli    -> skill/openrouter-cli   (Codex user skills)
#   ~/.claude/skills/openrouter-cli    -> ../../.agents/skills/openrouter-cli (Claude Code)
# Every step runs even if an earlier one fails, so one log shows everything; nothing is
# installed unless every earlier step passed. Output is saved to build/logs/build-<UTC>.log
# and build/logs/latest.log. Run scripts/build.sh --help for the options.

set -Euo pipefail

clear

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1

# = delete("reason") and the C++26 library features the code uses need at least Clang 19.
MIN_CLANG_MAJOR=19

usage() {
    cat <<'EOF'
Usage: scripts/build.sh [options]

Checks, builds and installs openrouter-cli with the newest Clang on PATH. With no options:
format check, debug build with ASan+UBSan and all tests, clang-tidy, cppcheck, release build,
static-link check, then install the binary and the agent skill links.

  --force           overwrite an existing, different openrouter-cli in the bin dir, and
                    re-point skill links that point elsewhere
  --bin-dir DIR     where the binary goes (default ~/.local/bin)
  --no-skill        install only the binary
  --no-install      run the checks and build, install nothing
  --quick           skip the format check, debug build, tests, clang-tidy and cppcheck:
                    release build, static-link check and install only
  --fresh           reconfigure every build directory from scratch
  --jobs N          parallel build jobs (default: number of CPUs)
  --list-compilers  show every Clang found and the one used, then exit
  -h, --help        show this help

Logs: build/logs/build-<UTC time>.log, newest at build/logs/latest.log.
EOF
}

usage_error() {
    echo "build.sh: $1 (see --help)" >&2
    exit 2
}

FORCE=0
BIN_DIR="$HOME/.local/bin"
INSTALL_SKILL=1
INSTALL=1
QUICK=0
FRESH=0
JOBS=""
LIST_ONLY=0

while [[ $# -gt 0 ]]; do
    case "$1" in
    --force) FORCE=1 ;;
    --bin-dir)
        if [[ $# -lt 2 || -z "$2" ]]; then
            usage_error "--bin-dir needs a directory"
        fi
        BIN_DIR="$2"
        shift
        ;;
    --bin-dir=*)
        BIN_DIR="${1#--bin-dir=}"
        if [[ -z "$BIN_DIR" ]]; then
            usage_error "--bin-dir needs a directory"
        fi
        ;;
    --no-skill) INSTALL_SKILL=0 ;;
    --no-install) INSTALL=0 ;;
    --quick) QUICK=1 ;;
    --fresh) FRESH=1 ;;
    --jobs)
        if [[ $# -lt 2 ]]; then
            usage_error "--jobs needs a number"
        fi
        JOBS="$2"
        shift
        ;;
    --jobs=*) JOBS="${1#--jobs=}" ;;
    --list-compilers) LIST_ONLY=1 ;;
    -h | --help)
        usage
        exit 0
        ;;
    *) usage_error "unknown option: $1" ;;
    esac
    shift
done

if [[ -z "$JOBS" ]]; then
    JOBS="$(nproc)"
fi
if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    usage_error "--jobs must be a positive whole number"
fi

# ----- compiler discovery ------------------------------------------------------------------

# clang_version PROGRAM: prints the full version (e.g. 23.1.5), or fails.
clang_version() {
    local version banner
    banner="$("$1" --version 2>/dev/null)" || return 1
    [[ "${banner%%$'\n'*}" == *clang* ]] || return 1
    version="$("$1" -dumpversion 2>/dev/null)" || return 1
    [[ "$version" =~ ^[0-9]+(\.[0-9]+)*$ ]] || return 1
    printf '%s\n' "$version"
}

# clang_candidates: prints "version<TAB>clang<TAB>clang++" for every usable pair named
# clang or clang-N, in PATH order; the first PATH entry wins for each name, which keeps
# ccache wrappers.
clang_candidates() {
    local -A seen=()
    local -a dirs=()
    local dir cc name cxx version cxx_version
    IFS=: read -r -a dirs <<<"$PATH"
    for dir in "${dirs[@]}"; do
        [[ -n "$dir" && -d "$dir" ]] || continue
        for cc in "$dir/clang" "$dir"/clang-[0-9]*; do
            [[ -f "$cc" && -x "$cc" ]] || continue
            name="${cc##*/}"
            [[ "$name" =~ ^clang(-[0-9]+(\.[0-9]+)*)?$ ]] || continue
            [[ -z "${seen[$name]:-}" ]] || continue
            seen[$name]=1
            cxx="$dir/clang++${name#clang}"
            [[ -f "$cxx" && -x "$cxx" ]] || continue
            version="$(clang_version "$cc")" || continue
            cxx_version="$(clang_version "$cxx")" || continue
            [[ "$version" == "$cxx_version" ]] || continue
            printf '%s\t%s\t%s\n' "$version" "$cc" "$cxx"
        done
    done
}

# Highest version first; the stable sort keeps PATH order among equal versions.
sorted_candidates() {
    clang_candidates | sort -t $'\t' -k1,1Vr -s
}

if [[ "$LIST_ONLY" == 1 ]]; then
    FOUND=0
    while IFS=$'\t' read -r version cc cxx; do
        [[ -n "$version" ]] || continue
        if [[ "$FOUND" == 0 ]]; then
            printf '%-10s %s  %s   <- used\n' "$version" "$cc" "$cxx"
        else
            printf '%-10s %s  %s\n' "$version" "$cc" "$cxx"
        fi
        FOUND=1
    done < <(sorted_candidates)
    if [[ "$FOUND" == 0 ]]; then
        echo "no clang/clang++ pair found on PATH"
    fi
    exit 0
fi

SELECTED="$(sorted_candidates | head -n 1)"
if [[ -z "$SELECTED" ]]; then
    echo "build.sh: no clang and clang++ pair found on PATH" >&2
    exit 1
fi
IFS=$'\t' read -r CC_VERSION CC_PATH CXX_PATH <<<"$SELECTED"
CLANG_MAJOR="${CC_VERSION%%.*}"
if ((CLANG_MAJOR < MIN_CLANG_MAJOR)); then
    echo "build.sh: clang $CC_VERSION is too old; openrouter-cli needs clang $MIN_CLANG_MAJOR or newer" >&2
    exit 1
fi

# LLD when this Clang can find ld.lld (faster links with LTO), else the toolchain default.
LLD_PATH="$("$CC_PATH" -print-prog-name=ld.lld 2>/dev/null || true)"
LINKER_TYPE="DEFAULT"
if [[ "$LLD_PATH" == /* && -x "$LLD_PATH" ]]; then
    LINKER_TYPE="LLD"
fi

find_tool() {
    local tool
    for tool in "$@"; do
        if [[ -n "$tool" ]] && command -v "$tool" >/dev/null 2>&1; then
            command -v "$tool"
            return 0
        fi
    done
    return 1
}

CLANG_FORMAT="$(find_tool "clang-format-$CLANG_MAJOR" clang-format || true)"
CLANG_TIDY="$(find_tool "clang-tidy-$CLANG_MAJOR" clang-tidy || true)"
CPPCHECK="$(find_tool cppcheck || true)"

# ----- logging and step bookkeeping --------------------------------------------------------

LOG_DIR="$ROOT/build/logs"
mkdir -p "$LOG_DIR"
LOG="$LOG_DIR/build-$(date -u +%Y-%m-%dT%H%M%SZ).log"
ln -sfn "$(basename "$LOG")" "$LOG_DIR/latest.log"
exec > >(tee "$LOG") 2>&1

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

skipped() {
    printf '\n==== %s: skipped (%s) ====\n' "$1" "$2"
    FAILED+=("$1 (not run)")
}

# The preset supplies build type and options; the compiler and build directory come from here
# (CMake 4.4: -B overrides a preset's binaryDir, -D its variables). One directory per Clang
# version avoids CMake's "compiler changed" error after an upgrade.
COMMON_ARGS=("-DCMAKE_C_COMPILER=$CC_PATH" "-DCMAKE_CXX_COMPILER=$CXX_PATH"
    "-DCMAKE_LINKER_TYPE=$LINKER_TYPE")
if [[ "$FRESH" == 1 ]]; then
    COMMON_ARGS+=(--fresh)
fi
DEBUG_DIR="build/debug-clang-$CC_VERSION"
RELEASE_DIR="build/release-clang-$CC_VERSION"

printf 'openrouter-cli build on %s, %s\n' "$(hostname)" "$(date -u +%FT%TZ)"
printf 'compiler: clang %s (%s, %s), linker %s\n' "$CC_VERSION" "$CC_PATH" "$CXX_PATH" \
    "$LINKER_TYPE"
printf 'jobs: %s\n' "$JOBS"

mapfile -t FIRST_PARTY < <(find src tests -name '*.cpp' -o -name '*.hpp' | sort)
mapfile -t SOURCES < <(find src -name '*.cpp' | sort)

# ----- checks ------------------------------------------------------------------------------

format_check() {
    if [[ -z "$CLANG_FORMAT" ]]; then
        echo "clang-format not found"
        return 1
    fi
    echo "using $CLANG_FORMAT"
    "$CLANG_FORMAT" --dry-run --Werror "${FIRST_PARTY[@]}"
}

tidy_check() {
    if [[ -z "$CLANG_TIDY" ]]; then
        echo "clang-tidy not found"
        return 1
    fi
    if [[ ! -f "$DEBUG_DIR/compile_commands.json" ]]; then
        echo "no compile_commands.json in $DEBUG_DIR"
        return 1
    fi
    echo "using $CLANG_TIDY with $DEBUG_DIR/compile_commands.json"
    "$CLANG_TIDY" -p "$DEBUG_DIR" --quiet "${SOURCES[@]}"
}

cppcheck_check() {
    if [[ -z "$CPPCHECK" ]]; then
        echo "cppcheck not found"
        return 1
    fi
    if [[ ! -f "$DEBUG_DIR/compile_commands.json" ]]; then
        echo "no compile_commands.json in $DEBUG_DIR"
        return 1
    fi
    "$CPPCHECK" --project="$DEBUG_DIR/compile_commands.json" --file-filter="$ROOT/src/*" \
        --enable=warning,performance,portability --error-exitcode=1 --quiet -j "$JOBS"
}

if [[ "$QUICK" == 0 ]]; then
    begin "format check"
    format_check
    finish "format check" "$?"

    DEBUG_BUILT=0
    if step "configure debug" cmake --preset debug -B "$DEBUG_DIR" "${COMMON_ARGS[@]}"; then
        if step "build debug" cmake --build "$DEBUG_DIR" -j "$JOBS"; then
            DEBUG_BUILT=1
        fi
    fi
    if [[ "$DEBUG_BUILT" == 1 ]]; then
        step "tests (debug, ASan+UBSan)" ctest --test-dir "$DEBUG_DIR" --output-on-failure
    else
        skipped "tests" "the debug build failed"
    fi

    # The compile database comes from configuring the debug tree, so these run even when the
    # debug build itself failed.
    begin "clang-tidy (src)"
    tidy_check
    finish "clang-tidy (src)" "$?"
    begin "cppcheck (src)"
    cppcheck_check
    finish "cppcheck (src)" "$?"
fi

# ----- release build and static-link check -------------------------------------------------

RELEASE_BUILT=0
if step "configure release" cmake --preset release -B "$RELEASE_DIR" "${COMMON_ARGS[@]}"; then
    if step "build release" cmake --build "$RELEASE_DIR" -j "$JOBS"; then
        RELEASE_BUILT=1
    fi
fi
BINARY="$RELEASE_DIR/openrouter-cli"

# Only glibc may stay shared; libstdc++, libgcc, OpenSSL, curl and the rest are linked in.
static_check() {
    local deps line entry bad=0
    echo "binary: $BINARY ($(stat -c %s "$BINARY") bytes)"
    if ! deps="$(ldd "$BINARY" 2>&1)"; then
        echo "ldd could not inspect the binary:"
        echo "$deps"
        return 1
    fi
    echo "shared libraries it needs:"
    echo "$deps"
    while IFS= read -r line; do
        line="${line#"${line%%[![:space:]]*}"}"
        [[ -n "$line" ]] || continue
        entry="${line%%[[:space:]]*}"
        case "${entry##*/}" in
        linux-vdso.so.* | libc.so.* | libm.so.* | ld-linux*.so.*) ;;
        *)
            echo "unexpected shared library: $entry"
            bad=1
            ;;
        esac
    done <<<"$deps"
    if [[ "$bad" != 0 ]]; then
        return 1
    fi
    "$BINARY" --version
}

if [[ "$RELEASE_BUILT" == 1 ]]; then
    begin "static link check"
    static_check
    finish "static link check" "$?"
else
    skipped "static link check" "the release build failed"
fi

# ----- install -----------------------------------------------------------------------------

install_binary() {
    local dest="$BIN_DIR/openrouter-cli"
    local temp target
    if ! mkdir -p "$BIN_DIR"; then
        echo "cannot create $BIN_DIR"
        return 1
    fi
    if [[ -d "$dest" && ! -L "$dest" ]]; then
        echo "refusing to replace $dest: it is a directory"
        return 1
    fi
    if [[ -L "$dest" ]]; then
        target="$(readlink -f "$dest" || true)"
        # The previous install.sh linked here into this project's build tree.
        if [[ "$target" != "$ROOT"/* && "$FORCE" != 1 ]]; then
            echo "refusing to replace $dest: it links to $(readlink "$dest"); pass --force"
            return 1
        fi
    elif [[ -e "$dest" ]]; then
        if cmp -s "$BINARY" "$dest"; then
            echo "up to date: $dest"
            return 0
        fi
        if [[ "$FORCE" != 1 ]]; then
            echo "refusing to overwrite $dest: it is a different file; pass --force"
            return 1
        fi
    fi
    if ! temp="$(mktemp "$BIN_DIR/.openrouter-cli.XXXXXX")"; then
        echo "cannot create a temporary file in $BIN_DIR"
        return 1
    fi
    if ! cp "$BINARY" "$temp" || ! chmod 0755 "$temp" || ! mv -fT "$temp" "$dest"; then
        rm -f "$temp"
        echo "copying to $dest failed"
        return 1
    fi
    echo "installed $dest"
}

# link TARGET LINK: creates or refreshes LINK -> TARGET; never replaces a real file or
# directory, and re-points a link that points elsewhere only with --force.
link() {
    local target="$1"
    local link_path="$2"
    if [[ -L "$link_path" ]]; then
        if [[ "$(readlink "$link_path")" == "$target" ]]; then
            echo "ok       $link_path -> $target"
            return 0
        fi
        if [[ "$FORCE" != 1 ]]; then
            echo "refusing to change $link_path: it points to $(readlink "$link_path"); pass --force"
            return 1
        fi
        ln -sfn "$target" "$link_path" || return 1
        echo "updated  $link_path -> $target"
        return 0
    fi
    if [[ -e "$link_path" ]]; then
        echo "refusing to replace $link_path: it is a real file or directory"
        return 1
    fi
    mkdir -p "$(dirname "$link_path")" || return 1
    ln -s "$target" "$link_path" || return 1
    echo "created  $link_path -> $target"
}

install_skill() {
    local status=0
    if [[ ! -f "$ROOT/skill/openrouter-cli/SKILL.md" ]]; then
        echo "missing $ROOT/skill/openrouter-cli/SKILL.md"
        return 1
    fi
    link "$ROOT/skill/openrouter-cli" "$HOME/.agents/skills/openrouter-cli" || status=1
    link "../../.agents/skills/openrouter-cli" "$HOME/.claude/skills/openrouter-cli" || status=1
    return "$status"
}

if [[ "$INSTALL" == 1 ]]; then
    if [[ ${#FAILED[@]} -eq 0 ]]; then
        begin "install binary"
        install_binary
        # The skill describes this build, so it is linked only once this binary is in place.
        if finish "install binary" "$?" && [[ "$INSTALL_SKILL" == 1 ]]; then
            begin "install skill links"
            install_skill
            finish "install skill links" "$?"
        fi
        if [[ ":$PATH:" != *":$BIN_DIR:"* ]]; then
            echo "note: $BIN_DIR is not on PATH in this shell"
        fi
    else
        printf '\n==== install skipped: earlier steps failed; nothing was changed ====\n'
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
