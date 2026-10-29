/*
 * kata 04 / 08 —— PSM 的 packet_length 到底怎么数（总长 − 6）
 * ==========================================================================
 * 【这一步解决什么问题】PSM（program_stream_map）是 PS 里的「节目流地图」：
 *   它告诉解码器「这条流里有哪几条 ES、各自的 stream_type 和 stream_id 是什么」。
 *   而它开头的 program_stream_map_length 必须填「从 current_next_indicator 数到
 *   CRC_32 的字节数」，也就是：
 *        packet_length = PSM 总字节数 − 6
 *   减掉的 6 = 4 字节起始码 00 00 01 BC + length 字段自身 2 字节。
 *   这是 MPEG 系 length 字段的统一惯例（「我后面还有多少字节」），也是最容易写错、
 *   写错后最难查的一个数字。
 * 【对应协议】ISO/IEC 13818-1 §2.5.4 program_stream_map；GB/T 28181 附录 C 的
 *   PSM 就是 ISO 的 PSM 加固定条目：视频 0x1B/0xE0（H.264），音频 0x90/0xC0（G.711A）。
 *   §2.5.4 的出处：libmpeg 的 mpeg-psm.c 文件头注释即标注「ITU-T H.222.0(10/2014) 2.5.4 Program stream map(p82)」。
 * 【面试会怎么问 / 怎么答】
 *   Q1 length 为什么减 6？  A: 不含起始码（4）也不含自己（2）；其余全算，包括标志位、
 *      两个 length 子字段、每条 ES 条目、CRC_32。
 *   Q2 写错了会怎样？  A: RTP 照发、平台照收，但解不出流——「能收到包却看不到画面」
 *      十有八九是 PSM 的 length 或 es_map_length 写错。
 *   Q3 ZLMediaKit 的 psm_fetch 为什么把没在 PSM 里声明的 PES 直接丢掉？
 *      A: ZLMediaKit 的 PS 解析用的是它 vendored 进来的 libmpeg，实现在
 *          3rdpart/media-server/libmpeg/source/mpeg-ps-dec.c   ← psm_fetch() 在这里
 *          3rdpart/media-server/libmpeg/source/mpeg-psm.c      ← PSM 解析在这里
 *         （注意：ZLMediaKit 自己的 src/Rtp/PSDecoder.cpp 只是薄封装，
 *           ps_demuxer_create/input 转调 libmpeg，PSM 逻辑不在那儿。）
 *         mpeg-ps-dec.c 里 psm_fetch(psm, sid) 的语义是：按 sid 去 PSM 已登记的
 *         streams[] 里查，查到就返回，查不到直接 `return NULL` ——
 *         只有在编译期开了 MPEG_GUESS_STREAM（视频那支的条件还含 MPEG_H26X_VERIFY）时
 *         才会去「猜」：0xE0~0xEF 猜成 RESERVED，0xC0~0xDF 一律猜成 AAC。
 *         默认构建没有这些分支：没声明的 PES 直接丢。于是没声明的 PES 就没有 track
 *         可投递，被直接跳过。
 *         所以推流端把视频 PES 的 stream_id 写成 0xE1、而 PSM 里只声明了 0xE0 时，
 *         平台会「收到 RTP 但没有视频帧」；同理 es_map_length 写 0 相当于声明零条流，
 *         所有 PES 都被丢。这就是「PSM 必须和真实 PES 对得上」的硬约束。
 *   Q4 怎么用下游源码反过来证明「length 不含起始码、不含自身」这个结论？
 *      A: mpeg-psm.c 的 psm_read() 里有这两行：
 *           program_stream_map_length = mpeg_bits_read16(reader);
 *           end = mpeg_bits_tell(reader) + program_stream_map_length;
 *         注意 mpeg_bits_tell() 的位置已经在 length 字段之后 ——
 *         说明 length 是从「它自己之后」开始数的。
 *         下面还有一行更直白的：
 *           element_stream_map_length = program_stream_map_length
 *                                     - program_stream_info_length - 10;
 *         那个常量 10 的构成（都是 length 字段**之后**的字节）：
 *           标志位 2（current_next/version 1 字节 + reserved/marker 1 字节）
 *           + program_stream_info_length 2 + elementary_stream_map_length 2 + CRC_32 4 = 10。
 *         这里**没有**「起始码 4 字节」这一项——length 从它自己之后开始数，起始码不在其中
 *         （4 和 4 只是数值上巧合相等，别把 CRC 那份算到起始码头上）。
 *         代入单视频版：14 - 0 - 10 = 4，正好是一条 ES 条目的 4 字节；
 *         之后 mpeg_bits_read32() 读掉 CRC_32，合计 6 + 14 = 20 字节。
 *         这就是「packet_length 含 CRC、不含起始码、不含自身」的源码级佐证。
 */
#include <stdio.h>
#include <string.h>

/* 单视频：20 字节，packet_length = 0x000E = 14，es_map_length = 0x0004（1 条） */
static const unsigned char PSM_V[20] = {0x00,0x00,0x01,0xBC,0x00,0x0E,0x01,0x01,0x00,0x00,0x00,0x04,0x1B,0xE0,0x00,0x00,0x00,0x00,0x00,0x00};
/* 视频+音频：24 字节，packet_length = 0x0012 = 18，es_map_length = 0x0008（2 条） */
static const unsigned char PSM_AV[24] = {0x00,0x00,0x01,0xBC,0x00,0x12,0x01,0x01,0x00,0x00,0x00,0x08,0x1B,0xE0,0x00,0x00,0x90,0xC0,0x00,0x00,0x00,0x00,0x00,0x00};

static const char *field_name(size_t i, int has_audio)
{
    switch (i) {
    case 0: case 1: case 2: case 3: return "起始码 00 00 01 BC（不计入 length）";
    case 4: return "packet_length 高字节（自身不计入）";
    case 5: return "packet_length 低字节（自身不计入）";
    case 6: return "current_next_indicator(1)+reserved(2)+version(5)";
    case 7: return "reserved(7)+marker_bit(1)";
    case 8: return "program_stream_info_length 高（=0，无描述子）";
    case 9: return "program_stream_info_length 低";
    case 10: return "elementary_stream_map_length 高（=流地图总字节数）";
    case 11: return "elementary_stream_map_length 低";
    default:
        if (i == 12) return "ES条目1: stream_type=0x1B（H.264）";
        if (i == 13) return "ES条目1: elementary_stream_id=0xE0";
        if (i == 14 || i == 15) return "ES条目1: elementary_stream_info_length=0x0000";
        if (has_audio && i == 16) return "ES条目2: stream_type=0x90（G.711A）";
        if (has_audio && i == 17) return "ES条目2: elementary_stream_id=0xC0";
        if (has_audio && (i == 18 || i == 19)) return "ES条目2: elementary_stream_info_length=0x0000";
        return "CRC_32（很多实现直接写 0）";
    }
}

static void dump_psm(const char *tag, const unsigned char *p, size_t n, int has_audio)
{
    size_t i, counted = 0;
    unsigned len = (unsigned)((p[4] << 8) | p[5]);
    unsigned esmap = (unsigned)((p[10] << 8) | p[11]);

    printf("\n---- %s（%zu 字节）----\n", tag, n);
    for (i = 0; i < n; i++) {
        int in = (i >= 6);
        if (in) counted++;
        printf("  偏移 %02zu  %02X  %-56s %s\n", i, p[i], field_name(i, has_audio),
               in ? "→ 计入 length" : "（不计入）");
    }
    printf("  ── 数一遍：计入 length 的字节 = %zu；总长 %zu − 6 = %zu\n", counted, n, n - 6);
    printf("  ── length 字段实际写的是 0x%04X（%u）→ %s\n", len, len,
           ((size_t)len == n - 6) ? "一致 ✓" : "不一致 ✗");
    printf("  ── elementary_stream_map_length = 0x%04X（%u）= %u 条 × 4 字节\n",
           esmap, esmap, esmap / 4);
}

int main(void)
{
    char reason[200] = "";
    unsigned v_len, av_len, v_es, av_es;
    int fail = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=========== kata 04：PSM 的 packet_length = 总长 − 6 ===========\n");

    dump_psm("单视频 PSM", PSM_V, sizeof PSM_V, 0);
    dump_psm("视频+音频 PSM", PSM_AV, sizeof PSM_AV, 1);

    printf("\n--------------- 为什么「PSM 里没声明的 PES 会被丢掉」 ---------------\n");
    printf("  解码顺序：PSM 先到 → 建 stream_id 表（0xE0→H.264，0xC0→G.711A）\n");
    printf("            PES 后到 → 用 PES 的 stream_id 去查表 → 查到才交给对应 track\n");
    printf("  查不到就没有能投递的 track（不知道按 H.264 还是 G.711A 解），只能跳过。\n");
    printf("  两个典型翻车现场：\n");
    printf("    ① PSM 声明 es_id=0xE0，PES 用 0xE1 → 平台收到 RTP 但一个视频帧都没有；\n");
    printf("    ② es_map_length 写成 0（等于声明零条流）→ 所有 PES 全被丢，拉流黑屏。\n");

    v_len  = (unsigned)((PSM_V[4] << 8) | PSM_V[5]);
    av_len = (unsigned)((PSM_AV[4] << 8) | PSM_AV[5]);
    v_es   = (unsigned)((PSM_V[10] << 8) | PSM_V[11]);
    av_es  = (unsigned)((PSM_AV[10] << 8) | PSM_AV[11]);

    printf("\n--------------- 断言 ---------------\n");
    printf("  单视频：总长 %zu，length=0x%04X，总长-6=0x%04X\n",
           sizeof PSM_V, v_len, (unsigned)(sizeof PSM_V - 6));
    printf("  含音频：总长 %zu，length=0x%04X，总长-6=0x%04X，es_map=%u\n",
           sizeof PSM_AV, av_len, (unsigned)(sizeof PSM_AV - 6), av_es);
    if (v_len != 0x000E || v_len != sizeof PSM_V - 6) {
        fail = 1; snprintf(reason, sizeof reason, "单视频 PSM length 应为 0x000E，实际 0x%04X", v_len);
    } else if (av_len != 0x0012 || av_len != sizeof PSM_AV - 6) {
        fail = 1; snprintf(reason, sizeof reason, "含音频 PSM length 应为 0x0012，实际 0x%04X", av_len);
    } else if (av_es != 0x0008 || v_es != 0x0004) {
        fail = 1; snprintf(reason, sizeof reason, "es_map_length 应为 0x0008/0x0004，实际 0x%04X/0x%04X", av_es, v_es);
    } else if (memcmp(PSM_V + 12, "\x1B\xE0\x00\x00", 4) != 0 ||
               memcmp(PSM_AV + 16, "\x90\xC0\x00\x00", 4) != 0) {
        fail = 1; snprintf(reason, sizeof reason, "ES 条目的 stream_type/stream_id 不对");
    }
    if (fail) { printf("SELF-CHECK FAIL: %s\n", reason); return 1; }
    printf("SELF-CHECK PASS\n");
    return 0;
}
