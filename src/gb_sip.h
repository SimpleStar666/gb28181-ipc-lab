/*
 * gb_sip.h — SIP 信令 + Digest 鉴权
 * ============================================================
 * GB28181 的控制面走 SIP（RFC 3261），设备端要处理四件事：
 *   1. REGISTER   注册（带 Digest 鉴权，401 → 重发）
 *   2. MESSAGE    收发 MANSCDP XML（心跳、目录、设备信息）
 *   3. INVITE     接收点播；回 200 OK + SDP；随后开始推流
 *   4. BYE        停止推流
 *
 * 【Digest 鉴权（RFC 2617）——面试高频，务必说清每个缩写】
 *   平台回 401 并在 WWW-Authenticate 里给出：
 *     realm  —— 认证域，通常是平台编码
 *     nonce  —— 服务器生成的一次性随机串，防止重放
 *     qop    —— quality of protection，取值 "auth" 表示只认证不加密
 *   设备要回 Authorization 头，其中：
 *     HA1      = MD5(username : realm : password)
 *     HA2      = MD5(method : uri)                  method 这里是 "REGISTER"
 *     response = MD5(HA1 : nonce : nc : cnonce : qop : HA2)     （qop=auth 时）
 *                MD5(HA1 : nonce : HA2)                          （无 qop 时）
 *   其中：
 *     nc     = nonce count，本次是第几次用这个 nonce，8 位十六进制，首次 00000001
 *     cnonce = client nonce，客户端自己生成的随机串，参与运算防止服务器预计算
 *
 * 【踩坑记录】不用 qop 的那条分支也必须实现。
 *   不同平台/WVP 版本行为不同，只写 qop=auth 分支会在某些平台上注册失败，
 *   而且失败时平台往往只回一个 403，看不出是鉴权串算错了。
 */
#ifndef GB_SIP_H
#define GB_SIP_H

#include <stddef.h>
#include <stdint.h>
#include "gb_common.h"

#define GB_SIP_BUF        8192
#define GB_SIP_URI_LEN    128
#define GB_SIP_AUTH_LEN   512
#define GB_SIP_BRANCH_LEN 32

typedef struct {
    int      fd;                    /* UDP socket */
    uint32_t cseq;                  /* CSeq 序号，每条新请求 +1 */
    char     call_id[64];           /* 会话标识，注册流程内保持不变 */
    char     local_tag[64];         /* From 头的 tag */
    char     local_ip[64];          /* 本机 IP，写进 Via / Contact */
} gb_sip_t;

/* 建立 UDP socket 并 bind 到 cfg->local_port。
 * 务必设置 SO_REUSEADDR —— 否则调试时反复重启会被
 * "bind: Address already in use" 挡住，白白浪费几分钟。 */
int  gb_sip_init(gb_sip_t *sip);
void gb_sip_close(gb_sip_t *sip);

/* 发送到配置里的平台地址。返回 sendto 的返回值。 */
int  gb_sip_send(gb_sip_t *sip, const char *msg);
/* 发送到指定地址（点播时 RTP 目标由 SDP 决定，与 SIP 服务器可能不同） */
int  gb_sip_send_to(gb_sip_t *sip, const char *msg, const char *ip, uint16_t port);

uint32_t gb_sip_next_cseq(gb_sip_t *sip);

/* ---- 消息构造（返回写入长度，失败返回 GB_ERR） ---- */
int gb_sip_build_register(gb_sip_t *sip, char *buf, size_t cap, const char *auth_header);
int gb_sip_build_message (gb_sip_t *sip, char *buf, size_t cap, const char *xml_body);
int gb_sip_build_200     (gb_sip_t *sip, char *buf, size_t cap,
                          const char *req, const char *extra_headers, const char *body);

/* ---- 字段提取 ---- */
/* 取某个头字段的值（"Via:"、"CSeq:" ...）。取不到时 out 置空串。 */
void gb_sip_header_value(const char *text, const char *name, char *out, size_t cap);
/* 取 key="value" 形式（Digest 的 realm/nonce/qop 就是这个形式） */
void gb_sip_kv_value(const char *text, const char *key, char *out, size_t cap);
/* 取 key=value（不带引号，例如 qop=auth、algorithm=MD5） */
void gb_sip_kv_value_raw(const char *text, const char *key, char *out, size_t cap);

/* ---- Digest ---- */
/* 构造 Authorization 头的值（不含 "Authorization: " 前缀）。
 * qop 传 NULL 或空串走「无 qop」分支。返回 GB_OK / GB_ERR。 */
int gb_sip_build_digest_auth(const char *user, const char *password,
                             const char *method, const char *uri,
                             const char *realm, const char *nonce, const char *qop,
                             const char *cnonce, unsigned nc,
                             char *out, size_t cap);

/* 完整注册流程：REGISTER → 401 → 带 Authorization 重发 → 200。
 * 返回 GB_OK / GB_ERR（失败时打印最后一次响应首行，便于定位）。 */
int gb_sip_register(gb_sip_t *sip);

#endif /* GB_SIP_H */
