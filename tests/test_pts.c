/*
 * test_pts.c — MPEG-PS 的 33bit PTS 位域编码
 * ============================================================
 * 【本项目存在的理由之一】
 *   原版实现的 byte2 用了 `>> 15`，正确是 `>> 14`。
 *   因为下游（ZLMediaKit 等）多数路径不依赖 PES 的 PTS、只按 RTP 时间戳走，
 *   画面照样能解码出来 —— 所以这个 bug 活了好几个月没人发现。
 *
 *   本文件用两种互相独立的方式把它钉死：
 *     A. 三个「肉眼可核对」的固定向量（pts=0 / 6000 / 90000），
 *        其中 pts=90000 的 byte2 必须是 0x05；写成 >>15 会得到 0x03。
 *     B. 往返校验：把 5 个字节按 ISO/IEC 13818-1 的 PTS 位域表
 *        逐位还原成 33bit，断言 == 输入。这条覆盖全部 33 位，
 *        比几个孤立向量有力得多（任何一位错位都会在某个测试点上暴露）。
 *
 *   ISO 13818-1 PES PTS 位域（33 位铺在 5 字节里，带 4 个标记位）：
 *     byte0 = 0010 | P32 P31 P30 | marker(1)
 *     byte1 = P29 .. P22
 *     byte2 = P21 .. P15 | marker(1)
 *     byte3 = P14 .. P7
 *     byte4 = P6  .. P0  | marker(1)
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

static void dump5(char *dst, size_t cap, const uint8_t b[5])
{
    snprintf(dst, cap, "%02X %02X %02X %02X %02X", b[0], b[1], b[2], b[3], b[4]);
}

/* ---- 独立的解码器：严格按 ISO/IEC 13818-1 PES 的位域表逐位还原 ----
 * 注意：这里刻意不抄 gb_ps.c 的公式，而是按「第几个 bit」来写，
 * 这样才和编码器构成真正的交叉验证。 */
static uint64_t pts_decode_iso(const uint8_t b[5])
{
    uint64_t p = 0;

    /* byte0: 低 4 位里的高 3 位是 P32 P31 P30（bit1..bit3） */
    p |= (uint64_t)((b[0] >> 1) & 0x07) << 30;

    /* byte1: 8 位全用 */
    p |= (uint64_t)b[1] << 22;

    /* byte2: 高 7 位是 P21..P15 */
    p |= (uint64_t)((b[2] >> 1) & 0x7F) << 15;

    /* byte3: 8 位全用 */
    p |= (uint64_t)b[3] << 7;

    /* byte4: 高 7 位是 P6..P0 */
    p |= (uint64_t)((b[4] >> 1) & 0x7F);

    return p & GB_PTS_MASK;
}

/* 编码后 4 个 marker 位必须都在（这是解码器用来定位的同步位） */
static int markers_ok(const uint8_t b[5])
{
    return (b[0] & 0x01) == 0x01 &&
           (b[2] & 0x01) == 0x01 &&
           (b[4] & 0x01) == 0x01 &&
           (b[0] & 0xF0) == 0x20;   /* 前缀 0010 */
}

int main(void)
{
    uint8_t b[5];
    char got[32], want[32];
    size_t i;

    /* ---------- A. 固定向量 ---------- */
    static const struct { uint64_t pts; const char *hex; } vec[] = {
        { 0,      "21 00 01 00 01" },   /* 全 0：四个 marker 位可见 */
        { 6000,   "21 00 01 2E E1" },   /* 15fps 一帧步进 */
        { 90000,  "21 00 05 BF 21" },   /* 1 秒：byte2 必须是 0x05（>>15 会得 0x03） */
        { 0x1FFFFFFFFULL, "2F FF FF FF FF" }, /* 33bit 全 1，位域边界 */
    };

    for (i = 0; i < sizeof(vec) / sizeof(vec[0]); i++) {
        gb_ps_encode_pts(b, vec[i].pts);
        dump5(got, sizeof(got), b);
        CHECK(strcmp(got, vec[i].hex) == 0,
              "gb_ps_encode_pts(%llu) = [%s], expected [%s]\n",
              (unsigned long long)vec[i].pts, got, vec[i].hex);
        CHECK(markers_ok(b), "pts=%llu 的 5 字节里 marker/前缀位不对: [%s]\n",
              (unsigned long long)vec[i].pts, got);
    }

    /* 单独把「经典错误点」再强调一次：byte2 用了 >>15 就会是 0x03 */
    gb_ps_encode_pts(b, 90000);
    CHECK(b[2] == 0x05,
          "pts=90000 -> byte2 = 0x%02X, expected 0x05"
          "   <- 若为 0x03 说明 byte2 写成了 (pts>>15) 而不是 (pts>>14)\n",
          b[2]);

    /* ---------- B. 往返解码校验（覆盖 33 位的每一位） ---------- */
    static const uint64_t round[] = {
        0, 1, 2, 127, 128, 16383, 16384, 32767, 32768,
        90000, 900000, 0x1FFFFFFFFULL,
        /* 再加几个「只动某一位」的用例，逐位证明位域没错位 */
        0x1ULL, 0x2ULL, 0x4ULL, 0x8ULL, 0x10ULL, 0x20ULL, 0x40ULL,
        0x80ULL, 0x100ULL, 0x200ULL, 0x400ULL, 0x800ULL, 0x1000ULL,
        0x2000ULL, 0x4000ULL, 0x8000ULL, 0x10000ULL, 0x20000ULL,
        0x40000ULL, 0x80000ULL, 0x100000ULL, 0x200000ULL, 0x400000ULL,
        0x800000ULL, 0x1000000ULL, 0x2000000ULL, 0x4000000ULL,
        0x8000000ULL, 0x10000000ULL, 0x20000000ULL, 0x40000000ULL,
        0x80000000ULL, 0x100000000ULL, 0x1F0000000ULL,
        GB_PTS_MASK, GB_PTS_MASK - 1, GB_PTS_MASK ^ 0x55555555ULL,
        0x0AAAAAAAAULL, 0x055555555ULL, 0x155555555ULL,
    };

    for (i = 0; i < sizeof(round) / sizeof(round[0]); i++) {
        uint64_t back;
        gb_ps_encode_pts(b, round[i]);
        back = pts_decode_iso(b);
        dump5(got, sizeof(got), b);
        snprintf(want, sizeof(want), "%llu", (unsigned long long)round[i]);
        CHECK(back == (round[i] & GB_PTS_MASK),
              "往返校验失败: pts=%s -> [%s] -> 解出 %llu"
              "   <- 5 字节位域与 ISO 13818-1 不一致\n",
              want, got, (unsigned long long)back);
    }

    /* ---------- C. 5 字节里不允许有多余的脏位 ---------- */
    for (i = 0; i < sizeof(round) / sizeof(round[0]); i++) {
        gb_ps_encode_pts(b, round[i]);
        CHECK((b[0] & 0xF0) == 0x20, "pts=%llu: byte0 高 4 位应为 0010, got 0x%02X\n",
              (unsigned long long)round[i], b[0]);
    }

    printf("PASS: PTS 编码 %d 组固定向量 + %d 组往返解码 全部一致"
           "（byte2 用 >>14，pts=90000 -> 0x05）\n",
           (int)(sizeof(vec) / sizeof(vec[0])),
           (int)(sizeof(round) / sizeof(round[0])));
    return 0;
}
