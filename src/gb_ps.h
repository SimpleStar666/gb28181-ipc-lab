/*
 * gb_ps.h — MPEG-PS（Program Stream）打包器
 * ============================================================
 * 这是整个项目最核心的一块，也是 GB28181 面试最爱问的一块。
 *
 * 【为什么国标非用 PS 不可？】
 *   RTP 本身只负责「送包」，不负责描述「这一坨字节是什么」。
 *   国标要在一条 RTP 流里同时传视频和音频，并且让两者共享同一个时间基，
 *   于是选了 MPEG-2 Program Stream 当容器：
 *     - PS 里的 PES 带 33bit PTS，音视频用同一个 90kHz 时钟；
 *     - 于是接收端只要解析 PS，就天然拿到了音画同步的依据，
 *       不需要像 WebRTC 那样再靠 RTCP SR 做 NTP↔RTP 映射。
 *   这条「为什么」就是面试时区分「背过」和「做过」的分水岭。
 *
 * 【PS 的四层结构】（每帧都要重新打一遍前 3 层）
 *   1. Pack Header     00 00 01 BA  + 14 字节
 *   2. System Header   00 00 01 BB  + 16 字节
 *   3. PSM             00 00 01 BC  + 20 字节（单视频）/ 24 字节（含音频）
 *   4. PES             00 00 01 E0  + 14 字节头 + 载荷
 *
 * 【PSM 的 length 字段到底算哪些字节？】（面试高频追问）
 *   packet_length 从「current_next_indicator」开始数到 CRC_32 结束，
 *   也就是它**不包含**前面的 4 字节起始码 00 00 01 BC，
 *   也**不包含**它自己这 2 字节。
 *   单视频时 = 14（0x000E），含音频时 = 18（0x0012）。
 *   验证方式：数一遍字节 + 用 ffprobe 读回 PTS，见 tools/verify_pts.sh。
 *
 * 【PTS 的 5 字节编码】
 *   MPEG-PS 的 PTS 是 33 位，但铺开到 5 个字节里，每个字节都有标记位：
 *     byte0 = 0010 P32 P31 P30 1     -> 0x21 | ((pts >> 29) & 0x0E)
 *     byte1 = P29..P22               -> (pts >> 22) & 0xFF
 *     byte2 = P21..P15 1             -> 0x01 | ((pts >> 14) & 0xFE)   <-- 是 14，不是 15！
 *     byte3 = P14..P7                -> (pts >> 7)  & 0xFF
 *     byte4 = P6..P0 1               -> 0x01 | ((pts & 0x7F) << 1)
 *   byte2 那一处「>>14」是经典错误点：写成 >>15 会整体错位一位，
 *   而错位之后画面**照样能解码出来**，所以极难发现。
 *   本项目的单元测试用 pts=90000 断言 byte2 == 0x05（错写会得到 0x03）。
 */
#ifndef GB_PS_H
#define GB_PS_H

#include <stddef.h>
#include <stdint.h>
#include "gb_common.h"

/* 单帧 PS 输出上限。1080p 的 I 帧可能上百 KB，所以给足。 */
#define GB_PS_MAX_PACKET  (512 * 1024)

/* 固定的头部字节（教学与单测都直接引用这些常量） ---- */

/* Pack Header：14 字节（4 + 6 + 4）
 *   00 00 01 BA               起始码（4 字节）
 *   44 00 04 00 04 01         '01' 标志(2位) + SCR(33位 + 3个 marker) —— 6 字节
 *   86 66 CF F8               program_mux_rate + reserved + pack_stuffing_length
 *
 * 注意 0x44 不是独立字节，它是那 6 字节的第一个；
 * 最高两位 '01' 表示 MPEG-2 PS（PES 按 MPEG-2 规则解析）。
 * 写成 0x21（'00'）就是 MPEG-1，接收端会换一套 PES 解析规则 —— 这是常见坑。 */
extern const uint8_t GB_PS_PACK_HEADER[14];

/* System Header：16 字节。
 *   00 00 01 BB               起始码
 *   00 09                     header_length = 9
 *   C3 33 67 00 21 FF E0 E0 E6   固定域 6 字节 + 1 条视频流条目 3 字节 = 9 ✓
 *   00                        ★ 多出来的 1 个填充字节
 *
 * 【关于第 16 个字节 —— 一个"知道但不改"的 quirk】
 *   严格按 ISO 13818-1，System Header 总长应是 15 字节（4 起始码 + 2 长度字段 + 9），
 *   但原实现给的是 16 字节，最后多一个 0x00。
 *   它无害，因为解析器不依赖长度字段走到下一段，而是扫描 00 00 01 起始码重新同步
 *   （读满 9 字节后多出一个 0x00，继续往下扫就撞上 PSM 的 00 00 01 BC）。
 *
 *   本项目保留 16 字节：它是已实测通过 ZLMediaKit + wvp-pro 全链路的字节序列，
 *   改动收益为零、风险非零。这个 quirk 在 docs/06 里作为教学点展开 ——
 *   「多一个字节不报错、少一个字节也不报错，现象上无法区分，唯一可靠的办法是逐字节数」。 */
extern const uint8_t GB_PS_SYSTEM_HEADER[16];

/* PSM：单视频流，20 字节。stream_type=0x1B(H.264)，elementary_stream_id=0xE0 */
extern const uint8_t GB_PS_PSM_VIDEO[20];

/* PSM：视频 + G.711A 音频，24 字节。
 * 多一条 stream_type=0x90(G.711A)，elementary_stream_id=0xC0 */
extern const uint8_t GB_PS_PSM_AV[24];

/* PES 的 stream_id 常量 */
#define GB_PS_SID_VIDEO  0xE0u
#define GB_PS_SID_AUDIO  0xC0u
/* PSM 里的 stream_type 常量 */
#define GB_PS_ST_H264    0x1Bu
#define GB_PS_ST_G711A   0x90u

/* ---- 纯函数接口 ---- */

/* 33bit PTS → 5 字节。见文件头的位域表。 */
void gb_ps_encode_pts(uint8_t out[5], uint64_t pts90k);

/* 写 PES 头（14 字节）到 out。
 *   sid        : GB_PS_SID_VIDEO / GB_PS_SID_AUDIO
 *   payload_len: 该 PES 的载荷总字节数
 *   pts90k     : 显示时间戳
 *   idr        : 为接口兼容而保留；当前实现恒写 flags = 0x80 0x80 0x05
 * 返回写入字节数（固定 14）。
 *
 * 【关于 idr 与 data_alignment_indicator —— 一个"知道但选择不改"的取舍】
 *   flags1（第 7 个字节）的 bit2 是 data_alignment_indicator，置 1 表示
 *   "这个 PES 从一个访问单元边界开始"。ZLMediaKit 的 mpeg-ps-dec.c 里
 *   正是用它判定 IDR：
 *       pes->flags = pes->data_alignment_indicator ? MPEG_FLAG_IDR_FRAME : 0;
 *   所以语义上 idr=1 时应当写 0x84 而不是 0x80。
 *
 *   本项目沿用恒写 0x80，原因有两条：
 *     1) 该字节序列已在 ZLMediaKit + wvp-pro 全链路上实测通过；
 *     2) 下游还有"H.264 NAL 类型"这条独立路径判定 IDR（类型 5 = IDR slice），
 *        不依赖这个标志位。
 *   保留 idr 参数是为了接口稳定，并把取舍写在注释里 ——
 *   "知道正确做法，也知道当前为什么不必改"，比默默忽略它更有价值。 */
int gb_ps_write_pes_header(uint8_t out[14], uint8_t sid, size_t payload_len,
                           uint64_t pts90k, int idr);

/* 打一帧视频成 PS。
 *   out/cap  : 输出缓冲
 *   frame    : 该帧的 AnnexB 数据（SPS/PPS/SEI 在前，VCL NAL 在后，已按顺序拼好）
 *   frame_len: 该帧总字节数
 *   pts90k   : 该帧 PTS
 *   with_audio 通过 psm 选择：传 GB_PS_PSM_VIDEO 或 GB_PS_PSM_AV
 * 返回写入字节数；缓冲不够返回 GB_ERR。 */
int gb_ps_pack_video(uint8_t *out, size_t cap,
                     const uint8_t *frame, size_t frame_len,
                     uint64_t pts90k, const uint8_t *psm, size_t psm_len);

/* 打一帧音频（G.711A）成 PS：Pack Header + PES(audio)。
 * 注意音频包**不带** System Header 和 PSM —— 它们每帧打一次会浪费带宽，
 * 惯例是每个视频关键帧带上即可。
 * 返回写入字节数；缓冲不够返回 GB_ERR。 */
int gb_ps_pack_audio(uint8_t *out, size_t cap,
                     const uint8_t *pcm_a, size_t len,
                     uint64_t pts90k);

#endif /* GB_PS_H */
