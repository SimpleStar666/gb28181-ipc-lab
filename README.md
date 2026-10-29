# GB28181 IPC 设备端实战课

**从零讲清一台网络摄像机是怎么把画面送到平台上的。**

这个仓库是一个**可运行、可验证**的 GB28181 设备端（IPC 侧）实现，配套 13 章讲义。
它不是"能跑就行"的示例代码 —— 每一处协议字节都有单元测试盯着，每一处设计选择都在讲义里讲了"为什么"。

---

## 这个仓库为什么存在

原项目（[AV_Study](https://github.com/SimpleStar666/AV_Study) 里的那 795 行 `gb28181_device.c`）
功能是**真的跑通了**：注册、点播、推流、平台出画面，全链路验过。它的问题不在"能不能跑"，在三件事：

| 问题 | 后果 |
|---|---|
| 一个文件塞了 SIP 鉴权、PS 打包、RTP 封装、AnnexB 切帧 | 看不出哪段代码对应协议里哪个字段，学完记不住 |
| 零测试 | 有一处位域写错了一位，画面照样正常，几个月没人发现 |
| 缺音频轨、缺配置、缺构建系统 | 与真实 IPC 的形态差一截，被追问就露馅 |

本仓库在这套已经验证过的协议知识上做三件事：
**按协议分层拆开** → **补上缺的（音频轨、配置、构建、交叉编译）** → **用测试把"看不见的错误"钉死**。

---

## 学完你能回答什么

这个项目直接对着「IPC / 安防嵌入式音视频」岗位的 JD 写。学完之后下面每一条你都能讲出**链路 + 原理 + 你踩过的坑**：

| JD 常见要求 | 对应章节 | 你能讲什么 |
|---|---|---|
| IPC 方案设计、开发、调试 | 01 协议全景 | 设备端要跑哪几个进程/线程，各自负责什么 |
| C/C++ 指针、内存、多线程 | 08 实时流 | 滚动缓冲为什么必须保留尾部残留；`pos > 0` 那个 bug |
| Linux 嵌入式 / 交叉编译 | 10 交叉编译 | `-static` 为什么是嵌入式默认；QEMU 用户态验证的边界在哪 |
| H.264/H.265 编码基础 | 06 PS 打包 | NAL 类型、AnnexB 起始码、IDR、为什么单 slice |
| RTSP / RTP / ONVIF / WebRTC 之一 | 07 RTP 封装 | 12 字节 RTP 头逐字段、marker 位为什么在第 1 字节 |
| 音视频流水线调试：花屏 / 卡顿 / 音画不同步 | 09 音画同步 + 11 排障手册 | PS 的 PTS 是唯一同步依据；花屏/卡顿/不同步三分类的排查顺序 |

---

## 8 步路线图

每一步都有：讲义（为什么）→ 可独立运行的 kata（看字节）→ 主程序里的对应模块 → 验收命令。

| 步 | 主题 | 讲义 | 独立 kata | 主程序模块 |
|---|---|---|---|---|
| 1 | GB28181 全景：信令 / 容器 / 传输三层 | [01](docs/01-协议全景.md) | — | — |
| 2 | 环境：WSL + wvp-pro + ZLMediaKit | [02](docs/02-环境搭建.md) | — | — |
| 3 | SIP 注册与 Digest 鉴权 | [03](docs/03-SIP注册与Digest鉴权.md) | `06_md5_digest.c` | `gb_sip.c` `gb_md5.c` |
| 4 | 目录上报与心跳（MANSCDP XML） | [04](docs/04-目录上报与心跳.md) | — | `gb_manscdp.c` |
| 5 | INVITE 与 SDP 协商 | [05](docs/05-INVITE与SDP协商.md) | — | `gb_sip.c` |
| 6 | MPEG-PS 打包器（四层结构） | [06](docs/06-MPEG-PS打包器.md) | `02/03/04_*.c` | `gb_ps.c` |
| 7 | RTP 封装与推流 | [07](docs/07-RTP封装与推流.md) | `05_rtp_slice.c` | `gb_rtp.c` |
| 8 | 实时流、音频轨、音画同步 | [08](docs/08-实时流与音频轨.md) [09](docs/09-音画同步原理.md) | `01/07/08_*.c` | `gb_media.c` `gb_g711.c` |
| 9 | aarch64 交叉编译与真机差异 | [10](docs/10-交叉编译与真机差异.md) | — | `Makefile` |
| 10 | 排障手册：12 条真实踩坑链 | [11](docs/11-排障手册.md) | — | — |
| 11 | 面试怎么讲 | [12](docs/12-面试怎么讲.md) | — | — |

> 先读 [00-学习地图](docs/00-学习地图.md)，它会告诉你按什么顺序看、每一步花多久、以及
> **什么时候该跳过字节细节**。

---

## 目录结构

```
gb28181-ipc-lab/
├── README.md                 本文件
├── LICENSE                   MIT
├── Makefile                  make / make aarch64 / make clean
├── config.ini                全部部署参数（IP、设备 ID、MTU、音频开关…）
├── src/                      主程序，按协议分层
│   ├── gb_common.h           公共定义（时间基、帧长常量）
│   ├── gb_log.[ch]           日志 + hexdump（排障用）
│   ├── gb_config.[ch]        配置解析与校验
│   ├── gb_md5.[ch]           RFC 1321 MD5（Digest 鉴权用）
│   ├── gb_g711.[ch]          G.711 A-law 编解码（音频）
│   ├── gb_annexb.[ch]        H.264 AnnexB 起始码识别与 NAL 切分
│   ├── gb_ps.[ch]            ★ MPEG-PS 打包器（核心）
│   ├── gb_rtp.[ch]           RTP 分片与封装
│   ├── gb_sip.[ch]           SIP 消息构造 + Digest + 注册流程
│   ├── gb_manscdp.[ch]       MANSCDP XML（心跳/目录/设备信息）
│   ├── gb_media.[ch]         媒体源（文件/FIFO）+ 音频生成 + 推流线程
│   └── main.c                装配与主循环
├── steps/                    8 个「一次只讲一个机制」的独立小程序
├── tests/                    单元测试（逐字节断言）
├── tools/
│   ├── selftest.sh               一键跑全部单测
│   ├── verify_pts.sh             ★ 用 ffprobe 反读 PTS，闭环验证位域
│   ├── verify_pts_mutation.sh    ★ 负向对照：故意写错位域，看测试抓不抓得住
│   ├── demo.sh                   端到端演示（自带假 SIP 平台，不需要装 wvp-pro）
│   ├── demo_stress.sh            连续多次跑 demo，验证没有竞态 flake
│   ├── fake_sip_platform.py      最小 SIP 平台，会真算 Digest 校验
│   ├── gen_teststream.sh         生成测试码流
│   └── build_steps.sh            编译并运行全部 kata
├── docs/                     13 章讲义
└── assets/                   测试码流（由脚本生成，不入库）
```

---

## 快速开始

需要一个 Linux 环境（WSL2 Ubuntu 20.04 即可）和 `gcc`。**前三步不需要任何服务器。**

```bash
# 0. 编译并跑全部单元测试 —— 先确认字节层是对的
make clean && make && make aarch64 && make test

# 1. 跑 8 个 kata，一次看懂一个机制（不需要网络）
bash tools/build_steps.sh

# 2. 闭环验证 PTS 位域：打一个 .ps 出来，用 ffprobe 反读回来比对
bash tools/verify_pts.sh

# 3. ★ 负向对照：故意把 PTS 位域改回错的写法，看测试是不是真的抓得住
bash tools/verify_pts_mutation.sh
```

第 3 步是这个仓库最值得看的一个设计。它把源码复制一份、只把 `>>14` 改成 `>>15`，
然后跑同一套验证 —— 结果：

```
单测抓住：  gb_ps_encode_pts(90000) = [21 00 03 BF 21], expected [21 00 05 BF 21]
ffprobe 抓住：写入 PTS=90000 → ffprobe 读回 pts=57232（差 -32768 ticks / -0.3641 秒）
```

**这就是"画面照样正常、几个月没人发现"的那类错误。**
现在它在 CI 里活不过 1 秒 —— 而一个只会说"我跑通了"的项目，永远证明不了这一点。

### 端到端连整条链路（也不需要装平台）

```bash
bash tools/gen_teststream.sh     # 用 ffmpeg 生成测试码流
bash tools/demo.sh               # 起一个假 SIP 平台 + 设备模拟器，跑完整流程
```

`tools/demo.sh` 会启动 `tools/fake_sip_platform.py` —— 一个**真的会算 Digest 校验**的
最小 SIP 平台，走完 `REGISTER(401→200) → MESSAGE(Catalog) → INVITE → ACK → RTP → BYE`，
并断言：RTP 头字节、载荷以 PS 起始码 `00 00 01 BA` 开头、`SSRC == SDP 里的 y=`、
音频轨时间戳序列等（当前 18 项断言）。

不想连跑就 `RUNS=4 bash tools/demo_stress.sh` 连续跑 4 次。

> 要连**真实**平台（wvp-pro + ZLMediaKit），见 [02 环境搭建](docs/02-环境搭建.md)。
> 那一步主要是为了验证与真实商用平台的互通性，日常学习用 `demo.sh` 就够。

---

## 两个必须先说清的边界

**一、这个仓库没有真机。** 摄像头采集与 H.264 编码由宿主侧 ffmpeg 完成
（`v4l2 → libx264 → FIFO`），aarch64 二进制跑在 QEMU 用户态。
这**恰好等于真实 IPC 的分工**：真机上 `Sensor → ISP → VENC(硬件编码器)` 出码流，
应用层只负责"拿码流 → 打 PS → 发 RTP"。所以本项目验证的软件栈与真机固件里那一层是等价的。
差别在：QEMU 不模拟中断、外设、驱动，也不验证硬件编码器（VENC/MPP）的调用。
这一条在 [10-交叉编译与真机差异](docs/10-交叉编译与真机差异.md) 里有详细口径。

**二、字节细节是为了"看懂"，不是为了"背"。** 讲义里会逐字节拆 PS 结构，
但面试考的是**概念、方法论、链路**：为什么国标用 PS、PSM 少了会怎样、
音画同步靠什么对齐、花屏该从哪一层查。具体某个字节是 `0x0E` 还是 `0x12`，
面试官不会问 —— 但你要能现场画出结构、说出怎么验证。
[12-面试怎么讲](docs/12-面试怎么讲.md) 专门做了这个区分。

---

## 与原项目的关系

本项目在 [AV_Study](https://github.com/SimpleStar666/AV_Study) 那套已验证的协议知识基础上重构而来，
**原仓库保持原样不动**。协议字节布局、12 条排障链、`-live FIFO` 的架构语义予以保留；
在此之上补齐了音频轨（G.711A）、配置化、构建系统、单元测试与闭环验证，
并修正了原实现中 PTS 位域错位一位的 bug（详见 [06 章](docs/06-MPEG-PS打包器.md)）。

## License

MIT
