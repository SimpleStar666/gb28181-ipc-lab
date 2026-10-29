/*
 * kata 08 / 08 —— 本机 UDP 环回：发 5 帧 RTP，收端拼帧并统计
 * ==========================================================================
 * 【这一步解决什么问题】把 kata 05 的分片逻辑真的发到网上，再写一个「平台侧」接收端，
 *   按 seq / timestamp / marker 把包拼回帧，验证三件事：
 *     ① 收包数 == 帧数 × 每帧片数        ② 每帧只有最后一片 M=1
 *     ③ 同一帧所有分片的 timestamp 相同
 *   这三条就是平台（ZLMediaKit / WVP / 各种 GB28181 平台）PS-RTP 收流的核心逻辑。
 * 【对应协议】RFC 3550（RTP）＋ GB/T 28181 附录 C（PS over RTP，PT 96）；
 *   组帧判据 M 位、帧时间戳、seq 连续性都来自 RTP 规范。
 * 【设计】fork 一个子进程当接收端（绑 127.0.0.1 的随机端口），父进程切片发送；
 *   子进程打印每包/每帧的解析结果与统计并决定退出码，父进程 waitpid 后给出总结论。
 *   用进程不用线程：不依赖 -pthread，也避免共享状态互相干扰。
 * 【面试会怎么问 / 怎么答】
 *   Q1 平台怎么知道一帧收齐了？  A: 看 marker=1；同时靠 timestamp 变化判断新帧、
 *      靠 seq 判丢包。marker 丢了就只能在 ts 变化时结帧（延迟一帧）。
 *   Q2 seq 是 16 位会回绕，怎么判连续？  A: 用 (unsigned short)(a - b) 的差值，
 *      不能直接比大小。
 *   Q3 丢了一片怎么办？  A: PS over RTP 无重传，MS 丢了整帧就没法拼，只能丢这帧等
 *      下一个 I 帧；对流媒体来说「宁可花屏一瞬，也不要延迟累积」。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/time.h>

#define PT        96
#define MTU       1400
#define SSRC      0x12345678u
#define NFRAMES   5
#define FRAME_LEN 3000
#define NPIECE    ((FRAME_LEN + MTU - 1) / MTU)     /* 3 */
#define NPKT      (NFRAMES * NPIECE)                /* 15 */
#define TS_STEP   6000u                             /* 90kHz / 15fps */

static int build_rtp(unsigned char *pkt, unsigned int seq, unsigned int ts, int marker,
                     const unsigned char *pay, size_t len)
{
    pkt[0] = 0x80;
    pkt[1] = (unsigned char)(PT | (marker ? 0x80 : 0));   /* marker 是最高位 */
    pkt[2] = (unsigned char)((seq >> 8) & 0xFF);
    pkt[3] = (unsigned char)(seq & 0xFF);
    pkt[4] = (unsigned char)((ts >> 24) & 0xFF);
    pkt[5] = (unsigned char)((ts >> 16) & 0xFF);
    pkt[6] = (unsigned char)((ts >> 8) & 0xFF);
    pkt[7] = (unsigned char)(ts & 0xFF);
    pkt[8]  = (unsigned char)((SSRC >> 24) & 0xFF);
    pkt[9]  = (unsigned char)((SSRC >> 16) & 0xFF);
    pkt[10] = (unsigned char)((SSRC >> 8) & 0xFF);
    pkt[11] = (unsigned char)(SSRC & 0xFF);
    memcpy(pkt + 12, pay, len);
    return (int)(12 + len);
}

static int receiver(int fd)
{
    unsigned char buf[2048];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    unsigned int cur_ts = 0, last_seq = 0;
    int got = 0, frames = 0, piece = 0, have_seq = 0, seq_ok = 1, pt_ok = 1, ts_ok = 1, m_ok = 1;

    printf("  [接收端] 开始收包，期望 %d 包（%d 帧 × %d 片）\n", NPKT, NFRAMES, NPIECE);
    while (got < NPKT) {
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        unsigned int seq, ts, ssrc;
        size_t pay;
        int m, pt;
        if (n < 12) { printf("  [接收端] 收包失败或超时（已收 %d 个）\n", got); return 1; }
        seq = (unsigned int)((buf[2] << 8) | buf[3]);
        ts  = ((unsigned int)buf[4] << 24) | ((unsigned int)buf[5] << 16) |
              ((unsigned int)buf[6] << 8) | (unsigned int)buf[7];
        ssrc = ((unsigned int)buf[8] << 24) | ((unsigned int)buf[9] << 16) |
               ((unsigned int)buf[10] << 8) | (unsigned int)buf[11];
        m = (buf[1] >> 7) & 1;
        pt = buf[1] & 0x7F;
        pay = (size_t)n - 12;

        printf("  [接收端] 包 %2d: seq=%-5u ts=%-6u M=%d PT=%d SSRC=%08X 载荷=%zu 字节\n",
               got + 1, seq, ts, m, pt, ssrc, pay);
        if (have_seq && seq != (unsigned int)((last_seq + 1) & 0xFFFF)) seq_ok = 0;
        last_seq = seq; have_seq = 1;
        if (pt != PT) pt_ok = 0;
        if (piece == 0) cur_ts = ts;
        else if (ts != cur_ts) ts_ok = 0;
        if (m && piece + 1 != NPIECE) m_ok = 0;
        if (!m && piece + 1 == NPIECE) m_ok = 0;
        piece++;
        if (m) {
            frames++;
            printf("  [接收端] >>> 第 %d 帧收齐：%d 片，末片 M=1，ts=%u\n", frames, piece, ts);
            piece = 0;
        }
        got++;
    }

    printf("\n  [接收端] ===== 统计 =====\n");
    printf("  [接收端] 收包数        = %d（期望 %d）%s\n", got, NPKT, got == NPKT ? " ✓" : " ✗");
    printf("  [接收端] 完整帧数      = %d（期望 %d）%s\n", frames, NFRAMES, frames == NFRAMES ? " ✓" : " ✗");
    printf("  [接收端] 每帧分片数    = %d（期望 %d）\n", NPIECE, NPIECE);
    printf("  [接收端] 每帧末片 M=1  = %s\n", m_ok ? "全部满足 ✓" : "有帧不满足 ✗");
    printf("  [接收端] 同帧 ts 一致  = %s；seq 连续 = %s；PT 全为 %d = %s\n",
           ts_ok ? "是 ✓" : "否 ✗", seq_ok ? "是 ✓" : "否 ✗", PT, pt_ok ? "是 ✓" : "否 ✗");
    return (got == NPKT && frames == NFRAMES && seq_ok && pt_ok && ts_ok && m_ok) ? 0 : 1;
}

int main(void)
{
    unsigned char payload[FRAME_LEN], pkt[12 + MTU];
    struct sockaddr_in addr;
    struct timeval tv;
    socklen_t alen = sizeof addr;
    unsigned int seq = 1000;
    int fd, pid, i, f, status = 0, send_err = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=========== kata 08：UDP 环回发 5 帧 RTP ===========\n");
    for (i = 0; i < FRAME_LEN; i++) payload[i] = (unsigned char)(i & 0xFF);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (getsockname(fd, (struct sockaddr *)&addr, &alen) < 0) { perror("getsockname"); return 1; }
    printf("  接收端绑定 127.0.0.1:%u\n", (unsigned)ntohs(addr.sin_port));
    tv.tv_sec = 2; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    fflush(stdout);
    pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) exit(receiver(fd));      /* 子进程 = 平台侧接收端 */

    for (f = 0; f < NFRAMES; f++) {
        unsigned int ts = 90000u + (unsigned int)f * TS_STEP;
        size_t off = 0;
        while (off < FRAME_LEN) {
            size_t piece = FRAME_LEN - off;
            int last, len;
            if (piece > MTU) piece = MTU;
            last = (off + piece >= FRAME_LEN);
            len = build_rtp(pkt, seq, ts, last, payload + off, piece);
            if (sendto(fd, pkt, (size_t)len, 0, (struct sockaddr *)&addr, sizeof addr) != len) send_err = 1;
            seq++; off += piece;
        }
        printf("  [发送端] 第 %d/%d 帧已发：ts=%u，%d 片，末片 M=1，seq 到 %u\n",
               f + 1, NFRAMES, ts, NPIECE, seq - 1);
        usleep(2000);
    }

    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("\nSELF-CHECK FAIL: 接收端统计不满足预期（退出码 %d）\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return 1;
    }
    if (send_err) { printf("\nSELF-CHECK FAIL: 有 sendto 未发全\n"); return 1; }
    printf("\nSELF-CHECK PASS\n");
    return 0;
}
