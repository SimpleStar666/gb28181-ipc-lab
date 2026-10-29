/*
 * test_rtp.c — RTP 头（RFC 3550）+ PS 分片
 * ============================================================
 * 这一层有两个「一错就整条流作废」的点，都在这里钉死：
 *
 *   1. marker 位在第 1 字节的**最高位**：
 *        byte1 = pt | 0x80   （最后一片）
 *        byte1 = pt         （中间片）
 *      写成 `pt | 1` 会把 PT 变成 97，接收端直接认不出码流 ——
 *      现象是「SIP 通了、RTP 也在发，但平台就是不出画面」。
 *
 *   2. 同一帧切出来的所有 RTP 包 timestamp 必须**完全相同**，
 *      只有最后一片置 marker。接收端靠这个判断帧边界。
 *
 * 另外顺带核对：第 0 字节 == 0x80、seq/timestamp/ssrc 一律大端。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "gb_rtp.h"
#include "gb_common.h"

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL: ");                                               \
            printf(__VA_ARGS__);                                            \
            fflush(stdout);                                                 \
            return 1;                                                       \
        }                                                                   \
    } while (0)

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

int main(void)
{
    uint8_t hdr[GB_RTP_HEADER_LEN];
    static uint8_t ps[3000];
    static uint8_t pkt[4096];
    gb_rtp_slice_t slices[GB_RTP_MAX_SLICES];
    gb_rtp_tx_t tx;
    size_t i;
    int n;
    const uint16_t seq = 0x1234;
    const uint32_t ts = 0x01020304u;
    const uint32_t ssrc = 0xA0B0C0D0u;
    const uint8_t pt = GB_RTP_PT_PS_DYNAMIC;   /* 96 */

    for (i = 0; i < sizeof(ps); i++)
        ps[i] = (uint8_t)(i * 31 + 7);

    /* ================= 1. RTP 头 12 字节 ================= */
    gb_rtp_build_header(hdr, seq, ts, ssrc, pt, 0);
    CHECK(hdr[0] == 0x80, "RTP byte0 = 0x%02X, expected 0x80 (V=2,P=0,X=0,CC=0)\n", hdr[0]);
    CHECK(hdr[1] == pt, "marker=0 时 byte1 = 0x%02X, expected PT 0x%02X\n", hdr[1], pt);
    CHECK(be16(hdr + 2) == seq, "seq = 0x%04X, expected 0x%04X（大端）\n", be16(hdr + 2), seq);
    CHECK(be32(hdr + 4) == ts, "timestamp = 0x%08X, expected 0x%08X（大端）\n", be32(hdr + 4), ts);
    CHECK(be32(hdr + 8) == ssrc, "ssrc = 0x%08X, expected 0x%08X（大端）\n", be32(hdr + 8), ssrc);

    gb_rtp_build_header(hdr, seq, ts, ssrc, pt, 1);
    CHECK(hdr[1] == (uint8_t)(pt | 0x80),
          "marker=1 时 byte1 = 0x%02X, expected 0x%02X"
          "   <- 常见错误是 pt|1（会把 PT 变成 0x61）\n",
          hdr[1], (uint8_t)(pt | 0x80));
    CHECK((hdr[1] & 0x7F) == pt,
          "marker=1 时 byte1 的低 7 位 = %u, expected PT %u\n", hdr[1] & 0x7F, pt);

    /* 音频 PT=8（PCMA）：同样只动最高位 */
    gb_rtp_build_header(hdr, 1, 1800, ssrc, 8, 0);
    CHECK(hdr[1] == 0x08, "PT=8 marker=0 时 byte1 = 0x%02X, expected 0x08\n", hdr[1]);
    gb_rtp_build_header(hdr, 1, 1800, ssrc, 8, 1);
    CHECK(hdr[1] == 0x88, "PT=8 marker=1 时 byte1 = 0x%02X, expected 0x88\n", hdr[1]);

    /* 大端再单独验一组「一眼能看出字节序」的值 */
    gb_rtp_build_header(hdr, 0x0102, 0xDEADBEEF, 0x01020304, 96, 0);
    CHECK(hdr[2] == 0x01 && hdr[3] == 0x02, "seq 大端错: %02X %02X (want 01 02)\n", hdr[2], hdr[3]);
    CHECK(hdr[4] == 0xDE && hdr[5] == 0xAD && hdr[6] == 0xBE && hdr[7] == 0xEF,
          "timestamp 大端错: %02X %02X %02X %02X (want DE AD BE EF)\n",
          hdr[4], hdr[5], hdr[6], hdr[7]);
    CHECK(hdr[8] == 0x01 && hdr[9] == 0x02 && hdr[10] == 0x03 && hdr[11] == 0x04,
          "ssrc 大端错: %02X %02X %02X %02X (want 01 02 03 04)\n",
          hdr[8], hdr[9], hdr[10], hdr[11]);

    /* ================= 2. 分片：3000 字节 / MTU 1400 → 3 片 ================= */
    n = gb_rtp_slice(ps, sizeof(ps), 1400, slices, GB_RTP_MAX_SLICES);
    CHECK(n == 3, "3000 字节 MTU=1400 应切 3 片, got %d\n", n);
    CHECK(slices[0].len == 1400 && slices[1].len == 1400 && slices[2].len == 200,
          "分片长度 = %zu/%zu/%zu, expected 1400/1400/200\n",
          slices[0].len, slices[1].len, slices[2].len);
    CHECK(slices[0].marker == 0 && slices[1].marker == 0 && slices[2].marker == 1,
          "marker 标志 = %d/%d/%d, expected 只有最后一片为 1\n",
          slices[0].marker, slices[1].marker, slices[2].marker);
    CHECK(slices[0].payload == ps && slices[1].payload == ps + 1400 &&
          slices[2].payload == ps + 2800,
          "分片指针没有按顺序指向原缓冲\n");
    CHECK(slices[0].payload[0] == ps[0] && slices[2].payload[0] == ps[2800],
          "分片内容与原数据不对应\n");

    /* 恰好整除：2800 / 1400 → 2 片，最后一片仍有 marker */
    n = gb_rtp_slice(ps, 2800, 1400, slices, GB_RTP_MAX_SLICES);
    CHECK(n == 2, "2800 字节 MTU=1400 应切 2 片（整除）, got %d\n", n);
    CHECK(slices[0].marker == 0 && slices[1].marker == 1,
          "整除时 marker = %d/%d, expected 0/1\n", slices[0].marker, slices[1].marker);

    /* 小载荷：单片也要置 marker */
    n = gb_rtp_slice(ps, 100, 1400, slices, GB_RTP_MAX_SLICES);
    CHECK(n == 1, "100 字节应切 1 片, got %d\n", n);
    CHECK(slices[0].len == 100 && slices[0].marker == 1,
          "单片: len=%zu marker=%d, expected 100/1\n", slices[0].len, slices[0].marker);

    n = gb_rtp_slice(ps, 1, 1400, slices, GB_RTP_MAX_SLICES);
    CHECK(n == 1 && slices[0].len == 1 && slices[0].marker == 1,
          "1 字节边界: n=%d len=%zu marker=%d\n", n, slices[0].len, slices[0].marker);

    /* 切片数超过 max_slices 必须报错，不能越界写数组 */
    n = gb_rtp_slice(ps, 3000, 1400, slices, 2);
    CHECK(n == GB_ERR, "max_slices=2 装不下 3 片时应返回 GB_ERR, got %d\n", n);

    /* ================= 3. gb_rtp_pack：12 + 载荷 ================= */
    {
        size_t total;
        total = gb_rtp_pack(pkt, sizeof(pkt), seq, ts, ssrc, pt, 0, ps, 1400);
        CHECK(total == 12 + 1400, "gb_rtp_pack 返回 %zu, expected %zu\n", total, (size_t)(12 + 1400));
        gb_rtp_build_header(hdr, seq, ts, ssrc, pt, 0);
        CHECK(memcmp(pkt, hdr, 12) == 0, "gb_rtp_pack 的头 12 字节与 gb_rtp_build_header 不一致\n");
        CHECK(memcmp(pkt + 12, ps, 1400) == 0, "gb_rtp_pack 的载荷与输入不一致\n");

        total = gb_rtp_pack(pkt, sizeof(pkt), seq, ts, ssrc, pt, 1, ps, 200);
        CHECK(total == 12 + 200, "小载荷 gb_rtp_pack 返回 %zu, expected 212\n", total);
        CHECK(pkt[1] == (uint8_t)(pt | 0x80), "gb_rtp_pack marker=1 时 byte1 = 0x%02X\n", pkt[1]);

        /* 容量不足返回 0，且不越界 */
        memset(pkt, 0xEE, 32);
        total = gb_rtp_pack(pkt, 12, seq, ts, ssrc, pt, 0, ps, 1400);
        CHECK(total == 0, "容量不足时 gb_rtp_pack 返回 %zu, expected 0\n", total);
    }

    /* ================= 4. 模拟整帧发送：同一 timestamp、递增 seq ================= */
    n = gb_rtp_slice(ps, sizeof(ps), 1400, slices, GB_RTP_MAX_SLICES);
    CHECK(n == 3, "重新分片失败 (%d)\n", n);
    {
        uint16_t s = 0x00FE;
        uint32_t seen_ts[4];
        int k;
        for (k = 0; k < n; k++) {
            size_t total = gb_rtp_pack(pkt, sizeof(pkt), s, ts, ssrc, pt,
                                       slices[k].marker, slices[k].payload, slices[k].len);
            CHECK(total == 12 + slices[k].len,
                  "第 %d 片 RTP 包长度 = %zu, expected %zu\n", k, total, 12 + slices[k].len);
            CHECK(pkt[0] == 0x80 && (pkt[1] & 0x7F) == pt,
                  "第 %d 片头部字段错: byte0=0x%02X byte1=0x%02X\n", k, pkt[0], pkt[1]);
            CHECK(be16(pkt + 2) == (uint16_t)(0x00FE + k),
                  "第 %d 片 seq = 0x%04X, expected 0x%04X\n", k, be16(pkt + 2),
                  (uint16_t)(0x00FE + k));
            seen_ts[k] = be32(pkt + 4);
            s++;
        }
        for (k = 1; k < n; k++)
            CHECK(seen_ts[k] == seen_ts[0],
                  "第 %d 片的 timestamp = %u, 与第 0 片 %u 不同"
                  "   <- 同一帧的所有分片必须共享同一个时间戳\n",
                  k, seen_ts[k], seen_ts[0]);
        CHECK(seen_ts[0] == ts, "RTP timestamp = %u, expected 帧 PTS %u\n", seen_ts[0], ts);
    }

    /* ================= 5. gb_rtp_tx_init 保存的参数 ================= */
    gb_rtp_tx_init(&tx, pt, 1400, ssrc, 0x00FE);
    CHECK(tx.pt == pt, "tx.pt = %u, expected %u\n", tx.pt, pt);
    CHECK(tx.max_payload == 1400, "tx.max_payload = %zu, expected 1400\n", tx.max_payload);
    CHECK(tx.ssrc == ssrc, "tx.ssrc = 0x%08X, expected 0x%08X\n", tx.ssrc, ssrc);
    CHECK(tx.seq == 0x00FE, "tx.seq = 0x%04X, expected 0x00FE\n", tx.seq);

    printf("PASS: RTP 头 0x80/PT%u/marker 最高位/大端 seq·ts·ssrc 正确；"
           "3000B@MTU1400 → 3 片且仅末片 marker，同帧 timestamp 一致；"
           "gb_rtp_pack 总长 = 12 + 分片长\n", pt);
    return 0;
}
