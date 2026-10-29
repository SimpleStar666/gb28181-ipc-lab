/*
 * gb_media.h — 媒体源与推流
 * ============================================================
 * 本模块把「码流从哪来」和「码流怎么送出去」隔离开。
 *
 * 【两种媒体源】
 *   文件模式：预生成的 testsrc.h264 整文件读进内存，循环推送。
 *             用途是「不依赖任何外部进程」地验证 PS/RTP 封装对不对。
 *   实时模式：从命名管道（FIFO）流式读 H.264，由宿主侧 ffmpeg 写进来。
 *             用途是验证「边采边推」——滚动缓冲、按 NAL 切帧、
 *             跨 read 边界不丢 NAL、锚定 SPS 保证首帧是 IDR。
 *
 * 【为什么实时模式要外部 ffmpeg 编码？】
 *   这个分工恰好等于真实 IPC 的分工：
 *     真实设备：Sensor → MIPI → ISP → VENC(硬件编码器) → 应用层封装推流
 *     本项目：  ffmpeg(v4l2 → libx264)  ────────→ FIFO ────────→ 应用层封装推流
 *   应用层（也就是本项目要教的这部分）在真机上同样只负责「拿到编码码流、
 *   打 PS、发 RTP」，硬件编码器对它是黑盒。所以这个抽象不是为了省事，
 *   而是**本来就该这么分层**。面试时这句话比任何结论都值钱。
 *
 * 【音频从哪来】
 *   本项目用正弦波合成 G.711A 帧（模拟麦克风采集），
 *   因为 WSL 里没有真实声卡。真机上换掉 gb_audio_gen_next 的实现即可。
 */
#ifndef GB_MEDIA_H
#define GB_MEDIA_H

#include <stddef.h>
#include <stdint.h>
#include "gb_common.h"

typedef struct gb_frame_src gb_frame_src_t;

/* 打开文件循环源。失败返回 NULL。 */
gb_frame_src_t *gb_src_open_file(const char *path);
/* 打开 FIFO 实时源。FIFO 打开会阻塞直到写端出现（这是 POSIX FIFO 的语义）。 */
gb_frame_src_t *gb_src_open_fifo(const char *path);
void            gb_src_close(gb_frame_src_t *s);

/* 取下一帧。
 *   返回 >0 : 帧字节数，*out 指向帧首（AnnexB，可含 SPS/PPS/SEI + 一个 VCL NAL）
 *   返回  0 : 暂时没有完整帧，调用方稍后重试
 *   返回 -1 : 源结束（文件模式一般不返回 -1，因为会循环）
 * 返回的指针在下一次调用前有效。 */
int gb_src_next_frame(gb_frame_src_t *s, const uint8_t **out, size_t *out_len);

/* 该帧是否关键帧（含 IDR） */
int gb_src_frame_is_idr(const uint8_t *frame, size_t len);

/* ---------------- 音频生成（模拟麦克风） ---------------- */
typedef struct {
    uint64_t pts90k;
    uint32_t phase;      /* 相位累加器，用于生成正弦波 */
    int      freq_hz;
} gb_audio_gen_t;

void gb_audio_gen_init(gb_audio_gen_t *g, int freq_hz, uint64_t start_pts90k);

/* 生成下一帧 A-law 数据（GB_AUDIO_FRAME_LEN 字节）。
 * 返回字节数，并把该帧 PTS 写入 *pts90k。 */
int gb_audio_gen_next(gb_audio_gen_t *g, uint8_t *out, size_t cap, uint64_t *pts90k);

/* ---------------- 推流 ---------------- */
typedef struct gb_push_ctx gb_push_ctx_t;

/* 创建推流上下文。
 *   sip_fd    : 用于发 RTP 的 UDP socket（复用 SIP socket 也可以，见文档说明）
 *   dst_ip/dst_port : RTP 目标，来自 INVITE 的 SDP c= / m= 行
 *   ssrc      : 来自 SDP 的 y= 行
 *   audio_on  : 是否在 PS 里复用音频 */
gb_push_ctx_t *gb_push_create(int sip_fd, const char *dst_ip, uint16_t dst_port,
                              uint32_t ssrc, int audio_on);
void gb_push_destroy(gb_push_ctx_t *ctx);

/* 请求停止（由 BYE 触发） */
void gb_push_stop(gb_push_ctx_t *ctx);
int  gb_push_running(const gb_push_ctx_t *ctx);

/* 线程入口：pthread_create(&th, NULL, gb_push_loop, ctx) */
void *gb_push_loop(void *arg);

#endif /* GB_MEDIA_H */
