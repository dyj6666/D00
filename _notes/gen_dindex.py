# -*- coding: utf-8 -*-
"""生成 D 盘目录地图导航页 D:\_INDEX.html（分类点击直达，不移动任何文件）
带大小缓存：目录未变则复用上次统计，实现秒开"""
import os, html, json

ROOT = "D:\\"
CACHE = r"D:\01_DevTools\Scripts\index_cache.json"

# 分类规则：按目录名匹配（顺序优先）
RULES = [
    ("data", "📂 数据区（新结构）", None, ["01_DevTools", "02_Projects", "03_Work", "04_Study", "05_Docs", "06_Media", "07_Downloads", "08_Apps", "09_Archive", "10_AppData", "99_Temp"]),
    ("sys", "🗑 系统与缓存（勿动/可清）", None, ["$RECYCLE.BIN", "System Volume Information", "WindowsApps", "WpSystem", "Config.Msi", "DeliveryOptimization", "WUDownloadCache", "WPS_CACH", "Program Files", "Documents and Settings"]),
    ("tools", "💻 开发工具（已安装，勿移动）", None, ["QT", "MDK", "Keil_v5", "cubmx", "STM32Cube", "STM32CUBECLT", "STM32CUBEMX", "HPM", "MSP430", "RT-ThreadStudio", "MounRiver", "CH32", "C51", "S32CHINA", "RT1064", "VScode", "Clion", "pycharm", "Python", "MinGW", "SEC_Stdio", "openmvide", "freecad", "SecureCRT", "MobaXtern", "Putty", "FileZilla", "FINALShell", "Source Insight 3", "Dism++", "WinRAR", "360压缩", "HexView", "VOFA", "Logic", "CEIWEI_CommMonitor_20253_x64", "CommMonitor12", "SDFormatter", "mathtype", "Steam++", "DEEPL", "DEV", "TwinCAT", "Git", "WPS Software", "立创EDA", "立创专业板", "腾讯会议", "钉钉", "DINGDING", "LenovoSoftstore", "电脑管家迁移文件", "S32CHINA"]),
    ("proj", "📦 项目代码（勿移动）", None, ["stm32", "GIT-SPACE", "XJ_robot", "FreeRTOS_projects", "Lwip_projects", "MainProject", "SoInProject", "TwinCAT_project", "tempp", "MSP430"]),
    ("data2", "📚 资料与文档", None, ["【正点原子】ESP32 AI BOX1资料（A盘）", "文档", "桌面"]),
    ("app", "📱 应用与数据", None, ["微信", "xwechat_files", "QQ", "QQnew", "QQFiles", "WeChatFiles", "BDWP"]),
    ("dl", "⬇️ 下载器目录（改保存在应用内设置）", None, ["迅雷下载", "LenovoQMDownload", "LeStoreDownload", "天翼云盘下载"]),
    ("misc", "❓ 待确认/其他", None, ["Duang游戏盒", "MapData", "搜狗", "天翼云盘", "天翼云盘同步盘"]),
]
# 兜底分类
FALLBACK = ("misc", "❓ 待确认/其他")

CAT_TITLES = {r[0]: r[1] for r in RULES}
CAT_ORDER = [r[0] for r in RULES]

def categorize(name):
    for key, title, pat, names in RULES:
        if name in names:
            return key
    return FALLBACK[0]

def size_str(path):
    """带缓存的目录大小：目录 mtime 未变则复用缓存，否则重新统计并写缓存（秒开）"""
    cache = {}
    try:
        with open(CACHE, encoding="utf-8") as f:
            cache = json.load(f)
    except Exception:
        pass
    key = os.path.basename(path.rstrip("\\"))
    try:
        mtime = os.stat(path).st_mtime
    except Exception:
        mtime = 0
    if key in cache and abs(cache[key][0] - mtime) < 1:
        return cache[key][1]
    total = 0
    try:
        for dp, dn, fn in os.walk(path):
            for f in fn:
                try:
                    total += os.path.getsize(os.path.join(dp, f))
                except OSError:
                    pass
            if total > 2 * 1024 ** 3:  # 超过 2GB 提前返回（部分统计）
                break
    except Exception:
        pass
    s = "%.2f GB" % (total / 1024 ** 3)
    cache[key] = [mtime, s]
    try:
        with open(CACHE, "w", encoding="utf-8") as f:
            json.dump(cache, f, ensure_ascii=False)
    except Exception:
        pass
    return s

items = []
for name in sorted(os.listdir(ROOT)):
    p = os.path.join(ROOT, name)
    if not os.path.isdir(p):
        continue
    cat = categorize(name)
    items.append((cat, name, size_str(p)))

groups = {}
for cat, name, sz in items:
    groups.setdefault(cat, []).append((name, sz))

cards = ""
for cat in CAT_ORDER:
    if cat not in groups:
        continue
    rows = ""
    for name, sz in sorted(groups[cat], key=lambda x: -float(x[1].replace(" GB", "").replace("?", "0"))):
        lock = "🔒" if cat in ("sys", "tools", "proj", "data2", "app") else ""
        rows += f'<a class="row" href="file:///D:/{html.escape(name)}"><span class="nm">{html.escape(name)}</span><span class="sz">{sz}</span><span class="go">打开 →</span></a>'
    cards += f'<div class="card"><h2>{CAT_TITLES[cat]}</h2>{rows}</div>'

RULES_HTML = """
<div class="card"><h2>📋 D 盘使用守则（让管理永远不乱）</h2>
<div class="row"><span class="nm">📥 下载的安装包/文件 → <b>07_Downloads</b>（装完即删）</span></div>
<div class="row"><span class="nm">💾 开发工具安装路径 → <b>D:\\01_DevTools\\&lt;软件名&gt;</b>（Keil/IDE/串口工具…）</span></div>
<div class="row"><span class="nm">💾 普通软件安装路径 → <b>D:\\08_Apps\\&lt;软件名&gt;</b>（网盘/办公/播放器…）</span></div>
<div class="row"><span class="nm">🧩 绿色/便携软件 → <b>D:\\01_DevTools\\</b> 或 <b>D:\\08_Apps\\Green\\</b></span></div>
<div class="row"><span class="nm">📚 学习资料 → <b>04_Study</b> ｜ 📁 工作文件 → <b>03_Work</b> ｜ 📝 个人文档 → <b>05_Docs</b></span></div>
<div class="row"><span class="nm">🗄 旧资料/不常用 → <b>09_Archive</b>（建议压缩成 rar）</span></div>
<div class="row"><span class="nm">⏳ 临时文件 → <b>99_Temp</b>（每周清空）</span></div>
<div class="row"><span class="nm">🔄 新项目代码 → <b>02_Projects</b> ｜ 📱 微信/QQ/网盘数据 → <b>10_AppData</b>（勿手动动）</span></div>
<div class="row"><span class="nm">🚫 新软件不要装 C 盘（C 盘只留系统）· 安装包用后即删</span></div>
</div>
<div class="card"><h2>🛠 常用操作</h2>
<div class="row"><span class="nm">🔍 恢复显示隐藏目录：资源管理器 → 查看 → 勾选「隐藏的项目」（隐藏的是兼容传送门，程序照常工作）</span></div>
<div class="row"><span class="nm">🗺 本地图显示<b>全部</b>目录（含隐藏），点击任意目录直达 —— 找东西第一入口</span></div>
<div class="row"><span class="nm">🔄 刷新本地图：<code>D:\\Python\\python.exe D:\\01_DevTools\\Scripts\\gen_dindex.py</code>（已设开机+每日 9:00 自动刷新）</span></div>
</div>
"""

doc = f"""<!DOCTYPE html>
<html lang="zh"><head><meta charset="utf-8"><title>D 盘目录地图</title><style>
body{{margin:0;font-family:"Microsoft YaHei",sans-serif;background:#121418;color:#e8edf3;padding:24px}}
h1{{text-align:center;background:linear-gradient(90deg,#7ab8ff,#e8edf3);-webkit-background-clip:text;background-clip:text;color:transparent;font-size:30px;margin:6px 0}}
.sub{{text-align:center;color:#93a0b0;font-size:13px;margin-bottom:18px}}
.stats{{text-align:center;color:#93a0b0;font-size:12px;margin-bottom:16px}}
.card{{background:#1d2126;border:1px solid #2a3038;border-radius:14px;padding:14px 18px;margin:0 auto 16px;max-width:1080px}}
.card h2{{color:#9fc3ff;font-size:17px;margin:0 0 10px}}
.row{{display:flex;align-items:center;gap:12px;padding:7px 12px;border-radius:8px;text-decoration:none;color:#e8edf3;font-size:14px;transition:background .12s}}
a.row:hover{{background:#2a323d}}
.nm{{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}}
.sz{{color:#f0d98c;font-size:12px;min-width:70px;text-align:right}}
.go{{color:#4f8cff;font-size:12px}}
.legend{{text-align:center;color:#5d6875;font-size:12px;margin-top:10px}}
code{{background:#2a323d;padding:1px 6px;border-radius:4px;color:#f0d98c}}
</style></head><body>
<h1>🗂 D 盘目录地图</h1>
<div class="sub">点击任意目录直达 · 🔒 = 已安装软件/项目/系统目录（勿移动改名）· 隐藏目录也在此可见</div>
<div class="stats">共 {len(items)} 个顶层目录 · 顶层可见：仅编号目录 01~99</div>
{RULES_HTML}
{cards}
<div class="legend">维护规则：下载进 07_Downloads · 临时进 99_Temp · 旧资料进 09_Archive · 开发工具装 01_DevTools · 普通软件装 08_Apps</div>
</body></html>"""

with open(ROOT + "_INDEX.html", "w", encoding="utf-8") as f:
    f.write(doc)
print("已生成 D:\\_INDEX.html")
print("目录数:", len(items))

