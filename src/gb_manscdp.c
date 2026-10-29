/*
 * gb_manscdp.c — MANSCDP XML（心跳 / 目录 / 设备信息）
 * ============================================================
 * 三个必须做对的地方：
 *   1. XML 声明与实际字节一致：源码里的设备名是 UTF-8，声明就必须是 UTF-8。
 *      原型声明 encoding="GB2312" 却发 UTF-8 字节 —— 平台按 GB2312 解 UTF-8，
 *      中文名必然乱码。GB/T 28181-2016 允许 UTF-8，这里统一用
 *      GB_MANSCDP_XML_DECL（头文件里已定义）。
 *   2. SN 必须回填请求里的值：设备应答的 SN 和平台请求的 SN 对不上，
 *      平台就匹配不到这条应答，界面表现为「点了查询一直转圈」。
 *   3. 文本要转义：设备名里出现 & < > 时，不转义就是一个非法 XML，
 *      平台直接解析失败（而且报错信息通常只有一句「报文格式错误」）。
 */
#include "gb_manscdp.h"
#include "gb_common.h"   /* GB_OK / GB_ERR（gb_manscdp.h 只依赖 stddef） */
#include "gb_config.h"
#include "gb_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 设备固有属性：gb_config_t 里没有厂商/型号/固件字段 ——
 * 它们是固件的一部分（烧在固件里），不是每个现场都要改的部署参数。
 * 真要做成可配置，往 gb_config_t 加字段即可，但只要没加，就不该散落在各处。 */
static const char *const DEV_MANUFACTURER = "TS";
static const char *const DEV_MODEL        = "SIM-IPC";
static const char *const DEV_FIRMWARE     = "V1.0";
static const char *const DEV_OWNER        = "ts";
static const char *const DEV_ADDRESS      = "WSL";

/* snprintf 返回「本该写多少字节」，>= cap 说明被截断了 —— 截断的 XML 绝不能发出去 */
static int xml_check(int n, size_t cap)
{
    return (n >= 0 && (size_t)n < cap) ? GB_OK : GB_ERR;
}

static size_t xml_escape(char *out, size_t cap, const char *in)
{
    size_t o = 0;

    if (!out || cap == 0) return 0;
    if (!in) { out[0] = '\0'; return 0; }

    for (; *in; in++) {
        const char *rep = NULL;
        switch (*in) {
        case '&':  rep = "&amp;";  break;
        case '<':  rep = "&lt;";   break;
        case '>':  rep = "&gt;";   break;
        case '"':  rep = "&quot;"; break;
        case '\'': rep = "&apos;"; break;
        default: break;
        }
        if (rep) {
            size_t rl = strlen(rep);
            if (o + rl + 1 > cap) break;
            memcpy(out + o, rep, rl);
            o += rl;
        } else {
            if (o + 2 > cap) break;
            out[o++] = *in;
        }
    }
    out[o] = '\0';
    return o;
}

/* 国标 20 位设备编码的前 6 位是行政区划码（CivilCode） */
static void civil_code_of(const char *id, char *out, size_t cap)
{
    size_t i;

    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!id) return;
    for (i = 0; i < 6 && id[i]; i++) {
        if (i + 1 >= cap) break;
        out[i] = id[i];
    }
    out[i] = '\0';
}

int gb_xml_keepalive(char *buf, size_t cap, long sn)
{
    gb_config_t *cfg = gb_config();
    int n;

    if (!buf || cap == 0) return GB_ERR;

    n = snprintf(buf, cap,
                 GB_MANSCDP_XML_DECL
                 "<Notify>\n"
                 "<CmdType>Keepalive</CmdType>\n"
                 "<SN>%ld</SN>\n"
                 "<DeviceID>%s</DeviceID>\n"
                 "<Status>OK</Status>\n"
                 "</Notify>\n",
                 sn, cfg->device_id);
    if (xml_check(n, cap) != GB_OK) {
        LOGE("MANSCDP", "Keepalive XML 缓冲不足（需要 %d 字节，cap=%zu）", n, cap);
        return GB_ERR;
    }
    return GB_OK;
}

int gb_xml_catalog_response(char *buf, size_t cap, long sn)
{
    gb_config_t *cfg = gb_config();
    const char *channel;
    char name[GB_CFG_STR * 6 + 1];
    char civil[16];
    int n;

    if (!buf || cap == 0) return GB_ERR;

    /* 单通道设备：通道编码缺省等于设备编码 */
    channel = cfg->channel_id[0] ? cfg->channel_id : cfg->device_id;
    xml_escape(name, sizeof(name), cfg->device_name[0] ? cfg->device_name : "IPC");
    civil_code_of(channel, civil, sizeof(civil));

    n = snprintf(buf, cap,
                 GB_MANSCDP_XML_DECL
                 "<Response>\n"
                 "<CmdType>Catalog</CmdType>\n"
                 "<SN>%ld</SN>\n"
                 "<DeviceID>%s</DeviceID>\n"
                 "<SumNum>1</SumNum>\n"
                 "<DeviceList>\n"
                 "<Item>\n"
                 "<DeviceID>%s</DeviceID>\n"
                 "<Name>%s</Name>\n"
                 "<Manufacturer>%s</Manufacturer>\n"
                 "<Model>%s</Model>\n"
                 "<Owner>%s</Owner>\n"
                 "<CivilCode>%s</CivilCode>\n"
                 "<Address>%s</Address>\n"
                 "<Parental>0</Parental>\n"
                 "<SafetyWay>0</SafetyWay>\n"
                 "<RegisterWay>1</RegisterWay>\n"
                 "<Secrecy>0</Secrecy>\n"
                 "<Status>ON</Status>\n"
                 "<PTZType>0</PTZType>\n"
                 "</Item>\n"
                 "</DeviceList>\n"
                 "</Response>\n",
                 sn, cfg->device_id, channel, name,
                 DEV_MANUFACTURER, DEV_MODEL, DEV_OWNER, civil, DEV_ADDRESS);
    if (xml_check(n, cap) != GB_OK) {
        LOGE("MANSCDP", "Catalog XML 缓冲不足（需要 %d 字节，cap=%zu）", n, cap);
        return GB_ERR;
    }
    return GB_OK;
}

int gb_xml_device_info(char *buf, size_t cap, long sn)
{
    gb_config_t *cfg = gb_config();
    char name[GB_CFG_STR * 6 + 1];
    int n;

    if (!buf || cap == 0) return GB_ERR;

    xml_escape(name, sizeof(name), cfg->device_name[0] ? cfg->device_name : "IPC");

    n = snprintf(buf, cap,
                 GB_MANSCDP_XML_DECL
                 "<Response>\n"
                 "<CmdType>DeviceInfo</CmdType>\n"
                 "<SN>%ld</SN>\n"
                 "<DeviceID>%s</DeviceID>\n"
                 "<Result>OK</Result>\n"
                 "<DeviceName>%s</DeviceName>\n"
                 "<Manufacturer>%s</Manufacturer>\n"
                 "<Model>%s</Model>\n"
                 "<Firmware>%s</Firmware>\n"
                 "<Channel>1</Channel>\n"
                 "</Response>\n",
                 sn, cfg->device_id, name,
                 DEV_MANUFACTURER, DEV_MODEL, DEV_FIRMWARE);
    if (xml_check(n, cap) != GB_OK) {
        LOGE("MANSCDP", "DeviceInfo XML 缓冲不足（需要 %d 字节，cap=%zu）", n, cap);
        return GB_ERR;
    }
    return GB_OK;
}

/* 取 <tag> 与 </tag> 之间的文本，去掉首尾空白 */
static void xml_text_of(const char *body, const char *tag, char *out, size_t cap)
{
    char open[32], close[32];
    const char *p, *q;

    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!body || !tag || !*tag) return;

    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);

    p = strstr(body, open);
    if (!p) return;
    p += strlen(open);
    q = strstr(p, close);
    if (!q) return;

    while (p < q && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    while (q > p && (q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\r' || q[-1] == '\n')) q--;

    {
        size_t len = (size_t)(q - p);
        if (len >= cap) len = cap - 1;
        memcpy(out, p, len);
        out[len] = '\0';
    }
}

void gb_xml_parse_query(const char *body, long *sn, char *cmd, size_t cmd_cap)
{
    char sns[32];

    if (sn) *sn = 0;
    if (cmd && cmd_cap) cmd[0] = '\0';
    if (!body) return;

    xml_text_of(body, "SN", sns, sizeof(sns));
    if (sns[0] && sn) *sn = atol(sns);

    if (cmd && cmd_cap) xml_text_of(body, "CmdType", cmd, cmd_cap);
}
