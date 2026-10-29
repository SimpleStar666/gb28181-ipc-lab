/*
 * gb_config.c — 配置解析（手写 key = value 解析器）
 * ============================================================
 * 为什么要自己写解析：
 *   嵌入式设备端连 libini 都嫌重，而且「解析 20 个键」这件事本身
 *   就是一个很好的教学样本 —— 校验、报错、定位行号，一样都不能少。
 *
 * 三个必须做到的细节：
 *   1. 键名与 gb_config_t 字段一一对应，用一张 offsetof 表驱动，
 *      加字段时只改表，不会出现「解析了但没赋值」的幽灵字段。
 *   2. 数值越界/非法立刻报错并指出行号：设备 ID 长度错了，
 *      平台只会回一个含糊的 403/400，事后抓包找原因非常费劲。
 *   3. 未知键只告警不报错 —— 老配置文件里多几个键不该让设备起不来。
 */
#include "gb_config.h"
#include "gb_common.h"   /* GB_OK / GB_ERR（gb_config.h 本身只依赖 stddef/stdint） */
#include "gb_log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    T_STR, T_U8, T_U16, T_U32, T_INT
} ftype_t;

typedef struct {
    const char *key;
    ftype_t     type;
    size_t      off;      /* 字段偏移 */
    size_t      cap;      /* T_STR：字段容量 */
    long        lo, hi;   /* 数值范围 */
} keymap_t;

/* 这张表就是「配置文件键名 ↔ gb_config_t 字段」的唯一映射。
 * 注意 cap 用 sizeof(((gb_config_t *)0)->field) 在编译期取到真实容量，
 * 手写数字迟早会和头文件对不上。 */
static const keymap_t KEYMAP[] = {
    /* --- SIP 信令 --- */
    { "server_ip",     T_STR, offsetof(gb_config_t, server_ip),     sizeof(((gb_config_t *)0)->server_ip),     0, 0 },
    { "server_port",   T_U16, offsetof(gb_config_t, server_port),   0, 1, 65535 },
    { "local_port",    T_U16, offsetof(gb_config_t, local_port),    0, 1, 65535 },
    { "device_id",     T_STR, offsetof(gb_config_t, device_id),     sizeof(((gb_config_t *)0)->device_id),     0, 0 },
    { "device_pwd",    T_STR, offsetof(gb_config_t, device_pwd),    sizeof(((gb_config_t *)0)->device_pwd),    0, 0 },
    { "platform_id",   T_STR, offsetof(gb_config_t, platform_id),   sizeof(((gb_config_t *)0)->platform_id),   0, 0 },
    { "domain",        T_STR, offsetof(gb_config_t, domain),        sizeof(((gb_config_t *)0)->domain),        0, 0 },
    { "channel_id",    T_STR, offsetof(gb_config_t, channel_id),    sizeof(((gb_config_t *)0)->channel_id),    0, 0 },
    { "device_name",   T_STR, offsetof(gb_config_t, device_name),   sizeof(((gb_config_t *)0)->device_name),   0, 0 },

    /* --- 媒体 --- */
    { "rtp_pt",        T_U8,  offsetof(gb_config_t, rtp_pt),        0, 0, 127 },
    { "mtu",           T_U16, offsetof(gb_config_t, mtu),           0, 0, 65507 },
    { "fps",           T_U16, offsetof(gb_config_t, fps),           0, 1, 240 },
    { "ssrc_default",  T_U32, offsetof(gb_config_t, ssrc_default),  0, 0, 0xFFFFFFFFL },
    { "h264_file",     T_STR, offsetof(gb_config_t, h264_file),     sizeof(((gb_config_t *)0)->h264_file),     0, 0 },
    { "fifo_path",     T_STR, offsetof(gb_config_t, fifo_path),     sizeof(((gb_config_t *)0)->fifo_path),     0, 0 },

    /* --- 音频 --- */
    { "audio_enable",  T_INT, offsetof(gb_config_t, audio_enable),  0, 0, 1 },
    { "audio_pt",      T_INT, offsetof(gb_config_t, audio_pt),      0, 0, 127 },
    { "audio_rate",    T_INT, offsetof(gb_config_t, audio_rate),    0, 1000, 48000 },
    { "audio_channels",T_INT, offsetof(gb_config_t, audio_channels),0, 1, 2 },

    /* --- 行为 --- */
    { "heartbeat_s",   T_INT, offsetof(gb_config_t, heartbeat_s),   0, 1, 86400 },
    { "expires",       T_INT, offsetof(gb_config_t, expires),       0, 1, 86400 },
    { "log_level",     T_INT, offsetof(gb_config_t, log_level),     0, 0, 3 },

    /* --- 兼容别名：老配置文件/别的命名习惯也能吃进来 --- */
    { "sip_server_ip",   T_STR, offsetof(gb_config_t, server_ip),  sizeof(((gb_config_t *)0)->server_ip),  0, 0 },
    { "sip_server_port", T_U16, offsetof(gb_config_t, server_port),0, 1, 65535 },
    { "sip_local_port",  T_U16, offsetof(gb_config_t, local_port), 0, 1, 65535 },
    { "device_password", T_STR, offsetof(gb_config_t, device_pwd), sizeof(((gb_config_t *)0)->device_pwd), 0, 0 },
    { "password",        T_STR, offsetof(gb_config_t, device_pwd), sizeof(((gb_config_t *)0)->device_pwd), 0, 0 }
};

#define KEYMAP_N ((int)(sizeof(KEYMAP) / sizeof(KEYMAP[0])))

/* 全局单例：各模块通过 gb_config() 读，避免到处传参 */
static gb_config_t  g_default_cfg;
static int          g_default_ready = 0;
static gb_config_t *g_active = NULL;

static void cfg_set_defaults(gb_config_t *c)
{
    memset(c, 0, sizeof(*c));

    snprintf(c->server_ip,   sizeof(c->server_ip),   "%s", "192.168.0.197");
    c->server_port = 5060;
    c->local_port  = 5061;
    snprintf(c->device_id,   sizeof(c->device_id),   "%s", "34020000001320000001");
    snprintf(c->device_pwd,  sizeof(c->device_pwd),  "%s", "12345678");
    snprintf(c->platform_id, sizeof(c->platform_id), "%s", "44030000002000000001");
    snprintf(c->domain,      sizeof(c->domain),      "%s", "4403000000");
    /* channel_id 故意留空：载入结束时由 cfg_finalize() 用 device_id 补齐。
     * 若在这里写死默认通道号，配置文件只改了 device_id 时通道号会「留着旧值」，
     * 表现为目录上报里的通道编码和设备编码不一致（平台侧看不到通道）。 */
    c->channel_id[0] = '\0';
    /* 设备名是 UTF-8，MANSCDP 的 XML 声明也必须是 UTF-8（见 gb_manscdp.c） */
    snprintf(c->device_name, sizeof(c->device_name), "%s", "TS模拟摄像头");

    c->rtp_pt       = 96;            /* 96 = PS over RTP 的动态载荷类型 */
    c->mtu          = 1400;          /* +40 字节 RTP/UDP/IP 头 = 1440 < 1500 */
    c->fps          = 15;
    c->ssrc_default = 0x12345678u;
    /* 与 config.ini / README 快速开始指向同一份码流（tools/gen_teststream.sh 生成），
     * 否则「配置文件的默认值」和「内置默认值」会把人带到两个不同的文件上。 */
    snprintf(c->h264_file, sizeof(c->h264_file), "%s", "assets/testsrc.h264");
    c->fifo_path[0] = '\0';          /* 空 = 文件循环模式；-live 时由 main 填 */

    c->audio_enable   = 1;
    c->audio_pt       = 8;           /* PCMA（G.711A）静态载荷类型 */
    c->audio_rate     = 8000;
    c->audio_channels = 1;

    c->heartbeat_s = 60;
    c->expires     = 3600;
    c->log_level   = GB_LOG_INFO;
}

/* 解析完成后的收尾：单通道设备的通道编码缺省等于设备编码。
 * 必须放在「解析之后」——放在默认值里会把配置文件里的 device_id 盖掉。 */
static void cfg_finalize(gb_config_t *c)
{
    if (c->channel_id[0] == '\0') {
        snprintf(c->channel_id, sizeof(c->channel_id), "%s", c->device_id);
    }
}

static char *trim_inplace(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        e--;
        *e = '\0';
    }
    return s;
}

static int cfg_set_field(gb_config_t *c, const keymap_t *k, const char *val,
                         const char *path, int lineno)
{
    char *base = (char *)c + k->off;

    if (k->type == T_STR) {
        if (strlen(val) >= k->cap) {
            LOGE("CONFIG", "%s:%d 键 %s 的值太长（最多 %zu 字节）",
                 path, lineno, k->key, k->cap - 1);
            return GB_ERR;
        }
        snprintf(base, k->cap, "%s", val);
        return GB_OK;
    }

    {
        char *end = NULL;
        long v;

        errno = 0;
        v = strtol(val, &end, (k->type == T_U32) ? 0 : 10);
        if (end == val || (end && *end != '\0') || errno != 0) {
            LOGE("CONFIG", "%s:%d 键 %s 的值不是合法数字：'%s'", path, lineno, k->key, val);
            return GB_ERR;
        }
        if (v < k->lo || v > k->hi) {
            LOGE("CONFIG", "%s:%d 键 %s = %ld 超出范围 [%ld, %ld]",
                 path, lineno, k->key, v, k->lo, k->hi);
            return GB_ERR;
        }
        switch (k->type) {
        case T_U8:  *(uint8_t *)base  = (uint8_t)v;  break;
        case T_U16: *(uint16_t *)base = (uint16_t)v; break;
        case T_U32: *(uint32_t *)base = (uint32_t)v; break;
        default:    *(int *)base      = (int)v;      break;
        }
    }
    return GB_OK;
}

int gb_config_load(gb_config_t *cfg, const char *path)
{
    FILE *fp;
    char line[512];
    int lineno = 0;

    if (!cfg) return GB_ERR;

    cfg_set_defaults(cfg);

    if (!path) {
        LOGI("CONFIG", "未指定配置文件，使用内置默认值");
        cfg_finalize(cfg);
        return GB_OK;
    }

    fp = fopen(path, "r");
    if (!fp) {
        LOGE("CONFIG", "打开配置文件失败：%s（%s）", path, strerror(errno));
        return GB_ERR;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *s, *eq, *key, *val, *hash;
        int i, found = 0;

        lineno++;
        s = trim_inplace(line);
        if (*s == '\0' || *s == '#' || *s == ';') continue;   /* 空行与整行注释 */

        eq = strchr(s, '=');
        if (!eq) {
            LOGE("CONFIG", "%s:%d 不是 key = value 形式：'%s'", path, lineno, s);
            fclose(fp);
            return GB_ERR;
        }
        *eq = '\0';
        key = trim_inplace(s);
        val = trim_inplace(eq + 1);

        /* 行尾注释：'#' 或 ';' 之后一律算注释（值里不能出现这两个字符） */
        hash = strpbrk(val, "#;");
        if (hash) {
            *hash = '\0';
            val = trim_inplace(val);
        }

        if (*key == '\0') {
            LOGE("CONFIG", "%s:%d 键名为空", path, lineno);
            fclose(fp);
            return GB_ERR;
        }

        for (i = 0; i < KEYMAP_N; i++) {
            if (strcmp(key, KEYMAP[i].key) != 0) continue;
            found = 1;
            if (cfg_set_field(cfg, &KEYMAP[i], val, path, lineno) != GB_OK) {
                fclose(fp);
                return GB_ERR;
            }
            LOGD("CONFIG", "%s:%d %s = %s", path, lineno, key, val);
            break;
        }
        if (!found) {
            /* 不认识就告警跳过：多一个键不该让设备起不来 */
            LOGW("CONFIG", "%s:%d 未知键 '%s'，已忽略", path, lineno, key);
        }
    }
    fclose(fp);

    cfg_finalize(cfg);
    return GB_OK;
}

static int all_digits(const char *s)
{
    size_t i;

    if (!s || !*s) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

int gb_config_validate(const gb_config_t *cfg)
{
    int bad = 0;
    struct in_addr a;

    if (!cfg) return GB_ERR;

    if (cfg->server_ip[0] == '\0') {
        LOGE("CONFIG", "server_ip 为空");
        bad = 1;
    } else if (inet_pton(AF_INET, cfg->server_ip, &a) != 1) {
        LOGE("CONFIG", "server_ip 不是合法的 IPv4 地址：'%s'", cfg->server_ip);
        bad = 1;
    }
    if (cfg->server_port == 0) { LOGE("CONFIG", "server_port 不能为 0"); bad = 1; }
    if (cfg->local_port == 0)  { LOGE("CONFIG", "local_port 不能为 0");  bad = 1; }

    if (strlen(cfg->device_id) != 20 || !all_digits(cfg->device_id)) {
        LOGE("CONFIG", "device_id 必须是 20 位数字（当前 '%s'，%zu 位）"
             " —— 长度不对会被平台拒注册，而平台通常只回一个含糊的 403/400",
             cfg->device_id, strlen(cfg->device_id));
        bad = 1;
    }
    if (strlen(cfg->platform_id) != 20 || !all_digits(cfg->platform_id)) {
        LOGE("CONFIG", "platform_id 必须是 20 位数字（当前 '%s'）", cfg->platform_id);
        bad = 1;
    }
    if (cfg->domain[0] == '\0' || !all_digits(cfg->domain)) {
        LOGE("CONFIG", "domain 不能为空且必须是数字（当前 '%s'）", cfg->domain);
        bad = 1;
    }
    if (cfg->channel_id[0] == '\0') {
        /* 单通道设备常见：通道编码 = 设备编码。这里只告警，不就地改 const 参数，
         * gb_config_load 已经在载入结束时补齐过；XML 生成处也会再兜底一次。 */
        LOGW("CONFIG", "channel_id 为空，将按 device_id 处理");
    }
    if (strlen(cfg->device_pwd) == 0) {
        LOGW("CONFIG", "device_pwd 为空：平台若开启鉴权会注册失败");
    }

    if (cfg->rtp_pt < 96 || cfg->rtp_pt > 127) {
        LOGE("CONFIG", "rtp_pt = %u 非法：PS over RTP 必须用动态载荷类型 96..127"
             "（<96 是静态类型，且必须在 SDP 里用 a=rtpmap 声明）", cfg->rtp_pt);
        bad = 1;
    }
    if (cfg->mtu < 576 || cfg->mtu > 65507) {
        LOGE("CONFIG", "mtu = %u 非法：合法区间 576..65507（建议 1400，加 40 字节协议头仍 < 1500）", cfg->mtu);
        bad = 1;
    }
    if (cfg->fps == 0 || cfg->fps > 240) {
        LOGE("CONFIG", "fps = %u 非法", cfg->fps);
        bad = 1;
    }
    if (cfg->audio_enable) {
        if (cfg->audio_rate != 8000 || cfg->audio_channels != 1) {
            LOGW("CONFIG", "G.711A 固定 8kHz 单声道，当前 audio_rate=%d channels=%d 会被忽略",
                 cfg->audio_rate, cfg->audio_channels);
        }
        if (cfg->audio_pt <= 0 || cfg->audio_pt > 127) {
            LOGE("CONFIG", "audio_pt = %d 非法（G.711A 通常是 8 = PCMA）", cfg->audio_pt);
            bad = 1;
        }
    }
    if (cfg->heartbeat_s <= 0) { LOGE("CONFIG", "heartbeat_s 必须 > 0"); bad = 1; }
    if (cfg->expires < 60)     { LOGE("CONFIG", "expires 必须 >= 60 秒（太小会被平台频繁要求重注册）"); bad = 1; }

    return bad ? GB_ERR : GB_OK;
}

void gb_config_set_global(gb_config_t *cfg)
{
    g_active = cfg;
}

gb_config_t *gb_config(void)
{
    if (g_active) return g_active;
    if (!g_default_ready) {           /* 没设过就懒加载一份默认值， */
        cfg_set_defaults(&g_default_cfg);   /* 保证任何模块都不会拿到 NULL */
        cfg_finalize(&g_default_cfg);
        g_default_ready = 1;
    }
    return &g_default_cfg;
}

void gb_config_dump(const gb_config_t *cfg)
{
    size_t i;
    char masked[64];

    if (!cfg) return;

    /* 密码打码：启动自检要能核对配置，但没必要把口令写进日志文件 */
    i = strlen(cfg->device_pwd);
    if (i >= sizeof(masked)) i = sizeof(masked) - 1;
    memset(masked, '*', i);
    masked[i] = '\0';

    LOGI("CONFIG", "---------------- 配置自检 ----------------");
    LOGI("CONFIG", "server_ip      = %s", cfg->server_ip);
    LOGI("CONFIG", "server_port    = %u", (unsigned)cfg->server_port);
    LOGI("CONFIG", "local_port     = %u", (unsigned)cfg->local_port);
    LOGI("CONFIG", "device_id      = %s", cfg->device_id);
    LOGI("CONFIG", "device_pwd     = %s", masked);
    LOGD("CONFIG", "device_pwd(明文，仅排障才看) = %s", cfg->device_pwd);
    LOGI("CONFIG", "platform_id    = %s", cfg->platform_id);
    LOGI("CONFIG", "domain         = %s", cfg->domain);
    LOGI("CONFIG", "channel_id     = %s", cfg->channel_id);
    LOGI("CONFIG", "device_name    = %s", cfg->device_name);
    LOGI("CONFIG", "rtp_pt         = %u", (unsigned)cfg->rtp_pt);
    LOGI("CONFIG", "mtu            = %u", (unsigned)cfg->mtu);
    LOGI("CONFIG", "fps            = %u", (unsigned)cfg->fps);
    LOGI("CONFIG", "ssrc_default   = 0x%08X", cfg->ssrc_default);
    LOGI("CONFIG", "h264_file      = %s", cfg->h264_file[0] ? cfg->h264_file : "(空)");
    LOGI("CONFIG", "fifo_path      = %s", cfg->fifo_path[0] ? cfg->fifo_path : "(空 = 文件循环模式)");
    LOGI("CONFIG", "audio_enable   = %d", cfg->audio_enable);
    LOGI("CONFIG", "audio_pt       = %d", cfg->audio_pt);
    LOGI("CONFIG", "audio          = %d Hz / %d ch", cfg->audio_rate, cfg->audio_channels);
    LOGI("CONFIG", "heartbeat_s    = %d", cfg->heartbeat_s);
    LOGI("CONFIG", "expires        = %d", cfg->expires);
    LOGI("CONFIG", "log_level      = %d", cfg->log_level);
    LOGI("CONFIG", "-----------------------------------------");
}
