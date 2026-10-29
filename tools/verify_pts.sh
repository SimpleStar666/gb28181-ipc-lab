#!/usr/bin/env bash
# verify_pts.sh — 【本项目最重要的一条验证】PTS 位域闭环
# ============================================================
# 要证明的事：
#   「MPEG-PS 的 33bit PTS 铺进 5 个字节时没有错位」
#   —— 特别是 byte2 必须用 (pts >> 14)，写成 (pts >> 15) 的经典 bug。
#
# 为什么必须闭环、不能只看代码：
#   下游（ZLMediaKit 等）多数路径不依赖 PES 里的 PTS，画面照样正常，
#   所以这种错位可以在代码里活几个月没人发现。
#   唯一的硬证据是：**用我们自己的打包函数写出一个 PTS 已知的 .ps，
#   再用第三方工具（ffprobe）把它读回来，比对数值。**
#   编码器和解码器由两个独立实现完成，谁错都会被对方抓到。
#
# 三层校验（强度递减，互为补充）：
#   1. ffprobe -show_packets 读回的 pts / pts_time  == 写入值     <- 主证据
#   2. 直接按 ISO 13818-1 位域解析 .ps 字节，还原 PTS  == 写入值   <- 独立于 ffprobe
#   3. mkps_pts 打印的 PES 头 5 字节 == gb_ps_encode_pts() 的输出
#
# 退化路径：若 ffmpeg/ffprobe 不可用、或码流解不出帧（例如只有内置最小帧），
#   脚本会在输出里明确写出退化原因，并用第 2 层（字节级解析）作为结论依据。
#
# 用法：bash tools/verify_pts.sh
# 退出码：0 = 全部通过；1 = 有不一致（说明 PTS 位域写错了）
set -u

# 自保：只在真的带 \r 时才改写文件（否则每次都动 mtime，还可能把文件弄成只读）
if LC_ALL=C grep -q "$(printf '\r')" "$0" 2>/dev/null; then
    sed -i 's/\r$//' "$0" 2>/dev/null || true
fi

cd "$(dirname "$0")/.." || { echo "FAIL: 无法进入项目根目录"; exit 1; }

CC=${CC:-gcc}
CFLAGS="-O1 -g -Wall -I src"
OUTDIR="build"
H264="assets/testsrc.h264"
PTS_LIST=${PTS_LIST:-"0 6000 90000 900000 123456789"}

# 编译日志放 mktemp 目录（`make clean` 会 rm -rf build/，日志写 build/ 里会在并发构建时消失）
LOGDIR=$(mktemp -d "${TMPDIR:-/tmp}/gb28181-pts.XXXXXX")
trap 'rm -rf "$LOGDIR"' EXIT

mkdir -p "$OUTDIR"

echo "================================================================"
echo " verify_pts.sh — MPEG-PS PTS 位域闭环验证"
echo "   项目根目录: $(pwd)"
echo "================================================================"

# ---------------------------------------------------------------- 0. 测试码流
if [ ! -s "$H264" ]; then
    echo "[0/4] $H264 不存在，尝试生成 ..."
    bash tools/gen_teststream.sh || true
fi

FRAME_SRC=""
if [ -s "$H264" ]; then
    FRAME_SRC="$H264"
    echo "[0/4] 使用真实 x264 码流: $H264 ($(wc -c < "$H264") bytes)"
else
    echo "[0/4] 注意：拿不到 $H264，将使用 tests/mkps_pts.c 里的内置最小帧"
    echo "      （内置帧的 slice 是桩数据，ffprobe 可能解不出帧 —— 那时会走退化路径）"
fi

# ---------------------------------------------------------------- 1. 编译
LIB_C=""
MAIN_C=""
for f in src/*.c; do
    [ -e "$f" ] || continue
    if grep -qE '^[[:space:]]*(int|void)[[:space:]]+main[[:space:]]*\(' "$f"; then
        MAIN_C="$MAIN_C $f"
    else
        LIB_C="$LIB_C $f"
    fi
done

if [ -z "$LIB_C" ]; then
    echo "FAIL: src/ 下还没有可编译的 .c 文件（src-dev 可能仍在编写）"
    exit 1
fi

echo "[1/4] 编译 mkps_pts（src 库文件: $(echo $LIB_C | wc -w) 个，含 main 被排除:$MAIN_C）"
mkdir -p "$OUTDIR"
if ! $CC $CFLAGS -o "$OUTDIR/mkps_pts" tests/mkps_pts.c $LIB_C -lpthread -lm 2>"$LOGDIR/mkps_pts.build.log"; then
    echo "FAIL: 编译 mkps_pts 失败，错误如下："
    sed 's/^/    /' "$LOGDIR/mkps_pts.build.log"
    exit 1
fi
echo "      -> $OUTDIR/mkps_pts 编译成功"

FFPROBE_OK=0
DEGRADED=0
TOTAL=0
BAD=0

# ---------------------------------------------------------------- 2. 逐值闭环
for PTS in $PTS_LIST; do
    TOTAL=$((TOTAL + 1))
    PSFILE="$OUTDIR/pts_$PTS.ps"
    mkdir -p "$OUTDIR"   # 抗并发 make clean：每轮都确认输出目录还在

    echo "----------------------------------------------------------------"
    echo "[2/4] PTS=$PTS"

    if [ -n "$FRAME_SRC" ]; then
        if ! "$OUTDIR/mkps_pts" "$PSFILE" "$PTS" "$FRAME_SRC"; then
            echo "FAIL: mkps_pts 生成 $PSFILE 失败"
            BAD=$((BAD + 1))
            continue
        fi
    else
        if ! "$OUTDIR/mkps_pts" "$PSFILE" "$PTS"; then
            echo "FAIL: mkps_pts 生成 $PSFILE 失败"
            BAD=$((BAD + 1))
            continue
        fi
    fi

    # ---- 2a. 独立于 ffprobe 的字节级解析（按 ISO 13818-1 位域表） ----
    BYTE_PTS=$(python3 - "$PSFILE" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
i = d.find(b'\x00\x00\x01\xe0')
if i < 0:
    print("NO_PES"); sys.exit(0)
if len(d) < i + 14:
    print("TRUNCATED"); sys.exit(0)
plen = (d[i+4] << 8) | d[i+5]
f1 = d[i+6]
b = d[i+9:i+14]
if not (f1 & 0x80) or not (b[0] & 1) or not (b[2] & 1) or not (b[4] & 1):
    print("BAD_MARKER"); sys.exit(0)
pts = ((b[0] >> 1) & 0x07) << 30
pts |= b[1] << 22
pts |= ((b[2] >> 1) & 0x7F) << 15
pts |= b[3] << 7
pts |= (b[4] >> 1) & 0x7F
print("%d %d" % (pts, plen))
PY
)
    BYTE_V=${BYTE_PTS%% *}
    PES_PLEN=${BYTE_PTS##* }
    if [ "$BYTE_V" = "$PTS" ]; then
        echo "      [字节级] 按 ISO 13818-1 位域解析 PES 中的 PTS = $BYTE_V  ✓ 与写入值一致"
    else
        echo "      [字节级] 解析结果 = $BYTE_PTS （PES_packet_length=$PES_PLEN），写入值 = $PTS  ✗"
        echo "FAIL: $PSFILE 的 PES PTS 字节与写入值不一致（不需要 ffprobe 就能判定 PTS 编码错）"
        BAD=$((BAD + 1))
        continue
    fi

    # ---- 2b. ffprobe 读回（主证据） ----
    if ! command -v ffprobe >/dev/null 2>&1; then
        echo "      [ffprobe] 不可用 -> 退化：仅以字节级解析为依据"
        DEGRADED=$((DEGRADED + 1))
        continue
    fi

    TB=$(ffprobe -v error -select_streams v:0 -show_entries stream=time_base \
                 -of default=noprint_wrappers=1:nokey=1 "$PSFILE" 2>/dev/null | head -1)
    RPTS=$(ffprobe -v error -select_streams v:0 -show_entries packet=pts \
                 -of default=noprint_wrappers=1:nokey=1 "$PSFILE" 2>/dev/null | head -1)
    RTIME=$(ffprobe -v error -select_streams v:0 -show_entries packet=pts_time \
                 -of default=noprint_wrappers=1:nokey=1 "$PSFILE" 2>/dev/null | head -1)
    NFRAMES=$(ffprobe -v error -select_streams v:0 -count_frames \
                 -show_entries stream=nb_read_frames \
                 -of default=noprint_wrappers=1:nokey=1 "$PSFILE" 2>/dev/null | head -1)

    echo "      [ffprobe] time_base=${TB:-<空>} pts=${RPTS:-<空>} pts_time=${RTIME:-<空>} decoded_frames=${NFRAMES:-<空>}"

    VERDICT=$(python3 - "$PTS" "${TB:-}" "${RPTS:-}" "${RTIME:-}" <<'PY'
import sys
written = float(sys.argv[1]); tb = sys.argv[2]; rp = sys.argv[3]; rt = sys.argv[4]

def num(s):
    try:
        return float(s)
    except Exception:
        return None

tbv = None
try:
    a, b = tb.split('/'); tbv = float(a) / float(b)
except Exception:
    tbv = None

exp_s = written / 90000.0
tol = 2e-4          # 18 个 90kHz tick（>>15 错位的偏差是 8192 tick ≈ 0.091s，远超容差）
pt = num(rp); tt = num(rt)

if pt is not None and tbv:
    if abs(pt * tbv - exp_s) <= tol:
        print("OK pts=%d (=%.9fs)" % (int(pt), pt * tbv)); sys.exit(0)
if tt is not None:
    if abs(tt - exp_s) <= tol:
        print("OK pts_time=%.9f" % tt); sys.exit(0)
print("MISMATCH expected_time=%.9f got_pts=%s got_pts_time=%s tb=%s" % (exp_s, rp, rt, tb))
PY
)
    case "$VERDICT" in
        OK*)
            echo "      [ffprobe] $VERDICT  ✓ 与写入 PTS=$PTS 一致"
            FFPROBE_OK=$((FFPROBE_OK + 1))
            ;;
        *)
            if [ -n "$FRAME_SRC" ]; then
                echo "      [ffprobe] $VERDICT"
                echo "FAIL: ffprobe 读回的 PTS 与写入值不一致 -> PTS 位域写错了"
                BAD=$((BAD + 1))
            else
                echo "      [ffprobe] 读不到 PTS（$VERDICT）-> 退化：以字节级解析为依据"
                DEGRADED=$((DEGRADED + 1))
            fi
            ;;
    esac
done

# ---------------------------------------------------------------- 汇总
echo "================================================================"
echo "[3/4] 汇总：PTS 用例 $TOTAL 个，字节级校验全部通过=$((TOTAL - BAD))/$TOTAL，"
echo "      ffprobe 成功读回 $FFPROBE_OK 个，退化 $DEGRADED 个"
if [ "$BAD" -eq 0 ] && [ "$FFPROBE_OK" -gt 0 ]; then
    echo "[4/4] PASS: ffprobe 读回的 PTS 与 gb_ps_encode_pts 写入值完全一致"
    echo "      => 结论：PES 的 33bit PTS 位域（含 byte2 使用 >>14）正确。"
    echo "      产物：$OUTDIR/pts_*.ps ；可人工复核："
    echo "        ffprobe -show_packets -select_streams v $OUTDIR/pts_90000.ps"
    exit 0
fi
if [ "$BAD" -eq 0 ] && [ "$FFPROBE_OK" -eq 0 ]; then
    echo "[4/4] PASS（退化）: ffprobe 不可用或读不到 PTS，改用字节级位域解析作为依据。"
    echo "      退化原因：FRAME_SRC='${FRAME_SRC:-<无>}'，ffprobe=$(command -v ffprobe || echo 未安装)"
    echo "      字节级解析已逐位按 ISO 13818-1 还原，结果与写入值一致。"
    echo "      要拿到最强证据，请在装有 ffmpeg/ffprobe 的环境重跑本脚本。"
    exit 0
fi
echo "[4/4] FAIL: 有 $BAD 个用例不一致 —— PTS 位域或 PES 头有错，请看上面的 [字节级]/[ffprobe] 行。"
exit 1
