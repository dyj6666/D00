#!/usr/bin/env python3
"""生成 ECDSA P-256 签名密钥对（安全实践：私钥只输出到终端，绝不写入仓库）。

用法:
    python gen_keys.py

输出:
    私钥(hex)  -> 立即安全保管（密码管理器/HSM/离线保险柜）
    公钥(hex)  -> 写入 BOOT/BootServices/security.c 的 ECDSA_PUB_KEY

轮换流程:
    1) 运行本脚本生成新密钥对
    2) 当前 ECDSA_PUB_KEY 移入 ECDSA_PUB_KEY_LEGACY（兼容旧包）
    3) 新公钥写入 ECDSA_PUB_KEY
    4) 打包/升级工具私钥经环境变量 OTA_PRIVKEY 注入
    5) 过渡期后移除 LEGACY（旧密钥彻底作废）
"""

import secrets
from ecdsa import SigningKey, NIST256p


def main() -> int:
    sk = SigningKey.generate(curve=NIST256p, entropy=secrets.token_bytes)
    vk = sk.get_verifying_key()
    print("=" * 60)
    print("私钥(hex) - 立即安全保管，勿提交仓库/勿外泄:")
    print(sk.to_string().hex())
    print("=" * 60)
    print("公钥(64B X||Y) - 写入 security.c ECDSA_PUB_KEY:")
    xy = vk.to_string()
    print("  " + ", ".join("0x%02X" % b for b in xy))
    print("=" * 60)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
