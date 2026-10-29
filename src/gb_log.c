/*
 * gb_log.c — 日志与十六进制 dump 的实现
 * ============================================================
 * 三个设计点，都值得在面试里讲：
 *   1. 级别过滤放在 gb_log() 入口，宏只是转发 —— 关掉的日志连格式化都不做。
 *   2. 每条日志用 flockfile/funlockfile 包住：多线程（SIP 主循环 + 心跳 +
 *      推流线程）同时打日志时不会互相插进半行，这是 stdio 层面的最小代价方案。
 *   3. 时间戳用 clock_gettime(CLOCK_REALTIME) + localtime_r（可重入），
 *      不用 localtime（返回静态缓冲区，多线程下会互相踩）。
 *
 * 关于缓冲：main() 里已经 setvbuf(stdout, NULL, _IOLBF, 0)。
 *   stdout 重定向到文件时 C 库默认全缓冲（4KB 才落盘），qemu 下看不到实时日志，
 *   很容易误判成「程序卡死了」。行缓冲就是为这件事准备的。
 *   只有 ERROR 级别在 gb_log 里额外 fflush —— 崩溃/退出前最后一条必须落盘，
 *   这一条不能赌缓冲。
 */
#include "gb_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static gb_log_level_t g_level = GB_LOG_INFO;

static const char *level_label(gb_log_level_t lv)
{
    switch (lv) {
    case GB_LOG_ERROR: return "E";
    case GB_LOG_WARN:  return "W";
    case GB_LOG_INFO:  return "I";
    default:           return "D";
    }
}

void gb_log_set_level(gb_log_level_t lv)
{
    if (lv < GB_LOG_ERROR) lv = GB_LOG_ERROR;
    if (lv > GB_LOG_DEBUG) lv = GB_LOG_DEBUG;
    g_level = lv;
}

gb_log_level_t gb_log_get_level(void)
{
    return g_level;
}

void gb_log_init_from_env(void)
{
    const char *e = getenv("GB_LOG");

    if (!e || !*e) return;                 /* 不设置就保留当前级别（缺省 info） */
    if (!strcmp(e, "debug"))                    g_level = GB_LOG_DEBUG;
    else if (!strcmp(e, "info"))                g_level = GB_LOG_INFO;
    else if (!strcmp(e, "warn") || !strcmp(e, "warning")) g_level = GB_LOG_WARN;
    else if (!strcmp(e, "error"))               g_level = GB_LOG_ERROR;
    else if (e[0] >= '0' && e[0] <= '3')        g_level = (gb_log_level_t)(e[0] - '0');
    /* 其它取值保持原级别，不报错 —— 日志配置不值得让程序启动失败 */
}

void gb_log(gb_log_level_t lv, const char *tag, const char *fmt, ...)
{
    va_list ap;
    struct timespec ts;
    struct tm tmv;

    if (lv > g_level) return;              /* 热点路径：先过滤，再谈格式化 */
    if (!fmt) return;
    if (!tag) tag = "-";

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tmv);

    flockfile(stdout);
    printf("[%02d:%02d:%02d.%03ld] [%s] [%s] ",
           tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
           (long)(ts.tv_nsec / 1000000L), level_label(lv), tag);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    if (lv == GB_LOG_ERROR) fflush(stdout);   /* 错误必须立刻可见 */
    funlockfile(stdout);
}

/* 打一行： [TAG] 0000  00 00 01 ba ...  |....D......f....| */
static void dump_line(const char *tag, const uint8_t *d, size_t off, size_t n)
{
    char hex[16 * 3 + 1];
    char asc[17];
    size_t i;

    for (i = 0; i < 16; i++) {
        if (i < n) {
            hex[i * 3 + 0] = "0123456789abcdef"[d[off + i] >> 4];
            hex[i * 3 + 1] = "0123456789abcdef"[d[off + i] & 0x0F];
            hex[i * 3 + 2] = ' ';
            asc[i] = (d[off + i] >= 0x20 && d[off + i] <= 0x7E) ? (char)d[off + i] : '.';
        } else {
            hex[i * 3 + 0] = ' ';
            hex[i * 3 + 1] = ' ';
            hex[i * 3 + 2] = ' ';
            asc[i] = ' ';
        }
    }
    hex[16 * 3] = '\0';
    asc[16] = '\0';

    printf("[%s] %04zX  %s |%s|\n", tag ? tag : "-", off, hex, asc);
}

void gb_hexdump(const char *tag, const void *data, size_t len)
{
    const uint8_t *d = (const uint8_t *)data;
    size_t off;

    flockfile(stdout);
    if (!d || len == 0) {
        printf("[%s] (len=%zu 无数据)\n", tag ? tag : "-", len);
        funlockfile(stdout);
        return;
    }
    for (off = 0; off < len; off += 16) {
        size_t n = (len - off > 16) ? 16 : (len - off);
        dump_line(tag, d, off, n);
    }
    funlockfile(stdout);
}

void gb_hexdump_head(const char *tag, const void *data, size_t len, size_t n)
{
    const uint8_t *d = (const uint8_t *)data;
    size_t show, off;

    if (!d || len == 0 || n == 0) return;
    show = (n < len) ? n : len;

    flockfile(stdout);
    for (off = 0; off < show; off += 16) {
        size_t chunk = (show - off > 16) ? 16 : (show - off);
        dump_line(tag, d, off, chunk);
    }
    if (show < len) printf("[%s] ... 共 %zu 字节（只显示前 %zu）\n", tag ? tag : "-", len, show);
    funlockfile(stdout);
}
