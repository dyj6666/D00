import sys
sys.path.insert(0, ".")

from core.ymodem_sender import YmodemSender

# 直接发送 Qt 生成的加密包
sender = YmodemSender(
    port="COM13",          # 根据你实际情况修改
    baudrate=115200,
    log_callback=lambda msg, color: print(f"[{color}] {msg}"),
    progress_callback=lambda p: print(f"进度: {p}%")
)

success = sender.send_file("temp_secure.bin")
if success:
    print("升级成功！")
else:
    print("升级失败！")