/*
 * kata 02 / 08 —— MPEG-PS 的 33bit PTS → 5 字节位域编码
 * ============================================================================
 * 【这一步解决什么问题】
 *   时间戳 PTS 有 33 位，可 PES 头里只给它 5 个字节（40 位）；多出来的 7 位
 *   要用来塞固定前缀和 marker 位，否则解析器没法同步。于是 PTS 被切成 5 段：
 *       byte0: 0010(4位前缀) | PTS[32:30](3位) | marker(1) → 0x21 | ((pts >> 29) & 0x0E)
 *       byte1:                 PTS[29:22](8位)           → (pts >> 22) & 0xFF
 *       byte2: 0x01(1位前缀)  | PTS[21:15](7位) | marker(1) → 0x01 | ((pts >> 14) & 0xFE)  ★★
 *       byte3:                 PTS[14:7](8位)            → (pts >> 7) & 0xFF
 *       byte4: 0x01(1位前缀)  | PTS[6:0](7位)   | marker(1) → 0x01 | ((pts & 0x7F) << 1)
 *   位数核对：3 + 8 + 7 + 8 + 7 = 33 位，一位不多一位不少。
 *
 * 【对应协议的哪一部分】
 *   ISO/IEC 13818-1（MPEG-2 系统层）PES 头里的 PTS/DTS 编码（PTS_DTS_flags=10/11），
 *   GB/T 28181 附录 C 规定 PS 流用 90kHz 时钟：视频 15fps → 每帧 +6000。
 *
 * 【面试会怎么问 / 怎么答】
 *   Q1: PTS 是 33 位，为什么编码要用右移 14 和 &0xFE 这种怪式子？
 *   A : byte2 里 marker 位占最低位，PTS[21:15] 只能从 bit1 往上摆，
 *       所以先把 bit15 移到 bit1（右移 14），再用 &0xFE 清掉 bit0 给 marker 让位。
 *   Q2（真正的坑）: 如果写成 (pts >> 15) & 0xFE 会怎样？
 *   A : PTS[21:15] 被换成了 PTS[22:16]，第 15 位丢掉、第 22 位重复使用。
 *       解码端拿到的时间戳整体错位——但画面**照样能解码出来**（只是时间戳抖动、
 *       偶尔回退），功能测试根本发现不了。本 kata 会把错误写法也跑一遍，
 *       让你看到 90000 被解回 57232（差 32768 个 tick ≈ 364ms）。
 *       这是「能跑但不对」的教科书案例，也是本仓库原型里真实存在的 bug。
 */
#include <stdio.h>
#include <string.h>

/* bug=0 正确写法；bug=1 故意复现 >>15 的错误写法 */
static void pts_encode(unsigned char o[5], unsigned long long pts, int bug)
{
    pts &= 0x1FFFFFFFFULL;                      /* 只保留低 33 位 */
    o[0] = (unsigned char)(0x21 | ((pts >> 29) & 0x0E));
    o[1] = (unsigned char)((pts >> 22) & 0xFF);
    o[2] = (unsigned char)(0x01 | (bug ? ((pts >> 15) & 0xFE) : ((pts >> 14) & 0xFE)));
    o[3] = (unsigned char)((pts >> 7) & 0xFF);
    o[4] = (unsigned char)(0x01 | ((pts & 0x7F) << 1));
}

/* 解码端（demuxer）的做法：把 5 个字节里的 PTS 段拼回来 */
static unsigned long long pts_decode(const unsigned char b[5])
{
    return ((unsigned long long)((b[0] >> 1) & 0x07) << 30) |
           ((unsigned long long)b[1] << 22) |
           ((unsigned long long)((b[2] >> 1) & 0x7F) << 15) |
           ((unsigned long long)b[3] << 7) |
           ((unsigned long long)((b[4] >> 1) & 0x7F));
}

static void bin_str(char *out, unsigned long long v, int bits)
{
    int i;
    for (i = bits - 1; i >= 0; i--) out[bits - 1 - i] = (char)('0' + ((v >> i) & 1));
    out[bits] = 0;
}

static void hex5(char *out, const unsigned char b[5])
{
    snprintf(out, 15, "%02X %02X %02X %02X %02X", b[0], b[1], b[2], b[3], b[4]);
}

static void dump_fields(unsigned long long p)
{
    char b[40];
    bin_str(b, (p >> 30) & 0x07, 3); printf("     PTS[32:30] = %s  (3 位)\n", b);
    bin_str(b, (p >> 22) & 0xFF, 8); printf("     PTS[29:22] = %s  (8 位)\n", b);
    bin_str(b, (p >> 15) & 0x7F, 7); printf("     PTS[21:15] = %s  (7 位)  ← 装进 byte2 的 bit7..bit1\n", b);
    bin_str(b, (p >> 7)  & 0xFF, 8); printf("     PTS[14:7]  = %s  (8 位)\n", b);
    bin_str(b, (p & 0x7F),       7); printf("     PTS[6:0]   = %s  (7 位)  ← 装进 byte4 的 bit7..bit1\n", b);
}

int main(void)
{
    static const struct { unsigned long long pts; const char *expect; } VEC[4] = {
        {0ULL,          "21 00 01 00 01"},
        {6000ULL,       "21 00 01 2E E1"},
        {90000ULL,      "21 00 05 BF 21"},
        {0x1FFFFFFFFULL,"2F FF FF FF FF"}    /* 33 位全 1 的边界 */
    };
    unsigned char enc[5], bad[5];
    char got[16], reason[200] = "";
    int i, fail = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=========== kata 02：33bit PTS → 5 字节位域编码 ===========\n");

    for (i = 0; i < 4; i++) {
        unsigned long long p = VEC[i].pts;
        printf("\n── 向量 %d：PTS = %llu（0x%09llX）\n", i + 1, p, p);
        dump_fields(p);
        pts_encode(enc, p, 0);
        printf("   逐字节合成：\n");
        printf("     byte0 = 0x21 | ((%llu >> 29) & 0x0E) = 0x%02X\n", p, enc[0]);
        printf("     byte1 =        ((%llu >> 22) & 0xFF) = 0x%02X\n", p, enc[1]);
        printf("     byte2 = 0x01 | ((%llu >> 14) & 0xFE) = 0x%02X   ← 写成 >>15 就错\n", p, enc[2]);
        printf("     byte3 =        ((%llu >>  7) & 0xFF) = 0x%02X\n", p, enc[3]);
        printf("     byte4 = 0x01 | ((%llu & 0x7F) << 1)  = 0x%02X\n", p, enc[4]);
        hex5(got, enc);
        printf("   结果 %s | 期望 %s | %s\n", got, VEC[i].expect,
               strcmp(got, VEC[i].expect) == 0 ? "一致 ✓" : "不一致 ✗");
        printf("   解码端回读 PTS = %llu %s\n", pts_decode(enc),
               pts_decode(enc) == p ? "（与写入一致 ✓）" : "（与写入不一致 ✗）");
        if (strcmp(got, VEC[i].expect) != 0 || pts_decode(enc) != p) {
            fail = 1;
            snprintf(reason, sizeof reason, "pts=%llu 编码结果 %s（解回 %llu），期望 %s（解回 %llu）",
                     p, got, pts_decode(enc), VEC[i].expect, p);
            break;
        }
    }

    printf("\n──────────────── >>15 与 >>14 的差别（同一个 PTS=90000）────────────────\n");
    pts_encode(enc, 90000ULL, 0);
    pts_encode(bad, 90000ULL, 1);
    hex5(got, enc); printf("  正确 >>14 : %s  → 解码回读 PTS = %-8llu\n", got, pts_decode(enc));
    hex5(got, bad); printf("  错误 >>15 : %s  → 解码回读 PTS = %-8llu\n", got, pts_decode(bad));
    printf("  两者差 %llu 个 tick；90kHz 下 ≈ %.1f ms。\n",
           pts_decode(enc) - pts_decode(bad),
           (double)(pts_decode(enc) - pts_decode(bad)) * 1000.0 / 90000.0);
    printf("  关键点：错误字节仍然长得「像」合法 PTS（前缀/marker 都对），播放器照样出画面，\n");
    printf("          所以只能靠位域断言或 ffprobe 读回 PTS 才能发现。\n");
    if (enc[2] == bad[2]) {
        fail = 1;
        snprintf(reason, sizeof reason, "错误写法与正确写法产生相同 byte2，演示失效");
    }
    if (!fail && pts_decode(bad) == 90000ULL) {
        fail = 1;
        snprintf(reason, sizeof reason, "错误写法竟然解回 90000，演示失效");
    }

    if (fail) { printf("\nSELF-CHECK FAIL: %s\n", reason); return 1; }
    printf("\nSELF-CHECK PASS\n");
    return 0;
}
