/*
 * kata 07 / 08 —— G.711 A-law 编解码（PCMA）
 * ==========================================================================
 * 【这一步解决什么问题】GB28181 的音频 ES 是 G.711A：8kHz 采样、每个样点 1 字节、
 *   20ms 一帧 = 160 字节。A-law 是「伪对数」压扩：小信号步长小、大信号步长大，
 *   用 8 bit 装下 13 bit 的动态范围。本 kata 实现编解码，用标准测试表核对，
 *   再生成一帧 160 字节的音频并打印。
 * 【对应协议】ITU-T G.711（A-law 压扩）；GB/T 28181 附录 C 规定音频编码 G.711A，
 *   RTP 载荷类型 PT=8（PCMA）；SDP 写作 m=audio 0 RTP/AVP 8 + a=rtpmap:8 PCMA/8000。
 * 【为什么中国用 A-law、美国用 μ-law】
 *   G.711 有两套曲线：A-law（PCMA，PT 8）配 30 路 E1 制式（欧洲/中国/国际），
 *   μ-law（PCMU，PT 0）配 24 路 T1 制式（美国/日本）。中国 PSTN 是 E1，
 *   所以 GB/T 28181 规定的是 G.711A；对接国外平台必须先确认对端要 PCMA 还是 PCMU。
 * 【面试会怎么问 / 怎么答】
 *   Q1 A-law/μ-law 能直接把字节丢给对方吗？  A: 不能。要先按本侧曲线解码成线性 PCM，
 *      再按对端曲线编码；字节直接透传会变成噪音。
 *   Q2 编解码为什么都要 XOR 0x55？  A: A-law 刻意让码字交替翻转，避免全 0 码流；
 *      编码后 XOR、解码前 XOR 回来（正负号的 mask 分别是 0xD5/0x55）。
 *   Q3 20ms 一帧为什么是 160 字节？  A: 8000Hz × 0.02s = 160 个样点，PCMA 每样点 1 字节。
 */
#include <stdio.h>

#define SEG_SHIFT  4
#define QUANT_MASK 0x0F
#define SEG_MASK   0x70
#define SIGN_BIT   0x80

static const short seg_end[8] = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};

/* 16bit 线性 PCM → A-law 字节 */
static unsigned char linear2alaw(short pcm)
{
    short mask, seg;
    unsigned char aval;

    pcm = (short)(pcm >> 3);                 /* 16 位压到 13 位有效位 */
    if (pcm >= 0) mask = 0xD5;
    else { mask = 0x55; pcm = (short)(-pcm - 1); }
    for (seg = 0; seg < 8; seg++) if (pcm <= seg_end[seg]) break;
    if (seg >= 8) return (unsigned char)(0x7F ^ mask);
    aval = (unsigned char)(seg << SEG_SHIFT);
    if (seg < 2) aval |= (unsigned char)((pcm >> 1) & QUANT_MASK);
    else         aval |= (unsigned char)((pcm >> seg) & QUANT_MASK);
    return (unsigned char)(aval ^ mask);
}

/* A-law 字节 → 16bit 线性 PCM */
static short alaw2linear(unsigned char a)
{
    short t, seg;

    a ^= 0x55;
    t = (short)((a & QUANT_MASK) << 4);
    seg = (short)(((unsigned)a & SEG_MASK) >> SEG_SHIFT);
    if (seg == 0) t += 8;
    else if (seg == 1) t += 0x108;
    else { t += 0x108; t = (short)(t << (seg - 1)); }
    return (a & SIGN_BIT) ? t : (short)(-t);
}

static void hexdump(const char *tag, const unsigned char *d, size_t n)
{
    size_t i, j;
    printf("%s（%zu 字节）\n", tag, n);
    for (i = 0; i < n; i += 16) {
        printf("  %04zx  ", i);
        for (j = 0; j < 16; j++) { if (i + j < n) printf("%02x ", d[i + j]); else printf("   "); }
        printf("\n");
    }
}

int main(void)
{
    static const unsigned char TV[5] = {0x00, 0xFF, 0x55, 0xAA, 0x80};
    static const short TE[5] = {-5504, 848, -8, 32256, 5504};
    unsigned char frame[160];
    long maxerr = 0, v, rt_bad = 0;
    int i, distinct = 0, seen[256] = {0}, fail = 0;
    char reason[200] = "";

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=========== kata 07：G.711 A-law 编解码 ===========\n");

    printf("\n[1] 标准测试点核对（8bit A-law → 16bit PCM，再编回去必须还原）\n");
    for (i = 0; i < 5; i++) {
        short dec = alaw2linear(TV[i]);
        unsigned char re = linear2alaw(dec);
        printf("    0x%02X → %6d，再编码回 0x%02X %s\n", TV[i], dec, re, re == TV[i] ? "✓" : "✗");
        if (dec != TE[i]) { fail = 1; snprintf(reason, sizeof reason, "0x%02X 应解码为 %d，实际 %d", TV[i], TE[i], dec); }
        else if (re != TV[i]) { fail = 1; snprintf(reason, sizeof reason, "0x%02X 往返后变成 0x%02X", TV[i], re); }
    }

    printf("\n[2] 全 256 个码字往返 + 量化误差\n");
    for (i = 0; i < 256; i++) if (linear2alaw(alaw2linear((unsigned char)i)) != (unsigned char)i) rt_bad++;
    for (v = -32768; v <= 32767; v++) {
        long e = (long)alaw2linear(linear2alaw((short)v)) - v;
        if (e < 0) e = -e;
        if (e > maxerr) maxerr = e;
    }
    printf("    256 个码字往返不一致的个数 = %ld（应为 0）\n", rt_bad);
    printf("    16bit 全量程最大往返误差 = %ld（满幅 32768 的 %.2f%%）——这是 A-law 的量化损失，不是 bug\n",
           maxerr, (double)maxerr * 100.0 / 32768.0);
    if (!fail && rt_bad != 0) { fail = 1; snprintf(reason, sizeof reason, "%ld 个码字往返不一致", rt_bad); }

    printf("\n[3] 生成一帧 160 字节（20ms @8kHz，1kHz 方波，幅度 ±8000）\n");
    for (i = 0; i < 160; i++) {
        short s = ((i / 4) % 2) ? (short)8000 : (short)-8000;   /* 每 4 个样点翻转 → 周期 8 → 1kHz */
        frame[i] = linear2alaw(s);
    }
    for (i = 0; i < 160; i++) if (!seen[frame[i]]) { seen[frame[i]] = 1; distinct++; }
    hexdump("帧数据", frame, sizeof frame);
    printf("  解码回看：第一个样点 = %d，第五个样点 = %d（原值 -8000 / +8000）\n",
           alaw2linear(frame[0]), alaw2linear(frame[4]));
    printf("  这一帧只有 %d 种码字，说明方波的两个幅度被稳定地量化到同两个码字\n", distinct);
    printf("  RTP 封装时：PT=8(PCMA)，时间戳每帧 +160（8kHz 下 20ms = 160），另见 kata 05\n");

    printf("\n--------------- 断言 ---------------\n");
    printf("  帧长 = %zu（应为 160）；frame[0]=0x%02X（-8000），frame[4]=0x%02X（+8000）\n",
           sizeof frame, frame[0], frame[4]);
    if (!fail && sizeof frame != 160) { fail = 1; snprintf(reason, sizeof reason, "帧长应为 160"); }
    if (!fail && (frame[0] != 0x0A || frame[4] != 0x8A)) {
        fail = 1; snprintf(reason, sizeof reason, "±8000 应编码为 0x0A/0x8A，实际 0x%02X/0x%02X", frame[0], frame[4]);
    }
    if (!fail && distinct != 2) { fail = 1; snprintf(reason, sizeof reason, "方波帧应只有 2 种码字，实际 %d", distinct); }
    if (!fail && (alaw2linear(0x00) >= 0 || alaw2linear(0xAA) <= 0)) {
        fail = 1; snprintf(reason, sizeof reason, "0x00 应为负、0xAA 应为正（符号位检查）");
    }
    if (fail) { printf("SELF-CHECK FAIL: %s\n", reason); return 1; }
    printf("SELF-CHECK PASS\n");
    return 0;
}
