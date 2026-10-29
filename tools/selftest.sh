#!/usr/bin/env bash
# selftest.sh — 一键单元测试
# ============================================================
# 做什么：
#   1. 把 src/*.c 编成对象文件（自动跳过含 main() 的文件，例如 src/main.c）；
#   2. 把 tests/test_*.c 逐个与这些对象文件链接成独立可执行；
#   3. 逐个运行，退出码 0 且打印 PASS 才算通过；
#   4. 汇总 PASS n / FAIL m，有 FAIL 则本脚本退出码非 0。
#
# 为什么每个用例都是独立可执行：
#   一个断言失败（比如 PTS 错位）不应该让后面的用例不跑 ——
#   排障时最需要的是「一次跑完，看到底有几处不对」。
#   tests/ 下的 mkps_pts.c 不是用例（它是 verify_pts.sh 的夹具），
#   所以这里只匹配 test_*.c。
#
# 用法：
#   bash tools/selftest.sh                # 全部用例
#   bash tools/selftest.sh test_pts       # 只跑名字含 test_pts 的用例
set -u

# 自保：只在真的带 \r 时才改写文件（否则每次都动 mtime，还可能把文件弄成只读）
if LC_ALL=C grep -q "$(printf '\r')" "$0" 2>/dev/null; then
    sed -i 's/\r$//' "$0" 2>/dev/null || true
fi

cd "$(dirname "$0")/.." || { echo "FAIL: 无法进入项目根目录"; exit 1; }

CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-O1 -g -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -I src"}
LDLIBS=${LDLIBS:-"-lpthread -lm"}
BUILD="build"
OBJDIR="$BUILD/obj"
BINDIR="$BUILD/tests"
FILTER="${1:-}"

mkdir -p "$OBJDIR" "$BINDIR"

echo "================================================================"
echo " selftest.sh — gb28181-ipc-lab 单元测试"
echo "   CC=$CC"
echo "   CFLAGS=$CFLAGS"
echo "   工作目录: $(pwd)"
echo "================================================================"

# ---------------------------------------------------------------- src 清单
SRC_FILES=""
SKIPPED_MAIN=""
for f in src/*.c; do
    [ -e "$f" ] || continue
    if grep -qE '^[[:space:]]*(int|void)[[:space:]]+main[[:space:]]*\(' "$f"; then
        SKIPPED_MAIN="$SKIPPED_MAIN $(basename "$f")"
        continue
    fi
    SRC_FILES="$SRC_FILES $f"
done

if [ -z "$SRC_FILES" ]; then
    echo "FAIL: src/ 下没有可编译的 .c（src-dev 可能还在写；本脚本需要 src/*.c 存在）"
    exit 1
fi

echo "[1/4] 编译 src（跳过含 main 的文件:$SKIPPED_MAIN）"
OBJS=""
RC=0
for f in $SRC_FILES; do
    o="$OBJDIR/$(basename "${f%.c}").o"
    if $CC $CFLAGS -c "$f" -o "$o" 2>"$o.log"; then
        echo "      ok   $f"
        OBJS="$OBJS $o"
    else
        echo "      FAIL $f"
        sed 's/^/           /' "$o.log"
        RC=1
    fi
done
if [ "$RC" -ne 0 ]; then
    echo "FAIL: src 编译失败，先修 src 再跑测试（测试无法链接到有编译错误的库）"
    exit 1
fi

# ---------------------------------------------------------------- 编译用例
echo "[2/4] 编译 tests/test_*.c"
TESTS=""
for t in tests/test_*.c; do
    [ -e "$t" ] || continue
    name=$(basename "${t%.c}")
    if [ -n "$FILTER" ]; then
        case "$name" in
            *"$FILTER"*) ;;
            *) continue ;;
        esac
    fi
    bin="$BINDIR/$name"
    if $CC $CFLAGS -o "$bin" "$t" $OBJS $LDLIBS 2>"$bin.build.log"; then
        echo "      ok   $t"
        TESTS="$TESTS $name"
    else
        echo "      FAIL $t"
        sed 's/^/           /' "$bin.build.log"
        TESTS="$TESTS $name"   # 仍计入汇总，运行阶段会报「编译期失败」
    fi
done

if [ -z "$TESTS" ]; then
    echo "FAIL: 没有匹配到任何 tests/test_*.c（filter='$FILTER'）"
    exit 1
fi

# ---- 抗「并发 make clean」：make clean 是 rm -rf build/，如果它正好在本脚本
#      编译过程中执行，产物会凭空消失，表现为全部用例「编译期失败」。
#      检测缺失的可执行文件并按需重建一次，避免把这种环境噪声误判成代码问题。
MISSING=""
for name in $TESTS; do
    [ -x "$BINDIR/$name" ] || MISSING="$MISSING $name"
done
if [ -n "$MISSING" ]; then
    echo "      注意:$MISSING 的可执行文件不在（build/ 可能被并发 make clean 清掉了），重建一次 ..."
    mkdir -p "$OBJDIR" "$BINDIR"
    OBJS=""
    for f in $SRC_FILES; do
        o="$OBJDIR/$(basename "${f%.c}").o"
        if $CC $CFLAGS -c "$f" -o "$o" 2>/dev/null; then
            OBJS="$OBJS $o"
        fi
    done
    for name in $MISSING; do
        $CC $CFLAGS -o "$BINDIR/$name" "tests/$name.c" $OBJS $LDLIBS \
            2>"$BINDIR/$name.build.log" || true
    done
fi

# ---------------------------------------------------------------- 运行
echo "[3/4] 运行"
PASS=0
FAIL=0
FAILED_LIST=""

for name in $TESTS; do
    bin="$BINDIR/$name"
    if [ ! -x "$bin" ]; then
        echo "      [FAIL] $name  （可执行文件没生成：编译期失败）"
        FAIL=$((FAIL + 1))
        FAILED_LIST="$FAILED_LIST $name(compile)"
        continue
    fi

    t0=$(date +%s%N)
    out=$("$bin" 2>&1)
    code=$?
    t1=$(date +%s%N)
    ms=$(( (t1 - t0) / 1000000 ))

    last=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tail -1)
    if [ "$code" -eq 0 ] && printf '%s\n' "$out" | grep -q '^PASS'; then
        echo "      [PASS] $name  (${ms}ms)  $last"
        PASS=$((PASS + 1))
    else
        echo "      [FAIL] $name  (退出码 $code, ${ms}ms)"
        printf '%s\n' "$out" | sed 's/^/             | /'
        FAIL=$((FAIL + 1))
        FAILED_LIST="$FAILED_LIST $name"
    fi
done

# ---------------------------------------------------------------- 汇总
echo "================================================================"
echo "[4/4] 汇总: PASS $PASS / FAIL $FAIL  (共 $((PASS + FAIL)) 个用例)"
if [ "$FAIL" -ne 0 ]; then
    echo "      失败用例:$FAILED_LIST"
    echo "RESULT: FAIL"
    exit 1
fi
echo "RESULT: PASS —— 全部单元测试通过"
exit 0
