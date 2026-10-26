#!/bin/sh
# The Mac proof of concept, checked away from a Mac. First it is compiled
# against stand-in Apple headers (stubs/), which catches a wrong name or type
# in our code but proves nothing about Apple's real ones. Then it is run
# against a pretend pad on a cable (fake_apple.c) and a pretend DS5-USBIP
# (fake_listener.py), five ways, with the address and undefined-behaviour
# checkers on. What only a Mac can show (IOKit's real answers, the seize,
# the report id conventions) is left to the Mac.
#
# Needs gcc and python3. From Windows, with Docker:
#   docker run --rm -v "<this repository>:/core:ro" gcc:13 sh /core/mac/poc/desk-test/run.sh
# It ends "all desk checks passed", or "FAILED" with what was missing.
set -u
here=$(cd "$(dirname "$0")" && pwd)
poc=$(cd "$here/.." && pwd)
shared=$(cd "$here/../../../src/shared" && pwd)
work=$(mktemp -d)
fails=0

expect() {   # expect <file> <text>: the text must be in the file
    if ! grep -qF -- "$2" "$1"; then
        echo "  MISSING from $(basename "$1"): $2"
        fails=$((fails + 1))
    fi
}

expect_exit() {   # expect_exit <got> <wanted>
    if [ "$1" != "$2" ]; then
        echo "  EXIT $1, expected $2"
        fails=$((fails + 1))
    fi
}

echo "== 1. it compiles, without a warning, against the stand-in Apple headers"
if gcc -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-function -I"$here/stubs" -I"$shared" \
        -c "$poc/ds5_mac_bridge.c" -o "$work/a.o"; then
    echo "  ok"
else
    fails=$((fails + 1))
fi

echo "== 2. control: a misspelled field must NOT compile"
sed 's/caps.product_id == DS5_EDGE_PID/caps.productid == DS5_EDGE_PID/' "$poc/ds5_mac_bridge.c" > "$work/broken.c"
if ! grep -q 'caps.productid' "$work/broken.c"; then
    echo "  the control was not applied: the line it changes has moved"
    fails=$((fails + 1))
elif gcc -std=gnu11 -I"$here/stubs" -I"$shared" -I"$poc" -c "$work/broken.c" -o "$work/b.o" 2>/dev/null; then
    echo "  IT COMPILED: this check cannot see a mistake"
    fails=$((fails + 1))
else
    echo "  ok, it failed"
fi

echo "== building it with the pretend macOS"
gcc -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Wall -Wextra -Wno-unused-function -I"$here/stubs" -I"$shared" \
    "$poc/ds5_mac_bridge.c" "$shared/ctm_transport.c" "$shared/ctm_hid.c" "$here/fake_apple.c" \
    -lpthread -o "$work/ds5-mac-bridge" || { echo "FAILED: it did not build"; exit 1; }
export ASAN_OPTIONS=detect_leaks=0
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

listen() {   # listen <mode>: the pretend DS5-USBIP, in the background
    python3 "$here/fake_listener.py" "$1" > "$work/listener.txt" 2>&1 &
    L=$!
    sleep 0.5
}

stop_listening() {
    sleep 0.3
    kill $L 2>/dev/null
    wait $L 2>/dev/null
}

echo "== 3. the PC goes away in the middle of the bridge"
listen drop
timeout 20 "$work/ds5-mac-bridge" 127.0.0.1 > "$work/mac.txt" 2>&1
code=$?
stop_listening
expect_exit $code 1
expect "$work/mac.txt" "opened for this program alone"
expect "$work/mac.txt" "MAC 66:55:44:33:22:11"
expect "$work/mac.txt" "fake: SetReport output id=0x02 48 bytes, the first 0x02"
expect "$work/mac.txt" "the link to the PC dropped"
expect "$work/listener.txt" "BRIDGE_START is what the TV sends for a cabled DualSense"
expect "$work/listener.txt" "bus=3 in=1024 out=1024 feature=64 capsflags=1 path=iokit serial=66:55:44:33:22:11"
expect "$work/listener.txt" 'product="Sony Interactive Entertainment DualSense Wireless Controller" maker=hidraw'
expect "$work/listener.txt" "payload 343 = 343 expected"
expect "$work/listener.txt" "NOTHING before the settings"
expect "$work/listener.txt" "first input: id 0x01, 64 bytes, flags=1 request_id=0"
expect "$work/listener.txt" "feature reply to request 7: flags=1, 41 bytes, id 0x05"
expect "$work/listener.txt" "feature reply to request 8: flags=1, 0 bytes"
expect "$work/listener.txt" "BRIDGE_STOP names the bridge it started"
cat "$work/mac.txt" "$work/listener.txt" > "$work/all3.txt"

echo "== 4. Ctrl+C, after the exclusive open was refused, with IOKit leaving the input id off, and --strip-id"
listen wait
FAKE_DENY_SEIZE=1 FAKE_STRIP_INPUT_ID=1 timeout 20 "$work/ds5-mac-bridge" 127.0.0.1 --strip-id > "$work/mac.txt" 2>&1 &
P=$!
sleep 2
kill -INT $P
wait $P
code=$?
stop_listening
expect_exit $code 0
expect "$work/mac.txt" "macOS may want Input Monitoring"
expect "$work/mac.txt" "opened shared"
expect "$work/mac.txt" "input reports from IOKit leave off their report id"
expect "$work/mac.txt" "fake: SetReport output id=0x02 47 bytes, the first 0x01"
expect "$work/mac.txt" "releasing"
expect "$work/listener.txt" "first input: id 0x01, 64 bytes"
expect "$work/listener.txt" "the Mac side closed the bridge connection"
expect "$work/listener.txt" "BRIDGE_STOP names the bridge it started"
cat "$work/mac.txt" "$work/listener.txt" > "$work/all4.txt"

echo "== 5. the pad unplugged while bridged"
listen wait
FAKE_UNPLUG_MS=1500 timeout 20 "$work/ds5-mac-bridge" 127.0.0.1 > "$work/mac.txt" 2>&1
code=$?
stop_listening
expect_exit $code 0
expect "$work/mac.txt" "the pad was unplugged"
expect "$work/listener.txt" "BRIDGE_STOP names the bridge it started"
cat "$work/mac.txt" "$work/listener.txt" > "$work/all5.txt"

echo "== 6. DS5-USBIP not running"
timeout 20 "$work/ds5-mac-bridge" 127.0.0.1 > "$work/all6.txt" 2>&1
expect_exit $? 1
expect "$work/all6.txt" "DS5-USBIP did not answer at 127.0.0.1 port 48054"

echo "== 7. no DualSense connected"
FAKE_NO_PAD=1 timeout 20 "$work/ds5-mac-bridge" 127.0.0.1 > "$work/all7.txt" 2>&1
expect_exit $? 1
expect "$work/all7.txt" "no DualSense found on a cable"

echo "== 8. the checkers found nothing"
if grep -l -e "runtime error" -e "AddressSanitizer" "$work"/all*.txt; then
    fails=$((fails + 1))
else
    echo "  ok"
fi

if [ $fails -eq 0 ]; then
    echo "all desk checks passed"
else
    echo "FAILED: $fails check(s); the logs are in $work"
    for f in "$work"/all*.txt; do echo "---- $f"; cat "$f"; done
    exit 1
fi
