#!/usr/bin/env python3
"""CAN 总线监听/压测验证脚本（不入库）：PCAN-USB 监听并校验载荷序列。

用法：
    python _can_monitor.py <seconds> [<target_id_hex>]
退出码 0 = 未丢帧且序列完整；1 = 发现丢帧/序列错乱。
"""

import sys
import time

sys.path.insert(0, r"D:\GIT-SPACE\D00\HOST\D00Term")
import d00term as dt  # noqa: E402 复用 PCAN ctypes 封装


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
    target = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x300
    api = dt._PcanApi(dt._load_pcan_dll())
    rc = api.initialize()
    if rc != dt._PCAN_ERROR_OK:
        print(f"MON: init failed rc={rc}")
        return 1

    counts = {}
    seq_ok = 0
    seq_bad = 0
    next_seq = 0
    seen = set()
    t0 = time.time()
    while time.time() - t0 < seconds:
        r = api.read_raw()
        if r is None:
            time.sleep(0.001)
            continue
        mid, data = r
        counts[mid] = counts.get(mid, 0) + 1
        if mid == target and len(data) >= 8:
            i = data[0] | (data[1] << 8)
            if data[2] == 0xAA and data[3] == 0x55 and (data[4] & 0xFF) == (~i & 0xFF):
                seq_ok += 1
                if i in seen:
                    seq_bad += 1
                seen.add(i)
            else:
                seq_bad += 1
    api.uninitialize()

    print(f"MON: {seconds:.0f}s window, frames by ID: "
          + ", ".join(f"0x{mid:03X}={n}" for mid, n in sorted(counts.items())))
    if target in counts:
        n = counts[target]
        print(f"MON: target 0x{target:03X} total={n} seq_ok={seq_ok} seq_bad={seq_bad}")
        missing = [i for i in range(0, max(seen) + 1) if i not in seen] if seen else []
        if missing:
            print(f"MON: missing seqs: {missing[:20]}{'...' if len(missing) > 20 else ''}")
        return 0 if n > 0 and seq_bad == 0 else 1
    print("MON: target ID not seen")
    return 1


if __name__ == "__main__":
    sys.exit(main())
