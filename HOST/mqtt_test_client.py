#!/usr/bin/env python3
"""MQTT 双向链路测试客户端（amqtt）：订阅 d00/#，发布 d00/cmd。

配合板端：
    python mqtt_test_client.py [broker_ip] [port]
    - 订阅 d00/# 收板端发布（含周期遥测 d00/status）；
    - 5s 后向 d00/cmd 发布一条消息，验证板端订阅回调。
"""

import asyncio
import sys

from amqtt.client import MQTTClient
from amqtt.mqtt.constants import QOS_1


async def main():
    ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.10.201"
    port = sys.argv[2] if len(sys.argv) > 2 else "1883"
    client = MQTTClient()
    await client.connect(f"mqtt://{ip}:{port}/")
    await client.subscribe([("d00/#", QOS_1)])
    print(f"[PC] subscribed d00/# on {ip}:{port}", flush=True)

    messages = []

    async def collect():
        while True:
            msg = await client.deliver_message()
            messages.append((msg.topic, msg.data.decode("utf-8", "replace")))
            print(f"[PC] recv {msg.topic}: {messages[-1][1]}", flush=True)

    task = asyncio.ensure_future(collect())
    await asyncio.sleep(5)
    await client.publish("d00/cmd", b"hello-from-pc", qos=0)
    print("[PC] published d00/cmd: hello-from-pc", flush=True)
    await asyncio.sleep(8)
    task.cancel()
    await client.disconnect()
    print(f"[PC] total received: {len(messages)}", flush=True)


if __name__ == "__main__":
    asyncio.run(main())
