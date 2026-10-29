#!/usr/bin/env bash
# build_steps.sh — 编译并运行 steps/ 下的全部 kata
# ============================================================
# steps/ 里的每个 .c 是一节「手工实现」的教学 kata：
#   自带 main()、不依赖 src/，跑完打印 "SELF-CHECK PASS" 或 "SELF-CHECK FAIL: <原因>"。
# 本脚本把它们逐个编译到 build/steps/、逐个运行、汇总结果。
#
# 为什么还要单独跑一遍？
#   单测（tests/）验证的是 src/ 的实现；kata 验证的是「读者照着文档手写能不能复现」。
#   两者失败的含义不同：kata 挂了说明讲义里的字节/公式写错了。
#
# 用法：
#   bash tools/build_steps.sh            # 编译 + 运行全部
#   bash tools/build_steps.sh 05         # 只跑文件名含 05 的
set -u

# 自保：只在真的带 \r 时才改写文件（否则每次都动 mtime，还可能把文件弄成只读）
if LC_ALL=C grep -q "$(printf '\r')" "$0" 2>/dev/null; then
    sed -i 's/\r$//' "$0" 2>/dev/null || true
fi

cd "$(dirname "$0")/.." || { echo "FAIL: 无法进入项目根目录"; exit 1; }

CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-O1 -g -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -I src"}
LDLIBS=${LDLIBS:-"-lpthread -lm"}
OUTDIR="build/steps"
FILTER="${1:-}"
# 编译日志放 mktemp 目录：`make clean` 是 rm -rf build/，日志写在 build/ 里
# 会在并发构建时凭空消失（真实踩过：重定向失败 → 假报"编译失败"）
LOGDIR=$(mktemp -d "${TMPDIR:-/tmp}/gb28181-steps.XXXXXX")
trap 'rm -rf "$LOGDIR"' EXIT

mkdir -p "$OUTDIR"

echo "================================================================"
echo " build_steps.sh — steps/ kata 编译与自检"
echo "================================================================"

if [ ! -d steps ]; then
    echo "FAIL: 没有 steps/ 目录"
    exit 1
fi

# 只有需要链接 src 的 kata 才用得上这些对象；先准备好（也顺手充当一次 src 编译检查）
LIB_C=""
for f in src/*.c; do
    [ -e "$f" ] || continue
    if grep -qE '^[[:space:]]*(int|void)[[:space:]]+main[[:space:]]*\(' "$f"; then
        continue
    fi
    LIB_C="$LIB_C $f"
done

COUNT=0
PASS=0
FAIL=0
FAILED_LIST=""

for src_file in steps/*.c; do
    [ -e "$src_file" ] || continue
    name=$(basename "${src_file%.c}")
    if [ -n "$FILTER" ]; then
        case "$name" in
            *"$FILTER"*) ;;
            *) continue ;;
        esac
    fi
    COUNT=$((COUNT + 1))
    bin="$OUTDIR/$name"

    printf '%-24s ' "$name"

    # 每次编译前重建输出目录（抗并发 make clean）
    mkdir -p "$OUTDIR"
    LOG="$LOGDIR/$name.build.log"

    # kata 设计上是自包含的：先单独编译，失败再尝试链接 src/
    if ! $CC $CFLAGS -o "$bin" "$src_file" $LDLIBS 2>"$LOG"; then
        if [ -n "$LIB_C" ] && $CC $CFLAGS -o "$bin" "$src_file" $LIB_C $LDLIBS \
                                  >>"$LOG" 2>&1; then
            :
        else
            echo "[编译失败]"
            [ -f "$LOG" ] && sed 's/^/    /' "$LOG" | head -20
            FAIL=$((FAIL + 1))
            FAILED_LIST="$FAILED_LIST $name(compile)"
            continue
        fi
    fi

    # 编译成功但产物被并发清掉 → 重建一次再跑
    if [ ! -x "$bin" ]; then
        mkdir -p "$OUTDIR"
        $CC $CFLAGS -o "$bin" "$src_file" $LDLIBS >>"$LOG" 2>&1 || \
            $CC $CFLAGS -o "$bin" "$src_file" $LIB_C $LDLIBS >>"$LOG" 2>&1 || true
    fi

    out=$("$bin" 2>&1)
    code=$?
    last=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tail -1)

    if [ "$code" -eq 0 ] && printf '%s\n' "$out" | grep -q 'SELF-CHECK PASS'; then
        echo "[PASS] $last"
        PASS=$((PASS + 1))
    else
        echo "[FAIL] 退出码 $code"
        printf '%s\n' "$out" | sed 's/^/    | /'
        FAIL=$((FAIL + 1))
        FAILED_LIST="$FAILED_LIST $name"
    fi
done

echo "================================================================"
if [ "$COUNT" -eq 0 ]; then
    echo "FAIL: 没有匹配到任何 steps/*.c（filter='$FILTER'）"
    exit 1
fi
echo "汇总: PASS $PASS / FAIL $FAIL （共 $COUNT 个 kata）"
if [ "$FAIL" -ne 0 ]; then
    echo "失败:$FAILED_LIST"
    echo "RESULT: FAIL"
    exit 1
fi
echo "RESULT: PASS —— 全部 kata 自检通过"
exit 0
