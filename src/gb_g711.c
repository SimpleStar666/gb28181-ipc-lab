/*
 * gb_g711.c — G.711 A-law 编解码
 * ============================================================
 * 用的是 ITU-T G.711 / Sun Microsystems 的经典实现（也就是 RFC 3551 里
 * 附录的那套算法），因为它有两个好处：
 *   1. 它是「参考实现」，和 sox / ffmpeg / python audioop 的结果逐字节一致，
 *      单元测试可以直接拿工具生成的表来对。
 *   2. A-law 本质是 13 段折线 + 符号位取反（0xD5 / 0x55 掩码），
 *      代码只有十几行，面试让你手写也写得出来。
 *
 * A-law 的两条性质，测试里最好覆盖到：
 *   - 输入 0 编成 0xD5（不是 0x00），这是 A-law 的「符号位反转」造成的；
 *   - 编解码不是无损的：decode(encode(x)) 只能还原到量化台阶的中心。
 *     所以 test_g711.c 里不要写「round-trip 完全相等」这种断言，
 *     要写「误差不超过该段量化台阶的一半」或者直接对标准表。
 */
#include "gb_g711.h"

#define G711_SIGN_BIT  0x80
#define G711_QUANT_MASK 0x0F
#define G711_SEG_SHIFT  4
#define G711_SEG_MASK   0x70

/* 段上界（A-law）：pcm_val 已经被 >>3 压到 13bit 量级 */
static const int16_t seg_aend[8] = { 0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF };

static int seg_search(int val, const int16_t *table, int size)
{
    int i;

    for (i = 0; i < size; i++) {
        if (val <= table[i]) return i;
    }
    return size;
}

uint8_t gb_g711a_encode(int16_t pcm)
{
    int mask, seg;
    int v = pcm;

    v >>= 3;                     /* 16bit -> 13bit（A-law 只有 13 位精度） */
    if (v >= 0) {
        mask = 0xD5;
    } else {
        mask = 0x55;
        v = -v - 1;              /* -1 的补码技巧：保证 -32768 也不会溢出 */
    }

    seg = seg_search(v, seg_aend, 8);
    if (seg >= 8) {
        return (uint8_t)(0x7F ^ mask);   /* 饱和到最大码 */
    }

    {
        uint8_t aval = (uint8_t)((unsigned)seg << G711_SEG_SHIFT);
        if (seg < 2) aval |= (uint8_t)((v >> 1) & G711_QUANT_MASK);
        else         aval |= (uint8_t)((v >> seg) & G711_QUANT_MASK);
        return (uint8_t)(aval ^ mask);   /* 奇数位取反 */
    }
}

int16_t gb_g711a_decode(uint8_t a)
{
    int t, seg;
    uint8_t aval = (uint8_t)(a ^ 0x55);   /* 还原：把编码时的取反去掉 */

    t = (int)((aval & G711_QUANT_MASK) << 4);
    seg = (int)(((unsigned)aval & G711_SEG_MASK) >> G711_SEG_SHIFT);

    switch (seg) {
    case 0:
        t += 8;
        break;
    case 1:
        t += 0x108;
        break;
    default:
        t += 0x108;
        t <<= (seg - 1);
        break;
    }
    return (int16_t)((aval & G711_SIGN_BIT) ? t : -t);
}

size_t gb_g711a_encode_buf(const int16_t *pcm, size_t samples, uint8_t *out, size_t out_cap)
{
    size_t i, n;

    if (!pcm || !out) return 0;
    n = (samples < out_cap) ? samples : out_cap;
    for (i = 0; i < n; i++) out[i] = gb_g711a_encode(pcm[i]);
    return n;
}
