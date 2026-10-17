# ================================================================
# idf.ps1 -- ESP-IDF one-shot environment wrapper (project local)
#
# Usage (run in the project directory):
#   .\idf.ps1 build                  # build
#   .\idf.ps1 -p COM9  flash         # flash
#   .\idf.ps1 -p COM9  monitor       # serial monitor (quit: Ctrl+])
#   .\idf.ps1 -p COM9  flash monitor # flash then monitor
#   .\idf.ps1 menuconfig             # sdkconfig menu
#   .\idf.ps1 fullclean              # clean everything
#
# It works around two classic traps of ESP-IDF on a Chinese Windows host:
#   1) export.ps1 picks the system Python 3.12 (D:\Python) by default and then
#      fails with "python_env\idf5.2_py3.12_env\Scripts\python.exe doesn't
#      exist", because the virtualenv was created with the bundled Python 3.11.
#   2) Python defaults to GBK here and dies with UnicodeDecodeError while
#      reading build output / logs.
#
# IMPORTANT - keep this file ASCII only, and never pipe the dot-source below.
#   * ASCII only: Windows PowerShell 5.1 reads a BOM-less script as ANSI
#     (GBK on this host). Non-ASCII comments then get mis-decoded and can
#     swallow the following line break, which silently turns the next code
#     line into a comment. That is how ". export.ps1" used to disappear and
#     why every call ended with "idf.py is not recognized".
#   * no pipe: ". script | Out-Null" runs the script inside a pipeline scope,
#     so the idf.py function it defines is lost.
# ================================================================
$ErrorActionPreference = 'Stop'

# 1) Put the bundled Python 3.11 (the one that created idf5.2_py3.11_env) first
$env:PATH = 'E:\ESP-IDF\Espressif\tools\idf-python\3.11.2;E:\ESP-IDF\Espressif\tools\idf-python\3.11.2\Scripts;' + $env:PATH

# 2) Force UTF-8 so Python does not fall back to GBK
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'

# Load the IDF environment (defines the idf.py function, extends PATH)
. 'E:\ESP-IDF\Espressif\frameworks\esp-idf-v5.2.1\export.ps1'

# Work from the project root no matter where the caller was
Set-Location -LiteralPath $PSScriptRoot

# Forward every argument to idf.py
idf.py @args
