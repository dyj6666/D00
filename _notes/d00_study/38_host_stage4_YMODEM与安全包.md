# D00 源码研学 · 笔记 38 —— YMODEM 发送器与安全包构建

> 精读对象：`HOST/OTA_Tool/core/ymodem_sender.py`(408)
> 阶段 4 第 3 批：与 BOOT ymodem.c 状态机**逐状态对称**的发送器 + 加密签名

---

## 一、⭐ 安全包构建（encrypt_and_sign :100-120）—— 与 BOOT 六道关对称

```python
aes_key = derive_aes_key_from_uid(uid_hex)   # SHA256(UID 12B LE + salt "OTA-AES-KEY-V1\x00")
iv = os.urandom(12)
encrypted = aes_ctr_encrypt(aes_key, iv, plain)   # ECB 加密 counter 流（手工 CTR）
header = struct.pack('<III12sII', magic=0x4F5441FE, version, len(plain), iv, chip_id, build_no)
digest = SHA256(header + encrypted)
signature = sk.sign_digest(digest, sigencode=sigencode_string)  # ECDSA secp256r1 显式 64B r||s
→ header + encrypted + signature（OTA_SIGN_SIZE=64，uECC 格式）
```

**密钥对称**：
| 侧 | 密钥 | 用途 |
| --- | --- | --- |
| HOST | 私钥（离线保管不入库）+ UID | 签名 + 派生 AES 密钥 |
| BOOT | 双公钥（新+LEGACY）+ UID | 验签 + 同款派生（security.c） |

**手工 AES-CTR**（:84-97）：ECB 加密 16B counter（iv16 = iv12 + 4B 零）+ 逐字节异或 + 大端进位——与 BOOT aes.c 的 CTR 模式等价（注意 counter 进位方向需与固件端一致，已实测验证）。

## 二、⭐ YMODEM 发送器状态机（12 态，与 BOOT 对称）

```
WAIT_C → SEND_FILE_INFO → WAIT_ACK_C → SEND_DATA → ... → SEND_EOT_FIRST
  → WAIT_NAK（等 NAK）→ SEND_EOT_SECOND → WAIT_ACK_C2（等 ACK+'C'）
  → SEND_END_FRAME → WAIT_ACK_END → DONE
```

| 对应 BOOT 端 | 发送器实现 |
| --- | --- |
| 'C'×5 握手 | WAIT_C（10s 超时） |
| 文件信息帧解析（filename\0 size HEX crc HEX） | `f"{name}\0 0x{size:X} 0x{crc:X}\0"` + 128B 填充（:181-189） |
| **帧 CRC32**（非标准 CRC16） | 查表 CRC32 + final xor（:69-73）——**与 BOOT 同款** |
| 数据帧 STX 1KB | seq + ~seq + 1KB + CRC32 LE |
| 重复帧 ACK | 发送器按 seq 递增（0xFF 回绕跳过 0） |
| 双 EOT + 结束帧 | 完整对称（EOT→NAK→EOT→ACK+'C'→结束帧→ACK） |
| 重试 | MAX_RETRY=5（BOOT 端 10） |
| 结束帧未确认 | **超时仍判 DONE**（传输可能已完成，:406-407） |

## 三、设计要点

1. **回调解耦**：log_callback / progress_callback——QT 集成（复用已验证命令行脚本核心）
2. **序列 0xFF 回绕**（:297-298）：seq 到 0 跳 1（与 BOOT 期望一致）
3. **尾部补零**（:336-337）：不足 1KB 补 0（BOOT 端有 padding 处理）
4. **INTER_BYTE_DELAY 5ms**：帧间间隔（低速场景稳健）
5. **文件信息 128B 上限**：超长抛 ValueError

## 四、设计亮点

1. **协议全对称**：帧格式/CRC/握手序列/重试语义与 BOOT ymodem.c 逐状态对应——"协议是双端的契约"
2. **安全包三要素**：AES-CTR（UID 派生密钥）+ ECDSA（64B 裸签名）+ build_no 防重放
3. **手工 CTR 与固件等价**：counter 进位方向与固件一致的实测验证
4. **容错收尾**：结束帧未确认判完成（双端不对称容忍）

## 五、待读清单（下一课）

- [x] ymodem_sender（本轮完成）
- [ ] `hostlink.py`：HOSTLINK 帧构建/解析（与 APP protocol.h 对应）
- [ ] `version_lib.py` + config/version.json：版本单一事实源
- [ ] VLink_Debugger（变量/波形）/ LogicAnalyzer（解码器 decoders.py）/ EthLab / D00Term

## 六、自测题

1. 安全包的三个组成部分？（header + 密文 + 签名）
2. 手工 AES-CTR 怎么实现？（ECB 加密 counter + 异或 + 进位）
3. 为什么用显式 64B r||s 签名？（uECC 格式 OTA_SIGN_SIZE=64）
4. YMODEM 帧 CRC 用什么？（CRC32 查表 + final xor，非标准 CRC16）
5. 双 EOT 握手的过程？（EOT→NAK→EOT→ACK+'C'）
6. 结束帧未确认为什么判完成？（传输可能已完成）
7. 重试上限？（发送器 5 vs BOOT 10）
8. 密钥体系怎么对称？（HOST 私钥+UID / BOOT 公钥+UID）
