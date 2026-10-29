/*
 * gb_md5.h — MD5（RFC 1321）
 * ============================================================
 * 为什么 GB28181 设备端必须自己实现 MD5？
 *   SIP 的 Digest 鉴权（RFC 2617）要用 MD5 算 response。
 *   OpenSSL 依赖太重，嵌入式设备端常见做法就是内置一份 100 行的 MD5。
 *   面试时被问到「你为什么不链接 openssl」——这就是答案。
 *
 * 【最容易踩的坑】Round 1 第 15 步的常量是 0xa679438e，不是 0xa6434387。
 *   很多网上抄来的版本这一位是错的，而且往往四份拷贝全错，互相「印证」，
 *   形成假一致。验证方法只有一个：拿标准测试向量跑。
 *     MD5("")    = d41d8cd98f00b204e9800998ecf8427e
 *     MD5("abc") = 900150983cd24fb0d6963f7d28e17f72
 *   这两个向量必须写进单元测试（tests/test_md5.c）。
 */
#ifndef GB_MD5_H
#define GB_MD5_H

#include <stddef.h>
#include <stdint.h>

#define GB_MD5_DIGEST_LEN 16
#define GB_MD5_HEX_LEN    33   /* 32 个 hex 字符 + '\0' */

typedef struct {
    uint32_t      state[4];
    uint32_t      count[2];
    unsigned char buffer[64];
} gb_md5_ctx_t;

void gb_md5_init(gb_md5_ctx_t *ctx);
void gb_md5_update(gb_md5_ctx_t *ctx, const void *data, size_t len);
void gb_md5_final(gb_md5_ctx_t *ctx, uint8_t digest[GB_MD5_DIGEST_LEN]);

/* 一次性接口：算 data 的 MD5 并输出小写十六进制串。
 * out_hex 至少 GB_MD5_HEX_LEN 字节。 */
void gb_md5_hex(const void *data, size_t len, char out_hex[GB_MD5_HEX_LEN]);

/* 同上，但输入是 C 字符串 */
void gb_md5_hex_str(const char *str, char out_hex[GB_MD5_HEX_LEN]);

#endif /* GB_MD5_H */
