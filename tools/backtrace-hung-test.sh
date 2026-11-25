#!/bin/sh
# Runs a gtest binary several times and, if a run outlives the time limit, prints every
# thread's stack with lldb (macOS) or gdb before killing it, so CI shows where a test hangs.
# Usage: tools/backtrace-hung-test.sh <test binary> <gtest filter> [runs] [seconds]
set -eu
binary=$1 filter=$2 runs=${3:-5} limit=${4:-60}
[ -x "$binary" ] || { echo "not an executable: $binary" >&2; exit 2; }

backtrace() {
    # Attaching to a process needs root on macOS; CI runners have password-free sudo.
    sudo=
    if [ "$(uname)" = Darwin ] && sudo -n true 2>/dev/null; then sudo="sudo -n"; fi
    if command -v lldb >/dev/null; then
        $sudo lldb --batch -p "$1" -o "thread backtrace all" -o "process detach"
    elif command -v gdb >/dev/null; then
        $sudo gdb --batch -p "$1" -ex "thread apply all bt"
    else
        echo "neither lldb nor gdb is installed" >&2
    fi
}

run=1
while [ "$run" -le "$runs" ]; do
    echo "== run $run of $runs: $filter"
    "$binary" --gtest_filter="$filter" &
    pid=$!
    waited=0
    while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt "$limit" ]; do
        sleep 1
        waited=$((waited + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        echo "== run $run still running after ${limit}s; stacks of every thread:"
        backtrace "$pid" || echo "== could not read the stacks" >&2
        kill -9 "$pid"
        exit 1
    fi
    wait "$pid" || { echo "== run $run failed" >&2; exit 1; }
    run=$((run + 1))
done
echo "== no run hung"
