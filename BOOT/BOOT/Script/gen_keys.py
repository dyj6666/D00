from ecdsa import SigningKey, NIST256p

sk = SigningKey.generate(curve=NIST256p)
vk = sk.verifying_key

print("私钥 (hex，请妥善保管，切勿写入设备):", sk.to_string().hex())

# 获取未压缩公钥，去掉第一个字节 0x04
pub_key = vk.to_string('uncompressed')[1:]  # 64 字节 (X || Y)

print("\n// 公钥数组 (可直接粘贴到 security.c 的 ECDSA_PUB_KEY[64] 中):")
print("static const uint8_t ECDSA_PUB_KEY[64] = {")
# 每行打印 8 个字节
for i in range(0, 64, 8):
    line = ", ".join("0x{:02X}".format(pub_key[j]) for j in range(i, min(i+8, 64)))
    if i + 8 < 64:
        print("    " + line + ",")
    else:
        print("    " + line)
print("};")