/*
 * gb_annexb.h — H.264 AnnexB 码流切分
 * ============================================================
 * H.264 裸流（AnnexB 格式）用起始码分隔 NAL：
 *   3 字节：00 00 01
 *   4 字节：00 00 00 01
 * 两种都要支持，因为编码器（x264）会在 SPS/PPS/关键帧前用 4 字节，其余用 3 字节。
 *
 * NAL 的类型在「起始码之后第一个字节」的低 5 位：
 *   1  = 非 IDR 的 slice（P 帧）
 *   5  = IDR slice（I 帧）
 *   6  = SEI
 *   7  = SPS
 *   8  = PPS
 *   9  = AUD（访问单元分隔符）
 *
 * 【为什么不能直接把文件读进来一次性 split】
 *   实时流是从 FIFO 一段一段读的，一段的结尾很可能把某个 NAL 切成两半。
 *   必须「保留尾部残留、等下一段数据到了再拼」——这就是本书说的「流式切帧」。
 *   反面教材见 src/gb_media.c 里对 pos / stream_used 的处理。
 */
#ifndef GB_ANNEXB_H
#define GB_ANNEXB_H

#include <stddef.h>
#include <stdint.h>

/* 从 d[from] 开始找下一个起始码。
 * 找到：返回起始码偏移，*sc_len = 3 或 4。
 * 没找到：返回 -1。
 *
 * 注意 4 字节起始码 00 00 00 01 必须优先于 3 字节匹配，
 * 否则会在错误的位置切成 00 00 01，把 NAL 头多留一个 0x00。 */
int gb_annexb_find_startcode(const uint8_t *d, size_t len, size_t from, size_t *sc_len);

/* nal 指向起始码，sc_len 是起始码长度。
 * 返回 NAL 类型（0..31）；数据不足返回 -1。 */
int gb_annexb_nal_type(const uint8_t *nal, size_t sc_len);

/* 是否 IDR（关键帧） */
int gb_annexb_is_idr(int nal_type);

/* 是否 VCL（真正的图像数据：1..5），可以据此判断「一帧结束」 */
int gb_annexb_is_vcl(int nal_type);

#endif /* GB_ANNEXB_H */
