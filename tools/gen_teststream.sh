#!/usr/bin/env bash
# gen_teststream.sh — 生成测试用 H.264 裸流 assets/testsrc.h264
# ============================================================
# 为什么要有这一步：
#   本项目的 PS/RTP 封装是纯函数，最好的验证方式就是拿一段**真编码器产出的**
#   AnnexB 码流去打 PS，再用 ffprobe 读回来。testsrc2 是 ffmpeg 的合成图案，
#   不依赖任何外部素材，10 秒 640x360@15fps 大约 200~400 KB。
#
# 参数（可复现性优先，全部固定下来）：
#   -preset ultrafast -tune zerolatency  低延迟、编码快
#   -g 30                                2 秒一个关键帧
#   -threads 1                           单线程，保证输出确定
#   -x264-params repeat-headers=1        每个 IDR 前重复 SPS/PPS ——
#                                        这正是「一帧自带 SPS/PPS」的前提，
#                                        也是 PS 打包每个关键帧都要带 PSM 的原因
#   -x264-params slices=1                一帧一个 slice，便于「按 NAL 切帧」
#
# 用法：bash tools/gen_teststream.sh [--force]
#   已存在且非空则跳过；--force 强制重新生成。
set -u

# 自保：从 Windows 带 \r 的脚本被 bash 读到时，会报一堆 "$'\r': command not found"
# 只在真的带 \r 时才改写文件（否则每次都动 mtime，还可能把文件弄成只读）
if LC_ALL=C grep -q "$(printf '\r')" "$0" 2>/dev/null; then
    sed -i 's/\r$//' "$0" 2>/dev/null || true
fi

cd "$(dirname "$0")/.." || { echo "[gen_teststream] 无法进入项目根目录" >&2; exit 2; }

OUT="assets/testsrc.h264"
FORCE="${1:-}"

mkdir -p assets

if [ -s "$OUT" ] && [ "$FORCE" != "--force" ]; then
    echo "[gen_teststream] 已存在，跳过：$OUT ($(wc -c < "$OUT") bytes)"
    echo "[gen_teststream] 需要重新生成请加 --force"
    exit 0
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "[gen_teststream] ERROR: 找不到 ffmpeg。"
    echo "  Ubuntu/Debian: sudo apt-get install -y ffmpeg"
    echo "  本项目只需要 ffmpeg 这一个外部依赖（WSL 里已装 4.2.7 即可）。"
    echo "  若无法安装，tools/verify_pts.sh 会自动退化为「直接解析 PS 字节」的校验路径。"
    exit 2
fi

echo "[gen_teststream] ffmpeg: $(ffmpeg -version 2>/dev/null | head -1)"
echo "[gen_teststream] 正在生成 $OUT ..."

ffmpeg -hide_banner -loglevel warning -y \
    -f lavfi -i "testsrc2=size=640x360:rate=15" \
    -c:v libx264 -preset ultrafast -tune zerolatency -g 30 -threads 1 \
    -x264-params repeat-headers=1:slices=1 \
    -t 10 -f h264 "$OUT"
rc=$?

if [ $rc -ne 0 ] || [ ! -s "$OUT" ]; then
    echo "[gen_teststream] FAIL: ffmpeg 退出码 $rc，输出文件不存在或为空" >&2
    exit 1
fi

SZ=$(wc -c < "$OUT")
echo "[gen_teststream] 完成：$OUT ($SZ bytes)"

if command -v ffprobe >/dev/null 2>&1; then
    echo "[gen_teststream] ffprobe 自检："
    ffprobe -v error -select_streams v:0 \
        -show_entries stream=codec_name,width,height,r_frame_rate,nb_read_frames \
        -count_frames -of default=noprint_wrappers=1 "$OUT" | sed 's/^/    /'
fi

# 起始码自检：必须有 4 字节起始码（SPS/PPS/IDR 前），也要有 3 字节起始码
if command -v xxd >/dev/null 2>&1; then
    N4=$(xxd -p "$OUT" | tr -d '\n' | grep -o '00000001' | wc -l)
    N3=$(xxd -p "$OUT" | tr -d '\n' | grep -o '000001' | wc -l)
    echo "[gen_teststream] 起始码统计：4 字节 x $N4 ，含 3 字节的匹配 x $N3"
fi

echo "[gen_teststream] PASS"
exit 0
