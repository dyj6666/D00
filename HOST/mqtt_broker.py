#!/usr/bin/env python3
"""本机 MQTT broker（amqtt，纯 Python）——供直连开发板验证 MQTT 客户端。

用法：
    python mqtt_broker.py [port]      # 默认 1883
板端：`mqtt connect 192.168.10.201`。
"""

import asyncio
import sys

from amqtt.broker import Broker


async def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 1883
    broker = Broker()
    broker.config["listeners"]["default"].bind = "0.0.0.0:%d" % port
    print(f"[MQTT] broker on tcp 0.0.0.0:{port}", flush=True)
    await broker.start()
    try:
        await asyncio.Event().wait()
    finally:
        await broker.shutdown()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
