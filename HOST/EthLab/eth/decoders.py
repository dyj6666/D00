# -*- coding: utf-8 -*-
"""Ethernet 帧协议解码引擎。

支持: Ethernet II / 802.1Q VLAN / ARP / IPv4 / IPv6 / ICMP / TCP / UDP。
每个字段携带: 名称/绝对偏移/长度/值/说明/所属层。偏移基于整帧起点，
可直接用于逐字节结构图渲染。IPv4/TCP/UDP/ICMP 校验和逐帧验算。
"""

from dataclasses import dataclass, field


@dataclass
class Field:
    name: str
    offset: int
    length: int
    value: str
    desc: str = ""
    layer: str = ""


@dataclass
class Frame:
    direction: str            # "TX" / "RX"
    length: int               # 原始帧长
    raw: bytes                # 实际收到的字节
    orig_len: int = 0         # 板端原始帧长（截断时 > len(raw)）
    truncated: bool = False
    ts: float = 0.0
    fields: list = field(default_factory=list)
    summary: str = ""
    proto: str = "?"
    l4: str = ""
    src: str = ""
    dst: str = ""

    def field(self, name):
        for f in self.fields:
            if f.name == name:
                return f
        return None


# ---------------- 基础工具 ----------------

def mac(b, off=0):
    return ":".join("%02X" % x for x in b[off:off + 6])


def ip4(b, off=0):
    return ".".join(str(x) for x in b[off:off + 4])


def ip6(b, off=0):
    return ":".join("%x" % ((b[off + i] << 8) | b[off + i + 1])
                    for i in range(0, 16, 2))


def csum(data):
    """Internet 校验和（16 位反码和）。"""
    if len(data) % 2:
        data += b"\x00"
    s = sum(((data[i] << 8) | data[i + 1]) for i in range(0, len(data), 2))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


PORT_NAMES = {
    7: "echo", 9: "discard", 20: "ftp-data", 21: "ftp", 22: "ssh",
    23: "telnet", 25: "smtp", 53: "dns", 67: "dhcp-server", 68: "dhcp-client",
    69: "tftp", 80: "http", 110: "pop3", 123: "ntp", 143: "imap",
    161: "snmp", 443: "https", 514: "syslog", 1900: "ssdp",
    7777: "d00-echo", 7778: "d00-capture", 8080: "http-alt",
    9000: "d00-console",
}

ICMP_TYPES = {
    0: "Echo Reply", 3: "Destination Unreachable", 4: "Source Quench",
    5: "Redirect", 8: "Echo Request", 11: "Time Exceeded",
    12: "Parameter Problem", 13: "Timestamp", 14: "Timestamp Reply",
}


def port_name(p):
    if p in PORT_NAMES:
        return "%u (%s)" % (p, PORT_NAMES[p])
    return str(p)


def ip4_bytes(s):
    return bytes(int(x) for x in s.split("."))


def l4_checksum(src_ip, dst_ip, proto, seg):
    """IPv4 L4 校验和: 伪首部(12B) + 段（校验和字段清零）。"""
    pseudo = ip4_bytes(src_ip) + ip4_bytes(dst_ip) + bytes([0, proto])
    pseudo += len(seg).to_bytes(2, "big")
    return csum(pseudo + seg)


def l4_checksum_v6(src6, dst6, proto, seg):
    """IPv6 L4 校验和: 128 位伪首部 + 段。"""
    pseudo = src6 + dst6 + len(seg).to_bytes(4, "big")
    pseudo += bytes([0, 0, 0, proto]) + seg
    return csum(pseudo)


LAYER_COLORS = {
    "Ethernet II": "#8ec9ff",
    "VLAN": "#ffd28e",
    "ARP": "#a4e6a4",
    "IPv4": "#ffd98e",
    "IPv6": "#ffcf9e",
    "ICMP": "#ff9e9e",
    "TCP": "#c2b8ff",
    "UDP": "#8fe0e0",
    "Payload": "#9aa5b5",
}


def layer_color(layer):
    return LAYER_COLORS.get(layer, "#9aa5b5")


# ---------------- 各层解析 ----------------

def parse_ethernet(fr, b):
    fr.dst = mac(b, 0)
    fr.src = mac(b, 6)
    etype = (b[12] << 8) | b[13]
    fr.fields.append(Field("目的 MAC", 0, 6, fr.dst,
                           "Destination Address", "Ethernet II"))
    fr.fields.append(Field("源 MAC", 6, 6, fr.src,
                           "Source Address", "Ethernet II"))
    fr.fields.append(Field("EtherType", 12, 2, "0x%04X" % etype,
                           {0x0800: "IPv4", 0x0806: "ARP", 0x86DD: "IPv6",
                            0x8100: "VLAN", 0x88CC: "LLDP"}.get(etype, "?"),
                           "Ethernet II"))
    off = 14
    if etype == 0x0800:
        fr.proto = "IPv4"
        parse_ipv4(fr, b, off)
    elif etype == 0x0806:
        fr.proto = "ARP"
        parse_arp(fr, b, off)
    elif etype == 0x86DD:
        fr.proto = "IPv6"
        parse_ipv6(fr, b, off)
    elif etype == 0x8100 and len(b) >= 18:
        tci = (b[14] << 8) | b[15]
        fr.fields.append(Field("VLAN TCI", 14, 2, "0x%04X" % tci,
                               "PCP=%d DEI=%d VID=%d" % (tci >> 13,
                                                         (tci >> 12) & 1,
                                                         tci & 0xFFF),
                               "VLAN"))
        inner = (b[16] << 8) | b[17]
        fr.fields.append(Field("内层 EtherType", 16, 2, "0x%04X" % inner,
                               "IPv4/ARP/IPv6", "VLAN"))
        off = 18
        if inner == 0x0800:
            fr.proto = "IPv4"
            parse_ipv4(fr, b, off)
        elif inner == 0x0806:
            fr.proto = "ARP"
            parse_arp(fr, b, off)
        elif inner == 0x86DD:
            fr.proto = "IPv6"
            parse_ipv6(fr, b, off)
        else:
            fr.summary = "VLAN EtherType 0x%04X" % inner
    else:
        fr.summary = "EtherType 0x%04X (%dB)" % (etype, len(b))


def parse_arp(fr, b, off):
    htype = (b[off] << 8) | b[off + 1]
    ptype = (b[off + 2] << 8) | b[off + 3]
    hlen = b[off + 4]
    plen = b[off + 5]
    op = (b[off + 6] << 8) | b[off + 7]
    fr.fields.append(Field("硬件类型", off, 2, "%d" % htype,
                           {1: "Ethernet"}.get(htype, "?"), "ARP"))
    fr.fields.append(Field("协议类型", off + 2, 2, "0x%04X" % ptype,
                           "0x0800=IPv4", "ARP"))
    fr.fields.append(Field("硬件/协议长度", off + 4, 2, "%d/%d" % (hlen, plen),
                           "", "ARP"))
    op_s = "Request" if op == 1 else ("Reply" if op == 2 else "op=%d" % op)
    fr.fields.append(Field("操作码", off + 6, 2, "%d" % op, op_s, "ARP"))
    sha = mac(b, off + 8)
    spa = ip4(b, off + 14)
    tha = mac(b, off + 18)
    tpa = ip4(b, off + 24)
    fr.fields.append(Field("发送方 MAC", off + 8, 6, sha, "", "ARP"))
    fr.fields.append(Field("发送方 IP", off + 14, 4, spa, "", "ARP"))
    fr.fields.append(Field("目标 MAC", off + 18, 6, tha, "", "ARP"))
    fr.fields.append(Field("目标 IP", off + 24, 4, tpa, "", "ARP"))
    fr.src = spa
    fr.dst = tpa
    fr.summary = "ARP %s %s -> %s" % (op_s, spa, tpa)


def parse_ipv4(fr, b, off):
    ver = b[off] >> 4
    ihl = (b[off] & 0x0F) * 4
    tos = b[off + 1]
    total = (b[off + 2] << 8) | b[off + 3]
    ident = (b[off + 4] << 8) | b[off + 5]
    flags_frag = (b[off + 6] << 8) | b[off + 7]
    ttl = b[off + 8]
    proto = b[off + 9]
    hdr_chk = (b[off + 10] << 8) | b[off + 11]
    src = ip4(b, off + 12)
    dst = ip4(b, off + 16)

    fr.src = src
    fr.dst = dst
    fr.fields.append(Field("版本/IHL", off, 1, "%d/%d (%dB)" % (ver, ihl // 4,
                                                                ihl),
                           "IPv4 / Header Length", "IPv4"))
    fr.fields.append(Field("TOS/DSCP", off + 1, 1, "0x%02X" % tos,
                           "DSCP=%d ECN=%d" % (tos >> 2, tos & 3), "IPv4"))
    fr.fields.append(Field("总长度", off + 2, 2, "%d" % total, "", "IPv4"))
    fr.fields.append(Field("标识", off + 4, 2, "0x%04X" % ident, "", "IPv4"))
    df = "DF" if flags_frag & 0x4000 else ""
    mf = "MF" if flags_frag & 0x2000 else ""
    fr.fields.append(Field("标志/片偏移", off + 6, 2, "0x%04X" % flags_frag,
                           ("%s %s" % (df, mf)).strip() or "0", "IPv4"))
    fr.fields.append(Field("TTL", off + 8, 1, "%d" % ttl, "", "IPv4"))
    fr.fields.append(Field("协议", off + 9, 1, "%d" % proto,
                           {1: "ICMP", 6: "TCP", 17: "UDP"}.get(proto, "?"),
                           "IPv4"))
    calc = csum(b[off:off + ihl].replace(b[off + 10:off + 12], b"\x00\x00"))
    ok = (calc == hdr_chk)
    fr.fields.append(Field("头部校验和", off + 10, 2, "0x%04X" % hdr_chk,
                           "计算 0x%04X - %s" % (calc, "OK" if ok else "FAIL"),
                           "IPv4"))
    fr.fields.append(Field("源地址", off + 12, 4, src, "", "IPv4"))
    fr.fields.append(Field("目的地址", off + 16, 4, dst, "", "IPv4"))
    if ihl > 20:
        fr.fields.append(Field("选项", off + 20, ihl - 20,
                               b[off + 20:off + ihl].hex(" "),
                               "%dB" % (ihl - 20), "IPv4"))

    l4off = off + ihl
    l4len = max(total - ihl, 0)
    if l4off + l4len > len(b):
        l4len = max(len(b) - l4off, 0)
    if proto == 1:
        fr.l4 = "ICMP"
        parse_icmp(fr, b, l4off, l4len)
    elif proto == 6:
        fr.l4 = "TCP"
        parse_tcp(fr, b, l4off, src, dst, seglen=l4len)
    elif proto == 17:
        fr.l4 = "UDP"
        parse_udp(fr, b, l4off, src, dst, seglen=l4len)
    else:
        fr.summary = "IPv4 %s -> %s proto=%d" % (src, dst, proto)


def parse_ipv6(fr, b, off):
    vtf = int.from_bytes(b[off:off + 4], "big")
    plen = int.from_bytes(b[off + 4:off + 6], "big")
    nh = b[off + 6]
    hop = b[off + 7]
    src = ip6(b, off + 8)
    dst = ip6(b, off + 24)
    fr.src = src
    fr.dst = dst
    fr.fields.append(Field("版本/流标签", off, 4, "0x%08X" % vtf,
                           "Ver=%d TC=%d Flow=%d" % (vtf >> 28,
                                                     (vtf >> 20) & 0xFF,
                                                     vtf & 0xFFFFF),
                           "IPv6"))
    fr.fields.append(Field("载荷长度", off + 4, 2, "%d" % plen, "", "IPv6"))
    fr.fields.append(Field("下一头部", off + 6, 1, "%d" % nh,
                           {1: "ICMPv6", 6: "TCP", 17: "UDP"}.get(nh, "?"),
                           "IPv6"))
    fr.fields.append(Field("跳数限制", off + 7, 1, "%d" % hop, "", "IPv6"))
    fr.fields.append(Field("源地址", off + 8, 16, src, "", "IPv6"))
    fr.fields.append(Field("目的地址", off + 24, 16, dst, "", "IPv6"))
    l4off = off + 40
    l4len = max(plen, 0)
    if l4off + l4len > len(b):
        l4len = max(len(b) - l4off, 0)
    if nh == 6:
        fr.l4 = "TCP"
        parse_tcp(fr, b, l4off, src, dst, v6=True, seglen=l4len)
    elif nh == 17:
        fr.l4 = "UDP"
        parse_udp(fr, b, l4off, src, dst, v6=True, seglen=l4len)
    else:
        fr.summary = "IPv6 %s -> %s nh=%d" % (src, dst, nh)


def parse_icmp(fr, b, off, seglen=None):
    typ = b[off]
    code = b[off + 1]
    chk = (b[off + 2] << 8) | b[off + 3]
    if seglen is None:
        seg = bytearray(b[off:])
    else:
        seg = bytearray(b[off:off + seglen])
    if len(seg) >= 4:
        seg[2:4] = b"\x00\x00"
    calc = csum(bytes(seg))
    fr.fields.append(Field("类型", off, 1, "%d" % typ,
                           ICMP_TYPES.get(typ, "?"), "ICMP"))
    fr.fields.append(Field("代码", off + 1, 1, "%d" % code, "", "ICMP"))
    fr.fields.append(Field("校验和", off + 2, 2, "0x%04X" % chk,
                           "计算 0x%04X - %s" % (calc, "OK" if calc == chk
                                                 else "FAIL"), "ICMP"))
    if typ in (0, 8):
        ident = (b[off + 4] << 8) | b[off + 5]
        seq = (b[off + 6] << 8) | b[off + 7]
        fr.fields.append(Field("标识", off + 4, 2, "0x%04X" % ident, "", "ICMP"))
        fr.fields.append(Field("序号", off + 6, 2, "%d" % seq, "", "ICMP"))
        fr.summary = "ICMP %s id=0x%04X seq=%d" % (ICMP_TYPES.get(typ, typ),
                                                   ident, seq)
    else:
        fr.summary = "ICMP %s (%d/%d)" % (ICMP_TYPES.get(typ, "?"), typ, code)


TCP_FLAGS = (("FIN", 0x01), ("SYN", 0x02), ("RST", 0x04), ("PSH", 0x08),
             ("ACK", 0x10), ("URG", 0x20), ("ECE", 0x40), ("CWR", 0x80))


def parse_tcp(fr, b, off, src_ip, dst_ip, v6=False, seglen=None):
    sport = (b[off] << 8) | b[off + 1]
    dport = (b[off + 2] << 8) | b[off + 3]
    seq = int.from_bytes(b[off + 4:off + 8], "big")
    ack = int.from_bytes(b[off + 8:off + 12], "big")
    data_off = (b[off + 12] >> 4) * 4
    flags = b[off + 13]
    win = (b[off + 14] << 8) | b[off + 15]
    chk = (b[off + 16] << 8) | b[off + 17]
    urg = (b[off + 18] << 8) | b[off + 19]
    flag_names = [n for n, m in TCP_FLAGS if flags & m]
    fr.fields.append(Field("源端口", off, 2, port_name(sport), "", "TCP"))
    fr.fields.append(Field("目的端口", off + 2, 2, port_name(dport), "", "TCP"))
    fr.fields.append(Field("序号", off + 4, 4, str(seq), "", "TCP"))
    fr.fields.append(Field("确认号", off + 8, 4, str(ack), "", "TCP"))
    fr.fields.append(Field("头部长度", off + 12, 1, "%d (%dB)" % (data_off // 4,
                                                                 data_off),
                           "", "TCP"))
    fr.fields.append(Field("标志", off + 13, 2, "0x%04X" % flags,
                           " ".join(flag_names) or "-", "TCP"))
    fr.fields.append(Field("窗口", off + 14, 2, "%d" % win, "", "TCP"))
    if seglen is None:
        seg = bytearray(b[off:])
    else:
        seg = bytearray(b[off:off + seglen])
    if len(seg) >= 18:
        seg[16:18] = b"\x00\x00"
    calc = (l4_checksum_v6(src_ip, dst_ip, 6, bytes(seg)) if v6
            else l4_checksum(src_ip, dst_ip, 6, bytes(seg)))
    fr.fields.append(Field("校验和", off + 16, 2, "0x%04X" % chk,
                           "计算 0x%04X - %s" % (calc, "OK" if calc == chk
                                                 else "FAIL"), "TCP"))
    fr.fields.append(Field("紧急指针", off + 18, 2, "%d" % urg, "", "TCP"))
    if data_off > 20 and len(b) >= off + data_off:
        parse_tcp_options(fr, b, off + 20, data_off - 20)
    fr.summary = "TCP %s:%s -> %s:%s %s" % (src_ip, sport, dst_ip, dport,
                                            " ".join(flag_names) or "-")


def parse_tcp_options(fr, b, off, length):
    opts = []
    i = 0
    while i < length:
        kind = b[off + i]
        if kind == 0:
            opts.append("EOL")
            break
        if kind == 1:
            opts.append("NOP")
            i += 1
            continue
        if i + 1 >= length:
            break
        olen = b[off + i + 1]
        if olen < 2 or i + olen > length:
            break
        val = b[off + i:off + i + olen]
        if kind == 2 and olen == 4:
            opts.append("MSS=%d" % int.from_bytes(val[2:4], "big"))
        elif kind == 3 and olen == 3:
            opts.append("WS=%d" % val[2])
        elif kind == 4:
            opts.append("SACK_OK")
        elif kind == 5:
            opts.append("SACK")
        elif kind == 8 and olen == 10:
            opts.append("TS=%d/%d" % (int.from_bytes(val[2:6], "big"),
                                      int.from_bytes(val[6:10], "big")))
        else:
            opts.append("K%d" % kind)
        i += olen
    if opts:
        fr.fields.append(Field("TCP 选项", off, length, "; ".join(opts),
                               "%dB" % length, "TCP"))


def parse_udp(fr, b, off, src_ip, dst_ip, v6=False, seglen=None):
    sport = (b[off] << 8) | b[off + 1]
    dport = (b[off + 2] << 8) | b[off + 3]
    ulen = (b[off + 4] << 8) | b[off + 5]
    chk = (b[off + 6] << 8) | b[off + 7]
    fr.fields.append(Field("源端口", off, 2, port_name(sport), "", "UDP"))
    fr.fields.append(Field("目的端口", off + 2, 2, port_name(dport), "", "UDP"))
    fr.fields.append(Field("长度", off + 4, 2, "%d" % ulen, "", "UDP"))
    if chk == 0:
        chk_s = "0x0000 (IPv4 可选)"
    else:
        seg_end = (off + ulen) if seglen is None else (off + min(ulen, seglen))
        seg = bytearray(b[off:seg_end])
        if len(seg) >= 8:
            seg[6:8] = b"\x00\x00"
        calc = (l4_checksum_v6(src_ip, dst_ip, 17, bytes(seg)) if v6
                else l4_checksum(src_ip, dst_ip, 17, bytes(seg)))
        chk_s = "0x%04X (计算 0x%04X - %s)" % (chk, calc,
                                               "OK" if calc == chk else "FAIL")
    fr.fields.append(Field("校验和", off + 6, 2, "0x%04X" % chk, chk_s, "UDP"))
    fr.summary = "UDP %s:%s -> %s:%s len=%d" % (src_ip, sport, dst_ip, dport,
                                                ulen)


# ---------------- 入口 ----------------

def decode(direction, raw, orig_len=None, truncated=False, ts=0.0):
    """解析一帧。direction='TX'/'RX'，raw=帧字节。
    orig_len/truncated 由板端抓帧头给出（截断时 orig_len > len(raw)）。"""
    orig = orig_len if orig_len else len(raw)
    fr = Frame(direction=direction, length=orig, raw=raw,
               orig_len=orig, truncated=truncated, ts=ts)
    if len(raw) < 14:
        fr.summary = "短帧 (%d 字节)" % len(raw)
        return fr
    parse_ethernet(fr, raw)
    return fr
