# -*- coding: utf-8 -*-
"""生成 Wii U Top 100 Markdown 文档。"""
import json, os, re, datetime

top = json.load(open('top100.json'))
GEN = "2026-09-19"

RATING_NOTE = {
 'Perfect':'完美：可全程通关，无已知影响体验的问题',
 'Playable':'可玩：可通关，存在轻微图形/音频瑕疵',
 'Runs':'可运行：能进入游戏，但有明显问题或需额外设置',
 'Loads':'仅加载：能启动到标题/菜单，无法正常游玩',
 'Unplayable':'无法运行：崩溃或黑屏',
}
BADGE = {'Perfect':'🟢','Playable':'🔵','Runs':'🟡','Loads':'🟠','Unplayable':'🔴'}
ZH_BADGE = {'官方中文':'✅ 官方','民间汉化':'🈶 汉化','未见中文':'—'}

GENRE_CN = {
 'action':'动作','adventure':'冒险','platformer':'平台','2d platformer':'2D平台',
 '3d platformer':'3D平台','role-playing':'角色扮演','action rpg':'动作RPG',
 'racing':'竞速','fighting':'格斗','party':'聚会','strategy':'策略','puzzle':'解谜',
 'shooter':'射击','first-person shooter':'FPS','third-person shooter':'TPS',
 'sports':'体育','simulation':'模拟','music':'音乐','rhythm':'节奏','horror':'恐怖',
 'sandbox':'沙盒','arcade':'街机','education':'教育','board game':'桌游','dancing':'舞蹈',
 'survival':'生存','tennis':'网球','golf':'高尔夫','virtual pet':'虚拟宠物','other':'其他',
}

def fmt_genre(g, maxn=3):
    """把 GameTDB 的英文 genre 列表转成简短中文标签。"""
    if not g: return '—'
    parts=[p.strip().lower() for p in g.replace('/',',').split(',') if p.strip()]
    out=[]
    for p in parts:
        cn=GENRE_CN.get(p)
        if cn and cn not in out: out.append(cn)
    if not out:
        out=[parts[0].title()] if parts else ['—']
    return ' / '.join(out[:maxn])

def fmt_pub(p):
    """发行商去掉公司后缀噪声。"""
    if not p: return '—'
    p=re.sub(r'\s*(Entertainment|Interactive|Games|Studios?|Inc\.?|Ltd\.?|LLC|Co\.?|Corporation|Company)\b\.?','',p, flags=re.I)
    p=re.sub(r'\s{2,}',' ',p).strip(' ,.')
    return p or '—'

def esc(s):
    return (s or '').replace('|','\\|')

def zh_cell(r):
    b = ZH_BADGE[r['zh_support']]
    if r['zh_support'] == '未见中文':
        return b
    parts = [b, esc(r['zh_kind'])] if r['zh_kind'] else [b]
    if r.get('zh_target_version'): parts.append(f"<sub>{esc(r['zh_target_version'])}</sub>")
    if r['zh_team']: parts.append(f"<sub>{esc(r['zh_team'])}</sub>")
    return '<br>'.join(parts)

def name_cell(r):
    en = esc(r['name'])
    zh = esc(r['zh']) if r['zh'] else ''
    return f"**{zh}**<br><sub>{en}</sub>" if zh else f"**{en}**"

def cover_cell(r):
    if not r.get('cover'): return '—'
    return f"<img src=\"covers/{r['cover']}\" width=\"64\">"

def heat_bar(h):
    n = max(1, round(h/12.5))
    return '█'*n

lines = []
A = lines.append

A("# Wii U 游戏 Top 100")
A("")
A(f"> 生成时间：{GEN}　|　数据源：Cemu 官方 Wiki、GameTDB、Wikipedia、社区汉化目录")
A("> ")
A("> 本表面向 **Cemu 模拟器用户**：热度以「模拟器社区关注度」为主，而非单纯的当年销量。")
A("")

# ---- 概览 ----
tot = len(top)
by_r = {}
for r in top: by_r[r['rating']] = by_r.get(r['rating'],0)+1
n_off = sum(1 for r in top if r['zh_support']=='官方中文')
n_fan = sum(1 for r in top if r['zh_support']=='民间汉化')
A("## 一眼概览")
A("")
A(f"- **兼容性**：完美 {by_r.get('Perfect',0)} · 可玩 {by_r.get('Playable',0)} · 可运行 {by_r.get('Runs',0)} · 仅加载 {by_r.get('Loads',0)} · 无法运行 {by_r.get('Unplayable',0)}")
A(f"- **中文支持**：官方中文 {n_off} · 民间汉化 {n_fan} · 未见中文 {tot-n_off-n_fan}")
A(f"- **可放心玩**（完美/可玩 且 有中文）：{sum(1 for r in top if r['rating'] in ('Perfect','Playable') and r['zh_support']!='未见中文')} 款")
A("")
A("> **关于中文的重要事实**：Wii U 从未在中国大陆、台湾、香港正式发售，主机本身锁区。")
A("> GameTDB 全库 2860 条 Wii U 记录中，仅 YouTube 应用带中文语言标记——**没有任何一款 Wii U 游戏原生内置中文**。")
A("> 表中「官方中文」指该作在欧美版中自带简/繁中文（多为第三方独立游戏），其余中文均来自民间汉化组。")
A("")

# ---- 榜单 ----
A("## 完整榜单")
A("")
A("| # | 封面 | 游戏 | 热度 | Cemu 兼容性 | 中文 | 类型 | 发行 | 年份 |")
A("|--:|:--:|:--|:--|:--|:--|:--|:--|--:|")
for r in top:
    genre = esc(fmt_genre(r['genre']))
    pub   = esc(fmt_pub(r['publisher']))
    yr    = r['year'] or '—'
    A(f"| {r['rank']} | {cover_cell(r)} | {name_cell(r)} | `{heat_bar(r['heat'])}` {r['heat']} "
      f"| {BADGE[r['rating']]} {r['rating_cn']} | {zh_cell(r)} | {genre} | {pub} | {yr} |")
A("")

# ---- 分组视图 ----
A("## 按中文支持分组")
A("")
for grp in ('官方中文','民间汉化'):
    g=[r for r in top if r['zh_support']==grp]
    if not g: continue
    A(f"### {ZH_BADGE[grp]} {grp}（{len(g)} 款）")
    A("")
    A("| # | 游戏 | Cemu 兼容性 | 中文类型 | 对应版本 | 汉化组 |")
    A("|--:|:--|:--|:--|:--|:--|")
    for r in g:
        A(f"| {r['rank']} | {esc(r['zh'] or r['name'])} | {BADGE[r['rating']]} {r['rating_cn']} "
          f"| {esc(r['zh_kind'])} | {esc(r.get('zh_target_version')) or '—'} | {esc(r['zh_team']) or '—'} |")
    A("")

A("## 按 Cemu 兼容性分组")
A("")
for rt in ('Perfect','Playable','Runs','Loads','Unplayable'):
    g=[r for r in top if r['rating']==rt]
    if not g: continue
    A(f"### {BADGE[rt]} {RATING_NOTE[rt]}（{len(g)} 款）")
    A("")
    A('　'.join(f"{r['rank']}. {r['zh'] or r['name']}" for r in g))
    A("")

# ---- 方法论 ----
A("## 数据来源与口径")
A("")
A("### 热度指数（0–100）")
A("")
A("三个来源按百分位秩加权，而非直接数值相加（避免量纲差异与长尾失真）：")
A("")
A("| 权重 | 指标 | 含义 | 为何这样取 |")
A("|--:|:--|:--|:--|")
A("| 60% | Cemu Wiki 条目信息量 | 模拟器社区关注度 | 本表读者是 Cemu 用户。该指标天然排除「在 PS/PC 很火但没人用 Cemu 玩」的多平台大作偏差 |")
A("| 22% | en.wikipedia 近 12 个月浏览量 | 大众知名度 | 客观、可复现；匹配到同名概念页的条目按 0 计，不倒扣 |")
A("| 18% | 官方累计销量 | 当年市场表现 | 仅 20 款破百万。权重低于关注度，因 Wii U 总销量仅 1356 万台，销量更多反映平台规模而非作品地位 |")
A("")
A("### 各字段来源")
A("")
A("| 字段 | 来源 | 说明 |")
A("|:--|:--|:--|")
A("| Cemu 兼容性 | [Cemu 官方 Wiki](https://wiki.cemu.info) `Infobox VG` 的 `rating` | 五档：Perfect / Playable / Runs / Loads / Unplayable |")
A("| 中文支持 | 社区「WIIU中文游戏全集」目录（133 条，去重 62 个标题） | 区分官方中文与民间汉化，并保留汉化组署名 |")
A("| 中文名 | Wikipedia 跨语言链接（en→zh）+ 汉化目录译名 | 只采用可验证的对应条目；指向「系列」「列表」页的模糊结果已剔除 |")
A("| 封面 / 类型 / 发行 / 年份 | [GameTDB](https://www.gametdb.com) Wii U 数据库（2860 条） | 按 6 位游戏 ID 取封面，区域目录依 ID 第 4 位推导 |")
A("")
A("### 候选池与筛选")
A("")
A("1. 取 Cemu Wiki 主命名空间全部 1924 个页面，保留含 `Infobox VG` 的 1215 个游戏页；")
A("2. 剔除 Virtual Console 复刻页（437 个，非原生 Wii U 作品）与系统应用/流媒体，得 **582 款原生 Wii U 游戏**；")
A("3. 要求兼容性评级有效（五档之一），按热度指数取前 100。")
A("")
A("> **口径说明**：入榜不以 Wikipedia 命中为前提——以 Cemu Wiki 为主库，Wikipedia 仅作增强。")
A("> 早期版本曾因此漏掉《异度之刃X》《耀西的毛线世界》《幻影异闻录♯FE》等重要作品，已修正。")
A("")
A("### 已知局限")
A("")
A("- 热度指数是**相对排序**工具，不是绝对权威；靠后名次之间的差异不具统计显著性。")
A("- 中文支持以上述社区目录为准，该目录已通过知乎、gamer520、精英模拟网等多个独立快照交叉核对，编号 WIIUCH001–133 一致。")
A("  目录未收录 ≠ 一定没有汉化。romhacking.net 的 Wii U 条目以波兰语等欧洲语言补丁为主，未见中文补丁收录，故不作为中文判据。")
A("- Cemu 兼容性评级由 Wiki 社区维护，可能滞后于最新版本；Android 端（本仓库移植目标）实际表现可能与桌面端不同。")
A("- 仅《140》未取到中文译名（抽象音游，本身无通行译名），表中保留英文原名。")
A("")
A("### 封面图来源与版权")
A("")
A("`covers/` 下 100 张封面来自 [GameTDB](https://www.gametdb.com) 社区数据库，按各作 6 位游戏 ID 取回")
A("（`https://art.gametdb.com/wiiu/cover/<区域>/<ID>.jpg`）。封面美术版权归各游戏发行商所有，")
A("此处仅作本表识别用途的缩略引用；GameTDB 的元数据本身按其站点条款开放取用。")
A("如需重新拉取或更换尺寸，见 [scripts/fetch_covers.py](scripts/fetch_covers.py) 与 [DATA-SOURCES.md](DATA-SOURCES.md#封面图)。")
A("")

open('WiiU-Top100.md','w').write('\n'.join(lines))
print("wrote WiiU-Top100.md", len(lines), "lines")
