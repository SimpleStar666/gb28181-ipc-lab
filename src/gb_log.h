/*
 * gb_log.h — 日志与十六进制 dump
 * ============================================================
 * 为什么单独做一个日志模块？
 *   设备端排障 80% 的时间花在「看日志判断卡在哪一层」。
 *   一个带【模块标签】的日志能让你一眼看出问题在 SIP 层、PS 层还是 RTP 层。
 *
 * 关键设计：stdout 必须设为「行缓冲」。
 *   C 标准库在 stdout 不是 tty（重定向到文件、或跑在 qemu 下）时默认用全缓冲，
 *   日志会攒到 4KB 才落盘 —— 你会以为程序卡死了，其实只是没刷出来。
 *   main() 里必须调 setvbuf(stdout, NULL, _IOLBF, 0)。
 */
#ifndef GB_LOG_H
#define GB_LOG_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    GB_LOG_ERROR = 0,
    GB_LOG_WARN  = 1,
    GB_LOG_INFO  = 2,
    GB_LOG_DEBUG = 3
} gb_log_level_t;

void gb_log_set_level(gb_log_level_t lv);
gb_log_level_t gb_log_get_level(void);

/* 从环境变量 GB_LOG 读级别（debug/info/warn/error），缺省 info。 */
void gb_log_init_from_env(void);

void gb_log(gb_log_level_t lv, const char *tag, const char *fmt, ...);

#define LOGE(tag, ...) gb_log(GB_LOG_ERROR, tag, __VA_ARGS__)
#define LOGW(tag, ...) gb_log(GB_LOG_WARN,  tag, __VA_ARGS__)
#define LOGI(tag, ...) gb_log(GB_LOG_INFO,  tag, __VA_ARGS__)
#define LOGD(tag, ...) gb_log(GB_LOG_DEBUG, tag, __VA_ARGS__)

/* 十六进制 dump：每行 16 字节，形如
 *   [PS] 0000  00 00 01 ba 44 00 04 00 04 01 86 66 cf f8 00 00  |....D......f....|
 * 排障时用它对着协议文档逐字节核对，比读代码可靠得多。 */
void gb_hexdump(const char *tag, const void *data, size_t len);

/* dump 前 n 字节，结束打省略号（日志里只关心头部时用） */
void gb_hexdump_head(const char *tag, const void *data, size_t len, size_t n);

#endif /* GB_LOG_H */
