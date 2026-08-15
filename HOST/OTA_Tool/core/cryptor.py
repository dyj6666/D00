# core/cryptor.py

import os
import struct
from Crypto.Cipher import AES
from Crypto.Hash import SHA256
from ecdsa import SigningKey, NIST256p
from ecdsa.util import sigencode_string

def derive_aes_key_from_uid(uid_hex: str) -> bytes:
    """根据设备UID派生32字节AES密钥。
    与固件一致：UID hex 按 3 个 uint32 小端字节序排列（同 ymodem_sender 实现）。"""
    uid_ints = [int(uid_hex[i:i + 8], 16) for i in range(0, 24, 8)]
    uid_bytes = struct.pack("<III", *uid_ints)
    salt = b"OTA-AES-KEY-V1\x00"
    h = SHA256.new(uid_bytes + salt)
    return h.digest()

# ------------------------------------------------------------
# 与 TinyAES 完全一致的 CTR 加密（从原始脚本完整移植）
# ------------------------------------------------------------
def aes_ctr_encrypt(key: bytes, iv12: bytes, plain: bytes) -> bytes:
    """
    AES-256-CTR 加密，模拟 MCU TinyAES 行为：
    - 12字节 IV + 4字节零 → 16字节初始计数器
    - 128位大端自增
    - 使用 ECB 模式生成密钥流
    """
    iv16 = iv12 + b'\x00' * 4          # 扩展为 16 字节
    cipher = AES.new(key, AES.MODE_ECB)
    encrypted = bytearray()
    counter = bytearray(iv16)

    for i in range(0, len(plain), 16):
        block = plain[i:i+16]
        keystream = cipher.encrypt(bytes(counter))
        for j in range(len(block)):
            encrypted.append(block[j] ^ keystream[j])
        # 128位大端自增
        carry = 1
        for k in range(15, -1, -1):
            carry, counter[k] = divmod(counter[k] + carry, 256)

    return bytes(encrypted)
# ------------------------------------------------------------

def encrypt_and_sign(input_bin: str, output_bin: str, private_key_hex: str,
                     aes_key_hex: str = None, version: int = 1,
                     chip_id: int = 0x413, build_no: int = 1) -> None:
    """生成加密+签名的安全固件包"""
    with open(input_bin, 'rb') as f:
        plain = f.read()

    # AES 密钥
    aes_key = bytes.fromhex(aes_key_hex) if aes_key_hex else bytes(32)
    iv = os.urandom(12)

    # 使用与原始脚本完全一致的 aes_ctr_encrypt
    encrypted = aes_ctr_encrypt(aes_key, iv, plain)

    # 构造头部（32 字节）
    magic = 0x4F5441FE
    header = struct.pack('<III12sII', magic, version, len(plain), iv, chip_id, build_no)

    # SHA256 + 签名
    h = SHA256.new(header + encrypted)
    digest = h.digest()
    sk = SigningKey.from_string(bytes.fromhex(private_key_hex), curve=NIST256p)
    # 显式 64B 裸 r||s（设备 OTA_SIGN_SIZE=64，uECC 格式），
    # 不依赖 ecdsa 版本默认值，避免旧版 DER 编码导致验签失败
    signature = sk.sign_digest(digest, sigencode=sigencode_string)

    with open(output_bin, 'wb') as f:
        f.write(header + encrypted + signature)
