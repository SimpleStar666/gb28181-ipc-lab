# 05 INVITE 与 SDP 协商

> **一句话**：平台要画面时发 `INVITE`，设备回 `200 OK` 并在 SDP 里
> 告诉平台两件事 —— "我推给你，你准备收"（`sendonly`），
> 以及"往哪个 IP、哪个端口、用什么编码推"。

---

## ① 这一步解决什么问题

注册和心跳解决了"平台知道设备存在"。现在用户点了"播放"按钮，接下来发生什么？

平台不知道设备会把流推到哪 —— 这是平台决定的，不是设备决定的。
所以流程是：

```
平台 ──── INVITE（我来点播） ──────────────► 设备
                                             │
                        ┌────────────────────┘
                        │ 设备解析 INVITE 里的 SDP：
                        │   目标 IP    = c=IN IP4 ...
                        │   目标端口   = m=video <port>
                        │   同步源 SSRC = y=...
                        ▼
平台 ◄─── 200 OK（SDP: 我往那个地址推）───── 设备
       ──── ACK ──────────────────────────────►
                                             │
                                             ▼ 启动推流线程
                                       ═══ RTP/PS 流 ═══► 平台
```

**关键认知：推流目标是平台在 INVITE 里指定的。**
设备不配置"往哪推"，它只是解析 SDP 然后照着推。
这也解释了为什么 ZLMediaKit 的收流端口（11000）是平台侧的事，设备不需要知道。

---

## ② 协议长什么样

### 先解释缩写

| 缩写 | 全称 | 人话 |
|---|---|---|
| **SDP** | Session Description Protocol | 会话描述协议，一段 `key=value` 文本，描述"用什么编码、发到哪、怎么发" |
| **INVITE** | — | SIP 请求：我要开始一个会话 |
| **200 OK** | — | SIP 响应：同意，参数在 body 的 SDP 里 |
| **ACK** | — | SIP 请求：确认收到 200 OK（SIP 三次握手的第三步） |
| **SSRC** | Synchronization Source | 同步源标识，32 位随机数，标识"这一路媒体流" |
| **PS** | Program Stream | 见 [06 章](06-MPEG-PS打包器.md)，国标用的媒体容器 |
| **PT** | Payload Type | 载荷类型，7 位。0–95 静态（RFC 3551 规定），96–127 动态（必须 `a=rtpmap` 声明） |
| **sendonly** | — | SDP 属性：我只发不收 |

### 平台发来的 INVITE（简化）

```text
INVITE sip:34020000001320000001@4403000000 SIP/2.0
Via: SIP/2.0/UDP 192.168.0.197:5060;rport;branch=z9hG4bK...
From: <sip:44030000002000000001@4403000000>;tag=...
To: <sip:34020000001320000001@4403000000>
Call-ID: ...
CSeq: 20 INVITE
Content-Type: application/sdp
Content-Length: 245

v=0
o=34020000001320000001 0 0 IN IP4 127.0.0.1
s=Play
c=IN IP4 192.168.0.197          ← 设备往这个 IP 推
t=0 0
m=video 11000 RTP/AVP 96        ← 往这个端口推，PT=96
a=rtpmap:96 PS/90000            ← PT 96 是 PS，时钟 90000
a=recvonly                      ← 平台只收
y=0123456789                    ← SSRC
```

### SDP 逐行读法

| 行 | 含义 | 代码里怎么用 |
|---|---|---|
| `v=0` | SDP 版本，固定 0 | 忽略 |
| `o=` | 会话发起者信息 | 忽略 |
| `s=Play` | 会话名称，`Play` 表示实时点播 | 忽略 |
| `c=IN IP4 <IP>` | **网络连接信息**：媒体目标地址 | `inet_addr()` 存起来 |
| `t=0 0` | 会话时间，0 0 = 永久 | 忽略 |
| `m=video <port> RTP/AVP 96` | **媒体描述**：端口、传输协议、载荷类型 | 存起来 |
| `a=rtpmap:96 PS/90000` | 载荷类型 96 = PS，时钟频率 90000 | 确认 PT 与时钟 |
| `a=recvonly` | 平台只收 | 设备可以开始推 |
| `y=<SSRC>` | 同步源标识 | `strtoul` 十进制解析 |

**`y=` 是国标特有的扩大属性（RFC 5576 定义），值是 10 位十进制 SSRC。**
不是十六进制，别解析错。

### 设备回的 200 OK

```text
SIP/2.0 200 OK
Via: SIP/2.0/UDP 192.168.0.197:5060;rport;branch=z9hG4bK...
From: <sip:44030000002000000001@4403000000>;tag=...
To: <sip:34020000001320000001@4403000000>;tag=9c8d7e6f
Call-ID: ...
CSeq: 20 INVITE
Content-Type: application/sdp
Content-Length: 165

v=0
o=34020000001320000001 0 0 IN IP4 127.0.0.1
s=Play
c=IN IP4 127.0.0.1
t=0 0
m=video 0 RTP/AVP 96        ← 端口 0：sendonly 时端口无意义（不接收）
a=rtpmap:96 PS/90000
a=sendonly                  ← 我只发
y=0123456789                ← 回显平台的 SSRC
```

**这里有三个容易搞错的点**：

1. **`Via`、`From`、`Call-ID`、`CSeq` 必须从请求里原样回填**，
   否则平台匹配不上这个响应，会认为设备没应答。
2. **`To` 头要加一个 `tag`**，这是 SIP 规定的（响应方生成自己的 tag）。
3. **`m=video 0`** 里的端口写 0 是合法的 —— 因为 `sendonly` 表示设备不接收，
   端口号没意义。真实实现里很多写 0。

---

## ③ 代码怎么长出来的

### 响应构造：从请求里"抄"字段

```c
static void build_200(sip_t *sip, char *buf, size_t cap,
                      const char *req, const char *extra_headers, const char *body)
{
    char via[512], from[512], to[512], cseq[128];
    header_value(req, "Via:",  via,  sizeof(via));
    header_value(req, "From:", from, sizeof(from));
    header_value(req, "To:",   to,   sizeof(to));
    header_value(req, "CSeq:", cseq, sizeof(cseq));
    /* ... 拼 SIP/2.0 200 OK ... */
}
```

**这里藏着一个真实的坑**：`header_value()` 用的是 `strstr()`。
`strstr(text, "To:")` 有可能匹配到别的位置（例如某个头字段的值里出现 `To:`），
而且更方便出错的是 —— **它不区分大小写、不做行首锚定**。

**更稳的写法**是按 `\r\n` 逐行扫描，只匹配行首。
本项目保留了简化的 `strstr` 版本（因为国标消息结构固定），
但**在代码注释里写明了这个限制**，并额外加了一道保护：

```c
/* INVITE 场景下如果 CSeq 没取到，用首个方法名兜底 */
if (strncmp(req, "INVITE", 6) == 0 && !cseq[0])
    snprintf(cseq, sizeof(cseq), "%d INVITE", next_cseq());
```

> **面试价值**：能说出"我这个解析器在什么情况下会错"，
> 比声称"我的解析器很健壮"可信得多。

### SDP 解析：三个 `strstr` 就够

```c
const char *p = strstr(body, "c=IN IP4 ");
const char *m = strstr(body, "m=video ");
const char *y = strstr(body, "y=");

sscanf(p + 9, "%63s", ip);          /* 跳过 "c=IN IP4 " 这 9 个字符 */
sscanf(m + 8, "%ld", &port);        /* 跳过 "m=video " 这 8 个字符 */
sscanf(y + 2, "%31s", ssrc_str);    /* 跳过 "y=" 这 2 个字符 */
```

三个偏移量（9、8、2）**必须是字面量长度的准确值**。
写错了不报错，只会解析出垃圾 —— 所以这些数字要配注释。

### 启动推流：只启动一次

```c
if (!streaming) {
    streaming = 1;
    pthread_create(&th, NULL, live_mode ? push_loop_live : push_loop, NULL);
}
```

**为什么要判 `!streaming`？** 因为平台可能重复发 INVITE（重试、或者多个人同时点播）。
不判断就会起多个推流线程，同一个 SSRC 发两路流，画面会撕裂。

### BYE：停止推流

```c
} else if (strncmp(text, "BYE ", 4) == 0) {
    build_200(...);  sip_send(...);
    streaming = 0;              /* 推流线程下一轮循环自己退出 */
}
```

**用标志位而不是强杀线程**，是因为推流线程可能正在 `sendto()` 或
`usleep(66ms)` 中间，强杀会留下半发送的帧。让它自己看到标志位退出，干净。

---

## ④ 怎么验证

```bash
./build/gb28181_device -c config.ini
```

期望日志：

```
[SIP] 注册成功（200 OK）
[SIP] 收到 MESSAGE 查询: Catalog
[SIP] 已上报目录（通道 34020000001320000001）
[SIP] 收到 INVITE 点播 → 推流目标 127.0.0.1:11000 SSRC=01234567
[SIP] 已回 200 OK + SDP(sendonly)，启动推流
[媒体] 推流线程启动（目标 127.0.0.1:11000，SSRC=01234567）
[媒体] 已推 10 帧 32 个 RTP 包，seq=...
[SIP] 收到 BYE（停止点播）
[媒体] 推流结束（BYE/停止）
```

平台侧：**日志里出现 `推流目标` 那一行，说明 SDP 解析对了。**

抓包看 RTP 有没有真的发出去：

```bash
sudo tcpdump -i any -c 20 -nn udp port 11000 -X
# 载荷应该以 00 00 01 BA 开头（PS 的 Pack Header）
```

---

## ⑤ 常见坑

| 坑 | 现象 | 原因 |
|---|---|---|
| `Via`/`From`/`CSeq` 没回填 | 平台认为设备无应答，点播超时 | 必须从请求原样抄 |
| `To` 没加 `tag` | 部分平台不认这个 200 OK | SIP 规定响应方要生成 tag |
| `y=` 按十六进制解析 | SSRC 完全对不上，收流失败 | `y=` 是**十进制** |
| SDP 偏移量写错一位 | IP/端口解析出垃圾，推流发到错误地址 | `sscanf` 的偏移要对着字面量数 |
| 每次都新建推流线程 | 重复 INVITE 导致多路流，画面撕裂 | 判 `!streaming` |
| BYE 里强杀线程 | 最后几帧不完整 | 用标志位优雅退出 |
| 忘了回 ACK 的处理 | 一般来说设备不需要处理 ACK，但要能忽略它不报错 | 主循环对未知方法要静默跳过 |

---

## ⑥ 面试怎么答

**问：点播流程是怎样的？设备怎么知道往哪推流？**

> "平台发 SIP 的 INVITE，body 是 SDP。设备从 SDP 里解析三样东西：
> `c=IN IP4` 给目标 IP，`m=video <port>` 给目标端口，`y=` 给 SSRC。
> 然后设备回 200 OK，SDP 里写 `a=sendonly` 表示我只发不收，
> `m=video 0` 的端口写 0 是合法的因为不接收。
> 平台回 ACK 后设备启动推流线程。
> 所以推流目标是平台在 INVITE 里指定的，不是设备自己配置的。"

**追问：为什么设备端口写 0？**
→ "因为 `sendonly` 表示设备不接收媒体，收流端口没有意义。
   SDP 里 `m=` 行的端口为 0 表示这路媒体被禁用（不接收）。"

**追问：`y=` 是什么？**
→ "国标用来传 SSRC 的扩大属性，值是十进制。SSRC 是 RTP 的同步源标识，
   平台用它区分不同的流。设备回 200 OK 时要回显同一个 SSRC。"

**追问：为什么 `m=video` 的 PT 是 96？**
→ "96 是动态载荷类型。0 到 95 是 RFC 3551 预定义的静态类型
   （比如 0 是 PCMU、8 是 PCMA、96 以上留给私有约定），
   96 到 127 必须在 SDP 里用 `a=rtpmap` 声明成什么编码。
   国标用 `a=rtpmap:96 PS/90000`，说明 96 号载荷是 PS，时钟 90000。"

---

## 下一步

→ [06 MPEG-PS 打包器](06-MPEG-PS打包器.md) ← **全项目核心**
