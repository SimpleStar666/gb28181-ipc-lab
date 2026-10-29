/*
 * test_annexb.c — H.264 AnnexB 起始码扫描与 NAL 类型
 * ============================================================
 * AnnexB 裸流用 00 00 01（3 字节）或 00 00 00 01（4 字节）分隔 NAL。
 * x264 会在 SPS/PPS/IDR 前用 4 字节、其余用 3 字节，所以两种都必须支持。
 *
 * 这一层最经典的 bug：
 *   先匹配 3 字节、再匹配 4 字节 → 在 00 00 00 01 上返回偏移 +1、sc_len 3，
 *   于是每个 NAL 前面都多留一个 0x00 字节。
 *   下游解码器往往「能解但偶尔报错」，同样属于肉眼看不见的错误。
 *   所以本测试强制断言：00 00 00 01 必须被识别成 offset=0 / sc_len=4。
 *
 * 另测：
 *   - 前缀垃圾（真实码流从 FIFO 读进来时前面可能带残留）能跳过并找回起始码；
 *   - NAL 类型 = 起始码之后第一个字节的低 5 位；
 *   - is_idr / is_vcl 的分类；
 *   - 找不到起始码时返回 -1（而不是 0 或 len）。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "gb_annexb.h"

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL: ");                                               \
            printf(__VA_ARGS__);                                            \
            fflush(stdout);                                                 \
            return 1;                                                       \
        }                                                                   \
    } while (0)

/* 混合 4 字节 / 3 字节起始码的一段数据：
 *   off  0 : 00 00 00 01 | 67 ...   SPS    (sc_len 4)
 *   off  7 : 00 00 01    | 68 ..    PPS    (sc_len 3)
 *   off 12 : 00 00 01    | 65 ...   IDR    (sc_len 3)
 *   off 18 : 00 00 00 01 | 06 ..    SEI    (sc_len 4)
 *   off 24 : 00 00 01    | 41 ..    非 IDR (sc_len 3)
 */
static const uint8_t mixed[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0xAA, 0xBB,
    0x00, 0x00, 0x01,       0x68, 0xCC,
    0x00, 0x00, 0x01,       0x65, 0xDD, 0xEE,
    0x00, 0x00, 0x00, 0x01, 0x06, 0x11,
    0x00, 0x00, 0x01,       0x41, 0x22
};
#define MIXED_LEN (sizeof(mixed))

#define MIXED_EXPECT_SC 5

static const size_t expect_off[MIXED_EXPECT_SC] = { 0, 7, 12, 18, 24 };
static const size_t expect_sclen[MIXED_EXPECT_SC] = { 4, 3, 3, 4, 3 };
static const int    expect_type[MIXED_EXPECT_SC]  = { 7, 8, 5, 6, 1 };

int main(void)
{
    int rc, i;

    /* ================= 1. 逐个起始码：偏移 + sc_len + NAL 类型 ================= */
    for (i = 0; i < MIXED_EXPECT_SC; i++) {
        size_t from = (i == 0) ? 0 : expect_off[i - 1] + expect_sclen[i - 1];
        size_t slen = 0;
        rc = gb_annexb_find_startcode(mixed, MIXED_LEN, from, &slen);
        CHECK(rc == (int)expect_off[i],
              "从 %zu 起找第 %d 个起始码: 得到偏移 %d, expected %zu\n",
              from, i, rc, expect_off[i]);
        CHECK(slen == expect_sclen[i],
              "第 %d 个起始码 sc_len = %zu, expected %zu"
              "   <- 00 00 00 01 必须被识别成 4 字节（先匹配 4 字节再匹配 3 字节）\n",
              i, slen, expect_sclen[i]);

        rc = gb_annexb_nal_type(mixed + expect_off[i], expect_sclen[i]);
        CHECK(rc == expect_type[i],
              "第 %d 个 NAL 类型 = %d, expected %d (字节 0x%02X)\n",
              i, rc, expect_type[i], mixed[expect_off[i] + expect_sclen[i]]);
    }

    /* 全部起始码走完之后必须是 -1 */
    {
        size_t slen = 0;
        rc = gb_annexb_find_startcode(mixed, MIXED_LEN, 27, &slen);
        CHECK(rc == -1, "末尾之后仍报起始码: 偏移 %d\n", rc);
    }

    /* ================= 2. 4 字节起始码必须优先于 3 字节 ================= */
    {
        static const uint8_t four[] = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x42 };
        size_t slen = 99;
        rc = gb_annexb_find_startcode(four, sizeof(four), 0, &slen);
        CHECK(rc == 0, "00 00 00 01 应返回 offset 0, got %d"
                       "   <- 先匹配 3 字节会错报 offset 1\n", rc);
        CHECK(slen == 4, "00 00 00 01 的 sc_len = %zu, expected 4\n", slen);
        CHECK(gb_annexb_nal_type(four, slen) == 7, "NAL 类型应为 7(SPS), got %d\n",
              gb_annexb_nal_type(four, slen));
    }
    {
        /* 00 00 00 00 01：正确结果是在偏移 1 处找到一个 4 字节起始码 */
        static const uint8_t zeros[] = { 0x00, 0x00, 0x00, 0x00, 0x01, 0x67 };
        size_t slen = 0;
        rc = gb_annexb_find_startcode(zeros, sizeof(zeros), 0, &slen);
        CHECK(rc == 1 && slen == 4,
              "00 00 00 00 01 应从偏移 1 找到 4 字节起始码, got off=%d sc_len=%zu\n",
              rc, slen);
    }

    /* ================= 3. 前缀垃圾里找回起始码 ================= */
    {
        static const uint8_t junk4[] = {
            0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x34,   /* 6 字节残留 */
            0x00, 0x00, 0x00, 0x01, 0x67, 0x64
        };
        size_t slen = 0;
        rc = gb_annexb_find_startcode(junk4, sizeof(junk4), 0, &slen);
        CHECK(rc == 6, "带 6 字节垃圾前缀: 偏移 = %d, expected 6\n", rc);
        CHECK(slen == 4, "带垃圾前缀时 sc_len = %zu, expected 4\n", slen);
        CHECK(gb_annexb_nal_type(junk4 + 6, slen) == 7, "垃圾前缀后 NAL 类型应为 7\n");

        {
            static const uint8_t junk3[] = {
                0xFF, 0xFE, 0xFD,
                0x00, 0x00, 0x01, 0x65, 0x88
            };
            slen = 0;
            rc = gb_annexb_find_startcode(junk3, sizeof(junk3), 0, &slen);
            CHECK(rc == 3 && slen == 3,
                  "3 字节起始码带垃圾前缀: off=%d sc_len=%zu, expected 3/3\n", rc, slen);
            CHECK(gb_annexb_nal_type(junk3 + 3, slen) == 5, "应为 IDR(5)\n");
        }
    }

    /* ================= 4. 没有起始码 / 数据太短 ================= */
    {
        static const uint8_t none[] = { 0x11, 0x22, 0x00, 0x00, 0x33, 0x44 };
        size_t slen = 0;
        rc = gb_annexb_find_startcode(none, sizeof(none), 0, &slen);
        CHECK(rc == -1, "没有起始码时应返回 -1, got %d\n", rc);

        {
            static const uint8_t two[] = { 0x00, 0x00 };
            slen = 0;
            rc = gb_annexb_find_startcode(two, sizeof(two), 0, &slen);
            CHECK(rc == -1, "只有 00 00 时应返回 -1, got %d\n", rc);
        }
        {
            /* 起始码被 FIFO 边界截断的典型情形：以 00 00 00 结尾 */
            static const uint8_t tail[] = { 0x67, 0x00, 0x00, 0x00 };
            slen = 0;
            rc = gb_annexb_find_startcode(tail, sizeof(tail), 0, &slen);
            CHECK(rc == -1, "尾部不完整起始码应返回 -1, got %d\n", rc);
        }
        {
            /* 从超出长度的地方开始找 */
            size_t slen = 0;
            rc = gb_annexb_find_startcode(mixed, MIXED_LEN, MIXED_LEN + 10, &slen);
            CHECK(rc == -1, "from 超出长度应返回 -1, got %d\n", rc);
        }
    }

    /* ================= 5. is_idr / is_vcl 分类 ================= */
    CHECK(gb_annexb_is_idr(5) == 1, "is_idr(5) 应为真\n");
    for (i = 0; i <= 4; i++)
        CHECK(gb_annexb_is_idr(i) == 0, "is_idr(%d) 应为假\n", i);
    for (i = 6; i <= 12; i++)
        CHECK(gb_annexb_is_idr(i) == 0, "is_idr(%d) 应为假\n", i);

    for (i = 1; i <= 5; i++)
        CHECK(gb_annexb_is_vcl(i) == 1, "is_vcl(%d) 应为真（VCL 图像数据）\n", i);
    CHECK(gb_annexb_is_vcl(0) == 0, "is_vcl(0) 应为假\n");
    CHECK(gb_annexb_is_vcl(6) == 0, "is_vcl(6)=SEI 应为假\n");
    CHECK(gb_annexb_is_vcl(7) == 0, "is_vcl(7)=SPS 应为假\n");
    CHECK(gb_annexb_is_vcl(8) == 0, "is_vcl(8)=PPS 应为假\n");
    CHECK(gb_annexb_is_vcl(9) == 0, "is_vcl(9)=AUD 应为假\n");

    /* 遍历整段数据：确认 5 个起始码的顺序与类型都是预期的 */
    {
        size_t pos = 0, slen = 0;
        int k = 0;
        while (1) {
            int r = gb_annexb_find_startcode(mixed, MIXED_LEN, pos, &slen);
            if (r < 0)
                break;
            CHECK(k < MIXED_EXPECT_SC, "扫出了超过 %d 个起始码（偏移 %d）\n",
                  MIXED_EXPECT_SC, r);
            CHECK((size_t)r == expect_off[k] && slen == expect_sclen[k],
                  "第 %d 个起始码 off=%d sc_len=%zu, expected %zu/%zu\n",
                  k, r, slen, expect_off[k], expect_sclen[k]);
            CHECK(gb_annexb_nal_type(mixed + r, slen) == expect_type[k],
                  "第 %d 个 NAL 类型 = %d, expected %d\n",
                  k, gb_annexb_nal_type(mixed + r, slen), expect_type[k]);
            pos = (size_t)r + slen;
            k++;
        }
        CHECK(k == MIXED_EXPECT_SC, "整段扫描只找到 %d 个起始码, expected %d\n",
              k, MIXED_EXPECT_SC);
    }

    printf("PASS: AnnexB 起始码（3/4 字节优先级）偏移与 sc_len 全对、"
           "NAL 类型 7/8/5/6/1 正确、垃圾前缀能跳过、无起始码返回 -1\n");
    return 0;
}
