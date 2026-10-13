#!/usr/bin/env bash
# ==============================================================================
# bench.sh - Automated Performance & Correctness Benchmark for MountX
#
# Compares MountX Native C Substrate (./mountx-native) against the
# Python Host Reference (python3 host/main.py).
#
# Usage:
#   ./bench.sh              Run standard benchmark suite (fbtest.mx + tour.mx)
#   ./bench.sh <file.mx>    Benchmark a specific MountX program
#   ./bench.sh --repeat 3   Run benchmarks 3 times and report averages
# ==============================================================================

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

# ANSI Colors
BOLD="\033[1m"
GREEN="\033[0;32m"
BLUE="\033[0;34m"
YELLOW="\033[1;33m"
CYAN="\033[0;36m"
RED="\033[0;31m"
DIM="\033[2m"
RESET="\033[0m"

PYTHON_BIN="python3"
HOST_MAIN="host/main.py"
NATIVE_BIN="./mountx-native"

REPEATS=1
CUSTOM_FILE=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --repeat|-r)
            REPEATS="$2"
            shift 2
            ;;
        --help|-h)
            echo -e "${BOLD}Usage:${RESET} $0 [options] [file.mx]"
            echo "Options:"
            echo "  -r, --repeat N    Run each benchmark N times (default: 1)"
            echo "  -h, --help        Show this help message"
            exit 0
            ;;
        *)
            CUSTOM_FILE="$1"
            shift
            ;;
    esac
done

# Ensure binaries exist
if [[ ! -x "$NATIVE_BIN" ]]; then
    echo -e "${YELLOW}[!] Warning: $NATIVE_BIN not found. Compiling now...${RESET}"
    gcc -std=c99 -O2 -Wall -Wextra -DMX_SUBSTRATE -DMX_BOTH -I gate2/g1 \
        gate2/g1/mini.c gate2/g1/interp.emitted.c gate2/g1/stdlib_aot.c tmp/sqlite3_linux.o \
        -lm -lpthread -ldl -o mountx-native
    echo -e "${GREEN}[+] mountx-native compiled successfully.${RESET}"
fi

if [[ ! -f "$HOST_MAIN" ]]; then
    echo -e "${RED}[!] Error: Python host entry $HOST_MAIN not found!${RESET}"
    exit 1
fi

# High-precision timer function (returns seconds with 3 decimals)
run_timed() {
    local cmd=("$@")
    local t_start t_end elapsed_ns
    t_start=$(date +%s%N)
    "${cmd[@]}" > /dev/null 2>&1
    local ret=$?
    t_end=$(date +%s%N)
    elapsed_ns=$((t_end - t_start))
    
    # Format elapsed nanoseconds into seconds with 3 decimals
    local secs ms
    secs=$((elapsed_ns / 1000000000))
    ms=$(( (elapsed_ns % 1000000000) / 1000000 ))
    printf "%d.%03d:%d" "$secs" "$ms" "$ret"
}

# Run benchmark on a given file
benchmark_file() {
    local label="$1"
    local file="$2"

    if [[ ! -f "$file" ]]; then
        echo -e "${RED}File not found: $file${RESET}"
        return 1
    fi

    local py_total_ms=0
    local nat_total_ms=0
    local py_ok=1
    local nat_ok=1

    for ((i=1; i<=REPEATS; i++)); do
        # Run Python
        res_py=$(run_timed "$PYTHON_BIN" "$HOST_MAIN" "$file")
        time_py="${res_py%%:*}"
        code_py="${res_py##*:}"
        if [[ "$code_py" -ne 0 ]]; then py_ok=0; fi

        # Run Native C
        res_nat=$(run_timed "$NATIVE_BIN" "$file")
        time_nat="${res_nat%%:*}"
        code_nat="${res_nat##*:}"
        if [[ "$code_nat" -ne 0 ]]; then nat_ok=0; fi

        # Convert to integer milliseconds for averaging
        p_s="${time_py%.*}"
        p_m="${time_py#*.}"
        py_total_ms=$((py_total_ms + (10#$p_s * 1000) + 10#$p_m))

        n_s="${time_nat%.*}"
        n_m="${time_nat#*.}"
        nat_total_ms=$((nat_total_ms + (10#$n_s * 1000) + 10#$n_m))
    done

    # Calculate averages
    avg_py_ms=$((py_total_ms / REPEATS))
    avg_nat_ms=$((nat_total_ms / REPEATS))

    avg_py=$(printf "%d.%03ds" "$((avg_py_ms / 1000))" "$((avg_py_ms % 1000))")
    avg_nat=$(printf "%d.%03ds" "$((avg_nat_ms / 1000))" "$((avg_nat_ms % 1000))")

    # Ratio
    local ratio_str="--"
    if [[ $avg_py_ms -gt 0 && $avg_nat_ms -gt 0 ]]; then
        if [[ $avg_nat_ms -le $avg_py_ms ]]; then
            ratio=$(awk -v py="$avg_py_ms" -v nat="$avg_nat_ms" 'BEGIN { printf "%.2fx faster", py/nat }')
            ratio_str="${GREEN}${ratio}${RESET}"
        else
            ratio=$(awk -v py="$avg_py_ms" -v nat="$avg_nat_ms" 'BEGIN { printf "%.2fx slower", nat/py }')
            ratio_str="${YELLOW}${ratio}${RESET}"
        fi
    fi

    # Status
    local status="${GREEN}PASS${RESET}"
    if [[ $nat_ok -ne 1 || $py_ok -ne 1 ]]; then
        status="${RED}FAIL${RESET}"
    fi

    printf "  %-24s | %10s | %10s | %-16b | %b\n" "$label" "$avg_py" "$avg_nat" "$ratio_str" "$status"
    return $(( 1 - nat_ok ))
}

echo -e "\n${BOLD}${CYAN}========================================================================${RESET}"
echo -e "${BOLD}${CYAN}              MountX Benchmark & Regression Gate Runner                 ${RESET}"
echo -e "${BOLD}${CYAN}========================================================================${RESET}"
echo -e "${DIM}Platform: $(uname -s) $(uname -m) | GCC $(gcc -dumpversion) | Python $($PYTHON_BIN -V 2>&1 | cut -d' ' -f2)${RESET}"
echo -e "${DIM}Iterations per test: ${REPEATS}${RESET}\n"

printf "  ${BOLD}%-24s | %10s | %10s | %-16s | %s${RESET}\n" "Benchmark" "Python" "Native C" "Comparison" "Status"
echo "  -------------------------+------------+------------+------------------+--------"

failures=0

if [[ -n "$CUSTOM_FILE" ]]; then
    benchmark_file "$(basename "$CUSTOM_FILE")" "$CUSTOM_FILE" || failures=$((failures + 1))
else
    benchmark_file "fbtest.mx (318 checks)" "host/fbtest.mx" || failures=$((failures + 1))
    benchmark_file "tour.mx (Full Tour)" "example/tour.mx" || failures=$((failures + 1))
fi

echo "  -------------------------+------------+------------+------------------+--------"
echo ""

if [[ $failures -eq 0 ]]; then
    echo -e "  ${GREEN}${BOLD}✔ All benchmarks executed successfully with zero failures!${RESET}\n"
    exit 0
else
    echo -e "  ${RED}${BOLD}✘ One or more benchmarks reported errors!${RESET}\n"
    exit 1
fi
