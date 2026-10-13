#!/usr/bin/env bash
# ==============================================================================
# examples/run_all.sh - Runs all 65 MountX examples and reports test status
# ==============================================================================

set -e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

NATIVE="./mountx-native"

if [ ! -x "$NATIVE" ]; then
    echo "Error: mountx-native not found or not executable. Compile it first."
    exit 1
fi

echo "================================================================="
echo "       MountX Example Suite Runner (65 Curated Examples)         "
echo "================================================================="

TOTAL=0
PASSED=0
FAILED=0

for file in $(find examples -name "*.mx" | sort); do
    TOTAL=$((TOTAL + 1))
    printf "Running %-55s ... " "$file"
    if $NATIVE "$file" > /dev/null 2>&1; then
        echo -e "\033[0;32m[PASS]\033[0m"
        PASSED=$((PASSED + 1))
    else
        echo -e "\033[0;31m[FAIL]\033[0m"
        FAILED=$((FAILED + 1))
    fi
done

echo "================================================================="
echo "Finished running $TOTAL examples."
echo -e "Passed: \033[0;32m$PASSED\033[0m | Failed: \033[0;31m$FAILED\033[0m"

if [ "$FAILED" -eq 0 ]; then
    echo -e "\033[1;32mALL 65 EXAMPLES PASSED SUCCESSFULLY! ✔\033[0m"
    exit 0
else
    echo -e "\033[1;31mSOME EXAMPLES FAILED! ✖\033[0m"
    exit 1
fi
