#!/usr/bin/env bash
# Release checks finish and their log is closed successfully before installation starts.

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
    printf '%s\n\n' "Builds, tests and installs openrouter-cli with the newest Clang on PATH."
    printf '%s\n' "${header}USAGE${reset}"
    printf '  %s %s\n\n' "${literal}scripts/build.sh${reset}" "${placeholder}[OPTIONS]${reset}"
    printf '%s\n' "With no options it builds release, runs offline tests and checks linkage," \
        "then installs the binary. --check adds sanitizer and static checks."
    printf '\n%s\n' "${header}OPTIONS${reset}"
    help_row "" --check "" "Add formatting, debug sanitizers and static analysis," \
        "and treat compiler warnings as errors"
    help_row "" --portable "" "Build for any x86-64 CPU instead of this machine's"
    help_row "" --quick "" "Build and check linkage only; no tests or installation"
    help_row "" --force "" "Replace or remove an unrecognized file or symlink"
    help_row "" --bin-dir DIR "Binary directory (default ~/.local/bin)"
    help_row "" --with-skills "" "Also install the optional agent skill links"
    help_row "" --no-skill "" "With --uninstall, leave all skill links in place"
    help_row "" --no-install "" "Run the selected checks and build; install nothing"
    help_row "" --uninstall "" "Remove owned binary and skill links without building"
    help_row "" --fresh "" "Discard configure caches, including old static OpenSSL"
    help_row "" --jobs N "Parallel build jobs (default: number of CPUs)"
    help_row "" --list-compilers "" "Show Clang pairs and the selected one, then exit"
    help_row -h --help "" "Show this help and exit"
    printf '\n%s\n' "${header}LOGS${reset}"
    printf '  %s\n' "Each run: build/logs/build-<UTC time>-<unique>.log" "Newest run: build/logs/latest.log"
}

usage_error() {
    printf 'build.sh: %s (see --help)\n' "$1" >&2
    return 2
}

parse_options() {
    FORCE=0 BIN_DIR="${HOME:-}/.local/bin" INSTALL_SKILL=0 REMOVE_SKILL=1
    INSTALL=1 QUICK=0 CHECK=0 FRESH=0 PORTABLE=0 JOBS="" LIST_ONLY=0 UNINSTALL=0 HELP=0
    local build_option="" no_skill=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
        --force) FORCE=1 ;;
        --bin-dir)
            if [[ $# -lt 2 || -z "$2" || "$2" == --* ]]; then
                usage_error "--bin-dir needs a directory"; return 2
            fi
            BIN_DIR="$2"; shift ;;
        --bin-dir=*)
            BIN_DIR="${1#*=}"
            [[ -n "$BIN_DIR" ]] || { usage_error "--bin-dir needs a directory"; return 2; } ;;
        --with-skills) INSTALL_SKILL=1; build_option="$1" ;;
        --no-skill) REMOVE_SKILL=0; no_skill=1 ;;
        --no-install) INSTALL=0; build_option="$1" ;;
        --uninstall) UNINSTALL=1 ;;
        --quick) QUICK=1; build_option="$1" ;;
        --check) CHECK=1; build_option="$1" ;;
        --fresh) FRESH=1; build_option="$1" ;;
        --portable) PORTABLE=1; build_option="$1" ;;
        --jobs)
            [[ $# -ge 2 ]] || { usage_error "--jobs needs a number"; return 2; }
            JOBS="$2"; build_option="$1"; shift ;;
        --jobs=*) JOBS="${1#*=}"; build_option="--jobs" ;;
        --list-compilers) LIST_ONLY=1; build_option="$1" ;;
        -h|--help) HELP=1 ;;
        *) usage_error "unknown option: $1"; return 2 ;;
        esac
        shift
    done
    if [[ "$UNINSTALL" == 1 && -n "$build_option" ]]; then
        usage_error "--uninstall cannot be combined with $build_option"; return 2
    fi
    if [[ "$QUICK" == 1 && "$CHECK" == 1 ]]; then
        usage_error "--quick cannot be combined with --check"; return 2
    fi
    if [[ "$no_skill" == 1 && "$UNINSTALL" == 0 ]]; then
        usage_error "--no-skill applies only to --uninstall; skills are opt-in"; return 2
    fi
    if [[ -n "$JOBS" && ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
        usage_error "--jobs must be a positive whole number"; return 2
    fi
    if [[ "$QUICK" == 1 ]]; then INSTALL=0; fi
    if [[ "$BIN_DIR" != /* ]]; then BIN_DIR="$PWD/$BIN_DIR"; fi
}

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

binary_digest() {
    local line
    line="$(sha256sum -- "$1")" || return 1
    printf '%s\n' "${line%% *}"
}

owned_binary() {
    local path="$1" receipt="$BIN_DIR/.openrouter-cli.sha256" recorded actual target
    if [[ -L "$path" ]]; then
        target="$(readlink -m -- "$path")" || return 1
        [[ "$target" == "$ROOT"/build/release-clang-*/openrouter-cli ]]
        return
    fi
    [[ -f "$path" ]] || return 1
    if [[ -f "$receipt" && ! -L "$receipt" ]]; then
        IFS= read -r recorded < "$receipt" || return 1
        [[ "$recorded" =~ ^[0-9a-f]{64}$ ]] || return 1
        actual="$(binary_digest "$path")" || return 1
        [[ "$recorded" == "$actual" ]] && return 0
    fi
    [[ -n "${BINARY:-}" && -f "$BINARY" ]] && cmp -s -- "$BINARY" "$path"
}

remove_installed() {
    local path="$1" kind="$2" expected="${3:-}" owned=0
    if [[ ! -e "$path" && ! -L "$path" ]]; then
        visible "${path%/*}" || { echo "cannot inspect $path"; return 1; }
        echo "absent   $path"; return 0
    fi
    if [[ "$kind" == skill ]]; then
        [[ -L "$path" ]] || { echo "refusing to remove real skill file or directory: $path"; return 1; }
        [[ "$(readlink -m -- "$path")" == "$(readlink -m -- "$expected")" ]] && owned=1
    else
        [[ -L "$path" || -f "$path" ]] || { echo "refusing nonregular binary: $path"; return 1; }
        owned_binary "$path" && owned=1
    fi
    if [[ "$owned" == 0 && "$FORCE" != 1 ]]; then
        echo "refusing unrecognized $path; pass --force"; return 1
    fi
    rm -- "$path" || return 1
    echo "removed  $path"
}

uninstall() {
    local status=0 receipt="$BIN_DIR/.openrouter-cli.sha256"
    if remove_installed "$BIN_DIR/openrouter-cli" binary; then
        if [[ -e "$receipt" || -L "$receipt" ]]; then
            if [[ -f "$receipt" && ! -L "$receipt" ]]; then
                rm -- "$receipt" || status=1
            else
                echo "refusing unsafe install receipt: $receipt"; status=1
            fi
        fi
    else
        status=1
    fi
    if [[ "$REMOVE_SKILL" == 1 ]]; then
        remove_installed "$HOME/.claude/skills/openrouter-cli" skill "$HOME/.agents/skills/openrouter-cli" || status=1
        remove_installed "$HOME/.agents/skills/openrouter-cli" skill "$ROOT/skill/openrouter-cli" || status=1
    fi
    return "$status"
}

install_binary() {
    local dest="$BIN_DIR/openrouter-cli" receipt="$BIN_DIR/.openrouter-cli.sha256"
    local temp receipt_temp digest
    mkdir -p -- "$BIN_DIR" || return 1
    if [[ -e "$dest" && ! -L "$dest" && ! -f "$dest" ]]; then
        echo "refusing nonregular binary destination: $dest"; return 1
    fi
    if [[ -e "$receipt" || -L "$receipt" ]]; then
        [[ -f "$receipt" && ! -L "$receipt" ]] || { echo "refusing unsafe install receipt: $receipt"; return 1; }
    fi
    if [[ -e "$dest" || -L "$dest" ]]; then
        if ! owned_binary "$dest" && [[ "$FORCE" != 1 ]]; then
            echo "refusing unrecognized $dest; pass --force"; return 1
        fi
    fi
    digest="$(binary_digest "$BINARY")" || return 1
    temp="$(mktemp "$BIN_DIR/.openrouter-cli.XXXXXX")" || return 1
    receipt_temp="$(mktemp "$BIN_DIR/.openrouter-cli-receipt.XXXXXX")" || { rm -- "$temp"; return 1; }
    if ! cp -- "$BINARY" "$temp" || ! chmod 0755 "$temp" ||
        ! printf '%s\n' "$digest" > "$receipt_temp" ||
        ! mv -fT -- "$temp" "$dest" || ! mv -fT -- "$receipt_temp" "$receipt"; then
        rm -f -- "$temp" "$receipt_temp"
        echo "installing $dest or its ownership receipt failed"; return 1
    fi
    echo "installed $dest"
}

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

# launcher_for COMPILER: prints ccache when it is installed and COMPILER is not already one of
# its PATH wrappers, so a newer unwrapped clang-N is cached too and nothing is cached twice.
launcher_for() {
    local ccache
    ccache="$(find_tool ccache)" || return 0
    [[ "$(readlink -f -- "$1")" == "$(readlink -f -- "$ccache")" ]] && return 0
    printf '%s\n' "$ccache"
}

# Every value is passed on each run, so a setting cached by an earlier run never carries over.
# Warnings fail only the --check builds, so a newer compiler cannot block a normal install.
set_common_args() {
    local native=ON werror=OFF
    if [[ "$PORTABLE" == 1 ]]; then native=OFF; fi
    if [[ "$CHECK" == 1 ]]; then werror=ON; fi
    COMMON_ARGS=("-DCMAKE_C_COMPILER=$CC_PATH" "-DCMAKE_CXX_COMPILER=$CXX_PATH"
        "-DCMAKE_LINKER_TYPE=$LINKER_TYPE" "-DORCLI_NATIVE=$native" "-DORCLI_WERROR=$werror"
        "-DCMAKE_C_COMPILER_LAUNCHER=$(launcher_for "$CC_PATH")"
        "-DCMAKE_CXX_COMPILER_LAUNCHER=$(launcher_for "$CXX_PATH")")
    if [[ "$FRESH" == 1 ]]; then COMMON_ARGS+=(--fresh); fi
}

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
    # uninitMemberVarNoCtor flags aggregates such as Error, whose fields have no default values
    # on purpose: with -Wextra and --check's warnings as errors, clang then rejects any
    # designated initializer that omits one, and clang-tidy's
    # cppcoreguidelines-pro-type-member-init rejects uninitialized use.
    "$CPPCHECK" --project="$DEBUG_DIR/compile_commands.json" --file-filter="$ROOT/src/*" \
        --enable=warning,performance,portability --suppress=uninitMemberVarNoCtor \
        --error-exitcode=1 --quiet -j "$JOBS"
}

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
        ln -sfnT "$target" "$link_path" || return 1
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
    if [[ ! -f "$ROOT/skill/openrouter-cli/SKILL.md" ]]; then
        echo "missing $ROOT/skill/openrouter-cli/SKILL.md"
        return 1
    fi
    link "$ROOT/skill/openrouter-cli" "$HOME/.agents/skills/openrouter-cli" || return 1
    link "../../.agents/skills/openrouter-cli" "$HOME/.claude/skills/openrouter-cli"
}

begin() { printf '\n==== %s ====\n' "$1"; }
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
    local name="$1" status
    shift
    begin "$name"
    "$@"; status=$?
    finish "$name" "$status"
}
skipped() {
    printf '\n==== %s: skipped (%s) ====\n' "$1" "$2"
    FAILED+=("$1 (not run)")
}

# Wait for tee explicitly. Process-substitution logging cannot report its exit status.
run_logged() {
    local -a statuses
    "$@" 2>&1 | tee --output-error=exit -a "$LOG"
    statuses=("${PIPESTATUS[@]}")
    if [[ "${statuses[1]}" != 0 ]]; then
        echo "build.sh: logging failed" >&2
        return 1
    fi
    return "${statuses[0]}"
}

# The CET compatibility property (IBT and SHSTK) is kept only when every linked object has it.
# It marks the binary as compatible; the CPU, kernel and loader decide whether CET is enforced.
cet_check() {
    local notes line features
    notes="$(LC_ALL=C readelf -nW -- "$BINARY")" || { echo "readelf -n failed on $BINARY"; return 1; }
    while IFS= read -r line; do
        [[ "$line" == *'x86 feature: '* ]] || continue
        features="${line#*x86 feature: }"
        features="${features%%, x86 *}"
        if [[ ", $features," == *', IBT,'* && ", $features," == *', SHSTK,'* ]]; then
            echo "CET property: $features"
            return 0
        fi
    done <<< "$notes"
    echo "the binary lacks the CET property (IBT and SHSTK): an object was built without"
    echo "-fcf-protection; link with -Wl,-z,cet-report=warning to name it"
    return 1
}

linkage_check() {
    local deps dynamic line entry resolved expected needed ssl=0 crypto=0
    local -a selected
    dynamic="$(readelf -d -- "$BINARY")" || return 1
    if [[ "$dynamic" == *'(RPATH)'* || "$dynamic" == *'(RUNPATH)'* ]]; then
        echo "embedded RPATH/RUNPATH is not permitted"; return 1
    fi
    while IFS= read -r line; do
        [[ "$line" == *'(NEEDED)'* ]] || continue
        needed="${line#*[}"; needed="${needed%%]*}"
        case "$needed" in
        libssl.so.[34]|libcrypto.so.[34]|libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|ld-linux*.so.*) ;;
        *) echo "unexpected direct shared dependency: $needed"; return 1 ;;
        esac
    done <<< "$dynamic"
    # Ignore session loader overrides: the binary must work with the normal system loader.
    deps="$(env -u LD_LIBRARY_PATH -u LD_PRELOAD ldd "$BINARY" 2>&1)" || return 1
    printf '%s\n' "$deps"
    [[ "$deps" != *'not found'* ]] || return 1
    mapfile -t selected < "$RELEASE_DIR/openssl-libraries.txt" || return 1
    [[ ${#selected[@]} == 2 ]] || return 1
    while IFS= read -r line; do
        line="${line#"${line%%[![:space:]]*}"}"
        [[ -n "$line" ]] || continue
        entry="${line%%[[:space:]]*}"
        case "${entry##*/}" in
        libssl.so.[34]) expected="${selected[0]}"; ssl=1 ;;
        libcrypto.so.[34]) expected="${selected[1]}"; crypto=1 ;;
        linux-vdso.so.*|libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|ld-linux*.so.*) continue ;;
        *)
            # System OpenSSL may itself depend on distro-provided compression libraries.
            # Direct dependencies above are constrained; all transitive entries must resolve.
            resolved="${line#*=> }"; resolved="${resolved% (*}"
            [[ "$resolved" == /* && -f "$resolved" ]] || { echo "unresolved $entry"; return 1; }
            continue ;;
        esac
        resolved="${line#*=> }"; resolved="${resolved% (*}"
        [[ "$resolved" == /* && -f "$resolved" ]] || { echo "unresolved $entry"; return 1; }
        [[ "$(readlink -f -- "$resolved")" == "$(readlink -f -- "$expected")" ]] || {
            echo "$entry does not resolve to the selected system library"; return 1;
        }
    done <<< "$deps"
    [[ "$ssl" == 1 && "$crypto" == 1 ]] || { echo "shared OpenSSL 3 or 4 is missing"; return 1; }
    cet_check || return 1
    "$BINARY" --version
}

run_checks() {
    FAILED=()
    local debug_configured=0 debug_built=0 release_built=0
    if [[ "$CHECK" == 1 ]]; then
        step "format check" format_check
        if step "configure debug" cmake --preset debug -B "$DEBUG_DIR" "${COMMON_ARGS[@]}"; then
            debug_configured=1
            if step "build debug" cmake --build "$DEBUG_DIR" -j "$JOBS"; then debug_built=1; fi
        fi
        if [[ "$debug_built" == 1 ]]; then
            step "tests (debug, ASan+UBSan)" ctest --test-dir "$DEBUG_DIR" --output-on-failure --no-tests=error
        else
            skipped "debug tests" "debug build failed"
        fi
        # A failed configure leaves the previous compile_commands.json in place, whose flags and
        # file list may no longer match the project. A failed compile still has a current one.
        if [[ "$debug_configured" == 1 ]]; then
            step "clang-tidy (src)" tidy_check
            step "cppcheck (src)" cppcheck_check
        else
            skipped "clang-tidy (src)" "debug configure failed"
            skipped "cppcheck (src)" "debug configure failed"
        fi
    fi
    if step "configure release" cmake --preset release -B "$RELEASE_DIR" "${COMMON_ARGS[@]}" "-DBUILD_TESTING=$([[ "$QUICK" == 1 ]] && echo OFF || echo ON)"; then
        if step "build release" cmake --build "$RELEASE_DIR" -j "$JOBS"; then release_built=1; fi
    fi
    if [[ "$release_built" == 1 ]]; then
        if [[ "$QUICK" == 0 ]]; then
            step "tests (release)" ctest --test-dir "$RELEASE_DIR" --output-on-failure --no-tests=error
        fi
        step "linkage check" linkage_check
    else
        skipped "release checks" "release build failed"
    fi
    if [[ ${#FAILED[@]} != 0 ]]; then
        printf 'failed: %s\n' "${FAILED[@]}"; return 1
    fi
    echo "all selected build checks passed"
}

run_install() {
    install_binary || return 1
    if [[ "$INSTALL_SKILL" == 1 ]]; then install_skill || return 1; fi
    if [[ ":$PATH:" != *":$BIN_DIR:"* ]]; then echo "note: $BIN_DIR is not on PATH"; fi
}

# Only compiler diagnostics (file:line:column: warning:) count; driver and linker warnings and
# this script's own messages do not.
warning_note() {
    local count
    count="$(sed 's/\x1b\[[0-9;]*[A-Za-z]//g' -- "$LOG" | grep -cE '^[^:]+:[0-9]+:[0-9]+: warning: ')"
    if [[ "$count" =~ ^[0-9]+$ && "$count" -gt 0 ]]; then
        echo "note: $count compiler warnings in this build (see the log);" \
            "--check fails on warnings in this project's own code"
    fi
    return 0
}

complete_run() {
    if ! run_logged run_checks; then
        echo "build.sh: checks or logging failed; nothing installed" >&2; return 1
    fi
    if [[ "$CHECK" == 0 ]]; then run_logged warning_note || return 1; fi
    if [[ "$INSTALL" == 1 ]]; then run_logged run_install || return 1; fi
}

main() {
    set -Euo pipefail
    ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)" || return 1
    HELP_COLUMN=22
    parse_options "$@" || return $?
    if [[ "$HELP" == 1 ]]; then usage; return; fi
    if [[ "$UNINSTALL" == 1 ]]; then uninstall; return; fi
    cd "$ROOT" || return 1
    local candidates selected version cc cxx found=0
    candidates="$(sorted_candidates)" || return 1
    if [[ "$LIST_ONLY" == 1 ]]; then
        while IFS=$'\t' read -r version cc cxx; do
            [[ -n "$version" ]] || continue
            printf '%-10s %s  %s' "$version" "$cc" "$cxx"
            if [[ "$found" == 0 ]]; then printf '   <- used'; fi
            printf '\n'; found=1
        done <<< "$candidates"
        [[ "$found" == 1 ]] || echo "no clang/clang++ pair found on PATH"
        return 0
    fi
    selected="${candidates%%$'\n'*}"
    [[ -n "$selected" ]] || { echo "no clang/clang++ pair found on PATH" >&2; return 1; }
    IFS=$'\t' read -r CC_VERSION CC_PATH CXX_PATH <<< "$selected"
    CLANG_MAJOR="${CC_VERSION%%.*}"
    LLD_PATH="$("$CC_PATH" -print-prog-name=ld.lld 2>/dev/null || true)"
    LINKER_TYPE=DEFAULT
    if [[ "$LLD_PATH" == /* && -x "$LLD_PATH" ]]; then LINKER_TYPE=LLD; fi
    CLANG_FORMAT="$(find_tool "clang-format-$CLANG_MAJOR" clang-format || true)"
    CLANG_TIDY="$(find_tool "clang-tidy-$CLANG_MAJOR" clang-tidy || true)"
    CPPCHECK="$(find_tool cppcheck || true)"
    [[ -n "$JOBS" ]] || JOBS="$(nproc)"
    set_common_args
    DEBUG_DIR="build/debug-clang-$CC_VERSION"
    RELEASE_DIR="build/release-clang-$CC_VERSION"
    BINARY="$RELEASE_DIR/openrouter-cli"
    mapfile -d '' -t FIRST_PARTY < <(find src tests -type f \( -name '*.cpp' -o -name '*.hpp' \) -print0 | sort -z)
    mapfile -d '' -t SOURCES < <(find src -type f -name '*.cpp' -print0 | sort -z)
    local log_dir="$ROOT/build/logs"
    mkdir -p -- "$log_dir" || return 1
    LOG="$(mktemp "$log_dir/build-$(date -u +%Y-%m-%dT%H%M%SZ)-XXXXXX.log")" || return 1
    ln -sfnT -- "${LOG##*/}" "$log_dir/latest.log" || return 1
    printf 'compiler: Clang %s; jobs: %s; log: %s\n' "$CC_VERSION" "$JOBS" "$LOG"
    complete_run || return 1
    echo "completed; log: $LOG"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
