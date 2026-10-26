#!/bin/sh
# Builds ds5-mac-bridge beside this file, on a Mac. Apple's command line
# tools are all it needs: xcode-select --install.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
shared="$here/../../src/shared"

# The core's transport sends with MSG_NOSIGNAL, which macOS has not always
# had. This program ignores SIGPIPE itself, so 0 is safe where it is missing.
nosignal=
if ! printf '#include <sys/socket.h>\nint x = MSG_NOSIGNAL;\n' | clang -x c -fsyntax-only - >/dev/null 2>&1; then
    nosignal=-DMSG_NOSIGNAL=0
fi

# -Wno-unused-function: the core's shared .inl files carry helpers this
# program does not call.
clang -std=gnu11 -O2 -Wall -Wextra -Wno-unused-function $nosignal \
    -I"$shared" \
    "$here/ds5_mac_bridge.c" "$shared/ctm_transport.c" "$shared/ctm_hid.c" \
    -framework IOKit -framework CoreFoundation \
    -o "$here/ds5-mac-bridge"
echo "built $here/ds5-mac-bridge"
