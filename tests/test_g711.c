/*
 * test_g711.c — G.711 A-law 编解码
 * ============================================================
 * A-law 是 8bit 对数压缩：16bit 线性 PCM 经量化后只剩 256 个码字，
 * 所以「往返等于原值」是**不成立**的 —— 断言那一条的人一定写错了测试。
 * 正确的判据是：
 *
 *   1. 量化误差有界：误差必须落在该段的量化步长之内
 *      （这里用 |err| <= |x|/4 + 32，A-law 的步长是分段倍增的，
 *       这个上界在所有段上都成立，同时又能拦住「符号搞错」「段号算错」这类硬错）。
 *   2. 已知点核对（ITU-T G.711 的 A-law 约定）：
 *        正满量程 = 0xAA、负满量程 = 0x2A
 *        正零 = 0xD5、负零 = 0x55
 *        最高位是符号位：1 = 正、0 = 负
 *   3. 结构性恒等式：encode(decode(a)) == a 对全部 256 个码字成立
 *      —— 这是 A-law 码表最重要的自洽性，比逐点比对更不容易漏。
 *   4. 覆盖性：线性域扫一遍必须能产生全部 256 个码字（不能有不可达的码）。
 *   5. gb_g711a_encode_buf 与逐样本 encode 结果一致，且遵守 out_cap。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "gb_g711.h"

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL: ");                                               \
            printf(__VA_ARGS__);                                            \
            fflush(stdout);                                                 \
            return 1;                                                       \
        }                                                                   \
    } while (0)

int main(void)
{
    int i;
    int16_t x;

    /* ================= 1. 已知点：零与满量程 ================= */
    CHECK(gb_g711a_encode(0) == 0xD5,
          "encode(0) = 0x%02X, expected 0xD5（A-law 正零）"
          "   <- 若得到 0x55 说明符号位约定反了\n",
          gb_g711a_encode(0));
    CHECK(gb_g711a_encode(-1) == 0x55,
          "encode(-1) = 0x%02X, expected 0x55（A-law 负零）\n",
          gb_g711a_encode(-1));

    CHECK(gb_g711a_decode(0xD5) >= 0 && gb_g711a_decode(0xD5) <= 8,
          "decode(0xD5) = %d, expected 0..8（正零附近）\n", gb_g711a_decode(0xD5));
    CHECK(gb_g711a_decode(0x55) <= 0 && gb_g711a_decode(0x55) >= -8,
          "decode(0x55) = %d, expected -8..0（负零附近）\n", gb_g711a_decode(0x55));

    CHECK(gb_g711a_decode(0xAA) >= 30000,
          "decode(0xAA) = %d, expected 接近正满量程 (>=30000)\n", gb_g711a_decode(0xAA));
    CHECK(gb_g711a_decode(0x2A) <= -30000,
          "decode(0x2A) = %d, expected 接近负满量程 (<=-30000)\n", gb_g711a_decode(0x2A));

    /* ================= 2. 符号位约定：全 256 个码字 ================= */
    for (i = 0; i < 256; i++) {
        int v = gb_g711a_decode((uint8_t)i);
        if ((i & 0x80) != 0) {
            CHECK(v >= 0, "decode(0x%02X) = %d, 最高位为 1（正）时解码值必须 >= 0\n", i, v);
        } else {
            CHECK(v <= 0, "decode(0x%02X) = %d, 最高位为 0（负）时解码值必须 <= 0\n", i, v);
        }
    }

    /* ================= 3. 结构性恒等式：encode(decode(a)) == a ================= */
    {
        int bad = 0;
        for (i = 0; i < 256; i++) {
            uint8_t a = (uint8_t)i;
            int16_t lin = gb_g711a_decode(a);
            uint8_t back = gb_g711a_encode(lin);
            if (back != a) {
                if (bad < 8)
                    printf("  encode(decode(0x%02X)) = 0x%02X (decode=%d)\n", a, back, lin);
                bad++;
            }
        }
        CHECK(bad == 0,
              "有 %d/256 个码字不满足 encode(decode(a)) == a"
              "   <- 编码表与解码表不互逆（两侧段号/量化位算法不一致）\n", bad);
    }

    /* ================= 4. 往返误差有界（线性域全程扫描） ================= */
    {
        long worst = 0;
        int worst_x = 0;
        for (x = -32768; x < 32767; x = (int16_t)(x + 13)) {  /* 步进 13，快速扫全程 */
            int v = gb_g711a_decode(gb_g711a_encode(x));
            long err = (long)v - (long)x;
            long bound = (x < 0 ? -(long)x : (long)x) / 4 + 32;
            if (err < 0)
                err = -err;
            if (err > worst) {
                worst = err;
                worst_x = x;
            }
            if (err > bound) {
                printf("  x=%d -> %d, 误差 %ld 超出上界 %ld\n", x, v, (long)err, bound);
                CHECK(0, "A-law 往返误差超出量化步长上界 (x=%d)\n", x);
            }
        }
        printf("  [info] 最大往返误差 %ld（出现在 x=%d，A-law 分段量化所致）\n",
               worst, worst_x);
    }

    /* 几个具体量级上的往返（含 0 与满量程附近的饱和行为） */
    {
        static const int16_t pts[] = {
            0, 1, -1, 8, -8, 100, -100, 1000, -1000, 8000, -8000,
            16000, -16000, 30000, -30000, 32767, -32768
        };
        size_t k;
        for (k = 0; k < sizeof(pts) / sizeof(pts[0]); k++) {
            int16_t in = pts[k];
            int out = gb_g711a_decode(gb_g711a_encode(in));
            long err = (long)out - (long)in;
            long bound = (in < 0 ? -(long)in : (long)in) / 4 + 32;
            if (err < 0)
                err = -err;
            CHECK(err <= bound,
                  "x=%d -> %d 误差 %ld 超出上界 %ld\n", in, out, (long)err, bound);

            /* 大信号必须保持符号 */
            if (in > 256)
                CHECK(out > 0, "正信号 %d 往返后变成 %d（符号丢失）\n", in, out);
            if (in < -256)
                CHECK(out < 0, "负信号 %d 往返后变成 %d（符号丢失）\n", in, out);
        }
    }

    /* ================= 5. 覆盖性：256 个码字全部可达 ================= */
    {
        int seen[256];
        int codes = 0;
        memset(seen, 0, sizeof(seen));
        for (x = -32768; x < 32767; x = (int16_t)(x + 1)) {
            int c = gb_g711a_encode(x);
            if (!seen[c]) {
                seen[c] = 1;
                codes++;
            }
        }
        if (codes != 256) {
            printf("  缺失的码字:");
            for (i = 0; i < 256; i++)
                if (!seen[i])
                    printf(" %02X", i);
            printf("\n");
        }
        CHECK(codes == 256, "线性域扫完后只产生了 %d/256 个 A-law 码字\n", codes);
    }

    /* ================= 6. encode_buf：与逐样本一致 + 遵守 out_cap ================= */
    {
        int16_t pcm[GB_AUDIO_FRAME_LEN];
        uint8_t out[GB_AUDIO_FRAME_LEN];
        size_t n, k;

        for (k = 0; k < GB_AUDIO_FRAME_LEN; k++) {
            /* 方波 ±16000（模拟麦克风采集的一帧，G.711A 是纯幅度压缩，与波形无关） */
            pcm[k] = (int16_t)((((k * 440) % 8000) < 4000) ? 16000 : -16000);
        }

        memset(out, 0, sizeof(out));
        n = gb_g711a_encode_buf(pcm, GB_AUDIO_FRAME_LEN, out, sizeof(out));
        CHECK(n == GB_AUDIO_FRAME_LEN,
              "encode_buf 返回 %zu, expected %zu\n", n, (size_t)GB_AUDIO_FRAME_LEN);
        for (k = 0; k < GB_AUDIO_FRAME_LEN; k++) {
            uint8_t want = gb_g711a_encode(pcm[k]);
            if (out[k] != want) {
                printf("  [%zu] 0x%02X vs encode(0x%04X)=0x%02X\n", k, out[k], (uint16_t)pcm[k], want);
                CHECK(0, "encode_buf 第 %zu 个样本与 gb_g711a_encode 不一致\n", k);
            }
        }

        /* out_cap 小于样本数时不能越界 */
        {
            uint8_t small[8];
            memset(small, 0xA5, sizeof(small));
            n = gb_g711a_encode_buf(pcm, GB_AUDIO_FRAME_LEN, small, sizeof(small));
            CHECK(n <= sizeof(small), "out_cap=8 时 encode_buf 返回 %zu（越界）\n", n);
            for (k = 0; k < sizeof(small); k++) {
                uint8_t want = (k < n) ? gb_g711a_encode(pcm[k]) : 0xA5;
                CHECK(small[k] == want,
                      "out_cap=8 时 small[%zu] = 0x%02X, expected 0x%02X\n", k, small[k], want);
            }
        }
    }

    printf("PASS: G.711A 已知点(0->0xD5/-1->0x55/满量程 0xAA,0x2A) 与符号约定正确；"
           "全部 256 码字满足 encode(decode(a))==a；往返误差在量化步长内\n");
    return 0;
}
