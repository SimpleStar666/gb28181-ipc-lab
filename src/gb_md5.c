/*
 * gb_md5.c — MD5（RFC 1321）公共域实现的自写版本
 * ============================================================
 * 只为了 SIP Digest 鉴权，所以不链接 OpenSSL。
 * 全实现 4 个状态字 + 64 次循环，代码量比一个库的依赖链还小。
 *
 * 【本项目特意保留的教学点：Round 1 第 15 步的常量】
 *   MD5_FF(a,b,c,d,x[14],17,0xa679438e)  —— 必须是 0xa679438e。
 *   网上流传的不少版本这一位写成 0xa6434387（或别的），而且往往一份错、抄的人
 *   四份全错，互相「印证」形成假一致。唯一的验证办法是跑标准测试向量：
 *       MD5("")    = d41d8cd98f00b204e9800998ecf8427e
 *       MD5("abc") = 900150983cd24fb0d6963f7d28e17f72
 *   tests/test_md5.c 里必须断言这两条。
 *
 * 字节序：MD5 全程小端。state 输出成 digest 时也要按小端铺开，
 *   漏了这一步会得到「看起来对但长度对不上」的十六进制串（等于把 digest 反转）。
 */
#include "gb_md5.h"

#include <string.h>

#define MD5_F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define MD5_G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | ~(z)))
#define MD5_ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define MD5_FF(a, b, c, d, x, s, ac)                    \
    do {                                                \
        (a) += MD5_F((b), (c), (d)) + (x) + (ac);       \
        (a) = MD5_ROTL((a), (s));                       \
        (a) += (b);                                     \
    } while (0)
#define MD5_GG(a, b, c, d, x, s, ac)                    \
    do {                                                \
        (a) += MD5_G((b), (c), (d)) + (x) + (ac);       \
        (a) = MD5_ROTL((a), (s));                       \
        (a) += (b);                                     \
    } while (0)
#define MD5_HH(a, b, c, d, x, s, ac)                    \
    do {                                                \
        (a) += MD5_H((b), (c), (d)) + (x) + (ac);       \
        (a) = MD5_ROTL((a), (s));                       \
        (a) += (b);                                     \
    } while (0)
#define MD5_II(a, b, c, d, x, s, ac)                    \
    do {                                                \
        (a) += MD5_I((b), (c), (d)) + (x) + (ac);       \
        (a) = MD5_ROTL((a), (s));                       \
        (a) += (b);                                     \
    } while (0)

static const unsigned char md5_padding[64] = { 0x80 /* 其余为 0 */ };

static void md5_encode(unsigned char *out, const uint32_t *in, unsigned len)
{
    unsigned i, j;

    for (i = 0, j = 0; j < len; i++, j += 4) {
        out[j + 0] = (unsigned char)(in[i] & 0xFF);
        out[j + 1] = (unsigned char)((in[i] >> 8) & 0xFF);
        out[j + 2] = (unsigned char)((in[i] >> 16) & 0xFF);
        out[j + 3] = (unsigned char)((in[i] >> 24) & 0xFF);
    }
}

static void md5_transform(gb_md5_ctx_t *ctx, const unsigned char block[64])
{
    uint32_t a = ctx->state[0], b = ctx->state[1];
    uint32_t c = ctx->state[2], d = ctx->state[3];
    uint32_t x[16];
    int i;

    for (i = 0; i < 16; i++) {
        x[i] = (uint32_t)block[i * 4] |
               ((uint32_t)block[i * 4 + 1] << 8) |
               ((uint32_t)block[i * 4 + 2] << 16) |
               ((uint32_t)block[i * 4 + 3] << 24);
    }

    MD5_FF(a, b, c, d, x[ 0],  7, 0xd76aa478); MD5_FF(d, a, b, c, x[ 1], 12, 0xe8c7b756);
    MD5_FF(c, d, a, b, x[ 2], 17, 0x242070db); MD5_FF(b, c, d, a, x[ 3], 22, 0xc1bdceee);
    MD5_FF(a, b, c, d, x[ 4],  7, 0xf57c0faf); MD5_FF(d, a, b, c, x[ 5], 12, 0x4787c62a);
    MD5_FF(c, d, a, b, x[ 6], 17, 0xa8304613); MD5_FF(b, c, d, a, x[ 7], 22, 0xfd469501);
    MD5_FF(a, b, c, d, x[ 8],  7, 0x698098d8); MD5_FF(d, a, b, c, x[ 9], 12, 0x8b44f7af);
    MD5_FF(c, d, a, b, x[10], 17, 0xffff5bb1); MD5_FF(b, c, d, a, x[11], 22, 0x895cd7be);
    MD5_FF(a, b, c, d, x[12],  7, 0x6b901122); MD5_FF(d, a, b, c, x[13], 12, 0xfd987193);
    MD5_FF(c, d, a, b, x[14], 17, 0xa679438e); /* <-- 就是这一位，抄错的版本会挂在这里 */
    MD5_FF(b, c, d, a, x[15], 22, 0x49b40821);

    MD5_GG(a, b, c, d, x[ 1],  5, 0xf61e2562); MD5_GG(d, a, b, c, x[ 6],  9, 0xc040b340);
    MD5_GG(c, d, a, b, x[11], 14, 0x265e5a51); MD5_GG(b, c, d, a, x[ 0], 20, 0xe9b6c7aa);
    MD5_GG(a, b, c, d, x[ 5],  5, 0xd62f105d); MD5_GG(d, a, b, c, x[10],  9, 0x02441453);
    MD5_GG(c, d, a, b, x[15], 14, 0xd8a1e681); MD5_GG(b, c, d, a, x[ 4], 20, 0xe7d3fbc8);
    MD5_GG(a, b, c, d, x[ 9],  5, 0x21e1cde6); MD5_GG(d, a, b, c, x[14],  9, 0xc33707d6);
    MD5_GG(c, d, a, b, x[ 3], 14, 0xf4d50d87); MD5_GG(b, c, d, a, x[ 8], 20, 0x455a14ed);
    MD5_GG(a, b, c, d, x[13],  5, 0xa9e3e905); MD5_GG(d, a, b, c, x[ 2],  9, 0xfcefa3f8);
    MD5_GG(c, d, a, b, x[ 7], 14, 0x676f02d9); MD5_GG(b, c, d, a, x[12], 20, 0x8d2a4c8a);

    MD5_HH(a, b, c, d, x[ 5],  4, 0xfffa3942); MD5_HH(d, a, b, c, x[ 8], 11, 0x8771f681);
    MD5_HH(c, d, a, b, x[11], 16, 0x6d9d6122); MD5_HH(b, c, d, a, x[14], 23, 0xfde5380c);
    MD5_HH(a, b, c, d, x[ 1],  4, 0xa4beea44); MD5_HH(d, a, b, c, x[ 4], 11, 0x4bdecfa9);
    MD5_HH(c, d, a, b, x[ 7], 16, 0xf6bb4b60); MD5_HH(b, c, d, a, x[10], 23, 0xbebfbc70);
    MD5_HH(a, b, c, d, x[13],  4, 0x289b7ec6); MD5_HH(d, a, b, c, x[ 0], 11, 0xeaa127fa);
    MD5_HH(c, d, a, b, x[ 3], 16, 0xd4ef3085); MD5_HH(b, c, d, a, x[ 6], 23, 0x04881d05);
    MD5_HH(a, b, c, d, x[ 9],  4, 0xd9d4d039); MD5_HH(d, a, b, c, x[12], 11, 0xe6db99e5);
    MD5_HH(c, d, a, b, x[15], 16, 0x1fa27cf8); MD5_HH(b, c, d, a, x[ 2], 23, 0xc4ac5665);

    MD5_II(a, b, c, d, x[ 0],  6, 0xf4292244); MD5_II(d, a, b, c, x[ 7], 10, 0x432aff97);
    MD5_II(c, d, a, b, x[14], 15, 0xab9423a7); MD5_II(b, c, d, a, x[ 5], 21, 0xfc93a039);
    MD5_II(a, b, c, d, x[12],  6, 0x655b59c3); MD5_II(d, a, b, c, x[ 3], 10, 0x8f0ccc92);
    MD5_II(c, d, a, b, x[10], 15, 0xffeff47d); MD5_II(b, c, d, a, x[ 1], 21, 0x85845dd1);
    MD5_II(a, b, c, d, x[ 8],  6, 0x6fa87e4f); MD5_II(d, a, b, c, x[15], 10, 0xfe2ce6e0);
    MD5_II(c, d, a, b, x[ 6], 15, 0xa3014314); MD5_II(b, c, d, a, x[13], 21, 0x4e0811a1);
    MD5_II(a, b, c, d, x[ 4],  6, 0xf7537e82); MD5_II(d, a, b, c, x[11], 10, 0xbd3af235);
    MD5_II(c, d, a, b, x[ 2], 15, 0x2ad7d2bb); MD5_II(b, c, d, a, x[ 9], 21, 0xeb86d391);

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
}

void gb_md5_init(gb_md5_ctx_t *ctx)
{
    if (!ctx) return;
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}

void gb_md5_update(gb_md5_ctx_t *ctx, const void *data, size_t len)
{
    const unsigned char *input = (const unsigned char *)data;
    size_t idx, part, i;

    if (!ctx || !input || len == 0) return;

    idx = (size_t)((ctx->count[0] >> 3) & 0x3F);

    /* 位计数，低 32 位溢出时进位到高 32 位 —— 支持 >512MB 的输入 */
    ctx->count[0] += (uint32_t)(len << 3);
    if (ctx->count[0] < (uint32_t)(len << 3)) ctx->count[1]++;
    ctx->count[1] += (uint32_t)(len >> 29);

    part = 64 - idx;
    if (len >= part) {
        memcpy(&ctx->buffer[idx], input, part);
        md5_transform(ctx, ctx->buffer);
        for (i = part; i + 63 < len; i += 64) md5_transform(ctx, &input[i]);
        idx = 0;
    } else {
        i = 0;
    }
    memcpy(&ctx->buffer[idx], &input[i], len - i);
}

void gb_md5_final(gb_md5_ctx_t *ctx, uint8_t digest[GB_MD5_DIGEST_LEN])
{
    unsigned char bits[8];
    size_t idx, padlen, i;

    if (!ctx || !digest) return;

    md5_encode(bits, ctx->count, 8);
    idx = (size_t)((ctx->count[0] >> 3) & 0x3F);
    padlen = (idx < 56) ? (56 - idx) : (120 - idx);
    gb_md5_update(ctx, md5_padding, padlen);   /* 0x80 + 若干 0x00 */
    gb_md5_update(ctx, bits, 8);               /* 原始长度（bit，小端） */

    for (i = 0; i < 4; i++) {
        digest[i * 4 + 0] = (uint8_t)(ctx->state[i] & 0xFF);
        digest[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 8) & 0xFF);
        digest[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 16) & 0xFF);
        digest[i * 4 + 3] = (uint8_t)((ctx->state[i] >> 24) & 0xFF);
    }
}

void gb_md5_hex(const void *data, size_t len, char out_hex[GB_MD5_HEX_LEN])
{
    static const char hexd[] = "0123456789abcdef";
    gb_md5_ctx_t ctx;
    uint8_t digest[GB_MD5_DIGEST_LEN];
    int i;

    if (!out_hex) return;
    if (!data && len) { out_hex[0] = '\0'; return; }

    gb_md5_init(&ctx);
    if (len) gb_md5_update(&ctx, data, len);
    gb_md5_final(&ctx, digest);
    for (i = 0; i < GB_MD5_DIGEST_LEN; i++) {
        out_hex[i * 2]     = hexd[(digest[i] >> 4) & 0x0F];
        out_hex[i * 2 + 1] = hexd[digest[i] & 0x0F];
    }
    out_hex[GB_MD5_DIGEST_LEN * 2] = '\0';
}

void gb_md5_hex_str(const char *str, char out_hex[GB_MD5_HEX_LEN])
{
    if (!out_hex) return;
    if (!str) { out_hex[0] = '\0'; return; }
    gb_md5_hex(str, strlen(str), out_hex);
}
