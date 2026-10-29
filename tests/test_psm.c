/*
 * test_psm.c — PS 里三块固定头部的逐字节契约
 * ============================================================
 * GB_PS_PACK_HEADER / GB_PS_SYSTEM_HEADER / GB_PS_PSM_VIDEO / GB_PS_PSM_AV
 * 都是 extern const 数组，尺寸在 gb_ps.h 里就被 `[14] / [16] / [20] / [24]`
 * 钉死了（写错长度编译期就过不去）。本测试做三件事：
 *
 *   1. 单视频 PSM 必须**逐字节**等于国标实现的经典 20 字节
 *      （这 20 字节是 extern 契约的一部分，任何一位改动都必须在这里被拦住）。
 *   2. 音视频 PSM 的结构字段：packet_length==0x0012、es_map_length==0x0008、
 *      含 (0x1B,0xE0) 与 (0x90,0xC0) 两条 elementary stream 描述。
 *   3. 长度自洽规律：packet_length == 该头总长 − 6
 *      （4 字节起始码 + 2 字节 length 字段本身不计入 length，
 *        这是 PSM「面试高频追问」的那个点）。
 *
 * 为什么值得单独测：
 *   若 packet_length 少算/多算，ZLMediaKit 之类的接收端会在解析 PS 时
 *   读偏一个字段，现象是「偶尔能出画面、偶尔花屏」，极难定位。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "gb_ps.h"

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL: ");                                               \
            printf(__VA_ARGS__);                                            \
            fflush(stdout);                                                 \
            return 1;                                                       \
        }                                                                   \
    } while (0)

static void dump(const char *name, const uint8_t *p, size_t n)
{
    size_t i;
    printf("  %s (%zu bytes):", name, n);
    for (i = 0; i < n; i++)
        printf(" %02X", p[i]);
    printf("\n");
}

static size_t find_pair(const uint8_t *p, size_t n, uint8_t a, uint8_t b, int *found)
{
    size_t i;
    *found = 0;
    for (i = 0; i + 1 < n; i++) {
        if (p[i] == a && p[i + 1] == b) {
            *found = 1;
            return i;
        }
    }
    return 0;
}

int main(void)
{
    static const uint8_t psm_video_expect[20] = {
        0x00, 0x00, 0x01, 0xBC, 0x00, 0x0E, 0x01, 0x01, 0x00, 0x00,
        0x00, 0x04, 0x1B, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    /* 逐字节契约来自 docs/06-MPEG-PS打包器.md 与 steps/03_ps_pack.c 里同一份常量：
     *   00 00 01 BB | 00 09 | C3 33 67 00 | 21 | FF | E0 E0 E6 00
     *   length 字段 = 9（注意：它 < 总长-6 = 10，接收端会提前 1 字节停下，
     *   再靠「扫描下一个 00 00 01」重新同步到 PSM —— ffmpeg/ZLMediaKit 都这么做。
     *   真正危险的是 length 写**大**了：那会直接跳过 PSM 的起始码。）
     */
    static const uint8_t sys_header_expect[16] = {
        0x00, 0x00, 0x01, 0xBB, 0x00, 0x09, 0xC3, 0x33,
        0x67, 0x00, 0x21, 0xFF, 0xE0, 0xE0, 0xE6, 0x00
    };
    static const uint8_t pack_header_expect[14] = {
        0x00, 0x00, 0x01, 0xBA, 0x44, 0x00, 0x04,
        0x00, 0x04, 0x01, 0x86, 0x66, 0xCF, 0xF8
    };
    static const uint8_t psm_av_expect[24] = {
        0x00, 0x00, 0x01, 0xBC, 0x00, 0x12, 0x01, 0x01, 0x00, 0x00, 0x00, 0x08,
        0x1B, 0xE0, 0x00, 0x00, 0x90, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    uint16_t plen;
    int found;

    /* ---------- 1. 尺寸：由 gb_ps.h 的数组声明保证，这里显式断言一次 ---------- */
    CHECK(sizeof(GB_PS_PACK_HEADER)   == 14, "GB_PS_PACK_HEADER 应为 14 字节, got %zu\n",
          sizeof(GB_PS_PACK_HEADER));
    CHECK(sizeof(GB_PS_SYSTEM_HEADER) == 16, "GB_PS_SYSTEM_HEADER 应为 16 字节, got %zu\n",
          sizeof(GB_PS_SYSTEM_HEADER));
    CHECK(sizeof(GB_PS_PSM_VIDEO)     == 20, "GB_PS_PSM_VIDEO 应为 20 字节, got %zu\n",
          sizeof(GB_PS_PSM_VIDEO));
    CHECK(sizeof(GB_PS_PSM_AV)        == 24, "GB_PS_PSM_AV 应为 24 字节, got %zu\n",
          sizeof(GB_PS_PSM_AV));

    /* ---------- 2. Pack Header：MPEG-2 PS 的身份位，14 字节逐字节 ---------- */
    if (memcmp(GB_PS_PACK_HEADER, pack_header_expect, sizeof(pack_header_expect)) != 0) {
        printf("FAIL: GB_PS_PACK_HEADER 与冻结的 14 字节不一致\n");
        dump("actual  ", GB_PS_PACK_HEADER, sizeof(GB_PS_PACK_HEADER));
        dump("expected", pack_header_expect, sizeof(pack_header_expect));
        return 1;
    }
    CHECK(memcmp(GB_PS_PACK_HEADER, "\x00\x00\x01\xBA", 4) == 0,
          "Pack Header 起始码不是 00 00 01 BA\n");
    CHECK(GB_PS_PACK_HEADER[4] == 0x44,
          "Pack Header[4] = 0x%02X, expected 0x44"
          "  <- 0x44 表示 MPEG-2 PS（用 0x21 会退化成 MPEG-1 的 PES 解析规则，"
          "PES 头会整体错位 3 字节）\n",
          GB_PS_PACK_HEADER[4]);
    CHECK((GB_PS_PACK_HEADER[4] & 0xC0) == 0x40,
          "Pack Header[4] 最高两位应为 '01'(MPEG-2), got 0x%02X\n", GB_PS_PACK_HEADER[4]);

    /* ---------- 3. System Header：起始码 + 逐字节 + length 不许越过 PSM ---------- */
    if (memcmp(GB_PS_SYSTEM_HEADER, sys_header_expect, sizeof(sys_header_expect)) != 0) {
        printf("FAIL: GB_PS_SYSTEM_HEADER 与冻结的 16 字节不一致\n");
        dump("actual  ", GB_PS_SYSTEM_HEADER, sizeof(GB_PS_SYSTEM_HEADER));
        dump("expected", sys_header_expect, sizeof(sys_header_expect));
        return 1;
    }
    CHECK(memcmp(GB_PS_SYSTEM_HEADER, "\x00\x00\x01\xBB", 4) == 0,
          "System Header 起始码不是 00 00 01 BB\n");
    plen = (uint16_t)((GB_PS_SYSTEM_HEADER[4] << 8) | GB_PS_SYSTEM_HEADER[5]);
    CHECK(plen <= sizeof(GB_PS_SYSTEM_HEADER) - 6,
          "System Header header_length = %u 大于 总长(%zu)-6 = %zu"
          "   <- length 写大，接收端会跳过头部的剩余字节，可能直接跳过 PSM 的 00 00 01 BC\n",
          plen, sizeof(GB_PS_SYSTEM_HEADER), sizeof(GB_PS_SYSTEM_HEADER) - 6);
    CHECK(plen >= 6, "System Header header_length = %u 过小，解析不出码率/流数量\n", plen);

    /* ---------- 4. PSM(单视频)：逐字节等于经典 20 字节 ---------- */
    if (memcmp(GB_PS_PSM_VIDEO, psm_video_expect, sizeof(psm_video_expect)) != 0) {
        printf("FAIL: GB_PS_PSM_VIDEO 与国标经典 20 字节不一致\n");
        dump("actual  ", GB_PS_PSM_VIDEO, sizeof(GB_PS_PSM_VIDEO));
        dump("expected", psm_video_expect, sizeof(psm_video_expect));
        return 1;
    }

    plen = (uint16_t)((GB_PS_PSM_VIDEO[4] << 8) | GB_PS_PSM_VIDEO[5]);
    CHECK(plen == 0x000E, "GB_PS_PSM_VIDEO packet_length = 0x%04X, expected 0x000E\n", plen);
    CHECK(plen == sizeof(GB_PS_PSM_VIDEO) - 6,
          "PSM(VIDEO) packet_length(0x%04X) != 总长(%zu) - 6\n",
          plen, sizeof(GB_PS_PSM_VIDEO));

    /* es_map_length = 4：只有一条 4 字节的 elementary stream 描述 */
    CHECK(((GB_PS_PSM_VIDEO[10] << 8) | GB_PS_PSM_VIDEO[11]) == 0x0004,
          "PSM(VIDEO) es_map_length 应为 0x0004, got 0x%04X\n",
          (GB_PS_PSM_VIDEO[10] << 8) | GB_PS_PSM_VIDEO[11]);
    (void)find_pair(GB_PS_PSM_VIDEO, sizeof(GB_PS_PSM_VIDEO), GB_PS_ST_H264, GB_PS_SID_VIDEO, &found);
    CHECK(found, "PSM(VIDEO) 里找不到 stream_type=0x1B / es_id=0xE0 的描述\n");

    /* ---------- 5. PSM(音视频)：结构字段 + 逐字节 ---------- */
    if (memcmp(GB_PS_PSM_AV, psm_av_expect, sizeof(psm_av_expect)) != 0) {
        printf("FAIL: GB_PS_PSM_AV 与冻结的 24 字节不一致\n");
        dump("actual  ", GB_PS_PSM_AV, sizeof(GB_PS_PSM_AV));
        dump("expected", psm_av_expect, sizeof(psm_av_expect));
        return 1;
    }
    CHECK(memcmp(GB_PS_PSM_AV, "\x00\x00\x01\xBC", 4) == 0,
          "GB_PS_PSM_AV 起始码不是 00 00 01 BC\n");

    plen = (uint16_t)((GB_PS_PSM_AV[4] << 8) | GB_PS_PSM_AV[5]);
    CHECK(plen == 0x0012, "GB_PS_PSM_AV packet_length = 0x%04X, expected 0x0012\n", plen);
    CHECK(plen == sizeof(GB_PS_PSM_AV) - 6,
          "PSM(AV) packet_length(0x%04X) != 总长(%zu) - 6\n",
          plen, sizeof(GB_PS_PSM_AV));

    CHECK(((GB_PS_PSM_AV[10] << 8) | GB_PS_PSM_AV[11]) == 0x0008,
          "PSM(AV) es_map_length 应为 0x0008（两条 4 字节描述）, got 0x%04X\n",
          (GB_PS_PSM_AV[10] << 8) | GB_PS_PSM_AV[11]);

    find_pair(GB_PS_PSM_AV, sizeof(GB_PS_PSM_AV), GB_PS_ST_H264, GB_PS_SID_VIDEO, &found);
    CHECK(found, "PSM(AV) 里找不到视频描述 (0x1B, 0xE0)\n");
    find_pair(GB_PS_PSM_AV, sizeof(GB_PS_PSM_AV), GB_PS_ST_G711A, GB_PS_SID_AUDIO, &found);
    CHECK(found, "PSM(AV) 里找不到 G.711A 音频描述 (0x90, 0xC0)\n");

    /* 两条流的描述顺序：视频在前，音频在后（国标惯例） */
    {
        size_t v = find_pair(GB_PS_PSM_AV, sizeof(GB_PS_PSM_AV), GB_PS_ST_H264, GB_PS_SID_VIDEO, &found);
        size_t a = find_pair(GB_PS_PSM_AV, sizeof(GB_PS_PSM_AV), GB_PS_ST_G711A, GB_PS_SID_AUDIO, &found);
        CHECK(v < a, "PSM(AV) 里音频描述(%zu)出现在视频描述(%zu)之前，顺序不对\n", a, v);
    }

    /* ---------- 6. 「packet_length == 总长 − 6」对两者都成立 ---------- */
    {
        size_t total_v = sizeof(GB_PS_PSM_VIDEO);
        size_t total_a = sizeof(GB_PS_PSM_AV);
        uint16_t lv = (uint16_t)((GB_PS_PSM_VIDEO[4] << 8) | GB_PS_PSM_VIDEO[5]);
        uint16_t la = (uint16_t)((GB_PS_PSM_AV[4] << 8) | GB_PS_PSM_AV[5]);
        CHECK(lv == total_v - 6 && la == total_a - 6,
              "规律 packet_length == 总长-6 不成立: video %u/%zu, av %u/%zu\n",
              lv, total_v, la, total_a);
        CHECK(la == lv + 4, "PSM(AV) 应比 PSM(VIDEO) 多 4 字节(0x12-0x0E), got %u vs %u\n", la, lv);
    }

    printf("PASS: Pack(14B)/System(16B)/PSM(20B/24B) 四份头部常量逐字节一致；"
           "PSM packet_length==总长-6（0x0E / 0x12，es_map 0x04 / 0x08）；"
           "System Header length 未越过 PSM 起始码\n");
    return 0;
}
