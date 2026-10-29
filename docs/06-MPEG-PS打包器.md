# 06 MPEG-PS 打包器

> **全项目最重要的一章。**
> 一句话：把一帧 H.264 和对应时刻的音频，按 MPEG-2 Program Stream 的格式
> 装进一个字节流，让接收端既拿到画面、又拿到"这帧该在第几秒播"。

---

## ① 这一步解决什么问题

编码器（x264 或硬件编码器）给你的是一坨**裸 H.264 字节**。这坨字节有两个问题：

**问题一：接收端不知道它的边界在哪。**
H.264 裸流（AnnexB 格式）靠起始码 `00 00 00 01` 分隔 NAL 单元，
但 NAL 有十几种类型（SPS、PPS、SEI、IDR slice、P slice…），
接收端得自己判断"哪几个 NAL 合成一帧"。

**问题二：接收端不知道每帧该在什么时刻播。**
裸流里没有时间戳。没有时间戳就只能"收到就播"，
网络一抖动画面就卡或跳。

**PS 容器同时解决这两个问题**：用 PES 包的边界划出帧，用 PTS 给出播放时刻。
而且 —— 这是国标选它的真正原因 —— **视频和音频的 PTS 走同一个 90kHz 时钟，
所以音画同步天然成立**（详见 [09 章](09-音画同步原理.md)）。

---

## ② 协议长什么样

### 四层结构

每一帧视频，设备都要打一个这样的包：

```
┌─────────────────────────────────────────────────────────────┐
│ ① Pack Header      14 字节    00 00 01 BA ...               │
│    整个 PS 流的开始，含系统时钟参考（SCR）                    │
├─────────────────────────────────────────────────────────────┤
│ ② System Header    16 字节    00 00 01 BB ...               │
│    声明码率上限和流数量                                       │
├─────────────────────────────────────────────────────────────┤
│ ③ PSM              20 字节    00 00 01 BC ...               │
│    Program Stream Map：声明"这条流里有 H.264，ES id 是 0xE0"  │
├─────────────────────────────────────────────────────────────┤
│ ④ PES              14 字节头 + 载荷                          │
│    00 00 01 E0 + 长度 + flags + PTS + [SPS/PPS/SEI/NAL]      │
└─────────────────────────────────────────────────────────────┘
```

音频帧自己一个 PES，也带自己的 Pack Header：

```
① Pack Header (14B) + ④ PES(音频) 14B头 + 160 字节 A-law
```

**注意：音频包不带 System Header 和 PSM。** 它们每帧重打一遍浪费带宽，
惯例是挂在每个视频关键帧上就够了。ZLMediaKit 解析时会记住上一次的 PSM。

### ① Pack Header 逐字节

```
00 00 01 BA           起始码（4 字节），BA = pack_start_code
44 00 04 00 04 01     '01' 标志(2位) + SCR(33位 + 3个 marker 位) —— 共 6 字节
86 66 CF F8           program_mux_rate(22位) + marker + reserved(5) + pack_stuffing_length(3)
```

**一共 14 字节**（4 + 6 + 4）。注意 `0x44` **不是一个独立字节**，
它是这 6 字节里的第一个：最高两位是 `01`，剩下 6 位属于 SCR。

**★ 那个 `0x44` 是关键。** 它的最高两位是 `01`，表示这是 **MPEG-2** 的 PS。
如果写成 `0x21`（最高两位 `00`），接收端会按 **MPEG-1** 的规则解析 PES 头 ——
而 MPEG-1 的 PES 头结构和 MPEG-2 完全不同（MPEG-2 多了 `flags` 那 3 个字节）。

> 顺带说一句为什么值得核对字节数：这一段的长度（14）和 PSM 的 `packet_length`（14）
> 是**同一个数字**，很容易把两处的数法混起来。数结构的时候一定要先问
> "我数的这一段，起点和终点分别在哪"。本项目 `steps/03_ps_pack.c` 里
> 把 pack header 和 `ffmpeg -f vob` 的真实输出逐字节比过，确认 14 字节一致。

**原项目在这里卡了很久**：写成 `0x21` 时 PES 头 `80 80 05` 会被错位解析，
现象是 ffprobe 报 `Invalid data`。**这块字节是"PES 解析模式的开关"。**

> 面试如果被问"PS 里有什么不显眼但关键的字段"，这就是最好的答案。

### ② System Header 逐字节

```
00 00 01 BB           起始码，BB = system_header_start_code
00 09                 ① 后续长度 = 9 字节
C3 33 67 00           码率上限（rate_bound）
21                    audio/video 流数量
FF                    video 流数量
E0 E0 E6 00           保留位 + 打包长度
```

**① `0x09` 是对的，但这一段有个"多一个字节"的 quirk，值得单独讲。**

先数一遍 9 是怎么来的。system_header 的固定域是 6 字节：

| 字段 | 字节 |
|---|---|
| `rate_bound`(22) + marker(1) | 3 |
| `audio_bound`(6) + `fixed_flag` + `CSPS_flag` | 1 |
| 两个 lock flag + marker + `video_bound`(5) | 1 |
| `packet_rate_restriction_flag` + reserved(7) | 1 |

再加上 1 条视频流条目 3 字节（`stream_id` 0xE0 + `'11'` + scale + 13 位 buffer size），
6 + 3 = **9** ✓

**但总长严格来说应该是 15 字节**（4 起始码 + 2 长度字段 + 9）。
而参考实现给的是 **16** 字节，最后多了一个 `0x00`。

**为什么多一个字节也能跑？**
因为解析器**不依赖长度字段走到下一段** —— 它是在字节流里扫描 `00 00 01`
这个起始码模式来重新同步的。长度字段写 9，解析器读完 9 字节后发现
还剩一个 `0x00`，继续往下扫，很快就撞上 PSM 的 `00 00 01 BC`，于是正常接上。

> **这个 quirk 值得记住，因为它说明一件事**：
> **协议里"多余的字节"往往被容错机制吃掉了，但这不等于结构是对的。**
> 多一个字节不会报错，少一个字节也不会报错（见下面 PSM 的那节）——
> 而你没法从现象上区分这两种情况。唯一可靠的办法是**逐字节数**。
>
> 本项目保留这 16 字节（它是实测通过的序列，改动收益为零），
> 但在 `gb_ps.h` 里把这个取舍写清楚了。
> 面试如果被问"你的实现和规范有差异吗"，这是一个很好的答案 ——
> 它证明你是**数过字节的**，而不是抄了一段能跑的代码。

### ③ PSM —— 面试最爱追问的地方

```
00 00 01 BC           起始码，BC = program_stream_map_start_code
00 0E                 ① ★ packet_length = 14
01                    current_next_indicator(1) + reserved(2) + version(5)
01                    reserved(7) + marker_bit(1)
00 00                 program_stream_info_length = 0
00 04                 ② ★ elementary_stream_map_length = 4
1B E0 00 00           ③ ★ stream_type=0x1B(H.264) + ES_id=0xE0 + ES_info_len=0
00 00 00 00           CRC_32（本项目写 0，接收端一般不校验）
```

**★ 追问最多的：`packet_length` 到底算哪些字节？**

答案：**从 `current_next_indicator` 开始，数到 `CRC_32` 结束。也就是
它不包含前面的 4 字节起始码 `00 00 01 BC`，也不包含它自己这 2 字节。**

数一遍（单视频版）：

| 字段 | 字节数 |
|---|---|
| `current_next_indicator` + reserved + version | 1 |
| reserved + `marker_bit` | 1 |
| `program_stream_info_length` | 2 |
| `elementary_stream_map_length` | 2 |
| 一条流条目（`stream_type` + `ES_id` + `ES_info_length`） | 4 |
| `CRC_32` | 4 |
| **合计** | **14 = 0x000E** ✓ |

所以 PSM 总长 = 4（起始码）+ 2（长度字段自身）+ 14 = **20 字节**。

**下游源码里的权威佐证。** ZLMediaKit 的 PS 解析用的是它 vendored 的 libmpeg，
在 `3rdpart/media-server/libmpeg/source/mpeg-psm.c` 的 `psm_read()` 里有两行：

```c
program_stream_map_length = mpeg_bits_read16(reader);
end = mpeg_bits_tell(reader) + program_stream_map_length;   /* 从 length 字段「之后」起算 */
...
element_stream_map_length = program_stream_map_length - program_stream_info_length - 10;
```

第二个式子里的常量 **10**，是 `psm_length` 里「**除 ES 条目之外**」的那部分：

| 构成 | 字节 |
|---|---|
| 标志位（`current_next`/version 1 字节 + reserved/`marker` 1 字节） | 2 |
| `program_stream_info_length` 字段自身 | 2 |
| `elementary_stream_map_length` 字段自身 | 2 |
| `CRC_32` | 4 |
| **合计** | **10** |

**注意这 10 里不含起始码** —— 因为 `end = mpeg_bits_tell(reader) + program_stream_map_length`
里的 `tell()` 已经在长度字段之后，说明 `psm_length` 的计量起点**本来就不含起始码**。
这正好从下游源码这一侧，反证了我们上面那条结论。

代入单视频版：`14 - 0 - 10 = 4`，正好是一条 ES 条目的 4 字节；
之后 `mpeg_bits_read32()` 再读掉 `CRC_32`，合计 6 + 14 = 20 字节 ✓

> （这里有个容易绕晕的地方：起始码 4 字节和 `CRC_32` 4 字节数值相同，
> 一不小心就会把 CRC 那份记到起始码头上去，得出"10 里含起始码"的错误分解。
> 数值巧合掩盖了结构差异 —— 判断依据只能是 `tell()` 的位置，不能靠凑数。）

**这是"我用下游实现反过来验证了我的理解"** —— 比只引用规范有说服力得多。
面试时如果被追问"你怎么确定 length 字段的口径"，这就是答案。

**含音频时**：多一条 4 字节条目，`elementary_stream_map_length` 变成 8，
`packet_length` 变成 18 = `0x0012`，PSM 总长 24 字节：

```
00 00 01 BC 00 12 01 01 00 00 00 08 1B E0 00 00 90 C0 00 00 00 00 00 00
                             └─H.264─┘ └G.711A┘
```

### 为什么 PSM 少了会「没有流」

看 ZLMediaKit 的源码（`3rdpart/media-server/libmpeg/source/mpeg-ps-dec.c`）：

```c
static struct pes_t* psm_fetch(struct psm_t* psm, uint8_t sid)
{
    for (i = 0; i < psm->stream_count; ++i)
        if (psm->streams[i].sid == sid) return &psm->streams[i];
    /* ... 只有在定义了 MPEG_GUESS_STREAM 宏时才会"猜" ... */
    return NULL;      /* ← 返回 NULL */
}
```

PES 包进来时按 `stream_id`（这里是 `0xE0`）去 PSM 里查：
**查不到就返回 NULL，这个 PES 被直接丢弃。**

现象是：设备在推流、包在网络上有、但平台 15 秒收流超时，`.video` dump 是 0 字节。
**完全看不出是 PSM 的问题** —— 直到逐字节数 PSM 长度才能发现。

> **原项目真实踩过**：Python 版 PSM 是 20 字节，C 移植时数组少写了 1 个 `0x00`，
> 变成 19 字节。长度字段声称 14 字节，实际只有 13 字节 → 长度错位 → 不建流。
> 这就是"少一个字节，全部功能失效"的典型案例。

### ④ PES 头逐字节

```
00 00 01 E0          起始码 + stream_id（E0 = 视频，C0 = 音频）
00 00                PES_packet_length
80                   ① flags1 = 0x80（'10' 标记在最前两位）
80 05                ② flags2 + PES_header_data_length = 5（PTS 占 5 字节）
<PTS 5 字节>          ③ 33 位 PTS 的位域编码（见下）
```

**① `flags1` 必须是 `0x80`。** 它的最高两位是 `10`（ISO 13818-1 规定），
写成 `0x10`（`00`）会让 ffprobe 报 `Invalid data`。
这一位错了不会崩，只是解析不了。

**② `PES_packet_length` 的语义要注意**：
它等于 `载荷长度 + 8`（8 = flags 3 字节 + PTS 5 字节）。
**超过 0xFFFF 时写 0** —— 因为 PES 长度字段只有 16 位，
而一帧 1080p 的 I 帧可能上百 KB。写 0 是约定俗成的"长度不限制"标记。

**③ PTS 的 5 字节位域** —— 见下一节，这是本项目抓出 bug 的地方。

### PTS 的位域编码

PTS 是 33 位，但要铺进 5 个字节，每个字节都要留标记位：

```
byte0 = 0010  P32 P31 P30  1        标记位在最低位
byte1 = P29 ... P22
byte2 = P21 ... P15  1              标记位在最低位
byte3 = P14 ... P7
byte4 = P6 ... P0  1                标记位在最低位
```

也就是：**33 位数据被拆成 3+8+7+8+7 = 33 位，中间插了 3 个标记位。**

C 代码：

```c
void gb_ps_encode_pts(uint8_t out[5], uint64_t pts90k)
{
    pts90k &= 0x1FFFFFFFFULL;                          /* 只保留 33 位 */
    out[0] = 0x21 | ((pts90k >> 29) & 0x0E);           /* 0010 P32P31P30 1 */
    out[1] = (pts90k >> 22) & 0xFF;                    /* P29..P22 */
    out[2] = 0x01 | ((pts90k >> 14) & 0xFE);           /* ★ P21..P15 1 —— 是 14！ */
    out[3] = (pts90k >> 7)  & 0xFF;                    /* P14..P7  */
    out[4] = 0x01 | ((pts90k & 0x7F) << 1);            /* P6..P0 1 */
}
```

### ★ 本项目修正的那个 bug

**原项目第 3 字节写的是 `(pts >> 15) & 0xFE`，正确是 `>> 14`。差一位。**

推导：

- `byte2` 的 bit1 到 bit7 应该依次放 `PTS[15]` 到 `PTS[21]`。
- `(pts >> 14)` 的 bit*k* 是 `PTS[14+k]` → `& 0xFE` 清掉 bit0 后，
  bit1 = `PTS[15]`，…，bit7 = `PTS[21]` ✓
- `(pts >> 15)` 的 bit*k* 是 `PTS[15+k]` → bit1 = `PTS[16]`，…，bit7 = `PTS[22]` ✗ 整体右移了一位

**为什么这个 bug 能潜伏几个月？**

因为它**不影响画面**。下游 ZLMediaKit 在多数路径上用 RTP 时间戳决定帧的播放时刻，
而不是 PES 里的 PTS。所以 PTS 算错了，画面照样流畅。

**"画面正常" ≠ "协议正确"。** 这句话是本项目存在的核心理由，
也是 [10-学习地图](00-学习地图.md) 里说的"验证驱动"的意思。

**怎么发现它？**

```bash
bash tools/verify_pts.sh
```

它打一个 PTS 已知的 `.ps` 文件，然后用 `ffprobe` **反读**回来，与写入值比对。
另外 `tests/test_pts.c` 会遍历一批 PTS 值做 **33 位往返解码校验** ——
自己写个解码函数按位域表还原，断言解出来的值等于输入值。
这比几个孤立测试向量有力得多。

---

## ③ 代码怎么长出来的

`gb_ps.c` 的结构很直白，四个函数：

```c
/* 1. 位域编码（纯函数，单测重点） */
void gb_ps_encode_pts(uint8_t out[5], uint64_t pts90k);

/* 2. 写 14 字节 PES 头 */
int gb_ps_write_pes_header(uint8_t out[14], uint8_t sid,
                           size_t payload_len, uint64_t pts90k, int idr);

/* 3. 视频：Pack + System + PSM + PES */
int gb_ps_pack_video(uint8_t *out, size_t cap,
                     const uint8_t *frame, size_t frame_len,
                     uint64_t pts90k, const uint8_t *psm, size_t psm_len);

/* 4. 音频：Pack + PES（不带 System/PSM） */
int gb_ps_pack_audio(uint8_t *out, size_t cap,
                     const uint8_t *pcm_a, size_t len, uint64_t pts90k);
```

固定头部做成 `const` 数组，**不每次现算**：

```c
const uint8_t GB_PS_PACK_HEADER[14] = {
    0x00,0x00,0x01,0xBA, 0x44, 0x00,0x04,0x00,0x04,0x01, 0x86,0x66,0xCF,0xF8
};
```

**为什么用常量而不是算？**
因为研究阶段这些字节是"试出来的"—— 原项目花了很久才确认 `0x44` 和 PSM 的
`0x0E`。把它们固化成常量并在注释里写清每个字节的含义，
比"用公式计算出来但没人看得懂"更有价值。
而且单元测试可以逐字节断言常量，比断言"算出来的值"更直接。

### 组帧：先攒 pending，撞到 slice 才发

一帧 H.264 通常长这样：

```
[SPS] [PPS] [SEI] [IDR slice]        ← 关键帧
[P slice]                             ← 普通帧
```

**只有 slice（类型 1 或 5）才代表"一帧的图像数据"。**
所以策略是：

```c
if (nal_type == 1 || nal_type == 5) {
    /* 撞到 slice → 一帧结束 */
    组 PS：Pack + System + PSM + PES(pending... + 这个 slice)
    pending_cnt = 0;         /* 清空 */
    发出去
} else {
    /* SPS/PPS/SEI/AUD → 先攒着，等下一个 slice */
    pending[pending_cnt++] = 当前 NAL;
}
```

这样 SPS/PPS/SEI 会和后面的 IDR 一起装进同一个 PES，
**这正是接收端建流所需要的**（它得先看到 SPS/PPS 才能初始化解码器）。

**原项目在这里还踩过一个坑**：libx264 多线程会把每帧切成 3 个 slice，
导致分帧逻辑把每个 slice 当成一帧，PTS 乱序，报 `DTS out of order`。
解法是编码时加 `-threads 1 -x264-params slices=1` 强制单 slice。

> 注意：**这是排障故事，不是面试考点。** 面试考的是"帧边界怎么判定"，
> 而不是"x264 的哪个参数叫什么名字"。

---

## ④ 跑哪些 kata 能看见

```bash
gcc -Wall -O2 -o /tmp/k02 steps/02_pts_encode.c && /tmp/k02   # PTS 位域
gcc -Wall -O2 -o /tmp/k03 steps/03_ps_pack.c    && /tmp/k03   # 完整 PS 逐字段
gcc -Wall -O2 -o /tmp/k04 steps/04_psm_layout.c && /tmp/k04   # PSM 长度字段
```

`04_psm_layout.c` 会把 PSM 的 20 个字节逐个数一遍，
让你亲眼确认 `packet_length = 总长 - 6`。

---

## ⑤ 怎么验证

```bash
# 1. 单元测试：PSM 常量、PTS 位域与往返解码、PES 长度
bash tools/selftest.sh

# 2. 闭环验证 PTS：打 .ps → ffprobe 反读 → 比对
bash tools/verify_pts.sh

# 3. ★ 负向对照：把 >>14 改回 >>15，看上面两招抓不抓得住
bash tools/verify_pts_mutation.sh

# 4. 手工看一眼字节（最强的手段）
./build/gb28181_device -c config.ini
# 日志里会打印 hexdump：
# [PS] 0000  00 00 01 ba 44 00 04 00 04 01 86 66 cf f8 00 00  |....D......f....|
# [PS] 0010  00 01 bb 00 09 c3 33 67 00 21 ff e0 e0 e6 00 ...  |.......3g.!.....|
```

**第 3 步（负向对照）是这个项目最有说服力的一环。** 它把源码复制到 `build/mutant/`，
**只**把那个位域改回错误的 `>>15`，然后重跑验证。结果：

```
单测抓住：  gb_ps_encode_pts(90000) = [21 00 03 BF 21], expected [21 00 05 BF 21]
ffprobe 抓住：写入 PTS=90000 → ffprobe 读回 pts=57232（差 -32768 ticks / -0.3641 秒）
```

**注意 `57232` 这个数**：它正是那一位错位之后被解码器还原出来的值 ——
比真实值小了 32768（= 2^15）。这就是"错一位"在数值上的样子。

面试时如果被问"你怎么证明你的测试有效"，这就是答案：
**不是"我的测试全绿"，而是"我把 bug 放回去，它当场红了"。**
"全绿"永远可能是"测试太弱"，负向对照才能排除这个可能。

**排障三件套**（这是原项目沉淀的方法论，非常值得记）：

| 手段 | 看什么 |
|---|---|
| `ZLM dumpDir` | ZLMediaKit 收到的**原始 PS**。文件是 0 字节 = 根本没解析出 track |
| `ffprobe <dump>` | 判断"是流本身坏了"还是"是 ZLM 特有的问题" |
| 按起始码扫描的脚本 | 逐字节核对第一帧结构，**排除推测，只信字节** |

---

## ⑥ 常见坑

| 坑 | 现象 | 定位 |
|---|---|---|
| Pack Header 首字节写成 `0x21` | ffprobe 报 `Invalid data` | PES 被按 MPEG-1 规则解析 |
| PSM 数组少一个字节 | 推流正常但收流超时 15 秒 | 数一遍 PSM 长度 |
| `packet_length` 把起始码算进去了 | 同上 | `packet_length = 总长 - 6` |
| PES `flags1` 写成 `0x10` | ffprobe 报 `Invalid data` | 最高两位必须是 `10` |
| PTS 位域错一位 | **画面正常但时间戳错**，极难发现 | 往返解码校验 + ffprobe 反读 |
| 多 slice 被当成多帧 | `DTS out of order`，不建流 | 编码时 `slices=1` |
| 音频 PES 也带 PSM | 浪费带宽，个别平台解析异常 | 音频只带 Pack Header |
| `PES_packet_length` 超 0xFFFF 没置 0 | 大 I 帧被截断 | 超了写 0 |

---

## ⑦ 面试怎么答

**问：GB28181 的 PS 封装是怎么做的？**

> "MPEG-2 Program Stream，每个视频帧打四层：
> Pack Header 14 字节，System Header 16 字节，PSM 20 字节，然后是 PES。
> PSM 是 Program Stream Map，作用是把 stream_type 和 elementary_stream_id
> 映射起来 —— 写 H.264 是 0x1B、ES id 0xE0。
> 接收端解析 PES 时按 stream_id 去 PSM 里查，查不到就丢弃这个包。
> 所以 PSM 里少写一个字节、长度字段对不上，整个流就建不起来，
> 但设备侧完全看不出问题，只会看到平台收流超时。"

**追问：PSM 的长度字段算哪些字节？**
→ 这是最能体现"你真数过"的问题，答：
   "从 current_next_indicator 数到 CRC_32，不含前面的 4 字节起始码，
   也不含长度字段自己。单视频是 14，含音频是 18。"

**追问：PTS 怎么编码的？**
→ "33 位拆成 3+8+7+8+7 铺进 5 个字节，中间插 3 个标记位。
   我们那份代码这里错过一位 —— 第 3 字节右移了 15 位，正确是 14。
   有意思的是这个错**不影响画面**，因为下游平台多数用 RTP 时间戳而不是
   PES PTS，所以潜伏了很久。最后是用往返解码校验 + 用 ffprobe 反读 PTS
   才抓出来的。"
   ← **这个回答的价值在于：它展示了"画面正常不等于协议正确"的判断力，
      以及"用工具反读验证"的方法论。比背公式强得多。**

**追问：为什么音频包不带 PSM？**
→ "PSM 和 System Header 声明的是整条流的构成，每帧重打浪费带宽。
   惯例是挂在每个关键帧上，接收端记住上一次的映射就够。"
   （可补一句：G.711A 的 stream_type 是 0x90、ES id 是 0xC0。）

---

## 下一步

→ [07 RTP 封装与推流](07-RTP封装与推流.md)
