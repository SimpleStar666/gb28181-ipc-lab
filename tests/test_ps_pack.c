/*
 * test_ps_pack.c — gb_ps_pack_video / gb_ps_pack_audio / gb_ps_write_pes_header
 * ============================================================
 * 一帧 PS 的四层结构必须**严格按顺序、逐字节**拼出来：
 *
 *   [ 0..13]  Pack Header     14 字节  00 00 01 BA 44 ...
 *   [14..29]  System Header   16 字节  00 00 01 BB ...
 *   [30..49]  PSM             20/24 字节 00 00 01 BC ...（单视频 / 含音频）
 *   [50..  ]  PES             14 字节头 + 载荷
 *
 * 本测试断言：
 *   1. 前三层就是 gb_ps.h 里那几个 extern const 常量本身（逐字节 memcmp），
 *      并且额外核对它们的起始码，防止「实现自造一份错常量」这种情况互相印证；
 *   2. PES 头 14 字节：00 00 01 E0/C0 | PES_packet_length | flags | 0x05 | PTS(5B)；
 *   3. PES_packet_length == payload_len + 8
 *      （8 = 3 字节 flag/长度字段 + 5 字节 PTS，这是 PES 长度字段的定义）；
 *   4. 返回值 == 各层长度之和，载荷被原样拷贝；
 *   5. 缓冲不足返回 GB_ERR（不能写越界）；
 *   6. 音频包不带 System Header / PSM（只在 Pack Header 之后直接跟 PES，省带宽）。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "gb_ps.h"
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

static void dump_head(const char *name, const uint8_t *p, size_t n)
{
    size_t i;
    printf("  %s (%zu):", name, n);
    for (i = 0; i < n; i++)
        printf(" %02X", p[i]);
    printf("\n");
}

/* 一段「像样」的假视频帧：SPS + PPS + IDR slice，都用 4 字节起始码 */
static const uint8_t fake_frame[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1E, 0xAC, 0xD9, 0x40, 0xA0,
    0x2F, 0xF9, 0x70, 0x11, 0x00, 0x00, 0x03, 0x03, 0xE9, 0x00, 0x00, 0xEA,
    0x60, 0x0F, 0x16, 0x2D, 0x96,
    0x00, 0x00, 0x00, 0x01, 0x68, 0xEB, 0xEC, 0xB2, 0x2C,
    0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x0A, 0xFF, 0x01, 0x02,
    0x03, 0x04, 0x05
};

#define FRAME_LEN (sizeof(fake_frame))
#define PS_OVERHEAD(psm_len) (14u + 16u + (psm_len) + 14u)

static int check_pes_header(const uint8_t *pes, uint8_t sid, size_t payload_len,
                            uint64_t pts, int idr, const char *what)
{
    uint8_t pts_expect[5];
    uint16_t plen;
    char got[64];

    /* 注意 idr 参数：本项目的硬性字节不变量是 PES 头恒为 `80 80 05`
     * （docs/06-MPEG-PS打包器.md / docs/11-排障手册.md 都按这三个字节描述，
     *  原型与多数国标实现的线上字节也是它），所以 gb_ps_write_pes_header
     * 显式忽略 idr —— 这里传 -1 表示不约束调用方怎么用该参数。 */
    (void)idr;

    if (memcmp(pes, "\x00\x00\x01", 3) != 0) {
        printf("FAIL: %s PES 起始码不是 00 00 01（got %02X %02X %02X）\n",
               what, pes[0], pes[1], pes[2]);
        return 1;
    }
    if (pes[3] != sid) {
        printf("FAIL: %s PES stream_id = 0x%02X, expected 0x%02X\n", what, pes[3], sid);
        return 1;
    }
    plen = (uint16_t)((pes[4] << 8) | pes[5]);
    if (plen != payload_len + 8) {
        printf("FAIL: %s PES_packet_length = %u, expected payload(%zu) + 8 = %zu\n",
               what, plen, payload_len, payload_len + 8);
        return 1;
    }
    if (pes[6] != 0x80) {
        printf("FAIL: %s PES flags1 = 0x%02X, expected 0x80"
               "（'10' + 各标志位 0；本项目不置 data_alignment_indicator）\n", what, pes[6]);
        return 1;
    }
    if (pes[7] != 0x80) {
        printf("FAIL: %s PES flags2 = 0x%02X, expected 0x80 (PTS_DTS_flags='10', 只带 PTS)\n",
               what, pes[7]);
        return 1;
    }
    if (pes[8] != 0x05) {
        printf("FAIL: %s PES_header_data_length = %u, expected 5（一个 PTS 的长度）\n",
               what, pes[8]);
        return 1;
    }
    gb_ps_encode_pts(pts_expect, pts);
    if (memcmp(pes + 9, pts_expect, 5) != 0) {
        snprintf(got, sizeof(got), "%02X %02X %02X %02X %02X",
                 pes[9], pes[10], pes[11], pes[12], pes[13]);
        printf("FAIL: %s PES 里的 PTS = [%s], expected [%02X %02X %02X %02X %02X] (pts=%llu)\n",
               what, got, pts_expect[0], pts_expect[1], pts_expect[2], pts_expect[3],
               pts_expect[4], (unsigned long long)pts);
        return 1;
    }
    return 0;
}

int main(void)
{
    static uint8_t out[GB_PS_MAX_PACKET];
    uint8_t audio[160];
    int n, i;
    const uint64_t pts = 90000;

    memset(audio, 0x5A, sizeof(audio));

    /* ================= 1. gb_ps_write_pes_header：14 字节精确契约 ================= */
    {
        static const uint8_t expect_video[14] = {
            0x00, 0x00, 0x01, 0xE0, 0x03, 0xF0, 0x80, 0x80, 0x05,
            0x21, 0x00, 0x05, 0xBF, 0x21   /* PTS = 90000 */
        };
        uint8_t hdr[14];
        int w = gb_ps_write_pes_header(hdr, GB_PS_SID_VIDEO, 1000, 90000, 1);
        CHECK(w == 14, "gb_ps_write_pes_header 返回 %d, expected 14\n", w);
        if (memcmp(hdr, expect_video, 14) != 0) {
            printf("FAIL: write_pes_header 14 字节与冻结契约不一致\n");
            dump_head("actual  ", hdr, 14);
            dump_head("expected", expect_video, 14);
            return 1;
        }
        CHECK(hdr[0] == 0x00 && hdr[1] == 0x00 && hdr[2] == 0x01 && hdr[3] == 0xE0,
              "write_pes_header 起始码/sid 错: %02X %02X %02X %02X\n",
              hdr[0], hdr[1], hdr[2], hdr[3]);
        CHECK(((hdr[4] << 8) | hdr[5]) == 1000 + 8,
              "write_pes_header PES_packet_length = %u, expected %u\n",
              (hdr[4] << 8) | hdr[5], 1000 + 8);
        CHECK(hdr[6] == 0x80 && hdr[7] == 0x80 && hdr[8] == 0x05,
              "write_pes_header flags = %02X %02X %02X, expected 80 80 05"
              "（'10'+全 0 标志 / 只带 PTS / PTS 占 5 字节）\n",
              hdr[6], hdr[7], hdr[8]);

        /* idr 参数：本项目按硬性字节不变量显式忽略它，两种取值必须产出同样的头 */
        {
            uint8_t hdr1[14];
            gb_ps_write_pes_header(hdr1, GB_PS_SID_VIDEO, 1000, 90000, 0);
            CHECK(memcmp(hdr, hdr1, 14) == 0,
                  "idr=0/1 产出的 PES 头不同（本项目要求恒为 80 80 05 的固定字节）\n");
        }

        w = gb_ps_write_pes_header(hdr, GB_PS_SID_AUDIO, 160, 1800, 0);
        CHECK(w == 14, "write_pes_header(audio) 返回 %d, expected 14\n", w);
        CHECK(hdr[3] == 0xC0, "write_pes_header(audio) stream_id = 0x%02X, expected 0xC0\n", hdr[3]);
        CHECK(hdr[6] == 0x80 && hdr[7] == 0x80 && hdr[8] == 0x05,
              "write_pes_header(audio) flags = %02X %02X %02X, expected 80 80 05\n",
              hdr[6], hdr[7], hdr[8]);
        CHECK(((hdr[4] << 8) | hdr[5]) == 160 + 8,
              "write_pes_header(audio) PES_packet_length = %u, expected 168\n",
              (hdr[4] << 8) | hdr[5]);

        /* PTS 段也逐字节核对一次（pts=1800 -> 21 00 01 0E 11） */
        CHECK(hdr[9] == 0x21 && hdr[10] == 0x00 && hdr[11] == 0x01 &&
              hdr[12] == 0x0E && hdr[13] == 0x11,
              "write_pes_header(audio, pts=1800) PTS 字节 = %02X %02X %02X %02X %02X,"
              " expected 21 00 01 0E 11\n",
              hdr[9], hdr[10], hdr[11], hdr[12], hdr[13]);
    }

    /* ================= 2. 单视频：四层顺序 + 逐字节 ================= */
    n = gb_ps_pack_video(out, sizeof(out), fake_frame, FRAME_LEN, pts,
                         GB_PS_PSM_VIDEO, sizeof(GB_PS_PSM_VIDEO));
    CHECK(n > 0, "gb_ps_pack_video 返回 %d（<0 表示失败）\n", n);
    CHECK((size_t)n == PS_OVERHEAD(sizeof(GB_PS_PSM_VIDEO)) + FRAME_LEN,
          "gb_ps_pack_video 总长 = %d, expected %zu (= 14 Pack + 16 Sys + 20 PSM + 14 PES + %zu payload)\n",
          n, PS_OVERHEAD(20) + FRAME_LEN, FRAME_LEN);

    if (memcmp(out, GB_PS_PACK_HEADER, 14) != 0) {
        printf("FAIL: 输出开头 14 字节 != GB_PS_PACK_HEADER\n");
        dump_head("actual  ", out, 14);
        dump_head("expected", GB_PS_PACK_HEADER, 14);
        return 1;
    }
    CHECK(memcmp(out, "\x00\x00\x01\xBA", 4) == 0, "Pack Header 起始码错\n");

    if (memcmp(out + 14, GB_PS_SYSTEM_HEADER, 16) != 0) {
        printf("FAIL: 输出 [14..29] != GB_PS_SYSTEM_HEADER\n");
        dump_head("actual  ", out + 14, 16);
        dump_head("expected", GB_PS_SYSTEM_HEADER, 16);
        return 1;
    }
    CHECK(memcmp(out + 14, "\x00\x00\x01\xBB", 4) == 0, "System Header 起始码错\n");

    if (memcmp(out + 30, GB_PS_PSM_VIDEO, 20) != 0) {
        printf("FAIL: 输出 [30..49] != GB_PS_PSM_VIDEO\n");
        dump_head("actual  ", out + 30, 20);
        dump_head("expected", GB_PS_PSM_VIDEO, 20);
        return 1;
    }
    CHECK(memcmp(out + 30, "\x00\x00\x01\xBC", 4) == 0, "PSM 起始码错\n");

    CHECK(check_pes_header(out + 50, GB_PS_SID_VIDEO, FRAME_LEN, pts, -1,
                           "video") == 0,
          "视频 PES 头校验失败\n");
    CHECK(memcmp(out + 50 + 14, fake_frame, FRAME_LEN) == 0,
          "PES 之后的载荷与输入帧不一致（没原样拷贝 / 长度算错）\n");

    /* ================= 3. 音视频 PSM：长度随之 +4 ================= */
    n = gb_ps_pack_video(out, sizeof(out), fake_frame, FRAME_LEN, pts,
                         GB_PS_PSM_AV, sizeof(GB_PS_PSM_AV));
    CHECK(n > 0, "gb_ps_pack_video(PSM_AV) 返回 %d\n", n);
    CHECK((size_t)n == PS_OVERHEAD(sizeof(GB_PS_PSM_AV)) + FRAME_LEN,
          "PSM_AV 时总长 = %d, expected %zu\n", n,
          PS_OVERHEAD(sizeof(GB_PS_PSM_AV)) + FRAME_LEN);
    CHECK(memcmp(out + 30, GB_PS_PSM_AV, sizeof(GB_PS_PSM_AV)) == 0,
          "PSM_AV 模式下 [30..] 不是 GB_PS_PSM_AV\n");
    CHECK(check_pes_header(out + 30 + sizeof(GB_PS_PSM_AV), GB_PS_SID_VIDEO,
                           FRAME_LEN, pts, -1, "video(AV)") == 0,
          "PSM_AV 模式下的 PES 头校验失败\n");

    /* ================= 4. 音频包：Pack Header + PES，且 sid == 0xC0 ================= */
    n = gb_ps_pack_audio(out, sizeof(out), audio, sizeof(audio), 1800);
    CHECK(n > 0, "gb_ps_pack_audio 返回 %d\n", n);
    CHECK((size_t)n == 14 + 14 + sizeof(audio),
          "gb_ps_pack_audio 总长 = %d, expected %zu (= 14 Pack + 14 PES + %zu payload)"
          "   <- 音频包不应带 System Header / PSM\n",
          n, 14 + 14 + sizeof(audio), sizeof(audio));
    CHECK(memcmp(out, GB_PS_PACK_HEADER, 14) == 0, "音频包开头不是 Pack Header\n");
    CHECK(out[14] == 0x00 && out[15] == 0x00 && out[16] == 0x01 && out[17] == 0xC0,
          "音频 PES 起始码/sid = %02X %02X %02X %02X, expected 00 00 01 C0\n",
          out[14], out[15], out[16], out[17]);
    CHECK(check_pes_header(out + 14, GB_PS_SID_AUDIO, sizeof(audio), 1800, 0,
                           "audio") == 0,
          "音频 PES 头校验失败\n");
    CHECK(memcmp(out + 28, audio, sizeof(audio)) == 0,
          "音频载荷与输入不一致\n");

    /* ================= 5. 缓冲不足必须返回 GB_ERR，且不越界写 ================= */
    {
        static uint8_t small[64];
        memset(small, 0xA5, sizeof(small));
        n = gb_ps_pack_video(small, sizeof(small), fake_frame, FRAME_LEN, pts,
                             GB_PS_PSM_VIDEO, sizeof(GB_PS_PSM_VIDEO));
        CHECK(n == GB_ERR, "缓冲不足时 gb_ps_pack_video 返回 %d, expected GB_ERR(%d)\n",
              n, GB_ERR);
        for (i = 0; i < (int)sizeof(small); i++)
            CHECK(small[i] == 0xA5,
                  "缓冲不足时发生了越界写: small[%d] = 0x%02X（应为哨兵 0xA5）\n",
                  i, small[i]);

        memset(small, 0xA5, sizeof(small));
        n = gb_ps_pack_audio(small, 8, audio, sizeof(audio), 0);
        CHECK(n == GB_ERR, "缓冲不足时 gb_ps_pack_audio 返回 %d, expected GB_ERR\n", n);
    }

    /* ================= 6. PTS 决定 PES 里的 5 字节（换一个值再验一次） ================= */
    {
        uint64_t pts2 = 6000;
        n = gb_ps_pack_video(out, sizeof(out), fake_frame, FRAME_LEN, pts2,
                             GB_PS_PSM_VIDEO, sizeof(GB_PS_PSM_VIDEO));
        CHECK(n > 0, "gb_ps_pack_video(pts=6000) 返回 %d\n", n);
        CHECK(check_pes_header(out + 50, GB_PS_SID_VIDEO, FRAME_LEN, pts2, -1,
                               "video(pts=6000)") == 0,
              "pts=6000 时 PES 头校验失败\n");
        CHECK(out[50 + 9] == 0x21 && out[50 + 12] == 0x2E && out[50 + 13] == 0xE1,
              "pts=6000 时 PES 内 PTS 字节 = %02X %02X %02X %02X %02X, expected 21 00 01 2E E1\n",
              out[59], out[60], out[61], out[62], out[63]);
    }

    printf("PASS: PS 四层拼接（Pack/Sys/PSM/PES）逐字节一致，"
           "PES_packet_length==payload+8，音频包 sid==0xC0 且不带 Sys/PSM\n");
    return 0;
}
