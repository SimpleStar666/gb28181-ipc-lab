#!/usr/bin/env bash
# ============================================================================
# steps/run_all.sh —— 把 8 个协议 kata 逐个编译到 steps/build/ 并运行
#
#   用法：  bash steps/run_all.sh          （在仓库任意位置都行，脚本会自己 cd）
#   判定：  编译零 warning + 退出码 0 + 输出里有 SELF-CHECK PASS，三者齐了才算 PASS
#   退出码：有任何一个 FAIL → 1；全绿 → 0
#
#   换行符：本文件是 LF 换行（用 `file run_all.sh` 或 `od -c` 可验证）。
#           万一被编辑器存成了 CRLF，bash 会报 $'\r' 之类的错，先手动跑一次：
#               sed -i 's/\r$//' run_all.sh
# ============================================================================
if LC_ALL=C grep -q "$(printf '\r')" "$0" 2>/dev/null; then
    sed -i 's/\r$//' "$0"
    exec bash "$0" "$@"
fi

cd "$(dirname "$0")" || exit 1

CC=${CC:-gcc}
CFLAGS="-Wall -Wextra -O2"
BUILD=build
mkdir -p "$BUILD"

KATAS="01_annexb_split 02_pts_encode 03_ps_pack 04_psm_layout 05_rtp_slice 06_md5_digest 07_g711a 08_rtp_loopback"

pass=0
fail=0
failed_list=""

echo "==================== steps/run_all.sh ===================="
echo "编译器  : $($CC --version | head -1)"
echo "编译参数: $CFLAGS -pthread（出现任何 warning 也算 FAIL）"
echo

for name in $KATAS; do
    src="$name.c"
    bin="$BUILD/$name"
    log="$BUILD/$name.compile.log"
    out="$BUILD/$name.out"
    echo "──────── [$name] ────────"
    if [ ! -f "$src" ]; then
        echo "  ✗ 源文件不存在：$src"
        fail=$((fail + 1))
        failed_list="$failed_list $name(缺失)"
        continue
    fi
    if ! $CC $CFLAGS -pthread -o "$bin" "$src" 2>"$log"; then
        echo "  ✗ 编译失败："
        sed 's/^/      /' "$log"
        fail=$((fail + 1))
        failed_list="$failed_list $name(编译失败)"
        echo
        continue
    fi
    warn=0
    if [ -s "$log" ]; then
        warn=1
        echo "  ✗ 编译有 warning（要求零 warning）："
        sed 's/^/      /' "$log"
    fi
    "$bin" >"$out" 2>&1
    rc=$?
    cat "$out"
    if [ $rc -eq 0 ] && grep -q "SELF-CHECK PASS" "$out" && [ $warn -eq 0 ]; then
        echo "  ✔ PASS（零 warning、退出码 0、SELF-CHECK PASS）"
        pass=$((pass + 1))
    else
        why=""
        [ $warn -ne 0 ] && why="$why warning"
        [ $rc -ne 0 ] && why="$why 退出码=$rc"
        grep -q "SELF-CHECK PASS" "$out" || why="$why 未打印SELF-CHECK_PASS"
        echo "  ✗ FAIL（$why）"
        fail=$((fail + 1))
        failed_list="$failed_list $name($why)"
    fi
    echo
done

echo "==================== 汇总 ===================="
echo "PASS: $pass / $((pass + fail))"
echo "FAIL: $fail"
if [ -n "$failed_list" ]; then
    echo "失败项：$failed_list"
fi
if [ $fail -ne 0 ]; then
    echo "结果：有 FAIL，退出码 1"
    exit 1
fi
echo "结果：8 个 kata 全部通过"
exit 0
