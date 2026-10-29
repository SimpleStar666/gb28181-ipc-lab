/*
 * gb_common.h — 公共定义
 * ============================================================
 * 本项目是一套「可运行、可验证」的 GB28181 设备端（IPC 侧）教学实现。
 *
 * 阅读顺序建议：
 *   gb_common.h → gb_log.h → gb_config.h → gb_md5.h → gb_g711.h
 *   → gb_annexb.h → gb_ps.h → gb_rtp.h → gb_sip.h → gb_manscdp.h → gb_media.h
 *
 * 设计原则（也是面试时最该讲的三条）：
 *   1. 字节层与 IO 层分离。gb_ps / gb_rtp / gb_md5 / gb_g711 / gb_annexb
 *      全部是纯函数，不碰 socket、不碰文件 —— 所以能被单元测试逐字节断言。
 *   2. 协议层（gb_sip / gb_manscdp）只管消息构造与解析，不管业务。
 *   3. 媒体层（gb_media）只管取帧与送包，不关心 PS/RTP 的字节长什么样。
 */
#ifndef GB_COMMON_H
#define GB_COMMON_H

#include <stddef.h>
#include <stdint.h>

#define GB_OK    0
#define GB_ERR  (-1)

/* 90kHz 系统时钟：MPEG-PS 与 RTP 都用它给视频打时间戳。
 * 一帧 15fps 的步进 = 90000 / 15 = 6000 ticks。 */
#define GB_TIMESCALE_90000  90000u
#define GB_VIDEO_FPS_DEFAULT 15
#define GB_VIDEO_PTS_STEP   (GB_TIMESCALE_90000 / GB_VIDEO_FPS_DEFAULT)  /* 6000 */

/* 音频：GB/T 28181 规定 G.711A（A-law），8kHz 单声道。
 * 每帧 20ms = 160 样本，PTS 步进 = 90000 * 20 / 1000 = 1800 ticks。 */
#define GB_AUDIO_RATE       8000
#define GB_AUDIO_FRAME_MS   20
#define GB_AUDIO_FRAME_LEN  (GB_AUDIO_RATE / 1000 * GB_AUDIO_FRAME_MS)  /* 160 */
#define GB_AUDIO_PTS_STEP   (GB_TIMESCALE_90000 / 1000 * GB_AUDIO_FRAME_MS) /* 1800 */

/* 33bit PTS 的掩码（MPEG-PS 的 PTS 只有 33 位有效） */
#define GB_PTS_MASK         0x1FFFFFFFFULL

#endif /* GB_COMMON_H */
