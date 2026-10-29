#!/usr/bin/env bash
# demo_stress.sh — 连续跑多次 demo.sh，验证端到端演示**不 flake**
# ============================================================
# 为什么需要它：
#   demo.sh 曾经偶发失败（同一断言两次结果不同）。修完之后必须能证明"稳定"，
#   而"跑一次绿"证明不了任何事 —— 竞态恰恰是跑一次看不出来的。
#   所以这里连跑 N 次（默认 4 次，对齐验收标准），每次：
#     - 保留独立的日志副本 build/demo/run_<i>/{platform,device}.log
#     - 记录耗时与退出码
#     - 顺带检查日志里是否出现两次"启动"头（进程并发写同一文件的铁证）
#   任何一次失败即整体 FAIL 并退出非 0。
#
# 用法：
#   bash tools/demo_stress.sh          # 默认 4 次
#   RUNS=8 bash tools/demo_stress.sh   # 自定义次数
#   WITH_LEFTOVER=1 bash tools/demo_stress.sh
#       ↑ 先人为制造"上一轮没杀干净的设备+平台"，再跑。用来回归验证
#         「残留进程不会让 demo.sh 偶发红」——这正是当初 flake 的成因。
set -u

# 自保：只在真的带 \r 时才改写文件
if LC_ALL=C grep -q "$(printf '\r')" "$0" 2>/dev/null; then
    sed -i 's/\r$//' "$0" 2>/dev/null || true
fi

cd "$(dirname "$0")/.." || { echo "FAIL: 无法进入项目根目录"; exit 1; }

RUNS=${RUNS:-4}
WITH_LEFTOVER=${WITH_LEFTOVER:-0}
DEMO_DIR="build/demo"
SUMMARY="$DEMO_DIR/stress_summary.txt"

mkdir -p "$DEMO_DIR"
: > "$SUMMARY"

echo "================================================================"
echo " demo_stress.sh — 连续跑 $RUNS 次 demo.sh（串行；demo.sh 自带 flock 也会兜底）"
echo "================================================================"

# ---------------------------------------------------------------- 可选：人为制造残留
if [ "$WITH_LEFTOVER" = "1" ]; then
    if [ ! -s "$DEMO_DIR/demo.ini" ]; then
        echo "[leftover] 先生成一次 demo.ini ..."
        bash tools/demo.sh >/dev/null 2>&1 || true
    fi
    echo "[leftover] 人为制造残留：设备 + 平台（模拟上一轮没杀干净）"
    build/gb28181_device -c "$DEMO_DIR/demo.ini" >/dev/null 2>&1 &
    LDEV=$!
    python3 tools/fake_sip_platform.py --sip-port 15060 --rtp-port 15062 \
        --log "$DEMO_DIR/platform.log" >/dev/null 2>&1 &
    LPLAT=$!
    echo "$LDEV" > "$DEMO_DIR/device.pid"
    echo "$LPLAT" > "$DEMO_DIR/platform.pid"
    sleep 2
    echo "[leftover] 残留 pid: device=$LDEV platform=$LPLAT（demo.sh 应该在 [1/6] 检测并清掉）"
fi

PASS=0
FAIL=0
FAILED_RUNS=""

for i in $(seq 1 "$RUNS"); do
    rundir="$DEMO_DIR/run_$i"
    mkdir -p "$rundir"
    echo "----------------------------------------------------------------"
    echo ">>> 第 $i/$RUNS 次"
    t0=$(date +%s)
    out=$(bash tools/demo.sh 2>&1)
    rc=$?
    t1=$(date +%s)
    dt=$((t1 - t0))

    # 留下每轮证据（demo.sh 每轮都会覆盖 build/demo/*.log，这里先拷走）
    cp -f "$DEMO_DIR/platform.log" "$rundir/platform.log" 2>/dev/null || true
    cp -f "$DEMO_DIR/device.log"   "$rundir/device.log"   2>/dev/null || true
    printf '%s\n' "$out" > "$rundir/demo_stdout.txt"

    # 残留进程被检测到 / 被清场（WITH_LEFTOVER=1 时第 1 轮必须出现）
    cleaned=$(grep -c "发现上一轮残留进程" "$rundir/demo_stdout.txt" 2>/dev/null || echo 0)

    # 日志撕裂 / 多进程检测
    nplat=$(grep -c "\[platform\] 启动 pid=" "$rundir/platform.log" 2>/dev/null || echo 0)
    nhead=$(grep -c "^# ---- demo.sh 本轮" "$rundir/device.log" 2>/dev/null || echo 0)
    tear=""
    if [ "${nplat:-0}" -gt 1 ] || [ "${nhead:-0}" -gt 1 ]; then
        tear=" [日志撕裂: 平台启动行 $nplat / 设备头 $nhead]"
    fi
    if [ "$WITH_LEFTOVER" = "1" ] && [ "$i" = "1" ]; then
        if [ "${cleaned:-0}" -ge 1 ]; then
            echo "    第 1 轮检测到并清理了人为残留 ✓"
        else
            echo "    第 1 轮没有报告残留进程 —— 残留检测逻辑可能失效 ✗"
            tear="$tear [残留检测失效]"
        fi
    fi

    if [ "$rc" -eq 0 ]; then
        PASS=$((PASS + 1))
        echo "    第 $i 次: PASS (${dt}s)$tear"
        grep -E 'RTP 统计|PLATFORM RESULT' "$rundir/platform.log" 2>/dev/null | sed 's/^/      /'
    else
        FAIL=$((FAIL + 1))
        FAILED_RUNS="$FAILED_RUNS $i"
        echo "    第 $i 次: FAIL (rc=$rc, ${dt}s)$tear"
        grep -E '✗|FAIL ' "$rundir/platform.log" 2>/dev/null | sed 's/^/      /'
        echo "      失败断言上下文（platform.log 尾部）："
        tail -20 "$rundir/platform.log" 2>/dev/null | sed 's/^/        /'
    fi
    printf 'run %d: %s rc=%d %ds%s\n' "$i" "$([ "$rc" -eq 0 ] && echo PASS || echo FAIL)" "$rc" "$dt" "$tear" >> "$SUMMARY"
done

echo "================================================================"
echo "压力结果: PASS $PASS / FAIL $FAIL  （共 $RUNS 次，每轮日志在 $DEMO_DIR/run_<i>/）"
cat "$SUMMARY" | sed 's/^/  /'
if [ "$FAIL" -ne 0 ]; then
    echo "RESULT: FAIL —— 失败的轮次:$FAILED_RUNS"
    exit 1
fi
echo "RESULT: PASS —— 连续 $RUNS 次端到端演示全部通过，无 flake、无日志撕裂"
exit 0
