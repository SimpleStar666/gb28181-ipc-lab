/*
 * gb_manscdp.h — MANSCDP XML（GB28181 的设备控制协议）
 * ============================================================
 * MANSCDP = MAN SC DP（公安行业标准里的设备控制描述协议），
 * 运行在 SIP MESSAGE 方法的 body 里，Content-Type: Application/MANSCDP+xml。
 *
 * 设备端要发的三种：
 *   Keepalive    心跳，平台据此判断设备在线
 *   Catalog      目录应答，告诉平台"我有几个通道"
 *   DeviceInfo   设备信息应答
 *
 * 【编码坑】XML 声明里写 encoding="GB2312" 但实际发的是 UTF-8 字节，
 *   是国标实现里最常见的乱码来源。
 *   原版代码就踩了这个坑：DEVICE_NAME 是 UTF-8 存的「TS模拟摄像头」，
 *   声明却写 GB2312。平台解析时按 GB2312 解 UTF-8，中文必然乱码。
 *   本实现统一声明 UTF-8（GB/T 28181-2016 允许），源码与字节一致。
 *
 * 【SN 语义】SN 是会话序号。设备应答时必须**回填请求里的 SN**，
 *   否则平台匹配不上应答，表现为"点了查询但界面一直转圈"。
 */
#ifndef GB_MANSCDP_H
#define GB_MANSCDP_H

#include <stddef.h>

/* XML 声明统一用 UTF-8 —— 与源码字节一致，避免声明与实际编码不符导致乱码 */
#define GB_MANSCDP_XML_DECL "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"

int gb_xml_keepalive(char *buf, size_t cap, long sn);
int gb_xml_catalog_response(char *buf, size_t cap, long sn);
int gb_xml_device_info(char *buf, size_t cap, long sn);

/* 从 MESSAGE 的 body 里解析 <SN> 和 <CmdType>。
 * sn 取不到时置 0；cmd 取不到时置空串。 */
void gb_xml_parse_query(const char *body, long *sn, char *cmd, size_t cmd_cap);

#endif /* GB_MANSCDP_H */
