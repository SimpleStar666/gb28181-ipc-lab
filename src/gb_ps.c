/*
 * gb_ps.c — MPEG-PS 打包（本项目最核心的一层）
 * ============================================================
 * 四层结构，每帧重打前 3 层：
 *   Pack Header(14) + System Header(16) + PSM(20 或 24) + PES(14 头 + 载荷)
 *
 * ============================================================
 * 【教学重点 1】PTS 第 3 字节：>>14 而不是 >>15
 * ============================================================
 * MPEG-PS 把 33bit PTS 铺到 5 个字节里，每个字节塞标记位：
 *   byte0 = 0010 P32 P31 P30 1   -> 0x21 | ((pts >> 29) & 0x0E)
 *   byte1 = P29..P22             -> (pts >> 22) & 0xFF
 *   byte2 = P21..P15 1           -> 0x01 | ((pts >> 14) & 0xFE)   <== 关键
 *   byte3 = P14..P7              -> (pts >> 7)  & 0xFF
 *   byte4 = P6..P0 1             -> 0x01 | ((pts & 0x7F) << 1)
 *
 * 推导（byte2）：
 *   byte2 的 bit1..bit7 必须依次放 PTS[15..21]，bit0 是固定标记位 1。
 *   (pts >> 14) 的 bit_k 就是 PTS[14+k]：
 *       bit1 = PTS[15], bit2 = PTS[16], ..., bit7 = PTS[21]     <-- 正是我们要的
 *   所以 & 0xFE（只留 bit1..bit7）再 | 0x01（补标记位）就对了。
 *   若写成 (pts >> 15)：bit1..bit7 = PTS[16..22]，整体错位一位，
 *   低位那一位被丢掉、高位多带进来一位，字节虽然合法但时间戳是错的。
 *   自检：pts = 90000
 *       正确：0x01 | ((90000 >> 14) & 0xFE) = 0x01 | (5 & 0xFE) = 0x05
 *       错误：0x01 | ((90000 >> 15) & 0xFE) = 0x01 | (2 & 0xFE) = 0x03
 *   （90000 = 0x15F90；(90000>>14) = 5，(90000>>15) = 2，差的就是这一位。）
 *   完整 5 字节应为 21 00 05 BF 21。
 *
 * 【为什么这个 bug 特别难发现】
 *   真实链路上，接收端（ZLMediaKit / 各类国标平台）在多数路径上直接用 RTP 头里的
 *   32bit 时间戳做同步与转发，PES 里的 PTS 只被转封装工具偶尔读一下。
 *   所以「PTS 错位一位」在实机上方能正常出画、正常播放 ——
 *   只有 ffprobe 逐帧看 pts、或做 33bit 精确定时对齐时才会暴露。
 *   == 「画面正常」不等于「协议正确」，这也是本项目把 PS 层做成纯函数 + 单测的原因：
 *      能被逐字节断言的协议实现，才谈得上正确。
 *
 * ============================================================
 * 【教学重点 2】PSM 的 length 到底数到哪
 * ============================================================
 *   packet_length 从 current_next_indicator 数到 CRC_32 结束，
 *   不含前面 4 字节起始码 00 00 01 BC，也不含它自己那 2 字节。
 *   单视频：1 + 1 + 2 + 2 + 4 + 4 = 14 = 0x000E  -> 总长 20 字节
 *   含音频：1 + 1 + 2 + 2 + 8 + 4 = 18 = 0x0012  -> 总长 24 字节
 *   交叉核对：ZLMediaKit 3rdpart/media-server/libmpeg/source/mpeg-psm.c 里
 *   element_stream_map_length = program_stream_map_length
 *                             - program_stream_info_length - 10，
 *   与这里数出来的字节数一致。
 *   数错了接收端会直接解析失败（或者把 CRC 当成下一条流条目）。
 */
#include "gb_ps.h"

#include <string.h>

/* ---- 冻结的头部常量：测试逐字节断言，一个 bit 都不许动 ---- */

/* 14 字节。第 5 字节 0x44 表示 MPEG-2 PS（PES 按 MPEG-2 规则解析）；
 * 写成 0x21 就是 MPEG-1，接收端会换一套 PES 解析规则。 */
const uint8_t GB_PS_PACK_HEADER[14] = {
    0x00, 0x00, 0x01, 0xBA, 0x44, 0x00, 0x04,
    0x00, 0x04, 0x01, 0x86, 0x66, 0xCF, 0xF8
};

/* 16 字节，声明码率与流数量 */
const uint8_t GB_PS_SYSTEM_HEADER[16] = {
    0x00, 0x00, 0x01, 0xBB, 0x00, 0x09, 0xC3, 0x33,
    0x67, 0x00, 0x21, 0xFF, 0xE0, 0xE0, 0xE6, 0x00
};

/* 20 字节：单视频流。packet_length = 0x000E = 14（见文件头推导） */
const uint8_t GB_PS_PSM_VIDEO[20] = {
    0x00, 0x00, 0x01, 0xBC, 0x00, 0x0E, 0x01, 0x01, 0x00, 0x00,
    0x00, 0x04, 0x1B, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* 24 字节：视频 + G.711A 音频。
 *   packet_length = 0x0012 = 18
 *   elementary_stream_map_length = 0x0008 = 两条 4 字节流条目：
 *     0x1B/0xE0 = H.264 视频（stream_type 0x1B，elementary_stream_id 0xE0）
 *     0x90/0xC0 = G.711A 音频（stream_type 0x90，elementary_stream_id 0xC0） */
const uint8_t GB_PS_PSM_AV[24] = {
    0x00, 0x00, 0x01, 0xBC, 0x00, 0x12, 0x01, 0x01, 0x00, 0x00, 0x00, 0x08,
    0x1B, 0xE0, 0x00, 0x00, 0x90, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

void gb_ps_encode_pts(uint8_t out[5], uint64_t pts90k)
{
    uint64_t pts;

    if (!out) return;
    pts = pts90k & GB_PTS_MASK;   /* PTS 只有 33 位有效，回绕是正常行为 */

    out[0] = (uint8_t)(0x21 | ((pts >> 29) & 0x0E));
    out[1] = (uint8_t)((pts >> 22) & 0xFF);
    /* ↓↓↓ 就这一行是原型里的 bug：原型写的是 >> 15，整体错位一位 */
    out[2] = (uint8_t)(0x01 | ((pts >> 14) & 0xFE));
    out[3] = (uint8_t)((pts >> 7) & 0xFF);
    out[4] = (uint8_t)(0x01 | ((pts & 0x7F) << 1));
}

int gb_ps_write_pes_header(uint8_t out[14], uint8_t sid, size_t payload_len,
                           uint64_t pts90k, int idr)
{
    size_t pes_len;

    if (!out) return GB_ERR;

    /* PES_packet_length 覆盖「长度字段之后」的全部字节：
     *   3 字节 flags + 5 字节 PTS + 载荷 = payload_len + 8
     * 视频帧可能超过 64KB（0xFFFF），此时按 MPEG 规定写 0 表示「不限制」。 */
    pes_len = payload_len + 8;

    out[0] = 0x00;
    out[1] = 0x00;
    out[2] = 0x01;
    out[3] = sid;
    if (pes_len > 0xFFFFu) {
        out[4] = 0x00;
        out[5] = 0x00;
    } else {
        out[4] = (uint8_t)((pes_len >> 8) & 0xFF);
        out[5] = (uint8_t)(pes_len & 0xFF);
    }
    /* 0x80 = '10' + 各标志位全 0；0x80 = 只有 PTS（无 DTS）；0x05 = PTS 占 5 字节。
     * 关于 idr 参数：gb_ps.h 里记的是「关键帧置 data_alignment_indicator」，
     * 也就是 0x80 | 0x04 = 0x84。但本项目的硬性字节不变量要求 PES 头恒为
     * 0x80 0x80 0x05（与原型/多数国标实现的线上字节一致，重构后要能逐字节对齐），
     * 所以这里显式忽略 idr，只用 (void) 标出它是「为接口兼容而保留」的参数。
     * 若某个平台确实要求关键帧带 data_alignment_indicator，改这一行即可：
     *     out[6] = (uint8_t)(0x80 | (idr ? 0x04 : 0x00)); */
    (void)idr;
    out[6] = 0x80;
    out[7] = 0x80;
    out[8] = 0x05;
    gb_ps_encode_pts(out + 9, pts90k);
    return 14;
}

int gb_ps_pack_video(uint8_t *out, size_t cap,
                     const uint8_t *frame, size_t frame_len,
                     uint64_t pts90k, const uint8_t *psm, size_t psm_len)
{
    size_t off = 0;
    size_t need;
    int pes;

    if (!out || !frame) return GB_ERR;

    need = sizeof(GB_PS_PACK_HEADER) + sizeof(GB_PS_SYSTEM_HEADER) + psm_len + 14 + frame_len;
    if (need > cap) return GB_ERR;   /* 宁可报错，也不截断一帧 —— 截断的 PS 接收端解不出来 */

    memcpy(out + off, GB_PS_PACK_HEADER, sizeof(GB_PS_PACK_HEADER));
    off += sizeof(GB_PS_PACK_HEADER);

    memcpy(out + off, GB_PS_SYSTEM_HEADER, sizeof(GB_PS_SYSTEM_HEADER));
    off += sizeof(GB_PS_SYSTEM_HEADER);

    if (psm && psm_len) {            /* 音频关掉时只传视频 PSM（20 字节） */
        memcpy(out + off, psm, psm_len);
        off += psm_len;
    }

    pes = gb_ps_write_pes_header(out + off, GB_PS_SID_VIDEO, frame_len, pts90k, 0);
    if (pes < 0) return GB_ERR;
    off += (size_t)pes;

    memcpy(out + off, frame, frame_len);
    off += frame_len;

    return (int)off;
}

int gb_ps_pack_audio(uint8_t *out, size_t cap,
                     const uint8_t *pcm_a, size_t len,
                     uint64_t pts90k)
{
    size_t off = 0;
    int pes;

    if (!out || !pcm_a) return GB_ERR;
    if (cap < sizeof(GB_PS_PACK_HEADER) + 14 + len) return GB_ERR;

    /* 音频包不带 System Header 和 PSM：那两层每 20ms 重打一次纯属浪费带宽，
     * 惯例是每个视频关键帧（或每个视频帧）带一次即可，接收端记住就不放手。 */
    memcpy(out + off, GB_PS_PACK_HEADER, sizeof(GB_PS_PACK_HEADER));
    off += sizeof(GB_PS_PACK_HEADER);

    pes = gb_ps_write_pes_header(out + off, GB_PS_SID_AUDIO, len, pts90k, 0);
    if (pes < 0) return GB_ERR;
    off += (size_t)pes;

    memcpy(out + off, pcm_a, len);
    off += len;

    return (int)off;
}
