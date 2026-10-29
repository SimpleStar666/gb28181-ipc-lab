/*
 * main.c — GB28181 设备端（IPC 侧）模拟器主程序
 * ============================================================
 * 启动顺序：
 *   解析参数 → 载入并校验配置 → 打配置自检 → 建 SIP socket → REGISTER(+Digest)
 *   → 起心跳线程 → 主循环收请求：
 *        INVITE  → 回 200 OK + SDP(y=SSRC) → 起推流线程（PS/RTP）
 *        MESSAGE → 回 200 OK，再按 CmdType 上报 Catalog / DeviceInfo
 *        BYE     → 回 200 OK → 停止推流并 join 推流线程
 *        OPTIONS → 回 200 OK
 *   → Ctrl-C：停推流、关 socket、退出
 *
 * 【为什么 main 第一行就必须 setvbuf(stdout, NULL, _IOLBF, 0)】
 *   stdout 不是 tty 时（重定向到文件、qemu 下跑），C 库默认用全缓冲，
 *   日志会攒到 4KB 才落盘 —— 现场排障时看到的「程序卡住了」，
 *   十有八九只是日志还没刷出来。这一行必须在任何输出之前执行。
 *
 * 【推流线程为什么能被 BYE 唤醒】
 *   用户态线程没有「异步取消」这种便宜事：靠的是 volatile stop 标志 +
 *   所有 sleep 都切成 ≤20ms 的分片（gb_media.c 里实现），
 *   所以 BYE 之后最多 20ms 线程就会退出，不会卡在 read/sleep 里。
 */
#include "gb_config.h"
#include "gb_log.h"
#include "gb_manscdp.h"
#include "gb_media.h"
#include "gb_ps.h"
#include "gb_sip.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* 只由信号处理器写、主循环读的标志 */
static volatile sig_atomic_t g_stop = 0;

static gb_push_ctx_t *g_push = NULL;
static pthread_t      g_push_th;
static int            g_push_th_valid = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *prog)
{
    printf("用法：%s [选项]\n\n", prog);
    printf("  -c <文件>       指定配置文件（缺省 config.ini，也可用环境变量 GB_CONFIG）\n");
    printf("  -live <FIFO>    实时流模式：从命名管道读 H.264（宿主侧 ffmpeg 编码写入）\n");
    printf("                    不带该参数 = 文件循环模式（读 h264_file）\n");
    printf("  -h, --help      显示本帮助\n\n");
    printf("环境变量：GB_LOG=debug|info|warn|error 覆盖配置里的 log_level\n");
    printf("RTP/媒体目标地址来自平台 INVITE 的 SDP，不在这里配置。\n");
}

/* 停推流：置标志 + join（join 保证释放前线程已经不再碰 ctx） */
static void stop_push(void)
{
    if (!g_push) return;

    gb_push_stop(g_push);
    if (g_push_th_valid) {
        pthread_join(g_push_th, NULL);
        g_push_th_valid = 0;
    }
    gb_push_destroy(g_push);
    g_push = NULL;
    LOGI("MAIN", "推流已停止");
}

static void start_push(gb_sip_t *sip, const char *ip, uint16_t port, uint32_t ssrc)
{
    gb_config_t *cfg = gb_config();

    stop_push();   /* 重复 INVITE（re-INVITE）先停旧的，避免两个线程同时发同一 SSRC */

    g_push = gb_push_create(sip->fd, ip, port, ssrc, cfg->audio_enable);
    if (!g_push) {
        LOGE("MAIN", "创建推流上下文失败（目标 %s:%u）", ip, (unsigned)port);
        return;
    }
    if (pthread_create(&g_push_th, NULL, gb_push_loop, g_push) != 0) {
        LOGE("MAIN", "创建推流线程失败：%s", strerror(errno));
        gb_push_destroy(g_push);
        g_push = NULL;
        return;
    }
    g_push_th_valid = 1;
    LOGI("MAIN", "推流线程已启动（目标 %s:%u，SSRC=%08X，音频 %s）",
         ip, (unsigned)port, ssrc, cfg->audio_enable ? "开" : "关");
}

/* 应答 INVITE 的 SDP。
 *   m=video 的端口写 0：本端是 sendonly，RTP 目标由平台 INVITE 里的 c=/m= 决定；
 *   y= 是国标扩展行，携带 SSRC，接收端用它校验 RTP 头里的 SSRC。
 *   音频不单独开一条流（国标做法：G.711A 的 PES 复用在同一条 PS/RTP 流里），
 *   所以 m=audio 的端口也写 0（= 禁用）。
 *
 * 【y= 必须是十进制】GB/T 28181 附录 C / RFC 5576 规定 y= 的值是 SSRC 的
 *   十进制表示，这里**有意偏离原型**：原型的 y=%08x 会把 123456789 写成
 *   "075bcd15"，按十进制解析的平台读到 'b' 就停下，拿到错误的 SSRC。
 *   因此用 %u，且不补零（补零不增加信息，反而引入「必须 10 位」的额外假设）。 */
static int build_sdp(char *buf, size_t cap, const char *local_ip, uint32_t ssrc)
{
    gb_config_t *cfg = gb_config();
    int n;

    if (cfg->audio_enable) {
        n = snprintf(buf, cap,
                     "v=0\r\n"
                     "o=%s 0 0 IN IP4 %s\r\n"
                     "s=Play\r\n"
                     "c=IN IP4 %s\r\n"
                     "t=0 0\r\n"
                     "m=video 0 RTP/AVP %u\r\n"
                     "a=rtpmap:%u PS/90000\r\n"
                     "a=sendonly\r\n"
                     "m=audio 0 RTP/AVP %d\r\n"
                     "a=rtpmap:%d PCMA/8000\r\n"
                     "y=%u\r\n",
                     cfg->device_id, local_ip, local_ip,
                     (unsigned)cfg->rtp_pt, (unsigned)cfg->rtp_pt,
                     cfg->audio_pt, cfg->audio_pt, ssrc);
    } else {
        n = snprintf(buf, cap,
                     "v=0\r\n"
                     "o=%s 0 0 IN IP4 %s\r\n"
                     "s=Play\r\n"
                     "c=IN IP4 %s\r\n"
                     "t=0 0\r\n"
                     "m=video 0 RTP/AVP %u\r\n"
                     "a=rtpmap:%u PS/90000\r\n"
                     "a=sendonly\r\n"
                     "y=%u\r\n",
                     cfg->device_id, local_ip, local_ip,
                     (unsigned)cfg->rtp_pt, (unsigned)cfg->rtp_pt, ssrc);
    }

    if (n < 0 || (size_t)n >= cap) {
        LOGE("MAIN", "SDP 缓冲不足（%d >= %zu）", n, cap);
        return GB_ERR;
    }
    return GB_OK;
}

static void handle_invite(gb_sip_t *sip, const char *req)
{
    gb_config_t *cfg = gb_config();
    const char *body = strstr(req, "\r\n\r\n");
    char ip[64] = "";
    char ssrc_s[32] = "";
    char sdp[512];
    char resp[GB_SIP_BUF];
    const char *p, *m, *y;
    long port = 0;
    uint32_t ssrc;

    if (!body) {
        LOGE("SIP", "INVITE 没有 SDP body，忽略");
        return;
    }
    body += 4;

    /* 注意：这里只取第一个 c= 行（会话级）。若平台把 c= 放在媒体级，
     * 严格实现应该按 m=video 段落就近取 —— 常见平台都放在会话级。 */
    p = strstr(body, "c=IN IP4 ");
    m = strstr(body, "m=video ");
    if (!p || !m) {
        LOGE("SIP", "INVITE 的 SDP 解析失败（缺 c= 或 m=video），忽略");
        return;
    }
    if (sscanf(p + 9, "%63s", ip) != 1) {
        LOGE("SIP", "INVITE 的 c= 行解析失败");
        return;
    }
    if (sscanf(m + 8, "%ld", &port) != 1) {
        LOGE("SIP", "INVITE 的 m=video 行解析失败");
        return;
    }
    if (port <= 0 || port > 65535) {
        LOGE("SIP", "INVITE 给的 RTP 端口非法：%ld", port);
        return;
    }

    y = strstr(body, "y=");
    ssrc = cfg->ssrc_default;
    if (y && sscanf(y + 2, "%31s", ssrc_s) == 1 && ssrc_s[0]) {
        /* y= 是十进制（不能用 base 0，否则前导 0 会被当八进制） */
        ssrc = (uint32_t)strtoul(ssrc_s, NULL, 10);
    }

    LOGI("SIP", "收到 INVITE 点播 → RTP 目标 %s:%ld，SSRC=%08X（%s）",
         ip, port, ssrc, y ? "来自 y=" : "无 y=，用默认 SSRC");

    if (build_sdp(sdp, sizeof(sdp), sip->local_ip, ssrc) != GB_OK) return;
    LOGD("SIP", "应答 SDP：\n%s", sdp);

    if (gb_sip_build_200(sip, resp, sizeof(resp), req, NULL, sdp) < 0) {
        LOGE("SIP", "构造 200 OK 失败，INVITE 未应答");
        return;
    }
    if (gb_sip_send(sip, resp) < 0) {
        LOGE("SIP", "发送 200 OK 失败");
        return;
    }
    LOGI("SIP", "已回 200 OK + SDP（sendonly），准备推流");

    start_push(sip, ip, (uint16_t)port, ssrc);
}

static void report_xml(gb_sip_t *sip, const char *what, int (*build)(char *, size_t, long), long sn)
{
    char xml[2048];
    char msg[GB_SIP_BUF];

    if (build(xml, sizeof(xml), sn) != GB_OK) return;
    if (gb_sip_build_message(sip, msg, sizeof(msg), xml) < 0) return;
    if (gb_sip_send(sip, msg) > 0) LOGI("SIP", "已上报%s（SN=%ld）", what, sn);
}

static void handle_request(gb_sip_t *sip, const char *req)
{
    char resp[GB_SIP_BUF];

    /* 主循环收到的报文有两类，先在首行上分开：
     *   1. 平台的**响应**（"SIP/2.0 200 OK" 等）—— 对我们 REGISTER / MESSAGE / BYE
     *      的回执。这些在各自的发送流程里已经处理完（注册流程内部自己 select+recv），
     *      主循环只负责把剩下的收掉，不能当成「未处理的请求」——
     *      否则日志里会混进一堆 "未处理的请求：SIP/2.0 200 OK"，
     *      让人以为设备收到了没处理的请求，排障时把方向带偏。
     *      （本仓库的卖点之一是「日志能一眼看出卡在哪一层」，这类噪声和它冲突。）
     *   2. 平台的**请求**（INVITE / MESSAGE / BYE / OPTIONS / ACK / REGISTER），
     *      下面按方法分发。 */
    if (strncmp(req, "SIP/2.0 ", 8) == 0) {
        LOGD("SIP", "收到平台响应（无需处理）：%.40s", req);
        return;
    }

    /* ACK 是平台对我们 INVITE 的 200 OK 的确认：按 RFC 3261，UAS 收到 ACK
     * 不需要回任何东西，也不需要做任何事 —— 但它是「已知且无需处理」，
     * 必须和「不认识的方法」区分开，否则每次点播都会刷一条假的「未处理」。 */
    if (strncmp(req, "ACK ", 4) == 0) {
        LOGD("SIP", "收到 ACK（INVITE 事务完成，按规范无需处理）");
        return;
    }

    if (strncmp(req, "INVITE ", 7) == 0) {
        handle_invite(sip, req);
        return;
    }

    if (strncmp(req, "BYE ", 4) == 0) {
        LOGI("SIP", "收到 BYE（停止点播）");
        if (gb_sip_build_200(sip, resp, sizeof(resp), req, NULL, NULL) > 0) gb_sip_send(sip, resp);
        stop_push();
        return;
    }

    if (strncmp(req, "MESSAGE ", 8) == 0) {
        const char *body = strstr(req, "\r\n\r\n");
        char cmd[64];
        long sn = 0;

        body = body ? body + 4 : "";
        gb_xml_parse_query(body, &sn, cmd, sizeof(cmd));
        LOGI("SIP", "收到 MESSAGE 查询：CmdType=%s SN=%ld", cmd[0] ? cmd : "(空)", sn);

        /* 先回 200 OK：MANSCDP 的应答是另发一条 MESSAGE，不是一个带 body 的 200 */
        if (gb_sip_build_200(sip, resp, sizeof(resp), req, NULL, NULL) > 0) gb_sip_send(sip, resp);

        if (strcmp(cmd, "Catalog") == 0) {
            report_xml(sip, "目录 Catalog", gb_xml_catalog_response, sn);
        } else if (strcmp(cmd, "DeviceInfo") == 0) {
            report_xml(sip, "设备信息 DeviceInfo", gb_xml_device_info, sn);
        } else if (cmd[0]) {
            /* SN 必须回填请求里的值，否则平台匹配不上应答（界面一直转圈） */
            LOGD("SIP", "未实现的查询类型 %s，按国标可以不响应", cmd);
        }
        return;
    }

    if (strncmp(req, "OPTIONS ", 8) == 0) {
        char extra[128];
        snprintf(extra, sizeof(extra),
                 "Allow: INVITE, ACK, BYE, CANCEL, OPTIONS, MESSAGE, INFO\r\n");
        if (gb_sip_build_200(sip, resp, sizeof(resp), req, extra, NULL) > 0) gb_sip_send(sip, resp);
        return;
    }

    if (strncmp(req, "REGISTER ", 9) == 0) {
        /* 平台有时也会主动向设备 REGISTER（做离线检测），回 200 即可 */
        if (gb_sip_build_200(sip, resp, sizeof(resp), req, NULL, NULL) > 0) gb_sip_send(sip, resp);
        return;
    }

    LOGD("SIP", "未处理的请求：%.48s", req);
}

static void *heartbeat_loop(void *arg)
{
    gb_sip_t *sip = (gb_sip_t *)arg;
    gb_config_t *cfg = gb_config();
    char xml[1024];
    char msg[GB_SIP_BUF];
    long sn = 1;

    LOGI("SIP", "心跳线程启动（周期 %d 秒）", cfg->heartbeat_s);

    while (!g_stop) {
        int waited = 0;

        /* sleep 切分片：Ctrl-C 时最多 200ms 就能退出 */
        while (waited < cfg->heartbeat_s * 1000 && !g_stop) {
            usleep(200 * 1000);
            waited += 200;
        }
        if (g_stop) break;

        if (gb_xml_keepalive(xml, sizeof(xml), sn) != GB_OK) continue;
        if (gb_sip_build_message(sip, msg, sizeof(msg), xml) < 0) continue;
        if (gb_sip_send(sip, msg) > 0) LOGD("SIP", "Keepalive 已发送（SN=%ld）", sn);
        sn++;
    }

    LOGI("SIP", "心跳线程退出");
    return NULL;
}

int main(int argc, char *argv[])
{
    gb_config_t cfg;
    gb_sip_t sip;
    const char *cfg_path = getenv("GB_CONFIG");
    const char *explicit_cfg = NULL;
    const char *live = NULL;
    pthread_t hb_th;
    int hb_th_valid = 0;
    time_t last_reg;
    int i;

    /* ★ 第一件事：stdout 行缓冲（理由见文件头） */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (!cfg_path) cfg_path = "config.ini";

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            cfg_path = argv[++i];
            explicit_cfg = cfg_path;
        } else if (strcmp(argv[i], "-live") == 0 && i + 1 < argc) {
            live = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "未知参数：%s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    /* ---- 配置 ---- */
    if (gb_config_load(&cfg, cfg_path) != GB_OK) {
        if (explicit_cfg) return 1;                 /* 用户明确指定的文件读不了 = 硬错误 */
        LOGW("MAIN", "默认配置文件 %s 不可用，改用内置默认值", cfg_path);
        if (gb_config_load(&cfg, NULL) != GB_OK) return 1;
    }

    /* -live 就把 FIFO 路径写进配置：媒体层只认配置，不认命令行 */
    if (live) {
        if (strlen(live) >= sizeof(cfg.fifo_path)) {
            LOGE("MAIN", "FIFO 路径过长（上限 %zu 字节）：%s", sizeof(cfg.fifo_path) - 1, live);
            return 1;
        }
        snprintf(cfg.fifo_path, sizeof(cfg.fifo_path), "%s", live);
    } else {
        cfg.fifo_path[0] = '\0';                    /* 空 = 文件循环模式 */
    }

    if (gb_config_validate(&cfg) != GB_OK) {
        LOGE("MAIN", "配置校验失败，修正后重试（%s -h 看用法）", argv[0]);
        return 1;
    }
    gb_config_set_global(&cfg);

    /* GB_LOG 环境变量优先于配置文件：现场排障不该去改配置文件 */
    if (getenv("GB_LOG")) {
        gb_log_init_from_env();
    } else {
        gb_log_set_level((gb_log_level_t)cfg.log_level);
    }

    gb_config_dump(&cfg);
    LOGI("MAIN", "=========================================");
    LOGI("MAIN", " GB28181 IPC 模拟器（设备端 / 教学实现）");
    LOGI("MAIN", " 设备 ID %s", cfg.device_id);
    LOGI("MAIN", " 平台    %s:%u", cfg.server_ip, (unsigned)cfg.server_port);
    LOGI("MAIN", " 本地    %u/UDP", (unsigned)cfg.local_port);
    LOGI("MAIN", " 媒体源  %s", cfg.fifo_path[0] ? cfg.fifo_path : cfg.h264_file);
    LOGI("MAIN", "=========================================");

    {
        /* 启动自检：PTS 编码是最容易错一位的地方，开机就把它打出来 */
        uint8_t pts5[5];
        gb_ps_encode_pts(pts5, 90000);
        LOGD("PS", "PTS 自检：gb_ps_encode_pts(90000) → %02X %02X %02X %02X %02X"
                   "（正确值 21 00 05 BF 21；写成 >>15 会得到 21 00 03 BF 21）",
             pts5[0], pts5[1], pts5[2], pts5[3], pts5[4]);
    }

    /* ---- 信号 ---- */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);      /* UDP 用不上，但避免任何写意外把进程打死 */

    /* ---- SIP ---- */
    if (gb_sip_init(&sip) != GB_OK) {
        LOGE("MAIN", "SIP 初始化失败");
        return 1;
    }
    if (gb_sip_register(&sip) != GB_OK) {
        LOGE("MAIN", "注册失败，退出。请核对 config.ini 里的 server_ip / server_port / device_id / device_pwd");
        gb_sip_close(&sip);
        return 1;
    }
    LOGI("MAIN", "注册成功，设备 ID %s", cfg.device_id);

    if (pthread_create(&hb_th, NULL, heartbeat_loop, &sip) != 0) {
        LOGE("MAIN", "心跳线程创建失败：%s", strerror(errno));
    } else {
        hb_th_valid = 1;
    }

    last_reg = time(NULL);
    LOGI("MAIN", "进入主循环：等待平台 INVITE / MESSAGE / BYE（Ctrl-C 退出）");

    while (!g_stop) {
        fd_set rset;
        struct timeval tv;
        int r;

        FD_ZERO(&rset);
        FD_SET(sip.fd, &rset);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        r = select(sip.fd + 1, &rset, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(sip.fd, &rset)) {
            char buf[GB_SIP_BUF];
            ssize_t n = recv(sip.fd, buf, sizeof(buf) - 1, 0);

            if (n > 0) {
                buf[n] = '\0';
                handle_request(&sip, buf);
            }
        } else if (r < 0 && errno != EINTR) {
            LOGE("MAIN", "select 失败：%s", strerror(errno));
            break;
        }

        /* 续注册：有效期过半就重发 REGISTER，真机就是这么保活的 */
        if (cfg.expires >= 60 && (time(NULL) - last_reg) >= (time_t)(cfg.expires / 2)) {
            LOGI("MAIN", "注册有效期过半（%d 秒），续注册", cfg.expires);
            if (gb_sip_register(&sip) == GB_OK) last_reg = time(NULL);
        }
    }

    /* ---- 收尾 ---- */
    LOGI("MAIN", "收到退出信号，开始清理");
    stop_push();
    if (hb_th_valid) pthread_join(hb_th, NULL);
    gb_sip_close(&sip);
    LOGI("MAIN", "已退出");
    return 0;
}
