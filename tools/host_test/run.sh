#!/bin/sh
# 在 PC 上編譯並測試 log.c 的斷電恢復。
#   ./run.sh              測目前的程式
#   ./run.sh <git ref>    另外再測指定版本（例如 main），方便比較改之前和改之後
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SEEDS=${SEEDS:-2000}

build() {   # $1 = 輸出目錄，$2 = git ref（空白 = 目前的檔案）
    out=$1; ref=$2
    rm -rf "$out"; mkdir -p "$out"
    for f in Core/Inc/log.h Core/Inc/flash.h Core/Inc/wear.h Core/Inc/crc32.h Core/Inc/perf.h Core/Inc/log_uart.h \
             Core/Src/log.c Core/Src/wear.c Core/Src/crc32.c; do
        if [ -n "$ref" ]; then git -C "$ROOT" show "$ref:$f" > "$out/$(basename "$f")"
        else cp "$ROOT/$f" "$out/"; fi
    done
    cp "$HERE"/stub/*.h "$HERE"/sim.h "$out/"
    extra=""
    if grep -q log_inject_pad_cut "$out/log.h"; then extra="-DHAVE_NEW_COMMANDS"; fi
    gcc -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter $extra -I"$out" \
        "$out/log.c" "$out/wear.c" "$out/crc32.c" \
        "$HERE/sim_flash.c" "$HERE/stubs.c" "$HERE/test_log.c" -o "$out/test_log"
}

if [ -n "$1" ]; then
    echo "#### 版本：$1"
    build "$HERE/build_ref" "$1"
    "$HERE/build_ref/test_log" all "$SEEDS"
    echo
fi

echo "#### 版本：目前的程式"
build "$HERE/build_cur" ""
"$HERE/build_cur/test_log" all "$SEEDS"
