/*
 * gb_media.c — 媒体源（文件/FIFO）+ 音频合成 + 推流线程
 * ============================================================
 * 本模块只回答两个问题：码流从哪来、怎么送出去。
 * PS 的字节长什么样（gb_ps）、RTP 头怎么拼（gb_rtp）都不在这里，各管一层。
 *
 * 【流式切帧的两个坑（原型踩过，这里都修了）】
 *   1. 滚动缓冲：从 FIFO 读到的数据是「一段一段」的，一段的结尾很可能把某个 NAL
 *      切成两半。所以消费完一个 NAL 后必须 memmove 把未消费的尾部搬到缓冲头部，
 *      不能 memset 一把清掉。
 *   2. 归零条件：只有真的消费了字节（pos > 0）才滚动；滚动后 used 必须真的减少。
 *      若条件写成 pos > 0 && pos < used，那么 pos == used（正好消费完）时缓冲
 *      永不归零 —— 旧数据会被当成新数据反复推出去，
 *      现象是「画面在同一个位置反复抖 / 平台侧时间戳倒退」。
 *
 * 【为什么要锚定 SPS】
 *   从 FIFO 中途接入时，第一段数据很可能是半个 NAL。丢弃 SPS 之前的所有内容、
 *   从 SPS 开始累积，才能保证首帧是「SPS+PPS+SEI+IDR」这个完整关键帧 ——
 *   否则解码器拿不到参数集，表现为平台侧一直黑屏。
 *
 * 【音频为什么复用在同一条流里】
 *   GB/T 28181 的做法是：音频不单独开一条 RTP 流，G.711A 的 PES 直接和视频 PES
 *   复用进同一条 PS/RTP 流（SDP 里 m=audio 通常写 0 或被禁）。
 *   好处是音视频天然共用 90kHz 时钟，接收端不需要再做时钟映射。
 */
#include "gb_media.h"
#include "gb_annexb.h"
#include "gb_config.h"
#include "gb_g711.h"
#include "gb_log.h"
#include "gb_ps.h"
#include "gb_rtp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { GB_SRC_FILE = 0, GB_SRC_FIFO = 1 };

/* 缓冲预算（内存/鲁棒性常数，不是部署参数）：
 *   FIFO 滚动缓冲 2MB —— 够吸收几次读之间的突发，又远小于嵌入式设备余量；
 *   单帧上限与 GB_PS_MAX_PACKET 对齐（1080p 的 I 帧可能上百 KB）；
 *   单文件上限 64MB —— 再大基本可以断定路径指错了。 */
#define GB_SRC_FIFO_CAP    (2u << 20)
#define GB_SRC_READ_CHUNK  (64u << 10)
#define GB_SRC_FRAME_MAX   ((size_t)GB_PS_MAX_PACKET)
#define GB_SRC_FILE_MAX    (64u << 20)
/* 一次最多补多少音频帧（fps/时钟配错时不至于一次生成几千帧） */
#define GB_AUDIO_MAX_BURST 16
/* 联系多少次取不到帧就重开媒体源（FIFO 写端重启的场景） */
#define GB_SRC_IDLE_RETRY  100
/* 模拟麦克风的测试音频率 */
#define GB_AUDIO_TONE_HZ   1000
/* 推流线程一次睡眠的分片：保证 BYE 后最多 20ms 就能退出 */
#define GB_PUSH_SLEEP_SLICE_MS 20

struct gb_frame_src {
    int      kind;
    int      fd;                 /* FIFO 读端；文件模式读完即关 */
    uint8_t *buf;                /* FILE：整文件内容；FIFO：滚动缓冲 */
    size_t   cap;
    size_t   used;               /* buf 里有效字节数 */
    size_t   pos;                /* 已消费到的偏移（未消费尾部 = buf[pos..used)） */
    int      eof;                /* FIFO 写端已关闭 */
    int      anchored;           /* FIFO：是否已经锚定到 SPS */
    uint8_t *frame;              /* 组帧缓冲（累积 SPS/PPS/SEI + VCL） */
    size_t   frame_len;
};

/* ---------------- 媒体源 ---------------- */

static gb_frame_src_t *src_alloc(int kind, size_t cap)
{
    gb_frame_src_t *s = (gb_frame_src_t *)calloc(1, sizeof(*s));

    if (!s) {
        LOGE("MEDIA", "分配媒体源失败（内存不足）");
        return NULL;
    }
    s->kind = kind;
    s->fd = -1;
    s->cap = cap;
    s->buf = (uint8_t *)malloc(cap ? cap : 1);
    s->frame = (uint8_t *)malloc(GB_SRC_FRAME_MAX);
    if (!s->buf || !s->frame) {
        LOGE("MEDIA", "分配媒体缓冲失败（需要 %zu + %zu 字节）", cap, GB_SRC_FRAME_MAX);
        free(s->buf);
        free(s->frame);
        free(s);
        return NULL;
    }
    return s;
}

gb_frame_src_t *gb_src_open_file(const char *path)
{
    gb_frame_src_t *s;
    struct stat st;
    size_t done = 0;
    int fd;

    if (!path || !*path) {
        LOGE("MEDIA", "文件循环模式需要配置 h264_file（当前为空）");
        return NULL;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        LOGE("MEDIA", "打开 H.264 裸流失败：%s（%s）", path, strerror(errno));
        return NULL;
    }
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        LOGE("MEDIA", "读取文件信息失败或文件为空：%s", path);
        close(fd);
        return NULL;
    }
    if ((unsigned long long)st.st_size > (unsigned long long)GB_SRC_FILE_MAX) {
        LOGE("MEDIA", "文件太大（%lld 字节，上限 %u）：确认 h264_file 指向的是裸流而不是别的文件",
             (long long)st.st_size, (unsigned)GB_SRC_FILE_MAX);
        close(fd);
        return NULL;
    }

    s = src_alloc(GB_SRC_FILE, (size_t)st.st_size);
    if (!s) {
        close(fd);
        return NULL;
    }
    while (done < (size_t)st.st_size) {
        ssize_t n = read(fd, s->buf + done, (size_t)st.st_size - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOGE("MEDIA", "读取 %s 失败：%s", path, strerror(errno));
            close(fd);
            gb_src_close(s);
            return NULL;
        }
        if (n == 0) break;
        done += (size_t)n;
    }
    close(fd);
    s->used = done;
    LOGI("MEDIA", "文件循环模式：%s（%zu 字节，整文件读入内存循环推送）", path, s->used);
    return s;
}

gb_frame_src_t *gb_src_open_fifo(const char *path)
{
    gb_frame_src_t *s;
    int fd, fl;

    if (!path || !*path) {
        LOGE("MEDIA", "实时流模式需要 FIFO 路径（-live <path>）");
        return NULL;
    }

    LOGI("MEDIA", "等待 FIFO 写端：%s（open 会阻塞，直到编码器/ffmpeg 打开写端）", path);
    fd = open(path, O_RDONLY);            /* 阻塞：这就是 POSIX FIFO 语义 */
    if (fd < 0) {
        LOGE("MEDIA", "打开 FIFO 失败：%s（%s）", path, strerror(errno));
        return NULL;
    }

    /* 连上写端后立刻改成非阻塞读：否则 read() 会一直挂着，
     * 推流线程就没法及时看到 stop 标志（BYE 唤不停）。 */
    fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
        LOGW("MEDIA", "设置非阻塞读失败：%s（BYE 停止可能不及时）", strerror(errno));
    }

    s = src_alloc(GB_SRC_FIFO, GB_SRC_FIFO_CAP);
    if (!s) {
        close(fd);
        return NULL;
    }
    s->fd = fd;
    LOGI("MEDIA", "FIFO 已连接：%s（滚动缓冲 %u 字节，按 NAL 切帧）", path, (unsigned)GB_SRC_FIFO_CAP);
    return s;
}

void gb_src_close(gb_frame_src_t *s)
{
    if (!s) return;
    if (s->fd >= 0) close(s->fd);
    free(s->buf);
    free(s->frame);
    free(s);
}

/* 从 FIFO 再读一段。返回读到的字节数；0 = 暂无数据（或写端已关）。 */
static size_t src_fill(gb_frame_src_t *s)
{
    ssize_t n;

    if (s->kind != GB_SRC_FIFO || s->fd < 0) return 0;
    if (s->used >= s->cap) return 0;                   /* 缓冲满：先消费 */
    if (s->eof) return 0;

    n = read(s->fd, s->buf + s->used, s->cap - s->used);
    if (n > 0) {
        s->used += (size_t)n;
        return (size_t)n;
    }
    if (n == 0) {                                      /* 写端关闭 */
        s->eof = 1;
        return 0;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    LOGE("MEDIA", "读 FIFO 失败：%s", strerror(errno));
    s->eof = 1;
    return 0;
}

/* 滚动缓冲：丢掉已消费的前缀，保留未消费的尾部（可能是半个 NAL）。
 * 只用于 FIFO 模式 —— 文件模式的 buf 是「整份文件」，pos 只是读游标，
 * 到尾部回绕即可；对它 memmove 会把文件内容整体搬走，文件就不再是文件了。 */
static void src_roll(gb_frame_src_t *s)
{
    if (s->kind != GB_SRC_FIFO) return;
    if (s->pos > 0) {
        memmove(s->buf, s->buf + s->pos, s->used - s->pos);
        s->used -= s->pos;
        s->pos = 0;
        /* pos == used 时上面两行的结果正是 used = 0、pos = 0：
         * 缓冲真的被清空了，旧数据不会复发。 */
    }
}

static int src_append(gb_frame_src_t *s, const uint8_t *p, size_t n)
{
    if (s->frame_len + n > GB_SRC_FRAME_MAX) {
        LOGE("MEDIA", "单帧超过 %u 字节（已攒 %zu + 本次 %zu），丢弃该帧：检查分辨率/码率",
             (unsigned)GB_SRC_FRAME_MAX, s->frame_len, n);
        s->frame_len = 0;
        return GB_ERR;
    }
    memcpy(s->frame + s->frame_len, p, n);
    s->frame_len += n;
    return GB_OK;
}

int gb_src_next_frame(gb_frame_src_t *s, const uint8_t **out, size_t *out_len)
{
    if (!s || !out || !out_len) return GB_ERR;
    *out = NULL;
    *out_len = 0;

    /* FIFO：每次进来都尽量再读一段（缓冲满或暂无数据时 read 返回 0，不会阻塞 ——
     * 打开时已经设了 O_NONBLOCK，否则线程会卡在 read 里响应不了 BYE）。 */
    if (s->kind == GB_SRC_FIFO) (void)src_fill(s);

    for (;;) {
        size_t sc1 = 3, sc2 = 3;
        size_t nal_off, nal_end, nal_len;
        int f1, f2, nt;

        if (s->kind == GB_SRC_FILE && s->pos >= s->used) {
            s->pos = 0;                    /* 文件尾 → 回绕，循环推送 */
            s->frame_len = 0;              /* 文件末尾的残渣不跨圈带入 */
        }

        f1 = gb_annexb_find_startcode(s->buf, s->used, s->pos, &sc1);
        if (f1 < 0) {
            if (s->kind == GB_SRC_FILE) {
                if (s->used == 0) return 0;
                s->pos = 0;                /* 尾部残留不足一个起始码，丢掉 */
                s->frame_len = 0;
                continue;
            }
            return 0;                      /* FIFO：等更多数据 */
        }
        nal_off = (size_t)f1;

        if (s->pos < nal_off) {
            /* 起始码之前是上一个 NAL 的尾巴或脏数据，跳过 */
            s->pos = nal_off;
        }

        f2 = gb_annexb_find_startcode(s->buf, s->used, nal_off + sc1, &sc2);
        if (f2 < 0) {
            if (s->kind == GB_SRC_FILE) {
                nal_end = s->used;         /* 文件尾：最后一个 NAL 延伸到 EOF */
            } else {
                return 0;                  /* FIFO：这个 NAL 还不完整，等下一段 */
            }
        } else {
            nal_end = (size_t)f2;
        }

        if (nal_end <= nal_off) {
            s->pos = nal_off + sc1;
            continue;
        }
        nal_len = nal_end - nal_off;
        nt = gb_annexb_nal_type(s->buf + nal_off, sc1);

        if (s->kind == GB_SRC_FIFO && !s->anchored) {
            /* 从流中间接入：丢到第一个 SPS 才开始，保证首帧是 IDR */
            if (nt != 7) {
                s->pos = nal_end;
                s->frame_len = 0;
                continue;
            }
            s->anchored = 1;
            LOGD("MEDIA", "已锚定到 SPS（偏移 %zu），开始组帧", nal_off);
        }

        if (!gb_annexb_is_vcl(nt)) {
            /* SPS/PPS/SEI/AUD 先攒着，等 VCL 到了拼成一整帧 */
            if (src_append(s, s->buf + nal_off, nal_len) != GB_OK) s->frame_len = 0;
            s->pos = nal_end;
            continue;
        }

        /* VCL = 一帧结束 */
        if (src_append(s, s->buf + nal_off, nal_len) != GB_OK) {
            s->pos = nal_end;
            src_roll(s);
            continue;
        }
        s->pos = nal_end;
        {
            size_t n = s->frame_len;
            s->frame_len = 0;
            *out = s->frame;
            *out_len = n;
            src_roll(s);                   /* 见文件头：只有消费了才滚，且 used 真的减 */
            return (int)n;
        }
    }
}

int gb_src_frame_is_idr(const uint8_t *frame, size_t len)
{
    size_t pos = 0;

    if (!frame) return 0;
    while (pos + 3 <= len) {
        size_t sc = 3;
        int at = gb_annexb_find_startcode(frame, len, pos, &sc);

        if (at < 0) return 0;
        if (gb_annexb_nal_type(frame + at, sc) == 5) return 1;
        pos = (size_t)at + sc;
    }
    return 0;
}

/* ---------------- 音频合成（模拟麦克风） ----------------
 * 65 点四分之一周期正弦表（0°..90°，步进 π/128）。
 * 用查表而不是 sin()：嵌入式里 libm 是要额外占 flash 的，
 * 而且真机音频多半由硬件/定点算法产生，查表才是常态。
 * 这里合成的是 1kHz 测试音，精度绰绰有余。 */
static const int16_t SINE_Q[65] = {
        0,   804,  1608,  2410,  3212,  4011,  4808,  5602,
     6393,  7179,  7962,  8739,  9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
    32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767
};

void gb_audio_gen_init(gb_audio_gen_t *g, int freq_hz, uint64_t start_pts90k)
{
    if (!g) return;
    g->pts90k = start_pts90k;
    g->phase = 0;
    g->freq_hz = freq_hz;
}

int gb_audio_gen_next(gb_audio_gen_t *g, uint8_t *out, size_t cap, uint64_t *pts90k)
{
    int16_t pcm[GB_AUDIO_FRAME_LEN];
    uint32_t step;
    int i;

    if (!g || !out || cap < GB_AUDIO_FRAME_LEN) return GB_ERR;

    if (pts90k) *pts90k = g->pts90k;

    /* phase 是 Q32 的相位累加器：转一圈 = 一个周期。
     * 每样本步进 = freq/rate * 2^32 */
    {
        uint32_t f = (g->freq_hz > 0) ? (uint32_t)g->freq_hz : 0u;
        step = (uint32_t)(((uint64_t)f << 32) / (uint64_t)GB_AUDIO_RATE);
    }

    for (i = 0; i < GB_AUDIO_FRAME_LEN; i++) {
        uint32_t ph = g->phase;
        int quad = (int)(ph >> 30);                       /* 0..3 象限 */
        uint32_t idx = (ph & 0x3FFFFFFFu) >> 24;          /* 0..63 */
        int v;

        switch (quad) {
        case 0:  v =  SINE_Q[idx];           break;       /* 0..90   */
        case 1:  v =  SINE_Q[64 - idx];      break;       /* 90..180 */
        case 2:  v = -SINE_Q[idx];           break;       /* 180..270*/
        default: v = -SINE_Q[64 - idx];      break;       /* 270..360*/
        }
        pcm[i] = (int16_t)v;
        g->phase = ph + step;
    }

    (void)gb_g711a_encode_buf(pcm, GB_AUDIO_FRAME_LEN, out, cap);
    g->pts90k += GB_AUDIO_PTS_STEP;
    return (int)GB_AUDIO_FRAME_LEN;
}

/* ---------------- 推流 ---------------- */

struct gb_push_ctx {
    int      fd;                  /* RTP 发送 socket（复用 SIP 的 UDP socket） */
    struct sockaddr_in dst;
    gb_rtp_tx_t tx;
    int      audio_on;
    uint32_t ssrc;
    volatile int stop;            /* BYE 置 1；volatile 保证推流线程能看到 */
    volatile int running;
};

gb_push_ctx_t *gb_push_create(int sip_fd, const char *dst_ip, uint16_t dst_port,
                              uint32_t ssrc, int audio_on)
{
    gb_config_t *cfg = gb_config();
    gb_push_ctx_t *ctx;

    if (sip_fd < 0 || !dst_ip || !*dst_ip || dst_port == 0) {
        LOGE("MEDIA", "推流参数非法（fd=%d ip=%s port=%u）", sip_fd,
             dst_ip ? dst_ip : "(null)", (unsigned)dst_port);
        return NULL;
    }

    ctx = (gb_push_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        LOGE("MEDIA", "分配推流上下文失败");
        return NULL;
    }

    ctx->fd = sip_fd;
    ctx->dst.sin_family = AF_INET;
    ctx->dst.sin_port = htons(dst_port);
    if (inet_pton(AF_INET, dst_ip, &ctx->dst.sin_addr) != 1) {
        LOGE("MEDIA", "RTP 目标地址非法：%s", dst_ip);
        free(ctx);
        return NULL;
    }

    ctx->ssrc = ssrc ? ssrc : cfg->ssrc_default;
    ctx->audio_on = audio_on ? 1 : 0;
    ctx->stop = 0;
    ctx->running = 0;

    {
        /* 初始序列号取随机值（RFC 3550 建议）：固定值容易被猜到，
         * 而且在同一 SSRC 下重复起始序号会干扰接收端的丢包统计。 */
        struct timespec ts;
        uint16_t seq0;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        seq0 = (uint16_t)(((uint32_t)(ts.tv_nsec >> 7)) ^ (uint32_t)getpid());
        gb_rtp_tx_init(&ctx->tx, cfg->rtp_pt, cfg->mtu, ctx->ssrc, seq0);
    }

    return ctx;
}

void gb_push_destroy(gb_push_ctx_t *ctx)
{
    free(ctx);
}

void gb_push_stop(gb_push_ctx_t *ctx)
{
    if (ctx) ctx->stop = 1;
}

int gb_push_running(const gb_push_ctx_t *ctx)
{
    return ctx ? ctx->running : 0;
}

static int push_sleep(gb_push_ctx_t *ctx, long ms)
{
    while (ms > 0 && !ctx->stop) {
        struct timespec ts;
        long slice = (ms > GB_PUSH_SLEEP_SLICE_MS) ? GB_PUSH_SLEEP_SLICE_MS : ms;

        ts.tv_sec = 0;
        ts.tv_nsec = slice * 1000000L;
        nanosleep(&ts, NULL);
        ms -= slice;
    }
    return ctx->stop ? GB_ERR : GB_OK;
}

/* 帧率控制：按绝对时间对齐，避免 usleep 的误差逐帧累积 */
static void push_wait_until(gb_push_ctx_t *ctx, struct timespec *deadline)
{
    for (;;) {
        struct timespec now;
        long long diff;

        if (ctx->stop) return;
        clock_gettime(CLOCK_MONOTONIC, &now);
        diff = (long long)(deadline->tv_sec - now.tv_sec) * 1000000000LL +
               (long long)(deadline->tv_nsec - now.tv_nsec);
        if (diff <= 0) {
            if (diff < -1000000000LL) *deadline = now;   /* 落后 >1s：重新对齐，不做疯狂追帧 */
            return;
        }
        {
            struct timespec ts;
            long slice = (diff < 20000000LL) ? (long)diff : 20000000L;
            ts.tv_sec = 0;
            ts.tv_nsec = slice;
            nanosleep(&ts, NULL);
        }
    }
}

/* 按配置打开媒体源。FIFO 会一直重试（等宿主侧 ffmpeg 起写端），文件模式只试一次。 */
static gb_frame_src_t *push_open_source(gb_push_ctx_t *ctx, gb_config_t *cfg)
{
    for (;;) {
        gb_frame_src_t *s;

        if (ctx->stop) return NULL;

        if (cfg->fifo_path[0]) {
            s = gb_src_open_fifo(cfg->fifo_path);
            if (s) return s;
            LOGW("MEDIA", "FIFO 尚未就绪，200ms 后重试（等 ffmpeg 打开写端）");
            if (push_sleep(ctx, 200) != GB_OK) return NULL;
        } else {
            return gb_src_open_file(cfg->h264_file);
        }
    }
}

void *gb_push_loop(void *arg)
{
    gb_push_ctx_t *ctx = (gb_push_ctx_t *)arg;
    gb_config_t *cfg;
    gb_frame_src_t *src = NULL;
    gb_audio_gen_t agen;
    uint8_t *ps = NULL;
    uint8_t *aps = NULL;
    uint8_t abuf[GB_AUDIO_FRAME_LEN];
    uint64_t video_pts = 0;
    uint64_t pts_step;
    long frame_ns;
    int frames = 0, idle = 0;
    struct timespec next_ts;

    if (!ctx) return NULL;
    cfg = gb_config();

    ctx->running = 1;

    pts_step = (cfg->fps > 0) ? (GB_TIMESCALE_90000 / (uint64_t)cfg->fps)
                             : (uint64_t)GB_VIDEO_PTS_STEP;
    frame_ns = (cfg->fps > 0) ? (1000000000L / (long)cfg->fps) : 66666666L;

    ps = (uint8_t *)malloc((size_t)GB_PS_MAX_PACKET + 1024);
    aps = (uint8_t *)malloc((size_t)GB_PS_MAX_PACKET + 1024);
    if (!ps || !aps) {
        LOGE("MEDIA", "分配 PS 缓冲失败");
        goto out;
    }

    {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ctx->dst.sin_addr, ip, sizeof(ip));
        LOGI("MEDIA", "推流线程启动：目标 %s:%u，SSRC=%08X，PT=%u，MTU=%zu，fps=%u，音频=%s",
             ip, (unsigned)ntohs(ctx->dst.sin_port), ctx->ssrc,
             (unsigned)ctx->tx.pt, ctx->tx.max_payload, (unsigned)cfg->fps,
             ctx->audio_on ? "开（G.711A 复用在同一条 PS/RTP 流）" : "关");
    }

    src = push_open_source(ctx, cfg);
    if (!src) {
        LOGE("MEDIA", "打开媒体源失败，推流线程退出");
        goto out;
    }

    gb_audio_gen_init(&agen, GB_AUDIO_TONE_HZ, 0);
    clock_gettime(CLOCK_MONOTONIC, &next_ts);

    while (!ctx->stop) {
        const uint8_t *frame = NULL;
        size_t flen = 0;
        int n = gb_src_next_frame(src, &frame, &flen);

        if (n <= 0) {
            if (++idle >= GB_SRC_IDLE_RETRY) {
                LOGI("MEDIA", "媒体源长时间无数据，重开：%s",
                     cfg->fifo_path[0] ? cfg->fifo_path : cfg->h264_file);
                gb_src_close(src);
                idle = 0;
                src = push_open_source(ctx, cfg);
                if (!src) break;
            } else {
                (void)push_sleep(ctx, 20);
            }
            continue;
        }
        idle = 0;
        if (ctx->stop) break;   /* 取完帧再看一眼停止标志：BYE 之后不要再发新数据 */

        /* ---- 视频先发 ----
         * 顺序很重要：视频 PS 里带 System Header + PSM（流的「目录」），
         * 接收端的解复用器要先看到它才知道 0xE0/0xC0 分别是什么。
         * 先发音频的话，第一包只能是「Pack Header + 音频 PES」，
         * 有些解复用器会先抱怨一段「未知流」再自愈。 */
        {
            const uint8_t *psm = ctx->audio_on ? GB_PS_PSM_AV : GB_PS_PSM_VIDEO;
            size_t psm_len = ctx->audio_on ? sizeof(GB_PS_PSM_AV) : sizeof(GB_PS_PSM_VIDEO);
            int is_idr = gb_src_frame_is_idr(frame, flen);
            int pn = gb_ps_pack_video(ps, (size_t)GB_PS_MAX_PACKET + 1024,
                                      frame, flen, video_pts, psm, psm_len);

            if (pn < 0) {
                LOGE("MEDIA", "组 PS 失败（帧 %zu 字节，缓冲 %u 字节）",
                     flen, (unsigned)(GB_PS_MAX_PACKET + 1024));
            } else if (gb_rtp_send_ps(ctx->fd, &ctx->dst, (unsigned)sizeof(ctx->dst),
                                      &ctx->tx, ps, (size_t)pn) < 0) {
                LOGW("MEDIA", "RTP 发送失败：%s", strerror(errno));
            } else if (frames == 0) {
                LOGI("MEDIA", "首帧已送出：%zu 字节 → PS %d 字节，PTS=%llu，关键帧=%s",
                     flen, pn, (unsigned long long)video_pts, is_idr ? "是" : "否");
                if (!is_idr) {
                    LOGW("MEDIA", "首帧不是关键帧：接收端可能一直黑屏（文件模式请确认裸流以 SPS+PPS+IDR 开头）");
                }
                LOGD("MEDIA", "首包 PS 头部（对着协议文档逐字节核对）");
                gb_hexdump_head("PS", ps, (size_t)pn, 64);
            }
        }

        /* ---- 再补这一帧周期里的音频：和视频共用 90kHz 时钟 ----
         * 国标做法：音频不单独开 RTP 流，G.711A 的 PES 直接复用到同一条 PS/RTP 流里
         *（SDP 里 m=audio 写 0 表示不用独立音频端口）。
         * 每 20ms 一帧 = 1800 ticks，把「本视频帧周期内该产的音频」补齐即可，
         * 两个 PTS 在同一时钟域，接收端直接可比。 */
        if (ctx->audio_on) {
            uint64_t horizon = video_pts + pts_step;
            int guard = 0;

            while (!ctx->stop && agen.pts90k < horizon && guard++ < GB_AUDIO_MAX_BURST) {
                uint64_t apts = 0;
                int alen = gb_audio_gen_next(&agen, abuf, sizeof(abuf), &apts);
                int an;

                if (alen <= 0) break;
                an = gb_ps_pack_audio(aps, (size_t)GB_PS_MAX_PACKET, abuf, (size_t)alen, apts);
                if (an <= 0) {
                    LOGE("MEDIA", "组音频 PS 失败");
                    break;
                }
                if (gb_rtp_send_ps(ctx->fd, &ctx->dst, (unsigned)sizeof(ctx->dst),
                                   &ctx->tx, aps, (size_t)an) < 0) {
                    LOGW("MEDIA", "音频 RTP 发送失败：%s", strerror(errno));
                }
            }
        }

        frames++;
        video_pts += pts_step;

        if (cfg->fps > 0 && (frames % (int)cfg->fps) == 0) {
            LOGI("MEDIA", "已推送 %d 帧（下一个 seq=%u，PTS=%llu）",
                 frames, (unsigned)ctx->tx.seq, (unsigned long long)video_pts);
        }

        next_ts.tv_nsec += frame_ns;
        while (next_ts.tv_nsec >= 1000000000L) {
            next_ts.tv_nsec -= 1000000000L;
            next_ts.tv_sec += 1;
        }
        push_wait_until(ctx, &next_ts);
    }

out:
    if (src) gb_src_close(src);
    free(ps);
    free(aps);
    ctx->running = 0;
    LOGI("MEDIA", "推流线程退出（本次共推送 %d 帧）", frames);
    return NULL;
}
