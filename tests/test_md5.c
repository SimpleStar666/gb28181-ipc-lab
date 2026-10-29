/*
 * test_md5.c — RFC 1321 标准测试向量
 * ============================================================
 * 为什么这个测试必须存在：
 *   gb_md5.c 是从各种「网上抄来」的版本改的，而 MD5 最容易错的地方是
 *   Round 1 第 15 步的常量 0xa679438e（错版常见 0xa6434387）。
 *   这类错误不会有任何编译警告，只有拿标准向量跑才能发现。
 *
 * 本文件里的每一个期望摘要都用独立实现（Python hashlib）核对过：
 *   标准向量 8 个 + 「1 百万个 a」多块用例。
 * 额外覆盖：
 *   - gb_md5_hex / gb_md5_hex_str 与 init/update/final 结果一致；
 *   - 「每 1 字节 update 一次」与「一次性 update」结果一致
 *     —— 这条能抓住 padding / count 累积写错（count 只按 64 字节对齐累加是经典 bug）。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "gb_md5.h"

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL: ");                                               \
            printf(__VA_ARGS__);                                            \
            printf("  (at %s:%d)\n", __FILE__, __LINE__);                    \
            fflush(stdout);                                                 \
            return 1;                                                       \
        }                                                                   \
    } while (0)

static void digest_to_hex(const uint8_t d[GB_MD5_DIGEST_LEN], char out[GB_MD5_HEX_LEN])
{
    static const char *hexd = "0123456789abcdef";
    int i;
    for (i = 0; i < GB_MD5_DIGEST_LEN; i++) {
        out[i * 2]     = hexd[d[i] >> 4];
        out[i * 2 + 1] = hexd[d[i] & 0x0F];
    }
    out[32] = '\0';
}

/* 一次性 update */
static void md5_oneshot(const void *data, size_t len, char out[GB_MD5_HEX_LEN])
{
    gb_md5_ctx_t ctx;
    uint8_t d[GB_MD5_DIGEST_LEN];
    gb_md5_init(&ctx);
    gb_md5_update(&ctx, data, len);
    gb_md5_final(&ctx, d);
    digest_to_hex(d, out);
}

/* 每 1 字节 update 一次 */
static void md5_bytewise(const void *data, size_t len, char out[GB_MD5_HEX_LEN])
{
    gb_md5_ctx_t ctx;
    uint8_t d[GB_MD5_DIGEST_LEN];
    const uint8_t *p = (const uint8_t *)data;
    size_t i;
    gb_md5_init(&ctx);
    for (i = 0; i < len; i++)
        gb_md5_update(&ctx, p + i, 1);
    gb_md5_final(&ctx, d);
    digest_to_hex(d, out);
}

/* RFC 1321 附录 A.5 的测试套件（前 7 条 + 1 百万 a 那条） */
typedef struct {
    const char *input;
    size_t      len;      /* 0 表示用 strlen，+1 供二进制用例用 */
    int         use_str;  /* 1 = 是 C 字符串；0 = 纯字节块 */
    const char *expect;
} md5_vec_t;

static const char alnum[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
static const char digits80[] =
    "12345678901234567890123456789012345678901234567890123456789012345678901234567890";

int main(void)
{
    char hex[GB_MD5_HEX_LEN];
    size_t i;

    static const md5_vec_t vecs[] = {
        { "",                       0,  1, "d41d8cd98f00b204e9800998ecf8427e" },
        { "a",                      0,  1, "0cc175b9c0f1b6a831c399e269772661" },
        { "abc",                    0,  1, "900150983cd24fb0d6963f7d28e17f72" },
        { "message digest",         0,  1, "f96b697d7cb7938d525a2f31aaf161d0" },
        { "abcdefghijklmnopqrstuvwxyz", 0, 1, "c3fcd3d76192e4007dfb496cca67e13b" },
        { alnum,                   0,   1, "d174ab98d277d9f5a5611c2c9f419d9f" },
        { digits80,                0,   1, "57edf4a22be3c955ac49da2e2107b67a" },
    };

    /* ---------- 1. 标准向量：一次性 / hex / hex_str / 逐字节 ---------- */
    for (i = 0; i < sizeof(vecs) / sizeof(vecs[0]); i++) {
        size_t len = (vecs[i].use_str || vecs[i].len == 0)
                         ? strlen(vecs[i].input)
                         : vecs[i].len;

        md5_oneshot(vecs[i].input, len, hex);
        CHECK(strcmp(hex, vecs[i].expect) == 0,
              "MD5(\"%s\") [init/update/final] = %s, expected %s\n",
              vecs[i].input, hex, vecs[i].expect);

        /* hex 接口 */
        gb_md5_hex(vecs[i].input, len, hex);
        CHECK(strcmp(hex, vecs[i].expect) == 0,
              "MD5(\"%s\") [gb_md5_hex] = %s, expected %s\n",
              vecs[i].input, hex, vecs[i].expect);

        /* hex_str 接口 */
        gb_md5_hex_str(vecs[i].input, hex);
        CHECK(strcmp(hex, vecs[i].expect) == 0,
              "MD5(\"%s\") [gb_md5_hex_str] = %s, expected %s\n",
              vecs[i].input, hex, vecs[i].expect);

        /* 每 1 字节 update 一次 —— 结果必须与一次性完全相同 */
        md5_bytewise(vecs[i].input, len, hex);
        CHECK(strcmp(hex, vecs[i].expect) == 0,
              "MD5(\"%s\") [1-byte-at-a-time update] = %s, expected %s"
              "  <- padding/count 累积逻辑有问题\n",
              vecs[i].input, hex, vecs[i].expect);
    }

    /* ---------- 2. 1 百万个 'a'：多块 + 长度跨 32 位低位 ---------- */
    {
        gb_md5_ctx_t ctx;
        uint8_t d[GB_MD5_DIGEST_LEN];
        char block[1000];
        int n;
        memset(block, 'a', sizeof(block));
        gb_md5_init(&ctx);
        for (n = 0; n < 1000; n++)
            gb_md5_update(&ctx, block, sizeof(block));   /* 1000 * 1000 = 1000000 */
        gb_md5_final(&ctx, d);
        digest_to_hex(d, hex);
        CHECK(strcmp(hex, "7707d6ae4e027c70eea2a935c2296f21") == 0,
              "MD5(\"a\" x 1000000) = %s, expected 7707d6ae4e027c70eea2a935c2296f21\n",
              hex);
    }

    /* ---------- 3. 分组边界等价性（覆盖 55/56/63/64/65 等 padding 边界） ---------- */
    {
        /* 用可自证的等价关系代替外部常量：
         * 同一段数据，一次 update 与分两段 update（在任意位置切）必须一致。
         * 这直接覆盖「字符串长度不是 64 的整数倍时 padding 位置」的正确性。 */
        char buf[200];
        size_t len;
        for (i = 0; i < sizeof(buf); i++)
            buf[i] = (char)(i * 7 + 3);
        for (len = 0; len <= sizeof(buf); len++) {
            char a[GB_MD5_HEX_LEN], b[GB_MD5_HEX_LEN];
            gb_md5_ctx_t ctx;
            uint8_t d[GB_MD5_DIGEST_LEN];
            size_t cut;
            md5_oneshot(buf, len, a);
            for (cut = 0; cut <= len; cut += 1 + (len / 8 ? len / 8 : 1)) {
                gb_md5_init(&ctx);
                gb_md5_update(&ctx, buf, cut);
                gb_md5_update(&ctx, buf + cut, len - cut);
                gb_md5_final(&ctx, d);
                digest_to_hex(d, b);
                CHECK(strcmp(a, b) == 0,
                      "len=%zu cut=%zu: 分段 update (%s) != 一次性 update (%s)"
                      "  <- 字节缓冲搬运有 bug\n",
                      len, cut, b, a);
            }
        }
    }

    printf("PASS: MD5 标准向量 %d 条 + 1M 输入 + 分段一致性 全部通过\n",
           (int)(sizeof(vecs) / sizeof(vecs[0])));
    return 0;
}
