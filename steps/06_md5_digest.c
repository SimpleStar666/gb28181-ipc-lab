/*
 * kata 06 / 08 —— MD5（RFC 1321）+ SIP Digest 认证 response 计算
 * ==========================================================================
 * 【这一步解决什么问题】GB28181 设备注册/点播前都要过 SIP Digest 认证：平台回 401
 *   并带上挑战参数（realm/nonce/qop），设备算出一个 response 再重发请求。
 *   本 kata 先用 RFC 1321 的标准向量把 MD5 自证一遍，再走一次完整的 401 → response。
 * 【对应协议】RFC 1321（MD5 算法）；RFC 2617 §3.2.2（HTTP/SIP Digest 认证，qop=auth）；
 *   GB/T 28181 第 9 章注册流程（401 → 带 Authorization 重发 REGISTER）。
 * 【缩写表（面试常问）】
 *   realm    认证域：平台的名字，设备必须原样回填
 *   nonce    平台发的一次性随机串（防重放；变了就要重算）
 *   qop      保护质量，取值 auth = 只做认证、不加密媒体
 *   nc       nonce-count：同一个 nonce 这是第几次用（8 位十六进制字符串）
 *   cnonce   client nonce：客户端自己产生的随机串
 *   HA1      MD5(username:realm:password)
 *   HA2      MD5(method:uri)
 *   response = MD5(HA1:nonce:nc:cnonce:qop:HA2)   ← 有 qop 时
 *              MD5(HA1:nonce:HA2)                 ← 没有 qop 时（老平台）
 * 【面试会怎么问 / 怎么答】
 *   Q1 密码在链路上怎么传？  A: 不传明文。平台给 nonce，设备用 MD5 算出 response，
 *      平台那边用同样的用户名/密码再算一遍比对。
 *   Q2 有 qop 和没 qop 的差别？  A: 有 qop 时把 nc/cnonce 也拼进摘要，能进一步防重放；
 *      实现时两套都要写，因为不少老平台只发 nonce（本仓库原型就是按 qop 有无分支的）。
 */
#include <stdio.h>
#include <string.h>

typedef unsigned int u32;

static void md5_block(u32 h[4], const unsigned char b[64])
{
    static const u32 K[64] = {
        0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu,0xf57c0fafu,0x4787c62au,0xa8304613u,0xfd469501u,
        0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu,0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,
        0xf61e2562u,0xc040b340u,0x265e5a51u,0xe9b6c7aau,0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,
        0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu,0xa9e3e905u,0xfcefa3f8u,0x676f02d9u,0x8d2a4c8au,
        0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu,0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,
        0x289b7ec6u,0xeaa127fau,0xd4ef3085u,0x04881d05u,0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,
        0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u,0x655b59c3u,0x8f0ccc92u,0xffeff47du,0x85845dd1u,
        0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u,0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u
    };
    static const int S[4][4] = {{7,12,17,22},{5,9,14,20},{4,11,16,23},{6,10,15,21}};
    u32 m[16], a = h[0], bb = h[1], c = h[2], d = h[3], f, t;
    int i, g;

    for (i = 0; i < 16; i++)
        m[i] = (u32)b[i*4] | ((u32)b[i*4+1] << 8) | ((u32)b[i*4+2] << 16) | ((u32)b[i*4+3] << 24);

    for (i = 0; i < 64; i++) {
        if      (i < 16) { f = (bb & c) | (~bb & d); g = i; }
        else if (i < 32) { f = (d & bb) | (~d & c);  g = (5 * i + 1) & 15; }
        else if (i < 48) { f = bb ^ c ^ d;           g = (3 * i + 5) & 15; }
        else             { f = c ^ (bb | ~d);        g = (7 * i) & 15; }
        t = d; d = c; c = bb;
        f = a + f + K[i] + m[g];
        bb = bb + ((f << S[i / 16][i % 4]) | (f >> (32 - S[i / 16][i % 4])));
        a = t;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d;
}

/* 一次性 MD5：内部自己补位（0x80 + 0 填充 + 64 位小端长度） */
static void md5(const unsigned char *msg, size_t len, unsigned char out[16])
{
    u32 h[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    unsigned char blk[64];
    unsigned long long bits = (unsigned long long)len * 8;
    size_t i, rem;

    for (i = 0; i + 64 <= len; i += 64) md5_block(h, msg + i);
    rem = len - i;
    memset(blk, 0, sizeof blk);
    memcpy(blk, msg + i, rem);
    blk[rem] = 0x80;
    if (rem >= 56) { md5_block(h, blk); memset(blk, 0, sizeof blk); }
    for (i = 0; i < 8; i++) blk[56 + i] = (unsigned char)((bits >> (8 * i)) & 0xFF);
    md5_block(h, blk);
    for (i = 0; i < 4; i++) {
        out[i*4]   = (unsigned char)(h[i] & 0xFF);
        out[i*4+1] = (unsigned char)((h[i] >> 8) & 0xFF);
        out[i*4+2] = (unsigned char)((h[i] >> 16) & 0xFF);
        out[i*4+3] = (unsigned char)((h[i] >> 24) & 0xFF);
    }
}

static void md5_hex(const char *s, char out[33])
{
    unsigned char d[16];
    int i;
    md5((const unsigned char *)s, strlen(s), d);
    for (i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
}

int main(void)
{
    /* RFC 2617 §3.5 的官方例子，三个值均可用 python3 hashlib 复核 */
    const char *user = "Mufasa", *realm = "testrealm@host.com", *pass = "Circle Of Life";
    const char *nonce = "dcd98b7102dd2f0e8b11d0f600bfb0c093";
    const char *uri = "/dir/index.html", *method = "GET";
    const char *nc = "00000001", *cnonce = "0a4f113b", *qop = "auth";
    char s[512], ha1[33], ha2[33], resp[33], noqop[33], got[33];
    char reason[200] = "";
    int fail = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=========== kata 06：MD5 + SIP Digest response ===========\n");

    printf("\n[1] RFC 1321 标准向量（先把算法自证一遍）\n");
    md5_hex("", got);    printf("    MD5(\"\")   = %s  期望 d41d8cd98f00b204e9800998ecf8427e  %s\n",
                               got, strcmp(got, "d41d8cd98f00b204e9800998ecf8427e") ? "✗" : "✓");
    if (strcmp(got, "d41d8cd98f00b204e9800998ecf8427e")) { fail = 1; snprintf(reason, sizeof reason, "MD5(\"\") 不对：%s", got); }
    md5_hex("abc", got); printf("    MD5(\"abc\") = %s  期望 900150983cd24fb0d6963f7d28e17f72  %s\n",
                               got, strcmp(got, "900150983cd24fb0d6963f7d28e17f72") ? "✗" : "✓");
    if (!fail && strcmp(got, "900150983cd24fb0d6963f7d28e17f72")) { fail = 1; snprintf(reason, sizeof reason, "MD5(abc) 不对：%s", got); }

    printf("\n[2] 平台发来的 401 挑战（SIP 头里的 WWW-Authenticate）\n");
    printf("    WWW-Authenticate: Digest realm=\"%s\", qop=\"%s\", nonce=\"%s\", algorithm=MD5\n", realm, qop, nonce);
    printf("    缩写：realm=认证域  nonce=一次性随机数  qop=保护质量(auth)\n");
    printf("          nc=nonce 用了几次  cnonce=客户端随机数\n");

    snprintf(s, sizeof s, "%s:%s:%s", user, realm, pass); md5_hex(s, ha1);
    snprintf(s, sizeof s, "%s:%s", method, uri);          md5_hex(s, ha2);
    snprintf(s, sizeof s, "%s:%s:%s:%s:%s:%s", ha1, nonce, nc, cnonce, qop, ha2); md5_hex(s, resp);
    snprintf(s, sizeof s, "%s:%s:%s", ha1, nonce, ha2);   md5_hex(s, noqop);

    printf("\n[3] 设备侧计算\n");
    printf("    HA1 = MD5(\"%s:%s:%s\")\n", user, realm, pass);
    printf("        = %s\n", ha1);
    printf("    HA2 = MD5(\"%s:%s\") = %s\n", method, uri, ha2);
    printf("    response = MD5(HA1:nonce:nc:cnonce:qop:HA2) = %s\n", resp);
    printf("    （若平台没给 qop，则 response = MD5(HA1:nonce:HA2) = %s）\n", noqop);
    printf("    重发请求时带上：Authorization: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\",\n", user, realm, nonce);
    printf("        uri=\"%s\", response=\"%s\", algorithm=MD5, qop=%s, nc=%s, cnonce=\"%s\"\n",
           uri, resp, qop, nc, cnonce);

    printf("\n--------------- 断言 ---------------\n");
    if (!fail && strcmp(ha1, "939e7578ed9e3c518a452acee763bce9")) {
        fail = 1; snprintf(reason, sizeof reason, "HA1 应为 939e7578ed9e3c518a452acee763bce9，实际 %s", ha1);
    }
    if (!fail && strcmp(ha2, "39aff3a2bab6126f332b942af96d3366")) {
        fail = 1; snprintf(reason, sizeof reason, "HA2 应为 39aff3a2bab6126f332b942af96d3366，实际 %s", ha2);
    }
    if (!fail && strcmp(resp, "6629fae49393a05397450978507c4ef1")) {
        fail = 1; snprintf(reason, sizeof reason, "response 应为 6629fae49393a05397450978507c4ef1，实际 %s", resp);
    }
    if (!fail && strcmp(noqop, resp) == 0) {
        fail = 1; snprintf(reason, sizeof reason, "有无 qop 的 response 不应该相同");
    }
    if (fail) { printf("SELF-CHECK FAIL: %s\n", reason); return 1; }
    printf("SELF-CHECK PASS\n");
    return 0;
}
