#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# tools/analyze.sh - run a C++ binary of this project under an analysis tool.
#
#   tools/analyze.sh <mode> [options] [--] <binary> [args...]
#   tools/analyze.sh perf-system <seconds> [options]
#
# Modes
#   memcheck     valgrind memcheck: invalid accesses, uninitialised reads, leaks
#   helgrind     valgrind helgrind: data races, lock-order problems
#   drd          valgrind drd:      data races (second opinion to helgrind)
#   callgrind    valgrind callgrind: exact call counts/costs  -> kcachegrind
#   perf         perf record of the program (and its children) -> hotspot
#   perf-system  perf record of the WHOLE system for <seconds>  -> hotspot
#                (for independently started processes, e.g. producer and
#                consumer processes sharing one ring buffer)
#   c2c          perf c2c: cache lines shared/bouncing between CPU cores
#                (false sharing check for the 64-byte ring-buffer layout)
#   san          run an ASan/TSan build with the project's sanitizer options,
#                one log file per process (asan.<pid> / tsan.<pid>)
#
# Options
#   --no-open    do not start hotspot / kcachegrind afterwards
#   --dwarf      perf: DWARF call graphs instead of frame pointers (use when
#                stacks through system libraries matter; much larger data)
#
# Results go to build/reports/<mode>-<timestamp>/. Every tool writes one file
# per process (%p / <pid>), so runs with several cooperating processes stay
# apart. Details: Docs/Tooling.md.
# ---------------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUPP_DIR="$ROOT/tools/sanitizers"
VG_SUPP="$ROOT/tools/valgrind/hik.supp"

die()  { echo "analyze.sh: $*" >&2; exit 2; }
note() { echo "analyze.sh: $*" >&2; }

usage()
{
    sed -n '3,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

[[ $# -ge 1 ]] || usage 2
MODE="$1"; shift
case "$MODE" in
    memcheck|helgrind|drd|callgrind|perf|perf-system|c2c|san) ;;
    -h|--help) usage 0 ;;
    *) die "unknown mode '$MODE' (see --help)" ;;
esac

SECONDS_ARG=""
if [[ "$MODE" == "perf-system" ]]; then
    [[ $# -ge 1 && "$1" =~ ^[0-9]+$ ]] || die "perf-system needs a duration in seconds"
    SECONDS_ARG="$1"; shift
fi

OPEN=1
CALLGRAPH="fp"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-open) OPEN=0; shift ;;
        --dwarf)   CALLGRAPH="dwarf"; shift ;;
        --)        shift; break ;;
        -*)        die "unknown option '$1'" ;;
        *)         break ;;
    esac
done

BIN=""
if [[ "$MODE" != "perf-system" ]]; then
    [[ $# -ge 1 ]] || die "no binary given"
    BIN="$1"; shift
    [[ -x "$BIN" ]] || die "'$BIN' is not an executable file"
    BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
fi

OUT="$ROOT/build/reports/${MODE}-$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT"

# --- Sanity checks ---------------------------------------------------------

is_sanitized()
{
    ldd "$1" 2>/dev/null | grep -Eq 'lib(a|t|ub|l)san\.so'
}

check_page_size()
{
    # valgrind on arm64 does not cope with 16 KB page kernels (Raspberry Pi 5
    # default kernel_2712.img); the 4 KB kernel8.img works.
    local ps
    ps="$(getconf PAGESIZE)"
    if [[ "$ps" != "4096" ]]; then
        die "page size is $ps bytes; valgrind needs a 4 KB page kernel.
  Raspberry Pi 5: add 'kernel=kernel8.img' to /boot/firmware/config.txt and reboot."
    fi
}

check_paranoid()
{
    # $1 = highest perf_event_paranoid value the mode can live with
    local cur
    cur="$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 2)"
    if (( cur > $1 )) && [[ $EUID -ne 0 ]]; then
        die "kernel.perf_event_paranoid is $cur, '$MODE' needs <= $1.
  Temporarily:  sudo sysctl kernel.perf_event_paranoid=$1
  (resets on reboot; see Docs/Tooling.md)"
    fi
}

warn_not_profile_build()
{
    case "$BIN" in
        "$ROOT"/build/profile/*) ;;
        *) note "note: '$BIN' is not from build/profile - timings of a Debug or"
           note "      sanitizer build are not representative (cmake --preset profile)." ;;
    esac
}

# --- Viewers ---------------------------------------------------------------

have_display() { [[ -n "${DISPLAY:-}" || -n "${WAYLAND_DISPLAY:-}" ]]; }

open_viewer()
{
    # $1 = viewer, $2... = files
    local viewer="$1"; shift
    if (( OPEN )) && have_display && command -v "$viewer" >/dev/null; then
        note "opening $viewer ..."
        nohup "$viewer" "$@" >/dev/null 2>&1 &
    else
        note "view with: $viewer $*"
    fi
}

summarise_valgrind()
{
    local f
    for f in "$OUT"/*.log; do
        [[ -e "$f" ]] || continue
        printf '%s: %s\n' "$(basename "$f")" \
            "$(grep -h 'ERROR SUMMARY' "$f" | tail -1 | sed 's/^==[0-9]*== //')"
    done
}

# --- Modes -----------------------------------------------------------------

STATUS=0
VG_COMMON=(--trace-children=yes --num-callers=30 --error-exitcode=99
           --suppressions="$VG_SUPP")

case "$MODE" in
memcheck|helgrind|drd)
    check_page_size
    is_sanitized "$BIN" && die "valgrind cannot run sanitizer builds - use build/debug"
    extra=()
    [[ "$MODE" == memcheck ]] && extra=(--leak-check=full --show-leak-kinds=definite,indirect
                                        --errors-for-leak-kinds=definite,indirect
                                        --track-origins=yes)
    valgrind --tool="$MODE" "${VG_COMMON[@]}" "${extra[@]}" \
        --log-file="$OUT/$MODE.%p.log" -- "$BIN" "$@" || STATUS=$?
    summarise_valgrind
    ;;

callgrind)
    check_page_size
    is_sanitized "$BIN" && die "valgrind cannot run sanitizer builds - use build/profile"
    warn_not_profile_build
    valgrind --tool=callgrind --trace-children=yes \
        --callgrind-out-file="$OUT/callgrind.out.%p" -- "$BIN" "$@" || STATUS=$?
    shopt -s nullglob
    files=("$OUT"/callgrind.out.*)
    (( ${#files[@]} )) && open_viewer kcachegrind "${files[0]}"
    ;;

perf)
    check_paranoid 2
    warn_not_profile_build
    # perf record follows fork/exec children of the program automatically.
    perf record -g --call-graph "$CALLGRAPH" -o "$OUT/perf.data" -- "$BIN" "$@" || STATUS=$?
    open_viewer hotspot "$OUT/perf.data"
    ;;

perf-system)
    check_paranoid 0
    note "recording all CPUs for ${SECONDS_ARG}s - start/keep your processes running now"
    perf record -a -g --call-graph "$CALLGRAPH" -o "$OUT/perf.data" -- sleep "$SECONDS_ARG" || STATUS=$?
    open_viewer hotspot "$OUT/perf.data"
    ;;

c2c)
    if [[ "$(uname -m)" == "aarch64" ]] && ! ls /sys/bus/event_source/devices 2>/dev/null | grep -q '^arm_spe'; then
        die "perf c2c needs Arm SPE, which this CPU lacks (e.g. Cortex-A76 / Raspberry Pi 5).
  Alternatives: perf stat -e l1d_cache_refill,l2d_cache_refill,l1d_cache -- <binary>
                valgrind --tool=cachegrind --cache-sim=yes <binary>"
    fi
    check_paranoid 1
    warn_not_profile_build
    perf c2c record -g -o "$OUT/perf.data" -- "$BIN" "$@" || STATUS=$?
    perf c2c report -i "$OUT/perf.data" --stdio > "$OUT/c2c-report.txt" 2>/dev/null || true
    note "report: $OUT/c2c-report.txt  (interactive: perf c2c report -i $OUT/perf.data)"
    ;;

san)
    is_sanitized "$BIN" || die "'$BIN' is not a sanitizer build - use build/asan or build/tsan"
    export ASAN_OPTIONS="detect_leaks=1:fast_unwind_on_malloc=0:detect_stack_use_after_return=1:strict_string_checks=1:check_initialization_order=1:log_path=$OUT/asan"
    export LSAN_OPTIONS="suppressions=$SUPP_DIR/lsan.supp"
    export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1:suppressions=$SUPP_DIR/ubsan.supp:log_path=$OUT/ubsan"
    export TSAN_OPTIONS="suppressions=$SUPP_DIR/tsan.supp:second_deadlock_stack=1:log_path=$OUT/tsan"
    "$BIN" "$@" || STATUS=$?
    shopt -s nullglob
    logs=("$OUT"/asan.* "$OUT"/ubsan.* "$OUT"/tsan.*)
    if (( ${#logs[@]} )); then
        note "sanitizer reports (one file per process):"
        printf '  %s\n' "${logs[@]}" >&2
    else
        note "no sanitizer reports"
    fi
    ;;
esac

note "results: $OUT"
exit "$STATUS"
