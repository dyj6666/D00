# -*- coding: utf-8 -*-
"""OTA_Tool 加密打包单测：cryptor 包格式与固件端验签契约一致性。

覆盖：
  1. AES-256-CTR 加解密往返（与 TinyAES 大端计数器语义一致）
  2. 包头 32B 布局（<III12sII：magic/version/size/iv/chip_id/build_no）
  3. ECDSA 签名为 64B 裸 r||s（设备 OTA_SIGN_SIZE=64，uECC 格式）
  4. 签名覆盖 header+密文，验签通过；篡改任一字节验签失败
  5. UID 派生密钥确定性（与固件盐值/字节序一致）
运行：python tests/test_cryptor.py
"""

import hashlib
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from core.cryptor import (aes_ctr_encrypt, derive_aes_key_from_uid,
                          encrypt_and_sign)
from ecdsa import SigningKey, NIST256p
from ecdsa.util import sigdecode_string

FAILURES = 0


def check(cond, msg):
    global FAILURES
    if cond:
        print(f"  ok: {msg}")
    else:
        FAILURES += 1
        print(f"  FAIL: {msg}")


def test_ctr_roundtrip():
    print("[test] AES-256-CTR 往返")
    key = bytes(range(32))
    iv = b"\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c"
    plain = b"hello stm32f407 secure ota " * 8
    enc = aes_ctr_encrypt(key, iv, plain)
    dec = aes_ctr_encrypt(key, iv, enc)   # CTR 加解密同构
    check(dec == plain, "CTR 加解密往返一致")
    check(enc != plain, "密文与明文不同")
    check(len(enc) == len(plain), "密文长度不变")


def test_uid_key_derivation():
    print("[test] UID 派生密钥确定性")
    uid = "001E0038434F313330363631"   # 12 字节 UID hex（24 字符）
    k1 = derive_aes_key_from_uid(uid)
    k2 = derive_aes_key_from_uid(uid)
    check(k1 == k2 and len(k1) == 32, "同 UID 派生 32B 密钥且确定")
    # 与固件一致：UID 按 3×uint32 小端 + 盐 OTA-AES-KEY-V1\0 做 SHA256
    uid_ints = [int(uid[i:i + 8], 16) for i in range(0, 24, 8)]
    uid_bytes = struct.pack("<III", *uid_ints)
    expect = hashlib.sha256(uid_bytes + b"OTA-AES-KEY-V1\x00").digest()
    check(k1 == expect, "派生公式与固件逐字节一致")


def test_package_format_and_signature():
    print("[test] 打包格式 + 64B 裸签名契约")
    sk = SigningKey.generate(curve=NIST256p)
    priv = sk.to_string().hex()
    vk = sk.get_verifying_key()

    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "firmware.bin")
        pkg = os.path.join(td, "secure.bin")
        payload = os.urandom(4096)
        with open(src, "wb") as f:
            f.write(payload)

        key = bytes(range(32)).hex()
        iv = None
        # 固定 IV 便于可复现验证：encrypt_and_sign 内部随机 IV，
        # 这里解析包后按包内 IV 解密验证
        encrypt_and_sign(src, pkg, priv, aes_key_hex=key,
                         version=213, chip_id=0x413, build_no=9156)

        data = open(pkg, "rb").read()
        check(len(data) == 32 + 4096 + 64, "包长 = 32B 头 + 密文 + 64B 签名")

        magic, version, size, iv12, chip, build = struct.unpack(
            "<III12sII", data[:32])
        check(magic == 0x4F5441FE, "包头魔数 0x4F5441FE")
        check(version == 213, "版本字段")
        check(size == 4096, "固件大小字段")
        check(chip == 0x413, "chip_id 字段")
        check(build == 9156, "build_no 字段")

        # 解密验证
        ciphertext = data[32:32 + 4096]
        check(len(ciphertext) == size, "密文长度 == firmware_size")
        plain = aes_ctr_encrypt(bytes(range(32)), iv12, ciphertext)
        check(plain == payload, "用包内 IV 解密还原原始固件")

        # 签名：64B 裸 r||s，覆盖 header+密文
        sig = data[32 + 4096:]
        check(len(sig) == 64, "签名为 64 字节裸 r||s（设备 OTA_SIGN_SIZE=64）")
        digest = hashlib.sha256(data[:32 + 4096]).digest()
        # 设备端 uECC 验签语义：64B 裸 r||s 直接验 SHA256 摘要
        check(vk.verify_digest(sig, digest, sigdecode=sigdecode_string),
              "签名验证通过（64B 裸 r||s，uECC 兼容）")
        tampered = bytearray(digest)
        tampered[0] ^= 0xFF
        try:
            vk.verify_digest(sig, bytes(tampered), sigdecode=sigdecode_string)
            tamper_rejected = False
        except Exception:
            tamper_rejected = True
        check(tamper_rejected, "篡改摘要后验签失败")


def main():
    test_ctr_roundtrip()
    test_uid_key_derivation()
    test_package_format_and_signature()
    if FAILURES:
        print(f"\nRESULT: {FAILURES} failure(s)")
        sys.exit(1)
    print("\nRESULT: all passed")


if __name__ == "__main__":
    main()
