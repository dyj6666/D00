# -*- mode: python ; coding: utf-8 -*-
# PyInstaller 打包：pyinstaller d00term.spec
from PyInstaller.utils.hooks import collect_submodules

a = Analysis(
    ["d00term.py"],
    pathex=[],
    binaries=[],
    datas=[],
    hiddenimports=collect_submodules("serial"),
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name="D00Term",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=True,
)
collect = COLLECT(exe, a.binaries, a.datas, strip=False, upx=False, name="D00Term")
