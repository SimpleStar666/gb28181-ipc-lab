#!/usr/bin/env bash
# demo.sh — 端到端演示：设备模拟器 ↔ 假 GB28181 平台
# ============================================================
# 这一条把整个项目串起来（控制面 + 媒体面都真发真收）：
#
#   ① 启动 假 SIP 平台（tools/fake_sip_platform.py）
#   ② 启动 设备模拟器 build/gb28181_device -c build/demo/demo.ini（文件循环模式，
#      码流来自 assets/testsrc.h264）
#   ③ 平台收 REGISTER → 回 401 挑战 → 设备带 Authorization 重发
#      → **平台按 RFC 2617 真算一遍 Digest，通得过才回 200 OK**
#      （这一步同时验证 gb_md5 + gb_sip 的鉴权实现）
#   ④ 平台发 MESSAGE(Catalog 查询, SN=1) → 断言设备回 <SN>1</SN> 的 Catalog 应答
#   ⑤ 平台发 INVITE（SDP: c=IN IP4 127.0.0.1 / m=video <port> RTP/AVP 96 / y=<ssrc>）
#      → 设备回 200 OK（带自己的 SDP）→ 平台回 ACK
#   ⑥ 平台在 RTP 端口收包，逐字段断言：
#        byte0 == 0x80、byte1 & 0x7F == 96、载荷开头 == 00 00 01 BA、
#        RTP 头的 SSRC == SDP 里 y= 指定的值
#   ⑦ 平台发 BYE（复用 INVITE 的 Call-ID / From tag，最多重传 3 次）→ 设备回 200 OK
#
# 【为什么脚本要"上锁 + 清场"】（这两条都是在修真实 flake，不是保险丝）
#   1. 上锁（flock build/demo/demo.lock）：两个 demo.sh 同时跑会互相抢 SIP/RTP
#      端口、互相杀进程，断言结果不可能稳定。串行化之后，"偶发红"才有意义。
#   2. 清场：gb_sip_init() 会设 SO_REUSEADDR，所以**上一轮残留的设备进程**能让
#      新进程同时 bind 同一个端口 —— 两个设备都在收平台的请求、都在写同一份
#      日志，现象就是随机断言失败 + 日志撕裂（同一文件里两轮的行交错）。
#      因此开工前按「本轮 demo 的配置路径 + 平台日志路径」精确杀掉残留并等它退出；
#      启动后再数一遍设备进程数，>1 直接 FAIL，而不是让 flake 混进结果。
#
# 日志：平台 → build/demo/platform.log，设备 → build/demo/device.log（各自独立，
#   各带本轮 PID/时间戳头；日志里出现两个不同的头 = 有残留进程并发写）。
#
# 用法：
#   bash tools/demo.sh                 # 正常跑
#   bash tools/demo_stress.sh          # 连跑 4 次验证无 flake（验收用）
#   KEEP_LOG=1 bash tools/demo.sh      # 打印完整日志
#   SIP_PORT=25060 DEV_PORT=25061 RTP_PORT=25062 bash tools/demo.sh   # 换端口
#
# 环境变量：DEVICE_BIN 指定已有的设备可执行文件（跳过构建）。
set -u

# 自保：只在真的带 \r 时才改写文件（否则每次都动 mtime，还容易把文件改成只读）
if LC_ALL=C grep -q "$(printf '\r')" "$0" 2>/dev/null; then
    sed -i 's/\r$//' "$0" 2>/dev/null || true
fi

cd "$(dirname "$0")/.." || { echo "FAIL: 无法进入项目根目录"; exit 1; }

DEMO_DIR="build/demo"
mkdir -p "$DEMO_DIR"

SIP_PORT=${SIP_PORT:-15060}
DEV_PORT=${DEV_PORT:-15061}
RTP_PORT=${RTP_PORT:-15062}
DEVICE_ID=${DEVICE_ID:-34020000001320000001}
PLATFORM_ID=${PLATFORM_ID:-44030000002000000001}
DOMAIN=${DOMAIN:-4403000000}
PASSWORD=${PASSWORD:-12345678}
SSRC=${SSRC:-1234567890}
MIN_RTP=${MIN_RTP:-2}
KEEP_LOG=${KEEP_LOG:-0}
DEADLINE=${DEADLINE:-20}

PLAT_LOG="$DEMO_DIR/platform.log"
DEV_LOG="$DEMO_DIR/device.log"
INI="$DEMO_DIR/demo.ini"
RUN_TAG="$(date '+%Y-%m-%d %H:%M:%S') pid=$$"

# ---------------------------------------------------------------- 0. 串行化（flock）
exec 9>"$DEMO_DIR/demo.lock"
if ! flock -n 9; then
    echo "[lock] 已有另一个 demo.sh 在跑，排队等待（$DEMO_DIR/demo.lock）..."
    if ! flock -w 180 9; then
        echo "FAIL: 等锁超过 180 秒仍未拿到，可能有僵死进程占着 $DEMO_DIR/demo.lock"
        exit 1
    fi
fi

echo "================================================================"
echo " demo.sh — 端到端演示（设备模拟器 + 假 GB28181 平台）"
echo "   本轮标记: $RUN_TAG"
echo "   SIP: 平台 127.0.0.1:$SIP_PORT  ←→  设备 127.0.0.1:$DEV_PORT"
echo "   RTP: 平台收流端口 $RTP_PORT"
echo "================================================================"

# ---------------------------------------------------------------- 0b. 测试码流
if [ ! -s assets/testsrc.h264 ]; then
    echo "[0/6] 生成测试码流 assets/testsrc.h264 ..."
    bash tools/gen_teststream.sh || {
        echo "FAIL: 生成测试码流失败（缺 ffmpeg？）"
        exit 1
    }
fi
echo "[0/6] 码流: assets/testsrc.h264 ($(wc -c < assets/testsrc.h264) bytes)"

# ---------------------------------------------------------------- 1. 清场
# 【踩坑记录】这里绝不能用 `pkill -f -- "-c build/demo/demo.ini"` 这种宽泛模式：
#   调用 demo.sh 的那个 shell 自己的命令行里也含有这段文字，于是会把**自己**杀掉
#   （真实踩过：demo.sh 一声不响地退出、退出码 1，因为父 shell 被清理逻辑干掉了）。
#   所以必须「进程名 + 命令行」双重匹配：先按 cmdline 找候选，再用 /proc/<pid>/comm
#   过滤掉 shell/python 包装进程；再加一层 pidfile（带 cmdline 校验，防 PID 复用误杀）。
PLAT_PIDFILE="$DEMO_DIR/platform.pid"
DEV_PIDFILE="$DEMO_DIR/device.pid"

pids_by_name_and_cmd() {   # $1 = comm 的通配模式, $2 = cmdline 必须包含的字符串
    local p c
    for p in $(pgrep -f -- "$2" 2>/dev/null); do
        [ -r "/proc/$p/cmdline" ] || continue
        c=$(cat "/proc/$p/comm" 2>/dev/null || echo "")
        case "$c" in
            $1) echo "$p" ;;
        esac
    done
}

device_pids() {
    pids_by_name_and_cmd 'gb28181_device' "$INI"
}

platform_pids() {
    {
        pids_by_name_and_cmd 'python*' "fake_sip_platform.py --sip-port $SIP_PORT "
        pids_by_name_and_cmd 'python*' "fake_sip_platform.py --log $PLAT_LOG"
    } | sort -u
}

leftovers() {
    { device_pids; platform_pids; } | sort -u
}

kill_pidfile() {   # $1 = pidfile, $2 = 该进程 cmdline 里必须有的字符串（防误杀）
    local f="$1" expect="$2" p
    [ -s "$f" ] || return 0
    p=$(cat "$f" 2>/dev/null)
    case "$p" in ''|*[!0-9]*) return 0 ;; esac
    [ -r "/proc/$p/cmdline" ] || return 0
    if tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -qF -- "$expect"; then
        echo "      -> 按 pidfile 清理 pid=$p ($(basename "$f"))"
        kill "$p" 2>/dev/null || true
    fi
}

kill_pidfile "$DEV_PIDFILE" "-c $INI"
kill_pidfile "$PLAT_PIDFILE" "fake_sip_platform.py"

BEFORE=$(leftovers | tr '\n' ' ')
if [ -n "${BEFORE// /}" ]; then
    echo "[1/6] 发现上一轮残留进程（pid:$BEFORE），先清掉并等其退出 ..."
    # 逐个 kill（不用 pkill -f），确认没有把调用方自己算进去
    for p in $BEFORE; do kill "$p" 2>/dev/null || true; done
    for _ in $(seq 1 40); do
        [ -z "$(leftovers)" ] && break
        sleep 0.1
    done
    STILL=$(leftovers | tr '\n' ' ')
    if [ -n "${STILL// /}" ]; then
        echo "      -> 仍在运行（pid:$STILL），强杀 ..."
        for p in $STILL; do kill -9 "$p" 2>/dev/null || true; done
        sleep 0.5
    fi
    echo "      -> 清场完成"
else
    echo "[1/6] 无残留进程"
fi

# ---------------------------------------------------------------- 2. 设备二进制
DEVICE_BIN=${DEVICE_BIN:-build/gb28181_device}
if [ ! -x "$DEVICE_BIN" ]; then
    echo "[2/6] 没有 $DEVICE_BIN，尝试构建 ..."
    if [ -f Makefile ]; then
        if make -s gb28181_device 2>"$DEMO_DIR/make.log" || make -s 2>>"$DEMO_DIR/make.log"; then
            :
        fi
        [ -s "$DEMO_DIR/make.log" ] && sed 's/^/      make| /' "$DEMO_DIR/make.log" | tail -10
    fi
fi
if [ ! -x "$DEVICE_BIN" ]; then
    echo "      没有 Makefile 产物，直接用 gcc 编 src/*.c ..."
    if ! gcc -O2 -g -std=gnu11 -D_GNU_SOURCE -Wall -I src src/*.c -lpthread -lm \
              -o "$DEMO_DIR/gb28181_device" 2>"$DEMO_DIR/gcc.log"; then
        echo "FAIL: 设备模拟器编译失败："
        sed 's/^/      /' "$DEMO_DIR/gcc.log" | head -30
        exit 1
    fi
    DEVICE_BIN="$DEMO_DIR/gb28181_device"
fi
echo "[2/6] 设备二进制: $DEVICE_BIN"

# ---------------------------------------------------------------- 3. 生成演示配置
cat > "$INI" <<EOF
# demo.sh 自动生成 —— 只用于端到端演示，不要手工改（改 tools/demo.sh）
server_ip      = 127.0.0.1
server_port    = $SIP_PORT
local_port     = $DEV_PORT
device_id      = $DEVICE_ID
device_pwd     = $PASSWORD
platform_id    = $PLATFORM_ID
domain         = $DOMAIN
channel_id     = $DEVICE_ID
device_name    = DemoCamera
rtp_pt         = 96
mtu            = 1400
fps            = 15
ssrc_default   = 305419896
h264_file      = assets/testsrc.h264
audio_enable   = 1
audio_pt       = 8
audio_rate     = 8000
audio_channels = 1
heartbeat_s    = 5
expires        = 3600
log_level      = 3
EOF
echo "[3/6] 演示配置: $INI"

# ---------------------------------------------------------------- 4. 启动假平台
PLAT_PID=""
DEV_PID=""

cleanup() {
    [ -n "$DEV_PID" ] && kill "$DEV_PID" 2>/dev/null
    [ -n "$PLAT_PID" ] && kill "$PLAT_PID" 2>/dev/null
    # 等它们真的退出：残留进程会给下一轮埋雷（之前 demo.sh 偶发红的根因）
    for _ in $(seq 1 30); do
        alive=0
        if [ -n "$DEV_PID" ] && kill -0 "$DEV_PID" 2>/dev/null; then alive=1; fi
        if [ -n "$PLAT_PID" ] && kill -0 "$PLAT_PID" 2>/dev/null; then alive=1; fi
        [ "$alive" -eq 0 ] && break
        sleep 0.1
    done
    [ -n "$DEV_PID" ] && kill -9 "$DEV_PID" 2>/dev/null
    [ -n "$PLAT_PID" ] && kill -9 "$PLAT_PID" 2>/dev/null
    wait 2>/dev/null
    return 0
}
trap cleanup EXIT INT TERM

python3 tools/fake_sip_platform.py \
    --sip-port "$SIP_PORT" --rtp-port "$RTP_PORT" \
    --device-id "$DEVICE_ID" --platform-id "$PLATFORM_ID" --domain "$DOMAIN" \
    --password "$PASSWORD" --ssrc "$SSRC" \
    --min-rtp "$MIN_RTP" --deadline "$DEADLINE" --log "$PLAT_LOG" &
PLAT_PID=$!
echo "$PLAT_PID" > "$PLAT_PIDFILE"
sleep 0.5
if ! kill -0 "$PLAT_PID" 2>/dev/null; then
    echo "FAIL: 假平台启动失败（端口 $SIP_PORT/$RTP_PORT 被占用？）"
    [ -f "$PLAT_LOG" ] && sed 's/^/      /' "$PLAT_LOG"
    exit 1
fi
echo "[4/6] 假平台已启动 (pid $PLAT_PID)，日志: $PLAT_LOG"

# ---------------------------------------------------------------- 5. 启动设备
# 设备日志：清空 + 写本轮标记，之后用 >> 追加（避免把标记冲掉）
echo "# ---- demo.sh 本轮: $RUN_TAG ----" > "$DEV_LOG"
echo "[5/6] 启动设备模拟器: $DEVICE_BIN -c $INI"
"$DEVICE_BIN" -c "$INI" >>"$DEV_LOG" 2>&1 &
DEV_PID=$!
echo "$DEV_PID" > "$DEV_PIDFILE"
sleep 1.0

NDEV=$(device_pids | grep -c . 2>/dev/null || true)
NPLAT_RUN=$(platform_pids | grep -c . 2>/dev/null || true)
if [ "${NDEV:-0}" -ne 1 ] || [ "${NPLAT_RUN:-0}" -ne 1 ]; then
    echo "FAIL: 进程数不对 —— 设备 $NDEV 个（期望 1）、平台 $NPLAT_RUN 个（期望 1）"
    echo "      多出来的进程会让断言结果不可重复（端口争抢 + 日志撕裂）。"
    echo "      残留 pid: $(leftovers | tr '\n' ' ')"
    exit 1
fi
if ! kill -0 "$DEV_PID" 2>/dev/null; then
    echo "      注意：设备进程已退出，日志如下（可能是 CLI/配置不匹配）："
    sed 's/^/      /' "$DEV_LOG" | head -40
fi

# ---------------------------------------------------------------- 6. 等平台跑完
echo "[6/6] 等待假平台走完流程（REGISTER→Catalog→INVITE→RTP→BYE，约 12 秒，最坏 ${DEADLINE}s）..."
wait "$PLAT_PID"
PLAT_RC=$?
PLAT_PID=""

echo "----------------------------------------------------------------"
echo "假平台日志（关键行）："
grep -E '✓|✗|RTP 统计|首个 RTP|RTP 时间戳|注意|PLATFORM RESULT' "$PLAT_LOG" 2>/dev/null | sed 's/^/  /' || true
echo "----------------------------------------------------------------"
echo "设备日志末尾 12 行："
tail -12 "$DEV_LOG" 2>/dev/null | sed 's/^/  /' || true
echo "----------------------------------------------------------------"

# 日志撕裂检测：本轮设备头 / 平台启动行都只应出现一次
NHEAD=$(grep -c "^# ---- demo.sh 本轮" "$DEV_LOG" 2>/dev/null || echo 0)
NPLAT=$(grep -c "\[platform\] 启动 pid=" "$PLAT_LOG" 2>/dev/null || echo 0)
if [ "${NHEAD:-0}" -gt 1 ] || [ "${NPLAT:-0}" -gt 1 ]; then
    echo "警告: 日志里出现 $NHEAD 个设备头 / $NPLAT 个平台启动行 → 有进程并发写同一文件（残留未清干净）"
fi

if [ "$KEEP_LOG" = "1" ]; then
    echo "完整平台日志："; sed 's/^/  /' "$PLAT_LOG" 2>/dev/null
    echo "完整设备日志："; sed 's/^/  /' "$DEV_LOG" 2>/dev/null
fi

if [ "$PLAT_RC" -eq 0 ]; then
    echo "RESULT: PASS [$RUN_TAG] —— 控制面（注册/鉴权/目录/INVITE/ACK/BYE）与媒体面（RTP+PS+SSRC）全部断言通过"
    echo "  证据：$PLAT_LOG ；设备日志：$DEV_LOG"
    exit 0
fi

echo "RESULT: FAIL [$RUN_TAG] —— 假平台有断言未通过（退出码 $PLAT_RC）"
echo "  逐条失败原因："
grep -E '✗|FAIL ' "$PLAT_LOG" 2>/dev/null | sed 's/^/    /' || true
echo "  排障：KEEP_LOG=1 bash tools/demo.sh 可打印完整日志"
exit 1
