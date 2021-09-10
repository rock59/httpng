#!/bin/bash

set -ex

echo "linkability-helper 1=$1 2=$2"

ASAN_OPTIONS=detect_leaks=0 LUA_PATH="$LUA_PATH;$1" tarantool $2
