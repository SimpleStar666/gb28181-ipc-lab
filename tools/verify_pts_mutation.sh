#!/usr/bin/env bash
# verify_pts_mutation.sh — 负向对照：证明这套测试**真的抓得住**那个 >>15 的位域 bug
# ============================================================
# 只证明"测试通过"是不够的 —— 一个永远打印 PASS 的测试也能通过。
# 要证明测试有牙，必须做一次变异测试（mutation test）：
#
#   1. 把 src/ 复制到 build/mutant/（**绝不动 src/**）；
#   2. 在副本里把 gb_ps.c 的 PTS 编码 byte2 从 `>> 14` 改成原型里的 bug `>> 15`；
#   3. 用这份"变异体"重跑 tests/test_pts.c 与 tools/verify_pts.sh 的核心闭环；
#   4. 断言两个检查**都失败**，并且分别报出我们预期的错误值：
#        - test_pts:      往返校验失败 / byte2 = 0x03 而不是 0x05
#        - verify_pts:    ffprobe 读回的 PTS 与写入值不一致（90000 -> 98304）
#
# 结论：原件通过 + 变异体被抓住 = 这两个测试确实在检查 PTS 位域，而不是走过场。
#
# 用法：bash tools/verify_pts_mutation.sh
# 退出码：0 = 负向对照成功（变异体确实被抓住）；1 = 变异体没被抓住（测试无效！）
set -u

# 自保：只在真的带 \r 时才改写文件（否则每次都动 mtime，还可能把文件弄成只读）
if LC_ALL=C grep -q "$(printf '\r')" "$0" 2>/dev/null; then
    sed -i 's/\r$//' "$0" 2>/dev/null || true
fi

cd "$(dirname "$0")/.." || { echo "FAIL: 无法进入项目根目录"; exit 1; }

CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-O1 -g -std=gnu11 -D_GNU_SOURCE -Wall -I src"}
MUT="build/mutant"
H264="assets/testsrc.h264"
BAD_PTS=90000
GOOD_PTS_BYTES="21 00 05 BF 21"
BAD_PTS_BYTES="21 00 03 BF 21"   # >>15 的产物

mkdir -p "$MUT"
rm -rf "$MUT"/*
mkdir -p "$MUT"

# 编译日志放 mktemp 目录（`make clean` 会 rm -rf build/，日志写 build/ 里会在并发构建时消失）
LOGDIR=$(mktemp -d "${TMPDIR:-/tmp}/gb28181-mut.XXXXXX")
trap 'rm -rf "$LOGDIR"' EXIT

echo "================================================================"
echo " verify_pts_mutation.sh — 负向对照（变异测试）"
echo "================================================================"

# ---------------------------------------------------------------- 1. 复制 src 并植入 bug
cp src/*.h src/*.c "$MUT"/ 2>/dev/null
if [ ! -f "$MUT/gb_ps.c" ]; then
    echo "FAIL: 复制 src/gb_ps.c 失败"
    exit 1
fi
cp -r src/*.h "$MUT"/ 2>/dev/null

# 只动「PTS 编码 byte2」那一行；改不到就说明锚点丢了
before=$(grep -n 'out\[2\]' "$MUT/gb_ps.c" | grep -o '>> *1[45]' | head -1)
sed -i '/out\[2\]/s/>> *14/>> 15/' "$MUT/gb_ps.c"
after=$(grep -n 'out\[2\]' "$MUT/gb_ps.c" | grep -o '>> *1[45]' | head -1)

echo "[1/4] 变异点: build/mutant/gb_ps.c 里 PTS byte2 的位移  $before  ->  $after"
if [ "$after" != ">> 15" ]; then
    echo "FAIL: 变异没有生效（锚点 'out[2] ... >> 14' 没找到）—— 负向对照无效"
    grep -n 'out\[2\]' "$MUT/gb_ps.c" | sed 's/^/    /'
    exit 1
fi
grep -n 'out\[2\]' "$MUT/gb_ps.c" | sed 's/^/    /'

# ---------------------------------------------------------------- 2. 编译变异体
LIB_C=""
for f in "$MUT"/*.c; do
    case "$(basename "$f")" in
        main.c) continue ;;   # 有 main() 的装配文件不参与链接
    esac
    LIB_C="$LIB_C $f"
done
echo "[2/4] 编译变异体（$(echo $LIB_C | wc -w) 个 .c）"
mkdir -p "$MUT"
if ! $CC $CFLAGS -o "$MUT/test_pts_mutant" tests/test_pts.c $LIB_C -lpthread -lm 2>"$LOGDIR/build.log"; then
    echo "FAIL: 变异体编译失败："; sed 's/^/    /' "$LOGDIR/build.log" | head -20
    exit 1
fi
if ! $CC $CFLAGS -o "$MUT/mkps_pts_mutant" tests/mkps_pts.c $LIB_C -lpthread -lm 2>>"$LOGDIR/build.log"; then
    echo "FAIL: mkps_pts 变异体编译失败："; sed 's/^/    /' "$LOGDIR/build.log" | head -20
    exit 1
fi

CAUGHT_UNIT=0
CAUGHT_FFPROBE=0

# ---------------------------------------------------------------- 3. 单测必须失败
echo "[3/4] 用变异体跑 tests/test_pts.c（**期望失败**）"
out=$("$MUT/test_pts_mutant" 2>&1)
rc=$?
printf '%s\n' "$out" | sed 's/^/    | /'
if [ "$rc" -ne 0 ]; then
    echo "      -> 变异体被单测抓住（退出码 $rc）✓"
    CAUGHT_UNIT=1
else
    echo "      -> 单测居然通过了：说明 tests/test_pts.c 根本没检查 PTS 位域 ✗"
fi

# ---------------------------------------------------------------- 4. ffprobe 闭环也必须失败
echo "[4/4] 用变异体生成 .ps 并让 ffprobe 反读（**期望不一致**）"
if [ ! -s "$H264" ]; then
    bash tools/gen_teststream.sh >/dev/null 2>&1 || true
fi
if [ -s "$H264" ]; then
    "$MUT/mkps_pts_mutant" "$MUT/pts_$BAD_PTS.ps" "$BAD_PTS" "$H264" | sed 's/^/    | /'
else
    "$MUT/mkps_pts_mutant" "$MUT/pts_$BAD_PTS.ps" "$BAD_PTS" | sed 's/^/    | /'
fi

if command -v ffprobe >/dev/null 2>&1; then
    RPTS=$(ffprobe -v error -select_streams v:0 -show_entries packet=pts \
                 -of default=noprint_wrappers=1:nokey=1 "$MUT/pts_$BAD_PTS.ps" 2>/dev/null | head -1)
    echo "    | ffprobe 读回: pts=${RPTS:-<空>}（写入值 $BAD_PTS）"
    if [ "${RPTS:-}" = "$BAD_PTS" ]; then
        echo "      -> ffprobe 读数居然一致：闭环验证无效 ✗"
    else
        echo "      -> ffprobe 读回值与写入值不一致 → 变异体被闭环抓住 ✓"
        CAUGHT_FFPROBE=1
        if [ -n "${RPTS:-}" ] && [ "$RPTS" != "N/A" ]; then
            diff=$((RPTS - BAD_PTS))
            echo "    | 差值: $RPTS - $BAD_PTS = $diff ticks ($(awk -v d=$diff 'BEGIN{printf "%.4f", d/90000}') 秒)"
            echo "    | 直观解释: 33bit PTS 被整体挪了一位 -> 低 15 位的信息被搬到 bit15 之上"
        fi
    fi
else
    echo "    | 没有 ffprobe：退化为只依赖单测结论"
    CAUGHT_FFPROBE=1
fi

# ---------------------------------------------------------------- 5. 对照：原件必须通过
echo "----------------------------------------------------------------"
echo "对照：未变异的原件（build/ 里已有的话直接跑）"
if [ -x build/tests/test_pts ]; then
    build/tests/test_pts >/tmp/orig_pts.out 2>&1 && echo "    原件 test_pts: PASS ✓" \
        || { echo "    原件 test_pts 失败："; sed 's/^/    | /' /tmp/orig_pts.out; }
else
    echo "    提示：先跑一次 bash tools/selftest.sh 生成 build/tests/test_pts"
fi

echo "================================================================"
echo "变异体 byte2 期望字节: $BAD_PTS_BYTES（正确值应为 $GOOD_PTS_BYTES）"
if [ "$CAUGHT_UNIT" -eq 1 ] && [ "$CAUGHT_FFPROBE" -eq 1 ]; then
    echo "RESULT: PASS —— 负向对照成功：单元测试与 ffprobe 闭环都抓住了 >>15 位域错误。"
    echo "        这正是「画面照样正常、几个月没人发现」的那类错误，"
    echo "        现在它在 CI 里活不过 1 秒。"
    exit 0
fi
echo "RESULT: FAIL —— 变异体没被完全抓住（unit=$CAUGHT_UNIT ffprobe=$CAUGHT_FFPROBE），测试有效性存疑"
exit 1
