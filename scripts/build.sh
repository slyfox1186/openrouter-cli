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
# and build/logs/latest.log. --uninstall removes those three paths instead and builds nothing.
# Run scripts/build.sh --help for the options.

# Parse the complete run before executing it, so edits cannot change unread commands mid-build.
{
set -Euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1

# = delete("reason") and the C++26 library features the code uses need at least Clang 19.
MIN_CLANG_MAJOR=19

# Descriptions in the help start this many columns after the two-space indent.
HELP_COLUMN=22

# Same rules as openrouter-cli --help: a non-empty NO_COLOR turns color off, CLICOLOR_FORCE
# other than empty or 0 turns it on, otherwise color needs a terminal whose TERM is not dumb.
help_color() {
    if [[ -n "${NO_COLOR:-}" ]]; then
        return 1
    fi
    if [[ -n "${CLICOLOR_FORCE:-}" && "$CLICOLOR_FORCE" != 0 ]]; then
        return 0
    fi
    [[ -t 1 && -n "${TERM:-}" && "$TERM" != dumb ]]
}

# help_row SHORT LONG METAVAR DESCRIPTION [MORE_LINES...]
# Pads on the plain text, then adds the color codes set by usage (bash's dynamic scope).
help_row() {
    local short="$1" long="$2" metavar="$3" plain styled line
    shift 3
    if [[ -n "$short" ]]; then
        plain="$short, $long"
        styled="$literal$short$reset, $literal$long$reset"
    else
        plain="    $long"
        styled="    $literal$long$reset"
    fi
    if [[ -n "$metavar" ]]; then
        plain+=" $metavar"
        styled+=" $placeholder$metavar$reset"
    fi
    printf '  %s%*s%s\n' "$styled" "$((HELP_COLUMN - ${#plain}))" '' "$1"
    shift
    for line in "$@"; do
        printf '  %*s%s\n' "$HELP_COLUMN" '' "$line"
    done
}

usage() {
    local title="" header="" literal="" placeholder="" reset=""
    if help_color; then
        title=$'\e[1m'
        header=$'\e[1;32m'
        literal=$'\e[1;36m'
        placeholder=$'\e[36m'
        reset=$'\e[0m'
    fi
    printf '%s\n' "${title}scripts/build.sh${reset}"
    printf '%s\n\n' "Checks, builds and installs openrouter-cli with the newest Clang on PATH."
    printf '%s\n' "${header}USAGE${reset}"
    printf '  %s %s\n\n' "${literal}scripts/build.sh${reset}" "${placeholder}[OPTIONS]${reset}"
    printf '%s\n' "With no options it runs the format check, a debug build with ASan+UBSan and" \
        "all tests, clang-tidy, cppcheck, the release build and the static-link check," \
        "then installs the binary and the agent skill links."
    printf '\n%s\n' "${header}OPTIONS${reset}"
    help_row "" --force "" "Overwrite an existing, different openrouter-cli in the" \
        "bin dir, and re-point skill links that point elsewhere"
    help_row "" --bin-dir DIR "Where the binary goes (default ~/.local/bin)"
    help_row "" --no-skill "" "Install or remove only the binary, not the skill links"
    help_row "" --no-install "" "Run the checks and build; install nothing"
    help_row "" --uninstall "" "Remove the installed binary and skill links, then exit;" \
        "builds nothing. Combines only with --bin-dir and" "--no-skill"
    help_row "" --quick "" "Skip the format check, debug build, tests, clang-tidy" \
        "and cppcheck; run only the release build, the" "static-link check and the install"
    help_row "" --fresh "" "Reconfigure every build directory from scratch"
    help_row "" --jobs N "Parallel build jobs (default: number of CPUs)"
    help_row "" --list-compilers "" "Show every Clang found and the one used, then exit"
    help_row -h --help "" "Show this help and exit"
    printf '\n%s\n' "${header}LOGS${reset}"
    printf '  %s\n' "Each run: build/logs/build-<UTC time>.log" "Newest run: build/logs/latest.log"
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
UNINSTALL=0
# The last option given that only matters to a build or install, for rejecting it with --uninstall.
BUILD_OPTION=""

while [[ $# -gt 0 ]]; do
    case "$1" in
    --force)
        FORCE=1
        BUILD_OPTION="$1"
        ;;
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
    --no-install)
        INSTALL=0
        BUILD_OPTION="$1"
        ;;
    --uninstall) UNINSTALL=1 ;;
    --quick)
        QUICK=1
        BUILD_OPTION="$1"
        ;;
    --fresh)
        FRESH=1
        BUILD_OPTION="$1"
        ;;
    --jobs)
        if [[ $# -lt 2 ]]; then
            usage_error "--jobs needs a number"
        fi
        JOBS="$2"
        BUILD_OPTION="$1"
        shift
        ;;
    --jobs=*)
        JOBS="${1#--jobs=}"
        BUILD_OPTION="--jobs"
        ;;
    --list-compilers)
        LIST_ONLY=1
        BUILD_OPTION="$1"
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    *) usage_error "unknown option: $1" ;;
    esac
    shift
done

if [[ "$UNINSTALL" == 1 && -n "$BUILD_OPTION" ]]; then
    usage_error "--uninstall cannot be combined with $BUILD_OPTION"
fi
if [[ -z "$JOBS" ]]; then
    JOBS="$(nproc)"
fi
if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    usage_error "--jobs must be a positive whole number"
fi

# Cleared only once the options are valid, so --help and usage errors stay on screen.
clear

# ----- uninstall ---------------------------------------------------------------------------

# visible DIR: true when the nearest existing directory at or above DIR can be searched, so a
# failed existence test below it means the path is missing, not hidden by permissions.
visible() {
    local dir="$1"
    local -A followed=()
    while [[ ! -e "$dir" ]]; do
        # A link here may point beneath an unsearchable directory, so the walk continues at
        # its target. A loop of links counts as not visible; GNU readlink -m can return a
        # path inside the loop instead of failing, hence the record of links followed.
        if [[ -L "$dir" ]]; then
            if [[ -n "${followed[$dir]:-}" ]]; then
                return 1
            fi
            followed[$dir]=1
            dir="$(readlink -m -- "$dir")" || return 1
            continue
        fi
        case "$dir" in
        */*) dir="${dir%/*}" ;;
        *) dir=. ;;
        esac
        if [[ -z "$dir" ]]; then
            dir=/
        fi
    done
    [[ -d "$dir" && -x "$dir" ]]
}

# remove_installed PATH KIND: removes PATH when it is a symbolic link, or also a regular file
# when KIND is binary. A real file or directory at a skill path, or a directory at the binary
# path, was not made by this script and stays. Removing a link leaves what it points to.
remove_installed() {
    local path="$1" kind="$2" target
    if [[ -L "$path" ]]; then
        target="$(readlink "$path")"
        rm -f -- "$path" || return 1
        echo "removed  $path (link to $target)"
        return 0
    fi
    if [[ ! -e "$path" ]]; then
        if visible "${path%/*}"; then
            echo "absent   $path"
            return 0
        fi
        echo "cannot check $path: a directory above it cannot be searched"
        return 1
    fi
    if [[ "$kind" == binary && -f "$path" ]]; then
        rm -f -- "$path" || return 1
        echo "removed  $path"
        return 0
    fi
    if [[ "$kind" == binary ]]; then
        echo "refusing to remove $path: it is not a regular file or link"
    else
        echo "refusing to remove $path: it is a real file or directory, not a link"
    fi
    return 1
}

uninstall() {
    local status=0 other
    remove_installed "$BIN_DIR/openrouter-cli" binary || status=1
    if [[ "$INSTALL_SKILL" == 1 ]]; then
        remove_installed "$HOME/.claude/skills/openrouter-cli" skill || status=1
        remove_installed "$HOME/.agents/skills/openrouter-cli" skill || status=1
    fi
    while IFS= read -r other; do
        echo "note: $other is still on PATH; it was not removed (see --bin-dir)"
    done < <(type -aP openrouter-cli)
    return "$status"
}

if [[ "$UNINSTALL" == 1 ]]; then
    uninstall
    exit
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
    local source failed=0 checked=0 started
    for source in "${SOURCES[@]}"; do
        checked=$((checked + 1))
        started=$SECONDS
        printf '[%s/%s] clang-tidy: %s\n' "$checked" "${#SOURCES[@]}" "$source"
        if "$CLANG_TIDY" -p "$DEBUG_DIR" --quiet "$source"; then
            printf '%s: ok (%ss)\n' "$source" "$((SECONDS - started))"
        else
            printf '%s: FAILED (%ss)\n' "$source" "$((SECONDS - started))"
            failed=1
        fi
    done
    return "$failed"
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
        begin "tests (debug, ASan+UBSan)"
        echo "Integration tests report each completed case and its duration."
        echo "Some cases deliberately wait through timeouts and retries, including a 30-second lookup."
        ctest --test-dir "$DEBUG_DIR" --verbose --no-tests=error
        finish "tests (debug, ASan+UBSan)" "$?"
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
}
