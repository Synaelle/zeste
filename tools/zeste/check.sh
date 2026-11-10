#!/usr/bin/env bash
# Zeste build check: the unit tests, then the Zeste test ROM headless in strict mode, with the
# lines its output must contain. Run by build_linux.sh after every build, or alone:
#
#   tools/zeste/check.sh [zeste binary] [test ROM .cia]
#
# The test ROM is not in the repository: give it as the second argument or in the ZESTE_TEST_ROM
# environment variable. Without it only the unit tests run.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
exe=${1:-$root/build/bin/Release/zeste}
rom=${2:-${ZESTE_TEST_ROM:-}}
tests=$(dirname "$exe")/tests
failed=()

if [ -x "$tests" ]; then
    if out=$(timeout 600 "$tests" 2>&1); then
        echo "check: unit tests ok ($(grep "test cases:" <<<"$out" | tr -s ' '))"
    else
        echo "$out" | tail -30
        failed+=("unit tests")
    fi
fi

if [ -z "$rom" ] || [ ! -f "$rom" ]; then
    echo "check: no test ROM (second argument or ZESTE_TEST_ROM), skipped"
else
    # Waits for the line that says the ROM is ready (no guessed frame numbers), then X: a write
    # to NULL, which the ROM's exception handler answers right away with "[X] PASS".
    script=$(mktemp --suffix=.zs)
    trap 'rm -f "$script"' EXIT
    printf '%s\n' "@1 wait-log timeout=120s C3D_GetDrawingTime" "@2 press X 5" "@120 quit 0" > "$script"
    # No display needed: the software renderer draws offscreen.
    out=$(QT_QPA_PLATFORM=offscreen timeout 300 "$exe" --headless --strict --renderer software \
        --boot-cia "$rom" --script "$script" 2>&1)
    code=$?
    folder=$(grep -o "Zeste run folder: .*" <<<"$out" | head -1 | cut -d' ' -f4-)
    game=""
    [ -n "$folder" ] && [ -f "$folder/game_output.txt" ] && game=$(cat "$folder/game_output.txt")
    expected=(
        "Zeste test ROM"
        "svcGetProcessorID (core 1 thread) = 1"          # kernel_fixes
        "getaddrinfo conntest.nintendowifi.net -> failed" # Nintendo's servers are never reached
        "gethostbyname account.nintendo.net -> failed"
        "C3D_GetDrawingTime after 60 frames"
        "[X] exception handler ran"                       # cpu_exceptions
        "[X] PASS"
    )
    missing=()
    for line in "${expected[@]}"; do
        grep -qF -- "$line" <<<"$game" || missing+=("$line")
    done
    [ $code -ne 0 ] && failed+=("test ROM exit code $code")
    [ ${#missing[@]} -gt 0 ] && failed+=("test ROM output lacks: $(printf '%s | ' "${missing[@]}")")
    [ -z "$folder" ] && failed+=("no run folder in the output")
    if [ $code -eq 0 ] && [ ${#missing[@]} -eq 0 ]; then
        echo "check: test ROM ok (${#expected[@]} lines, $folder)"
    elif [ -n "$folder" ]; then
        echo "check: see $folder (summary.md, game_output.txt)"
    else
        echo "$out" | tail -20
    fi
fi

if [ ${#failed[@]} -gt 0 ]; then
    echo "check FAILED: $(printf '%s; ' "${failed[@]}")"
    exit 1
fi
echo "check: all good"
