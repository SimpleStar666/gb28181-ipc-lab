/*
 * kata 01 / 08 —— AnnexB 起始码切分 + 「跨包残留」
 * ==========================================================================
 * 【这一步解决什么问题】H.264 裸流是 [起始码][NAL][起始码][NAL]... 拼成的字节流，
 *   文件里没有「共几个 NAL」的清单，边界只能靠扫起始码找：
 *       3 字节起始码 00 00 01    /    4 字节起始码 00 00 00 01（前面多一个 0）
 *   设备端一帧 = 一个 slice NAL(type 1/5) + 挂在它前面的 SPS(7)/PPS(8)/SEI(6)，
 *   所以「哪几个 NAL 属于同一帧」必须先能正确切 NAL 才谈得上。
 * 【对应协议】ITU-T H.264 附录 B（B.1.1 byte_stream_nal_unit）AnnexB 字节流语法；
 *   GB/T 28181 附录 C 的 PS 流里，PES 载荷装的就是 AnnexB 形式的 H.264 NAL。
 * 【面试会怎么问 / 怎么答】
 *   Q1 怎么切 NAL？  A: 扫 00 00 01；若它前面还有一个 00，这个起始码其实是 4 字节的。
 *   Q2 数据正好切在 NAL 中间怎么办？  A: 后面找不到下一个起始码时，这个 NAL 的长度
 *      无法确定，必须 memmove 到缓冲头部留到下一包（本 kata 第 2 段演的就是这个）；
 *      丢掉尾巴 → 该 NAL 直接消失 → 花屏/丢帧，裸流解析最经典的一类 bug。
 *   Q3 取 NAL 类型能写死 p[3] 吗？  A: 不能。4 字节起始码时 NAL 头在 p[4]，
 *      写死 p[3] 会把起始码末尾的 0x01 当成 NAL 头，读出「类型 1」这种假结果。
 */
#include <stdio.h>
#include <string.h>

#define CAP 256

/* 测试数据：故意把 IDR(类型 5) 从中间切成两段；注意载荷末尾都不会是 0x00（见文末陷阱） */
static const unsigned char part1[] = {
    0x00,0x00,0x00,0x01, 0x67,0x42,0xC0,0x1F,0xDA,0x01,0x40,0x16,0xEC,0x04,0x40,0xAA, /* SPS 类型7，起始码4字节，共16字节 */
    0x00,0x00,0x01,      0x68,0xCE,0x3C,0x80,                                           /* PPS 类型8，共 7 字节 */
    0x00,0x00,0x01,      0x06,0x05,0xFF,0x80,                                           /* SEI 类型6，共 7 字节 */
    0x00,0x00,0x01,      0x65,0x88,0x84,0x00,0x10,0x20,0x30,0x40                        /* IDR 类型5 …… 此处被切断！ */
};
static const unsigned char part2[] = {
    0x50,0x60,0x70,0x80,0x90,0xA0,0xB0,0xC0,                        /* IDR 后半段（拼回共 19 字节） */
    0x00,0x00,0x00,0x01, 0x41,0x9A,0x00,0x11,0x22                   /* 普通 slice 类型1，共 9 字节 */
};

static unsigned char buf[CAP];      /* 滚动缓冲：只装「还没消费完」的字节 */
static size_t used = 0;
static int nals_total = 0, nseen = 0;
static int sc_seen[8], type_seen[8], len_seen[8];

static void hexdump(const char *tag, const unsigned char *d, size_t n)
{
    size_t i, j;
    printf("---- %s（%zu 字节）----\n", tag, n);
    for (i = 0; i < n; i += 16) {
        printf("%04zx  ", i);
        for (j = 0; j < 16; j++) { if (i + j < n) printf("%02x ", d[i + j]); else printf("   "); }
        printf(" |");
        for (j = 0; j < 16 && i + j < n; j++) printf("%c", (d[i + j] >= 0x20 && d[i + j] < 0x7F) ? d[i + j] : '.');
        printf("|\n");
    }
}

/* 从 from 开始找下一个起始码：返回位置，*sc = 起始码长度(3/4)；找不到返回 -1 */
static int find_sc(const unsigned char *d, size_t len, size_t from, size_t *sc)
{
    size_t j;
    for (j = from; j + 3 <= len; j++) {
        if (d[j] == 0 && d[j+1] == 0 && d[j+2] == 1) { *sc = 3; return (int)j; }
        if (j + 4 <= len && d[j] == 0 && d[j+1] == 0 && d[j+2] == 0 && d[j+3] == 1) { *sc = 4; return (int)j; }
    }
    return -1;
}

static const char *nal_name(int t)
{
    switch (t) {
    case 1:  return "非IDR slice";
    case 5:  return "IDR slice";
    case 6:  return "SEI";
    case 7:  return "SPS";
    case 8:  return "PPS";
    default: return "其他";
    }
}

static void emit(const unsigned char *nal, size_t len, size_t sc)
{
    int t = nal[sc] & 0x1F;   /* NAL 头紧跟在起始码之后 → 必须用 sc 取，不能写死 3 */
    nals_total++;
    if (nseen < 8) { sc_seen[nseen] = (int)sc; type_seen[nseen] = t; len_seen[nseen] = (int)len; nseen++; }
    printf("  >> NAL #%d  起始码=%zu 字节  类型=%d(%-11s) 长度=%zu 字节  NAL头=0x%02x\n",
           nals_total, sc, t, nal_name(t), len, nal[sc]);
}

static void feed(const unsigned char *chunk, size_t n, const char *label)
{
    size_t pos = 0, sc1, sc2;
    int f1, f2;

    printf("\n[喂入] %s：追加 %zu 字节（缓冲里原有 %zu 字节）\n", label, n, used);
    if (used + n > (size_t)CAP) { printf("  测试数据超过缓冲 %d 字节\n", CAP); return; }
    memcpy(buf + used, chunk, n);
    used += n;

    for (;;) {
        f1 = find_sc(buf, used, pos, &sc1);
        if (f1 < 0) {  /* 完全没有起始码：丢垃圾，但尾部最多留 3 字节（可能是被切断的起始码） */
            size_t keep = used - pos;
            if (keep > 3) keep = 3;
            if (used - pos > keep)
                printf("  [丢垃圾] 丢弃 %zu 字节非起始码数据，尾部保留 %zu 字节\n", used - pos - keep, keep);
            pos = used - keep;
            break;
        }
        f2 = find_sc(buf, used, (size_t)f1 + sc1, &sc2);
        if (f2 < 0) {  /* 后面没有起始码 → 这个 NAL 还没结束、长度未知 → 必须留着 */
            printf("  [残留] 偏移 %d 起的 NAL 还不完整（缺下一个起始码），%zu 字节留在缓冲等下一包\n",
                   f1, used - (size_t)f1);
            pos = (size_t)f1;
            break;
        }
        emit(buf + f1, (size_t)f2 - (size_t)f1, sc1);
        pos = (size_t)f2;
    }
    if (pos > 0) { memmove(buf, buf + pos, used - pos); used -= pos; }
    printf("  [缓冲] 本轮结束还剩 %zu 字节未消费（这就是「尾巴」）\n", used);
}

static void flush(void)
{
    size_t sc = 0;
    int f1;
    printf("\n[流结束 flush] 缓冲区还剩 %zu 字节\n", used);
    f1 = find_sc(buf, used, 0, &sc);
    if (f1 >= 0) emit(buf + f1, used - (size_t)f1, sc);
    else         printf("  没有起始码，丢弃 %zu 字节\n", used);
    used = 0;
}

int main(void)
{
    static const size_t sc_exp[5]  = {4, 3, 3, 3, 4};
    static const int    ty_exp[5]  = {7, 8, 6, 5, 1};
    static const size_t len_exp[5] = {16, 7, 7, 19, 9};
    const char *nm[5] = {"SPS", "PPS", "SEI", "IDR slice", "非IDR slice"};
    unsigned char trap[8] = {0xAA,0x00,0x00,0x00,0x01,0x68,0xCE,0x3C};
    char reason[200] = "";
    size_t residue1, tsc = 0;
    int fail = 0, i, tp;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=========== kata 01：AnnexB 起始码切分与跨包残留 ===========\n");

    hexdump("第 1 段数据 part1", part1, sizeof part1);
    feed(part1, sizeof part1, "第 1 段（故意切在 IDR 中间）");
    residue1 = used;
    hexdump("第 2 段数据 part2", part2, sizeof part2);
    feed(part2, sizeof part2, "第 2 段（IDR 后半段 + 一个新 NAL）");
    flush();

    printf("\n--------------- 自检 ---------------\n");
    printf("起始码长度：期望 4/3/3/3/4   实际 %d/%d/%d/%d/%d\n", sc_seen[0], sc_seen[1], sc_seen[2], sc_seen[3], sc_seen[4]);
    printf("NAL 类型　：期望 7/8/6/5/1   实际 %d/%d/%d/%d/%d\n", type_seen[0], type_seen[1], type_seen[2], type_seen[3], type_seen[4]);
    printf("NAL 长度　：期望 16/7/7/19/9 实际 %d/%d/%d/%d/%d\n", len_seen[0], len_seen[1], len_seen[2], len_seen[3], len_seen[4]);
    printf("IDR 分两包喂入：第 1 包留尾巴 %zu 字节，第 2 包拼回 %d 字节完整 NAL → 残留机制生效\n", residue1, len_seen[3]);
    printf("反例：SPS 用 4 字节起始码，写死 p[3] 取类型会得到 0x%02x（类型 %d）——错的，必须用 p[sc_len]\n",
           part1[3], part1[3] & 0x1F);

    tp = find_sc(trap, sizeof trap, 0, &tsc);
    printf("\n[陷阱] 上一个 NAL 载荷末尾是 0x00、下一个 NAL 用 3 字节起始码时：\n");
    printf("  AA 00 00 00 01 68 CE 3C → 第 1 个起始码落在偏移 %d、长度 %zu（被当成 4 字节），\n", tp, tsc);
    printf("  于是 0xAA 那个 NAL 少算 1 字节。真实码流靠「NAL 内不得出现 00 00 0x 序列」（emulation prevention 0x03）避开此坑。\n");

    if (nseen != 5) { fail = 1; snprintf(reason, sizeof reason, "切出的 NAL 数量是 %d，应为 5", nseen); }
    for (i = 0; i < 5 && !fail; i++) {
        if ((size_t)sc_seen[i] != sc_exp[i] || type_seen[i] != ty_exp[i] || (size_t)len_seen[i] != len_exp[i]) {
            fail = 1;
            snprintf(reason, sizeof reason, "第 %d 个 NAL(%s) 期望 起始码%zu/类型%d/长度%zu，实际 %d/%d/%d",
                     i + 1, nm[i], sc_exp[i], ty_exp[i], len_exp[i], sc_seen[i], type_seen[i], len_seen[i]);
        }
    }
    if (!fail && residue1 != 11) {
        fail = 1;
        snprintf(reason, sizeof reason, "第 1 包喂完应残留 11 字节（半个 IDR），实际 %zu", residue1);
    }
    if (fail) { printf("SELF-CHECK FAIL: %s\n", reason); return 1; }
    printf("SELF-CHECK PASS\n");
    return 0;
}
