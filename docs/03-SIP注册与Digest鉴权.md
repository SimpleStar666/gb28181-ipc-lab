# 03 SIP 注册与 Digest 鉴权

> **一句话**：设备开机往平台发 `REGISTER`，平台回 `401` 加一串挑战参数，
> 设备用 MD5 算出应答再发一次，平台回 `200` —— 注册完成。
>
> **本章出现的每一个英文缩写都会解释。** 不认识的词直接往下找。

---

## ① 这一步解决什么问题

设备要能"被平台叫起来"，前提是平台知道它存在。这就是注册。

注册必须解决一个安全问题：**怎么证明发消息的真是这台设备，而不是别人冒充的？**
明文传密码不行（UDP 上谁都能抓）。所以 SIP 用 **Digest 摘要认证**：
双方各自用密码算一个哈希，比较哈希而不是比较密码。密码本身**永远不上网**。

---

## ② 协议长什么样

### 先解释缩写

| 缩写 | 全称 | 人话 |
|---|---|---|
| **SIP** | Session Initiation Protocol | 会话初始协议，管"建立/拆除通话"的信令协议 |
| **REGISTER** | — | SIP 方法之一：我要注册 |
| **MESSAGE** | — | SIP 方法之一：我发一条即时消息（国标用来传 XML） |
| **INVITE** | — | SIP 方法之一：我邀请你开始一个会话（国标用来点播） |
| **BYE** | — | SIP 方法之一：结束会话 |
| **CSeq** | Command Sequence | 命令序号，同一个会话里每条新请求 +1，用来匹配请求与响应 |
| **Call-ID** | — | 会话标识，注册流程内保持不变 |
| **Via** | — | 记录请求走过的路径，响应按原路返回 |
| **From / To** | — | 发起方 / 接收方的 SIP URI，各带一个 `tag` 参数 |
| **Contact** | — | "我在这个地址"，告诉对方后续请求发到哪 |
| **branch** | — | Via 里的分支参数，必须以 `z9hG4bK` 开头（RFC 3261 的魔法 cookie） |
| **Digest** | — | 摘要认证方法（RFC 2617），用哈希代替明文密码 |
| **realm** | — | 认证域，可以理解为"哪个密码库" |
| **nonce** | number used once | 服务器给的一次性随机串，防止重放攻击 |
| **qop** | quality of protection | 保护质量，取值 `auth` 表示"只认证，不加密" |
| **nc** | nonce count | 这个 nonce 是第几次被使用，8 位十六进制，首次 `00000001` |
| **cnonce** | client nonce | 客户端自己生成的随机串，参与运算，防止服务器预先算好哈希 |
| **HA1 / HA2** | — | Digest 计算的两个中间哈希值，见下文 |

### 注册时序

```
   设备                                              平台
    │                                                 │
    │──── REGISTER (无 Authorization) ───────────────►│
    │                                                 │
    │◄─── 401 Unauthorized ───────────────────────────│
    │     WWW-Authenticate: Digest realm="4403000000", │
    │       nonce="abc123...", qop="auth"             │
    │                                                 │
    │  ① 用 realm/nonce/qop + 密码算 response          │
    │                                                 │
    │──── REGISTER (带 Authorization) ───────────────►│
    │                                                 │
    │◄─── 200 OK ─────────────────────────────────────│
    │                                                 │
    │  （之后每 60 秒发一次 MESSAGE Keepalive）        │
```

### REGISTER 消息长什么样

```text
REGISTER sip:44030000002000000001@4403000000 SIP/2.0
Via: SIP/2.0/UDP 127.0.0.1:5061;rport;branch=z9hG4bK1a2b3c4d
From: <sip:34020000001320000001@4403000000>;tag=5e6f7a8b
To: <sip:34020000001320000001@4403000000>
Call-ID: 9c8d7e6f@127.0.0.1
CSeq: 1 REGISTER
Contact: <sip:34020000001320000001@127.0.0.1:5061>
Max-Forwards: 70
User-Agent: C-GB28181Sim
Expires: 3600
Content-Length: 0
```

**逐行读法**：

- 第一行 = 请求行：`方法 URI 版本`。URI 里的域部分（`4403000000`）是平台的 SIP 域。
- `Via` 里的 `127.0.0.1:5061` 是**设备自己**的地址 —— 平台回响应时按这个地址发回去。
- `From` 是设备，`To` 是平台（注册阶段 To 和 From 用同一个设备标识，这是 SIP 的特性：
  注册不是"呼叫某人"，而是"登记我自己"）。
- `Expires: 3600` 表示"我这个注册 1 小时内有效"。到期前必须重新注册，否则平台认为设备掉线。
- `Content-Length: 0` 因为 REGISTER 没有 body。

带的 `Authorization` 头长这样：

```text
Authorization: Digest username="34020000001320001",
  realm="4403000000",
  nonce="f4e3d2c1b0a9...",
  uri="sip:44030000002000000001@4403000000",
  response="8f14e45fceea167a5a36dedd4bea2543",
  algorithm=MD5,
  qop=auth, nc=00000001, cnonce="0a4f113b"
```

---

## ③ 代码怎么长出来的

### 第一步：MD5

Digest 全建立在 MD5 上，所以先要有 MD5。`gb_md5.c` 是标准的 RFC 1321 实现。

**这里有个真实事故**：原项目四份"独立"的 MD5 实现，**全都把 Round 1 第 15 步的常量
写成了 `0xa6434387`，正确值是 `0xa679438e`**。

因为四份都错，改 A 用 B 验证，B 也说"对的"，形成**假一致**。
最后是靠标准测试向量发现的：

```
MD5("")    = d41d8cd98f00b204e9800998ecf8427e
MD5("abc") = 900150983cd24fb0d6963f7d28e17f72
```

现象是平台回 `403 wrong password` —— **看起来像密码错了，实际是哈希算错了。**
这个坑的教训是：**跨实现的验证必须用外部标准向量，不能自己跟自己对。**

（跑 `steps/06_md5_digest.c` 可以亲眼看到这两个向量。）

### 第二步：三个哈希

Digest 的计算分三步。记住这张表，面试就不会卡：

```c
/* HA1 = MD5(用户名 : realm : 密码) */
snprintf(buf, sizeof(buf), "%s:%s:%s", user, realm, password);
gb_md5_hex_str(buf, ha1);

/* HA2 = MD5(方法 : URI)     —— 注意方法是大写，如 "REGISTER" */
snprintf(buf, sizeof(buf), "%s:%s", method, uri);
gb_md5_hex_str(buf, ha2);

/* response：
 *   qop=auth 时： MD5(HA1 : nonce : nc : cnonce : qop : HA2)
 *   无 qop 时  ： MD5(HA1 : nonce : HA2)                         */
```

**为什么要加 `nc` 和 `cnonce`？**
只带 `nonce` 的话，攻击者抓到一次 Authorization 就能原样重放。
加入 `nc`（第几次）和 `cnonce`（客户端随机数）后，每次请求的 response 都不同，
重放立刻失效。这是 Digest 比"密码哈希直传"安全的地方。

### 第三步：两条分支都要写

```c
if (qop[0]) {
    /* 带 qop=auth —— 必须同时给 nc 和 cnonce */
} else {
    /* 无 qop —— 老平台/老设备用这个，response 只需要三个输入 */
}
```

**原项目只写了 `qop=auth` 分支**，换一个平台就注册失败，而且对方只回一个 `403`，
完全看不出是鉴权串算错了。**这就是"只按我测过的环境写代码"的代价。**

### 第四步：注册流程的状态机

```c
for (tries = 0; tries < 30; tries++) {
    发送 REGISTER（auth 为空则不带 Authorization）
    等 2 秒：
      收到 "SIP/2.0 401" → 解析 realm/nonce/qop → 算 Authorization → 下轮带上
      收到 "SIP/2.0 200" → 成功，退出
}
```

注意 `gbed_sip_build_digest_auth()` 里用到了一个细节：
**`uri` 参数必须和第一行请求行里的 URI 完全一致**，差一个字符 response 就错。
所以代码里 `build_register()` 和 Digest 用的是同一个 `uri` 字符串。

---

## ④ 跑哪个 kata 能看见

```bash
gcc -Wall -O2 -o /tmp/k06 steps/06_md5_digest.c && /tmp/k06
```

它会：
1. 断言两个 RFC 1321 标准向量
2. 打印一次完整的 401 → response 计算过程，把中间值（HA1/HA2/response）都列出来
3. 把 `realm` / `nonce` / `qop` / `nc` / `cnonce` 的含义打在屏幕上

---

## ⑤ 怎么验证"我做对了"

```bash
# 单元测试：MD5 标准向量 + 分块 update 一致性
make test
# 或单独跑
bash tools/selftest.sh

# 真机验证：起模拟器，看注册日志
./build/gb28181_device -c config.ini
# 期望输出：
#   [SIP] 开始注册 sip:44030000002000000001@4403000000 ...
#   [SIP] REGISTER 已发送
#   [SIP] 收到 401（qop=auth），已计算 Digest response
#   [SIP] REGISTER 已发送（带 Digest 认证）
#   [SIP] 注册成功（200 OK）
```

**注册失败的三种表现，对应三种原因**：

| 平台回应 | 原因 |
|---|---|
| `401` 一直循环，从不成功 | response 算错（MD5 实现、字符串拼接、大小写） |
| `403 Forbidden` | 密码错，或者**用的是错的 MD5 常量**（原项目就是这种） |
| 完全没响应 | SIP 服务器地址/端口错，或设备没绑住本地端口 |

---

## ⑥ 常见坑

| 坑 | 现象 | 定位方法 |
|---|---|---|
| MD5 常量抄错 | 403，但密码明明是对的 | 跑标准向量 `MD5("")` |
| 只实现 `qop=auth` 分支 | 特定平台必失败 | 抓包看 401 里有没有 qop |
| 字符串拼接时多/少一个分隔符 | 全部 401 | 把待哈希的原始串打印出来 |
| 方法名大小写不一致 | 全部 401 | HA2 里必须是 `REGISTER` 不是 `register` |
| 本地端口被占 | `bind: Address already in use` | 加 `SO_REUSEADDR`，或换端口 |
| `Expires` 到期没重注册 | 平台显示"设备离线"但设备还在跑 | 加定时重注册（本项目用 60 秒心跳维持） |

---

## ⑦ 面试怎么答

**问：SIP 鉴权是怎么做的？**

> "用的是 HTTP Digest 那套（RFC 2617）。设备先发一次不带认证的 REGISTER，
> 平台回 401 并在 WWW-Authenticate 里给出 realm、nonce、qop。
> 设备用 MD5 算三个值：HA1 是用户名、realm、密码拼起来；
> HA2 是方法和 URI 拼起来；最后的 response 在 qop=auth 时
> 是 HA1、nonce、nc、cnonce、qop、HA2 六段拼接后的 MD5。
> 密码本身不在网络上传输，而且因为带了 nc 和 cnonce，抓包也无法重放。"

**追问：为什么要有 nonce 和 cnonce？**
→ "nonce 是服务器给的一次性随机串，cnonce 是客户端给的随机串。
   只有 nonce 的话，攻击者抓到一次认证消息就能重放；
   加上客户端随机数和序号后，每次 response 都不同。"

**追问：这个 MD5 是自己实现的还是用库？**
→ "自己实现的，100 行左右。设备端不引 OpenSSL，依赖太重。
   不过自己实现必须用标准测试向量验证 —— 我们那份代码就在常量上错过一次，
   而且是四份拷贝同时错，只能靠 `MD5("")` 这个外部向量发现。"
   ← **这个回答非常有说服力**，因为它同时展现了"会用轮子"和"知道轮子为什么危险"。

**追问：遇到过什么鉴权问题？**
→ "两个。一个是 MD5 常量错导致 403，看着像密码错。
   还有一个是只实现了带 qop 的分支，换平台就失效 ——
   后来补了无 qop 分支，因为不同版本的平台行为不一样。"

---

## 下一步

→ [04 目录上报与心跳](04-目录上报与心跳.md)
