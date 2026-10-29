/*
 * gb_annexb.c — H.264 AnnexB 起始码扫描
 * ============================================================
 * 全部是纯函数：给一段字节，回答「下一个起始码在哪、有多长、里面的 NAL 是什么类型」。
 * 真正的「跨 read 边界拼帧」逻辑在 gb_media.c，这里只做最底层的判断。
 *
 * 【为什么 4 字节起始码必须优先？】
 *   x264 这类编码器在 SPS/PPS/IDR 前写 00 00 00 01，其余 NAL 前写 00 00 01。
 *   如果先按 3 字节匹配，00 00 00 01 会被切成「起始码 00 00 01」+「NAL 头 0x00 ...」，
 *   于是每个关键帧的 NAL 都多出一个前导 0x00，
 *   解码器看到 nal_ref_idc/nal_unit_type 全错位 —— 表现为「偶发花屏/首帧黑」。
 */
#include "gb_annexb.h"

int gb_annexb_find_startcode(const uint8_t *d, size_t len, size_t from, size_t *sc_len)
{
    size_t i;

    if (!d || !sc_len) return -1;
    if (from >= len) return -1;

    for (i = from; i + 3 <= len; i++) {
        if (d[i] != 0x00 || d[i + 1] != 0x00) continue;

        /* 同一个偏移上先看 4 字节，再看 3 字节 —— 等价于「最长匹配优先」 */
        if (i + 4 <= len && d[i + 2] == 0x00 && d[i + 3] == 0x01) {
            *sc_len = 4;
            return (int)i;
        }
        if (d[i + 2] == 0x01) {
            *sc_len = 3;
            return (int)i;
        }
    }
    return -1;
}

int gb_annexb_nal_type(const uint8_t *nal, size_t sc_len)
{
    if (!nal) return -1;
    if (sc_len != 3 && sc_len != 4) return -1;
    /* 起始码之后第一个字节的低 5 位就是 nal_unit_type */
    return (int)(nal[sc_len] & 0x1F);
}

int gb_annexb_is_idr(int nal_type)
{
    return nal_type == 5;                 /* IDR slice */
}

int gb_annexb_is_vcl(int nal_type)
{
    return nal_type >= 1 && nal_type <= 5; /* 1..5 才是真正带图像数据的 NAL */
}
