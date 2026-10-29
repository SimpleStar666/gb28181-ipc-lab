/*
 * gb_config.h — 配置
 * ============================================================
 * 原版把 SIP 服务器 IP、设备 ID、密码、码流路径全部 #define 在源码里，
 * 换一个部署环境就得重新编译。真实设备端不会这么干：
 *   出厂烧写的是配置文件（/etc/gb28181.conf 之类），代码一行不改。
 *
 * 本模块用最土的 `key = value` 文本格式，手写解析，不引第三方库 ——
 * 嵌入式环境连 libini 都嫌重。顺带也就有了一个「解析器怎么写」的教学样本。
 *
 * 反过来的教学点：解析配置必须**校验**。设备 ID 是国标 20 位数字，
 * 长度不对会导致 REGISTER 被平台拒绝，而平台只会回你一个含糊的 403/400。
 * 与其事后抓包找原因，不如在启动时就把配置打出来核对一遍（gb_config_dump）。
 */
#ifndef GB_CONFIG_H
#define GB_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define GB_CFG_STR 128

typedef struct {
    /* --- SIP 信令 --- */
    char     server_ip[64];       /* 平台 SIP 服务器地址 */
    uint16_t server_port;         /* 通常是 5060 */
    uint16_t local_port;          /* 设备本地 SIP 端口，通常 5061 */
    char     device_id[32];       /* 国标 20 位设备编码 */
    char     device_pwd[64];      /* SIP 注册密码（Digest 鉴权用） */
    char     platform_id[32];     /* 平台国标编码 */
    char     domain[32];          /* SIP 域，通常是平台编码前 10 位 */
    char     channel_id[32];      /* 通道编码（单通道设备通常等于设备 ID） */
    char     device_name[GB_CFG_STR];  /* 显示名，UTF-8 */

    /* --- 媒体 --- */
    uint8_t  rtp_pt;              /* 默认 96 */
    uint16_t mtu;                 /* RTP 载荷上限，默认 1400 */
    uint16_t fps;
    uint32_t ssrc_default;        /* SDP 里没有 y= 时用的默认 SSRC */
    char     h264_file[256];      /* 文件循环模式的裸流路径 */
    char     fifo_path[256];      /* -live 模式的命名管道 */

    /* --- 音频 --- */
    int      audio_enable;        /* 1 = 在 PS 里复用 G.711A 音频 */
    int      audio_pt;            /* SDP audio 载荷类型，8 = PCMA（G.711A） */
    int      audio_rate;          /* 8000 */
    int      audio_channels;      /* 1 */

    /* --- 行为 --- */
    int      heartbeat_s;         /* 心跳周期，默认 60 秒 */
    int      expires;             /* 注册有效期，默认 3600 秒 */
    int      log_level;           /* 0=error 1=warn 2=info 3=debug */
} gb_config_t;

/* 载入配置。path 为 NULL 时用内置默认值。
 * 返回 GB_OK / GB_ERR。出错会打印具体哪一行有问题。 */
int gb_config_load(gb_config_t *cfg, const char *path);

/* 把配置打到 stdout —— 启动时必做的自检，比事后抓包省事。 */
void gb_config_dump(const gb_config_t *cfg);

/* 全局单例：各模块通过它读配置，避免到处传参。
 * 由 main() 在 gb_config_load 之后调用 gb_config_set_global 设置。 */
void gb_config_set_global(gb_config_t *cfg);
gb_config_t *gb_config(void);

/* 校验：设备 ID 必须 20 位数字、端口范围合法等。返回 GB_OK / GB_ERR。 */
int gb_config_validate(const gb_config_t *cfg);

#endif /* GB_CONFIG_H */
