# core/version_lib.py
"""固件版本库：登记/查询版本与构建号，构建号单调递增（防重放配套）。"""

import json
import os

LIB_PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "version_lib.json")

DEFAULT_LIB = {
    "chip_id": "0x413",          # STM32F405/407/415/417 系列
    "next_build": 1,
    "entries": [],
}


def load_lib() -> dict:
    if os.path.exists(LIB_PATH):
        try:
            with open(LIB_PATH, "r", encoding="utf-8") as f:
                return json.load(f)
        except (ValueError, OSError):
            pass
    return dict(DEFAULT_LIB)


def save_lib(lib: dict) -> None:
    with open(LIB_PATH, "w", encoding="utf-8") as f:
        json.dump(lib, f, ensure_ascii=False, indent=2)


def alloc_build_no(lib: dict) -> int:
    """分配并持久化下一个构建号（单调递增）。"""
    build = int(lib.get("next_build", 1))
    lib["next_build"] = build + 1
    save_lib(lib)
    return build


def add_entry(version: int, build: int, file_path: str, note: str = "") -> None:
    lib = load_lib()
    lib.setdefault("entries", [])
    lib["entries"].append({
        "version": version,
        "build": build,
        "file": file_path,
        "note": note,
        "date": __import__("datetime").date.today().isoformat(),
    })
    save_lib(lib)


def chip_id_int(lib: dict = None) -> int:
    lib = lib or load_lib()
    return int(str(lib.get("chip_id", "0x413")), 16)
