/*
 * mkps_pts.c — 生成「只含 1 帧、PTS 已知」的 .ps 文件（verify_pts.sh 的夹具）
 * ============================================================
 * 这是「PTS 位域没写错」这条结论的输入端：
 *   写：gb_ps_pack_video(..., pts90k = N)  → build/pts_*.ps
 *   读：ffprobe -show_packets 报出的 pts 必须 == N
 * 只要 byte2 写成 >>15，ffprobe 读回来的值就会和 N 差一截（例如 90000 → 98304），
 * 而**画面照样能解**，所以单靠肉眼看不出问题 —— 必须靠这条闭环。
 *
 * 用法：
 *   mkps_pts <out.ps> <pts90k> [h264_file]
 *     h264_file 省略时用内置的最小 AnnexB 帧（SPS/PPS + 桩 slice）。
 *     内置帧的 SPS/PPS 是从 x264 真实输出里截下来的，
 *     所以 ffprobe 至少能认出这是 H.264；能否解码出画面取决于 slice，
 *     解不出时 verify_pts.sh 会退化为「按位域解析 PS 字节」的校验路径。
 *
 * 退出码：0 = 成功写出；1 = 参数/IO/打包失败。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "gb_ps.h"
#include "gb_annexb.h"
#include "gb_common.h"

/* 内置最小帧：来自 ffmpeg(libx264) 生成的 640x360@15 码流头部
 *   SPS(29B) + PPS(8B) + 桩 IDR slice(15B) */
static const uint8_t fallback_au[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x16, 0xDA, 0x02, 0x80, 0xBF,
    0xE5, 0xC0, 0x44, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00,
    0x7A, 0x3C, 0x58, 0xBA, 0x80,
    0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x0F, 0xC8,
    0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x0A, 0xFF, 0x01, 0x02,
    0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A
};

#define MAX_FILE (8u * 1024u * 1024u)

/* 取第一个「访问单元」：从第一个起始码开始，到第一个 VCL NAL 之后的下一个起始码为止。
 * x264 用 repeat-headers=1 时每个 IDR 前都会重复 SPS/PPS，
 * 所以第一帧天然自带参数集 —— 这正是打 PS 想要的样子（PSM 声明 0x1B/H.264）。 */
static int extract_first_au(const uint8_t *d, size_t len, size_t *au_off, size_t *au_len)
{
    size_t pos = 0, sc = 0;
    int have_start = 0, seen_vcl = 0;
    size_t start = 0;

    while (pos < len) {
        int r = gb_annexb_find_startcode(d, len, pos, &sc);
        if (r < 0)
            break;
        if (!have_start) {
            start = (size_t)r;
            have_start = 1;
        } else if (seen_vcl) {
            *au_off = start;
            *au_len = (size_t)r - start;
            return 0;
        }
        {
            int t = gb_annexb_nal_type(d + r, sc);
            if (t >= 1 && t <= 5)
                seen_vcl = 1;
        }
        pos = (size_t)r + sc;
    }
    if (have_start) {
        *au_off = start;
        *au_len = len - start;
        return 0;
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *outpath;
    uint64_t pts;
    static uint8_t filebuf[MAX_FILE];
    static uint8_t ps[GB_PS_MAX_PACKET];
    const uint8_t *frame;
    size_t frame_len;
    int ps_len;
    FILE *fp;
    uint8_t pts_bytes[5];
    const char *src_desc;

    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <out.ps> <pts90k> [h264_file]\n"
                "  example: %s build/pts_90000.ps 90000 assets/testsrc.h264\n",
                argv[0], argv[0]);
        return 1;
    }
    outpath = argv[1];
    pts = strtoull(argv[2], NULL, 0);
    if (pts > GB_PTS_MASK) {
        fprintf(stderr, "[mkps_pts] pts=%llu 超出 33bit 范围\n", (unsigned long long)pts);
        return 1;
    }

    if (argc >= 4) {
        size_t off = 0, len = 0;
        size_t n;
        fp = fopen(argv[3], "rb");
        if (!fp) {
            fprintf(stderr, "[mkps_pts] 打不开 h264 文件: %s\n", argv[3]);
            return 1;
        }
        n = fread(filebuf, 1, sizeof(filebuf), fp);
        fclose(fp);
        if (n == 0) {
            fprintf(stderr, "[mkps_pts] h264 文件为空: %s\n", argv[3]);
            return 1;
        }
        if (extract_first_au(filebuf, n, &off, &len) != 0) {
            fprintf(stderr, "[mkps_pts] 在 %s 里找不到 AnnexB 起始码\n", argv[3]);
            return 1;
        }
        frame = filebuf + off;
        frame_len = len;
        src_desc = argv[3];
    } else {
        frame = fallback_au;
        frame_len = sizeof(fallback_au);
        src_desc = "<内置最小帧>";
    }

    ps_len = gb_ps_pack_video(ps, sizeof(ps), frame, frame_len, pts,
                              GB_PS_PSM_VIDEO, sizeof(GB_PS_PSM_VIDEO));
    if (ps_len <= 0) {
        fprintf(stderr, "[mkps_pts] gb_ps_pack_video 失败 (返回 %d, cap=%zu)\n",
                ps_len, sizeof(ps));
        return 1;
    }

    fp = fopen(outpath, "wb");
    if (!fp) {
        fprintf(stderr, "[mkps_pts] 写不了输出文件: %s\n", outpath);
        return 1;
    }
    if (fwrite(ps, 1, (size_t)ps_len, fp) != (size_t)ps_len) {
        fprintf(stderr, "[mkps_pts] 写入不完整: %s\n", outpath);
        fclose(fp);
        return 1;
    }
    fclose(fp);

    gb_ps_encode_pts(pts_bytes, pts);
    printf("[mkps_pts] %s: 源=%s frame_len=%zu ps_len=%d\n",
           outpath, src_desc, frame_len, ps_len);
    printf("[mkps_pts] 写入 PTS=%llu，PES 里的 5 字节应为 %02X %02X %02X %02X %02X\n",
           (unsigned long long)pts,
           pts_bytes[0], pts_bytes[1], pts_bytes[2], pts_bytes[3], pts_bytes[4]);

    /* 顺手把 PES 头前 14 字节打出来，便于人工核对（PSM 20 + Sys 16 + Pack 14 = 50） */
    if (ps_len >= 64) {
        int i;
        printf("[mkps_pts] 偏移 50 起的 PES 头:");
        for (i = 0; i < 14; i++)
            printf(" %02X", ps[50 + i]);
        printf("\n");
    }
    return 0;
}
