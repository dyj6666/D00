"""VLinkClient 变量发现分片回归测试。

回归背景：client.py 曾只收 LIST_VARS 第一分片即返回，变量表
超一帧（约 247B）即静默丢变量。本测试用 FakeTransport 注入多帧，
验证 discover_variables 收齐全部变量并正确合并。
"""
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from vlink.client import VLinkClient
from vlink.protocol import Command

failures = 0


def check(cond, msg):
    global failures
    if cond:
        print(f"  ok: {msg}")
    else:
        print(f"  FAIL: {msg}")
        failures += 1


class FakeSerial:
    def reset_input_buffer(self):
        pass


class _DummyLock:
    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


class FakeQueue:
    def empty(self):
        return True

    def get_nowait(self):
        return None


class FakeTransport:
    """按预设列表逐次返回帧，模拟多分片 LIST_VARS 下发"""

    def __init__(self, frames):
        self.serial = FakeSerial()
        self.lock = _DummyLock()
        self.rx_buf = bytearray()
        self.rx_queue = FakeQueue()
        self._frames = list(frames)
        self.sent = []

    def send(self, cmd, payload=b""):
        self.sent.append((cmd, payload))

    def get_frame(self, timeout=0.01):
        if self._frames:
            return self._frames.pop(0)
        return None


def var_entry(vid, vtype, perm, name):
    """按固件 VarList_BuildPacket 条目编码"""
    return int(vid).to_bytes(2, "little") + bytes([vtype, perm]) + \
        bytes([len(name)]) + name.encode("ascii")


def list_vars_payload(total, index, entries):
    return bytes([total, index]) + b"".join(entries)


def test_multi_fragment_discovery():
    # 两个分片共 6 个变量（第二片有 3 个，旧实现会丢掉）
    p0 = list_vars_payload(2, 0, [
        var_entry(0x1001, 3, 0, "temp"),
        var_entry(0x1002, 3, 0, "humid"),
        var_entry(0x1003, 1, 0, "state"),
    ])
    p1 = list_vars_payload(2, 1, [
        var_entry(0x2001, 3, 0, "acc_x"),
        var_entry(0x2002, 3, 0, "acc_y"),
        var_entry(0x2003, 3, 0, "acc_z"),
    ])
    t = FakeTransport([(Command.LIST_VARS, p0), (Command.LIST_VARS, p1)])
    client = VLinkClient(t)
    vars_ = client.discover_variables()

    check(len(t.sent) == 1 and t.sent[0][0] == Command.LIST_VARS,
          "发送 LIST_VARS 请求")
    check(len(vars_) == 6, f"收集全部 6 个变量（实际 {len(vars_)}）")
    names = [v.name for v in vars_]
    check("acc_z" in names, "第二分片变量未被丢弃")


def test_single_fragment():
    p0 = list_vars_payload(1, 0, [
        var_entry(0x1001, 3, 0, "temp"),
    ])
    t = FakeTransport([(Command.LIST_VARS, p0)])
    client = VLinkClient(t)
    vars_ = client.discover_variables()
    check(len(vars_) == 1 and vars_[0].name == "temp", "单分片兼容")


def test_out_of_order_fragments():
    # 分片乱序到达（index 1 先到）也应正确排序合并
    p0 = list_vars_payload(2, 0, [var_entry(0x1001, 3, 0, "a")])
    p1 = list_vars_payload(2, 1, [var_entry(0x2001, 3, 0, "b")])
    t = FakeTransport([(Command.LIST_VARS, p1), (Command.LIST_VARS, p0)])
    client = VLinkClient(t)
    vars_ = client.discover_variables()
    check([v.name for v in vars_] == ["a", "b"], "乱序分片按 index 排序合并")


if __name__ == "__main__":
    print("== VLink 变量分片回归测试 ==")
    test_multi_fragment_discovery()
    test_single_fragment()
    test_out_of_order_fragments()
    if failures == 0:
        print("\nALL TESTS PASSED")
        sys.exit(0)
    print(f"\n{failures} TEST(S) FAILED")
    sys.exit(1)
