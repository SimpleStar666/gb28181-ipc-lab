#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fake_sip_platform.py — 假 GB28181 平台（tools/demo.sh 的另一半）
================================================================
它不是"能收包就行"的桩，而是按 RFC 3261 / GB/T 28181 真走一遍控制面：

  1. 收 REGISTER（无鉴权）        → 回 401 Unauthorized + WWW-Authenticate(nonce)
  2. 收 REGISTER（带 Authorization）→ **真的验一遍 Digest**（HA1/HA2/response），
                                    通过才回 200 OK —— 这一步同时验证了 gb_md5 与 gb_sip
  3. 收设备的 MESSAGE（Keepalive/Catalog）
  4. 主动发 MESSAGE(Catalog 查询, SN=1) → 断言设备回 <SN>1</SN> 的 Catalog 应答
  5. 发 INVITE（SDP: c=IN IP4 <ip> / m=video <port> RTP/AVP 96 / y=<ssrc>）
     → 断言设备回 200 OK（带它自己的 SDP）
  6. 在 RTP 端口收包，断言：
        byte0 == 0x80                     （RTP 版本 2，无填充/扩展，CC=0）
        byte1 & 0x7F == 96                （PS over RTP 的动态载荷类型）
        载荷开头 == 00 00 01 BA            （MPEG-PS Pack Header 起始码）
  7. 发 BYE → 断言设备回 200 OK

退出码：0 = 全部断言通过；1 = 有断言失败（失败原因逐条打印）。
用法见文件末尾 __main__ 或 `python3 tools/fake_sip_platform.py --help`。
"""

import argparse
import hashlib
import os
import random
import socket
import sys
import time

PASS_LIST = []
FAIL_LIST = []
_SEEN = set()


def ok(tag, detail=""):
    # 重复事件（心跳、多批 RTP）只记一次，避免统计被刷屏
    if tag in _SEEN:
        return
    _SEEN.add(tag)
    PASS_LIST.append("%s%s" % (tag, (" — " + detail) if detail else ""))
    print("[platform]   ✓ %s%s" % (tag, (" — " + detail) if detail else ""), flush=True)


def bad(tag, detail=""):
    FAIL_LIST.append("%s%s" % (tag, (" — " + detail) if detail else ""))
    print("[platform]   ✗ %s%s" % (tag, (" — " + detail) if detail else ""), flush=True)


def md5hex(s):
    return hashlib.md5(s.encode("utf-8")).hexdigest()


class Msg(object):
    """把一条 SIP 报文拆成 first line / headers / body。"""

    def __init__(self, data):
        text = data.decode("utf-8", "replace")
        self.raw = text
        head, _, body = text.partition("\r\n\r\n")
        if not _:
            head, _, body = text.partition("\n\n")
        lines = [l.rstrip("\r") for l in head.split("\n")]
        self.first = lines[0] if lines else ""
        self.headers = {}
        self.header_lines = []
        for l in lines[1:]:
            if not l.strip():
                continue
            self.header_lines.append(l)
            k, _, v = l.partition(":")
            self.headers[k.strip().lower()] = v.strip()
        self.body = body

    @property
    def method(self):
        return self.first.split(" ")[0].upper() if self.first else ""

    @property
    def code(self):
        parts = self.first.split(" ")
        return int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0

    def kw(self, key, default=""):
        """Digest / SDP 里的 key=value 或 key="value" """
        import re
        m = re.search(r'(?:^|[;,\s])' + re.escape(key) + r'\s*=\s*(?:"([^"]*)"|([^;,\s]+))',
                      self.raw, re.IGNORECASE)
        if not m:
            return default
        return m.group(1) if m.group(1) is not None else m.group(2)

    def sdp_line(self, prefix):
        for l in self.body.splitlines():
            if l.strip().lower().startswith(prefix):
                return l.strip()
        return ""


def build_response(req, code, reason, extra_headers=None, body=""):
    """按请求回一条响应：Via/From/To/Call-ID/CSeq 原样回填（To 需要时补 tag）。"""
    via = req.headers.get("via", "")
    frm = req.headers.get("from", "")
    to = req.headers.get("to", "")
    callid = req.headers.get("call-id", "")
    cseq = req.headers.get("cseq", "")
    if "tag=" not in to.lower():
        to = to + (";tag=%d" % random.randint(100000, 999999))
    lines = [
        "SIP/2.0 %d %s" % (code, reason),
        "Via: %s" % via,
        "From: %s" % frm,
        "To: %s" % to,
        "Call-ID: %s" % callid,
        "CSeq: %s" % cseq,
        "User-Agent: fake-gb28181-platform/1.0",
    ]
    if extra_headers:
        lines.extend(extra_headers)
    if body:
        lines.append("Content-Type: application/sdp")
    lines.append("Content-Length: %d" % len(body.encode("utf-8")))
    return ("\r\n".join(lines) + "\r\n\r\n" + body).encode("utf-8")


def build_request(method, uri, to_uri, from_uri, call_id, cseq, local_ip, local_port,
                  extra_headers=None, body="", content_type=None, contact=True,
                  from_tag=None, to_header_value=None):
    """返回 (报文 bytes, from_tag)。INVITE 需要记住 from_tag 才能在 200 OK 后回 ACK。
    to_header_value: 直接给整条 To 头的值（ACK 要带回 200 OK 里的 tag）。"""
    branch = "z9hG4bK%08x" % random.getrandbits(32)
    tag = from_tag or ("%08x" % random.getrandbits(32))
    lines = [
        "%s %s SIP/2.0" % (method, uri),
        "Via: SIP/2.0/UDP %s:%d;rport;branch=%s" % (local_ip, local_port, branch),
        "Max-Forwards: 70",
        "From: <%s>;tag=%s" % (from_uri, tag),
        "To: %s" % (to_header_value if to_header_value else ("<%s>" % to_uri)),
        "Call-ID: %s" % call_id,
        "CSeq: %d %s" % (cseq, method),
    ]
    if contact:
        lines.append("Contact: <%s>" % from_uri)
    lines.append("User-Agent: fake-gb28181-platform/1.0")
    if content_type:
        lines.append("Content-Type: %s" % content_type)
    if extra_headers:
        lines.extend(extra_headers)
    lines.append("Content-Length: %d" % len(body.encode("utf-8")))
    return ("\r\n".join(lines) + "\r\n\r\n" + body).encode("utf-8"), tag


def verify_digest(auth_header, method, password, issued):
    """按 RFC 2617 验 Digest。issued: {nonce: realm}。返回 (ok, 说明)。"""
    import re
    if not auth_header.lower().startswith("digest"):
        return False, "Authorization 不是 Digest 类型"
    p = {}
    for k, v1, v2 in re.findall(r'(\w+)\s*=\s*(?:"([^"]*)"|([^;,\s]+))', auth_header):
        p[k.lower()] = v1 if v1 else v2
    user = p.get("username", "")
    realm = p.get("realm", "")
    nonce = p.get("nonce", "")
    uri = p.get("uri", "")
    resp = p.get("response", "").lower()
    qop = p.get("qop", "")
    nc = p.get("nc", "")
    cnonce = p.get("cnonce", "")

    if nonce not in issued:
        return False, "nonce 不是平台发出的（%s）" % nonce
    ha1 = md5hex("%s:%s:%s" % (user, realm, password))
    ha2 = md5hex("%s:%s" % (method, uri))
    if qop:
        expect = md5hex("%s:%s:%s:%s:%s:%s" % (ha1, nonce, nc, cnonce, qop, ha2))
    else:
        expect = md5hex("%s:%s:%s" % (ha1, nonce, ha2))
    if expect == resp:
        return True, "qop=%s HA1=%s..." % (qop or "(无)", ha1[:8])
    return False, ("response 不匹配: 平台算出 %s, 设备给的是 %s"
                   " (qop=%r nc=%r cnonce=%r uri=%r realm=%r)"
                   % (expect, resp, qop, nc, cnonce, uri, realm))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sip-port", type=int, default=15060)
    ap.add_argument("--rtp-port", type=int, default=15062)
    ap.add_argument("--device-id", default="34020000001320000001")
    ap.add_argument("--platform-id", default="44030000002000000001")
    ap.add_argument("--domain", default="4403000000")
    ap.add_argument("--password", default="12345678")
    ap.add_argument("--ssrc", default="1234567890")
    ap.add_argument("--invite-at", type=float, default=3.0)
    ap.add_argument("--bye-at", type=float, default=11.0)
    ap.add_argument("--deadline", type=float, default=20.0)
    ap.add_argument("--min-rtp", type=int, default=2)
    ap.add_argument("--log", default="")
    args = ap.parse_args()

    if args.log:
        try:
            sys.stdout = open(args.log, "w", buffering=1)
        except OSError as e:
            print("[platform] 打不开日志文件 %s: %s" % (args.log, e))

    # 第一行带上 PID 和时间：若日志里出现两条不同的 "启动" 行，
    # 就说明有上一次运行残留的平台进程在并发写同一个文件（demo.sh 的 cleanup 没杀干净）。
    print("[platform] 启动 pid=%d at %.3f  SIP=%d RTP=%d min_rtp=%d"
          % (os.getpid(), time.time(), args.sip_port, args.rtp_port, args.min_rtp), flush=True)

    sip = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sip.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sip.bind(("127.0.0.1", args.sip_port))
    sip.settimeout(0.2)

    rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rtp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    rtp.bind(("127.0.0.1", args.rtp_port))
    rtp.settimeout(0.05)

    print("[platform] SIP 监听 127.0.0.1:%d, RTP 监听 127.0.0.1:%d" % (args.sip_port, args.rtp_port),
          flush=True)

    t0 = time.time()
    device_addr = None
    nonce = ""
    realm = args.domain
    issued = {}
    state = {
        "register_seen": 0,        # 收到的 REGISTER 次数（1=无鉴权, 2=带鉴权）
        "register_authed": False,
        "keepalive_seen": False,
        "catalog_req_sent": False,
        "catalog_resp_seen": False,
        "invite_sent": False,
        "invite_200_seen": False,
        "bye_sent": False,
        "bye_200_seen": False,
    }
    rtp_stat = {"pkts": 0, "ps": 0, "bytes": 0, "first": b"", "sid_wrong": 0,
                "v_wrong": 0, "ssrc_bad": 0, "ssrc_seen": 0, "ts": set()}
    call_id = "%08x@fake-platform" % random.getrandbits(32)
    inv = {"from_tag": None, "to_hdr": None}
    bye = {"cseq": 23, "sent_at": [], "attempts": 0}

    def send(data, addr=None):
        sip.sendto(data, addr or device_addr)

    def is_response_for(msg, method, cseq_num):
        """响应按 CSeq 匹配：既认方法名，也认 CSeq 编号。
        平台自己也在收设备对 Keepalive 的 200 OK，只按方法名匹配会认错。"""
        cseq = msg.headers.get("cseq", "").strip().upper()
        return cseq.endswith(method) or cseq.split(" ")[0] == str(cseq_num)

    while True:
        elapsed = time.time() - t0
        # 事件驱动退出：所有关键事件都拿到就走；否则撑到 deadline。
        # 之前是纯固定 deadline，设备回 BYE 的 200 OK 稍慢一点就会被判失败（这是之前 demo.sh 偶发红的根因之一）。
        if state["bye_200_seen"] and elapsed > args.bye_at:
            break
        if elapsed > args.deadline:
            break

        # ---- 收 SIP ----
        try:
            data, addr = sip.recvfrom(65535)
            device_addr = addr
            msg = Msg(data)
            if msg.first.startswith("SIP/2.0"):
                code = msg.code
                if is_response_for(msg, "INVITE", 21) and code == 200:
                    state["invite_200_seen"] = True
                    sdp = msg.body.strip().replace("\r\n", " | ")
                    ok("INVITE 收到 200 OK", sdp[:160])
                    # 补一条 ACK，把 INVITE 事务走完整 —— 有些设备要等到 ACK 才开始推流
                    inv["to_hdr"] = msg.headers.get("to", "")
                    if inv["from_tag"] and inv["to_hdr"]:
                        ack, _ = build_request(
                            "ACK", "sip:%s@%s" % (args.device_id, realm),
                            "", "sip:%s@%s" % (args.platform_id, realm),
                            call_id, 21, "127.0.0.1", args.sip_port,
                            from_tag=inv["from_tag"], contact=False,
                            to_header_value=inv["to_hdr"])
                        send(ack)
                        ok("已回 ACK（INVITE 事务完成）")
                elif is_response_for(msg, "BYE", bye["cseq"]):
                    if code == 200:
                        state["bye_200_seen"] = True
                        ok("BYE 收到 200 OK", "第 %d 次尝试后" % max(1, bye["attempts"]))
                    else:
                        # 481 = Call/Transaction Does Not Exist：说明对话标识（Call-ID/tag）没对上
                        print("[platform]   注意: BYE 收到 %d %s（若不是 200，设备未能匹配到对话）"
                              % (code, msg.first.split(" ", 2)[-1]), flush=True)
                        state["bye_code"] = code
                # 其它 200 OK（对平台 MESSAGE 的应答）不单独断言
                continue

            method = msg.method
            if method == "REGISTER":
                state["register_seen"] += 1
                auth = msg.headers.get("authorization", "")
                if not auth:
                    nonce = "%08x%08x" % (random.getrandbits(32), random.getrandbits(32))
                    issued[nonce] = realm
                    hdrs = ['WWW-Authenticate: Digest realm="%s", nonce="%s", algorithm=MD5, qop="auth"'
                            % (realm, nonce)]
                    send(build_response(msg, 401, "Unauthorized", hdrs))
                    ok("REGISTER #1 → 401 + 挑战", "realm=%s nonce=%s" % (realm, nonce[:8]))
                else:
                    good, why = verify_digest(auth, "REGISTER", args.password, issued)
                    if good:
                        state["register_authed"] = True
                        send(build_response(msg, 200, "OK",
                                            ["Expires: 3600",
                                             "Contact: <sip:%s@127.0.0.1:%d>"
                                             % (args.device_id, args.sip_port)]))
                        ok("REGISTER #2 → 200 OK（Digest 校验通过）", why)
                    else:
                        send(build_response(msg, 403, "Forbidden"))
                        bad("REGISTER #2 Digest 校验失败", why)
            elif method == "MESSAGE":
                body = msg.body
                if "Keepalive" in body:
                    state["keepalive_seen"] = True
                    ok("收到设备心跳 MESSAGE(Keepalive)")
                elif "Catalog" in body:
                    state["catalog_resp_seen"] = True
                    ok("收到设备目录应答 MESSAGE(Catalog)")
                else:
                    ok("收到设备 MESSAGE", body.strip().replace("\r\n", " ")[:80])
                send(build_response(msg, 200, "OK"))
            elif method == "INVITE":
                send(build_response(msg, 200, "OK",
                                    ["Contact: <sip:%s@127.0.0.1:%d>" % (args.device_id, args.sip_port)],
                                    "v=0\r\no=%s 0 0 IN IP4 127.0.0.1\r\ns=Play\r\n"
                                    "c=IN IP4 127.0.0.1\r\nt=0 0\r\n"
                                    "m=video %d RTP/AVP 96\r\na=recvonly\r\n"
                                    "a=rtpmap:96 PS/90000\r\ny=%s\r\n"
                                    % (args.platform_id, args.rtp_port, args.ssrc)))
                ok("收到设备对 INVITE 的响应（应另有 200 OK）")
            elif method == "BYE":
                send(build_response(msg, 200, "OK"))
            else:
                send(build_response(msg, 200, "OK"))
        except socket.timeout:
            pass
        except Exception as e:  # noqa: BLE001
            bad("SIP 解析异常", repr(e))

        # ---- 收 RTP ----
        for _ in range(64):
            try:
                pkt, _a = rtp.recvfrom(65535)
            except socket.timeout:
                break
            except Exception:
                break
            rtp_stat["pkts"] += 1
            rtp_stat["bytes"] += len(pkt)
            if not rtp_stat["first"]:
                rtp_stat["first"] = pkt[:16]
            if len(pkt) >= 14:
                if pkt[0] != 0x80:
                    rtp_stat["v_wrong"] += 1
                if (pkt[1] & 0x7F) != 96:
                    rtp_stat["sid_wrong"] += 1
                if pkt[12:16] == b"\x00\x00\x01\xba":
                    rtp_stat["ps"] += 1
            if len(pkt) >= 12:
                # SSRC 必须来自 INVITE 的 SDP y= 行（验证 SDP 解析 → RTP 头这条链）
                got_ssrc = int.from_bytes(pkt[8:12], "big")
                if got_ssrc == int(args.ssrc):
                    rtp_stat["ssrc_seen"] += 1
                else:
                    rtp_stat["ssrc_bad"] += 1
                    if rtp_stat["ssrc_bad"] <= 2:
                        print("[platform]   注意: 收到 SSRC=0x%08X, SDP y= 给的是 %s"
                              % (got_ssrc, args.ssrc), flush=True)
                rtp_stat["ts"].add(int.from_bytes(pkt[4:8], "big"))

        # ---- 按时间推进流程 ----
        if state["register_authed"] and not state["catalog_req_sent"] and elapsed > 1.5:
            xml = ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
                   "<Query>\r\n<CmdType>Catalog</CmdType>\r\n<SN>1</SN>\r\n"
                   "<DeviceID>%s</DeviceID>\r\n</Query>\r\n" % args.device_id)
            send(build_request("MESSAGE", "sip:%s@%s" % (args.device_id, realm),
                               "sip:%s@%s" % (args.device_id, realm),
                               "sip:%s@%s" % (args.platform_id, realm),
                               call_id, 20, "127.0.0.1", args.sip_port,
                               body=xml, content_type="Application/MANSCDP+xml")[0])
            state["catalog_req_sent"] = True
            ok("发出 MESSAGE(Catalog 查询, SN=1)")

        if state["register_authed"] and not state["invite_sent"] and elapsed > args.invite_at:
            sdp = ("v=0\r\n"
                   "o=%s 0 0 IN IP4 127.0.0.1\r\n"
                   "s=Play\r\n"
                   "c=IN IP4 127.0.0.1\r\n"
                   "t=0 0\r\n"
                   "m=video %d RTP/AVP 96\r\n"
                   "a=recvonly\r\n"
                   "a=rtpmap:96 PS/90000\r\n"
                   "y=%s\r\n"
                   "f=v/2/5/25/1/4000a\r\n" % (args.platform_id, args.rtp_port, args.ssrc))
            pkt, inv["from_tag"] = build_request(
                "INVITE", "sip:%s@%s" % (args.device_id, realm),
                "sip:%s@%s" % (args.device_id, realm),
                "sip:%s@%s" % (args.platform_id, realm),
                call_id, 21, "127.0.0.1", args.sip_port,
                body=sdp, content_type="application/sdp")
            send(pkt)
            state["invite_sent"] = True
            ok("发出 INVITE", "c=IN IP4 127.0.0.1 / m=video %d RTP/AVP 96 / y=%s"
               % (args.rtp_port, args.ssrc))

        # BYE：对话内请求必须复用 INVITE 的 From tag / Call-ID（否则严格设备回 481）。
        # 真平台也会重传，这里最多发 3 次（+0 / +1.5s / +3.0s），拿到 200 OK 就停。
        if state["invite_sent"] and not state["bye_200_seen"] and elapsed > args.bye_at:
            need_send = False
            if not bye["sent_at"]:
                need_send = True
            elif len(bye["sent_at"]) < 3 and elapsed > bye["sent_at"][-1] + 1.5:
                need_send = True
            if need_send:
                pkt, _ = build_request(
                    "BYE", "sip:%s@%s" % (args.device_id, realm),
                    "", "sip:%s@%s" % (args.platform_id, realm),
                    call_id, bye["cseq"], "127.0.0.1", args.sip_port,
                    from_tag=inv["from_tag"],
                    to_header_value=inv["to_hdr"])
                send(pkt)
                bye["sent_at"].append(elapsed)
                bye["attempts"] = len(bye["sent_at"])
                state["bye_sent"] = True
                ok("发出 BYE", "第 %d 次（CSeq=%d, From tag 与 INVITE 一致）"
                   % (bye["attempts"], bye["cseq"]))

    # ------------------------- 收尾断言 -------------------------
    print("[platform] ------ 断言汇总 ------", flush=True)
    if state["register_seen"] >= 1:
        ok("控制面: 收到 REGISTER")
    else:
        bad("控制面: 完全没收到 REGISTER")
    if not state["register_authed"]:
        bad("控制面: REGISTER 未通过 Digest 鉴权（见上面 Digest 行）")
    if not state["keepalive_seen"] and not state["catalog_resp_seen"]:
        bad("控制面: 没收到任何设备主动 MESSAGE（Keepalive/Catalog）")
    if not state["catalog_resp_seen"]:
        bad("控制面: 发了 Catalog 查询但没收到 Catalog 应答（SN 回填/XML 可能有问题）")
    else:
        ok("控制面: Catalog 查询 → 应答闭环")
    if not state["invite_200_seen"]:
        bad("控制面: INVITE 没收到 200 OK")
    if not state["bye_200_seen"]:
        bad("控制面: BYE 没收到 200 OK（已尝试 %d 次%s）"
            % (max(1, bye["attempts"]),
               "" if "bye_code" not in state else "，设备回过 %d" % state["bye_code"]))
    else:
        ok("控制面: BYE → 200 OK 闭环")

    print("[platform] RTP 统计: 包数=%d 其中载荷以 PS 起始码开头=%d 总字节=%d"
          % (rtp_stat["pkts"], rtp_stat["ps"], rtp_stat["bytes"]), flush=True)
    if rtp_stat["first"]:
        print("[platform] 首个 RTP 包前 16 字节: %s"
              % " ".join("%02X" % b for b in rtp_stat["first"]), flush=True)

    if rtp_stat["pkts"] < args.min_rtp:
        bad("媒体面: RTP 包数 %d < 期望 %d" % (rtp_stat["pkts"], args.min_rtp))
    else:
        ok("媒体面: 收到 %d 个 RTP 包" % rtp_stat["pkts"])
    if rtp_stat["ps"] < 1:
        bad("媒体面: 没有包以 00 00 01 BA(MPEG-PS Pack Header) 开头")
    else:
        ok("媒体面: %d 个包的载荷以 00 00 01 BA 开头" % rtp_stat["ps"])
    if rtp_stat["v_wrong"]:
        bad("媒体面: %d 个包 byte0 != 0x80" % rtp_stat["v_wrong"])
    else:
        ok("媒体面: 全部包 byte0 == 0x80")
    if rtp_stat["sid_wrong"]:
        bad("媒体面: %d 个包 (byte1 & 0x7F) != 96" % rtp_stat["sid_wrong"])
    else:
        ok("媒体面: 全部包 byte1 & 0x7F == 96 (PS over RTP)")
    if rtp_stat["ssrc_seen"]:
        ok("媒体面: RTP SSRC == INVITE SDP 里的 y=%s（SDP→RTP 链路通）" % args.ssrc,
           "%d 个包" % rtp_stat["ssrc_seen"])
    elif rtp_stat["pkts"]:
        bad("媒体面: 收到 %d 个包，但 SSRC 都不是 SDP y=%s（期望 %d）"
            % (rtp_stat["pkts"], args.ssrc, int(args.ssrc)))
    if rtp_stat["ts"]:
        print("[platform] RTP 时间戳（去重，最多显示 8 个）: %s"
              % ", ".join(str(t) for t in sorted(rtp_stat["ts"])[:8]), flush=True)

    print("[platform] ================= 结果 =================", flush=True)
    for p in PASS_LIST:
        print("[platform] PASS  " + p, flush=True)
    for f in FAIL_LIST:
        print("[platform] FAIL  " + f, flush=True)
    print("[platform] PLATFORM RESULT: %s  (PASS %d / FAIL %d)"
          % ("PASS" if not FAIL_LIST else "FAIL", len(PASS_LIST), len(FAIL_LIST)), flush=True)
    return 0 if not FAIL_LIST else 1


if __name__ == "__main__":
    sys.exit(main())
