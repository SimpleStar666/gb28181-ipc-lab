/*
 * kata 03 / 08 —— 手工组一个完整 PS：Pack Header + System Header + PSM + PES
 * ==========================================================================
 * 【这一步解决什么问题】GB28181 推的不是裸 H.264，而是 MPEG-PS（节目流）：每一帧前
 *   都要挂一整套「节目单」，平台从任意一个 RTP 包接入就能立刻知道这条流里有几条 ES、
 *   分别是什么编码。本 kata 把这 4 段手工拼出来并逐字段标注：
 *     ① Pack Header    14 字节  00 00 01 BA + SCR(系统时钟) + program_mux_rate
 *     ② System Header  16 字节  00 00 01 BB + 码率上界 + 每条流的标准缓冲大小
 *     ③ PSM            20 字节  00 00 01 BC + 节目流地图（stream_type ↔ stream_id）
 *     ④ PES 头         14 字节  00 00 01 E0 + 长度 + flags(3) + PTS(5)
 *   注意 ①②③ **每一帧都重发**，不是只在开头发一次：RTP 会丢包，平台可能从中间某帧
 *   才收到流，必须每帧自带完整节目单（本仓库原型就是这么做的）。
 * 【对应协议】ISO/IEC 13818-1（MPEG-2 系统层）pack_header / system_header /
 *   program_stream_map / PES packet；GB/T 28181 附录 C 规定 PS over RTP 封装。
 * 【面试会怎么问 / 怎么答】
 *   Q1 PS 封装每帧的固定开销？  A: 14+16+20+14 = 64 字节；一帧一个 PES，不把多帧塞一起。
 *   Q2 PES_packet_length 怎么算？  A: 长度字段之后的字节数 = flags(3) + PTS(5) + 载荷
 *      = 载荷 + 8；超过 0xFFFF（单帧 > 64KB）时按规范写 0，让解码端读到下一个起始码为止。
 *   Q3 这三个头能不能只在第一帧发？  A: 不能，每帧都发；这就是「自包含」设计。
 *   附：本 kata 用的 14 字节 pack header 与 `ffmpeg -f vob` 吐出的真实 MPEG-2 pack
 *   header 逐字节相同（可用 xxd 自行对比）；而 ffmpeg 的 `-f mpeg` 走的是 12 字节
 *   MPEG-1 pack header（前缀是 0x21 而不是 0x44）。
 */
#include <stdio.h>
#include <string.h>

#define PT_VIDEO 0xE0
#define PTS_VAL  90000ULL          /* 第 1 帧 PTS，90kHz 时钟 */

static const unsigned char PACK_HDR[14] = {0x00,0x00,0x01,0xBA,0x44,0x00,0x04,0x00,0x04,0x01,0x86,0x66,0xCF,0xF8};
static const unsigned char SYS_HDR[16]  = {0x00,0x00,0x01,0xBB,0x00,0x09,0xC3,0x33,0x67,0x00,0x21,0xFF,0xE0,0xE0,0xE6,0x00};
static const unsigned char PSM_VID[20]  = {0x00,0x00,0x01,0xBC,0x00,0x0E,0x01,0x01,0x00,0x00,0x00,0x04,0x1B,0xE0,0x00,0x00,0x00,0x00,0x00,0x00};
static const unsigned char PAYLOAD[]    = {0x00,0x00,0x00,0x01,0x65,0x88,0x84,0x00,0x10,0x20,0x30,0x40}; /* 假的 IDR NAL，12 字节 */

static void pts_encode(unsigned char o[5], unsigned long long pts)
{
    pts &= 0x1FFFFFFFFULL;
    o[0] = (unsigned char)(0x21 | ((pts >> 29) & 0x0E));
    o[1] = (unsigned char)((pts >> 22) & 0xFF);
    o[2] = (unsigned char)(0x01 | ((pts >> 14) & 0xFE));   /* 写成 >>15 就整体错位（见 kata 02） */
    o[3] = (unsigned char)((pts >> 7) & 0xFF);
    o[4] = (unsigned char)(0x01 | ((pts & 0x7F) << 1));
}

static unsigned long long pts_decode(const unsigned char b[5])
{
    return ((unsigned long long)((b[0] >> 1) & 0x07) << 30) | ((unsigned long long)b[1] << 22) |
           ((unsigned long long)((b[2] >> 1) & 0x7F) << 15) | ((unsigned long long)b[3] << 7) |
           ((unsigned long long)((b[4] >> 1) & 0x7F));
}

static void hexdump(const char *tag, const unsigned char *d, size_t n)
{
    size_t i, j;
    printf("---- %s（%zu 字节）----\n", tag, n);
    for (i = 0; i < n; i += 16) {
        printf("%04zx  ", i);
        for (j = 0; j < 16; j++) { if (i + j < n) printf("%02x ", d[i + j]); else printf("   "); }
        printf("\n");
    }
}

/* 打印一行「字节 + 字段名」标注 */
static void anno(const unsigned char *p, size_t off, size_t n, const char *what)
{
    char hex[64];
    size_t i, k = 0;
    for (i = 0; i < n && k + 4 < sizeof hex; i++)
        k += (size_t)snprintf(hex + k, sizeof hex - k, "%02X ", p[off + i]);
    if (n == 1) printf("  [%02zX]      %-16s %s\n", off, hex, what);
    else        printf("  [%02zX-%02zX] %-16s %s\n", off, off + n - 1, hex, what);
}

int main(void)
{
    unsigned char ps[128], pes[14];
    size_t ps_len = 0, pay_len = sizeof PAYLOAD, off;
    unsigned int pes_len;
    char reason[200] = "";
    int fail = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=========== kata 03：手工组一个完整 PS ===========\n");
    hexdump("pack header", PACK_HDR, sizeof PACK_HDR);

    /* ---- 拼 PS ---- */
    memcpy(ps + ps_len, PACK_HDR, sizeof PACK_HDR); ps_len += sizeof PACK_HDR;
    memcpy(ps + ps_len, SYS_HDR,  sizeof SYS_HDR);  ps_len += sizeof SYS_HDR;
    memcpy(ps + ps_len, PSM_VID,  sizeof PSM_VID);  ps_len += sizeof PSM_VID;
    pes_len = (unsigned int)pay_len + 8;                 /* flags(3)+PTS(5)+载荷 */
    if (pes_len > 0xFFFF) pes_len = 0;
    pes[0] = 0x00; pes[1] = 0x00; pes[2] = 0x01; pes[3] = PT_VIDEO;
    pes[4] = (unsigned char)((pes_len >> 8) & 0xFF); pes[5] = (unsigned char)(pes_len & 0xFF);
    pes[6] = 0x80; pes[7] = 0x80; pes[8] = 0x05;         /* '10'+flags / PTS_DTS='10' / 头长 5 */
    pts_encode(pes + 9, PTS_VAL);
    memcpy(ps + ps_len, pes, sizeof pes); ps_len += sizeof pes;
    memcpy(ps + ps_len, PAYLOAD, pay_len); ps_len += pay_len;

    hexdump("完整 PS（一帧）", ps, ps_len);
    printf("\n--------------- 逐字段标注 ---------------\n");
    anno(ps, 0, 4, "① pack_start_code（MPEG-2 pack header 起始码）");
    anno(ps, 4, 1, "   0x44 = '01' + SCR[32:30]=000 + marker=1 + SCR[29:28]=00");
    anno(ps, 5, 1, "   SCR[27:20]");
    anno(ps, 6, 1, "   0x04 = SCR[19:15] + marker(1) + SCR[14:13]");
    anno(ps, 7, 1, "   SCR[12:5]");
    anno(ps, 8, 1, "   0x04 = SCR[4:0] + marker(1) + SCR_ext[8:7]");
    anno(ps, 9, 1, "   0x01 = SCR_ext[6:0] + marker → 本例 SCR=0（真机填 90kHz STC）");
    anno(ps, 10, 3, "   program_mux_rate(22 位)=0x2199B3 + marker + reserved（单位 50 字节/秒）");
    anno(ps, 13, 1, "   0xF8 = reserved 余 4 位(1111) + pack_stuffing_length(000) + 对齐填充 0");
    off = 14;
    anno(ps, off, 4, "② system_header_start_code");
    anno(ps, off + 4, 2, "   header_length = 0x0009（其后 9 字节；本数组多带 1 个 0x00 填充）");
    anno(ps, off + 6, 1, "   0xC3 = marker=1 + rate_bound[21:15]");
    anno(ps, off + 7, 3, "   rate_bound 余下位 + marker + audio_bound + fixed/CSPS 标志");
    anno(ps, off + 10, 1, "   0x21 = audio_lock/video_lock + marker + video_bound=1 条流");
    anno(ps, off + 11, 1, "   0xFF = reserved 位全 1");
    anno(ps, off + 12, 3, "   流条目：stream_id=0xE0 + '11' + STD scale=1 + size_bound=0x0E6");
    anno(ps, off + 15, 1, "   0x00 = 数组多带的填充字节（不破坏后面的 00 00 01 BC）");
    off = 30;
    anno(ps, off, 4, "③ program_stream_map_start_code");
    anno(ps, off + 4, 2, "   program_stream_map_length = 0x000E = 20-6（不含起始码 4B 和自身 2B）");
    anno(ps, off + 6, 1, "   current_next_indicator(1)+reserved(2)+version(5)");
    anno(ps, off + 7, 1, "   reserved(7)+marker_bit(1)");
    anno(ps, off + 8, 2, "   program_stream_info_length = 0x0000（没有描述子）");
    anno(ps, off + 10, 2, "   elementary_stream_map_length = 0x0004（1 条 4 字节条目）");
    anno(ps, off + 12, 4, "   ES 条目：stream_type=0x1B(H.264) + es_id=0xE0 + info_len=0");
    anno(ps, off + 16, 4, "   CRC_32 = 0（GB28181 实测里很多实现直接写 0）");
    off = 50;
    anno(ps, off, 4, "④ 00 00 01 + stream_id=0xE0（视频；音频是 0xC0）");
    anno(ps, off + 4, 2, "   PES_packet_length = 载荷+8");
    anno(ps, off + 6, 1, "   0x80 = '10'+scrambling/priority/alignment 等全 0");
    anno(ps, off + 7, 1, "   0x80 = PTS_DTS_flags='10'（只有 PTS，没有 DTS）");
    anno(ps, off + 8, 1, "   0x05 = PES_header_data_length（后面 5 字节 PTS）");
    anno(ps, off + 9, 5, "   PTS 位域 → 解码回 90000（编码规则见 kata 02）");
    anno(ps, 64, pay_len, "   帧数据：AnnexB 的 IDR NAL（切分见 kata 01）");
    printf("PS 总长 %zu = 固定头 %zu + PES 头 14 + 载荷 %zu；PES_packet_length=0x%04X=%u = 载荷+8\n",
           ps_len, (size_t)64, pay_len, pes_len, pes_len);

    printf("\n--------------- 断言 ---------------\n");
    if (memcmp(ps, "\x00\x00\x01\xBA\x44\x00", 6) != 0) {
        fail = 1; snprintf(reason, sizeof reason, "前 6 字节不是 00 00 01 BA 44 00");
    } else if (memcmp(ps + 14, SYS_HDR, sizeof SYS_HDR) != 0 ||
               memcmp(ps + 30, PSM_VID, sizeof PSM_VID) != 0) {
        fail = 1; snprintf(reason, sizeof reason, "System Header / PSM 位置不对");
    } else if ((size_t)((ps[34] << 8) | ps[35]) != sizeof PSM_VID - 6) {
        fail = 1; snprintf(reason, sizeof reason, "PSM packet_length 应等于 20-6=14");
    } else if (ps[50] != 0 || ps[51] != 0 || ps[52] != 1 || ps[53] != PT_VIDEO) {
        fail = 1; snprintf(reason, sizeof reason, "PES 起始码/stream_id 不对");
    } else if ((size_t)((ps[54] << 8) | ps[55]) != pay_len + 8) {
        fail = 1; snprintf(reason, sizeof reason, "PES_packet_length 应为 载荷+8");
    } else if (pts_decode(ps + 59) != PTS_VAL) {
        fail = 1; snprintf(reason, sizeof reason, "PES 里的 PTS 解不回 90000");
    } else if (ps_len != 76) {
        fail = 1; snprintf(reason, sizeof reason, "PS 总长应为 76，实际 %zu", ps_len);
    }
    if (fail) { printf("SELF-CHECK FAIL: %s\n", reason); return 1; }
    printf("SELF-CHECK PASS\n");
    return 0;
}
