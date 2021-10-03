#!/usr/bin/bash
set -exu

if [ ! -e .rocks/bin/luatest ]; then
    tarantoolctl rocks install luatest
fi
killall process_helper 2>/dev/null || true
rm tmp_reaper_socket 2>/dev/null || true

if [ ! -e tests/all.lua ]; then
    ln -s $1/tests tests
fi

SRC=$1
if [ -v LUA_PATH ]; then
    export LUA_PATH=$SRC/?.lua;$LUA_PATH
else
    export LUA_PATH=$SRC/?.lua
fi

ASAN_OPTIONS=detect_leaks=0 ./.rocks/bin/luatest -v tests/all.lua --shuffle all
