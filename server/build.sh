#!/bin/bash
# usage: bash build.sh          release build
#        bash build.sh test     + test hooks (bots can teleport)
#        bash build.sh tsan     + thread sanitizer
#        bash build.sh asan     + address sanitizer
set -e
pushd bin > /dev/null

flags="-Wall -Wextra -O2 -g $EXTRA_FLAGS"
case "$1" in
    test) flags="$flags -DL2_TEST_TELEPORT" ;;
    tsan) flags="-Wall -Wextra -O1 -g -DL2_TEST_TELEPORT -fsanitize=thread" ;;
    asan) flags="-Wall -Wextra -O1 -g -DL2_TEST_TELEPORT -fsanitize=address,undefined -fno-omit-frame-pointer" ;;
esac

gcc $flags -o login_server ../login_server.c ../directory.c ../net_linux.c ../wqueue.c -I../openssl-1.1/x64/include -lcrypto -lpthread

gcc $flags -o game_server.so -fPIC -rdynamic -shared ../game_server_lib.c ../directory.c ../net_linux.c -lpthread -lm

gcc $flags -o game_server ../game_server.c ../net_linux.c ../library.c -lpthread -ldl

popd > /dev/null
