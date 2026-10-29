/*
 * gb_g711.h — G.711 A-law 编解码
 * ============================================================
 * 为什么是 A-law 而不是 μ-law？
 *   GB/T 28181-2016 规定的语音编码是 G.711A（A-law），
 *   中国和欧洲用 A-law，美国和日本用 μ-law（PCMU）。
 *   SDP 里：PCMA = 静态载荷类型 8，PCMU = 静态载荷类型 0。
 *
 * 8kHz 单声道，每样本 1 字节，码率固定 64 kbps。
 * 20ms 一帧 = 160 样本 = 160 字节。
 *
 * 这两个函数是纯函数，单元测试可以直接拿标准表核对（见 tests/test_g711.c）。
 */
#ifndef GB_G711_H
#define GB_G711_H

#include <stddef.h>
#include <stdint.h>
/* 引入 GB_AUDIO_FRAME_LEN / GB_AUDIO_RATE 等音频帧常量。
 * 与 gb_ps.h / gb_rtp.h / gb_sip.h / gb_media.h 保持一致：这几个头都
 * 直接 re-export gb_common.h，调用方只需要 include 一个头就能拿到常量。 */
#include "gb_common.h"

/* 16bit 线性 PCM → 8bit A-law */
uint8_t gb_g711a_encode(int16_t pcm);

/* 8bit A-law → 16bit 线性 PCM */
int16_t gb_g711a_decode(uint8_t a);

/* 把一段线性 PCM 编成 A-law。返回输出字节数。 */
size_t gb_g711a_encode_buf(const int16_t *pcm, size_t samples, uint8_t *out, size_t out_cap);

#endif /* GB_G711_H */
