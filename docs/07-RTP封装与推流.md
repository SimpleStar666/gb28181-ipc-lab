# 07 RTP 封装与推流

> **一句话**：PS 打好的一帧可能有 200 KB，一个 UDP 包最多装一千多字节，
> 所以要切开。RTP 就是管"怎么切、怎么让接收端拼回来"的那一层。

---

## ① 这一步解决什么问题

打成 PS 之后，你手上是一个几十 KB 到几百 KB 的字节块。
UDP 有 MTU 限制（以太网 1500 字节，减去 IP 20 + UDP 8，实际可用 1472），
所以必须切。

但切开之后接收端要面对三个问题：

1. **怎么知道这些包属于同一帧？** → 时间戳
2. **怎么知道这帧发完了？** → marker 位
3. **丢了一个包怎么办？** → 序号（检测）+ 上层容错

RTP 用 12 个字节解决这三件事。

---

## ② 协议长什么样

### RTP 头 12 字节

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|X|  CC   |M|     PT      |       sequence number         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           timestamp                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                             SSRC                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| 字段 | 位宽 | 本项目取值 | 说明 |
|---|---|---|---|
| **V** | 2 | 2 | 版本，固定 2 |
| **P** | 1 | 0 | Padding，有填充字节时置 1 |
| **X** | 1 | 0 | Extension，有扩展头时置 1 |
| **CC** | 4 | 0 | CSRC 计数，没有贡献源时为 0 |
| **M** | 1 | 见下 | **Marker**，标记"这是本帧最后一个包" |
| **PT** | 7 | 96 | Payload Type，动态类型 |
| **sequence** | 16 | 随机起始 | 包序号，每包 +1，回绕到 0 |
| **timestamp** | 32 | = PTS | 90kHz 时钟下的采样时刻 |
| **SSRC** | 32 | 来自 `y=` | 同步源标识 |

第一字节就是 `V<<6 | P<<5 | X<<4 | CC` = `10 0 0 0000` = **`0x80`**。

### ★ 两个最常写错的细节

**细节一：marker 位在第 1 字节的最高位。**

```c
rtp[1] = pt | (last ? 0x80 : 0);      /* ✓ 正确 */
rtp[1] = pt | (last ? 0x01 : 0);      /* ✗ 错误！ */
```

写成 `| 1` 的话，`96 | 1 = 97` —— **PT 变成 97**，接收端按错误的载荷类型解，
解码直接崩。原项目就是这个错，现象是 ffmpeg 报 `mb_type 384`。

**细节二：同一帧的所有分片，timestamp 必须完全相同。**

```c
/* 一帧 3000 字节 → 3 个分片 */
分片0: seq=N   ts=T   M=0
分片1: seq=N+1 ts=T   M=0
分片2: seq=N+2 ts=T   M=1      ← 只有最后一片置 marker
```

**timestamp 只在换帧时递增**（视频每帧 +6000），**不是每个包都加**。
如果每个包都加，接收端会认为这是 3 个不同时刻的帧，播放出来就是卡顿或跳帧。

### 分片大小与协议开销

```
MTU 1400 (本项目取的安全值)
  + 12  RTP 头
  + 8   UDP 头
  + 20  IP 头
  ────
  1440  实际报文长度 < 1500 ✓
```

**协议开销 = 40 字节/包**（IP 20 + UDP 8 + RTP 12）。
这个数字在估算带宽时很有用：如果每秒发 100 个包，光协议头就吃掉 4 KB/s。

> 这个 `40B/包` 是原项目抓包时实测出来的（722 个 RTP 包），
> 不是从文档抄的 —— 抓包实测和文档互相印证，这才是"做过"的样子。

---

## ③ 代码怎么长出来的

`gb_rtp.c` 分成纯函数和 IO 两层：

```c
/* --- 纯函数层（可单测） --- */
void   gb_rtp_build_header(uint8_t hdr[12], uint16_t seq, uint32_t ts,
                           uint32_t ssrc, uint8_t pt, int marker);
int    gb_rtp_slice(const uint8_t *ps, size_t ps_len, size_t max_payload,
                    gb_rtp_slice_t slices[], int max_slices);
size_t gb_rtp_pack(uint8_t *out, size_t out_cap, uint16_t seq, uint32_t ts,
                   uint32_t ssrc, uint8_t pt, int marker,
                   const uint8_t *payload, size_t payload_len);

/* --- IO 层 --- */
int    gb_rtp_send_ps(int fd, const void *dst, unsigned dst_len,
                      gb_rtp_tx_t *tx, const uint8_t *ps, size_t ps_len);
```

**为什么要拆开？**
因为 `gb_rtp_slice` 和 `gb_rtp_build_header` 的行为可以用单元测试逐字节断言，
而 `gb_rtp_send_ps` 需要真 socket。
把"算字节"和"发字节"分开，前者就能离线反复验证。

### 分片逻辑

```c
int gb_rtp_slice(const uint8_t *ps, size_t ps_len, size_t max_payload,
                 gb_rtp_slice_t slices[], int max_slices)
{
    int n = 0;
    size_t off = 0;
    while (off < ps_len) {
        if (n >= max_slices) return GB_ERR;
        size_t piece = (ps_len - off > max_payload) ? max_payload : (ps_len - off);
        slices[n].payload = ps + off;
        slices[n].len     = piece;
        slices[n].marker  = (off + piece >= ps_len);   /* 只有最后一片 */
        off += piece;
        n++;
    }
    return n;
}
```

**注意 `off + piece >= ps_len` 这个判断**，写成 `>` 就会在正好整除时漏掉 marker。

### 发送：序号在这里推进

```c
int gb_rtp_send_ps(int fd, const void *dst, unsigned dst_len,
                   gb_rtp_tx_t *tx, const uint8_t *ps, size_t ps_len)
{
    gb_rtp_slice_t slices[GB_RTP_MAX_SLICES];
    int n = gb_rtp_slice(ps, ps_len, tx->max_payload, slices, GB_RTP_MAX_SLICES);
    if (n < 0) return GB_ERR;

    uint8_t buf[12 + 1500];
    for (int i = 0; i < n; i++) {
        size_t total = gb_rtp_pack(buf, sizeof(buf), tx->seq, tx->ts_cur,
                                   tx->ssrc, tx->pt, slices[i].marker,
                                   slices[i].payload, slices[i].len);
        if (total) {
            sendto(fd, buf, total, 0, (const struct sockaddr *)dst, dst_len);
            tx->seq++;                 /* ★ 只有发送时才推进序号 */
        }
    }
    return n;
}
```

**序号在发送层推进，不在切片层推进。** 这样重发、丢弃、单元测试都不会搞乱序号状态。

### 序号回绕

`seq` 是 `uint16_t`，到 65535 之后自然回绕到 0（C 的无符号整型就是模 2^16），
不需要特殊处理。这正是 RTP 规范期望的行为。

---

## ④ 跑哪个 kata 能看见

```bash
gcc -Wall -O2 -o /tmp/k05 steps/05_rtp_slice.c && /tmp/k05
```

它会拿 3000 字节载荷按 MTU=1400 切，打印每一片的
`seq / marker / timestamp`，并断言：

- 切出 3 片
- **只有最后一片 marker = 1**
- **3 片的 timestamp 完全相同**

```bash
gcc -Wall -O2 -o /tmp/k08 steps/08_rtp_loopback.c && /tmp/k08
```

这个更直观：本机 UDP 环回，一头发一头收，把收到的包的
`seq / ts / marker` 打出来，你会看到 `M=0 M=0 M=1 M=0 M=0 M=1 ...` 的节奏。

---

## ⑤ 怎么验证

```bash
bash tools/selftest.sh          # 含 test_rtp.c：header 逐字节 + 分片断言

# 真实链路上用 tcpdump 核对
sudo tcpdump -i any -c 10 -nn -X udp port 11000
```

看第一个包的字节：

```
0x0000:  8060 41f2 0001 5b3c 1234 5678 0000 01ba   .`A...[<.4x....
         │  │  │    │         │         └── 载荷以 00 00 01 BA 开头 = PS Pack Header ✓
         │  │  │    │         └── SSRC = 0x12345678
         │  │  │    └── timestamp（= 该帧 PTS）
         │  │  └── 序号
         │  └── byte1 = 0x60 → M=0, PT=0x60&0x7F=96 ✓
         └── byte0 = 0x80 → V=2, P=0, X=0, CC=0 ✓
```

**学会读 `byte1` 是排障的基本功**：它同时编码了 marker 和 PT。
`PT=96` 的二进制是 `110 0000`，占低 7 位，最高位留给 marker。

| byte1 | M | PT | 判定 |
|---|---|---|---|
| `0x60` | 0 | 96 | 正确，本帧的非末片 |
| `0xE0` | 1 | 96 | 正确，本帧的末片 |
| `0x61` | 0 | 97 | ✗ **marker 写到 bit0 了**（原项目的 bug） |
| `0xE1` | 1 | 97 | ✗ 同上 |

`0x61` 这个值看起来"只差一点"，但接收端会按 PT=97 去查表，
查不到就按未知类型处理，解码直接失败。
**抓到包先看 byte1，是 `0x60`/`0xE0` 就对了，出现 `0x61` 立刻回头查代码。**

---

## ⑥ 常见坑

| 坑 | 现象 | byte1 长什么样 |
|---|---|---|
| marker 写进 bit0 | ffmpeg 报 `mb_type` 解码崩 | `0x61` |
| 每个分片 timestamp 都 +6000 | 播放卡顿/跳帧 | ts 正常但节奏错 |
| 分片超过 MTU | 丢包率高，大包被路径 MTU 分片 | — |
| 每帧都用新 SSRC | 接收端认为流一直在切换 | SSRC 字段在跳 |
| RTP 头和 PS 载荷顺序颠倒 | 接收端找不到 `00 00 01 BA` | — |
| 序号在切片层推进 | 重发/测试时分片序号断档 | — |

---

## ⑦ 面试怎么答

**问：RTP 层你是怎么做的？**

> "一帧 PS 按 MTU 切片，每个包加 12 字节 RTP 头。
> 头里最关键是三个字段：序号每包递增，用来检测丢包；
> 时间戳取该帧的 PTS，90kHz 时钟下视频每帧 +6000；
> marker 位只在本帧最后一个包上置 1，接收端靠它判断帧边界。
> 同一帧切出来的所有包时间戳必须相同 —— 这点写错就会变成'
> 接收端以为这是多帧'，播放出来卡顿。"

**追问：marker 位在哪个字节？**
→ "第 1 字节的最高位。所以代码是 `pt | (last ? 0x80 : 0)`。
   这里我们踩过坑：写成 `| 1` 了，结果 PT 从 96 变成 97，
   接收端按错误载荷类型解，解码直接崩。抓包看 byte1 是 `0x61` 而不是 `0x60`
   就能立刻定位。"

**追问：为什么要切片，不能一个包发完？**
→ "以太网 MTU 1500，减去 IP 20 + UDP 8 + RTP 12，实际载荷上限大概 1460。
   一帧 1080p 的 I 帧可能上百 KB，必须切。
   切太小了协议开销占比高 —— 每包固定 40 字节头，包越小浪费越大。"

**追问：丢包了怎么办？**
→ "RTP 层本身不重传。接收端靠序号发现丢包，
   国标这条链路上通常靠上层（平台侧）做丢包隐藏。
   跟 WebRTC 比，国标没有 NACK/PLI 那套反馈机制 ——
   这也是为什么 WebRTC 在弱网下体验更好。"

---

## 下一步

→ [08 实时流与音频轨](08-实时流与音频轨.md)
