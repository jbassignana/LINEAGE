#!/bin/bash
# builds the server (test build), the c++ mover tests, and runs every test.
# usage: bash tests/run_all.sh [tsan|asan]
set -e
cd "$(dirname "$0")/.."
mode=${1:-test}
bash build.sh "$mode" > /dev/null 2>&1
g++ -std=c++17 -O2 -Wall -Wextra ../client/ue5/test_keyboard_mover.cpp -o bin/test_keyboard_mover
g++ -std=c++17 -O2 -shared -fPIC tests/mover_capi.cpp -o bin/libkm.so

echo "== keyboard mover (c++ unit)"; ./bin/test_keyboard_mover

run() {
    rm -rf bin/data
    (cd bin && TSAN_OPTIONS="halt_on_error=1" ASAN_OPTIONS="detect_leaks=0:halt_on_error=1" exec ./game_server > server.log 2>&1) &
    pid=$!
    sleep 2
    echo "== $*"
    timeout 300 python3 tests/l2bot.py "$@" > bin/test.log 2>&1 || true
    grep -vE "socket.send\(\) raised|Task was destroyed|task: <Task" bin/test.log
    kill -0 $pid 2> /dev/null || { echo "SERVER DIED"; tail -30 bin/server.log; exit 1; }
    kill $pid; wait $pid 2> /dev/null || true
    if grep -q "FAIL" bin/test.log; then echo "TEST FAILED"; exit 1; fi
    if grep -qE "ThreadSanitizer|AddressSanitizer|runtime error" bin/server.log; then
        echo "SANITIZER REPORT"; grep -A20 -E "ThreadSanitizer|AddressSanitizer|runtime error" bin/server.log | head -40; exit 1
    fi
}
run smoke
run wasd
run mover "$PWD/bin/libkm.so"
run fuzz
run load 80 6
run siege 200 8
echo "all done"
