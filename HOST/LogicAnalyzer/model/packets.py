"""解码结果容器"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import List

from la.decoders import Packet


@dataclass
class DecodeResult:
    protocol: str
    packets: List[Packet] = field(default_factory=list)
    note: str = ""

    def summary(self) -> List[dict]:
        return [
            {
                "kind": p.kind,
                "start": p.start,
                "end": p.end,
                "data": p.data.hex(" ") if p.data else "",
                "info": p.info,
            }
            for p in self.packets
        ]
