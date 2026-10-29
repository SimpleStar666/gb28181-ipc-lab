/*
 * gb_sip.c — SIP 信令（RFC 3261）+ Digest 鉴权（RFC 2617）
 * ============================================================
 * 设备端只做四件事：REGISTER / MESSAGE / 回 200 OK / 停推流（BYE 由 main 处理）。
 *
 * 【Digest 鉴权，面试务必说清每个缩写】
 *   平台 401 里给：realm（认证域）、nonce（一次性随机串，防重放）、qop（保护质量）
 *   设备算：
 *     HA1      = MD5(username : realm : password)
 *     HA2      = MD5(method : uri)                     method 这里是 "REGISTER"
 *     response = MD5(HA1 : nonce : nc : cnonce : qop : HA2)   （qop=auth）
 *     response = MD5(HA1 : nonce : HA2)                       （平台不给 qop）
 *   其中 nc = nonce 使用计数（8 位十六进制，首次 00000001），
 *        cnonce = 客户端随机串（防止服务端预计算）。
 *
 * 【必须实现「无 qop」那条分支】
 *   不同平台/WVP 版本行为不同：有的回 qop="auth"，有的干脆不给 qop。
 *   只写 qop=auth 分支的话，在某些平台上注册会失败，而且平台往往只回一个 403，
 *   从报文里完全看不出是鉴权串算错了还是账号不对。
 *
 * 【本文件的两个实现细节，也值得讲】
 *   1. 本机 IP 用「临时 socket connect + getsockname」探测：
 *      UDP 的 connect 不发包，只是让内核按路由选一个源地址。写进 Via/Contact 的
 *      IP 必须是平台能回包的地址，硬编码 127.0.0.1 在真机上必挂。
 *   2. SIP 头是「大小写不敏感 + 允许紧凑形式（v/f/t/i/m/l）」的，
 *      而且字段值必须按行解析 —— 用 strstr 在整个报文里找 "To:"，
 *      很容易命中消息体里的文本，或者把 "To:" 匹配到别的地方。
 */
#include "gb_sip.h"
#include "gb_common.h"
#include "gb_config.h"
#include "gb_log.h"
#include "gb_md5.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static const char *const SIP_USER_AGENT = "gb28181-ipc-lab/1.0";

/* ---------------- 小工具：不污染全局 rand() 的伪随机 ---------------- */

static uint32_t g_rnd = 0;

static void sip_rand_seed_once(void)
{
    struct timespec ts;

    if (g_rnd) return;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_rnd = (uint32_t)((uint64_t)ts.tv_nsec ^ ((uint64_t)(unsigned)getpid() << 16) ^ 0x5bf03635u);
    if (!g_rnd) g_rnd = 0x12345678u;
}

/* xorshift32：C 标准库的 rand() 是全局状态，测试代码也可能在用，
 * 信令层自己拿一套伪随机，互不干扰。 */
static uint32_t sip_rand(void)
{
    uint32_t x = g_rnd ? g_rnd : 0x12345678u;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rnd = x ? x : 0x1u;
    return g_rnd;
}

static void sip_make_branch(char *out, size_t cap)
{
    /* RFC 3261 要求 branch 以 "z9hG4bK" 开头（magic cookie），
     * 平台据此识别「这条事务是否已经处理过」。 */
    snprintf(out, cap, "z9hG4bK%08x%08x", sip_rand(), sip_rand());
}

/* ---------------- socket ---------------- */

/* 探测本机 IP：临时 UDP socket connect 到平台地址（不发包），
 * 再 getsockname 问内核「走这条路源地址是谁」。 */
static int sip_probe_local_ip(const char *server_ip, uint16_t server_port,
                              char *out, size_t cap)
{
    int fd;
    struct sockaddr_in sa;
    socklen_t sl = (socklen_t)sizeof(sa);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return GB_ERR;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &sa.sin_addr) != 1) {
        close(fd);
        return GB_ERR;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return GB_ERR;
    }
    if (getsockname(fd, (struct sockaddr *)&sa, &sl) < 0) {
        close(fd);
        return GB_ERR;
    }
    close(fd);

    return inet_ntop(AF_INET, &sa.sin_addr, out, (socklen_t)cap) ? GB_OK : GB_ERR;
}

int gb_sip_init(gb_sip_t *sip)
{
    gb_config_t *cfg = gb_config();
    struct sockaddr_in sa;
    int on = 1;

    if (!sip) return GB_ERR;
    memset(sip, 0, sizeof(*sip));
    sip->fd = -1;
    sip_rand_seed_once();

    sip->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sip->fd < 0) {
        LOGE("SIP", "socket() 失败：%s", strerror(errno));
        return GB_ERR;
    }

    /* SO_REUSEADDR 必设：否则调试时反复重启会被上一次的 socket 挡住，
     * bind 报 "Address already in use"，白白浪费几分钟。 */
    if (setsockopt(sip->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        LOGW("SIP", "setsockopt(SO_REUSEADDR) 失败：%s（继续）", strerror(errno));
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(cfg->local_port);
    if (bind(sip->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        LOGE("SIP", "bind 0.0.0.0:%u 失败：%s", (unsigned)cfg->local_port, strerror(errno));
        close(sip->fd);
        sip->fd = -1;
        return GB_ERR;
    }

    if (sip_probe_local_ip(cfg->server_ip, cfg->server_port,
                           sip->local_ip, sizeof(sip->local_ip)) != GB_OK) {
        snprintf(sip->local_ip, sizeof(sip->local_ip), "%s", "127.0.0.1");
        LOGW("SIP", "探测本机 IP 失败（平台 %s 不可达？），回退 127.0.0.1", cfg->server_ip);
    }

    /* call_id 缓冲 64 字节：IPv4 最长 15 字符，用 %.15s 限宽，避免格式截断告警 */
    snprintf(sip->call_id, sizeof(sip->call_id), "%08x%08x@%.15s",
             sip_rand(), sip_rand(), sip->local_ip);
    snprintf(sip->local_tag, sizeof(sip->local_tag), "%08x", sip_rand());
    sip->cseq = 0;

    LOGI("SIP", "本地 socket 就绪 %s:%u（平台 %s:%u，Call-ID %s）",
         sip->local_ip, (unsigned)cfg->local_port,
         cfg->server_ip, (unsigned)cfg->server_port, sip->call_id);
    return GB_OK;
}

void gb_sip_close(gb_sip_t *sip)
{
    if (!sip) return;
    if (sip->fd >= 0) close(sip->fd);
    sip->fd = -1;
}

int gb_sip_send_to(gb_sip_t *sip, const char *msg, const char *ip, uint16_t port)
{
    struct sockaddr_in sa;
    ssize_t n;

    if (!sip || sip->fd < 0 || !msg || !ip) return GB_ERR;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
        LOGE("SIP", "目标地址非法：%s", ip);
        return GB_ERR;
    }

    n = sendto(sip->fd, msg, strlen(msg), 0, (struct sockaddr *)&sa, sizeof(sa));
    if (n < 0) {
        LOGE("SIP", "sendto %s:%u 失败：%s", ip, (unsigned)port, strerror(errno));
        return GB_ERR;
    }
    return (int)n;
}

int gb_sip_send(gb_sip_t *sip, const char *msg)
{
    gb_config_t *cfg = gb_config();

    return gb_sip_send_to(sip, msg, cfg->server_ip, cfg->server_port);
}

uint32_t gb_sip_next_cseq(gb_sip_t *sip)
{
    if (!sip) return 1;
    /* 原子自增：心跳线程和主循环都会构造消息，普通 ++ 会丢号。
     * 用 GCC 内建原子操作，不引入 pthread 依赖。 */
    return __sync_add_and_fetch(&sip->cseq, 1);
}

/* ---------------- 头字段解析 ---------------- */

static int ci_eq(const char *a, size_t alen, const char *b, size_t blen)
{
    size_t i;

    if (alen != blen) return 0;
    for (i = 0; i < alen; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
}

/* 标准字段名 -> 紧凑形式（RFC 3261 §7.3.3，真实平台确实会发 l: / i: 这类） */
static char compact_of(const char *name, size_t nl)
{
    if (ci_eq(name, nl, "via", 3))            return 'v';
    if (ci_eq(name, nl, "from", 4))           return 'f';
    if (ci_eq(name, nl, "to", 2))             return 't';
    if (ci_eq(name, nl, "call-id", 7))        return 'i';
    if (ci_eq(name, nl, "contact", 7))        return 'm';
    if (ci_eq(name, nl, "content-length", 14))return 'l';
    if (ci_eq(name, nl, "content-type", 12))  return 'c';
    return '\0';
}

/* 逐行找头字段（大小写不敏感）。找到返回 1。 */
static int sip_find_header(const char *text, const char *name, char *out, size_t cap)
{
    const char *p;
    size_t nl;
    char compact;

    if (!text || !name || !out || cap == 0) return 0;
    out[0] = '\0';

    nl = strlen(name);
    if (nl && name[nl - 1] == ':') nl--;      /* 允许调用方带冒号调用 */
    compact = compact_of(name, nl);

    for (p = text; *p;) {
        const char *eol = strchr(p, '\n');
        const char *colon;
        size_t len;

        if (!eol) eol = p + strlen(p);
        colon = (const char *)memchr(p, ':', (size_t)(eol - p));

        /* 第一行是请求行/状态行，跳过（URI 里的 "sip:" 里也有冒号） */
        if (colon && p != text) {
            len = (size_t)(colon - p);
            if (ci_eq(p, len, name, nl) || (compact && len == 1 && (p[0] | 0x20) == compact)) {
                const char *v = colon + 1;
                size_t i = 0;
                while (v < eol && (*v == ' ' || *v == '\t')) v++;
                while (v < eol && *v != '\r' && *v != '\n' && i + 1 < cap) out[i++] = *v++;
                out[i] = '\0';
                return 1;
            }
        }

        if (*eol == '\0') break;
        p = eol + 1;
        if (*p == '\r' || *p == '\n') break;   /* 空行 = 头结束，后面的 body 不再看 */
    }
    return 0;
}

void gb_sip_header_value(const char *text, const char *name, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    (void)sip_find_header(text, name, out, cap);
}

static void strip_quotes(char *s)
{
    size_t n;

    if (!s || !*s) return;
    n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

/* key="value" 形式（Digest 的 realm/nonce/qop 都是这个形式）。
 * 边界判断很重要：不判前面那个字符，找 "nonce" 会命中 "cnonce"。 */
void gb_sip_kv_value(const char *text, const char *key, char *out, size_t cap)
{
    const char *p;
    size_t klen;

    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!text || !key) return;
    klen = strlen(key);
    if (klen == 0) return;

    for (p = text; (p = strstr(p, key)) != NULL; p++) {
        const char *q;
        size_t i;

        if (p > text) {
            char c = p[-1];
            if (isalnum((unsigned char)c) || c == '_' || c == '-') continue;
        }
        q = p + klen;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') continue;
        q++;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '"') continue;
        q++;
        for (i = 0; i + 1 < cap && *q && *q != '"'; i++) out[i] = *q++;
        out[i] = '\0';
        return;
    }
}

/* key=value 形式（不带引号，例如 qop=auth、algorithm=MD5）。
 * 按「原样」返回：值本身带引号就把引号一起带出来，调用方自己决定去不去。 */
void gb_sip_kv_value_raw(const char *text, const char *key, char *out, size_t cap)
{
    const char *p;
    size_t klen;

    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!text || !key) return;
    klen = strlen(key);
    if (klen == 0) return;

    for (p = text; (p = strstr(p, key)) != NULL; p++) {
        const char *q;
        size_t i;

        if (p > text) {
            char c = p[-1];
            if (isalnum((unsigned char)c) || c == '_' || c == '-') continue;
        }
        q = p + klen;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') continue;
        q++;
        while (*q == ' ' || *q == '\t') q++;
        for (i = 0; i + 1 < cap; ) {
            char c = *q;
            if (c == '\0' || c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
                c == ',' || c == ';' || c == ')') break;
            out[i++] = c;
            q++;
        }
        out[i] = '\0';
        return;
    }
}

/* ---------------- 消息构造 ---------------- */

int gb_sip_build_register(gb_sip_t *sip, char *buf, size_t cap, const char *auth_header)
{
    gb_config_t *cfg = gb_config();
    char uri[GB_SIP_URI_LEN];
    char branch[GB_SIP_BRANCH_LEN];
    char auth_line[GB_SIP_AUTH_LEN + 32];
    int n;

    if (!sip || !buf || cap == 0) return GB_ERR;

    snprintf(uri, sizeof(uri), "sip:%s@%s", cfg->platform_id, cfg->domain);
    sip_make_branch(branch, sizeof(branch));

    if (auth_header && auth_header[0]) {
        snprintf(auth_line, sizeof(auth_line), "Authorization: %s\r\n", auth_header);
    } else {
        auth_line[0] = '\0';
    }

    n = snprintf(buf, cap,
                 "REGISTER %s SIP/2.0\r\n"
                 "Via: SIP/2.0/UDP %s:%u;rport;branch=%s\r\n"
                 "From: <sip:%s@%s>;tag=%s\r\n"
                 "To: <sip:%s@%s>\r\n"
                 "Call-ID: %s\r\n"
                 "CSeq: %u REGISTER\r\n"
                 "Contact: <sip:%s@%s:%u>\r\n"
                 "Max-Forwards: 70\r\n"
                 "User-Agent: %s\r\n"
                 "Expires: %d\r\n"
                 "%s"
                 "Content-Length: 0\r\n\r\n",
                 uri,
                 sip->local_ip, (unsigned)cfg->local_port, branch,
                 cfg->device_id, cfg->domain, sip->local_tag,
                 cfg->platform_id, cfg->domain,
                 sip->call_id,
                 (unsigned)gb_sip_next_cseq(sip),
                 cfg->device_id, sip->local_ip, (unsigned)cfg->local_port,
                 SIP_USER_AGENT, cfg->expires,
                 auth_line);

    if (n < 0 || (size_t)n >= cap) {
        LOGE("SIP", "REGISTER 报文超过缓冲（%d >= %zu）", n, cap);
        return GB_ERR;
    }
    return n;
}

int gb_sip_build_message(gb_sip_t *sip, char *buf, size_t cap, const char *xml_body)
{
    gb_config_t *cfg = gb_config();
    char uri[GB_SIP_URI_LEN];
    char branch[GB_SIP_BRANCH_LEN];
    int n;

    if (!sip || !buf || cap == 0 || !xml_body) return GB_ERR;

    snprintf(uri, sizeof(uri), "sip:%s@%s", cfg->platform_id, cfg->domain);
    sip_make_branch(branch, sizeof(branch));

    n = snprintf(buf, cap,
                 "MESSAGE %s SIP/2.0\r\n"
                 "Via: SIP/2.0/UDP %s:%u;rport;branch=%s\r\n"
                 "From: <sip:%s@%s>;tag=%s\r\n"
                 "To: <sip:%s@%s>\r\n"
                 "Call-ID: %s\r\n"
                 "CSeq: %u MESSAGE\r\n"
                 "Max-Forwards: 70\r\n"
                 "User-Agent: %s\r\n"
                 "Content-Type: Application/MANSCDP+xml\r\n"
                 "Content-Length: %zu\r\n\r\n%s",
                 uri,
                 sip->local_ip, (unsigned)cfg->local_port, branch,
                 cfg->device_id, cfg->domain, sip->local_tag,
                 cfg->platform_id, cfg->domain,
                 sip->call_id,
                 (unsigned)gb_sip_next_cseq(sip),
                 SIP_USER_AGENT,
                 strlen(xml_body), xml_body);

    if (n < 0 || (size_t)n >= cap) {
        LOGE("SIP", "MESSAGE 报文超过缓冲（%d >= %zu）", n, cap);
        return GB_ERR;
    }
    return n;
}

/* 取请求行里的方法名（"INVITE sip:... SIP/2.0" -> "INVITE"） */
static const char *request_method(const char *req)
{
    static const char *const methods[] = {
        "INVITE", "ACK", "BYE", "CANCEL", "REGISTER",
        "OPTIONS", "MESSAGE", "INFO", "SUBSCRIBE", "NOTIFY"
    };
    size_t i;

    for (i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        size_t n = strlen(methods[i]);
        if (strncmp(req, methods[i], n) == 0 && req[n] == ' ') return methods[i];
    }
    return "MESSAGE";
}

int gb_sip_build_200(gb_sip_t *sip, char *buf, size_t cap,
                     const char *req, const char *extra_headers, const char *body)
{
    gb_config_t *cfg = gb_config();
    char via[512], from[512], to[512], cseq[128], callid[128], to_full[640];
    const char *extra = extra_headers ? extra_headers : "";
    int n;

    if (!sip || !buf || cap == 0 || !req) return GB_ERR;

    gb_sip_header_value(req, "Via", via, sizeof(via));
    gb_sip_header_value(req, "From", from, sizeof(from));
    gb_sip_header_value(req, "To", to, sizeof(to));
    gb_sip_header_value(req, "CSeq", cseq, sizeof(cseq));
    gb_sip_header_value(req, "Call-ID", callid, sizeof(callid));

    if (!via[0]) {
        snprintf(via, sizeof(via), "SIP/2.0/UDP %s:%u", sip->local_ip, (unsigned)cfg->local_port);
    }

    /* To 头必须带 tag。请求里已经带过就原样回（不能加第二个 tag，否则平台
     * 认为对不上事务）；没带就补本端 tag —— 这是 200 OK 的对话框标识。 */
    if (!to[0]) {
        snprintf(to_full, sizeof(to_full), "<sip:%s@%s>;tag=%s",
                 cfg->device_id, cfg->domain, sip->local_tag);
    } else if (strstr(to, "tag=")) {
        snprintf(to_full, sizeof(to_full), "%s", to);
    } else {
        snprintf(to_full, sizeof(to_full), "%s;tag=%s", to, sip->local_tag);
    }

    if (!callid[0]) snprintf(callid, sizeof(callid), "%s", sip->call_id);
    if (!cseq[0])   snprintf(cseq, sizeof(cseq), "%u %s",
                             (unsigned)gb_sip_next_cseq(sip), request_method(req));
    if (!from[0])   snprintf(from, sizeof(from), "<sip:%s@%s>;tag=%s",
                             cfg->device_id, cfg->domain, sip->local_tag);

    if (body) {
        n = snprintf(buf, cap,
                     "SIP/2.0 200 OK\r\n"
                     "Via: %s\r\n"
                     "From: %s\r\n"
                     "To: %s\r\n"
                     "Call-ID: %s\r\n"
                     "CSeq: %s\r\n"
                     "User-Agent: %s\r\n"
                     "%s"
                     "Content-Type: application/sdp\r\n"
                     "Content-Length: %zu\r\n\r\n%s",
                     via, from, to_full, callid, cseq, SIP_USER_AGENT,
                     extra, strlen(body), body);
    } else {
        n = snprintf(buf, cap,
                     "SIP/2.0 200 OK\r\n"
                     "Via: %s\r\n"
                     "From: %s\r\n"
                     "To: %s\r\n"
                     "Call-ID: %s\r\n"
                     "CSeq: %s\r\n"
                     "User-Agent: %s\r\n"
                     "%s"
                     "Content-Length: 0\r\n\r\n",
                     via, from, to_full, callid, cseq, SIP_USER_AGENT, extra);
    }

    if (n < 0 || (size_t)n >= cap) {
        LOGE("SIP", "200 OK 报文超过缓冲（%d >= %zu）", n, cap);
        return GB_ERR;
    }
    return n;
}

/* ---------------- Digest ---------------- */

int gb_sip_build_digest_auth(const char *user, const char *password,
                             const char *method, const char *uri,
                             const char *realm, const char *nonce, const char *qop,
                             const char *cnonce, unsigned nc,
                             char *out, size_t cap)
{
    char ha1[GB_MD5_HEX_LEN], ha2[GB_MD5_HEX_LEN], resp[GB_MD5_HEX_LEN];
    char buf[512];
    int n;

    if (!user || !password || !method || !uri || !nonce || !out || cap == 0) return GB_ERR;
    if (!realm)   realm = "";
    if (!qop)     qop = "";
    if (!cnonce)  cnonce = "";

    snprintf(buf, sizeof(buf), "%s:%s:%s", user, realm, password);   /* HA1 */
    gb_md5_hex_str(buf, ha1);

    snprintf(buf, sizeof(buf), "%s:%s", method, uri);                /* HA2 */
    gb_md5_hex_str(buf, ha2);

    if (qop[0]) {
        snprintf(buf, sizeof(buf), "%s:%s:%08x:%s:%s:%s", ha1, nonce, nc, cnonce, qop, ha2);
        gb_md5_hex_str(buf, resp);
        n = snprintf(out, cap,
                     "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
                     "response=\"%s\", algorithm=MD5, qop=%s, nc=%08x, cnonce=\"%s\"",
                     user, realm, nonce, uri, resp, qop, nc, cnonce);
    } else {
        /* 平台不给 qop 时走 RFC 2069 风格的老公式 —— 这条分支不能省 */
        snprintf(buf, sizeof(buf), "%s:%s:%s", ha1, nonce, ha2);
        gb_md5_hex_str(buf, resp);
        n = snprintf(out, cap,
                     "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
                     "response=\"%s\", algorithm=MD5",
                     user, realm, nonce, uri, resp);
    }

    if (n < 0 || (size_t)n >= cap) {
        LOGE("SIP", "Authorization 头超过缓冲（%d >= %zu）", n, cap);
        return GB_ERR;
    }
    return GB_OK;
}

/* ---------------- 注册流程 ---------------- */

/* 从 401/407 里取出鉴权挑战。有 qop 取 qop，没有就留空走无 qop 分支。 */
static int parse_challenge(const char *msg, char *realm, size_t realm_cap,
                           char *nonce, size_t nonce_cap, char *qop, size_t qop_cap)
{
    char hdr[GB_SIP_BUF];
    const char *src = msg;

    realm[0] = nonce[0] = qop[0] = '\0';

    if (sip_find_header(msg, "WWW-Authenticate", hdr, sizeof(hdr)) ||
        sip_find_header(msg, "Proxy-Authenticate", hdr, sizeof(hdr))) {
        src = hdr;
    }

    gb_sip_kv_value(src, "realm", realm, realm_cap);
    if (!realm[0]) { gb_sip_kv_value_raw(src, "realm", realm, realm_cap); strip_quotes(realm); }

    gb_sip_kv_value(src, "nonce", nonce, nonce_cap);
    if (!nonce[0]) { gb_sip_kv_value_raw(src, "nonce", nonce, nonce_cap); strip_quotes(nonce); }

    gb_sip_kv_value(src, "qop", qop, qop_cap);
    if (!qop[0]) { gb_sip_kv_value_raw(src, "qop", qop, qop_cap); strip_quotes(qop); }

    if (qop[0]) {
        /* qop 可能是列表 "auth,auth-int"：只取 auth */
        char *comma = strchr(qop, ',');
        char *s;
        if (comma) *comma = '\0';
        s = qop;
        while (*s == ' ' || *s == '\t') s++;
        if (s != qop) memmove(qop, s, strlen(s) + 1);
        if (strcmp(qop, "auth") != 0) {
            LOGW("SIP", "平台只提供 qop=%s，本项目只实现了 qop=auth", qop);
        }
    }

    return nonce[0] ? GB_OK : GB_ERR;
}

int gb_sip_register(gb_sip_t *sip)
{
    gb_config_t *cfg = gb_config();
    char uri[GB_SIP_URI_LEN];
    char auth[GB_SIP_AUTH_LEN];
    char cnonce[32];
    char req[GB_SIP_BUF];
    char buf[GB_SIP_BUF];
    char realm[128], nonce[128], qop[64];
    char last[160];
    /* SIP 重传节奏：T1=500ms 起翻倍（RFC 3261 的经典参数，属于协议常量） */
    static const int wait_ms[3] = { 1000, 2000, 4000 };
    int attempt, ok = 0;

    if (!sip || sip->fd < 0) return GB_ERR;

    snprintf(uri, sizeof(uri), "sip:%s@%s", cfg->platform_id, cfg->domain);
    snprintf(cnonce, sizeof(cnonce), "%08x", sip_rand());
    auth[0] = '\0';
    realm[0] = nonce[0] = qop[0] = '\0';
    last[0] = '\0';

    LOGI("SIP", "开始注册 %s（本机 %s:%u）", uri, sip->local_ip, (unsigned)cfg->local_port);

    for (attempt = 0; attempt < 3 && !ok; attempt++) {
        int waited = 0;

        if (gb_sip_build_register(sip, req, sizeof(req), auth[0] ? auth : NULL) < 0) return GB_ERR;
        if (gb_sip_send(sip, req) < 0) return GB_ERR;
        LOGI("SIP", "REGISTER 已发送（%s）", auth[0] ? "带 Digest 认证" : "无认证");

        while (waited < wait_ms[attempt] && !ok) {
            fd_set rset;
            struct timeval tv;
            int slice = wait_ms[attempt] - waited;
            int r;

            if (slice > 500) slice = 500;
            tv.tv_sec = slice / 1000;
            tv.tv_usec = (slice % 1000) * 1000;

            FD_ZERO(&rset);
            FD_SET(sip->fd, &rset);
            r = select(sip->fd + 1, &rset, NULL, NULL, &tv);
            if (r < 0) {
                if (errno == EINTR) continue;
                LOGE("SIP", "select 失败：%s", strerror(errno));
                return GB_ERR;
            }
            if (r == 0) {                 /* 超时：继续等本次窗口 */
                waited += slice;
                continue;
            }

            {
                ssize_t got = recv(sip->fd, buf, sizeof(buf) - 1, 0);
                const char *eol;
                size_t ll;

                if (got <= 0) continue;
                buf[got] = '\0';
                eol = strpbrk(buf, "\r\n");
                ll = eol ? (size_t)(eol - buf) : strlen(buf);
                if (ll >= sizeof(last)) ll = sizeof(last) - 1;
                memcpy(last, buf, ll);
                last[ll] = '\0';
            }
            waited = wait_ms[attempt];    /* 已有响应，不再等 */

            if (strncmp(buf, "SIP/2.0 401", 11) == 0 || strncmp(buf, "SIP/2.0 407", 11) == 0) {
                if (parse_challenge(buf, realm, sizeof(realm),
                                    nonce, sizeof(nonce), qop, sizeof(qop)) != GB_OK) {
                    LOGE("SIP", "401/407 里没有 nonce，无法计算 Digest：%s", last);
                    return GB_ERR;
                }
                if (!realm[0]) snprintf(realm, sizeof(realm), "%s", cfg->domain);
                if (gb_sip_build_digest_auth(cfg->device_id, cfg->device_pwd,
                                             "REGISTER", uri, realm, nonce, qop,
                                             cnonce, 1, auth, sizeof(auth)) != GB_OK) {
                    return GB_ERR;
                }
                LOGI("SIP", "收到 401（realm=%s，qop=%s）→ 已算出 Digest response",
                     realm, qop[0] ? qop : "无");
                break;                    /* 带 auth 重发 */
            }

            if (strncmp(buf, "SIP/2.0 2", 9) == 0) {
                LOGI("SIP", "注册成功：%s", last);
                ok = 1;
            } else if (buf[0] == 'S' && (strncmp(buf, "SIP/2.0 4", 9) == 0 ||
                                         strncmp(buf, "SIP/2.0 5", 9) == 0)) {
                LOGE("SIP", "注册被拒绝：%s", last);
                if (auth[0]) return GB_ERR;   /* 带上鉴权还被拒，重试没意义 */
            }
            /* 其它请求（例如平台上立刻下发的 MESSAGE）在注册阶段先忽略 */
        }
    }

    if (!ok) {
        LOGE("SIP", "注册失败：3 次尝试都没等到 200 OK（最后响应：%s）",
             last[0] ? last : "无响应");
        LOGE("SIP", "排查顺序：server_ip/server_port 是否可达 → 设备 ID 是否 20 位 → 密码是否正确 → 平台是否要求 qop");
        return GB_ERR;
    }
    return GB_OK;
}
