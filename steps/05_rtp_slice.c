/*
 * kata 05 / 08 —— PS 包按 MTU 切成多个 RTP 分片
 * ==========================================================================
 * 【这一步解决什么问题】一个 PS 帧有几 KB 到几十 KB，超过 MTU 就得拆成多个 RTP 包。
 *   拆的时候有三条铁律，任何一条错了平台都拼不回一帧：
 *     ① seq 每个包 +1（跨帧连续，16 位回绕）
 *     ② 同一帧的所有分片 timestamp 完全相同
 *     ③ 只有最后一片 marker(M)=1 —— 平台靠它判断「这一帧收齐了」
 * 【对应协议】RFC 3550（RTP：12 字节固定头，byte1 = M(1位) | PT(7位)）；
 *   GB/T 28181 附录 C：PS 流用 RTP 承载，动态载荷类型 96（PS/90000）。
 * 【最容易踩的坑】marker 位是 byte1 的**最高位 0x80**，不是最低位！
 *     rtp[1] = PT | (last ? 0x80 : 0) = 0x60 / 0xE0
 *   写成 PT | 1 会同时错两处：PT 96 变成 97（另一种载荷类型），而 marker 永远为 0
 *   —— 平台要么把 3 片当成 3 个残缺帧，要么永远等不到帧结束。本 kata 会把两种写法都打出来。
 * 【面试会怎么问 / 怎么答】
 *   Q1 MTU 为什么取 1400 而不是 1500？  A: RTP 载荷之外还有 12 字节 RTP + 8 字节 UDP
 *      + 20 字节 IP = 40 字节开销，1500 的链路 MTU 下最多 1460；取 1400 留余量，
 *      避免 VPN/PPPoE 再吃掉几字节导致分片（IP 分片会显著增加丢包代价）。
 *   Q2 一帧的 ts 为什么必须一样？  A: RTP 时间戳表示「这一帧的采样时刻」，同一帧的
 *      多个分片属于同一时刻；ts 变化会被解码端当成新帧。
 *   Q3 丢了一片怎么办？  A: 平台按 M 位凑帧，凑不齐就丢整帧（GB28181 是 PS over RTP，
 *      没有重传/FEC 时只能丢帧等下一个 I 帧）。
 */
#include <stdio.h>
#include <string.h>

#define PT          96
#define MTU         1400
#define SSRC        0x12345678u
#define PAYLOAD_LEN 3000

static void hexdump(const char *tag, const unsigned char *d, size_t n)
{
    size_t i, j;
    printf("      %s：", tag);
    for (i = 0; i < n; i += 16) {
        if (i) printf("             ");
        for (j = 0; j < 16; j++) { if (i + j < n) printf("%02x ", d[i + j]); else printf("   "); }
        printf("\n");
    }
}

static int build_rtp(unsigned char *pkt, unsigned int seq, unsigned int ts, int marker,
                     const unsigned char *pay, size_t len)
{
    pkt[0] = 0x80;                                        /* V=2 P=0 X=0 CC=0 */
    pkt[1] = (unsigned char)(PT | (marker ? 0x80 : 0));   /* ★ marker 是最高位 0x80，不是 1 */
    pkt[2] = (unsigned char)((seq >> 8) & 0xFF);
    pkt[3] = (unsigned char)(seq & 0xFF);
    pkt[4] = (unsigned char)((ts >> 24) & 0xFF);
    pkt[5] = (unsigned char)((ts >> 16) & 0xFF);
    pkt[6] = (unsigned char)((ts >> 8) & 0xFF);
    pkt[7] = (unsigned char)(ts & 0xFF);
    pkt[8]  = (unsigned char)((SSRC >> 24) & 0xFF);
    pkt[9]  = (unsigned char)((SSRC >> 16) & 0xFF);
    pkt[10] = (unsigned char)((SSRC >> 8) & 0xFF);
    pkt[11] = (unsigned char)(SSRC & 0xFF);
    memcpy(pkt + 12, pay, len);
    return (int)(12 + len);
}

int main(void)
{
    unsigned char payload[PAYLOAD_LEN], pkt[12 + MTU];
    unsigned int seq_seen[8] = {0}, ts_seen[8] = {0};
    int m_seen[8] = {0}, b1_seen[8] = {0};
    size_t len_seen[8] = {0};
    size_t off = 0, i;
    unsigned int seq = 1000, ts = 90000;
    int n = 0, fail = 0;
    char reason[200] = "";

    setvbuf(stdout, NULL, _IOLBF, 0);
    for (i = 0; i < PAYLOAD_LEN; i++) payload[i] = (unsigned char)(i & 0xFF);

    printf("=========== kata 05：一个 %d 字节的 PS 包按 MTU=%d 切片 ===========\n", PAYLOAD_LEN, MTU);
    printf("预期片数 = ceil(%d / %d) = %d 片\n\n", PAYLOAD_LEN, MTU, (PAYLOAD_LEN + MTU - 1) / MTU);

    while (off < PAYLOAD_LEN && n < 8) {
        size_t piece = PAYLOAD_LEN - off;
        int last, len;
        if (piece > MTU) piece = MTU;
        last = (off + piece >= PAYLOAD_LEN);
        len = build_rtp(pkt, seq, ts, last, payload + off, piece);
        printf("  分片 %d/%d：偏移 %zu，载荷 %zu 字节，RTP 总长 %d 字节（12 头 + 载荷），seq=%u，ts=%u，M=%d\n",
               n + 1, (PAYLOAD_LEN + MTU - 1) / MTU, off, piece, len, seq, ts, last);
        printf("      RTP 头 byte0=0x%02X byte1=0x%02X → M=%d，PT=%d\n",
               pkt[0], pkt[1], (pkt[1] >> 7) & 1, pkt[1] & 0x7F);
        hexdump("前 12 字节 RTP 固定头", pkt, 12);
        seq_seen[n] = seq; ts_seen[n] = ts; m_seen[n] = last;
        b1_seen[n] = pkt[1]; len_seen[n] = piece;
        seq++; off += piece; n++;
    }

    printf("\n--------------- 对照：如果写成 PT|1 会怎样 ---------------\n");
    printf("  正确：byte1 = 0x%02X | 0x80 = 0x%02X → M=1 PT=96（最后一片）\n", PT, PT | 0x80);
    printf("  错误：byte1 = 0x%02X | 0x01 = 0x%02X → M=%d PT=%d （PT 都被改了！）\n",
           PT, PT | 1, (PT | 1) >> 7, (PT | 1) & 0x7F);
    printf("  中间片：byte1 = 0x%02X → M=0，平台知道「这帧还没收完，继续等」\n", PT);

    printf("\n--------------- 断言 ---------------\n");
    printf("  片数=%d，载荷=%zu/%zu/%zu，ts=%u/%u/%u，M=%d/%d/%d，byte1=0x%02X/0x%02X/0x%02X\n",
           n, len_seen[0], len_seen[1], len_seen[2], ts_seen[0], ts_seen[1], ts_seen[2],
           m_seen[0], m_seen[1], m_seen[2], b1_seen[0], b1_seen[1], b1_seen[2]);
    if (n != 3) { fail = 1; snprintf(reason, sizeof reason, "应切 3 片，实际 %d 片", n); }
    else if (len_seen[0] != MTU || len_seen[1] != MTU || len_seen[2] != PAYLOAD_LEN - 2 * MTU) {
        fail = 1; snprintf(reason, sizeof reason, "分片长度应为 1400/1400/200");
    } else if (m_seen[0] != 0 || m_seen[1] != 0 || m_seen[2] != 1) {
        fail = 1; snprintf(reason, sizeof reason, "只有最后一片 marker 应为 1");
    } else if (ts_seen[0] != ts_seen[1] || ts_seen[1] != ts_seen[2]) {
        fail = 1; snprintf(reason, sizeof reason, "同一帧 3 片的 timestamp 必须相同");
    } else if (b1_seen[0] != PT || b1_seen[2] != (PT | 0x80)) {
        fail = 1; snprintf(reason, sizeof reason, "byte1 应为 0x60/0xE0，实际 0x%02X/0x%02X", b1_seen[0], b1_seen[2]);
    } else if ((b1_seen[0] & 0x7F) != PT || (b1_seen[2] & 0x7F) != PT) {
        fail = 1; snprintf(reason, sizeof reason, "PT 必须始终是 96");
    } else if (seq_seen[0] + 1 != seq_seen[1] || seq_seen[1] + 1 != seq_seen[2]) {
        fail = 1; snprintf(reason, sizeof reason, "seq 必须每片 +1");
    } else if (((PT | 1) & 0x7F) == PT) {
        fail = 1; snprintf(reason, sizeof reason, "PT|1 演示失效");
    }
    if (fail) { printf("SELF-CHECK FAIL: %s\n", reason); return 1; }
    printf("SELF-CHECK PASS\n");
    return 0;
}
