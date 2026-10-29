/*
 * gb_rtp.h — RTP 封装（RFC 3550）
 * ============================================================
 * RTP 头固定 12 字节：
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |V=2|P|X|  CC   |M|     PT      |       sequence number         |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                           timestamp                           |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                             SSRC                              |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * 【三个必须记住的细节】
 *   1. 第 0 字节 = 0x80（V=2, P=0, X=0, CC=0）。
 *   2. M（marker）位在第 1 字节的最高位。
 *      所以是 `pt | (last ? 0x80 : 0)`，而**不是** `pt | 1` ——
 *      写成 `| 1` 会变成「PT 变成 97」，接收端直接认不出码流。
 *   3. timestamp 在 90kHz 下就是该帧的 PTS。
 *      同一帧切出来的多个 RTP 包，timestamp 必须**完全相同**；
 *      只有 M 位标记最后一个包，接收端据此判断一帧结束。
 *
 * 【协议开销】每个包 20(IP) + 8(UDP) + 12(RTP) = 40 字节。
 *   MTU 1400 的载荷 → 实际发 1440 字节，不超 1500，安全。
 */
#ifndef GB_RTP_H
#define GB_RTP_H

#include <stddef.h>
#include <stdint.h>
#include "gb_common.h"

#define GB_RTP_HEADER_LEN 12
#define GB_RTP_MAX_SLICES 64

/* GB28181 的 PS over RTP 使用动态载荷类型 96（>=96 必须在 SDP 里用 a=rtpmap 声明） */
#define GB_RTP_PT_PS_DYNAMIC 96

typedef struct {
    uint16_t seq;          /* 下一个要用的序号（自动 +1 并回绕） */
    uint32_t ssrc;         /* 同步源标识，由平台在 SDP 的 y= 行指定 */
    uint8_t  pt;           /* 载荷类型 */
    size_t   max_payload;  /* 单个 RTP 包的载荷上限（= MTU） */
} gb_rtp_tx_t;

/* 一个 RTP 包的载荷切片（不含 12 字节头） */
typedef struct {
    const uint8_t *payload;
    size_t         len;
    int            marker;
} gb_rtp_slice_t;

void gb_rtp_tx_init(gb_rtp_tx_t *tx, uint8_t pt, size_t max_payload,
                    uint32_t ssrc, uint16_t seq0);

/* 构造 12 字节 RTP 头。marker != 0 时置 M 位。 */
void gb_rtp_build_header(uint8_t hdr[GB_RTP_HEADER_LEN],
                         uint16_t seq, uint32_t ts, uint32_t ssrc,
                         uint8_t pt, int marker);

/* 把一段 PS 数据按 max_payload 切成若干载荷切片（不含 RTP 头）。
 * 返回切片数（>=1）；超过 max_slices 返回 GB_ERR。
 * 纯函数：不推进 seq、不发送。 */
int gb_rtp_slice(const uint8_t *ps, size_t ps_len, size_t max_payload,
                 gb_rtp_slice_t slices[], int max_slices);

/* 组装一个完整 RTP 包（12 字节头 + 载荷）到 out。
 * 返回总字节数；容量不足返回 0。纯函数。 */
size_t gb_rtp_pack(uint8_t *out, size_t out_cap,
                   uint16_t seq, uint32_t ts, uint32_t ssrc,
                   uint8_t pt, int marker,
                   const uint8_t *payload, size_t payload_len);

/* 高层封装：把一帧 PS 拆包并 sendto 到 dst。
 * 内部推进 tx->seq。返回成功发出的包数；出错返回 GB_ERR。
 * fd 必须是已 connect/bind 的 UDP socket。 */
int gb_rtp_send_ps(int fd, const void *dst, unsigned dst_len,
                   gb_rtp_tx_t *tx, const uint8_t *ps, size_t ps_len);

#endif /* GB_RTP_H */
