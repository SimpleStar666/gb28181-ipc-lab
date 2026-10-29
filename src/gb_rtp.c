/*
 * gb_rtp.c — RTP 封装（RFC 3550）
 * ============================================================
 * 三个必须记住的细节（对应三条硬性不变量，测试逐字节断言）：
 *   1. rtp[0] = 0x80（V=2, P=0, X=0, CC=0）
 *   2. rtp[1] = pt | (last ? 0x80 : 0)   —— M 位在第 1 字节最高位。
 *      写成 `pt | 1` 是经典 bug：PT 会从 96 变成 97，接收端直接认不出码流。
 *   3. 同一帧切出来的所有 RTP 包，timestamp 必须完全相同，只有最后一个包置 M 位。
 *
 * 【RTP 时间戳从哪来】
 *   gb_rtp_send_ps() 的签名里没有 ts 参数（头文件已冻结），
 *   所以这里从 PS 流里取出第一个 PES 的 PTS 当 RTP 时间戳 ——
 *   这恰恰就是国标的做法：RTP 头 32bit 时间戳 = PES 里 33bit PTS 的低 32 位。
 *   好处是「PS 和 RTP 的时间基天然一致」，接收端不需要额外的时钟映射。
 */
#include "gb_rtp.h"

#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

void gb_rtp_tx_init(gb_rtp_tx_t *tx, uint8_t pt, size_t max_payload,
                    uint32_t ssrc, uint16_t seq0)
{
    if (!tx) return;
    tx->seq = seq0;
    tx->ssrc = ssrc;
    tx->pt = pt;
    tx->max_payload = max_payload;
}

void gb_rtp_build_header(uint8_t hdr[GB_RTP_HEADER_LEN],
                         uint16_t seq, uint32_t ts, uint32_t ssrc,
                         uint8_t pt, int marker)
{
    if (!hdr) return;

    hdr[0] = 0x80;                                        /* V=2,P=0,X=0,CC=0 */
    hdr[1] = (uint8_t)((pt & 0x7F) | (marker ? 0x80 : 0x00));
    hdr[2] = (uint8_t)((seq >> 8) & 0xFF);
    hdr[3] = (uint8_t)(seq & 0xFF);
    hdr[4] = (uint8_t)((ts >> 24) & 0xFF);
    hdr[5] = (uint8_t)((ts >> 16) & 0xFF);
    hdr[6] = (uint8_t)((ts >> 8) & 0xFF);
    hdr[7] = (uint8_t)(ts & 0xFF);
    hdr[8]  = (uint8_t)((ssrc >> 24) & 0xFF);
    hdr[9]  = (uint8_t)((ssrc >> 16) & 0xFF);
    hdr[10] = (uint8_t)((ssrc >> 8) & 0xFF);
    hdr[11] = (uint8_t)(ssrc & 0xFF);
}

int gb_rtp_slice(const uint8_t *ps, size_t ps_len, size_t max_payload,
                 gb_rtp_slice_t slices[], int max_slices)
{
    size_t off = 0;
    int n = 0;

    if (!ps || !slices || max_slices <= 0 || max_payload == 0 || ps_len == 0) return GB_ERR;

    while (off < ps_len) {
        size_t piece = ps_len - off;
        int last;

        if (piece > max_payload) piece = max_payload;
        if (n >= max_slices) return GB_ERR;   /* 切片太多：说明 MTU 配太小或帧太大 */

        last = (off + piece >= ps_len);
        slices[n].payload = ps + off;
        slices[n].len = piece;
        slices[n].marker = last;
        off += piece;
        n++;
    }
    return n;
}

size_t gb_rtp_pack(uint8_t *out, size_t out_cap,
                   uint16_t seq, uint32_t ts, uint32_t ssrc,
                   uint8_t pt, int marker,
                   const uint8_t *payload, size_t payload_len)
{
    if (!out) return 0;
    if (!payload && payload_len) return 0;
    if (out_cap < (size_t)GB_RTP_HEADER_LEN + payload_len) return 0;

    gb_rtp_build_header(out, seq, ts, ssrc, pt, marker);
    if (payload_len) memcpy(out + GB_RTP_HEADER_LEN, payload, payload_len);
    return (size_t)GB_RTP_HEADER_LEN + payload_len;
}

/* 从 PS 里取出第一个 PES 的 PTS（33bit -> 低 32 位）。
 * 按 PS 结构前进（不裸扫 00 00 01），否则 H.264 载荷里恰好出现的
 * 00 00 01 E0 会被误判成 PES 头。 */
static int ps_first_pes_pts(const uint8_t *ps, size_t len, uint32_t *out)
{
    size_t i = 0;

    if (!ps || !out) return GB_ERR;

    while (i + 4 <= len) {
        uint8_t sid;
        size_t skip = 0;

        if (ps[i] != 0x00 || ps[i + 1] != 0x00 || ps[i + 2] != 0x01) {
            i++;
            continue;
        }
        sid = ps[i + 3];

        if (sid == 0xBA) {                 /* Pack Header：固定 14 字节 */
            skip = 14;
        } else if (sid == 0xBB) {          /* System Header：固定 16 字节 */
            skip = 16;
        } else if (sid == 0xBC) {          /* PSM：6 字节头 + packet_length */
            size_t plen;
            if (i + 6 > len) return GB_ERR;
            plen = ((size_t)ps[i + 4] << 8) | (size_t)ps[i + 5];
            skip = 6 + plen;
        } else if (sid >= 0xC0 && sid <= 0xEF) {
            const uint8_t *p;
            uint64_t pts;
            if (i + 14 > len) return GB_ERR;
            if ((ps[i + 7] & 0x80) == 0) return GB_ERR;   /* 这一段 PES 没带 PTS */
            p = ps + i + 9;
            /* 与 gb_ps_encode_pts 严格互逆的还原 */
            pts = ((uint64_t)(p[0] & 0x0E) << 29) |
                  ((uint64_t)p[1] << 22) |
                  ((uint64_t)(p[2] & 0xFE) << 14) |
                  ((uint64_t)p[3] << 7) |
                  ((uint64_t)(p[4] & 0xFE) >> 1);
            *out = (uint32_t)(pts & 0xFFFFFFFFu);
            return GB_OK;
        } else {
            i++;
            continue;
        }

        if (skip == 0 || i + skip > len) return GB_ERR;
        i += skip;
    }
    return GB_ERR;
}

int gb_rtp_send_ps(int fd, const void *dst, unsigned dst_len,
                   gb_rtp_tx_t *tx, const uint8_t *ps, size_t ps_len)
{
    gb_rtp_slice_t slices[GB_RTP_MAX_SLICES];
    uint8_t hdr[GB_RTP_HEADER_LEN];
    struct iovec iov[2];
    struct msghdr mh;
    uint32_t ts = 0;
    int n, i, sent = 0;

    if (fd < 0 || !dst || !tx || !ps || ps_len == 0) return GB_ERR;

    if (ps_first_pes_pts(ps, ps_len, &ts) != GB_OK) {
        ts = 0;   /* 兜底：PS 里没有任何 PES 才可能走到这里 */
    }

    n = gb_rtp_slice(ps, ps_len, tx->max_payload, slices, GB_RTP_MAX_SLICES);
    if (n <= 0) return GB_ERR;

    for (i = 0; i < n; i++) {
        /* 每个分片都是同样的 ts/ssrc/pt，只有 M 位不同 —— 见文件头第 3 点 */
        gb_rtp_build_header(hdr, tx->seq, ts, tx->ssrc, tx->pt, slices[i].marker);

        iov[0].iov_base = hdr;
        iov[0].iov_len = sizeof(hdr);
        iov[1].iov_base = (void *)(uintptr_t)slices[i].payload;
        iov[1].iov_len = slices[i].len;

        memset(&mh, 0, sizeof(mh));
        mh.msg_name = (void *)(uintptr_t)dst;
        mh.msg_namelen = dst_len;
        mh.msg_iov = iov;
        mh.msg_iovlen = 2;

        /* 用 sendmsg + iovec 而不是先 memcpy 到一个大缓冲：
         * 头 12 字节和载荷在内存里本就不连续，一次拷贝都不做，
         * 也就没有「payload 比栈缓冲大」的隐患。 */
        if (sendmsg(fd, &mh, 0) < 0) return GB_ERR;

        tx->seq = (uint16_t)(tx->seq + 1);
        sent++;
    }
    return sent;
}
