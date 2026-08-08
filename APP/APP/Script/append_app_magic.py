#!/usr/bin/env python3
"""为直烧生成带 APP 有效性魔数的完整镜像。

背景：BOOT 通过 0x0805FFF8 处的魔数 (0x4F54412E) 判断 APP 是否有效，
      该魔数平时由 OTA 流程写入。直接烧录（SWD/Keil）时必须先用本脚本
      把魔数与版本号补齐，否则 BOOT 会判定 APP 无效而进入升级模式。

用法：
    python append_app_magic.py <raw_app.bin> [--version N] [--out out.bin]

输出：320 KB 的完整 APP 分区镜像（RUN 区），直接烧录到 0x08010000 即可。
"""

import argparse
import struct
from pathlib import Path

APP_PARTITION_SIZE = 320 * 1024          # 0x50000 (RUN 区, 扇区4-6)
APP_VALID_OFFSET = APP_PARTITION_SIZE - 8
APP_VALID_MAGIC = 0x4F54412E


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="原始 APP.bin（不带魔数）")
    parser.add_argument("--version", type=int, default=1, help="固件版本号（默认 1）")
    parser.add_argument("--out", type=Path, default=None, help="输出文件名（默认 APP_flash.bin）")
    args = parser.parse_args()

    raw = args.input.read_bytes()
    if len(raw) >= APP_VALID_OFFSET:
        parser.error(f"APP 镜像过大（{len(raw)} 字节），超过魔数区偏移")

    image = bytearray(b"\xFF" * APP_PARTITION_SIZE)
    image[0:len(raw)] = raw
    struct.pack_into("<I", image, APP_VALID_OFFSET, APP_VALID_MAGIC)
    struct.pack_into("<I", image, APP_VALID_OFFSET + 4, args.version)

    out = args.out or args.input.with_name("APP_flash.bin")
    out.write_bytes(image)
    print(f"[OK] {out} 生成完毕")
    print(f"     镜像大小: {len(image)} 字节 (烧录地址 0x08010000)")
    print(f"     有效代码: {len(raw)} 字节")
    print(f"     魔数@0x{APP_VALID_OFFSET:06X} = 0x{APP_VALID_MAGIC:08X}")
    print(f"     版本@0x{APP_VALID_OFFSET + 4:06X} = {args.version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
